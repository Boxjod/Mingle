# AtomS3R-M12 Volcengine Kit - Hardware Reference

> Quick lookup for pinouts, chips, datasheets, and wiring.
> Board: M5Stack AtomS3R-M12 + Atomic Echo Base

## 1. System Block Diagram

```
┌─────────────────────────────────────────────────────┐
│              AtomS3R-M12 Controller                  │
│                                                      │
│  ┌───────────────────────────────────────────────┐  │
│  │         ESP32-S3-PICO-1-N8R8                  │  │
│  │         Dual-core LX7 @ 240MHz               │  │
│  │         8MB Flash + 8MB PSRAM                 │  │
│  │         WiFi 802.11 b/g/n + BLE 5.0          │  │
│  │         3D Enhanced Antenna                   │  │
│  └──┬────┬────┬────┬────┬────┬────┬────┬────┬───┘  │
│     │    │    │    │    │    │    │    │    │        │
│  ┌──┴──┐ │ ┌──┴──┐ │ ┌──┴──┐ │  ┌─┴─┐  ┌─┴──┐   │
│  │OV3660│ │ │BMI270│ │ │0.85"│ │  │IR │  │RGB │   │
│  │3MP   │ │ │6-axis│ │ │IPS  │ │  │LED│  │LED │   │
│  │M12   │ │ │IMU   │ │ │LCD  │ │  │   │  │    │   │
│  └──────┘ │ └──┬───┘ │ └─────┘ │  └───┘  └────┘   │
│           │ ┌──┴───┐  │        │                    │
│           │ │BMM150│  │        │                    │
│           │ │3-axis│  │        │                    │
│           │ │MAG   │  │        │                    │
│           │ └──────┘  │        │                    │
│  USB-C    HY2.0-4P   BTN(41)  GPIO Bottom Pins     │
└────┬──────────┬────────────────┬────────────────────┘
     │          │                │
     │   ┌──────┴────────────────┴───────┐
     │   │     Atomic Echo Base           │
     │   │  ┌─────────┐  ┌───────────┐  │
     │   │  │ ES8311   │  │ NS4150B   │  │
     │   │  │ 24-bit   │  │ Class-D   │  │
     │   │  │ Codec    │  │ Amp       │  │
     │   │  └────┬─────┘  └─────┬─────┘  │
     │   │  ┌────┴─────┐  ┌────┴──────┐  │
     │   │  │ MEMS Mic │  │ 1W 8ohm   │  │
     │   │  │ (digital)│  │ Speaker   │  │
     │   │  └──────────┘  └───────────┘  │
     │   │                                │
     │   │  PI4IOE5V6408 I/O Expander    │
     │   │  (controls NS4150B mute pin)  │
     │   └────────────────────────────────┘
     │
   PC / 5V Power
```

## 2. Complete GPIO Pin Map

### 2.1 OV3660 Camera (DVP 8-bit parallel)

| Signal | GPIO | Notes |
|--------|------|-------|
| XCLK | **21** | External clock 20MHz |
| SIOD (SDA) | **12** | SCCB/I2C data (camera config) |
| SIOC (SCL) | **9** | SCCB/I2C clock |
| D0 (Y2) | **3** | Data bit 0 |
| D1 (Y3) | **42** | Data bit 1 |
| D2 (Y4) | **46** | Data bit 2 |
| D3 (Y5) | **48** | Data bit 3 |
| D4 (Y6) | **4** | Data bit 4 |
| D5 (Y7) | **17** | Data bit 5 |
| D6 (Y8) | **11** | Data bit 6 |
| D7 (Y9) | **13** | Data bit 7 |
| VSYNC | **10** | Vertical sync |
| HREF | **14** | Horizontal reference |
| PCLK | **40** | Pixel clock |
| POWER | **18** | Active LOW - drive LOW to enable |
| PWDN | -1 | Not connected |
| RESET | -1 | Not connected |

**Camera I2C bus**: Uses SCCB protocol on GPIO 12/9. esp32-camera driver manages this internally via `sccb_i2c_port`.

### 2.2 ES8311 Audio Codec (Atomic Echo Base)

#### I2C Control Bus

| Signal | GPIO | Notes |
|--------|------|-------|
| SDA | **38** | ES8311 + PI4IOE 共用 |
| SCL | **39** | ES8311 + PI4IOE 共用 |

- I2C Address: **ES8311 = 0x18**, **PI4IOE5V6408 = 0x43**
- I2C Speed: 100kHz
- I2C Port: **I2C_NUM_0** (camera SCCB uses separate port)

#### I2S Audio Data Bus

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| BCK | **8** | ESP32 -> Codec | Bit clock |
| WS (LRCK) | **6** | ESP32 -> Codec | Word select / Left-Right clock |
| DOUT | **5** | ESP32 -> Codec | DAC path (speaker output) |
| DIN | **7** | Codec -> ESP32 | ADC path (microphone input) |
| MCLK | **unused** | - | ES8311 derives MCLK from BCLK (CLK_MGR1=0xC0) |

- I2S Port: **I2S_NUM_0**
- Mode: I2S Philips Standard, 16-bit, Mono
- Sample Rate: 16000 Hz

### 2.3 Other Peripherals

| Peripheral | GPIO | Notes |
|------------|------|-------|
| Button | **41** | 屏幕下方, active LOW |
| RGB LED (WS2812) | **35** | NeoPixel, 内置 |
| IR LED | **44** | 红外发射, 180 degree, 12.46m range |
| Status LED | **2** | (if present) |
| 0.85" IPS Display | SPI (auto-config) | 通过 M5GFX/M5Unified 管理 |
| HY2.0-4P Port | **G1/G2** | Grove compatible expansion |

### 2.4 IMU Sensors (I2C)

| Sensor | Bus | Address | Notes |
|--------|-----|---------|-------|
| BMI270 | Main I2C | 0x68 or 0x69 | 6-axis (accel + gyro) |
| BMM150 | BMI270 aux I2C | 0x10 | 3-axis magnetometer, via BMI270 sensor hub |

## 3. I2C Bus Architecture

```
                ESP32-S3
                   │
    ┌──────────────┼──────────────────┐
    │              │                  │
 I2C_NUM_0      I2C_NUM_1         SCCB (internal)
 GPIO 38/39     (reserved)        GPIO 12/9
    │                                │
    ├── ES8311 (0x18)                └── OV3660 camera
    ├── PI4IOE5V6408 (0x43)
    └── BMI270 (0x68) / BMM150 (via aux)
```

**IMPORTANT**: Camera SCCB 和 Audio I2C 使用不同的物理引脚和不同的 I2C port, 不会冲突.
在 `camera_config_t` 中设置 `sccb_i2c_port = 0` 但使用独立的 GPIO 12/9.
Audio ES8311 使用 GPIO 38/39 on I2C_NUM_0.

## 4. Chip Datasheets

| Chip | Manufacturer | Datasheet Link | Key Specs |
|------|-------------|----------------|-----------|
| ESP32-S3-PICO-1-N8R8 | Espressif | [espressif.com/esp32-s3](https://www.espressif.com/en/products/socs/esp32-s3) | Dual LX7@240MHz, WiFi+BLE5, 8MB Flash+8MB PSRAM |
| OV3660 | OmniVision | [ovt.com/ov3660](https://www.ovt.com/products/ov3660/) | 3MP, 2048x1536, 1/5" CMOS, DVP 8-bit |
| ES8311 | Everest Semi | [ES8311 datasheet PDF](http://www.everest-semi.com/pdf/ES8311%20PB.pdf) | 24-bit mono codec, I2S, low power |
| BMI270 | Bosch | [bosch BMI270](https://www.bosch-sensortec.com/products/motion-sensors/imus/bmi270/) | 6-axis IMU, ultra-low power |
| BMM150 | Bosch | [bosch BMM150](https://www.bosch-sensortec.com/products/motion-sensors/magnetometers/bmm150/) | 3-axis magnetometer |
| NS4150B | Nsiway | Search "NS4150B datasheet" | Class-D mono amp, 3W max |
| PI4IOE5V6408 | Diodes Inc | Search "PI4IOE5V6408 datasheet" | 8-bit I2C GPIO expander |

## 5. OV3660 Camera Specs

| Parameter | Value |
|-----------|-------|
| Resolution | 3MP (2048 x 1536) |
| Pixel Size | 1.4um x 1.4um |
| Formats | 8/10-bit RAW, RGB, YCbCr, JPEG |
| Max FPS | Full res@15fps, 1080p@20fps, VGA@60fps |
| Lens | M12 mount, 120 degree wide angle (可换) |
| Interface | DVP 8-bit parallel + SCCB (I2C-like) |
| XCLK | 6~27MHz (推荐 20MHz) |
| Supply | 2.5V (core) + 1.8V/2.8V (I/O, analog) |
| Power enable | GPIO 18, **Active LOW** |

## 6. ES8311 Audio Codec Specs

| Parameter | Value |
|-----------|-------|
| Type | Mono ADC + mono DAC |
| Resolution | 24-bit |
| Sample Rate | 8kHz ~ 96kHz |
| SNR (DAC) | 106dB |
| SNR (ADC) | 95dB |
| Interface (control) | I2C, addr 0x18 |
| Interface (audio) | I2S (Standard/Left-J/Right-J/DSP) |
| Clock Source | MCLK (external) or BCLK (derived) |
| Mic Input | Analog single-ended (MIC1P) or DMIC |
| Output | Line out -> NS4150B amp -> speaker |
| Supply | 1.8V ~ 3.3V |

## 7. Atomic Echo Base Audio Chain

```
                    I2C (GPIO 38/39)
                         │
  MEMS Mic ──analog──> ES8311 ADC ──I2S DIN (GPIO 7)──> ESP32
                         │
  ESP32 ──I2S DOUT (GPIO 5)──> ES8311 DAC ──line out──> NS4150B ──> Speaker
                                                            │
                                              PI4IOE5V6408 (0x43)
                                              Pin0 = unmute control
```

### NS4150B Amp Mute Control (via PI4IOE5V6408)

```c
// PI4IOE5V6408 registers:
// 0x03 = Direction (0x00 = all output)
// 0x05 = Output value (bit0 = 1 -> unmute NS4150B)
pi4ioe_write_reg(0x03, 0x00);  // All pins output
pi4ioe_write_reg(0x05, 0x01);  // Pin 0 HIGH = unmute
```

## 8. Physical Dimensions

| Component | Size |
|-----------|------|
| AtomS3R-M12 controller | 24.0 x 24.0 x 22.1 mm |
| Atomic Echo Base | ~24 x 24 x ~10 mm (stacks below) |
| Combined stack | ~24 x 24 x ~32 mm |

## 9. Power

- **Input**: USB-C 5V
- **Internal regulator**: 5V -> 3.3V (onboard)
- **Camera power**: GPIO 18 controls enable (active LOW)
- **Typical current**: ~300mA (WiFi + Camera + Audio active)

## 10. Development Resources

### Official Repos
| Repo | URL | Content |
|------|-----|---------|
| M5AtomS3 Driver | [github.com/m5stack/M5AtomS3](https://github.com/m5stack/M5AtomS3) | Arduino driver + camera example |
| M5 Camera Examples | [github.com/m5stack/M5_Camera_Examples](https://github.com/m5stack/M5_Camera_Examples) | OV3660 usage examples |
| M5Unified | [github.com/m5stack/M5Unified](https://github.com/m5stack/M5Unified) | Unified hardware abstraction |
| M5GFX | [github.com/m5stack/M5GFX](https://github.com/m5stack/M5GFX) | Display driver |
| M5-Schematic | [github.com/m5stack/M5-Schematic](https://github.com/m5stack/M5-Schematic) | Board schematics (PDF) |
| M5_Hardware | [github.com/m5stack/M5_Hardware](https://github.com/m5stack/M5_Hardware) | PCB design files |

### Official Docs
| Page | URL |
|------|-----|
| AtomS3R-M12 Product | [docs.m5stack.com AtomS3R-M12](https://docs.m5stack.com/en/core/AtomS3R-M12) |
| Volcengine Kit | [docs.m5stack.com Volcengine Kit](https://docs.m5stack.com/en/core/AtomS3R-M12%20Volcengine%20Kit) |
| Arduino Camera Guide | [docs.m5stack.com Arduino](https://docs.m5stack.com/en/arduino/m5atoms3r-m12/program) |
| Atomic Echo Base | [docs.m5stack.com Echo Base](https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base) |
| Echo Base Arduino Tutorial | [docs.m5stack.com Tutorial](https://docs.m5stack.com/en/arduino/projects/atomic/atomic_echo_base) |
| Home Assistant Camera | [docs.m5stack.com HA](https://docs.m5stack.com/en/homeassistant/camera/atoms3r_m12) |
| Volcengine Voice Guide | [docs.m5stack.com 火山引擎](https://docs.m5stack.com/zh_CN/guide/realtime/volcengine/atomic_echo_base) |

### ESP-IDF Reference
| Topic | URL |
|-------|-----|
| I2S Driver (ESP-IDF v5) | [docs.espressif.com I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html) |
| I2C Master Driver | [docs.espressif.com I2C](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2c.html) |
| ESP-IDF ES8311 Example | `examples/peripherals/i2s/i2s_codec/i2s_es8311` in ESP-IDF |
| esp32-camera Component | [github.com/espressif/esp32-camera](https://github.com/espressif/esp32-camera) |

### Community / Troubleshooting
| Topic | URL |
|-------|-----|
| xiaozhi-esp32 ES8311+OV2640 I2C 冲突 | [github issue #1119](https://github.com/78/xiaozhi-esp32/issues/1119) |
| xiaozhi-esp32 OV3660 crash | [github issue #1588](https://github.com/78/xiaozhi-esp32/issues/1588) |
| ESP32-CAM I2C conflict | [esp32.com forum](https://esp32.com/viewtopic.php?t=27158) |
| esp32-camera I2C sharing | [github issue #434](https://github.com/espressif/esp32-camera/issues/434) |
| AtomS3R camera example | [M5Stack Community](https://community.m5stack.com/topic/7325/atoms3r-camera-esp_camera-h-example) |
