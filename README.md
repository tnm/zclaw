# zclaw

<img
  src="docs/images/lobster_xiao_cropped_left.png"
  alt="Lobster soldering a Seeed Studio XIAO ESP32-C3"
  height="200"
  align="right"
/>

The smallest possible AI personal assistant for ESP32.

zclaw is written in C and runs on ESP32 boards with a strict all-in firmware budget target of **<= 888 KiB** on the default build. It supports scheduled tasks, GPIO control, persistent memory, and custom tool composition through natural language.

The **888 KiB** cap is all-in firmware size, not just app code.
It includes `zclaw` logic plus ESP-IDF/FreeRTOS runtime, Wi-Fi/networking, TLS/crypto, and cert bundle overhead.

Fun to use, fun to hack on.
<br clear="right" />

## Full Documentation

Use the docs site for complete guides and reference.

- [Full documentation](https://zclaw.dev)
- [Use cases: useful + fun](https://zclaw.dev/use-cases.html)
- [Email bridge guide](https://zclaw.dev/email-bridge.html)
- [Changelog (web)](https://zclaw.dev/changelog.html)
- [Complete README (verbatim)](https://zclaw.dev/reference/README_COMPLETE.md)


## Quick Start

One-line bootstrap (macOS/Linux):

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh)
```

Already cloned?

```bash
./install.sh
```

Non-interactive install:

```bash
./install.sh -y
```

<details>
<summary>Setup notes</summary>

- `bootstrap.sh` clones/updates the repo and then runs `./install.sh`. You can inspect/verify the bootstrap flow first (including `ZCLAW_BOOTSTRAP_SHA256` integrity checks); see the [Getting Started docs](https://zclaw.dev/getting-started.html).
- Linux dependency installs auto-detect `apt-get`, `pacman`, `dnf`, or `zypper` during `install.sh` runs.
- In non-interactive mode, unanswered install prompts default to `no` unless you pass `-y` (or saved preferences/explicit flags apply).
- For encrypted credentials in flash, use secure mode (`--flash-mode secure` in install flow, or `./scripts/flash-secure.sh` directly).
- After flashing, provision WiFi + LLM credentials with `./scripts/provision.sh`.
- You can re-run either `./scripts/provision.sh` or `./scripts/provision-dev.sh` at any time (no reflash required) to update runtime credentials: WiFi SSID/password, LLM backend/model/API key (or Ollama API URL), and Telegram token/chat ID allowlist.
- Optional email tools (`email_send`, `email_list`, `email_read`) use an external bridge; provision `--email-bridge-url` and `--email-bridge-key` to enable.
- Host coverage for email tools runs in `./scripts/test.sh host` (validation, payload wiring, and bridge error handling for send/list/read).
- Default LLM rate limits are `100/hour` and `1000/day`; change compile-time limits in `main/config.h` (`RATELIMIT_*`).
- Quick validation path: run `./scripts/web-relay.sh` and send a test message to confirm the device can answer.
- If serial port is busy, run `./scripts/release-port.sh` and retry.
- For repeat local reprovisioning without retyping secrets, use `./scripts/provision-dev.sh` with a local profile file (`provision-dev.sh` wraps `provision.sh --yes`).

</details>

## Highlights

- Chat via Telegram or hosted web relay
- Timezone-aware schedules (`daily`, `periodic`, and one-shot `once`)
- Built-in + user-defined tools
- Runtime diagnostics via `get_diagnostics` (quick/runtime/memory/rates/time/all scopes)
- Optional email bridge tools for Gmail-backed send/list/read flows
- GPIO read/write control with guardrails (including bulk `gpio_read_all`)
- Persistent memory across reboots
- Persona options: `neutral`, `friendly`, `technical`, `witty`
- Provider support for Anthropic, OpenAI, OpenRouter, and Ollama (custom endpoint)

## Hardware

Tested targets: **ESP32-C3**, **ESP32-S3**, and **ESP32-C6**.
Other ESP32 variants should work fine (some may require manual ESP-IDF target setup).
Tests reports are very welcome!

Recommended starter board: [Seeed XIAO ESP32-C3](https://www.seeedstudio.com/Seeed-XIAO-ESP32C3-p-5431.html)

## Local Dev & Hacking

Typical fast loop:

```bash
./scripts/test.sh host
./scripts/build.sh
./scripts/flash.sh --kill-monitor /dev/cu.usbmodem1101
./scripts/provision-dev.sh --port /dev/cu.usbmodem1101
./scripts/monitor.sh /dev/cu.usbmodem1101
```

Profile setup once, then re-use:

```bash
./scripts/provision-dev.sh --write-template
# edit ~/.config/zclaw/dev.env
./scripts/provision-dev.sh --show-config
./scripts/provision-dev.sh

# if Telegram keeps replaying stale updates:
./scripts/telegram-clear-backlog.sh --show-config
```

More details in the [Local Dev & Hacking guide](https://zclaw.dev/local-dev.html).

### Other Useful Scripts

<details>
<summary>Show scripts</summary>

- `./scripts/flash-secure.sh` - Flash with encryption
- `./scripts/provision.sh` - Provision credentials to NVS
- `./scripts/provision-dev.sh` - Local profile wrapper for repeat provisioning
- `./scripts/telegram-clear-backlog.sh` - Clear queued Telegram updates
- `./scripts/erase.sh` - Erase NVS only (`--nvs`) or full flash (`--all`) with guardrails
- `./scripts/monitor.sh` - Serial monitor
- `./scripts/emulate.sh` - Run QEMU profile
- `./scripts/web-relay.sh` - Hosted relay + mobile chat UI
- `./scripts/benchmark.sh` - Benchmark relay/serial latency
- `./scripts/test.sh` - Run host/device test flows
- `./scripts/test-api.sh` - Run live provider API checks (manual/local)

</details>

## Size Breakdown

Current default `esp32s3` breakdown (grouped loadable image bytes from `idf.py -B build size-components`; rows sum to total image size):

| Segment | Bytes | Size | Share |
| --- | ---: | ---: | ---: |
| zclaw app logic (`libmain.a`) | `35742` | ~34.9 KiB | ~4.1% |
| Wi-Fi + networking stack | `397356` | ~388.0 KiB | ~45.7% |
| TLS/crypto stack | `112922` | ~110.3 KiB | ~13.0% |
| cert bundle + app metadata | `99722` | ~97.4 KiB | ~11.5% |
| other ESP-IDF/runtime/drivers/libc | `224096` | ~218.8 KiB | ~25.8% |

Total image size from this build is `869838` bytes; padded `zclaw.bin` is `869952` bytes (~849.6 KiB), still under the cap.

## Latency Benchmarking

Relay path benchmark (includes web relay processing + device round trip):

```bash
./scripts/benchmark.sh --mode relay --count 20 --message "ping"
```

Direct serial benchmark (host round trip + first response time). If firmware logs
`METRIC request ...` lines, the report also includes device-side timing:

```bash
./scripts/benchmark.sh --mode serial --serial-port /dev/cu.usbmodem1101 --count 20 --message "ping"
```

## License

MIT
