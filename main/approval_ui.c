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
#include <math.h>

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
static uint8_t s_dim_idx = 2;   // 40/60/80/100

// ---- 状态 ----
typedef enum { PG_HOME, PG_MENU, PG_ACTIVITY, PG_USAGE, PG_PUSH, PG_SETTINGS, PG_ABOUT } page_t;
typedef enum { OV_NONE, OV_REQ_P1, OV_REQ_P2, OV_NOTIFY, OV_FLASH } overlay_t;

static QueueHandle_t s_decision_q;
static QueueHandle_t s_msg_q;
static QueueHandle_t s_key_q;
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } key_evt_t;

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
static char s_health_line[96];        // "up 1" / "down 0"
static char s_health_pending[3][64];  // 待决推送摘要（最多展示 3 行）
static int s_health_pend_n;
static int64_t s_notify_until_us;

// ---- 对象 ----
static lv_obj_t *scr;
static lv_obj_t *lbl_time, *lbl_batt;
static lv_obj_t *batt_body, *batt_fill, *batt_nub, *wifi_canvas;
static lv_obj_t *page_cont, *footer_label;
static page_t s_page = PG_HOME;
static int s_menu_sel;
// 首页
static lv_obj_t *lbl_date, *lbl_usage, *lbl_status;
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
static void show_push_page(void);
static void settings_cycle_dim(void);
static uint8_t s_set_sel;
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
    "###....................#####....",
    ".####................###########",
    "######..............############",
    "#######............##########...",
    "########..........###########...",
    "..#######..............#######..",
    ".....#########........#.#######.",
    "......######......#####..######.",
    "........#############.......###.",
    ".......############.............",
    "......##########................",
    "........#####...................",
    "...........##...................",
};
static const char *const MAGPIE_B[] = {
    "###....................#####....",
    ".####................###########",
    "######.............#############",
    "#######...........###########...",
    "########............##########..",
    "..#######.............#########.",
    ".....#########....#####.#######.",
    "......######..#######....######.",
    "........###########.........#...",
    ".......#######..................",
    "......##########................",
    "........#####...................",
    "...........##...................",
};
#define MAGPIE_W 32
#define MAGPIE_H 13

#define MAGPIE_H (int)(sizeof(MAGPIE) / sizeof(MAGPIE[0]))
#define MP_SCALE 2
static uint16_t magpie_buf[MAGPIE_W * MP_SCALE * MAGPIE_H * MP_SCALE];
static lv_obj_t *magpie_cv;
static bool s_wag_b;                 // 当前是否帧 B
static int s_wag_left;               // 剩余切换次数
static int64_t s_wag_next_us;

static void magpie_draw(const char *const art[/*MAGPIE_H*/]) {
    const int w = MAGPIE_W * MP_SCALE, h = MAGPIE_H * MP_SCALE;
    lv_canvas_fill_bg(magpie_cv, lv_color_hex(P->ground), LV_OPA_COVER);
    for (int ry = 0; ry < MAGPIE_H; ry++) {
        for (int rx = 0; rx < MAGPIE_W; rx++) {
            char c = art[ry][rx];
            uint32_t col;
            if (c == '#') col = P->ink;
            else if (c == 'W' || c == 'E') col = 0xFFFFFF;
            else if (c == 'A') col = P->accent;
            else continue;
            for (int dy = 0; dy < MP_SCALE; dy++)
                for (int dx = 0; dx < MP_SCALE; dx++)
                    lv_canvas_set_px(magpie_cv, rx * MP_SCALE + dx,
                                     ry * MP_SCALE + dy, lv_color_hex(col),
                                     LV_OPA_COVER);
        }
    }
    lv_obj_invalidate(magpie_cv);
}

static void magpie_build(lv_obj_t *parent) {
    magpie_cv = lv_canvas_create(parent);
    const int w = MAGPIE_W * MP_SCALE, h = MAGPIE_H * MP_SCALE;
    lv_canvas_set_buffer(magpie_cv, magpie_buf, w, h, LV_COLOR_FORMAT_RGB565);
    magpie_draw(MAGPIE);
    lv_obj_set_pos(magpie_cv, 6, 0);   // 首页左上
}

// 摆翅：切帧 4 次（约 0.5s），非阻塞
static void magpie_wag(void) {
    s_wag_b = false;
    s_wag_left = 4;
    s_wag_next_us = esp_timer_get_time();
}

static void magpie_pump_wag(void) {
    if (!s_wag_left) return;
    int64_t now = esp_timer_get_time();
    if (now < s_wag_next_us) return;
    s_wag_next_us = now + 130 * 1000;
    s_wag_left--;
    s_wag_b = !s_wag_b;
    magpie_draw(s_wag_b ? MAGPIE_B : MAGPIE);
}

// ======================================================================
// 状态栏（全局）：时间左 / 信号+电量右
// ======================================================================
// Wi-Fi 扇形图标（加大版，像素直绘）
static uint16_t wifi_buf[26 * 18];
static lv_obj_t *wifi_canvas;

static void wifi_icon_draw(bool active) {
    uint32_t col = active ? P->ink : P->hair;
    lv_canvas_fill_bg(wifi_canvas, lv_color_hex(P->ground), LV_OPA_COVER);
    const int cx = 13, cy = 16;
    for (int a = -54; a <= 54; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        float sn = sinf(rad), cs = cosf(rad);
        for (int t = 0; t < 2; t++) {
            lv_canvas_set_px(wifi_canvas, cx + (int)((11.5f - t) * sn),
                             cy - (int)((11.5f - t) * cs),
                             lv_color_hex(col), LV_OPA_COVER);
        }
    }
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -2; dx <= 2; dx++)
            lv_canvas_set_px(wifi_canvas, cx + dx, cy - 3 + dy,
                             lv_color_hex(col), LV_OPA_COVER);
}

static void build_status_bar(void) {
    // 水平一行：时间(左) …… WiFi(大) → 电池(最右，百分比在框内，横向填充)
    lbl_time = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(P->ink), 0);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_set_pos(lbl_time, 20, 10);

    wifi_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(wifi_canvas, wifi_buf, 26, 18, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(wifi_canvas, 112, 7);

    // 电池（最右）：先画填充（横向、左→右），再画框和百分比（叠在上层）
    batt_fill = lv_obj_create(scr);
    style_reset(batt_fill);
    lv_obj_set_size(batt_fill, 4, 12);
    lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(P->allow), 0);
    lv_obj_set_pos(batt_fill, 152, 10);

    batt_body = lv_obj_create(scr);
    style_reset(batt_body);
    lv_obj_set_size(batt_body, 38, 18);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(batt_body, 1, 0);
    lv_obj_set_style_border_color(batt_body, lv_color_hex(P->body), 0);
    lv_obj_set_style_radius(batt_body, 3, 0);
    lv_obj_set_pos(batt_body, 150, 8);

    batt_nub = lv_obj_create(scr);
    style_reset(batt_nub);
    lv_obj_set_size(batt_nub, 2, 6);
    lv_obj_set_style_radius(batt_nub, 1, 0);
    lv_obj_set_style_bg_opa(batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(batt_nub, lv_color_hex(P->body), 0);
    lv_obj_set_pos(batt_nub, 189, 14);

    lbl_batt = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(P->allow), 0);
    lv_label_set_text(lbl_batt, "--");
    lv_obj_set_pos(lbl_batt, 158, 10);

    lv_obj_t *line = lv_obj_create(scr);
    style_reset(line);
    lv_obj_set_size(line, 240, 1);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(P->hair), 0);
    lv_obj_set_pos(line, 0, 34);
}

static void refresh_signal(void) {
    wifi_icon_draw(s_rssi != -100 && s_rssi > -85);
}

static int backlight_level(void) {
    static const int kLevels[4] = { 40, 60, 80, 100 };
    return kLevels[s_dim_idx];
}

static void apply_backlight(void) {
    bsp_display_backlight(backlight_level());
}

static void refresh_battery(void) {
    int soc = bsp_battery_soc();
    uint32_t col;
    if (soc < 0) {
        lv_label_set_text(lbl_batt, "--");
        lv_obj_set_style_text_color(lbl_batt, lv_color_hex(P->sub), 0);
        lv_obj_set_size(batt_fill, 4, 12);
        col = P->hair;
    } else {
        lv_label_set_text_fmt(lbl_batt, "%d", soc);
        col = soc >= 60 ? P->allow : (soc >= 30 ? 0xE8A13A : P->deny);
        lv_obj_set_style_text_color(lbl_batt, lv_color_hex(col), 0);
        lv_obj_set_size(batt_fill, 2 + soc * 32 / 100, 12);
    }
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(col), 0);
    lv_obj_set_pos(batt_fill, 152, 10);
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
    { "推送", "用量与额度", "设置", "关于" };
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
// 推送页（健康状态：桥/设备/待决请求）
// ======================================================================
static void show_push_page(void) {
    s_page = PG_PUSH;
    open_page_container();
    lv_obj_t *list = lv_obj_create(page_cont);
    style_reset(list);
    lv_obj_set_size(list, 208, 244);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    lv_obj_t *card = make_card(list, 208, 56);
    lv_obj_set_style_pad_hor(card, 12, 0);
    lv_obj_t *t1 = make_label(card, &app_font_16, P->sub, 0, 8, 184);
    lv_label_set_text(t1, "桥连接");
    lv_obj_t *v1 = make_label(card, &app_font_16,
                              s_health_line[0] == 'u' ? P->allow : P->deny,
                              0, 28, 184);
    lv_label_set_text(v1, s_health_line[0] ? s_health_line : "未知");

    lv_obj_t *card2 = make_card(list, 208, 62);
    lv_obj_set_style_pad_hor(card2, 12, 0);
    lv_obj_t *t2 = make_label(card2, &app_font_16, P->sub, 0, 8, 184);
    lv_label_set_text(t2, "待决推送");
    if (s_health_pend_n == 0) {
        lv_obj_t *v2 = make_label(card2, &app_font_16, P->body, 0, 30, 184);
        lv_label_set_text(v2, "无（全部已处理）");
    } else {
        lv_obj_t *v2 = make_label(card2, &lv_font_montserrat_14,
                                  P->body, 0, 28, 184);
        lv_label_set_text(v2, s_health_pending[0]);
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
// 设置页（外观 + 屏幕调光，NVS 持久化，双行选择）
// ======================================================================
static void settings_toggle_dark(void);

static void show_settings_page(void) {
    s_page = PG_SETTINGS;
    open_page_container();
    lv_obj_set_flex_flow(page_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page_cont, 8, 0);
    // 行 0：外观（深色/浅色）
    lv_obj_t *r0 = make_card(page_cont, 208, 52);
    lv_obj_set_style_pad_hor(r0, 12, 0);
    if (s_set_sel == 0) {
        lv_obj_set_style_bg_color(r0, lv_color_hex(P->sel_bg), 0);
        lv_obj_set_style_border_color(r0, lv_color_hex(P->accent), 0);
    }
    lv_obj_t *t0 = make_label(r0, &app_font_16, P->ink, 0, 16, 110);
    lv_label_set_text(t0, "外观");
    lv_obj_t *v0 = make_label(r0, &app_font_16,
                              s_dark ? P->accent : P->sub, 110, 16, 86);
    lv_obj_set_style_text_align(v0, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(v0, s_dark ? "深色" : "浅色");
    // 行 1：屏幕调光（40/60/80/100 循环）
    lv_obj_t *r1 = make_card(page_cont, 208, 52);
    lv_obj_set_style_pad_hor(r1, 12, 0);
    if (s_set_sel == 1) {
        lv_obj_set_style_bg_color(r1, lv_color_hex(P->sel_bg), 0);
        lv_obj_set_style_border_color(r1, lv_color_hex(P->accent), 0);
    }
    lv_obj_t *t1 = make_label(r1, &app_font_16, P->ink, 0, 16, 110);
    lv_label_set_text(t1, "屏幕调光");
    static const char *const kDimTxt[4] = { "40%", "60%", "80%", "100%" };
    lv_obj_t *v1 = make_label(r1, &lv_font_montserrat_14,
                              s_set_sel == 1 ? P->accent : P->sub, 110, 19, 86);
    lv_obj_set_style_text_align(v1, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(v1, kDimTxt[s_dim_idx]);
    ui_footer("上下 选择 · OK 调整 · 长按返回");
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
    approval_ui_build_all();
    show_settings_page();
}

static void settings_cycle_dim(void) {
    s_dim_idx = (s_dim_idx + 1) % 4;
    nvs_handle_t h;
    if (nvs_open("appr", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "dim", s_dim_idx);
        nvs_commit(h);
        nvs_close(h);
    }
    apply_backlight();
    show_settings_page();
}

// ======================================================================
// 关于页
// ======================================================================
static void show_about_page(void) {
    s_page = PG_ABOUT;
    open_page_container();
    const char *rows[] = {
        "Approval Remote v0.5",
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
    lv_obj_set_style_bg_opa(veil, LV_OPA_50, 0);
    lv_obj_set_style_border_width(veil, 0, 0);
    lv_obj_set_style_radius(veil, 0, 0);
    lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);

    ov_panel = lv_obj_create(veil);
    style_reset(ov_panel);
    lv_obj_set_size(ov_panel, 208, 176);          // 240×320 正中：上下各 72
    lv_obj_set_pos(ov_panel, 16, 72);
    lv_obj_set_style_bg_color(ov_panel, lv_color_hex(P->panel), 0);
    lv_obj_set_style_bg_opa(ov_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ov_panel, 16, 0);
    lv_obj_set_style_border_width(ov_panel, 2, 0);
    lv_obj_set_style_border_color(ov_panel, lv_color_hex(P->hair), 0);

    ov_tag = make_label(ov_panel, &lv_font_montserrat_14, P->accent, 14, 14, 180);
    ov_title = make_label(ov_panel, &app_font_16, P->ink,
                          16, 38, 176);
    ov_detail = make_label(ov_panel, &lv_font_montserrat_14, P->body,
                           16, 68, 176);
    ov_page_dots = make_label(ov_panel, &app_font_16,
                              P->sub, 16, 140, 176);
    lv_label_set_text(ov_page_dots, "1/2");
}

static int backlight_level(void);
static void apply_backlight(void);

static void overlay_brightness(bool boost) {
    bsp_display_backlight(boost ? 100 : backlight_level());
}

static void overlay_show(bool show) {
    if (show) {
        // 运行时创建的页面卡片会盖住弹窗——显示前把蒙层与提示条提到最顶
        lv_obj_move_foreground(veil);
        if (footer_label) lv_obj_move_foreground(footer_label);
        lv_obj_clear_flag(veil, LV_OBJ_FLAG_HIDDEN);
        overlay_brightness(true);
    } else {
        lv_obj_add_flag(veil, LV_OBJ_FLAG_HIDDEN);
        s_overlay = OV_NONE;
        overlay_brightness(false);
    }
}

static void show_request_p1(void) {
    s_overlay = OV_REQ_P1;
    lv_label_set_text_fmt(ov_tag, "%s", s_req_agent);
    lv_label_set_text(ov_title, "请求确认");
    lv_label_set_text_fmt(ov_detail, "%.60s", s_req_title);
    lv_obj_set_style_text_color(ov_detail, lv_color_hex(P->ink), 0);
    lv_label_set_text(ov_page_dots, "1/2");
    overlay_show(true);
    ui_footer("OK 详情 · 长按OK 拒绝");
}

static void show_request_p2(void) {
    s_overlay = OV_REQ_P2;
    lv_label_set_text_fmt(ov_tag, "%s", s_req_agent);
    lv_label_set_text(ov_title, "完整命令");
    lv_label_set_text_fmt(ov_detail, "%.150s",
                          s_req_detail[0] ? s_req_detail : s_req_title);
    lv_obj_set_style_text_color(ov_detail, lv_color_hex(P->ink), 0);
    lv_label_set_text(ov_page_dots, "2/2");
    overlay_show(true);
    ui_footer("OK 允许 · UP 返回 · 长按OK 拒绝");
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
    lv_obj_set_style_bg_color(veil,
        lv_color_hex(allow ? P->allow : 0x000000), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0);
    lv_label_set_text(ov_page_dots, "");
    lv_label_set_text(ov_detail, allow ? "已允许" : "已拒绝");
    lv_obj_set_style_text_color(ov_detail,
        lv_color_hex(allow ? 0xFFFFFF : P->sub), 0);
    ui_footer(allow ? "已允许" : "已拒绝");
}

static void pump_flash(void) {
    if (!s_flash_state) return;
    if (esp_timer_get_time() < s_flash_next_us) return;
    s_flash_next_us = esp_timer_get_time() + 150 * 1000;
    s_flash_state--;
    if (!s_flash_state) {
        lv_obj_set_style_bg_color(veil, lv_color_hex(P->veil), 0);
        lv_obj_set_style_bg_opa(veil, LV_OPA_50, 0);
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

static void usage_apply(const ui_msg_t *m) {
    strlcpy(s_usage_tokens, m->tokens, sizeof(s_usage_tokens));
    s_quota_n = m->quota_n;
    for (int i = 0; i < m->quota_n && i < UI_QUOTA_MAX; i++) {
        strlcpy(s_quotas[i].name, m->quotas[i].name, sizeof(s_quotas[i].name));
        strlcpy(s_quotas[i].left, m->quotas[i].left, sizeof(s_quotas[i].left));
    }
    s_usage_valid = true;
    if (s_page == PG_HOME && lbl_usage) {
        lv_label_set_text(lbl_usage,
            s_quota_n > 0 ? s_quotas[0].left : s_usage_tokens);
    }
}

// ======================================================================
// 按键
// ======================================================================
static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_RELEASE) return;

    if (s_overlay == OV_REQ_P1 || s_overlay == OV_REQ_P2) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            emit_decision(false);   // 长按 OK = 拒绝
            return;
        }
        if (ev == BSP_BTN_CLICK) {
            if (s_overlay == OV_REQ_P1 && btn == BSP_BTN_OK) show_request_p2();
            else if (s_overlay == OV_REQ_P1 && btn == BSP_BTN_DOWN) show_request_p2();
            else if (s_overlay == OV_REQ_P2 && btn == BSP_BTN_OK) emit_decision(true);
            else if (s_overlay == OV_REQ_P2 && btn == BSP_BTN_UP) show_request_p1();
        }
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
        else magpie_wag();            // 闲按一下，它挥挥翅膀
    } else if (s_page == PG_MENU) {
        if (btn == BSP_BTN_UP) {
            s_menu_sel = (s_menu_sel + MENU_COUNT - 1) % MENU_COUNT;
            menu_refresh();
        } else if (btn == BSP_BTN_DOWN) {
            s_menu_sel = (s_menu_sel + 1) % MENU_COUNT;
            menu_refresh();
        } else if (btn == BSP_BTN_OK) {
            switch (s_menu_sel) {
                case 0: show_push_page(); break;
                case 1: show_usage_page(); break;
                case 2: show_settings_page(); break;
                case 3: show_about_page(); break;
            }
        }
    } else if (s_page == PG_SETTINGS) {
        if (btn == BSP_BTN_UP) {
            s_set_sel = (s_set_sel + 1) % 2;
            show_settings_page();
        } else if (btn == BSP_BTN_DOWN) {
            s_set_sel = (s_set_sel + 1) % 2;
            show_settings_page();
        } else if (btn == BSP_BTN_OK) {
            if (s_set_sel == 0) settings_toggle_dark();
            else settings_cycle_dim();
        }
    } else if (btn == BSP_BTN_OK) {
        show_menu();
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
            if (s_overlay == OV_NONE || s_overlay == OV_NOTIFY) {
                show_notify_overlay(m);
            }
            break;
        case UI_MSG_USAGE:
            usage_apply(m);
            break;
        case UI_MSG_HEALTH: {
            // title = "up 1"；detail = 待决行（| 分隔）
            strlcpy(s_health_line, m->title, sizeof(s_health_line));
            s_health_pend_n = 0;
            char tmp[160];
            strlcpy(tmp, m->detail, sizeof(tmp));
            char *seg = strtok(tmp, "|");
            while (seg && s_health_pend_n < 3) {
                strlcpy(s_health_pending[s_health_pend_n], seg, 64);
                s_health_pend_n++;
                seg = strtok(NULL, "|");
            }
            break;
        }
        case UI_MSG_STATUS:
            snprintf(s_health_line, sizeof(s_health_line), "%s",
                     strstr(m->title, "up") ? "up" : "down");
            break;
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
    if (nvs_open("appr", NVS_READONLY, &h) == ESP_OK) {
        uint8_t d2 = 2;
        if (nvs_get_u8(h, "dim", &d2) == ESP_OK && d2 < 4) s_dim_idx = d2;
        nvs_close(h);
    }
    P = s_dark ? &PAL_DARK : &PAL_LIGHT;
    apply_backlight();
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
    magpie_pump_wag();
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
    if (s_page == PG_HOME) {
        if (lbl_date) {
            lv_label_set_text_fmt(lbl_date, "%s\n%d月%d日",
                                  WDAY_CN[s_wday % 7], s_month, s_day);
        }
        if (lbl_status) {
            lv_label_set_text(lbl_status, "Magpie 用量");
        }
    }
}

void approval_ui_set_rssi(int rssi_dbm) {
    s_rssi = rssi_dbm;
}
