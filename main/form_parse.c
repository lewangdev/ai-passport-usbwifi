// main/form_parse.c —— 见 form_parse.h。
#include "form_parse.h"

#include <stdio.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool form_get_field(const char *body, const char *key, char *out, size_t out_size)
{
    if (!body || !key || !out || out_size == 0) return false;
    size_t key_len = strlen(key);
    const char *p = body;

    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *v = p + key_len + 1;
            size_t used = 0;
            while (v < end) {
                char c = *v++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (end - v < 2) return false;
                    int hi = hex_value(v[0]), lo = hex_value(v[1]);
                    if (hi < 0 || lo < 0) return false;
                    c = (char)(hi * 16 + lo);
                    v += 2;
                    if (c == '\0') return false;   // 拒绝内嵌 NUL
                }
                if (used + 1 >= out_size) return false;
                out[used++] = c;
            }
            out[used] = '\0';
            return true;
        }
        if (*end == '\0') break;
        p = end + 1;
    }
    return false;
}

size_t json_escape(const char *in, char *out, size_t out_size)
{
    size_t used = 0;
    if (out_size == 0) return 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        char tmp[7];
        size_t n;
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)*p;
            n = 2;
        } else if (*p < 0x20) {
            n = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
        } else {
            tmp[0] = (char)*p;
            n = 1;
        }
        if (used + n >= out_size) break;
        memcpy(out + used, tmp, n);
        used += n;
    }
    out[used] = '\0';
    return used;
}
