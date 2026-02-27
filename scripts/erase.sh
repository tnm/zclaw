#!/bin/bash
# Erase NVS settings or full flash with explicit guardrails.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PORT=""
MODE=""
ASSUME_YES=false
DRY_RUN=false
KILL_MONITOR=false
COMMAND_EXECUTED=false

YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
    cat << EOF
Usage: $0 [options] [PORT]

Erase modes (choose exactly one):
  --nvs                   Erase only NVS (credentials/settings)
  --all                   Erase entire flash (firmware + data)

Options:
  --port <serial-port>    Serial port (auto-detect if omitted)
  --yes                   Skip interactive confirmation prompts
  --dry-run               Print resolved erase command without executing
  --kill-monitor          Stop stale ESP-IDF monitor process holding the port
  -h, --help              Show help

Examples:
  ./scripts/erase.sh --nvs
  ./scripts/erase.sh --all --port /dev/cu.usbmodem1101
  ./scripts/erase.sh --all --yes --port /dev/cu.usbmodem1101
EOF
}

set_mode() {
    local next_mode="$1"

    if [ -n "$MODE" ] && [ "$MODE" != "$next_mode" ]; then
        echo "Error: choose one of --nvs or --all (not both)."
        exit 1
    fi

    MODE="$next_mode"
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

    if [ -t 0 ] && [ "$ASSUME_YES" != true ]; then
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
            if [ -z "${IDF_PATH:-}" ]; then
                IDF_PATH="$(cd "$(dirname "$script")" && pwd)"
            fi
            return 0
        fi
    done

    if [ "$found" -eq 1 ]; then
        echo "Error: ESP-IDF found but failed to activate."
        echo "Run:"
        echo "  cd ~/esp/esp-idf && ./install.sh esp32,esp32c3,esp32c6,esp32s3"
    else
        echo "Error: ESP-IDF not found"
    fi
    return 1
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
    echo "  ./scripts/erase.sh --$MODE $port"
    echo "or:"
    echo "  ./scripts/erase.sh --$MODE --kill-monitor $port"
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

print_command_line() {
    local arg

    printf "Command:"
    for arg in "$@"; do
        printf " %q" "$arg"
    done
    printf "\n"
}

run_command() {
    local log_file

    print_command_line "$@"
    if [ "$DRY_RUN" = true ]; then
        echo "Dry run only; erase command not executed."
        return 0
    fi

    log_file="$(mktemp -t zclaw-erase.XXXXXX.log 2>/dev/null || mktemp)"
    set +e
    set -o pipefail
    "$@" 2>&1 | tee "$log_file"
    local rc=${PIPESTATUS[0]}
    set +o pipefail
    set -e

    if [ "$rc" -ne 0 ]; then
        if grep -Eqi "operation not permitted|permission denied" "$log_file"; then
            print_port_permission_help "$PORT"
        elif grep -Eqi "port is busy|resource busy|could not open .*/dev/(tty|cu)" "$log_file"; then
            print_port_busy_help "$PORT"
        fi
        rm -f "$log_file"
        return "$rc"
    fi

    rm -f "$log_file"
    COMMAND_EXECUTED=true
}

confirm_action() {
    local confirm

    if [ "$ASSUME_YES" = true ] || [ "$DRY_RUN" = true ]; then
        return 0
    fi

    if [ ! -t 0 ]; then
        echo "Error: interactive confirmation required in non-interactive mode."
        echo "Re-run with --yes after verifying the target port."
        return 1
    fi

    case "$MODE" in
        nvs)
            read -r -p "Erase NVS on $PORT? [y/N] " confirm
            [[ "$confirm" =~ ^[Yy]$ ]]
            ;;
        all)
            echo "DANGER: full flash erase removes firmware and all stored data."
            read -r -p "Type ERASE ALL to continue: " confirm
            [ "$confirm" = "ERASE ALL" ]
            ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --port)
            if [ $# -lt 2 ]; then
                echo "Error: --port requires a value."
                usage
                exit 1
            fi
            PORT="$2"
            shift 2
            ;;
        --nvs)
            set_mode "nvs"
            shift
            ;;
        --all)
            set_mode "all"
            shift
            ;;
        --yes)
            ASSUME_YES=true
            shift
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --kill-monitor)
            KILL_MONITOR=true
            shift
            ;;
        -h|--help)
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
                shift
            else
                echo "Error: multiple ports provided: '$PORT' and '$1'"
                usage
                exit 1
            fi
            ;;
    esac
done

if [ -z "$MODE" ]; then
    echo "Error: choose one of --nvs or --all."
    usage
    exit 1
fi

source_idf_env || exit 1

if [ -z "$PORT" ]; then
    select_serial_port || true
fi

if [ -z "$PORT" ]; then
    echo "Error: no serial port detected. Use --port."
    exit 1
fi

NORMALIZED_PORT="$(normalize_serial_port "$PORT")"
if [ "$NORMALIZED_PORT" != "$PORT" ]; then
    echo "Using callout serial port: $NORMALIZED_PORT"
    PORT="$NORMALIZED_PORT"
fi

if [ ! -e "$PORT" ]; then
    echo "Error: serial port not found: $PORT"
    exit 1
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

echo "Target port: $PORT"
case "$MODE" in
    nvs)
        echo "Action: erase NVS partition (credentials/settings)."
        ;;
    all)
        echo "Action: erase entire flash (firmware + credentials + settings)."
        ;;
esac

if ! confirm_action; then
    echo "Aborted."
    exit 1
fi

if [ "$MODE" = "nvs" ]; then
    PARTTOOL="$IDF_PATH/components/partition_table/parttool.py"
    if [ ! -f "$PARTTOOL" ]; then
        echo "Error: parttool.py not found at $PARTTOOL"
        exit 1
    fi

    PYTHON_BIN=""
    if command -v python3 >/dev/null 2>&1; then
        PYTHON_BIN="python3"
    elif command -v python >/dev/null 2>&1; then
        PYTHON_BIN="python"
    else
        echo "Error: python3/python is required for parttool.py"
        exit 1
    fi

    run_command "$PYTHON_BIN" "$PARTTOOL" --port "$PORT" erase_partition --partition-name nvs
else
    run_command idf.py -p "$PORT" erase-flash
fi

echo ""
if [ "$COMMAND_EXECUTED" = true ]; then
    if [ "$MODE" = "nvs" ]; then
        echo "NVS erase complete."
        echo "Next:"
        echo "  1) Re-provision credentials: ./scripts/provision.sh --port $PORT"
        echo "  2) Monitor startup:          ./scripts/monitor.sh $PORT"
    else
        echo "Full flash erase complete."
        echo "Next:"
        echo "  1) Flash firmware:           ./scripts/flash.sh $PORT"
        echo "  2) Re-provision credentials: ./scripts/provision.sh --port $PORT"
        echo "  3) Monitor startup:          ./scripts/monitor.sh $PORT"
    fi
else
    echo "Dry run complete. No erase was performed."
fi
