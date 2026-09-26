/* Host-testable four-colour EPD conversion helpers. */
#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "rotation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_EPD_WIDTH 768
#define DISPLAY_EPD_HEIGHT 552

/* EPD 2bpp values; these match epd_config.h but this header has no ESP-IDF
 * dependency so it can be compiled by a host unit-test runner. */
typedef enum {
    DISPLAY_COLOR_BLACK = 0,
    DISPLAY_COLOR_WHITE = 1,
    DISPLAY_COLOR_YELLOW = 2,
    DISPLAY_COLOR_RED = 3,
} display_color_t;

/* RGB565 compatibility helper for image uploads/tools. */
display_color_t display_quantize_rgb565(uint16_t rgb565);
display_color_t display_quantize_rgb888(uint8_t red, uint8_t green,
                                        uint8_t blue);

/* Convert count pixels. src and dst may not overlap. */
void display_quantize_rgb565_buffer(const uint16_t *src, uint8_t *dst,
                                    size_t count);

/* Four 2-bit pixels, leftmost first, packed in panel wire order. */
uint8_t display_pack_2bpp(display_color_t p0, display_color_t p1,
                          display_color_t p2, display_color_t p3);

/* Quantize and pack one RGB565 scanline. width need not be a multiple of four;
 * unused trailing pixels are white. Returns bytes written, or zero on invalid
 * arguments. */
size_t display_quantize_rgb565_line(const uint16_t *src, size_t width,
                                    uint8_t *dst, size_t dst_capacity);

/* Pack a 2D RGB565 image. src_stride is measured in pixels and dst_stride in
 * bytes; both may be larger than the visible row. Returns zero on invalid
 * arguments or insufficient destination stride. */
size_t display_quantize_rgb565_frame(const uint16_t *src, size_t width,
                                     size_t height, size_t src_stride,
                                     uint8_t *dst, size_t dst_stride);

/* Convenience mapping for a logical pixel to its native panel address. */
int display_map_lvgl_point(display_rotation_t rotation, int32_t x, int32_t y,
                           display_point_t *native);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_DRIVER_H */
