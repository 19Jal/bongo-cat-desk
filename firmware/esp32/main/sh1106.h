/*
 * Minimal SH1106 128x64 OLED driver (I2C) for ESP-IDF 5.3+ / 6.x
 * Framebuffer in RAM; draw into it, then call sh1106_flush() to send it.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SH1106_WIDTH   128
#define SH1106_HEIGHT  64

/* SH1106 has 132 columns of RAM but the glass shows 128; most 1.3" modules
 * show column 2..129. If your image is shifted by 2 px, set this to 0. */
#define SH1106_COL_OFFSET 2

typedef struct {
    int     sda_gpio;
    int     scl_gpio;
    uint8_t addr;        /* 0x3C on most modules (0x3D if the address jumper is moved); 0 = auto-detect */
    uint32_t clk_hz;     /* 400000 is fine for SH1106 */
} sh1106_config_t;

esp_err_t sh1106_init(const sh1106_config_t *cfg);
esp_err_t sh1106_flush(void);            /* send the whole framebuffer to the screen */
esp_err_t sh1106_set_contrast(uint8_t level);
esp_err_t sh1106_display_on(bool on);

/* ---- drawing into the framebuffer (nothing is sent until sh1106_flush) ---- */
void sh1106_clear(void);
void sh1106_pixel(int x, int y, bool on);
void sh1106_hline(int x, int y, int w, bool on);
void sh1106_vline(int x, int y, int h, bool on);
void sh1106_line(int x0, int y0, int x1, int y1, bool on);
void sh1106_rect(int x, int y, int w, int h, bool on);
void sh1106_fill_rect(int x, int y, int w, int h, bool on);
/* Draws a row-major, MSB-first bitmap (Adafruit drawBitmap / image2cpp "horizontal" format).
 * Set bits turn pixels on; clear bits leave the framebuffer as it is. */
void sh1106_bitmap(int x, int y, int w, int h, const uint8_t *bmp);
/* 5x7 font, 6 px per character at scale 1. Returns the x after the last character. */
int  sh1106_text(int x, int y, const char *s, int scale);
