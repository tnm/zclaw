# Changelog

All notable changes to this project are documented in this file.

The format is based on Keep a Changelog and this project follows Semantic Versioning.

## [Unreleased]

### Fixed
- Linux installer dependency steps no longer hard-code `apt`; `install.sh` now detects and uses `apt-get`, `pacman`, `dnf`, or `zypper`.

### Changed
- Non-interactive installer prompts now default to `no` unless `-y`, explicit flags, or saved defaults are provided.
- Linux optional dependency installs (`QEMU`, `cJSON`) now use distro-specific package names per detected package manager.

### Docs
- Updated README, full reference README, and Getting Started docs with Linux package-manager detection and non-interactive prompt behavior.

### Tests
- Added host install-script coverage for Linux package-manager detection (`pacman`) and unsupported-manager fallback behavior.

## [2.8.2] - 2026-03-01

### Fixed
- GPIO reads now preserve driven output state instead of clobbering pin configuration before sampling.
- `gpio_read_all` no longer clears previously written pin levels when collecting multi-pin status.

### Changed
- `gpio_write` now configures pins as `GPIO_MODE_INPUT_OUTPUT` for reliable readback semantics.
- Read paths use `gpio_input_enable` instead of reset/reconfigure so active output drive is retained.

### Tests
- Added host regressions for `write -> read`, `read_all` preserving output state, and `HIGH -> LOW -> read` roundtrip behavior.
- Improved host GPIO driver stub to model input-buffer semantics and pin state transitions.

## [2.8.1] - 2026-03-01

### Added
- Added scoped runtime diagnostics via the `get_diagnostics` tool and local `/diag` command support for fast on-device checks.

### Fixed
- On classic `esp32` targets, GPIO tool operations now block flash/PSRAM-reserved pins (`GPIO6-11`) to avoid crashes on WROOM/VROOM-class boards.

### Changed
- Hardened GPIO read/write paths with stricter pin validation and clearer errors when pin configuration/read operations fail.

### Docs
- Updated README/docs references for runtime diagnostics usage and ESP32 GPIO flash-pin guardrails.

### Tests
- Added host coverage for runtime diagnostics command handling and ESP32 flash-pin exclusion behavior in GPIO policy tests.

## [2.8.0] - 2026-02-28

### Fixed
- Set the default runtime UART channel to `uart0` for classic ESP32/WROOM workflows to avoid channel mismatches.
- Hardened flash/install guardrails by adding connected-chip detection and target-mismatch checks before flashing.

### Changed
- Build/flash/monitor/erase/provision scripts now include improved serial-port handling and safer board-target defaults during install flows.
- Added factory-reset pin/hold Kconfig options consumed by runtime defaults.

### Docs
- Updated Getting Started and full reference docs for the safer install/flash flow and new serial troubleshooting guidance.

### Tests
- Added host coverage for install/provision script argument handling and flash target-guard behavior.

## [2.7.1] - 2026-02-23

### Fixed
- Added board-preset support for `esp32s3-box-3` across `scripts/build.sh`, `scripts/flash.sh`, and `scripts/flash-secure.sh`, including `--box-3` aliases and the required `IDF_TARGET`/`SDKCONFIG_DEFAULTS` overrides.
- Flash flows now fail fast when `--board esp32s3-box-3` is used against non-ESP32-S3 hardware.

### Changed
- Factory-reset pin/hold settings are now configurable via Kconfig (`CONFIG_ZCLAW_FACTORY_RESET_PIN`, `CONFIG_ZCLAW_FACTORY_RESET_HOLD_MS`) and consumed by runtime defaults.
- Added `sdkconfig.esp32s3-box-3.defaults` with BOX-3-safe GPIO allowlist and factory-reset button mapping.

### Docs
- Added Getting Started and reference notes for using BOX-3 preset flags in build/flash flows.

### Tests
- Added host tests for board-preset argument plumbing and target-mismatch rejection paths.
- Added `esp32s3-box-3` coverage to the firmware target matrix workflow.

## [2.7.0] - 2026-02-23

### Added
- Added first-class Ollama backend support with OpenAI-compatible request/response handling and runtime endpoint override via `llm_api_url`.
- Provisioning now supports custom API endpoint input via `--api-url` (`provision.sh` and `provision-dev.sh`) and persists endpoint overrides into NVS.
- Added host coverage for Ollama backend init/defaults, custom API URL override behavior, and provisioning flows around `--backend ollama`/`--api-url`.

### Changed
- Ollama is now included in backend choices across runtime provisioning/docs, with default model set to `qwen3:8b`.
- API key is optional for Ollama (while remaining required for Anthropic/OpenAI/OpenRouter).
- Provisioning API checks for OpenAI/OpenRouter now normalize chat-completions override URLs to `/models`, so valid runtime chat endpoints no longer cause false verification failures.

### Fixed
- Cleared LLM API key runtime state on `llm_init()` re-init before NVS load to prevent stale in-memory keys from carrying across backend/key changes.

### Docs
- Updated README + docs-site guidance for Ollama provisioning, including LAN-reachable endpoint requirement (`--api-url`) and backend support lists.

### Tests
- Added host tests ensuring OpenAI/OpenRouter API-check URL normalization and LLM re-init key-state clearing.

## [2.6.1] - 2026-02-23

### Changed
- Telegram polling now uses backend-specific timeout policy: OpenRouter uses a shorter poll timeout to reduce TLS memory-pressure overlap on constrained targets, while Anthropic/OpenAI behavior remains unchanged.

### Tests
- Added host unit tests for Telegram poll-timeout policy to ensure the OpenRouter override stays scoped and other backends keep the default timeout.

## [2.6.0] - 2026-02-23

### Added
- Telegram chat allowlist support (`tg_chat_ids`) for up to four authorized chat IDs, with per-chat reply routing for inbound Telegram messages.
- Host coverage for Telegram allowlist parsing and outbound target resolution behavior.

### Changed
- Provisioning now accepts comma-separated Telegram chat IDs (`--tg-chat-id` / `--tg-chat-ids`) and writes both `tg_chat_ids` and the backward-compatible primary `tg_chat_id`.
- Telegram outbound target routing now fails closed when a requested chat ID is unauthorized.
- Fixed debounce replay log milliseconds formatting in agent suppression logs.

### Docs
- Updated Telegram chat allowlist guidance across README and docs-site getting started/security/local-dev references.

## [2.5.3] - 2026-02-22

### Added
- Host coverage for byte-accurate WiFi credential validation paths and `llm.c` runtime initialization/request behavior in stub mode.

### Changed
- Provisioning now validates WiFi SSID/password by byte length (`32`/`63`) to match on-device limits and behavior.
- WiFi credential copy/validation logic is centralized in shared firmware helpers used by startup connection flow.
- Web relay `/api/chat` now requires JSON `Content-Type` and uses canonical origin matching across request and CORS handling.

## [2.5.2] - 2026-02-22

### Changed
- `/start` and `/help` onboarding examples now lead with natural-language usage and include scheduling/tool-creation examples.
- Getting Started now places Telegram setup ahead of first-chat examples and clarifies both non-interactive flags and interactive prompt entry for Telegram credentials.
- Added a short user/agent example flow in Getting Started for creating a tool and scheduling it by natural language.

## [2.5.1] - 2026-02-22

### Changed
- Capability-priority guidance in the system prompt now emphasizes custom tools, schedules, memory, and GPIO ahead of optional I2C details when users ask what zclaw can do.

## [2.5.0] - 2026-02-22

### Added
- Added `gpio_read_all`, a bulk GPIO state tool that reads all tool-allowed pins in one call.

### Changed
- Agent help text now includes `gpio read all`.
- System prompt now instructs the model to prefer `gpio_read_all` for all/multi-pin state requests.
- Increased composed system-prompt buffer size to avoid overflow fallback in host/device runs.

### Docs
- Updated `docs-site/tools.html` and `docs-site/reference/README_COMPLETE.md` with `gpio_read_all`.
- Updated top-level `README.md` highlights to mention bulk GPIO reads.

### Tests
- Added host tests for `gpio_read_all` output over the configured range and input-schema tolerance.

## [2.4.3] - 2026-02-22

### Changed
- Web relay defaults to `127.0.0.1` and now requires `ZCLAW_WEB_API_KEY` when binding to non-loopback hosts.
- Web relay CORS no longer uses wildcard behavior; optional exact-origin access is available via `--cors-origin` or `ZCLAW_WEB_CORS_ORIGIN`.
- Encrypted-boot startup now fails closed when encrypted NVS initialization fails, with an explicit dev-only override via `CONFIG_ZCLAW_ALLOW_UNENCRYPTED_NVS_FALLBACK`.

### Docs
- Updated relay setup docs and examples in `docs-site/getting-started.html`.
- Updated full reference README with relay CORS and encrypted-NVS startup notes in `docs-site/reference/README_COMPLETE.md`.

### Tests
- Added host tests covering origin normalization, loopback host detection, and non-loopback bind validation in `test/host/test_web_relay.py`.

## [2.4.2] - 2026-02-22

### Added
- docs-site `use-cases.html` chapter focused on practical and playful on-device assistant scenarios.
- docs-site `changelog.html` page for release notes on the website.
- top-level README links to the web changelog and repository changelog.
- Host regression test covering cron-triggered `cron_set` blocking behavior.

### Changed
- Agent system prompt now injects runtime device target and configured GPIO tool policy to reduce generic ESP32 pin-answer hallucinations.
- Cron-triggered turns now block `cron_set` calls so scheduled actions execute directly instead of self-rescheduling.
- Field Guide docs pages now use page-specific titles/social metadata.
- Use-cases chapter now describes observed runtime heap headroom instead of a generic 400KB runtime claim.

## [2.4.1] - 2026-02-22

### Added
- Built-in persona tools: `set_persona`, `get_persona`, `reset_persona`, with persistent storage.
- Host tests for persona changes through LLM tool-calling.

### Changed
- Persona/tone changes now route through normal LLM tool-calling flow instead of local parser shortcuts.
- Runtime persona prompt/context sync improved after persona tool calls.
- System prompt clarified on-device execution, plain-text output requirement, and persistent persona behavior.
- README setup notes moved into a collapsible section.

## [2.4.0] - 2026-02-22

### Changed
- Cron/scheduling responsiveness tightened (10-second check interval).
- Telegram output format shifted toward plain text defaults.
- Release defaults tuned for reliability and response quality.

## [2.3.1] - 2026-02-22

### Added
- Expanded network diagnostics/telemetry for LLM and Telegram transport behavior.

### Changed
- Rate limits increased for better real-world usability.
- Boot/task stability thresholds adjusted (including stack guard and boot-loop thresholds).

## [2.3.0] - 2026-02-22

### Added
- Telegram backlog clear helper script for local/dev operations.

### Changed
- Telegram polling hardened (stale/duplicate update handling, runtime state handling, UX reliability).
