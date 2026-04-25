/**
 * device_reg.h - Device Registration & Binding
 *
 * Flow: generate device_id from MAC → POST /api/ota/activate → get bind_code
 *       → poll until bound → receive token + ws_url
 *
 * API: POST https://<your-server>/api/ota/activate
 *   Headers: Device-Id, Client-Id
 *   Body: {"device_type":"mingle_bot","firmware":{"version":"x.x.x"}}
 *   Response: {"status":"need_bind","bind_code":"123456"}
 *          or {"status":"activated","websocket":{"url":"...","token":"..."}}
 */
#pragma once

#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "config.h"

static const char* TAG_REG = "dev_reg";

// State
static char g_device_id[20] = {};   // "MGL-AABBCCDDEEFF"
static char g_client_id[20] = {};   // "xxxxxxxx-xxxx"
static char g_bind_code[8] = {};
static char g_token[65] = {};
static char g_ws_url[128] = {};

// ============ Generate device_id from eFuse MAC ============

static void device_reg_gen_ids() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_device_id, sizeof(g_device_id), "MGL-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Load or generate client_id
    nvs_handle_t h;
    if (nvs_open(NVS_NS_AUTH, NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(g_client_id);
        if (nvs_get_str(h, NVS_KEY_CLIENT_ID, g_client_id, &len) != ESP_OK || strlen(g_client_id) == 0) {
            // Generate from MAC
            snprintf(g_client_id, sizeof(g_client_id), "%02x%02x%02x%02x-%02x%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            nvs_set_str(h, NVS_KEY_CLIENT_ID, g_client_id);
            nvs_commit(h);
        }

        // Check for existing token
        len = sizeof(g_token);
        if (nvs_get_str(h, NVS_KEY_TOKEN, g_token, &len) != ESP_OK) g_token[0] = 0;
        len = sizeof(g_ws_url);
        if (nvs_get_str(h, NVS_KEY_WS_URL, g_ws_url, &len) != ESP_OK) g_ws_url[0] = 0;

        nvs_close(h);
    }

    ESP_LOGI(TAG_REG, "Device ID: %s", g_device_id);
    ESP_LOGI(TAG_REG, "Client ID: %s", g_client_id);
    if (g_token[0]) ESP_LOGI(TAG_REG, "Saved token: %.8s...", g_token);
}

// ============ HTTP response buffer ============

static char g_http_buf[512];
static int g_http_buf_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (g_http_buf_len + copy >= (int)sizeof(g_http_buf) - 1)
            copy = sizeof(g_http_buf) - 1 - g_http_buf_len;
        if (copy > 0) {
            memcpy(g_http_buf + g_http_buf_len, evt->data, copy);
            g_http_buf_len += copy;
            g_http_buf[g_http_buf_len] = 0;
        }
    }
    return ESP_OK;
}

// Simple JSON string extractor: find "key":"value" or "key": "value"
static bool json_get_str(const char* json, const char* key, char* out, int out_len) {
    char pattern[64];
    // Try without space first: "key":"
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) {
        // Try with space: "key": "
        snprintf(pattern, sizeof(pattern), "\"%s\": \"", key);
        p = strstr(json, pattern);
    }
    if (!p) return false;
    p += strlen(pattern);
    const char* e = strchr(p, '"');
    if (!e) return false;
    int len = e - p;
    if (len >= out_len) len = out_len - 1;
    strncpy(out, p, len);
    out[len] = 0;
    return true;
}

// ============ Activate (POST /api/ota/activate) ============

typedef enum {
    REG_NEED_BIND,    // Got bind_code, waiting for user
    REG_ACTIVATED,    // Got token + ws_url
    REG_ERROR,        // HTTP or server error
} reg_result_t;

static reg_result_t device_reg_activate() {
    char url[128];
    snprintf(url, sizeof(url), "%s/api/ota/activate", SERVER_BASE_URL);

    // POST body
    char body[200];
    snprintf(body, sizeof(body),
        "{\"device_type\":\"%s\",\"firmware\":{\"version\":\"%s\"}}",
        DEVICE_TYPE, FW_VERSION);

    g_http_buf_len = 0;
    g_http_buf[0] = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.event_handler = http_event_handler;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { ESP_LOGE(TAG_REG, "HTTP client init fail"); return REG_ERROR; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Device-Id", g_device_id);
    esp_http_client_set_header(client, "Client-Id", g_client_id);
    esp_http_client_set_post_field(client, body, strlen(body));

    ESP_LOGI(TAG_REG, "POST %s", url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG_REG, "HTTP request failed: %s", esp_err_to_name(err));
        return REG_ERROR;
    }

    ESP_LOGI(TAG_REG, "HTTP %d: %s", status, g_http_buf);

    if (status != 200) return REG_ERROR;

    // Parse response
    char status_str[16] = {};
    json_get_str(g_http_buf, "status", status_str, sizeof(status_str));

    if (strcmp(status_str, "need_bind") == 0) {
        json_get_str(g_http_buf, "bind_code", g_bind_code, sizeof(g_bind_code));
        ESP_LOGI(TAG_REG, "Bind code: %s", g_bind_code);
        return REG_NEED_BIND;
    }

    if (strcmp(status_str, "activated") == 0) {
        // Extract nested websocket.url and websocket.token
        json_get_str(g_http_buf, "url", g_ws_url, sizeof(g_ws_url));
        json_get_str(g_http_buf, "token", g_token, sizeof(g_token));

        // Save to NVS
        nvs_handle_t h;
        if (nvs_open(NVS_NS_AUTH, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_KEY_TOKEN, g_token);
            nvs_set_str(h, NVS_KEY_WS_URL, g_ws_url);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG_REG, "Activated! WS: %s", g_ws_url);
        return REG_ACTIVATED;
    }

    return REG_ERROR;
}

// ============ Poll until bound (call activate every 5s) ============

static reg_result_t device_reg_poll_bind(int max_attempts) {
    for (int i = 0; i < max_attempts; i++) {
        ESP_LOGI(TAG_REG, "Poll bind %d/%d...", i + 1, max_attempts);
        reg_result_t r = device_reg_activate();
        if (r == REG_ACTIVATED) return REG_ACTIVATED;
        if (r == REG_ERROR) ESP_LOGW(TAG_REG, "Server error, retrying...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    return REG_NEED_BIND;  // timeout
}

// ============ Getters ============

static const char* device_reg_get_id() { return g_device_id; }
static const char* device_reg_get_bind_code() { return g_bind_code; }
static const char* device_reg_get_token() { return g_token; }
static const char* device_reg_get_ws_url() { return g_ws_url; }
static bool device_reg_is_bound() { return g_token[0] != 0; }
