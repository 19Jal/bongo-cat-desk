/*
 * Bongo Cat screen: one of three full-screen frames from bongo_frame.h,
 * with the tap counter in the top-left corner and keys/second in the top-right
 * (both corners are blank in all three frames).
 */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "sh1106.h"
#include "bongo_frame.h"

void ui_draw(uint32_t taps, uint32_t kps, ui_paw_t paw)
{
    const unsigned char *frame =
        (paw == UI_PAW_LEFT)  ? left_paw  :
        (paw == UI_PAW_RIGHT) ? right_paw :
                                default_frame;

    sh1106_clear();
    sh1106_bitmap(0, 0, FRAME_WIDTH, FRAME_HEIGHT, frame);

    char buf[16];

    /* top-left: total taps (fits up to 6 digits before reaching the cat's ear) */
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(taps % 1000000));
    sh1106_text(0, 0, buf, 1);

    /* top-right: keys per second, right-aligned */
    snprintf(buf, sizeof(buf), "%lu/s", (unsigned long)(kps > 99 ? 99 : kps));
    int w = (int)strlen(buf) * 6 - 1;
    sh1106_text(SH1106_WIDTH - w, 0, buf, 1);
}
