// main/web_cfg.h —— 通过 USB 链路提供的本地配置页(Wi-Fi 配网、状态、日志)。
// 仅接受目的地址为 USB_LINK_LOCAL_IP 的连接;Wi-Fi 侧访问一律 403。
#pragma once

#include "esp_err.h"

esp_err_t web_cfg_start(void);
