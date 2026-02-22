# Changelog

All notable changes to this project are documented in this file.

The format is based on Keep a Changelog and this project follows Semantic Versioning.

## [Unreleased]

### Changed
- _None yet._

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
