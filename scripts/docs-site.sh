#!/bin/bash
# Serve the custom docs-site locally.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DOCS_DIR="$PROJECT_DIR/docs-site"

HOST="127.0.0.1"
PORT="8788"
AUTO_OPEN=false

usage() {
    cat << USAGE
Usage: $0 [options]

Options:
  --host <host>     Bind host (default: 127.0.0.1)
  --port <port>     Bind port (default: 8788)
  --open            Open docs URL in default browser
  -h, --help        Show this help

Examples:
  $0
  $0 --host 0.0.0.0 --port 8788
  $0 --open
USAGE
}

open_browser() {
    local url="$1"

    if command -v open >/dev/null 2>&1; then
        open "$url" >/dev/null 2>&1 || true
        return
    fi

    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$url" >/dev/null 2>&1 || true
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --host)
            shift
            [ $# -gt 0 ] || { echo "Error: --host requires a value" >&2; exit 1; }
            HOST="$1"
            ;;
        --host=*)
            HOST="${1#*=}"
            ;;
        --port)
            shift
            [ $# -gt 0 ] || { echo "Error: --port requires a value" >&2; exit 1; }
            PORT="$1"
            ;;
        --port=*)
            PORT="${1#*=}"
            ;;
        --open)
            AUTO_OPEN=true
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'" >&2
            usage
            exit 1
            ;;
    esac
    shift
done

if [ ! -d "$DOCS_DIR" ]; then
    echo "Error: docs-site directory not found at $DOCS_DIR" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 is required to run docs preview" >&2
    exit 1
fi

URL="http://$HOST:$PORT"

echo "Serving docs-site from: $DOCS_DIR"
echo "URL: $URL"
echo "Press Ctrl+C to stop"

if [ "$AUTO_OPEN" = true ]; then
    open_browser "$URL"
fi

cd "$DOCS_DIR"
exec python3 -m http.server "$PORT" --bind "$HOST"
