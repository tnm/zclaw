#!/bin/bash
#
# zclaw Development Environment Setup
# Installs ESP-IDF, QEMU, and dependencies
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION_FILE="$SCRIPT_DIR/VERSION"
ZCLAW_RELEASE_VERSION="dev"
ESP_IDF_VERSION="v5.4"
ESP_IDF_DIR="$HOME/esp/esp-idf"
ESP_IDF_CHIPS="esp32,esp32c3,esp32c6,esp32s3"
PREFS_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/zclaw"
PREFS_FILE="$PREFS_DIR/install.env"

if [ -f "$VERSION_FILE" ]; then
    ZCLAW_RELEASE_VERSION="$(sed -n '1p' "$VERSION_FILE" | tr -d '\r')"
    if [ -z "$ZCLAW_RELEASE_VERSION" ]; then
        ZCLAW_RELEASE_VERSION="dev"
    fi
fi

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m' # No Color

# Remembered installer preferences
REMEMBER_PREFS=true
PREFS_LOADED=false
PREFS_DIRTY=false
PREF_INSTALL_IDF=""
PREF_REPAIR_IDF=""
PREF_INSTALL_QEMU=""
PREF_INSTALL_CJSON=""
PREF_BUILD_NOW=""
PREF_REPAIR_BUILD_IDF=""
PREF_FLASH_NOW=""
PREF_FLASH_MODE=""
PREF_PROVISION_NOW=""
PREF_MONITOR_AFTER_FLASH=""
PREF_LAST_PORT=""

# CLI overrides
ASSUME_YES=false
FORCE_INSTALL_IDF=""
FORCE_REPAIR_IDF=""
FORCE_INSTALL_QEMU=""
FORCE_INSTALL_CJSON=""
FORCE_BUILD=""
FORCE_FLASH=""
FORCE_FLASH_MODE=""
FORCE_PROVISION=""
FORCE_MONITOR=""
FORCE_PORT=""
FORCE_KILL_MONITOR=""
LINUX_PKG_MANAGER=""
LINUX_PKG_MANAGER_LABEL=""

print_banner() {
    echo ""
    echo -e "${CYAN}${BOLD}"
    cat << 'EOF'
███████  ██████ ██       █████  ██     ██
   ███  ██      ██      ██   ██ ██     ██
  ███   ██      ██      ███████ ██  █  ██
 ███    ██      ██      ██   ██ ██ ███ ██
███████  ██████ ███████ ██   ██  ███ ███
EOF
    echo -e "${NC}"
    echo -e "${DIM}─────────────────────────────────----------───────────${NC}"
    echo -e "${MAGENTA}${BOLD}       The 5-dollar assistant in 888kb${NC}"
    echo -e "${DIM}                  zclaw release v${ZCLAW_RELEASE_VERSION}${NC}"
    echo -e "${DIM}───────────--------─────────────────────--────────────${NC}"
    echo ""
}

print_header() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

print_status() {
    echo -e "${GREEN}✓${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}!${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

usage() {
    cat << EOF
Usage: ./install.sh [options]

Options:
  -V, --version                         Print zclaw release version and exit
  -y, --yes                            Assume "yes" for prompts (explicit --no-* still wins)
  --build / --no-build                  Build firmware now
  --flash / --no-flash                  Flash firmware after successful build
  --flash-mode standard|secure          Explicit flash mode for this run (default: standard)
  --provision / --no-provision          Provision credentials after successful flash
  --monitor / --no-monitor              Open serial monitor after standard flash
  --port <serial-port>                  Use this serial port for flash/monitor
  --kill-monitor / --no-kill-monitor    Auto-stop stale ESP-IDF monitor before flash
  --qemu / --no-qemu                    Install optional QEMU dependency
  --cjson / --no-cjson                  Install optional cJSON dependency
  --install-idf / --no-install-idf      Install ESP-IDF if missing
  --repair-idf / --no-repair-idf        Repair ESP-IDF when activation fails
  --remember / --no-remember            Enable/disable saved installer defaults
  -h, --help                            Show this help

Saved defaults path:
  $PREFS_FILE
EOF
}

normalize_yes_no() {
    case "$1" in
        [Yy]|[Yy][Ee][Ss]|1|[Tt][Rr][Uu][Ee]|[Oo][Nn]) echo "y" ;;
        [Nn]|[Nn][Oo]|0|[Ff][Aa][Ll][Ss][Ee]|[Oo][Ff][Ff]) echo "n" ;;
        *) echo "" ;;
    esac
}

normalize_flash_mode() {
    case "$1" in
        1|standard|STANDARD|Standard) echo "1" ;;
        2|secure|SECURE|Secure) echo "2" ;;
        *) echo "" ;;
    esac
}

normalize_serial_port() {
    local port="$1"
    local callout_port

    case "$port" in
        /dev/tty.usb*)
            callout_port="/dev/cu.${port#/dev/tty.}"
            if [ -e "$callout_port" ]; then
                echo "$callout_port"
                return
            fi
            ;;
    esac

    echo "$port"
}

get_preference() {
    case "$1" in
        INSTALL_IDF) echo "$PREF_INSTALL_IDF" ;;
        REPAIR_IDF) echo "$PREF_REPAIR_IDF" ;;
        INSTALL_QEMU) echo "$PREF_INSTALL_QEMU" ;;
        INSTALL_CJSON) echo "$PREF_INSTALL_CJSON" ;;
        BUILD_NOW) echo "$PREF_BUILD_NOW" ;;
        REPAIR_BUILD_IDF) echo "$PREF_REPAIR_BUILD_IDF" ;;
        FLASH_NOW) echo "$PREF_FLASH_NOW" ;;
        FLASH_MODE) echo "$PREF_FLASH_MODE" ;;
        PROVISION_NOW) echo "$PREF_PROVISION_NOW" ;;
        MONITOR_AFTER_FLASH) echo "$PREF_MONITOR_AFTER_FLASH" ;;
        LAST_PORT) echo "$PREF_LAST_PORT" ;;
        *) echo "" ;;
    esac
}

set_preference() {
    local key="$1"
    local value="$2"

    case "$key" in
        INSTALL_IDF)
            [ "$PREF_INSTALL_IDF" = "$value" ] && return 0
            PREF_INSTALL_IDF="$value"
            ;;
        REPAIR_IDF)
            [ "$PREF_REPAIR_IDF" = "$value" ] && return 0
            PREF_REPAIR_IDF="$value"
            ;;
        INSTALL_QEMU)
            [ "$PREF_INSTALL_QEMU" = "$value" ] && return 0
            PREF_INSTALL_QEMU="$value"
            ;;
        INSTALL_CJSON)
            [ "$PREF_INSTALL_CJSON" = "$value" ] && return 0
            PREF_INSTALL_CJSON="$value"
            ;;
        BUILD_NOW)
            [ "$PREF_BUILD_NOW" = "$value" ] && return 0
            PREF_BUILD_NOW="$value"
            ;;
        REPAIR_BUILD_IDF)
            [ "$PREF_REPAIR_BUILD_IDF" = "$value" ] && return 0
            PREF_REPAIR_BUILD_IDF="$value"
            ;;
        FLASH_NOW)
            [ "$PREF_FLASH_NOW" = "$value" ] && return 0
            PREF_FLASH_NOW="$value"
            ;;
        FLASH_MODE)
            [ "$PREF_FLASH_MODE" = "$value" ] && return 0
            PREF_FLASH_MODE="$value"
            ;;
        PROVISION_NOW)
            [ "$PREF_PROVISION_NOW" = "$value" ] && return 0
            PREF_PROVISION_NOW="$value"
            ;;
        MONITOR_AFTER_FLASH)
            [ "$PREF_MONITOR_AFTER_FLASH" = "$value" ] && return 0
            PREF_MONITOR_AFTER_FLASH="$value"
            ;;
        LAST_PORT)
            [ "$PREF_LAST_PORT" = "$value" ] && return 0
            PREF_LAST_PORT="$value"
            ;;
        *)
            return 1
            ;;
    esac

    PREFS_DIRTY=true
    return 0
}

load_preferences() {
    local key value normalized

    [ "$REMEMBER_PREFS" = true ] || return 0
    [ -f "$PREFS_FILE" ] || return 0

    while IFS='=' read -r key value; do
        [ -n "$key" ] || continue
        case "$key" in
            \#*) continue ;;
        esac

        value="${value%$'\r'}"

        case "$key" in
            INSTALL_IDF|REPAIR_IDF|INSTALL_QEMU|INSTALL_CJSON|BUILD_NOW|REPAIR_BUILD_IDF|FLASH_NOW|PROVISION_NOW|MONITOR_AFTER_FLASH)
                normalized="$(normalize_yes_no "$value")"
                [ -n "$normalized" ] && set_preference "$key" "$normalized"
                ;;
            FLASH_MODE)
                case "$value" in
                    1|2) set_preference FLASH_MODE "$value" ;;
                esac
                ;;
            LAST_PORT)
                [ -n "$value" ] && set_preference LAST_PORT "$value"
                ;;
        esac
    done < "$PREFS_FILE"

    PREFS_DIRTY=false
    PREFS_LOADED=true
}

save_preferences() {
    local tmp_file

    [ "$REMEMBER_PREFS" = true ] || return 0
    [ "$PREFS_DIRTY" = true ] || return 0

    if ! mkdir -p "$PREFS_DIR" 2>/dev/null; then
        print_warning "Could not save installer defaults to $PREFS_FILE (directory not writable)"
        return 0
    fi

    tmp_file="$(mktemp -t zclaw-install-prefs.XXXXXX 2>/dev/null || true)"
    if [ -z "$tmp_file" ]; then
        print_warning "Could not create temporary file for installer defaults"
        return 0
    fi

    if ! cat > "$tmp_file" << EOF
# zclaw install.sh preferences
INSTALL_IDF=$PREF_INSTALL_IDF
REPAIR_IDF=$PREF_REPAIR_IDF
INSTALL_QEMU=$PREF_INSTALL_QEMU
INSTALL_CJSON=$PREF_INSTALL_CJSON
BUILD_NOW=$PREF_BUILD_NOW
REPAIR_BUILD_IDF=$PREF_REPAIR_BUILD_IDF
FLASH_NOW=$PREF_FLASH_NOW
FLASH_MODE=$PREF_FLASH_MODE
PROVISION_NOW=$PREF_PROVISION_NOW
MONITOR_AFTER_FLASH=$PREF_MONITOR_AFTER_FLASH
LAST_PORT=$PREF_LAST_PORT
EOF
    then
        rm -f "$tmp_file"
        print_warning "Could not write installer defaults to $PREFS_FILE"
        return 0
    fi

    if ! mv "$tmp_file" "$PREFS_FILE" 2>/dev/null; then
        rm -f "$tmp_file"
        print_warning "Could not update installer defaults at $PREFS_FILE"
        return 0
    fi

    PREFS_DIRTY=false
}

on_exit() {
    save_preferences || true
}

parse_args() {
    local mode_value

    while [ $# -gt 0 ]; do
        case "$1" in
            -V|--version)
                echo "zclaw release v$ZCLAW_RELEASE_VERSION"
                exit 0
                ;;
            --build) FORCE_BUILD="y" ;;
            --no-build) FORCE_BUILD="n" ;;
            --flash) FORCE_FLASH="y" ;;
            --no-flash) FORCE_FLASH="n" ;;
            --flash-mode)
                shift
                [ $# -gt 0 ] || { print_error "--flash-mode requires a value"; exit 1; }
                mode_value="$(normalize_flash_mode "$1")"
                [ -n "$mode_value" ] || { print_error "Invalid --flash-mode value: $1"; exit 1; }
                FORCE_FLASH_MODE="$mode_value"
                ;;
            --flash-mode=*)
                mode_value="$(normalize_flash_mode "${1#*=}")"
                [ -n "$mode_value" ] || { print_error "Invalid --flash-mode value: ${1#*=}"; exit 1; }
                FORCE_FLASH_MODE="$mode_value"
                ;;
            --provision) FORCE_PROVISION="y" ;;
            --no-provision) FORCE_PROVISION="n" ;;
            --monitor) FORCE_MONITOR="y" ;;
            --no-monitor) FORCE_MONITOR="n" ;;
            --port)
                shift
                [ $# -gt 0 ] || { print_error "--port requires a value"; exit 1; }
                FORCE_PORT="$1"
                ;;
            --port=*)
                FORCE_PORT="${1#*=}"
                ;;
            --kill-monitor) FORCE_KILL_MONITOR="y" ;;
            --no-kill-monitor) FORCE_KILL_MONITOR="n" ;;
            --qemu) FORCE_INSTALL_QEMU="y" ;;
            --no-qemu) FORCE_INSTALL_QEMU="n" ;;
            --cjson) FORCE_INSTALL_CJSON="y" ;;
            --no-cjson) FORCE_INSTALL_CJSON="n" ;;
            --install-idf) FORCE_INSTALL_IDF="y" ;;
            --no-install-idf) FORCE_INSTALL_IDF="n" ;;
            --repair-idf) FORCE_REPAIR_IDF="y" ;;
            --no-repair-idf) FORCE_REPAIR_IDF="n" ;;
            --remember) REMEMBER_PREFS=true ;;
            --no-remember) REMEMBER_PREFS=false ;;
            -y|--yes) ASSUME_YES=true ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
        shift
    done
}

ask_yes_no() {
    local prompt="$1"
    local default="${2:-y}"
    local pref_key="${3:-}"
    local forced_answer="${4:-}"
    local auto_apply_saved="${5:-false}"
    local answer
    local normalized
    local saved_default
    local has_saved_default=false

    if [ -n "$forced_answer" ]; then
        normalized="$(normalize_yes_no "$forced_answer")"
        [ -n "$normalized" ] || normalized="n"
        [ -n "$pref_key" ] && set_preference "$pref_key" "$normalized"

        if [ "$normalized" = "y" ]; then
            print_status "$prompt: yes"
            return 0
        fi
        print_status "$prompt: no"
        return 1
    fi

    if [ "$ASSUME_YES" = true ]; then
        [ -n "$pref_key" ] && set_preference "$pref_key" "y"
        print_status "$prompt: yes (-y)"
        return 0
    fi

    if [ -n "$pref_key" ]; then
        saved_default="$(get_preference "$pref_key")"
        if [ "$saved_default" = "y" ] || [ "$saved_default" = "n" ]; then
            default="$saved_default"
            has_saved_default=true
            if [ "$auto_apply_saved" = "true" ]; then
                if [ "$saved_default" = "y" ]; then
                    print_status "$prompt: yes (saved)"
                    return 0
                fi
                print_status "$prompt: no (saved)"
                return 1
            fi
        fi
    fi

    if [ "$default" = "y" ]; then
        prompt="$prompt [Y/n] "
    else
        prompt="$prompt [y/N] "
    fi

    if [ -t 0 ]; then
        read -r -p "$prompt" answer
        answer="${answer:-$default}"
    else
        # In non-interactive mode, default to "no" unless a saved preference exists.
        if [ "$has_saved_default" = true ]; then
            answer="$default"
        else
            answer="n"
        fi
    fi
    normalized="$(normalize_yes_no "$answer")"
    [ -n "$normalized" ] || normalized="n"
    [ -n "$pref_key" ] && set_preference "$pref_key" "$normalized"

    [ "$normalized" = "y" ]
}

resolve_flash_mode() {
    local choice="${FORCE_FLASH_MODE:-}"
    local default="1"

    if [ -z "$choice" ]; then
        if [ "$(get_preference FLASH_MODE)" = "2" ]; then
            default="2"
        fi
        if [ -t 0 ]; then
            read -r -p "Choose [1/2] (default: $default): " choice
            choice="${choice:-$default}"
        else
            choice="$default"
        fi
        choice="$(normalize_flash_mode "$choice")"
        [ -n "$choice" ] || choice="$default"
    fi

    set_preference FLASH_MODE "$choice"
    FLASH_MODE_CHOICE="$choice"
}

flash_mode_name() {
    if [ "$1" = "2" ]; then
        echo "secure"
    else
        echo "standard"
    fi
}

print_first_boot_steps() {
    echo -e "  ${DIM}1.${NC} Run ${CYAN}./scripts/provision.sh${NC} (or use install prompt)"
    echo -e "  ${DIM}2.${NC} Enter required values: ${CYAN}WiFi SSID, LLM provider, API key${NC}"
    echo -e "  ${DIM}3.${NC} WiFi password and Telegram token/chat ID are optional"
    echo -e "  ${DIM}4.${NC} Reboot the board after provisioning"
    echo -e "  ${DIM}5.${NC} Open ${CYAN}./scripts/monitor.sh${NC} to confirm WiFi connect + ready logs"
}

check_command() {
    command -v "$1" &> /dev/null
}

run_with_privileges() {
    if check_command sudo; then
        sudo "$@"
    else
        "$@"
    fi
}

detect_os() {
    case "$(uname -s)" in
        Darwin*) echo "macos" ;;
        Linux*)  echo "linux" ;;
        *)       echo "unknown" ;;
    esac
}

detect_linux_package_manager() {
    if check_command apt-get && apt-get --version >/dev/null 2>&1; then
        echo "apt"
    elif check_command pacman && pacman --version >/dev/null 2>&1; then
        echo "pacman"
    elif check_command dnf && dnf --version >/dev/null 2>&1; then
        echo "dnf"
    elif check_command zypper && zypper --version >/dev/null 2>&1; then
        echo "zypper"
    else
        echo "unknown"
    fi
}

linux_package_manager_label() {
    case "$1" in
        apt) echo "apt-get" ;;
        pacman) echo "pacman" ;;
        dnf) echo "dnf" ;;
        zypper) echo "zypper" ;;
        *) echo "unknown" ;;
    esac
}

install_linux_packages() {
    local purpose="$1"
    shift

    [ "$OS" = "linux" ] || return 0
    [ "$#" -gt 0 ] || return 0

    if [ "$LINUX_PKG_MANAGER" = "unknown" ] || [ -z "$LINUX_PKG_MANAGER" ]; then
        print_warning "No supported Linux package manager detected; skipping $purpose install."
        echo "Install manually: $*"
        return 1
    fi

    case "$LINUX_PKG_MANAGER" in
        apt)
            echo "Installing $purpose via apt-get..."
            if run_with_privileges apt-get update && run_with_privileges apt-get install -y "$@"; then
                return 0
            fi
            ;;
        pacman)
            echo "Installing $purpose via pacman..."
            if run_with_privileges pacman -Sy --noconfirm --needed "$@"; then
                return 0
            fi
            ;;
        dnf)
            echo "Installing $purpose via dnf..."
            if run_with_privileges dnf install -y "$@"; then
                return 0
            fi
            ;;
        zypper)
            echo "Installing $purpose via zypper..."
            if run_with_privileges zypper --non-interactive install "$@"; then
                return 0
            fi
            ;;
    esac

    print_warning "Automatic $purpose install failed via $LINUX_PKG_MANAGER_LABEL."
    echo "Install manually: $*"
    return 1
}

idf_export_works() {
    local export_script="$1"
    local py_bin py_dir check_path

    [ -f "$export_script" ] || return 1

    # Keep Python selection stable across checks; login shells may pick a different python3.
    py_bin="$(command -v python3 || true)"
    check_path="$PATH"
    if [ -n "$py_bin" ]; then
        py_dir="$(dirname "$py_bin")"
        check_path="$py_dir:$PATH"
    fi

    env PATH="$check_path" bash -c "source \"$export_script\" >/dev/null 2>&1 && command -v idf.py >/dev/null 2>&1"
}

repair_idf_toolchain() {
    local py_bin py_dir repair_log repair_path

    if [ ! -f "$ESP_IDF_DIR/install.sh" ]; then
        print_error "Cannot repair ESP-IDF tools (missing $ESP_IDF_DIR/install.sh)"
        return 1
    fi

    py_bin="$(command -v python3 || true)"
    py_dir=""
    if [ -n "$py_bin" ]; then
        py_dir="$(dirname "$py_bin")"
    fi
    repair_path="$PATH"
    if [ -n "$py_dir" ]; then
        repair_path="$py_dir:$PATH"
    fi
    repair_log="$(mktemp -t zclaw-idf-repair.XXXXXX.log 2>/dev/null || mktemp)"

    print_warning "Running ESP-IDF repair: ./install.sh $ESP_IDF_CHIPS"
    if (cd "$ESP_IDF_DIR" && PATH="$repair_path" ./install.sh "$ESP_IDF_CHIPS") >"$repair_log" 2>&1 && \
       idf_export_works "$ESP_IDF_DIR/export.sh"; then
        rm -f "$repair_log"
        print_status "ESP-IDF toolchain repaired"
        return 0
    fi

    print_error "ESP-IDF is still not usable after repair"
    echo "Last repair log lines:"
    tail -n 20 "$repair_log" | sed 's/^/  /'
    if grep -q "Operation not permitted: '.*\\.espressif/python_env" "$repair_log"; then
        echo ""
        print_warning "Permission issue writing to ~/.espressif/python_env"
        echo "Try:"
        echo "  mkdir -p ~/.espressif/python_env"
        echo "  chmod -R u+rwX ~/.espressif"
    fi
    rm -f "$repair_log"
    echo ""
    echo "Try manually:"
    echo "  cd $ESP_IDF_DIR"
    echo "  ./install.sh $ESP_IDF_CHIPS"
    return 1
}

# ============================================================================
# Main
# ============================================================================

parse_args "$@"
if [ -n "$FORCE_PORT" ]; then
    FORCE_PORT="$(normalize_serial_port "$FORCE_PORT")"
fi
load_preferences
trap on_exit EXIT

clear 2>/dev/null || true
print_banner

echo -e "${BOLD}Welcome to the zclaw installer!${NC}"
echo ""
echo "This script will set up your development environment:"
echo -e "  ${GREEN}•${NC} ESP-IDF $ESP_IDF_VERSION ${DIM}(required for building)${NC}"
echo -e "  ${GREEN}•${NC} QEMU ${DIM}(optional, for emulation)${NC}"
echo -e "  ${GREEN}•${NC} cJSON ${DIM}(optional, for host tests)${NC}"
echo -e "  ${GREEN}•${NC} Flash helpers with serial + board-chip detection"
echo ""

OS=$(detect_os)
if [ "$OS" = "unknown" ]; then
    print_error "Unsupported operating system"
    exit 1
fi

print_status "Detected OS: $OS"
if [ "$OS" = "linux" ]; then
    LINUX_PKG_MANAGER="$(detect_linux_package_manager)"
    LINUX_PKG_MANAGER_LABEL="$(linux_package_manager_label "$LINUX_PKG_MANAGER")"
    if [ "$LINUX_PKG_MANAGER" = "unknown" ]; then
        print_warning "No supported package manager detected (tried apt-get, pacman, dnf, zypper)."
        print_warning "System dependency installs will be skipped; install packages manually if needed."
    else
        print_status "Detected Linux package manager: $LINUX_PKG_MANAGER_LABEL"
    fi
fi
if [ "$PREFS_LOADED" = true ]; then
    print_status "Loaded saved installer defaults from $PREFS_FILE"
elif [ "$REMEMBER_PREFS" = false ]; then
    print_status "Installer defaults disabled for this run (--no-remember)"
fi
IDF_READY=false

# ============================================================================
# Check/Install ESP-IDF
# ============================================================================

print_header "ESP-IDF Toolchain"

if [ -f "$ESP_IDF_DIR/export.sh" ]; then
    print_status "ESP-IDF found at $ESP_IDF_DIR"

    # Check version
    if [ -f "$ESP_IDF_DIR/version.txt" ]; then
        INSTALLED_VERSION=$(cat "$ESP_IDF_DIR/version.txt" 2>/dev/null || echo "unknown")
        echo "  Installed version: $INSTALLED_VERSION"
    fi

    if idf_export_works "$ESP_IDF_DIR/export.sh"; then
        print_status "ESP-IDF environment is healthy (export.sh + idf.py)"
        IDF_READY=true
    else
        print_warning "ESP-IDF found, but export.sh failed to activate idf.py"
        echo "  This usually means the ESP-IDF Python env/tools are missing."
        if ask_yes_no "Repair ESP-IDF tools now with ./install.sh $ESP_IDF_CHIPS?" "y" "REPAIR_IDF" "$FORCE_REPAIR_IDF"; then
            if repair_idf_toolchain; then
                IDF_READY=true
            fi
        fi
    fi
else
    print_warning "ESP-IDF not found"

    if ask_yes_no "Install ESP-IDF $ESP_IDF_VERSION?" "y" "INSTALL_IDF" "$FORCE_INSTALL_IDF"; then
        echo ""
        echo "Installing ESP-IDF..."

        # Prerequisites
        if [ "$OS" = "macos" ]; then
            if ! check_command brew; then
                print_error "Homebrew not found. Install from https://brew.sh"
                exit 1
            fi

            echo "Installing prerequisites via Homebrew..."
            brew install cmake ninja dfu-util python3 || true
        elif [ "$OS" = "linux" ]; then
            case "$LINUX_PKG_MANAGER" in
                apt)
                    IDF_PACKAGES=(git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0)
                    ;;
                pacman)
                    IDF_PACKAGES=(git wget flex bison gperf python python-pip cmake ninja ccache libffi openssl dfu-util libusb)
                    ;;
                dnf)
                    IDF_PACKAGES=(git wget flex bison gperf python3 python3-pip python3-virtualenv cmake ninja-build ccache libffi-devel openssl-devel dfu-util libusbx)
                    ;;
                zypper)
                    IDF_PACKAGES=(git wget flex bison gperf python3 python3-pip python3-virtualenv cmake ninja ccache libffi-devel libopenssl-devel dfu-util libusb-1_0-0)
                    ;;
                *)
                    IDF_PACKAGES=()
                    ;;
            esac

            if [ "${#IDF_PACKAGES[@]}" -gt 0 ]; then
                if ! install_linux_packages "ESP-IDF prerequisites" "${IDF_PACKAGES[@]}"; then
                    print_warning "Continuing without auto-installed prerequisites; ESP-IDF install may fail if deps are missing."
                fi
            else
                print_warning "Skipping Linux prerequisite install for ESP-IDF (unsupported package manager)."
            fi
        fi

        # Clone ESP-IDF
        mkdir -p "$HOME/esp"

        if [ -d "$ESP_IDF_DIR" ]; then
            print_warning "Directory exists, updating..."
            cd "$ESP_IDF_DIR"
            git fetch
            git checkout "$ESP_IDF_VERSION"
            git submodule update --init --recursive
        else
            echo "Cloning ESP-IDF $ESP_IDF_VERSION (this may take a few minutes)..."
            git clone -b "$ESP_IDF_VERSION" --recursive \
                https://github.com/espressif/esp-idf.git "$ESP_IDF_DIR"
        fi

        # Install ESP-IDF tools
        echo ""
        echo "Installing ESP-IDF tools for: $ESP_IDF_CHIPS"
        cd "$ESP_IDF_DIR"
        ./install.sh "$ESP_IDF_CHIPS"

        if idf_export_works "$ESP_IDF_DIR/export.sh"; then
            print_status "ESP-IDF installed successfully"
            IDF_READY=true
        else
            print_warning "ESP-IDF installed, but export.sh still can't activate idf.py"
            if ask_yes_no "Repair ESP-IDF tools now with ./install.sh $ESP_IDF_CHIPS?" "y" "REPAIR_IDF" "$FORCE_REPAIR_IDF"; then
                if repair_idf_toolchain; then
                    IDF_READY=true
                fi
            fi
        fi
    else
        print_warning "Skipping ESP-IDF installation"
        print_warning "You'll need to install it manually to build firmware"
    fi
fi

# ============================================================================
# Check/Install QEMU
# ============================================================================

print_header "QEMU Emulator (Optional)"

if check_command qemu-system-riscv32; then
    print_status "QEMU found: $(which qemu-system-riscv32)"
else
    print_warning "QEMU not found"

    if ask_yes_no "Install QEMU for ESP32 emulation?" "y" "INSTALL_QEMU" "$FORCE_INSTALL_QEMU" "true"; then
        if [ "$OS" = "macos" ]; then
            echo "Installing QEMU via Homebrew..."
            brew install qemu
        elif [ "$OS" = "linux" ]; then
            case "$LINUX_PKG_MANAGER" in
                apt) QEMU_PACKAGE="qemu-system-misc" ;;
                pacman) QEMU_PACKAGE="qemu-system-riscv" ;;
                dnf) QEMU_PACKAGE="qemu-system-riscv" ;;
                zypper) QEMU_PACKAGE="qemu-riscv" ;;
                *) QEMU_PACKAGE="" ;;
            esac

            if [ -n "$QEMU_PACKAGE" ]; then
                if ! install_linux_packages "QEMU" "$QEMU_PACKAGE"; then
                    print_warning "QEMU install did not complete automatically."
                fi
            else
                print_warning "Unsupported Linux package manager for QEMU install; install QEMU manually."
            fi
        fi

        if check_command qemu-system-riscv32; then
            print_status "QEMU installed successfully"
        else
            print_error "QEMU installation failed"
        fi
    else
        print_warning "Skipping QEMU installation"
    fi
fi

# ============================================================================
# Check/Install cJSON (for host tests)
# ============================================================================

print_header "cJSON Library (Optional, for tests)"

CJSON_FOUND=false
if [ -f "/opt/homebrew/include/cjson/cJSON.h" ] || \
   [ -f "/usr/local/include/cjson/cJSON.h" ] || \
   [ -f "/usr/include/cjson/cJSON.h" ]; then
    CJSON_FOUND=true
fi

if [ "$CJSON_FOUND" = true ]; then
    print_status "cJSON library found"
else
    print_warning "cJSON not found"

    if ask_yes_no "Install cJSON for running host tests?" "y" "INSTALL_CJSON" "$FORCE_INSTALL_CJSON" "true"; then
        if [ "$OS" = "macos" ]; then
            echo "Installing cJSON via Homebrew..."
            brew install cjson
        elif [ "$OS" = "linux" ]; then
            case "$LINUX_PKG_MANAGER" in
                apt) CJSON_PACKAGE="libcjson-dev" ;;
                pacman) CJSON_PACKAGE="cjson" ;;
                dnf) CJSON_PACKAGE="cjson-devel" ;;
                zypper) CJSON_PACKAGE="libcjson-devel" ;;
                *) CJSON_PACKAGE="" ;;
            esac

            if [ -n "$CJSON_PACKAGE" ]; then
                if ! install_linux_packages "cJSON" "$CJSON_PACKAGE"; then
                    print_warning "cJSON install did not complete automatically."
                fi
            else
                print_warning "Unsupported Linux package manager for cJSON install; install cJSON manually."
            fi
        fi

        if [ -f "/opt/homebrew/include/cjson/cJSON.h" ] || \
           [ -f "/usr/local/include/cjson/cJSON.h" ] || \
           [ -f "/usr/include/cjson/cJSON.h" ]; then
            print_status "cJSON installed"
        else
            print_warning "cJSON headers still not detected; host tests may be unavailable."
        fi
    else
        print_warning "Skipping cJSON installation"
        print_warning "Host tests won't be available"
    fi
fi

# ============================================================================
# Build project
# ============================================================================

print_header "Build zclaw"

BUILD_SUCCESS=false
BUILD_REQUESTED=false
FLASH_REQUESTED=false
FLASH_SUCCESS=false
PROVISION_REQUESTED=false
PROVISION_SUCCESS=false
if [ -f "$ESP_IDF_DIR/export.sh" ]; then
    if ask_yes_no "Build the firmware now?" "y" "BUILD_NOW" "$FORCE_BUILD"; then
        BUILD_REQUESTED=true
        echo ""
        echo "Building zclaw..."
        cd "$SCRIPT_DIR"

        if [ "$IDF_READY" != true ] && idf_export_works "$ESP_IDF_DIR/export.sh"; then
            IDF_READY=true
        fi

        if [ "$IDF_READY" != true ]; then
            print_warning "ESP-IDF environment is not active (idf.py unavailable)"
            if ask_yes_no "Run ESP-IDF repair now and retry build?" "y" "REPAIR_BUILD_IDF" "$FORCE_REPAIR_IDF"; then
                if repair_idf_toolchain; then
                    IDF_READY=true
                fi
            fi
        fi

        # Source ESP-IDF and build
        if [ "$IDF_READY" != true ]; then
            print_error "ESP-IDF environment is not active (idf.py unavailable)"
            echo "Repair with:"
            echo "  cd $ESP_IDF_DIR"
            echo "  ./install.sh $ESP_IDF_CHIPS"
        elif bash -c "source '$ESP_IDF_DIR/export.sh' && idf.py build"; then
            echo ""
            print_status "Build complete!"
            BUILD_SUCCESS=true
        else
            print_error "Build failed"
        fi
    fi
fi

# ============================================================================
# Flash to device
# ============================================================================

if [ "$BUILD_SUCCESS" = true ]; then
    print_header "Flash to Device"

    echo ""
    echo -e "${YELLOW}Before flashing, make sure:${NC}"
    echo ""
    echo "  1. Connect your ESP32 board via USB"
    echo "  2. The board should appear as a serial port:"
    echo "     • macOS: /dev/cu.usbmodem* (preferred) or /dev/tty.usbmodem*"
    echo "     • Linux: /dev/ttyUSB0 or /dev/ttyACM0"
    echo ""

    # Detect connected serial ports
    echo "Scanning for connected devices..."
    echo ""

    if [ "$OS" = "macos" ]; then
        PORTS=$(ls /dev/cu.usb* 2>/dev/null || true)
        if [ -z "$PORTS" ]; then
            PORTS=$(ls /dev/tty.usb* 2>/dev/null || true)
        fi
    else
        PORTS=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)
    fi

    if [ -z "$PORTS" ] && [ -n "$FORCE_PORT" ]; then
        PORTS="$FORCE_PORT"
    fi

    if [ -n "$PORTS" ]; then
        print_status "Found serial port(s):"
        PORT_LIST=()
        port_idx=1
        for port in $PORTS; do
            echo "     ${port_idx}. $port"
            PORT_LIST+=("$port")
            port_idx=$((port_idx + 1))
        done
        echo ""

        FLASH_PORT=""
        if [ -n "$FORCE_PORT" ]; then
            FLASH_PORT="$FORCE_PORT"
            print_status "Using port from CLI: $FLASH_PORT"
            set_preference LAST_PORT "$FLASH_PORT"
            echo ""
        elif [ "${#PORT_LIST[@]}" -eq 1 ]; then
            FLASH_PORT="${PORT_LIST[0]}"
            print_status "Auto-selected device: $FLASH_PORT"
            set_preference LAST_PORT "$FLASH_PORT"
            echo ""
        else
            if [ -n "$PREF_LAST_PORT" ]; then
                for port in "${PORT_LIST[@]}"; do
                    if [ "$port" = "$PREF_LAST_PORT" ]; then
                        FLASH_PORT="$port"
                        print_status "Auto-selected saved device: $FLASH_PORT"
                        echo ""
                        break
                    fi
                done
            fi
        fi

        if [ -z "$FLASH_PORT" ] && [ -t 0 ] && [ "$ASSUME_YES" != true ]; then
            read -r -p "Select device [1-${#PORT_LIST[@]}] or Enter for auto-detect in flash script: " port_choice
            if [ -n "$port_choice" ]; then
                if [[ "$port_choice" =~ ^[0-9]+$ ]] && [ "$port_choice" -ge 1 ] && [ "$port_choice" -le "${#PORT_LIST[@]}" ]; then
                    FLASH_PORT="${PORT_LIST[$((port_choice - 1))]}"
                    print_status "Using selected device: $FLASH_PORT"
                    set_preference LAST_PORT "$FLASH_PORT"
                    echo ""
                else
                    print_warning "Invalid selection. Flash script will auto-detect instead."
                    echo ""
                fi
            fi
        fi

        FLASH_MODE_CHOICE="1"
        if [ -n "$FORCE_FLASH_MODE" ]; then
            FLASH_MODE_CHOICE="$FORCE_FLASH_MODE"
            if [ "$FLASH_MODE_CHOICE" = "2" ]; then
                print_warning "Encrypted flash selected via --flash-mode secure (flash encryption, not secure boot)."
            else
                print_status "Flash mode selected via --flash-mode standard."
            fi
        else
            print_status "Default flash mode: standard"
            echo "Use --flash-mode secure to enable flash encryption (not secure boot)."
        fi

        if ask_yes_no "Flash firmware now? (mode: $(flash_mode_name "$FLASH_MODE_CHOICE"))" "y" "FLASH_NOW" "$FORCE_FLASH"; then
            FLASH_REQUESTED=true

            echo ""
            echo -e "${YELLOW}Tip: If flash fails, hold BOOT button while pressing RESET${NC}"
            echo ""

            cd "$SCRIPT_DIR"

            if [ "$FLASH_MODE_CHOICE" = "2" ]; then
                flash_cmd=(./scripts/flash-secure.sh)
                [ -n "$FLASH_PORT" ] && flash_cmd+=("$FLASH_PORT")
                [ "$FORCE_KILL_MONITOR" = "y" ] && flash_cmd+=(--kill-monitor)

                if "${flash_cmd[@]}"; then
                    echo ""
                    print_status "Encrypted flash complete!"
                    FLASH_SUCCESS=true
                else
                    print_error "Encrypted flash failed"
                fi
            else
                flash_cmd=(./scripts/flash.sh)
                [ -n "$FLASH_PORT" ] && flash_cmd+=("$FLASH_PORT")
                [ "$FORCE_KILL_MONITOR" = "y" ] && flash_cmd+=(--kill-monitor)

                if "${flash_cmd[@]}"; then
                    echo ""
                    print_status "Flash complete!"
                    FLASH_SUCCESS=true
                else
                    print_error "Flash failed"
                    echo ""
                    echo "Troubleshooting:"
                    echo "  • Hold BOOT button while pressing RESET, then try again"
                    echo "  • Check USB cable (some cables are charge-only)"
                    if [ -n "$FLASH_PORT" ]; then
                        echo "  • If port is busy, close monitor/serial apps: lsof $FLASH_PORT"
                        echo "  • Retry with explicit port: ./scripts/flash.sh $FLASH_PORT"
                        echo "  • Auto-kill stale IDF monitor: ./scripts/flash.sh --kill-monitor $FLASH_PORT"
                    else
                        echo "  • If port is busy, close monitor/serial apps: lsof /dev/cu.usb*"
                        echo "  • Try: ./scripts/flash.sh"
                        echo "  • Or: ./scripts/flash.sh --kill-monitor"
                    fi
                fi
            fi
        fi
    else
        print_warning "No ESP32 devices detected"
        echo ""
        echo "Connect your ESP32 board and run:"
        echo -e "  ${YELLOW}./scripts/flash.sh${NC}        (standard)"
        echo -e "  ${YELLOW}./scripts/flash-secure.sh${NC} (encrypted)"
    fi
fi

if [ "$FLASH_SUCCESS" = true ]; then
    echo ""
    echo "Provisioning writes WiFi + LLM credentials to device NVS over USB."
    if ask_yes_no "Provision now (required before normal boot)?" "y" "PROVISION_NOW" "$FORCE_PROVISION"; then
        PROVISION_REQUESTED=true
        provision_cmd=(./scripts/provision.sh)
        if [ -n "$FLASH_PORT" ]; then
            provision_cmd+=(--port "$FLASH_PORT")
        fi

        if "${provision_cmd[@]}"; then
            PROVISION_SUCCESS=true
            print_status "Provisioning complete!"
        else
            print_error "Provisioning failed"
        fi
    fi

    echo ""
    if ask_yes_no "Open serial monitor to see output?" "y" "MONITOR_AFTER_FLASH" "$FORCE_MONITOR"; then
        echo ""
        echo "Starting monitor (Ctrl+] to exit)..."
        echo ""
        monitor_cmd=(./scripts/monitor.sh)
        [ -n "$FLASH_PORT" ] && monitor_cmd+=("$FLASH_PORT")
        "${monitor_cmd[@]}"
    fi
fi

# ============================================================================
# Summary
# ============================================================================

SETUP_INCOMPLETE=false
if [ "$BUILD_REQUESTED" = true ] && [ "$BUILD_SUCCESS" != true ]; then
    SETUP_INCOMPLETE=true
fi
if [ "$FLASH_REQUESTED" = true ] && [ "$FLASH_SUCCESS" != true ]; then
    SETUP_INCOMPLETE=true
fi
if [ "$PROVISION_REQUESTED" = true ] && [ "$PROVISION_SUCCESS" != true ]; then
    SETUP_INCOMPLETE=true
fi

echo ""
echo -e "${CYAN}${BOLD}"
if [ "$SETUP_INCOMPLETE" = true ]; then
cat << 'EOF'
    ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
    ┃                  SETUP INCOMPLETE                        ┃
    ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
EOF
else
cat << 'EOF'
    ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
    ┃                   SETUP COMPLETE                          ┃
    ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
EOF
fi
echo -e "${NC}"

if [ "$BUILD_REQUESTED" = true ] && [ "$BUILD_SUCCESS" != true ]; then
    echo -e "${YELLOW}${BOLD}Build did not complete.${NC}"
    if [ "$IDF_READY" != true ]; then
        echo "ESP-IDF tools are still unavailable."
        echo "Repair with:"
        echo "  cd $ESP_IDF_DIR"
        echo "  ./install.sh $ESP_IDF_CHIPS"
    else
        echo "Check the build error above, then rerun:"
        echo "  ./scripts/build.sh"
    fi
    echo ""
fi

if [ "$PROVISION_REQUESTED" = true ] && [ "$PROVISION_SUCCESS" != true ]; then
    echo -e "${YELLOW}${BOLD}Provisioning did not complete.${NC}"
    echo "Retry with:"
    if [ -n "$FLASH_PORT" ]; then
        echo "  ./scripts/provision.sh --port $FLASH_PORT"
    else
        echo "  ./scripts/provision.sh"
    fi
    echo ""
fi

if [ "$FLASH_SUCCESS" = true ] && [ "$PROVISION_SUCCESS" = true ]; then
    echo -e "${BOLD}Provisioned:${NC}"
    echo ""
    echo "  Credentials are written to NVS."
    echo "  Reboot the board and watch logs with:"
    if [ -n "$FLASH_PORT" ]; then
        echo -e "  ${YELLOW}./scripts/monitor.sh $FLASH_PORT${NC}"
    else
        echo -e "  ${YELLOW}./scripts/monitor.sh${NC}"
    fi
    echo ""
elif [ "$FLASH_SUCCESS" = true ]; then
    echo -e "${BOLD}Next Step:${NC}"
    echo ""
    echo "  Provision credentials (required before normal boot):"
    if [ -n "$FLASH_PORT" ]; then
        echo -e "  ${YELLOW}./scripts/provision.sh --port $FLASH_PORT${NC}"
    else
        echo -e "  ${YELLOW}./scripts/provision.sh${NC}"
    fi
    echo ""
    echo "  First boot flow after provisioning:"
    print_first_boot_steps
    echo ""
elif [ "$BUILD_SUCCESS" = true ]; then
    echo -e "${BOLD}Next Step:${NC}"
    echo ""
    echo "  Flash firmware to device:"
    echo -e "  ${YELLOW}./scripts/flash.sh${NC}"
    echo ""
    echo "  After flash, first boot flow:"
    print_first_boot_steps
    echo ""
elif [ "$BUILD_REQUESTED" = true ]; then
    echo -e "${BOLD}Next Step:${NC}"
    echo ""
    echo "  Resolve ESP-IDF/build issues above, then rerun:"
    echo -e "  ${YELLOW}./scripts/build.sh${NC}"
    echo ""
fi

echo -e "${BOLD}Commands:${NC}"
echo ""
echo -e "  ${YELLOW}./scripts/build.sh${NC}          ${DIM}Build firmware${NC}"
echo -e "  ${YELLOW}./scripts/flash.sh${NC}          ${DIM}Flash to device${NC}"
echo -e "  ${YELLOW}./scripts/flash-secure.sh${NC}   ${DIM}Flash with encryption${NC}"
echo -e "  ${YELLOW}./scripts/provision.sh${NC}      ${DIM}Write WiFi/API credentials to NVS${NC}"
echo -e "  ${YELLOW}./scripts/monitor.sh${NC}        ${DIM}Serial monitor${NC}"
echo -e "  ${YELLOW}./scripts/emulate.sh${NC}        ${DIM}Run in QEMU${NC}"
echo ""

echo -e "${BOLD}Install flags:${NC}"
echo ""
echo -e "  ${YELLOW}./install.sh --build --flash --flash-mode secure${NC}"
echo -e "  ${YELLOW}./install.sh -y --build --flash --provision${NC}"
echo -e "  ${YELLOW}./install.sh --flash --provision${NC}"
echo -e "  ${YELLOW}./install.sh --port /dev/cu.usbmodem* --monitor${NC}"
echo -e "  ${YELLOW}./install.sh --flash --kill-monitor${NC}"
echo -e "  ${YELLOW}./install.sh --no-qemu --no-cjson${NC}"
echo ""

echo -e "${BOLD}Pro tip:${NC} Add to your shell config:"
echo -e "  ${YELLOW}alias idf='source ~/esp/esp-idf/export.sh'${NC}"
echo -e "  ${DIM}If that fails: cd ~/esp/esp-idf && ./install.sh esp32,esp32c3,esp32c6,esp32s3${NC}"
if [ "$REMEMBER_PREFS" = true ]; then
    echo -e "  ${DIM}Installer defaults: $PREFS_FILE (${YELLOW}--no-remember${DIM} to disable)${NC}"
fi
echo ""

echo -e "${DIM}─────────────────────────────────────────────────────────────${NC}"
echo ""
if [ "$SETUP_INCOMPLETE" = true ]; then
    echo -e "  ${YELLOW}${BOLD}Almost there.${NC}  ${DIM}Finish repair/build, then re-run install or flash scripts.${NC}"
else
    echo -e "  ${GREEN}${BOLD}Ready to hack!${NC}  ${DIM}Questions? github.com/tnm/zclaw${NC}"
fi
echo ""
echo -e "${DIM}─────────────────────────────────────────────────────────────${NC}"
echo ""

if [ "$SETUP_INCOMPLETE" = true ]; then
    exit 1
fi
