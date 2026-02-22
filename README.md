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
- [README (web)](https://zclaw.dev/README.html)
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

- `bootstrap.sh` clones/updates the repo and then runs `./install.sh`. Read more in the full docs.
- For encrypted credentials in flash, use secure mode (`--flash-mode secure` in install flow, or `./scripts/flash-secure.sh` directly).
- After flashing, provision WiFi + LLM credentials with `./scripts/provision.sh`.
- Default LLM rate limits are `100/hour` and `1000/day`; change compile-time limits in `main/config.h` (`RATELIMIT_*`).
- Quick validation path: run `./scripts/web-relay.sh` and send a test message to confirm the device can answer.
- If serial port is busy, run `./scripts/release-port.sh` and retry.
- For repeat local reprovisioning without retyping secrets, use `./scripts/provision-dev.sh` with a local profile file.

</details>

## Highlights

- Chat via Telegram or hosted web relay
- Timezone-aware schedules (`daily`, `periodic`, and one-shot `once`)
- Built-in + user-defined tools
- GPIO read/write control with guardrails
- Persistent memory across reboots
- Provider support for Anthropic, OpenAI, and OpenRouter

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

## Size Breakdown

Current default `esp32s3` breakdown (`idf.py -B build size-components`, flash totals):

- zclaw app logic (`libmain.a`): `26430` bytes (~25.8 KiB, ~3.1%)
- Wi-Fi + networking stack: `375278` bytes (~366.5 KiB, ~43.7%)
- TLS/crypto stack: `125701` bytes (~122.8 KiB, ~14.7%)
- cert bundle + app metadata: `92654` bytes (~90.5 KiB, ~10.8%)
- other ESP-IDF/runtime/drivers/libc: `237889` bytes (~232.3 KiB, ~27.7%)

`zclaw.bin` from the same build is `865888` bytes (~845.6 KiB), which stays under the cap.

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
