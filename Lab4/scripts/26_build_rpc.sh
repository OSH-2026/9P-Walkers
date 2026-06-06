#!/usr/bin/env bash
# 26_build_rpc.sh - report section 2.6 RPC-enabled llama.cpp build.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  ./scripts/26_build_rpc.sh
EOF
}

if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then
    usage
    exit 0
fi

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
print_common_summary

ggml_rpc_rdma="${GGML_RPC_RDMA:-OFF}"
ggml_ccache="${GGML_CCACHE:-OFF}"

cd "$llama_dir"
printf '%s配置 CMake (-DGGML_RPC=ON -DGGML_RPC_RDMA=%s -DGGML_CCACHE=%s) ...%s\n' "$color_cyan" "$ggml_rpc_rdma" "$ggml_ccache" "$color_reset"
cmake -B build-rpc -DGGML_RPC=ON -DGGML_RPC_RDMA="$ggml_rpc_rdma" -DGGML_CCACHE="$ggml_ccache"
printf '%s编译 ...%s\n' "$color_cyan" "$color_reset"
cmake --build build-rpc --config Release -j

rpc_server="$(resolve_tool "$rpc_bin_dir" "rpc-server")"
if [[ -e "$rpc_server" ]]; then
    printf '\n%s成功: %s%s\n' "$color_green" "$rpc_server" "$color_reset"
else
    printf '\n%s未找到 rpc-server，请检查编译输出。%s\n' "$color_red" "$color_reset" >&2
    exit 1
fi
