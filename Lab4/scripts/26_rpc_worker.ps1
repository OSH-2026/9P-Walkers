# 26_rpc_worker.ps1 —— 报告 2.6 节：在【从机】上启动 rpc-server
# 在从机执行；监听端口等待主机连接。会打印本机局域网 IP 方便填到主机命令里。
#
# 用法(从机):
#   pwsh -File .\scripts\26_rpc_worker.ps1
#   pwsh -File .\scripts\26_rpc_worker.ps1 -Port 50052
#
param(
    [int]$Port = 50052,
    [string]$BindHost = "0.0.0.0"
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\common.ps1"
$rpcSrv = Join-Path $RpcBinDir "rpc-server.exe"
Assert-File $rpcSrv "请先运行 26_build_rpc.ps1 完成 RPC 构建"

Write-Host "本机局域网 IP(填到主机 --rpc 参数):" -ForegroundColor Cyan
Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    Select-Object IPAddress, InterfaceAlias | Format-Table -AutoSize

Write-Host "启动 rpc-server  $BindHost`:$Port ..." -ForegroundColor Green
& $rpcSrv -H $BindHost -p $Port
