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
#include "libs/qrcode/lv_qrcode.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "approval_ui";

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(lv_font_montserrat_14);

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
static uint8_t s_dim_idx = 2;        // 40/60/80/100
static uint8_t s_off_idx = 2;        // 息屏：0 关 / 1=30s / 2=60s / 3=120s
static bool s_screen_off;
static int64_t s_last_activity_us;
static const uint16_t kOffSeconds[4] = { 0, 30, 60, 120 };
static uint8_t s_notify_ttl = 5;                 // 网页可调的通知停留秒数
static char s_quiet_start[6] = "23:00";          // 免打扰开始
static char s_quiet_end[6] = "07:00";            // 免打扰结束

// ---- 状态 ----
typedef enum { PG_HOME, PG_MENU, PG_ACTIVITY, PG_USAGE, PG_PUSH, PG_PROV, PG_SETTINGS, PG_ABOUT } page_t;
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

// "HH:MM" → 分钟数；失败返回 -1
static int hhmm_to_min(const char *t) {
    if (!t || strlen(t) < 4) return -1;
    int h = atoi(t), m = 0;
    const char *colon = strchr(t, ':');
    if (colon) m = atoi(colon + 1);
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

// 当前是否处于免打扰区间（支持跨零点）
static bool in_quiet_hours(void) {
    int a = hhmm_to_min(s_quiet_start), b = hhmm_to_min(s_quiet_end);
    if (a < 0 || b < 0 || a == b || s_hour < 0) return false;
    int now = s_hour * 60 + s_min;
    return a < b ? (now >= a && now < b) : (now >= a || now < b);
}

static int s_rssi = -100;

// 覆盖层当前请求
static char s_req_id[24], s_req_agent[16], s_req_title[64], s_req_detail[160];
static char s_prov_ap[33];            // 配网热点名
static char s_prov_pin[5];            // 配网码
static char s_health_line[96];        // "up 1" / "down 0"
static char s_health_pending[3][64];  // 待决推送摘要（最多展示 3 行）
static int s_health_pend_n;
// Agent 在线状态（桥侧适配器连接即"已连接"）
#define AGENT_MAX 4
static struct { char name[16]; bool active; } s_agents[AGENT_MAX];
static int s_agent_n;
static int64_t s_notify_until_us;

// ---- 对象 ----
static lv_obj_t *scr;
static lv_obj_t *lbl_time, *lbl_batt;
static lv_obj_t *batt_body, *batt_fill, *batt_nub;
static lv_obj_t *page_cont, *footer_label;
static page_t s_page = PG_HOME;
static int s_menu_sel;
// 首页
static lv_obj_t *lbl_date, *lbl_usage, *lbl_status;
static lv_obj_t *bar_used, *bar_left;   // Token 卡内进度填充
// 覆盖层
static lv_obj_t *veil, *ov_panel, *ov_tag, *ov_title, *ov_detail, *ov_page_dots;
static overlay_t s_overlay = OV_NONE;
// 闪烁
static int s_flash_state;
static bool s_flash_allow;
static int64_t s_flash_next_us;

static const char *const WDAY_CN[7] =
    { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

static void (*s_prov_cb)(void);
static void show_home(void);
static void show_menu(void);
static void show_push_page(void);
static void settings_cycle_dim(void);
static uint8_t s_set_sel;
static const char *const kOffTxt[4] = { "关闭", "30 秒", "60 秒", "120 秒" };
#define SET_ROWS 3
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
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
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

// 官网悬停式动效：轻微上下浮动 + 振翅，共 6 个 90ms 帧（约 0.55s）
#define WAG_FRAMES 6
static int s_wag_frame;
static int s_wag_y0;

static void magpie_wag(void) {
    if (!magpie_cv) return;
    s_wag_b = false;
    s_wag_frame = 0;
    s_wag_left = WAG_FRAMES;
    s_wag_y0 = lv_obj_get_y(magpie_cv);
    s_wag_next_us = esp_timer_get_time();
}

static void magpie_pump_wag(void) {
    if (!s_wag_left || !magpie_cv) return;
    int64_t now = esp_timer_get_time();
    if (now < s_wag_next_us) return;
    s_wag_next_us = now + 90 * 1000;
    s_wag_left--;
    s_wag_frame++;
    // 浮动：0,-2,0,-2,0,0（上浮两格两次）
    static const int dy[WAG_FRAMES] = { 0, -2, 0, -2, 0, 0 };
    lv_obj_set_y(magpie_cv, s_wag_y0 + dy[s_wag_frame % WAG_FRAMES]);
    // 振翅：帧 A/B 交替
    s_wag_b = !s_wag_b;
    magpie_draw(s_wag_b ? MAGPIE_B : MAGPIE);
    if (!s_wag_left) {
        lv_obj_set_y(magpie_cv, s_wag_y0);   // 回到原位
        magpie_draw(MAGPIE);
    }
}

// ======================================================================
// 状态栏（全局）：时间左 / 信号+电量右
// ======================================================================
// WiFi 三格信号（竖条，由短到高；亮格数按 RSSI）
static lv_obj_t *sig_bars[3];

// 电池：外框 + 极板 + 横向填充 + 框内数字（深色/浅色自适应）
static void build_status_bar(void) {
    // 时间（左）
    lbl_time = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(P->ink), 0);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_set_pos(lbl_time, 14, 10);

    // WiFi 三格（右移；三格底边对齐 y=26）
    for (int i = 0; i < 3; i++) {
        sig_bars[i] = lv_obj_create(scr);
        style_reset(sig_bars[i]);
        lv_obj_set_style_radius(sig_bars[i], 1, 0);
        lv_obj_set_style_bg_opa(sig_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sig_bars[i], lv_color_hex(P->hair), 0);
        const int hh = 5 + i * 3;               // 5 / 8 / 11
        lv_obj_set_size(sig_bars[i], 4, hh);
        lv_obj_set_pos(sig_bars[i], 146 + i * 5, 26 - hh);
    }

    // 电池外框（最右）
    batt_body = lv_obj_create(scr);
    style_reset(batt_body);
    lv_obj_set_style_radius(batt_body, 3, 0);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(batt_body, 1, 0);
    lv_obj_set_style_border_color(batt_body, lv_color_hex(P->body), 0);
    lv_obj_set_size(batt_body, 44, 18);
    lv_obj_set_pos(batt_body, 170, 8);

    // 横向填充条（在框内，从左向右生长；层级低于数字）
    batt_fill = lv_obj_create(scr);
    style_reset(batt_fill);
    lv_obj_set_style_radius(batt_fill, 2, 0);
    lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(P->allow), 0);
    lv_obj_set_size(batt_fill, 2, 12);
    lv_obj_set_pos(batt_fill, 172, 11);

    // 极板
    batt_nub = lv_obj_create(scr);
    style_reset(batt_nub);
    lv_obj_set_style_radius(batt_nub, 1, 0);
    lv_obj_set_style_bg_opa(batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(batt_nub, lv_color_hex(P->body), 0);
    lv_obj_set_size(batt_nub, 2, 6);
    lv_obj_set_pos(batt_nub, 215, 14);

    // 数字（画在框内，最后创建以置顶）
    lbl_batt = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(P->ink), 0);
    lv_obj_set_style_text_align(lbl_batt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_batt, 44);
    lv_label_set_text(lbl_batt, "--");
    lv_obj_set_pos(lbl_batt, 170, 10);   // 与电池框同 x、同宽、居中

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
    else if (s_rssi >= -60) bars = 3;
    else if (s_rssi >= -72) bars = 2;
    else if (s_rssi >= -85) bars = 1;
    else bars = 0;
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(sig_bars[i], lv_color_hex(P->hair), 0);
    }
    for (int i = 0; i < bars; i++) {
        lv_obj_set_style_bg_color(sig_bars[i], lv_color_hex(P->ink), 0);
    }
}

static int backlight_level(void) {
    static const int kLevels[4] = { 40, 60, 80, 100 };
    return kLevels[s_dim_idx];
}

static void apply_backlight(void) {
    bsp_display_backlight(backlight_level());
}

static void screen_off_enter(void) {
    s_screen_off = true;
    bsp_display_backlight(0);
}

static void screen_off_exit(void) {
    if (!s_screen_off) return;
    s_screen_off = false;
    apply_backlight();
    s_last_activity_us = esp_timer_get_time();
}

static void refresh_battery(void) {
    int soc = bsp_battery_soc();
    uint32_t col;
    if (soc < 0) {
        lv_label_set_text(lbl_batt, "--");
        lv_obj_set_style_text_color(lbl_batt, lv_color_hex(P->sub), 0);
        lv_obj_set_size(batt_fill, 2, 12);
        col = P->hair;
    } else {
        lv_label_set_text_fmt(lbl_batt, "%d", soc);
        // 数字用前景色保证深浅主题都可读（填充在它下面）
        lv_obj_set_style_text_color(lbl_batt, lv_color_hex(P->ink), 0);
        col = soc >= 60 ? P->allow : (soc >= 30 ? 0xE8A13A : P->deny);
        int wfill = 2 + soc * 38 / 100;          // 框内约 40px 可用
        lv_obj_set_size(batt_fill, wfill, 12);
    }
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(col), 0);
    lv_obj_set_pos(batt_fill, 172, 11);
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
    magpie_build(page_cont);                       // 左上喜鹊
    lbl_date = make_label(page_cont, &app_font_16, P->body, 108, 12, 100);
    lv_obj_set_style_text_align(lbl_date, LV_TEXT_ALIGN_RIGHT, 0);
    if (s_hour >= 0) {
        lv_label_set_text_fmt(lbl_date, "%s\n%d月%d日",
                              WDAY_CN[s_wday % 7], s_month, s_day);
    } else {
        lv_label_set_text(lbl_date, "时间\n同步中");
    }
    // 卡 1：7 日 token 消耗（紫色进度，从左到右表示用量占满程度）
    lv_obj_t *c1 = make_card(page_cont, 208, 72);
    lv_obj_set_pos(c1, 0, 84);
    lv_obj_set_style_pad_hor(c1, 12, 0);
    lv_obj_t *l1 = make_label(c1, &app_font_16, P->sub, 0, 8, 184);
    lv_label_set_text(l1, "Token 消耗 · 7 日");
    lbl_usage = make_label(c1, &lv_font_montserrat_20, P->ink, 0, 28, 184);
    lv_label_set_text(lbl_usage, s_usage_valid && s_usage_tokens[0]
                                 ? s_usage_tokens : "--");
    // 无独立槽对象：填充条直接放在卡片上（避免多占 LVGL 池）
    bar_used = lv_obj_create(c1);
    style_reset(bar_used);
    lv_obj_set_style_radius(bar_used, 3, 0);
    lv_obj_set_style_bg_opa(bar_used, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar_used, lv_color_hex(0x7C5CFF), 0);
    lv_obj_set_size(bar_used, 2, 5);
    lv_obj_set_pos(bar_used, 0, 59);

    // 卡 2：剩余额度（绿色进度，从左到右表示剩余比例）
    lv_obj_t *c2 = make_card(page_cont, 208, 72);
    lv_obj_set_pos(c2, 0, 164);
    lv_obj_set_style_pad_hor(c2, 12, 0);
    lv_obj_t *l2 = make_label(c2, &app_font_16, P->sub, 0, 6, 184);
    if (s_usage_valid && s_quota_n > 0) {
        lv_label_set_text_fmt(l2, "Token 剩余 · %.26s", s_quotas[0].name);
    } else {
        lv_label_set_text(l2, "Token 剩余");
    }
    lbl_status = make_label(c2, &lv_font_montserrat_20, P->ink, 0, 26, 184);
    lv_label_set_text(lbl_status,
        (s_usage_valid && s_quota_n > 0) ? s_quotas[0].left : "--");
    bar_left = lv_obj_create(c2);
    style_reset(bar_left);
    lv_obj_set_style_radius(bar_left, 3, 0);
    lv_obj_set_style_bg_opa(bar_left, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar_left, lv_color_hex(P->allow), 0);
    lv_obj_set_size(bar_left, 2, 5);
    lv_obj_set_pos(bar_left, 0, 59);

    ui_footer("OK 菜单");
    magpie_wag();                                  // 回到桌面挥翅
}

// ======================================================================
// 菜单（灰底 + 白卡片）
// ======================================================================
static const char *const MENU_ITEMS[] =
    { "推送", "用量与额度", "设置", "配网", "关于" };
#define MENU_COUNT 5
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

    // Agent 在线状态行
    lv_obj_t *cardA = make_card(list, 208, 62);
    lv_obj_set_style_pad_hor(cardA, 12, 0);
    lv_obj_t *tA = make_label(cardA, &app_font_16, P->sub, 0, 8, 184);
    lv_label_set_text(tA, "Agent 在线");
    int shown = s_agent_n < 3 ? s_agent_n : 3;
    if (shown == 0) {
        lv_obj_t *v = make_label(cardA, &app_font_16, P->sub, 0, 30, 184);
        lv_label_set_text(v, "暂无已绑定的 Agent");
    }
    for (int i = 0; i < shown; i++) {
        lv_obj_t *dot = lv_obj_create(cardA);
        style_reset(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(
            s_agents[i].active ? P->allow : P->hair), 0);
        lv_obj_set_pos(dot, 2, 16 + i * 14);
        lv_obj_t *nm = make_label(cardA, &lv_font_montserrat_14,
                                  s_agents[i].active ? P->ink : P->sub,
                                  16, 12 + i * 14, 168);
        lv_label_set_text(nm, s_agents[i].name);
    }
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
// 配网页（QR 加 Wi-Fi + 文字说明 + 配网码）
// ======================================================================
static void show_provision_page(void) {
    s_page = PG_PROV;
    open_page_container();

    // QR：手机相机扫一扫直接加入热点
    char wifi_uri[72];
    snprintf(wifi_uri, sizeof(wifi_uri), "WIFI:T:nopass;S:%s;;", s_prov_ap);
    lv_obj_t *qr = lv_qrcode_create(page_cont);
    lv_qrcode_update(qr, wifi_uri, (uint32_t)strlen(wifi_uri));
    lv_obj_set_size(qr, 100, 100);
    lv_obj_set_pos(qr, 6, 4);
    lv_qrcode_set_dark_color(qr, lv_color_hex(P->ink));
    lv_qrcode_set_light_color(qr, lv_color_hex(P->panel));

    // 右侧说明
    lv_obj_t *t = make_label(page_cont, &app_font_16, P->body, 112, 8, 96);
    lv_label_set_text_fmt(t, "扫码加入\n%s", s_prov_ap);
    lv_obj_t *u = make_label(page_cont, &app_font_16, P->sub, 112, 66, 96);
    lv_label_set_text_fmt(u, "浏览器打开\n192.168.4.1\n配网码: %s", s_prov_pin);

    ui_footer("OK 退出配网");
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
    // 行 2：息屏时间
    lv_obj_t *r2 = make_card(page_cont, 208, 52);
    lv_obj_set_style_pad_hor(r2, 12, 0);
    if (s_set_sel == 2) {
        lv_obj_set_style_bg_color(r2, lv_color_hex(P->sel_bg), 0);
        lv_obj_set_style_border_color(r2, lv_color_hex(P->accent), 0);
    }
    lv_obj_t *t2 = make_label(r2, &app_font_16, P->ink, 0, 16, 110);
    lv_label_set_text(t2, "息屏时间");
    lv_obj_t *v2 = make_label(r2, &lv_font_montserrat_14,
                              s_set_sel == 2 ? P->accent : P->sub, 110, 19, 86);
    lv_obj_set_style_text_align(v2, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(v2, kOffTxt[s_off_idx]);
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

static void settings_cycle_off(void) {
    s_off_idx = (s_off_idx + 1) % 4;
    nvs_handle_t h;
    if (nvs_open("appr", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "off", s_off_idx);
        nvs_commit(h);
        nvs_close(h);
    }
    s_last_activity_us = esp_timer_get_time();
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
    s_last_activity_us = esp_timer_get_time();
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
    int ttl = s_notify_ttl > 0 ? s_notify_ttl : m->timeout_s;
    s_notify_until_us = esp_timer_get_time() + (int64_t)ttl * 1000000;
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
    if (s_page == PG_HOME) {
        if (lbl_usage) {                     // 卡 1：7 日消耗
            lv_label_set_text(lbl_usage,
                s_usage_tokens[0] ? s_usage_tokens : "--");
        }
        if (lbl_status) {                    // 卡 2：剩余额度
            if (s_quota_n > 0) {
                lv_label_set_text(lbl_status, s_quotas[0].left);
            } else {
                lv_label_set_text(lbl_status, "--");
            }
        }
        // 进度条：消耗条按对数量级给视觉比例（10G 视为满）；
        // 剩余条按百分比（解析 "63%" 数字）
        if (bar_used) {
            double v = 0;
            const char *t = s_usage_tokens;
            double num = strtod(t, NULL);
            char unit = t[strlen(t) ? strlen(t) - 1 : 0];
            if (unit == 'G') num *= 1.0;
            else if (unit == 'M') num /= 1000.0;
            else if (unit == 'K') num /= 1000000.0;
            v = num / 10.0;                 // 10G 满格
            if (v > 1) v = 1;
            if (v < 0.02 && num > 0) v = 0.02;
            lv_obj_set_width(bar_used, (int32_t)(2 + v * 182));
        }
        if (bar_left) {
            int pct = 0;
            if (s_quota_n > 0) {
                const char *d = s_quotas[0].left;
                while (*d && (*d < '0' || *d > '9')) d++;
                pct = atoi(d);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
            }
            lv_obj_set_width(bar_left, pct ? (int32_t)(2 + pct * 1.82) : 2);
        }
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
                case 3:
                    if (s_prov_cb) s_prov_cb();
                    break;
                case 4: show_about_page(); break;
            }
        }
    } else if (s_page == PG_PROV) {
        if (btn == BSP_BTN_OK) esp_restart();   // 退出配网 → 重启正常连接
    } else if (s_page == PG_SETTINGS) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_set_sel = (s_set_sel + 1) % SET_ROWS;
            show_settings_page();
        } else if (btn == BSP_BTN_OK) {
            if (s_set_sel == 0) settings_toggle_dark();
            else if (s_set_sel == 1) settings_cycle_dim();
            else settings_cycle_off();
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
            if (in_quiet_hours()) break;          // 免打扰：不弹，仅在页面留痕
            if (s_overlay == OV_NONE || s_overlay == OV_NOTIFY) {
                show_notify_overlay(m);
            }
            break;
        case UI_MSG_USAGE:
            usage_apply(m);
            break;
        case UI_MSG_SETTINGS: {
            // quota_n=dim, timeout_s=off, id[0]=dark, agent[0]=ttl,
            // title=quiet_start, detail=quiet_end
            if (m->quota_n >= 40 && m->quota_n <= 100) {
                s_dim_idx = m->quota_n >= 100 ? 3
                          : m->quota_n >= 80 ? 2
                          : m->quota_n >= 60 ? 1 : 0;
                apply_backlight();
            }
            if (m->timeout_s >= 0) {
                s_off_idx = m->timeout_s >= 120 ? 3
                          : m->timeout_s >= 60 ? 2
                          : m->timeout_s >= 30 ? 1 : 0;
            }
            bool want_dark = (m->id[0] == '1');
            if (want_dark != s_dark) {
                s_dark = want_dark;
                P = s_dark ? &PAL_DARK : &PAL_LIGHT;
                approval_ui_build_all();
                show_home();
            }
            if ((unsigned char)m->agent[0] >= 2) {
                s_notify_ttl = (unsigned char)m->agent[0];
            }
            strlcpy(s_quiet_start, m->title, sizeof(s_quiet_start));
            strlcpy(s_quiet_end, m->detail, sizeof(s_quiet_end));
            nvs_handle_t hh;
            if (nvs_open("appr", NVS_READWRITE, &hh) == ESP_OK) {
                nvs_set_u8(hh, "dim", s_dim_idx);
                nvs_set_u8(hh, "off", s_off_idx);
                nvs_set_u8(hh, "dark", s_dark ? 1 : 0);
                nvs_set_u8(hh, "ttl", s_notify_ttl);
                nvs_set_str(hh, "qst", s_quiet_start);
                nvs_set_str(hh, "qen", s_quiet_end);
                nvs_commit(hh);
                nvs_close(hh);
            }
            ESP_LOGI("approval_ui", "设置已应用 dim=%d off=%d dark=%d ttl=%d",
                     m->quota_n, m->timeout_s, s_dark, s_notify_ttl);
            break;
        }
        case UI_MSG_PROVISION:
            strlcpy(s_prov_ap, m->title, sizeof(s_prov_ap));
            strlcpy(s_prov_pin, m->detail, sizeof(s_prov_pin));
            show_provision_page();
            break;
        case UI_MSG_HEALTH: {
            // title = "up 1"；detail = 待决行（| 分隔）
            strlcpy(s_health_line, m->title, sizeof(s_health_line));
            s_health_pend_n = 0;
            // 解析 "codex=1|zcode=0|..." 到 Agent 在线表
            s_agent_n = 0;
            {
                char tmp2[96];
                strlcpy(tmp2, m->detail, sizeof(tmp2));
                char *seg = strtok(tmp2, "|");
                while (seg && s_agent_n < AGENT_MAX) {
                    char *eq = strchr(seg, '=');
                    if (eq) {
                        *eq = 0;
                        strlcpy(s_agents[s_agent_n].name, seg,
                                sizeof(s_agents[0].name));
                        s_agents[s_agent_n].active = eq[1] == '1';
                        s_agent_n++;
                    }
                    seg = strtok(NULL, "|");
                }
            }
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

void approval_ui_set_provision_cb(void (*fn)(void)) {
    s_prov_cb = fn;
}

void approval_ui_init(QueueHandle_t decision_out) {
    s_decision_q = decision_out;
    s_msg_q = xQueueCreate(4, sizeof(ui_msg_t));
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
        uint8_t d3 = 2;
        if (nvs_get_u8(h, "off", &d3) == ESP_OK && d3 < 4) s_off_idx = d3;
        uint8_t t4 = 5;
        if (nvs_get_u8(h, "ttl", &t4) == ESP_OK && t4 >= 2 && t4 <= 30) s_notify_ttl = t4;
        size_t sz = sizeof(s_quiet_start);
        if (nvs_get_str(h, "qst", s_quiet_start, &sz) != ESP_OK) {
            strlcpy(s_quiet_start, "23:00", sizeof(s_quiet_start));
        }
        sz = sizeof(s_quiet_end);
        if (nvs_get_str(h, "qen", s_quiet_end, &sz) != ESP_OK) {
            strlcpy(s_quiet_end, "07:00", sizeof(s_quiet_end));
        }
        nvs_close(h);
    }
    s_last_activity_us = esp_timer_get_time();
    P = s_dark ? &PAL_DARK : &PAL_LIGHT;
    apply_backlight();
    approval_ui_build_all();
    show_home();
}

void approval_ui_post(const ui_msg_t *msg) {
    if (!s_msg_q) return;
    (void)xQueueSend(s_msg_q, msg, 0);
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
    while (s_key_q && xQueueReceive(s_key_q, &k, 0) == pdTRUE) {
        if (s_screen_off) { screen_off_exit(); continue; }   // 唤醒即吞键
        handle_key(k.btn, k.ev);
        s_last_activity_us = esp_timer_get_time();
    }
    if (s_screen_off) return;            // 息屏中不做任何绘制
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
    // 息屏倒计时（弹窗/闪烁期间不熄屏）
    uint16_t off_s = kOffSeconds[s_off_idx];
    if (off_s && s_overlay == OV_NONE && s_page != PG_SETTINGS
        && esp_timer_get_time() - s_last_activity_us
               >= (int64_t)off_s * 1000000) {
        screen_off_enter();
    }
}

void approval_ui_set_time(int hour, int minute, int wday, int month, int day) {
    s_hour = hour; s_min = minute; s_wday = wday; s_month = month; s_day = day;
    if (s_page == PG_HOME && lbl_date) {
        lv_label_set_text_fmt(lbl_date, "%s\n%d月%d日",
                              WDAY_CN[s_wday % 7], s_month, s_day);
    }
}

void approval_ui_set_rssi(int rssi_dbm) {
    s_rssi = rssi_dbm;
}
