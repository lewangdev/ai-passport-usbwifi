#!/usr/bin/env bash
# AI Passport USB 网卡分层诊断(macOS / Linux)。连接脚本运行中时,在另一个终端执行:
#   ./host/diagnose.sh
# 把全部输出贴给开发者即可。不需要 root,不修改任何系统设置。
set -u

DEV=10.77.0.1
step() { printf '\n=== %s ===\n' "$1"; }

step "1. PPP 接口"
if command -v ip >/dev/null; then
    ip -o -4 addr show | grep 10.77.0. || echo "!! 没有 10.77.0.x 地址的接口:PPP 链路未建立"
else
    ifconfig | grep -B3 "inet 10.77.0." || echo "!! 没有 10.77.0.x 地址的接口:PPP 链路未建立"
fi

step "2. 设备状态 (Wi-Fi 是否已连接)"
curl -s --max-time 5 "http://$DEV/api/status" || echo "!! 无法访问设备配置接口"
echo

step "3. 到设备的连通性"
ping -c 2 "$DEV" 2>&1 | tail -2

step "4. 默认路由 (应指向 PPP 接口)"
if command -v ip >/dev/null; then
    ip route show default
    ip route get 223.5.5.5
else
    netstat -rn -f inet | awk 'NR<=4 || $1=="default"'
    route -n get 223.5.5.5 2>&1 | grep -E "gateway|interface"
fi

step "5. 公网 IP 连通 (绕过 DNS)"
ping -c 3 223.5.5.5 2>&1 | tail -2
ppp_if="$( (ip -o -4 addr show 2>/dev/null || true) | awk '/10\.77\.0\./ {print $2; exit}')"
[[ -z "$ppp_if" ]] && ppp_if="$(ifconfig 2>/dev/null | awk '/^ppp[0-9]+:/ {sub(":","",$1); i=$1} /inet 10\.77\.0\./ {print i; exit}')"
if [[ -n "$ppp_if" ]]; then
    echo "-- 强制走 $ppp_if 的 HTTP 请求:"
    curl -s -o /dev/null --max-time 8 --interface "$ppp_if" -w "HTTP %{http_code}, %{time_total}s\n" http://223.5.5.5/ \
        || echo "!! 经 $ppp_if 访问公网失败"
fi

step "6. DNS 解析"
if command -v dig >/dev/null; then
    dig +time=3 +tries=1 +short www.baidu.com || true
    echo "-- 直接问 223.5.5.5:"
    dig +time=3 +tries=1 +short @223.5.5.5 www.baidu.com || true
else
    nslookup www.baidu.com 2>&1 | tail -4
fi
[[ -f /etc/resolv.conf ]] && grep nameserver /etc/resolv.conf
command -v scutil >/dev/null && scutil --dns | grep -m4 nameserver

step "7. 设备日志 (最后 40 行)"
curl -s --max-time 5 "http://$DEV/api/log" | tail -40
