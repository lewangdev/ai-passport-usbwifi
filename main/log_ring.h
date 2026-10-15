// main/log_ring.h —— USB-Serial-JTAG 被 PPP 占用后没有串口日志,
// 把 ESP_LOG 输出截留到内存环形缓冲,通过配置网页 /api/log 查看。
#pragma once

#include <stddef.h>

void log_ring_install(void);

// 按时间顺序复制最多 max 字节日志到 out(不含 '\0'),返回字节数。
size_t log_ring_read(char *out, size_t max);
