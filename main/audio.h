/**
 * audio.h - ES8311 + I2S audio for M5AtomS3R + Atomic Echo Base
 *
 * Register sequence EXACTLY matches M5Atomic-EchoBase library (es8311.cpp).
 * Uses new I2C master driver to avoid conflict with esp32-camera.
 */
#pragma once

#include <cstring>
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "config.h"

static const char* TAG_AUDIO = "audio";

// Handles
// I2C bus handle shared with IMU (BMI270 on same bus GPIO 38/39)
i2c_master_bus_handle_t g_i2c_bus = NULL;
static i2c_master_dev_handle_t g_es8311_dev = NULL;
static i2c_master_dev_handle_t g_pi4ioe_dev = NULL;
// I2S handles (non-static: tts_simple.h needs g_i2s_tx)
i2s_chan_handle_t g_i2s_tx = NULL;
i2s_chan_handle_t g_i2s_rx = NULL;
static RingbufHandle_t g_mic_ringbuf = NULL;
#define MIC_RINGBUF_SIZE (32 * 1024)

// Mic diagnostic counter (visible to status endpoint)
static volatile uint32_t g_mic_total_samples = 0;
static volatile int32_t g_mic_peak = 0;

// ============ I2C helpers ============

static esp_err_t es_wr(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(g_es8311_dev, buf, 2, pdMS_TO_TICKS(100));
}

static uint8_t es_rd(uint8_t reg) {
    uint8_t val = 0;
    i2c_master_transmit_receive(g_es8311_dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100));
    return val;
}

static esp_err_t pi_wr(uint8_t reg, uint8_t val) {
    if (!g_pi4ioe_dev) return ESP_FAIL;
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(g_pi4ioe_dev, buf, 2, pdMS_TO_TICKS(100));
}

static uint8_t pi_rd(uint8_t reg) {
    if (!g_pi4ioe_dev) return 0xFF;
    uint8_t val = 0;
    i2c_master_transmit_receive(g_pi4ioe_dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100));
    return val;
}

// ============ I2C bus init ============

static esp_err_t audio_i2c_init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = AUDIO_I2C_PORT;
    bus_cfg.sda_io_num = (gpio_num_t)AUDIO_I2C_SDA;
    bus_cfg.scl_io_num = (gpio_num_t)AUDIO_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_i2c_bus);
    if (err != ESP_OK) { ESP_LOGE(TAG_AUDIO, "I2C bus fail: 0x%x", err); return err; }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = 100000;

    dev_cfg.device_address = ES8311_ADDR;
    i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_es8311_dev);

    dev_cfg.device_address = PI4IOE_ADDR;
    err = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_pi4ioe_dev);
    if (err != ESP_OK) { g_pi4ioe_dev = NULL; ESP_LOGW(TAG_AUDIO, "PI4IOE not found"); }

    ESP_LOGI(TAG_AUDIO, "I2C OK (SDA=%d SCL=%d)", AUDIO_I2C_SDA, AUDIO_I2C_SCL);
    return ESP_OK;
}

// ============ PI4IOE init — exact M5EchoBase sequence ============

static void pi4ioe_init() {
    if (!g_pi4ioe_dev) { ESP_LOGW(TAG_AUDIO, "PI4IOE skip"); return; }
    pi_rd(0x00);              // Read CTRL register
    pi_wr(0x07, 0x00);       // Push-pull output
    pi_rd(0x07);              // Read back
    pi_wr(0x0D, 0xFF);       // Pull-up enable all
    pi_wr(0x03, 0x6F);       // IO direction (0x6F)
    pi_rd(0x03);              // Read back
    pi_wr(0x05, 0xFF);       // Output all HIGH = unmute
    pi_rd(0x05);              // Read back
    ESP_LOGI(TAG_AUDIO, "PI4IOE init OK (unmuted)");
}

// ============ I2S init — match M5EchoBase: 16-bit stereo ============

static esp_err_t audio_i2s_init() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &g_i2s_tx, &g_i2s_rx);
    if (err != ESP_OK) { ESP_LOGE(TAG_AUDIO, "I2S chan fail"); return err; }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    // 16-bit data, stereo — exactly like M5EchoBase Arduino library
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)AUDIO_I2S_BCK;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)AUDIO_I2S_WS;
    std_cfg.gpio_cfg.dout = (gpio_num_t)AUDIO_I2S_DOUT;
    std_cfg.gpio_cfg.din  = (gpio_num_t)AUDIO_I2S_DIN;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    i2s_channel_init_std_mode(g_i2s_tx, &std_cfg);
    i2s_channel_init_std_mode(g_i2s_rx, &std_cfg);
    i2s_channel_enable(g_i2s_tx);
    i2s_channel_enable(g_i2s_rx);

    // CRITICAL: Wait for I2S clocks to stabilize before ES8311 PLL lock
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG_AUDIO, "I2S OK (16bit stereo, BCLK=512kHz)");
    return ESP_OK;
}

// ============ ES8311 init — exact M5EchoBase es8311_init() ============

static esp_err_t audio_codec_init() {
    uint8_t chip_id = es_rd(0xFD);
    ESP_LOGI(TAG_AUDIO, "ES8311 chip ID: 0x%02X", chip_id);
    if (chip_id == 0x00 || chip_id == 0xFF) {
        ESP_LOGE(TAG_AUDIO, "ES8311 not found!"); return ESP_FAIL;
    }

    // ---- Step 1: Reset (M5EchoBase es8311_init) ----
    es_wr(0x00, 0x1F);   // Assert reset
    vTaskDelay(pdMS_TO_TICKS(20));
    es_wr(0x00, 0x00);   // Clear reset
    es_wr(0x00, 0x80);   // Power on
    ESP_LOGI(TAG_AUDIO, "ES8311 reset done");

    // ---- Step 2: Clock config (es8311_clock_config) ----
    // mclk_from_mclk_pin=false → REG01 bit7=1, mclk = 16000*32*2 = 1,024,000
    uint8_t reg01 = 0x3F | (1 << 7);  // 0xBF: all clocks on + MCLK from SCLK
    es_wr(0x01, reg01);
    ESP_LOGI(TAG_AUDIO, "REG01=0x%02X (MCLK from SCLK)", reg01);

    // SCLK not inverted
    uint8_t reg06 = es_rd(0x06);
    reg06 &= ~(1 << 5);  // clear sclk_inverted
    es_wr(0x06, reg06);

    // ---- Step 3: Clock coefficients for {1024000, 16000} ----
    // FROM M5EchoBase coeff_div table (NOT esp-adf!):
    // {1024000, 16000, pre_div=0x01, pre_multi=0x02, adc_div=0x01, dac_div=0x01,
    //  fs_mode=0x00, lrck_h=0x00, lrck_l=0xFF, bclk_div=0x04, adc_osr=0x10, dac_osr=0x10}

    // REG02: (pre_div-1)<<5 | pre_multi<<3 = 0<<5 | 2<<3 = 0x10
    uint8_t reg02 = es_rd(0x02);
    reg02 &= 0x07;
    reg02 |= (0x01 - 1) << 5;  // pre_div=1 → 0
    reg02 |= 0x02 << 3;        // pre_multi=2
    es_wr(0x02, reg02);

    // REG03: (fs_mode<<6) | adc_osr = 0|0x10 = 0x10
    es_wr(0x03, (0x00 << 6) | 0x10);

    // REG04: dac_osr = 0x10
    es_wr(0x04, 0x10);

    // REG05: (adc_div-1)<<4 | (dac_div-1) = 0
    es_wr(0x05, 0x00);

    // REG06: bclk_div=4, since <19: write (4-1)=3
    reg06 = es_rd(0x06);
    reg06 &= 0xE0;
    reg06 |= (0x04 - 1);  // = 3
    es_wr(0x06, reg06);

    // REG07: lrck_h=0x00
    uint8_t reg07 = es_rd(0x07);
    reg07 &= 0xC0;
    reg07 |= 0x00;
    es_wr(0x07, reg07);

    // REG08: lrck_l=0xFF
    es_wr(0x08, 0xFF);

    ESP_LOGI(TAG_AUDIO, "Clock coeffs set (mclk=1024000 rate=16000)");

    // ---- Step 4: Format config (es8311_fmt_config) ----
    // Slave mode: clear bit6 of REG00
    uint8_t reg00 = es_rd(0x00);
    reg00 &= 0xBF;  // clear bit6
    es_wr(0x00, reg00);

    // SDP: I2S format (bits[1:0]=0), 32-bit resolution (bits[4:2]=100 → 0x10)
    // Matches ES8311_RESOLUTION_32
    es_wr(0x09, 0x10);  // SDP_IN (DAC): 32-bit I2S
    es_wr(0x0A, 0x10);  // SDP_OUT (ADC): 32-bit I2S
    ESP_LOGI(TAG_AUDIO, "SDP: 32-bit I2S slave mode");

    // ---- Step 5: Power up registers (es8311_init) ----
    es_wr(0x0D, 0x01);  // Power up analog
    es_wr(0x0E, 0x02);  // Power up
    es_wr(0x12, 0x00);  // DAC enable
    es_wr(0x13, 0x10);  // HP driver
    es_wr(0x1C, 0x6A);  // ADC EQ bypass + DC cancel
    es_wr(0x37, 0x08);  // DAC ramp rate

    // ---- Step 6: Volume 80% ----
    // reg32 数值越大声音越大: 0x00=最小, 0xFF=最大
    // 公式: reg = 0xFF * vol%; 80%→0xCC
    es_wr(0x32, 0xCC);

    // ---- Step 7: Mic config (es8311_microphone_config, digital=false) ----
    es_wr(0x17, 0xFF);  // ADC volume max
    es_wr(0x14, 0x1A);  // Analog PGA, analog mic

    // ---- Step 8: Mic gain 0dB (es8311_microphone_gain_set) ----
    es_wr(0x16, 0x00);  // ES8311_MIC_GAIN_0DB

    ESP_LOGI(TAG_AUDIO, "ES8311 init complete");

    // Wait for ES8311 PLL to lock onto I2S BCLK
    vTaskDelay(pdMS_TO_TICKS(100));

    // ---- Register dump ----
    ESP_LOGI(TAG_AUDIO, "--- Register Dump ---");
    const uint8_t dregs[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                             0x09,0x0A,0x0D,0x0E,0x12,0x13,0x14,0x16,0x17,
                             0x1C,0x32,0x37};
    for (int i = 0; i < (int)sizeof(dregs); i++) {
        ESP_LOGI(TAG_AUDIO, "  R[0x%02X]=0x%02X", dregs[i], es_rd(dregs[i]));
    }

    return ESP_OK;
}

// ============ Boot voice: play "Mingle! Mingle!" ============

extern "C" {
    extern const uint8_t VDATA_PROMPT_BOOT_MINGLE[];
    extern const unsigned int VDATA_PROMPT_BOOT_MINGLE_SIZE;
}

static void audio_play_boot_voice() {
    ESP_LOGI(TAG_AUDIO, "Playing boot voice: Mingle! Mingle!");
    const int16_t* samples = (const int16_t*)VDATA_PROMPT_BOOT_MINGLE;
    int total_samples = VDATA_PROMPT_BOOT_MINGLE_SIZE / 2;
    const int chunk = 128;
    int16_t stereo[128 * 2];
    size_t written = 0;
    for (int i = 0; i < total_samples; i += chunk) {
        int n = (total_samples - i < chunk) ? (total_samples - i) : chunk;
        for (int j = 0; j < n; j++) {
            stereo[j * 2]     = samples[i + j];
            stereo[j * 2 + 1] = samples[i + j];
        }
        i2s_channel_write(g_i2s_tx, stereo, n * 4, &written, pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG_AUDIO, "Boot voice done");
}

// Quick mic diagnostic: read 0.5s, report stats
static void audio_test_mic() {
    ESP_LOGI(TAG_AUDIO, "Mic test: reading 0.5s...");
    static int16_t stereo[128 * 2];
    int32_t sum = 0, peak = 0, total_samples = 0;

    for (int i = 0; i < AUDIO_SAMPLE_RATE / 2 / 128; i++) {
        size_t bytes_read = 0;
        i2s_channel_read(g_i2s_rx, stereo, 128 * 4, &bytes_read, pdMS_TO_TICKS(1000));
        int n = bytes_read / 4;
        for (int j = 0; j < n; j++) {
            int16_t s = stereo[j * 2];  // left channel
            int32_t abs_s = s < 0 ? -s : s;
            sum += abs_s;
            if (abs_s > peak) peak = abs_s;
            total_samples++;
        }
    }

    int32_t avg = total_samples > 0 ? sum / total_samples : 0;
    ESP_LOGW(TAG_AUDIO, "Mic result: %d samples, avg=%d, peak=%d",
             (int)total_samples, (int)avg, (int)peak);

    if (peak < 10) {
        ESP_LOGE(TAG_AUDIO, "Mic: NO SIGNAL (all zeros) - check ADC config");
    } else if (peak < 100) {
        ESP_LOGW(TAG_AUDIO, "Mic: Very weak signal - try increasing PGA gain");
    } else {
        ESP_LOGI(TAG_AUDIO, "Mic: Signal OK");
    }

    // Print first 20 raw samples
    size_t br = 0;
    i2s_channel_read(g_i2s_rx, stereo, 128 * 4, &br, pdMS_TO_TICKS(500));
    ESP_LOGI(TAG_AUDIO, "Raw samples (L ch):");
    for (int i = 0; i < 20 && i < (int)(br/4); i++) {
        ESP_LOGI(TAG_AUDIO, "  [%d] = %d", i, stereo[i * 2]);
    }
}

// ============ Mic capture task ============

static void mic_capture_task(void* arg) {
    const int frames = 128;
    static int16_t stereo_buf[128 * 2];
    static int16_t mono_buf[128];

    ESP_LOGI(TAG_AUDIO, "Mic task started");
    while (true) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(g_i2s_rx, stereo_buf, frames * 4,
                                          &bytes_read, pdMS_TO_TICKS(1000));
        if (err == ESP_OK && bytes_read > 0) {
            int n = bytes_read / 4;
            int32_t peak = 0;
            for (int i = 0; i < n; i++) {
                mono_buf[i] = stereo_buf[i * 2];
                int32_t abs_s = mono_buf[i] < 0 ? -mono_buf[i] : mono_buf[i];
                if (abs_s > peak) peak = abs_s;
            }
            g_mic_total_samples += n;
            if (peak > g_mic_peak) g_mic_peak = peak;
            xRingbufferSend(g_mic_ringbuf, mono_buf, n * 2, 0);
        }
    }
}

// ============ Public API ============

static bool audio_init() {
    if (audio_i2c_init() != ESP_OK) return false;
    if (audio_i2s_init() != ESP_OK) return false;
    if (audio_codec_init() != ESP_OK) return false;
    pi4ioe_init();

    // Verify amp unmute: read back PI4IOE output register
    if (g_pi4ioe_dev) {
        uint8_t out_val = pi_rd(0x05);
        ESP_LOGW(TAG_AUDIO, ">>> PI4IOE output reg=0x%02X (0xFF=unmuted, 0x00=muted)", out_val);
        if (out_val != 0xFF) {
            ESP_LOGE(TAG_AUDIO, "AMP STILL MUTED! Forcing unmute...");
            pi_wr(0x05, 0xFF);
        }
    }

    // Verify DAC volume
    uint8_t vol = es_rd(0x32);
    ESP_LOGI(TAG_AUDIO, "DAC volume=0x%02X (0x00=min, 0xFF=max)", vol);

    audio_play_boot_voice();
    audio_test_mic();

    g_mic_ringbuf = xRingbufferCreate(MIC_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!g_mic_ringbuf) { ESP_LOGE(TAG_AUDIO, "Ringbuf fail"); return false; }
    BaseType_t ret = xTaskCreatePinnedToCore(mic_capture_task, "mic_cap", 4096, NULL, 3, NULL, 1);
    if (ret != pdPASS) { ESP_LOGE(TAG_AUDIO, "Mic task fail"); return false; }
    ESP_LOGI(TAG_AUDIO, "Audio ready");
    return true;
}

