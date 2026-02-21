#!/bin/bash
# Run zclaw latency benchmark helpers.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

if command -v uv >/dev/null 2>&1; then
    exec uv run --with-requirements scripts/requirements-web-relay.txt \
        scripts/benchmark_latency.py "$@"
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 is required (or install uv)."
    exit 1
fi

exec python3 scripts/benchmark_latency.py "$@"
