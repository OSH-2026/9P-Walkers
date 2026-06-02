# 23_metrics.ps1 —— 报告 2.3 节：核心指标测量
# 测量: ① 加载时间  ② 首Token延迟(TTFT)  ③ Prefill吞吐(pp512)
#       ④ Decode吞吐(tg128)  ⑤ 峰值内存
#
# 用法:
#   pwsh -File .\scripts\23_metrics.ps1
#   pwsh -File .\scripts\23_metrics.ps1 -Threads 8 -NPredict 128
#
param(
    [int]$Threads  = 8,     # 生成阶段线程数(用于 cli 单跑)
    [int]$NPredict = 128,   # 生成 token 数
    [string]$Prompt = "What is a large language model? Explain in detail."
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\common.ps1"
Assert-File $Model "请先用 modelscope 下载模型到 llama.cpp\models\"

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"

# --- A. llama-bench: prefill(pp512) 与 decode(tg128) 吞吐 ---
Write-Host "`n[1/2] llama-bench: pp512 / tg128 ..." -ForegroundColor Cyan
$benchMd = Join-Path $ResultsDir "23_bench_$stamp.md"
Invoke-Bench -BenchArgs @("-m",$Model,"-t","$Threads") -SaveAs $benchMd

# --- B. llama-cli: 加载时间 / TTFT / 峰值内存 ---
Write-Host "`n[2/2] llama-cli: 加载时间 / TTFT / 峰值内存 ..." -ForegroundColor Cyan
$cliOut = Join-Path $ResultsDir "23_cli_$stamp.out.log"
$cliErr = Join-Path $ResultsDir "23_cli_$stamp.err.log"
# -v 让 llama-cli 把 prompt eval / eval / total time 打到 stderr，便于解析 TTFT
$cliArgs = @("-m", $Model, "-p", $Prompt, "-n", "$NPredict", "--threads", "$Threads", "-st", "-v")
$r = Invoke-WithPeakMemory -FilePath $LlamaCli -Arguments $cliArgs -OutLog $cliOut -ErrLog $cliErr
$t = Parse-LlamaTimings $r.StdErr $r.Seconds

Write-Host "`n========== 2.3 指标汇总 ==========" -ForegroundColor Green
$summary = [pscustomobject]@{
    "加载时间约(ms)"      = $t.LoadMs     # 近似 = 墙钟 - 推理总时长
    "首Token延迟TTFT(ms)" = $t.PromptEvalMs
    "推理总时长(ms)"      = $t.TotalMs
    "峰值内存(MB)"        = $r.PeakMB
    "墙钟耗时(s)"         = $r.Seconds
    "线程数"              = $Threads
}
$summary | Format-List
Write-Host "Prefill/Decode 吞吐见上方 llama-bench 表 (pp512 / tg128)。"
Write-Host "原始日志: $cliErr"

$summaryCsv = Join-Path $ResultsDir "23_summary_$stamp.csv"
$summary | Export-Csv -NoTypeInformation -Encoding utf8 $summaryCsv
Write-Host "汇总已保存: $summaryCsv" -ForegroundColor Green
