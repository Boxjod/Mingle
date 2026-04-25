# Mingle Bot - 肩上社交萌宠 (固件端)

> 一只栖息在你肩膀上的 AI 小鸟，帮你破冰、活跃气氛、听你指挥。

#AttraX_Spring_Hackathon

## What is Mingle Bot?

Mingle Bot 是一只毛绒小鸟形态的社交僚机的硬件端。把它放在肩膀上，它是你社交场合的得力搭档:

- **眼睛** -- 摄像头 3Hz 观察周围，发现可以搭话的人
- **耳朵** -- 麦克风持续录音，实时上传云端理解对话
- **嘴巴** -- 扬声器播放 TTS，替你开口打招呼、抛话题、活跃气氛
- **动作** -- 2-DOF 舵机转头/点头，像一只真正活着的小鸟

所有 AI 决策在云端完成，Bot 只负责采集和输出。

## How It Works

**肩上模式** (陀螺仪检测到佩戴):
1. 摄像头 3Hz 拍照 -> 上传 Server -> Soul 人脸识别 (发现落单的人)
2. 麦克风持续录音 -> 上传 Server -> Audio ASR (理解对话内容)
3. Server 社交决策 -> TTS 生成 -> 小鸟扬声器说话 + 舵机转头配合
4. 语音指令 "mingle + 指令" -> Server 解析 -> 执行 (关闭摄像头/设定时器/等)

**桌面模式** (鸟窝充电座):
1. 陀螺仪检测稳定放置 -> 触发复盘
2. 播报今日社交摘要 -> 充电待机

## Hardware

| Component | Model | Role |
|-----------|-------|------|
| Controller | ESP32-S3-PICO-1-N8R8 (8MB Flash + 8MB PSRAM) | 主控 |
| Camera | GC0308 0.3MP (VGA max) | 场景观察 (3Hz) |
| Audio Codec | ES8311 (I2C + I2S) | 录音 + 播放 |
| Speaker Amp | NS4150B Class-D, 1W 8ohm | 小鸟说话 |
| IMU | 内置陀螺仪 | 佩戴/放置检测 |
| Servo x2 | PWM (G1=Yaw, G2=Pitch) | 转头/点头 |
| Form Factor | M5Stack AtomS3R-M12 + Atomic Echo Base | 内置于毛绒小鸟 |

## Quick Start

### Build & Flash

```bash
cd Mingle_bot
pio run                # Build
pio run -t upload      # Flash (close serial monitor first!)
pio device monitor     # Monitor (115200 baud)
```

> After modifying `sdkconfig.defaults`: `rm sdkconfig.esp32s3-cam && rm -rf .pio && pio run`

## Usage Flow

### First Boot (No WiFi)

1. Flash firmware via USB-C, wait 3 seconds for boot
2. Device enters AP mode, speaker announces WiFi name and password
3. Phone connects to `Mingle_XXXX` WiFi, Captive Portal auto-opens
4. Select WiFi, enter password, click Connect
5. Device connects and saves WiFi to NVS

### Device Binding

1. After WiFi connected, device contacts server
2. Speaker announces 6-digit bind code
3. Open app, login, enter bind code
4. Device bound! Speaker: *"设备绑定成功"*

### Normal Operation -- Shoulder Mode

1. 将小鸟放在肩膀上
2. 陀螺仪检测到佩戴 -> 自动激活
3. 摄像头观察周围 -> 发现可以搭话的人 -> 主动打招呼
4. 麦克风听对话 -> AI 分析氛围 -> 适时插话活跃气氛
5. 说 "mingle + 指令" -> 控制小鸟行为

### Button (GPIO 41)

| Action | Effect |
|--------|--------|
| Short press | Restart device |
| Long press (3s) | Factory reset: clear WiFi + token, restart |

## Architecture

```
+------------------------------------------------------+
|              Mingle Bot (ESP32-S3)                    |
|                                                       |
|  Camera      WiFi       Audio                        |
|  GC0308      AP/STA     ES8311 I2S 16kHz             |
|  3Hz JPEG    Portal     Speaker + Mic                |
|       |           |            |                      |
|  ws_cam_task  HTTP Srv  mic_cap_task                 |
|  (JPEG->WS)  Port 80   (I2S->RingBuf->WS)           |
|       |                        |                      |
|       +------------------------+                      |
|       |    WebSocket Client                           |
|       |    wss://server/ws/device                     |
|       |                                               |
|       |  IMU -> shoulder_detect -> activate/sleep     |
|       |  tts_simple.h: PCM Speaker (小鸟说话)          |
|       |  ota_cam.h: HTTPS OTA Update                  |
|       +-----------------------------------------------+
|                        ^v                              |
|              Server (cloud)                           |
|   ASR + 人脸识别 + 社交决策 + TTS                      |
+-------------------------------------------------------+
```

## WebSocket Protocol

| Direction | Type | Content |
|-----------|------|---------|
| ESP->Server | Text JSON | `auth`, `camera_info`, `camera_status`, `imu_state` |
| ESP->Server | Binary | JPEG frames (3Hz shoulder mode) |
| ESP->Server | Binary | PCM audio (realtime mic, 16kHz mono) |
| Server->ESP | Text JSON | `auth_ok`, `stream_mode`, `set_config`, `ota_update`, `speak`, `servo_cmd`, `timer_event` |
| Server->ESP | Binary | TTS PCM audio |

## TTS Voice System

| Item | Value |
|------|-------|
| Engine | [edge-tts](https://github.com/rany2/edge-tts) (free) |
| Voice | `zh-CN-YunxiaNeural` (cute boy voice) |
| Format | 16-bit PCM, 16000Hz, Mono |
| Clips | 60 total: digits + letters + prompts |

### Regenerate Voice Clips

```bash
pip install edge-tts pydub imageio-ffmpeg
python scripts/generate_voice.py
```

## Project Structure

```
Mingle_bot/
+-- main/
|   +-- main.cpp              # Entry: boot pipeline, HTTP server, main loop
|   +-- config.h              # Pin definitions, server URL, constants
|   +-- audio.h               # ES8311 codec, I2S, mic capture
|   +-- tts_simple.h          # Voice prompts: PCM playback
|   +-- wifi_prov.h           # WiFi: multi-NVS, AP, Captive Portal
|   +-- device_reg.h          # Device registration
|   +-- ws_cam.h              # WebSocket: auth, streams, commands
|   +-- ota_cam.h             # OTA firmware update
|   +-- runtime_config_cam.h  # Remote config
|   +-- adpcm.h               # ADPCM codec
|   +-- voice_*.cpp           # [Generated] PCM data (~1.9MB)
+-- scripts/                  # Tools (voice gen, monitor)
+-- docs/                     # Hardware docs
+-- platformio.ini
+-- sdkconfig.defaults
+-- partitions.csv             # 8MB flash: dual OTA + SPIFFS
+-- CLAUDE.md                  # Development notes
+-- README.md                  # This file
```

## Key References

| Resource | Link |
|----------|------|
| M5Atomic-EchoBase (audio reference) | [github.com/m5stack/M5Atomic-EchoBase](https://github.com/m5stack/M5Atomic-EchoBase) |
| AtomS3R-M12 Product Page | [docs.m5stack.com](https://docs.m5stack.com/en/core/AtomS3R-M12) |
| xiaozhi-esp32 (voice reference) | [github.com/78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) |
| edge-tts (voice generation) | [github.com/rany2/edge-tts](https://github.com/rany2/edge-tts) |

## License

MIT
