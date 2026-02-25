#!/bin/bash
# Flash zclaw to device

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PORT=""
KILL_MONITOR=false
BOARD_PRESET=""
BOARD_SDKCONFIG_FILE=""
IDF_TARGET_OVERRIDE=""
SDKCONFIG_DEFAULTS_OVERRIDE="sdkconfig.defaults"
SDKCONFIG_FILE_OVERRIDE="sdkconfig"

cd "$PROJECT_DIR"

# Colors
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
    echo "Usage: $0 [PORT] [--kill-monitor] [--board <preset>] [--box-3] [--s3-voice] [--s3-sense-voice]"
    echo "  --kill-monitor  Stop stale ESP-IDF monitor processes holding the selected port"
    echo "  --board         Apply a board preset (esp32s3-box-3 | esp32s3-voice | esp32s3-sense-voice)"
    echo "  --box-3         Alias for --board esp32s3-box-3"
    echo "  --s3-voice      Alias for --board esp32s3-voice"
    echo "  --s3-sense-voice Alias for --board esp32s3-sense-voice"
}

normalize_board_preset() {
    case "$1" in
        esp32s3-box-3|esp32-s3-box-3|box-3|esp-box-3)
            echo "esp32s3-box-3"
            ;;
        esp32s3-sense-voice|esp32-s3-sense-voice|s3-sense-voice|sense-voice|voice-s3)
            echo "esp32s3-sense-voice"
            ;;
        esp32s3-voice|esp32-s3-voice|s3-voice|voice-s3-generic)
            echo "esp32s3-voice"
            ;;
        *)
            echo ""
            ;;
    esac
}

resolve_board_preset() {
    local normalized

    [ -n "$BOARD_PRESET" ] || return 0

    normalized="$(normalize_board_preset "$BOARD_PRESET")"
    if [ -z "$normalized" ]; then
        echo "Error: Unknown board preset '$BOARD_PRESET'"
        echo "Supported presets: esp32s3-box-3, esp32s3-voice, esp32s3-sense-voice"
        return 1
    fi

    BOARD_PRESET="$normalized"
    case "$BOARD_PRESET" in
        esp32s3-box-3)
            BOARD_SDKCONFIG_FILE="sdkconfig.esp32s3-box-3.defaults"
            IDF_TARGET_OVERRIDE="esp32s3"
            ;;
        esp32s3-sense-voice)
            BOARD_SDKCONFIG_FILE="sdkconfig.esp32s3-sense-voice.defaults"
            IDF_TARGET_OVERRIDE="esp32s3"
            ;;
        esp32s3-voice)
            BOARD_SDKCONFIG_FILE="sdkconfig.esp32s3-voice.defaults"
            IDF_TARGET_OVERRIDE="esp32s3"
            ;;
        *)
            echo "Error: Unsupported board preset '$BOARD_PRESET'"
            return 1
            ;;
    esac

    if [ ! -f "$PROJECT_DIR/$BOARD_SDKCONFIG_FILE" ]; then
        echo "Error: Board preset file missing: $BOARD_SDKCONFIG_FILE"
        return 1
    fi

    SDKCONFIG_DEFAULTS_OVERRIDE="sdkconfig.defaults;$BOARD_SDKCONFIG_FILE"
    SDKCONFIG_FILE_OVERRIDE="build/sdkconfig.$BOARD_PRESET"
}

detect_serial_ports() {
    local ports=()
    local os_name

    os_name="$(uname -s)"
    shopt -s nullglob
    if [ "$os_name" = "Darwin" ]; then
        ports+=(/dev/cu.usbserial-*)
        ports+=(/dev/cu.usbmodem*)
        if [ "${#ports[@]}" -eq 0 ]; then
            ports+=(/dev/tty.usbserial-*)
            ports+=(/dev/tty.usbmodem*)
        fi
    else
        ports+=(/dev/ttyUSB*)
        ports+=(/dev/ttyACM*)
    fi
    shopt -u nullglob

    local p
    for p in "${ports[@]}"; do
        [ -e "$p" ] && echo "$p"
    done
}

normalize_serial_port() {
    local port="$1"
    local callout_port

    case "$port" in
        /dev/tty.usb*)
            callout_port="/dev/cu.${port#/dev/tty.}"
            if [ -e "$callout_port" ]; then
                echo "$callout_port"
                return
            fi
            ;;
    esac

    echo "$port"
}

port_is_busy() {
    local port="$1"

    if command -v lsof >/dev/null 2>&1; then
        lsof "$port" >/dev/null 2>&1
        return $?
    fi

    if command -v fuser >/dev/null 2>&1; then
        fuser "$port" >/dev/null 2>&1
        return $?
    fi

    return 1
}

show_port_holders() {
    local port="$1"

    if command -v lsof >/dev/null 2>&1; then
        lsof "$port" 2>/dev/null || true
        return
    fi

    if command -v fuser >/dev/null 2>&1; then
        fuser "$port" 2>/dev/null || true
    fi
}

print_port_busy_help() {
    local port="$1"

    echo ""
    echo -e "${YELLOW}Serial port appears busy: $port${NC}"
    echo "Close serial monitor tools (idf.py monitor, screen, CoolTerm, etc.) and retry."
    echo ""
    echo "Port holders:"
    show_port_holders "$port"
    echo ""
    echo "Then run:"
    echo "  ./scripts/flash.sh $port"
    echo "or:"
    echo "  ./scripts/flash.sh --kill-monitor $port"
    echo "or:"
    echo "  ./scripts/release-port.sh $port"
}

print_port_permission_help() {
    local port="$1"

    echo ""
    echo -e "${YELLOW}Serial port is not accessible: $port${NC}"
    echo "macOS denied access to the serial device."
    echo "Try:"
    echo "  1) Close monitor tools and unplug/replug the board"
    echo "  2) Re-open your terminal (or reboot) to refresh device permissions"
    echo "  3) Confirm the port exists: ls $port"
    echo ""
    echo "Then run:"
    echo "  ./scripts/flash.sh $port"
}

port_holder_pids() {
    local port="$1"

    if command -v lsof >/dev/null 2>&1; then
        lsof -t "$port" 2>/dev/null | sort -u || true
        return
    fi

    if command -v fuser >/dev/null 2>&1; then
        fuser "$port" 2>/dev/null | tr ' ' '\n' | sed -E 's/[^0-9].*$//' | sed '/^$/d' | sort -u || true
    fi
}

is_idf_monitor_process() {
    local pid="$1"
    local cmdline

    cmdline="$(ps -p "$pid" -o 'command=' 2>/dev/null || true)"
    if [ -z "$cmdline" ]; then
        cmdline="$(ps -p "$pid" -o command 2>/dev/null | sed -n '2p' | sed -E 's/^[[:space:]]+//')"
    fi
    case "$cmdline" in
        *esp_idf_monitor*|*idf_monitor.py*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

kill_idf_monitors_holding_port() {
    local port="$1"
    local pids
    local pid
    local matched=0
    local killed=0

    pids="$(port_holder_pids "$port")"
    [ -n "$pids" ] || return 1

    while IFS= read -r pid; do
        [ -n "$pid" ] || continue
        if is_idf_monitor_process "$pid"; then
            matched=$((matched + 1))
            echo "Stopping ESP-IDF monitor process: PID $pid"
            if kill "$pid" 2>/dev/null; then
                killed=$((killed + 1))
            fi
        fi
    done << EOF
$pids
EOF

    if [ "$matched" -eq 0 ]; then
        echo "No ESP-IDF monitor holders detected on $port."
        return 1
    fi

    sleep 1
    if [ "$killed" -gt 0 ] && ! port_is_busy "$port"; then
        echo "Released serial port: $port"
        return 0
    fi

    return 1
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

    if [ -n "$IDF_TARGET_OVERRIDE" ]; then
        if [ "$IDF_TARGET_OVERRIDE" = "$detected_target" ]; then
            return 0
        fi

        echo "Error: board preset '$BOARD_PRESET' requires target '$IDF_TARGET_OVERRIDE',"
        echo "but connected board is '$chip_name' ($detected_target)."
        return 1
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
        echo "Error: ESP-IDF not found"
    fi
    return 1
}

source_idf_env || exit 1

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --kill-monitor)
            KILL_MONITOR=true
            ;;
        --board)
            shift
            [ $# -gt 0 ] || { echo "Error: --board requires a value"; usage; exit 1; }
            BOARD_PRESET="$1"
            ;;
        --board=*)
            BOARD_PRESET="${1#*=}"
            ;;
        --box-3)
            BOARD_PRESET="esp32s3-box-3"
            ;;
        --s3-sense-voice)
            BOARD_PRESET="esp32s3-sense-voice"
            ;;
        --s3-voice)
            BOARD_PRESET="esp32s3-voice"
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        -*)
            echo "Error: unknown option '$1'"
            usage
            exit 1
            ;;
        *)
            if [ -z "$PORT" ]; then
                PORT="$1"
            else
                echo "Error: multiple ports provided: '$PORT' and '$1'"
                usage
                exit 1
            fi
            ;;
    esac
    shift
done

resolve_board_preset || exit 1

# Auto-detect port if needed
if [ -z "$PORT" ]; then
    select_serial_port || true
fi

if [ -z "$PORT" ]; then
    echo "No serial port detected. Usage: $0 [PORT]"
    echo "Example: $0 /dev/cu.usbmodem1101"
    exit 1
fi

NORMALIZED_PORT="$(normalize_serial_port "$PORT")"
if [ "$NORMALIZED_PORT" != "$PORT" ]; then
    echo "Using callout serial port: $NORMALIZED_PORT"
    PORT="$NORMALIZED_PORT"
fi

if port_is_busy "$PORT"; then
    if [ "$KILL_MONITOR" = true ]; then
        echo "Port is busy. Trying to stop stale ESP-IDF monitor holders..."
        kill_idf_monitors_holding_port "$PORT" || true
    fi
fi

if port_is_busy "$PORT"; then
    print_port_busy_help "$PORT"
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
    if [ -n "$BOARD_PRESET" ]; then
        echo "  ./scripts/flash-secure.sh --board $BOARD_PRESET $PORT"
    else
        echo "  ./scripts/flash-secure.sh $PORT"
    fi
    echo ""
    exit 1
fi

echo "Flashing to $PORT..."
if [ -n "$BOARD_PRESET" ]; then
    echo "Board preset: $BOARD_PRESET"
fi
FLASH_LOG="$(mktemp -t zclaw-flash.XXXXXX.log 2>/dev/null || mktemp)"
set +e
set -o pipefail
idf_flash_cmd=(idf.py)
if [ -n "$IDF_TARGET_OVERRIDE" ]; then
    idf_flash_cmd+=(-D "IDF_TARGET=$IDF_TARGET_OVERRIDE")
fi
if [ -n "$SDKCONFIG_DEFAULTS_OVERRIDE" ]; then
    idf_flash_cmd+=(-D "SDKCONFIG_DEFAULTS=$SDKCONFIG_DEFAULTS_OVERRIDE")
fi
if [ -n "$SDKCONFIG_FILE_OVERRIDE" ]; then
    mkdir -p "$PROJECT_DIR/$(dirname "$SDKCONFIG_FILE_OVERRIDE")"
    idf_flash_cmd+=(-D "SDKCONFIG=$SDKCONFIG_FILE_OVERRIDE")
fi
idf_flash_cmd+=(-p "$PORT" flash)
"${idf_flash_cmd[@]}" 2>&1 | tee "$FLASH_LOG"
FLASH_RC=${PIPESTATUS[0]}
set +o pipefail
set -e

if [ "$FLASH_RC" -ne 0 ]; then
    if grep -Eqi "operation not permitted|permission denied" "$FLASH_LOG"; then
        print_port_permission_help "$PORT"
    elif grep -Eqi "port is busy|resource busy|could not open .*/dev/(tty|cu)" "$FLASH_LOG"; then
        print_port_busy_help "$PORT"
    fi
    rm -f "$FLASH_LOG"
    exit "$FLASH_RC"
fi
rm -f "$FLASH_LOG"

echo ""
echo "Flash complete!"
echo "Next:"
echo "  1) Provision credentials: ./scripts/provision.sh --port $PORT"
echo "  2) Monitor logs:          ./scripts/monitor.sh $PORT"
