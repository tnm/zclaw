#!/usr/bin/env python3
"""Run QEMU and proxy emulator LLM requests to Anthropic, OpenAI, or Azure OpenAI from host."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import threading
import time
import tty
import termios
import urllib.error
import urllib.request


REQ_PREFIX = "__zclaw_llm_req__:"
RESP_PREFIX = "__zclaw_llm_resp__:"
ANTHROPIC_API_URL = "https://api.anthropic.com/v1/messages"
OPENAI_API_URL = "https://api.openai.com/v1/chat/completions"
REQ_PREFIX_B = REQ_PREFIX.encode("utf-8")
RESP_PREFIX_B = RESP_PREFIX.encode("utf-8")


def build_error_payload(message: str) -> str:
    return json.dumps(
        {"error": {"message": f"Host bridge error: {message}"}}, separators=(",", ":")
    )


def write_host_line(stream, message: str) -> None:
    stream.write(f"{message}\r\n")
    stream.flush()


def compact_json_or_error(raw_json: str) -> str:
    try:
        parsed = json.loads(raw_json)
    except json.JSONDecodeError:
        return build_error_payload("Provider returned non-JSON response")
    return json.dumps(parsed, separators=(",", ":"))


def call_anthropic(request_json: str, timeout_s: int) -> str:
    api_key = os.environ.get("ANTHROPIC_API_KEY", "")
    if not api_key:
        return build_error_payload("ANTHROPIC_API_KEY is not set")

    req = urllib.request.Request(
        ANTHROPIC_API_URL,
        data=request_json.encode("utf-8"),
        headers={
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return compact_json_or_error(body)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        if body:
            return compact_json_or_error(body)
        return build_error_payload(f"HTTP {exc.code}")
    except Exception as exc:  # pragma: no cover - network/runtime dependent
        return build_error_payload(str(exc))


def call_openai(request_json: str, timeout_s: int) -> str:
    api_key = os.environ.get("OPENAI_API_KEY", "")
    if not api_key:
        return build_error_payload("OPENAI_API_KEY is not set")

    api_url = os.environ.get("OPENAI_API_URL", OPENAI_API_URL)
    req = urllib.request.Request(
        api_url,
        data=request_json.encode("utf-8"),
        headers={
            "authorization": f"Bearer {api_key}",
            "content-type": "application/json",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return compact_json_or_error(body)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        if body:
            return compact_json_or_error(body)
        return build_error_payload(f"HTTP {exc.code}")
    except Exception as exc:  # pragma: no cover - network/runtime dependent
        return build_error_payload(str(exc))


def call_azure_openai(request_json: str, timeout_s: int) -> str:
    api_key = os.environ.get("AZURE_OPENAI_API_KEY", "")
    if not api_key:
        return build_error_payload("AZURE_OPENAI_API_KEY is not set")

    api_url = os.environ.get("AZURE_OPENAI_API_URL", "")
    if not api_url:
        return build_error_payload("AZURE_OPENAI_API_URL is not set")

    req = urllib.request.Request(
        api_url,
        data=request_json.encode("utf-8"),
        headers={
            "api-key": api_key,
            "content-type": "application/json",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return compact_json_or_error(body)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        if body:
            return compact_json_or_error(body)
        return build_error_payload(f"HTTP {exc.code}")
    except Exception as exc:  # pragma: no cover - network/runtime dependent
        return build_error_payload(str(exc))


def detect_provider_from_request(request_json: str) -> str:
    try:
        payload = json.loads(request_json)
    except json.JSONDecodeError:
        return "openai"

    if not isinstance(payload, dict):
        return "openai"

    # zclaw currently uses Responses-shaped payloads only for Azure OpenAI.
    if "input" in payload or "instructions" in payload or "max_output_tokens" in payload:
        return "azure-openai"

    tools = payload.get("tools")
    if isinstance(tools, list) and tools:
        first_tool = tools[0]
        if isinstance(first_tool, dict):
            if "input_schema" in first_tool:
                return "anthropic"
            if first_tool.get("type") == "function" or "function" in first_tool:
                return "openai"

    if "system" in payload:
        return "anthropic"

    messages = payload.get("messages")
    if isinstance(messages, list):
        for msg in messages:
            if isinstance(msg, dict) and msg.get("role") == "system":
                return "openai"

    return "openai"


def resolve_provider(provider: str, request_json: str) -> str:
    if provider == "auto":
        return detect_provider_from_request(request_json)
    return provider


def call_provider(provider: str, request_json: str, timeout_s: int) -> str:
    if provider == "openai":
        return call_openai(request_json, timeout_s)
    if provider == "azure-openai":
        return call_azure_openai(request_json, timeout_s)
    return call_anthropic(request_json, timeout_s)


class RawStdin:
    def __init__(self) -> None:
        self._fd: int | None = None
        self._attrs: list | None = None

    def __enter__(self) -> "RawStdin":
        if sys.stdin.isatty():
            self._fd = sys.stdin.fileno()
            self._attrs = termios.tcgetattr(self._fd)
            tty.setraw(self._fd)
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self._fd is not None and self._attrs is not None:
            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._attrs)


class QemuLiveBridge:
    def __init__(
        self,
        qemu_cmd: list[str],
        provider: str,
        api_timeout_s: int,
        log_requests: bool,
    ) -> None:
        self.qemu_cmd = qemu_cmd
        self.provider = provider
        self.api_timeout_s = api_timeout_s
        self.log_requests = log_requests
        self.proc: subprocess.Popen[bytes] | None = None
        self._lock = threading.Lock()
        self._pending_request = False
        self._buffered_input = bytearray()
        self._stop = False

    def _write_qemu(self, data: bytes) -> None:
        if not self.proc or not self.proc.stdin:
            return
        try:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
        except BrokenPipeError:
            self._stop = True

    def _set_pending(self, pending: bool) -> None:
        flush_bytes = b""
        with self._lock:
            self._pending_request = pending
            if not pending and self._buffered_input:
                flush_bytes = bytes(self._buffered_input)
                self._buffered_input.clear()
        if flush_bytes:
            self._write_qemu(flush_bytes)

    def _stdin_pump(self) -> None:
        fd = sys.stdin.fileno()
        while not self._stop:
            try:
                data = os.read(fd, 1)
            except OSError:
                return
            if not data:
                return
            with self._lock:
                pending = self._pending_request
                if pending:
                    self._buffered_input.extend(data)
            if not pending:
                self._write_qemu(data)

    def _handle_request_line(self, request_json: str) -> None:
        provider = resolve_provider(self.provider, request_json)
        started = time.monotonic()
        if self.log_requests:
            write_host_line(
                sys.stderr,
                f"[qemu-live-llm] Forwarding request ({len(request_json)} bytes) via {provider}",
            )
        self._set_pending(True)
        response_json = call_provider(provider, request_json, self.api_timeout_s)
        self._write_qemu((RESP_PREFIX + response_json + "\n").encode("utf-8"))
        self._set_pending(False)
        if self.log_requests:
            elapsed = time.monotonic() - started
            write_host_line(
                sys.stderr,
                f"[qemu-live-llm] Returned {provider} response ({len(response_json)} bytes) "
                f"in {elapsed:.1f}s",
            )

    def run(self) -> int:
        self.proc = subprocess.Popen(
            self.qemu_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )

        stdin_thread = threading.Thread(target=self._stdin_pump, daemon=True)
        stdin_thread.start()

        assert self.proc.stdout is not None
        read_fd = self.proc.stdout.fileno()
        mode = "detect"  # detect | passthrough | suppress_req | suppress_resp
        prefix_buf = bytearray()
        req_payload = bytearray()

        while True:
            chunk = os.read(read_fd, 1024)
            if not chunk:
                break
            for b in chunk:
                if mode == "passthrough":
                    os.write(sys.stdout.fileno(), bytes((b,)))
                    if b == 0x0A:
                        mode = "detect"
                    continue

                if mode == "suppress_resp":
                    if b == 0x0A:
                        mode = "detect"
                    continue

                if mode == "suppress_req":
                    if b == 0x0D:
                        continue
                    if b == 0x0A:
                        request_json = req_payload.decode("utf-8", errors="replace")
                        req_payload.clear()
                        mode = "detect"
                        self._handle_request_line(request_json)
                    else:
                        req_payload.append(b)
                    continue

                # mode == detect
                prefix_buf.append(b)
                candidate = bytes(prefix_buf)

                if candidate == REQ_PREFIX_B:
                    prefix_buf.clear()
                    mode = "suppress_req"
                    continue
                if candidate == RESP_PREFIX_B:
                    prefix_buf.clear()
                    mode = "suppress_resp"
                    continue
                if REQ_PREFIX_B.startswith(candidate) or RESP_PREFIX_B.startswith(
                    candidate
                ):
                    continue

                os.write(sys.stdout.fileno(), candidate)
                prefix_buf.clear()
                if b == 0x0A:
                    mode = "detect"
                else:
                    mode = "passthrough"

        if mode == "detect" and prefix_buf:
            os.write(sys.stdout.fileno(), bytes(prefix_buf))
        elif mode == "suppress_req" and req_payload:
            request_json = req_payload.decode("utf-8", errors="replace")
            self._handle_request_line(request_json)

        self._stop = True
        stdin_thread.join(timeout=0.2)
        return self.proc.wait()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="QEMU live LLM bridge for zclaw emulator"
    )
    parser.add_argument(
        "--provider",
        choices=("auto", "anthropic", "openai", "azure-openai"),
        default="auto",
        help="Host API provider: auto-detect from request format (default), anthropic, openai, or azure-openai",
    )
    parser.add_argument(
        "--api-timeout",
        type=int,
        default=50,
        help="Timeout in seconds for provider API requests (default: 50)",
    )
    parser.add_argument(
        "--bridge-logs",
        action="store_true",
        help="Print per-request bridge forwarding/timing logs",
    )
    parser.add_argument(
        "--anthropic-timeout",
        type=int,
        default=None,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "qemu_cmd",
        nargs=argparse.REMAINDER,
        help="QEMU command to run, passed after --",
    )
    args = parser.parse_args()
    if args.anthropic_timeout is not None and args.api_timeout == 50:
        args.api_timeout = args.anthropic_timeout

    if args.qemu_cmd and args.qemu_cmd[0] == "--":
        args.qemu_cmd = args.qemu_cmd[1:]

    if not args.qemu_cmd:
        parser.error("Missing QEMU command (pass it after --)")

    return args


def main() -> int:
    args = parse_args()
    if args.provider == "auto":
        provider_note = "auto-detect (anthropic/openai)"
    else:
        provider_note = args.provider
    write_host_line(
        sys.stdout, f"[qemu-live-llm] Bridge active (provider: {provider_note})."
    )
    write_host_line(sys.stdout, "[qemu-live-llm] Press Ctrl+A then X to exit QEMU.")
    bridge = QemuLiveBridge(
        args.qemu_cmd, args.provider, args.api_timeout, args.bridge_logs
    )
    with RawStdin():
        return bridge.run()


if __name__ == "__main__":
    raise SystemExit(main())
