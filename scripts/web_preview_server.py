#!/usr/bin/env python3
"""Local preview server for the zclaw web setup pages."""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs


REPO_ROOT = Path(__file__).resolve().parent.parent
SETUP_HTML_PATH = REPO_ROOT / "main" / "web" / "setup.html"
SUCCESS_HTML_PATH = REPO_ROOT / "main" / "web" / "success.html"
WATCHED_HTML_PATHS = (SETUP_HTML_PATH, SUCCESS_HTML_PATH)


def load_html(path: Path) -> bytes:
    return path.read_bytes()


def compute_reload_version(paths: tuple[Path, ...]) -> str:
    hasher = hashlib.sha1()
    for path in sorted(paths, key=lambda item: str(item)):
        hasher.update(str(path).encode("utf-8"))
        try:
            stat = path.stat()
            hasher.update(str(stat.st_mtime_ns).encode("ascii"))
            hasher.update(str(stat.st_size).encode("ascii"))
        except FileNotFoundError:
            hasher.update(b"missing")
    return hasher.hexdigest()[:12]


def inject_live_reload_script(html: bytes, version: str, poll_ms: int) -> bytes:
    snippet = (
        "<script>\n"
        "(function(){\n"
        f"  let version = {json.dumps(version)};\n"
        "  async function checkForChanges() {\n"
        "    try {\n"
        "      const response = await fetch('/__reload?ts=' + Date.now(), { cache: 'no-store' });\n"
        "      if (!response.ok) {\n"
        "        return;\n"
        "      }\n"
        "      const payload = await response.json();\n"
        "      if (payload && typeof payload.version === 'string' && payload.version !== version) {\n"
        "        window.location.reload();\n"
        "      }\n"
        "    } catch (_err) {\n"
        "      /* ignore transient preview polling errors */\n"
        "    }\n"
        "  }\n"
        f"  setInterval(checkForChanges, {poll_ms});\n"
        "})();\n"
        "</script>\n"
    ).encode("utf-8")

    marker = b"</body>"
    lower_html = html.lower()
    insert_at = lower_html.rfind(marker)
    if insert_at < 0:
        return html + snippet
    return html[:insert_at] + snippet + html[insert_at:]


class PreviewHandler(BaseHTTPRequestHandler):
    reload_enabled = True
    reload_poll_ms = 1000
    watched_paths = WATCHED_HTML_PATHS
    mock_ssids = [
        "Home-2.4G",
        "Home-5G",
        "Office-WiFi",
        "Guest-Network",
    ]

    def _load_html_page(self, path: Path) -> bytes | None:
        try:
            body = load_html(path)
        except OSError as exc:
            self.send_error(500, f"Failed to read {path.name}: {exc}")
            return None

        if self.reload_enabled:
            version = compute_reload_version(self.watched_paths)
            body = inject_live_reload_script(body, version, self.reload_poll_ms)
        return body

    def _send_html(self, body: bytes, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, body: bytes, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 (BaseHTTPRequestHandler API)
        if self.path == "/" or self.path.startswith("/?"):
            setup_html = self._load_html_page(SETUP_HTML_PATH)
            if setup_html is not None:
                self._send_html(setup_html)
            return

        if self.path == "/networks" or self.path.startswith("/networks?"):
            payload = {
                "ssids": self.mock_ssids,
                "count": len(self.mock_ssids),
            }
            self._send_json(json.dumps(payload).encode("utf-8"))
            return

        if self.path == "/__reload" or self.path.startswith("/__reload?"):
            if not self.reload_enabled:
                self.send_error(404)
                return
            payload = {"version": compute_reload_version(self.watched_paths)}
            self._send_json(json.dumps(payload).encode("utf-8"))
            return

        if self.path == "/success":
            success_html = self._load_html_page(SUCCESS_HTML_PATH)
            if success_html is not None:
                self._send_html(success_html)
            return

        self.send_response(302)
        self.send_header("Location", "/")
        self.end_headers()

    def do_POST(self) -> None:  # noqa: N802 (BaseHTTPRequestHandler API)
        if self.path != "/save":
            self.send_error(404)
            return

        content_length = int(self.headers.get("Content-Length", "0"))
        payload = self.rfile.read(content_length).decode("utf-8", errors="replace")
        fields = parse_qs(payload, keep_blank_values=True)

        ssid = fields.get("ssid", [""])[0]
        backend = fields.get("backend", [""])[0]
        print(
            f"[web-preview] Save request received: ssid='{ssid}' backend='{backend}' "
            "(not persisted in preview mode)"
        )

        success_html = self._load_html_page(SUCCESS_HTML_PATH)
        if success_html is not None:
            self._send_html(success_html)

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[web-preview] {self.address_string()} - {fmt % args}")


def bind_server(host: str, requested_port: int, max_attempts: int = 10) -> tuple[HTTPServer, int]:
    for offset in range(max_attempts):
        port = requested_port + offset
        try:
            return HTTPServer((host, port), PreviewHandler), port
        except OSError as exc:
            if exc.errno == errno.EADDRINUSE and offset < max_attempts - 1:
                continue
            raise
    raise RuntimeError("Unable to bind preview server")


def main() -> None:
    parser = argparse.ArgumentParser(description="zclaw web setup preview server")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="Bind port (default: 8080)")
    parser.add_argument(
        "--reload",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Enable browser live reload when HTML files change (default: enabled)",
    )
    parser.add_argument(
        "--reload-interval-ms",
        type=int,
        default=1000,
        help="Live reload polling interval in milliseconds (default: 1000)",
    )
    args = parser.parse_args()

    PreviewHandler.reload_enabled = args.reload
    PreviewHandler.reload_poll_ms = max(args.reload_interval_ms, 250)
    PreviewHandler.watched_paths = WATCHED_HTML_PATHS

    server, bound_port = bind_server(args.host, args.port)
    if bound_port != args.port:
        print(f"[web-preview] Port {args.port} in use, switched to {bound_port}")
    print(f"[web-preview] Serving setup UI on http://{args.host}:{bound_port}")
    print("[web-preview] POST /save returns success page but does not persist settings.")
    if args.reload:
        print(
            "[web-preview] Live reload enabled "
            f"({PreviewHandler.reload_poll_ms}ms polling)."
        )
    else:
        print("[web-preview] Live reload disabled.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[web-preview] Stopping.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
