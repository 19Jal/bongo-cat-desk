#pragma once
#include <stdint.h>

typedef enum {
    UI_PAWS_IDLE = 0,
    UI_PAW_LEFT,
    UI_PAW_RIGHT,
} ui_paw_t;

/* Draws one frame into the SH1106 framebuffer (call sh1106_flush() afterwards). */
void ui_draw(uint32_t kps, ui_paw_t paw);
