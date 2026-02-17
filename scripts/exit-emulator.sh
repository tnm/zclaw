#!/bin/bash
# Stop a running zclaw QEMU emulator process.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
QEMU_PID_FILE="$PROJECT_DIR/build-qemu/qemu.pid"

read_pid_file() {
    local pid

    if [ ! -f "$QEMU_PID_FILE" ]; then
        return 1
    fi

    pid="$(sed -n '1p' "$QEMU_PID_FILE" | tr -d '[:space:]')"
    if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
        echo "$pid"
        return 0
    fi

    rm -f "$QEMU_PID_FILE"
    return 1
}

find_emulator_pids() {
    ps -eo pid=,args= 2>/dev/null | awk '
        /qemu-system-riscv32/ &&
        /-M esp32c3/ &&
        /build-qemu\/merged-qemu-4mb.bin/ {
            print $1
        }'
}

find_fallback_pids() {
    ps -eo pid=,args= 2>/dev/null | awk '
        /qemu-system-riscv32/ &&
        /-M esp32c3/ {
            print $1
        }'
}

pids="$(read_pid_file || true)"
if [ -z "$pids" ]; then
    pids="$(find_emulator_pids || true)"
fi
if [ -z "$pids" ]; then
    pids="$(find_fallback_pids || true)"
fi

if [ -z "$pids" ]; then
    echo "No ESP32 QEMU emulator process found."
    exit 0
fi

echo "Stopping emulator PID(s): $(echo "$pids" | tr '\n' ' ')"
for pid in $pids; do
    kill "$pid" 2>/dev/null || true
done

deadline=$((SECONDS + 3))
while [ "$SECONDS" -lt "$deadline" ]; do
    remaining=""
    for pid in $pids; do
        if kill -0 "$pid" 2>/dev/null; then
            remaining="$remaining $pid"
        fi
    done

    if [ -z "${remaining# }" ]; then
        rm -f "$QEMU_PID_FILE"
        echo "Emulator stopped."
        exit 0
    fi

    sleep 0.1
done

echo "Force stopping PID(s): ${remaining# }"
for pid in $remaining; do
    kill -9 "$pid" 2>/dev/null || true
done
rm -f "$QEMU_PID_FILE"
echo "Emulator stopped."
