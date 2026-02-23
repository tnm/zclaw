#!/bin/bash
# Run zclaw tests

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

TEST_TYPE="${1:-all}"

run_host_tests() {
    echo "=== Running host tests ==="
    cd "$PROJECT_DIR/test/host"

    # Compile and run host tests
    if [ ! -d "build" ]; then
        mkdir build
    fi

    # Find cJSON include/lib paths
    CJSON_CFLAGS=""
    CJSON_LDFLAGS="-lcjson"

    # macOS with Homebrew
    if [ -d "/opt/homebrew/include/cjson" ]; then
        CJSON_CFLAGS="-I/opt/homebrew/include"
        CJSON_LDFLAGS="-L/opt/homebrew/lib -lcjson"
    elif [ -d "/usr/local/include/cjson" ]; then
        CJSON_CFLAGS="-I/usr/local/include"
        CJSON_LDFLAGS="-L/usr/local/lib -lcjson"
    fi

    # AddressSanitizer flags for memory error detection (enabled by default).
    SANITIZE_FLAGS=""
    if [ "${ASAN:-1}" = "1" ]; then
        echo "AddressSanitizer enabled (set ASAN=0 to disable)"
        SANITIZE_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
    fi

    # Keep host tests warning-clean to prevent quality regressions.
    WARNING_FLAGS="-Wall -Wextra -Werror -Wshadow -Wformat=2"

    # Compile test runner
    gcc -o build/test_runner $SANITIZE_FLAGS \
        -std=c99 \
        $WARNING_FLAGS \
        -I../../main \
        -I. \
        $CJSON_CFLAGS \
        -DTEST_BUILD \
        test_json.c \
        test_tools_parse.c \
        test_json_util_integration.c \
        test_runtime_utils.c \
        test_memory_keys.c \
        test_telegram_update.c \
        test_telegram_token.c \
        test_telegram_chat_ids.c \
        test_telegram_poll_policy.c \
        test_agent.c \
        test_tools_gpio_policy.c \
        test_llm_auth.c \
        test_wifi_credentials.c \
        test_runner.c \
        mock_esp.c \
        mock_llm.c \
        mock_user_tools.c \
        mock_freertos.c \
        mock_tools.c \
        mock_ratelimit.c \
        ../../main/json_util.c \
        ../../main/cron_utils.c \
        ../../main/security.c \
        ../../main/text_buffer.c \
        ../../main/boot_guard.c \
        ../../main/memory_keys.c \
        ../../main/llm_auth.c \
        ../../main/wifi_credentials.c \
        ../../main/telegram_update.c \
        ../../main/telegram_token.c \
        ../../main/telegram_chat_ids.c \
        ../../main/telegram_poll_policy.c \
        ../../main/agent.c \
        ../../main/tools_gpio.c \
        $CJSON_LDFLAGS 2>&1 || {
        echo "Note: Failed to compile tests. Install cJSON:"
        echo "  macOS:  brew install cjson"
        echo "  Ubuntu: apt install libcjson-dev"
        return 1
    }

    ./build/test_runner

    # Compile and run real llm.c runtime tests in stub mode to keep host
    # coverage on production LLM backend initialization and request paths.
    gcc -o build/test_llm_runtime_runner $SANITIZE_FLAGS \
        -std=c99 \
        $WARNING_FLAGS \
        -I../../main \
        -I. \
        -DTEST_BUILD \
        -DCONFIG_ZCLAW_STUB_LLM=1 \
        -DCONFIG_ZCLAW_EMULATOR_LIVE_LLM=0 \
        test_llm_runtime.c \
        test_llm_runtime_runner.c \
        mock_memory.c \
        mock_esp.c \
        ../../main/llm.c \
        ../../main/llm_auth.c || {
        echo "Note: Failed to compile llm runtime tests."
        return 1
    }

    ./build/test_llm_runtime_runner

    echo "=== Running host bridge Python tests ==="
    python3 -m unittest -q \
        test_qemu_live_llm_bridge.py \
        test_web_relay.py \
        test_install_provision_scripts.py \
        test_api_provider_harness.py
    echo ""
}

run_device_tests() {
    echo "=== Running device tests ==="

    if [ ! -f "$PROJECT_DIR/sdkconfig.test" ]; then
        echo "Skipping device test build: sdkconfig.test not found."
        echo "Add sdkconfig.test to enable dedicated device-test builds."
        return 0
    fi

    # Find and source ESP-IDF
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        source "$HOME/esp/esp-idf/export.sh"
    elif [ -f "$HOME/esp/v5.4/esp-idf/export.sh" ]; then
        source "$HOME/esp/v5.4/esp-idf/export.sh"
    elif [ -n "$IDF_PATH" ]; then
        source "$IDF_PATH/export.sh"
    else
        echo "Error: ESP-IDF not found"
        return 1
    fi

    cd "$PROJECT_DIR"

    # Build with test configuration
    echo "Building test firmware..."
    idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test" build

    echo ""
    echo "Test build complete. Flash to device to run tests."
    echo "Use: ./scripts/flash.sh && ./scripts/monitor.sh"
}

case "$TEST_TYPE" in
    host)
        run_host_tests
        ;;
    device)
        run_device_tests
        ;;
    all)
        run_host_tests
        # Device tests require hardware, just build them
        echo "=== Building device tests ==="
        run_device_tests
        ;;
    *)
        echo "Usage: $0 [host|device|all]"
        echo "  host   - Run host-based unit tests (no hardware needed)"
        echo "  device - Build device tests (requires flashing)"
        echo "  all    - Run host tests and build device tests"
        exit 1
        ;;
esac

echo "Tests complete!"
