#include "rotation.h"

#include <stddef.h>

bool display_rotation_valid(display_rotation_t rotation)
{
    return rotation == ROTATION_0 || rotation == ROTATION_90 ||
           rotation == ROTATION_180 || rotation == ROTATION_270;
}

display_rotation_t display_rotation_normalize(int degrees)
{
    int value = degrees % 360;
    if (value < 0) {
        value += 360;
    }
    switch (value) {
    case 90:
        return ROTATION_90;
    case 180:
        return ROTATION_180;
    case 270:
        return ROTATION_270;
    default:
        return ROTATION_0;
    }
}

int display_rotation_degrees(display_rotation_t rotation)
{
    return display_rotation_valid(rotation) ? (int)rotation : 0;
}

void display_rotation_dimensions(display_rotation_t rotation, int32_t width,
                                 int32_t height, int32_t *out_width,
                                 int32_t *out_height)
{
    if (out_width == NULL || out_height == NULL) {
        return;
    }
    if (!display_rotation_valid(rotation) || width <= 0 || height <= 0) {
        *out_width = 0;
        *out_height = 0;
        return;
    }
    if (rotation == ROTATION_90 || rotation == ROTATION_270) {
        *out_width = height;
        *out_height = width;
    } else {
        *out_width = width;
        *out_height = height;
    }
}

bool display_rotation_map_point(display_rotation_t rotation, int32_t width,
                                int32_t height, int32_t x, int32_t y,
                                display_point_t *out)
{
    if (out == NULL || width <= 0 || height <= 0 || x < 0 || y < 0) {
        return false;
    }
    int32_t logical_width;
    int32_t logical_height;
    display_rotation_dimensions(rotation, width, height, &logical_width,
                                &logical_height);
    if (x >= logical_width || y >= logical_height ||
        !display_rotation_valid(rotation)) {
        return false;
    }
    switch (rotation) {
    case ROTATION_90:
        /* Logical space is H x W after a clockwise quarter turn. */
        out->x = y;
        out->y = height - 1 - x;
        break;
    case ROTATION_180:
        out->x = width - 1 - x;
        out->y = height - 1 - y;
        break;
    case ROTATION_270:
        out->x = width - 1 - y;
        out->y = x;
        break;
    case ROTATION_0:
    default:
        out->x = x;
        out->y = y;
        break;
    }
    return true;
}

bool display_rotation_unmap_point(display_rotation_t rotation, int32_t width,
                                  int32_t height, int32_t x, int32_t y,
                                  display_point_t *out)
{
    if (out == NULL || width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x >= width || y >= height || !display_rotation_valid(rotation)) {
        return false;
    }
    switch (rotation) {
    case ROTATION_90:
        out->x = height - 1 - y;
        out->y = x;
        break;
    case ROTATION_180:
        out->x = width - 1 - x;
        out->y = height - 1 - y;
        break;
    case ROTATION_270:
        out->x = y;
        out->y = width - 1 - x;
        break;
    case ROTATION_0:
    default:
        out->x = x;
        out->y = y;
        break;
    }
    return true;
}

bool display_rotation_map_rect(display_rotation_t rotation, int32_t width,
                               int32_t height, const display_rect_t *rect,
                               display_rect_t *out)
{
    if (rect == NULL || out == NULL || rect->width <= 0 || rect->height <= 0 ||
        rect->x < 0 || rect->y < 0) {
        return false;
    }
    const int32_t logical_width = rotation == ROTATION_90 || rotation == ROTATION_270
                                      ? height
                                      : width;
    const int32_t logical_height = rotation == ROTATION_90 || rotation == ROTATION_270
                                       ? width
                                       : height;
    if (rect->x >= logical_width || rect->y >= logical_height ||
        rect->width > logical_width - rect->x ||
        rect->height > logical_height - rect->y ||
        !display_rotation_valid(rotation)) {
        return false;
    }
    display_point_t p[4];
    if (!display_rotation_map_point(rotation, width, height, rect->x, rect->y,
                                    &p[0]) ||
        !display_rotation_map_point(rotation, width, height,
                                    rect->x + rect->width - 1, rect->y,
                                    &p[1]) ||
        !display_rotation_map_point(rotation, width, height, rect->x,
                                    rect->y + rect->height - 1, &p[2]) ||
        !display_rotation_map_point(rotation, width, height,
                                    rect->x + rect->width - 1,
                                    rect->y + rect->height - 1, &p[3])) {
        return false;
    }
    int32_t min_x = p[0].x, max_x = p[0].x;
    int32_t min_y = p[0].y, max_y = p[0].y;
    for (size_t i = 1; i < 4; ++i) {
        if (p[i].x < min_x) min_x = p[i].x;
        if (p[i].x > max_x) max_x = p[i].x;
        if (p[i].y < min_y) min_y = p[i].y;
        if (p[i].y > max_y) max_y = p[i].y;
    }
    *out = (display_rect_t){min_x, min_y, max_x - min_x + 1,
                            max_y - min_y + 1};
    return true;
}
