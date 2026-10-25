// main/approval_main.c —— Approval Remote：远程审批器应用（最小走通版）。
//
// 链路: Wi-Fi STA → TCP 连接审批桥(approval-bridge/bridge.py hub)
// 协议: JSON 行(hello/welcome/request/decision/ack/cancel/ping/pong)，见桥文件头注释。
// 按键: OK 短按=允许, DOWN 短按=拒绝(入队由 net 任务发送, 回调不阻塞)。
// 线程纪律: net 任务绝不触碰 LVGL(经 ui 队列), 按键回调只入队; LVGL 操作全部
//           在主任务的 lv_timer 回调里(LVGL 任务自身已持锁)。
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "wifi_cred.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "approval";

#define WIFI_BIT_GOT_IP   (1 << 0)
#define UI_QUEUE_DEPTH    8
#define NET_QUEUE_DEPTH   8
#define FIELD_ID          24
#define FIELD_AGENT       16
#define FIELD_TITLE       64
#define FIELD_DETAIL      160

// ---- 应用状态 ----
typedef enum { ST_WIFI, ST_BRIDGE, ST_IDLE, ST_PENDING } app_state_t;
static volatile app_state_t s_state = ST_WIFI;

// net → ui 队列消息
typedef enum { UI_STATUS, UI_REQUEST, UI_CANCEL } ui_kind_t;
typedef struct {
    ui_kind_t kind;
    char id[FIELD_ID];
    char agent[FIELD_AGENT];
    char title[FIELD_TITLE];
    char detail[FIELD_DETAIL];
    int timeout_s;
} ui_msg_t;

// 按键 → net 队列消息
typedef enum { NET_DECISION_ALLOW, NET_DECISION_DENY } net_kind_t;
typedef struct {
    net_kind_t kind;
    char id[FIELD_ID];
} net_msg_t;

static QueueHandle_t s_ui_queue;    // net_task → LVGL 主任务
static QueueHandle_t s_net_queue;   // 按键回调 → net_task
static char s_pending_id[FIELD_ID]; // 当前待决请求(仅主任务/net 任务按约定分写, 先走通不加锁)

// ==========================================================================
// Wi-Fi STA（标准 IDF 模式；模拟器的虚拟网络层会接管实际连通性）
// ==========================================================================
static EventGroupHandle_t s_wifi_events;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state = ST_WIFI;
        esp_wifi_connect();  // 自动重连
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi 已连接, IP=" IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_BIT_GOT_IP);
    }
}

static void wifi_init_sta(void) {
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(s_wifi_events, WIFI_BIT_GOT_IP,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

// ==========================================================================
// net 任务: TCP 客户端 + JSON 行协议
// ==========================================================================
static void send_line(int sock, const char *line) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s\n", line);
    if (n > 0 && n < (int)sizeof(buf)) {
        lwip_send(sock, buf, n, 0);
    }
}

static void dispatch_request(int sock, cJSON *root) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
    const char *agent = cJSON_GetStringValue(cJSON_GetObjectItem(root, "agent"));
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char *detail = cJSON_GetStringValue(cJSON_GetObjectItem(root, "detail"));
    cJSON *to = cJSON_GetObjectItem(root, "timeout_s");
    ui_msg_t m = { .kind = UI_REQUEST, .timeout_s = to ? to->valueint : 120 };
    strlcpy(m.id, id ? id : "", sizeof(m.id));
    strlcpy(m.agent, agent ? agent : "?", sizeof(m.agent));
    strlcpy(m.title, title ? title : "(no title)", sizeof(m.title));
    strlcpy(m.detail, detail ? detail : "", sizeof(m.detail));
    strlcpy(s_pending_id, m.id, sizeof(s_pending_id));
    s_state = ST_PENDING;
    xQueueSend(s_ui_queue, &m, 0);
    ESP_LOGI(TAG, "审批请求 [%s] %s: %s", m.agent, m.title, m.id);
    (void)sock;
}

static void send_decision(int sock, const char *id, const char *decision) {
    char line[160];
    snprintf(line, sizeof(line),
             "{\"type\":\"decision\",\"id\":\"%s\",\"decision\":\"%s\"}", id, decision);
    send_line(sock, line);
    ESP_LOGI(TAG, "已发送决定 %s → %s", id, decision);
}

static void net_task(void *arg) {
    (void)arg;
    for (;;) {
        s_state = ST_BRIDGE;
        struct sockaddr_in dst = { 0 };
        dst.sin_family = AF_INET;
        dst.sin_port = htons(BRIDGE_PORT);
        inet_aton(BRIDGE_HOST, &dst.sin_addr);
        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (lwip_connect(sock, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
            char hello[128];
            snprintf(hello, sizeof(hello),
                     "{\"type\":\"hello\",\"device\":\"passport\",\"proto\":1,"
                     "\"token\":\"%s\"}", BRIDGE_TOKEN);
            send_line(sock, hello);
            s_state = ST_IDLE;
            ESP_LOGI(TAG, "已连接审批桥 %s:%d", BRIDGE_HOST, BRIDGE_PORT);

            char rbuf[1024];
            int rlen = 0;
            for (;;) {
                // 1) 非阻塞收一行
                char line[1024];
                struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
                lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                int n = lwip_recv(sock, rbuf + rlen, sizeof(rbuf) - rlen - 1, 0);
                if (n > 0) {
                    rlen += n;
                    rbuf[rlen] = 0;
                    char *nl;
                    while ((nl = strchr(rbuf, '\n')) != NULL) {
                        *nl = 0;
                        strlcpy(line, rbuf, sizeof(line));
                        int rest = rlen - (nl - rbuf) - 1;
                        memmove(rbuf, nl + 1, rest > 0 ? rest : 0);
                        rlen = rest > 0 ? rest : 0;

                        cJSON *root = cJSON_Parse(line);
                        if (!root) continue;
                        const char *type =
                            cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
                        if (type && strcmp(type, "request") == 0) {
                            dispatch_request(sock, root);
                        } else if (type && strcmp(type, "cancel") == 0) {
                            ui_msg_t m = { .kind = UI_CANCEL };
                            const char *id =
                                cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
                            strlcpy(m.id, id ? id : "", sizeof(m.id));
                            xQueueSend(s_ui_queue, &m, 0);
                        } else if (type && strcmp(type, "ping") == 0) {
                            send_line(sock, "{\"type\":\"pong\"}");
                        } else if (type && strcmp(type, "ack") == 0) {
                            // 决定已被桥接收；UI 已在按键时回待机
                        } else if (type && strcmp(type, "welcome") == 0) {
                            ESP_LOGI(TAG, "桥已认证本设备");
                        } else if (type && strcmp(type, "reject") == 0) {
                            ESP_LOGE(TAG, "桥拒绝(token 错误?)");
                        }
                        cJSON_Delete(root);
                    }
                } else if (n == 0) {
                    break;  // 对端关闭
                } else {
                    if (errno != EWOULDBLOCK && errno != EAGAIN) break;
                }
                // 2) 按键决定出队即发
                net_msg_t nm;
                while (xQueueReceive(s_net_queue, &nm, 0) == pdTRUE) {
                    send_decision(sock, nm.id,
                                  nm.kind == NET_DECISION_ALLOW ? "allow" : "deny");
                }
            }
        } else {
            ESP_LOGW(TAG, "连接审批桥失败(%s), 3s 后重试",
                     strerror(errno));
        }
        close(sock);
        s_state = ST_BRIDGE;
        ui_msg_t m = { .kind = UI_CANCEL };
        xQueueSend(s_ui_queue, &m, 0);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ==========================================================================
// UI: 主任务 lv_timer 消费 ui 队列(始终在 LVGL 上下文, 无需另加锁)
// ==========================================================================
static lv_obj_t *s_status_scr, *s_pending_scr;
static lv_obj_t *s_lbl_state, *s_lbl_ip;
static lv_obj_t *s_lbl_agent, *s_lbl_title, *s_lbl_detail, *s_lbl_hint;

static void screen_common(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
}

static void build_status_screen(void) {
    s_status_scr = lv_obj_create(NULL);
    screen_common(s_status_scr);
    lv_obj_t *title = lv_label_create(s_status_scr);
    lv_label_set_text(title, "Approval Remote");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8E8E8), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

    s_lbl_state = lv_label_create(s_status_scr);
    lv_obj_set_style_text_font(s_lbl_state, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(0x9AA0A6), 0);
    lv_obj_align(s_lbl_state, LV_ALIGN_TOP_MID, 0, 120);

    s_lbl_ip = lv_label_create(s_status_scr);
    lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(0x5F6B76), 0);
    lv_obj_align(s_lbl_ip, LV_ALIGN_TOP_MID, 0, 156);
}

static void build_pending_screen(void) {
    s_pending_scr = lv_obj_create(NULL);
    screen_common(s_pending_scr);

    s_lbl_agent = lv_label_create(s_pending_scr);
    lv_obj_set_style_text_font(s_lbl_agent, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_agent, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(s_lbl_agent, LV_ALIGN_TOP_LEFT, 20, 40);

    s_lbl_title = lv_label_create(s_pending_scr);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0xF5F5F5), 0);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_title, 200);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, 20, 66);

    s_lbl_detail = lv_label_create(s_pending_scr);
    lv_obj_set_style_text_font(s_lbl_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_detail, lv_color_hex(0xC9D1D9), 0);
    lv_label_set_long_mode(s_lbl_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_detail, 200);
    lv_obj_align(s_lbl_detail, LV_ALIGN_TOP_LEFT, 20, 140);

    s_lbl_hint = lv_label_create(s_pending_scr);
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(0x7A2020), 0);
    lv_label_set_text(s_lbl_hint, "OK: allow   DOWN: deny");
    lv_obj_align(s_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -44);
}

static void ui_timer_cb(lv_timer_t *t) {
    (void)t;
    ui_msg_t m;
    while (xQueueReceive(s_ui_queue, &m, 0) == pdTRUE) {
        if (m.kind == UI_REQUEST) {
            lv_label_set_text_fmt(s_lbl_agent, "%s", m.agent);
            lv_label_set_text_fmt(s_lbl_title, "%s", m.title);
            lv_label_set_text_fmt(s_lbl_detail, "%s", m.detail);
            lv_screen_load(s_pending_scr);
        } else {
            lv_screen_load(s_status_scr);
        }
    }
    if (s_state == ST_WIFI) {
        lv_label_set_text(s_lbl_state, "Wi-Fi connecting...");
    } else if (s_state == ST_BRIDGE) {
        lv_label_set_text(s_lbl_state, "Bridge connecting...");
    }
}

// ==========================================================================
// 按键: esp_timer 回调只入队
// ==========================================================================
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (ev != BSP_BTN_PRESS || s_state != ST_PENDING) return;
    net_kind_t kind = btn == BSP_BTN_OK ? NET_DECISION_ALLOW : NET_DECISION_DENY;
    if (btn != BSP_BTN_OK && btn != BSP_BTN_DOWN) return;
    net_msg_t nm = { .kind = kind };
    strlcpy(nm.id, s_pending_id, sizeof(nm.id));
    (void)xQueueSend(s_net_queue, &nm, 0);
    s_state = ST_IDLE;  // 回待机, 防重复按键
}

// ==========================================================================
void app_main(void) {
    ESP_LOGI(TAG, "Approval Remote 启动");
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }
    bsp_display_backlight(60);

    s_ui_queue = xQueueCreate(UI_QUEUE_DEPTH, sizeof(ui_msg_t));
    s_net_queue = xQueueCreate(NET_QUEUE_DEPTH, sizeof(net_msg_t));

    build_status_screen();
    build_pending_screen();
    lv_screen_load(s_status_scr);
    lv_timer_create(ui_timer_cb, 300, NULL);

    ESP_ERROR_CHECK(bsp_button_init(on_key, NULL));

    wifi_init_sta();
    // IP 显示在状态屏
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"),
                              &ip) == ESP_OK) {
        lv_label_set_text_fmt(s_lbl_ip, IPSTR, IP2STR(&ip.ip));
    }

    xTaskCreate(net_task, "approval_net", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "装配完成, 等待审批请求");
}
