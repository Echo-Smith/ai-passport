// main/approval_ui.h —— 功能机看板 UI：页面栈 + 覆盖弹窗层。
#pragma once

#include "bsp_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

// net → ui 的显示消息（队列元素）
typedef enum {
    UI_MSG_STATUS,     // 状态文本变化（含连接状态）
    UI_MSG_REQUEST,    // 审批请求（覆盖弹窗）
    UI_MSG_NOTIFY,     // 活动通知（顶部横条，TTL 自动消失）
    UI_MSG_CANCEL,     // 清除覆盖层（桥取消/断线）
} ui_msg_kind_t;

typedef struct {
    ui_msg_kind_t kind;
    char id[24];
    char agent[16];
    char title[64];
    char detail[160];
    int timeout_s;     // request: 审批超时; notify: 显示 ttl
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

// 时钟数据注入（SNTP 同步后调用；时间字符串由 UI 自行刷新）
void approval_ui_set_time(int hour, int minute, int wday, int month, int day);
