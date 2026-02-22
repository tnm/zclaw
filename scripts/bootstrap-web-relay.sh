#!/usr/bin/env bash
# Bootstrap zclaw web relay without cloning the full repository.
#
# Examples:
#   bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap-web-relay.sh) -- --serial-port /dev/cu.usbmodem1101
#   bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap-web-relay.sh) -- --mock-agent --port 8787

set -euo pipefail

RAW_BASE="${ZCLAW_WEB_RELAY_RAW_BASE:-https://raw.githubusercontent.com/tnm/zclaw}"
BRANCH="${ZCLAW_WEB_RELAY_BRANCH:-main}"
TARGET_DIR="${ZCLAW_WEB_RELAY_DIR:-$HOME/.local/share/zclaw/web-relay}"
EXPECTED_SHA256="${ZCLAW_WEB_RELAY_BOOTSTRAP_SHA256:-}"
RUN_RELAY=true
RELAY_ARGS=()

usage() {
    cat <<EOF
Usage: bootstrap-web-relay.sh [bootstrap-options] [-- relay-options]

Bootstrap options:
  --branch <name>      Branch/tag to fetch from (default: $BRANCH)
  --dir <path>         Target directory for relay files (default: $TARGET_DIR)
  --sha256 <hex>       Verify bootstrap script SHA-256 before running
  --no-run             Download/update files only, do not launch relay
  -h, --help           Show this help

Examples:
  bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap-web-relay.sh) -- --serial-port /dev/cu.usbmodem1101
  bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap-web-relay.sh) -- --mock-agent --port 8787
  ZCLAW_WEB_RELAY_BOOTSTRAP_SHA256=<sha256> bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap-web-relay.sh) -- --mock-agent
EOF
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: required command not found: $1"
        exit 1
    fi
}

is_sha256_hex() {
    [[ "$1" =~ ^[A-Fa-f0-9]{64}$ ]]
}

sha256_file() {
    local file="$1"

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$file" | awk '{print $1}'
        return 0
    fi
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$file" | awk '{print $1}'
        return 0
    fi
    if command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$file" | awk '{print $NF}'
        return 0
    fi

    echo "Error: SHA-256 tool not found (need sha256sum, shasum, or openssl)."
    exit 1
}

verify_self_sha256() {
    local script_path actual expected

    [ -n "$EXPECTED_SHA256" ] || return 0

    if ! is_sha256_hex "$EXPECTED_SHA256"; then
        echo "Error: invalid SHA-256 value: $EXPECTED_SHA256"
        exit 1
    fi

    script_path="${BASH_SOURCE[0]:-$0}"
    if [ ! -r "$script_path" ]; then
        echo "Error: cannot read bootstrap script for checksum verification: $script_path"
        exit 1
    fi

    actual="$(sha256_file "$script_path")"
    expected="$(printf '%s' "$EXPECTED_SHA256" | tr '[:upper:]' '[:lower:]')"

    if [ "$actual" != "$expected" ]; then
        echo "Error: bootstrap checksum mismatch."
        echo "Expected: $expected"
        echo "Actual:   $actual"
        exit 1
    fi

    echo "Bootstrap checksum verified."
}

download_file() {
    local rel_path="$1"
    local url="$RAW_BASE/$BRANCH/$rel_path"
    local dst="$TARGET_DIR/$rel_path"
    local tmp

    mkdir -p "$(dirname "$dst")"
    tmp="$(mktemp "${dst##*/}.XXXXXX")"
    if ! curl -fsSL "$url" -o "$tmp"; then
        rm -f "$tmp"
        echo "Error: failed to download $url"
        exit 1
    fi
    mv "$tmp" "$dst"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --branch)
            [ "$#" -ge 2 ] || { echo "Error: --branch requires a value"; exit 1; }
            BRANCH="$2"
            shift 2
            ;;
        --branch=*)
            BRANCH="${1#*=}"
            shift
            ;;
        --dir)
            [ "$#" -ge 2 ] || { echo "Error: --dir requires a value"; exit 1; }
            TARGET_DIR="$2"
            shift 2
            ;;
        --dir=*)
            TARGET_DIR="${1#*=}"
            shift
            ;;
        --sha256)
            [ "$#" -ge 2 ] || { echo "Error: --sha256 requires a value"; exit 1; }
            EXPECTED_SHA256="$2"
            shift 2
            ;;
        --sha256=*)
            EXPECTED_SHA256="${1#*=}"
            shift
            ;;
        --no-run)
            RUN_RELAY=false
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            while [ "$#" -gt 0 ]; do
                RELAY_ARGS+=("$1")
                shift
            done
            break
            ;;
        *)
            RELAY_ARGS+=("$1")
            shift
            ;;
    esac
done

require_command curl
verify_self_sha256

download_file scripts/web-relay.sh
download_file scripts/web_relay.py
download_file scripts/requirements-web-relay.txt
download_file scripts/release-port.sh

chmod +x "$TARGET_DIR/scripts/web-relay.sh"
chmod +x "$TARGET_DIR/scripts/release-port.sh"
chmod +x "$TARGET_DIR/scripts/web_relay.py"

echo "Relay files are ready at: $TARGET_DIR"

if [ "$RUN_RELAY" = false ]; then
    if [ "${#RELAY_ARGS[@]}" -gt 0 ]; then
        echo "Next step: cd \"$TARGET_DIR\" && ./scripts/web-relay.sh ${RELAY_ARGS[*]}"
    else
        echo "Next step: cd \"$TARGET_DIR\" && ./scripts/web-relay.sh"
    fi
    exit 0
fi

cd "$TARGET_DIR"
if [ "${#RELAY_ARGS[@]}" -gt 0 ]; then
    echo "Running: ./scripts/web-relay.sh ${RELAY_ARGS[*]}"
    exec ./scripts/web-relay.sh "${RELAY_ARGS[@]}"
fi

echo "Running: ./scripts/web-relay.sh"
exec ./scripts/web-relay.sh
