// main/wifi_provision.h —— 软 AP 配网： captive portal + 设备屏 QR。
#pragma once
#include <stdbool.h>
#include <stddef.h>

// SoftAP 名称（AI-Passport-XXXX，XXXX=MAC 后 3 字节）
void wifi_prov_ap_ssid(char *buf, size_t n);
// 配网码（4 位数字，来自 MAC；网页表单需填写以防旁邻误配）
void wifi_prov_pin(char *buf, size_t n);
// 启动软 AP + DNS + captive portal（在独立任务里调用；UI 先调 show_provision）
void wifi_prov_start(void);
bool wifi_prov_active(void);
