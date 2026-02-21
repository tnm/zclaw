#!/bin/bash
# Guard critical stack usage for selected functions from compile_commands.json.

set -euo pipefail

BUILD_DIR="build"
SOURCE_SUFFIX="/main/cron.c"
FUNCTION_NAME="check_entries"
MAX_BYTES=1024

usage() {
    cat <<EOF
Usage: $0 [--build-dir DIR] [--source-suffix SUFFIX] [--function NAME] [--max-bytes N]

Defaults:
  --build-dir      build
  --source-suffix  /main/cron.c
  --function       check_entries
  --max-bytes      1024
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --source-suffix)
            SOURCE_SUFFIX="$2"
            shift 2
            ;;
        --function)
            FUNCTION_NAME="$2"
            shift 2
            ;;
        --max-bytes)
            MAX_BYTES="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument '$1'" >&2
            usage
            exit 1
            ;;
    esac
done

if ! command -v jq >/dev/null 2>&1; then
    echo "Error: jq is required for stack usage check" >&2
    exit 1
fi

COMPILE_DB="$BUILD_DIR/compile_commands.json"
if [ ! -f "$COMPILE_DB" ]; then
    echo "Error: compile commands not found: $COMPILE_DB" >&2
    echo "Run a firmware build first (e.g. idf.py build)." >&2
    exit 1
fi

CMD="$(jq -r --arg sfx "$SOURCE_SUFFIX" '.[] | select(.file | endswith($sfx)) | .command' "$COMPILE_DB" | head -n 1)"
if [ -z "$CMD" ]; then
    echo "Error: no compile command found for source suffix '$SOURCE_SUFFIX' in $COMPILE_DB" >&2
    exit 1
fi

TMP_BASE="${TMPDIR:-/tmp}/zclaw-stack-${FUNCTION_NAME}-$$"
TMP_OBJ="${TMP_BASE}.o"
TMP_SU="${TMP_BASE}.su"

cleanup() {
    rm -f "$TMP_OBJ" "$TMP_SU"
}
trap cleanup EXIT

# Reuse the exact project compile flags but redirect output to a temp object.
CMD="$(printf '%s' "$CMD" | sed -E "s# -o [^ ]+# -o ${TMP_OBJ}#")"
eval "$CMD -fstack-usage"

if [ ! -f "$TMP_SU" ]; then
    echo "Error: stack usage report not generated: $TMP_SU" >&2
    exit 1
fi

USAGE_BYTES="$(awk -F'\t' -v fn="$FUNCTION_NAME" '$1 ~ (":" fn "$") {print $2; exit}' "$TMP_SU")"
if [ -z "$USAGE_BYTES" ]; then
    echo "Error: function '$FUNCTION_NAME' not found in stack report" >&2
    cat "$TMP_SU" >&2
    exit 1
fi

if [ "$USAGE_BYTES" -gt "$MAX_BYTES" ]; then
    echo "FAIL: ${FUNCTION_NAME} stack usage ${USAGE_BYTES} bytes exceeds limit ${MAX_BYTES} bytes" >&2
    exit 1
fi

echo "PASS: ${FUNCTION_NAME} stack usage ${USAGE_BYTES} bytes (limit ${MAX_BYTES})"
