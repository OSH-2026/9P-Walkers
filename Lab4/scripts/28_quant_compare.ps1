# 28_quant_compare.ps1 —— 报告 2.8 节(选做)：不同量化格式对比 Q4/Q5/Q8
# 对比同模型不同量化的: 文件体积 / prefill / decode / 峰值内存。
# 质量评分请配合 25_quality.ps1 用不同 -Model -Tag 跑后人工打分。
#
# 准备(先把对应 gguf 下载到 models 下):
#   modelscope download --model Qwen/Qwen2.5-7B-Instruct-GGUF qwen2.5-7b-instruct-q5_k_m.gguf --local_dir .\llama.cpp\models
#   modelscope download --model Qwen/Qwen2.5-7B-Instruct-GGUF qwen2.5-7b-instruct-q8_0.gguf   --local_dir .\llama.cpp\models
#
# 用法:
#   pwsh -File .\scripts\28_quant_compare.ps1
#   pwsh -File .\scripts\28_quant_compare.ps1 -Models q4_k_m,q5_k_m,q8_0
#
param(
    [string[]]$Models = @("q4_k_m","q5_k_m","q8_0")   # 文件名中量化标识
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\common.ps1"
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"

$rows = foreach ($q in $Models) {
    $path = Join-Path $ModelDir "qwen2.5-7b-instruct-$q.gguf"
    if (-not (Test-Path $path)) {
        Write-Host "[跳过] 未找到 $path (请先下载该量化)" -ForegroundColor Yellow
        continue
    }
    Write-Host "`n=== 量化 $q ===" -ForegroundColor Cyan
    $sizeGiB = [math]::Round((Get-Item $path).Length / 1GB, 2)

    # 吞吐
    $benchMd = Join-Path $ResultsDir "28_${q}_bench_$stamp.md"
    Invoke-Bench -BenchArgs @("-m",$path,"-t","8") -SaveAs $benchMd

    # 峰值内存
    $a = @("-m",$path,"-p","Hello","-n","64","--threads","8","-st")
    $r = Invoke-WithPeakMemory -FilePath $LlamaCli -Arguments $a

    [pscustomobject]@{ 量化=$q; 体积GiB=$sizeGiB; 峰值内存MB=$r.PeakMB; "吞吐表"=(Split-Path $benchMd -Leaf) }
}

Write-Host "`n========== 量化对比汇总 ==========" -ForegroundColor Green
$rows | Format-Table -AutoSize
$csv = Join-Path $ResultsDir "28_quant_compare_$stamp.csv"
$rows | Export-Csv -NoTypeInformation -Encoding utf8 $csv
Write-Host "汇总: $csv  (prefill/decode 见各 *_bench_*.md)"
