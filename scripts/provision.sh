#!/bin/bash
# Provision runtime credentials into NVS (no web setup required).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PORT=""
WIFI_SSID=""
WIFI_PASS=""
BACKEND=""
MODEL=""
API_KEY=""
API_URL=""
TG_TOKEN=""
TG_CHAT_IDS=""
EMAIL_BRIDGE_URL=""
EMAIL_BRIDGE_KEY=""
ASSUME_YES=false
VERIFY_API_KEY=true
PRINT_DETECTED_SSID=false
WIFI_SSID_MAX_LEN=32
WIFI_PASS_MAX_LEN=63
WIFI_PASS_MIN_LEN=8

usage() {
    cat << EOF
Usage: $0 [options]

Options:
  --port <serial-port>      Serial port (auto-detect if omitted)
  --ssid <wifi-ssid>        WiFi SSID (auto-detected when possible)
  --pass <wifi-pass>        WiFi password (optional)
  --backend <provider>      anthropic | openai | openrouter | ollama
  --model <model-id>        Model ID (defaults by backend)
  --api-key <key>           LLM API key (required for anthropic/openai/openrouter)
  --api-url <url>           Optional custom API endpoint URL
  --tg-token <token>        Telegram bot token (optional)
  --tg-chat-id <id[,id...]> Telegram chat ID allowlist (optional)
  --tg-chat-ids <list>      Alias of --tg-chat-id
  --email-bridge-url <url>  Email bridge base URL (optional, required for email tools)
  --email-bridge-key <key>  Email bridge API key/token (optional)
  --yes                     Non-interactive (requires --api-key except ollama; SSID auto-detect if possible)
  --skip-api-check          Skip live API key verification step
  --print-detected-ssid     Print detected host WiFi SSID and exit (test/troubleshooting helper)
  -h, --help                Show help
EOF
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

is_placeholder_ssid() {
    local ssid="$1"
    case "$ssid" in
        "<redacted>"|"<hidden>"|"[hidden]"|"***")
            return 0
            ;;
    esac
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

detect_host_wifi_ssid() {
    local ssid=""

    if [ -n "${ZCLAW_WIFI_SSID:-}" ]; then
        if ! is_placeholder_ssid "$ZCLAW_WIFI_SSID"; then
            echo "$ZCLAW_WIFI_SSID"
            return 0
        fi
    fi

    case "$(uname -s)" in
        Darwin)
            # Deprecated, but still present on current macOS and often the most direct.
            if [ -x "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport" ]; then
                ssid="$(/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport -I 2>/dev/null | sed -nE 's/^[[:space:]]*SSID:[[:space:]]*(.+)$/\1/p' | head -1)"
                if [ -n "$ssid" ] && ! is_placeholder_ssid "$ssid"; then
                    echo "$ssid"
                    return 0
                fi
            fi

            # Fallback that works in some shells where networksetup/scutil is restricted.
            if command -v system_profiler >/dev/null 2>&1; then
                ssid="$(system_profiler SPAirPortDataType 2>/dev/null | awk '
                    /Current Network Information:/ {in_block=1; next}
                    in_block && /^[[:space:]]+[[:graph:]].*:$/ {
                        line=$0
                        sub(/^[[:space:]]+/, "", line)
                        sub(/:$/, "", line)
                        print line
                        exit
                    }
                    in_block && /^$/ {in_block=0}
                ' | head -1)"
                if [ -n "$ssid" ] && ! is_placeholder_ssid "$ssid"; then
                    echo "$ssid"
                    return 0
                fi
            fi

            if command -v networksetup >/dev/null 2>&1; then
                local dev
                while IFS= read -r dev; do
                    [ -n "$dev" ] || continue
                    ssid="$(networksetup -getairportnetwork "$dev" 2>/dev/null | sed -nE 's/^Current Wi-Fi Network: (.*)$/\1/p')"
                    if [ -n "$ssid" ] && ! is_placeholder_ssid "$ssid"; then
                        echo "$ssid"
                        return 0
                    fi
                done < <(
                    networksetup -listallhardwareports 2>/dev/null | awk '
                        /^Hardware Port: Wi-Fi$/ { getline; if ($1 == "Device:") print $2 }
                        /^Hardware Port: AirPort$/ { getline; if ($1 == "Device:") print $2 }
                    '
                )
            fi
            ;;
        Linux)
            if command -v nmcli >/dev/null 2>&1; then
                ssid="$(nmcli -t -f active,ssid dev wifi 2>/dev/null | awk -F: '$1=="yes" {print $2; exit}')"
                if [ -n "$ssid" ] && ! is_placeholder_ssid "$ssid"; then
                    echo "$ssid"
                    return 0
                fi
            fi
            if command -v iwgetid >/dev/null 2>&1; then
                ssid="$(iwgetid -r 2>/dev/null || true)"
                if [ -n "$ssid" ] && ! is_placeholder_ssid "$ssid"; then
                    echo "$ssid"
                    return 0
                fi
            fi
            ;;
    esac

    return 1
}

validate_wifi_ssid_length() {
    local ssid_len
    ssid_len="$(LC_ALL=C printf '%s' "$WIFI_SSID" | wc -c | tr -d '[:space:]')"

    if [ "$ssid_len" -eq 0 ]; then
        echo "Error: WiFi SSID is required"
        return 1
    fi

    if [ "$ssid_len" -gt "$WIFI_SSID_MAX_LEN" ]; then
        echo "Error: WiFi SSID must be at most ${WIFI_SSID_MAX_LEN} bytes (got ${ssid_len})."
        return 1
    fi

    return 0
}

validate_wifi_password_length() {
    local pass_len
    pass_len="$(LC_ALL=C printf '%s' "$WIFI_PASS" | wc -c | tr -d '[:space:]')"

    if [ "$pass_len" -eq 0 ]; then
        return 0
    fi

    if [ "$pass_len" -gt "$WIFI_PASS_MAX_LEN" ]; then
        echo "  Error: password exceeds ${WIFI_PASS_MAX_LEN} bytes."
        return 1
    fi

    if [ "$pass_len" -lt "$WIFI_PASS_MIN_LEN" ]; then
        echo "  Error: password must be ${WIFI_PASS_MIN_LEN}-${WIFI_PASS_MAX_LEN} bytes, or empty for open network."
        return 1
    fi

    return 0
}

check_wifi_credentials() {
    local current_ssid
    local warnings=0
    local confirm

    echo ""
    echo "WiFi credentials check:"
    echo "  SSID: $WIFI_SSID"

    if [ -n "$WIFI_PASS" ]; then
        echo "  Password: $WIFI_PASS"

        if ! validate_wifi_password_length; then
            return 1
        fi

        if [[ "$WIFI_PASS" =~ ^[[:space:]] || "$WIFI_PASS" =~ [[:space:]]$ ]]; then
            echo "  Warning: password has leading or trailing whitespace."
            warnings=1
        fi
    else
        echo "  Password: (empty/open network)"
    fi

    current_ssid="$(detect_host_wifi_ssid || true)"
    if [ -n "$current_ssid" ]; then
        if [ "$current_ssid" = "$WIFI_SSID" ]; then
            echo "  Host WiFi: connected to '$current_ssid'"
        else
            echo "  Warning: host is connected to '$current_ssid' (not '$WIFI_SSID')."
            warnings=1
        fi
    else
        echo "  Warning: could not detect current host WiFi network."
        warnings=1
    fi

    if [ "$ASSUME_YES" = true ]; then
        if [ "$warnings" -ne 0 ]; then
            echo "  Continuing in --yes mode despite WiFi warnings."
        fi
        return 0
    fi

    read -r -p "Use these WiFi credentials? [Y/n] " confirm
    confirm="${confirm:-Y}"
    [[ "$confirm" =~ ^[Yy]$ ]]
}

prompt_wifi_password() {
    local confirm

    while true; do
        read -r -p "WiFi password (visible; leave blank for open network): " WIFI_PASS
        echo "Entered WiFi password: ${WIFI_PASS:-<empty>}"
        read -r -p "Use this password? [Y/n] " confirm
        confirm="${confirm:-Y}"
        if [[ "$confirm" =~ ^[Yy]$ ]]; then
            return 0
        fi
    done
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

default_model_for_backend() {
    case "$1" in
        anthropic) echo "claude-sonnet-4-5" ;;
        openai) echo "gpt-5.2" ;;
        openrouter) echo "minimax/minimax-m2.5" ;;
        ollama) echo "qwen3:8b" ;;
        *) echo "claude-sonnet-4-5" ;;
    esac
}

validate_backend() {
    case "$1" in
        anthropic|openai|openrouter|ollama) return 0 ;;
        *) return 1 ;;
    esac
}

normalize_ollama_api_url() {
    local raw="$1"
    local trimmed
    local no_query
    local scheme_rest

    trimmed="$(trim_spaces "$raw")"
    if [ -z "$trimmed" ]; then
        return 1
    fi

    no_query="${trimmed%%\?*}"
    no_query="${no_query%%\#*}"
    no_query="${no_query%/}"

    if [[ ! "$no_query" =~ ^https?:// ]]; then
        return 1
    fi

    if [[ "$no_query" == */v1/chat/completions ]]; then
        printf '%s\n' "$no_query"
        return 0
    fi

    if [[ "$no_query" == */v1 ]]; then
        printf '%s/chat/completions\n' "$no_query"
        return 0
    fi

    scheme_rest="${no_query#*://}"
    if [[ "$scheme_rest" != */* ]]; then
        printf '%s/v1/chat/completions\n' "$no_query"
        return 0
    fi

    printf '%s\n' "$no_query"
    return 0
}

models_endpoint_from_chat_endpoint() {
    local api_url="$1"
    if [[ "$api_url" == */chat/completions ]]; then
        printf '%s\n' "${api_url%/chat/completions}/models"
        return
    fi
    printf '%s\n' "$api_url"
}

trim_spaces() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

normalize_telegram_chat_ids() {
    local raw="$1"
    local token
    local seen=" "
    local normalized=""
    local count=0
    local max_ids=4
    local IFS=','
    local part

    for part in $raw; do
        token="$(trim_spaces "$part")"
        if [ -z "$token" ]; then
            continue
        fi

        if ! [[ "$token" =~ ^-?[0-9]+$ ]] || [ "$token" = "0" ]; then
            return 1
        fi

        if [[ "$seen" == *" $token "* ]]; then
            continue
        fi

        if [ "$count" -ge "$max_ids" ]; then
            return 1
        fi

        seen="${seen}${token} "
        if [ -z "$normalized" ]; then
            normalized="$token"
        else
            normalized="$normalized,$token"
        fi
        count=$((count + 1))
    done

    if [ "$count" -eq 0 ]; then
        return 1
    fi

    printf '%s\n' "$normalized"
}

first_telegram_chat_id() {
    local raw="$1"
    local IFS=','
    local first=""
    local _rest=""
    read -r first _rest <<< "$raw"
    printf '%s\n' "$first"
}

csv_escape() {
    local value="$1"
    value="${value//$'\r'/ }"
    value="${value//$'\n'/ }"
    value="${value//\"/\"\"}"
    printf "\"%s\"" "$value"
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

    [ $((value % 2)) -eq 1 ]
}

verify_anthropic_api_key() {
    local api_key="$1"
    local model="$2"
    local api_url_override="$3"
    local api_url="${api_url_override:-${ANTHROPIC_API_URL:-https://api.anthropic.com/v1/messages}}"
    local response_file
    local http_code
    local req_body

    if ! command -v curl >/dev/null 2>&1; then
        echo "Warning: curl not found; skipping Anthropic API check."
        return 2
    fi

    req_body=$(cat <<EOF
{"model":"$model","max_tokens":16,"messages":[{"role":"user","content":"hello"}]}
EOF
)

    response_file="$(mktemp -t zclaw-anthropic-check.XXXXXX 2>/dev/null || mktemp)"
    if ! http_code="$(curl -sS -o "$response_file" -w "%{http_code}" \
        -H "content-type: application/json" \
        -H "x-api-key: $api_key" \
        -H "anthropic-version: 2023-06-01" \
        "$api_url" \
        -d "$req_body")"; then
        rm -f "$response_file"
        echo "Anthropic API check failed: network/transport error."
        return 1
    fi

    if [ "$http_code" = "200" ]; then
        rm -f "$response_file"
        echo "Anthropic API check passed (hello request succeeded)."
        return 0
    fi

    echo "Anthropic API check failed (HTTP $http_code)."
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$response_file" <<'PY'
import json
import sys
from pathlib import Path

p = Path(sys.argv[1])
try:
    data = json.loads(p.read_text(encoding="utf-8"))
except Exception:
    print("Response preview: " + p.read_text(encoding="utf-8", errors="ignore")[:200])
    raise SystemExit(0)

msg = ""
if isinstance(data, dict):
    if isinstance(data.get("error"), dict):
        msg = data["error"].get("message") or data["error"].get("type") or ""
    elif isinstance(data.get("error"), str):
        msg = data["error"]
if msg:
    print("API said: " + msg)
PY
    else
        echo "Response preview: $(head -c 200 "$response_file")"
    fi

    rm -f "$response_file"
    return 1
}

verify_openai_api_key() {
    local api_key="$1"
    local _model="$2"
    local api_url_override="$3"
    local api_url="${api_url_override:-${OPENAI_API_URL:-https://api.openai.com/v1/models}}"
    local response_file
    local http_code

    if ! command -v curl >/dev/null 2>&1; then
        echo "Warning: curl not found; skipping OpenAI API check."
        return 2
    fi

    # Runtime uses chat-completions. For key verification, always hit models endpoint.
    api_url="$(models_endpoint_from_chat_endpoint "$api_url")"

    response_file="$(mktemp -t zclaw-openai-check.XXXXXX 2>/dev/null || mktemp)"
    if ! http_code="$(curl -sS -o "$response_file" -w "%{http_code}" \
        -H "authorization: Bearer $api_key" \
        "$api_url")"; then
        rm -f "$response_file"
        echo "OpenAI API check failed: network/transport error."
        return 1
    fi

    if [ "$http_code" = "200" ]; then
        rm -f "$response_file"
        echo "OpenAI API check passed (models endpoint reachable)."
        return 0
    fi

    echo "OpenAI API check failed (HTTP $http_code)."
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$response_file" <<'PY'
import json
import sys
from pathlib import Path

p = Path(sys.argv[1])
try:
    data = json.loads(p.read_text(encoding="utf-8"))
except Exception:
    print("Response preview: " + p.read_text(encoding="utf-8", errors="ignore")[:200])
    raise SystemExit(0)

msg = ""
if isinstance(data, dict):
    if isinstance(data.get("error"), dict):
        msg = data["error"].get("message") or data["error"].get("type") or ""
    elif isinstance(data.get("error"), str):
        msg = data["error"]
if msg:
    print("API said: " + msg)
PY
    else
        echo "Response preview: $(head -c 200 "$response_file")"
    fi

    rm -f "$response_file"
    return 1
}

verify_openrouter_api_key() {
    local api_key="$1"
    local _model="$2"
    local api_url_override="$3"
    local api_url="${api_url_override:-${OPENROUTER_API_URL:-https://openrouter.ai/api/v1/models}}"
    local response_file
    local http_code
    local referer="${OPENROUTER_HTTP_REFERER:-https://github.com/tnm/zclaw}"
    local title="${OPENROUTER_APP_TITLE:-zclaw}"

    if ! command -v curl >/dev/null 2>&1; then
        echo "Warning: curl not found; skipping OpenRouter API check."
        return 2
    fi

    # Runtime uses chat-completions. For key verification, always hit models endpoint.
    api_url="$(models_endpoint_from_chat_endpoint "$api_url")"

    response_file="$(mktemp -t zclaw-openrouter-check.XXXXXX 2>/dev/null || mktemp)"
    if ! http_code="$(curl -sS -o "$response_file" -w "%{http_code}" \
        -H "authorization: Bearer $api_key" \
        -H "HTTP-Referer: $referer" \
        -H "X-Title: $title" \
        "$api_url")"; then
        rm -f "$response_file"
        echo "OpenRouter API check failed: network/transport error."
        return 1
    fi

    if [ "$http_code" = "200" ]; then
        rm -f "$response_file"
        echo "OpenRouter API check passed (models endpoint reachable)."
        return 0
    fi

    echo "OpenRouter API check failed (HTTP $http_code)."
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$response_file" <<'PY'
import json
import sys
from pathlib import Path

p = Path(sys.argv[1])
try:
    data = json.loads(p.read_text(encoding="utf-8"))
except Exception:
    print("Response preview: " + p.read_text(encoding="utf-8", errors="ignore")[:200])
    raise SystemExit(0)

msg = ""
if isinstance(data, dict):
    if isinstance(data.get("error"), dict):
        msg = data["error"].get("message") or data["error"].get("type") or ""
    elif isinstance(data.get("error"), str):
        msg = data["error"]
if msg:
    print("API said: " + msg)
PY
    else
        echo "Response preview: $(head -c 200 "$response_file")"
    fi

    rm -f "$response_file"
    return 1
}

verify_ollama_endpoint() {
    local api_key="$1"
    local _model="$2"
    local api_url="$3"
    local models_url
    local response_file
    local http_code
    local curl_args=()

    if [ -z "$api_url" ]; then
        echo "Ollama endpoint check skipped: no API URL configured."
        return 1
    fi

    if ! command -v curl >/dev/null 2>&1; then
        echo "Warning: curl not found; skipping Ollama endpoint check."
        return 2
    fi

    models_url="$(models_endpoint_from_chat_endpoint "$api_url")"
    response_file="$(mktemp -t zclaw-ollama-check.XXXXXX 2>/dev/null || mktemp)"
    curl_args=(-sS -o "$response_file" -w "%{http_code}")
    if [ -n "$api_key" ]; then
        curl_args+=(-H "authorization: Bearer $api_key")
    fi
    curl_args+=("$models_url")

    if ! http_code="$(curl "${curl_args[@]}")"; then
        rm -f "$response_file"
        echo "Ollama endpoint check failed: network/transport error."
        return 1
    fi

    if [ "$http_code" = "200" ]; then
        rm -f "$response_file"
        echo "Ollama endpoint check passed (models endpoint reachable)."
        return 0
    fi

    echo "Ollama endpoint check failed (HTTP $http_code)."
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$response_file" <<'PY'
import json
import sys
from pathlib import Path

p = Path(sys.argv[1])
try:
    data = json.loads(p.read_text(encoding="utf-8"))
except Exception:
    print("Response preview: " + p.read_text(encoding="utf-8", errors="ignore")[:200])
    raise SystemExit(0)

msg = ""
if isinstance(data, dict):
    if isinstance(data.get("error"), dict):
        msg = data["error"].get("message") or data["error"].get("type") or ""
    elif isinstance(data.get("error"), str):
        msg = data["error"]
if msg:
    print("API said: " + msg)
PY
    else
        echo "Response preview: $(head -c 200 "$response_file")"
    fi

    rm -f "$response_file"
    return 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --port)
            shift
            [ $# -gt 0 ] || { echo "Error: --port requires a value"; exit 1; }
            PORT="$1"
            ;;
        --port=*)
            PORT="${1#*=}"
            ;;
        --ssid)
            shift
            [ $# -gt 0 ] || { echo "Error: --ssid requires a value"; exit 1; }
            WIFI_SSID="$1"
            ;;
        --ssid=*)
            WIFI_SSID="${1#*=}"
            ;;
        --pass)
            shift
            [ $# -gt 0 ] || { echo "Error: --pass requires a value"; exit 1; }
            WIFI_PASS="$1"
            ;;
        --pass=*)
            WIFI_PASS="${1#*=}"
            ;;
        --backend)
            shift
            [ $# -gt 0 ] || { echo "Error: --backend requires a value"; exit 1; }
            BACKEND="$1"
            ;;
        --backend=*)
            BACKEND="${1#*=}"
            ;;
        --model)
            shift
            [ $# -gt 0 ] || { echo "Error: --model requires a value"; exit 1; }
            MODEL="$1"
            ;;
        --model=*)
            MODEL="${1#*=}"
            ;;
        --api-key)
            shift
            [ $# -gt 0 ] || { echo "Error: --api-key requires a value"; exit 1; }
            API_KEY="$1"
            ;;
        --api-key=*)
            API_KEY="${1#*=}"
            ;;
        --api-url)
            shift
            [ $# -gt 0 ] || { echo "Error: --api-url requires a value"; exit 1; }
            API_URL="$1"
            ;;
        --api-url=*)
            API_URL="${1#*=}"
            ;;
        --tg-token)
            shift
            [ $# -gt 0 ] || { echo "Error: --tg-token requires a value"; exit 1; }
            TG_TOKEN="$1"
            ;;
        --tg-token=*)
            TG_TOKEN="${1#*=}"
            ;;
        --tg-chat-id)
            shift
            [ $# -gt 0 ] || { echo "Error: --tg-chat-id requires a value"; exit 1; }
            TG_CHAT_IDS="$1"
            ;;
        --tg-chat-id=*)
            TG_CHAT_IDS="${1#*=}"
            ;;
        --tg-chat-ids)
            shift
            [ $# -gt 0 ] || { echo "Error: --tg-chat-ids requires a value"; exit 1; }
            TG_CHAT_IDS="$1"
            ;;
        --tg-chat-ids=*)
            TG_CHAT_IDS="${1#*=}"
            ;;
        --email-bridge-url)
            shift
            [ $# -gt 0 ] || { echo "Error: --email-bridge-url requires a value"; exit 1; }
            EMAIL_BRIDGE_URL="$1"
            ;;
        --email-bridge-url=*)
            EMAIL_BRIDGE_URL="${1#*=}"
            ;;
        --email-bridge-key)
            shift
            [ $# -gt 0 ] || { echo "Error: --email-bridge-key requires a value"; exit 1; }
            EMAIL_BRIDGE_KEY="$1"
            ;;
        --email-bridge-key=*)
            EMAIL_BRIDGE_KEY="${1#*=}"
            ;;
        --yes)
            ASSUME_YES=true
            ;;
        --skip-api-check)
            VERIFY_API_KEY=false
            ;;
        --print-detected-ssid)
            PRINT_DETECTED_SSID=true
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'"
            usage
            exit 1
            ;;
    esac
    shift
done

if [ "$PRINT_DETECTED_SSID" = true ]; then
    DETECTED_SSID="$(detect_host_wifi_ssid || true)"
    if [ -n "$DETECTED_SSID" ]; then
        echo "$DETECTED_SSID"
    fi
    exit 0
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

if [ -z "$WIFI_SSID" ]; then
    DETECTED_SSID="$(detect_host_wifi_ssid || true)"

    if [ "$ASSUME_YES" = true ]; then
        if [ -n "$DETECTED_SSID" ]; then
            WIFI_SSID="$DETECTED_SSID"
            echo "Detected host WiFi SSID: $WIFI_SSID"
        else
            echo "Error: --ssid is required with --yes (auto-detect failed)"
            echo "Tip: pass --ssid <network> or set ZCLAW_WIFI_SSID=<network>"
            exit 1
        fi
    else
        if [ -n "$DETECTED_SSID" ]; then
            echo "Detected host WiFi SSID: $DETECTED_SSID"
        fi

        if [ -t 0 ]; then
            if [ -n "$DETECTED_SSID" ]; then
                echo "Press Enter to use detected network, or type a different SSID."
                read -r -p "WiFi SSID [$DETECTED_SSID]: " WIFI_SSID
                WIFI_SSID="${WIFI_SSID:-$DETECTED_SSID}"
            else
                read -r -p "WiFi SSID: " WIFI_SSID
            fi
        elif [ -n "$DETECTED_SSID" ]; then
            WIFI_SSID="$DETECTED_SSID"
        fi
    fi
fi

if [ -z "$WIFI_SSID" ]; then
    echo "Error: WiFi SSID is required"
    exit 1
fi

validate_wifi_ssid_length || exit 1

if [ -z "$BACKEND" ]; then
    if [ "$ASSUME_YES" = true ]; then
        BACKEND="openai"
    else
        read -r -p "LLM provider [openai/anthropic/openrouter/ollama] (default: openai): " BACKEND
        BACKEND="${BACKEND:-openai}"
    fi
fi

if ! validate_backend "$BACKEND"; then
    echo "Error: invalid backend '$BACKEND' (expected anthropic|openai|openrouter|ollama)"
    exit 1
fi

if [ -z "$MODEL" ]; then
    MODEL="$(default_model_for_backend "$BACKEND")"
fi

if [ "$BACKEND" = "ollama" ]; then
    if [ -z "$API_URL" ]; then
        if [ "$ASSUME_YES" = true ]; then
            echo "Error: --api-url is required with --backend ollama in --yes mode"
            exit 1
        fi
        read -r -p "Ollama endpoint URL (base or /v1/chat/completions): " API_URL
    fi
    API_URL="$(normalize_ollama_api_url "$API_URL" || true)"
    if [ -z "$API_URL" ]; then
        echo "Error: invalid --api-url. Expected http(s) URL."
        exit 1
    fi
fi

if [ "$BACKEND" != "ollama" ] && [ -z "$API_KEY" ]; then
    if [ "$ASSUME_YES" = true ]; then
        echo "Error: --api-key is required with --yes"
        exit 1
    fi
    read -r -p "LLM API key (input is visible): " API_KEY
fi

if [ "$BACKEND" != "ollama" ] && [ -z "$API_KEY" ]; then
    echo "Error: API key is required"
    exit 1
fi

if [ "$VERIFY_API_KEY" = true ]; then
    VERIFY_LABEL=""
    VERIFY_FN=""
    case "$BACKEND" in
        anthropic)
            VERIFY_LABEL="Anthropic"
            VERIFY_FN="verify_anthropic_api_key"
            ;;
        openai)
            VERIFY_LABEL="OpenAI"
            VERIFY_FN="verify_openai_api_key"
            ;;
        openrouter)
            VERIFY_LABEL="OpenRouter"
            VERIFY_FN="verify_openrouter_api_key"
            ;;
        ollama)
            VERIFY_LABEL="Ollama endpoint"
            VERIFY_FN="verify_ollama_endpoint"
            ;;
    esac

    if [ -n "$VERIFY_FN" ]; then
        echo ""
        while true; do
            if [ "$BACKEND" = "ollama" ]; then
                echo "Verifying ${VERIFY_LABEL} with a quick connectivity check..."
            else
                echo "Verifying ${VERIFY_LABEL} API key with a quick connectivity check..."
            fi
            if "$VERIFY_FN" "$API_KEY" "$MODEL" "$API_URL"; then
                break
            fi

            if [ "$ASSUME_YES" = true ]; then
                echo "Error: API check failed in --yes mode."
                echo "Use --skip-api-check to bypass."
                exit 1
            fi

            echo ""
            if [ "$BACKEND" = "ollama" ]; then
                read -r -p "Re-enter Ollama endpoint URL and retry? [Y/n] " retry_key
            else
                read -r -p "Re-enter API key and retry? [Y/n] " retry_key
            fi
            retry_key="${retry_key:-Y}"
            if [[ "$retry_key" =~ ^[Yy]$ ]]; then
                if [ "$BACKEND" = "ollama" ]; then
                    read -r -p "Ollama endpoint URL (base or /v1/chat/completions): " API_URL
                    API_URL="$(normalize_ollama_api_url "$API_URL" || true)"
                    if [ -z "$API_URL" ]; then
                        echo "Valid API URL is required."
                    fi
                else
                    read -r -p "LLM API key (input is visible): " API_KEY
                    if [ -z "$API_KEY" ]; then
                        echo "API key is required."
                    fi
                fi
                continue
            fi

            read -r -p "Continue provisioning anyway? [y/N] " continue_anyway
            if [[ "$continue_anyway" =~ ^[Yy]$ ]]; then
                break
            fi

            echo "Aborted."
            exit 1
        done
    fi
fi

if [ "$ASSUME_YES" != true ] && [ -z "$WIFI_PASS" ]; then
    prompt_wifi_password
fi

while ! check_wifi_credentials; do
    if [ "$ASSUME_YES" = true ]; then
        echo "Aborted."
        exit 1
    fi

    read -r -p "Re-enter WiFi password? [Y/n] " retry_wifi_pass
    retry_wifi_pass="${retry_wifi_pass:-Y}"
    if [[ "$retry_wifi_pass" =~ ^[Yy]$ ]]; then
        prompt_wifi_password
        continue
    fi

    echo "Aborted."
    exit 1
done

if [ "$ASSUME_YES" != true ]; then
    if [ -z "$TG_TOKEN" ]; then
        read -r -p "Telegram bot token (optional): " TG_TOKEN
    fi

    if [ -z "$TG_CHAT_IDS" ]; then
        read -r -p "Telegram chat ID(s) (optional, comma-separated): " TG_CHAT_IDS
    fi
fi

if [ -n "$TG_CHAT_IDS" ]; then
    NORMALIZED_TG_CHAT_IDS="$(normalize_telegram_chat_ids "$TG_CHAT_IDS" || true)"
    if [ -z "$NORMALIZED_TG_CHAT_IDS" ]; then
        echo "Error: invalid --tg-chat-id value. Use 1-4 non-zero integers (comma-separated)."
        exit 1
    fi
    TG_CHAT_IDS="$NORMALIZED_TG_CHAT_IDS"
fi

if [ -n "$TG_TOKEN" ] && [ -z "$TG_CHAT_IDS" ]; then
    echo "Warning: Telegram token set without chat ID allowlist; incoming messages will be ignored."
fi
if [ -n "$EMAIL_BRIDGE_URL" ] && [ -z "$EMAIL_BRIDGE_KEY" ]; then
    echo "Warning: email bridge URL set without key; email tools will fail authentication."
fi
if [ -n "$EMAIL_BRIDGE_KEY" ] && [ -z "$EMAIL_BRIDGE_URL" ]; then
    echo "Warning: email bridge key set without URL; email tools will remain disabled."
fi

NVS_GEN="$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
PARTTOOL="$IDF_PATH/components/partition_table/parttool.py"

if [ ! -f "$NVS_GEN" ]; then
    echo "Error: nvs_partition_gen.py not found at $NVS_GEN"
    exit 1
fi
if [ ! -f "$PARTTOOL" ]; then
    echo "Error: parttool.py not found at $PARTTOOL"
    exit 1
fi

tmpdir="$(mktemp -d 2>/dev/null || mktemp -d -t zclaw-provision)"
csv_file="$tmpdir/nvs.csv"
nvs_bin="$tmpdir/nvs.bin"
trap 'rm -rf "$tmpdir"' EXIT

{
    echo "key,type,encoding,value"
    echo "zclaw,namespace,,"
    printf "wifi_ssid,data,string,%s\n" "$(csv_escape "$WIFI_SSID")"
    printf "wifi_pass,data,string,%s\n" "$(csv_escape "$WIFI_PASS")"
    printf "llm_backend,data,string,%s\n" "$(csv_escape "$BACKEND")"
    printf "api_key,data,string,%s\n" "$(csv_escape "$API_KEY")"
    printf "llm_model,data,string,%s\n" "$(csv_escape "$MODEL")"
    if [ -n "$API_URL" ]; then
        printf "llm_api_url,data,string,%s\n" "$(csv_escape "$API_URL")"
    fi

    if [ -n "$TG_TOKEN" ]; then
        printf "tg_token,data,string,%s\n" "$(csv_escape "$TG_TOKEN")"
    fi
    if [ -n "$TG_CHAT_IDS" ]; then
        PRIMARY_TG_CHAT_ID="$(first_telegram_chat_id "$TG_CHAT_IDS")"
        printf "tg_chat_id,data,string,%s\n" "$(csv_escape "$PRIMARY_TG_CHAT_ID")"
        printf "tg_chat_ids,data,string,%s\n" "$(csv_escape "$TG_CHAT_IDS")"
    fi
    if [ -n "$EMAIL_BRIDGE_URL" ]; then
        printf "email_br_url,data,string,%s\n" "$(csv_escape "$EMAIL_BRIDGE_URL")"
    fi
    if [ -n "$EMAIL_BRIDGE_KEY" ]; then
        printf "email_br_key,data,string,%s\n" "$(csv_escape "$EMAIL_BRIDGE_KEY")"
    fi
} > "$csv_file"

echo "Generating NVS credential image..."
python "$NVS_GEN" generate "$csv_file" "$nvs_bin" 0x4000

echo "Writing credentials to NVS on $PORT..."
EFUSE_SUMMARY="$(espefuse.py --port "$PORT" summary 2>/dev/null || true)"
PARTTOOL_ARGS=(python "$PARTTOOL" --port "$PORT")
if flash_encryption_enabled "$EFUSE_SUMMARY"; then
    PARTTOOL_ARGS+=(--esptool-write-args encrypt)
fi
PARTTOOL_ARGS+=(write_partition --partition-name nvs --input "$nvs_bin")
"${PARTTOOL_ARGS[@]}"

echo ""
echo "Provisioning complete."
echo "  WiFi SSID: $WIFI_SSID"
echo "  WiFi password: ${WIFI_PASS:-<empty>}"
echo "  Backend:   $BACKEND"
echo "  Model:     $MODEL"
if [ -n "$API_URL" ]; then
    echo "  API URL:   $API_URL"
fi
if [ -n "$EMAIL_BRIDGE_URL" ]; then
    echo "  Email bridge URL: $EMAIL_BRIDGE_URL"
fi
if [ -n "$EMAIL_BRIDGE_KEY" ]; then
    echo "  Email bridge key: <redacted>"
fi
echo ""
echo "Next steps:"
echo "  1) Board reset is automatic after provisioning"
echo "     If it does not boot, reset or power-cycle manually"
echo "  2) Run ./scripts/monitor.sh $PORT"
echo "  3) Wait for WiFi connected + Ready logs"
echo "     Look for startup marker:"
echo "       I (...) main: ========================================"
echo "       I (...) main:   Ready! Free heap: <bytes> bytes"
echo "       I (...) main: ========================================"
echo "  4) Run ./scripts/web-relay.sh and send a test message to confirm end-to-end chat"
