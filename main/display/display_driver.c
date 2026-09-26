#include "display_driver.h"

#include <limits.h>

static uint8_t expand5(uint8_t value)
{
    return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t expand6(uint8_t value)
{
    return (uint8_t)((value << 2) | (value >> 4));
}

display_color_t display_quantize_rgb888(uint8_t red, uint8_t green,
                                        uint8_t blue)
{
    /* Squared RGB distance avoids floating point and is sufficient for the
     * small fixed palette.  Tie-breaking follows palette order. */
    static const uint8_t palette[][3] = {
        {0, 0, 0},       /* black */
        {255, 255, 255}, /* white */
        {255, 255, 0},   /* yellow */
        {255, 0, 0},     /* red */
    };
    uint32_t best_distance = UINT_MAX;
    display_color_t best = DISPLAY_COLOR_BLACK;
    for (display_color_t color = DISPLAY_COLOR_BLACK;
         color <= DISPLAY_COLOR_RED; color++) {
        const int dr = (int)red - palette[color][0];
        const int dg = (int)green - palette[color][1];
        const int db = (int)blue - palette[color][2];
        const uint32_t distance = (uint32_t)(dr * dr + dg * dg + db * db);
        bool wins_tie = false;
        if (distance == best_distance) {
            /* The four-colour palette has unavoidable equidistant points.
             * Prefer the chromatic ink when the source hue clearly points at
             * it (for example RGB green is closer semantically to yellow than
             * to black), while keeping neutral mid-grey on the black side. */
            if (color == DISPLAY_COLOR_YELLOW && green > 160 &&
                blue < 160 && (red + green) > 160) {
                wins_tie = true;
            } else if (color == DISPLAY_COLOR_RED && red > 160 &&
                       green < 160 && blue < 160) {
                wins_tie = true;
            } else if (color == DISPLAY_COLOR_WHITE && red > 160 &&
                       green > 160 && blue > 160) {
                wins_tie = true;
            }
        }
        if (distance < best_distance || wins_tie) {
            best_distance = distance;
            best = color;
        }
    }
    return best;
}

display_color_t display_quantize_rgb565(uint16_t rgb565)
{
    const uint8_t red = expand5((uint8_t)((rgb565 >> 11) & 0x1f));
    const uint8_t green = expand6((uint8_t)((rgb565 >> 5) & 0x3f));
    const uint8_t blue = expand5((uint8_t)(rgb565 & 0x1f));
    return display_quantize_rgb888(red, green, blue);
}

void display_quantize_rgb565_buffer(const uint16_t *src, uint8_t *dst,
                                    size_t count)
{
    if (src == NULL || dst == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        dst[i] = (uint8_t)display_quantize_rgb565(src[i]);
    }
}

uint8_t display_pack_2bpp(display_color_t p0, display_color_t p1,
                          display_color_t p2, display_color_t p3)
{
    return (uint8_t)(((uint8_t)p0 & 0x03u) << 6 |
                     ((uint8_t)p1 & 0x03u) << 4 |
                     ((uint8_t)p2 & 0x03u) << 2 |
                     ((uint8_t)p3 & 0x03u));
}

size_t display_quantize_rgb565_line(const uint16_t *src, size_t width,
                                    uint8_t *dst, size_t dst_capacity)
{
    if (src == NULL || dst == NULL || width == 0) {
        return 0;
    }
    if (width > SIZE_MAX - 3u) {
        return 0;
    }
    const size_t bytes = (width + 3u) / 4u;
    if (dst_capacity < bytes) {
        return 0;
    }
    for (size_t i = 0; i < bytes; ++i) {
        display_color_t pixel[4] = {DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                                     DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE};
        for (size_t j = 0; j < 4; ++j) {
            const size_t x = i * 4u + j;
            if (x < width) {
                pixel[j] = display_quantize_rgb565(src[x]);
            }
        }
        dst[i] = display_pack_2bpp(pixel[0], pixel[1], pixel[2], pixel[3]);
    }
    return bytes;
}

size_t display_quantize_rgb565_frame(const uint16_t *src, size_t width,
                                     size_t height, size_t src_stride,
                                     uint8_t *dst, size_t dst_stride)
{
    if (src == NULL || dst == NULL || width == 0 || height == 0 ||
        src_stride < width) {
        return 0;
    }
    if (width > SIZE_MAX - 3u) {
        return 0;
    }
    const size_t row_bytes = (width + 3u) / 4u;
    if (dst_stride < row_bytes) {
        return 0;
    }
    if (height > SIZE_MAX / src_stride || height > SIZE_MAX / dst_stride) {
        return 0;
    }
    for (size_t y = 0; y < height; ++y) {
        if (display_quantize_rgb565_line(src + y * src_stride, width,
                                         dst + y * dst_stride,
                                         dst_stride) != row_bytes) {
            return 0;
        }
    }
    return row_bytes * height;
}

int display_map_lvgl_point(display_rotation_t rotation, int32_t x, int32_t y,
                           display_point_t *native)
{
    return display_rotation_map_point(rotation, DISPLAY_EPD_WIDTH,
                                      DISPLAY_EPD_HEIGHT, x, y, native)
               ? 0
               : -1;
}
