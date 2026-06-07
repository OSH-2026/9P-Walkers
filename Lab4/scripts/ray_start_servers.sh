#!/usr/bin/env bash
# ray_start_servers.sh - report section 3.2 llama-server launcher.

set -euo pipefail

port=8080
threads=8
bind_host=0.0.0.0
ctx_size=4096

usage() {
    cat <<'EOF'
Usage:
  ./scripts/ray_start_servers.sh [--port 8080] [--threads 8]
                                [--bind-host 0.0.0.0] [--ctx-size 4096]

Compatibility aliases:
  -Port, -Threads, -BindHost, -CtxSize
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port|-Port)
            port=$2
            shift 2
            ;;
        --threads|-Threads)
            threads=$2
            shift 2
            ;;
        --bind-host|-BindHost)
            bind_host=$2
            shift 2
            ;;
        --ctx-size|-CtxSize)
            ctx_size=$2
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
assert_file "$llama_server"
assert_file "$model"

printf '%s本机局域网 IP:%s\n' "$color_cyan" "$color_reset"
if command -v ip >/dev/null 2>&1; then
    ip -4 addr show scope global 2>/dev/null | awk '/inet / { sub(/\/.*/, "", $2); print "  " $2 }' || true
else
    hostname -I 2>/dev/null | tr ' ' '\n' | awk 'NF { print "  " $0 }' || true
fi

printf '%s启动 llama-server  http://%s:%s  (t=%s, c=%s)%s\n' \
    "$color_green" "$bind_host" "$port" "$threads" "$ctx_size" "$color_reset"
exec "$llama_server" -m "$model" --host "$bind_host" --port "$port" -t "$threads" -c "$ctx_size"
