#!/usr/bin/env bash
# 27_rpc_compare.sh - report sections 2.6/2.7 single-node vs RPC comparison.

set -euo pipefail

rpc=''
mode=both
n_predict=128
prompt="请介绍分布式推理。"

usage() {
    cat <<'EOF'
Usage:
  ./scripts/27_rpc_compare.sh --rpc 192.168.1.20:50052 [--mode single|rpc|both]
                             [--n-predict 128] [--prompt TEXT]

Compatibility aliases:
  -Rpc, -Mode, -NPredict, -Prompt
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rpc|-Rpc)
            rpc=$2
            shift 2
            ;;
        --mode|-Mode)
            mode=$2
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

case "$mode" in
    single|rpc|both) ;;
    *)
        echo "Invalid --mode value: $mode" >&2
        usage >&2
        exit 2
        ;;
esac

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
print_common_summary

rpc_cli="$(resolve_tool "$rpc_bin_dir" "llama-cli")"
rpc_bench="$(resolve_tool "$rpc_bin_dir" "llama-bench")"
cli="$llama_cli"
bench="$llama_bench"
if [[ -e "$rpc_cli" ]]; then
    cli="$rpc_cli"
fi
if [[ -e "$rpc_bench" ]]; then
    bench="$rpc_bench"
fi

assert_file "$model"
assert_file "$cli"
assert_file "$bench"

stamp="$(date +%Y%m%d_%H%M%S)"
rows=()

run_config() {
    local label=$1
    shift
    local extra=("$@")

    printf '\n%s========== [%s] ==========%s\n' "$color_cyan" "$label" "$color_reset"
    printf -- '-- llama-bench (pp512/tg128) --\n'
    local bench_log="$results_dir/27_${label}_bench_$stamp.md"
    set +e
    "$bench" -m "$model" "${extra[@]}" -o md 2>&1 | tee "$bench_log"
    local bench_status=${PIPESTATUS[0]}
    set -e
    if [[ "$bench_status" -ne 0 ]]; then
        die "llama-bench 失败(exit code=$bench_status): $bench_log"
    fi

    printf -- '-- llama-cli (加载时间/TTFT, 看日志中 RPC backend 注册) --\n'
    local pf err_log
    pf="$(new_prompt_file "$prompt")"
    err_log="$results_dir/27_${label}_cli_$stamp.err.log"
    invoke_with_peak_memory "$cli" \
        --err-log "$err_log" \
        -- -m "$model" -f "$pf" -n "$n_predict" --threads 8 -st -v "${extra[@]}"
    rm -f "$pf"
    if [[ "$INVOKE_EXIT_CODE" -ne 0 ]]; then
        die "llama-cli 失败(exit code=$INVOKE_EXIT_CODE): $err_log"
    fi
    parse_llama_timings "$INVOKE_STDERR" "$INVOKE_SECONDS"

    rows+=("$(csv_escape "$label"),${PARSE_LOAD_MS:-},${PARSE_PROMPT_EVAL_MS:-},$INVOKE_PEAK_MB")
}

if [[ "$mode" == "single" || "$mode" == "both" ]]; then
    run_config "single"
fi
if [[ "$mode" == "rpc" || "$mode" == "both" ]]; then
    [[ -n "$rpc" ]] || die "RPC 模式需要 --rpc <从机ip:port>"
    printf '\n%s使用 RPC 从机: %s%s\n' "$color_yellow" "$rpc" "$color_reset"
    run_config "rpc" --rpc "$rpc"
fi

printf '\n%s========== 单机 vs RPC 汇总 ==========%s\n' "$color_green" "$color_reset"
printf '%-10s %-14s %-10s %-12s\n' "配置" "加载时间ms" "TTFTms" "峰值内存MB"
for row in "${rows[@]}"; do
    IFS=',' read -r cfg load ttft peak <<<"$row"
    cfg=${cfg#\"}
    cfg=${cfg%\"}
    printf '%-10s %-14s %-10s %-12s\n' "$cfg" "$load" "$ttft" "$peak"
done

csv="$results_dir/27_compare_$stamp.csv"
{
    printf '配置,加载时间ms,TTFTms,峰值内存MB\n'
    printf '%s\n' "${rows[@]}"
} >"$csv"
printf 'prefill/decode 见各 *_bench_*.md ; 汇总: %s\n' "$csv"
