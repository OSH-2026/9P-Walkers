# 24_tune.ps1 —— 报告 2.4 节：部署参数调优(单变量扫描)
# 子项:
#   threads   线程数对 decode 的影响        (llama-bench -t)
#   batch     批大小对 prefill 的影响        (llama-bench -b)
#   ctx       上下文长度对峰值内存的影响     (llama-cli -c, 测内存)
#   mmap      内存映射开关对加载时间的影响   (llama-bench -mmp)
#   ngl       GPU offload 层数对 decode 的影响(llama-bench -ngl, 需 CUDA 构建)
#   all       依次执行除 ngl 外的全部
#
# 用法:
#   pwsh -File .\scripts\24_tune.ps1 -Test threads
#   pwsh -File .\scripts\24_tune.ps1 -Test all
#   pwsh -File .\scripts\24_tune.ps1 -Test ngl     # 仅当 CUDA 构建可用
#
param(
    [ValidateSet("threads","batch","ctx","mmap","ngl","all")]
    [string]$Test = "all"
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\common.ps1"
Assert-File $Model
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"

function Run-ThreadSweep {
    Write-Host "`n=== (1) 线程数扫描 -t 4,8,16,24,32 (看 tg/decode) ===" -ForegroundColor Cyan
    $md = Join-Path $ResultsDir "24_threads_$stamp.md"
    Invoke-Bench -BenchArgs @("-m",$Model,"-t","4,8,16,24,32") -SaveAs $md
}
function Run-BatchSweep {
    Write-Host "`n=== (2) 批大小扫描 -b 128,256,512,1024 (看 pp/prefill) ===" -ForegroundColor Cyan
    $md = Join-Path $ResultsDir "24_batch_$stamp.md"
    Invoke-Bench -BenchArgs @("-m",$Model,"-b","128,256,512,1024") -SaveAs $md
}
function Run-CtxSweep {
    Write-Host "`n=== (3) 上下文长度对峰值内存的影响 -c 512/2048/8192/16384 ===" -ForegroundColor Cyan
    $rows = foreach ($c in 512,2048,8192,16384) {
        Write-Host "  -c $c ..."
        $a = @("-m",$Model,"-p","Hello","-n","32","-c","$c","--threads","8","-st")
        $r = Invoke-WithPeakMemory -FilePath $LlamaCli -Arguments $a
        [pscustomobject]@{ ctx=$c; PeakMB=$r.PeakMB }
    }
    $rows | Format-Table -AutoSize
    $csv = Join-Path $ResultsDir "24_ctx_$stamp.csv"
    $rows | Export-Csv -NoTypeInformation -Encoding utf8 $csv
    Write-Host "  -> $csv"
}
function Run-MmapSweep {
    Write-Host "`n=== (4) 内存映射开关对加载时间的影响 -mmp 0,1 ===" -ForegroundColor Cyan
    $md = Join-Path $ResultsDir "24_mmap_$stamp.md"
    # -mmp 1 = 开启mmap(默认), 0 = --no-mmap
    Invoke-Bench -BenchArgs @("-m",$Model,"-mmp","0,1") -SaveAs $md
    Write-Host "  (补充: llama-cli 的 'load time' 也可对比 --no-mmap 开/关)"
}
function Run-NglSweep {
    Write-Host "`n=== (5) GPU offload 层数扫描 -ngl 0,10,20,33 (需 CUDA 构建) ===" -ForegroundColor Cyan
    $md = Join-Path $ResultsDir "24_ngl_$stamp.md"
    Invoke-Bench -BenchArgs @("-m",$Model,"-ngl","0,10,20,33") -SaveAs $md
}

switch ($Test) {
    "threads" { Run-ThreadSweep }
    "batch"   { Run-BatchSweep }
    "ctx"     { Run-CtxSweep }
    "mmap"    { Run-MmapSweep }
    "ngl"     { Run-NglSweep }
    "all"     { Run-ThreadSweep; Run-BatchSweep; Run-CtxSweep; Run-MmapSweep }
}
Write-Host "`n完成。结果 CSV 在 $ResultsDir" -ForegroundColor Green
