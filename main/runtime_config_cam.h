/**
 * runtime_config_cam.h - Remotely tunable parameters for Box2Robot CAM
 *
 * Changed at runtime via WS "set_config" command from server.
 * All params persisted to NVS namespace "cfg" — survives power cycle.
 * On boot: load from NVS → apply ES8311 registers.
 */
#pragma once

#include <cstring>
#include <cstdlib>
#include "esp_log.h"
#include "nvs.h"
#include "esp_camera.h"
#include "config.h"

static const char* TAG_CFG = "cfg_cam";
#define NVS_NS_CFG "cfg"

// ============ Runtime variables ============

// Camera
static volatile uint8_t g_cfg_jpeg_quality   = 80;    // frame2jpg 0-100
static volatile uint8_t g_cfg_cam_fps        = 1;     // idle FPS
static volatile uint8_t g_cfg_cam_resolution = 2;     // 0=QQVGA 1=QVGA 2=VGA (GC0308 max)

// Audio (ES8311 register values)
static volatile uint8_t g_cfg_dac_volume     = 0xCC;  // REG32 speaker (80%)
static volatile uint8_t g_cfg_tts_volume     = 0xCC;  // TTS playback volume (80%)
static volatile uint8_t g_cfg_mic_pga        = 0x1A;  // REG14 analog gain
static volatile uint8_t g_cfg_mic_dig_gain   = 0x00;  // REG16 digital gain

// ============ NVS helpers ============

static void cfg_nvs_save_u8(const char* key, uint8_t val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CFG, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t cfg_nvs_load_u8(const char* key, uint8_t def) {
    nvs_handle_t h;
    uint8_t val = def;
    if (nvs_open(NVS_NS_CFG, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &val);
        nvs_close(h);
    }
    return val;
}

// ============ Load from NVS on boot ============

static void runtime_config_load() {
    g_cfg_jpeg_quality   = cfg_nvs_load_u8("jpeg_q", 80);
    g_cfg_cam_fps        = cfg_nvs_load_u8("cam_fps", 1);
    g_cfg_cam_resolution = cfg_nvs_load_u8("cam_res", 2);  // default VGA (index 2)
    g_cfg_dac_volume     = cfg_nvs_load_u8("dac_vol", 0xCC);
    g_cfg_tts_volume     = cfg_nvs_load_u8("tts_vol", 0xCC);
    g_cfg_mic_pga        = cfg_nvs_load_u8("mic_pga", 0x1A);
    g_cfg_mic_dig_gain   = cfg_nvs_load_u8("mic_dig", 0x00);

    ESP_LOGI(TAG_CFG, "NVS loaded: jpeg=%d fps=%d dac=0x%02X mic=0x%02X",
             g_cfg_jpeg_quality, g_cfg_cam_fps, g_cfg_dac_volume, g_cfg_mic_pga);
}

// Apply audio settings to ES8311 (call after audio_init)
static void runtime_config_apply_audio() {
    es_wr(0x32, g_cfg_dac_volume);
    es_wr(0x14, g_cfg_mic_pga);
    es_wr(0x16, g_cfg_mic_dig_gain);
    ESP_LOGI(TAG_CFG, "Audio applied: dac=0x%02X pga=0x%02X dig=0x%02X",
             g_cfg_dac_volume, g_cfg_mic_pga, g_cfg_mic_dig_gain);
}

// ============ Simple integer extractor from JSON ============

static bool cfg_json_get_int(const char* json, const char* key, int* out) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (*p == '"') p++;
    *out = atoi(p);
    return true;
}

// ============ Resolution mapping ============
// GC0308: max VGA (640x480), no hardware JPEG, RGB565 + software frame2jpg
// All framesizes below VGA are stable via ISP downscale

struct ResolutionInfo {
    uint8_t id;
    framesize_t framesize;
    uint16_t width;
    uint16_t height;
    const char* label;
};

static const ResolutionInfo RESOLUTIONS[] = {
    {0, FRAMESIZE_QQVGA, 160,  120,  "QQVGA"},
    {1, FRAMESIZE_QVGA,  320,  240,  "QVGA"},
    {2, FRAMESIZE_VGA,   640,  480,  "VGA"},
};
static const int RESOLUTION_COUNT = sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0]);

// Forward declaration — camera_reinit defined in main.cpp
extern bool camera_reinit(framesize_t fs);

// Flag to pause streaming task during camera reinit
static volatile bool g_cam_reinit_in_progress = false;
// Deferred resolution change (set from WS callback, executed in main loop)
static volatile int8_t g_cam_pending_resolution = -1;  // -1 = no pending change

static bool cam_set_resolution(uint8_t idx) {
    if (idx >= RESOLUTION_COUNT) return false;
    // Skip if already at this resolution (avoid unnecessary reinit on server reconnect)
    if (idx == g_cfg_cam_resolution) {
        ESP_LOGI(TAG_CFG, "Resolution unchanged (%s), skip reinit", RESOLUTIONS[idx].label);
        return true;
    }
    ESP_LOGI(TAG_CFG, "Resolution changing %s → %s, pausing stream...",
             RESOLUTIONS[g_cfg_cam_resolution < RESOLUTION_COUNT ? g_cfg_cam_resolution : 2].label,
             RESOLUTIONS[idx].label);
    // Signal streaming task to stop calling esp_camera_fb_get
    g_cam_reinit_in_progress = true;
    vTaskDelay(pdMS_TO_TICKS(500));  // Wait for streaming task to pause

    bool ok = camera_reinit(RESOLUTIONS[idx].framesize);
    g_cam_reinit_in_progress = false;

    if (ok) {
        ESP_LOGI(TAG_CFG, "Resolution → %s (%dx%d)",
                 RESOLUTIONS[idx].label, RESOLUTIONS[idx].width, RESOLUTIONS[idx].height);
    } else {
        ESP_LOGE(TAG_CFG, "Resolution change to %s failed", RESOLUTIONS[idx].label);
    }
    return ok;
}

// 在主循环中执行延迟的分辨率切换 (避免在 WS 回调中阻塞)
static void cam_handle_pending_resolution() {
    int8_t pending = g_cam_pending_resolution;
    if (pending >= 0) {
        g_cam_pending_resolution = -1;
        if (cam_set_resolution((uint8_t)pending)) {
            g_cfg_cam_resolution = (uint8_t)pending;
            cfg_nvs_save_u8("cam_res", (uint8_t)pending);
        }
    }
}

static const ResolutionInfo* cam_get_resolution_info(uint8_t idx) {
    if (idx >= RESOLUTION_COUNT) idx = 2;  // fallback VGA (index 2)
    return &RESOLUTIONS[idx];
}

// Apply resolution from NVS on boot (call after camera_init, BEFORE streaming tasks)
// Uses set_framesize directly — no deinit/reinit needed since no tasks are running yet
static void runtime_config_apply_resolution() {
    if (g_cfg_cam_resolution >= RESOLUTION_COUNT) {
        ESP_LOGW(TAG_CFG, "Invalid NVS cam_res=%d (max=%d), reset to VGA",
                 g_cfg_cam_resolution, RESOLUTION_COUNT - 1);
        g_cfg_cam_resolution = 2;  // VGA = index 2
        cfg_nvs_save_u8("cam_res", 2);
    }
    if (g_cfg_cam_resolution == 2) return;  // VGA is boot default (index 2), skip

    // At boot: camera just init'd, no streaming tasks — safe to use set_framesize directly
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        int ret = s->set_framesize(s, RESOLUTIONS[g_cfg_cam_resolution].framesize);
        if (ret == 0) {
            ESP_LOGI(TAG_CFG, "Boot resolution → %s", RESOLUTIONS[g_cfg_cam_resolution].label);
        } else {
            ESP_LOGW(TAG_CFG, "Boot set_framesize failed, staying VGA");
            g_cfg_cam_resolution = 2;
        }
    }
}

// ============ Apply config from WS "set_config" + save to NVS ============

static int runtime_config_apply(const char* json, int json_len) {
    int val, changed = 0;

    if (cfg_json_get_int(json, "jpeg_quality", &val) && val >= 10 && val <= 100) {
        g_cfg_jpeg_quality = (uint8_t)val;
        cfg_nvs_save_u8("jpeg_q", (uint8_t)val);
        ESP_LOGI(TAG_CFG, "jpeg_quality → %d (saved)", val);
        changed++;
    }
    if (cfg_json_get_int(json, "cam_fps", &val) && val >= 0 && val <= 30) {
        g_cfg_cam_fps = (uint8_t)val;
        extern volatile uint8_t g_cam_stream_fps;
        g_cam_stream_fps = (uint8_t)val;
        cfg_nvs_save_u8("cam_fps", (uint8_t)val);
        ESP_LOGI(TAG_CFG, "cam_fps → %d (saved)", val);
        changed++;
    }
    if (cfg_json_get_int(json, "cam_resolution", &val) && val >= 0 && val < RESOLUTION_COUNT) {
        // 延迟到主循环执行 (cam_set_resolution 含 vTaskDelay, 不能在 WS 回调中阻塞)
        if ((uint8_t)val != g_cfg_cam_resolution) {
            g_cam_pending_resolution = (int8_t)val;
            ESP_LOGI(TAG_CFG, "cam_resolution → %d (deferred to main loop)", val);
        }
        changed++;
    }
    if (cfg_json_get_int(json, "dac_volume", &val) && val >= 0 && val <= 255) {
        g_cfg_dac_volume = (uint8_t)val;
        es_wr(0x32, (uint8_t)val);
        cfg_nvs_save_u8("dac_vol", (uint8_t)val);
        ESP_LOGI(TAG_CFG, "dac_volume → 0x%02X (saved)", val);
        changed++;
    }
    if (cfg_json_get_int(json, "tts_volume", &val) && val >= 0 && val <= 255) {
        g_cfg_tts_volume = (uint8_t)val;
        cfg_nvs_save_u8("tts_vol", (uint8_t)val);
        ESP_LOGI(TAG_CFG, "tts_volume → 0x%02X (saved)", val);
        changed++;
    }
    if (cfg_json_get_int(json, "mic_pga_gain", &val) && val >= 0 && val <= 255) {
        g_cfg_mic_pga = (uint8_t)val;
        es_wr(0x14, (uint8_t)val);
        cfg_nvs_save_u8("mic_pga", (uint8_t)val);
        ESP_LOGI(TAG_CFG, "mic_pga_gain → 0x%02X (saved)", val);
        changed++;
    }
    if (cfg_json_get_int(json, "mic_dig_gain", &val) && val >= 0 && val <= 255) {
        g_cfg_mic_dig_gain = (uint8_t)val;
        es_wr(0x16, (uint8_t)val);
        cfg_nvs_save_u8("mic_dig", (uint8_t)val);
        ESP_LOGI(TAG_CFG, "mic_dig_gain → 0x%02X (saved)", val);
        changed++;
    }
    return changed;
}
