#include "manual_canvas.h"

#include <stddef.h>

#include "display_driver.h"
#include "font_bitmap.h"

#ifdef ESP_PLATFORM
#include "epd_panel.h"
#endif

static int map_point(const manual_canvas_t *canvas, int32_t x, int32_t y,
                     display_point_t *native)
{
    if (!canvas || !native || x < 0 || y < 0 || x >= canvas->width ||
        y >= canvas->height) {
        return 0;
    }
    return display_rotation_map_point(canvas->rotation, DISPLAY_EPD_WIDTH,
                                      DISPLAY_EPD_HEIGHT, x, y,
                                      native) ? 1 : 0;
}

int manual_canvas_init(manual_canvas_t *canvas, display_rotation_t rotation)
{
    if (!canvas || !display_rotation_valid(rotation)) {
        return 0;
    }
    display_rotation_dimensions(rotation, DISPLAY_EPD_WIDTH, DISPLAY_EPD_HEIGHT,
                                &canvas->width, &canvas->height);
#ifdef ESP_PLATFORM
    /* 条带缓冲为静态数组，恒可用；无需检查 epd_fb_raw。 */
#endif
    canvas->rotation = rotation;
    return canvas->width > 0 && canvas->height > 0;
}

void manual_canvas_clear(const manual_canvas_t *canvas, uint8_t color)
{
    (void)canvas;
#ifdef ESP_PLATFORM
    epd_fb_fill(color & 0x03u);
#else
    (void)color;
#endif
}

void manual_canvas_pixel(const manual_canvas_t *canvas, int32_t x, int32_t y,
                         uint8_t color)
{
    display_point_t native;
    if (!map_point(canvas, x, y, &native)) {
        return;
    }
#ifdef ESP_PLATFORM
    epd_fb_set_pixel(native.x, native.y, color & 0x03u);
#else
    (void)native;
    (void)color;
#endif
}

void manual_canvas_line(const manual_canvas_t *canvas, int32_t x0, int32_t y0,
                        int32_t x1, int32_t y1, uint8_t color)
{
    /* Bresenham keeps diagonal markers useful without a temporary line. */
    int32_t dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    while (1) {
        manual_canvas_pixel(canvas, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int32_t twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void manual_canvas_rect(const manual_canvas_t *canvas, int32_t x, int32_t y,
                        int32_t width, int32_t height, uint8_t color)
{
    if (width <= 0 || height <= 0) return;
    manual_canvas_line(canvas, x, y, x + width - 1, y, color);
    manual_canvas_line(canvas, x, y + height - 1, x + width - 1,
                       y + height - 1, color);
    manual_canvas_line(canvas, x, y, x, y + height - 1, color);
    manual_canvas_line(canvas, x + width - 1, y, x + width - 1,
                       y + height - 1, color);
}

void manual_canvas_rect_fill(const manual_canvas_t *canvas, int32_t x,
                             int32_t y, int32_t width, int32_t height,
                             uint8_t color)
{
    if (!canvas || width <= 0 || height <= 0) return;
    /* Clip before entering the per-pixel path; this also prevents integer
     * overflow from a malformed status rectangle. */
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > canvas->width) width = canvas->width - x;
    if (y + height > canvas->height) height = canvas->height - y;
    if (width <= 0 || height <= 0) return;
    for (int32_t row = 0; row < height; ++row) {
        for (int32_t col = 0; col < width; ++col) {
            manual_canvas_pixel(canvas, x + col, y + row, color);
        }
    }
}

static uint32_t utf8_next(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    if (*p == 0) return 0;
    uint32_t codepoint = *p++;
    if (codepoint < 0x80u) {
        *text = (const char *)p;
        return codepoint;
    }
    if ((codepoint & 0xe0u) == 0xc0u && p[0]) {
        const uint8_t b1 = *p++;
        codepoint = ((codepoint & 0x1fu) << 6) | (b1 & 0x3fu);
    } else if ((codepoint & 0xf0u) == 0xe0u && p[0] && p[1]) {
        const uint8_t b1 = *p++;
        const uint8_t b2 = *p++;
        codepoint = ((codepoint & 0x0fu) << 12) | ((b1 & 0x3fu) << 6) |
                    (b2 & 0x3fu);
    } else if ((codepoint & 0xf8u) == 0xf0u && p[0] && p[1] && p[2]) {
        const uint8_t b1 = *p++;
        const uint8_t b2 = *p++;
        const uint8_t b3 = *p++;
        codepoint = ((codepoint & 0x07u) << 18) | ((b1 & 0x3fu) << 12) |
                    ((b2 & 0x3fu) << 6) | (b3 & 0x3fu);
    } else {
        codepoint = '?';
    }
    *text = (const char *)p;
    return codepoint;
}

static void draw_bitmap(const manual_canvas_t *canvas, int32_t x, int32_t y,
                        const uint8_t *rows, int width, int height,
                        int row_bytes, uint8_t color, int scale)
{
    if (!canvas || !rows || scale < 1) return;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const uint8_t value = rows[row * row_bytes + (col >> 3)];
            if ((value & (uint8_t)(1u << (7 - (col & 7)))) == 0) continue;
            manual_canvas_rect_fill(canvas, x + col * scale, y + row * scale,
                                    scale, scale, color);
        }
    }
}

static bool bitmap_pixel(const uint8_t *rows, int row_bytes, int x, int y)
{
    return (rows[y * row_bytes + (x >> 3)] &
            (uint8_t)(1u << (7 - (x & 7)))) != 0;
}

static void downsample_bitmap_2x(const uint8_t *source, int width, int height,
                                 int source_row_bytes, uint8_t *destination)
{
    const int output_width = width / 2;
    const int output_height = height / 2;
    const int output_row_bytes = output_width / 8;
    const size_t output_size = (size_t)output_row_bytes * output_height;
    for (size_t i = 0; i < output_size; ++i) destination[i] = 0;
    for (int y = 0; y < output_height; ++y) {
        for (int x = 0; x < output_width; ++x) {
            const int source_x = x * 2;
            const int source_y = y * 2;
            const bool ink = bitmap_pixel(source, source_row_bytes,
                                          source_x, source_y) ||
                             bitmap_pixel(source, source_row_bytes,
                                          source_x + 1, source_y) ||
                             bitmap_pixel(source, source_row_bytes,
                                          source_x, source_y + 1) ||
                             bitmap_pixel(source, source_row_bytes,
                                          source_x + 1, source_y + 1);
            if (ink) {
                destination[y * output_row_bytes + (x >> 3)] |=
                    (uint8_t)(1u << (7 - (x & 7)));
            }
        }
    }
}

static void draw_bitmap_resampled(const manual_canvas_t *canvas, int32_t x,
                                  int32_t y, const uint8_t *source,
                                  int width, int height, int source_row_bytes,
                                  int output_width, int output_height,
                                  uint8_t color)
{
    for (int out_y = 0; out_y < output_height; ++out_y) {
        const int source_y = out_y * height / output_height;
        for (int out_x = 0; out_x < output_width; ++out_x) {
            const int source_x = out_x * width / output_width;
            if (bitmap_pixel(source, source_row_bytes, source_x, source_y)) {
                manual_canvas_pixel(canvas, x + out_x, y + out_y, color);
            }
        }
    }
}

void manual_canvas_text_utf8(const manual_canvas_t *canvas, int32_t x,
                             int32_t y, const char *text, uint8_t color,
                             int scale)
{
    if (!canvas || !text) return;
    if (scale < 1) scale = 1;
    int32_t cursor_x = x;
    int32_t cursor_y = y;
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            ++p;
            cursor_x = x;
            cursor_y += (MANUAL_ASCII_HEIGHT + 2) * scale;
            continue;
        }
        const uint32_t codepoint = utf8_next(&p);
        if (codepoint < 0x80u) {
            draw_bitmap(canvas, cursor_x, cursor_y,
                        manual_font_ascii_rows((uint8_t)codepoint),
                        MANUAL_ASCII_WIDTH, MANUAL_ASCII_HEIGHT, 2, color,
                        scale);
            cursor_x += (MANUAL_ASCII_WIDTH + 2) * scale;
        } else {
            draw_bitmap(canvas, cursor_x, cursor_y,
                        manual_font_cjk_rows(codepoint), MANUAL_CJK_WIDTH,
                        MANUAL_CJK_HEIGHT, 4, color, scale);
            cursor_x += (MANUAL_CJK_WIDTH + 2) * scale;
        }
        if (cursor_x >= canvas->width + MANUAL_CJK_WIDTH * scale) break;
    }
}

void manual_canvas_text_compact(const manual_canvas_t *canvas, int32_t x,
                                int32_t y, const char *text, uint8_t color)
{
    uint8_t compact_ascii[12];
    uint8_t compact_cjk[32];
    if (!canvas || !text) return;
    int32_t cursor_x = x;
    const char *p = text;
    while (*p) {
        const uint32_t codepoint = utf8_next(&p);
        if (codepoint < 0x80u) {
            downsample_bitmap_2x(manual_font_ascii_rows((uint8_t)codepoint),
                                 MANUAL_ASCII_WIDTH, MANUAL_ASCII_HEIGHT, 2,
                                 compact_ascii);
            draw_bitmap(canvas, cursor_x, y, compact_ascii, 8, 12, 1, color, 1);
            cursor_x += 10;
        } else {
            downsample_bitmap_2x(manual_font_cjk_rows(codepoint),
                                 MANUAL_CJK_WIDTH, MANUAL_CJK_HEIGHT, 4,
                                 compact_cjk);
            draw_bitmap(canvas, cursor_x, y, compact_cjk, 16, 16, 2, color, 1);
            cursor_x += 18;
        }
        if (cursor_x >= canvas->width + 16) break;
    }
}

void manual_canvas_text_medium(const manual_canvas_t *canvas, int32_t x,
                               int32_t y, const char *text, uint8_t color)
{
    if (!canvas || !text) return;
    int32_t cursor_x = x;
    const char *p = text;
    while (*p) {
        const uint32_t codepoint = utf8_next(&p);
        if (codepoint < 0x80u) {
            draw_bitmap_resampled(canvas, cursor_x, y,
                                  manual_font_ascii_rows((uint8_t)codepoint),
                                  MANUAL_ASCII_WIDTH, MANUAL_ASCII_HEIGHT, 2,
                                  10, 15, color);
            cursor_x += 12;
        } else {
            draw_bitmap_resampled(canvas, cursor_x, y,
                                  manual_font_cjk_rows(codepoint),
                                  MANUAL_CJK_WIDTH, MANUAL_CJK_HEIGHT, 4,
                                  20, 20, color);
            cursor_x += 22;
        }
        if (cursor_x >= canvas->width + 20) break;
    }
}

void manual_canvas_date_digits(const manual_canvas_t *canvas, int32_t x,
                               int32_t y, const char *text, uint8_t color)
{
    if (!canvas || !text) return;
    int32_t cursor_x = x;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            cursor_x += MANUAL_DATE_DIGIT_WIDTH / 3;
            continue;
        }
        draw_bitmap(canvas, cursor_x, y,
                    manual_font_date_digit_rows((uint8_t)(*p - '0')),
                    MANUAL_DATE_DIGIT_WIDTH, MANUAL_DATE_DIGIT_HEIGHT, 6,
                    color, 1);
        cursor_x += MANUAL_DATE_DIGIT_WIDTH + 4;
    }
}

void manual_canvas_text(const manual_canvas_t *canvas, int32_t x, int32_t y,
                        const char *text, uint8_t color, int scale)
{
    manual_canvas_text_utf8(canvas, x, y, text, color, scale);
}

void manual_canvas_weather_icon(const manual_canvas_t *canvas, int32_t x,
                                int32_t y, uint8_t kind, uint8_t color,
                                int scale)
{
    if (!canvas || scale < 1) return;
    const int s = scale;
    if (kind == 2u || kind == 3u) { /* cloud / rain */
        manual_canvas_rect_fill(canvas, x + 5 * s, y + 9 * s, 14 * s, 7 * s,
                                color);
        manual_canvas_rect_fill(canvas, x + 9 * s, y + 5 * s, 8 * s, 8 * s,
                                color);
        manual_canvas_rect(canvas, x + 4 * s, y + 8 * s, 17 * s, 9 * s,
                           color);
        if (kind == 3u) {
            manual_canvas_line(canvas, x + 8 * s, y + 19 * s, x + 6 * s,
                               y + 23 * s, color);
            manual_canvas_line(canvas, x + 14 * s, y + 19 * s, x + 12 * s,
                               y + 23 * s, color);
            manual_canvas_line(canvas, x + 20 * s, y + 19 * s, x + 18 * s,
                               y + 23 * s, color);
        }
        return;
    }
    if (kind == 4u) { /* snow */
        manual_canvas_line(canvas, x + 12 * s, y + 3 * s, x + 12 * s,
                           y + 21 * s, color);
        manual_canvas_line(canvas, x + 3 * s, y + 12 * s, x + 21 * s,
                           y + 12 * s, color);
        manual_canvas_line(canvas, x + 5 * s, y + 5 * s, x + 19 * s,
                           y + 19 * s, color);
        manual_canvas_line(canvas, x + 19 * s, y + 5 * s, x + 5 * s,
                           y + 19 * s, color);
        return;
    }
    /* Sun / clear sky: a filled core with eight short rays. */
    manual_canvas_rect_fill(canvas, x + 8 * s, y + 8 * s, 9 * s, 9 * s,
                            color);
    manual_canvas_line(canvas, x + 12 * s, y + 1 * s, x + 12 * s, y + 7 * s,
                       color);
    manual_canvas_line(canvas, x + 12 * s, y + 18 * s, x + 12 * s, y + 23 * s,
                       color);
    manual_canvas_line(canvas, x + 1 * s, y + 12 * s, x + 7 * s, y + 12 * s,
                       color);
    manual_canvas_line(canvas, x + 18 * s, y + 12 * s, x + 23 * s, y + 12 * s,
                       color);
}

void manual_canvas_wifi_icon(const manual_canvas_t *canvas, int32_t x,
                             int32_t y, bool connected, uint8_t color,
                             int scale)
{
    if (!canvas || scale < 1) return;
    const int s = scale;
    manual_canvas_line(canvas, x + 1 * s, y + 5 * s, x + 8 * s,
                       y + 1 * s, color);
    manual_canvas_line(canvas, x + 8 * s, y + 1 * s, x + 15 * s,
                       y + 5 * s, color);
    manual_canvas_line(canvas, x + 4 * s, y + 9 * s, x + 8 * s,
                       y + 6 * s, color);
    manual_canvas_line(canvas, x + 8 * s, y + 6 * s, x + 12 * s,
                       y + 9 * s, color);
    manual_canvas_rect_fill(canvas, x + 7 * s, y + 12 * s, 3 * s, 3 * s,
                            connected ? color : MANUAL_COLOR_WHITE);
    if (!connected) {
        manual_canvas_line(canvas, x + 1 * s, y + 15 * s, x + 15 * s,
                           y + 1 * s, color);
    }
}
