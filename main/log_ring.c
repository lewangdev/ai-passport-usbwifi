// main/log_ring.c —— 见 log_ring.h。
#include "log_ring.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_RING_SIZE  4096
#define LOG_LINE_MAX   160

static char s_ring[LOG_RING_SIZE];
static size_t s_head;      // 下一个写入位置
static bool s_wrapped;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static int ring_vprintf(const char *fmt, va_list args)
{
    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n <= 0) return n;
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;

    taskENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < len; i++) {
        s_ring[s_head++] = line[i];
        if (s_head == LOG_RING_SIZE) {
            s_head = 0;
            s_wrapped = true;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

void log_ring_install(void)
{
    esp_log_set_vprintf(ring_vprintf);
}

size_t log_ring_read(char *out, size_t max)
{
    taskENTER_CRITICAL(&s_lock);
    size_t total = s_wrapped ? LOG_RING_SIZE : s_head;
    size_t n = total < max ? total : max;
    // 取最新的 n 字节。
    size_t start = (s_head + LOG_RING_SIZE - n) % LOG_RING_SIZE;
    size_t first = LOG_RING_SIZE - start < n ? LOG_RING_SIZE - start : n;
    memcpy(out, s_ring + start, first);
    memcpy(out + first, s_ring, n - first);
    taskEXIT_CRITICAL(&s_lock);
    return n;
}
