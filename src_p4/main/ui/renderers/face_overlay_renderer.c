
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

static int rounded_rect_inset_for_row(int edge_row, int radius)
{
    if (radius <= 1 || edge_row >= radius) {
        return 0;
    }

    const int circle_radius = radius - 1;
    const int dy = circle_radius - edge_row;
    const int radius_squared = circle_radius * circle_radius;
    int dx = 0;

    while (dx < circle_radius) {
        const int next_dx = dx + 1;
        if (next_dx * next_dx + dy * dy > radius_squared) {
            break;
        }
        dx = next_dx;
    }

    return circle_radius - dx;
}

static void draw_filled_rounded_rect_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int x1,
    int y1,
    int x2,
    int y2,
    int radius,
    uint16_t color)
{
    if (!fb || fb_w == 0U || fb_h == 0U) {
        return;
    }

    if (x1 > x2) {
        const int temp = x1;
        x1 = x2;
        x2 = temp;
    }

    if (y1 > y2) {
        const int temp = y1;
        y1 = y2;
        y2 = temp;
    }

    const int width = x2 - x1 + 1;
    const int height = y2 - y1 + 1;
    const int maximum_radius = (width < height ? width : height) / 2;

    if (radius > maximum_radius) {
        radius = maximum_radius;
    }

    if (radius <= 1) {
        draw_filled_rect_rgb565(
            fb, fb_w, fb_h, x1, y1, x2, y2, color);
        return;
    }

    for (int y = y1; y <= y2; y++) {
        int edge_row = y - y1;
        const int bottom_edge_row = y2 - y;
        if (bottom_edge_row < edge_row) {
            edge_row = bottom_edge_row;
        }

        const int inset = rounded_rect_inset_for_row(edge_row, radius);
        draw_filled_rect_rgb565(
            fb,
            fb_w,
            fb_h,
            x1 + inset,
            y,
            x2 - inset,
            y,
            color);
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

static void draw_text_emphasized_rgb565(
    uint16_t *fb,
    uint32_t fb_w,
    uint32_t fb_h,
    int start_x,
    int start_y,
    const char *text,
    int scale,
    uint16_t color)
{
    draw_text_rgb565(
        fb, fb_w, fb_h, start_x, start_y, text, scale, color);
    draw_text_rgb565(
        fb, fb_w, fb_h, start_x + 1, start_y, text, scale, color);
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

static void format_inference_value(
    char *buffer,
    size_t buffer_size,
    bool valid,
    uint32_t inference_us)
{
    if (!buffer || buffer_size == 0U) {
        return;
    }

    if (!valid) {
        snprintf(buffer, buffer_size, "--.- MS");
        return;
    }

    const uint32_t whole_ms = inference_us / 1000U;
    const uint32_t tenths_ms = (inference_us % 1000U) / 100U;

    snprintf(
        buffer,
        buffer_size,
        "%" PRIu32 ".%" PRIu32 " MS",
        whole_ms,
        tenths_ms);
}

static void format_cpu_usage_value(
    char *buffer,
    size_t buffer_size,
    bool valid,
    uint32_t usage_x10)
{
    if (!buffer || buffer_size == 0U) {
        return;
    }

    if (!valid) {
        snprintf(buffer, buffer_size, "--.-%%");
        return;
    }

    if (usage_x10 > 1000U) {
        usage_x10 = 1000U;
    }

    snprintf(
        buffer,
        buffer_size,
        "%" PRIu32 ".%" PRIu32 "%%",
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

    /* Camera FPS is intentionally not shown because preview pacing changes
     * between active and power-optimized states. Keep the public renderer
     * signature stable for existing callers. */
    (void)fps_x10;

    static const char *labels[4] = {
        "FACE RECOGNITION",
        "FACE DETECTION",
        "HP CORE 0 USAGE",
        "HP CORE 1 USAGE",
    };

    char values[4][24] = {{0}};
    format_inference_value(
        values[0], sizeof(values[0]), recognizer_valid,
        recognizer_inference_us);
    format_inference_value(
        values[1], sizeof(values[1]), detector_valid,
        detector_inference_us);
    format_cpu_usage_value(
        values[2], sizeof(values[2]), hp_cpu_usage_valid,
        hp_core0_usage_x10);
    format_cpu_usage_value(
        values[3], sizeof(values[3]), hp_cpu_usage_valid,
        hp_core1_usage_x10);

    int scale = 2;
    int margin = 0;
    int outer_padding = 0;
    int left_content_padding = 0;
    int right_content_padding = 0;
    int column_gap = 0;
    int header_height = 0;
    int row_height = 0;
    int group_padding = 0;
    int group_gap = 0;
    int panel_width = 0;
    int panel_height = 0;

    for (;;) {
        int maximum_label_width = 0;
        int maximum_value_width = 0;

        for (size_t i = 0; i < 4U; i++) {
            const int label_width = text_width_pixels(labels[i], scale);
            const int value_width = text_width_pixels(values[i], scale);

            if (label_width > maximum_label_width) {
                maximum_label_width = label_width;
            }
            if (value_width > maximum_value_width) {
                maximum_value_width = value_width;
            }
        }

        margin = 4 * scale;
        outer_padding = 4 * scale;
        left_content_padding = 6 * scale;
        right_content_padding = 4 * scale;
        column_gap = 6 * scale;
        header_height = 14 * scale;
        row_height = 10 * scale;
        group_padding = 2 * scale;
        group_gap = 3 * scale;

        const int rows_width =
            maximum_label_width + column_gap + maximum_value_width;
        const int content_width =
            left_content_padding + rows_width + right_content_padding;

        const int title_width = text_width_pixels("SYSTEM PERFORMANCE", scale);
        const int live_width = text_width_pixels("LIVE", scale) + 4 * scale;
        const int header_width = title_width + 5 * scale + live_width;

        panel_width = 2 * outer_padding +
            (content_width > header_width ? content_width : header_width);

        const int ai_group_height = 2 * group_padding + 2 * row_height;
        const int cpu_group_height = 2 * group_padding + 2 * row_height;
        panel_height =
            2 * outer_padding + header_height + 2 * group_gap +
            ai_group_height + cpu_group_height;

        if (scale == 1 ||
            (panel_width + 2 * margin <= (int)fb_w &&
             panel_height + 2 * margin <= (int)fb_h)) {
            break;
        }

        scale = 1;
    }

    int panel_x2 = (int)fb_w - margin - 1;
    int panel_x1 = panel_x2 - panel_width + 1;
    const int panel_y1 = margin;
    int panel_y2 = panel_y1 + panel_height - 1;

    if (panel_x1 < 0) {
        panel_x1 = 0;
    }
    if (panel_y2 >= (int)fb_h) {
        panel_y2 = (int)fb_h - 1;
    }

    const uint16_t panel_background = 0x0864;
    const uint16_t group_background = 0x10E6;
    const uint16_t divider_color = 0x2A2B;
    const uint16_t primary_text = 0xFFDF;
    const uint16_t secondary_text = 0xBDF7;
    const uint16_t ai_accent = 0x2EB7;
    const uint16_t live_accent = 0x2E6E;
    const uint16_t cpu_accent = 0x3DFF;

    /* Opaque RGB565 surfaces keep the panel readable without alpha blending. */
    const int panel_radius = 6 * scale;
    draw_filled_rounded_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        panel_x1,
        panel_y1,
        panel_x2,
        panel_y2,
        panel_radius,
        panel_background);

    draw_filled_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        panel_x1 + panel_radius,
        panel_y1,
        panel_x2 - panel_radius,
        panel_y1 + scale - 1,
        ai_accent);

    const int header_y1 = panel_y1 + outer_padding;
    const int header_text_y =
        header_y1 + (header_height - 7 * scale) / 2;

    draw_text_emphasized_rgb565(
        fb,
        fb_w,
        fb_h,
        panel_x1 + outer_padding,
        header_text_y,
        "SYSTEM PERFORMANCE",
        scale,
        primary_text);

    const int live_text_width = text_width_pixels("LIVE", scale);
    const int live_badge_width = live_text_width + 4 * scale;
    const int live_badge_height = 10 * scale;
    const int live_badge_x2 = panel_x2 - outer_padding;
    const int live_badge_x1 = live_badge_x2 - live_badge_width + 1;
    const int live_badge_y1 =
        header_y1 + (header_height - live_badge_height) / 2;
    const int live_badge_y2 = live_badge_y1 + live_badge_height - 1;

    draw_filled_rounded_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        live_badge_x1,
        live_badge_y1,
        live_badge_x2,
        live_badge_y2,
        3 * scale,
        live_accent);
    draw_text_emphasized_rgb565(
        fb,
        fb_w,
        fb_h,
        live_badge_x1 + 2 * scale,
        live_badge_y1 + (live_badge_height - 7 * scale) / 2,
        "LIVE",
        scale,
        panel_background);

    const int group_x1 = panel_x1 + outer_padding;
    const int group_x2 = panel_x2 - outer_padding;
    const int ai_group_y1 = header_y1 + header_height + group_gap;
    const int ai_group_height = 2 * group_padding + 2 * row_height;
    const int ai_group_y2 = ai_group_y1 + ai_group_height - 1;
    const int cpu_group_y1 = ai_group_y2 + 1 + group_gap;
    const int cpu_group_height = 2 * group_padding + 2 * row_height;
    const int cpu_group_y2 = cpu_group_y1 + cpu_group_height - 1;
    const int group_radius = 3 * scale;

    draw_filled_rounded_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        group_x1,
        ai_group_y1,
        group_x2,
        ai_group_y2,
        group_radius,
        group_background);
    draw_filled_rounded_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        group_x1,
        cpu_group_y1,
        group_x2,
        cpu_group_y2,
        group_radius,
        group_background);

    draw_filled_rounded_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        group_x1 + 2 * scale,
        ai_group_y1 + 2 * scale,
        group_x1 + 3 * scale - 1,
        ai_group_y2 - 2 * scale,
        scale,
        ai_accent);
    draw_filled_rounded_rect_rgb565(
        fb,
        fb_w,
        fb_h,
        group_x1 + 2 * scale,
        cpu_group_y1 + 2 * scale,
        group_x1 + 3 * scale - 1,
        cpu_group_y2 - 2 * scale,
        scale,
        cpu_accent);

    const uint16_t value_colors[4] = {
        ai_accent,
        ai_accent,
        cpu_accent,
        cpu_accent,
    };

    for (size_t i = 0; i < 4U; i++) {
        const bool ai_row = i < 2U;
        const int local_row = ai_row ? (int)i : (int)i - 2;
        const int group_y1 = ai_row ? ai_group_y1 : cpu_group_y1;
        const int row_y1 =
            group_y1 + group_padding + local_row * row_height;

        if (local_row > 0) {
            const int divider_y = row_y1 - 1;
            draw_filled_rect_rgb565(
                fb,
                fb_w,
                fb_h,
                group_x1 + left_content_padding,
                divider_y,
                group_x2 - right_content_padding,
                divider_y,
                divider_color);
        }

        const int text_y = row_y1 + (row_height - 7 * scale) / 2;
        draw_text_rgb565(
            fb,
            fb_w,
            fb_h,
            group_x1 + left_content_padding,
            text_y,
            labels[i],
            scale,
            secondary_text);

        const int value_width = text_width_pixels(values[i], scale);
        const int value_x =
            group_x2 - right_content_padding - value_width;
        draw_text_emphasized_rgb565(
            fb,
            fb_w,
            fb_h,
            value_x,
            text_y,
            values[i],
            scale,
            value_colors[i]);
    }
}
