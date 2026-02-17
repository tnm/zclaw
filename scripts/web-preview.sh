#!/bin/bash
# Run local preview server for the zclaw setup web UI.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

reload_flag_seen=0
for arg in "$@"; do
    if [[ "$arg" == "--reload" || "$arg" == "--no-reload" ]]; then
        reload_flag_seen=1
        break
    fi
done

if [[ "$reload_flag_seen" -eq 0 ]]; then
    set -- --reload "$@"
fi

exec python3 "$SCRIPT_DIR/web_preview_server.py" "$@"
