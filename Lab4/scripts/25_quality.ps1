# 25_quality.ps1 —— 报告 2.5 节：5 个 Prompt 的输出质量评估
# 把 prompts_5.json 中的每个 prompt 跑一遍，输出落盘，供人工 1~5 分打分。
# 通过 -Temp / -Model 可对比不同配置(配置A vs 配置B)。
#
# 用法:
#   # 配置A: Q4_K_M, temp=0.7
#   pwsh -File .\scripts\25_quality.ps1 -Tag A -Temp 0.7
#   # 配置B: 换更高位量化模型(先放到 models 下), 或换 temperature
#   pwsh -File .\scripts\25_quality.ps1 -Tag B -Temp 0.7 -Model .\llama.cpp\models\qwen2.5-7b-instruct-q8_0.gguf
#
param(
    [string]$Tag      = "A",
    [double]$Temp     = 0.7,
    [int]$NPredict    = 256,
    [string]$Model    = $null,    # 不填则用默认 Q4_K_M
    [string]$PromptFile = $null
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\common.ps1"
if ($Model) { $Script:Model = (Resolve-Path $Model).Path }
if (-not $PromptFile) { $PromptFile = Join-Path $ScriptsDir "prompts_5.json" }
Assert-File $Model
Assert-File $PromptFile

$prompts = Get-Content $PromptFile -Raw -Encoding utf8 | ConvertFrom-Json
$stamp   = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir  = Join-Path $ResultsDir "25_quality_${Tag}_$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Write-Host "配置[$Tag]  model=$(Split-Path $Model -Leaf)  temp=$Temp  n=$NPredict" -ForegroundColor Cyan

foreach ($item in $prompts) {
    Write-Host "`n--- #$($item.id) [$($item.category)] ---" -ForegroundColor Yellow
    $args = @("-m",$Model,"-p",$item.prompt,"-n","$NPredict",
              "--temp","$Temp","--threads","8","-st","--no-display-prompt")
    $r = Invoke-WithPeakMemory -FilePath $LlamaCli -Arguments $args
    $file = Join-Path $outDir ("p{0}_{1}.txt" -f $item.id, $item.category)
    @(
        "# Prompt $($item.id) [$($item.category)]  config=$Tag temp=$Temp model=$(Split-Path $Model -Leaf)",
        "## PROMPT", $item.prompt, "",
        "## OUTPUT", $r.StdOut
    ) | Out-File -Encoding utf8 $file
    Write-Host "  -> $file"
}
Write-Host "`n完成。请人工打分(正确性/完整性/流畅性, 1~5)，输出在: $outDir" -ForegroundColor Green
