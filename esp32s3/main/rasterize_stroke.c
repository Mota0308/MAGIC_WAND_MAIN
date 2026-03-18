/* Rasterize stroke to image - C port from Magic Wand (TensorFlow) */

#include "rasterize_stroke.h"
#include <string.h>

#define K_FIXED_POINT 256

static int32_t mul_fp(int32_t a, int32_t b)
{
    return (a * b) / K_FIXED_POINT;
}

static int32_t div_fp(int32_t a, int32_t b)
{
    if (b == 0) b = 1;
    return (a * K_FIXED_POINT) / b;
}

static int32_t float_to_fp(float a)
{
    return (int32_t)(a * K_FIXED_POINT);
}

static int32_t norm_to_coord_fp(int32_t a_fp, int32_t range_fp, int32_t half_size_fp)
{
    int32_t norm_fp = div_fp(a_fp, range_fp);
    return mul_fp(norm_fp, half_size_fp) + half_size_fp;
}

static int32_t round_fp_to_int(int32_t a)
{
    return (int32_t)((a + (K_FIXED_POINT / 2)) / K_FIXED_POINT);
}

static int32_t gate(int32_t a, int32_t min_val, int32_t max_val)
{
    if (a < min_val) return min_val;
    if (a > max_val) return max_val;
    return a;
}

static int32_t abs_i32(int32_t a)
{
    return a > 0 ? a : -a;
}

void rasterize_stroke(
    int8_t *stroke_points,
    int stroke_points_count,
    float x_range,
    float y_range,
    int width,
    int height,
    int8_t *out_buffer)
{
    const int num_channels = 3;
    const int buffer_byte_count = height * width * num_channels;

    for (int i = 0; i < buffer_byte_count; i++)
        out_buffer[i] = -128;

    const int32_t width_fp = width * K_FIXED_POINT;
    const int32_t height_fp = height * K_FIXED_POINT;
    const int32_t half_width_fp = width_fp / 2;
    const int32_t half_height_fp = height_fp / 2;
    const int32_t x_range_fp = float_to_fp(x_range);
    const int32_t y_range_fp = float_to_fp(y_range);

    const int t_inc_fp = stroke_points_count > 1 ? (K_FIXED_POINT / stroke_points_count) : K_FIXED_POINT;
    const int32_t one_half_fp = (K_FIXED_POINT / 2);

    for (int point_index = 0; point_index < stroke_points_count - 1; point_index++) {
        const int8_t *start_point = &stroke_points[point_index * 2];
        int32_t start_point_x_fp = (start_point[0] * K_FIXED_POINT) / 128;
        int32_t start_point_y_fp = (start_point[1] * K_FIXED_POINT) / 128;

        const int8_t *end_point = &stroke_points[(point_index + 1) * 2];
        int32_t end_point_x_fp = (end_point[0] * K_FIXED_POINT) / 128;
        int32_t end_point_y_fp = (end_point[1] * K_FIXED_POINT) / 128;

        int32_t start_x_fp = norm_to_coord_fp(start_point_x_fp, x_range_fp, half_width_fp);
        int32_t start_y_fp = norm_to_coord_fp(-start_point_y_fp, y_range_fp, half_height_fp);
        int32_t end_x_fp = norm_to_coord_fp(end_point_x_fp, x_range_fp, half_width_fp);
        int32_t end_y_fp = norm_to_coord_fp(-end_point_y_fp, y_range_fp, half_height_fp);
        int32_t delta_x_fp = end_x_fp - start_x_fp;
        int32_t delta_y_fp = end_y_fp - start_y_fp;

        int32_t t_fp = point_index * t_inc_fp;
        int32_t red_i32, green_i32, blue_i32;
        if (t_fp < one_half_fp) {
            int32_t local_t_fp = div_fp(t_fp, one_half_fp);
            int32_t one_minus_t_fp = K_FIXED_POINT - local_t_fp;
            red_i32 = round_fp_to_int(one_minus_t_fp * 255) - 128;
            green_i32 = round_fp_to_int(local_t_fp * 255) - 128;
            blue_i32 = -128;
        } else {
            int32_t local_t_fp = div_fp(t_fp - one_half_fp, one_half_fp);
            int32_t one_minus_t_fp = K_FIXED_POINT - local_t_fp;
            red_i32 = -128;
            green_i32 = round_fp_to_int(one_minus_t_fp * 255) - 128;
            blue_i32 = round_fp_to_int(local_t_fp * 255) - 128;
        }
        int8_t red_i8 = (int8_t)gate(red_i32, -128, 127);
        int8_t green_i8 = (int8_t)gate(green_i32, -128, 127);
        int8_t blue_i8 = (int8_t)gate(blue_i32, -128, 127);

        int line_length;
        int32_t x_inc_fp, y_inc_fp;
        if (abs_i32(delta_x_fp) > abs_i32(delta_y_fp)) {
            line_length = abs_i32(round_fp_to_int(delta_x_fp));
            if (delta_x_fp > 0) {
                x_inc_fp = 1 * K_FIXED_POINT;
                y_inc_fp = div_fp(delta_y_fp, delta_x_fp);
            } else {
                x_inc_fp = -1 * K_FIXED_POINT;
                y_inc_fp = -div_fp(delta_y_fp, delta_x_fp);
            }
        } else {
            line_length = abs_i32(round_fp_to_int(delta_y_fp));
            if (delta_y_fp > 0) {
                y_inc_fp = 1 * K_FIXED_POINT;
                x_inc_fp = div_fp(delta_x_fp, delta_y_fp);
            } else {
                y_inc_fp = -1 * K_FIXED_POINT;
                x_inc_fp = -div_fp(delta_x_fp, delta_y_fp);
            }
        }

        for (int i = 0; i <= line_length; i++) {
            int32_t x_fp = start_x_fp + (i * x_inc_fp);
            int32_t y_fp = start_y_fp + (i * y_inc_fp);
            int x = round_fp_to_int(x_fp);
            int y = round_fp_to_int(y_fp);
            if (x < 0 || x >= width || y < 0 || y >= height)
                continue;
            int buffer_index = (y * width * num_channels) + (x * num_channels);
            out_buffer[buffer_index + 0] = red_i8;
            out_buffer[buffer_index + 1] = green_i8;
            out_buffer[buffer_index + 2] = blue_i8;
        }
    }
}
