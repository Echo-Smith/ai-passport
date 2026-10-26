# 凭据来源（单一决策，避免增量构建残留旧头）：
#   1) 四个环境变量都给了 → 用环境变量
#   2) 否则 ~/.config/passport/wifi_cred.h 存在 → 整份复制
#   3) 否则模拟器占位值
set(AP_WIFI_SSID "$ENV{WIFI_SSID}")
set(AP_WIFI_PASS "$ENV{WIFI_PASS}")
set(AP_BRIDGE_HOST "$ENV{BRIDGE_HOST}")
set(AP_BRIDGE_TOKEN "$ENV{BRIDGE_TOKEN}")
set(AP_CRED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/wifi_cred_gen.h")
set(AP_SAVED_CRED "$ENV{HOME}/.config/passport/wifi_cred.h")

if(NOT AP_WIFI_SSID STREQUAL "")
    set(AP_CRED_SRC "env")
    # 每次都重写，保证与本次环境变量一致（增量构建不残留）
    file(WRITE "${AP_CRED_HEADER}"
"#pragma once\n"
"#define WIFI_SSID \"${AP_WIFI_SSID}\"\n"
"#define WIFI_PASS \"${AP_WIFI_PASS}\"\n"
"#define BRIDGE_HOST \"${AP_BRIDGE_HOST}\"\n"
"#define BRIDGE_PORT 7788\n"
"#define BRIDGE_TOKEN \"${AP_BRIDGE_TOKEN}\"\n")
elseif(EXISTS "${AP_SAVED_CRED}")
    set(AP_CRED_SRC "saved-file")
    configure_file("${AP_SAVED_CRED}" "${AP_CRED_HEADER}" COPYONLY)
else()
    set(AP_CRED_SRC "emulator-fallback")
    file(WRITE "${AP_CRED_HEADER}"
"#pragma once\n"
"#define WIFI_SSID \"Emulator Host Bridge\"\n"
"#define WIFI_PASS \"\"\n"
"#define BRIDGE_HOST \"192.168.3.168\"\n"
"#define BRIDGE_PORT 7788\n"
"#define BRIDGE_TOKEN \"test123\"\n")
endif()

# 自检：token 为空时报错而不是静默发空 token
file(READ "${AP_CRED_HEADER}" AP_CRED_CONTENT)
if(AP_CRED_CONTENT MATCHES "BRIDGE_TOKEN \"\"")
    message(FATAL_ERROR "approval: BRIDGE_TOKEN 为空（来源=${AP_CRED_SRC}）——检查环境变量或 ${AP_SAVED_CRED}")
endif()
message(STATUS "approval: credentials from ${AP_CRED_SRC}")

idf_component_register(
    SRCS "approval_main.c" "approval_ui.c" "app_font_16.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash json lwip
)
