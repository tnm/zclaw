# zclaw

<img
  src="docs/images/lobster_xiao_cropped_left.png"
  alt="Lobster soldering a Seeed Studio XIAO ESP32-C3"
  height="200"
  align="right"
/>

The smallest possible AI personal assistant for ESP32.

zclaw is written in C and runs on ESP32 boards with a strict firmware budget target of **<= 888 KB** on the default build. It supports scheduled tasks, GPIO control, persistent memory, and custom tool composition through natural language.

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

Important setup notes:

- `bootstrap.sh` clones/updates the repo and then runs `./install.sh`.
- For encrypted credentials in flash, use secure mode (`--flash-mode secure` in install flow, or `./scripts/flash-secure.sh` directly).
- After flashing, provision WiFi + LLM credentials with `./scripts/provision.sh`.
- If serial port is busy, run `./scripts/release-port.sh` and retry.
- Full setup/provisioning details are in the docs site index.

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

## Useful Scripts

- `./scripts/build.sh` - Build firmware
- `./scripts/flash.sh` - Flash firmware
- `./scripts/flash-secure.sh` - Flash with encryption
- `./scripts/provision.sh` - Provision credentials to NVS
- `./scripts/monitor.sh` - Serial monitor
- `./scripts/emulate.sh` - Run QEMU profile
- `./scripts/web-relay.sh` - Hosted relay + mobile chat UI
- `./scripts/benchmark.sh` - Benchmark relay/serial latency
- `./scripts/docs-site.sh` - Serve docs site
- `./scripts/test.sh` - Run host/device test flows

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
