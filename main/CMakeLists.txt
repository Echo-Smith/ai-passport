# 凭据来源优先级：环境变量 → ~/.config/passport/wifi_cred.h（本机保护文件）
# → 模拟器占位值。生成头文件不进源码树。
set(AP_WIFI_SSID "$ENV{WIFI_SSID}")
set(AP_WIFI_PASS "$ENV{WIFI_PASS}")
set(AP_BRIDGE_HOST "$ENV{BRIDGE_HOST}")
set(AP_BRIDGE_TOKEN "$ENV{BRIDGE_TOKEN}")
set(AP_CRED_SRC "env")
if(AP_WIFI_SSID STREQUAL "")
    set(AP_SAVED_CRED "$ENV{HOME}/.config/passport/wifi_cred.h")
    if(EXISTS "${AP_SAVED_CRED}")
        configure_file("${AP_SAVED_CRED}"
                       "${CMAKE_CURRENT_BINARY_DIR}/wifi_cred_gen.h" COPYONLY)
        set(AP_CRED_SRC "saved-file")
    else()
        set(AP_WIFI_SSID "Emulator Host Bridge")
        set(AP_BRIDGE_HOST "192.168.3.168")
        set(AP_BRIDGE_TOKEN "test123")
        set(AP_CRED_SRC "emulator-fallback")
    endif()
endif()
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/wifi_cred_gen.h")
    if(AP_WIFI_PASS STREQUAL "")
        set(AP_WIFI_PASS "")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wifi_cred_gen.h"
"#pragma once\n"
"#define WIFI_SSID \"${AP_WIFI_SSID}\"\n"
"#define WIFI_PASS \"${AP_WIFI_PASS}\"\n"
"#define BRIDGE_HOST \"${AP_BRIDGE_HOST}\"\n"
"#define BRIDGE_PORT 7788\n"
"#define BRIDGE_TOKEN \"${AP_BRIDGE_TOKEN}\"\n")
endif()
message(STATUS "approval: credentials from ${AP_CRED_SRC}")

idf_component_register(
    SRCS "approval_main.c" "approval_ui.c" "app_font_16.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash json lwip
)
