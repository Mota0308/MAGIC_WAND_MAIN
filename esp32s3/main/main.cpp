/* Magic Wand - ESP32-S3 N16R8 with MPU6050 and TFLite Micro */

#include <cmath>
#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_mpu6050.h"
#include "model_data.h"
#include "rasterize_stroke.h"

/* TFLite Micro - esp-tflite-micro component */
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"

static const char *TAG = "magic_wand";

enum StrokeState { eWaiting = 0, eDrawing = 1, eDone = 2 };

/* Sample rate assumed for MPU6050 (adjust if you change IMU config) */
static const float GYRO_SAMPLE_RATE_HZ = 100.0f;

/* Buffers */
static float acceleration_data[ACCEL_DATA_LENGTH];
static float gyroscope_data[GYRO_DATA_LENGTH];
static float orientation_data[GYRO_DATA_LENGTH];
static int acceleration_data_index = 0;
static int gyroscope_data_index = 0;

static float current_velocity[3] = {0.0f, 0.0f, 0.0f};
static float current_gravity[3] = {0.0f, 0.0f, 0.0f};
static float current_gyroscope_drift[3] = {0.0f, 0.0f, 0.0f};

static int32_t stroke_length = 0;
static int32_t stroke_state = eWaiting;
static int32_t stroke_transmit_length = 0;
static int8_t stroke_points[STROKE_TRANSMIT_MAX_LEN * 2];
static int8_t raster_buffer[RASTER_BYTE_COUNT];

static uint8_t tensor_arena[TENSOR_ARENA_SIZE];

static tflite::MicroErrorReporter micro_error_reporter;
static tflite::MicroInterpreter *interpreter = nullptr;
static const tflite::Model *model = nullptr;

static const char *labels[] = { LABELS };

static float vector_magnitude(const float *vec) {
    float x = vec[0], y = vec[1], z = vec[2];
    return sqrtf(x * x + y * y + z * z);
}

static void estimate_gravity(float *gravity) {
    int n = 100;
    if (n * 3 > acceleration_data_index) n = acceleration_data_index / 3;
    if (n <= 0) return;

    int start = (acceleration_data_index + ACCEL_DATA_LENGTH - 3 * (n + 1)) % ACCEL_DATA_LENGTH;
    float sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < n; i++) {
        int idx = (start + i * 3) % ACCEL_DATA_LENGTH;
        sx += acceleration_data[idx + 0];
        sy += acceleration_data[idx + 1];
        sz += acceleration_data[idx + 2];
    }
    gravity[0] = sx / n;
    gravity[1] = sy / n;
    gravity[2] = sz / n;
}

static void update_velocity(int new_samples, const float *gravity) {
    float gx = gravity[0], gy = gravity[1], gz = gravity[2];
    int start = (acceleration_data_index + ACCEL_DATA_LENGTH - 3 * (new_samples + 1)) % ACCEL_DATA_LENGTH;
    const float friction = 0.98f;

    for (int i = 0; i < new_samples; i++) {
        int idx = (start + i * 3) % ACCEL_DATA_LENGTH;
        float ax = acceleration_data[idx + 0] - gx;
        float ay = acceleration_data[idx + 1] - gy;
        float az = acceleration_data[idx + 2] - gz;
        current_velocity[0] += ax;
        current_velocity[1] += ay;
        current_velocity[2] += az;
        current_velocity[0] *= friction;
        current_velocity[1] *= friction;
        current_velocity[2] *= friction;
    }
}

static void estimate_gyro_drift(float *drift) {
    if (vector_magnitude(current_velocity) > 0.1f) return;
    int n = 20;
    if (n * 3 > gyroscope_data_index) n = gyroscope_data_index / 3;
    if (n <= 0) return;

    int start = (gyroscope_data_index + GYRO_DATA_LENGTH - 3 * (n + 1)) % GYRO_DATA_LENGTH;
    float sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < n; i++) {
        int idx = (start + i * 3) % GYRO_DATA_LENGTH;
        sx += gyroscope_data[idx + 0];
        sy += gyroscope_data[idx + 1];
        sz += gyroscope_data[idx + 2];
    }
    drift[0] = sx / n;
    drift[1] = sy / n;
    drift[2] = sz / n;
}

static void update_orientation(int new_samples, const float *gravity, const float *drift) {
    float rx = 1.0f / GYRO_SAMPLE_RATE_HZ;
    int start = (gyroscope_data_index + GYRO_DATA_LENGTH - 3 * new_samples) % GYRO_DATA_LENGTH;

    for (int i = 0; i < new_samples; i++) {
        int idx = (start + i * 3) % GYRO_DATA_LENGTH;
        float dx = (gyroscope_data[idx + 0] - drift[0]) * rx;
        float dy = (gyroscope_data[idx + 1] - drift[1]) * rx;
        float dz = (gyroscope_data[idx + 2] - drift[2]) * rx;
        int prev = (idx + GYRO_DATA_LENGTH - 3) % GYRO_DATA_LENGTH;
        orientation_data[idx + 0] = orientation_data[prev + 0] + dx;
        orientation_data[idx + 1] = orientation_data[prev + 1] + dy;
        orientation_data[idx + 2] = orientation_data[prev + 2] + dz;
    }
}

static bool is_moving(int samples_before) {
    const float threshold = 10.0f;
    if (gyroscope_data_index - samples_before < MOVING_SAMPLE_COUNT * 3) return false;

    int start = (gyroscope_data_index + GYRO_DATA_LENGTH - 3 * (MOVING_SAMPLE_COUNT + samples_before)) % GYRO_DATA_LENGTH;
    float total = 0;
    for (int i = 0; i < MOVING_SAMPLE_COUNT; i++) {
        int idx = (start + i * 3) % GYRO_DATA_LENGTH;
        int prev = (idx + GYRO_DATA_LENGTH - 3) % GYRO_DATA_LENGTH;
        float dx = orientation_data[idx + 0] - orientation_data[prev + 0];
        float dy = orientation_data[idx + 1] - orientation_data[prev + 1];
        float dz = orientation_data[idx + 2] - orientation_data[prev + 2];
        total += dx * dx + dy * dy + dz * dz;
    }
    return total > threshold;
}

static void update_stroke(int new_samples, bool *done_just_triggered) {
    const int min_stroke_len = MOVING_SAMPLE_COUNT + 10;
    const float min_stroke_size = 0.2f;
    *done_just_triggered = false;

    for (int i = 0; i < new_samples; i++) {
        int current_head = new_samples - (i + 1);
        bool moving = is_moving(current_head);
        int32_t old_state = stroke_state;

        if (old_state == eWaiting || old_state == eDone) {
            if (moving) {
                stroke_length = MOVING_SAMPLE_COUNT;
                stroke_state = eDrawing;
            }
        } else if (old_state == eDrawing) {
            if (!moving) {
                if (stroke_length > min_stroke_len)
                    stroke_state = eDone;
                else {
                    stroke_length = 0;
                    stroke_state = eWaiting;
                }
            }
        }

        if (stroke_state == eWaiting) continue;

        stroke_length++;
        if (stroke_length > STROKE_MAX_LENGTH) stroke_length = STROKE_MAX_LENGTH;

        bool draw_last = (i == new_samples - 1) && (stroke_state == eDrawing);
        *done_just_triggered = (old_state != eDone && stroke_state == eDone);
        if (!*done_just_triggered && !draw_last) continue;

        int start_index = (gyroscope_data_index + GYRO_DATA_LENGTH - 3 * (stroke_length + current_head)) % GYRO_DATA_LENGTH;

        float x_total = 0, y_total = 0, z_total = 0;
        for (int j = 0; j < stroke_length; j++) {
            int idx = (start_index + j * 3) % GYRO_DATA_LENGTH;
            x_total += orientation_data[idx + 0];
            y_total += orientation_data[idx + 1];
            z_total += orientation_data[idx + 2];
        }
        float x_mean = x_total / stroke_length;
        float y_mean = y_total / stroke_length;
        float z_mean = z_total / stroke_length;
        const float range = 90.0f;

        float gy = current_gravity[1], gz = current_gravity[2];
        float gmag = sqrtf(gy * gy + gz * gz);
        if (gmag < 0.0001f) gmag = 0.0001f;
        float ngy = gy / gmag, ngz = gz / gmag;
        float xaxisz = -ngz, xaxisy = -ngy;
        float yaxisz = -ngy, yaxisy = ngz;

        stroke_transmit_length = stroke_length / STROKE_TRANSMIT_STRIDE;

        float x_min = 0, y_min = 0, x_max = 0, y_max = 0;
        bool first = true;

        for (int j = 0; j < stroke_transmit_length; j++) {
            int oidx = (start_index + (j * STROKE_TRANSMIT_STRIDE) * 3) % GYRO_DATA_LENGTH;
            float ox = orientation_data[oidx + 0];
            float oy = orientation_data[oidx + 1];
            float oz = orientation_data[oidx + 2];
            float nx = (ox - x_mean) / range;
            float ny = (oy - y_mean) / range;
            float nz = (oz - z_mean) / range;
            float x_axis = xaxisz * nz + xaxisy * ny;
            float y_axis = yaxisz * nz + yaxisy * ny;

            int sx = (int)roundf(x_axis * 128.0f);
            int sy = (int)roundf(y_axis * 128.0f);
            if (sx > 127) sx = 127;
            if (sx < -128) sx = -128;
            if (sy > 127) sy = 127;
            if (sy < -128) sy = -128;
            stroke_points[j * 2 + 0] = (int8_t)sx;
            stroke_points[j * 2 + 1] = (int8_t)sy;

            if (first || x_axis < x_min) x_min = x_axis;
            if (first || y_axis < y_min) y_min = y_axis;
            if (first || x_axis > x_max) x_max = x_axis;
            if (first || y_axis > y_max) y_max = y_axis;
            first = false;
        }

        if (*done_just_triggered) {
            float xr = x_max - x_min, yr = y_max - y_min;
            if (xr < min_stroke_size && yr < min_stroke_size) {
                *done_just_triggered = false;
                stroke_state = eWaiting;
                stroke_transmit_length = 0;
                stroke_length = 0;
            }
        }
    }
}

static bool init_tflite(void) {
    if (g_magic_wand_model_data_len < 1000) {
        ESP_LOGE(TAG, "Model too small. Replace main/model_data.c with your trained model.");
        ESP_LOGE(TAG, "Run: python3 train_esp32s3/tflite_to_c_array.py quantized_model.tflite main/model_data.c");
        return false;
    }

    model = tflite::GetModel(g_magic_wand_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema version %d != supported %d", model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    static tflite::MicroMutableOpResolver<4> resolver;
    resolver.AddConv2D();
    resolver.AddMean();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, TENSOR_ARENA_SIZE, &micro_error_reporter);
    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        return false;
    }

    TfLiteTensor *input = interpreter->input(0);
    if (input->dims->size != 4 || input->dims->data[1] != RASTER_HEIGHT ||
        input->dims->data[2] != RASTER_WIDTH || input->dims->data[3] != RASTER_CHANNELS) {
        ESP_LOGE(TAG, "Unexpected input shape");
        return false;
    }

    TfLiteTensor *output = interpreter->output(0);
    if (output->dims->size != 2 || output->dims->data[1] != LABEL_COUNT) {
        ESP_LOGE(TAG, "Unexpected output shape");
        return false;
    }

    ESP_LOGI(TAG, "TFLite Micro ready");
    return true;
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Magic Wand ESP32-S3 N16R8");

    if (!imu_mpu6050_init()) {
        ESP_LOGE(TAG, "IMU init failed. Check MPU6050 wiring (SDA=%d SCL=%d)", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
        return;
    }

    if (!init_tflite()) {
        return;
    }

    memset(acceleration_data, 0, sizeof(acceleration_data));
    memset(gyroscope_data, 0, sizeof(gyroscope_data));
    memset(orientation_data, 0, sizeof(orientation_data));

    const int poll_ms = 10;
    bool done_triggered = false;

    while (1) {
        float gx, gy, gz, ax, ay, az;
        int accel_count = 0, gyro_count = 0;

        while (imu_mpu6050_read(&gx, &gy, &gz, &ax, &ay, &az)) {
            int gidx = gyroscope_data_index % GYRO_DATA_LENGTH;
            gyroscope_data[gidx + 0] = gx;
            gyroscope_data[gidx + 1] = gy;
            gyroscope_data[gidx + 2] = gz;
            gyroscope_data_index += 3;
            gyro_count++;

            int aidx = acceleration_data_index % ACCEL_DATA_LENGTH;
            acceleration_data[aidx + 0] = ax;
            acceleration_data[aidx + 1] = ay;
            acceleration_data[aidx + 2] = az;
            acceleration_data_index += 3;
            accel_count++;
        }

        if (gyro_count > 0) {
            estimate_gyro_drift(current_gyroscope_drift);
            update_orientation(gyro_count, current_gravity, current_gyroscope_drift);
            update_stroke(gyro_count, &done_triggered);
        }
        if (accel_count > 0) {
            estimate_gravity(current_gravity);
            update_velocity(accel_count, current_gravity);
        }

        if (done_triggered && stroke_transmit_length > 0) {
            rasterize_stroke(stroke_points, stroke_transmit_length, 0.6f, 0.6f,
                            RASTER_WIDTH, RASTER_HEIGHT, raster_buffer);

            TfLiteTensor *input_tensor = interpreter->input(0);
            memcpy(input_tensor->data.int8, raster_buffer, RASTER_BYTE_COUNT);

            if (interpreter->Invoke() != kTfLiteOk) {
                ESP_LOGE(TAG, "Invoke failed");
            } else {
                TfLiteTensor *out = interpreter->output(0);
                int8_t max_score = -128;
                int max_idx = 0;
                for (int i = 0; i < LABEL_COUNT; i++) {
                    if (out->data.int8[i] > max_score) {
                        max_score = out->data.int8[i];
                        max_idx = i;
                    }
                }
                ESP_LOGI(TAG, "Gesture: %s (score %d)", labels[max_idx], (int)max_score);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}
