/*
 * Bongo Cat Companion - ESP32-S3 SuperMini firmware (ESP-IDF v5.3+ / v6.x)
 *
 * Receives 1-byte events from the PC:  'L' = left tap, 'R' = right tap
 *   - flashes the built-in RGB LED once per key press
 *   - shows the bongo cat animation (bongo_frame.h) + tap stats on a 128x64 SH1106 OLED (I2C)
 *
 * Tasks:
 *   rx_task      : reads bytes from USB, pushes valid events into a queue
 *   event_task   : owns the LED; flashes it per event and records tap stats
 *   display_task : owns the OLED; redraws ~30x per second, only when something changed
 *
 * OLED wiring (SuperMini):
 *   OLED VCC -> 3V3     OLED GND -> GND
 *   OLED SDA -> GPIO 8  OLED SCL -> GPIO 9
 */

#include <stdint.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sh1106.h"
#include "ui.h"

/* ---------------- Which USB port is the PC plugged into? ----------------
 * 1 = the S3's native USB (the SuperMini's only USB-C port; shows up as VID 303A)
 * 0 = a USB-to-UART bridge chip on UART0 (boards like the DevKitC; not the SuperMini) */
#define USE_NATIVE_USB 1

#if USE_NATIVE_USB
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#define PC_UART      UART_NUM_0
#define PC_UART_BAUD 115200          /* must match BAUD in bongo_pc.py */
#endif

/* ---------------- LED ---------------- */
#define LED_GPIO    48               /* built-in WS2812 RGB LED on the ESP32-S3 SuperMini */
#define FLASH_MS    40               /* how long the LED stays on per key press */
#define BRIGHTNESS  24               /* 0-255; the onboard LED is very bright */

/* ---------------- OLED ---------------- */
#define OLED_SDA_GPIO  8
#define OLED_SCL_GPIO  9
#define OLED_ADDR      0             /* 0 = auto-detect 0x3C / 0x3D */
#define FRAME_MS       33            /* ~30 fps max; a full SH1106 frame takes ~25 ms at 400 kHz */
#define PAW_SHOW_MS    100           /* how long a paw stays down on screen per tap */

static const char *TAG = "bongo";

static QueueHandle_t      evq;
static led_strip_handle_t led;

/* Shared between event_task (writer) and display_task (reader). */
static atomic_uint s_taps;
static atomic_uint s_last_tap_ms[2];   /* [0] = left, [1] = right */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------- LED helpers ---------------- */

static void led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num         = LED_GPIO,
        .max_leds               = 1,
        .led_model              = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led));
    ESP_ERROR_CHECK(led_strip_clear(led));   /* start off */
}

static void led_on(uint8_t ev)
{
    /* Left = blue, right = pink, so you can see the paws alternate. */
    if (ev == 'L') led_strip_set_pixel(led, 0, 0, BRIGHTNESS / 3, BRIGHTNESS);
    else           led_strip_set_pixel(led, 0, BRIGHTNESS, 0, BRIGHTNESS / 2);
    led_strip_refresh(led);
}

static void led_off(void)
{
    led_strip_clear(led);
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
            xQueueSend(evq, &b, 0);   /* drop rather than lag if the event task is behind */
        }
    }
}

static void event_task(void *arg)
{
    int64_t off_at_us = 0;            /* 0 = LED is off */

    for (;;) {
        /* Wait for a key press, but no longer than the time the LED should turn off. */
        TickType_t wait = portMAX_DELAY;
        if (off_at_us) {
            int64_t ms = (off_at_us - esp_timer_get_time() + 999) / 1000;
            wait = (ms > 0) ? pdMS_TO_TICKS(ms) : 0;
            if (ms > 0 && wait == 0) wait = 1;
        }

        uint8_t ev;
        if (xQueueReceive(evq, &ev, wait) == pdTRUE) {
            led_on(ev);
            off_at_us = esp_timer_get_time() + (int64_t)FLASH_MS * 1000;

            atomic_fetch_add(&s_taps, 1);
            atomic_store(&s_last_tap_ms[ev == 'R' ? 1 : 0], now_ms());
        } else if (off_at_us && esp_timer_get_time() >= off_at_us) {
            led_off();
            off_at_us = 0;
        }
    }
}

static void display_task(void *arg)
{
    const sh1106_config_t cfg = {
        .sda_gpio = OLED_SDA_GPIO,
        .scl_gpio = OLED_SCL_GPIO,
        .addr     = OLED_ADDR,
        .clk_hz   = 400000,
    };
    if (sh1106_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "OLED not available; LED still works");
        vTaskDelete(NULL);
    }

    uint32_t kps = 0, window_start = now_ms(), window_taps = 0;
    uint32_t shown_taps = UINT32_MAX, shown_kps = UINT32_MAX;
    int      shown_paws = -1;
    bool     flush_error_logged = false;

    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        uint32_t now  = now_ms();
        uint32_t taps = atomic_load(&s_taps);

        /* keys per second over the last full second */
        if (now - window_start >= 1000) {
            kps          = taps - window_taps;
            window_taps  = taps;
            window_start = now;
        }

        /* Show the paw that was tapped most recently, if it was within PAW_SHOW_MS. */
        uint32_t age_l = now - atomic_load(&s_last_tap_ms[0]);
        uint32_t age_r = now - atomic_load(&s_last_tap_ms[1]);
        ui_paw_t paws  = UI_PAWS_IDLE;
        if (age_l < PAW_SHOW_MS || age_r < PAW_SHOW_MS) {
            paws = (age_l <= age_r) ? UI_PAW_LEFT : UI_PAW_RIGHT;
        }

        /* Only push a new frame over I2C when something on screen changed. */
        if (taps != shown_taps || kps != shown_kps || (int)paws != shown_paws) {
            ui_draw(taps, kps, paws);
            esp_err_t err = sh1106_flush();
            if (err != ESP_OK && !flush_error_logged) {
                ESP_LOGW(TAG, "OLED flush failed: %s", esp_err_to_name(err));
                flush_error_logged = true;
            }
            shown_taps = taps;
            shown_kps  = kps;
            shown_paws = (int)paws;
        }

        xTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));
    }
}

void app_main(void)
{
    /* Paws start "up": pretend the last tap was long ago. */
    atomic_init(&s_taps, 0);
    atomic_init(&s_last_tap_ms[0], now_ms() - 10000);
    atomic_init(&s_last_tap_ms[1], now_ms() - 10000);

    led_init();
    pc_link_init();

    evq = xQueueCreate(32, sizeof(uint8_t));
    configASSERT(evq);

    /* Display gets the lowest priority so screen updates never delay the LED or USB reads. */
    xTaskCreate(event_task,   "event",   3072, NULL, 6, NULL);
    xTaskCreate(rx_task,      "rx",      3072, NULL, 5, NULL);
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "Bongo Cat ready (%s), LED on GPIO %d, OLED on SDA %d / SCL %d",
             USE_NATIVE_USB ? "native USB" : "UART bridge", LED_GPIO, OLED_SDA_GPIO, OLED_SCL_GPIO);
}
