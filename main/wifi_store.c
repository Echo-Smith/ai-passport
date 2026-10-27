// main/wifi_store.c —— 多组 Wi-Fi 凭证存 NVS（namespace "wificfg"）。
// 结构：count(u8) + ssid{i}/pass{i}(string, i=0..5)。add 时同 SSID 覆盖。
#include "wifi_store.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_store";
static const char *NS = "wificfg";

static wifi_net_t s_nets[WIFI_STORE_MAX];
static int s_count;

esp_err_t wifi_store_load(void) {
    nvs_handle_t h;
    s_count = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "无已存 Wi-Fi（首次使用）");
        return ESP_OK;   // 空列表不是错误
    }
    uint8_t n = 0;
    nvs_get_u8(h, "count", &n);
    if (n > WIFI_STORE_MAX) n = WIFI_STORE_MAX;
    for (int i = 0; i < n; i++) {
        char key[16];
        size_t sl = sizeof(s_nets[i].ssid), pl = sizeof(s_nets[i].pass);
        snprintf(key, sizeof(key), "ssid%d", i);
        if (nvs_get_str(h, key, s_nets[i].ssid, &sl) != ESP_OK) continue;
        snprintf(key, sizeof(key), "pass%d", i);
        if (nvs_get_str(h, key, s_nets[i].pass, &pl) != ESP_OK) s_nets[i].pass[0] = 0;
        s_count++;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "已存 Wi-Fi %d 组", s_count);
    return ESP_OK;
}

int wifi_store_count(void) { return s_count; }

const wifi_net_t *wifi_store_get(int idx) {
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_nets[idx];
}

esp_err_t wifi_store_add(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0] || strlen(ssid) >= sizeof(s_nets[0].ssid)) return ESP_ERR_INVALID_ARG;
    // 同名覆盖
    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_count < WIFI_STORE_MAX) slot = s_count++;
        else slot = WIFI_STORE_MAX - 1;   // 满：挤掉最后一组（最早加入的靠后序）
    }
    strlcpy(s_nets[slot].ssid, ssid, sizeof(s_nets[0].ssid));
    strlcpy(s_nets[slot].pass, pass ? pass : "", sizeof(s_nets[0].pass));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[16];
    nvs_set_u8(h, "count", (uint8_t)s_count);
    for (int i = 0; i < s_count; i++) {
        snprintf(key, sizeof(key), "ssid%d", i);
        nvs_set_str(h, key, s_nets[i].ssid);
        snprintf(key, sizeof(key), "pass%d", i);
        nvs_set_str(h, key, s_nets[i].pass);
    }
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "已保存 Wi-Fi: %s (%d/%d)", ssid, s_count, WIFI_STORE_MAX);
    return err;
}

esp_err_t wifi_store_remove(const char *ssid) {
    int found = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) { found = i; break; }
    }
    if (found < 0) return ESP_ERR_NOT_FOUND;
    for (int i = found; i < s_count - 1; i++) s_nets[i] = s_nets[i + 1];
    s_count--;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_ERR_NVS_BASE;
    char key[16];
    nvs_set_u8(h, "count", (uint8_t)s_count);
    for (int i = 0; i < s_count; i++) {
        snprintf(key, sizeof(key), "ssid%d", i);
        nvs_set_str(h, key, s_nets[i].ssid);
        snprintf(key, sizeof(key), "pass%d", i);
        nvs_set_str(h, key, s_nets[i].pass);
    }
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
