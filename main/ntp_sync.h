/**
 * ntp_sync.h - NTP time synchronization for Box2Robot
 *
 * Syncs system clock to Beijing time (UTC+8) via NTP after WiFi connects.
 * Uses Aliyun NTP servers for low-latency in China.
 *
 * Usage:
 *   ntp_sync_start();                    // call once after WiFi connected
 *   int64_t ms = ntp_timestamp_ms();     // get current timestamp in ms (UTC)
 *   bool ok = ntp_is_synced();           // check if time is valid
 */
#pragma once

#include <ctime>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_log.h"

static const char* TAG_NTP = "ntp";

static volatile bool g_ntp_synced = false;

// NTP sync callback
static void ntp_sync_cb(struct timeval* tv) {
    g_ntp_synced = true;
    time_t now = tv->tv_sec;
    struct tm ti;
    localtime_r(&now, &ti);
    ESP_LOGI(TAG_NTP, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d (Beijing)",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

// Start NTP sync (call once after WiFi connected, non-blocking)
static void ntp_sync_start() {
    ESP_LOGI(TAG_NTP, "Starting NTP sync...");

    // Set timezone to Beijing (UTC+8)
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.ntp.org.cn");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(ntp_sync_cb);
    esp_sntp_set_sync_interval(3600000);  // Re-sync every hour
    esp_sntp_init();
}

// Get current timestamp in milliseconds (Unix epoch, UTC)
static int64_t ntp_timestamp_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

// Check if NTP has synced at least once
static bool ntp_is_synced() {
    return g_ntp_synced;
}
