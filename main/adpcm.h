/**
 * adpcm.h - IMA-ADPCM encoder/decoder for ESP32
 *
 * Compresses 16-bit PCM audio to 4-bit ADPCM (4:1 compression).
 * Quality is near-transparent for speech at 16kHz.
 *
 * Packet format: [2B state header: predicted(16bit) + index(8bit) + pad] + [ADPCM nibbles]
 * Header = 4 bytes: int16_t predicted, uint8_t step_index, uint8_t reserved
 */
#pragma once
#include <cstdint>
#include <cstring>

// IMA step size table (89 entries)
static const int16_t ima_step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,
    337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
    1707,1878,2066,2272,2499,2749,3024,3327,3660,3996,4397,4837,5321,5853,
    6438,7082,7790,8569,9426,10368,11405,12546,13801,15181,16699,18369,20206,
    22226,24449,26894,29583,32542
};

// IMA index adjustment table
static const int8_t ima_index_table[16] = {
    -1,-1,-1,-1, 2, 4, 6, 8,
    -1,-1,-1,-1, 2, 4, 6, 8
};

struct AdpcmState {
    int16_t predicted;
    uint8_t step_index;
};

// Encode one PCM sample → 4-bit ADPCM nibble
static inline uint8_t adpcm_encode_sample(int16_t sample, AdpcmState& st) {
    int step = ima_step_table[st.step_index];
    int diff = sample - st.predicted;
    uint8_t nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    if (diff >= step)     { nibble |= 4; diff -= step; }
    if (diff >= step / 2) { nibble |= 2; diff -= step / 2; }
    if (diff >= step / 4) { nibble |= 1; }

    // Update predictor
    int delta = 0;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step / 2;
    if (nibble & 1) delta += step / 4;
    delta += step / 8;
    if (nibble & 8) delta = -delta;

    int pred = st.predicted + delta;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    st.predicted = (int16_t)pred;

    int idx = st.step_index + ima_index_table[nibble];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    st.step_index = (uint8_t)idx;

    return nibble & 0x0F;
}

/**
 * Encode PCM block → ADPCM with 4-byte header
 * Input:  pcm (int16_t[]), n_samples
 * Output: out buffer, returns total bytes written
 * Format: [int16_t predicted][uint8_t step_index][uint8_t 0x00][nibbles...]
 * Output size = 4 + ceil(n_samples / 2)
 */
static int adpcm_encode(const int16_t* pcm, int n_samples, uint8_t* out, AdpcmState& st) {
    // Write header
    memcpy(out, &st.predicted, 2);
    out[2] = st.step_index;
    out[3] = 0;
    int pos = 4;

    for (int i = 0; i < n_samples; i += 2) {
        uint8_t lo = adpcm_encode_sample(pcm[i], st);
        uint8_t hi = (i + 1 < n_samples) ? adpcm_encode_sample(pcm[i + 1], st) : 0;
        out[pos++] = lo | (hi << 4);
    }
    return pos;
}

/**
 * Decode ADPCM packet → PCM
 * Input:  adpcm data with 4-byte header [predicted(2) + index(1) + pad(1) + nibbles...]
 * Output: pcm buffer (int16_t[]), returns number of samples decoded
 * Caller must ensure pcm buffer is large enough: (data_len - 4) * 2 samples
 */
static int adpcm_decode(const uint8_t* data, int data_len, int16_t* pcm) {
    if (data_len < 5) return 0;
    int16_t predicted;
    memcpy(&predicted, data, 2);
    uint8_t step_index = data[2];
    if (step_index > 88) step_index = 88;
    int out = 0;

    for (int i = 4; i < data_len; i++) {
        uint8_t byte = data[i];
        for (int shift = 0; shift <= 4; shift += 4) {
            uint8_t nibble = (byte >> shift) & 0x0F;
            int step = ima_step_table[step_index];
            int delta = step / 8;
            if (nibble & 4) delta += step;
            if (nibble & 2) delta += step / 2;
            if (nibble & 1) delta += step / 4;
            if (nibble & 8) delta = -delta;
            int pred = predicted + delta;
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            predicted = (int16_t)pred;
            pcm[out++] = predicted;
            int idx = step_index + ima_index_table[nibble];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            step_index = (uint8_t)idx;
        }
    }
    return out;
}
