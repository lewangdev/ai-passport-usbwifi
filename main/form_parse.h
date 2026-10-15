// main/form_parse.h —— application/x-www-form-urlencoded 解析与 JSON 转义(纯 C,可主机测试)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 从 body 中取出 key 对应的值并 URL 解码到 out(含 '\0')。
// 找不到、解码错误或超出 out_size 返回 false。
bool form_get_field(const char *body, const char *key, char *out, size_t out_size);

// 把 in 写成 JSON 字符串内容(不含两侧引号)。返回写入长度;空间不足时截断
// 在完整的转义序列边界,保证 out 始终以 '\0' 结尾。
size_t json_escape(const char *in, char *out, size_t out_size);
