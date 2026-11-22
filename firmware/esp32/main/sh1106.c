#include <string.h>
#include <stdlib.h>
#include "sh1106.h"
#include "font5x7.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "sh1106";

#define I2C_TIMEOUT_MS 100
#define CTRL_CMD  0x00   /* control byte: following bytes are commands */
#define CTRL_DATA 0x40   /* control byte: following bytes are display data */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_fb[SH1106_HEIGHT / 8][SH1106_WIDTH];   /* 8 pages x 128 columns */

/* ---------------- low-level I2C ---------------- */

static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[1 + 32];
    if (n > sizeof(buf) - 1) return ESP_ERR_INVALID_SIZE;
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, I2C_TIMEOUT_MS);
}

esp_err_t sh1106_init(const sh1106_config_t *cfg)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = -1,                    /* auto-select */
        .sda_io_num        = cfg->sda_gpio,
        .scl_io_num        = cfg->scl_gpio,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,       /* most OLED modules also have their own pull-ups */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err)); return err; }

    uint8_t addr = cfg->addr;
    if (addr == 0) {
        if (i2c_master_probe(s_bus, 0x3C, I2C_TIMEOUT_MS) == ESP_OK)      addr = 0x3C;
        else if (i2c_master_probe(s_bus, 0x3D, I2C_TIMEOUT_MS) == ESP_OK) addr = 0x3D;
        else {
            ESP_LOGE(TAG, "no OLED found at 0x3C or 0x3D - check wiring (SDA=%d, SCL=%d) and power",
                     cfg->sda_gpio, cfg->scl_gpio);
            return ESP_ERR_NOT_FOUND;
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = cfg->clk_hz ? cfg->clk_hz : 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) { ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err)); return err; }

    static const uint8_t init_seq[] = {
        0xAE,        /* display off */
        0xD5, 0x80,  /* clock divide / oscillator */
        0xA8, 0x3F,  /* multiplex ratio = 64 */
        0xD3, 0x00,  /* display offset = 0 */
        0x40,        /* start line = 0 */
        0xAD, 0x8B,  /* DC-DC converter on (SH1106-specific) */
        0x32,        /* charge pump voltage 8.0 V (SH1106-specific) */
        0xA1,        /* segment remap: column 0 on the left */
        0xC8,        /* COM scan direction: top to bottom */
        0xDA, 0x12,  /* COM pins configuration */
        0x81, 0x80,  /* contrast */
        0xD9, 0x22,  /* pre-charge period */
        0xDB, 0x35,  /* VCOM deselect level */
        0xA4,        /* display follows RAM */
        0xA6,        /* normal (not inverted) */
    };
    err = send_cmds(init_seq, sizeof(init_seq));
    if (err != ESP_OK) { ESP_LOGE(TAG, "init sequence failed: %s", esp_err_to_name(err)); return err; }

    sh1106_clear();
    err = sh1106_flush();                 /* clear RAM before turning the panel on */
    if (err != ESP_OK) return err;
    err = sh1106_display_on(true);
    if (err == ESP_OK) ESP_LOGI(TAG, "SH1106 ready at 0x%02X", addr);
    return err;
}

esp_err_t sh1106_flush(void)
{
    /* SH1106 has no horizontal auto-increment across pages, so send one page at a time. */
    uint8_t data[1 + SH1106_WIDTH];
    data[0] = CTRL_DATA;
    for (int page = 0; page < SH1106_HEIGHT / 8; page++) {
        const uint8_t cmds[] = {
            (uint8_t)(0xB0 | page),                          /* page address */
            (uint8_t)(0x00 | (SH1106_COL_OFFSET & 0x0F)),    /* column low nibble */
            (uint8_t)(0x10 | (SH1106_COL_OFFSET >> 4)),      /* column high nibble */
        };
        esp_err_t err = send_cmds(cmds, sizeof(cmds));
        if (err != ESP_OK) return err;
        memcpy(&data[1], s_fb[page], SH1106_WIDTH);
        err = i2c_master_transmit(s_dev, data, sizeof(data), I2C_TIMEOUT_MS);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t sh1106_set_contrast(uint8_t level)
{
    const uint8_t cmds[] = { 0x81, level };
    return send_cmds(cmds, sizeof(cmds));
}

esp_err_t sh1106_display_on(bool on)
{
    const uint8_t cmd = on ? 0xAF : 0xAE;
    return send_cmds(&cmd, 1);
}

/* ---------------- drawing ---------------- */

void sh1106_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void sh1106_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SH1106_WIDTH || y < 0 || y >= SH1106_HEIGHT) return;
    if (on) s_fb[y >> 3][x] |=  (uint8_t)(1u << (y & 7));
    else    s_fb[y >> 3][x] &= (uint8_t)~(1u << (y & 7));
}

void sh1106_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) sh1106_pixel(x + i, y, on);
}

void sh1106_vline(int x, int y, int h, bool on)
{
    for (int i = 0; i < h; i++) sh1106_pixel(x, y + i, on);
}

void sh1106_line(int x0, int y0, int x1, int y1, bool on)
{
    /* Bresenham */
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        sh1106_pixel(x0, y0, on);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void sh1106_rect(int x, int y, int w, int h, bool on)
{
    sh1106_hline(x, y, w, on);
    sh1106_hline(x, y + h - 1, w, on);
    sh1106_vline(x, y, h, on);
    sh1106_vline(x + w - 1, y, h, on);
}

void sh1106_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int j = 0; j < h; j++) sh1106_hline(x, y + j, w, on);
}

void sh1106_bitmap(int x, int y, int w, int h, const uint8_t *bmp)
{
    int bytes_per_row = (w + 7) / 8;
    for (int j = 0; j < h; j++) {
        const uint8_t *row = bmp + j * bytes_per_row;
        for (int i = 0; i < w; i++) {
            if (row[i >> 3] & (0x80 >> (i & 7))) sh1106_pixel(x + i, y + j, true);
        }
    }
}

int sh1106_text(int x, int y, const char *s, int scale)
{
    if (scale < 1) scale = 1;
    for (; *s; s++) {
        char c = *s;
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        const uint8_t *g = font5x7[c - FONT_FIRST];
        for (int col = 0; col < FONT_W; col++) {
            for (int row = 0; row < FONT_H; row++) {
                if (g[col] & (1u << row)) {
                    sh1106_fill_rect(x + col * scale, y + row * scale, scale, scale, true);
                }
            }
        }
        x += (FONT_W + 1) * scale;
    }
    return x;
}
