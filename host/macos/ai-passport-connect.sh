#!/usr/bin/env bash
# 通过 AI Passport(USB 串口 + PPP)让 macOS 上网。macOS 自带 /usr/sbin/pppd。
# 用法: ./ai-passport-connect.sh [/dev/cu.usbmodemXXXX]     Ctrl+C 断开。
# 菜单栏应用以 root 后台运行本脚本,并通过环境变量控制会话:
#   AIP_WATCH_PID  应用进程号,应用退出后自动断开
#   AIP_STOP_FILE  该文件出现时断开
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

port="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)}"
if [[ -z "$port" || ! -e "$port" ]]; then
    echo "找不到 AI Passport 串口 (/dev/cu.usbmodem*)。请用数据线连接设备,或手动指定。" >&2
    exit 1
fi

# macOS 自带的 pppd 启动时必须能读到 /etc/ppp/options,但系统默认不提供这个文件,
# 缺失时 pppd 会立即退出:"Can't open options file /etc/ppp/options"。
if [[ ! -e /etc/ppp/options ]]; then
    mkdir -p /etc/ppp
    touch /etc/ppp/options
    echo "已创建空的 /etc/ppp/options(macOS pppd 必需)"
fi

service="State:/Network/Service/ai-passport"
cleanup() {
    kill "${pppd_pid:-}" 2>/dev/null || true
    wait "${pppd_pid:-}" 2>/dev/null || true
    scutil <<SC >/dev/null 2>&1 || true
remove $service/IPv4
remove $service/DNS
SC
    echo "已断开"
}
trap cleanup INT TERM EXIT

echo "使用串口 $port"
/usr/sbin/pppd "$port" 115200 \
    nodetach noauth local nocrtscts \
    noipdefault ipcp-accept-local ipcp-accept-remote defaultroute \
    novj nobsdcomp nodeflate noaccomp nopcomp \
    lcp-echo-interval 5 lcp-echo-failure 4 &
pppd_pid=$!

# 菜单栏应用要求结束会话时返回 0。
stop_requested() {
    [[ -n "${AIP_WATCH_PID:-}" ]] && ! kill -0 "$AIP_WATCH_PID" 2>/dev/null && return 0
    [[ -n "${AIP_STOP_FILE:-}" && -e "$AIP_STOP_FILE" ]] && return 0
    return 1
}

iface=""
for _ in $(seq 1 30); do
    stop_requested && exit 0
    if ! kill -0 "$pppd_pid" 2>/dev/null; then
        echo "pppd 已退出,请查看上面的 pppd 输出。" >&2
        exit 1
    fi
    iface="$(ifconfig | awk '/^ppp[0-9]+:/ {sub(":", "", $1); i=$1} /inet 10\.77\.0\.2 / {print i; exit}')"
    [[ -n "$iface" ]] && break
    sleep 1
done
if [[ -z "$iface" ]]; then
    echo "30 秒内未建立 PPP 链路。检查设备屏幕上的 USB 状态。" >&2
    [[ -n "${AIP_WATCH_PID:-}" ]] && exit 1
    wait "$pppd_pid"
    exit 1
fi

# 向设备查询它通过 PPP 下发的 DNS(路由器 DNS 或公共 DNS 兜底)。
dns="$(curl -s --max-time 3 -H 'Host: 10.77.0.1' http://10.77.0.1/api/status \
      | sed -n 's/.*"dns":"\([^"]*\)".*/\1/p')"
dns="${dns:-223.5.5.5} 8.8.8.8"
# 向 configd 登记一个动态网络服务,写法参照 OpenConnect vpnc-script:
# 先写 DNS 再写 IPv4(IPv4 写入会触发重新选主);OverridePrimary 让它成为
# 首选服务,系统 DNS 才会改用这里的服务器。
scutil <<SC
d.init
d.add ServerAddresses * $dns
set $service/DNS
d.init
d.add Addresses * 10.77.0.2
d.add SubnetMasks * 255.255.255.255
d.add DestAddresses * 10.77.0.1
d.add InterfaceName $iface
d.add Router 10.77.0.1
d.add OverridePrimary # 1
set $service/IPv4
SC
echo "已连接: $iface 10.77.0.2 -> 10.77.0.1    配置页: http://10.77.0.1/"

# 确认系统首选 DNS 已切换。
sleep 1
first_ns="$(scutil --dns | awk '/^resolver #1/ {r=1} r && /nameserver\[0\]/ {print $3; exit}')"
echo "系统 DNS: ${first_ns:-未知}(期望: ${dns%% *})"
if [[ "$first_ns" != "${dns%% *}" ]]; then
    echo "!! 系统 DNS 没有切换过来。请确认已关闭代理/VPN 和 Mac 的 Wi-Fi,然后重新运行本脚本。" >&2
fi
if [[ -n "${AIP_WATCH_PID:-}" ]]; then
    while kill -0 "$pppd_pid" 2>/dev/null && ! stop_requested; do
        sleep 1
    done
    exit 0
fi
wait "$pppd_pid"
