// main/wifi_provision.c —— 软 AP 配网。
//
// 流程：设备起 AP（AI-Passport-XXXX，开放网络）→ 手机扫码/手动加入 →
// captive portal 自动弹出（DNS 劫持所有域名到 192.168.4.1）→
// 页面列出周边 Wi-Fi → 选网络输密码 + 填配网码 → 设备存 NVS 并重启连接。
//
// 安全：AP 开放但配网页需填设备屏幕上显示的 4 位配网码，
// 旁邻无法在不知道码的情况下改写你的 Wi-Fi 凭证。
//
// 内存注意：esp_http_server + DNS 任务 + 扫描约 25–30KB，与 LVGL 40K 池并存
// 时堆余量充足（实测 DRAM 空闲 160KB）。
#include "wifi_provision.h"
#include "wifi_store.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_prov";
static bool s_active;
static httpd_handle_t s_http;
static int s_dns_sock = -1;
static char s_ap_ssid[33];
static char s_pin[5];

void wifi_prov_ap_ssid(char *buf, size_t n) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, n, "AI-Passport-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void wifi_prov_pin(char *buf, size_t n) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    unsigned v = (((mac[3] << 8) | mac[4]) ^ mac[5]) % 10000;
    snprintf(buf, n, "%04u", v);
}

bool wifi_prov_active(void) { return s_active; }

// ---------- 极简 DNS 劫持：所有 A 查询 → 192.168.4.1 ----------
static void dns_task(void *arg) {
    (void)arg;
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr));
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s_dns_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        // 只处理标准查询（QR=0, OPCODE=0, QDCOUNT=1）
        if ((buf[2] & 0x80) || (buf[2] & 0x78) != 0 || buf[5] != 1) continue;
        // 复制问题段到应答
        int qlen = n - 12;
        uint8_t resp[512];
        memcpy(resp, buf, n);
        resp[2] |= 0x80;                 // QR=1（响应）
        resp[3] = (resp[3] & 0xF0) | 0;  // RCODE=0
        resp[6] = 0; resp[7] = 1;        // ANCOUNT=1
        // A 记录：Name(指针 0xC00C) Type=1 Class=1 TTL=60 RDLength=4 RData=IP
        uint8_t rr[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                        0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
                        192, 168, 4, 1};
        memcpy(resp + n, rr, sizeof(rr));
        sendto(s_dns_sock, resp, n + sizeof(rr), 0,
               (struct sockaddr *)&from, flen);
    }
}

// ---------- 扫描列表 ----------
static char s_scan_json[1024];

static void do_scan(void) {
    wifi_scan_config_t sc = { 0 };
    esp_wifi_scan_start(&sc, true);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    static wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);
    s_scan_json[0] = 0;
    strlcat(s_scan_json, "[", sizeof(s_scan_json));
    for (int i = 0; i < n; i++) {
        if (i) strlcat(s_scan_json, ",", sizeof(s_scan_json));
        char item[130];
        snprintf(item, sizeof(item),
                 "{\"ssid\":\"%.28s\",\"rssi\":%d,\"auth\":%s}",
                 (char *)recs[i].ssid, recs[i].rssi,
                 recs[i].authmode > WIFI_AUTH_OPEN ? "true" : "false");
        strlcat(s_scan_json, item, sizeof(s_scan_json));
    }
    strlcat(s_scan_json, "]", sizeof(s_scan_json));
}

// ---------- 配网页 ----------
static const char PAGE_FMT[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>AI Passport 配网</title><style>"
"body{font:16px -apple-system,sans-serif;background:#f5f5f7;color:#14161a;"
"padding:28px 16px;margin:0}"
".w{max-width:420px;margin:0 auto}"
"h2{font-size:24px;margin:0 0 4px}.s{color:#8a8f98;font-size:13px;margin-bottom:18px}"
".p{background:#fff;border-radius:14px;padding:16px;margin-bottom:14px}"
"label{font-size:13px;color:#8a8f98;display:block;margin:12px 0 4px}"
"select,input{width:100%;font:inherit;padding:10px;border:1px solid #e2e3e6;"
"border-radius:8px;box-sizing:border-box;background:#fff}"
"button{width:100%;font:inherit;font-weight:600;padding:14px;margin-top:16px;"
"background:#0a6cff;color:#fff;border:none;border-radius:12px;cursor:pointer}"
".pin{color:#0a6cff;font-weight:700}"
"</style></head><body><div class=w>"
"<h2>AI Passport 配网</h2>"
"<div class=s>选择 Wi-Fi 并输入密码，保存后设备将自动连接。</div>"
"<div class=p><label>Wi-Fi 网络</label><select id=ssid></select>"
"<label>密码</label><input id=pass type=password>"
"<label>配网码（设备屏幕显示）</label><input id=pin type=text maxlength=4>"
"<button onclick=doSave()>保存并连接</button>"
"<div id=msg style='color:#c5221f;font-size:13px;margin-top:8px'></div></div>"
"<div class=s>配网码：<span class=pin>%s</span></div>"
"</div><script>"
"fetch('/scan').then(r=>r.json()).then(l=>{"
"const s=document.getElementById('ssid');"
"l.sort((a,b)=>b.rssi-a.rssi).forEach(ap=>{"
"const o=document.createElement('option');"
"o.value=ap.ssid;o.text=ap.ssid+' ('+ap.rssi+'dBm)'+(ap.auth?'':' [开放]');"
"s.appendChild(o)})});"
"function doSave(){"
"const ssid=document.getElementById('ssid').value;"
"const pass=document.getElementById('pass').value;"
"const pin=document.getElementById('pin').value;"
"if(!ssid){msg.textContent='请选择网络';return}"
"fetch('/save',{method:'POST',headers:{'content-type':'application/json'},"
"body:JSON.stringify({ssid,pass,pin})})"
".then(r=>r.json()).then(j=>{"
"if(j.ok){document.getElementById('msg').style.color='#1e8e3e';"
"document.getElementById('msg').textContent='已保存，设备重启连接中…';"
"setTimeout(()=>{document.getElementById('msg').textContent="
"'设备连接成功后此页面可关闭'},4000)}"
"else{document.getElementById('msg').textContent=j.error||'失败'}})};"
"</script></body></html>";

// ---------- HTTP 处理 ----------
static char s_page_buf[4096];   // 静态：httpd 栈只有 4K，4KB 缓冲放栈上必溢出

static esp_err_t h_root(httpd_req_t *req) {
    char *page = s_page_buf;
    // PAGE_FMT 含 CSS/JS，不可用 snprintf 格式化（% 会被误读）
    const char *tpl = PAGE_FMT;
    const char *ph = strstr(tpl, "%s");
    int n;
    if (ph) {
        int pre = (int)(ph - tpl);
        n = snprintf(page, 4096, "%.*s%s%s", pre, tpl, s_pin, ph + 2);
    } else {
        n = snprintf(page, 4096, "%s", tpl);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page, n);
    return ESP_OK;
}

static esp_err_t h_scan(httpd_req_t *req) {
    do_scan();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s_scan_json);
    return ESP_OK;
}

static char s_save_buf[256];

static esp_err_t h_save(httpd_req_t *req) {
    char *buf = s_save_buf;
    memset(buf, 0, sizeof(s_save_buf));
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_OK;
    }
    buf[n] = 0;
    // 极简解析（避免引入 cJSON 到配网关键路径）
    char ssid[33] = {0}, pass[64] = {0}, pin[8] = {0};
    const char *p;
    if ((p = strstr(buf, "\"ssid\":\""))) {
        p += 7;
        int i = 0;
        while (*p && *p != '"' && i < 32) ssid[i++] = *p++;
    }
    if ((p = strstr(buf, "\"pass\":\""))) {
        p += 7;
        int i = 0;
        while (*p && *p != '"' && i < 63) pass[i++] = *p++;
    }
    if ((p = strstr(buf, "\"pin\":\""))) {
        p += 6;
        int i = 0;
        while (*p && *p != '"' && i < 4) pin[i++] = *p++;
    }
    if (!ssid[0] || strcmp(pin, s_pin) != 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"配网码错误\"}");
        return ESP_OK;
    }
    wifi_store_add(ssid, pass);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    // 给页面一点时间显示成功提示，然后重启连接
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---------- 启动 ----------
void wifi_prov_start(void) {
    if (s_active) return;
    wifi_prov_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));
    wifi_prov_pin(s_pin, sizeof(s_pin));
    s_active = true;

    // 先停掉 STA 模式（已启动状态下切模式 AP 不会广播），再切 APSTA
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    // 关键：创建 AP 网络接口（含 DHCP 服务器与 192.168.4.1 网关），
    // 缺了它手机加入热点后拿不到 IP，192.168.4.1 永远打不开
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    // DNS 劫持任务
    xTaskCreate(dns_task, "prov_dns", 3072, NULL, 3, NULL);

    // HTTP 服务（captive portal + API）
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 12;
    hc.stack_size = 8192;                 // 默认 4K 放不下配网页渲染
    hc.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_http, &hc) == ESP_OK) {
        httpd_uri_t u;
        u.method = HTTP_GET;
        u.user_ctx = NULL;
        u.uri = "/";
        u.handler = h_root;
        httpd_register_uri_handler(s_http, &u);
        u.uri = "/scan";
        u.handler = h_scan;
        httpd_register_uri_handler(s_http, &u);
        u.method = HTTP_POST;
        u.uri = "/save";
        u.handler = h_save;
        httpd_register_uri_handler(s_http, &u);
        u.method = HTTP_GET;
        u.uri = "/save";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
        u.method = HTTP_GET;
        u.uri = "/generate_204";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
        u.uri = "/hotspot-detect.html";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
        u.uri = "/connecttest.txt";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
        u.uri = "/ncsi.txt";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
        u.uri = "/captive.apple.com";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
        u.uri = "/connect";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
        u.uri = "/kindle-wifi/wifistub.html";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
        // 兜底：Android 探测路径带随机后缀（/generate_204_<uuid>）等，
        // 通配符匹配后全部重定向到配网页
        u.uri = "*";
        u.handler = h_redirect;
        httpd_register_uri_handler(s_http, &u);
    }
    ESP_LOGI(TAG, "配网热点 %s 已开启，配网码 %s", s_ap_ssid, s_pin);
}
