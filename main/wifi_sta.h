// main/wifi_sta.h —— 上游 Wi-Fi(STA):开机自动连接、断线重连、扫描、
// 以及"先验证再保存"的新凭据切换。所有慢操作在内部 worker 任务中执行。
#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>
#include <stddef.h>

#define WIFI_STA_SCAN_MAX 12

// 创建 STA netif、初始化并启动 Wi-Fi。调用前须完成 nvs_flash_init、
// esp_netif_init 与默认事件循环创建。若已保存凭据则自动连接。
esp_err_t wifi_sta_start(void);

// 异步:用新凭据尝试连接;成功后写入 NVS,失败则恢复旧凭据。
// 结果见 app_status_t.try_state。password 可为空串(开放网络)。
esp_err_t wifi_sta_request_connect(const char *ssid, const char *password);

// 异步:清除已保存凭据并断开。
esp_err_t wifi_sta_request_forget(void);

// 异步:发起一次扫描。
esp_err_t wifi_sta_request_scan(void);

// 复制最近一次扫描结果,返回条数;busy 回填扫描是否仍在进行。
size_t wifi_sta_scan_results(wifi_ap_record_t *out, size_t max, bool *busy);
