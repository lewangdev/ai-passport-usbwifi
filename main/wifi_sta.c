// main/wifi_sta.c —— 见 wifi_sta.h。
//
// 凭据不交给 Wi-Fi 驱动的 NVS 存储(WIFI_STORAGE_RAM),而是由本模块在验证成功
// 后自行写入 "usbwifi" 命名空间,这样输错密码不会覆盖原来可用的配置。
#include "wifi_sta.h"

#include "app_status.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "wifi_sta";

#define NVS_NS            "usbwifi"
#define NVS_KEY_SSID      "ssid"
#define NVS_KEY_PASS      "pass"

#define TRY_ATTEMPTS      3
#define TRY_ATTEMPT_MS    12000
#define RETRY_MIN_MS      1000
#define RETRY_MAX_MS      30000

#define BIT_GOT_IP        BIT0
#define BIT_DISCONNECTED  BIT1

typedef enum {
    CMD_RECONNECT = 0,
    CMD_TRY,
    CMD_FORGET,
    CMD_SCAN,
    CMD_DISCONNECTED,     // 事件回调通知:STA 断开,由 worker 安排退避重连
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char ssid[33];
    char pass[65];
} cmd_t;

static esp_netif_t *s_netif;
static QueueHandle_t s_queue;
static EventGroupHandle_t s_events;

// 以下仅在 worker 任务中读写(s_trying 也会被事件回调读取,单字节读写安全)。
static char s_ssid[33];
static char s_pass[65];
static volatile bool s_trying;
static volatile bool s_forgotten;
static uint32_t s_retry_ms = RETRY_MIN_MS;
static TickType_t s_retry_at;
static bool s_retry_armed;

static wifi_ap_record_t s_scan[WIFI_STA_SCAN_MAX];
static size_t s_scan_count;
static volatile bool s_scan_busy;
static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// 持久化
// ---------------------------------------------------------------------------

static void creds_load(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_ssid);
        if (nvs_get_str(h, NVS_KEY_SSID, s_ssid, &n) != ESP_OK) s_ssid[0] = '\0';
        n = sizeof(s_pass);
        if (nvs_get_str(h, NVS_KEY_PASS, s_pass, &n) != ESP_OK) s_pass[0] = '\0';
        nvs_close(h);
    }
    if (s_ssid[0] == '\0' && CONFIG_USBWIFI_DEFAULT_SSID[0] != '\0') {
        strlcpy(s_ssid, CONFIG_USBWIFI_DEFAULT_SSID, sizeof(s_ssid));
        strlcpy(s_pass, CONFIG_USBWIFI_DEFAULT_PASSWORD, sizeof(s_pass));
        ESP_LOGI(TAG, "使用编译期默认 Wi-Fi: %s", s_ssid);
    }
}

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (ssid) {
        err = nvs_set_str(h, NVS_KEY_SSID, ssid);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PASS, pass);
    } else {
        err = nvs_erase_key(h, NVS_KEY_SSID);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        esp_err_t e2 = nvs_erase_key(h, NVS_KEY_PASS);
        if (err == ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) err = e2;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// 状态辅助
// ---------------------------------------------------------------------------

static void status_set_wifi(app_wifi_state_t state, const char *ssid)
{
    app_status_t *st = app_status_lock();
    st->wifi = state;
    if (ssid) strlcpy(st->ssid, ssid, sizeof(st->ssid));
    if (state != APP_WIFI_CONNECTED) {
        st->sta_ip = st->sta_gw = 0;
        st->rssi = 0;
    }
    app_status_unlock();
}

static void status_set_try(app_try_state_t state, const char *ssid)
{
    app_status_t *st = app_status_lock();
    st->try_state = state;
    if (ssid) strlcpy(st->try_ssid, ssid, sizeof(st->try_ssid));
    app_status_unlock();
}

static esp_err_t apply_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    // 开放网络不设置阈值;否则至少 WPA,拒绝被降级到 OPEN/WEP。
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static void connect_saved(void)
{
    if (s_ssid[0] == '\0') {
        status_set_wifi(APP_WIFI_NO_CONFIG, "");
        return;
    }
    status_set_wifi(APP_WIFI_CONNECTING, s_ssid);
    esp_err_t err = apply_config(s_ssid, s_pass);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGW(TAG, "连接 %s 失败: %s", s_ssid, esp_err_to_name(err));
}

// 断开当前连接并等待断开事件落定,避免旧连接的断开事件被误判为新尝试的失败。
static void disconnect_and_settle(void)
{
    s_retry_armed = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED);
}

// ---------------------------------------------------------------------------
// worker 命令
// ---------------------------------------------------------------------------

static void do_try(const cmd_t *cmd)
{
    ESP_LOGI(TAG, "验证新 Wi-Fi: %s", cmd->ssid);
    status_set_try(APP_TRY_RUNNING, cmd->ssid);
    s_trying = true;
    disconnect_and_settle();

    bool ok = false;
    if (apply_config(cmd->ssid, cmd->pass) == ESP_OK) {
        status_set_wifi(APP_WIFI_CONNECTING, cmd->ssid);
        for (int i = 0; i < TRY_ATTEMPTS && !ok; i++) {
            xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED);
            if (esp_wifi_connect() != ESP_OK) break;
            EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED,
                                                   pdTRUE, pdFALSE,
                                                   pdMS_TO_TICKS(TRY_ATTEMPT_MS));
            ok = (bits & BIT_GOT_IP) != 0;
            if (!ok && !(bits & BIT_DISCONNECTED)) esp_wifi_disconnect();
        }
    }
    s_trying = false;

    if (ok) {
        esp_err_t err = creds_save(cmd->ssid, cmd->pass);
        if (err != ESP_OK) ESP_LOGE(TAG, "保存凭据失败: %s", esp_err_to_name(err));
        strlcpy(s_ssid, cmd->ssid, sizeof(s_ssid));
        strlcpy(s_pass, cmd->pass, sizeof(s_pass));
        s_forgotten = false;
        s_retry_ms = RETRY_MIN_MS;
        status_set_try(APP_TRY_OK, NULL);
        ESP_LOGI(TAG, "新 Wi-Fi 验证成功并已保存");
        return;
    }

    ESP_LOGW(TAG, "新 Wi-Fi 验证失败,恢复原配置");
    status_set_try(APP_TRY_FAILED, NULL);
    disconnect_and_settle();
    connect_saved();
}

static void do_forget(void)
{
    s_forgotten = true;
    disconnect_and_settle();
    esp_err_t err = creds_save(NULL, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "清除凭据失败: %s", esp_err_to_name(err));
    s_ssid[0] = s_pass[0] = '\0';
    status_set_wifi(APP_WIFI_NO_CONFIG, "");
}

static void do_scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    uint16_t n = WIFI_STA_SCAN_MAX;
    static wifi_ap_record_t recs[WIFI_STA_SCAN_MAX];
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        n = 0;
    }
    taskENTER_CRITICAL(&s_scan_lock);
    memcpy(s_scan, recs, sizeof(recs[0]) * n);
    s_scan_count = n;
    s_scan_busy = false;
    taskEXIT_CRITICAL(&s_scan_lock);
}

static bool wifi_connected(void)
{
    app_status_t st;
    app_status_get(&st);
    return st.wifi == APP_WIFI_CONNECTED;
}

static void on_disconnected(void)
{
    if (s_trying || s_forgotten || s_ssid[0] == '\0' || wifi_connected()) return;
    status_set_wifi(APP_WIFI_RETRYING, NULL);
    s_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(s_retry_ms);
    s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
    s_retry_armed = true;
}

static void worker(void *arg)
{
    (void)arg;
    connect_saved();
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_retry_armed) {
            TickType_t now = xTaskGetTickCount();
            wait = (int32_t)(s_retry_at - now) > 0 ? s_retry_at - now : 0;
        }
        cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, wait) != pdTRUE) {
            cmd.type = CMD_RECONNECT;
            s_retry_armed = false;
        }
        switch (cmd.type) {
        case CMD_RECONNECT:
            if (!s_forgotten && !wifi_connected()) connect_saved();
            break;
        case CMD_DISCONNECTED:
            on_disconnected();
            break;
        case CMD_TRY:
            do_try(&cmd);
            break;
        case CMD_FORGET:
            do_forget();
            break;
        case CMD_SCAN:
            do_scan();
            break;
        }
        memset(&cmd, 0, sizeof(cmd));   // 不在栈上残留密码
    }
}

// ---------------------------------------------------------------------------
// 事件(运行于默认事件循环任务,只做轻量操作)
// ---------------------------------------------------------------------------

// 断开后立即清掉"已连接"状态,否则 worker 会误以为仍在线而不重连。
static bool st_wifi_was_connected(void)
{
    return wifi_connected() && !s_trying;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "STA 断开, reason=%d", ev->reason);
        app_status_t *st = app_status_lock();
        st->last_reason = ev->reason;
        app_status_unlock();
        if (st_wifi_was_connected()) status_set_wifi(APP_WIFI_RETRYING, NULL);
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
        // 退避重连交给 worker,事件循环里不做等待。
        cmd_t cmd = { .type = CMD_DISCONNECTED };
        (void)xQueueSend(s_queue, &cmd, 0);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *ev = data;
    esp_netif_dns_info_t dns = { 0 };
    esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    wifi_ap_record_t ap = { 0 };
    esp_wifi_sta_get_ap_info(&ap);

    app_status_t *st = app_status_lock();
    st->wifi = APP_WIFI_CONNECTED;
    st->sta_ip = ev->ip_info.ip.addr;
    st->sta_gw = ev->ip_info.gw.addr;
    st->sta_dns = dns.ip.u_addr.ip4.addr;
    st->rssi = ap.rssi;
    if (ap.ssid[0]) strlcpy(st->ssid, (const char *)ap.ssid, sizeof(st->ssid));
    app_status_unlock();

    s_retry_ms = RETRY_MIN_MS;
    xEventGroupSetBits(s_events, BIT_GOT_IP);
    ESP_LOGI(TAG, "STA 已获取 IP " IPSTR, IP2STR(&ev->ip_info.ip));
}

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------

static esp_err_t post(const cmd_t *cmd)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_queue, cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_request_connect(const char *ssid, const char *password)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    size_t sl = strlen(ssid), pl = strlen(password);
    if (sl == 0 || sl > 32 || pl > 64 || (pl > 0 && pl < 8)) return ESP_ERR_INVALID_ARG;
    cmd_t cmd = { .type = CMD_TRY };
    memcpy(cmd.ssid, ssid, sl);
    memcpy(cmd.pass, password, pl);
    status_set_try(APP_TRY_RUNNING, ssid);
    esp_err_t err = post(&cmd);
    memset(&cmd, 0, sizeof(cmd));
    if (err != ESP_OK) status_set_try(APP_TRY_IDLE, NULL);
    return err;
}

esp_err_t wifi_sta_request_forget(void)
{
    cmd_t cmd = { .type = CMD_FORGET };
    return post(&cmd);
}

esp_err_t wifi_sta_request_scan(void)
{
    if (s_scan_busy) return ESP_OK;
    s_scan_busy = true;
    cmd_t cmd = { .type = CMD_SCAN };
    esp_err_t err = post(&cmd);
    if (err != ESP_OK) s_scan_busy = false;
    return err;
}

size_t wifi_sta_scan_results(wifi_ap_record_t *out, size_t max, bool *busy)
{
    taskENTER_CRITICAL(&s_scan_lock);
    size_t n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, sizeof(out[0]) * n);
    if (busy) *busy = s_scan_busy;
    taskEXIT_CRITICAL(&s_scan_lock);
    return n;
}

esp_err_t wifi_sta_start(void)
{
    if (s_netif) return ESP_ERR_INVALID_STATE;

    s_queue = xQueueCreate(4, sizeof(cmd_t));
    s_events = xEventGroupCreate();
    if (!s_queue || !s_events) return ESP_ERR_NO_MEM;

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              on_wifi_event, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  on_ip_event, NULL, NULL);
    }
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    // 作为网卡使用时关闭省电,降低延迟、提高吞吐。
    if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) return err;

    creds_load();
    if (xTaskCreate(worker, "wifi_sta", 4096, NULL, 6, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
