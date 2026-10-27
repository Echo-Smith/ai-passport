// main/approval_ui.h —— 功能机看板 UI：状态栏 + 页面栈 + 覆盖弹窗层。
#pragma once

#include "bsp_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

// net → ui 的显示消息（队列元素）
typedef enum {
    UI_MSG_STATUS,     // 连接状态（title = "up"/"down"）
    UI_MSG_REQUEST,    // 审批请求（覆盖弹窗）
    UI_MSG_NOTIFY,     // 活动通知（覆盖条，TTL 自动消失）
    UI_MSG_USAGE,      // Magpie 用量/额度（刷新用量页与首页摘要）
    UI_MSG_HEALTH,     // 推送健康（桥连接/设备数/待决列表）
    UI_MSG_SETTINGS,   // 网页下发的详细设置
    UI_MSG_PROVISION,  // 配网模式（title=AP名 detail=配网码）
    UI_MSG_CANCEL,     // 清除覆盖层（桥取消/断线）
} ui_msg_kind_t;

#define UI_QUOTA_MAX 3

typedef struct {
    ui_msg_kind_t kind;
    char id[24];
    char agent[16];
    char title[64];
    char detail[160];
    int timeout_s;          // request: 审批超时; notify: 显示 ttl
    // usage 专用
    char tokens[24];        // 形如 "12.3M"（桥侧已格式化）
    struct {
        char name[24];
        char left[24];
    } quotas[UI_QUOTA_MAX];
    int quota_n;
} ui_msg_t;

// ui → net 的决定（队列元素）
typedef struct {
    bool allow;
    char id[24];
} ui_decision_t;

// 初始化（在 LVGL 就绪后调用，内部建屏与定时器）。提供决定出口队列。
void approval_ui_init(QueueHandle_t decision_out);

// net 任务投递显示消息（任意上下文安全，仅入队）
void approval_ui_post(const ui_msg_t *msg);

// 按键投递（按键回调上下文安全，仅入队；UI 任务内消费）
void approval_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// UI 任务侧：消费消息与按键（在 LVGL 定时器里调用，已持锁）
void approval_ui_pump(void);

// 时钟数据注入（SNTP 同步后调用）
void approval_ui_set_time(int hour, int minute, int wday, int month, int day);

// Wi-Fi 信号注入（main 侧定时取 RSSI 后调用；<=-100 视为未知）
void approval_ui_set_unpaired(bool unpaired);
void approval_ui_set_rssi(int rssi_dbm);

// 注册「重新配网」回调（设置页触发；main 侧起配网任务）
void approval_ui_set_provision_cb(void (*fn)(void));
