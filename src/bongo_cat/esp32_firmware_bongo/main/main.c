/*
 * Bongo Cat Companion - ESP32-S3 firmware (ESP-IDF v5.x)
 *
 * Receives 1-byte events from the PC:  'L' = left paw tap, 'R' = right paw tap
 * and drives two hobby servos (SG90 / MG90S style) as paws.
 *
 * Tasks:
 *   rx_task  : reads bytes from USB, pushes valid events into a queue (never blocks on servos)
 *   paw_task : sole owner of the LEDC/servo hardware; puts a paw down on an event and
 *              lifts it again TAP_HOLD_MS later, using queue timeouts as the timer.
 *
 * Wiring (default):
 *   Left servo signal  -> GPIO 4
 *   Right servo signal -> GPIO 5
 *   Servo V+ -> separate 5 V supply (NOT the 3.3 V pin), servo GND -> ESP32 GND (common ground)
 */

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"

/* ---------------- Which USB port is the PC plugged into? ----------------
 * 1 = the S3's native USB port (labelled "USB" on DevKitC; shows up as VID 303A)
 * 0 = the USB-to-UART bridge port (labelled "UART"/"COM"; CP210x / CH343 chip)   */
#define USE_NATIVE_USB 1

#if USE_NATIVE_USB
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#define PC_UART      UART_NUM_0
#define PC_UART_BAUD 115200          /* must match BAUD in bongo_pc.py */
#endif

/* ---------------- Paw / servo tuning ---------------- */
#define LEFT_PAW_GPIO   4
#define RIGHT_PAW_GPIO  5

#define SERVO_FREQ_HZ   50
#define SERVO_MIN_US    500          /* pulse width at 0 degrees   */
#define SERVO_MAX_US    2500         /* pulse width at 180 degrees */
#define SERVO_RES       LEDC_TIMER_14_BIT
#define SERVO_PERIOD_US (1000000 / SERVO_FREQ_HZ)

#define TAP_HOLD_MS     90           /* how long a paw stays down per tap; tune to servo speed */

static const char *TAG = "bongo";

typedef struct {
    const char    *name;
    int            gpio;
    ledc_channel_t channel;
    int            up_deg;           /* resting angle */
    int            down_deg;         /* "tap" angle   */
    int64_t        release_at_us;    /* 0 = paw is up */
} paw_t;

/* Right paw is mirrored so both paws swing "inward" toward the desk. Adjust to your build. */
static paw_t paws[2] = {
    { "left",  LEFT_PAW_GPIO,  LEDC_CHANNEL_0,  30,  70, 0 },
    { "right", RIGHT_PAW_GPIO, LEDC_CHANNEL_1, 150, 110, 0 },
};

static QueueHandle_t evq;

/* ---------------- Servo helpers ---------------- */

static uint32_t deg_to_duty(int deg)
{
    if (deg < 0)   deg = 0;
    if (deg > 180) deg = 180;
    uint32_t us = SERVO_MIN_US + (uint32_t)(SERVO_MAX_US - SERVO_MIN_US) * deg / 180;
    return (uint32_t)(((uint64_t)us * ((1u << SERVO_RES) - 1)) / SERVO_PERIOD_US);
}

static void servo_write(const paw_t *p, int deg)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, p->channel, deg_to_duty(deg));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, p->channel);
}

static void servos_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,   /* S3 only has low-speed mode */
        .duty_resolution = SERVO_RES,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    for (int i = 0; i < 2; i++) {
        ledc_channel_config_t ch = {
            .gpio_num   = paws[i].gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = paws[i].channel,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = deg_to_duty(paws[i].up_deg),   /* start with paws up */
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
}

/* ---------------- PC link ---------------- */

static void pc_link_init(void)
{
#if USE_NATIVE_USB
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
#else
    uart_config_t cfg = {
        .baud_rate  = PC_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(PC_UART, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PC_UART, &cfg));
    /* Deliver single bytes after ~1 symbol of idle instead of the default ~10 (lower latency). */
    ESP_ERROR_CHECK(uart_set_rx_timeout(PC_UART, 1));
#endif
}

static int pc_read_byte(uint8_t *b)
{
#if USE_NATIVE_USB
    return usb_serial_jtag_read_bytes(b, 1, portMAX_DELAY);
#else
    return uart_read_bytes(PC_UART, b, 1, portMAX_DELAY);
#endif
}

/* ---------------- Tasks ---------------- */

static void rx_task(void *arg)
{
    uint8_t b;
    for (;;) {
        if (pc_read_byte(&b) == 1 && (b == 'L' || b == 'R')) {
            /* If the paw task is somehow behind, drop the tap rather than lag. */
            xQueueSend(evq, &b, 0);
        }
    }
}

static void paw_task(void *arg)
{
    for (;;) {
        /* 1. Lift any paw whose hold time is over, and find the next deadline. */
        int64_t now  = esp_timer_get_time();
        int64_t next = INT64_MAX;
        for (int i = 0; i < 2; i++) {
            paw_t *p = &paws[i];
            if (p->release_at_us == 0) continue;
            if (now >= p->release_at_us) {
                servo_write(p, p->up_deg);
                p->release_at_us = 0;
            } else if (p->release_at_us < next) {
                next = p->release_at_us;
            }
        }

        /* 2. Wait for a new tap, but no longer than the next paw-lift deadline. */
        TickType_t wait = portMAX_DELAY;
        if (next != INT64_MAX) {
            int64_t ms = (next - now + 999) / 1000;
            wait = pdMS_TO_TICKS(ms);
            if (wait == 0) wait = 1;
        }

        uint8_t ev;
        if (xQueueReceive(evq, &ev, wait) == pdTRUE) {
            paw_t *p = (ev == 'L') ? &paws[0] : &paws[1];
            if (p->release_at_us == 0) {
                servo_write(p, p->down_deg);
            }
            /* A tap while already down just keeps the paw down a bit longer. */
            p->release_at_us = esp_timer_get_time() + (int64_t)TAP_HOLD_MS * 1000;
        }
    }
}

void app_main(void)
{
    servos_init();
    pc_link_init();

    evq = xQueueCreate(32, sizeof(uint8_t));
    configASSERT(evq);

    xTaskCreate(paw_task, "paw", 3072, NULL, 6, NULL);
    xTaskCreate(rx_task,  "rx",  3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Bongo Cat ready (%s)", USE_NATIVE_USB ? "native USB" : "UART bridge");
}