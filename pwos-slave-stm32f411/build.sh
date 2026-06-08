#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-build}"
PC_SERIAL_DEV="${PC_SERIAL_DEV:-/dev/ttyUSB0}"
PC_SERIAL_BAUD="${PC_SERIAL_BAUD:-1000000}"
PC_NODE_COUNT="${PC_NODE_COUNT:-1}"
PWOS_ENABLE_SECOND_MESH_UART="${PWOS_ENABLE_SECOND_MESH_UART:-OFF}"

cd "$(dirname "$0")"
REPO_ROOT="$(cd .. && pwd)"

usage() {
    cat <<EOF
usage: $0 [build|flash|test|flash-test]

env:
  PC_SERIAL_DEV   serial device for pc_master_emulator, default: /dev/ttyUSB0
  PC_SERIAL_BAUD  serial baud for pc_master_emulator, default: 1000000
  PC_NODE_COUNT   number of nodes to wait for, default: 1
  PWOS_ENABLE_SECOND_MESH_UART
                  enable USART1 as mesh port1, default: OFF
EOF
}

build_firmware() {
    cmake --preset Debug -DPWOS_ENABLE_SECOND_MESH_UART="$PWOS_ENABLE_SECOND_MESH_UART"
    cmake --build --preset Debug
}

flash_firmware() {
    dfu-util -a 0 -s 0x08000000:leave -D build/Debug/pwos-slave-stm32f411.bin
}

run_pc_master_emulator() {
    cmake -S "$REPO_ROOT/tools/pc_master_emulator" -B "$REPO_ROOT/tools/pc_master_emulator/build"
    cmake --build "$REPO_ROOT/tools/pc_master_emulator/build"
    "$REPO_ROOT/tools/pc_master_emulator/build/pc_master_emulator" "$PC_SERIAL_DEV" "$PC_SERIAL_BAUD" "$PC_NODE_COUNT"
}

case "$ACTION" in
    build)
        build_firmware
        ;;
    flash)
        build_firmware
        flash_firmware
        ;;
    test)
        run_pc_master_emulator
        ;;
    flash-test)
        build_firmware
        flash_firmware
        printf 'Set BOOT0=0, then press Enter to start pc_master_emulator. Reset the board after it starts waiting...'
        read -r _
        run_pc_master_emulator
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        exit 2
        ;;
esac
