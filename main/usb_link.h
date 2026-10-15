// main/usb_link.h —— 把 ESP32-C3 的 USB-Serial-JTAG(主机侧看到的 CDC-ACM 串口)
// 变成一条 PPP 点对点链路,并对主机流量做 NAT 转发到 Wi-Fi STA。
//
// 本端地址 USB_LINK_LOCAL_IP,主机地址 USB_LINK_PEER_IP。PPP 建立前支持 AT 拨号/
// CLIENT 握手(见 link_cmd.h),便于 Windows 拨号网络接入。
#pragma once

#include "esp_err.h"
#include <stdint.h>

#define USB_LINK_LOCAL_IP_STR  "10.77.0.1"
#define USB_LINK_PEER_IP_STR   "10.77.0.2"

// 安装 USB-Serial-JTAG 驱动、创建 PPP netif 并开始等待主机。
// 前置条件:esp_netif_init、默认事件循环已就绪;控制台不得占用 USB-Serial-JTAG。
esp_err_t usb_link_start(void);

// 本端 PPP 地址(网络字节序)。
uint32_t usb_link_local_ip(void);

// 最近一次 PPP 会话下发给主机的首选 DNS(网络字节序)。
uint32_t usb_link_host_dns(void);

// USB 链路累计收发字节数(含 PPP 开销)。
void usb_link_counters(uint32_t *rx_bytes, uint32_t *tx_bytes, uint32_t *tx_drops);
