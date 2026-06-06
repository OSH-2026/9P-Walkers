#!/usr/bin/env bash
# 28_quant_compare.sh - report section 2.8 quantization comparison.

set -euo pipefail

models_arg="q4_k_m,q5_k_m,q8_0"

usage() {
    cat <<'EOF'
Usage:
  ./scripts/28_quant_compare.sh [--models q4_k_m,q5_k_m,q8_0]

Compatibility alias:
  -Models
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --models|-Models)
            models_arg=$2
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

IFS=',' read -r -a quant_models <<<"$models_arg"

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
print_common_summary
assert_file "$llama_cli"
assert_file "$llama_bench"

stamp="$(date +%Y%m%d_%H%M%S)"
rows=()

for q in "${quant_models[@]}"; do
    q="${q//[[:space:]]/}"
    [[ -n "$q" ]] || continue
    path="$model_dir/qwen2.5-7b-instruct-$q.gguf"
    if [[ ! -e "$path" ]]; then
        printf '%s[跳过]%s 未找到 %s (请先下载该量化)\n' "$color_yellow" "$color_reset" "$path"
        continue
    fi

    printf '\n%s=== 量化 %s ===%s\n' "$color_cyan" "$q" "$color_reset"
    size_bytes="$(stat -c '%s' "$path")"
    size_gib="$(awk -v bytes="$size_bytes" 'BEGIN { printf "%.2f", bytes / 1024 / 1024 / 1024 }')"

    bench_md="$results_dir/28_${q}_bench_$stamp.md"
    invoke_bench "$bench_md" -m "$path" -t 8

    invoke_with_peak_memory "$llama_cli" -- -m "$path" -p "Hello" -n 64 --threads 8 -st
    rows+=("$(csv_escape "$q"),$size_gib,$INVOKE_PEAK_MB,$(csv_escape "$(basename "$bench_md")")")
done

printf '\n%s========== 量化对比汇总 ==========%s\n' "$color_green" "$color_reset"
printf '%-12s %-10s %-14s %s\n' "量化" "体积GiB" "峰值内存MB" "吞吐表"
for row in "${rows[@]}"; do
    IFS=',' read -r q size peak bench_file <<<"$row"
    q=${q#\"}; q=${q%\"}
    bench_file=${bench_file#\"}; bench_file=${bench_file%\"}
    printf '%-12s %-10s %-14s %s\n' "$q" "$size" "$peak" "$bench_file"
done

csv="$results_dir/28_quant_compare_$stamp.csv"
{
    printf '量化,体积GiB,峰值内存MB,吞吐表\n'
    printf '%s\n' "${rows[@]}"
} >"$csv"
printf '汇总: %s  (prefill/decode 见各 *_bench_*.md)\n' "$csv"
