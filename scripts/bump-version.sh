#!/bin/bash
# Bump zclaw release version in VERSION file.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VERSION_FILE="$PROJECT_DIR/VERSION"

usage() {
    cat <<'EOF'
Usage: ./scripts/bump-version.sh [patch|minor|major|--set X.Y.Z] [--dry-run]

Defaults:
  action: patch

Examples:
  ./scripts/bump-version.sh
  ./scripts/bump-version.sh minor
  ./scripts/bump-version.sh --set 3.0.0
  ./scripts/bump-version.sh major --dry-run
EOF
}

is_semver_triplet() {
    local v="$1"
    [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

ACTION="patch"
SET_VERSION=""
DRY_RUN=false

while [ $# -gt 0 ]; do
    case "$1" in
        patch|minor|major)
            ACTION="$1"
            ;;
        --set)
            shift
            if [ $# -eq 0 ]; then
                echo "Error: --set requires a value"
                usage
                exit 1
            fi
            SET_VERSION="$1"
            ;;
        --set=*)
            SET_VERSION="${1#*=}"
            ;;
        --dry-run)
            DRY_RUN=true
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument '$1'"
            usage
            exit 1
            ;;
    esac
    shift
done

if [ ! -f "$VERSION_FILE" ]; then
    echo "Error: VERSION file not found at $VERSION_FILE"
    exit 1
fi

CURRENT="$(sed -n '1p' "$VERSION_FILE" | tr -d '\r\n')"
if ! is_semver_triplet "$CURRENT"; then
    echo "Error: VERSION must be in X.Y.Z format, found '$CURRENT'"
    exit 1
fi

IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT"

if [ -n "$SET_VERSION" ]; then
    if ! is_semver_triplet "$SET_VERSION"; then
        echo "Error: --set value must be X.Y.Z, found '$SET_VERSION'"
        exit 1
    fi
    NEXT="$SET_VERSION"
else
    case "$ACTION" in
        major)
            MAJOR=$((MAJOR + 1))
            MINOR=0
            PATCH=0
            ;;
        minor)
            MINOR=$((MINOR + 1))
            PATCH=0
            ;;
        patch)
            PATCH=$((PATCH + 1))
            ;;
    esac
    NEXT="${MAJOR}.${MINOR}.${PATCH}"
fi

if [ "$DRY_RUN" = true ]; then
    echo "$CURRENT -> $NEXT (dry run)"
    exit 0
fi

printf '%s\n' "$NEXT" > "$VERSION_FILE"
echo "Version bumped: $CURRENT -> $NEXT"
