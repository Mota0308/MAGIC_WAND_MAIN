/* MPU6050 IMU driver for ESP32-S3 - Magic Wand */

#ifndef IMU_MPU6050_H
#define IMU_MPU6050_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMU_MPU6050_GYRO_DIM   3
#define IMU_MPU6050_ACCEL_DIM  3

/* Initialize I2C and MPU6050. Returns true on success. */
bool imu_mpu6050_init(void);

/* Read one gyroscope sample (deg/s) and one accelerometer sample (g). */
bool imu_mpu6050_read(float *gyro_x_deg_s, float *gyro_y_deg_s, float *gyro_z_deg_s,
                      float *accel_x_g, float *accel_y_g, float *accel_z_g);

/* Check if new data is available (optional, for polling). */
bool imu_mpu6050_available(void);

#ifdef __cplusplus
}
#endif

#endif /* IMU_MPU6050_H */
