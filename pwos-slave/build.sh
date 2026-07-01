#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-build}"
PRESET="${PRESET:-F407Debug}"
OPENOCD_INTERFACE="${OPENOCD_INTERFACE:-interface/stlink.cfg}"
OPENOCD_TARGET="${OPENOCD_TARGET:-target/stm32f4x.cfg}"

cd "$(dirname "$0")"

usage() {
    cat <<EOF
usage: $0 [build|flash]

env:
  PRESET             CMake preset, default: F407Debug
  OPENOCD_INTERFACE  OpenOCD interface config, default: interface/stlink.cfg
  OPENOCD_TARGET     OpenOCD target config, default: target/stm32f4x.cfg
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

case "$ACTION" in
    build)
        build_firmware
        ;;
    flash)
        build_firmware
        flash_firmware
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        exit 2
        ;;
esac
