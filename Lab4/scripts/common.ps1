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

    # 手动给含空格/引号的参数补引号(Windows 下 .NET 以 UTF-16 传给 CreateProcessW，
    # 中文参数本身不会乱码)，拼成命令行字符串。
    $argLine = ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"','\"') + '"' } else { $_ }
    }) -join ' '

    # 关键：用 ProcessStartInfo 显式指定 StandardOutput/ErrorEncoding = UTF-8，
    # 否则 PowerShell 会用系统 GBK 代码页去解码 llama-cli 的 UTF-8 输出，导致中文乱码。
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName               = $FilePath
    $psi.Arguments              = $argLine
    $psi.UseShellExecute        = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.StandardOutputEncoding = New-Object System.Text.UTF8Encoding $false
    $psi.StandardErrorEncoding  = New-Object System.Text.UTF8Encoding $false

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$p.Start()
    # 异步读取两个流，避免缓冲区写满导致的死锁(verbose 输出可达上百 KB)。
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()

    $peak = 0L
    while (-not $p.HasExited) {
        try { $p.Refresh(); if ($p.WorkingSet64 -gt $peak) { $peak = $p.WorkingSet64 } } catch {}
        Start-Sleep -Milliseconds 150
    }
    try { if ($p.PeakWorkingSet64 -gt $peak) { $peak = $p.PeakWorkingSet64 } } catch {}
    $p.WaitForExit()
    $sw.Stop()

    $stdout = $outTask.Result
    $stderr = $errTask.Result
    # 落盘统一用 UTF-8(无 BOM)
    $utf8 = New-Object System.Text.UTF8Encoding $false
    if ($OutLog) { [System.IO.File]::WriteAllText($OutLog, $stdout, $utf8) }
    if ($ErrLog) { [System.IO.File]::WriteAllText($ErrLog, $stderr, $utf8) }

    return [pscustomobject]@{
        ExitCode = $p.ExitCode
        PeakMB   = [math]::Round($peak / 1MB, 1)
        Seconds  = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        StdOut   = $stdout
        StdErr   = $stderr
    }
}

# 把(可能含中文的) prompt 写到 UTF-8(无 BOM) 临时文件，返回路径。
# Windows 上 llama.cpp 用命令行 -p 传中文会因 argv 编码(GBK)而乱码，
# 必须改用 -f <文件> 让其按 UTF-8 直接读取。用完记得 Remove-Item。
function New-PromptFile($text) {
    $f = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($f, $text, (New-Object System.Text.UTF8Encoding $false))
    return $f
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
