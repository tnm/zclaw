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

detect_chip_name() {
    local port="$1"
    local chip_info
    local chip_name
    chip_info=$(esptool.py --port "$port" chip_id 2>/dev/null || true)

    # Common format: "Chip is ESP32-C3 (QFN32) ..."
    chip_name=$(echo "$chip_info" | sed -nE 's/.*Chip is ([^,(]+).*/\1/p' | head -1 | xargs)
    if [ -n "$chip_name" ]; then
        echo "$chip_name"
        return
    fi

    # Fallback format: "Detecting chip type... ESP32-C3"
    chip_name=$(echo "$chip_info" | sed -nE 's/.*Detecting chip type\.\.\. ([A-Za-z0-9-]+).*/\1/p' | head -1 | xargs)
    echo "$chip_name"
}

chip_name_to_target() {
    local chip_name="$1"
    case "$chip_name" in
        "ESP32-S2"*) echo "esp32s2" ;;
        "ESP32-S3"*) echo "esp32s3" ;;
        "ESP32-C2"*) echo "esp32c2" ;;
        "ESP32-C3"*) echo "esp32c3" ;;
        "ESP32-C6"*) echo "esp32c6" ;;
        "ESP32-H2"*) echo "esp32h2" ;;
        "ESP32-P4"*) echo "esp32p4" ;;
        "ESP32"*) echo "esp32" ;;
        *) echo "" ;;
    esac
}

project_target() {
    local cfg="$PROJECT_DIR/sdkconfig"
    if [ ! -f "$cfg" ]; then
        echo ""
        return
    fi
    grep '^CONFIG_IDF_TARGET=' "$cfg" | head -1 | cut -d'"' -f2
}

ensure_target_matches_connected_board() {
    local chip_name="$1"
    local detected_target="$2"
    local current_target

    if [ -z "$detected_target" ]; then
        return 0
    fi

    current_target="$(project_target)"
    if [ -z "$current_target" ] || [ "$current_target" = "$detected_target" ]; then
        return 0
    fi

    echo ""
    echo "Detected board chip: $chip_name ($detected_target)"
    echo "Current project target: $current_target"
    echo ""

    if [ -t 0 ]; then
        read -r -p "Switch project target to $detected_target now with 'idf.py set-target $detected_target'? [Y/n] " switch_target
        switch_target="${switch_target:-Y}"
        if [[ "$switch_target" =~ ^[Yy]$ ]]; then
            idf.py set-target "$detected_target"
            echo "Project target set to $detected_target."
            return 0
        fi
    fi

    echo "Target mismatch; refusing to flash."
    echo "Run: idf.py set-target $detected_target"
    return 1
}

flash_encryption_enabled() {
    local summary="$1"
    local raw_value
    local value

    raw_value=$(echo "$summary" | awk -F= '/SPI_BOOT_CRYPT_CNT|FLASH_CRYPT_CNT/ {print $2; exit}' | awk '{print $1}')
    if [ -z "$raw_value" ]; then
        return 1
    fi

    if [[ "$raw_value" =~ ^0x[0-9A-Fa-f]+$ ]]; then
        value=$((raw_value))
    elif [[ "$raw_value" =~ ^[0-9]+$ ]]; then
        value="$raw_value"
    elif [[ "$raw_value" = "0b001" || "$raw_value" = "0b011" || "$raw_value" = "0b111" ]]; then
        return 0
    else
        return 1
    fi

    # For FLASH_CRYPT/SPI_BOOT_CRYPT counters, odd means encryption is enabled.
    [ $((value % 2)) -eq 1 ]
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

# Detect chip and ensure target matches before any flash operation.
CHIP_NAME="$(detect_chip_name "$PORT")"
if [ -n "$CHIP_NAME" ]; then
    DETECTED_TARGET="$(chip_name_to_target "$CHIP_NAME")"
    if ! ensure_target_matches_connected_board "$CHIP_NAME" "$DETECTED_TARGET"; then
        exit 1
    fi
else
    echo "Warning: could not detect chip type on $PORT; continuing without target check."
fi

# Check if device has flash encryption enabled
echo "Checking device encryption status..."
EFUSE_SUMMARY=$(espefuse.py --port "$PORT" summary 2>/dev/null || true)

if flash_encryption_enabled "$EFUSE_SUMMARY"; then
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
