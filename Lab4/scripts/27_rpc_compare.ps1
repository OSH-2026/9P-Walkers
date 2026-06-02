# 27_rpc_compare.ps1 —— 报告 2.6/2.7 节：单机 vs RPC 分布式推理对比
# 在【主机】执行。分别测“仅本机”和“本机+从机(RPC)”的加载时间/prefill/decode。
# 先确保从机已运行 26_rpc_worker.ps1。
#
# 用法(主机):
#   pwsh -File .\scripts\27_rpc_compare.ps1 -Rpc 192.168.1.20:50052
#   pwsh -File .\scripts\27_rpc_compare.ps1 -Rpc 192.168.1.20:50052 -Mode rpc
#
param(
    [string]$Rpc,                                   # 从机 ip:port, 例 192.168.1.20:50052
    [ValidateSet("single","rpc","both")]
    [string]$Mode = "both",
    [int]$NPredict = 128,
    [string]$Prompt = "请介绍分布式推理。"
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\common.ps1"

# RPC 构建的二进制(若存在则优先用，单机测试也用同一套，保证可比)
$rpcCli   = Join-Path $RpcBinDir "llama-cli.exe"
$rpcBench = Join-Path $RpcBinDir "llama-bench.exe"
$cli   = if (Test-Path $rpcCli)   { $rpcCli }   else { $LlamaCli }
$bench = if (Test-Path $rpcBench) { $rpcBench } else { $LlamaBench }
Assert-File $Model
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"

function Test-Config($label, $extraBenchArgs, $extraCliArgs) {
    Write-Host "`n========== [$label] ==========" -ForegroundColor Cyan
    Write-Host "-- llama-bench (pp512/tg128) --"
    $benchArgs = @("-m",$Model) + $extraBenchArgs
    & $bench @benchArgs -o md 2>&1 | Tee-Object -FilePath (Join-Path $ResultsDir "27_${label}_bench_$stamp.md")

    Write-Host "-- llama-cli (加载时间/TTFT, 看日志中 RPC backend 注册) --"
    $cliArgs = @("-m",$Model,"-p",$Prompt,"-n","$NPredict","--threads","8","-st","-v") + $extraCliArgs
    $errLog = Join-Path $ResultsDir "27_${label}_cli_$stamp.err.log"
    $r = Invoke-WithPeakMemory -FilePath $cli -Arguments $cliArgs -ErrLog $errLog
    $t = Parse-LlamaTimings $r.StdErr $r.Seconds
    [pscustomobject]@{ 配置=$label; 加载时间ms=$t.LoadMs; TTFTms=$t.PromptEvalMs; 峰值内存MB=$r.PeakMB }
}

$rows = @()
if ($Mode -in "single","both") {
    $rows += Test-Config "single" @() @()
}
if ($Mode -in "rpc","both") {
    if (-not $Rpc) { throw "RPC 模式需要 -Rpc <从机ip:port>" }
    Write-Host "`n使用 RPC 从机: $Rpc" -ForegroundColor Yellow
    $rows += Test-Config "rpc" @("--rpc",$Rpc) @("--rpc",$Rpc)
}

Write-Host "`n========== 单机 vs RPC 汇总 ==========" -ForegroundColor Green
$rows | Format-Table -AutoSize
$csv = Join-Path $ResultsDir "27_compare_$stamp.csv"
$rows | Export-Csv -NoTypeInformation -Encoding utf8 $csv
Write-Host "prefill/decode 见各 *_bench_*.md ; 汇总: $csv"
