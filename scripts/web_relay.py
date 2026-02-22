#!/usr/bin/env python3
"""Hosted web relay for zclaw.

Users chat through a mobile-friendly web app, while this host process forwards
messages to the ESP32 over serial (or to a built-in mock agent).
"""

from __future__ import annotations

import argparse
import glob
import json
import logging
import os
import platform
import threading
import time
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Protocol
from urllib.parse import urlparse


ESP_LOG_PREFIXES = ("I (", "W (", "E (", "D (", "V (")
BOOT_LOG_PREFIXES = ("ets ", "rst:", "boot:", "SPIWP:", "mode:", "load:", "entry ")
MAX_CHAT_MESSAGE_LEN = 4096
SERIAL_BUSY_HINTS = (
    "multiple access on port",
    "resource busy",
    "could not exclusively lock port",
    "in use",
)
SERIAL_DISCONNECT_HINTS = (
    "device disconnected",
    "returned no data",
    "input/output error",
    "no such file or directory",
)


class AgentBridge(Protocol):
    def open(self) -> None: ...
    def close(self) -> None: ...
    def ask(self, prompt: str) -> str: ...


@dataclass(frozen=True)
class AppState:
    bridge: AgentBridge
    bridge_target: str
    api_key: str | None


def normalize_api_key(value: str | None) -> str | None:
    if value is None:
        return None
    stripped = value.strip()
    return stripped if stripped else None


def is_request_authorized(provided_key: str | None, expected_key: str | None) -> bool:
    if expected_key is None:
        return True
    if provided_key is None:
        return False
    return provided_key == expected_key


def is_probable_esp_log_line(line: str) -> bool:
    stripped = line.strip()
    if not stripped:
        return False
    return stripped.startswith(ESP_LOG_PREFIXES) or stripped.startswith(BOOT_LOG_PREFIXES)


def is_probable_serial_exception(exc: Exception) -> bool:
    module = exc.__class__.__module__
    name = exc.__class__.__name__.lower()
    if module.startswith("serial.") or "serial" in name:
        return True

    text = str(exc).lower()
    return any(hint in text for hint in (*SERIAL_BUSY_HINTS, *SERIAL_DISCONNECT_HINTS))


def describe_serial_exception(port: str, exc: Exception) -> str:
    text = str(exc).lower()
    if any(hint in text for hint in SERIAL_BUSY_HINTS):
        return (
            f"Serial port {port} appears busy (another process is using it). "
            "Stop other serial tools (for example: idf.py monitor) and retry."
        )
    if any(hint in text for hint in SERIAL_DISCONNECT_HINTS):
        return (
            f"Serial connection lost on {port}. Reconnect the device and retry. "
            "If a serial monitor is open, close it first."
        )
    return f"Serial error on {port}: {exc}"


def detect_serial_ports() -> list[str]:
    if platform.system() == "Darwin":
        patterns = (
            "/dev/cu.usbserial-*",
            "/dev/cu.usbmodem*",
            "/dev/tty.usbserial-*",
            "/dev/tty.usbmodem*",
        )
    else:
        patterns = ("/dev/ttyUSB*", "/dev/ttyACM*")

    ports: list[str] = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))
    return sorted(set(ports))


def resolve_serial_port(requested_port: str | None) -> str:
    if requested_port:
        return requested_port

    ports = detect_serial_ports()
    if not ports:
        raise ValueError("No serial port detected. Pass --serial-port explicitly.")
    if len(ports) > 1:
        raise ValueError(
            "Multiple serial ports detected. Pass --serial-port explicitly: "
            + ", ".join(ports)
        )
    return ports[0]


class SerialAgentBridge:
    def __init__(
        self,
        port: str,
        baudrate: int,
        serial_timeout_s: float,
        response_timeout_s: float,
        idle_timeout_s: float,
        log_serial: bool,
    ) -> None:
        self.port = port
        self.baudrate = baudrate
        self.serial_timeout_s = serial_timeout_s
        self.response_timeout_s = response_timeout_s
        self.idle_timeout_s = idle_timeout_s
        self.log_serial = log_serial
        self._serial = None
        self._lock = threading.Lock()

    def open(self) -> None:
        try:
            import serial  # type: ignore
        except ModuleNotFoundError as exc:
            raise RuntimeError(
                "pyserial is required for serial mode. Install with: "
                "uv run --with-requirements scripts/requirements-web-relay.txt "
                "scripts/web_relay.py --serial-port <port>"
            ) from exc

        try:
            self._serial = serial.Serial(
                self.port,
                self.baudrate,
                timeout=self.serial_timeout_s,
            )
        except Exception as exc:  # pragma: no cover - serial runtime dependent
            raise RuntimeError(f"Failed to open serial port {self.port}: {exc}") from exc

        time.sleep(0.2)
        self._drain_input_buffer()

    def close(self) -> None:
        if self._serial is None:
            return
        try:
            self._serial.close()
        except Exception:  # pragma: no cover - serial runtime dependent
            pass
        self._serial = None

    def ask(self, prompt: str) -> str:
        message = prompt.strip()
        if not message:
            raise ValueError("Message is empty")
        if self._serial is None:
            raise RuntimeError("Serial bridge is not open")

        try:
            with self._lock:
                self._drain_input_buffer()
                self._write_line(message)
                response_lines = self._read_response_lines(message)
        except TimeoutError:
            raise
        except Exception as exc:
            if is_probable_serial_exception(exc):
                raise RuntimeError(describe_serial_exception(self.port, exc)) from exc
            raise

        response = "\n".join(response_lines).strip()
        if not response:
            raise TimeoutError("No response text collected")
        return response

    def _drain_input_buffer(self) -> None:
        if self._serial is None:
            return
        try:
            self._serial.reset_input_buffer()
        except Exception:  # pragma: no cover - serial runtime dependent
            while True:
                data = self._serial.read(1024)
                if not data:
                    break

    def _write_line(self, line: str) -> None:
        if self._serial is None:
            return
        payload = (line + "\n").encode("utf-8")
        self._serial.write(payload)
        self._serial.flush()
        if self.log_serial:
            logging.info("serial>> %s", line)

    def _read_response_lines(self, sent_prompt: str) -> list[str]:
        if self._serial is None:
            return []

        deadline = time.monotonic() + self.response_timeout_s
        idle_deadline = time.monotonic() + self.idle_timeout_s
        saw_echo = False
        response_lines: list[str] = []

        while time.monotonic() < deadline:
            raw_line = self._serial.readline()
            now = time.monotonic()

            if not raw_line:
                if response_lines and now >= idle_deadline:
                    break
                continue

            line = raw_line.decode("utf-8", errors="replace").strip("\r\n")
            if self.log_serial:
                logging.info("serial<< %s", line)

            if not line:
                if response_lines:
                    # Preserve paragraph/list spacing from markdown responses.
                    # Response completion is determined by idle timeout instead of
                    # treating the first blank line as end-of-message.
                    if response_lines[-1] != "":
                        response_lines.append("")
                continue

            if not saw_echo and line.strip() == sent_prompt:
                saw_echo = True
                continue

            if is_probable_esp_log_line(line):
                continue

            response_lines.append(line)
            idle_deadline = now + self.idle_timeout_s

        if not response_lines:
            raise TimeoutError(
                f"No agent response received within {self.response_timeout_s:.1f}s"
            )
        return response_lines


class MockAgentBridge:
    """Mock responder for testing and UI development without hardware."""

    def __init__(self, latency_s: float) -> None:
        self.latency_s = max(0.0, latency_s)

    def open(self) -> None:
        return

    def close(self) -> None:
        return

    def ask(self, prompt: str) -> str:
        message = prompt.strip()
        if not message:
            raise ValueError("Message is empty")

        if self.latency_s > 0:
            time.sleep(self.latency_s)

        lower = message.lower()
        if lower in {"ping", "/ping"}:
            return "pong"
        if lower in {"status", "/status", "health", "/health"}:
            return (
                "mock-agent online\n"
                "mode: web relay test mode\n"
                "device: simulated\n"
                "latency: low"
            )
        return (
            f"[mock-agent] Received: {message}\n"
            "This response is generated by the host relay (no ESP32 required)."
        )


def create_agent_bridge(args: argparse.Namespace) -> tuple[AgentBridge, str]:
    if args.mock_agent:
        return MockAgentBridge(latency_s=args.mock_latency), "mock-agent"

    port = resolve_serial_port(args.serial_port)
    bridge = SerialAgentBridge(
        port=port,
        baudrate=args.baud,
        serial_timeout_s=args.serial_timeout,
        response_timeout_s=args.response_timeout,
        idle_timeout_s=args.idle_timeout,
        log_serial=args.log_serial,
    )
    return bridge, port


def make_handler(state: AppState):
    class RelayHandler(BaseHTTPRequestHandler):
        server_version = "zclaw-web-relay/1.0"

        def log_message(self, fmt: str, *args) -> None:  # pragma: no cover - stdlib logging
            logging.info("%s - %s", self.address_string(), fmt % args)

        def do_OPTIONS(self) -> None:  # noqa: N802
            self.send_response(HTTPStatus.NO_CONTENT)
            self._set_common_headers("text/plain; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type,X-Zclaw-Key")
            self.end_headers()

        def do_GET(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            if parsed.path == "/":
                self._send_html(INDEX_HTML)
                return
            if parsed.path == "/health":
                self._send_json(
                    HTTPStatus.OK,
                    {
                        "ok": True,
                        "bridge_target": state.bridge_target,
                        "mode": "mock" if state.bridge_target == "mock-agent" else "serial",
                    },
                )
                return
            if parsed.path == "/api/config":
                self._send_json(
                    HTTPStatus.OK,
                    {
                        "api_key_required": state.api_key is not None,
                        "bridge_target": state.bridge_target,
                    },
                )
                return
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "Not found"})

        def do_POST(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            if parsed.path != "/api/chat":
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "Not found"})
                return

            provided_key = normalize_api_key(self.headers.get("X-Zclaw-Key"))
            if not is_request_authorized(provided_key, state.api_key):
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "Unauthorized"})
                return

            payload = self._read_json()
            if payload is None:
                return

            raw_message = payload.get("message")
            if not isinstance(raw_message, str):
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "message must be a string"})
                return

            message = raw_message.strip()
            if not message:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "message is empty"})
                return
            if len(message) > MAX_CHAT_MESSAGE_LEN:
                self._send_json(
                    HTTPStatus.BAD_REQUEST,
                    {"error": f"message exceeds {MAX_CHAT_MESSAGE_LEN} characters"},
                )
                return

            started = time.monotonic()
            try:
                reply = state.bridge.ask(message)
            except TimeoutError as exc:
                self._send_json(HTTPStatus.GATEWAY_TIMEOUT, {"error": str(exc)})
                return
            except RuntimeError as exc:
                logging.warning("relay bridge unavailable: %s", exc)
                self._send_json(HTTPStatus.SERVICE_UNAVAILABLE, {"error": str(exc)})
                return
            except Exception as exc:
                logging.exception("relay chat failed")
                self._send_json(HTTPStatus.BAD_GATEWAY, {"error": f"Bridge error: {exc}"})
                return

            elapsed_ms = int((time.monotonic() - started) * 1000)
            self._send_json(
                HTTPStatus.OK,
                {
                    "reply": reply,
                    "bridge_target": state.bridge_target,
                    "elapsed_ms": elapsed_ms,
                },
            )

        def _read_json(self) -> dict | None:
            length_header = self.headers.get("Content-Length")
            if not length_header:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "Content-Length required"})
                return None

            try:
                length = int(length_header)
            except ValueError:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "Invalid Content-Length"})
                return None

            if length <= 0 or length > 1024 * 1024:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "Invalid body size"})
                return None

            raw = self.rfile.read(length)
            try:
                payload = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "Invalid JSON body"})
                return None

            if not isinstance(payload, dict):
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "JSON body must be an object"})
                return None
            return payload

        def _set_common_headers(self, content_type: str) -> None:
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")

        def _send_json(self, status: HTTPStatus, payload: dict) -> None:
            encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            self.send_response(status.value)
            self._set_common_headers("application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

        def _send_html(self, html: str) -> None:
            encoded = html.encode("utf-8")
            self.send_response(HTTPStatus.OK.value)
            self._set_common_headers("text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

    return RelayHandler


INDEX_HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>zclaw Relay</title>
  <style>
    :root {
      --bg: #0b0f14;
      --panel: rgba(11, 18, 26, 0.88);
      --panel-2: rgba(16, 26, 39, 0.9);
      --text: #d7e0ea;
      --muted: #91a2b3;
      --accent: #23c483;
      --accent-2: #58d3f7;
      --danger: #ff7676;
      --bubble-user: #1a6f78;
      --bubble-agent: rgba(41, 57, 73, 0.72);
      --shadow: 0 12px 36px rgba(0, 0, 0, 0.35);
      --radius: 16px;
    }

    * { box-sizing: border-box; }

    html, body {
      margin: 0;
      min-height: 100%;
      font-family: "IBM Plex Sans", "Avenir Next", "Segoe UI", sans-serif;
      background:
        radial-gradient(1300px 900px at 10% -10%, rgba(88, 211, 247, 0.07), transparent 55%),
        radial-gradient(1100px 800px at 100% 0%, rgba(35, 196, 131, 0.05), transparent 50%),
        var(--bg);
      color: var(--text);
    }

    .shell {
      max-width: 900px;
      margin: 0 auto;
      min-height: 100vh;
      padding: 18px 14px 24px;
      display: grid;
      gap: 12px;
      grid-template-rows: auto 1fr auto;
    }

    .header {
      border-radius: var(--radius);
      background: var(--panel);
      border: 1px solid rgba(145, 162, 179, 0.18);
      box-shadow: var(--shadow);
      padding: 14px 14px 12px;
      backdrop-filter: blur(6px);
    }

    .title {
      margin: 0;
      font-family: "IBM Plex Mono", "SFMono-Regular", "Consolas", monospace;
      font-size: 1.08rem;
      letter-spacing: 0.03em;
      text-transform: uppercase;
    }

    .status-row {
      margin-top: 8px;
      display: flex;
      align-items: center;
      gap: 10px;
      flex-wrap: wrap;
    }

    .status-pill {
      border-radius: 999px;
      font-size: 0.75rem;
      letter-spacing: 0.04em;
      text-transform: uppercase;
      padding: 4px 10px;
      background: rgba(35, 196, 131, 0.14);
      color: #84e9c2;
      border: 1px solid rgba(35, 196, 131, 0.5);
    }

    .status-pill.warn {
      background: rgba(255, 118, 118, 0.12);
      color: #ffc2c2;
      border-color: rgba(255, 118, 118, 0.45);
    }

    .muted { color: var(--muted); font-size: 0.88rem; }

    .chat {
      border-radius: var(--radius);
      background: var(--panel);
      border: 1px solid rgba(145, 162, 179, 0.18);
      box-shadow: var(--shadow);
      padding: 14px;
      overflow: hidden;
      display: grid;
      grid-template-rows: 1fr;
      min-height: 48vh;
    }

    .timeline {
      overflow-y: auto;
      display: grid;
      gap: 10px;
      align-content: start;
      padding-right: 4px;
      overscroll-behavior: contain;
    }

    .bubble {
      max-width: min(92%, 700px);
      border-radius: 14px;
      padding: 10px 12px;
      white-space: pre-wrap;
      line-height: 1.45;
      animation: enter 160ms ease;
      animation-iteration-count: 1;
      animation-fill-mode: both;
    }

    .bubble.user {
      justify-self: end;
      background: var(--bubble-user);
      color: #f7fbff;
    }

    .bubble.agent {
      justify-self: start;
      background: var(--bubble-agent);
      border: 1px solid rgba(145, 162, 179, 0.25);
      white-space: normal;
    }

    .bubble.agent p {
      margin: 0;
    }

    .bubble.agent p + p {
      margin-top: 0.55em;
    }

    .bubble.agent code {
      font-family: "IBM Plex Mono", "SFMono-Regular", "Consolas", monospace;
      font-size: 0.94em;
      background: rgba(5, 11, 19, 0.72);
      border: 1px solid rgba(145, 162, 179, 0.24);
      border-radius: 8px;
      padding: 0.08em 0.35em;
    }

    .bubble.agent pre {
      margin: 0.6em 0 0;
      background: rgba(5, 11, 19, 0.78);
      border: 1px solid rgba(145, 162, 179, 0.24);
      border-radius: 10px;
      padding: 8px 10px;
      overflow-x: auto;
    }

    .bubble.agent pre code {
      border: 0;
      background: transparent;
      padding: 0;
      border-radius: 0;
      display: block;
      white-space: pre;
    }

    .bubble.agent a {
      color: #9fe9ff;
    }

    .bubble.error {
      justify-self: start;
      background: rgba(121, 39, 39, 0.44);
      border: 1px solid rgba(255, 118, 118, 0.45);
      color: #ffd4d4;
    }

    .composer {
      border-radius: var(--radius);
      background: var(--panel-2);
      border: 1px solid rgba(145, 162, 179, 0.18);
      box-shadow: var(--shadow);
      padding: 12px;
      display: grid;
      gap: 10px;
    }

    .controls {
      display: grid;
      gap: 10px;
      grid-template-columns: 1fr auto;
      align-items: end;
    }

    textarea, input[type="password"] {
      width: 100%;
      border: 1px solid rgba(145, 162, 179, 0.36);
      background: rgba(11, 16, 24, 0.85);
      color: var(--text);
      border-radius: 12px;
      padding: 10px 11px;
      font: inherit;
    }

    textarea {
      min-height: 64px;
      max-height: 180px;
      resize: vertical;
      line-height: 1.4;
    }

    button {
      border: 0;
      border-radius: 12px;
      cursor: pointer;
      background: #42b9c3;
      color: #04121a;
      font-weight: 700;
      padding: 10px 14px;
      min-height: 42px;
      min-width: 92px;
    }

    button:disabled {
      opacity: 0.55;
      cursor: not-allowed;
    }

    .key-row {
      display: grid;
      gap: 8px;
    }

    .tip {
      font-size: 0.8rem;
      color: var(--muted);
    }

    @keyframes enter {
      from { opacity: 0; transform: translateY(4px); }
      to { opacity: 1; transform: translateY(0); }
    }

    @media (prefers-reduced-motion: reduce) {
      *,
      *::before,
      *::after {
        animation: none !important;
        transition: none !important;
      }
    }

    @media (max-width: 640px) {
      .shell {
        padding: 10px 8px 14px;
      }
      .controls {
        grid-template-columns: 1fr;
      }
      button {
        width: 100%;
      }
      .bubble {
        max-width: 100%;
      }
    }
  </style>
</head>
<body>
  <div class="shell">
    <header class="header">
      <h1 class="title">zclaw relay</h1>
      <div class="status-row">
        <span id="statusPill" class="status-pill">connecting</span>
        <span class="muted" id="statusText">Checking relay health...</span>
      </div>
    </header>

    <main class="chat">
      <section id="timeline" class="timeline"></section>
    </main>

    <footer class="composer">
      <div class="key-row">
        <input id="apiKey" type="password" placeholder="API key (if required)" autocomplete="off" />
        <div class="tip">Saved locally in this browser only. Leave empty if relay does not require key.</div>
      </div>
      <div class="controls">
        <textarea id="messageInput" placeholder="Message your device..." maxlength="4096"></textarea>
        <button id="sendButton" type="button">Send</button>
      </div>
      <div class="tip">Press Enter to send. Shift+Enter for a new line.</div>
    </footer>
  </div>

  <script>
    const timeline = document.getElementById("timeline");
    const messageInput = document.getElementById("messageInput");
    const sendButton = document.getElementById("sendButton");
    const apiKeyInput = document.getElementById("apiKey");
    const statusPill = document.getElementById("statusPill");
    const statusText = document.getElementById("statusText");

    const KEY_STORAGE = "zclaw_web_api_key";
    apiKeyInput.value = localStorage.getItem(KEY_STORAGE) || "";

    function escapeHtml(text) {
      return text
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#39;");
    }

    function renderMarkdown(text) {
      const source = (typeof text === "string") ? text : "";
      const placeholders = [];
      let html = escapeHtml(source);

      html = html.replace(/```([\\s\\S]*?)```/g, (_, block) => {
        const token = `@@BLOCK_${placeholders.length}@@`;
        const normalized = block.replace(/^\\n/, "").replace(/\\n$/, "");
        placeholders.push(`<pre><code>${normalized}</code></pre>`);
        return token;
      });

      html = html.replace(
        /\\[([^\\]]+)\\]\\((https?:\\/\\/[^)\\s]+)\\)/g,
        '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>',
      );
      html = html.replace(/\\*\\*(.+?)\\*\\*/g, "<strong>$1</strong>");
      html = html.replace(/\\*(.+?)\\*/g, "<em>$1</em>");
      html = html.replace(/`([^`]+)`/g, "<code>$1</code>");

      const paragraphs = html
        .split(/\\n{2,}/)
        .map((chunk) => {
          const trimmed = chunk.trim();
          if (/^@@BLOCK_\\d+@@$/.test(trimmed)) {
            return trimmed;
          }
          return `<p>${chunk.replace(/\\n/g, "<br>")}</p>`;
        })
        .join("");

      html = paragraphs;
      for (let i = 0; i < placeholders.length; i++) {
        html = html.replace(`@@BLOCK_${i}@@`, placeholders[i]);
      }
      return html;
    }

    function setBubbleContent(node, kind, text) {
      if (kind === "agent") {
        node.innerHTML = renderMarkdown(text);
      } else {
        node.textContent = text;
      }
    }

    function updateBubble(node, kind, text) {
      node.className = `bubble ${kind}`;
      setBubbleContent(node, kind, text);
    }

    function appendBubble(kind, text) {
      const node = document.createElement("div");
      updateBubble(node, kind, text);
      timeline.appendChild(node);
      timeline.scrollTop = timeline.scrollHeight;
      return node;
    }

    function setStatus(ok, text) {
      statusPill.textContent = ok ? "online" : "offline";
      statusPill.classList.toggle("warn", !ok);
      statusText.textContent = text;
    }

    async function fetchConfig() {
      try {
        const res = await fetch("/api/config", { cache: "no-store" });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const cfg = await res.json();
        setStatus(true, `Relay ready (${cfg.bridge_target})`);
      } catch (err) {
        setStatus(false, `Relay unreachable: ${err.message}`);
      }
    }

    async function sendMessage() {
      const message = messageInput.value.trim();
      if (!message || sendButton.disabled) return;

      const key = apiKeyInput.value.trim();
      localStorage.setItem(KEY_STORAGE, key);

      appendBubble("user", message);
      messageInput.value = "";
      sendButton.disabled = true;
      const pending = appendBubble("agent", "...");

      try {
        const headers = { "Content-Type": "application/json" };
        if (key) headers["X-Zclaw-Key"] = key;
        const res = await fetch("/api/chat", {
          method: "POST",
          headers,
          body: JSON.stringify({ message }),
        });
        const payload = await res.json();
        if (!res.ok) {
          updateBubble(pending, "error", payload.error || `Request failed (${res.status})`);
        } else {
          updateBubble(pending, "agent", payload.reply || "(empty reply)");
        }
      } catch (err) {
        updateBubble(pending, "error", `Network error: ${err.message}`);
      } finally {
        sendButton.disabled = false;
        messageInput.focus();
      }
    }

    sendButton.addEventListener("click", sendMessage);
    messageInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        sendMessage();
      }
    });

    fetchConfig();
    appendBubble("agent", "Relay connected. Send a message to start.");
  </script>
</body>
</html>
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="zclaw hosted web relay")
    parser.add_argument("--host", default="0.0.0.0", help="HTTP listen host (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8787, help="HTTP listen port (default: 8787)")
    parser.add_argument("--serial-port", default=None, help="Serial port for device mode")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    parser.add_argument(
        "--serial-timeout",
        type=float,
        default=0.15,
        help="Serial readline timeout in seconds (default: 0.15)",
    )
    parser.add_argument(
        "--response-timeout",
        type=float,
        default=90.0,
        help="Max wait for device response in seconds (default: 90)",
    )
    parser.add_argument(
        "--idle-timeout",
        type=float,
        default=1.2,
        help="Stop collecting response after this idle time (default: 1.2s)",
    )
    parser.add_argument(
        "--mock-agent",
        action="store_true",
        help="Use built-in mock responder (no hardware required)",
    )
    parser.add_argument(
        "--mock-latency",
        type=float,
        default=0.25,
        help="Mock response latency in seconds (default: 0.25)",
    )
    parser.add_argument(
        "--log-serial",
        action="store_true",
        help="Log serial traffic at INFO level",
    )
    parser.add_argument(
        "--log-file",
        default=None,
        help="Optional path for an additional log file sink",
    )
    parser.add_argument("--debug", action="store_true", help="Enable debug logging")
    return parser.parse_args()


def run_server(args: argparse.Namespace) -> int:
    api_key = normalize_api_key(os.environ.get("ZCLAW_WEB_API_KEY"))
    bridge, bridge_target = create_agent_bridge(args)
    bridge.open()

    state = AppState(bridge=bridge, bridge_target=bridge_target, api_key=api_key)
    handler = make_handler(state)
    httpd = ThreadingHTTPServer((args.host, args.port), handler)

    logging.info(
        "Web relay listening on http://%s:%d (bridge=%s, api_key=%s)",
        args.host,
        args.port,
        bridge_target,
        "set" if api_key else "unset",
    )

    try:
        httpd.serve_forever()
    finally:
        bridge.close()
        httpd.server_close()
    return 0


def configure_logging(args: argparse.Namespace) -> None:
    handlers: list[logging.Handler] = [logging.StreamHandler()]
    if args.log_file:
        handlers.append(logging.FileHandler(args.log_file, encoding="utf-8"))

    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=handlers,
    )


def main() -> int:
    args = parse_args()
    configure_logging(args)

    try:
        return run_server(args)
    except KeyboardInterrupt:
        logging.info("Interrupted, shutting down.")
        return 0
    except Exception as exc:
        logging.error("%s", exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
