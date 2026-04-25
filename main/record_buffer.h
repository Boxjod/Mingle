/**
 * record_buffer.h — PSRAM Ring Buffer for CAM Recording Aggregation
 *
 * Stores {JPEG + leader_positions + follower_positions} frames in PSRAM.
 * Producer: ws_cam recording capture (10Hz)
 * Consumer: ws_cam recording upload task
 *
 * Thread-safe via atomic read/write indices (single producer, single consumer).
 */
#pragma once

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <atomic>
#include <string.h>

static const char* TAG_REC = "rec_buf";

#define REC_MAX_SERVOS    20
#define REC_BUF_CAPACITY  60    // ~3MB at 50KB/frame avg
#define REC_JPEG_MAX_SIZE 100000  // 100KB max JPEG per frame

struct RecServo {
    uint8_t  id;
    uint16_t pos;
};

struct RecFrame {
    int64_t    ntp_ts;          // NTP timestamp ms
    uint8_t    f_count;         // follower servo count
    uint8_t    l_count;         // leader servo count
    RecServo   f_servos[REC_MAX_SERVOS];
    RecServo   l_servos[REC_MAX_SERVOS];
    uint8_t*   jpeg;            // PSRAM pointer (owned by buffer)
    size_t     jpeg_len;
    bool       valid;
};

class RecordBuffer {
public:
    bool init() {
        // Pre-allocate JPEG slots in PSRAM
        for (int i = 0; i < REC_BUF_CAPACITY; i++) {
            frames_[i].jpeg = (uint8_t*)heap_caps_malloc(REC_JPEG_MAX_SIZE, MALLOC_CAP_SPIRAM);
            if (!frames_[i].jpeg) {
                ESP_LOGE(TAG_REC, "PSRAM alloc failed at slot %d", i);
                deinit();
                return false;
            }
            frames_[i].valid = false;
        }
        write_idx_.store(0, std::memory_order_relaxed);
        read_idx_.store(0, std::memory_order_relaxed);
        recording_ = false;
        initialized_.store(true, std::memory_order_release);
        ESP_LOGI(TAG_REC, "Ring buffer ready: %d slots, %dKB PSRAM",
                 REC_BUF_CAPACITY, REC_BUF_CAPACITY * REC_JPEG_MAX_SIZE / 1024);
        return true;
    }

    void deinit() {
        for (int i = 0; i < REC_BUF_CAPACITY; i++) {
            if (frames_[i].jpeg) {
                heap_caps_free(frames_[i].jpeg);
                frames_[i].jpeg = nullptr;
            }
            frames_[i].valid = false;
        }
        initialized_.store(false, std::memory_order_release);
    }

    void startRecording() {
        write_idx_.store(0, std::memory_order_relaxed);
        read_idx_.store(0, std::memory_order_relaxed);
        frame_count_.store(0, std::memory_order_relaxed);
        recording_.store(true, std::memory_order_release);
        ESP_LOGI(TAG_REC, "Recording started");
    }

    void stopRecording() {
        recording_.store(false, std::memory_order_release);
        ESP_LOGI(TAG_REC, "Recording stopped, %d frames buffered", frame_count_.load());
    }

    bool isRecording() const { return recording_.load(std::memory_order_acquire); }
    bool isInitialized() const { return initialized_.load(std::memory_order_acquire); }

    /**
     * Push a frame into the ring buffer (producer side, called from capture task)
     * Returns false if buffer full (frame dropped)
     */
    bool push(int64_t ntp_ts,
              const RecServo* f_servos, uint8_t f_count,
              const RecServo* l_servos, uint8_t l_count,
              const uint8_t* jpeg_data, size_t jpeg_len) {
        if (!isInitialized() || !isRecording()) return false;

        uint32_t w = write_idx_.load(std::memory_order_relaxed);
        uint32_t r = read_idx_.load(std::memory_order_acquire);
        uint32_t next_w = (w + 1) % REC_BUF_CAPACITY;

        if (next_w == r) {
            // Buffer full — drop frame
            ESP_LOGW(TAG_REC, "Buffer full, dropping frame");
            return false;
        }

        RecFrame& f = frames_[w];
        f.ntp_ts = ntp_ts;
        f.f_count = f_count < REC_MAX_SERVOS ? f_count : REC_MAX_SERVOS;
        f.l_count = l_count < REC_MAX_SERVOS ? l_count : REC_MAX_SERVOS;
        memcpy(f.f_servos, f_servos, f.f_count * sizeof(RecServo));
        memcpy(f.l_servos, l_servos, f.l_count * sizeof(RecServo));

        size_t copy_len = jpeg_len < REC_JPEG_MAX_SIZE ? jpeg_len : REC_JPEG_MAX_SIZE;
        memcpy(f.jpeg, jpeg_data, copy_len);
        f.jpeg_len = copy_len;
        f.valid = true;

        write_idx_.store(next_w, std::memory_order_release);
        frame_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /**
     * Pop a frame from the ring buffer (consumer side, called from upload task)
     * Returns pointer to frame (valid until next pop) or nullptr if empty
     */
    const RecFrame* pop() {
        if (!isInitialized()) return nullptr;

        uint32_t r = read_idx_.load(std::memory_order_relaxed);
        uint32_t w = write_idx_.load(std::memory_order_acquire);

        if (r == w) return nullptr;  // empty

        const RecFrame& f = frames_[r];
        if (!f.valid) return nullptr;

        return &f;
    }

    void popCommit() {
        uint32_t r = read_idx_.load(std::memory_order_relaxed);
        frames_[r].valid = false;
        read_idx_.store((r + 1) % REC_BUF_CAPACITY, std::memory_order_release);
    }

    bool isEmpty() const {
        return read_idx_.load(std::memory_order_acquire) ==
               write_idx_.load(std::memory_order_acquire);
    }

    int pendingCount() const {
        int w = write_idx_.load(std::memory_order_acquire);
        int r = read_idx_.load(std::memory_order_acquire);
        return (w - r + REC_BUF_CAPACITY) % REC_BUF_CAPACITY;
    }

    int totalFrames() const { return frame_count_.load(std::memory_order_relaxed); }

private:
    RecFrame frames_[REC_BUF_CAPACITY] = {};
    std::atomic<uint32_t> write_idx_{0};
    std::atomic<uint32_t> read_idx_{0};
    std::atomic<bool> recording_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<int> frame_count_{0};
};

// Global instance
static RecordBuffer g_rec_buf;
