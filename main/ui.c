// main/ui.c —— 见 ui.h。
//
// 布局(240x320,四角有 30px 圆角遮罩):
//   顶栏   "USB WiFi"                       电量%
//   卡片   WI-FI  状态点 + SSID / 信号 / IP
//   连接线 ↓ 下行速率   ↑ 上行速率
//   卡片   USB    状态点 + 链路状态 / 电脑 IP
//   底栏   Setup: http://10.77.0.1
#include "ui.h"

#include "app_status.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "usb_link.h"

#include "lvgl.h"
#include "lwip/inet.h"
#include <stdio.h>

#define C_BG      0x0E1420
#define C_CARD    0x1A2333
#define C_INK     0xE8ECF3
#define C_MUTE    0x8892A6
#define C_OK      0x35C27A
#define C_WARN    0xF0B429
#define C_BAD     0xE5534B
#define C_IDLE    0x5A6478
#define C_ACCENT  0x4C8DFF

#define TICK_MS        500
#define BATTERY_EVERY  20    // 每 20 个 tick(10 s)读一次电量

typedef struct {
    lv_obj_t *dot;
    lv_obj_t *title;
    lv_obj_t *line1;
    lv_obj_t *line2;
} card_t;

static card_t s_wifi;
static card_t s_usb;
static lv_obj_t *s_battery;
static lv_obj_t *s_rate_down;
static lv_obj_t *s_rate_up;
static lv_obj_t *s_flow;
static uint32_t s_last_rx, s_last_tx;
static uint32_t s_tick;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void card_create(card_t *c, lv_obj_t *scr, int y, const char *title)
{
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, 14, y);
    lv_obj_set_size(box, 212, 92);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_pad_all(box, 12, 0);

    c->dot = lv_obj_create(box);
    lv_obj_remove_flag(c->dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c->dot, 10, 10);
    lv_obj_set_style_radius(c->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c->dot, 0, 0);
    lv_obj_set_style_bg_color(c->dot, lv_color_hex(C_IDLE), 0);
    lv_obj_set_pos(c->dot, 0, 3);

    c->title = label(box, &lv_font_montserrat_14, C_MUTE);
    lv_label_set_text(c->title, title);
    lv_obj_set_pos(c->title, 18, 0);

    c->line1 = label(box, &lv_font_montserrat_20, C_INK);
    lv_label_set_long_mode(c->line1, LV_LABEL_LONG_DOT);
    lv_obj_set_width(c->line1, 188);
    lv_obj_set_pos(c->line1, 0, 22);

    c->line2 = label(box, &lv_font_montserrat_14, C_MUTE);
    lv_label_set_long_mode(c->line2, LV_LABEL_LONG_DOT);
    lv_obj_set_width(c->line2, 188);
    lv_obj_set_pos(c->line2, 0, 50);
}

static void card_set(card_t *c, uint32_t color, const char *l1, const char *l2)
{
    lv_obj_set_style_bg_color(c->dot, lv_color_hex(color), 0);
    lv_label_set_text(c->line1, l1);
    lv_label_set_text(c->line2, l2);
}

static void ip_str(uint32_t addr, char *out, size_t n)
{
    struct in_addr a = { .s_addr = addr };
    inet_ntoa_r(a, out, n);
}

// 字节/秒 → 简短文本。
static void rate_str(uint32_t bytes_per_s, char *out, size_t n)
{
    if (bytes_per_s >= 1024 * 1024) {
        snprintf(out, n, "%lu.%lu MB/s", (unsigned long)(bytes_per_s / (1024 * 1024)),
                 (unsigned long)(bytes_per_s % (1024 * 1024) * 10 / (1024 * 1024)));
    } else {
        snprintf(out, n, "%lu KB/s", (unsigned long)(bytes_per_s / 1024));
    }
}

static void update_wifi(const app_status_t *st)
{
    char l2[48];
    switch (st->wifi) {
    case APP_WIFI_NO_CONFIG:
        card_set(&s_wifi, C_IDLE, "Not set up", "Open setup page on PC");
        break;
    case APP_WIFI_CONNECTING:
        card_set(&s_wifi, C_WARN, st->ssid, "Connecting...");
        break;
    case APP_WIFI_RETRYING:
        snprintf(l2, sizeof(l2), "Retrying (reason %u)", st->last_reason);
        card_set(&s_wifi, C_BAD, st->ssid, l2);
        break;
    case APP_WIFI_CONNECTED: {
        char ip[16];
        ip_str(st->sta_ip, ip, sizeof(ip));
        snprintf(l2, sizeof(l2), "%d dBm   %s", st->rssi, ip);
        card_set(&s_wifi, C_OK, st->ssid, l2);
        break;
    }
    }
}

static void update_usb(const app_status_t *st)
{
    switch (st->link) {
    case APP_LINK_NO_USB:
        card_set(&s_usb, C_IDLE, "No computer", "Plug into a PC data port");
        break;
    case APP_LINK_WAITING:
        card_set(&s_usb, C_WARN, "Waiting", "Start the connect script");
        break;
    case APP_LINK_NEGOTIATING:
        card_set(&s_usb, C_WARN, "Negotiating", "PPP handshake...");
        break;
    case APP_LINK_UP: {
        char ip[16], l2[40];
        ip_str(st->host_ip, ip, sizeof(ip));
        snprintf(l2, sizeof(l2), "PC %s", ip);
        card_set(&s_usb, C_OK, "Online", l2);
        break;
    }
    }
}

static void update_rates(bool flowing)
{
    uint32_t rx, tx;
    usb_link_counters(&rx, &tx, NULL);
    // USB rx = 电脑上行;USB tx = 电脑下行。
    uint32_t up = (rx - s_last_rx) * (1000 / TICK_MS);
    uint32_t down = (tx - s_last_tx) * (1000 / TICK_MS);
    s_last_rx = rx;
    s_last_tx = tx;

    char buf[24], text[40];
    rate_str(down, buf, sizeof(buf));
    snprintf(text, sizeof(text), LV_SYMBOL_DOWN " %s", buf);
    lv_label_set_text(s_rate_down, text);
    rate_str(up, buf, sizeof(buf));
    snprintf(text, sizeof(text), LV_SYMBOL_UP " %s", buf);
    lv_label_set_text(s_rate_up, text);
    lv_obj_set_style_bg_color(s_flow, lv_color_hex(flowing ? C_ACCENT : C_IDLE), 0);
}

static void update_battery(void)
{
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_battery, "");
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", soc);
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    app_status_t st;
    app_status_get(&st);
    update_wifi(&st);
    update_usb(&st);
    update_rates(st.wifi == APP_WIFI_CONNECTED && st.link == APP_LINK_UP);
    if (s_tick++ % BATTERY_EVERY == 0) update_battery();
}

void ui_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

    lv_obj_t *title = label(scr, &lv_font_montserrat_20, C_INK);
    lv_label_set_text(title, LV_SYMBOL_USB " USB WiFi");
    lv_obj_set_pos(title, 26, 16);

    s_battery = label(scr, &lv_font_montserrat_14, C_MUTE);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -26, 20);

    card_create(&s_wifi, scr, 50, LV_SYMBOL_WIFI "  WI-FI");

    // 两张卡片之间的"连接线"与速率。
    s_flow = lv_obj_create(scr);
    lv_obj_remove_flag(s_flow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_flow, 4, 44);
    lv_obj_set_pos(s_flow, 38, 144);
    lv_obj_set_style_border_width(s_flow, 0, 0);
    lv_obj_set_style_radius(s_flow, 2, 0);
    lv_obj_set_style_bg_color(s_flow, lv_color_hex(C_IDLE), 0);

    s_rate_down = label(scr, &lv_font_montserrat_14, C_INK);
    lv_obj_set_pos(s_rate_down, 56, 147);
    s_rate_up = label(scr, &lv_font_montserrat_14, C_MUTE);
    lv_obj_set_pos(s_rate_up, 56, 167);

    card_create(&s_usb, scr, 190, LV_SYMBOL_USB "  USB");

    lv_obj_t *hint = label(scr, &lv_font_montserrat_14, C_MUTE);
    lv_label_set_text(hint, "Setup: http://" USB_LINK_LOCAL_IP_STR);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    tick(NULL);
    lv_timer_create(tick, TICK_MS, NULL);
    lv_screen_load(scr);
}
