#!/bin/bash
# Run zclaw in QEMU emulator

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
QEMU_BUILD_DIR="build-qemu"
QEMU_SDKCONFIG="sdkconfig.qemu"
QEMU_SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu.defaults"
QEMU_PID_FILE="$QEMU_BUILD_DIR/qemu.pid"

cd "$PROJECT_DIR"

# Check for QEMU
if ! command -v qemu-system-riscv32 &> /dev/null; then
    echo "QEMU not found. Install it:"
    echo "  macOS:  brew install qemu"
    echo "  Ubuntu: apt install qemu-system-misc"
    exit 1
fi

# Find and source ESP-IDF
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    source "$HOME/esp/esp-idf/export.sh"
elif [ -f "$HOME/esp/v5.4/esp-idf/export.sh" ]; then
    source "$HOME/esp/v5.4/esp-idf/export.sh"
elif [ -n "$IDF_PATH" ]; then
    source "$IDF_PATH/export.sh"
else
    echo "Error: ESP-IDF not found"
    exit 1
fi

# Build emulator profile
echo "Building QEMU profile..."
# Always regenerate sdkconfig.qemu from defaults to avoid stale local overrides.
rm -f "$QEMU_SDKCONFIG"
idf.py \
    -B "$QEMU_BUILD_DIR" \
    -D SDKCONFIG="$QEMU_SDKCONFIG" \
    -D SDKCONFIG_DEFAULTS="$QEMU_SDKCONFIG_DEFAULTS" \
    build

echo "Starting QEMU emulator..."
echo "Note: WiFi/TLS don't work in QEMU. Enable stub mode via menuconfig for testing."
echo "Press Ctrl+A, X to exit."
echo ""

# Merge bootloader + partition table + app into single image
esptool.py --chip esp32c3 merge_bin -o "$QEMU_BUILD_DIR/merged.bin" \
    --flash_mode dio --flash_size 4MB \
    0x0 "$QEMU_BUILD_DIR/bootloader/bootloader.bin" \
    0x8000 "$QEMU_BUILD_DIR/partition_table/partition-table.bin" \
    0x20000 "$QEMU_BUILD_DIR/zclaw.bin" 2>/dev/null

# QEMU for ESP32 requires fixed-size flash images (2/4/8/16MB).
# Pad merged image to 4MB so it is always accepted.
QEMU_FLASH_IMAGE="$QEMU_BUILD_DIR/merged-qemu-4mb.bin"
cp "$QEMU_BUILD_DIR/merged.bin" "$QEMU_FLASH_IMAGE"
truncate -s 4M "$QEMU_FLASH_IMAGE"

# Run QEMU
rm -f "$QEMU_PID_FILE"
trap 'rm -f "$QEMU_PID_FILE"' EXIT
qemu-system-riscv32 \
    -M esp32c3 \
    -drive file="$QEMU_FLASH_IMAGE",if=mtd,format=raw \
    -pidfile "$QEMU_PID_FILE" \
    -serial mon:stdio \
    -nographic
