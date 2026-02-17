#!/bin/bash
#
# Secure Flash Script for zclaw
# Handles flash encryption setup and encrypted flashing
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
KEY_DIR="$PROJECT_DIR/keys"
BUILD_DIR="$PROJECT_DIR/build-secure"
PRODUCTION_MODE=false
PORT=""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

print_status() { echo -e "${GREEN}✓${NC} $1"; }
print_warning() { echo -e "${YELLOW}!${NC} $1"; }
print_error() { echo -e "${RED}✗${NC} $1"; }

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

    print_error "Target mismatch; refusing secure flash."
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

usage() {
    echo "Usage: $0 [PORT] [--production]"
    echo "  --production  Burn key with hardware read protection (recommended for deployed devices)"
}

cd "$PROJECT_DIR"

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --production)
            PRODUCTION_MODE=true
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        -*)
            print_error "Unknown option: $arg"
            usage
            exit 1
            ;;
        *)
            if [ -z "$PORT" ]; then
                PORT="$arg"
            else
                print_error "Multiple ports provided: '$PORT' and '$arg'"
                usage
                exit 1
            fi
            ;;
    esac
done

# Find and source ESP-IDF
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    source "$HOME/esp/esp-idf/export.sh" > /dev/null 2>&1
elif [ -f "$HOME/esp/v5.4/esp-idf/export.sh" ]; then
    source "$HOME/esp/v5.4/esp-idf/export.sh" > /dev/null 2>&1
elif [ -n "$IDF_PATH" ]; then
    source "$IDF_PATH/export.sh" > /dev/null 2>&1
else
    print_error "ESP-IDF not found"
    exit 1
fi

# Auto-detect port if not provided
if [ -z "$PORT" ]; then
    select_serial_port || true
fi

if [ -z "$PORT" ]; then
    print_error "No serial port detected"
    usage
    echo "Example: $0 /dev/tty.usbserial-1234"
    exit 1
fi

# Detect chip and ensure target matches before secure build/flash.
CHIP_NAME="$(detect_chip_name "$PORT")"
if [ -n "$CHIP_NAME" ]; then
    DETECTED_TARGET="$(chip_name_to_target "$CHIP_NAME")"
    if ! ensure_target_matches_connected_board "$CHIP_NAME" "$DETECTED_TARGET"; then
        exit 1
    fi
else
    print_warning "Could not detect chip type on $PORT; continuing without target check."
fi

echo ""
echo -e "${CYAN}${BOLD}"
cat << 'EOF'
    ╔═══════════════════════════════════════════════════════════╗
    ║              ZCLAW SECURE FLASH                        ║
    ╚═══════════════════════════════════════════════════════════╝
EOF
echo -e "${NC}"

if [ "$PRODUCTION_MODE" = true ]; then
    print_warning "Production mode enabled: encryption key will be read-protected"
else
    print_warning "Development mode: encryption key remains readable for USB reflash workflows"
fi

# Get chip info
echo "Reading device info from $PORT..."
CHIP_INFO=$(esptool.py --port "$PORT" chip_id 2>/dev/null || true)

if [ -z "$CHIP_INFO" ]; then
    print_error "Could not read chip info. Check connection."
    exit 1
fi

# Extract MAC for unique device ID
MAC=$(echo "$CHIP_INFO" | grep -i "MAC:" | head -1 | awk '{print $2}' | tr -d ':' | tr '[:lower:]' '[:upper:]')
if [ -z "$MAC" ]; then
    print_error "Could not read device MAC address"
    exit 1
fi

KEY_FILE="$KEY_DIR/flash_key_${MAC}.bin"
echo "Device MAC: $MAC"

# Check if device already has flash encryption enabled
echo "Checking flash encryption status..."
EFUSE_SUMMARY=$(espefuse.py --port "$PORT" summary 2>/dev/null || true)

ENCRYPTION_ENABLED=false
if flash_encryption_enabled "$EFUSE_SUMMARY"; then
    ENCRYPTION_ENABLED=true
fi

# Create keys directory
mkdir -p "$KEY_DIR"

if [ "$ENCRYPTION_ENABLED" = true ]; then
    echo ""
    print_status "Device has flash encryption enabled"

    if [ -f "$KEY_FILE" ]; then
        print_status "Found matching key file"
        echo ""
        echo "Building and flashing with encryption..."

        # Build with secure config
        idf.py -B "$BUILD_DIR" \
            -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secure" \
            build

        # Flash using the encrypted-flash target with saved key
        idf.py -B "$BUILD_DIR" -p "$PORT" encrypted-flash

        echo ""
        print_status "Secure flash complete!"
    else
        print_error "No key file found for this device: $KEY_FILE"
        echo ""
        echo "Options:"
        echo "  1. If you have the key file, copy it to: $KEY_FILE"
        echo "  2. Use OTA update from a running firmware that supports it"
        echo ""
        exit 1
    fi
else
    echo ""
    print_warning "Device does NOT have flash encryption enabled"
    echo ""
    echo -e "${YELLOW}┌─────────────────────────────────────────────────────────────┐${NC}"
    echo -e "${YELLOW}│  WARNING: FLASH ENCRYPTION IS PERMANENT!                    │${NC}"
    echo -e "${YELLOW}│                                                             │${NC}"
    echo -e "${YELLOW}│  Once enabled, you CANNOT flash unencrypted firmware.       │${NC}"
    echo -e "${YELLOW}│  The encryption key will be saved to: keys/                 │${NC}"
    echo -e "${YELLOW}│  BACK UP THIS KEY FILE - you need it for future flashes!    │${NC}"
    echo -e "${YELLOW}└─────────────────────────────────────────────────────────────┘${NC}"
    echo ""

    read -p "Enable flash encryption on this device? [y/N] " confirm
    if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 0
    fi

    echo ""
    echo "Step 1/4: Generating encryption key..."
    espsecure.py generate_flash_encryption_key "$KEY_FILE"
    print_status "Key saved to: $KEY_FILE"

    echo ""
    echo "Step 2/4: Building with secure configuration..."
    idf.py -B "$BUILD_DIR" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secure" \
        build
    print_status "Build complete"

    echo ""
    echo "Step 3/4: Burning encryption key to device eFuse..."
    echo -e "${YELLOW}This step is PERMANENT and cannot be undone!${NC}"
    if [ "$PRODUCTION_MODE" = true ]; then
        espefuse.py --port "$PORT" burn_key BLOCK_KEY0 "$KEY_FILE" XTS_AES_128_KEY
    else
        espefuse.py --port "$PORT" burn_key BLOCK_KEY0 "$KEY_FILE" XTS_AES_128_KEY --no-protect-key
    fi
    print_status "Encryption key burned to eFuse"

    echo ""
    echo "Step 4/4: Flashing encrypted firmware..."
    idf.py -B "$BUILD_DIR" -p "$PORT" encrypted-flash

    echo ""
    echo -e "${GREEN}${BOLD}"
    cat << 'EOF'
    ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
    ┃              SECURE FLASH COMPLETE                        ┃
    ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
EOF
    echo -e "${NC}"
    echo -e "  ${BOLD}Encryption key:${NC}"
    echo -e "    ${CYAN}$KEY_FILE${NC}"
    echo ""
    echo -e "  ${RED}${BOLD}Back up this file!${NC} ${DIM}Without it, USB flashing won't work.${NC}"
    echo -e "  ${DIM}OTA updates will still work regardless.${NC}"
    echo ""
fi

echo "To monitor: ./scripts/monitor.sh $PORT"
