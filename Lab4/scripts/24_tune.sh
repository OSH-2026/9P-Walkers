#!/usr/bin/env bash
# 24_tune.sh - report section 2.4 deployment parameter sweeps.

set -euo pipefail

test_name=all

usage() {
    cat <<'EOF'
Usage:
  ./scripts/24_tune.sh [--test threads|batch|ctx|mmap|ngl|all]

Compatibility alias:
  -Test
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test|-Test)
            test_name=$2
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

case "$test_name" in
    threads|batch|ctx|mmap|ngl|all) ;;
    *)
        echo "Invalid --test value: $test_name" >&2
        usage >&2
        exit 2
        ;;
esac

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
print_common_summary
assert_file "$model"
assert_file "$llama_cli"
assert_file "$llama_bench"

stamp="$(date +%Y%m%d_%H%M%S)"

run_thread_sweep() {
    printf '\n%s=== (1) 线程数扫描 -t 4,8,16,24,32 (看 tg/decode) ===%s\n' "$color_cyan" "$color_reset"
    invoke_bench "$results_dir/24_threads_$stamp.md" -m "$model" -t "4,8,16,24,32"
}

run_batch_sweep() {
    printf '\n%s=== (2) 批大小扫描 -b 128,256,512,1024 (看 pp/prefill) ===%s\n' "$color_cyan" "$color_reset"
    invoke_bench "$results_dir/24_batch_$stamp.md" -m "$model" -b "128,256,512,1024"
}

run_ctx_sweep() {
    printf '\n%s=== (3) 上下文长度对峰值内存的影响 -c 512/2048/8192/16384 ===%s\n' "$color_cyan" "$color_reset"
    local csv="$results_dir/24_ctx_$stamp.csv"
    printf 'ctx,PeakMB\n' >"$csv"
    local ctx
    for ctx in 512 2048 8192 16384; do
        printf '  -c %s ...\n' "$ctx"
        invoke_with_peak_memory "$llama_cli" -- -m "$model" -p "Hello" -n 32 -c "$ctx" --threads 8 -st
        printf '%s,%s\n' "$ctx" "$INVOKE_PEAK_MB" >>"$csv"
        printf '    PeakMB=%s\n' "$INVOKE_PEAK_MB"
    done
    printf '  -> %s\n' "$csv"
}

run_mmap_sweep() {
    printf '\n%s=== (4) 内存映射开关对加载时间的影响 -mmp 0,1 ===%s\n' "$color_cyan" "$color_reset"
    invoke_bench "$results_dir/24_mmap_$stamp.md" -m "$model" -mmp "0,1"
    printf "  (补充: llama-cli 的 'load time' 也可对比 --no-mmap 开/关)\n"
}

run_ngl_sweep() {
    printf '\n%s=== (5) GPU offload 层数扫描 -ngl 0,10,20,33 (需 CUDA 构建) ===%s\n' "$color_cyan" "$color_reset"
    invoke_bench "$results_dir/24_ngl_$stamp.md" -m "$model" -ngl "0,10,20,33"
}

case "$test_name" in
    threads) run_thread_sweep ;;
    batch) run_batch_sweep ;;
    ctx) run_ctx_sweep ;;
    mmap) run_mmap_sweep ;;
    ngl) run_ngl_sweep ;;
    all)
        run_thread_sweep
        run_batch_sweep
        run_ctx_sweep
        run_mmap_sweep
        ;;
esac

printf '\n%s完成。结果在 %s%s\n' "$color_green" "$results_dir" "$color_reset"
