#!/usr/bin/env bash
# Bootstrap zclaw without manual clone.
# Usage:
#   bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh)
#   bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh) -- --build --flash

set -euo pipefail

REPO_URL="${ZCLAW_BOOTSTRAP_REPO:-https://github.com/tnm/zclaw.git}"
BRANCH="${ZCLAW_BOOTSTRAP_BRANCH:-main}"
TARGET_DIR="${ZCLAW_BOOTSTRAP_DIR:-$HOME/.local/share/zclaw/repo}"
EXPECTED_SHA256="${ZCLAW_BOOTSTRAP_SHA256:-}"
RUN_INSTALL=true
INSTALL_ARGS=()

usage() {
    cat <<EOF
Usage: bootstrap.sh [bootstrap-options] [-- install-options]

Bootstrap options:
  --repo <url>         Git repository URL (default: $REPO_URL)
  --branch <name>      Git branch/tag to checkout (default: $BRANCH)
  --dir <path>         Target clone directory (default: $TARGET_DIR)
  --sha256 <hex>       Verify bootstrap script SHA-256 before running
  --no-run             Clone/update only, do not run install.sh
  -h, --help           Show this help

Examples:
  bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh)
  bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh) -- --build --flash
  ZCLAW_BOOTSTRAP_SHA256=<sha256> bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh)
  bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh) --dir ~/src/zclaw -- --no-qemu
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

while [ "$#" -gt 0 ]; do
    case "$1" in
        --repo)
            [ "$#" -ge 2 ] || { echo "Error: --repo requires a value"; exit 1; }
            REPO_URL="$2"
            shift 2
            ;;
        --repo=*)
            REPO_URL="${1#*=}"
            shift
            ;;
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
            RUN_INSTALL=false
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            while [ "$#" -gt 0 ]; do
                INSTALL_ARGS+=("$1")
                shift
            done
            break
            ;;
        *)
            # Unknown args are forwarded to install.sh for convenience.
            INSTALL_ARGS+=("$1")
            shift
            ;;
    esac
done

require_command git
verify_self_sha256

mkdir -p "$(dirname "$TARGET_DIR")"

if [ -d "$TARGET_DIR" ] && [ ! -d "$TARGET_DIR/.git" ]; then
    echo "Error: target exists but is not a git repository: $TARGET_DIR"
    echo "Choose another directory with --dir or remove this path."
    exit 1
fi

if [ -d "$TARGET_DIR/.git" ]; then
    echo "Using existing repository: $TARGET_DIR"
    if [ -n "$(git -C "$TARGET_DIR" status --porcelain --untracked-files=no)" ]; then
        echo "Warning: local modifications detected; skipping auto-update."
        echo "Run from a clean checkout to auto-update branch '$BRANCH'."
    else
        git -C "$TARGET_DIR" fetch --depth 1 origin "$BRANCH"
        git -C "$TARGET_DIR" checkout -B "$BRANCH" "origin/$BRANCH"
    fi
else
    echo "Cloning $REPO_URL into $TARGET_DIR"
    git clone --depth 1 --branch "$BRANCH" "$REPO_URL" "$TARGET_DIR"
fi

if [ ! -f "$TARGET_DIR/install.sh" ]; then
    echo "Error: install.sh not found in $TARGET_DIR"
    exit 1
fi

cd "$TARGET_DIR"

if [ "$RUN_INSTALL" = false ]; then
    echo "Bootstrap complete. Repository ready at: $TARGET_DIR"
    if [ "${#INSTALL_ARGS[@]}" -gt 0 ]; then
        echo "Next step: ./install.sh ${INSTALL_ARGS[*]}"
    else
        echo "Next step: ./install.sh"
    fi
    exit 0
fi

if [ "${#INSTALL_ARGS[@]}" -gt 0 ]; then
    echo "Running: ./install.sh ${INSTALL_ARGS[*]}"
    exec ./install.sh "${INSTALL_ARGS[@]}"
fi

echo "Running: ./install.sh"
exec ./install.sh
