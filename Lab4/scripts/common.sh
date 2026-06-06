#!/usr/bin/env bash
# common.sh - shared paths and helpers for Lab4 scripts.
# Source this file from other scripts; do not run it directly.

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "common.sh should be sourced by other scripts."
    exit 0
fi

scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$scripts_dir/.." && pwd)"
llama_dir="$root/llama.cpp"
model_dir="$llama_dir/models"
results_dir="$scripts_dir/results"
mkdir -p "$results_dir"

first_existing() {
    local path
    for path in "$@"; do
        if [[ -e "$path" ]]; then
            printf '%s\n' "$path"
            return 0
        fi
    done
    return 1
}

resolve_tool() {
    local dir=$1
    local name=$2
    first_existing \
        "$dir/$name" \
        "$dir/$name.exe" \
        "$dir/Release/$name" \
        "$dir/Release/$name.exe" \
        || printf '%s/%s\n' "$dir" "$name"
}

bin_dir="$(first_existing "$llama_dir/build/bin" "$llama_dir/build/bin/Release" 2>/dev/null || true)"
rpc_bin_dir="$(first_existing "$llama_dir/build-rpc/bin" "$llama_dir/build-rpc/bin/Release" 2>/dev/null || true)"
bin_dir="${bin_dir:-$llama_dir/build/bin}"
rpc_bin_dir="${rpc_bin_dir:-$llama_dir/build-rpc/bin}"

model="$model_dir/qwen2.5-7b-instruct-q4_k_m.gguf"
llama_cli="$(resolve_tool "$bin_dir" "llama-cli")"
llama_bench="$(resolve_tool "$bin_dir" "llama-bench")"
llama_server="$(resolve_tool "$bin_dir" "llama-server")"
rpc_server="$(resolve_tool "$rpc_bin_dir" "rpc-server")"

if [[ -t 1 ]]; then
    color_red=$'\033[31m'
    color_green=$'\033[32m'
    color_yellow=$'\033[33m'
    color_cyan=$'\033[36m'
    color_reset=$'\033[0m'
else
    color_red=''
    color_green=''
    color_yellow=''
    color_cyan=''
    color_reset=''
fi

info() {
    printf '%s\n' "$*"
}

warn() {
    printf '%s[警告]%s %s\n' "$color_yellow" "$color_reset" "$*" >&2
}

die() {
    printf '%s[错误]%s %s\n' "$color_red" "$color_reset" "$*" >&2
    exit 1
}

assert_file() {
    local path=${1:-}
    local hint=${2:-}
    if [[ -z "$path" || ! -e "$path" ]]; then
        printf '%s[缺失]%s %s\n' "$color_red" "$color_reset" "$path" >&2
        if [[ -n "$hint" ]]; then
            printf '       %s\n' "$hint" >&2
        fi
        exit 1
    fi
}

get_rss_kb() {
    local pid=$1
    local rss=''
    if [[ -r "/proc/$pid/status" ]]; then
        rss="$(awk '/^VmRSS:/ {print $2; exit}' "/proc/$pid/status" 2>/dev/null || true)"
    fi
    if [[ -z "$rss" ]]; then
        rss="$(ps -o rss= -p "$pid" 2>/dev/null | tr -d '[:space:]' || true)"
    fi
    printf '%s\n' "${rss:-0}"
}

invoke_with_peak_memory() {
    local file_path=$1
    shift
    local out_log=''
    local err_log=''

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --out-log)
                out_log=$2
                shift 2
                ;;
            --err-log)
                err_log=$2
                shift 2
                ;;
            --)
                shift
                break
                ;;
            *)
                break
                ;;
        esac
    done

    local args=("$@")
    assert_file "$file_path"

    local tmp_out tmp_err
    tmp_out="$(mktemp)"
    tmp_err="$(mktemp)"

    local start_ts end_ts pid status peak_kb rss
    start_ts="$(date +%s.%N)"
    "$file_path" "${args[@]}" >"$tmp_out" 2>"$tmp_err" &
    pid=$!

    peak_kb=0
    while kill -0 "$pid" 2>/dev/null; do
        rss="$(get_rss_kb "$pid")"
        if [[ "$rss" =~ ^[0-9]+$ && "$rss" -gt "$peak_kb" ]]; then
            peak_kb=$rss
        fi
        sleep 0.15
    done

    set +e
    wait "$pid"
    status=$?
    set -e

    end_ts="$(date +%s.%N)"
    if [[ -n "$out_log" ]]; then
        cp "$tmp_out" "$out_log"
    fi
    if [[ -n "$err_log" ]]; then
        cp "$tmp_err" "$err_log"
    fi

    INVOKE_EXIT_CODE=$status
    INVOKE_PEAK_MB="$(awk -v kb="$peak_kb" 'BEGIN { printf "%.1f", kb / 1024 }')"
    INVOKE_SECONDS="$(awk -v s="$start_ts" -v e="$end_ts" 'BEGIN { printf "%.2f", e - s }')"
    INVOKE_STDOUT="$(<"$tmp_out")"
    INVOKE_STDERR="$(<"$tmp_err")"
    rm -f "$tmp_out" "$tmp_err"
    return 0
}

new_prompt_file() {
    local text=$1
    local file
    file="$(mktemp)"
    printf '%s' "$text" >"$file"
    printf '%s\n' "$file"
}

invoke_bench() {
    local save_as=$1
    shift
    assert_file "$llama_bench"
    mkdir -p "$(dirname "$save_as")"

    set +e
    "$llama_bench" "$@" -o md 2>&1 | tee "$save_as"
    local status=${PIPESTATUS[0]}
    set -e

    printf '  -> %s\n' "$save_as"
    return "$status"
}

parse_llama_timings() {
    local text=$1
    local wall_seconds=${2:-}

    PARSE_PROMPT_EVAL_MS="$(printf '%s\n' "$text" | sed -nE 's/.*prompt eval time[[:space:]]*=[[:space:]]*([0-9.]+)[[:space:]]*ms.*/\1/p' | tail -n 1)"
    PARSE_EVAL_MS="$(printf '%s\n' "$text" | sed -nE '/prompt eval time/! s/.*eval time[[:space:]]*=[[:space:]]*([0-9.]+)[[:space:]]*ms.*/\1/p' | tail -n 1)"
    PARSE_TOTAL_MS="$(printf '%s\n' "$text" | sed -nE 's/.*total time[[:space:]]*=[[:space:]]*([0-9.]+)[[:space:]]*ms.*/\1/p' | tail -n 1)"
    PARSE_LOAD_MS="$(printf '%s\n' "$text" | sed -nE 's/.*load time[[:space:]]*=[[:space:]]*([0-9.]+)[[:space:]]*ms.*/\1/p' | tail -n 1)"

    if [[ -z "$PARSE_LOAD_MS" && -n "$wall_seconds" && -n "$PARSE_TOTAL_MS" ]]; then
        PARSE_LOAD_MS="$(awk -v wall="$wall_seconds" -v total="$PARSE_TOTAL_MS" 'BEGIN {
            load = wall * 1000 - total
            if (load >= 0) {
                printf "%.1f", load
            }
        }')"
    fi
}

csv_escape() {
    local value=${1:-}
    value=${value//\"/\"\"}
    printf '"%s"' "$value"
}

print_common_summary() {
    printf '[common] Model      = %s\n' "$model"
    printf '[common] Bin        = %s\n' "$bin_dir"
    printf '[common] Results    = %s\n' "$results_dir"
}
