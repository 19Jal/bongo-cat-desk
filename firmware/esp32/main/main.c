/*
 * Bongo Cat Companion - ESP32-S3 firmware (ESP-IDF v5.3+ / v6.x)
 *
 * Receives 1-byte events from the PC:  'L' = left tap, 'R' = right tap
 * and flashes the built-in RGB LED once per key press.
 *
 * Tasks:
 *   rx_task  : reads bytes from USB, pushes valid events into a queue
 *   led_task : sole owner of the LED; turns it on for each event and
 *              off again FLASH_MS later, using the queue timeout as the timer.
 */

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "led_strip.h"

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

/* ---------------- LED ---------------- */
#define LED_GPIO    48               /* built-in WS2812 on ESP32-S3-DevKitC-1 v1.0 (v1.1 uses GPIO 38) */
#define FLASH_MS    40               /* how long the LED stays on per key press */
#define BRIGHTNESS  24               /* 0-255; the onboard LED is very bright */

static const char *TAG = "bongo";

static QueueHandle_t      evq;
static led_strip_handle_t led;

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
            xQueueSend(evq, &b, 0);   /* drop rather than lag if the LED task is behind */
        }
    }
}

static void led_task(void *arg)
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
        } else if (off_at_us && esp_timer_get_time() >= off_at_us) {
            led_off();
            off_at_us = 0;
        }
    }
}

void app_main(void)
{
    led_init();
    pc_link_init();

    evq = xQueueCreate(32, sizeof(uint8_t));
    configASSERT(evq);

    xTaskCreate(led_task, "led", 3072, NULL, 6, NULL);
    xTaskCreate(rx_task,  "rx",  3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Bongo Cat ready (%s), LED on GPIO %d",
             USE_NATIVE_USB ? "native USB" : "UART bridge", LED_GPIO);
}
