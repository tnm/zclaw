#!/bin/bash
# Build zclaw firmware

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

PAD_TARGET_BYTES=""

usage() {
    echo "Usage: $0 [--pad-to-888kb]"
    echo "  --pad-to-888kb  Create build/zclaw-888kb.bin padded to exactly 888 KiB (909312 bytes)"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --pad-to-888kb)
            PAD_TARGET_BYTES=$((888 * 1024))
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: Unknown argument '$1'"
            usage
            exit 1
            ;;
    esac
done

file_size_bytes() {
    local path="$1"
    if stat -f %z "$path" >/dev/null 2>&1; then
        stat -f %z "$path"
    else
        stat -c %s "$path"
    fi
}

source_idf_env() {
    local candidates=(
        "$HOME/esp/esp-idf/export.sh"
        "$HOME/esp/v5.4/esp-idf/export.sh"
    )
    if [ -n "${IDF_PATH:-}" ]; then
        candidates+=("$IDF_PATH/export.sh")
    fi

    local script
    local found=0
    for script in "${candidates[@]}"; do
        [ -f "$script" ] || continue
        found=1
        if source "$script" > /dev/null 2>&1; then
            return 0
        fi
    done

    if [ "$found" -eq 1 ]; then
        echo "Error: ESP-IDF found but failed to activate."
        echo "Run:"
        echo "  cd ~/esp/esp-idf && ./install.sh esp32c3,esp32s3"
    else
        echo "Error: ESP-IDF not found. Install it first:"
        echo "  mkdir -p ~/esp && cd ~/esp"
        echo "  git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git"
        echo "  cd esp-idf && ./install.sh esp32c3,esp32s3"
    fi
    return 1
}

source_idf_env || exit 1

echo "Building zclaw..."
idf.py build

if [ -n "$PAD_TARGET_BYTES" ]; then
    SRC_BIN="$PROJECT_DIR/build/zclaw.bin"
    OUT_BIN="$PROJECT_DIR/build/zclaw-888kb.bin"

    if [ ! -f "$SRC_BIN" ]; then
        echo "Error: Expected firmware binary not found at $SRC_BIN"
        exit 1
    fi

    cp "$SRC_BIN" "$OUT_BIN"
    CUR_SIZE="$(file_size_bytes "$OUT_BIN")"

    if [ "$CUR_SIZE" -gt "$PAD_TARGET_BYTES" ]; then
        echo "Error: build/zclaw.bin is ${CUR_SIZE} bytes, larger than 888 KiB target (${PAD_TARGET_BYTES} bytes)."
        exit 1
    fi

    PAD_BYTES=$((PAD_TARGET_BYTES - CUR_SIZE))
    if [ "$PAD_BYTES" -gt 0 ]; then
        dd if=/dev/zero bs=1 count="$PAD_BYTES" 2>/dev/null | LC_ALL=C tr '\000' '\377' >> "$OUT_BIN"
    fi

    FINAL_SIZE="$(file_size_bytes "$OUT_BIN")"
    if [ "$FINAL_SIZE" -ne "$PAD_TARGET_BYTES" ]; then
        echo "Error: Padded binary size mismatch ($FINAL_SIZE bytes)."
        exit 1
    fi

    echo "Padded binary created: build/zclaw-888kb.bin ($FINAL_SIZE bytes)"
fi

echo ""
echo "Build complete! To flash: ./scripts/flash.sh"
