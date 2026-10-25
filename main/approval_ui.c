// main/approval_ui.c —— 功能机看板 UI（终端极简 × liquid-glass 转译）。
//
// 视觉体系（240×320 RGB565、圆角 30px 危险区、内容区 y≈36..284）：
//   - 浅色底 (0xF4F5F7) + 统一表面 + 发丝线分隔（rgba0,0,0,7% → RGB565 0xE2E3E6）
//   - 层级靠字号/灰阶：标题黑 0x14161A、正文 0x3A3E45、次级 0x8A8F98
//   - 单强调蓝 0x0A6CFF，仅选中态/agent 标签/连接点；语义色仅 allow 绿/deny 红
//   - "玻璃"只用于覆盖层：深色半透明蒙层 + 中央浅色圆角面板
// 动效仅一种：按键决定后的确认闪烁（整块变色，低刷友好）；无滑入。
//
// 线程纪律：全部 LVGL 操作发生在 pump()（LVGL 定时器上下文）。
#include "approval_ui.h"
#include "bsp_battery.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "approval_ui";

// ---- 调色板（RGB565 由 lv_color_hex 转换） ----
#define COL_GROUND   0xF4F5F7
#define COL_PANEL    0xFFFFFF
#define COL_HAIR     0xE2E3E6
#define COL_INK      0x14161A
#define COL_BODY     0x3A3E45
#define COL_SUB      0x8A8F98
#define COL_ACCENT   0x0A6CFF
#define COL_ALLOW    0x1E8E3E
#define COL_DENY     0xC5221F
#define COL_VEIL     0x0B0D12

LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);

// ---- 状态 ----
typedef enum { PG_HOME, PG_MENU, PG_ACTIVITY, PG_AGENTS, PG_ABOUT } page_t;
typedef enum { OV_NONE, OV_REQ_P1, OV_REQ_P2, OV_NOTIFY, OV_FLASH } overlay_t;

static QueueHandle_t s_decision_q;
static QueueHandle_t s_msg_q;
static QueueHandle_t s_key_q;

typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } key_evt_t;

// 活动环形缓冲（notify 入栈）
#define ACTIVITY_MAX 10
typedef struct { char agent[16]; char model[40]; char at[24]; } activity_t;
static activity_t s_activity[ACTIVITY_MAX];
static int s_activity_count;

// 时钟
static int s_hour = -1, s_min, s_wday, s_month, s_day;

// 覆盖层当前请求
static char s_req_id[24];
static char s_req_agent[16];
static char s_req_title[64];
static char s_req_detail[160];
static int64_t s_notify_until_us;

// ---- 对象引用 ----
static lv_obj_t *scr;
// 状态栏
static lv_obj_t *dot_link, *lbl_batt;
// HOME
static lv_obj_t *lbl_clock, *lbl_date, *lbl_home_hint;
// MENU / 页面共用容器
static lv_obj_t *page_cont;          // 当前子页容器
static page_t s_page = PG_HOME;
static int s_menu_sel;
// 覆盖层
static lv_obj_t *veil, *ov_panel;    // 蒙层 + 面板
static lv_obj_t *ov_tag, *ov_title, *ov_detail, *ov_hint, *ov_page_dots;
static overlay_t s_overlay = OV_NONE;
// 闪烁
static int s_flash_state;            // 0..5 计数，奇偶切换颜色
static bool s_flash_allow;
static int64_t s_flash_next_us;

static const char *const WDAY_CN[7] =
    { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

// ======================================================================
// 小工具
// ======================================================================
static void style_reset(lv_obj_t *o) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color, int x, int y, int w) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    return l;
}

// ======================================================================
// HOME 页
// ======================================================================
static void build_home(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_GROUND), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 状态栏：左连接点 + 右电量（电量右上为仓库规范默认位）
    dot_link = lv_obj_create(scr);
    style_reset(dot_link);
    lv_obj_set_size(dot_link, 8, 8);
    lv_obj_set_style_radius(dot_link, 4, 0);
    lv_obj_set_style_bg_opa(dot_link, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot_link, lv_color_hex(COL_SUB), 0);
    lv_obj_set_pos(dot_link, 24, 18);

    lbl_batt = make_label(scr, &lv_font_source_han_sans_sc_16_cjk, COL_SUB,
                          150, 10, 70);
    lv_obj_set_style_text_align(lbl_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lbl_batt, "--%");

    // 中央大时钟（等宽数字观感：Montserrat 48 + 手动两位补零）
    lbl_clock = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_text_align(lbl_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_clock, 240);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_set_pos(lbl_clock, 0, 108);

    // 日期（中文 16px）
    lbl_date = make_label(scr, &lv_font_source_han_sans_sc_16_cjk, COL_SUB,
                          0, 172, 240);
    lv_obj_set_style_text_align(lbl_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_date, "时间同步中");

    // 底部最近活动/等待提示
    lbl_home_hint = make_label(scr, &lv_font_source_han_sans_sc_16_cjk, COL_SUB,
                               24, 250, 192);
    lv_label_set_text(lbl_home_hint, "OK 进入菜单");
}

// ======================================================================
// 菜单与子页（统一面板 + 发丝线）
// ======================================================================
static const char *const MENU_ITEMS[] = { "Magpie 活动", "Agent 统计", "关于" };
#define MENU_COUNT 3

static lv_obj_t *menu_list;

static void open_page_container(void) {
    if (page_cont) lv_obj_delete(page_cont);
    page_cont = lv_obj_create(scr);
    style_reset(page_cont);
    lv_obj_set_size(page_cont, 208, 248);
    lv_obj_set_pos(page_cont, 16, 36);
    lv_obj_set_style_bg_color(page_cont, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(page_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(page_cont, 14, 0);
    lv_obj_set_style_border_width(page_cont, 0, 0);
    lv_obj_clear_flag(page_cont, LV_OBJ_FLAG_SCROLLABLE);
}

static void menu_refresh(void) {
    lv_obj_clean(menu_list);
    for (int i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(menu_list);
        style_reset(row);
        lv_obj_set_size(row, 208, 62);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        if (i == s_menu_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0xEAF2FF), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }
        lv_obj_t *dot = lv_obj_create(row);
        style_reset(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i == s_menu_sel ? COL_ACCENT : COL_HAIR), 0);
        lv_obj_set_pos(dot, 14, 28);
        lv_obj_t *t = make_label(row, &lv_font_source_han_sans_sc_16_cjk,
                                 i == s_menu_sel ? COL_INK : COL_BODY, 30, 23, 160);
        lv_label_set_text(t, MENU_ITEMS[i]);
    }
}

static void show_menu(void) {
    s_page = PG_MENU;
    open_page_container();
    lv_obj_t *cap = make_label(page_cont, &lv_font_source_han_sans_sc_16_cjk,
                               COL_SUB, 16, 10, 180);
    lv_label_set_text(cap, "菜单");
    menu_list = lv_obj_create(page_cont);
    style_reset(menu_list);
    lv_obj_set_size(menu_list, 208, 200);
    lv_obj_set_pos(menu_list, 0, 40);
    lv_obj_clear_flag(menu_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(menu_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(menu_list, 6, 0);
    menu_refresh();
}

static void show_activity_page(void) {
    s_page = PG_ACTIVITY;
    open_page_container();
    lv_obj_t *cap = make_label(page_cont, &lv_font_source_han_sans_sc_16_cjk,
                               COL_SUB, 16, 10, 180);
    lv_label_set_text(cap, "Magpie 最近活动");
    lv_obj_t *list = lv_obj_create(page_cont);
    style_reset(list);
    lv_obj_set_size(list, 208, 226);
    lv_obj_set_pos(list, 0, 22);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    int n = s_activity_count < ACTIVITY_MAX ? s_activity_count : ACTIVITY_MAX;
    if (n == 0) {
        lv_obj_t *e = make_label(list, &lv_font_source_han_sans_sc_16_cjk,
                                 COL_SUB, 12, 8, 180);
        lv_label_set_text(e, "暂无活动（等待 Agent 流量）");
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(list);
        style_reset(row);
        lv_obj_set_size(row, 184, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_bottom(row, 10, 0);
        if (i > 0) {
            lv_obj_t *line = lv_obj_create(row);
            style_reset(line);
            lv_obj_set_size(line, 184, 1);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(line, lv_color_hex(COL_HAIR), 0);
            lv_obj_set_pos(line, 0, 0);
        }
        // 发丝线
        char head[64];
        snprintf(head, sizeof(head), "%s  %s", s_activity[i].at,
                 s_activity[i].agent);
        lv_obj_t *h = make_label(row, &lv_font_source_han_sans_sc_16_cjk,
                                 COL_BODY, 0, 10, 184);
        lv_label_set_text(h, head);
        lv_obj_t *m = make_label(row, &lv_font_montserrat_14, COL_SUB, 0, 32, 184);
        lv_label_set_text(m, s_activity[i].model);
    }
}

static void show_agents_page(void) {
    s_page = PG_AGENTS;
    open_page_container();
    lv_obj_t *cap = make_label(page_cont, &lv_font_source_han_sans_sc_16_cjk,
                               COL_SUB, 16, 10, 180);
    lv_label_set_text(cap, "Agent 活动统计");
    // 计数
    struct { char name[16]; int count; } stats[6];
    int nstats = 0;
    for (int i = 0; i < s_activity_count && i < ACTIVITY_MAX; i++) {
        int f = -1;
        for (int k = 0; k < nstats; k++) {
            if (strcmp(stats[k].name, s_activity[i].agent) == 0) { f = k; break; }
        }
        if (f < 0 && nstats < 6) {
            strlcpy(stats[nstats].name, s_activity[i].agent, 16);
            stats[nstats].count = 1;
            nstats++;
        } else if (f >= 0) stats[f].count++;
    }
    if (nstats == 0) {
        lv_obj_t *e = make_label(page_cont, &lv_font_source_han_sans_sc_16_cjk,
                                 COL_SUB, 16, 40, 180);
        lv_label_set_text(e, "暂无统计数据");
        return;
    }
    int y = 40;
    for (int k = 0; k < nstats; k++) {
        char line[48];
        snprintf(line, sizeof(line), "%s   %d 次", stats[k].name, stats[k].count);
        lv_obj_t *l = make_label(page_cont, &lv_font_source_han_sans_sc_16_cjk,
                                 COL_BODY, 16, y, 180);
        lv_label_set_text(l, line);
        y += 34;
    }
}

static void show_about_page(void) {
    s_page = PG_ABOUT;
    open_page_container();
    lv_obj_t *cap = make_label(page_cont, &lv_font_source_han_sans_sc_16_cjk,
                               COL_SUB, 16, 10, 180);
    lv_label_set_text(cap, "关于");
    const char *rows[] = {
        "Approval Remote v0.2",
        "固件: feature/approval-remote",
        "桥: 见 Mac approval-bridge",
        "电源键=硬件开关",
        NULL,
    };
    int y = 40;
    for (int i = 0; rows[i]; i++) {
        lv_obj_t *l = make_label(page_cont, &lv_font_source_han_sans_sc_16_cjk,
                                 COL_BODY, 16, y, 184);
        lv_label_set_text(l, rows[i]);
        y += 30;
    }
}

static void page_back_home(void) {
    s_page = PG_HOME;
    if (page_cont) { lv_obj_delete(page_cont); page_cont = NULL; }
    menu_list = NULL;
}

// ======================================================================
// 覆盖层（玻璃转译：深色蒙层 + 浅色面板）
// ======================================================================
static void build_overlay(void) {
    veil = lv_obj_create(scr);
    lv_obj_set_pos(veil, 0, 0);
    lv_obj_set_size(veil, 240, 320);
    lv_obj_set_style_bg_color(veil, lv_color_hex(COL_VEIL), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);
    lv_obj_set_style_border_width(veil, 0, 0);
    lv_obj_set_style_radius(veil, 0, 0);
    lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);

    ov_panel = lv_obj_create(veil);
    style_reset(ov_panel);
    lv_obj_set_size(ov_panel, 208, 208);
    lv_obj_set_pos(ov_panel, 16, 56);
    lv_obj_set_style_bg_color(ov_panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(ov_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ov_panel, 16, 0);
    lv_obj_set_style_border_width(ov_panel, 2, 0);
    lv_obj_set_style_border_color(ov_panel, lv_color_hex(COL_HAIR), 0);
    lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);

    ov_tag = make_label(ov_panel, &lv_font_montserrat_14, COL_ACCENT, 14, 12, 180);
    ov_title = make_label(ov_panel, &lv_font_source_han_sans_sc_16_cjk, COL_INK,
                          16, 34, 176);
    ov_detail = make_label(ov_panel, &lv_font_montserrat_14, COL_BODY,
                           16, 62, 176);
    lv_label_set_long_mode(ov_detail, LV_LABEL_LONG_WRAP);
    ov_page_dots = make_label(ov_panel, &lv_font_source_han_sans_sc_16_cjk,
                              COL_SUB, 16, 156, 176);
    lv_label_set_text(ov_page_dots, "1/2");
    ov_hint = make_label(ov_panel, &lv_font_source_han_sans_sc_16_cjk, COL_BODY,
                         16, 184, 176);
}

static void overlay_show(bool show) {
    if (show) {
        lv_obj_clear_flag(veil, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);
        s_overlay = OV_NONE;
    }
}

static void show_request_p1(void) {
    s_overlay = OV_REQ_P1;
    lv_label_set_text_fmt(ov_tag, "%s", s_req_agent);
    lv_label_set_text_fmt(ov_title, "请求确认");
    // 摘要：命令首行（60 字符内）
    char brief[64];
    strlcpy(brief, s_req_title, sizeof(brief));
    lv_label_set_text_fmt(ov_detail, "%.60s", brief);
    lv_obj_set_style_text_color(ov_detail, lv_color_hex(COL_BODY), 0);
    lv_label_set_text(ov_page_dots, "1/2");
    lv_label_set_text(ov_hint, "OK 详情  DOWN 拒绝");
    overlay_show(true);
}

static void show_request_p2(void) {
    s_overlay = OV_REQ_P2;
    lv_label_set_text_fmt(ov_tag, "%s", s_req_agent);
    lv_label_set_text_fmt(ov_title, "完整命令");
    lv_label_set_text_fmt(ov_detail, "%.150s", s_req_detail[0] ? s_req_detail
                                                               : s_req_title);
    lv_label_set_text(ov_page_dots, "2/2");
    lv_label_set_text(ov_hint, "OK 允许  DOWN 拒绝");
    overlay_show(true);
}

static void show_notify_overlay(const ui_msg_t *m) {
    s_overlay = OV_NOTIFY;
    lv_label_set_text_fmt(ov_tag, "%s", m->agent);
    lv_label_set_text_fmt(ov_title, "活动");
    lv_label_set_text_fmt(ov_detail, "%.60s", m->title);
    lv_label_set_text(ov_page_dots, "");
    lv_obj_set_style_text_color(ov_detail, lv_color_hex(COL_SUB), 0);
    lv_label_set_text(ov_hint, "任意键 关闭");
    overlay_show(true);
    s_notify_until_us = esp_timer_get_time() + (int64_t)m->timeout_s * 1000000;
}

// 决定后的确认闪烁（边框+底色，3 次 150ms）
static void start_flash(bool allow) {
    s_overlay = OV_FLASH;
    s_flash_allow = allow;
    s_flash_state = 6;               // 3 次亮暗
    s_flash_next_us = esp_timer_get_time() + 150 * 1000;
    lv_obj_set_style_bg_color(veil, lv_color_hex(allow ? COL_ALLOW : COL_DENY), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_40, 0);
    lv_label_set_text(ov_hint, allow ? "已允许" : "已拒绝");
    lv_obj_set_style_text_color(ov_hint,
        lv_color_hex(allow ? COL_ALLOW : COL_DENY), 0);
}

static void pump_flash(void) {
    if (!s_flash_state) return;
    if (esp_timer_get_time() < s_flash_next_us) return;
    s_flash_next_us = esp_timer_get_time() + 150 * 1000;
    s_flash_state--;
    if (s_flash_state % 2 == 0) {
        lv_obj_set_style_bg_opa(veil, LV_OPA_40, 0);   // 亮
    } else {
        lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);   // 暗
    }
    if (!s_flash_state) {
        lv_obj_set_style_bg_color(veil, lv_color_hex(COL_VEIL), 0);
        lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);
        lv_obj_set_style_text_color(ov_hint, lv_color_hex(COL_BODY), 0);
        overlay_show(false);
        lv_screen_load(scr);
    }
}

// ======================================================================
// 决定出口
// ======================================================================
static void emit_decision(bool allow) {
    ui_decision_t d = { .allow = allow };
    strlcpy(d.id, s_req_id, sizeof(d.id));
    if (s_decision_q) (void)xQueueSend(s_decision_q, &d, 0);
    start_flash(allow);
}

// ======================================================================
// 活动环形缓冲
// ======================================================================
static void activity_push(const ui_msg_t *m) {
    activity_t *a = &s_activity[ACTIVITY_MAX - 1];
    // 右移
    for (int i = ACTIVITY_MAX - 1; i > 0; i--) s_activity[i] = s_activity[i - 1];
    strlcpy(a->agent, m->agent, sizeof(a->agent));
    // title 形如 "codex / DeepSeek-V4.1"，取模型段
    const char *sep = strstr(m->title, " / ");
    strlcpy(a->model, sep ? sep + 3 : m->title, sizeof(a->model));
    if (s_hour >= 0) {
        snprintf(a->at, sizeof(a->at), "%02d:%02d", s_hour, s_min);
    } else {
        strlcpy(a->at, "--:--", sizeof(a->at));
    }
    if (s_activity_count < ACTIVITY_MAX) s_activity_count++;
}

// ======================================================================
// 按键处理（pump 上下文）
// ======================================================================
static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_RELEASE) return;

    // 覆盖层优先
    if (s_overlay == OV_REQ_P1 && ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_OK) show_request_p2();
        else if (btn == BSP_BTN_DOWN) emit_decision(false);
        return;
    }
    if (s_overlay == OV_REQ_P2 && ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_OK) emit_decision(true);
        else if (btn == BSP_BTN_DOWN) emit_decision(false);
        else if (btn == BSP_BTN_UP) show_request_p1();
        return;
    }
    if (s_overlay == OV_NOTIFY && ev == BSP_BTN_CLICK) {
        overlay_show(false);
        return;
    }
    if (s_overlay == OV_FLASH) return;   // 闪烁期间不响应

    // 页面栈
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK && s_page != PG_HOME) {
        if (s_page == PG_MENU) page_back_home();
        else show_menu();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (s_page == PG_HOME) {
        if (btn == BSP_BTN_OK) show_menu();
    } else if (s_page == PG_MENU) {
        if (btn == BSP_BTN_UP) { s_menu_sel = (s_menu_sel + MENU_COUNT - 1) % MENU_COUNT; menu_refresh(); }
        else if (btn == BSP_BTN_DOWN) { s_menu_sel = (s_menu_sel + 1) % MENU_COUNT; menu_refresh(); }
        else if (btn == BSP_BTN_OK) {
            switch (s_menu_sel) {
                case 0: show_activity_page(); break;
                case 1: show_agents_page(); break;
                case 2: show_about_page(); break;
            }
        }
    } else if (s_page == PG_ACTIVITY) {
        // 滚动由容器自带；这里留钩子
    }
}

// ======================================================================
// 消息处理（pump 上下文）
// ======================================================================
static void handle_msg(const ui_msg_t *m) {
    switch (m->kind) {
        case UI_MSG_REQUEST:
            strlcpy(s_req_id, m->id, sizeof(s_req_id));
            strlcpy(s_req_agent, m->agent, sizeof(s_req_agent));
            strlcpy(s_req_title, m->title, sizeof(s_req_title));
            strlcpy(s_req_detail, m->detail, sizeof(s_req_detail));
            show_request_p1();
            break;
        case UI_MSG_NOTIFY:
            activity_push(m);
            if (s_overlay == OV_NONE || s_overlay == OV_NOTIFY) {
                show_notify_overlay(m);
            } else {
                // 审批/闪烁中：只入活动缓冲不弹层
            }
            break;
        case UI_MSG_CANCEL:
            if (s_overlay == OV_REQ_P1 || s_overlay == OV_REQ_P2
                || s_overlay == OV_NOTIFY) {
                overlay_show(false);
            }
            break;
        case UI_MSG_STATUS:
            // 连接状态点：灰 = 未连桥，绿 = 已连
            lv_obj_set_style_bg_color(dot_link,
                lv_color_hex(strstr(m->title, "up") ? COL_ALLOW : COL_SUB), 0);
            break;
    }
}

// ======================================================================
// 时钟与电量刷新
// ======================================================================
static void refresh_clock(void) {
    if (s_hour < 0) return;
    lv_label_set_text_fmt(lbl_clock, "%02d:%02d", s_hour, s_min);
    lv_label_set_text_fmt(lbl_date, "%s  %d月%d日", WDAY_CN[s_wday % 7],
                          s_month, s_day);
}

static void refresh_battery(void) {
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(lbl_batt, "--%");
        lv_obj_set_style_text_color(lbl_batt, lv_color_hex(COL_SUB), 0);
    } else {
        lv_label_set_text_fmt(lbl_batt, "%d%%", soc);
        lv_obj_set_style_text_color(lbl_batt,
            lv_color_hex(soc <= 20 ? COL_DENY : COL_BODY), 0);
    }
}

// ======================================================================
// 公共接口
// ======================================================================
void approval_ui_init(QueueHandle_t decision_out) {
    s_decision_q = decision_out;
    s_msg_q = xQueueCreate(8, sizeof(ui_msg_t));
    s_key_q = xQueueCreate(8, sizeof(key_evt_t));
    build_home();
    build_overlay();
    lv_screen_load(scr);
}

void approval_ui_post(const ui_msg_t *msg) {
    if (s_msg_q) (void)xQueueSend(s_msg_q, msg, 0);
}

void approval_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    key_evt_t k = { .btn = btn, .ev = ev };
    if (s_key_q) (void)xQueueSend(s_key_q, &k, 0);
}

void approval_ui_pump(void) {
    static int64_t last_sec = -1;
    ui_msg_t m;
    while (s_msg_q && xQueueReceive(s_msg_q, &m, 0) == pdTRUE) handle_msg(&m);
    key_evt_t k;
    while (s_key_q && xQueueReceive(s_key_q, &k, 0) == pdTRUE) handle_key(k.btn, k.ev);
    pump_flash();
    // 通知 TTL
    if (s_overlay == OV_NOTIFY && s_notify_until_us
        && esp_timer_get_time() >= s_notify_until_us) {
        overlay_show(false);
        s_notify_until_us = 0;
    }
    // 每秒刷新时钟/电量
    int64_t now = esp_timer_get_time() / 1000000;
    if (now != last_sec) {
        last_sec = now;
        refresh_clock();
        refresh_battery();
    }
}

void approval_ui_set_time(int hour, int minute, int wday, int month, int day) {
    s_hour = hour; s_min = minute; s_wday = wday; s_month = month; s_day = day;
}
