/**
 * main.cpp - Box2Robot CAM (ESP32-S3 M5AtomS3R-CAM)
 *
 * Boot flow:
 *   NVS → Camera → Audio → WiFi (saved or AP+Portal) → WS cloud
 *   AP mode: voice announce WiFi info every 3s, wait for portal config
 *   STA mode: WS connect to server, camera + audio streaming
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "driver/ledc.h"
#include "nvs_flash.h"
#include "config.h"
#include "audio.h"
#include "tts_simple.h"
#include "wifi_prov.h"
#include "device_reg.h"
#include "ws_cam.h"
#include "ota_cam.h"
#include "espnow_listener.h"
#include "ntp_sync.h"
#include "imu_bmi270.h"

static const char* TAG = "cam";
bool audio_ok = false;  // shared with ws_cam.h for TTS prompts via extern

// OTA check via HTTP (like ARM's OtaManager::checkUpdate)
static void ota_check_once() {
    char url[384];
    snprintf(url, sizeof(url), "%s/api/ota/check?version=%s&model=mingle_bot",
             SERVER_BASE_URL, FW_VERSION);
    ESP_LOGI(TAG, "[OTA] Checking: %s", url);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[OTA] HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }
    esp_http_client_fetch_headers(client);

    char buf[512] = {};
    int read_len = esp_http_client_read(client, buf, sizeof(buf) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) return;
    buf[read_len] = '\0';

    // Simple JSON parse for "update_available" and "version"
    if (strstr(buf, "\"update_available\":true") || strstr(buf, "\"update_available\": true")) {
        // Extract version
        const char* vp = strstr(buf, "\"version\":\"");
        if (!vp) vp = strstr(buf, "\"version\": \"");
        char new_ver[32] = "?";
        if (vp) {
            vp = strchr(vp, ':') + 1;
            while (*vp == ' ' || *vp == '"') vp++;
            int i = 0;
            while (vp[i] && vp[i] != '"' && i < 31) { new_ver[i] = vp[i]; i++; }
            new_ver[i] = '\0';
        }
        // Check if URL available
        const char* up = strstr(buf, "\"url\":\"http");
        if (up) {
            // Extract URL for OTA
            const char* us = strchr(up + 6, '"') + 1;
            int i = 0;
            while (us[i] && us[i] != '"' && i < (int)sizeof(g_ota_url) - 1) {
                g_ota_url[i] = us[i]; i++;
            }
            g_ota_url[i] = '\0';
            ESP_LOGW(TAG, "[OTA] Update available: %s → %s (auto-download)", FW_VERSION, new_ver);
            g_ota_pending = true;
        } else {
            ESP_LOGW(TAG, "[OTA] Update available: %s → %s (no bin, manual flash needed)", FW_VERSION, new_ver);
        }
    } else {
        ESP_LOGI(TAG, "[OTA] Firmware up to date (v%s)", FW_VERSION);
    }
}

// ============ Servo PWM Test (G1=GPIO1, G2=GPIO2) ============
// LEDC Timer 1 (Timer 0 is camera XCLK), 50Hz, 14-bit resolution
// 50Hz @ 14-bit: period=20ms, 1 tick ≈ 1.22μs
// Servo pulse: 500μs~2500μs → duty 410~2048

#define SERVO_GPIO_1       1
#define SERVO_GPIO_2       2
#define SERVO_DUTY_MIN     410    // ~500μs  → 0°
#define SERVO_DUTY_MAX     2048   // ~2500μs → 280°
#define SERVO_ANGLE_RANGE  280    // 舵机实际角度范围
#define SERVO_SWEEP_MS     800    // 每次随机切换间隔 (ms)

static void servo_pwm_init() {
    ledc_timer_config_t timer = {};
    timer.speed_mode      = LEDC_LOW_SPEED_MODE;
    timer.timer_num       = LEDC_TIMER_1;
    timer.duty_resolution = LEDC_TIMER_14_BIT;
    timer.freq_hz         = 50;
    timer.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    ledc_channel_config_t ch1 = {};
    ch1.speed_mode = LEDC_LOW_SPEED_MODE;
    ch1.channel    = LEDC_CHANNEL_2;
    ch1.timer_sel  = LEDC_TIMER_1;
    ch1.gpio_num   = SERVO_GPIO_1;
    ch1.duty       = (SERVO_DUTY_MIN + SERVO_DUTY_MAX) / 2;  // 中位
    ledc_channel_config(&ch1);

    ledc_channel_config_t ch2 = {};
    ch2.speed_mode = LEDC_LOW_SPEED_MODE;
    ch2.channel    = LEDC_CHANNEL_3;
    ch2.timer_sel  = LEDC_TIMER_1;
    ch2.gpio_num   = SERVO_GPIO_2;
    ch2.duty       = (SERVO_DUTY_MIN + SERVO_DUTY_MAX) / 2;
    ledc_channel_config(&ch2);

    ESP_LOGI("servo", "PWM init: G1(GPIO%d) G2(GPIO%d) 50Hz 14-bit", SERVO_GPIO_1, SERVO_GPIO_2);
}

static void servo_set_duty(ledc_channel_t ch, uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

// Global servo control — called from WS callback (ws_cam.h servo_cmd)
// angle: 0 ~ SERVO_ANGLE_RANGE (280°), 中位 = 140°
// pulse: 500μs ~ 2500μs, duty: 410 ~ 2048 (14-bit 50Hz)
void servo_set_angle(int channel, int angle) {
    if (angle < 0) angle = 0;
    if (angle > SERVO_ANGLE_RANGE) angle = SERVO_ANGLE_RANGE;
    uint32_t duty = SERVO_DUTY_MIN + (uint32_t)((int64_t)angle * (SERVO_DUTY_MAX - SERVO_DUTY_MIN) / SERVO_ANGLE_RANGE);
    if (channel == 1) servo_set_duty(LEDC_CHANNEL_2, duty);
    else if (channel == 2) servo_set_duty(LEDC_CHANNEL_3, duty);
    ESP_LOGI("servo", "CH%d → %d° (duty=%lu)", channel, angle, (unsigned long)duty);
}

static void servo_random_task(void* arg) {
    ESP_LOGI("servo", "Random sweep task started (range: %d~%d duty, interval: %dms)",
             SERVO_DUTY_MIN, SERVO_DUTY_MAX, SERVO_SWEEP_MS);
    uint32_t range = SERVO_DUTY_MAX - SERVO_DUTY_MIN;
    while (true) {
        uint32_t d1 = SERVO_DUTY_MIN + (esp_random() % range);
        uint32_t d2 = SERVO_DUTY_MIN + (esp_random() % range);
        servo_set_duty(LEDC_CHANNEL_2, d1);
        servo_set_duty(LEDC_CHANNEL_3, d2);
        // 换算角度方便看日志
        int angle1 = (d1 - SERVO_DUTY_MIN) * 180 / range;
        int angle2 = (d2 - SERVO_DUTY_MIN) * 180 / range;
        ESP_LOGI("servo", "G1=%d° G2=%d° (duty: %lu, %lu)", angle1, angle2,
                 (unsigned long)d1, (unsigned long)d2);
        vTaskDelay(pdMS_TO_TICKS(SERVO_SWEEP_MS));
    }
}

// ============ Camera ============

static bool camera_init() {
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << CAM_PIN_POWER);
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)CAM_PIN_POWER, 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    camera_config_t config = {};
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_d7 = CAM_PIN_D7; config.pin_d6 = CAM_PIN_D6;
    config.pin_d5 = CAM_PIN_D5; config.pin_d4 = CAM_PIN_D4;
    config.pin_d3 = CAM_PIN_D3; config.pin_d2 = CAM_PIN_D2;
    config.pin_d1 = CAM_PIN_D1; config.pin_d0 = CAM_PIN_D0;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.xclk_freq_hz = CAM_XCLK_FREQ;
    config.ledc_timer   = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;   // GC0308 has no HW JPEG — use RGB565 + software frame2jpg
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;                 // Not used for RGB565, but set for completeness
    config.fb_count     = CAM_FB_COUNT;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = 1;   // SCCB 用 I2C_NUM_1 (GPIO 12/9), Audio 用 I2C_NUM_0 (GPIO 38/39)

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Camera init failed: 0x%x", err); return false; }
    ESP_LOGI(TAG, "Camera init OK (GC0308 RGB565 → software JPEG)");
    return true;
}

// Reinit camera with a new frame size (runtime resolution switch only)
// MUST be called with g_cam_reinit_in_progress=true to pause streaming task
bool camera_reinit(framesize_t fs) {
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));

    camera_config_t config = {};
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_d7 = CAM_PIN_D7; config.pin_d6 = CAM_PIN_D6;
    config.pin_d5 = CAM_PIN_D5; config.pin_d4 = CAM_PIN_D4;
    config.pin_d3 = CAM_PIN_D3; config.pin_d2 = CAM_PIN_D2;
    config.pin_d1 = CAM_PIN_D1; config.pin_d0 = CAM_PIN_D0;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.xclk_freq_hz = CAM_XCLK_FREQ;
    config.ledc_timer   = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;   // GC0308: RGB565 + software JPEG
    config.frame_size   = fs;
    config.jpeg_quality = 12;
    config.fb_count     = CAM_FB_COUNT;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = 1;   // SCCB 用 I2C_NUM_1 (GPIO 12/9), Audio 用 I2C_NUM_0 (GPIO 38/39)

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x (framesize=%d)", err, fs);
        // Fallback to VGA
        if (fs != FRAMESIZE_VGA) {
            config.frame_size = FRAMESIZE_VGA;
            err = esp_camera_init(&config);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "Fallback to VGA OK");
                return true;
            }
        }
        return false;
    }
    ESP_LOGI(TAG, "Camera init OK (framesize=%d)", fs);
    return true;
}

// ============ Entry ============

extern "C" void app_main(void) {
    // Wait for USB Serial/JTAG console ready
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGW(TAG, "=== Mingle Bot %s ===", FW_VERSION);

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "NVS no free pages — erasing");
        nvs_flash_erase(); nvs_flash_init();
    } else if (ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS version changed — erasing and reinit");
        nvs_flash_erase(); nvs_flash_init();
    }

    // 强制重写 WiFi 凭据 (确保最新配置生效)
    {
        // 清除旧的 WiFi 配置
        nvs_handle_t h;
        if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    if (nvs_wifi_count() == 0) {
        ESP_LOGI(TAG, "No saved WiFi — writing default credentials");
        // nvs_wifi_save("YourSSID", "YourPassword");  // TODO: configure via AP portal
    }

    // Servo PWM init (不启动随机测试任务, 由服务器 WS 命令控制)
    servo_pwm_init();

    // Audio — 占住 I2C_NUM_0 (GPIO 38/39) 给 ES8311 + BMI270
    audio_ok = audio_init();
    if (!audio_ok) ESP_LOGW(TAG, "Audio init failed");
    if (audio_ok) tts_play_prompt("prompt_mingle_once");  // 上电第一声 "Mingle~"

    // IMU (BMI270) — 共享 Audio I2C 总线 (GPIO 38/39, I2C_NUM_0)
    if (imu_init()) {
        imu_start_task();
    } else {
        ESP_LOGW(TAG, "IMU init failed — BMI270 not detected on audio I2C bus");
    }

    // Camera — SCCB 用 I2C_NUM_1 (GPIO 12/9), 不跟 Audio/IMU 冲突
    bool camera_ok = camera_init();
    if (!camera_ok) {
        ESP_LOGE(TAG, "Camera init failed — continuing without camera");
    }

    // WiFi init (creates netif, event loop)
    wifi_prov_init();

    // Load saved config from NVS + apply
    runtime_config_load();
    runtime_config_apply_resolution();
    if (audio_ok) runtime_config_apply_audio();

    // ---- WiFi connection ----
    bool connected = false;

    if (wifi_prov_has_saved()) {
        ESP_LOGI(TAG, "Saved WiFi found, trying STA...");
        connected = wifi_prov_try_saved();
    }

    if (!connected) {
        // AP + Captive Portal mode
        ESP_LOGW(TAG, "No WiFi — starting AP: %s", wifi_prov_get_ap_ssid());
        wifi_prov_start_ap();
        wifi_prov_start_portal();

        // Start ESP-NOW listener in AP mode — receive WiFi from companion
        espnow_listener_init();

        // Loop: announce WiFi + check for portal config + check ESP-NOW WiFi share
        // 播报退避: 15s, 30s, 30s, 1min, 3min, 5min, 10min, 之后每10min
        static const int announce_intervals[] = {15, 30, 30, 60, 180, 300, 600};
        static const int announce_intervals_count = sizeof(announce_intervals) / sizeof(announce_intervals[0]);
        int announce_idx = 0;
        int ticks_since_announce = 0;
        int next_announce_ticks = 0;  // 第一次立即播报
        while (!connected) {
            if (audio_ok && ticks_since_announce >= next_announce_ticks) {
                tts_announce_wifi(wifi_prov_get_ap_ssid(), AP_PASSWORD);
                // 设置下一次播报间隔
                int interval_s = announce_intervals[announce_idx < announce_intervals_count ? announce_idx : announce_intervals_count - 1];
                next_announce_ticks = interval_s * 10;  // 100ms per tick
                ticks_since_announce = 0;
                announce_idx++;
            }
            ticks_since_announce++;

            // Check if companion shared WiFi via ESP-NOW
            if (g_en_wifi_share_received) {
                g_en_wifi_share_received = false;
                ESP_LOGI(TAG, "Companion shared WiFi: %s — trying...", g_en_shared_ssid);
                // ESP-NOW WiFi 共享连接中 (不播声音, 启动只播2次 Mingle)

                // Save + try connecting
                nvs_wifi_save(g_en_shared_ssid, g_en_shared_pass);
                wifi_prov_stop_portal();
                esp_now_deinit();  // Free before WiFi mode switch

                if (wifi_prov_connect_sta(g_en_shared_ssid, g_en_shared_pass)) {
                    connected = true;
                    break;
                }
                // Failed — back to AP
                ESP_LOGW(TAG, "Shared WiFi failed, back to AP");
                wifi_prov_start_ap();
                wifi_prov_start_portal();
                espnow_listener_init();
            }

            // Check if user submitted WiFi via portal
            if (wifi_prov_handle_pending()) {
                connected = true;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(100));
        }

        wifi_prov_stop_portal();
    }

    ESP_LOGI(TAG, "WiFi connected! IP: %s", wifi_prov_get_sta_ip());
    g_wifi_runtime_reconnect = true;  // 启用运行时自动重连
    ntp_sync_start();  // NTP time sync (non-blocking, background)

    // Broadcast WiFi to help companions still in AP mode, then shut down ESP-NOW
    // (ESP-NOW shares RF with WiFi — keeping it active degrades WiFi throughput)
    {
        espnow_listener_init();  // Need init to send broadcast
        char ssid[33] = {}, pass[65] = {};
        int cnt = nvs_wifi_count();
        if (cnt > 0 && nvs_wifi_load(cnt - 1, ssid, pass)) {
            espnow_broadcast_wifi(ssid, pass);
        }
        // WiFi 流媒体优先: 关闭 ESP-NOW 释放 RF 带宽
        // 录制时由 record_start_cam WS 命令按需重新初始化
        esp_now_deinit();
        ESP_LOGI(TAG, "ESP-NOW shut down — will re-init on record_start_cam");
    }

    // ---- Device Registration + Binding ----
    device_reg_gen_ids();

    if (!device_reg_is_bound()) {
        // 未绑定: 先播绑定引导提示
        if (audio_ok) {
            tts_play_prompt("prompt_bind_wait");  // "请在手机上输入绑定码完成绑定"
        }

        reg_result_t reg = device_reg_activate();

        if (reg == REG_NEED_BIND) {
            ESP_LOGW(TAG, "========================================");
            ESP_LOGW(TAG, "  Bind code: %s", device_reg_get_bind_code());
            ESP_LOGW(TAG, "  Visit your server to bind");
            ESP_LOGW(TAG, "========================================");
            // 播报绑定码: "绑定码是 X X X X X X"
            if (audio_ok) tts_announce_code(device_reg_get_bind_code());

            int poll_count = 0;
            int64_t last_announce_ms = 0;
            while (!device_reg_is_bound() && poll_count < 600) {
                if (poll_count % 3 == 0) {
                    device_reg_activate();
                    if (device_reg_is_bound()) break;
                }

                // 每 15 秒重复播报绑定码
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (audio_ok && poll_count > 0 && (now_ms - last_announce_ms) > 15000) {
                    tts_announce_code(device_reg_get_bind_code());
                    last_announce_ms = now_ms;
                }

                vTaskDelay(pdMS_TO_TICKS(1000));
                poll_count++;
            }
        }

        if (device_reg_is_bound()) {
            ESP_LOGI(TAG, "Device bound! Token: %.8s...", device_reg_get_token());
            if (audio_ok) tts_play_prompt("prompt_bind_ok");  // "设备绑定成功"
        } else if (reg == REG_ERROR) {
            ESP_LOGW(TAG, "Server unreachable, continuing without binding");
            if (audio_ok) tts_play_prompt("prompt_server_fail");
        }
    } else {
        ESP_LOGI(TAG, "Already bound, token: %.8s...", device_reg_get_token());
    }

    // ---- WebSocket to cloud server ----
    bool ws_started = false;
    if (device_reg_is_bound()) {
        ESP_LOGI(TAG, "Connecting to %s ...", device_reg_get_ws_url());
        ws_started = ws_cam_start();
        if (ws_started) {
            ESP_LOGI(TAG, "WebSocket connected");
        } else {
            ESP_LOGW(TAG, "WS failed, will retry in main loop");
        }
    }

    // OTA rollback: 标记当前固件有效，防止回滚到旧版本
    esp_ota_mark_app_valid_cancel_rollback();

    // ---- OTA check on startup (after WS connects) ----
    if (ws_started) {
        ota_check_once();
    }

    // ---- Main loop ----
    int loop_tick = 0;
    bool ota_checked = ws_started;  // track if we've done OTA check
    while (true) {
        // 延迟分辨率切换 (从 WS 回调 defer 到主循环, 避免阻塞 WS 任务)
        cam_handle_pending_resolution();

        // OTA check (runs from main task which has 16KB stack — enough for TLS)
        if (g_ota_pending) {
            g_ota_pending = false;
            ESP_LOGW(TAG, "=== OTA UPDATE STARTING ===");
            ota_cam_start_update(g_ota_url, [](int stage) {
                switch (stage) {
                    case 0: tts_play_prompt("prompt_ota_download"); break;
                    case 1: tts_play_prompt("prompt_ota_flash");    break;
                    case 2: tts_play_prompt("prompt_ota_reboot");   break;
                }
            });
            ESP_LOGE(TAG, "OTA failed, continuing...");
        }

        // Deferred restart (from WS callback auth_fail/unbind/factory_reset)
        if (g_restart_pending) {
            ESP_LOGW(TAG, "Deferred restart executing...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }

        // WiFi 断连检测: 自动重连耗尽时重启
        if (strlen(wifi_prov_get_sta_ip()) == 0 && g_wifi_reconnect_count >= WIFI_MAX_FAST_RECONNECTS) {
            ESP_LOGW(TAG, "WiFi reconnect exhausted, restarting...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }

        // WS 重连策略:
        // esp_websocket_client 有内置自动重连 (reconnect_timeout_ms=1000)
        // 主循环只在以下情况 destroy+recreate:
        //   1. WS 从未启动成功 (ws_started=false)
        //   2. WS 连续断开超过 60s (内置重连失败, 可能需要重建 TLS/TCP)
        if (!ws_started && device_reg_is_bound() && strlen(wifi_prov_get_sta_ip()) > 0
            && (loop_tick % 100 == 0) && loop_tick > 0) {
            ESP_LOGI(TAG, "WS not started, attempting connection...");
            ws_started = ws_cam_start();
        }
        // 内置重连持续失败 60s → 强制 destroy+recreate
        if (ws_started && !g_ws_connected && strlen(wifi_prov_get_sta_ip()) > 0
            && g_ws_last_disconnect_us > 0
            && (esp_timer_get_time() - g_ws_last_disconnect_us) > 60000000LL
            && (loop_tick % 100 == 0)) {
            ESP_LOGW(TAG, "WS down >60s despite auto-reconnect, force restart");
            ws_started = ws_cam_start();
        }

        // OTA check after late WS connect
        if (ws_started && !ota_checked) {
            ota_checked = true;
            ota_check_once();
        }

        // Log status every 30s (方便诊断连接问题, 复制给开发者即可定位)
        if (loop_tick % 300 == 0 && loop_tick > 0) {
            wifi_ap_record_t ap_info;
            int rssi = 0;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) rssi = ap_info.rssi;
            ESP_LOGI(TAG, "[CAM-STATUS] heap=%u/%u psram=%u/%u ws=%s fps=%u rssi=%d "
                     "conn=%u disc=%u send_fail=%u wifi=%s",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                     ws_cam_is_connected() ? "OK" : "NO",
                     (unsigned)g_cam_stream_fps,
                     rssi,
                     g_ws_connect_count, g_ws_disconnect_count,
                     g_ws_send_fail_total,
                     strlen(wifi_prov_get_sta_ip()) > 0 ? wifi_prov_get_sta_ip() : "DOWN");
        }

        loop_tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
