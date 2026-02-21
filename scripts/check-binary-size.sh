#!/bin/bash
# Enforce maximum firmware binary size.

set -e

BUILD_DIR="build"
BINARY_NAME="zclaw.bin"
MAX_BYTES=$((888 * 1024))

usage() {
    cat << USAGE
Usage: $0 [--build-dir DIR] [--binary NAME] [--max-bytes N]

Options:
  --build-dir DIR  Build directory (default: build)
  --binary NAME    Firmware binary filename in build dir (default: zclaw.bin)
  --max-bytes N    Maximum allowed bytes (default: 909312 = 888 KiB)
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --binary)
            BINARY_NAME="$2"
            shift 2
            ;;
        --max-bytes)
            MAX_BYTES="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument '$1'" >&2
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

BIN_PATH="$BUILD_DIR/$BINARY_NAME"
if [ ! -f "$BIN_PATH" ]; then
    echo "Error: binary not found: $BIN_PATH" >&2
    echo "Build firmware first (e.g. idf.py build)." >&2
    exit 1
fi

ACTUAL_BYTES="$(file_size_bytes "$BIN_PATH")"
ACTUAL_KIB="$(awk -v b="$ACTUAL_BYTES" 'BEGIN { printf "%.2f", b / 1024.0 }')"
MAX_KIB="$(awk -v b="$MAX_BYTES" 'BEGIN { printf "%.2f", b / 1024.0 }')"

echo "Binary: $BIN_PATH"
echo "Size: $ACTUAL_BYTES bytes (${ACTUAL_KIB} KiB)"
echo "Limit: $MAX_BYTES bytes (${MAX_KIB} KiB)"

if [ "$ACTUAL_BYTES" -gt "$MAX_BYTES" ]; then
    OVER_BYTES=$((ACTUAL_BYTES - MAX_BYTES))
    OVER_KIB="$(awk -v b="$OVER_BYTES" 'BEGIN { printf "%.2f", b / 1024.0 }')"
    echo "FAIL: binary exceeds limit by $OVER_BYTES bytes (${OVER_KIB} KiB)." >&2
    exit 1
fi

echo "PASS: binary is within 888 KiB budget."
