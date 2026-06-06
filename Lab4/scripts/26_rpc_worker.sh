#!/usr/bin/env bash
# 26_rpc_worker.sh - report section 2.6 worker-side rpc-server launcher.

set -euo pipefail

port=50052
bind_host=0.0.0.0

usage() {
    cat <<'EOF'
Usage:
  ./scripts/26_rpc_worker.sh [--port 50052] [--bind-host 0.0.0.0]

Compatibility aliases:
  -Port, -BindHost
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port|-Port)
            port=$2
            shift 2
            ;;
        --bind-host|-BindHost)
            bind_host=$2
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
rpc_server="$(resolve_tool "$rpc_bin_dir" "rpc-server")"
assert_file "$rpc_server" "请先运行 26_build_rpc.sh 完成 RPC 构建"

printf '%s本机局域网 IP(填到主机 --rpc 参数):%s\n' "$color_cyan" "$color_reset"
if command -v ip >/dev/null 2>&1; then
    ip -4 addr show scope global | awk '/inet / { sub(/\/.*/, "", $2); printf "  %s  %s\n", $2, $NF }'
else
    hostname -I 2>/dev/null | tr ' ' '\n' | awk 'NF { print "  " $0 }' || true
fi

printf '%s启动 rpc-server  %s:%s ...%s\n' "$color_green" "$bind_host" "$port" "$color_reset"
exec "$rpc_server" -H "$bind_host" -p "$port"
