/* Magic Wand ESP32-S3 N16R8 - App configuration */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-S3 N16R8 I2C pins for MPU6050 (adjust to your wiring) */
#define I2C_MASTER_SCL_IO           8
#define I2C_MASTER_SDA_IO          9
#define I2C_MASTER_FREQ_HZ         400000
#define I2C_MASTER_NUM             0
#define MPU6050_I2C_ADDR           0x68

/* Stroke / model dimensions (must match trained model) */
#define STROKE_TRANSMIT_STRIDE     2
#define STROKE_TRANSMIT_MAX_LEN    160
#define STROKE_MAX_LENGTH          (STROKE_TRANSMIT_MAX_LEN * STROKE_TRANSMIT_STRIDE)
#define MOVING_SAMPLE_COUNT        50
#define GYRO_DATA_LENGTH           (600 * 3)
#define ACCEL_DATA_LENGTH          (600 * 3)

#define RASTER_WIDTH               32
#define RASTER_HEIGHT              32
#define RASTER_CHANNELS            3
#define RASTER_BYTE_COUNT          (RASTER_HEIGHT * RASTER_WIDTH * RASTER_CHANNELS)

/* Labels: update after training to match your gesture set */
#define LABEL_COUNT                10
#define LABELS                     "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"

/* Tensor arena size for TFLite (may need tuning for your model) */
#define TENSOR_ARENA_SIZE          (30 * 1024)

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
