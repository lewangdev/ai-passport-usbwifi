#!/usr/bin/env bash
# 通过 AI Passport(USB 串口 + PPP)让 Linux 电脑上网。
# 用法: sudo ./ai-passport-connect.sh [/dev/ttyACMx]
# Ctrl+C 断开,并恢复路由与 DNS。依赖: pppd(Debian/Ubuntu: apt install ppp)。
# 注意: 本脚本尚未在真机上验证,如遇问题请运行 host/diagnose.sh 并反馈输出。
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

DEV_IP=10.77.0.1
HOST_IP=10.77.0.2

port="${1:-}"
if [[ -z "$port" ]]; then
    # ESP32-C3 USB-Serial-JTAG: VID 303a PID 1001
    port="$(ls /dev/serial/by-id/*Espressif*USB_JTAG* 2>/dev/null | head -n1 || true)"
fi
if [[ -z "$port" || ! -e "$port" ]]; then
    echo "找不到 AI Passport 串口。请用数据线连接设备,或手动指定: $0 /dev/ttyACM0" >&2
    exit 1
fi
command -v pppd >/dev/null || { echo "未安装 pppd (apt install ppp / dnf install ppp)" >&2; exit 1; }

# 部分发行版的 ppp 包不带 /etc/ppp/options;某些 pppd 构建缺它时会拒绝启动。
if [[ ! -e /etc/ppp/options ]]; then
    mkdir -p /etc/ppp
    touch /etc/ppp/options
    echo "已创建空的 /etc/ppp/options"
fi

if systemctl is-active --quiet ModemManager 2>/dev/null; then
    echo "提示: ModemManager 正在运行,可能会抢占串口。建议安装 99-ai-passport.rules 后重新插拔设备。" >&2
fi

added_route=""
resolv_backup=""
iface=""

cleanup() {
    trap - INT TERM EXIT
    if [[ -n "$added_route" && -n "$iface" ]]; then
        ip route del default dev "$iface" metric 1 2>/dev/null || true
    fi
    if [[ -n "$resolv_backup" ]]; then
        # 原样恢复(包括原来是符号链接的情况)。
        rm -f /etc/resolv.conf
        mv "$resolv_backup" /etc/resolv.conf
    fi
    kill "${pppd_pid:-}" 2>/dev/null || true
    wait "${pppd_pid:-}" 2>/dev/null || true
    echo "已断开"
}
trap cleanup INT TERM EXIT

echo "使用串口 $port"
pppd "$port" 115200 \
    nodetach noauth local nocrtscts lock \
    noipdefault ipcp-accept-local ipcp-accept-remote \
    defaultroute usepeerdns \
    novj nobsdcomp nodeflate noaccomp nopcomp \
    lcp-echo-interval 5 lcp-echo-failure 4 \
    ipparam ai-passport &
pppd_pid=$!

# PPP 接口的地址形如 "inet 10.77.0.2 peer 10.77.0.1/32",本端地址后没有掩码。
for _ in $(seq 1 30); do
    if ! kill -0 "$pppd_pid" 2>/dev/null; then
        echo "pppd 已退出,请查看上面的 pppd 输出。" >&2
        exit 1
    fi
    iface="$(ip -o -4 addr show | awk -v ip="$HOST_IP" '{split($4, a, "/")} a[1] == ip {print $2; exit}')"
    [[ -n "$iface" ]] && break
    sleep 1
done
if [[ -z "$iface" ]]; then
    echo "30 秒内未建立 PPP 链路。检查设备屏幕上的 USB 状态,或运行 host/diagnose.sh。" >&2
    exit 1
fi
echo "已连接: $iface $HOST_IP -> $DEV_IP    配置页: http://$DEV_IP/"

# --- 默认路由 ---
# pppd 的 defaultroute 只在当时没有默认路由时生效。若已有(有线、代理/VPN 的 tun 等),
# 再加一条 metric 1 的默认路由优先走 AI Passport,断开时撤销。
if [[ "$(ip route get 223.5.5.5 2>/dev/null | grep -o "dev [^ ]*" | cut -d' ' -f2)" != "$iface" ]]; then
    if ip route add default dev "$iface" metric 1 2>/dev/null; then
        added_route=1
        echo "已有其他默认路由,已添加优先级更高的默认路由经 $iface"
    fi
fi

# --- DNS ---
dns="$(awk '/^nameserver/ {print $2}' /etc/ppp/resolv.conf 2>/dev/null | tr '\n' ' ' || true)"
if [[ -z "${dns// /}" ]]; then
    dns="$(curl -s --max-time 3 "http://$DEV_IP/api/status" | sed -n 's/.*"dns":"\([^"]*\)".*/\1/p' || true)"
fi
dns="${dns:-223.5.5.5}"
dns="${dns% } 8.8.8.8"

if command -v resolvectl >/dev/null && systemctl is-active --quiet systemd-resolved 2>/dev/null; then
    # shellcheck disable=SC2086
    resolvectl dns "$iface" $dns
    resolvectl domain "$iface" '~.'
    resolvectl default-route "$iface" true 2>/dev/null || true
    echo "DNS(systemd-resolved,$iface): $dns"
else
    resolv_backup="$(mktemp /etc/resolv.conf.ai-passport.XXXXXX)"
    rm -f "$resolv_backup"
    mv /etc/resolv.conf "$resolv_backup" 2>/dev/null || { resolv_backup=""; }
    {
        echo "# AI Passport 临时 DNS,断开时恢复原文件"
        for s in $dns; do echo "nameserver $s"; done
    } > /etc/resolv.conf
    echo "DNS(/etc/resolv.conf): $dns"
fi

# --- 自检 ---
route_dev="$(ip route get 223.5.5.5 2>/dev/null | grep -o "dev [^ ]*" | cut -d' ' -f2 || true)"
if [[ "$route_dev" != "$iface" ]]; then
    echo "!! 公网流量走的是 ${route_dev:-未知} 而不是 $iface。请关闭代理/VPN(tun 模式)后重新运行。" >&2
elif getent hosts www.baidu.com >/dev/null 2>&1; then
    echo "自检通过: 路由经 $iface,域名解析正常。"
else
    echo "!! 路由正常但域名解析失败。请确认设备已连上 Wi-Fi,或运行 host/diagnose.sh。" >&2
fi

wait "$pppd_pid"
