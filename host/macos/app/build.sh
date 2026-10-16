#!/usr/bin/env bash
# 构建菜单栏应用:输出 build/Pocket USB Wi-Fi.app 和 build/PocketUSBWiFi-<版本>.dmg。
# 用法: ./build.sh [版本号]      需要 Xcode 命令行工具(swift)。
# 默认 ad-hoc 签名;设置 SIGN_IDENTITY 可改用指定证书,例如 Developer ID。
set -euo pipefail
cd "$(dirname "$0")"

version="${1:-0.1.0}"
app="build/Pocket USB Wi-Fi.app"
dmg="build/PocketUSBWiFi-$version.dmg"

swift build -c release --arch arm64 --arch x86_64
bin="$(swift build -c release --arch arm64 --arch x86_64 --show-bin-path)/PocketUSBWiFi"

rm -rf "$app" "$dmg"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/PocketUSBWiFi"
sed "s/__VERSION__/$version/g" Resources/Info.plist > "$app/Contents/Info.plist"
cp Resources/AppIcon.icns "$app/Contents/Resources/"
# 拨号逻辑与命令行脚本共用同一份。
cp ../ai-passport-connect.sh "$app/Contents/Resources/"

codesign --force --deep --options runtime --sign "${SIGN_IDENTITY:--}" "$app"
codesign --verify --strict "$app"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
cp -R "$app" "$stage/"
ln -s /Applications "$stage/Applications"
hdiutil create -quiet -volname "Pocket USB Wi-Fi" -srcfolder "$stage" -format UDZO "$dmg"
echo "已生成: $app"
echo "已生成: $dmg"
