# 凭据仅来自构建环境变量（回退为模拟器占位值），生成头文件不进源码树。
set(AP_WIFI_SSID "$ENV{WIFI_SSID}")
set(AP_WIFI_PASS "$ENV{WIFI_PASS}")
set(AP_BRIDGE_HOST "$ENV{BRIDGE_HOST}")
set(AP_BRIDGE_TOKEN "$ENV{BRIDGE_TOKEN}")
if(AP_WIFI_SSID STREQUAL "")
    set(AP_WIFI_SSID "Emulator Host Bridge")
endif()
if(AP_BRIDGE_HOST STREQUAL "")
    set(AP_BRIDGE_HOST "192.168.3.168")
endif()
if(AP_BRIDGE_TOKEN STREQUAL "")
    set(AP_BRIDGE_TOKEN "test123")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wifi_cred_gen.h"
"#pragma once\n"
"#define WIFI_SSID \"${AP_WIFI_SSID}\"\n"
"#define WIFI_PASS \"${AP_WIFI_PASS}\"\n"
"#define BRIDGE_HOST \"${AP_BRIDGE_HOST}\"\n"
"#define BRIDGE_PORT 7788\n"
"#define BRIDGE_TOKEN \"${AP_BRIDGE_TOKEN}\"\n")

idf_component_register(
    SRCS "approval_main.c" "approval_ui.c" "app_font_16.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash json lwip
)
