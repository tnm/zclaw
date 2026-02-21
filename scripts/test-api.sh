#!/bin/bash
# Run live provider API integration checks (local/manual only).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

PROVIDER="${1:-all}"
MESSAGE="${2:-Create a tool to blink GPIO 2 twice and report what you did.}"

usage() {
    echo "Usage: $0 [anthropic|openai|openrouter|all] [message]"
    echo ""
    echo "Env keys:"
    echo "  ANTHROPIC_API_KEY"
    echo "  OPENAI_API_KEY"
    echo "  OPENROUTER_API_KEY"
    echo ""
    echo "Examples:"
    echo "  $0 all"
    echo "  $0 openai \"Turn GPIO 4 on\""
}

run_provider() {
    local name="$1"
    local key_env="$2"
    local script_path="$PROJECT_DIR/test/api/test_${name}.py"

    if [ -z "${!key_env:-}" ]; then
        echo "Skipping $name (${key_env} not set)"
        return 0
    fi

    echo "Running $name API test..."
    python3 "$script_path" --quiet "$MESSAGE"
}

case "$PROVIDER" in
    anthropic)
        run_provider "anthropic" "ANTHROPIC_API_KEY"
        ;;
    openai)
        run_provider "openai" "OPENAI_API_KEY"
        ;;
    openrouter)
        run_provider "openrouter" "OPENROUTER_API_KEY"
        ;;
    all)
        run_provider "anthropic" "ANTHROPIC_API_KEY"
        run_provider "openai" "OPENAI_API_KEY"
        run_provider "openrouter" "OPENROUTER_API_KEY"
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "Error: unknown provider '$PROVIDER'"
        usage
        exit 1
        ;;
esac

echo "API test run complete."
