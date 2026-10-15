#!/usr/bin/env bash
# 通过 AI Passport(USB 串口 + PPP)让 Linux 电脑上网。
# 用法: sudo ./ai-passport-connect.sh [/dev/ttyACMx]
# Ctrl+C 断开。依赖: pppd(Debian/Ubuntu: apt install ppp)。
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

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

if systemctl is-active --quiet ModemManager 2>/dev/null; then
    echo "提示: ModemManager 正在运行,可能会抢占串口。建议安装 99-ai-passport.rules 后重新插拔设备。" >&2
fi

echo "使用串口 $port"
pppd "$port" 115200 \
    nodetach noauth local nocrtscts lock \
    noipdefault ipcp-accept-local ipcp-accept-remote \
    defaultroute usepeerdns \
    novj nobsdcomp nodeflate noaccomp nopcomp \
    lcp-echo-interval 5 lcp-echo-failure 4 \
    persist maxfail 0 holdoff 2 \
    ipparam ai-passport &
pppd_pid=$!
trap 'kill $pppd_pid 2>/dev/null; wait $pppd_pid 2>/dev/null; echo "已断开"' INT TERM EXIT

# 等待 PPP 接口拿到地址,然后配置 DNS(systemd-resolved 系统需要手动设置)。
for _ in $(seq 1 30); do
    iface="$(ip -o -4 addr show | awk '$4 ~ /^10\.77\.0\.2\// {print $2; exit}')"
    [[ -n "$iface" ]] && break
    sleep 1
done
if [[ -z "${iface:-}" ]]; then
    echo "30 秒内未建立 PPP 链路,继续重试中…(检查设备屏幕上的 USB 状态)" >&2
else
    echo "已连接: $iface 10.77.0.2 -> 10.77.0.1"
    dns="$(awk '/^nameserver/ {print $2}' /etc/ppp/resolv.conf 2>/dev/null | tr '\n' ' ')"
    if [[ -n "$dns" ]] && command -v resolvectl >/dev/null; then
        # shellcheck disable=SC2086
        resolvectl dns "$iface" $dns && resolvectl domain "$iface" '~.' || true
    fi
    echo "DNS: ${dns:-未知}    配置页: http://10.77.0.1/"
fi
wait $pppd_pid
