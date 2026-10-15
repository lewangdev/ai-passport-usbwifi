// tests/test_form_parse.c —— form_parse 的主机测试。
#include "form_parse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_basic_fields(void)
{
    char v[64];
    assert(form_get_field("ssid=Home+Net&pass=abc%2612345", "ssid", v, sizeof(v)));
    assert(strcmp(v, "Home Net") == 0);
    assert(form_get_field("ssid=Home+Net&pass=abc%2612345", "pass", v, sizeof(v)));
    assert(strcmp(v, "abc&12345") == 0);
}

static void test_key_prefix_not_confused(void)
{
    char v[64];
    assert(form_get_field("ssid2=x&ssid=y", "ssid", v, sizeof(v)));
    assert(strcmp(v, "y") == 0);
    assert(!form_get_field("ssid2=x", "ssid", v, sizeof(v)));
}

static void test_empty_value(void)
{
    char v[8] = "zz";
    assert(form_get_field("ssid=a&pass=", "pass", v, sizeof(v)));
    assert(v[0] == '\0');
}

static void test_utf8_ssid(void)
{
    char v[64];
    assert(form_get_field("ssid=%E5%AE%B6", "ssid", v, sizeof(v)));
    assert(strcmp(v, "\xE5\xAE\xB6") == 0);
}

static void test_rejects_bad_input(void)
{
    char v[4];
    assert(!form_get_field("ssid=abcd", "ssid", v, sizeof(v)));     // 恰好放不下 '\0'
    assert(form_get_field("ssid=abc", "ssid", v, sizeof(v)));
    assert(!form_get_field("ssid=%4", "ssid", v, sizeof(v)));
    assert(!form_get_field("ssid=%zz", "ssid", v, sizeof(v)));
    assert(!form_get_field("ssid=%00", "ssid", v, sizeof(v)));
}

static void test_json_escape(void)
{
    char out[64];
    json_escape("a\"b\\c\n", out, sizeof(out));
    assert(strcmp(out, "a\\\"b\\\\c\\u000a") == 0);

    char small[5];
    json_escape("ab\"c", small, sizeof(small));   // 转义序列不被截断成半个
    assert(strcmp(small, "ab\\\"") == 0);
    json_escape("abc\"", small, sizeof(small));
    assert(strcmp(small, "abc") == 0);
}

int main(void)
{
    test_basic_fields();
    test_key_prefix_not_confused();
    test_empty_value();
    test_utf8_ssid();
    test_rejects_bad_input();
    test_json_escape();
    printf("test_form_parse: PASS\n");
    return 0;
}
