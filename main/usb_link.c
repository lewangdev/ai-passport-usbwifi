// main/usb_link.c —— 见 usb_link.h。
//
// 数据路径:
//   主机 TCP/IP ⇄ pppd/拨号网络 ⇄ CDC-ACM ⇄ USB-Serial-JTAG ⇄ 本模块 ⇄ lwIP PPP netif
//   ⇄ lwIP IP 转发 + NAPT ⇄ Wi-Fi STA netif ⇄ 路由器
#include "usb_link.h"

#include "app_status.h"
#include "link_cmd.h"

#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "usb_link";

#define USB_RX_BUF_SIZE     4096
#define USB_TX_BUF_SIZE     4096
#define RX_CHUNK            512
#define TX_TIMEOUT_MS       50
#define RESTART_DELAY_MS    500
// 公共 DNS 兜底:Wi-Fi 尚未拿到 DHCP DNS 时下发给主机。
#define FALLBACK_DNS1       "223.5.5.5"
#define FALLBACK_DNS2       "8.8.8.8"

typedef enum {
    MODE_COMMAND = 0,   // 解析 AT / CLIENT,等待 PPP
    MODE_PPP,           // 字节流直接交给 lwIP PPP
} link_mode_t;

typedef struct {
    esp_netif_driver_base_t base;
} usb_ppp_driver_t;

static usb_ppp_driver_t s_driver;
static esp_netif_t *s_netif;
static volatile link_mode_t s_mode;
static link_cmd_t s_cmd;
static volatile bool s_restart_pending;
static atomic_uint_fast32_t s_rx_bytes;
static atomic_uint_fast32_t s_tx_bytes;
static atomic_uint_fast32_t s_tx_drops;
static uint32_t s_local_ip;
static volatile uint32_t s_host_dns;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

static void status_set_link(app_link_state_t link, uint32_t host_ip)
{
    app_status_t *st = app_status_lock();
    st->link = link;
    st->host_ip = host_ip;
    app_status_unlock();
}

// ---------------------------------------------------------------------------
// esp_netif 驱动:PPP → USB
// ---------------------------------------------------------------------------

// 运行于 lwIP tcpip 任务。不能长时间阻塞:主机没在读时丢包,由 PPP/TCP 自行重传。
static esp_err_t usb_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (!usb_serial_jtag_is_connected()) {
        atomic_fetch_add(&s_tx_drops, 1);
        return ESP_OK;
    }
    int written = usb_serial_jtag_write_bytes(buffer, len, pdMS_TO_TICKS(TX_TIMEOUT_MS));
    if (written > 0) atomic_fetch_add(&s_tx_bytes, (uint32_t)written);
    if (written < (int)len) atomic_fetch_add(&s_tx_drops, 1);
    return ESP_OK;
}

static esp_err_t usb_post_attach(esp_netif_t *netif, esp_netif_iodriver_handle h)
{
    usb_ppp_driver_t *drv = h;
    drv->base.netif = netif;
    const esp_netif_driver_ifconfig_t ifcfg = {
        .handle = drv,
        .transmit = usb_transmit,
    };
    return esp_netif_set_driver_config(netif, &ifcfg);
}

// ---------------------------------------------------------------------------
// PPP 会话生命周期
// ---------------------------------------------------------------------------

static esp_err_t ppp_listen_start(void)
{
    app_status_t st;
    app_status_get(&st);

    esp_netif_ppp_config_t cfg = {
        .ppp_phase_event_enabled = false,
        .ppp_error_event_enabled = true,
        .ppp_passive = true,   // 服务端:等主机先发 LCP
    };
    cfg.ppp_our_ip4_addr.addr = s_local_ip;
    cfg.ppp_their_ip4_addr.addr = ipaddr_addr(USB_LINK_PEER_IP_STR);
    // 优先把路由器下发的 DNS 转告主机;没有时用公共 DNS。
    cfg.ppp_dns1_addr.addr = st.sta_dns ? st.sta_dns : ipaddr_addr(FALLBACK_DNS1);
    cfg.ppp_dns2_addr.addr = ipaddr_addr(FALLBACK_DNS2);
    s_host_dns = cfg.ppp_dns1_addr.addr;

    esp_err_t err = esp_netif_ppp_set_params(s_netif, &cfg);
    if (err == ESP_OK) {
        esp_netif_action_start(s_netif, NULL, 0, NULL);
    }
    return err;
}

static void enter_command_mode(void)
{
    s_mode = MODE_COMMAND;
    link_cmd_init(&s_cmd);
}

static void restart_session(void)
{
    s_restart_pending = false;
    enter_command_mode();
    status_set_link(usb_serial_jtag_is_connected() ? APP_LINK_WAITING : APP_LINK_NO_USB, 0);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_err_t err = ppp_listen_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPP 重新监听失败: %s", esp_err_to_name(err));
        s_restart_pending = true;
    }
}

static void on_ppp_status(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_netif_t *netif = data ? *(esp_netif_t **)data : NULL;
    if (netif != s_netif) return;
    // 0 = 连接成功;错误码在 PHASE 偏移之下;相位事件忽略。
    // 只在 PPP 模式下响应:同一次断开可能同时产生状态事件与 LOST_IP,
    // 重建后再到达的重复事件不能打断新会话。
    if (id > NETIF_PPP_ERRORNONE && id < NETIF_PP_PHASE_OFFSET && s_mode == MODE_PPP) {
        ESP_LOGI(TAG, "PPP 会话结束(%ld),重新等待主机", (long)id);
        s_restart_pending = true;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_PPP_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        if (ev->esp_netif != s_netif) return;
        // PPP 服务端:gw 字段即对端(主机)地址。
        status_set_link(APP_LINK_UP, ev->ip_info.gw.addr);
        esp_err_t err = esp_netif_napt_enable(s_netif);
        ESP_LOGI(TAG, "PPP 已建立,主机 " IPSTR ",NAT %s", IP2STR(&ev->ip_info.gw),
                 err == ESP_OK ? "开启" : "开启失败");
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        const ip_event_got_ip_t *ev = data;
        if (ev->esp_netif != s_netif || s_mode != MODE_PPP) return;
        ESP_LOGI(TAG, "PPP 链路断开");
        s_restart_pending = true;
    }
}

// ---------------------------------------------------------------------------
// 接收任务:USB → 命令解析 / PPP
// ---------------------------------------------------------------------------

static void write_reply(const link_cmd_result_t *r)
{
    if (r->reply_len) {
        usb_serial_jtag_write_bytes(r->reply, r->reply_len, pdMS_TO_TICKS(TX_TIMEOUT_MS));
    }
}

// 命令模式下逐字节处理;一旦切到 PPP,把本块剩余数据交给 PPP 并返回。
static void handle_command_bytes(uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        link_cmd_result_t r;
        link_cmd_feed(&s_cmd, buf[i], &r);
        if (r.action == LINK_CMD_NONE) continue;
        write_reply(&r);
        if (r.action != LINK_CMD_ENTER_PPP) continue;

        ESP_LOGI(TAG, "进入 PPP 模式(%s)", r.ppp_frame_start ? "主机直接发起" : "拨号握手");
        s_mode = MODE_PPP;
        status_set_link(APP_LINK_NEGOTIATING, 0);
        int start = r.ppp_frame_start ? i : i + 1;
        if (start < len) esp_netif_receive(s_netif, buf + start, len - start, NULL);
        return;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[RX_CHUNK];
    bool was_connected = false;

    for (;;) {
        if (s_restart_pending) restart_session();

        bool connected = usb_serial_jtag_is_connected();
        if (connected != was_connected) {
            was_connected = connected;
            if (!connected && s_mode == MODE_PPP) {
                // 拔线或主机休眠:主动结束会话,PPP 状态事件会触发重新监听。
                ESP_LOGI(TAG, "USB 断开,关闭 PPP 会话");
                esp_netif_action_stop(s_netif, NULL, 0, NULL);
            }
            if (s_mode == MODE_COMMAND || !connected) {
                status_set_link(connected ? APP_LINK_WAITING : APP_LINK_NO_USB, 0);
            }
        }

        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        atomic_fetch_add(&s_rx_bytes, (uint32_t)n);

        if (s_mode == MODE_PPP) {
            esp_netif_receive(s_netif, buf, n, NULL);
        } else {
            handle_command_bytes(buf, n);
        }
    }
}

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------

uint32_t usb_link_local_ip(void)
{
    return s_local_ip;
}

uint32_t usb_link_host_dns(void)
{
    return s_host_dns;
}

void usb_link_counters(uint32_t *rx_bytes, uint32_t *tx_bytes, uint32_t *tx_drops)
{
    if (rx_bytes) *rx_bytes = (uint32_t)atomic_load(&s_rx_bytes);
    if (tx_bytes) *tx_bytes = (uint32_t)atomic_load(&s_tx_bytes);
    if (tx_drops) *tx_drops = (uint32_t)atomic_load(&s_tx_drops);
}

esp_err_t usb_link_start(void)
{
    if (s_netif) return ESP_ERR_INVALID_STATE;
    s_local_ip = ipaddr_addr(USB_LINK_LOCAL_IP_STR);

    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = USB_TX_BUF_SIZE,
        .rx_buffer_size = USB_RX_BUF_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
    if (err != ESP_OK) return err;

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base.if_desc = "usb_ppp";
    esp_netif_config_t cfg = {
        .base = &base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP,
    };
    s_netif = esp_netif_new(&cfg);
    if (!s_netif) return ESP_ERR_NO_MEM;

    s_driver.base.post_attach = usb_post_attach;
    err = esp_netif_attach(s_netif, &s_driver);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                              on_ppp_status, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                                  on_ip_event, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                                  on_ip_event, NULL, NULL);
    }
    if (err != ESP_OK) return err;

    enter_command_mode();
    status_set_link(APP_LINK_NO_USB, 0);
    err = ppp_listen_start();
    if (err != ESP_OK) return err;

    // 优先级高于 Wi-Fi worker、低于 lwIP tcpip 任务。
    if (xTaskCreate(rx_task, "usb_link", 4096, NULL, 12, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
