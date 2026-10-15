#!/usr/bin/env bash
# 编译并运行 tests/ 下与 ESP-IDF 无关的主机测试。
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
CC="${CC:-cc}"
CFLAGS=(-std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$root/main")

"$CC" "${CFLAGS[@]}" "$root/main/link_cmd.c" "$root/tests/test_link_cmd.c" -o "$out/test_link_cmd"
"$CC" "${CFLAGS[@]}" "$root/main/form_parse.c" "$root/tests/test_form_parse.c" -o "$out/test_form_parse"

"$out/test_link_cmd"
"$out/test_form_parse"
