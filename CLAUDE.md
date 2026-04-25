# Mingle Bot - 肩上社交萌宠 (固件端)

## Overview
Mingle Bot 固件模块。基于 M5Stack AtomS3R-M12 + Atomic Echo Base，内置于毛绒小鸟玩具中。提供摄像头图像采集、麦克风实时录音、扬声器播放、陀螺仪状态检测、WiFi 配网(语音引导)、云端 WebSocket 通信。

**产品定位**: 社交场合的僚机 -- 帮你破冰、活跃气氛、听你指挥

**Bot 端职责 (只采集+输出, 不做 AI)**:
- 摄像头 3Hz 拍照上传 -> 云端人脸识别发现可搭话的人
- 麦克风持续录音上传 -> 云端 ASR 理解对话内容
- 接收云端 TTS 音频 -> 扬声器播放 (小鸟说话)
- 接收舵机指令 -> 转头/点头 (配合说话)
- 接收语音指令结果 -> 执行 (切换模式/定时器等)

**硬件**: ESP32-S3-PICO-1-N8R8 (8MB Flash + 8MB PSRAM) + GC0308 0.3MP Camera (VGA max) + ES8311 Audio Codec + NS4150B Amp + 陀螺仪 + 2-DOF Servo (yaw/pitch)

**框架**: Pure ESP-IDF 5.4 (非 Arduino)

**固件版本**: 0.7.0

## Architecture

```
Mingle_bot/
+-- main/
|   +-- main.cpp              # 入口: app_main, 启动流程, HTTP server, 主循环; NTP时间同步
|   +-- config.h              # GPIO/引脚/常量/服务器地址定义
|   +-- audio.h               # ES8311 codec + I2S + 麦克风采集 + HTTP 音频流
|   +-- tts_simple.h          # 语音播报: PCM->I2S 播放, 拼读数字/字母/提示
|   +-- voice_clips.h         # 语音索引 (自动生成, 60个clip的extern声明+数组)
|   +-- wifi_prov.h           # WiFi 配网: Multi-WiFi NVS(3槽) + AP + Captive Portal + DNS劫持
|   +-- device_reg.h          # 设备注册: MAC->DeviceID, activate API, 绑定码, Token
|   +-- ws_cam.h              # WebSocket: 认证/心跳/摄像头流/麦克风流/命令处理; 帧头8字节时间戳; WS buffer 64KB; snapshot deferred
|   +-- ota_cam.h             # OTA 固件升级 (HTTPS, 双OTA分区)
|   +-- runtime_config_cam.h  # 远程配置: FPS/JPEG质量/音量/分辨率
|   +-- espnow_listener.h     # ESP-NOW 监听 + WiFi 自动分享
|   +-- record_buffer.h       # 录音环形缓冲
|   +-- ntp_sync.h            # NTP 时间同步
|   +-- adpcm.h               # ADPCM 音频编解码
|   +-- voice_digits.cpp      # [生成] 数字0-9 PCM数据 (~130KB)
|   +-- voice_letters.cpp     # [生成] 字母A-Z PCM数据 (~350KB)
|   +-- voice_prompts.cpp     # [生成] 24个提示语音 PCM数据 (~1.4MB)
|   +-- voice_data/           # 60个 PCM 原始文件 (16kHz 16bit mono, ~1.9MB)
|   +-- idf_component.yml     # ESP-IDF 组件
|   +-- CMakeLists.txt
+-- scripts/
|   +-- generate_voice.py     # edge-tts 语音生成脚本
|   +-- voice_prompts.md      # 语音提示清单
|   +-- idf_monitor.py        # PlatformIO 串口监视脚本
|   +-- monitor.bat           # Windows 串口监视快捷方式
+-- docs/
|   +-- atoms3r_m12_hardware.md       # 硬件引脚参考
|   +-- es8311_audio_troubleshoot.md  # ES8311 调试手册
+-- backup/                   # 大改动前的文件备份
+-- platformio.ini
+-- sdkconfig.defaults
+-- partitions.csv
+-- CLAUDE.md                 # 本文件
+-- README.md
```

## 核心工作模式

### 肩上模式 (Shoulder Mode)
- **触发**: 陀螺仪检测到动态变化 (佩戴在肩上)
- **摄像头**: 3Hz JPEG 上传 -> 云端人脸识别 -> 发现可搭话的人
- **麦克风**: 持续录音上传 -> 云端 ASR -> 理解对话内容和氛围
- **输出**: 云端下发 TTS -> 小鸟说话 (破冰/活跃氛围/执行指令)
- **动作**: 云端下发舵机指令 -> 转头看向对方 / 点头配合

### 桌面模式 (Nest Mode)
- **触发**: 陀螺仪检测到稳定放置
- **复盘**: 播报今日社交摘要
- **充电**: 鸟窝充电座

## TTS 语音系统

### 语音引擎
- **生成工具**: [edge-tts](https://github.com/rany2/edge-tts) (Microsoft Azure TTS, 免费)
- **声音**: `zh-CN-YunxiaNeural` (云夏, 小男孩声)
- **格式**: 16-bit PCM, 16000Hz, Mono
- **语速**: rate="-10%"
- **嵌入方式**: PCM->C数组 (.cpp), 编译时链接到 Flash

### 语音素材 (60 clips, ~1.9MB)
| 类别 | 数量 | 用途 |
|------|------|------|
| 数字 | 10 | 配对码播报 |
| 字母 | 26 | SSID 名称播报 |
| 配网提示 | 7 | AP 模式语音引导 |
| 绑定提示 | 4 | 设备绑定流程 |
| 系统提示 | 10 | 系统状态播报 |
| 语音对话 | 3 | AI 对话 |

### 重新生成语音
```bash
pip install edge-tts pydub imageio-ffmpeg
cd Mingle_bot
python scripts/generate_voice.py
```

## Boot Pipeline

```
app_main() (16KB stack)
  |
  +-- NVS init
  +-- Button init (GPIO 41)
  +-- Camera init (GC0308, VGA RGB565, PSRAM双缓冲, 软件JPEG)
  +-- WiFi netif init
  +-- Audio init (I2C->PI4IOE->I2S->ES8311->测试音->麦克风测试)
  |
  +-- WiFi connection
  |   +-- 有保存 -> wifi_prov_try_saved() 最多3个WiFi循环尝试3分钟
  |   +-- 无保存/失败 -> AP模式 (Mingle_XXXX + 密码12345678)
  |       +-- Captive Portal (HTTP 80, DNS劫持)
  |       +-- 每3秒语音播报: WiFi名+密码
  |
  +-- NTP 时间同步
  +-- Device Registration (未绑定 -> 语音播报绑定码)
  +-- WebSocket 连接 (auth -> device_info -> 10s心跳)
  +-- Local HTTP Server (端口80, 调试用: /stream, /audio)
  +-- 语音: "设备已就绪"
  |
  +-- Main Loop (100ms tick)
      +-- IMU: 判断肩上/桌面模式
      +-- Button: 短按重启, 长按清NVS
      +-- OTA: 下载+烧录+重启
      +-- WS重连: 每30秒重试
```

## FreeRTOS Tasks

| 任务 | 核心 | 栈 | 优先级 | 职责 |
|------|------|-----|--------|------|
| main (app_main) | 0 | 16KB | - | 启动 + 主循环 + IMU + OTA |
| mic_cap | 1 | 4KB | 3 | I2S RX -> Ring Buffer |
| ws_cam | 1 | 4KB | 4 | 摄像头 JPEG -> WS Binary (3Hz) |
| ws_audio | 1 | 4KB | 3 | Ring Buffer -> WS Binary |
| ws_hb | 0 | 3KB | 2 | WS 认证 + 心跳 + IMU 上报 |
| dns | 0 | 4KB | 2 | DNS劫持 (仅AP模式) |

## Hardware Pin Map

| Function | GPIO | Notes |
|----------|------|-------|
| Camera XCLK | 21 | 20MHz |
| Camera SCCB SDA/SCL | 12/9 | I2C |
| Camera Data D0-D7 | 3,42,46,48,4,17,11,13 | DVP 8-bit |
| Camera VSYNC/HREF/PCLK | 10/14/40 | |
| Camera Power | 18 | Active LOW |
| Audio I2C SDA/SCL | 38/39 | ES8311(0x18) + PI4IOE(0x43) |
| Audio I2S BCK/WS/DOUT/DIN | 8/6/5/7 | 16kHz 16bit stereo |
| Servo Yaw | 1 (G1) | PWM, 水平转头 |
| Servo Pitch | 2 (G2) | PWM, 上下点头 |
| Button | 41 | Active LOW |

## Key Technical Decisions

### ES8311 Audio (CRITICAL)
**必须使用 M5Atomic-EchoBase 库的寄存器序列和系数表**, esp-adf 的系数表值不同会导致无声。

- I2S: **16-bit stereo** (BCLK=512kHz), MCLK=unused
- ES8311 SDP: **32-bit** (ES8311_RESOLUTION_32)
- PI4IOE: 完整 6 步初始化
- 音量: REG32 默认 0xD8 (85%)

### I2C Driver Conflict
esp32-camera 用新版 I2C driver, 不能混用旧版。**不能用 `espressif/es8311` 组件**, 必须手写寄存器。

### WebSocket Protocol

| Direction | Format | Content |
|-----------|--------|---------|
| ESP->Server Text | JSON | auth, device_info, device_status, imu_state |
| ESP->Server Binary | JPEG | 摄像头帧 (3Hz, 软件JPEG编码) |
| ESP->Server Binary | PCM | 麦克风音频 (16kHz 16bit mono) |
| Server->ESP Text | JSON | auth_ok, stream_mode, set_config, ota_update, speak, servo_cmd, timer_event |
| Server->ESP Binary | PCM | TTS回放音频 |

### 分段 TTS 播放
- Server 发 `play_audio_segment` JSON + ADPCM binary
- segments 入队 (ring buffer, max 8 段)
- TTS task 按序播放, 支持 barge-in 中断

## 调试策略 (Log-First)

**修改 2 次未解决 -> 插入 `ESP_LOGI` -> 看串口 -> 根据 log 定位根因**

```cpp
ESP_LOGI(TAG, "[DBG] handler=%s val=%d heap=%u", type, val, esp_get_free_heap_size());
```

串口监视: `cd Mingle_bot && pio device monitor`

## Build & Flash

```bash
cd Mingle_bot
pio run                    # 编译
pio run -t upload          # 烧录 (先关串口)
pio device monitor         # 串口监视 (115200)
```

> sdkconfig.defaults 修改后必须: `rm sdkconfig.esp32s3-cam && rm -rf .pio && pio run`

## Flash Partition (8MB)

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data | 0x9000 | 20KB |
| otadata | data | 0xE000 | 8KB |
| app0 | ota_0 | 0x10000 | 3.75MB |
| app1 | ota_1 | 0x3D0000 | 3.75MB |
| spiffs | data | 0x790000 | 448KB |

## Changelog

### 2026-04-24 (v0.7.1)
- **定位更新**: 社交僚机 -- 破冰/活跃氛围/语音指令

### 2026-04-23 (v0.7.0)
- **项目更名**: Box2Robot CAM -> Mingle Bot
- **核心模式**: 肩上模式 + 桌面模式

### 2026-04-08 (v0.5.1)
- NTP 时间同步, 帧时间戳, WS buffer 扩容

## Troubleshooting

详见 `docs/es8311_audio_troubleshoot.md`
