<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport USB Wi-Fi

Turn a [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) into a USB Wi-Fi adapter. The device joins your router over Wi-Fi and connects to a computer over a USB cable, so a computer with no working wireless card can still get online.

```text
 Router ))) Wi-Fi STA ── AI Passport (ESP32-C3) ── USB cable ── Computer
                         lwIP: NAT forwarding + PPP server       pppd / dial-up
                         10.77.0.1                               10.77.0.2
```

## Use cases

This is an **emergency connectivity tool you already carry**: a computer temporarily has no usable wireless card and you need it online now. Typical situations:

- **Missing Wi-Fi driver**: you have just installed Linux (or reinstalled Windows), the wireless card has no driver, and downloading the driver needs a network connection.
- **Broken or disabled wireless**: a laptop's Wi-Fi module has failed, or company policy has disabled wireless, and no Ethernet cable is at hand.
- **Machines that never had wireless**: desktops, mini PCs, industrial PCs, NAS boxes, or boards such as a Raspberry Pi that briefly need to install a package, sync time, or pull code.
- **Wi-Fi only, no Ethernet jack**: hotels, meeting rooms, or labs that provide only Wi-Fi, for a machine that has only wired Ethernet, or no network adapter at all.

Good for: downloading drivers, installing modest `apt`/`pip` packages, SSH, reading documentation, chat. Not good for: video, large downloads, or use as a permanent primary adapter.

Compared with alternatives:

| Option | Strengths | Weaknesses |
| --- | --- | --- |
| Phone USB tethering | Driverless and fast | Uses mobile data; many phones share only cellular, not Wi-Fi; iPhone needs iTunes drivers on Windows |
| USB Wi-Fi dongle | Fast | An extra item to carry; may also lack a Linux driver |
| **AI Passport (this project)** | Already in your pocket; the computer uses its built-in serial driver and PPP, available on Linux, macOS, and Windows; Wi-Fi is configured from the computer's browser, no phone required | Slow (full-speed USB serial); requires a script or dial-up connection, not plug-and-play |

Check these prerequisites in advance: Linux needs `pppd` (usually preinstalled on Ubuntu/Debian desktop, but possibly missing from minimal installs, so install it ahead of time). On Windows, the first use requires adding a modem and creating a dial-up connection, which takes about two minutes; do it once while you still have a network.

## Feasibility

**It works, but not as a driverless standard USB network adapter.**

The AI Passport uses an **ESP32-C3**, whose USB is a fixed-function **USB-Serial-JTAG** controller. In hardware it can only appear as a CDC-ACM serial port (plus JTAG). It has **no USB OTG**, so it cannot emulate RNDIS, CDC-ECM, or CDC-NCM network devices. Espressif's own `sta2eth` example (Wi-Fi to USB network adapter) supports only the ESP32-S2/S3.

The workable route is to run **PPP**, the protocol used for dial-up, over that serial port:

| Approach | Possible on ESP32-C3 | Computer side |
| --- | --- | --- |
| USB network adapter (RNDIS / ECM / NCM) | ❌ Not supported by hardware | Driverless |
| **USB serial + PPP + NAT (this project)** | ✅ | Linux/macOS run a `pppd` script; Windows uses a dial-up connection |
| USB serial + SLIP | ✅ | Works on Linux; poor support elsewhere |

## How it works

- **USB side**: USB-Serial-JTAG is no longer the log console; it carries PPP. On the ESP side, lwIP acts as a **PPP server**, assigning `10.77.0.2` to the computer and using `10.77.0.1` itself, and hands out DNS through IPCP (the router's DNS when available, otherwise `223.5.5.5`).
- **Dial-up compatibility**: before PPP starts, the firmware emulates a Hayes modem (`AT…`→`OK`, `ATD…`→`CONNECT`) and also supports the Windows direct-cable `CLIENT`→`CLIENTSERVER` handshake. If the computer sends PPP frames directly, the firmware switches immediately.
- **Forwarding**: lwIP `IP_FORWARD` and `IPV4_NAPT` are enabled, so the computer's IPv4 traffic is NATed out through the Wi-Fi STA interface.
- **Wi-Fi setup**: once PPP is up, open **http://10.77.0.1/** in the computer's browser to scan for and join a Wi-Fi network. New credentials are **verified before they are saved**; if the connection fails, the previous network is restored automatically, and the USB link stays up during the switch. The page accepts requests only from the USB link (other devices on the Wi-Fi LAN get 403), and state-changing requests require a custom header so other websites cannot change the configuration cross-site.
- **Logs**: once PPP owns the USB port there is no serial log, so logs go to a 4 KB in-memory ring buffer that can be viewed on the setup page.
- **Screen**: shows a Wi-Fi card (SSID / signal / IP), a USB card (link state / computer IP), live up/down rates, and battery level. Buttons: UP/DOWN adjust brightness, OK turns the screen on or off.

## Expected performance (not yet measured)

USB-Serial-JTAG is a USB full-speed (12 Mbit/s) device with 64-byte bulk packets, plus PPP's HDLC escaping overhead. **Throughput is expected to be in the range of a few hundred KB/s to about 1 MB/s**; the real figure needs measuring on hardware. Web browsing, text chat, and system updates are fine; HD video will struggle. NAT supports IPv4 only.

## Build and flash

Environment: ESP-IDF 5.5.3 (same as upstream AI Passport).

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p <port> flash        # afterwards the USB port carries PPP; idf.py monitor shows no logs
./tools/host_test.sh          # host unit tests (AT parser, form parsing)
```

You can preset a default Wi-Fi network under `idf.py menuconfig` → *AI Passport USB WiFi* (used only when no network has been saved through the setup page). Do not commit real passwords.

Flashing is unaffected: entering download mode over USB-Serial-JTAG is handled by hardware, regardless of whether the firmware is using the serial port.

## Using it from a computer

### Linux

```bash
sudo apt install ppp                                   # or: dnf install ppp
sudo cp host/linux/99-ai-passport.rules /etc/udev/rules.d/ && sudo udevadm control --reload
# Replug the device, then:
sudo host/linux/ai-passport-connect.sh                 # finds /dev/serial/by-id/*Espressif* automatically
```

The script handles the default route (when another default route exists, it adds a higher-priority one through PPP) and DNS (systemd-resolved if present, otherwise a temporary `/etc/resolv.conf`), restores both on Ctrl+C, and self-checks routing and name resolution after connecting.

The udev rule makes ModemManager ignore the device. Because the firmware answers AT commands, without the rule ModemManager may treat it as a modem and grab the serial port.

### macOS

Turn off any proxy/VPN and the Mac's Wi-Fi first, then run:

```bash
host/macos/ai-passport-connect.sh                      # uses the built-in /usr/sbin/pppd
```

The script registers the link as the system's primary network service so the system DNS switches over, prints the resulting "system DNS" for you to check, and undoes it on disconnect.

The built-in macOS `pppd` must be able to read `/etc/ppp/options`, which macOS does not ship; without it `pppd` exits immediately. The script creates an empty file automatically. If you run `pppd` by hand, run `sudo touch /etc/ppp/options` first.

### Windows (dial-up networking)

1. Device Manager shows a "USB Serial Device (COMx)".
2. Control Panel → Phone and Modem → Modems → Add → check "Don't detect my modem" → choose "Standard Modem Types / Standard 33600 bps Modem" → select port COMx.
3. Settings → Network & Internet → Dial-up → Set up a new connection → Connect to the Internet → Dial-up, choose that modem, enter `*99#` as the phone number, leave user name and password empty → Connect.

Windows configures DNS and the default gateway for the dial-up connection automatically (keep "Use default gateway on remote network" checked under connection Properties → Networking → IPv4 → Advanced).

## When you cannot get online

1. **Turn off proxies/VPNs.** TUN-mode proxies such as Clash, Shadowrocket, or Surge take over the default route and DNS (a common symptom is names resolving to `198.18.x.x`), so traffic never goes through AI Passport. Switching the proxy off is not enough; quit the app completely. On Windows, also turn off the system proxy.
2. **Disconnect the computer's existing network before connecting.** When a default route already exists, `pppd` does not move traffic to PPP. The Linux script handles this automatically; on macOS, turn off Wi-Fi before running the script.
3. **Read the script's self-check.** Linux prints "自检通过" (self-check passed) or the specific reason; macOS prints the "system DNS" next to the expected value.
4. **Run the diagnose script.** It checks the link, device Wi-Fi, routing, raw IP reachability, DNS, proxy settings, and the device log layer by layer; send its output to the developer:
   - macOS / Linux: `host/diagnose.sh`
   - Windows: `powershell -ExecutionPolicy Bypass -File host\windows\diagnose.ps1`

Symptoms and causes:

| Symptom | Cause |
| --- | --- |
| Setup page does not open, `ping 10.77.0.1` fails | The PPP link is not up; check the connect script or `pppd` output |
| Setup page opens, but IP addresses are unreachable | The device is not on Wi-Fi, or the default route is not on PPP (proxy/VPN, the computer's existing network) |
| IP addresses work, domain names do not | The system DNS did not switch (proxy leftovers, or the script's DNS setup did not take effect) |

## Known limitations

- Not a driverless adapter; the computer must run a script or a dial-up connection.
- IPv4 only; no IPv6 egress.
- Throughput is limited by the full-speed USB serial link (see above).
- Inbound connections to the computer are not possible (it is behind NAT).
- If the computer exits abnormally, the device returns to waiting for a dial-in only after the LCP keepalive times out (about 9–12 seconds).

## Verification status

```text
Build:        PASS (ESP-IDF 5.5.3, esp32c3)
Host tests:   PASS (tools/host_test.sh: link_cmd, form_parse)
Device tests: macOS end-to-end PASS: PPP handshake, Wi-Fi setup through the web
              page, NAT forwarding to the internet, DNS resolution; USB command
              mode answers correctly (AT→OK, ATE0, ATI)
Unverified:   real throughput, runtime free heap, long-run stability, item-by-item
              screen and button checks, Linux connect script, Windows dial-up flow
              and diagnose.ps1
```

## Layout

```text
main/
  main.c         startup order, buttons
  usb_link.c     USB-Serial-JTAG ⇄ PPP server, NAT enable, session restart
  link_cmd.c     pre-PPP AT / CLIENT handshake parser (pure C, host-tested)
  wifi_sta.c     STA connection, backoff reconnect, scan, verify-before-save
  web_cfg.c      http://10.77.0.1/ setup page and JSON API
  web_page.h     setup page HTML
  form_parse.c   form decoding and JSON escaping (pure C, host-tested)
  log_ring.c     log ring buffer
  ui.c           on-screen status page
  app_status.c   state shared between modules
components/bsp/  board support package from upstream AI Passport (unmodified)
host/            computer-side connect and diagnose scripts (diagnose.sh, windows/diagnose.ps1)
tests/           host unit tests
```

The BSP, partition table, and `dependencies.lock` are copied from upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) (MIT). The screen UI is designed for this application and does not reuse the upstream demo's menu shell.
