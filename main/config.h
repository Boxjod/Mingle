/**
 * config.h - Mingle Bot hardware config (M5AtomS3R-CAM + Atomic Echo Base)
 *
 * ESP32-S3 + GC0308 camera (0.3MP VGA), 8MB Flash, 8MB PSRAM
 * 肩上社交萌宠机器人 — 毛绒小鸟形态, 2-DOF servo (yaw/pitch)
 */
#pragma once

// ============ WiFi Provisioning ============
#define AP_SSID_PREFIX    "Mingle_"
#define AP_PASSWORD       "12345678"
#define AP_CHANNEL        1
#define STA_TIMEOUT_MS    10000
#define NVS_NS_WIFI       "wifi"
#define NVS_KEY_SSID      "ssid"
#define NVS_KEY_PASS      "password"

// ============ Server ============
// ESP32 直连 HTTPS 443, Nginx 转发到后端
#define SERVER_BASE_URL   "https://your-server.example.com"  // TODO: replace with your server
#define NVS_NS_AUTH       "auth"
#define NVS_KEY_TOKEN     "token"
#define NVS_KEY_CLIENT_ID "client_id"
#define NVS_KEY_WS_URL    "ws_url"
#define NVS_KEY_DEVICE_ID "device_id"

// ============ Device ============
#define DEVICE_TYPE       "mingle_bot"
#define FW_VERSION        "0.7.1"

// ============ Button ============
#define BTN_GPIO          41

// ============ Camera Pins (M5AtomS3R-CAM) ============
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    21
#define CAM_PIN_SIOD    12   // SCCB SDA
#define CAM_PIN_SIOC    9    // SCCB SCL
#define CAM_PIN_D7      13
#define CAM_PIN_D6      11
#define CAM_PIN_D5      17
#define CAM_PIN_D4      4
#define CAM_PIN_D3      48
#define CAM_PIN_D2      46
#define CAM_PIN_D1      42
#define CAM_PIN_D0      3
#define CAM_PIN_VSYNC   10
#define CAM_PIN_HREF    14
#define CAM_PIN_PCLK    40
#define CAM_PIN_POWER   18   // Active LOW — drive LOW to enable camera

// ============ Camera Settings ============
#define CAM_XCLK_FREQ   20000000  // 20 MHz
#define CAM_JPEG_QUALITY 80       // frame2jpg quality 0-100, higher = better (software JPEG, GC0308 has no HW JPEG)
#define CAM_FB_COUNT     2        // Double buffer in PSRAM

// ============ Audio — ES8311 + NS4150B (Atomic Echo Base) ============
// I2C control bus
#define AUDIO_I2C_SDA       38
#define AUDIO_I2C_SCL       39
#define AUDIO_I2C_PORT      I2C_NUM_0   // Camera SCCB uses I2C_NUM_1

// I2S audio data
#define AUDIO_I2S_NUM       I2S_NUM_0
#define AUDIO_I2S_BCK       8
#define AUDIO_I2S_WS        6
#define AUDIO_I2S_DOUT      5   // ESP32 -> ES8311 DAC (speaker)
#define AUDIO_I2S_DIN       7   // ES8311 ADC -> ESP32 (mic)

// ES8311 codec
#define ES8311_ADDR         0x18
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS          16

// PI4IOE5V6408 I/O expander (controls NS4150B amp mute)
#define PI4IOE_ADDR         0x43

// ============ Servo — 2-DOF Head (PWM via LEDC) ============
#define SERVO_YAW_GPIO      1    // G1 — 水平转头 (yaw)
#define SERVO_PITCH_GPIO    2    // G2 — 上下点头 (pitch)

// ============ HTTP Server ============
#define HTTP_STREAM_PORT 80
