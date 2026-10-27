// main/wifi_store.h —— 多组 Wi-Fi 凭证的 NVS 存取（最多 6 组，按 RSSI 择优连接）。
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define WIFI_STORE_MAX 6

typedef struct {
    char ssid[33];
    char pass[64];
    int  rssi;               // 最近一次扫描匹配到的信号（-1000=未知）
} wifi_net_t;

// 从 NVS 装入内存（wifi_init 前调用一次）
esp_err_t wifi_store_load(void);
int       wifi_store_count(void);
const wifi_net_t *wifi_store_get(int idx);
// 添加或更新（同 SSID 覆盖密码）；满 6 组时替换 RSSI 最差的
esp_err_t wifi_store_add(const char *ssid, const char *pass);
esp_err_t wifi_store_remove(const char *ssid);
