#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

cmake -B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$SCRIPT_DIR"
cmake --build "$BUILD_DIR"

echo "=== Running cluster_vfs_test ==="
"$BUILD_DIR/cluster_vfs_test"

echo "=== Running mesh_host_service_test ==="
"$BUILD_DIR/mesh_host_service_test"

echo "=== Running mesh_host_runtime_test ==="
"$BUILD_DIR/mesh_host_runtime_test"
