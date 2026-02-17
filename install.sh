#!/bin/bash
#
# zclaw Development Environment Setup
# Installs ESP-IDF, QEMU, and dependencies
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ESP_IDF_VERSION="v5.4"
ESP_IDF_DIR="$HOME/esp/esp-idf"
ESP_IDF_CHIPS="esp32c3,esp32s3"

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
    echo -e "${DIM}─────────────────────────────────────────────${NC}"
    echo -e "${MAGENTA}${BOLD}       The five-buck assistant.${NC}"
    echo -e "${DIM}─────────────────────────────────────────────${NC}"
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

ask_yes_no() {
    local prompt="$1"
    local default="${2:-y}"

    if [ "$default" = "y" ]; then
        prompt="$prompt [Y/n] "
    else
        prompt="$prompt [y/N] "
    fi

    read -p "$prompt" answer
    answer="${answer:-$default}"

    case "$answer" in
        [Yy]*) return 0 ;;
        *) return 1 ;;
    esac
}

check_command() {
    command -v "$1" &> /dev/null
}

detect_os() {
    case "$(uname -s)" in
        Darwin*) echo "macos" ;;
        Linux*)  echo "linux" ;;
        *)       echo "unknown" ;;
    esac
}

# ============================================================================
# Main
# ============================================================================

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
else
    print_warning "ESP-IDF not found"

    if ask_yes_no "Install ESP-IDF $ESP_IDF_VERSION?"; then
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
            echo "Installing prerequisites via apt..."
            sudo apt-get update
            sudo apt-get install -y git wget flex bison gperf python3 python3-pip \
                python3-venv cmake ninja-build ccache libffi-dev libssl-dev \
                dfu-util libusb-1.0-0
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

        print_status "ESP-IDF installed successfully"
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

    if ask_yes_no "Install QEMU for ESP32 emulation?"; then
        if [ "$OS" = "macos" ]; then
            echo "Installing QEMU via Homebrew..."
            brew install qemu
        elif [ "$OS" = "linux" ]; then
            echo "Installing QEMU via apt..."
            sudo apt-get install -y qemu-system-misc
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

    if ask_yes_no "Install cJSON for running host tests?"; then
        if [ "$OS" = "macos" ]; then
            echo "Installing cJSON via Homebrew..."
            brew install cjson
        elif [ "$OS" = "linux" ]; then
            echo "Installing cJSON via apt..."
            sudo apt-get install -y libcjson-dev
        fi
        print_status "cJSON installed"
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
if [ -f "$ESP_IDF_DIR/export.sh" ]; then
    if ask_yes_no "Build the firmware now?"; then
        echo ""
        echo "Building zclaw..."
        cd "$SCRIPT_DIR"

        # Source ESP-IDF and build
        if bash -c "source '$ESP_IDF_DIR/export.sh' && idf.py build"; then
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
    echo "     • macOS: /dev/tty.usbserial-* or /dev/tty.usbmodem*"
    echo "     • Linux: /dev/ttyUSB0 or /dev/ttyACM0"
    echo ""

    # Detect connected serial ports
    echo "Scanning for connected devices..."
    echo ""

    if [ "$OS" = "macos" ]; then
        PORTS=$(ls /dev/tty.usb* 2>/dev/null || true)
    else
        PORTS=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)
    fi

    if [ -n "$PORTS" ]; then
        print_status "Found serial port(s):"
        PORT_LIST=()
        for port in $PORTS; do
            echo "     $port"
            PORT_LIST+=("$port")
        done
        echo ""

        FLASH_PORT=""
        if [ "${#PORT_LIST[@]}" -eq 1 ]; then
            FLASH_PORT="${PORT_LIST[0]}"
            print_status "Auto-selected device: $FLASH_PORT"
            echo ""
        elif [ -t 0 ]; then
            read -r -p "Select device [1-${#PORT_LIST[@]}] or Enter for auto-detect in flash script: " port_choice
            if [ -n "$port_choice" ]; then
                if [[ "$port_choice" =~ ^[0-9]+$ ]] && [ "$port_choice" -ge 1 ] && [ "$port_choice" -le "${#PORT_LIST[@]}" ]; then
                    FLASH_PORT="${PORT_LIST[$((port_choice - 1))]}"
                    print_status "Using selected device: $FLASH_PORT"
                    echo ""
                else
                    print_warning "Invalid selection. Flash script will auto-detect instead."
                    echo ""
                fi
            fi
        fi

        if ask_yes_no "Flash firmware now?"; then
            echo ""
            echo "Flash options:"
            echo ""
            echo "  1. Standard flash (default)"
            echo "     Credentials stored unencrypted. Simple, easy to re-flash."
            echo ""
            echo "  2. Secure flash (enables flash encryption)"
            echo "     Credentials encrypted. PERMANENT - cannot be undone."
            echo "     Saves encryption key to keys/ for future flashing."
            echo ""
            echo "Both flash scripts auto-detect board chip and can switch target if needed."
            echo ""
            read -r -p "Choose [1/2]: " flash_choice
            flash_choice="${flash_choice:-1}"

            echo ""
            echo -e "${YELLOW}Tip: If flash fails, hold BOOT button while pressing RESET${NC}"
            echo ""

            cd "$SCRIPT_DIR"

            if [ "$flash_choice" = "2" ]; then
                flash_cmd=(./scripts/flash-secure.sh)
                [ -n "$FLASH_PORT" ] && flash_cmd+=("$FLASH_PORT")

                if "${flash_cmd[@]}"; then
                    echo ""
                    print_status "Secure flash complete!"
                else
                    print_error "Secure flash failed"
                fi
            else
                flash_cmd=(./scripts/flash.sh)
                [ -n "$FLASH_PORT" ] && flash_cmd+=("$FLASH_PORT")

                if "${flash_cmd[@]}"; then
                    echo ""
                    print_status "Flash complete!"

                    echo ""
                    if ask_yes_no "Open serial monitor to see output?"; then
                        echo ""
                        echo "Starting monitor (Ctrl+] to exit)..."
                        echo ""
                        monitor_cmd=(./scripts/monitor.sh)
                        [ -n "$FLASH_PORT" ] && monitor_cmd+=("$FLASH_PORT")
                        "${monitor_cmd[@]}"
                    fi
                else
                    print_error "Flash failed"
                    echo ""
                    echo "Troubleshooting:"
                    echo "  • Hold BOOT button while pressing RESET, then try again"
                    echo "  • Check USB cable (some cables are charge-only)"
                    echo "  • Try: ./scripts/flash.sh"
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

# ============================================================================
# Summary
# ============================================================================

echo ""
echo -e "${CYAN}${BOLD}"
cat << 'EOF'
    ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
    ┃                   SETUP COMPLETE                          ┃
    ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
EOF
echo -e "${NC}"

echo -e "${BOLD}First Boot:${NC}"
echo ""
echo -e "  ${DIM}1.${NC} zclaw creates WiFi hotspot: ${CYAN}zclaw-Setup${NC}"
echo -e "  ${DIM}2.${NC} Connect from your phone/laptop"
echo -e "  ${DIM}3.${NC} Open ${CYAN}http://192.168.4.1${NC}"
echo -e "  ${DIM}4.${NC} Enter WiFi + API credentials"
echo -e "  ${DIM}5.${NC} Device reboots and connects"
echo ""

echo -e "${BOLD}Commands:${NC}"
echo ""
echo -e "  ${YELLOW}./scripts/build.sh${NC}          ${DIM}Build firmware${NC}"
echo -e "  ${YELLOW}./scripts/flash.sh${NC}          ${DIM}Flash to device${NC}"
echo -e "  ${YELLOW}./scripts/flash-secure.sh${NC}   ${DIM}Flash with encryption${NC}"
echo -e "  ${YELLOW}./scripts/monitor.sh${NC}        ${DIM}Serial monitor${NC}"
echo -e "  ${YELLOW}./scripts/emulate.sh${NC}        ${DIM}Run in QEMU${NC}"
echo ""

echo -e "${BOLD}Pro tip:${NC} Add to your shell config:"
echo -e "  ${YELLOW}alias idf='source ~/esp/esp-idf/export.sh'${NC}"
echo ""

echo -e "${DIM}─────────────────────────────────────────────────────────────${NC}"
echo ""
echo -e "  ${GREEN}${BOLD}Ready to hack!${NC}  ${DIM}Questions? github.com/anthropics/zclaw${NC}"
echo ""
echo -e "${DIM}─────────────────────────────────────────────────────────────${NC}"
echo ""
