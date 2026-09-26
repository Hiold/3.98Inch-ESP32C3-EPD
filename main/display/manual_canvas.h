/*
 * Small immediate-mode canvas for the four-colour EPD.
 *
 * The canvas deliberately owns no pixel buffer.  It writes directly to the
 * driver's 2bpp framebuffer one pixel at a time, which keeps the ESP32-C3
 * heap usage constant at one EPD framebuffer.  Rotation is applied while
 * drawing, so callers can lay out a page in the current logical orientation.
 */
#ifndef MANUAL_CANVAS_H
#define MANUAL_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

#include "rotation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MANUAL_COLOR_BLACK = 0,
    MANUAL_COLOR_WHITE = 1,
    MANUAL_COLOR_YELLOW = 2,
    MANUAL_COLOR_RED = 3,
} manual_color_t;

typedef struct {
    display_rotation_t rotation;
    int32_t width;
    int32_t height;
} manual_canvas_t;

/* Returns false for an invalid rotation or a missing framebuffer. */
int manual_canvas_init(manual_canvas_t *canvas, display_rotation_t rotation);
void manual_canvas_clear(const manual_canvas_t *canvas, uint8_t color);
void manual_canvas_pixel(const manual_canvas_t *canvas, int32_t x, int32_t y,
                         uint8_t color);
void manual_canvas_line(const manual_canvas_t *canvas, int32_t x0, int32_t y0,
                        int32_t x1, int32_t y1, uint8_t color);
void manual_canvas_rect(const manual_canvas_t *canvas, int32_t x, int32_t y,
                        int32_t width, int32_t height, uint8_t color);
void manual_canvas_rect_fill(const manual_canvas_t *canvas, int32_t x,
                             int32_t y, int32_t width, int32_t height,
                             uint8_t color);

/* Flash-resident high-resolution ASCII and curated CJK glyphs. Unknown
 * Unicode code points are rendered as blank cells. */
void manual_canvas_text(const manual_canvas_t *canvas, int32_t x, int32_t y,
                        const char *text, uint8_t color, int scale);
void manual_canvas_text_utf8(const manual_canvas_t *canvas, int32_t x,
                             int32_t y, const char *text, uint8_t color,
                             int scale);
/* Smaller labels are downsampled directly from the MiSans glyph in constant
 * stack space; useful for status/footer rows without another font buffer. */
void manual_canvas_text_compact(const manual_canvas_t *canvas, int32_t x,
                                int32_t y, const char *text, uint8_t color);
/* Slightly larger compact MiSans text (20 px CJK / 10x15 px ASCII), drawn
 * directly from flash glyphs without an intermediate framebuffer. */
void manual_canvas_text_medium(const manual_canvas_t *canvas, int32_t x,
                               int32_t y, const char *text, uint8_t color);
void manual_canvas_date_digits(const manual_canvas_t *canvas, int32_t x,
                               int32_t y, const char *text, uint8_t color);

/* Small procedural icons keep weather/status semantics visible without an
 * icon font or another image buffer. */
void manual_canvas_weather_icon(const manual_canvas_t *canvas, int32_t x,
                                int32_t y, uint8_t kind, uint8_t color,
                                int scale);
void manual_canvas_wifi_icon(const manual_canvas_t *canvas, int32_t x,
                             int32_t y, bool connected, uint8_t color,
                             int scale);

#ifdef __cplusplus
}
#endif

#endif /* MANUAL_CANVAS_H */
