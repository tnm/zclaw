#!/bin/bash
# Build zclaw firmware

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
    echo "Error: ESP-IDF not found. Install it first:"
    echo "  mkdir -p ~/esp && cd ~/esp"
    echo "  git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git"
    echo "  cd esp-idf && ./install.sh esp32c3"
    exit 1
fi

echo "Building zclaw..."
idf.py build

echo ""
echo "Build complete! To flash: ./scripts/flash.sh"
