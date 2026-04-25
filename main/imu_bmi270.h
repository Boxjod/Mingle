/**
 * imu_bmi270.h - BMI270 6-axis IMU driver (raw I2C register access)
 *
 * Hardware: M5Stack AtomS3R built-in BMI270 + BMM150
 * I2C bus: GPIO45 (SDA) + GPIO0 (SCL) — independent from Audio and Camera I2C
 * Address: 0x68 (SDO=LOW default on AtomS3R)
 *
 * Usage: call imu_init() then imu_start_task()
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG_IMU = "imu";

// ============ I2C config ============
// BMI270 shares I2C bus with Audio (ES8311/PI4IOE) on GPIO 38/39, I2C_NUM_0
// No separate bus needed — uses g_i2c_bus from audio.h
#define IMU_I2C_FREQ_HZ   400000   // 400 kHz fast mode
#define BMI270_ADDR       0x68

// ============ BMI270 registers ============
#define BMI270_CHIP_ID         0x00   // expect 0x24
#define BMI270_ACC_X_LSB       0x0C   // 12 bytes: ACC XYZ + GYR XYZ
#define BMI270_INTERNAL_STATUS 0x21
#define BMI270_ACC_CONF        0x40
#define BMI270_ACC_RANGE       0x41
#define BMI270_GYR_CONF        0x42
#define BMI270_GYR_RANGE       0x43
#define BMI270_INIT_CTRL       0x59
#define BMI270_INIT_ADDR_0     0x5B
#define BMI270_INIT_ADDR_1     0x5C
#define BMI270_INIT_DATA       0x5E
#define BMI270_PWR_CONF        0x7C
#define BMI270_PWR_CTRL        0x7D

// Config blob from Bosch SensorAPI (bmi270_config.cpp)
extern "C" {
    extern const uint8_t bmi270_config_file[];
    extern const unsigned int bmi270_config_file_size;
}

// ============ I2C handle ============
// s_imu_bus removed — uses shared g_i2c_bus from audio.h
extern i2c_master_bus_handle_t g_i2c_bus;  // defined in audio.h
static i2c_master_dev_handle_t s_imu_dev = NULL;

// ============ I2C read/write helpers ============

static esp_err_t imu_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_imu_dev, buf, 2, 100);
}

static esp_err_t imu_write_burst(uint8_t reg, const uint8_t* data, size_t len) {
    // Prepend register address
    uint8_t* buf = (uint8_t*)malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    esp_err_t ret = i2c_master_transmit(s_imu_dev, buf, len + 1, 200);
    free(buf);
    return ret;
}

static uint8_t imu_read_reg(uint8_t reg) {
    uint8_t val = 0;
    uint8_t reg_addr = reg;
    i2c_master_transmit_receive(s_imu_dev, &reg_addr, 1, &val, 1, 100);
    return val;
}

static esp_err_t imu_read_burst(uint8_t reg, uint8_t* data, size_t len) {
    uint8_t reg_addr = reg;
    return i2c_master_transmit_receive(s_imu_dev, &reg_addr, 1, data, len, 100);
}

// ============ IMU data ============
struct ImuData {
    int16_t acc_x, acc_y, acc_z;   // raw (±2g default: 1 LSB = 0.061 mg)
    int16_t gyr_x, gyr_y, gyr_z;   // raw (±2000dps default: 1 LSB = 0.061 dps)
};

static volatile ImuData g_imu = {};

// ============ Init ============

static bool imu_init() {
    // Share I2C bus with Audio (g_i2c_bus on GPIO 38/39, I2C_NUM_0)
    if (!g_i2c_bus) {
        ESP_LOGE(TAG_IMU, "Audio I2C bus not ready — init audio first");
        return false;
    }

    // Add BMI270 device on shared bus
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = BMI270_ADDR;
    dev_cfg.scl_speed_hz = IMU_I2C_FREQ_HZ;

    esp_err_t err = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &s_imu_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_IMU, "BMI270 add device failed: %s", esp_err_to_name(err));
        return false;
    }

    // 1. Verify chip ID
    uint8_t chip_id = imu_read_reg(BMI270_CHIP_ID);
    if (chip_id != 0x24) {
        ESP_LOGE(TAG_IMU, "BMI270 not found! chip_id=0x%02X (expected 0x24)", chip_id);
        return false;
    }
    ESP_LOGI(TAG_IMU, "BMI270 detected (chip_id=0x%02X)", chip_id);

    // 2. Disable advanced power save
    imu_write_reg(BMI270_PWR_CONF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(1));

    // 3. Upload config file (8192 bytes in 32-byte blocks)
    imu_write_reg(BMI270_INIT_CTRL, 0x00);  // prepare
    for (int i = 0; i < 256; i++) {
        imu_write_reg(BMI270_INIT_ADDR_0, 0x00);
        imu_write_reg(BMI270_INIT_ADDR_1, (uint8_t)i);
        imu_write_burst(BMI270_INIT_DATA, &bmi270_config_file[i * 32], 32);
    }
    imu_write_reg(BMI270_INIT_CTRL, 0x01);  // done
    vTaskDelay(pdMS_TO_TICKS(20));

    // 4. Verify init
    uint8_t status = imu_read_reg(BMI270_INTERNAL_STATUS);
    if ((status & 0x0F) != 0x01) {
        ESP_LOGE(TAG_IMU, "BMI270 init failed! status=0x%02X", status);
        return false;
    }
    ESP_LOGI(TAG_IMU, "BMI270 config uploaded OK");

    // 5. Configure: 100Hz ODR, normal mode
    imu_write_reg(BMI270_ACC_CONF, 0xA8);   // 200Hz, normal BWP, perf mode
    imu_write_reg(BMI270_ACC_RANGE, 0x01);   // ±4g
    imu_write_reg(BMI270_GYR_CONF, 0xA9);   // 200Hz, normal BWP
    imu_write_reg(BMI270_GYR_RANGE, 0x01);   // ±1000 dps

    // 6. Enable accel + gyro
    imu_write_reg(BMI270_PWR_CTRL, 0x0E);   // ACC_EN | GYR_EN | TEMP_EN
    vTaskDelay(pdMS_TO_TICKS(50));           // Wait for sensors to stabilize

    ESP_LOGI(TAG_IMU, "BMI270 ready: ACC ±4g, GYR ±1000dps, 200Hz");
    return true;
}

// ============ Read task ============

static void imu_read_task(void* arg) {
    ESP_LOGI(TAG_IMU, "IMU read task started (500ms interval)");

    // Conversion factors
    // ±4g range: 1 LSB = 4000/32768 mg ≈ 0.122 mg → /8192 for g
    // ±1000dps range: 1 LSB = 1000/32768 ≈ 0.0305 dps → /32.768 for dps
    const float acc_scale = 4.0f / 32768.0f;    // → g
    const float gyr_scale = 1000.0f / 32768.0f;  // → dps

    while (true) {
        uint8_t raw[12];
        esp_err_t err = imu_read_burst(BMI270_ACC_X_LSB, raw, 12);
        if (err == ESP_OK) {
            g_imu.acc_x = (int16_t)(raw[1] << 8 | raw[0]);
            g_imu.acc_y = (int16_t)(raw[3] << 8 | raw[2]);
            g_imu.acc_z = (int16_t)(raw[5] << 8 | raw[4]);
            g_imu.gyr_x = (int16_t)(raw[7] << 8 | raw[6]);
            g_imu.gyr_y = (int16_t)(raw[9] << 8 | raw[8]);
            g_imu.gyr_z = (int16_t)(raw[11] << 8 | raw[10]);

            float ax = g_imu.acc_x * acc_scale;
            float ay = g_imu.acc_y * acc_scale;
            float az = g_imu.acc_z * acc_scale;
            float gx = g_imu.gyr_x * gyr_scale;
            float gy = g_imu.gyr_y * gyr_scale;
            float gz = g_imu.gyr_z * gyr_scale;

            ESP_LOGI(TAG_IMU, "ACC: %+.2fg %+.2fg %+.2fg | GYR: %+.1f %+.1f %+.1f dps",
                     ax, ay, az, gx, gy, gz);
        } else {
            ESP_LOGW(TAG_IMU, "Read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ============ Public API ============

static void imu_start_task() {
    xTaskCreatePinnedToCore(imu_read_task, "imu", 3072, NULL, 1, NULL, 0);
}
