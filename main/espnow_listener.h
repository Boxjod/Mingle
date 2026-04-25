/**
 * espnow_listener.h - ESP-NOW for Box2Robot CAM
 *
 * Three roles:
 *   1. WiFi sharing: In AP mode, listen for WiFi credentials from companion.
 *   2. Reset listener: Receive factory_reset broadcast from paired arm.
 *   3. Recording data aggregator: Receive follower feedback (0x31) with
 *      leader+follower positions for synchronized dataset recording.
 *
 * Protocol: compatible with box2robot_arm/main/espnow_sync.h
 * Packet: [0xAA][cmd][payload...][0x55]
 */
#pragma once

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <atomic>

static const char* TAG_EN = "espnow_cam";

// Protocol constants (must match box2robot_arm espnow_sync.h)
#define EN_HEAD              0xAA
#define EN_TAIL              0x55
#define EN_CMD_SYNC          0x30   // Leader → Follower position sync
#define EN_CMD_FEEDBACK      0x31   // Follower → CAM feedback (leader+follower positions)
#define EN_CMD_WIFI_SHARE    0xF1   // WiFi credentials broadcast

#define EN_MAX_SERVOS        20

// ============ WiFi sharing state ============
static volatile bool g_en_wifi_share_received = false;
static char g_en_shared_ssid[33] = {};
static char g_en_shared_pass[65] = {};

// ============ 配对组 MAC 白名单 (Server 下发) ============
static uint8_t g_en_pair_macs[4][6] = {};   // 最多 4 个配对设备的 MAC
static int     g_en_pair_mac_count = 0;
static volatile bool g_en_pair_group_set = false;

// ============ Follower 反馈数据缓存 (供录制打包) ============
struct EnServoEntry { uint8_t id; uint16_t pos; } __attribute__((packed));

static EnServoEntry g_en_follower_pos[EN_MAX_SERVOS] = {};
static uint8_t      g_en_follower_count = 0;
static EnServoEntry g_en_leader_pos[EN_MAX_SERVOS] = {};
static uint8_t      g_en_leader_count = 0;
static std::atomic<uint32_t> g_en_feedback_recv_ms{0};
static volatile bool g_en_has_feedback = false;
static portMUX_TYPE mux_espnow_data = portMUX_INITIALIZER_UNLOCKED;  // ESP-NOW 数据读写锁

// ============ MAC 白名单 + 工具函数 ============

static bool _mac_in_pair_group(const uint8_t* mac) {
    if (!g_en_pair_group_set) return true;
    for (int i = 0; i < g_en_pair_mac_count; i++) {
        if (memcmp(mac, g_en_pair_macs[i], 6) == 0) return true;
    }
    return false;
}

static void espnow_set_pair_group(const uint8_t macs[][6], int count) {
    g_en_pair_mac_count = count < 4 ? count : 4;
    for (int i = 0; i < g_en_pair_mac_count; i++) {
        memcpy(g_en_pair_macs[i], macs[i], 6);
    }
    g_en_pair_group_set = true;
    g_en_has_feedback = false;
    g_en_follower_count = 0;
    g_en_leader_count = 0;
    ESP_LOGI(TAG_EN, "Pair group set: %d MACs", g_en_pair_mac_count);
}

static void espnow_clear_pair_group() {
    g_en_pair_group_set = false;
    g_en_pair_mac_count = 0;
    g_en_has_feedback = false;
    ESP_LOGI(TAG_EN, "Pair group cleared");
}

// ============ Receive callback ============

static void espnow_recv_cb(const esp_now_recv_info_t* info,
                            const uint8_t* data, int len) {
    if (!data || len < 3) return;
    if (data[0] != EN_HEAD || data[len - 1] != EN_TAIL) return;

    uint8_t cmd = data[1];

    // WiFi 分享 (不过滤 MAC — 任何设备都可以分享 WiFi)
    if (cmd == EN_CMD_WIFI_SHARE) {
        if (len < 5) return;
        int pos = 2;
        uint8_t ssid_len = data[pos++];
        if (ssid_len > 32 || pos + ssid_len + 1 >= len) return;
        memcpy(g_en_shared_ssid, &data[pos], ssid_len);
        g_en_shared_ssid[ssid_len] = '\0';
        pos += ssid_len;
        uint8_t pass_len = data[pos++];
        if (pass_len > 64 || pos + pass_len >= len) return;
        memcpy(g_en_shared_pass, &data[pos], pass_len);
        g_en_shared_pass[pass_len] = '\0';
        ESP_LOGI(TAG_EN, "WiFi shared from " MACSTR ": SSID=%s",
                 MAC2STR(info->src_addr), g_en_shared_ssid);
        g_en_wifi_share_received = true;
        return;
    }

    // ===== 以下命令需要 MAC 白名单校验 =====
    if (!_mac_in_pair_group(info->src_addr)) return;  // 非配对组设备, 丢弃

    if (cmd == EN_CMD_FEEDBACK) {
        // Follower 反馈包 (0x31): 包含 follower 位置 + leader 位置
        // 格式: [0xAA][0x31][deviceId][f_count][l_count][timestamp:4B][seq]
        //        [f_servos: f_count * {id,pos_hi,pos_lo}]
        //        [l_servos: l_count * {id,pos_hi,pos_lo}]
        //        [0x55]
        if (len < 11) return;
        uint8_t f_count = data[3];
        uint8_t l_count = data[4];
        if (f_count > EN_MAX_SERVOS) f_count = EN_MAX_SERVOS;
        if (l_count > EN_MAX_SERVOS) l_count = EN_MAX_SERVOS;
        int expected = 10 + (f_count + l_count) * 3 + 1;
        if (len < expected) return;

        int pos = 10;  // skip header (10 bytes: head+cmd+devid+f_cnt+l_cnt+ts4+seq)
        // Critical section: 防止 camera stream task 读取时 torn read
        // mux_espnow_data 在 ws_cam.h 中定义 (espnow_listener.h 在 ws_cam.h 中被 include)
        taskENTER_CRITICAL(&mux_espnow_data);
        g_en_follower_count = f_count;
        for (int i = 0; i < f_count; i++) {
            g_en_follower_pos[i].id = data[pos++];
            g_en_follower_pos[i].pos = (uint16_t)data[pos] | ((uint16_t)data[pos+1] << 8);
            pos += 2;
        }
        g_en_leader_count = l_count;
        for (int i = 0; i < l_count; i++) {
            g_en_leader_pos[i].id = data[pos++];
            g_en_leader_pos[i].pos = (uint16_t)data[pos] | ((uint16_t)data[pos+1] << 8);
            pos += 2;
        }
        taskEXIT_CRITICAL(&mux_espnow_data);
        g_en_feedback_recv_ms.store(esp_log_timestamp(), std::memory_order_relaxed);
        g_en_has_feedback = true;
    }
}

// ============ Init (receive-only, works in AP or STA mode) ============

static bool espnow_listener_init() {
    // ESP-NOW needs WiFi started. In AP mode it's already started.
    esp_err_t err = esp_now_init();
    if (err == ESP_ERR_ESPNOW_INTERNAL) {
        // Already initialized
        ESP_LOGW(TAG_EN, "ESP-NOW already initialized");
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG_EN, "ESP-NOW init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_EN, "ESP-NOW register recv cb failed");
        esp_now_deinit();
        return false;
    }

    ESP_LOGI(TAG_EN, "ESP-NOW listener ready");
    return true;
}

// ============ Broadcast WiFi credentials ============

static void espnow_broadcast_wifi(const char* ssid, const char* pass) {
    // Add broadcast peer (if not already added)
    static bool broadcast_peer_added = false;
    if (!broadcast_peer_added) {
        esp_now_peer_info_t peer = {};
        memset(peer.peer_addr, 0xFF, 6);  // FF:FF:FF:FF:FF:FF
        peer.channel = 0;  // current channel
        peer.encrypt = false;
        esp_err_t err = esp_now_add_peer(&peer);
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGE(TAG_EN, "Add broadcast peer failed: %s", esp_err_to_name(err));
            return;
        }
        broadcast_peer_added = true;
    }

    uint8_t ssid_len = strlen(ssid);
    uint8_t pass_len = strlen(pass);
    // Build packet: [0xAA][0xF1][ssid_len][ssid][pass_len][pass][0x55]
    uint8_t buf[128];
    int pos = 0;
    buf[pos++] = EN_HEAD;
    buf[pos++] = EN_CMD_WIFI_SHARE;
    buf[pos++] = ssid_len;
    memcpy(&buf[pos], ssid, ssid_len); pos += ssid_len;
    buf[pos++] = pass_len;
    memcpy(&buf[pos], pass, pass_len); pos += pass_len;
    buf[pos++] = EN_TAIL;

    // Send 5 times with interval for reliability
    uint8_t broadcast_addr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (int i = 0; i < 5; i++) {
        esp_err_t err = esp_now_send(broadcast_addr, buf, pos);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_EN, "WiFi share send %d failed: %s", i, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG_EN, "WiFi shared: SSID=%s (5 broadcasts)", ssid);
}
