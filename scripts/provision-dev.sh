#!/bin/bash
# Local dev wrapper for repeatable non-interactive provisioning.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PROVISION_SH="${ZCLAW_PROVISION_SCRIPT:-$PROJECT_DIR/scripts/provision.sh}"
ENV_FILE="${ZCLAW_DEV_ENV_FILE:-$HOME/.config/zclaw/dev.env}"

PORT_OVERRIDE=""
SSID_OVERRIDE=""
PASS_OVERRIDE=""
PASS_OVERRIDE_SET=false
BACKEND_OVERRIDE=""
MODEL_OVERRIDE=""
API_KEY_OVERRIDE=""
TG_TOKEN_OVERRIDE=""
TG_CHAT_ID_OVERRIDE=""
SHOW_CONFIG=false
WRITE_TEMPLATE=false
SKIP_API_CHECK=false
DRY_RUN=false

usage() {
    cat << EOF
Usage: $0 [options]

Options:
  --env-file <path>      Profile file path (default: $HOME/.config/zclaw/dev.env)
  --write-template       Create/update a template profile and exit
  --show-config          Print resolved config with secrets redacted
  --dry-run              Print resolved provision command and exit
  --skip-api-check       Forward --skip-api-check to provision.sh

Overrides:
  --port <serial-port>
  --ssid <wifi-ssid>
  --pass <wifi-pass>
  --backend <provider>   anthropic | openai | openrouter
  --model <model-id>
  --api-key <key>
  --tg-token <token>
  --tg-chat-id <id>

Examples:
  $0 --write-template
  $0
  $0 --show-config --dry-run
  $0 --port /dev/cu.usbmodem1101 --skip-api-check
EOF
}

write_template() {
    local dir
    dir="$(dirname "$ENV_FILE")"
    mkdir -p "$dir"
    cat > "$ENV_FILE" << 'EOF'
# zclaw local dev provisioning profile
# Keep this file local and private.

ZCLAW_PORT=/dev/cu.usbmodem1101
ZCLAW_WIFI_SSID=YourWifi
ZCLAW_WIFI_PASS=YourWifiPassword
ZCLAW_BACKEND=openai
ZCLAW_MODEL=gpt-5.2

# Prefer setting one API key here:
ZCLAW_API_KEY=
# Or rely on provider env vars instead:
# OPENAI_API_KEY=
# ANTHROPIC_API_KEY=
# OPENROUTER_API_KEY=

# Optional Telegram credentials:
ZCLAW_TG_TOKEN=
ZCLAW_TG_CHAT_ID=
EOF
    chmod 600 "$ENV_FILE"
    echo "Wrote template: $ENV_FILE"
    echo "Next: edit values, then run: $0"
}

mask_secret() {
    local value="${1:-}"
    local len
    if [ -z "$value" ]; then
        echo "<empty>"
        return
    fi
    len=${#value}
    if [ "$len" -le 8 ]; then
        echo "<redacted>"
        return
    fi
    echo "${value:0:4}...${value: -4}"
}

mask_chat_id() {
    local value="${1:-}"
    local len
    if [ -z "$value" ]; then
        echo "<empty>"
        return
    fi
    len=${#value}
    if [ "$len" -le 4 ]; then
        echo "<redacted>"
        return
    fi
    echo "****${value: -4}"
}

extract_bot_id() {
    local token="${1:-}"
    local bot_id="${token%%:*}"
    if [ -z "$token" ] || [ "$bot_id" = "$token" ]; then
        return 1
    fi
    case "$bot_id" in
        *[!0-9]*|"")
            return 1
            ;;
    esac
    printf '%s\n' "$bot_id"
}

resolve_api_key() {
    local backend="$1"
    if [ -n "$API_KEY_OVERRIDE" ]; then
        printf '%s\n' "$API_KEY_OVERRIDE"
        return
    fi
    if [ -n "${ZCLAW_API_KEY:-}" ]; then
        printf '%s\n' "$ZCLAW_API_KEY"
        return
    fi

    case "$backend" in
        openai)
            printf '%s\n' "${OPENAI_API_KEY:-}"
            ;;
        anthropic)
            printf '%s\n' "${ANTHROPIC_API_KEY:-}"
            ;;
        openrouter)
            printf '%s\n' "${OPENROUTER_API_KEY:-}"
            ;;
        *)
            printf '%s\n' ""
            ;;
    esac
}

print_redacted_command() {
    local -a args=("$@")
    local i
    local arg
    local next_secret=false

    printf "Command:"
    for ((i = 0; i < ${#args[@]}; i++)); do
        arg="${args[$i]}"
        if [ "$next_secret" = true ]; then
            printf " %q" "<redacted>"
            next_secret=false
            continue
        fi
        case "$arg" in
            --api-key|--tg-token|--pass)
                printf " %q" "$arg"
                next_secret=true
                ;;
            *)
                printf " %q" "$arg"
                ;;
        esac
    done
    printf "\n"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --env-file)
            [ $# -ge 2 ] || { echo "Error: --env-file requires a value."; exit 1; }
            ENV_FILE="$2"
            shift 2
            ;;
        --port)
            [ $# -ge 2 ] || { echo "Error: --port requires a value."; exit 1; }
            PORT_OVERRIDE="$2"
            shift 2
            ;;
        --ssid)
            [ $# -ge 2 ] || { echo "Error: --ssid requires a value."; exit 1; }
            SSID_OVERRIDE="$2"
            shift 2
            ;;
        --pass)
            [ $# -ge 2 ] || { echo "Error: --pass requires a value."; exit 1; }
            PASS_OVERRIDE="$2"
            PASS_OVERRIDE_SET=true
            shift 2
            ;;
        --backend)
            [ $# -ge 2 ] || { echo "Error: --backend requires a value."; exit 1; }
            BACKEND_OVERRIDE="$2"
            shift 2
            ;;
        --model)
            [ $# -ge 2 ] || { echo "Error: --model requires a value."; exit 1; }
            MODEL_OVERRIDE="$2"
            shift 2
            ;;
        --api-key)
            [ $# -ge 2 ] || { echo "Error: --api-key requires a value."; exit 1; }
            API_KEY_OVERRIDE="$2"
            shift 2
            ;;
        --tg-token)
            [ $# -ge 2 ] || { echo "Error: --tg-token requires a value."; exit 1; }
            TG_TOKEN_OVERRIDE="$2"
            shift 2
            ;;
        --tg-chat-id)
            [ $# -ge 2 ] || { echo "Error: --tg-chat-id requires a value."; exit 1; }
            TG_CHAT_ID_OVERRIDE="$2"
            shift 2
            ;;
        --show-config)
            SHOW_CONFIG=true
            shift
            ;;
        --write-template)
            WRITE_TEMPLATE=true
            shift
            ;;
        --skip-api-check)
            SKIP_API_CHECK=true
            shift
            ;;
        --dry-run)
            DRY_RUN=true
            shift
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
done

if [ "$WRITE_TEMPLATE" = true ]; then
    write_template
    exit 0
fi

if [ ! -f "$ENV_FILE" ]; then
    echo "Error: profile not found: $ENV_FILE"
    echo "Create one with:"
    echo "  $0 --env-file $ENV_FILE --write-template"
    exit 1
fi

# shellcheck disable=SC1090
source "$ENV_FILE"

if [ ! -x "$PROVISION_SH" ]; then
    echo "Error: provision script not executable: $PROVISION_SH"
    exit 1
fi

PORT="${PORT_OVERRIDE:-${ZCLAW_PORT:-}}"
SSID="${SSID_OVERRIDE:-${ZCLAW_WIFI_SSID:-}}"
BACKEND="${BACKEND_OVERRIDE:-${ZCLAW_BACKEND:-openai}}"
MODEL="${MODEL_OVERRIDE:-${ZCLAW_MODEL:-}}"
TG_TOKEN="${TG_TOKEN_OVERRIDE:-${ZCLAW_TG_TOKEN:-}}"
TG_CHAT_ID="${TG_CHAT_ID_OVERRIDE:-${ZCLAW_TG_CHAT_ID:-}}"

if [ "$PASS_OVERRIDE_SET" = true ]; then
    WIFI_PASS="$PASS_OVERRIDE"
    WIFI_PASS_SET=true
elif [ "${ZCLAW_WIFI_PASS+x}" = "x" ]; then
    WIFI_PASS="${ZCLAW_WIFI_PASS}"
    WIFI_PASS_SET=true
else
    WIFI_PASS=""
    WIFI_PASS_SET=false
fi

API_KEY="$(resolve_api_key "$BACKEND")"
if [ -z "$API_KEY" ]; then
    echo "Error: API key not set."
    echo "Set ZCLAW_API_KEY in $ENV_FILE, pass --api-key, or export backend-specific key env vars."
    exit 1
fi

if [ "$SHOW_CONFIG" = true ]; then
    BOT_ID=""
    if BOT_ID="$(extract_bot_id "$TG_TOKEN" 2>/dev/null)"; then
        :
    else
        BOT_ID="<empty>"
    fi
    echo "Using profile: $ENV_FILE"
    echo "  Port: ${PORT:-<auto>}"
    echo "  WiFi SSID: ${SSID:-<auto-detect>}"
    echo "  WiFi pass: $(mask_secret "$WIFI_PASS")"
    echo "  Backend: $BACKEND"
    echo "  Model: ${MODEL:-<provider default>}"
    echo "  API key: $(mask_secret "$API_KEY")"
    echo "  Telegram bot ID: $BOT_ID (safe identifier)"
    echo "  Telegram token: $(mask_secret "$TG_TOKEN")"
    echo "  Telegram chat ID: $(mask_chat_id "$TG_CHAT_ID")"
fi

PROVISION_ARGS=(--yes)
if [ -n "$PORT" ]; then
    PROVISION_ARGS+=(--port "$PORT")
fi
if [ -n "$SSID" ]; then
    PROVISION_ARGS+=(--ssid "$SSID")
fi
if [ "$WIFI_PASS_SET" = true ]; then
    PROVISION_ARGS+=(--pass "$WIFI_PASS")
fi
PROVISION_ARGS+=(--backend "$BACKEND")
if [ -n "$MODEL" ]; then
    PROVISION_ARGS+=(--model "$MODEL")
fi
PROVISION_ARGS+=(--api-key "$API_KEY")
if [ -n "$TG_TOKEN" ]; then
    PROVISION_ARGS+=(--tg-token "$TG_TOKEN")
fi
if [ -n "$TG_CHAT_ID" ]; then
    PROVISION_ARGS+=(--tg-chat-id "$TG_CHAT_ID")
fi
if [ "$SKIP_API_CHECK" = true ]; then
    PROVISION_ARGS+=(--skip-api-check)
fi

print_redacted_command "$PROVISION_SH" "${PROVISION_ARGS[@]}"
if [ "$DRY_RUN" = true ]; then
    echo "Dry run only; provisioning not executed."
    exit 0
fi

"$PROVISION_SH" "${PROVISION_ARGS[@]}"
