/* Rasterize stroke points to 32x32x3 int8 image (Magic Wand compatible) */

#ifndef RASTERIZE_STROKE_H
#define RASTERIZE_STROKE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rasterize_stroke(
    int8_t *stroke_points,
    int stroke_points_count,
    float x_range,
    float y_range,
    int width,
    int height,
    int8_t *out_buffer);

#ifdef __cplusplus
}
#endif

#endif /* RASTERIZE_STROKE_H */
