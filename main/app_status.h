// main/app_status.h —— 各模块共享的运行状态(Wi-Fi、USB 链路),供 UI 和网页读取。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_WIFI_NO_CONFIG = 0,   // 尚未配置 Wi-Fi
    APP_WIFI_CONNECTING,
    APP_WIFI_CONNECTED,       // 已拿到 IP
    APP_WIFI_RETRYING,        // 断开,等待重连
} app_wifi_state_t;

typedef enum {
    APP_LINK_NO_USB = 0,      // 没有收到主机 USB SOF(未插电脑或只接了充电器)
    APP_LINK_WAITING,         // USB 已连接,等待主机拨号/PPP
    APP_LINK_NEGOTIATING,     // PPP 协商中
    APP_LINK_UP,              // PPP 已建立,主机可上网
} app_link_state_t;

typedef enum {
    APP_TRY_IDLE = 0,
    APP_TRY_RUNNING,          // 正在验证新的 Wi-Fi 凭据
    APP_TRY_OK,
    APP_TRY_FAILED,
} app_try_state_t;

typedef struct {
    app_wifi_state_t wifi;
    char ssid[33];
    int8_t rssi;
    uint8_t last_reason;      // 最近一次 STA 断开原因(wifi_err_reason_t)
    uint32_t sta_ip;          // 网络字节序,0 表示无
    uint32_t sta_gw;
    uint32_t sta_dns;

    app_try_state_t try_state;
    char try_ssid[33];

    app_link_state_t link;
    uint32_t host_ip;         // 主机在 PPP 链路上的地址
} app_status_t;

// 取当前状态快照。任何任务均可调用。
void app_status_get(app_status_t *out);

// 进入临界区并返回可写状态;修改完必须立即 app_status_unlock()。
// 两者之间只允许赋值/拷贝,不得阻塞或调用其他 FreeRTOS/ESP-IDF API。
app_status_t *app_status_lock(void);
void app_status_unlock(void);

const char *app_wifi_state_name(app_wifi_state_t s);
const char *app_link_state_name(app_link_state_t s);
