/* MPU6050 IMU driver for ESP32-S3 */

#include "imu_mpu6050.h"
#include "app_config.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mpu6050";
static bool s_initialized = false;

#define I2C_MASTER_TX_BUF_DISABLE 0
#define I2C_MASTER_RX_BUF_DISABLE 0

#define MPU6050_REG_PWR_MGMT_1    0x6B
#define MPU6050_REG_SMPLRT_DIV    0x19
#define MPU6050_REG_CONFIG        0x1A
#define MPU6050_REG_GYRO_CONFIG   0x1B
#define MPU6050_REG_ACCEL_CONFIG  0x1C
#define MPU6050_REG_WHO_AM_I      0x75
#define MPU6050_REG_ACCEL_XOUT_H  0x3B
#define MPU6050_REG_GYRO_XOUT_H   0x43

#define MPU6050_PWR_WAKE          0x00
#define MPU6050_GYRO_FS_250       0x00   /* +/- 250 deg/s */
#define MPU6050_ACCEL_FS_2G       0x00  /* +/- 2g */
#define GYRO_SCALE_250            131.0f
#define ACCEL_SCALE_2G            16384.0f

static esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode,
                              I2C_MASTER_RX_BUF_DISABLE,
                              I2C_MASTER_TX_BUF_DISABLE, 0);
}

static esp_err_t mpu6050_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_I2C_ADDR,
                                     buf, 2, pdMS_TO_TICKS(100));
}

static esp_err_t mpu6050_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_I2C_ADDR,
                                       &reg, 1, buf, len, pdMS_TO_TICKS(100));
}

static void swap_bytes_16(uint8_t *buf)
{
    uint8_t t = buf[0];
    buf[0] = buf[1];
    buf[1] = t;
}

bool imu_mpu6050_init(void)
{
    if (s_initialized) return true;

    esp_err_t err = i2c_master_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t who;
    err = mpu6050_read_regs(MPU6050_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK || (who != 0x68 && who != 0x72)) {
        ESP_LOGE(TAG, "MPU6050 not found (WHO_AM_I=0x%02x)", who);
        return false;
    }

    if (mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, MPU6050_PWR_WAKE) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (mpu6050_write_reg(MPU6050_REG_SMPLRT_DIV, 0x00) != ESP_OK) return false;  /* 1kHz */
    if (mpu6050_write_reg(MPU6050_REG_CONFIG, 0x06) != ESP_OK) return false;         /* 5Hz BW */
    if (mpu6050_write_reg(MPU6050_REG_GYRO_CONFIG, MPU6050_GYRO_FS_250) != ESP_OK) return false;
    if (mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG, MPU6050_ACCEL_FS_2G) != ESP_OK) return false;

    s_initialized = true;
    ESP_LOGI(TAG, "MPU6050 initialized");
    return true;
}

bool imu_mpu6050_read(float *gyro_x_deg_s, float *gyro_y_deg_s, float *gyro_z_deg_s,
                      float *accel_x_g, float *accel_y_g, float *accel_z_g)
{
    if (!s_initialized) return false;

    uint8_t buf[14];
    esp_err_t err = mpu6050_read_regs(MPU6050_REG_ACCEL_XOUT_H, buf, 14);
    if (err != ESP_OK) return false;

    int16_t ax, ay, az, gx, gy, gz;
    swap_bytes_16(buf + 0);
    swap_bytes_16(buf + 2);
    swap_bytes_16(buf + 4);
    memcpy(&ax, buf + 0, 2);
    memcpy(&ay, buf + 2, 2);
    memcpy(&az, buf + 4, 2);
    swap_bytes_16(buf + 8);
    swap_bytes_16(buf + 10);
    swap_bytes_16(buf + 12);
    memcpy(&gx, buf + 8, 2);
    memcpy(&gy, buf + 10, 2);
    memcpy(&gz, buf + 12, 2);

    *accel_x_g = (float)ax / ACCEL_SCALE_2G;
    *accel_y_g = (float)ay / ACCEL_SCALE_2G;
    *accel_z_g = (float)az / ACCEL_SCALE_2G;
    *gyro_x_deg_s = (float)gx / GYRO_SCALE_250;
    *gyro_y_deg_s = (float)gy / GYRO_SCALE_250;
    *gyro_z_deg_s = (float)gz / GYRO_SCALE_250;

    return true;
}

bool imu_mpu6050_available(void)
{
    return s_initialized;
}
