#!/bin/bash
# Start zclaw web relay with serial-port contention guards.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

KILL_MONITOR=false
MOCK_AGENT=false
PORT=""
RELAY_ARGS=()

usage() {
    echo "Usage: $0 [PORT] [--kill-monitor] [relay args]"
    echo ""
    echo "Examples:"
    echo "  $0 --port 8787"
    echo "  $0 /dev/cu.usbmodem1101 --port 8787"
    echo "  ZCLAW_WEB_API_KEY='long-random-secret' $0 --host 0.0.0.0 --port 8787"
    echo "  $0 --kill-monitor --serial-port /dev/cu.usbmodem1101 --port 8787"
    echo "  $0 --mock-agent --port 8787"
    echo "  $0 --serial-port /dev/cu.usbmodem1101 --log-file /tmp/zclaw-relay.log --log-serial"
    echo ""
    echo "Wrapper-only options:"
    echo "  --kill-monitor   Stop ESP-IDF monitor holders on the selected serial port"
    echo ""
    echo "Notes:"
    echo "  - web_relay.py defaults to --host 127.0.0.1"
    echo "  - binding non-loopback hosts requires ZCLAW_WEB_API_KEY"
    echo "  - voice STT sideband needs ZCLAW_STT_API_KEY (or OPENAI_API_KEY)"
    echo ""
    echo "All other flags are forwarded to scripts/web_relay.py."
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
    echo "Serial port appears busy: $port"
    echo "Close serial monitor tools (idf.py monitor, screen, CoolTerm, etc.) and retry."
    echo ""
    echo "Port holders:"
    show_port_holders "$port"
    echo ""
    echo "Then run:"
    echo "  ./scripts/web-relay.sh --serial-port $port"
    echo "or:"
    echo "  ./scripts/web-relay.sh --kill-monitor --serial-port $port"
    echo "or:"
    echo "  ./scripts/release-port.sh $port"
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

require_value_arg() {
    local flag="$1"
    local value="${2:-}"
    if [ -z "$value" ]; then
        echo "Error: $flag requires a value."
        usage
        exit 1
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --kill-monitor)
            KILL_MONITOR=true
            shift
            ;;
        --mock-agent)
            MOCK_AGENT=true
            RELAY_ARGS+=("$1")
            shift
            ;;
        --serial-port)
            require_value_arg "--serial-port" "${2:-}"
            PORT="$2"
            shift 2
            ;;
        --serial-port=*)
            PORT="${1#*=}"
            shift
            ;;
        --host|--port|--baud|--serial-timeout|--response-timeout|--idle-timeout|--mock-latency|--log-file|--cors-origin)
            require_value_arg "$1" "${2:-}"
            RELAY_ARGS+=("$1" "$2")
            shift 2
            ;;
        --host=*|--port=*|--baud=*|--serial-timeout=*|--response-timeout=*|--idle-timeout=*|--mock-latency=*|--log-file=*|--cors-origin=*)
            RELAY_ARGS+=("$1")
            shift
            ;;
        --log-serial|--debug)
            RELAY_ARGS+=("$1")
            shift
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
                shift
            else
                echo "Error: multiple ports provided: '$PORT' and '$1'"
                usage
                exit 1
            fi
            ;;
    esac
done

if [ "$MOCK_AGENT" != true ]; then
    if [ -z "$PORT" ]; then
        select_serial_port || true
    fi

    if [ -z "$PORT" ]; then
        echo "No serial port detected. Use --serial-port or pass PORT as first arg."
        exit 1
    fi

    NORMALIZED_PORT="$(normalize_serial_port "$PORT")"
    if [ "$NORMALIZED_PORT" != "$PORT" ]; then
        echo "Using callout serial port: $NORMALIZED_PORT"
        PORT="$NORMALIZED_PORT"
    fi

    if port_is_busy "$PORT"; then
        if [ "$KILL_MONITOR" = true ]; then
            echo "Port is busy. Trying to stop ESP-IDF monitor holders..."
            kill_idf_monitors_holding_port "$PORT" || true
        fi
    fi

    if port_is_busy "$PORT"; then
        print_port_busy_help "$PORT"
        exit 1
    fi

    RELAY_ARGS+=(--serial-port "$PORT")
fi

if command -v uv >/dev/null 2>&1; then
    exec uv run --with-requirements scripts/requirements-web-relay.txt \
        scripts/web_relay.py "${RELAY_ARGS[@]}"
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: neither 'uv' nor 'python3' is available."
    echo "Install uv (recommended), or install Python 3 and retry."
    exit 1
fi

echo "uv not found; falling back to python3 runtime."
if ! python3 -c "import serial" >/dev/null 2>&1; then
    if ! python3 -m pip --version >/dev/null 2>&1; then
        echo "Error: python3 is available but pip is missing."
        echo "Install uv (recommended), or install pip for Python 3 and retry."
        exit 1
    fi
    echo "Installing relay dependencies with pip (user site)..."
    if ! python3 -m pip install --user -r scripts/requirements-web-relay.txt; then
        echo "Error: failed to install relay dependencies via pip."
        exit 1
    fi
fi

exec python3 scripts/web_relay.py "${RELAY_ARGS[@]}"
