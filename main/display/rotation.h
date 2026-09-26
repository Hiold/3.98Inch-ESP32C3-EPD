/* Software display rotation and coordinate transforms. */
#ifndef DISPLAY_ROTATION_H
#define DISPLAY_ROTATION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROTATION_0 = 0,
    ROTATION_90 = 90,
    ROTATION_180 = 180,
    ROTATION_270 = 270,
} display_rotation_t;

typedef struct {
    int32_t x;
    int32_t y;
} display_point_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} display_rect_t;

bool display_rotation_valid(display_rotation_t rotation);
display_rotation_t display_rotation_normalize(int degrees);
int display_rotation_degrees(display_rotation_t rotation);
void display_rotation_dimensions(display_rotation_t rotation, int32_t width,
                                 int32_t height, int32_t *out_width,
                                 int32_t *out_height);

/* Map a point from the rotated (logical/LVGL) coordinate space to the native
 * panel space. The logical dimensions are the dimensions returned by
 * display_rotation_dimensions(). */
bool display_rotation_map_point(display_rotation_t rotation, int32_t width,
                                int32_t height, int32_t x, int32_t y,
                                display_point_t *out);

/* Inverse of display_rotation_map_point: native panel to logical coordinates. */
bool display_rotation_unmap_point(display_rotation_t rotation, int32_t width,
                                  int32_t height, int32_t x, int32_t y,
                                  display_point_t *out);

/* Transform an axis-aligned logical rectangle and return its native bounding
 * rectangle. */
bool display_rotation_map_rect(display_rotation_t rotation, int32_t width,
                               int32_t height, const display_rect_t *rect,
                               display_rect_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_ROTATION_H */
