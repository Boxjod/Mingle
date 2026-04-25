/**
 * ota_cam.h - OTA firmware update for Box2Robot CAM
 * Ported from box2robot_arm/main/ota_manager.h (pure ESP-IDF, no Arduino)
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "config.h"

static const char* OTA_TAG = "ota_cam";

// OTA pending flag (set by WS command, polled in main loop)
static volatile bool g_ota_pending = false;
static volatile bool g_ota_in_progress = false;  // true during OTA download/flash
static char g_ota_url[256] = {};

// OTA stage callback: called at each phase so caller can play TTS etc.
// stage: 0=download_start, 1=download_done, 2=reboot
typedef void (*ota_stage_cb_t)(int stage);

static bool ota_cam_start_update(const char* firmware_url, ota_stage_cb_t stage_cb = nullptr) {
    ESP_LOGI(OTA_TAG, "Starting OTA: %s", firmware_url);

    // 安全校验: URL 必须是 HTTPS (允许局域网 HTTP)
    if (strncmp(firmware_url, "https://", 8) != 0) {
        if (strncmp(firmware_url, "http://192.168.", 15) != 0) {
            ESP_LOGE(OTA_TAG, "OTA URL rejected: not HTTPS");
            return false;
        }
    }

    g_ota_in_progress = true;

    // Stage 0: download starting
    if (stage_cb) stage_cb(0);

    esp_http_client_config_t http_config = {};
    http_config.url = firmware_url;
    http_config.timeout_ms = 30000;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return false;
    }

    TickType_t ota_start = xTaskGetTickCount();
    const TickType_t OTA_TIMEOUT = pdMS_TO_TICKS(180000);  // 3 min
    while (true) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        if ((xTaskGetTickCount() - ota_start) > OTA_TIMEOUT) {
            ESP_LOGE(OTA_TAG, "OTA timeout");
            err = ESP_FAIL;
            break;
        }

        int image_size = esp_https_ota_get_image_size(ota_handle);
        int read_len = esp_https_ota_get_image_len_read(ota_handle);
        if (image_size > 0 && (read_len % (64 * 1024) < 4096)) {
            int pct = read_len * 100 / image_size;
            ESP_LOGI(OTA_TAG, "Progress: %d%% (%d/%d)", pct, read_len, image_size);
        }
    }

    if (err == ESP_OK) {
        // Stage 1: download done, flashing
        if (stage_cb) stage_cb(1);

        err = esp_https_ota_finish(ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(OTA_TAG, "OTA success, restarting...");
            // Stage 2: reboot
            if (stage_cb) stage_cb(2);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            return true;  // never reached
        }
    }

    esp_https_ota_abort(ota_handle);
    g_ota_in_progress = false;
    ESP_LOGE(OTA_TAG, "OTA failed: %s", esp_err_to_name(err));
    return false;
}
