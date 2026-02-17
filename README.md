# zclaw

The smallest possible AI personal assistant running on an ESP32. 

Talk to your agent via Telegram for ~$5 in hardware. 

Create scheduled tasks & custom tools, ask questions, use sensors and cameras.

```
You: "Remind me to water the plants every morning at 8am"
Agent: Done. I'll message you daily at 8:00 AM.

You: "What's the temperature?"
Agent: The sensor reads 72°F (22°C).

You: "Turn off the lights"
Agent: Done. GPIO2 is now off.
```

## Features

- **Chat via Telegram** — Message your agent from anywhere
- **Scheduled tasks** — "Remind me every hour" or "Check sensors at 6pm daily"
- **Built-in and custom tools** - Ships with a pre-built set of tools, easy to extend
- **GPIO control** — Read sensors, toggle relays, control LEDs
- **Persistent memory** — Remembers things across reboots
- **Any LLM backend** — Anthropic, OpenAI, or open source models via OpenRouter
- **$5 hardware** — Just an ESP32 dev board and WiFi
- **~900 KB binary** — Fits in dual OTA partitions with 37% free

### Coming Soon

- **Camera support** — "What do you see?" (ESP32-S3 with OV2640)
- **Voice input** — Talk to your agent via I2S microphone
- **Sensor plugins** — Temperature, humidity, motion, soil moisture
- **Home Assistant integration** — Bridge to your smart home

## Hardware

**Any ESP32 board** works - no board-specific dependencies.

Good choice: [Seeed XIAO ESP32-C3](https://www.seeedstudio.com/Seeed-XIAO-ESP32C3-p-5431.html) (~$5)
- Tiny (21x17mm), USB-C, built-in antenna
- RISC-V core, 160MHz, 400KB SRAM, 4MB flash

Other options: ESP32-DevKitM, Adafruit QT Py, any generic ESP32 module.

**ESP32-S3** (with *Sense* for built-in camera/voice)
- Dual-core 240MHz, 512KB+ SRAM, 8MB+ flash
- OV2640 camera, I2S microphone support
- Cost: ~$8-15 USD

## Quick Start

### One-Line Setup

```bash
./install.sh
```

This interactive script installs ESP-IDF, QEMU, and dependencies. Works on macOS and Linux.

### Manual Setup

<details>
<summary>Click to expand manual installation steps</summary>

```bash
# Install ESP-IDF v5.4
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32c3
```

</details>

### Build & Flash

```bash
# Source ESP-IDF environment (needed in each new terminal)
source ~/esp/esp-idf/export.sh

# Build
idf.py build

# Flash to device (replace PORT with your serial port)
idf.py -p /dev/tty.usbserial-* flash monitor
```

Or use the convenience scripts:

```bash
./scripts/build.sh          # Build firmware
./scripts/flash.sh          # Flash to device
./scripts/flash-secure.sh   # Flash with encryption (dev mode, key readable)
./scripts/flash-secure.sh --production  # Flash with key read-protected
./scripts/monitor.sh        # Serial monitor
./scripts/emulate.sh        # Run in QEMU emulator
./scripts/exit-emulator.sh  # Stop QEMU emulator
./scripts/web-preview.sh    # Preview setup UI locally
```

### First Boot

1. On first boot, zclaw creates a WiFi AP: `zclaw-setup`
2. Check serial output for the randomly generated AP password
3. Connect to the AP and open `http://192.168.4.1`
4. Scan/select your SSID (or enter manually), then fill credentials and tokens
5. Device reboots and connects to your network

### Telegram Setup

1. Message [@BotFather](https://t.me/botfather) on Telegram
2. Create a new bot with `/newbot`
3. Copy the bot token to the setup page
4. Get your chat ID from [@userinfobot](https://t.me/userinfobot) and enter it in setup
5. Only messages from your chat ID will be accepted (security feature)

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    main.c                           │
│  Boot → WiFi/AP → NTP → Start Tasks                 │
└─────────────────────────────────────────────────────┘
         │              │              │
         ▼              ▼              ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│  telegram.c │  │   agent.c   │  │   cron.c    │
│  Poll msgs  │→ │   LLM loop  │ ←│  Scheduler  │
│  Send reply │← │ Tool calls  │  │  NTP sync   │
└─────────────┘  └─────────────┘  └─────────────┘
                       │
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│   tools.c   │  │  memory.c   │  │   llm.c     │
│  GPIO, mem  │  │  NVS store  │  │  HTTPS API  │
│  cron, time │  │             │  │             │
└─────────────┘  └─────────────┘  └─────────────┘
```

## Tools

| Tool | Description |
|------|-------------|
| `gpio_write` | Set GPIO pin high/low |
| `gpio_read` | Read GPIO pin state |
| `delay` | Wait milliseconds (max 60000) |
| `memory_set` | Store persistent user key-value (`u_*` keys only) |
| `memory_get` | Retrieve stored user value (`u_*` keys only) |
| `memory_list` | List stored user keys (`u_*`) |
| `memory_delete` | Delete stored user key (`u_*` keys only) |
| `cron_set` | Schedule periodic/daily task |
| `cron_list` | List scheduled tasks |
| `cron_delete` | Delete scheduled task |
| `get_time` | Get current time |
| `get_version` | Get firmware version |
| `get_health` | Get device health (heap, rate limits, uptime) |
| `check_update` | Manifest-based update check (currently not implemented) |
| `install_update` | Download and install firmware update |
| `create_tool` | Create a custom user-defined tool |
| `list_user_tools` | List all user-created tools |
| `delete_user_tool` | Delete a user-created tool |

### User-Defined Tools

Create custom tools through natural conversation. The agent remembers context and composes tools from that knowledge:

```
You: "The plant watering relay is on GPIO 5"
Agent: Got it, I'll remember that.

You: "Create a tool to water the plants for 30 seconds"
Agent: Created tool 'water_plants': Activates the watering relay for 30 seconds

You: "Water the plants"
Agent: [GPIO 5 on → 30s → off] Done, plants watered.
```

User tools are stored persistently and survive reboots. Up to 8 custom tools can be defined.

**How it works:**

1. **Creation** — When you ask to create a tool, Claude calls `create_tool` with:
   - `name`: short identifier (e.g., `water_plants`)
   - `description`: shown in tool list (e.g., "Water plants via GPIO 5")
   - `action`: natural language instructions (e.g., "Turn GPIO 5 on, wait 30 seconds, turn off")

2. **Storage** — The tool definition is saved to NVS (flash) and persists across reboots.

3. **Execution** — When you invoke the tool:
   - Claude calls your custom tool (e.g., `water_plants()`)
   - The agent returns: "Execute this action now: Turn GPIO 5 on, wait 30 seconds, turn off"
   - Claude interprets the action and calls built-in tools: `gpio_write(5,1)` → `delay(30000)` → `gpio_write(5,0)`
   - The C code runs on the ESP32, controlling actual hardware

User tools are compositions of built-in primitives (`gpio_write`, `delay`, `memory_set`, `cron_set`, etc.) — no new code is generated, just natural language that Claude decomposes into tool calls.

## Configuration

Edit `main/config.h` to customize:

```c
#define LLM_DEFAULT_MODEL_ANTHROPIC "claude-sonnet-4-5"   // Anthropic default
#define LLM_DEFAULT_MODEL_OPENAI    "gpt-5.2"             // OpenAI default
#define LLM_DEFAULT_MODEL_OPENROUTER "minimax/minimax-m2.5" // OpenRouter default
#define LLM_MAX_TOKENS 1024                   // Max response tokens
#define MAX_HISTORY_TURNS 8                   // Conversation history length
#define RATELIMIT_MAX_PER_HOUR 30             // LLM requests per hour
#define RATELIMIT_MAX_PER_DAY 200             // LLM requests per day
```

## Development

### Project Structure

```
zclaw/
├── main/
│   ├── main.c          # Boot sequence, WiFi, task startup
│   ├── agent.c         # Conversation loop
│   ├── telegram.c      # Telegram bot integration
│   ├── cron.c          # Task scheduler + NTP
│   ├── websetup.c      # Captive portal setup
│   ├── tools.c         # Tool registry/dispatch
│   ├── tools_gpio.c    # GPIO + delay tool handlers
│   ├── tools_memory.c  # Persistent memory tool handlers
│   ├── tools_cron.c    # Scheduler/time tool handlers
│   ├── tools_system.c  # Health/OTA/user-tool handlers
│   ├── llm.c           # LLM API client
│   ├── memory.c        # NVS persistence
│   ├── json_util.c     # cJSON helpers
│   ├── ratelimit.c     # Request rate limiting
│   ├── ota.c           # Over-the-air updates
│   ├── config.h        # All configuration
│   └── web/
│       ├── setup.html
│       └── success.html
├── scripts/
│   ├── build.sh        # Build firmware
│   ├── flash.sh        # Flash to device
│   ├── flash-secure.sh # Flash with encryption
│   ├── monitor.sh      # Serial monitor
│   ├── emulate.sh      # QEMU emulator
│   ├── exit-emulator.sh # Stop QEMU emulator
│   ├── web-preview.sh  # Local setup UI preview
│   └── test.sh         # Run tests
├── test/
│   └── host/           # Host-based unit tests
├── install.sh          # One-line setup script
├── partitions.csv      # Flash partition layout (dual OTA)
└── sdkconfig.defaults  # SDK defaults
```

### Running in QEMU

For faster development without hardware:

```bash
./scripts/emulate.sh
```

`emulate.sh` builds a dedicated QEMU profile (`build-qemu/`) with:
- UART0 local chat channel (interactive in terminal)
- Stub LLM enabled
- Offline emulator mode (no WiFi/NTP/Telegram startup)

Type a message and press Enter to interact. Exit with `Ctrl+A`, then `X`.
If the console is stuck, run `./scripts/exit-emulator.sh` from another terminal.

### Local Web Setup Preview

For rapid UI iteration without flashing hardware:

```bash
./scripts/web-preview.sh
```

Then open http://127.0.0.1:8080.

- **Phone testing** — Run `./scripts/web-preview.sh --host 0.0.0.0` and browse to `http://<your-computer-ip>:8080`
- **Live reload** — Enabled by default; pages refresh when `main/web/*.html` changes. Disable with `--no-reload`
- **Mock data** — `GET /networks` returns fake SSIDs; `POST /save` shows success page but doesn't persist

### Testing

```bash
./scripts/test.sh         # Run all tests
./scripts/test.sh host    # Host tests only (no hardware needed)
```

## Memory Usage

| Resource | Used | Free |
|----------|------|------|
| DRAM | ~145 KB | ~176 KB |
| Flash (per OTA slot) | 932 KB | 540 KB (37%) |

## Safety Features

- **Rate limiting** — Default 30 requests/hour, 200/day to prevent runaway API costs
- **Boot loop protection** — Enters safe mode after 3 consecutive boot failures
- **Telegram authentication** — Only accepts messages from configured chat ID
- **Secure setup** — AP requires randomly generated password (printed to serial)
- **OTA rollback** — Automatically reverts to previous firmware if update fails
- **Input validation** — Sanitizes all tool inputs to prevent injection
- **Flash encryption** — Optional encrypted storage for credentials (see below)

## Flash Encryption (Optional)

By default, credentials (WiFi password, API keys, Telegram token) are stored unencrypted in flash. Anyone with physical access can dump the flash chip and extract them.

For enhanced security, enable **flash encryption**:

```bash
./scripts/flash-secure.sh
```

For deployed devices, prefer:

```bash
./scripts/flash-secure.sh --production
```

This script:
1. Generates a unique 256-bit encryption key for your device
2. Burns the key to the ESP32's eFuse (one-time, permanent)
3. Encrypts all flash contents including stored credentials
4. Saves the key to `keys/` for future USB flashing
5. In `--production` mode, enables key read protection in eFuse

### Can I Still Re-flash?

**Yes!** You can still flash new firmware via USB — you just need the saved key file:

```bash
# First time (new device, development mode)
./scripts/flash-secure.sh    # Generates key, enables encryption, flashes (key remains readable)

# First time (new device, deployed/production mode)
./scripts/flash-secure.sh --production

# Future updates (same device)
./scripts/flash-secure.sh    # Finds saved key, flashes encrypted firmware
```

The script automatically detects if a device has encryption enabled and uses the matching key file from `keys/`.

### What Changes After Enabling

| Before (unencrypted) | After (encrypted) |
|---------------------|-------------------|
| `./scripts/flash.sh` | `./scripts/flash-secure.sh` |
| `idf.py flash` works | Must use secure script |
| Anyone can flash | Need the key file |
| Credentials exposed in flash dump | Credentials encrypted |

### Important Notes

| Consideration | Details |
|---------------|---------|
| **Permanent** | Can't disable encryption or go back to unencrypted |
| **Key backup** | Back up `keys/flash_key_<MAC>.bin` — needed for USB flashing |
| **OTA works** | Encrypted devices receive OTA updates normally |
| **Lost key** | Can still OTA update, but no USB flashing |

### When to Use

| Scenario | Recommendation |
|----------|----------------|
| Personal project, device stays home | Optional (revoke keys if lost) |
| Device may be lost/stolen | Enable encryption |
| Distributing to others | Enable encryption |

### Without Flash Encryption

If you don't enable encryption and lose the device, immediately revoke:
- **API keys**: Regenerate in Anthropic/OpenAI/OpenRouter dashboard
- **Telegram bot**: Message @BotFather → `/revoke`

## Factory Reset

Hold GPIO9 (BOOT button) for 5+ seconds during startup to erase all settings.

## License

MIT
