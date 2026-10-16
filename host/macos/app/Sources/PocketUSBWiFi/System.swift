// 与系统打交道的部分:找设备串口、查网卡地址和路由、以管理员权限启动拨号脚本。
import Darwin
import Foundation
import IOKit
import IOKit.serial

/// 按 USB VID/PID 查找 AI Passport(ESP32-C3 USB-Serial-JTAG)的串口。
enum DeviceFinder {
    static let vendorID = 0x303A
    static let productID = 0x1001

    static func findPort() -> String? {
        guard let match = IOServiceMatching(kIOSerialBSDServiceValue) else { return nil }
        var iter: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(iter) }
        let opts = IOOptionBits(kIORegistryIterateRecursively | kIORegistryIterateParents)
        while true {
            let svc = IOIteratorNext(iter)
            if svc == 0 { return nil }
            defer { IOObjectRelease(svc) }
            let vid = IORegistryEntrySearchCFProperty(svc, kIOServicePlane, "idVendor" as CFString,
                                                      kCFAllocatorDefault, opts) as? Int
            let pid = IORegistryEntrySearchCFProperty(svc, kIOServicePlane, "idProduct" as CFString,
                                                      kCFAllocatorDefault, opts) as? Int
            guard vid == vendorID, pid == productID else { continue }
            if let path = IORegistryEntryCreateCFProperty(svc, kIOCalloutDeviceKey as CFString,
                                                          kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? String {
                return path
            }
        }
    }
}

enum NetInfo {
    /// 所有 IPv4 地址,(网卡名, 地址)。
    static func ipv4Addresses() -> [(name: String, ip: String)] {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }
        var out: [(String, String)] = []
        for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard let sa = p.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count),
                           nil, 0, NI_NUMERICHOST) == 0 {
                out.append((String(cString: p.pointee.ifa_name), String(cString: host)))
            }
        }
        return out
    }

    /// 本机 PPP 地址所在的网卡,如 ppp0。
    static func pppInterface(hostIP: String) -> String? {
        ipv4Addresses().first { $0.ip == hostIP }?.name
    }

    /// 带 IPv4 地址的 utun 网卡,通常意味着 VPN 或代理软件的 TUN 模式。
    static func tunInterfaces() -> [String] {
        ipv4Addresses().filter { $0.name.hasPrefix("utun") }.map(\.name)
    }

    /// 去往某个公网地址时系统选用的网卡。
    static func routeInterface(to ip: String) -> String? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/sbin/route")
        p.arguments = ["-n", "get", ip]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        for line in String(decoding: data, as: UTF8.self).split(separator: "\n") {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("interface:") {
                return t.dropFirst("interface:".count).trimmingCharacters(in: .whitespaces)
            }
        }
        return nil
    }
}

enum PIDCheck {
    /// 进程是否存在。root 进程对普通用户返回 EPERM,同样说明存在。
    static func isAlive(_ pid: Int32) -> Bool {
        kill(pid, 0) == 0 || errno == EPERM
    }
}

enum Privileged {
    struct Cancelled: Error {}
    struct Failed: Error { let message: String }

    /// 通过系统管理员密码框,以 root 在后台运行拨号脚本,返回脚本进程号。
    static func launch(script: String, port: String, env: [String: String], log: String) throws -> Int32 {
        let envPart = env.sorted { $0.key < $1.key }.map { "\($0.key)=\(shellQuote($0.value))" }.joined(separator: " ")
        let cmd = "\(envPart) /usr/bin/nohup /bin/bash \(shellQuote(script)) \(shellQuote(port))"
            + " >>\(shellQuote(log)) 2>&1 </dev/null & echo $!"
        let source = "do shell script \"\(appleScriptEscape(cmd))\" with administrator privileges"
        var error: NSDictionary?
        guard let result = NSAppleScript(source: source)?.executeAndReturnError(&error) else {
            if (error?[NSAppleScript.errorNumber] as? Int) == -128 { throw Cancelled() }
            throw Failed(message: error?[NSAppleScript.errorMessage] as? String ?? "unknown error")
        }
        guard let pid = Int32(result.stringValue?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "") else {
            throw Failed(message: "unexpected output: \(result.stringValue ?? "")")
        }
        return pid
    }

    static func shellQuote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    static func appleScriptEscape(_ s: String) -> String {
        s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
    }
}
