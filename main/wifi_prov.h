/**
 * wifi_prov.h - WiFi Provisioning: Multi-WiFi NVS + AP + Captive Portal
 *
 * Stores up to 3 WiFi credentials in NVS (slot 0-2).
 * Boot: try each saved WiFi in order → all fail after 3min → AP + Portal
 * Portal: new WiFi connects OK → save to NVS (don't overwrite until success)
 */
#pragma once

#include <cstring>
#include <cstdio>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "config.h"

static const char* TAG_WIFI = "wifi_prov";

#define WIFI_MAX_SAVED 3

// ============ State ============
static EventGroupHandle_t g_wifi_events = NULL;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static esp_netif_t* g_netif_sta = NULL;
static esp_netif_t* g_netif_ap = NULL;
static httpd_handle_t g_portal_server = NULL;
static TaskHandle_t g_dns_task = NULL;
static volatile bool g_sta_pending = false;
static std::atomic<int> g_connect_state{0};  // 0=idle, 1=connecting, 2=success, 3=failed
static char g_pending_ssid[33] = {};
static char g_pending_pass[65] = {};
static char g_ap_ssid[32] = {};
static char g_sta_ip[20] = {};

// ============ Multi-WiFi NVS (3 slots) ============
// Keys: "ssid0","pass0", "ssid1","pass1", "ssid2","pass2", "count"

static int nvs_wifi_count() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t cnt = 0;
    nvs_get_u8(h, "count", &cnt);
    nvs_close(h);
    return cnt > WIFI_MAX_SAVED ? WIFI_MAX_SAVED : cnt;
}

static bool nvs_wifi_load(int slot, char* ssid, char* pass) {
    if (slot < 0 || slot >= WIFI_MAX_SAVED) return false;
    char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "ssid%d", slot);
    snprintf(kp, sizeof(kp), "pass%d", slot);

    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = 33, pl = 65;
    bool ok = (nvs_get_str(h, ks, ssid, &sl) == ESP_OK &&
               nvs_get_str(h, kp, pass, &pl) == ESP_OK && strlen(ssid) > 0);
    nvs_close(h);
    return ok;
}

// Save new WiFi: append to list, max 3, newest at end. Don't duplicate.
static void nvs_wifi_save(const char* ssid, const char* pass) {
    char saved_ssid[WIFI_MAX_SAVED][33] = {};
    char saved_pass[WIFI_MAX_SAVED][65] = {};
    int cnt = nvs_wifi_count();

    // Load existing
    for (int i = 0; i < cnt; i++) {
        nvs_wifi_load(i, saved_ssid[i], saved_pass[i]);
    }

    // Check if already exists → update password, move to front
    int existing = -1;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(saved_ssid[i], ssid) == 0) { existing = i; break; }
    }

    if (existing >= 0) {
        // Update password for existing entry
        strncpy(saved_pass[existing], pass, 64);
        ESP_LOGI(TAG_WIFI, "WiFi updated: %s (slot %d)", ssid, existing);
    } else {
        // Add new entry
        if (cnt < WIFI_MAX_SAVED) {
            // Append
            strncpy(saved_ssid[cnt], ssid, 32);
            strncpy(saved_pass[cnt], pass, 64);
            cnt++;
        } else {
            // Full: shift out oldest (slot 0), append new at end
            for (int i = 0; i < WIFI_MAX_SAVED - 1; i++) {
                strncpy(saved_ssid[i], saved_ssid[i+1], 32);
                strncpy(saved_pass[i], saved_pass[i+1], 64);
            }
            strncpy(saved_ssid[WIFI_MAX_SAVED-1], ssid, 32);
            strncpy(saved_pass[WIFI_MAX_SAVED-1], pass, 64);
        }
        ESP_LOGI(TAG_WIFI, "WiFi added: %s (total %d)", ssid, cnt);
    }

    // Write all back
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "count", cnt);
    for (int i = 0; i < cnt; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        nvs_set_str(h, ks, saved_ssid[i]);
        nvs_set_str(h, kp, saved_pass[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

// Clear all saved WiFi credentials
static void nvs_wifi_clear_all() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG_WIFI, "All WiFi config cleared");
    }
}

// ============ Event handler ============

// WiFi 运行时自动重连 (boot 阶段由 wifi_prov_try_saved 控制，不自动重连)
static std::atomic<bool> g_wifi_runtime_reconnect{false};
static std::atomic<uint8_t> g_wifi_reconnect_count{0};
#define WIFI_MAX_FAST_RECONNECTS 10  // 快速重连次数，超过后走 trySaved 流程

// TTS flags for WiFi events (checked by TTS task in ws_cam.h)
static std::atomic<bool> g_tts_wifi_disc_pending{false};
static std::atomic<bool> g_tts_wifi_reconn_pending{false};

static void wifi_event_cb(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) esp_wifi_connect();
        else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t* disc = (wifi_event_sta_disconnected_t*)data;
            ESP_LOGW(TAG_WIFI, "[CAM-WIFI] Disconnected reason=%d heap=%u",
                     disc->reason, (unsigned)esp_get_free_heap_size());
            g_sta_ip[0] = '\0';  // 清除 IP 防止误报 connected
            if (g_wifi_events) xEventGroupSetBits(g_wifi_events, WIFI_FAIL_BIT);
            // 运行时自动重连 (仅 boot 完成后启用)
            if (g_wifi_runtime_reconnect) {
                uint8_t cnt = g_wifi_reconnect_count.fetch_add(1);
                if (cnt == 0) {
                    // 首次断连时触发 TTS (避免重试期间重复播报)
                    g_tts_wifi_disc_pending = true;
                }
                if (cnt < WIFI_MAX_FAST_RECONNECTS) {
                    ESP_LOGI(TAG_WIFI, "[CAM-WIFI] Auto-reconnect %d/%d...", cnt + 1, WIFI_MAX_FAST_RECONNECTS);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG_WIFI, "[CAM-WIFI] Reconnect exhausted (%d), will restart...", cnt);
                }
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        snprintf(g_sta_ip, sizeof(g_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        esp_wifi_set_ps(WIFI_PS_NONE);  // Re-assert after every reconnect
        // WiFi RSSI
        wifi_ap_record_t ap_info;
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) rssi = ap_info.rssi;
        // WiFi 重连成功 TTS (仅运行时重连，不含 boot 首次连接)
        uint8_t prev_count = g_wifi_reconnect_count.load();
        if (g_wifi_runtime_reconnect && prev_count > 0) {
            g_tts_wifi_reconn_pending = true;
        }
        g_wifi_reconnect_count = 0;  // 重置重连计数
        ESP_LOGI(TAG_WIFI, "[CAM-WIFI] Got IP: %s rssi=%d retries=%u heap=%u",
                 g_sta_ip, rssi, (unsigned)prev_count, (unsigned)esp_get_free_heap_size());
        if (g_wifi_events) xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// ============ Init (call once) ============

static void wifi_prov_init() {
    g_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    g_netif_sta = esp_netif_create_default_wifi_sta();
    g_netif_ap  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_cb, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_cb, NULL, NULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
}

// ============ STA connect (single attempt) ============

static bool wifi_prov_connect_sta(const char* ssid, const char* pass) {
    ESP_LOGI(TAG_WIFI, "Connecting to: %s", ssid);
    xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);

    EventBits_t bits = xEventGroupWaitBits(g_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(STA_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) return true;

    // Failed — stop wifi for clean state
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    return false;
}

// ============ Try all saved WiFi, loop for 3 minutes ============

static bool wifi_prov_try_saved() {
    int cnt = nvs_wifi_count();
    if (cnt == 0) return false;

    ESP_LOGI(TAG_WIFI, "Trying %d saved WiFi (1.4 min max)...", cnt);

    int64_t start = esp_timer_get_time();
    int64_t timeout_us = 84LL * 1000000;  // 1.4 minutes (84 seconds)

    while ((esp_timer_get_time() - start) < timeout_us) {
        for (int i = 0; i < cnt; i++) {
            char ssid[33] = {}, pass[65] = {};
            if (!nvs_wifi_load(i, ssid, pass)) continue;

            ESP_LOGI(TAG_WIFI, "Trying slot %d: %s", i, ssid);
            if (wifi_prov_connect_sta(ssid, pass)) {
                ESP_LOGI(TAG_WIFI, "Connected to %s (slot %d)", ssid, i);
                return true;
            }

            // Check timeout between attempts
            if ((esp_timer_get_time() - start) >= timeout_us) break;
        }
        ESP_LOGW(TAG_WIFI, "All saved WiFi failed, retrying...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGW(TAG_WIFI, "1.4 min timeout — all saved WiFi unreachable");
    return false;
}

// ============ AP mode ============

static void wifi_prov_start_ap() {
    ESP_LOGI(TAG_WIFI, "Starting AP: %s (pw: %s)", g_ap_ssid, AP_PASSWORD);

    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, g_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char*)ap_cfg.ap.password, AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = strlen(g_ap_ssid);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;
    ap_cfg.ap.pmf_cfg.capable = false;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    // DHCP 下发 DNS = 192.168.4.1 (关键!)
    // 没有这步, 手机不知道用 AP 做 DNS → DNS 走蜂窝 → 劫持失效 → 浏览器打不开
    if (g_netif_ap) {
        esp_netif_dhcps_stop(g_netif_ap);
        esp_netif_dns_info_t dns_info = {};
        dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(g_netif_ap, ESP_NETIF_DNS_MAIN, &dns_info);
        uint8_t dhcps_dns_opt = 1;
        esp_netif_dhcps_option(g_netif_ap, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_opt, sizeof(dhcps_dns_opt));
        esp_netif_dhcps_start(g_netif_ap);
        ESP_LOGI(TAG_WIFI, "DHCP DNS set to 192.168.4.1");
    }
    vTaskDelay(pdMS_TO_TICKS(300));
}

// ============ DNS Hijack ============

static volatile bool g_dns_running = false;

static void dns_hijack_task(void* arg) {
    ESP_LOGI(TAG_WIFI, "DNS hijack task started");

    int sock = -1;
    for (int retry = 0; retry < 3 && sock < 0; retry++) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGW(TAG_WIFI, "DNS socket create failed (attempt %d/3)", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (sock < 0) {
        ESP_LOGE(TAG_WIFI, "DNS socket create failed after 3 retries");
        g_dns_running = false;
        g_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG_WIFI, "DNS bind port 53 failed");
        close(sock);
        g_dns_running = false;
        g_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    struct sockaddr_in src;
    socklen_t src_len;
    uint8_t ap_ip[4] = {192, 168, 4, 1};

    while (g_dns_running) {
        src_len = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&src, &src_len);
        if (len < 12) continue;

        // Parse QTYPE (skip QNAME labels to reach QTYPE field)
        int qname_end = 12;
        while (qname_end < len && buf[qname_end] != 0) {
            uint8_t label_len = buf[qname_end];
            qname_end += 1 + label_len;
            if (qname_end > len) break;
        }
        qname_end++;  // skip terminating 0x00
        uint16_t qtype = 0;
        if (qname_end + 2 <= len) {
            qtype = (buf[qname_end] << 8) | buf[qname_end + 1];
        }

        buf[2] = 0x81; buf[3] = 0x80;  // Response + Recursion Available

        if (qtype == 1) {
            // A record → respond with AP IP
            buf[6] = 0x00; buf[7] = 0x01;  // 1 Answer
            int resp_len = len;
            if (resp_len + 16 <= (int)sizeof(buf)) {
                buf[resp_len++] = 0xC0; buf[resp_len++] = 0x0C;
                buf[resp_len++] = 0x00; buf[resp_len++] = 0x01;  // Type A
                buf[resp_len++] = 0x00; buf[resp_len++] = 0x01;  // Class IN
                buf[resp_len++] = 0x00; buf[resp_len++] = 0x00;
                buf[resp_len++] = 0x00; buf[resp_len++] = 0x0A;  // TTL 10s
                buf[resp_len++] = 0x00; buf[resp_len++] = 0x04;
                memcpy(&buf[resp_len], ap_ip, 4);
                resp_len += 4;
            }
            sendto(sock, buf, resp_len, 0, (struct sockaddr*)&src, src_len);
        } else {
            // AAAA/HTTPS/other → 0 answers (let client give up IPv6 quickly)
            buf[6] = 0x00; buf[7] = 0x00;
            sendto(sock, buf, len, 0, (struct sockaddr*)&src, src_len);
        }
    }

    close(sock);
    ESP_LOGI(TAG_WIFI, "DNS hijack task stopped");
    g_dns_task = NULL;
    vTaskDelete(NULL);
}

// ============ Captive Portal ============

static int g_portal_served_count = 0;
// WiFi scan state (async)
static volatile bool g_scan_started = false;

static const char PORTAL_HTML[] = R"rawhtml(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Mingle</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{font-family:-apple-system,system-ui,sans-serif;max-width:420px;margin:0 auto;padding:16px;background:#f0f2f5;color:#333}
h2{text-align:center;margin:12px 0;font-size:20px}
.card{background:#fff;border-radius:12px;padding:16px;margin:10px 0;box-shadow:0 1px 3px rgba(0,0,0,.1)}
.card h3{margin:0 0 10px;font-size:15px;color:#666}
input{width:100%;padding:12px;margin:4px 0;border:1px solid #ddd;border-radius:8px;font-size:15px;outline:none}
input:focus{border-color:#4CAF50}
button{width:100%;padding:12px;border:none;border-radius:8px;font-size:16px;cursor:pointer;margin:4px 0}
.btn-main{background:#4CAF50;color:#fff}
.btn-scan{background:#e8e8e8;color:#333}
#status{text-align:center;padding:8px;margin:8px 0;border-radius:8px}
.ok{background:#e8f5e9;color:#2e7d32}
.err{background:#fbe9e7;color:#c62828}
.wifi-item{padding:12px 8px;border-bottom:1px solid #f0f0f0;cursor:pointer;display:flex;justify-content:space-between}
.wifi-item:active{background:#e8f5e9}
.sig{color:#999;font-size:13px}
#wl{max-height:200px;overflow-y:auto}
.hide{display:none}
</style></head><body>
<h2>Mingle WiFi</h2>
<div class="card">
<h3>WiFi Network</h3>
<input id="ssid" placeholder="WiFi name">
<input id="pass" type="password" placeholder="Password">
<button class="btn-main" onclick="go()">Connect</button>
</div>
<div class="card">
<button class="btn-scan" onclick="scan()">Scan WiFi Networks</button>
<div id="wl" class="hide"></div>
</div>
<div id="status"></div>
<script>
function $(id){return document.getElementById(id)}
var scanRetry=0;
function scan(){
var w=$('wl');w.className='';w.innerHTML='Scanning...';
scanRetry=0;doScan()}
function doScan(){
fetch('/scan').then(function(r){return r.json()}).then(function(d){
var w=$('wl');
if(!d.length&&scanRetry<3){scanRetry++;setTimeout(doScan,1500);w.innerHTML='Scanning... ('+scanRetry+')';return}
if(!d.length){w.innerHTML='No networks found.';return}
var h='';for(var i=0;i<d.length;i++){
h+='<div class="wifi-item" onclick="pick(\''+d[i].ssid.replace(/\\/g,'\\\\').replace(/'/g,"\\'").replace(/"/g,'&quot;')+'\')">'
+d[i].ssid+'<span class="sig">'+d[i].rssi+'</span></div>'}
w.innerHTML=h}).catch(function(){$('wl').innerHTML='Scan failed.'})}
function pick(n){$('ssid').value=n;$('wl').className='hide'}
setTimeout(scan,500);
function go(){
var s=$('ssid').value,p=$('pass').value;
if(!s){$('status').innerHTML='<div class="err">Enter WiFi name</div>';return}
$('status').innerHTML='Connecting...';
fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({ssid:s,password:p})})
.then(function(r){return r.json()}).then(function(){
$('status').innerHTML='Connecting to <b>'+s+'</b> ...';
var tries=0,maxTries=10,done=false;
var poll=setInterval(function(){
if(done)return;
tries++;
if(tries>maxTries){done=true;clearInterval(poll);$('status').innerHTML='<div class="err">Timeout. Check device.</div>';return}
fetch('/status').then(function(r){return r.json()}).then(function(d){
if(done)return;
if(d.state===2&&d.connected){done=true;clearInterval(poll);$('status').innerHTML='<div class="ok">Connected! IP: '+d.ip+'</div>'}
else if(d.state===3){done=true;clearInterval(poll);$('status').innerHTML='<div class="err">Failed. Check password and retry.</div>'}
else{$('status').innerHTML='Connecting to <b>'+s+'</b> ... ('+tries*2+'s)'}
}).catch(function(){if(!done){done=true;clearInterval(poll);$('status').innerHTML='<div class="ok">WiFi connected. Setup complete!</div>'}})
},2000)})
.catch(function(){$('status').innerHTML='<div class="ok">Sent. Check device.</div>'})}
</script></body></html>
)rawhtml";

static esp_err_t portal_root_handler(httpd_req_t* req) {
    g_portal_served_count++;
    ESP_LOGI(TAG_WIFI, "[PORTAL] served page #%d", g_portal_served_count);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, strlen(PORTAL_HTML));
}

// Two-phase captive portal: Android/Windows/Firefox
static esp_err_t portal_check_handler(httpd_req_t* req) {
    ESP_LOGI(TAG_WIFI, "[PORTAL] check: %s phase=%d", req->uri, g_portal_served_count);
    if (g_portal_served_count >= 1) {
        // Phase 2: tell OS "internet works" → browser uses WiFi not cellular
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_send(req, NULL, 0);
    }
    // Phase 1: 302 redirect → trigger captive portal popup
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    return httpd_resp_send(req, NULL, 0);
}

// Two-phase captive portal: Apple iOS/macOS
static esp_err_t portal_apple_handler(httpd_req_t* req) {
    ESP_LOGI(TAG_WIFI, "[PORTAL] apple check: %s phase=%d", req->uri, g_portal_served_count);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (g_portal_served_count >= 1) {
        // Phase 2: Apple sees "Success" → considers WiFi as having internet
        return httpd_resp_sendstr(req,
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    }
    // Phase 1: no "Success" → trigger captive portal popup
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    return httpd_resp_sendstr(req,
        "<html><head><meta http-equiv='refresh' content='0;url=http://192.168.4.1/'></head>"
        "<body>Portal</body></html>");
}

// Async WiFi scan handler (non-blocking)
static esp_err_t portal_scan_handler(httpd_req_t* req) {
    // Switch to APSTA for scan (need STA interface)
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Check if scan is already done
    uint16_t count = 0;
    esp_err_t ret = esp_wifi_scan_get_ap_num(&count);

    if (!g_scan_started || count == 0) {
        // Start async scan
        wifi_scan_config_t scan_cfg = {};
        scan_cfg.show_hidden = true;
        esp_wifi_scan_start(&scan_cfg, false);  // async!
        g_scan_started = true;
        // Return empty — frontend JS retries after 1.5s
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }

    if (count > 20) count = 20;
    wifi_ap_record_t* records = NULL;
    if (count > 0) {
        records = (wifi_ap_record_t*)malloc(count * sizeof(wifi_ap_record_t));
        if (records) esp_wifi_scan_get_ap_records(&count, records);
        else count = 0;
    }

    char json[2048] = "[";
    for (int i = 0; i < count && records; i++) {
        // Escape SSID for JSON safety
        char escaped_ssid[66] = {};
        const char* src = (const char*)records[i].ssid;
        int ei = 0;
        for (int si = 0; src[si] && ei < 63; si++) {
            if (src[si] == '"' || src[si] == '\\') { escaped_ssid[ei++] = '\\'; }
            if (ei < 64) escaped_ssid[ei++] = src[si];
        }
        escaped_ssid[ei] = 0;

        char entry[100];
        snprintf(entry, sizeof(entry), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 i > 0 ? "," : "", escaped_ssid, records[i].rssi);
        if (strlen(json) + strlen(entry) < sizeof(json) - 2)
            strcat(json, entry);
    }
    strcat(json, "]");
    if (records) free(records);
    g_scan_started = false;  // allow re-scan next time

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t portal_connect_handler(httpd_req_t* req) {
    char buf[200] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data"); return ESP_FAIL; }

    char* s = strstr(buf, "\"ssid\":\"");
    char* p = strstr(buf, "\"password\":\"");
    if (!s) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No ssid"); return ESP_FAIL; }

    s += 8;
    char* se = strchr(s, '"');
    if (se) {
        int slen = se - s;
        if (slen > 32) slen = 32;
        strncpy(g_pending_ssid, s, slen);
        g_pending_ssid[slen] = 0;
    }

    g_pending_pass[0] = 0;
    if (p) {
        p += 12;
        char* pe = strchr(p, '"');
        if (pe) {
            int plen = pe - p;
            if (plen > 64) plen = 64;
            strncpy(g_pending_pass, p, plen);
            g_pending_pass[plen] = 0;
        }
    }

    g_sta_pending = true;
    g_connect_state = 0;
    ESP_LOGI(TAG_WIFI, "Pending connect to: %s", g_pending_ssid);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"msg\":\"Connecting...\"}");
}

static esp_err_t portal_status_handler(httpd_req_t* req) {
    char json[120];
    int state = g_connect_state.load();
    bool connected = (state == 2 && strlen(g_sta_ip) > 0);
    snprintf(json, sizeof(json), "{\"connected\":%s,\"ip\":\"%s\",\"state\":%d}",
             connected ? "true" : "false", g_sta_ip, state);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

// ============ Start/Stop Portal ============

static void wifi_prov_start_portal() {
    g_dns_running = true;
    g_portal_served_count = 0;
    g_scan_started = false;
    xTaskCreatePinnedToCore(dns_hijack_task, "dns", 4096, NULL, 2, &g_dns_task, 0);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    if (httpd_start(&g_portal_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Portal server fail");
        return;
    }

    // Core routes
    httpd_uri_t routes[] = {
        {"/",               HTTP_GET,  portal_root_handler, NULL},
        {"/scan",           HTTP_GET,  portal_scan_handler, NULL},
        {"/connect",        HTTP_POST, portal_connect_handler, NULL},
        {"/status",         HTTP_GET,  portal_status_handler, NULL},
        // Android: Google/Samsung/Chrome
        {"/generate_204",   HTTP_GET,  portal_check_handler, NULL},
        {"/gen_204",        HTTP_GET,  portal_check_handler, NULL},
        {"/generate204",    HTTP_GET,  portal_check_handler, NULL},
        // Apple iOS/macOS
        {"/hotspot-detect.html",        HTTP_GET, portal_apple_handler, NULL},
        {"/library/test/success.html",  HTTP_GET, portal_apple_handler, NULL},
        // Windows NCSI
        {"/connecttest.txt", HTTP_GET, portal_check_handler, NULL},
        {"/redirect",       HTTP_GET,  portal_check_handler, NULL},
        {"/ncsi.txt",       HTTP_GET,  portal_check_handler, NULL},
        // Firefox
        {"/success.txt",    HTTP_GET,  portal_check_handler, NULL},
    };
    for (int i = 0; i < 13; i++) httpd_register_uri_handler(g_portal_server, &routes[i]);

    ESP_LOGI(TAG_WIFI, "Captive portal at http://192.168.4.1/ (%d routes)", 13);
}

static void wifi_prov_stop_portal() {
    if (g_portal_server) { httpd_stop(g_portal_server); g_portal_server = NULL; }
    // Clean shutdown: signal DNS task to stop, wait for it to close socket
    if (g_dns_task) {
        g_dns_running = false;
        vTaskDelay(pdMS_TO_TICKS(1500));  // wait for recvfrom timeout + cleanup
        g_dns_task = NULL;
    }
}

// ============ Handle pending STA from portal ============
// Only saves to NVS if connection succeeds!

static bool wifi_prov_handle_pending() {
    if (!g_sta_pending) return false;
    g_sta_pending = false;
    g_connect_state = 1;  // connecting

    ESP_LOGI(TAG_WIFI, "Attempting STA (APSTA): %s", g_pending_ssid);

    // Set STA config first, then switch to APSTA.
    // STA_START event auto-calls esp_wifi_connect() with correct credentials.
    // AP + portal stay alive so phone can poll /status.
    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, g_pending_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char*)cfg.sta.password, g_pending_pass, sizeof(cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_set_mode(WIFI_MODE_APSTA);  // triggers STA_START → esp_wifi_connect()

    EventBits_t bits = xEventGroupWaitBits(g_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(STA_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        g_connect_state = 2;  // success
        nvs_wifi_save(g_pending_ssid, g_pending_pass);
        ESP_LOGI(TAG_WIFI, "Connected & saved: %s IP=%s", g_pending_ssid, g_sta_ip);
        // Keep portal alive briefly so phone can see success status
        vTaskDelay(pdMS_TO_TICKS(3000));
        wifi_prov_stop_portal();
        esp_wifi_set_mode(WIFI_MODE_STA);  // switch to STA-only, free AP resources
        return true;
    }

    // Failed — back to AP-only, portal stays alive for retry
    g_connect_state = 3;  // failed
    ESP_LOGW(TAG_WIFI, "STA failed, back to AP (portal stays)");
    g_sta_ip[0] = 0;
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_AP);
    // 重置 portal 计数 → 手机重连 AP 时 captive portal 检测会再次触发弹窗
    // 不重置的话: 检测返回 204 "有网" → 华为等手机把流量走蜂窝 → 192.168.4.1 不可达
    g_portal_served_count = 0;
    return false;
}

// ============ Public API ============

static bool wifi_prov_has_saved() { return nvs_wifi_count() > 0; }
static const char* wifi_prov_get_ap_ssid() { return g_ap_ssid; }
static const char* wifi_prov_get_sta_ip() { return g_sta_ip; }
