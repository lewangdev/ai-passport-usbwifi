// 连接状态机:定时检查设备、PPP 网卡和拨号脚本,并从设备 /api/status 读取 Wi-Fi 与流量。
import AppKit
import Foundation

let isChinese = Locale.preferredLanguages.first?.hasPrefix("zh") ?? false
func L(_ zh: String, _ en: String) -> String { isChinese ? zh : en }

struct DeviceStatus: Decodable {
    var wifi: String
    var ssid: String
    var rssi: Int
    var sta_ip: String
    var link: String
    var dns: String
    var usb_rx: UInt32
    var usb_tx: UInt32
    var try_state: String
    var try_ssid: String
}

@MainActor
final class Connection: ObservableObject {
    static let deviceIP = "10.77.0.1"
    static let hostIP = "10.77.0.2"
    static let configURL = URL(string: "http://\(deviceIP)/")!

    enum Phase: Equatable {
        case noDevice
        case ready
        case connecting
        case connected(iface: String, ours: Bool)
        case failed(String)
    }

    @Published private(set) var phase: Phase = .noDevice
    @Published private(set) var port: String?
    @Published private(set) var status: DeviceStatus?
    @Published private(set) var downRate: Double = 0   // 字节/秒,设备 → 电脑
    @Published private(set) var upRate: Double = 0
    @Published private(set) var warnings: [String] = []

    private var sessionPID: Int32?
    private var stopping = false
    private var lastCounters: (rx: UInt32, tx: UInt32, at: Date)?
    private var timer: Timer?
    private var fetching = false

    private let session: URLSession = {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.connectionProxyDictionary = [:]   // 绕过系统代理,直连设备
        cfg.timeoutIntervalForRequest = 2
        return URLSession(configuration: cfg)
    }()

    static let supportDir: URL = {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PocketUSBWiFi", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }()
    static let stopFile = supportDir.appendingPathComponent("stop")
    static let logFile: URL = {
        let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/PocketUSBWiFi", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("connect.log")
    }()

    /// 仅用于界面快照:固定状态,不轮询。
    init(preview phase: Phase, port: String?, status: DeviceStatus?, warnings: [String]) {
        self.phase = phase
        self.port = port
        self.status = status
        self.warnings = warnings
        downRate = status == nil ? 0 : 52_300
        upRate = status == nil ? 0 : 6_100
    }

    init() {
        tick()
        timer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }

    // MARK: - 操作

    func connect() {
        guard let port, sessionPID == nil else { return }
        guard let script = Bundle.main.path(forResource: "ai-passport-connect", ofType: "sh") else {
            phase = .failed(L("应用内缺少拨号脚本", "Dial script missing from app bundle"))
            return
        }
        try? FileManager.default.removeItem(at: Self.stopFile)
        try? Data().write(to: Self.logFile)
        do {
            sessionPID = try Privileged.launch(
                script: script, port: port,
                env: ["AIP_WATCH_PID": String(getpid()), "AIP_STOP_FILE": Self.stopFile.path],
                log: Self.logFile.path)
            stopping = false
            phase = .connecting
        } catch is Privileged.Cancelled {
            // 用户取消了密码框
        } catch let e as Privileged.Failed {
            phase = .failed(e.message)
        } catch {
            phase = .failed(error.localizedDescription)
        }
    }

    func disconnect() {
        guard sessionPID != nil else { return }
        stopping = true
        FileManager.default.createFile(atPath: Self.stopFile.path, contents: nil)
    }

    func openConfigPage() { NSWorkspace.shared.open(Self.configURL) }
    func openLog() { NSWorkspace.shared.open(Self.logFile) }

    // MARK: - 轮询

    private func tick() {
        port = DeviceFinder.findPort()
        let iface = NetInfo.pppInterface(hostIP: Self.hostIP)

        if let pid = sessionPID, !PIDCheck.isAlive(pid) {
            sessionPID = nil
            if !stopping && iface == nil && port != nil {
                phase = .failed(lastLogProblem() ?? L("拨号失败", "Dial-up failed"))
                resetLinkInfo()
                return
            }
        }

        if let iface {
            phase = .connected(iface: iface, ours: sessionPID != nil)
            refreshWarnings(iface: iface)
            fetchStatus()
        } else if sessionPID != nil {
            phase = .connecting
        } else {
            if case .failed = phase, port != nil { return }   // 保留错误信息直到用户重试
            phase = port == nil ? .noDevice : .ready
            resetLinkInfo()
        }
    }

    private func resetLinkInfo() {
        status = nil
        downRate = 0
        upRate = 0
        lastCounters = nil
        warnings = []
    }

    private func refreshWarnings(iface: String) {
        var w: [String] = []
        if let dev = NetInfo.routeInterface(to: "223.5.5.5"), dev != iface {
            w.append(L("上网流量走的是 \(dev) 而不是 AI Passport。请关闭 Mac 的 Wi-Fi 或其他网络后重新连接。",
                       "Traffic goes through \(dev), not AI Passport. Turn off the Mac's Wi-Fi or other network and reconnect."))
        }
        let tuns = NetInfo.tunInterfaces()
        if !tuns.isEmpty {
            w.append(L("检测到 VPN/代理（\(tuns.joined(separator: ", "))）。请完全退出代理软件，否则网页可能打不开。",
                       "VPN/proxy detected (\(tuns.joined(separator: ", "))). Quit it completely, or web pages may not load."))
        }
        if let text = try? String(contentsOf: Self.logFile, encoding: .utf8) {
            w += text.split(separator: "\n").filter { $0.hasPrefix("!!") }
                .map { $0.dropFirst(2).trimmingCharacters(in: .whitespaces) }
        }
        if let s = status, s.wifi != "connected" {
            w.append(s.wifi == "no_config"
                     ? L("设备还没有配置 Wi-Fi，请打开配置页。", "Wi-Fi is not configured yet. Open the setup page.")
                     : L("设备正在连接 Wi-Fi…", "The device is connecting to Wi-Fi…"))
        }
        warnings = w
    }

    private func fetchStatus() {
        guard !fetching else { return }
        fetching = true
        let url = URL(string: "http://\(Self.deviceIP)/api/status")!
        Task {
            defer { fetching = false }
            guard let (data, _) = try? await session.data(from: url),
                  let s = try? JSONDecoder().decode(DeviceStatus.self, from: data) else { return }
            let now = Date()
            if let last = lastCounters {
                let dt = now.timeIntervalSince(last.at)
                if dt > 0.2 {
                    downRate = Double(s.usb_tx &- last.tx) / dt
                    upRate = Double(s.usb_rx &- last.rx) / dt
                }
            }
            lastCounters = (s.usb_rx, s.usb_tx, now)
            status = s
        }
    }

    /// 脚本日志里最后一条问题提示,用于失败时展示。
    private func lastLogProblem() -> String? {
        guard let text = try? String(contentsOf: Self.logFile, encoding: .utf8) else { return nil }
        let lines = text.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }
        let keywords = ["!!", "失败", "找不到", "已退出", "未建立", "Can't", "error", "Error"]
        return lines.last { l in keywords.contains { l.contains($0) } } ?? lines.last
    }
}
