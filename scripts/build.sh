#!/bin/bash
# Build zclaw firmware

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

PAD_TARGET_BYTES=""
BOARD_PRESET=""
BOARD_SDKCONFIG_FILE=""
IDF_TARGET_OVERRIDE=""
SDKCONFIG_DEFAULTS_OVERRIDE="sdkconfig.defaults"
SDKCONFIG_FILE_OVERRIDE="sdkconfig"
VOICE_I2S_PORT_OVERRIDE=""
VOICE_I2S_BCLK_OVERRIDE=""
VOICE_I2S_WS_OVERRIDE=""
VOICE_I2S_DIN_OVERRIDE=""
VOICE_SDKCONFIG_FILE_REL="build/sdkconfig.voice-overrides.defaults"

usage() {
    echo "Usage: $0 [--pad-to-888kb] [--board <preset>] [--box-3] [--s3-voice] [--s3-sense-voice] [voice options]"
    echo "  --pad-to-888kb  Create build/zclaw-888kb.bin padded to exactly 888 KiB (909312 bytes)"
    echo "  --board         Apply a board preset (esp32s3-box-3 | esp32s3-voice | esp32s3-sense-voice)"
    echo "  --box-3         Alias for --board esp32s3-box-3"
    echo "  --s3-voice      Alias for --board esp32s3-voice"
    echo "  --s3-sense-voice Alias for --board esp32s3-sense-voice"
    echo "  --voice-i2s-port <0|1>      Override CONFIG_ZCLAW_VOICE_I2S_PORT"
    echo "  --voice-i2s-bclk <-1..48>   Override CONFIG_ZCLAW_VOICE_I2S_BCLK_GPIO"
    echo "  --voice-i2s-ws <-1..48>     Override CONFIG_ZCLAW_VOICE_I2S_WS_GPIO"
    echo "  --voice-i2s-din <-1..48>    Override CONFIG_ZCLAW_VOICE_I2S_DIN_GPIO"
}

normalize_board_preset() {
    case "$1" in
        esp32s3-box-3|esp32-s3-box-3|box-3|esp-box-3)
            echo "esp32s3-box-3"
            ;;
        esp32s3-sense-voice|esp32-s3-sense-voice|s3-sense-voice|sense-voice|voice-s3)
            echo "esp32s3-sense-voice"
            ;;
        esp32s3-voice|esp32-s3-voice|s3-voice|voice-s3-generic)
            echo "esp32s3-voice"
            ;;
        *)
            echo ""
            ;;
    esac
}

resolve_board_preset() {
    local normalized

    [ -n "$BOARD_PRESET" ] || return 0

    normalized="$(normalize_board_preset "$BOARD_PRESET")"
    if [ -z "$normalized" ]; then
        echo "Error: Unknown board preset '$BOARD_PRESET'"
        echo "Supported presets: esp32s3-box-3, esp32s3-voice, esp32s3-sense-voice"
        return 1
    fi

    BOARD_PRESET="$normalized"
    case "$BOARD_PRESET" in
        esp32s3-box-3)
            BOARD_SDKCONFIG_FILE="sdkconfig.esp32s3-box-3.defaults"
            IDF_TARGET_OVERRIDE="esp32s3"
            ;;
        esp32s3-sense-voice)
            BOARD_SDKCONFIG_FILE="sdkconfig.esp32s3-sense-voice.defaults"
            IDF_TARGET_OVERRIDE="esp32s3"
            ;;
        esp32s3-voice)
            BOARD_SDKCONFIG_FILE="sdkconfig.esp32s3-voice.defaults"
            IDF_TARGET_OVERRIDE="esp32s3"
            ;;
        *)
            echo "Error: Unsupported board preset '$BOARD_PRESET'"
            return 1
            ;;
    esac

    if [ ! -f "$PROJECT_DIR/$BOARD_SDKCONFIG_FILE" ]; then
        echo "Error: Board preset file missing: $BOARD_SDKCONFIG_FILE"
        return 1
    fi

    SDKCONFIG_DEFAULTS_OVERRIDE="sdkconfig.defaults;$BOARD_SDKCONFIG_FILE"
    SDKCONFIG_FILE_OVERRIDE="build/sdkconfig.$BOARD_PRESET"
}

is_integer() {
    [[ "$1" =~ ^-?[0-9]+$ ]]
}

validate_int_range() {
    local flag="$1"
    local value="$2"
    local min="$3"
    local max="$4"

    if ! is_integer "$value"; then
        echo "Error: $flag expects an integer value (got '$value')"
        return 1
    fi
    if [ "$value" -lt "$min" ] || [ "$value" -gt "$max" ]; then
        echo "Error: $flag out of range ($min..$max): $value"
        return 1
    fi
}

configure_voice_i2s_overrides() {
    local has_overrides=0
    local voice_file_abs="$PROJECT_DIR/$VOICE_SDKCONFIG_FILE_REL"

    [ -n "$VOICE_I2S_PORT_OVERRIDE" ] && has_overrides=1
    [ -n "$VOICE_I2S_BCLK_OVERRIDE" ] && has_overrides=1
    [ -n "$VOICE_I2S_WS_OVERRIDE" ] && has_overrides=1
    [ -n "$VOICE_I2S_DIN_OVERRIDE" ] && has_overrides=1
    [ "$has_overrides" -eq 1 ] || return 0

    if [ -n "$VOICE_I2S_PORT_OVERRIDE" ] && ! validate_int_range "--voice-i2s-port" "$VOICE_I2S_PORT_OVERRIDE" 0 1; then
        return 1
    fi
    if [ -n "$VOICE_I2S_BCLK_OVERRIDE" ] && ! validate_int_range "--voice-i2s-bclk" "$VOICE_I2S_BCLK_OVERRIDE" -1 48; then
        return 1
    fi
    if [ -n "$VOICE_I2S_WS_OVERRIDE" ] && ! validate_int_range "--voice-i2s-ws" "$VOICE_I2S_WS_OVERRIDE" -1 48; then
        return 1
    fi
    if [ -n "$VOICE_I2S_DIN_OVERRIDE" ] && ! validate_int_range "--voice-i2s-din" "$VOICE_I2S_DIN_OVERRIDE" -1 48; then
        return 1
    fi

    if [ "$BOARD_PRESET" = "esp32s3-box-3" ]; then
        echo "Warning: voice I2S overrides provided with non-voice board preset '$BOARD_PRESET'."
        echo "         These only take effect when CONFIG_ZCLAW_VOICE is enabled."
    fi

    mkdir -p "$PROJECT_DIR/$(dirname "$VOICE_SDKCONFIG_FILE_REL")"
    : > "$voice_file_abs"

    [ -n "$VOICE_I2S_PORT_OVERRIDE" ] && echo "CONFIG_ZCLAW_VOICE_I2S_PORT=$VOICE_I2S_PORT_OVERRIDE" >> "$voice_file_abs"
    [ -n "$VOICE_I2S_BCLK_OVERRIDE" ] && echo "CONFIG_ZCLAW_VOICE_I2S_BCLK_GPIO=$VOICE_I2S_BCLK_OVERRIDE" >> "$voice_file_abs"
    [ -n "$VOICE_I2S_WS_OVERRIDE" ] && echo "CONFIG_ZCLAW_VOICE_I2S_WS_GPIO=$VOICE_I2S_WS_OVERRIDE" >> "$voice_file_abs"
    [ -n "$VOICE_I2S_DIN_OVERRIDE" ] && echo "CONFIG_ZCLAW_VOICE_I2S_DIN_GPIO=$VOICE_I2S_DIN_OVERRIDE" >> "$voice_file_abs"

    SDKCONFIG_DEFAULTS_OVERRIDE="$SDKCONFIG_DEFAULTS_OVERRIDE;$VOICE_SDKCONFIG_FILE_REL"
    apply_voice_i2s_overrides_to_sdkconfig
}

upsert_sdkconfig_value() {
    local cfg_path="$1"
    local key="$2"
    local value="$3"
    local tmp_file

    mkdir -p "$(dirname "$cfg_path")"
    [ -f "$cfg_path" ] || touch "$cfg_path"

    if grep -q "^${key}=" "$cfg_path"; then
        tmp_file="$(mktemp -t zclaw-sdkcfg.XXXXXX 2>/dev/null || mktemp)"
        awk -v key="$key" -v value="$value" '
            BEGIN { prefix = key "=" }
            index($0, prefix) == 1 { print key "=" value; next }
            { print }
        ' "$cfg_path" > "$tmp_file"
        mv "$tmp_file" "$cfg_path"
    else
        printf '%s=%s\n' "$key" "$value" >> "$cfg_path"
    fi
}

apply_voice_i2s_overrides_to_sdkconfig() {
    local cfg_path="$PROJECT_DIR/$SDKCONFIG_FILE_OVERRIDE"

    [ -n "$VOICE_I2S_PORT_OVERRIDE" ] && upsert_sdkconfig_value "$cfg_path" "CONFIG_ZCLAW_VOICE_I2S_PORT" "$VOICE_I2S_PORT_OVERRIDE"
    [ -n "$VOICE_I2S_BCLK_OVERRIDE" ] && upsert_sdkconfig_value "$cfg_path" "CONFIG_ZCLAW_VOICE_I2S_BCLK_GPIO" "$VOICE_I2S_BCLK_OVERRIDE"
    [ -n "$VOICE_I2S_WS_OVERRIDE" ] && upsert_sdkconfig_value "$cfg_path" "CONFIG_ZCLAW_VOICE_I2S_WS_GPIO" "$VOICE_I2S_WS_OVERRIDE"
    [ -n "$VOICE_I2S_DIN_OVERRIDE" ] && upsert_sdkconfig_value "$cfg_path" "CONFIG_ZCLAW_VOICE_I2S_DIN_GPIO" "$VOICE_I2S_DIN_OVERRIDE"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --pad-to-888kb)
            PAD_TARGET_BYTES=$((888 * 1024))
            ;;
        --board)
            shift
            [ $# -gt 0 ] || { echo "Error: --board requires a value"; usage; exit 1; }
            BOARD_PRESET="$1"
            ;;
        --board=*)
            BOARD_PRESET="${1#*=}"
            ;;
        --box-3)
            BOARD_PRESET="esp32s3-box-3"
            ;;
        --s3-sense-voice)
            BOARD_PRESET="esp32s3-sense-voice"
            ;;
        --s3-voice)
            BOARD_PRESET="esp32s3-voice"
            ;;
        --voice-i2s-port)
            shift
            [ $# -gt 0 ] || { echo "Error: --voice-i2s-port requires a value"; usage; exit 1; }
            VOICE_I2S_PORT_OVERRIDE="$1"
            ;;
        --voice-i2s-port=*)
            VOICE_I2S_PORT_OVERRIDE="${1#*=}"
            ;;
        --voice-i2s-bclk)
            shift
            [ $# -gt 0 ] || { echo "Error: --voice-i2s-bclk requires a value"; usage; exit 1; }
            VOICE_I2S_BCLK_OVERRIDE="$1"
            ;;
        --voice-i2s-bclk=*)
            VOICE_I2S_BCLK_OVERRIDE="${1#*=}"
            ;;
        --voice-i2s-ws)
            shift
            [ $# -gt 0 ] || { echo "Error: --voice-i2s-ws requires a value"; usage; exit 1; }
            VOICE_I2S_WS_OVERRIDE="$1"
            ;;
        --voice-i2s-ws=*)
            VOICE_I2S_WS_OVERRIDE="${1#*=}"
            ;;
        --voice-i2s-din)
            shift
            [ $# -gt 0 ] || { echo "Error: --voice-i2s-din requires a value"; usage; exit 1; }
            VOICE_I2S_DIN_OVERRIDE="$1"
            ;;
        --voice-i2s-din=*)
            VOICE_I2S_DIN_OVERRIDE="${1#*=}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: Unknown argument '$1'"
            usage
            exit 1
            ;;
    esac
    shift
done

resolve_board_preset || exit 1
configure_voice_i2s_overrides || exit 1

file_size_bytes() {
    local path="$1"
    if stat -f %z "$path" >/dev/null 2>&1; then
        stat -f %z "$path"
    else
        stat -c %s "$path"
    fi
}

source_idf_env() {
    local candidates=(
        "$HOME/esp/esp-idf/export.sh"
        "$HOME/esp/v5.4/esp-idf/export.sh"
    )
    if [ -n "${IDF_PATH:-}" ]; then
        candidates+=("$IDF_PATH/export.sh")
    fi

    local script
    local found=0
    for script in "${candidates[@]}"; do
        [ -f "$script" ] || continue
        found=1
        if source "$script" > /dev/null 2>&1; then
            return 0
        fi
    done

    if [ "$found" -eq 1 ]; then
        echo "Error: ESP-IDF found but failed to activate."
        echo "Run:"
        echo "  cd ~/esp/esp-idf && ./install.sh esp32c3,esp32s3"
    else
        echo "Error: ESP-IDF not found. Install it first:"
        echo "  mkdir -p ~/esp && cd ~/esp"
        echo "  git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git"
        echo "  cd esp-idf && ./install.sh esp32c3,esp32s3"
    fi
    return 1
}

source_idf_env || exit 1

echo "Building zclaw..."
if [ -n "$BOARD_PRESET" ]; then
    echo "Board preset: $BOARD_PRESET"
fi
if [ -n "$VOICE_I2S_BCLK_OVERRIDE" ] || [ -n "$VOICE_I2S_WS_OVERRIDE" ] || [ -n "$VOICE_I2S_DIN_OVERRIDE" ] || [ -n "$VOICE_I2S_PORT_OVERRIDE" ]; then
    echo "Voice I2S overrides: port=${VOICE_I2S_PORT_OVERRIDE:-<default>} bclk=${VOICE_I2S_BCLK_OVERRIDE:-<default>} ws=${VOICE_I2S_WS_OVERRIDE:-<default>} din=${VOICE_I2S_DIN_OVERRIDE:-<default>}"
fi

build_cmd=(idf.py)
if [ -n "$IDF_TARGET_OVERRIDE" ]; then
    build_cmd+=(-D "IDF_TARGET=$IDF_TARGET_OVERRIDE")
fi
if [ -n "$SDKCONFIG_DEFAULTS_OVERRIDE" ]; then
    build_cmd+=(-D "SDKCONFIG_DEFAULTS=$SDKCONFIG_DEFAULTS_OVERRIDE")
fi
if [ -n "$SDKCONFIG_FILE_OVERRIDE" ]; then
    mkdir -p "$PROJECT_DIR/$(dirname "$SDKCONFIG_FILE_OVERRIDE")"
    build_cmd+=(-D "SDKCONFIG=$SDKCONFIG_FILE_OVERRIDE")
fi
build_cmd+=(build)
"${build_cmd[@]}"

if [ -n "$PAD_TARGET_BYTES" ]; then
    SRC_BIN="$PROJECT_DIR/build/zclaw.bin"
    OUT_BIN="$PROJECT_DIR/build/zclaw-888kb.bin"

    if [ ! -f "$SRC_BIN" ]; then
        echo "Error: Expected firmware binary not found at $SRC_BIN"
        exit 1
    fi

    cp "$SRC_BIN" "$OUT_BIN"
    CUR_SIZE="$(file_size_bytes "$OUT_BIN")"

    if [ "$CUR_SIZE" -gt "$PAD_TARGET_BYTES" ]; then
        echo "Error: build/zclaw.bin is ${CUR_SIZE} bytes, larger than 888 KiB target (${PAD_TARGET_BYTES} bytes)."
        exit 1
    fi

    PAD_BYTES=$((PAD_TARGET_BYTES - CUR_SIZE))
    if [ "$PAD_BYTES" -gt 0 ]; then
        dd if=/dev/zero bs=1 count="$PAD_BYTES" 2>/dev/null | LC_ALL=C tr '\000' '\377' >> "$OUT_BIN"
    fi

    FINAL_SIZE="$(file_size_bytes "$OUT_BIN")"
    if [ "$FINAL_SIZE" -ne "$PAD_TARGET_BYTES" ]; then
        echo "Error: Padded binary size mismatch ($FINAL_SIZE bytes)."
        exit 1
    fi

    echo "Padded binary created: build/zclaw-888kb.bin ($FINAL_SIZE bytes)"
fi

echo ""
echo "Build complete! To flash: ./scripts/flash.sh"
