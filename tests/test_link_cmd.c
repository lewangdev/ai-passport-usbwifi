// tests/test_link_cmd.c —— link_cmd 命令模式解析器的主机测试。
#include "link_cmd.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 逐字节喂入 input,收集全部回复到 out;返回最后一个非 NONE 动作。
static link_cmd_action_t feed_str(link_cmd_t *cmd, const char *input, char *out,
                                  size_t out_size, bool *frame_start)
{
    link_cmd_action_t last = LINK_CMD_NONE;
    size_t used = 0;
    out[0] = '\0';
    if (frame_start) *frame_start = false;
    for (const char *p = input; *p; p++) {
        link_cmd_result_t r;
        link_cmd_feed(cmd, (uint8_t)*p, &r);
        assert(used + r.reply_len < out_size);
        memcpy(out + used, r.reply, r.reply_len);
        used += r.reply_len;
        out[used] = '\0';
        if (r.action != LINK_CMD_NONE) last = r.action;
        if (r.action == LINK_CMD_ENTER_PPP) {
            if (frame_start) *frame_start = r.ppp_frame_start;
            assert(p[1] == '\0');   // 切换后不应再有命令字节
        }
    }
    return last;
}

static void test_at_ok_with_echo(void)
{
    link_cmd_t cmd;
    char out[256];
    link_cmd_init(&cmd);
    assert(feed_str(&cmd, "AT\r", out, sizeof(out), NULL) == LINK_CMD_REPLY);
    assert(strcmp(out, "AT\r\r\nOK\r\n") == 0);
}

static void test_echo_off(void)
{
    link_cmd_t cmd;
    char out[256];
    link_cmd_init(&cmd);
    feed_str(&cmd, "ATZE0V1\r", out, sizeof(out), NULL);
    assert(!cmd.echo);
    assert(feed_str(&cmd, "AT&F\r", out, sizeof(out), NULL) == LINK_CMD_REPLY);
    assert(strcmp(out, "\r\nOK\r\n") == 0);
}

static void test_dial_enters_ppp(void)
{
    link_cmd_t cmd;
    char out[256];
    bool frame_start = true;
    link_cmd_init(&cmd);
    cmd.echo = false;
    assert(feed_str(&cmd, "ATDT*99#\r", out, sizeof(out), &frame_start) == LINK_CMD_ENTER_PPP);
    assert(!frame_start);
    assert(strstr(out, "CONNECT") != NULL);
}

static void test_lowercase_dial(void)
{
    link_cmd_t cmd;
    char out[256];
    link_cmd_init(&cmd);
    cmd.echo = false;
    assert(feed_str(&cmd, "atd123\r", out, sizeof(out), NULL) == LINK_CMD_ENTER_PPP);
}

static void test_client_handshake(void)
{
    link_cmd_t cmd;
    char out[256];
    link_cmd_init(&cmd);
    assert(feed_str(&cmd, "CLIENT", out, sizeof(out), NULL) == LINK_CMD_ENTER_PPP);
    assert(strcmp(out, "CLIENTSERVER") == 0);
}

static void test_ppp_flag_switches_immediately(void)
{
    link_cmd_t cmd;
    link_cmd_result_t r;
    link_cmd_init(&cmd);
    link_cmd_feed(&cmd, 'A', &r);
    link_cmd_feed(&cmd, LINK_PPP_FLAG, &r);
    assert(r.action == LINK_CMD_ENTER_PPP);
    assert(r.ppp_frame_start);
    assert(r.reply_len == 0);
    assert(cmd.len == 0);
}

static void test_overflow_line_is_dropped(void)
{
    link_cmd_t cmd;
    char out[1024];
    char longline[200];
    link_cmd_init(&cmd);
    cmd.echo = false;
    memset(longline, 'A', sizeof(longline) - 2);
    longline[sizeof(longline) - 2] = '\r';
    longline[sizeof(longline) - 1] = '\0';
    assert(feed_str(&cmd, longline, out, sizeof(out), NULL) == LINK_CMD_NONE);
    assert(out[0] == '\0');
    assert(feed_str(&cmd, "AT\r", out, sizeof(out), NULL) == LINK_CMD_REPLY);
    assert(strcmp(out, "\r\nOK\r\n") == 0);
}

static void test_garbage_is_ignored(void)
{
    link_cmd_t cmd;
    char out[256];
    link_cmd_init(&cmd);
    cmd.echo = false;
    assert(feed_str(&cmd, "hello\r\n", out, sizeof(out), NULL) == LINK_CMD_NONE);
    assert(out[0] == '\0');
}

int main(void)
{
    test_at_ok_with_echo();
    test_echo_off();
    test_dial_enters_ppp();
    test_lowercase_dial();
    test_client_handshake();
    test_ppp_flag_switches_immediately();
    test_overflow_line_is_dropped();
    test_garbage_is_ignored();
    printf("test_link_cmd: PASS\n");
    return 0;
}
