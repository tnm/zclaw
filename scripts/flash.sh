#!/bin/bash
# Flash zclaw to device

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# Colors
YELLOW='\033[1;33m'
NC='\033[0m'

detect_serial_ports() {
    local ports=()
    shopt -s nullglob
    ports+=(/dev/tty.usbserial-*)
    ports+=(/dev/tty.usbmodem*)
    ports+=(/dev/ttyUSB*)
    ports+=(/dev/ttyACM*)
    shopt -u nullglob

    local p
    for p in "${ports[@]}"; do
        [ -e "$p" ] && echo "$p"
    done
}

select_serial_port() {
    local candidates=()
    local p

    while IFS= read -r p; do
        [ -n "$p" ] && candidates+=("$p")
    done < <(detect_serial_ports)

    if [ "${#candidates[@]}" -eq 0 ]; then
        return 1
    fi

    if [ "${#candidates[@]}" -eq 1 ]; then
        PORT="${candidates[0]}"
        echo "Auto-detected serial port: $PORT"
        return 0
    fi

    echo "Multiple serial ports detected:"
    local i
    for ((i = 0; i < ${#candidates[@]}; i++)); do
        echo "  $((i + 1)). ${candidates[$i]}"
    done

    if [ -t 0 ]; then
        read -r -p "Select device [1-${#candidates[@]}]: " choice
        if [[ "$choice" =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le "${#candidates[@]}" ]; then
            PORT="${candidates[$((choice - 1))]}"
            echo "Using selected serial port: $PORT"
            return 0
        fi
        echo "Invalid selection."
        return 1
    fi

    PORT="${candidates[0]}"
    echo "Non-interactive shell; defaulting to first detected port: $PORT"
    return 0
}

# Find and source ESP-IDF
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    source "$HOME/esp/esp-idf/export.sh" > /dev/null 2>&1
elif [ -f "$HOME/esp/v5.4/esp-idf/export.sh" ]; then
    source "$HOME/esp/v5.4/esp-idf/export.sh" > /dev/null 2>&1
elif [ -n "$IDF_PATH" ]; then
    source "$IDF_PATH/export.sh" > /dev/null 2>&1
else
    echo "Error: ESP-IDF not found"
    exit 1
fi

# Auto-detect port or use argument
PORT="${1:-}"
if [ -z "$PORT" ]; then
    select_serial_port || true
fi

if [ -z "$PORT" ]; then
    echo "No serial port detected. Usage: $0 [PORT]"
    echo "Example: $0 /dev/tty.usbserial-1234"
    exit 1
fi

# Check if device has flash encryption enabled
echo "Checking device encryption status..."
EFUSE_SUMMARY=$(espefuse.py --port "$PORT" summary 2>/dev/null || true)

if echo "$EFUSE_SUMMARY" | grep -q "SPI_BOOT_CRYPT_CNT.*= 1\|SPI_BOOT_CRYPT_CNT.*= 3\|SPI_BOOT_CRYPT_CNT.*= 7"; then
    echo ""
    echo -e "${YELLOW}This device has flash encryption enabled!${NC}"
    echo "You must use the secure flash script instead:"
    echo ""
    echo "  ./scripts/flash-secure.sh $PORT"
    echo ""
    exit 1
fi

echo "Flashing to $PORT..."
idf.py -p "$PORT" flash

echo ""
echo "Flash complete! To monitor: ./scripts/monitor.sh $PORT"
