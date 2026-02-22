#!/bin/bash
# Clear Telegram update backlog for a bot token (host-side helper).

set -euo pipefail

ENV_FILE="${ZCLAW_DEV_ENV_FILE:-$HOME/.config/zclaw/dev.env}"
TOKEN_OVERRIDE=""
SHOW_CONFIG=false
DRY_RUN=false

usage() {
    cat << EOF
Usage: $0 [options]

Options:
  --token <token>         Telegram bot token override
  --env-file <path>       Profile file path (default: $ENV_FILE)
  --show-config           Print resolved config with token redacted
  --dry-run               Show planned requests without executing
  -h, --help              Show help

Token resolution order:
  1) --token
  2) ZCLAW_TG_TOKEN environment variable
  3) ZCLAW_TG_TOKEN in --env-file profile

Examples:
  $0
  $0 --show-config
  $0 --token 123456789:AA... --dry-run
EOF
}

extract_bot_id() {
    local token="$1"
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

resolve_token() {
    if [ -n "$TOKEN_OVERRIDE" ]; then
        printf '%s\n' "$TOKEN_OVERRIDE"
        return
    fi

    if [ -n "${ZCLAW_TG_TOKEN:-}" ]; then
        printf '%s\n' "$ZCLAW_TG_TOKEN"
        return
    fi

    if [ -f "$ENV_FILE" ]; then
        # shellcheck disable=SC1090
        source "$ENV_FILE"
        if [ -n "${ZCLAW_TG_TOKEN:-}" ]; then
            printf '%s\n' "$ZCLAW_TG_TOKEN"
            return
        fi
    fi

    printf '%s\n' ""
}

fetch_getupdates() {
    local token="$1"
    local offset="$2"
    local response_file="$3"
    local http_code

    http_code="$(curl -sS --connect-timeout 10 --max-time 30 -o "$response_file" -w "%{http_code}" \
        "https://api.telegram.org/bot${token}/getUpdates?timeout=0&limit=1&offset=${offset}")"

    if [ "$http_code" != "200" ]; then
        echo "Error: Telegram API HTTP ${http_code} for offset=${offset}"
        if [ -s "$response_file" ]; then
            echo "Response: $(cat "$response_file")"
        fi
        return 1
    fi

    return 0
}

parse_update_id() {
    local response_file="$1"
    python3 - "$response_file" <<'PY'
import json
import sys

path = sys.argv[1]
try:
    with open(path, "r", encoding="utf-8") as fh:
        payload = json.load(fh)
except Exception as exc:
    print(f"ERR:invalid_json:{exc}")
    sys.exit(2)

if not isinstance(payload, dict):
    print("ERR:invalid_payload")
    sys.exit(2)

if payload.get("ok") is not True:
    code = payload.get("error_code")
    desc = payload.get("description", "")
    print(f"ERR:api:{code}:{desc}")
    sys.exit(3)

max_id = None
result = payload.get("result")
if isinstance(result, list):
    for item in result:
        if isinstance(item, dict):
            update_id = item.get("update_id")
            if isinstance(update_id, int):
                if max_id is None or update_id > max_id:
                    max_id = update_id

print("" if max_id is None else str(max_id))
PY
}

while [ $# -gt 0 ]; do
    case "$1" in
        --token)
            [ $# -ge 2 ] || { echo "Error: --token requires a value."; exit 1; }
            TOKEN_OVERRIDE="$2"
            shift 2
            ;;
        --env-file)
            [ $# -ge 2 ] || { echo "Error: --env-file requires a value."; exit 1; }
            ENV_FILE="$2"
            shift 2
            ;;
        --show-config)
            SHOW_CONFIG=true
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

if ! command -v curl >/dev/null 2>&1; then
    echo "Error: curl is required."
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 is required."
    exit 1
fi

TG_TOKEN="$(resolve_token)"
if [ -z "$TG_TOKEN" ]; then
    echo "Error: Telegram token not set."
    echo "Provide --token, export ZCLAW_TG_TOKEN, or set ZCLAW_TG_TOKEN in $ENV_FILE."
    exit 1
fi

BOT_ID="$(extract_bot_id "$TG_TOKEN" || true)"
if [ -z "$BOT_ID" ]; then
    echo "Error: token format invalid (missing numeric bot ID prefix)."
    exit 1
fi

if [ "$SHOW_CONFIG" = true ]; then
    echo "Telegram backlog clear:"
    echo "  Bot ID: $BOT_ID (safe identifier)"
    echo "  Token: $(mask_secret "$TG_TOKEN")"
    echo "  Env file: $ENV_FILE"
fi

if [ "$DRY_RUN" = true ]; then
    echo "Dry run requests:"
    echo "  getUpdates?timeout=0&limit=1&offset=-1"
    echo "  getUpdates?timeout=0&limit=1&offset=<latest+1>"
    exit 0
fi

tmpdir="$(mktemp -d 2>/dev/null || mktemp -d -t zclaw-tg-clear)"
resp1="$tmpdir/resp1.json"
resp2="$tmpdir/resp2.json"
trap 'rm -rf "$tmpdir"' EXIT

fetch_getupdates "$TG_TOKEN" "-1" "$resp1"
LATEST_ID="$(parse_update_id "$resp1" || true)"
case "$LATEST_ID" in
    ERR:*)
        echo "Error: failed to parse Telegram response: $LATEST_ID"
        exit 1
        ;;
esac

if [ -z "$LATEST_ID" ]; then
    echo "No pending Telegram updates found. Backlog already clear."
    exit 0
fi

NEXT_OFFSET=$((LATEST_ID + 1))
fetch_getupdates "$TG_TOKEN" "$NEXT_OFFSET" "$resp2"
PARSE2="$(parse_update_id "$resp2" || true)"
case "$PARSE2" in
    ERR:*)
        echo "Warning: follow-up confirmation parse failed: $PARSE2"
        ;;
esac

echo "Cleared Telegram backlog up to update_id=${LATEST_ID} for bot_id=${BOT_ID}."
echo "Next poll offset should be ${NEXT_OFFSET}."
