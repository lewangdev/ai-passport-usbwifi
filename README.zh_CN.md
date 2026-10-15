<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport USB 网卡

把 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 变成一个“USB 无线网卡”：设备用 Wi-Fi 连路由器，再用 USB 线连电脑，让没有无线网卡的电脑也能上网。

```text
 路由器 ))) Wi-Fi STA ── AI Passport (ESP32-C3) ── USB 线 ── 电脑
                         lwIP: NAT 转发 + PPP 服务端         pppd / 拨号网络
                         10.77.0.1                           10.77.0.2
```

## 适用场景

它的定位是**随身带着的应急上网工具**：电脑暂时没有可用的无线网卡，又急需连一下网。比较典型的情况：

- **Wi-Fi 驱动缺失**：刚装好 Linux（或重装 Windows），无线网卡没驱动，偏偏下载驱动又要先联网。
- **无线网卡坏了或被禁用**：笔记本的 Wi-Fi 模块故障，或者公司电脑的无线被策略关掉了，手边也没有网线。
- **本来就没有无线的机器**：台式机、迷你主机、工控机、NAS、树莓派这类开发板，临时需要装个软件包、同步一下时间，或者拉个代码。
- **只有 Wi-Fi、没有网口**：酒店、会议室、实验室只提供 Wi-Fi，机器上只有有线网卡，或者干脆没有网卡。

适合的用途：下载驱动、`apt`/`pip` 装一些不大的包、SSH、查文档、收发消息。不适合：看视频、大文件下载、长期当主力网卡用。

和其他办法比一下：

| 办法 | 优点 | 缺点 |
| --- | --- | --- |
| 手机 USB 共享网络 | 免驱、快 | 消耗手机流量；有些手机只能共享蜂窝网络，不能共享 Wi-Fi；iPhone 在 Windows 上要装 iTunes 驱动 |
| USB 无线网卡 | 快 | 要专门带一个；在 Linux 上同样可能缺驱动 |
| **AI Passport（本项目）** | 本来就随身带着；电脑端用的是系统自带的串口驱动和 PPP，Linux/macOS/Windows 都有；配网直接在电脑浏览器里完成，不需要手机 | 慢（USB 全速串口）；要运行脚本或建拨号连接，不是即插即用 |

几个前提要先确认：Linux 上要有 `pppd`（Ubuntu/Debian 桌面版通常预装，最小化安装可能没有，可以提前装好）；Windows 第一次用要手动添加调制解调器、建拨号连接，大约 2 分钟，最好在有网的时候先配一次。

## 可行性结论

**能做，但做不成“即插即用的标准 USB 网卡”。**

AI Passport 的主控是 **ESP32-C3**，它的 USB 是固定功能的 **USB-Serial-JTAG** 控制器，硬件上只能当一个 CDC-ACM 串口（外加 JTAG），**没有 USB OTG**，因此不能模拟 RNDIS、CDC-ECM、CDC-NCM 这些网卡类设备。乐鑫官方的 `sta2eth` 示例（Wi-Fi 转 USB 网卡）只支持 ESP32-S2/S3。

可行的路线是在这个串口上跑 **PPP**，也就是拨号上网用的协议：

| 方案 | ESP32-C3 能否实现 | 电脑端 |
| --- | --- | --- |
| USB 网卡（RNDIS / ECM / NCM） | ❌ 硬件不支持 | 免驱 |
| **USB 串口 + PPP + NAT（本项目）** | ✅ | Linux/macOS 运行一个 `pppd` 脚本；Windows 建一个拨号连接 |
| USB 串口 + SLIP | ✅ | Linux 可用，其他系统支持差 |

## 工作原理

- **USB 侧**：USB-Serial-JTAG 不再当日志控制台，而是作为 PPP 数据通道。ESP 一侧用 lwIP 做 **PPP 服务端**，给电脑分配 `10.77.0.2`，自己用 `10.77.0.1`，并通过 IPCP 下发 DNS（优先用路由器的 DNS，没有时用 `223.5.5.5`）。
- **拨号兼容**：PPP 建立前，固件会模拟一个 Hayes 调制解调器（应答 `AT…`→`OK`，`ATD…`→`CONNECT`），也支持 Windows 直接电缆连接的 `CLIENT`→`CLIENTSERVER` 握手。主机如果直接发 PPP 帧，固件也会立即切换。
- **转发**：开启 lwIP 的 `IP_FORWARD` 与 `IPV4_NAPT`，电脑发出的 IPv4 流量经 NAT 从 Wi-Fi STA 出去。
- **配网**：PPP 连通后，在电脑浏览器打开 **http://10.77.0.1/**，可以扫描并连接 Wi-Fi。新密码会**先验证再保存**，连接失败会自动恢复原来的网络，切换期间 USB 链路不断。这个页面只接受来自 USB 链路的访问，Wi-Fi 局域网里的其他设备访问会返回 403；修改类请求还要求带一个自定义请求头，防止其他网站跨站篡改配置。
- **日志**：USB 口被 PPP 独占后就没有串口日志了，日志改存到 4 KB 内存环形缓冲区，可以在配置页里查看。
- **屏幕**：显示 Wi-Fi 卡片（SSID / 信号 / IP）、USB 卡片（链路状态 / 电脑 IP）、实时上下行速率和电量。按键：UP/DOWN 调亮度，OK 开关屏幕。

## 预期性能（未实测）

USB-Serial-JTAG 是 USB 全速（12 Mbit/s）设备，批量端点每包 64 字节，再加上 PPP 的 HDLC 转义开销，**实际吞吐预计在几百 KB/s 到 1 MB/s 量级**，具体数字需要在真机上测量。网页浏览、文字聊天、系统更新这类用途没问题，看高清视频会比较吃力。另外，NAT 只支持 IPv4。

## 构建与烧录

环境：ESP-IDF 5.5.3（与上游 AI Passport 一致）。

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p <端口> flash        # 烧录后 USB 口变为 PPP 通道，idf.py monitor 看不到日志
./tools/host_test.sh          # 主机单元测试（AT 解析器、表单解析）
```

可以在 `idf.py menuconfig` → *AI Passport USB WiFi* 里预置一个默认 Wi-Fi（只在没有通过网页保存过网络时生效）。真实密码不要提交到仓库。

烧录不受影响：USB-Serial-JTAG 进入下载模式由硬件完成，跟固件是否占用这个串口无关。

## 电脑端使用

### Linux

```bash
sudo apt install ppp                                   # 或 dnf install ppp
sudo cp host/linux/99-ai-passport.rules /etc/udev/rules.d/ && sudo udevadm control --reload
# 重新插拔设备，然后：
sudo host/linux/ai-passport-connect.sh                 # 自动查找 /dev/serial/by-id/*Espressif*
```

脚本会自动处理默认路由（已有其他默认路由时，加一条优先级更高的经 PPP）和 DNS（支持 systemd-resolved，否则临时改写 `/etc/resolv.conf`），按 Ctrl+C 断开时一并恢复，连上后会自检路由和域名解析。

装 udev 规则是为了让 ModemManager 忽略这个设备。因为固件会应答 AT 命令，不加规则的话 ModemManager 可能把它当成调制解调器并抢占串口。

### macOS

先关闭代理/VPN 和 Mac 的 Wi-Fi，再运行：

```bash
host/macos/ai-passport-connect.sh                      # 使用系统自带 /usr/sbin/pppd
```

脚本会把这条连接登记为系统首选网络服务，系统 DNS 随之切换，连上后打印"系统 DNS"供核对，断开时撤销。

macOS 自带的 `pppd` 启动时必须读取 `/etc/ppp/options`，系统默认没有这个文件，缺失时 `pppd` 会立即退出。脚本会自动创建一个空文件；手动运行 `pppd` 的话，先执行 `sudo touch /etc/ppp/options`。

### Windows（拨号网络）

1. 设备管理器里会出现“USB 串行设备 (COMx)”。
2. 控制面板 → 电话和调制解调器 → 调制解调器 → 添加 → 勾选“不要检测我的调制解调器” → 选“标准调制解调器类型 / 标准 33600 bps 调制解调器” → 端口选 COMx。
3. 设置 → 网络和 Internet → 拨号 → 设置新连接 → 连接到 Internet → 拨号，调制解调器选上一步那个，电话号码填 `*99#`，用户名和密码留空 → 连接。

拨号连接的 DNS 和默认网关由 Windows 自动配置（连接属性 → 网络 → IPv4 → 高级 → "在远程网络上使用默认网关"保持勾选）。

## 连不上网时

1. **关闭代理/VPN。** Clash、Shadowrocket、Surge 这类 TUN 模式的代理会占住默认路由和 DNS（常见表现是域名被解析成 `198.18.x.x`），流量根本不走 AI Passport。仅关闭代理开关不够，要彻底退出软件。Windows 上还要关掉系统代理。
2. **先断开电脑原有的网络，再连接。** 已有默认路由时，`pppd` 不会改走 PPP。Linux 脚本会自动处理；macOS 请先关 Wi-Fi 再运行脚本。
3. **看脚本输出的自检结果。** Linux 会打印"自检通过"或具体原因；macOS 会打印"系统 DNS"和期望值。
4. **运行诊断脚本，**它会逐层检查链路、设备 Wi-Fi、路由、直连 IP、DNS、代理和设备日志，把输出发给开发者即可：
   - macOS / Linux：`host/diagnose.sh`
   - Windows：`powershell -ExecutionPolicy Bypass -File host\windows\diagnose.ps1`

常见现象对照：

| 现象 | 原因 |
| --- | --- |
| 配置页打不开，`ping 10.77.0.1` 不通 | PPP 链路没建立，看连接脚本或 `pppd` 的输出 |
| 配置页能开，但 IP 也不通 | 设备没连上 Wi-Fi，或者默认路由不在 PPP 上（代理/VPN、电脑原有网络） |
| IP 能通，域名不行 | 系统 DNS 没切换（代理残留、或脚本的 DNS 配置没生效） |

## 已知限制

- 不是免驱网卡，电脑端需要运行脚本或建立拨号连接。
- 只转发 IPv4，没有 IPv6 出口。
- 吞吐受 USB 全速串口限制（见上文）。
- 外部无法主动连入电脑（NAT 之后）。
- 主机异常退出时，设备要等 LCP 心跳超时（约 9–12 秒）才会回到等待拨号状态。

## 验证状态

```text
Build:        PASS（ESP-IDF 5.5.3，esp32c3）
Host tests:   PASS（tools/host_test.sh：link_cmd、form_parse）
Device tests: macOS 端到端通过：PPP 握手、配置页配网、NAT 转发上网、DNS 解析；
              USB 命令模式应答正确（AT→OK、ATE0、ATI）
Unverified:   实际吞吐、运行时空闲堆、长时间稳定性、屏幕渲染与按键的逐项检查、
              Linux 连接脚本、Windows 拨号流程与 diagnose.ps1
```

## 目录

```text
main/
  main.c         启动顺序、按键
  usb_link.c     USB-Serial-JTAG ⇄ PPP 服务端，NAT 开启，会话重建
  link_cmd.c     PPP 前的 AT / CLIENT 握手解析（纯 C，有主机测试）
  wifi_sta.c     STA 连接、退避重连、扫描、先验证后保存
  web_cfg.c      http://10.77.0.1/ 配置页与 JSON API
  web_page.h     配置页 HTML
  form_parse.c   表单解码与 JSON 转义（纯 C，有主机测试）
  log_ring.c     日志环形缓冲
  ui.c           屏幕状态页
  app_status.c   模块间共享状态
components/bsp/  来自上游 AI Passport 的板级支持包（未修改）
host/            电脑端连接脚本与诊断脚本（diagnose.sh、windows/diagnose.ps1）
tests/           主机单元测试
```

BSP、分区表和 `dependencies.lock` 复制自上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)（MIT），屏幕 UI 是为这个应用重新设计的，没有沿用上游 demo 的菜单外壳。
