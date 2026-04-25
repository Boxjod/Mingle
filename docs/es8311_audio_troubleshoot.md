# ES8311 Audio Codec - Troubleshooting & Register Reference

> Atomic Echo Base on M5Stack AtomS3R-M12
> Focus: Microphone capture + Speaker playback issues

## 1. Audio Signal Chain

```
┌──────────────────────────────────────────────────────────────┐
│                    Atomic Echo Base                           │
│                                                              │
│  MEMS Mic ─┐                                                 │
│  (analog)  │     ┌─────────────────────┐                    │
│            └────>│ ES8311              │                    │
│                  │ I2C addr: 0x18      │                    │
│   I2C ──────────>│ (config/control)    │                    │
│  (GPIO 38/39)    │                     │                    │
│                  │ ADC (mic in)        │──I2S DIN (GPIO 7)──> ESP32
│                  │                     │                    │
│   ESP32 ──I2S───>│ DAC (speaker out)   │──line out──┐       │
│   DOUT(5)        └─────────────────────┘            │       │
│   BCK(8)                                             │       │
│   WS(6)          ┌─────────────────────┐            │       │
│                  │ NS4150B             │<───────────┘       │
│                  │ Class-D Amp         │                    │
│   PI4IOE ───────>│ MUTE pin           │──> Speaker 1W 8ohm │
│  (0x43)          └─────────────────────┘                    │
│  Pin0=unmute                                                │
└──────────────────────────────────────────────────────────────┘
```

## 2. Troubleshooting Checklist

### Step 1: Verify I2C Communication

```
Expected log:
  I2C bus init OK (SDA=38 SCL=39)
  ES8311 chip ID: 0x83   <-- 正常值

If "ES8311 not found on I2C 0x18":
  - 检查 Atomic Echo Base 是否正确插入 (金手指对齐)
  - 用 I2C scanner 扫描总线, 确认 0x18 和 0x43 两个地址存在
  - 检查 GPIO 38/39 没有被其他外设占用
  - 确认 I2C_NUM_0 没有被 camera SCCB 抢占
```

### Step 2: Verify I2S Data Path

```
Expected log:
  I2S init OK (BCK=8 WS=6 DOUT=5 DIN=7, 16000Hz)

If I2S init fails:
  - ESP32-S3 只有 2 个 I2S 外设 (I2S_NUM_0, I2S_NUM_1)
  - 确认 I2S_NUM_0 没有被其他代码占用
  - 检查 GPIO 引脚是否与 camera DVP 冲突 (不应冲突, 但需确认)
```

### Step 3: Check Amp Unmute

```
Expected log:
  Amp unmuted via PI4IOE

If PI4IOE device add failed:
  - PI4IOE5V6408 地址 0x43 不响应
  - 即使 PI4IOE 失败, ES8311 line-out 仍有信号, 但无法驱动扬声器
  - 扬声器无声的常见原因就是 amp 没有 unmute!
```

### Step 4: Test Tone

```
Expected log:
  Playing 1kHz test tone...
  Test tone done

If 无声:
  - 先排除 amp unmute (Step 3)
  - 检查 ES8311 DAC volume 寄存器 (0x32), 0x00=最大, 0xFF=静音
  - 检查 I2S DOUT (GPIO 5) 确实连接到 ES8311 SDIN
  - 用示波器/逻辑分析仪检查 BCK/WS/DOUT 信号
```

### Step 5: Microphone

```
If mic 数据全0或很小:
  - 检查 ADC volume (寄存器 0x17), 0xC0 = 0dB (当前设置)
  - 检查 PGA gain (寄存器 0x16), 当前 0x14 = MIC1 + 6dB
  - 尝试增大: 0x34 = +12dB, 0x54 = +18dB, 0x74 = +24dB
  - 确认 MEMS mic 供电正常 (3.3V)
  - 确认 GPIO reg 0x15 = 0x00 (analog mic, not DMIC)

If mic 数据有但是噪声:
  - I2S 时钟可能不匹配, 检查 BCLK 频率
  - 当前配置: 16kHz * 16bit * 1ch = 256kHz BCLK
  - ES8311 CLK_MGR6=0x03 表示 BCLK divider
```

## 3. ES8311 Register Map (Key Registers)

### Clock Registers (0x01-0x08)

| Reg | Name | Current Value | Description |
|-----|------|---------------|-------------|
| 0x01 | CLK_MGR1 | **0xC0** | bit7: MCLK source (1=from SCLK), bit6: MCLK power on |
| 0x02 | CLK_MGR2 | 0x00 | MCLK/BCLK ratio, ADC/DAC clock sharing |
| 0x03 | CLK_MGR3 | 0x10 | ADC OSR = 256 |
| 0x04 | CLK_MGR4 | 0x10 | DAC OSR = 256 |
| 0x05 | CLK_MGR5 | 0x00 | ADC/DAC clock divider = /1 |
| 0x06 | CLK_MGR6 | **0x03** | BCLK divider (MCLK/BCLK ratio) |
| 0x07 | CLK_MGR7 | 0x00 | LRCK divider high byte |
| 0x08 | CLK_MGR8 | **0x20** | LRCK divider low byte (32 = BCLK/LRCK for 16bit mono) |

**CRITICAL: CLK_MGR1 bit7=1 表示 ES8311 从 BCLK 派生 MCLK, 不需要外部 MCLK 引脚.**
如果设 bit7=0, 则需要 ESP32 提供 MCLK 信号到 ES8311 MCLK pin.

### I2S Format Registers (0x09-0x0A)

| Reg | Name | Current Value | Description |
|-----|------|---------------|-------------|
| 0x09 | SDP_IN | **0x0C** | ADC I2S format: bits[3:2]=11 -> 16-bit |
| 0x0A | SDP_OUT | **0x0C** | DAC I2S format: bits[3:2]=11 -> 16-bit |

Format bits:
- `0x00` = 32-bit
- `0x04` = 24-bit
- `0x08` = 20-bit
- `0x0C` = 16-bit

### System Power Registers (0x0B-0x0C)

| Reg | Name | Current Value | Description |
|-----|------|---------------|-------------|
| 0x0B | SYSTEM | **0x00** | bit7: power down, 0x00 = powered up |
| 0x0C | SYSTEM2 | **0x00** | bit5: PDN_DAC, bit4: PDN_ADC, 0x00 = both on |

### ADC (Microphone) Registers (0x0D-0x17)

| Reg | Name | Current Value | Description |
|-----|------|---------------|-------------|
| 0x0D | ADC1 | **0x00** | bit6: ADC power down (0=on), bit4: PGA polarity |
| 0x0E | ADC2 | 0x00 | ADC digital volume/ramp |
| 0x10 | ADC4 | 0x00 | ALC (Auto Level Control) enable |
| 0x11 | ADC5 | 0x00 | ALC settings |
| 0x15 | GPIO | **0x00** | bit0: DMIC select (0=analog mic, 1=DMIC) |
| 0x16 | GP | **0x14** | bits[6:4]: PGA gain, bits[3:0]: MIC select |
| 0x17 | ADC_VOL | **0xC0** | ADC digital volume: 0xC0=0dB, 0x00=-95.5dB |

**PGA Gain (Reg 0x16, bits[6:4]):**

| Value | bits[6:4] | Gain |
|-------|-----------|------|
| 0x04 | 000 | 0dB |
| 0x14 | 001 | +6dB (current) |
| 0x24 | 010 | +12dB |
| 0x34 | 011 | +18dB |
| 0x44 | 100 | +24dB |
| 0x54 | 101 | +30dB |
| 0x64 | 110 | +36dB |
| 0x74 | 111 | +42dB |

**MIC Select (Reg 0x16, bits[3:0]):**
- 0x04 = MIC1P (single-ended, typical for MEMS mic)
- 0x14 = MIC1P with +6dB (current setting)

**ADC Volume (Reg 0x17):**
- 0x00 = -95.5 dB (近静音)
- 0xBF = -0.5 dB
- 0xC0 = 0 dB (current, default)
- 0xFF = +32 dB (最大)

### DAC (Speaker) Registers (0x12-0x32)

| Reg | Name | Current Value | Description |
|-----|------|---------------|-------------|
| 0x12 | DAC1 | **0x00** | bit7: DAC power down (0=on) |
| 0x13 | DAC2 | 0x00 | DAC DEM (Dynamic Element Matching) |
| 0x32 | DAC_VOL | **0x00** | DAC volume: 0x00=0dB, 0xFF=-95.5dB |

**DAC Volume (Reg 0x32):**
- 0x00 = 0 dB (maximum, current)
- 0x18 = -12 dB
- 0x30 = -24 dB
- 0xFF = -95.5 dB (mute)

### Chip ID Register

| Reg | Name | Expected | Description |
|-----|------|----------|-------------|
| 0xFD | CHIP_ID1 | **0x83** | Chip identification |
| 0xFE | CHIP_ID2 | 0x11 | |
| 0xFF | CHIP_VER | varies | Chip version |

## 4. Known Issues & Solutions

### Issue 1: I2C Bus Conflict (Camera SCCB vs ES8311)

**Symptom**: `sccb-ng: failed to install SCCB I2C master bus: ESP_ERR_INVALID_STATE`

**Root Cause**: Camera SCCB driver tries to create an I2C bus on a port already used by ES8311.

**Solution** (current code already handles this):
```c
// camera uses GPIO 12/9 (separate physical pins)
// ES8311 uses GPIO 38/39 (separate physical pins)
// They can use different I2C ports without conflict

// In camera_config_t:
config.pin_sscb_sda = CAM_PIN_SIOD;  // 12
config.pin_sscb_scl = CAM_PIN_SIOC;  // 9
config.sccb_i2c_port = 0;            // camera manages its own bus
```

**Alternative** (if sharing pins): Set camera SCCB pins to -1:
```c
config.pin_sscb_sda = -1;
config.pin_sscb_scl = -1;
config.sccb_i2c_port = I2C_NUM_1;  // pre-initialized bus
```

### Issue 2: Old vs New I2C Driver Conflict

**Symptom**: `CONFLICT! driver_ng is not allowed to be used with this old driver`

**Root Cause**: Mixing `i2c_master.h` (new) with `driver/i2c.h` (legacy).

**Solution**: Use ONLY `i2c_master.h` throughout. Current audio.h already does this correctly.
esp32-camera internally may use legacy driver - check esp32-camera version.

### Issue 3: No Sound from Speaker

**Checklist (in order)**:
1. PI4IOE unmute: Reg 0x03=0x00, Reg 0x05=0x01 (pin0 HIGH)
2. ES8311 DAC powered on: Reg 0x12 bit7=0
3. DAC volume not muted: Reg 0x32 != 0xFF
4. SYSTEM power on: Reg 0x0B = 0x00
5. I2S DOUT (GPIO 5) signal present
6. I2S clock config matches ES8311 expectations

### Issue 4: Microphone Data All Zeros

**Checklist (in order)**:
1. ES8311 ADC powered on: Reg 0x0D bit6=0
2. Analog mic selected (not DMIC): Reg 0x15 bit0=0
3. MIC input routed: Reg 0x16 bits[3:0] = 0x4 (MIC1P)
4. PGA gain sufficient: Reg 0x16 bits[6:4], try +24dB (0x44)
5. ADC volume not too low: Reg 0x17 >= 0xC0
6. I2S DIN (GPIO 7) connected to ES8311 SDOUT
7. I2S RX channel enabled: `i2s_channel_enable(g_i2s_rx)`
8. Ring buffer receiving data: check `bytes_read > 0` in mic task

### Issue 5: MCLK Configuration

**Symptom**: Garbled audio, wrong sample rate, no audio data

**Current config**: ES8311 derives MCLK from BCLK internally (CLK_MGR1=0xC0, bit7=1).
This means **no external MCLK pin is needed**.

**BCLK calculation**:
```
BCLK = Sample_Rate × Bits × Channels = 16000 × 16 × 1 = 256000 Hz
MCLK (internal) = BCLK × multiplier
```

ESP-IDF I2S driver config:
```c
std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
// This sets internal MCLK = 16000 * 256 = 4.096 MHz
// But since we don't output MCLK pin, ES8311 uses BCLK instead
```

**If audio sounds wrong**: Try changing CLK_MGR1 to 0x3F (MCLK from external pin), and provide MCLK from ESP32 I2S:
```c
std_cfg.gpio_cfg.mclk = (gpio_num_t)SOME_GPIO;  // Need to find available pin
```

### Issue 6: Camera Init Order Matters

**Best practice**: Init camera BEFORE audio I2C, because esp32-camera may reset I2C buses.

```c
// Correct order:
camera_init();    // 1st - uses SCCB on GPIO 12/9
wifi_init_sta();  // 2nd
audio_init();     // 3rd - uses I2C on GPIO 38/39
```

Current `main.cpp` already follows this order.

## 5. Debug Tools

### I2C Bus Scan

```c
// Add this before audio_codec_init() to verify bus:
for (uint8_t addr = 1; addr < 127; addr++) {
    uint8_t dummy;
    esp_err_t err = i2c_master_transmit_receive(
        /* need to create temp device for each addr */
    );
}
// Or use: i2c_master_probe(g_i2c_bus, addr, pdMS_TO_TICKS(100));
```

### Register Dump

```c
void es8311_dump_regs() {
    uint8_t val;
    const uint8_t regs[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                            0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,
                            0x12,0x13,0x15,0x16,0x17,0x32,0xFD,0xFE,0xFF};
    for (int i = 0; i < sizeof(regs); i++) {
        if (es8311_read_reg(regs[i], &val) == ESP_OK) {
            ESP_LOGI("ES8311", "Reg 0x%02X = 0x%02X", regs[i], val);
        }
    }
}
```

### I2S Signal Check

```c
// Quick test: read 1 second of mic data and check for non-zero
void test_mic_raw() {
    int16_t buf[512];
    size_t bytes_read = 0;
    int non_zero = 0;

    i2s_channel_read(g_i2s_rx, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(1000));

    int samples = bytes_read / sizeof(int16_t);
    for (int i = 0; i < samples; i++) {
        if (buf[i] != 0) non_zero++;
    }

    ESP_LOGI("MIC_TEST", "Read %d samples, %d non-zero (%.1f%%)",
             samples, non_zero, 100.0f * non_zero / samples);

    // Also print first 20 values to check for patterns
    for (int i = 0; i < 20 && i < samples; i++) {
        ESP_LOGI("MIC_TEST", "  [%d] = %d", i, buf[i]);
    }
}
```

## 6. ESP-IDF ES8311 Reference Example

The official ESP-IDF includes a complete working example at:
`examples/peripherals/i2s/i2s_codec/i2s_es8311`

Key differences from our manual register config:
- Uses `espressif/es8311` component (IDF component manager)
- Handles clock config automatically based on sample rate
- Provides both "music" and "echo" modes

To use the component instead of manual registers:
```yaml
# idf_component.yml
dependencies:
  espressif/es8311: "^1.0.0"
```

## 7. Quick Reference: Common Register Changes

```c
// Increase mic gain to +24dB (if mic too quiet):
es8311_write_reg(0x16, 0x44);

// Increase ADC digital volume to +6dB:
es8311_write_reg(0x17, 0xCC);

// Mute speaker:
es8311_write_reg(0x32, 0xFF);

// Unmute speaker at 0dB:
es8311_write_reg(0x32, 0x00);

// Switch to DMIC input:
es8311_write_reg(0x15, 0x03);  // DMIC on CLK/DAT

// Power down codec (sleep):
es8311_write_reg(0x0B, 0x80);  // analog off
es8311_write_reg(0x0C, 0x30);  // ADC+DAC off

// Wake up codec:
es8311_write_reg(0x0B, 0x00);
es8311_write_reg(0x0C, 0x00);

// Software reset:
es8311_write_reg(0x00, 0x1F);
vTaskDelay(pdMS_TO_TICKS(20));
es8311_write_reg(0x00, 0x00);
```
