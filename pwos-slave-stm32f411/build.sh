#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-build}"
PC_SERIAL_DEV="${PC_SERIAL_DEV:-/dev/ttyUSB0}"
PC_SERIAL_BAUD="${PC_SERIAL_BAUD:-115200}"

cd "$(dirname "$0")"
REPO_ROOT="$(cd .. && pwd)"

usage() {
    cat <<EOF
usage: $0 [build|flash|test|flash-test]

env:
  PC_SERIAL_DEV   serial device for pc_master_emulator, default: /dev/ttyUSB0
  PC_SERIAL_BAUD  serial baud for pc_master_emulator, default: 115200
EOF
}

build_firmware() {
    cmake --preset Debug
    cmake --build --preset Debug
}

flash_firmware() {
    dfu-util -a 0 -s 0x08000000:leave -D build/Debug/pwos-slave-stm32f411.bin
}

run_pc_master_emulator() {
    cmake -S "$REPO_ROOT/tools/pc_master_emulator" -B "$REPO_ROOT/tools/pc_master_emulator/build"
    cmake --build "$REPO_ROOT/tools/pc_master_emulator/build"
    "$REPO_ROOT/tools/pc_master_emulator/build/pc_master_emulator" "$PC_SERIAL_DEV" "$PC_SERIAL_BAUD"
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
        printf 'Set BOOT0=0, reset the board, then press Enter to run pc_master_emulator...'
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
