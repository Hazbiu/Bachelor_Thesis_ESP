#include "face_overlay_renderer.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config/app_config.h"

static void draw_rect_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    uint16_t color)
{
    if (!fb) {
        return;
    }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 < 0) x2 = 0;
    if (y2 < 0) y2 = 0;

    if (x1 >= (int)fb_w) x1 = fb_w - 1;
    if (x2 >= (int)fb_w) x2 = fb_w - 1;
    if (y1 >= (int)fb_h) y1 = fb_h - 1;
    if (y2 >= (int)fb_h) y2 = fb_h - 1;

    for (int x = x1; x <= x2; x++) {
        fb[y1 * fb_w + x] = color;
        fb[y2 * fb_w + x] = color;
    }

    for (int y = y1; y <= y2; y++) {
        fb[y * fb_w + x1] = color;
        fb[y * fb_w + x2] = color;
    }
}

static void draw_filled_rect_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    uint16_t color)
{
    if (!fb || fb_w == 0 || fb_h == 0) {
        return;
    }

    if (x1 > x2) {
        int temp = x1;
        x1 = x2;
        x2 = temp;
    }

    if (y1 > y2) {
        int temp = y1;
        y1 = y2;
        y2 = temp;
    }

    if (x2 < 0 || y2 < 0 || x1 >= (int)fb_w || y1 >= (int)fb_h) {
        return;
    }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int)fb_w) x2 = (int)fb_w - 1;
    if (y2 >= (int)fb_h) y2 = (int)fb_h - 1;

    for (int y = y1; y <= y2; y++) {
        uint16_t *row = fb + y * fb_w;
        for (int x = x1; x <= x2; x++) {
            row[x] = color;
        }
    }
}

void face_overlay_renderer_draw_box_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    int thickness,
    uint16_t color)
{
    for (int inset = 0; inset < thickness; inset++) {
        draw_rect_rgb565(
            fb,
            fb_w,
            fb_h,
            x1 + inset,
            y1 + inset,
            x2 - inset,
            y2 - inset,
            color
        );
    }
}

/* Five-pixel-wide uppercase font. Lowercase folder names are shown uppercase. */
static const uint8_t s_font_5x7[36][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}, /* 5 */
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}, /* 9 */
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
    {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};

static const uint8_t *font_5x7_glyph(char character)
{
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }

    if (character >= '0' && character <= '9') {
        return s_font_5x7[character - '0'];
    }

    if (character >= 'A' && character <= 'Z') {
        return s_font_5x7[10 + character - 'A'];
    }

    return NULL;
}

static void draw_large_text_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int start_x,
    int start_y,
    const char *text,
    int scale,
    uint16_t color)
{
    if (!fb || !text || scale <= 0) {
        return;
    }

    int cursor_x = start_x;

    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        const uint8_t *glyph = font_5x7_glyph(*cursor);

        if (glyph) {
            for (int row = 0; row < 7; row++) {
                for (int column = 0; column < 5; column++) {
                    if ((glyph[row] & (1U << (4 - column))) == 0) {
                        continue;
                    }

                    draw_filled_rect_rgb565(
                        fb,
                        fb_w,
                        fb_h,
                        cursor_x + column * scale,
                        start_y + row * scale,
                        cursor_x + (column + 1) * scale - 1,
                        start_y + (row + 1) * scale - 1,
                        color
                    );
                }
            }
        }

        cursor_x += 6 * scale;
    }
}

void face_overlay_renderer_draw_label_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int box_x2,
    int box_y1,
    const char *name,
    uint16_t background_color)
{
    if (!name || name[0] == '\0') {
        return;
    }

    const int scale = APP_FACE_LABEL_FONT_SCALE;
    const int padding = 2 * scale;
    const int text_width = (int)strlen(name) * 6 * scale - scale;
    const int text_height = 7 * scale;
    const int label_width = text_width + 2 * padding;
    const int label_height = text_height + 2 * padding;

    int label_x2 = box_x2;
    if (label_x2 >= (int)fb_w) label_x2 = (int)fb_w - 1;
    if (label_x2 < label_width - 1) label_x2 = label_width - 1;

    int label_x1 = label_x2 - label_width + 1;
    int label_y2 = box_y1 - 1;
    int label_y1 = label_y2 - label_height + 1;

    if (label_y1 < 0) {
        label_y1 = box_y1;
        label_y2 = label_y1 + label_height - 1;
    }

    draw_filled_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        label_x1,
        label_y1,
        label_x2,
        label_y2,
        background_color
    );

    draw_large_text_rgb565(
        fb,
        fb_w,
        fb_h,
        label_x1 + padding,
        label_y1 + padding,
        name,
        scale,
        0xFFFF
    );
}
