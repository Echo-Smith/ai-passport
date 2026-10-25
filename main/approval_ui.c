// main/approval_ui.c —— 功能机看板 UI（终端极简 × liquid-glass 转译，双主题）。
//
// 布局总纲（240×320，圆角 30px 危险区）：
//   顶部状态栏（全局）：左=时间(HH:MM)，右=Wi-Fi 信号三格 + 电量百分比
//   中部内容区  y≈42..282：首页（喜鹊 + 日期 + 用量摘要）/ 菜单 / 子页（灰底白卡片）
//   底部提示条（全局）：当前页/当前弹窗的按键说明
//   覆盖层"玻璃"：深色蒙层 + 中央浅色面板（审批两页/通知），提示走底部条
// 主题：浅色（默认）/ 深色（低功耗，背光联动 45%），NVS 持久化，设置页切换。
// 动效仅保留：决定确认闪烁（整块变色 3×150ms）。
//
// 线程纪律：全部 LVGL 操作发生在 pump()（LVGL 定时器上下文）；按键只入队。
#include "approval_ui.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "approval_ui";

LV_FONT_DECLARE(app_font_16);

// ---- 双主题调色板 ----
typedef struct {
    uint32_t ground, panel, hair, ink, body, sub, sel_bg, accent, allow, deny, veil;
} palette_t;
static const palette_t PAL_LIGHT = {
    .ground = 0xF4F5F7, .panel = 0xFFFFFF, .hair = 0xE2E3E6,
    .ink = 0x14161A, .body = 0x3A3E45, .sub = 0x8A8F98,
    .sel_bg = 0xEAF2FF, .accent = 0x0A6CFF, .allow = 0x1E8E3E,
    .deny = 0xC5221F, .veil = 0x0B0D12 };
static const palette_t PAL_DARK = {
    .ground = 0x000000, .panel = 0x16181D, .hair = 0x2A2E35,
    .ink = 0xF2F3F5, .body = 0xC9CDD3, .sub = 0x7C828B,
    .sel_bg = 0x14263D, .accent = 0x4C9DFF, .allow = 0x34A853,
    .deny = 0xEA4335, .veil = 0x000000 };
static const palette_t *P = &PAL_LIGHT;
static bool s_dark;

// ---- 状态 ----
typedef enum { PG_HOME, PG_MENU, PG_ACTIVITY, PG_USAGE, PG_SETTINGS, PG_ABOUT } page_t;
typedef enum { OV_NONE, OV_REQ_P1, OV_REQ_P2, OV_NOTIFY, OV_FLASH } overlay_t;

static QueueHandle_t s_decision_q;
static QueueHandle_t s_msg_q;
static QueueHandle_t s_key_q;
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } key_evt_t;

// 活动环形缓冲
#define ACTIVITY_MAX 10
typedef struct { char agent[16]; char model[40]; char at[24]; } activity_t;
static activity_t s_activity[ACTIVITY_MAX];
static int s_activity_count;

// 用量快照
static char s_usage_tokens[24];
static struct { char name[24]; char left[24]; } s_quotas[UI_QUOTA_MAX];
static int s_quota_n;
static bool s_usage_valid;

// 时钟 / 信号
static int s_hour = -1, s_min, s_wday, s_month, s_day;
static int s_rssi = -100;

// 覆盖层当前请求
static char s_req_id[24], s_req_agent[16], s_req_title[64], s_req_detail[160];
static int64_t s_notify_until_us;

// ---- 对象 ----
static lv_obj_t *scr;
static lv_obj_t *lbl_time, *lbl_batt, *sig_bars[3];
static lv_obj_t *page_cont, *footer_label;
static page_t s_page = PG_HOME;
static int s_menu_sel;
// 首页
static lv_obj_t *lbl_date, *lbl_usage;
// 覆盖层
static lv_obj_t *veil, *ov_panel, *ov_tag, *ov_title, *ov_detail, *ov_page_dots;
static overlay_t s_overlay = OV_NONE;
// 闪烁
static int s_flash_state;
static bool s_flash_allow;
static int64_t s_flash_next_us;

static const char *const WDAY_CN[7] =
    { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

static void show_home(void);
static void show_menu(void);
static void approval_ui_build_all(void);

// ======================================================================
// 基础
// ======================================================================
static void style_reset(lv_obj_t *o) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
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

static lv_obj_t *make_card(lv_obj_t *parent, int w, int h) {
    lv_obj_t *card = lv_obj_create(parent);
    style_reset(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(P->panel), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(P->hair), 0);
    return card;
}

// 底部提示条
static lv_obj_t *make_footer_label(lv_obj_t *parent) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &app_font_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(P->sub), 0);
    lv_obj_set_width(l, 216);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 12, 292);
    return l;
}

// ======================================================================
// 喜鹊精灵（像素画 ×2；# 墨 W 白 A 蓝 . 透明）
// ======================================================================
static const char *const MAGPIE[] = {
    "......####..........",
    ".....######.........",
    "..A.##W####.........",
    "...######...........",
    "....########........",
    "...##########..##...",
    "..W#########..###...",
    "..W########..###....",
    "...######..###......",
    "....W###..##........",
    ".....BB#..#.........",
    "....BBBB#...........",
    ".....BB.............",
    "......#.............",
    "...#...#............",
};
#define MAGPIE_W 20
#define MAGPIE_H (int)(sizeof(MAGPIE) / sizeof(MAGPIE[0]))
#define MP_SCALE 2
static uint16_t magpie_buf[MAGPIE_W * MP_SCALE * MAGPIE_H * MP_SCALE];

static void magpie_build(lv_obj_t *parent) {
    lv_obj_t *canvas = lv_canvas_create(parent);
    const int w = MAGPIE_W * MP_SCALE, h = MAGPIE_H * MP_SCALE;
    lv_canvas_set_buffer(canvas, magpie_buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(canvas, lv_color_hex(P->ground), LV_OPA_COVER);
    for (int ry = 0; ry < MAGPIE_H; ry++) {
        for (int rx = 0; rx < MAGPIE_W; rx++) {
            char c = MAGPIE[ry][rx];
            uint32_t col;
            if (c == '#') col = P->ink;
            else if (c == 'W' || c == 'B') col = 0xFFFFFF;
            else if (c == 'A') col = P->accent;
            else continue;
            for (int dy = 0; dy < MP_SCALE; dy++)
                for (int dx = 0; dx < MP_SCALE; dx++)
                    lv_canvas_set_px(canvas, rx * MP_SCALE + dx,
                                     ry * MP_SCALE + dy, lv_color_hex(col),
                                     LV_OPA_COVER);
        }
    }
    lv_obj_set_pos(canvas, (240 - w) / 2, 56);
}

// ======================================================================
// 状态栏（全局）：时间左 / 信号+电量右
// ======================================================================
static void build_status_bar(void) {
    lbl_time = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(P->ink), 0);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_set_pos(lbl_time, 20, 8);

    for (int i = 0; i < 3; i++) {
        sig_bars[i] = lv_obj_create(scr);
        style_reset(sig_bars[i]);
        lv_obj_set_size(sig_bars[i], 4, 5 + i * 3);
        lv_obj_set_style_bg_opa(sig_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sig_bars[i], lv_color_hex(P->hair), 0);
        lv_obj_set_pos(sig_bars[i], 146 + i * 6, 26 - (5 + i * 3));
    }

    lbl_batt = make_label(scr, &lv_font_montserrat_14, P->sub, 176, 10, 46);
    lv_obj_set_style_text_align(lbl_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lbl_batt, "--%");

    lv_obj_t *line = lv_obj_create(scr);
    style_reset(line);
    lv_obj_set_size(line, 240, 1);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(P->hair), 0);
    lv_obj_set_pos(line, 0, 34);
}

static void refresh_signal(void) {
    int bars;
    if (s_rssi == -100) bars = 0;
    else if (s_rssi >= -55) bars = 3;
    else if (s_rssi >= -67) bars = 2;
    else if (s_rssi >= -78) bars = 1;
    else bars = 0;
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(sig_bars[i],
            lv_color_hex(i < bars ? P->ink : P->hair), 0);
    }
}

static void refresh_battery(void) {
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(lbl_batt, "--%");
        lv_obj_set_style_text_color(lbl_batt, lv_color_hex(P->sub), 0);
    } else {
        lv_label_set_text_fmt(lbl_batt, "%d%%", soc);
        lv_obj_set_style_text_color(lbl_batt,
            lv_color_hex(soc <= 20 ? P->deny : P->body), 0);
    }
}

static void refresh_time(void) {
    if (s_hour < 0) return;
    lv_label_set_text_fmt(lbl_time, "%02d:%02d", s_hour, s_min);
}

// ======================================================================
// 页面容器（灰底透明）与底部提示
// ======================================================================
static void open_page_container(void) {
    if (page_cont) lv_obj_delete(page_cont);
    page_cont = lv_obj_create(scr);
    style_reset(page_cont);
    lv_obj_set_size(page_cont, 208, 240);
    lv_obj_set_pos(page_cont, 16, 42);
    lv_obj_clear_flag(page_cont, LV_OBJ_FLAG_SCROLLABLE);
}

static void ui_footer(const char *text) {
    if (footer_label) lv_label_set_text(footer_label, text);
}

// ======================================================================
// 首页
// ======================================================================
static void show_home(void) {
    s_page = PG_HOME;
    open_page_container();
    magpie_build(page_cont);
    lbl_date = make_label(page_cont, &app_font_16,
                          P->sub, 0, 112, 208);
    lv_obj_set_style_text_align(lbl_date, LV_TEXT_ALIGN_CENTER, 0);
    if (s_hour >= 0) {
        lv_label_set_text_fmt(lbl_date, "%s  %d月%d日", WDAY_CN[s_wday % 7],
                              s_month, s_day);
    } else {
        lv_label_set_text(lbl_date, "时间同步中");
    }
    lbl_usage = make_label(page_cont, &lv_font_montserrat_14, P->sub,
                           0, 140, 208);
    lv_obj_set_style_text_align(lbl_usage, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_usage, s_usage_valid ? s_usage_tokens : "");
    ui_footer("OK 菜单");
}

// ======================================================================
// 菜单（灰底 + 白卡片）
// ======================================================================
static const char *const MENU_ITEMS[] =
    { "Magpie 活动", "用量与额度", "设置", "关于" };
#define MENU_COUNT 4
static lv_obj_t *menu_list;

static void menu_refresh(void) {
    lv_obj_clean(menu_list);
    for (int i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *row = make_card(menu_list, 208, 52);
        lv_obj_set_style_pad_hor(row, 12, 0);
        if (i == s_menu_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(P->sel_bg), 0);
            lv_obj_set_style_border_color(row, lv_color_hex(P->accent), 0);
        }
        lv_obj_t *dot = lv_obj_create(row);
        style_reset(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot,
            lv_color_hex(i == s_menu_sel ? P->accent : P->hair), 0);
        lv_obj_set_pos(dot, 2, 23);
        lv_obj_t *t = make_label(row, &app_font_16,
                                 i == s_menu_sel ? P->ink : P->body, 16, 16, 160);
        lv_label_set_text(t, MENU_ITEMS[i]);
    }
}

static void show_menu(void) {
    s_page = PG_MENU;
    open_page_container();
    menu_list = lv_obj_create(page_cont);
    style_reset(menu_list);
    lv_obj_set_size(menu_list, 208, 244);
    lv_obj_set_pos(menu_list, 0, 0);
    lv_obj_clear_flag(menu_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(menu_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(menu_list, 8, 0);
    menu_refresh();
    ui_footer("上下 选择 · OK 打开");
}

// ======================================================================
// Magpie 活动页
// ======================================================================
static void show_activity_page(void) {
    s_page = PG_ACTIVITY;
    open_page_container();
    int n = s_activity_count < ACTIVITY_MAX ? s_activity_count : ACTIVITY_MAX;
    lv_obj_t *list = lv_obj_create(page_cont);
    style_reset(list);
    lv_obj_set_size(list, 208, 244);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    if (n == 0) {
        lv_obj_t *e = make_label(list, &app_font_16,
                                 P->sub, 4, 8, 196);
        lv_label_set_text(e, "暂无活动（等待 Agent 流量）");
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *card = make_card(list, 208, 56);
        lv_obj_set_style_pad_hor(card, 12, 0);
        char head[64];
        snprintf(head, sizeof(head), "%s  %s", s_activity[i].at,
                 s_activity[i].agent);
        lv_obj_t *h = make_label(card, &app_font_16,
                                 P->body, 0, 8, 184);
        lv_label_set_text(h, head);
        lv_obj_t *mo = make_label(card, &lv_font_montserrat_14, P->sub, 0, 32, 184);
        lv_label_set_text(mo, s_activity[i].model);
    }
    ui_footer("OK 返回");
}

// ======================================================================
// 用量与额度页
// ======================================================================
static void show_usage_page(void) {
    s_page = PG_USAGE;
    open_page_container();
    lv_obj_t *list = lv_obj_create(page_cont);
    style_reset(list);
    lv_obj_set_size(list, 208, 244);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    if (!s_usage_valid) {
        lv_obj_t *e = make_label(list, &app_font_16,
                                 P->sub, 4, 8, 196);
        lv_label_set_text(e, "暂无用量数据\n（Magpie 有流量后自动更新）");
    } else {
        lv_obj_t *card = make_card(list, 208, 62);
        lv_obj_set_style_pad_hor(card, 12, 0);
        lv_obj_t *t1 = make_label(card, &app_font_16,
                                  P->sub, 0, 8, 184);
        lv_label_set_text(t1, "7 日 tokens");
        lv_obj_t *t2 = make_label(card, &lv_font_montserrat_20, P->ink, 0, 28, 184);
        lv_label_set_text(t2, s_usage_tokens);
        for (int i = 0; i < s_quota_n; i++) {
            lv_obj_t *qc = make_card(list, 208, 48);
            lv_obj_set_style_pad_hor(qc, 12, 0);
            lv_obj_t *qn = make_label(qc, &app_font_16,
                                      P->body, 0, 14, 130);
            lv_label_set_text(qn, s_quotas[i].name);
            lv_obj_t *qv = make_label(qc, &app_font_16,
                                      P->allow, 130, 14, 66);
            lv_obj_set_style_text_align(qv, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_text(qv, s_quotas[i].left);
        }
    }
    ui_footer("OK 返回");
}

// ======================================================================
// 设置页（深色模式，NVS 持久化）
// ======================================================================
static void settings_toggle_dark(void);

static void show_settings_page(void) {
    s_page = PG_SETTINGS;
    open_page_container();
    lv_obj_t *card = make_card(page_cont, 208, 52);
    lv_obj_set_style_pad_hor(card, 12, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(P->accent), 0);
    lv_obj_t *t = make_label(card, &app_font_16,
                             P->ink, 0, 16, 120);
    lv_label_set_text(t, "深色模式");
    lv_obj_t *v = make_label(card, &app_font_16,
                             s_dark ? P->accent : P->sub, 130, 16, 66);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(v, s_dark ? "开" : "关");
    ui_footer("OK 切换 · 长按返回");
}

static void settings_toggle_dark(void) {
    s_dark = !s_dark;
    nvs_handle_t h;
    if (nvs_open("appr", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "dark", s_dark ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    P = s_dark ? &PAL_DARK : &PAL_LIGHT;
    bsp_display_backlight(s_dark ? 45 : 70);
    approval_ui_build_all();
    show_settings_page();
}

// ======================================================================
// 关于页
// ======================================================================
static void show_about_page(void) {
    s_page = PG_ABOUT;
    open_page_container();
    const char *rows[] = {
        "Approval Remote v0.3",
        "桥: Mac approval-bridge",
        "电源键=硬件开关",
        NULL,
    };
    int y = 12;
    for (int i = 0; rows[i]; i++) {
        lv_obj_t *l = make_label(page_cont, &app_font_16,
                                 P->body, 0, y, 200);
        lv_label_set_text(l, rows[i]);
        y += 34;
    }
    ui_footer("OK 返回");
}

// ======================================================================
// 覆盖层（蒙层 + 浅面板；提示走底部条）
// ======================================================================
static void build_overlay(void) {
    veil = lv_obj_create(scr);
    lv_obj_set_pos(veil, 0, 0);
    lv_obj_set_size(veil, 240, 320);
    lv_obj_set_style_bg_color(veil, lv_color_hex(P->veil), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);
    lv_obj_set_style_border_width(veil, 0, 0);
    lv_obj_set_style_radius(veil, 0, 0);
    lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);

    ov_panel = lv_obj_create(veil);
    style_reset(ov_panel);
    lv_obj_set_size(ov_panel, 208, 196);
    lv_obj_set_pos(ov_panel, 16, 52);
    lv_obj_set_style_bg_color(ov_panel, lv_color_hex(P->panel), 0);
    lv_obj_set_style_bg_opa(ov_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ov_panel, 16, 0);
    lv_obj_set_style_border_width(ov_panel, 2, 0);
    lv_obj_set_style_border_color(ov_panel, lv_color_hex(P->hair), 0);

    ov_tag = make_label(ov_panel, &lv_font_montserrat_14, P->accent, 14, 12, 180);
    ov_title = make_label(ov_panel, &app_font_16, P->ink,
                          16, 34, 176);
    ov_detail = make_label(ov_panel, &lv_font_montserrat_14, P->body,
                           16, 62, 176);
    ov_page_dots = make_label(ov_panel, &app_font_16,
                              P->sub, 16, 156, 176);
    lv_label_set_text(ov_page_dots, "1/2");
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
    lv_label_set_text(ov_title, "请求确认");
    lv_label_set_text_fmt(ov_detail, "%.60s", s_req_title);
    lv_obj_set_style_text_color(ov_detail, lv_color_hex(P->body), 0);
    lv_label_set_text(ov_page_dots, "1/2");
    overlay_show(true);
    ui_footer("OK 详情 · DOWN 拒绝");
}

static void show_request_p2(void) {
    s_overlay = OV_REQ_P2;
    lv_label_set_text_fmt(ov_tag, "%s", s_req_agent);
    lv_label_set_text(ov_title, "完整命令");
    lv_label_set_text_fmt(ov_detail, "%.150s",
                          s_req_detail[0] ? s_req_detail : s_req_title);
    lv_label_set_text(ov_page_dots, "2/2");
    overlay_show(true);
    ui_footer("OK 允许 · UP 返回 · DOWN 拒绝");
}

static void show_notify_overlay(const ui_msg_t *m) {
    s_overlay = OV_NOTIFY;
    lv_label_set_text_fmt(ov_tag, "%s", m->agent);
    lv_label_set_text(ov_title, "活动");
    lv_label_set_text_fmt(ov_detail, "%.60s", m->title);
    lv_obj_set_style_text_color(ov_detail, lv_color_hex(P->sub), 0);
    lv_label_set_text(ov_page_dots, "");
    overlay_show(true);
    ui_footer("任意键 关闭");
    s_notify_until_us = esp_timer_get_time() + (int64_t)m->timeout_s * 1000000;
}

static void start_flash(bool allow) {
    s_overlay = OV_FLASH;
    s_flash_allow = allow;
    s_flash_state = 6;
    s_flash_next_us = esp_timer_get_time() + 150 * 1000;
    lv_obj_set_style_bg_color(veil, lv_color_hex(allow ? P->allow : P->deny), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_40, 0);
    lv_label_set_text(ov_page_dots, "");
    lv_label_set_text(ov_detail, allow ? "已允许" : "已拒绝");
    lv_obj_set_style_text_color(ov_detail,
        lv_color_hex(allow ? P->allow : P->deny), 0);
    ui_footer(allow ? "已允许" : "已拒绝");
}

static void pump_flash(void) {
    if (!s_flash_state) return;
    if (esp_timer_get_time() < s_flash_next_us) return;
    s_flash_next_us = esp_timer_get_time() + 150 * 1000;
    s_flash_state--;
    if (!s_flash_state) {
        lv_obj_set_style_bg_color(veil, lv_color_hex(P->veil), 0);
        lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);
        overlay_show(false);
        if (s_page == PG_HOME) ui_footer("OK 菜单");
        else if (s_page == PG_MENU) ui_footer("上下 选择 · OK 打开");
        else ui_footer("OK 返回");
    }
}

// ======================================================================
// 决定 / 活动缓冲 / 用量
// ======================================================================
static void emit_decision(bool allow) {
    ui_decision_t d = { .allow = allow };
    strlcpy(d.id, s_req_id, sizeof(d.id));
    if (s_decision_q) (void)xQueueSend(s_decision_q, &d, 0);
    start_flash(allow);
}

static void activity_push(const ui_msg_t *m) {
    activity_t *a = &s_activity[ACTIVITY_MAX - 1];
    for (int i = ACTIVITY_MAX - 1; i > 0; i--) s_activity[i] = s_activity[i - 1];
    strlcpy(a->agent, m->agent, sizeof(a->agent));
    const char *sep = strstr(m->title, " / ");
    strlcpy(a->model, sep ? sep + 3 : m->title, sizeof(a->model));
    if (s_hour >= 0) {
        snprintf(a->at, sizeof(a->at), "%02d:%02d", s_hour, s_min);
    } else {
        strlcpy(a->at, "--:--", sizeof(a->at));
    }
    if (s_activity_count < ACTIVITY_MAX) s_activity_count++;
}

static void usage_apply(const ui_msg_t *m) {
    strlcpy(s_usage_tokens, m->tokens, sizeof(s_usage_tokens));
    s_quota_n = m->quota_n;
    for (int i = 0; i < m->quota_n && i < UI_QUOTA_MAX; i++) {
        strlcpy(s_quotas[i].name, m->quotas[i].name, sizeof(s_quotas[i].name));
        strlcpy(s_quotas[i].left, m->quotas[i].left, sizeof(s_quotas[i].left));
    }
    s_usage_valid = true;
    if (s_page == PG_HOME && lbl_usage) {
        lv_label_set_text(lbl_usage, s_usage_tokens);
    }
}

// ======================================================================
// 按键
// ======================================================================
static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_RELEASE) return;

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
        ui_footer("OK 菜单");
        return;
    }
    if (s_overlay == OV_FLASH) return;

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (s_page == PG_MENU) show_home();
        else if (s_page != PG_HOME) show_menu();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (s_page == PG_HOME) {
        if (btn == BSP_BTN_OK) show_menu();
    } else if (s_page == PG_MENU) {
        if (btn == BSP_BTN_UP) {
            s_menu_sel = (s_menu_sel + MENU_COUNT - 1) % MENU_COUNT;
            menu_refresh();
        } else if (btn == BSP_BTN_DOWN) {
            s_menu_sel = (s_menu_sel + 1) % MENU_COUNT;
            menu_refresh();
        } else if (btn == BSP_BTN_OK) {
            switch (s_menu_sel) {
                case 0: show_activity_page(); break;
                case 1: show_usage_page(); break;
                case 2: show_settings_page(); break;
                case 3: show_about_page(); break;
            }
        }
    } else if (btn == BSP_BTN_OK) {
        if (s_page == PG_SETTINGS) settings_toggle_dark();
        else show_menu();
    }
}

// ======================================================================
// 消息
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
            }
            break;
        case UI_MSG_USAGE:
            usage_apply(m);
            break;
        case UI_MSG_STATUS:
            break;   // 连接态由信号格与通知体现
        case UI_MSG_CANCEL:
            if (s_overlay == OV_REQ_P1 || s_overlay == OV_REQ_P2
                || s_overlay == OV_NOTIFY) {
                overlay_show(false);
                ui_footer("OK 菜单");
            }
            break;
    }
}

// ======================================================================
// 全量构建与公共接口
// ======================================================================
void approval_ui_build_all(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(P->ground), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    build_status_bar();
    footer_label = make_footer_label(scr);
    build_overlay();
    lv_screen_load(scr);
    refresh_time();
    refresh_battery();
    refresh_signal();
}

void approval_ui_init(QueueHandle_t decision_out) {
    s_decision_q = decision_out;
    s_msg_q = xQueueCreate(8, sizeof(ui_msg_t));
    s_key_q = xQueueCreate(8, sizeof(key_evt_t));
    nvs_handle_t h;
    if (nvs_open("appr", NVS_READONLY, &h) == ESP_OK) {
        uint8_t d = 0;
        if (nvs_get_u8(h, "dark", &d) == ESP_OK && d) s_dark = true;
        nvs_close(h);
    }
    P = s_dark ? &PAL_DARK : &PAL_LIGHT;
    bsp_display_backlight(s_dark ? 45 : 70);
    approval_ui_build_all();
    show_home();
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
    static int64_t last_refresh = 0;
    ui_msg_t m;
    while (s_msg_q && xQueueReceive(s_msg_q, &m, 0) == pdTRUE) handle_msg(&m);
    key_evt_t k;
    while (s_key_q && xQueueReceive(s_key_q, &k, 0) == pdTRUE) handle_key(k.btn, k.ev);
    pump_flash();
    if (s_overlay == OV_NOTIFY && s_notify_until_us
        && esp_timer_get_time() >= s_notify_until_us) {
        overlay_show(false);
        s_notify_until_us = 0;
        ui_footer("OK 菜单");
    }
    int64_t now = esp_timer_get_time() / 1000000;
    if (now != last_sec) {
        last_sec = now;
        refresh_time();
        refresh_battery();
    }
    if (now - last_refresh >= 3) {
        last_refresh = now;
        refresh_signal();
    }
}

void approval_ui_set_time(int hour, int minute, int wday, int month, int day) {
    s_hour = hour; s_min = minute; s_wday = wday; s_month = month; s_day = day;
    if (s_page == PG_HOME && lbl_date) {
        lv_label_set_text_fmt(lbl_date, "%s  %d月%d日",
                              WDAY_CN[s_wday % 7], s_month, s_day);
    }
}

void approval_ui_set_rssi(int rssi_dbm) {
    s_rssi = rssi_dbm;
}
