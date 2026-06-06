#!/usr/bin/env bash
# 23_metrics.sh - report section 2.3 core metrics.

set -euo pipefail

threads=8
n_predict=128
prompt="What is a large language model? Explain in detail."

usage() {
    cat <<'EOF'
Usage:
  ./scripts/23_metrics.sh [--threads 8] [--n-predict 128] [--prompt TEXT]

Compatibility aliases:
  -Threads, -NPredict, -Prompt
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threads|-Threads)
            threads=$2
            shift 2
            ;;
        --n-predict|-NPredict)
            n_predict=$2
            shift 2
            ;;
        --prompt|-Prompt)
            prompt=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
print_common_summary
assert_file "$model" "请先用 modelscope 下载模型到 llama.cpp/models/"
assert_file "$llama_cli" "请先构建 llama.cpp，生成 llama-cli"
assert_file "$llama_bench" "请先构建 llama.cpp，生成 llama-bench"

stamp="$(date +%Y%m%d_%H%M%S)"

printf '\n%s[1/2] llama-bench: pp512 / tg128 ...%s\n' "$color_cyan" "$color_reset"
bench_md="$results_dir/23_bench_$stamp.md"
invoke_bench "$bench_md" -m "$model" -t "$threads"

printf '\n%s[2/2] llama-cli: 加载时间 / TTFT / 峰值内存 ...%s\n' "$color_cyan" "$color_reset"
cli_out="$results_dir/23_cli_$stamp.out.log"
cli_err="$results_dir/23_cli_$stamp.err.log"
invoke_with_peak_memory "$llama_cli" \
    --out-log "$cli_out" \
    --err-log "$cli_err" \
    -- -m "$model" -p "$prompt" -n "$n_predict" --threads "$threads" -st -v

parse_llama_timings "$INVOKE_STDERR" "$INVOKE_SECONDS"

printf '\n%s========== 2.3 指标汇总 ==========%s\n' "$color_green" "$color_reset"
cat <<EOF
加载时间约(ms)      : ${PARSE_LOAD_MS:-}
首Token延迟TTFT(ms) : ${PARSE_PROMPT_EVAL_MS:-}
推理总时长(ms)      : ${PARSE_TOTAL_MS:-}
峰值内存(MB)        : $INVOKE_PEAK_MB
墙钟耗时(s)         : $INVOKE_SECONDS
线程数              : $threads
EOF
printf 'Prefill/Decode 吞吐见上方 llama-bench 表 (pp512 / tg128)。\n'
printf '原始日志: %s\n' "$cli_err"

summary_csv="$results_dir/23_summary_$stamp.csv"
{
    printf '加载时间约(ms),首Token延迟TTFT(ms),推理总时长(ms),峰值内存(MB),墙钟耗时(s),线程数\n'
    printf '%s,%s,%s,%s,%s,%s\n' \
        "${PARSE_LOAD_MS:-}" \
        "${PARSE_PROMPT_EVAL_MS:-}" \
        "${PARSE_TOTAL_MS:-}" \
        "$INVOKE_PEAK_MB" \
        "$INVOKE_SECONDS" \
        "$threads"
} >"$summary_csv"
printf '%s汇总已保存: %s%s\n' "$color_green" "$summary_csv" "$color_reset"
