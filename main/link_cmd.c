// main/link_cmd.c —— 见 link_cmd.h。
#include "link_cmd.h"

#include <ctype.h>
#include <string.h>

#define CONNECT_BANNER "\r\nCONNECT 115200\r\n"

static void reply_append(link_cmd_result_t *r, const char *text)
{
    size_t n = strlen(text);
    size_t room = sizeof(r->reply) - r->reply_len;
    if (n > room) n = room;
    memcpy(r->reply + r->reply_len, text, n);
    r->reply_len += n;
}

static void result_reset(link_cmd_result_t *r)
{
    memset(r, 0, sizeof(*r));
}

void link_cmd_init(link_cmd_t *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->echo = true;
}

// 不区分大小写比较前缀。
static bool starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

// 当前半行是否为 "CLIENT" 的(非空)前缀。
static bool is_client_prefix(const link_cmd_t *cmd)
{
    static const char client[] = "CLIENT";
    if (cmd->len == 0 || cmd->len > sizeof(client) - 1) return false;
    for (size_t i = 0; i < cmd->len; i++) {
        if (toupper((unsigned char)cmd->line[i]) != client[i]) return false;
    }
    return true;
}

// 扫描 AT 命令串里的 E0/E1(如 "ATZE0V1"),只关心回显开关。
static void apply_echo_flags(link_cmd_t *cmd, const char *body)
{
    for (const char *p = body; *p; p++) {
        if (toupper((unsigned char)*p) != 'E') continue;
        if (p[1] == '0') cmd->echo = false;
        else if (p[1] == '1') cmd->echo = true;
    }
}

static void handle_line(link_cmd_t *cmd, link_cmd_result_t *r)
{
    const char *line = cmd->line;

    // Windows 直接电缆连接握手。
    if (starts_with(line, "CLIENT")) {
        reply_append(r, "CLIENTSERVER");
        r->action = LINK_CMD_ENTER_PPP;
        return;
    }
    if (!starts_with(line, "AT")) {
        r->action = r->reply_len ? LINK_CMD_REPLY : LINK_CMD_NONE;
        return;
    }

    const char *body = line + 2;
    if (toupper((unsigned char)body[0]) == 'D') {
        // ATD<号码> / ATDT / ATDP:任何号码都视为"拨通"。
        reply_append(r, CONNECT_BANNER);
        r->action = LINK_CMD_ENTER_PPP;
        return;
    }
    if (toupper((unsigned char)body[0]) == 'O' && body[1] == '\0') {
        // ATO:回到在线(数据)模式。
        reply_append(r, CONNECT_BANNER);
        r->action = LINK_CMD_ENTER_PPP;
        return;
    }
    if (toupper((unsigned char)body[0]) == 'I') {
        reply_append(r, "\r\nAI Passport USB-WiFi\r\n");
    }
    apply_echo_flags(cmd, body);
    reply_append(r, "\r\nOK\r\n");
    r->action = LINK_CMD_REPLY;
}

void link_cmd_feed(link_cmd_t *cmd, uint8_t byte, link_cmd_result_t *r)
{
    result_reset(r);

    if (byte == LINK_PPP_FLAG) {
        // 主机直接开始 PPP 协商。丢弃半行命令,交给 PPP。
        cmd->len = 0;
        cmd->overflow = false;
        r->action = LINK_CMD_ENTER_PPP;
        r->ppp_frame_start = true;
        return;
    }

    if (byte == '\r' || byte == '\n') {
        if (cmd->echo && byte == '\r' && !is_client_prefix(cmd)) {
            r->reply[r->reply_len++] = '\r';
        }
        if (cmd->len == 0 || cmd->overflow) {
            cmd->len = 0;
            cmd->overflow = false;
            r->action = r->reply_len ? LINK_CMD_REPLY : LINK_CMD_NONE;
            return;
        }
        cmd->line[cmd->len] = '\0';
        handle_line(cmd, r);
        cmd->len = 0;
        return;
    }

    if (byte == 0x08 || byte == 0x7F) {   // 退格
        if (cmd->len) cmd->len--;
    } else if (byte >= 0x20 && byte < 0x7F) {
        if (cmd->len + 1 < sizeof(cmd->line)) {
            cmd->line[cmd->len++] = (char)byte;
        } else {
            cmd->overflow = true;
        }
    }
    // "CLIENT" 握手对端只等 "CLIENTSERVER",不回显它的前缀。
    if (cmd->echo && byte >= 0x20 && byte < 0x7F && !is_client_prefix(cmd)) {
        r->reply[r->reply_len++] = (char)byte;
    }

    // "CLIENT" 握手不以回车结尾。
    if (!cmd->overflow && cmd->len == 6) {
        cmd->line[cmd->len] = '\0';
        if (starts_with(cmd->line, "CLIENT")) {
            handle_line(cmd, r);
            cmd->len = 0;
            return;
        }
    }

    r->action = r->reply_len ? LINK_CMD_REPLY : LINK_CMD_NONE;
}
