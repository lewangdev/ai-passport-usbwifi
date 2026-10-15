# AI Passport USB 网卡分层诊断(Windows PowerShell 5+)。拨号连接建立后运行:
#   powershell -ExecutionPolicy Bypass -File host\windows\diagnose.ps1
# 把全部输出贴给开发者即可。只读,不修改任何系统设置。

$Dev = '10.77.0.1'
function Step($t) { Write-Host "`n=== $t ===" }

Step '1. 设备串口 (VID 303A / PID 1001)'
Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.PNPDeviceID -match 'VID_303A&PID_1001' } |
    Select-Object Name, Status | Format-Table -AutoSize
Get-CimInstance Win32_POTSModem |
    Select-Object Name, AttachedTo, Status | Format-Table -AutoSize

Step '2. PPP 地址'
$addr = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -like '10.77.0.*' }
if ($addr) { $addr | Select-Object InterfaceAlias, InterfaceIndex, IPAddress | Format-Table -AutoSize }
else { Write-Host '!! 没有 10.77.0.x 地址:拨号连接未建立' }

Step '3. 设备状态 (Wi-Fi 是否已连接)'
try { Invoke-RestMethod -Uri "http://$Dev/api/status" -TimeoutSec 5 | Format-List }
catch { Write-Host "!! 无法访问设备配置接口: $($_.Exception.Message)" }

Step '4. 到设备的连通性'
Test-Connection -ComputerName $Dev -Count 2 -ErrorAction SilentlyContinue |
    Select-Object Address, ResponseTime | Format-Table -AutoSize

Step '5. 公网路由 (应走 PPP 接口)'
Find-NetRoute -RemoteIPAddress 223.5.5.5 -ErrorAction SilentlyContinue |
    Select-Object -First 2 InterfaceAlias, DestinationPrefix, NextHop, RouteMetric | Format-Table -AutoSize

Step '6. 公网 IP 连通 (绕过 DNS)'
Test-Connection -ComputerName 223.5.5.5 -Count 3 -ErrorAction SilentlyContinue |
    Select-Object Address, ResponseTime | Format-Table -AutoSize

Step '7. DNS'
if ($addr) {
    Get-DnsClientServerAddress -InterfaceIndex $addr[0].InterfaceIndex -AddressFamily IPv4 |
        Select-Object InterfaceAlias, ServerAddresses | Format-Table -AutoSize
}
try { Resolve-DnsName www.baidu.com -Type A -DnsOnly -ErrorAction Stop | Select-Object -First 2 Name, IPAddress | Format-Table -AutoSize }
catch { Write-Host "!! 系统 DNS 解析失败: $($_.Exception.Message)" }
try { Resolve-DnsName www.baidu.com -Type A -Server 223.5.5.5 -DnsOnly -ErrorAction Stop | Select-Object -First 2 Name, IPAddress | Format-Table -AutoSize }
catch { Write-Host "!! 直接询问 223.5.5.5 也失败: $($_.Exception.Message)" }

Step '8. 系统代理 (浏览器会走这里)'
$inet = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings'
Write-Host "ProxyEnable=$($inet.ProxyEnable) ProxyServer=$($inet.ProxyServer) AutoConfigURL=$($inet.AutoConfigURL)"
if ($inet.ProxyEnable -eq 1 -or $inet.AutoConfigURL) { Write-Host '!! 系统代理已开启,请关闭代理软件后再测试' }

Step '9. 设备日志 (最后 40 行)'
try { ((Invoke-WebRequest -Uri "http://$Dev/api/log" -UseBasicParsing -TimeoutSec 5).Content -split "`n") | Select-Object -Last 40 }
catch { Write-Host "!! 无法读取日志: $($_.Exception.Message)" }
