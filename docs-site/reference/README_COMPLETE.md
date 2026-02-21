# zclaw

<img
  src="docs/images/lobster_xiao_cropped_left.png"
  alt="Lobster soldering a Seeed Studio XIAO ESP32-C3"
  height="200"
  align="right"
/>

The smallest possible AI personal assistant. 

Written in C. Runs on an ESP32. Or runs on many of them. Put one everywhere! Talk to your agent via Telegram for ~$5 in hardware. 

Create scheduled tasks & custom tools, ask questions, and use sensors. 888kb binary, easy flash, and terminal-based provisioning. 

As a rule, the binary will never exceed 888 kilobytes on the default build. MIT-licensed.

<br clear="right" />

```
You: "Remind me to water the plants every morning at 8am"
Agent: Done. I'll message you daily at 8:00 AM.

You: "What's the temperature?"
Agent: The sensor reads 72°F (22°C).

You: "Turn off the lights"
Agent: Done. GPIO2 is now off.
```

## Have fun

- **Chat via Telegram or hosted web relay** — Message your agent from anywhere
- **Scheduled tasks** — "Remind me every hour" or "Check sensors at 6pm daily" (timezone-aware)
- **Built-in and custom tools** - Ships with a pre-built set of tools, easy to extend
- **GPIO control** — Read sensors, toggle relays, control LEDs
- **Persistent memory** — Remembers things across reboots
- **Any LLM backend** — Anthropic, OpenAI, or open source models via OpenRouter
- **$5 hardware** — Just an ESP32 dev board and WiFi
- **~888 KB guaranteed max binary** — Fits in dual OTA partitions with ~40% free

### Soon?

- **Signed firmware updates** — Built-in `check_update`/`install_update` tools will return once verification is in place
- **Camera support** — "What do you see?" (ESP32-S3 with OV2640)
- **Voice input** — Talk to your agent via I2S microphone
- **Sensor plugins** — Temperature, humidity, motion, soil moisture
- **Home Assistant integration** — Bridge to your smart home

## Hardware

Tested targets are **ESP32-C3**, **ESP32-S3**, and **ESP32-C6**.
Other ESP32 variants should work fine (some may require manual ESP-IDF target setup):

- Default GPIO tool pin limits are configured for ESP32-C3 dev workflows (`GPIO 2-10`).
- If your board wiring differs, adjust `zclaw Configuration -> GPIO Tool Safety` in `idf.py menuconfig`.
- For boards with non-contiguous pins (for example XIAO ESP32S3), set `Allowed GPIO pins list` to a comma-separated whitelist. Example for XIAO ESP32S3 D0-D10: `1,2,3,4,5,6,7,8,9,43,44`

Good choice: [Seeed XIAO ESP32-C3](https://www.seeedstudio.com/Seeed-XIAO-ESP32C3-p-5431.html) (~$5)
- Tiny (21x17mm), USB-C, built-in antenna
- RISC-V core, 160MHz, 400KB SRAM, 4MB flash

Other options: ESP32-DevKitM, Adafruit QT Py, any generic ESP32 module.

## Quick Start

### One-Line Setup

This should generally work. If it does not, open an issue with details.

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh)
```

This bootstrap script clones/updates zclaw and then runs `./install.sh`. Works on macOS and Linux.

It also points you to flash helpers that auto-detect serial port/chip and can switch `idf.py` target on mismatch.
It remembers your choices in `~/.config/zclaw/install.env` (disable with `--no-remember`).
Saved QEMU/cJSON answers are auto-applied on future runs (override with `--qemu/--no-qemu` and `--cjson/--no-cjson`).
Interactive `install.sh` flashing defaults to standard mode; flash encryption is only enabled with `--flash-mode secure`.

<details>
<summary>You can also preseed install flags (click to expand)</summary>

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh) -- --build --flash --flash-mode secure
bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh) -- --build --flash --provision --monitor
bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh) -- --port /dev/cu.usbmodem1101 --monitor
bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh) -- --flash --kill-monitor
bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh) -- --no-qemu --no-cjson
```

</details>

Already cloned locally? You can still run:

```bash
./install.sh
```

Advanced board/app config is separate from install flow:

```bash
source ~/esp/esp-idf/export.sh
idf.py menuconfig
```

### Full Documentation (K&R-style)

The full old-school docs experience lives in `docs-site/` and is best viewed through the local docs server.

```bash
./scripts/docs-site.sh --open
# serves at http://127.0.0.1:8788
```

Direct chapter links:
- [Chapter 0: The 888 KB Assistant](docs-site/index.html)
- [Getting Started](docs-site/getting-started.html)
- [Tool Reference](docs-site/tools.html)
- [Architecture](docs-site/architecture.html)
- [Security](docs-site/security.html)
- [Docs site README](docs-site/README.md)

### Telegram Setup

1. Message [@BotFather](https://t.me/botfather) on Telegram
2. Create a new bot with `/newbot`
3. Copy the bot token into `./scripts/provision.sh --tg-token ...`
4. Get your chat ID from [@userinfobot](https://t.me/userinfobot) and set `--tg-chat-id ...`
5. Only messages from your chat ID will be accepted (security feature)

### Web Relay Setup (Optional, Host Relay + Phone UI)

This path keeps firmware unchanged and runs a host web app that forwards user
messages to the board over serial.

1. Optional but recommended: set an API key for browser/API access:
   ```bash
   export ZCLAW_WEB_API_KEY='choose-a-long-random-secret'
   ```
2. Run against a connected device.
   Local repo checkout:
   ```bash
   ./scripts/web-relay.sh --serial-port /dev/cu.usbmodem1101 --host 0.0.0.0 --port 8787
   ```
   No-clone bootstrap:
   ```bash
   bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap-web-relay.sh) -- --serial-port /dev/cu.usbmodem1101 --host 0.0.0.0 --port 8787
   ```
3. Open `http://<host>:8787` from phone or desktop.

Note: only one process should hold the serial port at a time. If you also run
`idf.py monitor` (or any serial console) against the same `/dev/cu.*` device,
relay chat calls can fail with a serial read error.
If needed, use `--kill-monitor` to stop ESP-IDF monitor holders before launch.
The no-clone bootstrap stores relay files in `~/.local/share/zclaw/web-relay` by default.

No board yet? Run with a built-in mock responder:

```bash
./scripts/web-relay.sh --mock-agent --host 0.0.0.0 --port 8787
```

This relay approach does not add web UI code to ESP32 firmware binary.

## Tools

| Tool | Description |
|------|-------------|
| `gpio_write` | Set GPIO pin high/low |
| `gpio_read` | Read GPIO pin state |
| `delay` | Wait milliseconds (max 60000) |
| `i2c_scan` | Scan I2C bus and list responding addresses |
| `memory_set` | Store persistent user key-value (`u_*` keys only) |
| `memory_get` | Retrieve stored user value (`u_*` keys only) |
| `memory_list` | List stored user keys (`u_*`) |
| `memory_delete` | Delete stored user key (`u_*` keys only) |
| `cron_set` | Schedule periodic/daily/one-time task |
| `cron_list` | List scheduled tasks |
| `cron_delete` | Delete scheduled task |
| `get_time` | Get current time |
| `set_timezone` | Set device timezone for daily schedules/time |
| `get_timezone` | Show current device timezone |
| `get_version` | Get firmware version |
| `get_health` | Get device health (heap, rate limits, time sync, version) |
| `create_tool` | Create a custom user-defined tool |
| `list_user_tools` | List all user-created tools |
| `delete_user_tool` | Delete a user-created tool |

Built-in firmware update tools are temporarily disabled and marked as coming soon.

`i2c_scan` requires `sda_pin` and `scl_pin` inputs (plus optional `frequency_hz`).
Example tool call input:

```json
{"sda_pin":8,"scl_pin":9,"frequency_hz":100000}
```

### Timezone And Daily Schedules

- `daily` schedules run in the device timezone.
- `once` schedules run a single time after N minutes.
- Default timezone is `UTC0` until changed.
- Use `set_timezone` first if you want local wall-clock reminders.

Example:

```text
You: Set timezone to America/Los_Angeles
Agent: Timezone set to PST8PDT,M3.2.0/2,M11.1.0/2 (PST)

You: Remind me daily at 8:15 to water the plants
Agent: Scheduled a daily reminder at 08:15 PST.

You: In 20 minutes, check the garage sensor
Agent: Scheduled a one-time reminder in 20 minutes.
```

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

1. **Creation** — When you ask to create a tool, the model calls `create_tool` with:
   - `name`: short identifier (e.g., `water_plants`)
   - `description`: shown in tool list (e.g., "Water plants via GPIO 5")
   - `action`: natural language instructions (e.g., "Turn GPIO 5 on, wait 30 seconds, turn off")

2. **Storage** — The tool definition is saved to NVS (flash) and persists across reboots.

3. **Execution** — When you invoke the tool:
   - The model calls your custom tool (e.g., `water_plants()`)
   - The agent returns: "Execute this action now: Turn GPIO 5 on, wait 30 seconds, turn off"
   - The model interprets the action and calls built-in tools: `gpio_write(5,1)` → `delay(30000)` → `gpio_write(5,0)`
   - The C code runs on the ESP32, controlling actual hardware

User tools are compositions of built-in primitives (`gpio_write`, `delay`, `memory_set`, `cron_set`, etc.) — no new code is generated, just natural language that the configured model decomposes into tool calls.

## Manual Setup

<details>
<summary>Click to expand manual installation steps</summary>

```bash
# Install ESP-IDF v5.4
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32c3,esp32s3
```

</details>

### Build & Flash

```bash
# Source ESP-IDF environment (needed in each new terminal)
source ~/esp/esp-idf/export.sh

# Build
idf.py build

# Flash to device (replace PORT with your serial port)
idf.py -p /dev/cu.usbmodem* flash monitor
```

If `source ~/esp/esp-idf/export.sh` fails, repair ESP-IDF tools:

```bash
cd ~/esp/esp-idf
./install.sh esp32c3,esp32s3
```

Or use the convenience scripts:

```bash
./scripts/build.sh          # Build firmware
./scripts/flash.sh          # Flash to device
./scripts/flash-secure.sh   # Flash with encryption (dev mode, key readable)
./scripts/flash-secure.sh --production  # Flash with key read-protected
./scripts/provision.sh      # Provision WiFi/API credentials into NVS
./scripts/monitor.sh        # Serial monitor
./scripts/release-port.sh   # Release busy serial port holders
./scripts/emulate.sh        # Run in QEMU emulator
./scripts/exit-emulator.sh  # Stop QEMU emulator
./scripts/docs-site.sh      # Serve the custom docs-site locally
./scripts/web-relay.sh      # Optional hosted web relay + mobile chat UI (safe launcher)
./scripts/bootstrap-web-relay.sh  # Download/run relay files without full repo clone
```

`flash.sh` and `flash-secure.sh` auto-detect connected chip type and prompt to run
`idf.py set-target <chip>` when project target does not match the board.

### First Boot

1. Flash firmware (`./scripts/flash.sh` or `./scripts/flash-secure.sh`)
2. Run provisioning (`./scripts/provision.sh --port <serial-port>`)
3. Enter required values:
   - WiFi SSID
   - LLM provider
   - LLM API key
4. Optional: WiFi password, Telegram bot token, Telegram chat ID
5. Reboot board and watch logs with `./scripts/monitor.sh`

`provision.sh` auto-detects your host WiFi SSID when possible.
For Anthropic, it also sends a quick `hello` API check after key entry.
Use `--skip-api-check` to bypass verification.


## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    main.c                           │
│  Boot → WiFi STA → NTP → Start Tasks                │
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

Board-specific GPIO safety range is configured in `idf.py menuconfig` under
`zclaw Configuration -> GPIO Tool Safety`.
You can use either min/max range or an explicit pin allowlist.

## Development

### Project Structure

```
zclaw/
├── main/
│   ├── main.c          # Boot sequence, WiFi, task startup
│   ├── agent.c         # Conversation loop
│   ├── telegram.c      # Telegram bot integration
│   ├── cron.c          # Task scheduler + NTP
│   ├── tools.c         # Tool registry/dispatch
│   ├── tools_gpio.c    # GPIO + delay tool handlers
│   ├── tools_memory.c  # Persistent memory tool handlers
│   ├── tools_cron.c    # Scheduler/time tool handlers
│   ├── tools_system.c  # Health/user-tool handlers
│   ├── llm.c           # LLM API client
│   ├── memory.c        # NVS persistence
│   ├── json_util.c     # cJSON helpers
│   ├── ratelimit.c     # Request rate limiting
│   ├── ota.c           # Version + rollback-state helpers
│   └── config.h        # All configuration
├── scripts/
│   ├── build.sh        # Build firmware
│   ├── bootstrap.sh    # Clone/update + run install.sh from GitHub
│   ├── flash.sh        # Flash to device
│   ├── flash-secure.sh # Flash with encryption
│   ├── provision.sh    # Provision credentials to NVS
│   ├── monitor.sh      # Serial monitor
│   ├── release-port.sh # Release busy serial port holders
│   ├── emulate.sh      # QEMU emulator
│   ├── exit-emulator.sh # Stop QEMU emulator
│   ├── benchmark.sh    # Latency benchmark launcher
│   ├── benchmark_latency.py # Relay/serial benchmark runner
│   ├── docs-site.sh    # Serve custom docs site locally
│   ├── web-relay.sh    # Web relay launcher with serial-port guards
│   ├── web_relay.py    # Hosted web relay + mobile chat UI
│   ├── requirements-web-relay.txt # Optional serial bridge deps
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

For real API calls from emulator, run host-bridged live mode:

```bash
# Anthropic
export ANTHROPIC_API_KEY=...
./scripts/emulate.sh --live-api --live-api-provider anthropic

# OpenAI
export OPENAI_API_KEY=...
./scripts/emulate.sh --live-api --live-api-provider openai
```

`--live-api` keeps QEMU offline but proxies LLM requests over UART to a host bridge process.
`--live-api-provider auto` (default) infers provider from request format.
Use `--live-api-logs` only when debugging bridge timing/forwarding.
Set `OPENAI_API_URL` to target an OpenAI-compatible endpoint other than the default.

Type a message and press Enter to interact. Exit with `Ctrl+A`, then `X`.
If the console is stuck, run `./scripts/exit-emulator.sh` from another terminal.

### Testing

```bash
./scripts/test.sh         # Run host tests (+ device-test build if sdkconfig.test exists)
./scripts/test.sh host    # Host tests only (no hardware needed)
./scripts/test.sh device  # Device-test build (requires sdkconfig.test)
```

This repo includes `sdkconfig.test` by default for dedicated device-test builds
with stubbed LLM/Telegram dependencies.

### Latency Benchmarking

```bash
# Relay benchmark (HTTP + relay + device response path)
./scripts/benchmark.sh --mode relay --count 20 --message "ping"

# Direct serial benchmark
./scripts/benchmark.sh --mode serial --serial-port /dev/cu.usbmodem1101 --count 20 --message "ping"
```

Serial mode reports host round-trip and first-response latency. If firmware logs
`METRIC request ...` lines, the benchmark also reports device-side total/LLM/tool timings.

## Memory Usage

| Resource | Used | Free |
|----------|------|------|
| DRAM | ~149 KB | ~172 KB |
| Flash (per OTA slot) | ~910 KB | ~598 KB (40%) |

## Safety Features

- **Rate limiting** — Default 30 requests/hour, 200/day to prevent runaway API costs
- **Boot loop protection** — Enters safe mode after 3 consecutive boot failures
- **Telegram authentication** — Only accepts messages from configured chat ID
- **Provisioning gate** — Device refuses normal boot until WiFi credentials are provisioned
- **Input validation** — Sanitizes all tool inputs to prevent injection
- **Flash encryption** — Optional encrypted storage for credentials (see below)

## Flash Encryption (Optional)

By default, credentials (WiFi password, API keys, Telegram token) are stored unencrypted in flash. Anyone with physical access can dump the flash chip and extract them.

For enhanced security, enable **flash encryption**:

```bash
./scripts/flash-secure.sh
```

Or use the installer with explicit opt-in:

```bash
./install.sh --build --flash --flash-mode secure
```

`--flash-mode secure` enables flash encryption only (not secure boot).

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

# Future reflashes (same device)
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
| **Remote OTA tools** | Coming soon (currently disabled) |
| **Lost key** | USB flashing won't work without the key backup |

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
- **Web relay secret**: Rotate `ZCLAW_WEB_API_KEY` on the host

## Factory Reset

Default is GPIO9 (BOOT on XIAO ESP32-C3): hold for 5+ seconds during startup
to erase all settings. On other boards, update `FACTORY_RESET_PIN` in `main/config.h`.

## License

MIT

![Lobster soldering a Seeed Studio XIAO ESP32-C3](docs/images/lobster_xiao_cropped_left.png)
