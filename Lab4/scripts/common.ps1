# common.ps1 —— 公共路径与辅助函数
# 其它脚本通过  . "$PSScriptRoot\common.ps1"  方式 dot-source 引用。
# 不要直接运行本文件。

# --- 路径 ---
$Script:ScriptsDir = $PSScriptRoot
$Script:Root       = Split-Path -Parent $PSScriptRoot          # ...\Lab4
$Script:LlamaDir   = Join-Path $Root "llama.cpp"
$Script:BinDir     = Join-Path $LlamaDir "build\bin\Release"
$Script:RpcBinDir  = Join-Path $LlamaDir "build-rpc\bin\Release"
$Script:ModelDir   = Join-Path $LlamaDir "models"

$Script:Model      = Join-Path $ModelDir "qwen2.5-7b-instruct-q4_k_m.gguf"
$Script:LlamaCli   = Join-Path $BinDir "llama-cli.exe"
$Script:LlamaBench = Join-Path $BinDir "llama-bench.exe"
$Script:LlamaServer= Join-Path $BinDir "llama-server.exe"

$Script:ResultsDir = Join-Path $ScriptsDir "results"
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

function Assert-File($path, $hint) {
    if (-not (Test-Path $path)) {
        Write-Host "[缺失] $path" -ForegroundColor Red
        if ($hint) { Write-Host "       $hint" -ForegroundColor Yellow }
        throw "文件不存在: $path"
    }
}

# 运行一个进程，记录峰值物理内存(MB)，并把 stdout/stderr 落盘。
# 返回: @{ ExitCode; PeakMB; Seconds; StdOut; StdErr }
function Invoke-WithPeakMemory {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$OutLog,
        [string]$ErrLog
    )
    Assert-File $FilePath
    if (-not $OutLog) { $OutLog = [System.IO.Path]::GetTempFileName() }
    if (-not $ErrLog) { $ErrLog = [System.IO.Path]::GetTempFileName() }

    # PowerShell 5.1 的 Start-Process -ArgumentList 不会给含空格的参数加引号，
    # 这里手动给含空格/引号的参数补引号，避免 prompt 被拆成多个参数。
    $quoted = $Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"','\"') + '"' } else { $_ }
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $FilePath -ArgumentList $quoted -PassThru `
            -NoNewWindow -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog
    $peak = 0L
    while (-not $p.HasExited) {
        try { $p.Refresh(); if ($p.WorkingSet64 -gt $peak) { $peak = $p.WorkingSet64 } } catch {}
        Start-Sleep -Milliseconds 150
    }
    $p.Refresh()
    try { if ($p.PeakWorkingSet64 -gt $peak) { $peak = $p.PeakWorkingSet64 } } catch {}
    $sw.Stop()

    return [pscustomobject]@{
        ExitCode = $p.ExitCode
        PeakMB   = [math]::Round($peak / 1MB, 1)
        Seconds  = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        StdOut   = (Get-Content $OutLog -Raw -ErrorAction SilentlyContinue)
        StdErr   = (Get-Content $ErrLog -Raw -ErrorAction SilentlyContinue)
    }
}

# 运行一次 llama-bench：结果(md 表)同时打印到屏幕并保存到 $SaveAs。
# 避免“显示一次、存盘再跑一次”导致的双倍耗时。
function Invoke-Bench {
    param([string[]]$BenchArgs, [string]$SaveAs)
    $out = & $LlamaBench @BenchArgs -o md 2>&1   # md 表 + 进度都收进来
    $out | ForEach-Object { Write-Host $_ }       # 屏幕显示
    $out | Out-File -FilePath $SaveAs -Encoding utf8   # UTF-8 落盘(避免 UTF-16/乱码)
    Write-Host "  -> $SaveAs"
}

# 从 llama-cli 的计时输出(stderr, 需 -v)里抽取关键指标(ms)。
# 该 build 在 -st -v 下打印:
#   prompt eval time = ... ms   (= TTFT 首Token延迟)
#   eval time        = ... ms   (生成阶段)
#   total time       = ... ms   (推理总时长)
# 没有显式 "load time" 行，故用 墙钟时间 - 推理总时长 近似加载时间。
function Parse-LlamaTimings($text, $wallSeconds) {
    $get = {
        param($pattern)
        if ($text -match $pattern) { return [double]$matches[1] } else { return $null }
    }
    $promptEval = & $get 'prompt eval time\s*=\s*([0-9.]+)\s*ms'
    $eval       = & $get '(?<!prompt )eval time\s*=\s*([0-9.]+)\s*ms'   # 排除 "prompt eval time"
    $total      = & $get 'total time\s*=\s*([0-9.]+)\s*ms'

    # 优先用显式 load time(若某些构建有)，否则用 墙钟-推理总时长 近似
    $load = & $get 'load time\s*=\s*([0-9.]+)\s*ms'
    if (-not $load -and $wallSeconds -and $total) {
        $load = [math]::Round($wallSeconds * 1000 - $total, 1)
        if ($load -lt 0) { $load = $null }
    }
    return [pscustomobject]@{
        LoadMs       = $load
        PromptEvalMs = $promptEval
        EvalMs       = $eval
        TotalMs      = $total
    }
}

Write-Host "[common] Model      = $Model"
Write-Host "[common] Bin        = $BinDir"
Write-Host "[common] Results    = $ResultsDir"
