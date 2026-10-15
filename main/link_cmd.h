// main/link_cmd.h —— USB 串口"命令模式"解析器(纯 C,不依赖 ESP-IDF,可主机测试)。
//
// PPP 链路建立前,主机可能先发:
//   - Hayes AT 命令(Windows 拨号网络的"标准调制解调器"会发 ATZ / ATE0 / ATD...);
//   - "CLIENT"(Windows 直接电缆连接握手,期望回 "CLIENTSERVER");
//   - 直接发 PPP 帧(Linux/macOS 上 pppd 不带 chat 脚本时),首字节为 0x7E。
// 本模块逐字节喂入,决定回复内容以及何时切换到 PPP 模式。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINK_CMD_LINE_MAX   64
#define LINK_CMD_REPLY_MAX  96

#define LINK_PPP_FLAG       0x7E

typedef enum {
    LINK_CMD_NONE = 0,     // 继续接收,无需动作
    LINK_CMD_REPLY,        // 发送 reply
    LINK_CMD_ENTER_PPP,    // 发送 reply(可能为空),随后进入 PPP 模式
} link_cmd_action_t;

typedef struct {
    char line[LINK_CMD_LINE_MAX];
    size_t len;
    bool overflow;
    bool echo;             // ATE0/ATE1,默认开启(与 Hayes 一致)
} link_cmd_t;

typedef struct {
    link_cmd_action_t action;
    // ENTER_PPP 且 ppp_frame_start 为真:触发字节本身就是 PPP 帧首,
    // 调用方必须把它连同后续字节一起交给 PPP 协议栈,不能丢弃。
    bool ppp_frame_start;
    char reply[LINK_CMD_REPLY_MAX];
    size_t reply_len;
} link_cmd_result_t;

void link_cmd_init(link_cmd_t *cmd);

// 喂入 1 个字节。result 每次都会被完整重写。
void link_cmd_feed(link_cmd_t *cmd, uint8_t byte, link_cmd_result_t *result);
