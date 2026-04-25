/**
 * ws_cam.h - WebSocket client for Box2Robot CAM
 *
 * After activation, connects to server via WS:
 *   - Auth with device_id + token
 *   - Send camera_info on connect
 *   - Periodically send camera_status heartbeat
 *   - Stream mic audio as binary frames when voice active
 *   - Handle server commands (snapshot, stream, voice)
 *
 * Protocol:
 *   Text frames: JSON { "type": "...", ... }
 *   Binary frames: raw PCM audio data (16kHz 16bit mono)
 */
#pragma once

#include <cstdio>
#include <cstring>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_websocket_client.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs.h"
#include "driver/ledc.h"
#include "config.h"
#include "ota_cam.h"
#include "runtime_config_cam.h"
#include "adpcm.h"
#include "record_buffer.h"
#include "espnow_listener.h"

static const char* TAG_WS = "ws_cam";

// ============ External references ============

extern char g_device_id[20];
extern char g_token[65];
extern char g_ws_url[128];
extern RingbufHandle_t g_mic_ringbuf;
extern i2s_chan_handle_t g_i2s_tx;
static int64_t ntp_timestamp_ms();  // Defined in ntp_sync.h (included before ws_cam.h)

// ============ State (atomic for cross-task safety) ============

static esp_websocket_client_handle_t g_ws_handle = NULL;
static std::atomic<bool> g_ws_connected{false};
static std::atomic<bool> g_ws_auth_ok{false};
static std::atomic<bool> g_ws_auth_needed{false};
static std::atomic<bool> g_voice_streaming{false};
static std::atomic<bool> g_snapshot_pending{false};
static std::atomic<bool> g_cam_recording{false};     // CAM 录制聚合模式
// mux_espnow_data 定义在 espnow_listener.h (included above)
static std::atomic<bool> g_rec_upload_done{false};     // 录制上传完成标志
static std::atomic<bool> g_restart_pending{false};
static std::atomic<bool> g_ws_ever_authed{false};  // 区分首次连接 vs 重连
// TTS 防震荡: 断连后延迟播报, 期间重连则静默
static int64_t g_tts_disc_defer_until = 0;         // >0: 到此时间点播断连提示
static std::atomic<bool> g_tts_disc_announced{false}; // 断连提示已播 → 重连时播恢复提示
#define TTS_DISC_DEFER_US  (30 * 1000000LL)  // 30s 冷却 (覆盖服务器重启窗口)
volatile uint8_t g_cam_stream_fps = 1;  // Also referenced by runtime_config_cam.h

// ============ Connection stats (for diagnostics) ============
static uint32_t g_ws_connect_count = 0;    // 连接成功次数
static uint32_t g_ws_disconnect_count = 0; // 断连次数
static uint32_t g_ws_send_fail_total = 0;  // JPEG 发送失败累计
static int64_t  g_ws_last_connect_us = 0;  // 最近连接时间 (us)
static int64_t  g_ws_last_disconnect_us = 0; // 最近断连时间 (us)

// ===== 发送退避: 失败后跳过几帧, 让 TCP 缓冲排空, 防止断连 =====
static uint32_t g_ws_backoff_until = 0;    // 退避截止时间 (xTaskGetTickCount)
static uint8_t  g_ws_fail_streak = 0;      // 连续失败次数

static bool ws_is_send_backoff() {
    if (g_ws_backoff_until == 0) return false;
    // 用有符号差值比较, 避免 tick 溢出 (~49.7天) 导致永久退避
    int32_t diff = (int32_t)(g_ws_backoff_until - xTaskGetTickCount());
    return diff > 0;
}
static void ws_send_ok() {
    if (g_ws_fail_streak > 0) {
        ESP_LOGI(TAG_WS, "[SERVER] Send recovered after %d fails", g_ws_fail_streak);
    }
    g_ws_fail_streak = 0;
    g_ws_backoff_until = 0;
}
static void ws_send_failed() {
    g_ws_fail_streak++;
    g_ws_send_fail_total++;
    // 退避: 200ms → 500ms → 1s → 2s
    uint32_t ms = 200;
    if (g_ws_fail_streak >= 4) ms = 2000;
    else if (g_ws_fail_streak >= 3) ms = 1000;
    else if (g_ws_fail_streak >= 2) ms = 500;
    g_ws_backoff_until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    if (g_ws_fail_streak <= 3 || g_ws_fail_streak % 10 == 0) {
        ESP_LOGW(TAG_WS, "[SERVER] Send fail #%d, backoff %lums", g_ws_fail_streak, (unsigned long)ms);
    }
}

// TTS prompt queue (single slot — WS callback writes, TTS task reads)
// 用 atomic flag 保护: 写入完成后置 true, 读取后置 false
static char g_tts_pending_prompt[32] = {};
static std::atomic<bool> g_tts_prompt_ready{false};

// Server→Speaker playback: ownership transfer via g_play_ready flag
// WS callback assembles → sets g_play_ready → TTS task takes ownership + frees
static std::atomic<bool> g_play_ready{false};
static uint8_t* g_play_buf = NULL;   // Owned by WS callback until g_play_ready, then TTS task
static int      g_play_len = 0;
static int      g_play_total = 0;
static bool     g_play_is_adpcm = false;  // true if buffer contains ADPCM (needs decode before play)
static std::atomic<bool> g_tts_playing{false};

// ============ Segmented TTS playback queue ============
// Supports receiving multiple audio segments and playing them sequentially.
// Server sends play_audio_segment JSON + binary data for each segment.
struct AudioSegment {
    uint8_t* data;
    int      len;
    bool     is_adpcm;
};
static const int SEGMENT_QUEUE_SIZE = 8;
static AudioSegment g_seg_queue[SEGMENT_QUEUE_SIZE];
static std::atomic<int> g_seg_write_idx{0};   // next write position
static std::atomic<int> g_seg_read_idx{0};    // next read position
static std::atomic<bool> g_seg_pending{false}; // segment assembly in progress

// Temporary assembly buffer for current segment (WS callback → queue)
static uint8_t* g_seg_asm_buf = NULL;
static int      g_seg_asm_len = 0;
static int      g_seg_asm_total = 0;
static bool     g_seg_asm_adpcm = false;

// ============ Send JSON helper ============

static bool ws_send_json(const char* json) {
    if (!g_ws_handle || !g_ws_connected) return false;
    int len = strlen(json);
    int ret = esp_websocket_client_send_text(g_ws_handle, json, len, pdMS_TO_TICKS(1000));
    return ret == len;
}

// ============ Send auth ============

static void ws_send_auth() {
    char msg[192];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"auth\",\"device_id\":\"%s\",\"token\":\"%s\",\"fw_version\":\"%s\"}",
             g_device_id, g_token, FW_VERSION);
    ws_send_json(msg);
}

// ============ Send camera_info ============

static void ws_send_camera_info() {
    const ResolutionInfo* cur = cam_get_resolution_info(g_cfg_cam_resolution);
    char res_arr[512] = "[";
    for (int i = 0; i < RESOLUTION_COUNT; i++) {
        char entry[80];
        snprintf(entry, sizeof(entry), "%s{\"id\":%d,\"w\":%d,\"h\":%d,\"label\":\"%s\"}",
                 i > 0 ? "," : "",
                 RESOLUTIONS[i].id, RESOLUTIONS[i].width, RESOLUTIONS[i].height, RESOLUTIONS[i].label);
        strncat(res_arr, entry, sizeof(res_arr) - strlen(res_arr) - 1);
    }
    strncat(res_arr, "]", sizeof(res_arr) - strlen(res_arr) - 1);

    // 获取 ESP-NOW AP MAC (供 Server 下发给 Follower 建立 ESP-NOW 链路)
    uint8_t ap_mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4], ap_mac[5]);

    char msg[700];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"camera_info\","
             "\"resolution\":\"%dx%d\","
             "\"resolution_id\":%d,"
             "\"max_fps\":30,"
             "\"supported_resolutions\":%s,"
             "\"audio_format\":\"pcm\","
             "\"sample_rate\":%d,"
             "\"channels\":1,"
             "\"espnow_ap_mac\":\"%s\"}",
             cur->width, cur->height, cur->id,
             res_arr,
             AUDIO_SAMPLE_RATE,
             mac_str);
    ws_send_json(msg);
}

// ============ Send camera_status heartbeat ============

static void ws_send_camera_status() {
    const ResolutionInfo* cur = cam_get_resolution_info(g_cfg_cam_resolution);
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"camera_status\","
             "\"streaming\":%s,"
             "\"stream_fps\":%u,"
             "\"resolution\":\"%dx%d\","
             "\"resolution_id\":%u,"
             "\"voice_active\":%s,"
             "\"free_heap\":%u}",
             g_cam_stream_fps > 0 ? "true" : "false",
             (unsigned)g_cam_stream_fps,
             cur->width, cur->height, (unsigned)g_cfg_cam_resolution,
             g_voice_streaming ? "true" : "false",
             (unsigned)esp_get_free_heap_size());
    ws_send_json(msg);
}

// ============ Simple JSON key extractor ============

static bool ws_json_get_str(const char* json, int json_len,
                             const char* key, char* out, int out_len) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) {
        snprintf(pattern, sizeof(pattern), "\"%s\": \"", key);
        p = strstr(json, pattern);
    }
    if (!p) return false;
    p += strlen(pattern);
    const char* e = strchr(p, '"');
    if (!e) return false;
    int len = e - p;
    if (len >= out_len) len = out_len - 1;
    strncpy(out, p, len);
    out[len] = 0;
    return true;
}

// ============ Handle server messages ============

static void ws_handle_message(const char* data, int len) {
    char type[24] = {};
    ws_json_get_str(data, len, "type", type, sizeof(type));

    if (strcmp(type, "auth_ok") == 0) {
        g_ws_auth_ok = true;
        g_ws_auth_needed = false;
        // TTS: 首次连接或断连重连都播第二声 "Mingle!"
        bool was_disc = g_tts_disc_announced.exchange(false);
        if (was_disc || !g_ws_ever_authed) {
            strncpy(g_tts_pending_prompt, "prompt_boot_mingle", sizeof(g_tts_pending_prompt) - 1);
            g_tts_prompt_ready = true;
        }
        g_ws_ever_authed = true;
        // 检查服务器版本, 提示是否需要更新
        char sv[24] = {};
        ws_json_get_str(data, len, "server_version", sv, sizeof(sv));
        if (sv[0] && strcmp(sv, FW_VERSION) != 0) {
            ESP_LOGW(TAG_WS, "Auth OK — VERSION MISMATCH: local=%s server=%s, OTA needed!", FW_VERSION, sv);
        } else {
            ESP_LOGI(TAG_WS, "Auth OK (v%s)", FW_VERSION);
        }
        ws_send_camera_info();
    }
    else if (strcmp(type, "auth_fail") == 0) {
        g_ws_auth_ok = false;
        g_ws_auth_needed = false;
        ESP_LOGE(TAG_WS, "Auth FAILED — clearing token, will re-bind after restart");
        // Clear stale token from NVS so next boot enters bind-code flow
        nvs_handle_t nvs_h;
        if (nvs_open(NVS_NS_AUTH, NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_erase_key(nvs_h, NVS_KEY_TOKEN);
            nvs_erase_key(nvs_h, NVS_KEY_WS_URL);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
        g_restart_pending = true;
    }
    else if (strcmp(type, "snapshot") == 0) {
        g_snapshot_pending = true;
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"snapshot\",\"ok\":true}");
    }
    else if (strcmp(type, "stream_mode") == 0) {
        int fps_val = -1;
        cfg_json_get_int(data, "fps", &fps_val);
        if (fps_val >= 0 && fps_val <= 30) {
            g_cam_stream_fps = (uint8_t)fps_val;
        } else {
            char mode[16] = {};
            ws_json_get_str(data, len, "mode", mode, sizeof(mode));
            if (strcmp(mode, "idle") == 0)           g_cam_stream_fps = 1;
            else if (strcmp(mode, "preview") == 0)   g_cam_stream_fps = 10;
            else if (strcmp(mode, "inference") == 0)  g_cam_stream_fps = 10;
            else if (strcmp(mode, "voice") == 0)      g_cam_stream_fps = 0;  // trigger mode: snapshot on demand
            else if (strcmp(mode, "stop") == 0)       g_cam_stream_fps = 0;
        }
        g_cfg_cam_fps = g_cam_stream_fps;
        char ack[80];
        snprintf(ack, sizeof(ack), "{\"type\":\"ack\",\"cmd\":\"stream_mode\",\"ok\":true,\"fps\":%u}",
                 (unsigned)g_cam_stream_fps);
        ws_send_json(ack);
    }
    else if (strcmp(type, "set_config") == 0) {
        // Defer resolution changes to avoid blocking WS callback with vTaskDelay
        int changed = runtime_config_apply(data, len);
        char ack[80];
        snprintf(ack, sizeof(ack), "{\"type\":\"ack\",\"cmd\":\"set_config\",\"ok\":true,\"changed\":%d}", changed);
        ws_send_json(ack);
    }
    else if (strcmp(type, "ota_update") == 0) {
        char url[256] = {};
        ws_json_get_str(data, len, "url", url, sizeof(url));
        if (url[0]) {
            strncpy(g_ota_url, url, sizeof(g_ota_url) - 1);
            g_ota_pending = true;
        }
    }
    else if (strcmp(type, "play_tone") == 0) {
        char sound[16] = {};
        ws_json_get_str(data, len, "sound", sound, sizeof(sound));
        // Queue to TTS task (non-blocking)
        strncpy(g_tts_pending_prompt, "__tone__", sizeof(g_tts_pending_prompt) - 1);
        g_tts_prompt_ready = true;
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"play_tone\",\"ok\":true}");
    }
    else if (strcmp(type, "voice_start") == 0) {
        // Set flag — drain happens in ws_audio_task before sending (not here in callback)
        g_voice_streaming = true;
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"voice_start\",\"ok\":true}");
    }
    else if (strcmp(type, "voice_stop") == 0) {
        g_voice_streaming = false;
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"voice_stop\",\"ok\":true}");
    }
    else if (strcmp(type, "play_audio_segment") == 0) {
        // Segmented TTS: prepare to receive next binary segment into queue
        g_seg_pending = true;
        int seg_size = 0;
        cfg_json_get_int(data, "size", &seg_size);  // PCM size (for logging)
        g_seg_asm_adpcm = true;  // Segments are always ADPCM from server
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"play_audio_segment\",\"ok\":true}");
    }
    else if (strcmp(type, "stop_audio") == 0) {
        // Barge-in: Server detected user interrupted TTS → abort playback immediately
        g_tts_abort = true;        // Interrupts tts_play_pcm loop within ~8ms
        g_play_ready = false;      // Cancel any pending (assembled but not yet playing) audio
        g_seg_pending = false;     // Cancel any in-progress segment assembly
        // 不在这里释放 g_play_buf — 由 TTS 任务统一管理生命周期
        // (避免 WS 回调 free 和 TTS 任务读取之间的竞态)
        // Free segment assembly buffer
        if (g_seg_asm_buf) {
            heap_caps_free(g_seg_asm_buf);
            g_seg_asm_buf = NULL;
        }
        // Note: segment queue is flushed by TTS task when it sees g_tts_abort
        ESP_LOGI(TAG_WS, "stop_audio: barge-in, aborting TTS playback");
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"stop_audio\",\"ok\":true}");
    }
    else if (strcmp(type, "tts_prompt") == 0) {
        ws_json_get_str(data, len, "prompt", g_tts_pending_prompt, sizeof(g_tts_pending_prompt));
        g_tts_prompt_ready = true;
        ESP_LOGI(TAG_WS, "TTS: %s", g_tts_pending_prompt);
        extern bool audio_ok;
        char ack[80];
        snprintf(ack, sizeof(ack), "{\"type\":\"ack\",\"cmd\":\"tts_prompt\",\"ok\":%s}", audio_ok ? "true" : "false");
        ws_send_json(ack);
    }
    else if (strcmp(type, "servo_cmd") == 0) {
        // Soul/Server 控制舵机: {"type":"servo_cmd","yaw":140,"pitch":140}
        // 角度范围 0~280, 中位 140
        int yaw_val = -1, pitch_val = -1;
        cfg_json_get_int(data, "yaw", &yaw_val);
        cfg_json_get_int(data, "pitch", &pitch_val);
        extern void servo_set_angle(int channel, int angle);
        if (yaw_val >= 0 && yaw_val <= 280) servo_set_angle(1, yaw_val);
        if (pitch_val >= 0 && pitch_val <= 280) servo_set_angle(2, pitch_val);
        ESP_LOGI(TAG_WS, "servo_cmd: yaw=%d pitch=%d", yaw_val, pitch_val);
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"servo_cmd\",\"ok\":true}");
    }
    else if (strcmp(type, "unbind") == 0) {
        ESP_LOGW(TAG_WS, "Device unbound by server — clearing token");
        // Clear token so next boot enters bind-code flow
        nvs_handle_t nvs_h;
        if (nvs_open(NVS_NS_AUTH, NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_erase_key(nvs_h, NVS_KEY_TOKEN);
            nvs_erase_key(nvs_h, NVS_KEY_WS_URL);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
        g_restart_pending = true;
    }
    else if (strcmp(type, "wifi_add") == 0) {
        char ssid[33] = {}, pass[65] = {};
        ws_json_get_str(data, len, "ssid", ssid, sizeof(ssid));
        ws_json_get_str(data, len, "password", pass, sizeof(pass));
        if (ssid[0]) {
            nvs_wifi_save(ssid, pass);
            ws_send_json("{\"type\":\"wifi_add_ack\"}");
        }
    }
    else if (strcmp(type, "wifi_clear") == 0) {
        ESP_LOGW(TAG_WS, "WiFi clear from server — erasing WiFi NVS");
        nvs_wifi_clear_all();
        ws_send_json("{\"type\":\"wifi_clear_ack\"}");
        g_restart_pending = true;
    }
    else if (strcmp(type, "factory_reset") == 0) {
        ESP_LOGW(TAG_WS, "Factory reset from server — erasing all NVS");
        // Clear WiFi credentials
        nvs_wifi_clear_all();
        // Clear auth token + ws_url
        nvs_handle_t nvs_h;
        if (nvs_open(NVS_NS_AUTH, NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_erase_key(nvs_h, NVS_KEY_TOKEN);
            nvs_erase_key(nvs_h, NVS_KEY_WS_URL);
            nvs_erase_key(nvs_h, NVS_KEY_CLIENT_ID);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
        g_restart_pending = true;
    }
    // ===== 舵机控制 (Server → yaw/pitch) =====
    else if (strcmp(type, "servo_move") == 0) {
        int yaw = -1, pitch = -1;
        cfg_json_get_int(data, "yaw", &yaw);
        cfg_json_get_int(data, "pitch", &pitch);
        // 角度 0-180 → duty 410-2048 (50Hz 14-bit PWM)
        if (yaw >= 0 && yaw <= 180) {
            uint32_t duty = 410 + (uint32_t)yaw * (2048 - 410) / 180;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
        }
        if (pitch >= 0 && pitch <= 180) {
            uint32_t duty = 410 + (uint32_t)pitch * (2048 - 410) / 180;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
        }
        char ack[80];
        snprintf(ack, sizeof(ack), "{\"type\":\"ack\",\"cmd\":\"servo_move\",\"yaw\":%d,\"pitch\":%d}", yaw, pitch);
        ws_send_json(ack);
    }
    // ===== 录制数据聚合 (CAM 作为数据聚合器) =====
    else if (strcmp(type, "record_start_cam") == 0) {
        // 按需启动 ESP-NOW (平时关闭, 避免干扰 WiFi 流媒体)
        espnow_listener_init();
        ESP_LOGI(TAG_WS, "ESP-NOW re-initialized for recording");

        g_cam_recording = true;
        g_rec_upload_done = false;
        g_en_has_feedback = false;
        if (!g_rec_buf.isInitialized()) g_rec_buf.init();
        g_rec_buf.startRecording();
        g_cam_stream_fps = 10;  // 录制时 10fps (减少 WiFi 带宽占用, 保护 ESP-NOW)
        ESP_LOGI(TAG_WS, "CAM recording started (aggregator mode)");
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"record_start_cam\",\"ok\":true}");
    }
    else if (strcmp(type, "record_stop_cam") == 0) {
        g_cam_recording = false;
        g_rec_buf.stopRecording();
        espnow_clear_pair_group();
        esp_now_deinit();
        ESP_LOGI(TAG_WS, "CAM recording stopped + ESP-NOW shut down, flushing %d frames", g_rec_buf.pendingCount());
        ws_send_json("{\"type\":\"ack\",\"cmd\":\"record_stop_cam\",\"ok\":true}");
    }
}

// ============ Server→Speaker audio assembly ============
// WS binary frames arrive in chunks. Assemble into g_play_buf, then hand off to TTS task.

static void ws_handle_audio_rx(const uint8_t* data, int len, int offset, int total) {
    // ── Segmented mode: queue into segment ring buffer ──
    if (g_seg_pending) {
        if (offset == 0) {
            // Free any previous assembly buffer
            if (g_seg_asm_buf) { heap_caps_free(g_seg_asm_buf); g_seg_asm_buf = NULL; }
            if (total > 256 * 1024) {
                ESP_LOGW(TAG_WS, "Segment too large: %d, skip", total);
                g_seg_pending = false;
                return;
            }
            // Detect and strip ADPCM header
            g_seg_asm_adpcm = (len >= 2 && data[0] == 0xB2 && data[1] == 0xA1);
            if (g_seg_asm_adpcm) {
                data += 2; len -= 2; total -= 2;
            }
            g_seg_asm_buf = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
            if (!g_seg_asm_buf) {
                ESP_LOGE(TAG_WS, "Seg malloc(%d) fail", total);
                g_seg_pending = false;
                return;
            }
            g_seg_asm_total = total;
            g_seg_asm_len = 0;
        }

        if (g_seg_asm_buf && g_seg_asm_len + len <= g_seg_asm_total) {
            memcpy(g_seg_asm_buf + g_seg_asm_len, data, len);
            g_seg_asm_len += len;
        }

        // Last chunk: push to queue
        if (g_seg_asm_buf && g_seg_asm_len >= g_seg_asm_total) {
            int widx = g_seg_write_idx.load();
            int next = (widx + 1) % SEGMENT_QUEUE_SIZE;
            if (next == g_seg_read_idx.load()) {
                // Queue full — drop incoming (不修改 read_idx, 保持 SPSC 安全)
                ESP_LOGW(TAG_WS, "Segment queue full, dropping new segment (%dB)", g_seg_asm_total);
                heap_caps_free(g_seg_asm_buf);
                g_seg_asm_buf = NULL;
            } else {
                g_seg_queue[widx] = { g_seg_asm_buf, g_seg_asm_total, g_seg_asm_adpcm };
                g_seg_write_idx = next;
                g_seg_asm_buf = NULL;  // Ownership transferred to queue
                ESP_LOGD(TAG_WS, "Segment queued (%dB)", g_seg_asm_total);
            }
            g_seg_pending = false;
        }
        return;
    }

    // ── Single-buffer mode (legacy): assemble into g_play_buf ──
    if (g_tts_playing || g_play_ready) {
        ESP_LOGD(TAG_WS, "Audio rx skipped (TTS busy)");
        return;
    }

    if (offset == 0) {
        if (g_play_buf) { heap_caps_free(g_play_buf); g_play_buf = NULL; }
        if (total > 512 * 1024) {
            ESP_LOGW(TAG_WS, "Audio too large: %d, skip", total);
            return;
        }
        g_play_is_adpcm = (len >= 2 && data[0] == 0xB2 && data[1] == 0xA1);
        if (g_play_is_adpcm) {
            data += 2; len -= 2; total -= 2;
        }
        g_play_buf = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
        if (!g_play_buf) {
            ESP_LOGE(TAG_WS, "Audio malloc(%d) fail", total);
            return;
        }
        g_play_total = total;
        g_play_len = 0;
    }

    if (g_play_buf && g_play_len + len <= g_play_total) {
        memcpy(g_play_buf + g_play_len, data, len);
        g_play_len += len;
    }

    if (g_play_buf && g_play_len >= g_play_total) {
        g_play_ready = true;
    }
}

// ============ WebSocket event handler ============

static void ws_event_handler(void* arg, esp_event_base_t base,
                              int32_t event_id, void* event_data) {
    esp_websocket_event_data_t* ev = (esp_websocket_event_data_t*)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            g_ws_connected = true;
            g_ws_auth_ok = false;
            g_ws_auth_needed = false;
            g_ws_connect_count++;
            // 快速重连 → 取消待播的断连提示 (震荡静默)
            if (g_tts_disc_defer_until > 0) {
                g_tts_disc_defer_until = 0;  // 还没播就恢复了, 两条都不播
            }
            int64_t now = esp_timer_get_time();
            int reconnect_ms = g_ws_last_disconnect_us > 0
                ? (int)((now - g_ws_last_disconnect_us) / 1000) : 0;
            g_ws_last_connect_us = now;
            // 获取 WiFi RSSI
            wifi_ap_record_t ap;
            int rssi = 0;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
            ESP_LOGI(TAG_WS, "[SERVER] Connected #%u (reconnect=%dms rssi=%d heap=%u)",
                     g_ws_connect_count, reconnect_ms, rssi,
                     (unsigned)esp_get_free_heap_size());
            ws_send_auth();
            break;
        }

        case WEBSOCKET_EVENT_DISCONNECTED: {
            // 服务器(WS)断连 ≠ WiFi 断连
            // WiFi 仍在, WS 会 1s 后自动重连, 不需要做激进处理
            g_ws_connected = false;
            g_ws_auth_ok = false;
            g_voice_streaming = false;
            g_ws_disconnect_count++;
            g_ws_last_disconnect_us = esp_timer_get_time();
            int uptime_s = g_ws_last_connect_us > 0
                ? (int)((g_ws_last_disconnect_us - g_ws_last_connect_us) / 1000000) : 0;
            wifi_ap_record_t ap;
            int rssi = 0;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
            bool wifi_ok = (strlen(g_sta_ip) > 0);
            if (wifi_ok) {
                // WiFi 在但服务器断了 — 自动重连, 不降 FPS (用户可能在预览)
                ESP_LOGW(TAG_WS, "[SERVER] Disconnected (WiFi OK, auto-reconnect 1s). "
                         "#%u uptime=%ds rssi=%d heap=%u",
                         g_ws_disconnect_count, uptime_s, rssi,
                         (unsigned)esp_get_free_heap_size());
            } else {
                // WiFi 也断了 — 降到 idle FPS 省功耗
                g_cam_stream_fps = 1;
                ESP_LOGW(TAG_WS, "[SERVER] Disconnected (WiFi DOWN). "
                         "#%u uptime=%ds heap=%u",
                         g_ws_disconnect_count, uptime_s,
                         (unsigned)esp_get_free_heap_size());
            }
            // TTS: 延迟播报 (防震荡: 断连后 10s 内重连则取消)
            // OTA 期间不播断连提示 (OTA 下载会主动断开 WS)
            if (g_ws_ever_authed && wifi_ok && !g_ota_in_progress) {
                g_tts_disc_defer_until = esp_timer_get_time() + TTS_DISC_DEFER_US;
            }
            // 标记取消: TTS 任务会检查 g_play_ready 并负责释放 buffer
            // (不在 WS 回调中直接 free, 避免与 TTS 任务竞态)
            if (!g_play_ready && g_play_buf) {
                // 组装中但未完成的 buffer, 安全释放 (TTS 不会碰未 ready 的 buffer)
                heap_caps_free(g_play_buf);
                g_play_buf = NULL;
                g_play_len = 0;
                g_play_total = 0;
            } else if (g_play_ready) {
                // 已 ready 但 TTS 还没取走 — 标记取消, 由 TTS 任务释放
                g_play_ready = false;
            }
            break;
        }

        case WEBSOCKET_EVENT_DATA:
            if (ev->op_code == 0x01 && ev->data_ptr && ev->data_len > 0) {
                ws_handle_message(ev->data_ptr, ev->data_len);
            }
            else if (ev->op_code == 0x02 && ev->data_ptr && ev->data_len > 0) {
                ws_handle_audio_rx((const uint8_t*)ev->data_ptr, ev->data_len,
                                   ev->payload_offset, ev->payload_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG_WS, "[SERVER] WS error (heap=%u wifi=%s)",
                     (unsigned)esp_get_free_heap_size(),
                     strlen(g_sta_ip) > 0 ? "OK" : "DOWN");
            break;

        default:
            break;
    }
}

// ============ Camera streaming task ============

// JPEG 帧发送用静态 PSRAM 缓冲, 避免每帧 malloc/free 造成碎片化
#define JPEG_SEND_BUF_SIZE (100 * 1024 + 8)  // 100KB max JPEG + 8B header
static uint8_t* s_jpeg_send_buf = NULL;

static void ws_cam_stream_task(void* arg) {
    ESP_LOGI(TAG_WS, "Camera stream task started");
    // 一次性分配, 永不释放
    if (!s_jpeg_send_buf) {
        s_jpeg_send_buf = (uint8_t*)heap_caps_malloc(JPEG_SEND_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_jpeg_send_buf) {
            ESP_LOGE(TAG_WS, "FATAL: PSRAM alloc for JPEG send buf failed");
        }
    }

    while (true) {
        uint32_t frame_start = xTaskGetTickCount();
        uint8_t fps = g_cam_stream_fps;
        bool do_snapshot = g_snapshot_pending.exchange(false);

        if (!do_snapshot && (fps == 0 || !g_ws_auth_ok || !g_ws_handle || g_cam_reinit_in_progress)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!g_ws_auth_ok || !g_ws_handle || g_cam_reinit_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            uint8_t* jpg = NULL;
            size_t jpg_len = 0;
            if (fb->format == PIXFORMAT_JPEG) {
                jpg = fb->buf;
                jpg_len = fb->len;
            } else {
                frame2jpg(fb, g_cfg_jpeg_quality, &jpg, &jpg_len);
            }
            if (jpg && jpg_len > 0) {
                int64_t ts = ntp_timestamp_ms();

                // === 录制模式: 把 JPEG + 位置数据推入 PSRAM 缓冲 ===
                if (g_cam_recording && g_rec_buf.isRecording() && g_en_has_feedback) {
                    // Critical section: 防止 ESP-NOW ISR 回调写 servo 数组时产生 torn read
                    RecServo f_servos[REC_MAX_SERVOS], l_servos[REC_MAX_SERVOS];
                    uint8_t fc, lc;
                    taskENTER_CRITICAL(&mux_espnow_data);
                    fc = g_en_follower_count;
                    lc = g_en_leader_count;
                    for (int i = 0; i < fc && i < REC_MAX_SERVOS; i++) {
                        f_servos[i].id = g_en_follower_pos[i].id;
                        f_servos[i].pos = g_en_follower_pos[i].pos;
                    }
                    for (int i = 0; i < lc && i < REC_MAX_SERVOS; i++) {
                        l_servos[i].id = g_en_leader_pos[i].id;
                        l_servos[i].pos = g_en_leader_pos[i].pos;
                    }
                    taskEXIT_CRITICAL(&mux_espnow_data);
                    g_rec_buf.push(ts, f_servos, fc, l_servos, lc, jpg, jpg_len);
                }

                // === 正常预览: 发送 JPEG 帧给 Server ===
                // 退避期内跳过 JPEG 发送 — 丢几帧比断连好
                if (!ws_is_send_backoff() && s_jpeg_send_buf) {
                    size_t total = 8 + jpg_len;
                    if (total <= JPEG_SEND_BUF_SIZE) {
                        s_jpeg_send_buf[0] = 0xB2; s_jpeg_send_buf[1] = 0xF0;
                        memcpy(&s_jpeg_send_buf[2], &ts, 6);
                        memcpy(&s_jpeg_send_buf[8], jpg, jpg_len);
                        int ret = esp_websocket_client_send_bin(g_ws_handle, (const char*)s_jpeg_send_buf,
                                                      total, pdMS_TO_TICKS(1000));
                        if (ret <= 0) {
                            ws_send_failed();  // 启动退避, 跳过后续几帧
                        } else {
                            ws_send_ok();
                        }
                    } else {
                        ESP_LOGW(TAG_WS, "JPEG too large: %u > %u, skipping", (unsigned)total, JPEG_SEND_BUF_SIZE);
                    }
                } // end backoff check
            }
            if (jpg && jpg != fb->buf) free(jpg);
            esp_camera_fb_return(fb);
        }

        // 自适应延迟: 扣除本帧已用时间, 保持精确 FPS
        fps = g_cam_stream_fps;
        if (fps > 0) {
            uint32_t elapsed_ms = (xTaskGetTickCount() - frame_start) * portTICK_PERIOD_MS;
            uint32_t target_ms = 1000 / fps;
            if (elapsed_ms < target_ms) {
                vTaskDelay(pdMS_TO_TICKS(target_ms - elapsed_ms));
            }
            // else: 已超时, 不delay, 立即下一帧
        }
    }
}

// ============ Recording upload task — dequeue frames and send to Server ============
// Binary format: [0xB2][0xF2][8B ntp_ts][1B f_count][f_servos...][1B l_count][l_servos...][JPEG...]

static void ws_rec_upload_task(void* arg) {
    ESP_LOGI(TAG_WS, "Recording upload task started");
    while (true) {
        if (!g_rec_buf.isInitialized() || g_rec_buf.isEmpty()) {
            // 录制已停止且队列空 → 通知 Server 完成
            if (!g_cam_recording && g_rec_buf.isInitialized() && !g_rec_buf.isRecording() && g_rec_buf.isEmpty()) {
                if (!g_rec_upload_done && g_ws_auth_ok && g_ws_handle) {
                    ws_send_json("{\"type\":\"record_cam_done\"}");
                    g_cam_stream_fps = 1;  // 回到 idle
                    g_rec_upload_done = true;
                    g_rec_buf.deinit();  // 释放 6MB PSRAM
                    ESP_LOGI(TAG_WS, "Recording upload complete, freed PSRAM, notified server");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const RecFrame* f = g_rec_buf.pop();
        if (!f) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        if (!g_ws_auth_ok || !g_ws_handle) {
            // WS 断了, 等重连
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 构建二进制帧: [0xB2][0xF2][8B ts][1B f_cnt][f_servos][1B l_cnt][l_servos][JPEG]
        size_t servo_size = 2 + 8 + 1 + f->f_count * 3 + 1 + f->l_count * 3;
        size_t total = servo_size + f->jpeg_len;
        uint8_t* buf = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGW(TAG_WS, "PSRAM alloc failed for upload frame");
            g_rec_buf.popCommit();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int pos = 0;
        buf[pos++] = 0xB2;
        buf[pos++] = 0xF2;  // recording frame magic
        memcpy(&buf[pos], &f->ntp_ts, 8); pos += 8;

        buf[pos++] = f->f_count;
        for (int i = 0; i < f->f_count; i++) {
            buf[pos++] = f->f_servos[i].id;
            buf[pos++] = f->f_servos[i].pos & 0xFF;
            buf[pos++] = (f->f_servos[i].pos >> 8) & 0xFF;
        }
        buf[pos++] = f->l_count;
        for (int i = 0; i < f->l_count; i++) {
            buf[pos++] = f->l_servos[i].id;
            buf[pos++] = f->l_servos[i].pos & 0xFF;
            buf[pos++] = (f->l_servos[i].pos >> 8) & 0xFF;
        }
        memcpy(&buf[pos], f->jpeg, f->jpeg_len);
        pos += f->jpeg_len;

        int ret = esp_websocket_client_send_bin(g_ws_handle, (const char*)buf, pos, pdMS_TO_TICKS(1000));
        heap_caps_free(buf);
        g_rec_buf.popCommit();

        if (ret <= 0) {
            ESP_LOGW(TAG_WS, "Recording frame send failed");
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

// ============ Audio streaming task (IMA-ADPCM compressed) ============
// PCM 4096 bytes (2048 samples) → ADPCM header(2B) + state(4B) + 1024 nibbles = ~1030 bytes
// Bandwidth: 32KB/s PCM → ~8KB/s ADPCM (4:1 compression)

#define AUDIO_PCM_BATCH   4096          // PCM batch size (bytes)
#define AUDIO_ADPCM_HDR   2             // Protocol header: 0xB2 0xA1
#define AUDIO_ADPCM_STATE 4             // ADPCM state: predicted(2) + index(1) + pad(1)
#define AUDIO_ADPCM_MAX   (AUDIO_ADPCM_HDR + AUDIO_ADPCM_STATE + AUDIO_PCM_BATCH / 4 + 16)

static void ws_audio_task(void* arg) {
    ESP_LOGI(TAG_WS, "Audio stream task started (ADPCM)");
    static uint8_t pcm_buf[AUDIO_PCM_BATCH];
    static uint8_t adpcm_buf[AUDIO_ADPCM_MAX];
    int pcm_len = 0;
    bool was_streaming = false;
    AdpcmState adpcm_state = {0, 0};

    while (true) {
        bool streaming = g_voice_streaming && g_ws_auth_ok && g_ws_handle;

        if (!streaming) {
            // Flush remaining PCM as ADPCM
            if (pcm_len > 0 && g_ws_handle && g_ws_auth_ok) {
                int n_samples = pcm_len / 2;
                adpcm_buf[0] = 0xB2; adpcm_buf[1] = 0xA1;  // ADPCM protocol header
                int enc_len = adpcm_encode((const int16_t*)pcm_buf, n_samples,
                                           adpcm_buf + AUDIO_ADPCM_HDR, adpcm_state);
                esp_websocket_client_send_bin(g_ws_handle, (const char*)adpcm_buf,
                                              AUDIO_ADPCM_HDR + enc_len, pdMS_TO_TICKS(1000));
                pcm_len = 0;
            }
            // Drain stale mic data on voice start transition
            if (!was_streaming && g_voice_streaming && g_mic_ringbuf) {
                size_t sz; void* item;
                while ((item = xRingbufferReceive(g_mic_ringbuf, &sz, 0)) != NULL)
                    vRingbufferReturnItem(g_mic_ringbuf, item);
                adpcm_state = {0, 0};  // Reset encoder state
            }
            was_streaming = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        was_streaming = true;

        // Read PCM from ring buffer
        size_t sz = 0;
        void* item = xRingbufferReceive(g_mic_ringbuf, &sz, pdMS_TO_TICKS(50));
        if (item && sz > 0) {
            if (pcm_len + (int)sz > AUDIO_PCM_BATCH) {
                // Batch full → encode and send
                int n_samples = pcm_len / 2;
                adpcm_buf[0] = 0xB2; adpcm_buf[1] = 0xA1;
                int enc_len = adpcm_encode((const int16_t*)pcm_buf, n_samples,
                                           adpcm_buf + AUDIO_ADPCM_HDR, adpcm_state);
                esp_websocket_client_send_bin(g_ws_handle, (const char*)adpcm_buf,
                                              AUDIO_ADPCM_HDR + enc_len, pdMS_TO_TICKS(1000));
                pcm_len = 0;
            }
            int copy = ((int)sz > AUDIO_PCM_BATCH) ? AUDIO_PCM_BATCH : (int)sz;
            memcpy(pcm_buf + pcm_len, item, copy);
            pcm_len += copy;
            vRingbufferReturnItem(g_mic_ringbuf, item);
        }

        // Timeout with partial data → send
        if (pcm_len > 0 && !item) {
            int n_samples = pcm_len / 2;
            adpcm_buf[0] = 0xB2; adpcm_buf[1] = 0xA1;
            int enc_len = adpcm_encode((const int16_t*)pcm_buf, n_samples,
                                       adpcm_buf + AUDIO_ADPCM_HDR, adpcm_state);
            esp_websocket_client_send_bin(g_ws_handle, (const char*)adpcm_buf,
                                          AUDIO_ADPCM_HDR + enc_len, pdMS_TO_TICKS(1000));
            pcm_len = 0;
        }
    }
}

// ============ TTS playback task ============

static void ws_tts_task(void* arg) {
    ESP_LOGI(TAG_WS, "TTS task started");
    extern bool audio_ok;
    extern void audio_play_boot_voice();
    while (true) {
        // Play TTS prompt (from WS commands or WS/WiFi event triggers)
        if (g_tts_prompt_ready.exchange(false) && audio_ok) {
            char name[32];
            strncpy(name, g_tts_pending_prompt, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            g_tts_playing = true;
            if (strcmp(name, "__tone__") == 0) {
                audio_play_boot_voice();
            } else {
                tts_play_prompt(name);
            }
            g_tts_playing = false;
        }
        // WS 断连延迟播报 (10s 冷却, 期间重连则静默)
        if (g_tts_disc_defer_until > 0 && esp_timer_get_time() >= g_tts_disc_defer_until) {
            g_tts_disc_defer_until = 0;
            g_tts_disc_announced = true;  // 标记: 已播断连, 重连时应播恢复
            if (audio_ok && !g_tts_playing) {
                g_tts_playing = true;
                tts_play_prompt("prompt_boot_mingle");
                g_tts_playing = false;
            }
        }
        // WiFi TTS: 也用延迟机制防震荡
        // 断连: 只设 flag, 在下一轮如果还断着才播
        if (g_tts_wifi_disc_pending.exchange(false)) {
            // 标记待播, 但等 5s (WiFi 重连通常 <3s)
            // 利用 reconn flag 来取消: 如果 reconn 先到, 两条都不播
            if (audio_ok && !g_tts_playing && strlen(g_sta_ip) == 0) {
                // WiFi 确实还是断的 → 播
                g_tts_playing = true;
                tts_play_prompt("prompt_boot_mingle");
                g_tts_playing = false;
            }
            // 如果 WiFi 已经恢复了 (g_sta_ip 有值), 静默跳过
        }
        // WiFi 重连成功
        if (g_tts_wifi_reconn_pending.exchange(false) && audio_ok && !g_tts_playing) {
            g_tts_playing = true;
            tts_play_prompt("prompt_boot_mingle");
            g_tts_playing = false;
        }
        // ── Segmented playback: drain queue (segments play sequentially) ──
        while (audio_ok && g_seg_read_idx.load() != g_seg_write_idx.load()) {
            int ridx = g_seg_read_idx.load();
            AudioSegment& seg = g_seg_queue[ridx];
            if (!seg.data) {
                g_seg_read_idx = (ridx + 1) % SEGMENT_QUEUE_SIZE;
                continue;
            }
            g_tts_playing = true;
            if (seg.is_adpcm && seg.len > 4) {
                int max_samples = (seg.len - 4) * 2;
                int16_t* pcm = (int16_t*)heap_caps_malloc(max_samples * 2, MALLOC_CAP_SPIRAM);
                if (pcm) {
                    int n = adpcm_decode(seg.data, seg.len, pcm);
                    tts_play_pcm((const uint8_t*)pcm, n * 2);
                    heap_caps_free(pcm);
                }
            } else if (!seg.is_adpcm && seg.len > 0) {
                tts_play_pcm(seg.data, seg.len);
            }
            heap_caps_free(seg.data);
            seg.data = NULL;
            seg.len = 0;
            g_seg_read_idx = (ridx + 1) % SEGMENT_QUEUE_SIZE;
            g_tts_playing = false;

            // Check abort between segments
            if (g_tts_abort) {
                // Flush remaining segments
                while (g_seg_read_idx.load() != g_seg_write_idx.load()) {
                    int ri = g_seg_read_idx.load();
                    if (g_seg_queue[ri].data) {
                        heap_caps_free(g_seg_queue[ri].data);
                        g_seg_queue[ri].data = NULL;
                    }
                    g_seg_read_idx = (ri + 1) % SEGMENT_QUEUE_SIZE;
                }
                g_tts_abort = false;
                ESP_LOGI(TAG_WS, "Segment playback aborted (barge-in)");
                break;
            }
        }

        // ── Single-buffer playback (legacy) ──
        if (g_play_buf && audio_ok) {
            if (g_play_ready) {
                g_play_ready = false;
                g_tts_playing = true;
                if (g_play_is_adpcm && g_play_total > 4) {
                    int max_samples = (g_play_total - 4) * 2;
                    int16_t* pcm = (int16_t*)heap_caps_malloc(max_samples * 2, MALLOC_CAP_SPIRAM);
                    if (pcm) {
                        int n = adpcm_decode(g_play_buf, g_play_total, pcm);
                        tts_play_pcm((const uint8_t*)pcm, n * 2);
                        heap_caps_free(pcm);
                    }
                } else if (!g_play_is_adpcm) {
                    tts_play_pcm(g_play_buf, g_play_total);
                }
                g_tts_playing = false;
            }
            // 无论是正常播完还是被 stop_audio 取消 (g_play_ready=false),
            // 都由 TTS 任务负责释放 buffer (消除竞态)
            if (!g_play_ready) {
                heap_caps_free(g_play_buf);
                g_play_buf = NULL;
                g_play_len = 0;
                g_play_total = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============ Heartbeat task ============

static void ws_heartbeat_task(void* arg) {
    while (true) {
        if (g_ws_auth_needed && g_ws_connected) {
            ws_send_auth();
            g_ws_auth_needed = false;
        }
        if (g_ws_auth_ok) {
            ws_send_camera_status();
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ============ Public API ============

static bool ws_cam_start() {
    if (!g_ws_url[0]) {
        ESP_LOGE(TAG_WS, "No WS URL");
        return false;
    }

    // 防止 handle 泄漏: 销毁旧的 WS client (如果存在)
    if (g_ws_handle) {
        ESP_LOGW(TAG_WS, "Destroying stale WS handle before reconnect");
        esp_websocket_client_close(g_ws_handle, pdMS_TO_TICKS(500));
        esp_websocket_client_stop(g_ws_handle);
        esp_websocket_client_destroy(g_ws_handle);
        g_ws_handle = NULL;
        g_ws_connected = false;
        g_ws_auth_ok = false;
    }

    ESP_LOGI(TAG_WS, "Connecting: %s", g_ws_url);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = g_ws_url;
    cfg.reconnect_timeout_ms = 1000;    // 1s 后开始重连
    cfg.network_timeout_ms = 20000;     // 20s 网络超时 (TLS 握手较慢)
    cfg.ping_interval_sec = 20;         // 20s 心跳 (匹配 Server heartbeat=45, 留足 JPEG 发送余量)
    cfg.pingpong_timeout_sec = 60;      // 60s 无 pong 才断连 (CAM 发大帧, 需要更长容忍)
    cfg.buffer_size = 65536;
    cfg.task_stack = 6144;
    // TCP Keepalive: 检测 NAT 超时/路由器断电等网络层死连接
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 20;           // 20s 空闲后发探针
    cfg.keep_alive_interval = 5;        // 每 5s 重试
    cfg.keep_alive_count = 3;           // 3 次失败才断连
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    g_ws_handle = esp_websocket_client_init(&cfg);
    if (!g_ws_handle) {
        ESP_LOGE(TAG_WS, "WS init fail");
        return false;
    }

    esp_websocket_register_events(g_ws_handle, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(g_ws_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WS, "WS start fail: 0x%x", err);
        esp_websocket_client_destroy(g_ws_handle);
        g_ws_handle = NULL;
        return false;
    }

    static bool tasks_created = false;
    if (!tasks_created) {
        xTaskCreatePinnedToCore(ws_cam_stream_task, "ws_cam", 6144, NULL, 4, NULL, 1);  // W1: 4KB→6KB
        xTaskCreatePinnedToCore(ws_audio_task, "ws_audio", 4096, NULL, 3, NULL, 1);
        xTaskCreatePinnedToCore(ws_heartbeat_task, "ws_hb", 3072, NULL, 2, NULL, 0);
        xTaskCreatePinnedToCore(ws_tts_task, "ws_tts", 4096, NULL, 2, NULL, 0);
        xTaskCreatePinnedToCore(ws_rec_upload_task, "ws_rec", 4096, NULL, 3, NULL, 1);
        tasks_created = true;
    }

    ESP_LOGI(TAG_WS, "WS started");
    return true;
}

static bool ws_cam_is_connected() {
    return g_ws_connected && g_ws_auth_ok;
}
