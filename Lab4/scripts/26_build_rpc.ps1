# 26_build_rpc.ps1 —— 报告 2.6 节：启用 RPC 重新构建 llama.cpp
# 在 build-rpc 目录生成带 rpc-server / 支持 --rpc 的二进制。
# 两台机器都需要执行本脚本(或至少从机需要 rpc-server.exe)。
#
# 用法:
#   pwsh -File .\scripts\26_build_rpc.ps1
#
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\common.ps1"
Push-Location $LlamaDir
try {
    Write-Host "配置 CMake (-DGGML_RPC=ON) ..." -ForegroundColor Cyan
    cmake -B build-rpc -DGGML_RPC=ON
    Write-Host "编译 Release ..." -ForegroundColor Cyan
    cmake --build build-rpc --config Release -j
} finally {
    Pop-Location
}
$rpcSrv = Join-Path $RpcBinDir "rpc-server.exe"
if (Test-Path $rpcSrv) {
    Write-Host "`n成功: $rpcSrv" -ForegroundColor Green
} else {
    Write-Host "`n未找到 rpc-server.exe，请检查编译输出。" -ForegroundColor Red
}
