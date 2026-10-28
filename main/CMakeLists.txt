# Wi-Fi 凭证头（wifi_cred_gen.h）在 configure 阶段从以下来源生成（按优先级）：
#   1) 环境变量 WIFI_SSID/WIFI_PASS/BRIDGE_HOST/BRIDGE_TOKEN
#      （改环境变量后需 idf.py reconfigure 才生效）
#   2) 无环境变量时回退 ~/.config/passport/wifi_cred.h（本机保护文件，真机部署用）
#   3) 均无时用模拟器占位值（Emulator Host Bridge，无 token）
set(AP_WIFI_SSID "$ENV{WIFI_SSID}")
set(AP_WIFI_PASS "$ENV{WIFI_PASS}")
set(AP_BRIDGE_HOST "$ENV{BRIDGE_HOST}")
set(AP_BRIDGE_TOKEN "$ENV{BRIDGE_TOKEN}")
set(AP_CRED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/wifi_cred_gen.h")
set(AP_SAVED_CRED "$ENV{HOME}/.config/passport/wifi_cred.h")

if(NOT AP_WIFI_SSID STREQUAL "")
    set(AP_CRED_SRC "env")
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
file(READ "${AP_CRED_HEADER}" AP_CRED_CHECK)
if(AP_CRED_CHECK MATCHES "BRIDGE_TOKEN \"\"")
    message(FATAL_ERROR "wifi_cred: BRIDGE_TOKEN 为空（来源=${AP_CRED_SRC}）")
endif()
message(STATUS "approval: credentials from ${AP_CRED_SRC}")

idf_component_register(
    SRCS "approval_main.c" "approval_ui.c" "app_font_16.c" "wifi_store.c" "wifi_provision.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash json lwip esp_http_server dns_server
)
