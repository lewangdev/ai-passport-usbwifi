// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "PocketUSBWiFi",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(name: "PocketUSBWiFi", path: "Sources/PocketUSBWiFi"),
    ]
)
