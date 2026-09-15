#include "face_overlay_renderer.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

/* Five-pixel-wide uppercase font. Lowercase text is rendered uppercase. */
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

static const uint8_t s_glyph_dash[7] =
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
static const uint8_t s_glyph_colon[7] =
    {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
static const uint8_t s_glyph_period[7] =
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06};
static const uint8_t s_glyph_slash[7] =
    {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
static const uint8_t s_glyph_percent[7] =
    {0x19, 0x1A, 0x04, 0x08, 0x0B, 0x13, 0x00};

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

    switch (character) {
    case '-':
        return s_glyph_dash;
    case ':':
        return s_glyph_colon;
    case '.':
        return s_glyph_period;
    case '/':
        return s_glyph_slash;
    case '%':
        return s_glyph_percent;
    default:
        return NULL;
    }
}

static void draw_text_rgb565(
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

static int text_width_pixels(const char *text, int scale)
{
    const size_t length = text ? strlen(text) : 0U;
    if (length == 0U) {
        return 0;
    }

    return (int)(length * 6U * (size_t)scale) - scale;
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
    const int text_width = text_width_pixels(name, scale);
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

    draw_text_rgb565(
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

static void format_inference_line(
    char *buffer,
    size_t buffer_size,
    const char *prefix,
    bool valid,
    uint32_t inference_us)
{
    if (!buffer || buffer_size == 0U || !prefix) {
        return;
    }

    if (!valid) {
        snprintf(buffer, buffer_size, "%s: -- MS", prefix);
        return;
    }

    const uint32_t whole_ms = inference_us / 1000U;
    const uint32_t tenths_ms = (inference_us % 1000U) / 100U;

    snprintf(
        buffer,
        buffer_size,
        "%s: %" PRIu32 ".%" PRIu32 " MS",
        prefix,
        whole_ms,
        tenths_ms);
}

static void format_cpu_usage_line(
    char *buffer,
    size_t buffer_size,
    const char *prefix,
    bool valid,
    uint32_t usage_x10)
{
    if (!buffer || buffer_size == 0U || !prefix) {
        return;
    }

    if (!valid) {
        snprintf(
            buffer,
            buffer_size,
            "%s: --.-%%",
            prefix);
        return;
    }

    if (usage_x10 > 1000U) {
        usage_x10 = 1000U;
    }

    snprintf(
        buffer,
        buffer_size,
        "%s: %" PRIu32 ".%" PRIu32 "%%",
        prefix,
        usage_x10 / 10U,
        usage_x10 % 10U);
}

void face_overlay_renderer_draw_metrics_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    bool detector_valid,
    uint32_t detector_inference_us,
    bool recognizer_valid,
    uint32_t recognizer_inference_us,
    uint32_t fps_x10,
    bool hp_cpu_usage_valid,
    uint32_t hp_core0_usage_x10,
    uint32_t hp_core1_usage_x10)
{
    if (!fb || fb_w == 0U || fb_h == 0U) {
        return;
    }

    /*
     * Camera overlay intentionally contains only:
     *   1. face-recognition model inference time,
     *   2. face-detection model inference time,
     *   3. live preview FPS,
     *   4. total HP Core 0 utilization,
     *   5. total HP Core 1 utilization.
     *
     * Only HP Core 0 and HP Core 1 CPU-utilization rows are rendered.
     */
    char lines[5][64] = {{0}};

    format_inference_line(
        lines[0],
        sizeof(lines[0]),
        "AI-FACE REC MODEL INFERENCE",
        recognizer_valid,
        recognizer_inference_us);

    format_inference_line(
        lines[1],
        sizeof(lines[1]),
        "AI-FACE DEC MODEL INFERENCE",
        detector_valid,
        detector_inference_us);

    snprintf(
        lines[2],
        sizeof(lines[2]),
        "FPS: %" PRIu32 ".%" PRIu32,
        fps_x10 / 10U,
        fps_x10 % 10U);

    format_cpu_usage_line(
        lines[3],
        sizeof(lines[3]),
        "CPU HP CORE 0 USAGE",
        hp_cpu_usage_valid,
        hp_core0_usage_x10);

    format_cpu_usage_line(
        lines[4],
        sizeof(lines[4]),
        "CPU HP CORE 1 USAGE",
        hp_cpu_usage_valid,
        hp_core1_usage_x10);

    int scale = 2;
    int max_text_width = 0;

    for (size_t i = 0; i < 5U; i++) {
        const int width =
            text_width_pixels(lines[i], scale);

        if (width > max_text_width) {
            max_text_width = width;
        }
    }

    const int margin = 8;
    const int padding = 4 * scale;

    if (max_text_width + 2 * padding + 2 * margin > (int)fb_w) {
        scale = 1;
        max_text_width = 0;

        for (size_t i = 0; i < 5U; i++) {
            const int width =
                text_width_pixels(lines[i], scale);

            if (width > max_text_width) {
                max_text_width = width;
            }
        }
    }

    const int actual_padding = 4 * scale;
    const int line_height = 8 * scale;
    const int panel_width =
        max_text_width + 2 * actual_padding;

    const int panel_height =
        5 * line_height - scale + 2 * actual_padding;

    int panel_x2 = (int)fb_w - margin - 1;
    int panel_x1 = panel_x2 - panel_width + 1;
    int panel_y1 = margin;
    int panel_y2 = panel_y1 + panel_height - 1;

    if (panel_x1 < 0) {
        panel_x1 = 0;
    }

    if (panel_y2 >= (int)fb_h) {
        panel_y2 = (int)fb_h - 1;
    }

    /*
     * Opaque black keeps diagnostics readable over any camera scene and avoids
     * per-pixel alpha blending in the live preview path.
     */
    draw_filled_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        panel_x1,
        panel_y1,
        panel_x2,
        panel_y2,
        0x0000);

    draw_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        panel_x1,
        panel_y1,
        panel_x2,
        panel_y2,
        0x8410);

    int text_y = panel_y1 + actual_padding;

    for (size_t i = 0; i < 5U; i++) {
        draw_text_rgb565(
            fb,
            fb_w,
            fb_h,
            panel_x1 + actual_padding,
            text_y,
            lines[i],
            scale,
            0xFFFF);

        text_y += line_height;

        if (text_y >= panel_y2) {
            break;
        }
    }
}
