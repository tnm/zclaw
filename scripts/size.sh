#!/bin/bash
# Show firmware size breakdown

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

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

# Build if needed
if [ ! -f "build/zclaw.elf" ]; then
    echo "Building first..."
    idf.py build
fi

echo ""
idf.py size

# Component breakdown
echo ""
echo "Component sizes:"
idf.py size-components 2>/dev/null | head -30
