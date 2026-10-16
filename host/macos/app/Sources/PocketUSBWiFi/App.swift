// 菜单栏应用入口与面板界面。
import SwiftUI

@main
struct PocketUSBWiFiApp: App {
    @StateObject private var conn = Connection()

    init() {
        // 开发用:PocketUSBWiFi --snapshot <目录>,把各状态的面板渲染成 PNG 后退出。
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count {
            renderSnapshots(to: URL(fileURLWithPath: args[i + 1]))
            exit(0)
        }
    }

    var body: some Scene {
        MenuBarExtra {
            PanelView(conn: conn)
        } label: {
            Image(systemName: conn.menuBarSymbol)
        }
        .menuBarExtraStyle(.window)
    }
}

extension Connection {
    var menuBarSymbol: String {
        switch phase {
        case .noDevice: return "cable.connector.slash"
        case .ready, .failed: return "cable.connector"
        case .connecting: return "arrow.triangle.2.circlepath"
        case .connected: return "antenna.radiowaves.left.and.right"
        }
    }
}

struct PanelView: View {
    @ObservedObject var conn: Connection

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            if case .connected = conn.phase { linkInfo }
            if case .failed(let msg) = conn.phase { notice(msg, color: .red) }
            ForEach(conn.warnings, id: \.self) { notice($0, color: .orange) }
            primaryButton
            Button {
                conn.openConfigPage()
            } label: {
                Label(L("打开配置页（设置 Wi-Fi）", "Open setup page (Wi-Fi)"), systemImage: "safari")
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .disabled(!isConnected)
            Divider()
            HStack {
                Button(L("查看日志", "View log")) { conn.openLog() }
                Spacer()
                Button(L("退出", "Quit")) {
                    conn.disconnect()
                    NSApp.terminate(nil)
                }
            }
            .buttonStyle(.borderless)
            .font(.callout)
        }
        .padding(16)
        .frame(width: 320)
    }

    private var isConnected: Bool {
        if case .connected = conn.phase { return true }
        return false
    }

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: conn.menuBarSymbol)
                .font(.title2)
                .foregroundStyle(headerColor)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(L("随身 USB 网卡", "Pocket USB Wi-Fi")).font(.headline)
                Text(phaseText).font(.callout).foregroundStyle(.secondary)
            }
        }
    }

    private var headerColor: Color {
        switch conn.phase {
        case .connected: return .green
        case .connecting: return .blue
        case .failed: return .red
        default: return .secondary
        }
    }

    private var phaseText: String {
        switch conn.phase {
        case .noDevice: return L("未检测到 AI Passport，请用数据线连接", "AI Passport not found. Connect it with a data cable.")
        case .ready: return L("已检测到设备，可以连接", "Device found, ready to connect")
        case .connecting: return L("正在拨号…", "Dialing…")
        case .connected(let iface, let ours):
            return ours ? L("已连接（\(iface)）", "Connected (\(iface))")
                        : L("已连接（\(iface)，由其他程序建立）", "Connected (\(iface), started elsewhere)")
        case .failed: return L("连接失败", "Connection failed")
        }
    }

    @ViewBuilder private var linkInfo: some View {
        if let s = conn.status {
            VStack(alignment: .leading, spacing: 6) {
                row("wifi", s.wifi == "connected" ? "\(s.ssid)  \(s.rssi) dBm" : wifiStateText(s.wifi))
                row("arrow.down", formatRate(conn.downRate))
                row("arrow.up", formatRate(conn.upRate))
            }
            .font(.callout)
        }
    }

    private func wifiStateText(_ state: String) -> String {
        switch state {
        case "no_config": return L("未配置 Wi-Fi", "Wi-Fi not configured")
        case "connecting", "retrying": return L("正在连接 Wi-Fi…", "Connecting to Wi-Fi…")
        default: return state
        }
    }

    private func row(_ symbol: String, _ text: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: symbol).frame(width: 18).foregroundStyle(.secondary)
            Text(text).monospacedDigit()
        }
    }

    private func wide(_ title: String) -> some View {
        Text(title).frame(maxWidth: .infinity)
    }

    private func notice(_ text: String, color: Color) -> some View {
        HStack(alignment: .top, spacing: 6) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(color)
            Text(text).font(.callout).fixedSize(horizontal: false, vertical: true)
        }
    }

    @ViewBuilder private var primaryButton: some View {
        switch conn.phase {
        case .connected(_, let ours):
            Button { conn.disconnect() } label: { wide(L("断开", "Disconnect")) }
                .controlSize(.large)
                .disabled(!ours)
        case .connecting:
            Button { conn.disconnect() } label: { wide(L("取消", "Cancel")) }
                .controlSize(.large)
        default:
            Button { conn.connect() } label: { wide(L("连接", "Connect")) }
                .controlSize(.large).keyboardShortcut(.defaultAction)
                .disabled(conn.port == nil)
        }
    }
}

func formatRate(_ bytesPerSec: Double) -> String {
    bytesPerSec >= 1024 * 1024
        ? String(format: "%.1f MB/s", bytesPerSec / 1024 / 1024)
        : String(format: "%.1f KB/s", bytesPerSec / 1024)
}

@MainActor
private func renderSnapshots(to dir: URL) {
    let st = DeviceStatus(wifi: "connected", ssid: "Cafe-WiFi", rssi: -52, sta_ip: "192.168.1.23",
                          link: "up", dns: "192.168.1.1", usb_rx: 0, usb_tx: 0, try_state: "idle", try_ssid: "")
    let port = "/dev/cu.usbmodem1101"
    let cases: [(String, Connection)] = [
        ("no-device", Connection(preview: .noDevice, port: nil, status: nil, warnings: [])),
        ("ready", Connection(preview: .ready, port: port, status: nil, warnings: [])),
        ("connecting", Connection(preview: .connecting, port: port, status: nil, warnings: [])),
        ("connected", Connection(preview: .connected(iface: "ppp0", ours: true), port: port, status: st, warnings: [])),
        ("warning", Connection(preview: .connected(iface: "ppp0", ours: true), port: port, status: st, warnings: [
            L("检测到 VPN/代理（utun4）。请完全退出代理软件，否则网页可能打不开。",
              "VPN/proxy detected (utun4). Quit it completely, or web pages may not load."),
        ])),
        ("failed", Connection(preview: .failed("30 秒内未建立 PPP 链路。检查设备屏幕上的 USB 状态。"),
                              port: port, status: nil, warnings: [])),
    ]
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    for (name, conn) in cases {
        let view = NSHostingView(rootView: PanelView(conn: conn)
            .background(Color(nsColor: .windowBackgroundColor)))
        view.frame.size = view.fittingSize
        view.layoutSubtreeIfNeeded()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { continue }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?
            .write(to: dir.appendingPathComponent("\(name).png"))
    }
}
