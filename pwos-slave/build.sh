#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-build}"
PC_SERIAL_DEV="${PC_SERIAL_DEV:-/dev/ttyUSB0}"
PC_SERIAL_BAUD="${PC_SERIAL_BAUD:-1000000}"
PC_NODE_COUNT="${PC_NODE_COUNT:-2}"
PRESET="${PRESET:-ZGT6Debug}"
OPENOCD_INTERFACE="${OPENOCD_INTERFACE:-interface/stlink.cfg}"
OPENOCD_TARGET="${OPENOCD_TARGET:-target/stm32f4x.cfg}"

cd "$(dirname "$0")"
REPO_ROOT="$(cd .. && pwd)"

usage() {
    cat <<EOF
usage: $0 [build|flash|test|flash-test]

env:
  PRESET             CMake preset, default: ZGT6Debug
  OPENOCD_INTERFACE  OpenOCD interface config, default: interface/stlink.cfg
  OPENOCD_TARGET     OpenOCD target config, default: target/stm32f4x.cfg
  PC_SERIAL_DEV      serial device for pc_master_emulator, default: /dev/ttyUSB0
  PC_SERIAL_BAUD     serial baud for pc_master_emulator, default: 1000000
  PC_NODE_COUNT      number of nodes to wait for, default: 1
EOF
}

build_firmware() {
    cmake --preset "$PRESET"
    cmake --build --preset "$PRESET"
}

flash_firmware() {
    local elf_path="build/$PRESET/pwos-slave.elf"

    if [[ ! -f "$elf_path" ]]; then
        echo "missing firmware ELF: $elf_path" >&2
        exit 1
    fi

    openocd \
        -f "$OPENOCD_INTERFACE" \
        -f "$OPENOCD_TARGET" \
        -c "program $elf_path verify reset exit"
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
        printf 'Starting pc_master_emulator. Reset the board after it starts waiting...\n'
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
