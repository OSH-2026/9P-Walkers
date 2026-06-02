# ray_start_servers.ps1 —— 报告 3.2 节：启动 llama-server HTTP 服务
# 在每个节点起一个常驻 server，供 Ray 分发 /completion 请求。
# 资源受限时可在同一台机上用不同端口起 2 个 server 模拟“多节点”。
#
# 用法:
#   # 节点1 / 实例1
#   pwsh -File .\scripts\ray_start_servers.ps1 -Port 8080 -Threads 8
#   # 同机模拟节点2 / 实例2(另开一个终端)
#   pwsh -File .\scripts\ray_start_servers.ps1 -Port 8081 -Threads 8
#
param(
    [int]$Port = 8080,
    [int]$Threads = 8,
    [string]$BindHost = "0.0.0.0",
    [int]$CtxSize = 4096
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\common.ps1"
Assert-File $LlamaServer
Assert-File $Model

Write-Host "本机局域网 IP:" -ForegroundColor Cyan
Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    Select-Object IPAddress | Format-Table -AutoSize

Write-Host "启动 llama-server  http://$BindHost`:$Port  (t=$Threads, c=$CtxSize)" -ForegroundColor Green
& $LlamaServer -m $Model --host $BindHost --port $Port -t $Threads -c $CtxSize
