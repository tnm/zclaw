#!/usr/bin/env python3
"""Unit tests for web_relay helpers."""

from __future__ import annotations

import base64
import http.client
import sys
import threading
import unittest
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "scripts"))

from web_relay import (  # noqa: E402
    AppState,
    MockAgentBridge,
    SerialAgentBridge,
    VOICE_STT_RESP_PREFIX,
    build_multipart_form_data,
    canonical_origin,
    create_agent_bridge,
    describe_serial_exception,
    is_cors_origin_allowed,
    is_json_content_type,
    is_loopback_host,
    is_post_origin_allowed,
    is_probable_serial_exception,
    is_probable_esp_log_line,
    is_request_authorized,
    make_handler,
    normalize_api_key,
    normalize_origin,
    pcm16le_to_wav_bytes,
    resolve_serial_port,
    sanitize_voice_payload,
    transcribe_voice_pcm,
    validate_bind_security,
)


class WebRelayTests(unittest.TestCase):
    def test_sanitize_voice_payload(self) -> None:
        self.assertEqual(sanitize_voice_payload("  hello\r\nworld  "), "hello  world")
        self.assertEqual(sanitize_voice_payload(""), "")

    def test_pcm16le_to_wav_bytes(self) -> None:
        pcm = b"\x01\x00\x02\x00\x03\x00\x04\x00"
        wav = pcm16le_to_wav_bytes(pcm, sample_rate_hz=16000)
        self.assertTrue(wav.startswith(b"RIFF"))
        self.assertIn(b"WAVE", wav[:16])

    def test_build_multipart_form_data(self) -> None:
        content_type, body = build_multipart_form_data(
            fields={"model": "whisper-1"},
            file_field="file",
            filename="speech.wav",
            content_type="audio/wav",
            file_bytes=b"abc",
        )
        self.assertIn("multipart/form-data; boundary=", content_type)
        self.assertIn(b'name="model"', body)
        self.assertIn(b'name="file"; filename="speech.wav"', body)

    def test_normalize_api_key(self) -> None:
        self.assertIsNone(normalize_api_key(None))
        self.assertIsNone(normalize_api_key("   "))
        self.assertEqual(normalize_api_key("  abc  "), "abc")

    def test_is_request_authorized(self) -> None:
        self.assertTrue(is_request_authorized(None, None))
        self.assertFalse(is_request_authorized(None, "secret"))
        self.assertFalse(is_request_authorized("bad", "secret"))
        self.assertTrue(is_request_authorized("secret", "secret"))

    def test_normalize_origin(self) -> None:
        self.assertIsNone(normalize_origin(None))
        self.assertIsNone(normalize_origin("   "))
        self.assertEqual(normalize_origin("  http://localhost:5173 "), "http://localhost:5173")

    def test_canonical_origin(self) -> None:
        self.assertEqual(canonical_origin(" HTTP://LOCALHOST:8787 "), "http://localhost:8787")
        self.assertEqual(canonical_origin("https://Example.com"), "https://example.com")
        self.assertIsNone(canonical_origin("localhost:8787"))
        self.assertIsNone(canonical_origin("   "))

    def test_is_loopback_host(self) -> None:
        self.assertTrue(is_loopback_host("127.0.0.1"))
        self.assertTrue(is_loopback_host("localhost"))
        self.assertTrue(is_loopback_host("::1"))
        self.assertFalse(is_loopback_host("0.0.0.0"))
        self.assertFalse(is_loopback_host("192.168.1.2"))

    def test_is_json_content_type(self) -> None:
        self.assertTrue(is_json_content_type("application/json"))
        self.assertTrue(is_json_content_type("application/json; charset=utf-8"))
        self.assertFalse(is_json_content_type(None))
        self.assertFalse(is_json_content_type("text/plain"))
        self.assertFalse(is_json_content_type("application/x-www-form-urlencoded"))

    def test_post_origin_policy_without_cors(self) -> None:
        self.assertTrue(is_post_origin_allowed(None, "127.0.0.1:8787", None))
        self.assertTrue(
            is_post_origin_allowed(
                "http://127.0.0.1:8787",
                "127.0.0.1:8787",
                None,
            )
        )
        self.assertFalse(
            is_post_origin_allowed(
                "http://evil.example",
                "127.0.0.1:8787",
                None,
            )
        )

    def test_post_origin_policy_with_cors(self) -> None:
        self.assertTrue(
            is_post_origin_allowed(
                "HTTPS://APP.EXAMPLE/",
                "127.0.0.1:8787",
                "https://app.example",
            )
        )
        self.assertFalse(
            is_post_origin_allowed(
                "https://other.example",
                "127.0.0.1:8787",
                "https://app.example",
            )
        )

    def test_is_cors_origin_allowed(self) -> None:
        self.assertTrue(is_cors_origin_allowed("HTTPS://APP.EXAMPLE/", "https://app.example"))
        self.assertFalse(is_cors_origin_allowed(None, "https://app.example"))
        self.assertFalse(is_cors_origin_allowed("not-a-url", "https://app.example"))
        self.assertFalse(is_cors_origin_allowed("https://other.example", "https://app.example"))

    def test_validate_bind_security(self) -> None:
        validate_bind_security("127.0.0.1", None)
        validate_bind_security("0.0.0.0", "secret")
        with self.assertRaises(RuntimeError):
            validate_bind_security("0.0.0.0", None)

    def test_log_line_classifier(self) -> None:
        self.assertTrue(is_probable_esp_log_line("I (12) main: hello"))
        self.assertTrue(is_probable_esp_log_line("ets Jun  8 2016 00:22:57"))
        self.assertFalse(is_probable_esp_log_line("assistant reply text"))

    def test_serial_error_classifier(self) -> None:
        self.assertTrue(
            is_probable_serial_exception(
                Exception(
                    "device reports readiness to read but returned no data "
                    "(device disconnected or multiple access on port?)"
                )
            )
        )
        self.assertFalse(is_probable_serial_exception(Exception("unexpected parsing failure")))

    def test_describe_serial_exception_busy(self) -> None:
        error = Exception("resource busy")
        text = describe_serial_exception("/dev/cu.usbmodem1101", error)
        self.assertIn("appears busy", text)
        self.assertIn("/dev/cu.usbmodem1101", text)

    def test_serial_bridge_wraps_serial_like_error(self) -> None:
        class FailingSerial:
            def reset_input_buffer(self) -> None:
                return

            def read(self, size: int) -> bytes:
                return b""

            def write(self, payload: bytes) -> int:
                raise Exception(
                    "device reports readiness to read but returned no data "
                    "(device disconnected or multiple access on port?)"
                )

            def flush(self) -> None:
                return

        bridge = SerialAgentBridge(
            port="/dev/cu.usbmodem1101",
            baudrate=115200,
            serial_timeout_s=0.1,
            response_timeout_s=1.0,
            idle_timeout_s=0.2,
            log_serial=False,
        )
        bridge._serial = FailingSerial()

        with self.assertRaises(RuntimeError) as ctx:
            bridge.ask("hello")
        self.assertIn("appears busy", str(ctx.exception))

    def test_serial_bridge_ignores_pre_echo_noise(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.lines = [
                    b"_free=161596 heap_delta=-4500\n",
                    b"hello\n",
                    b"I (123) agent: Processing: hello\n",
                    b"Hi there\n",
                    b"\n",
                ]
                self.writes: list[bytes] = []

            def reset_input_buffer(self) -> None:
                return

            def read(self, size: int) -> bytes:
                return b""

            def write(self, payload: bytes) -> int:
                self.writes.append(payload)
                return len(payload)

            def flush(self) -> None:
                return

            def readline(self) -> bytes:
                if self.lines:
                    return self.lines.pop(0)
                return b""

        bridge = SerialAgentBridge(
            port="/dev/cu.usbmodem1101",
            baudrate=115200,
            serial_timeout_s=0.05,
            response_timeout_s=0.2,
            idle_timeout_s=0.02,
            log_serial=False,
        )
        fake = FakeSerial()
        bridge._serial = fake

        reply = bridge.ask("hello")
        self.assertEqual(reply, "Hi there")
        self.assertEqual(fake.writes, [b"hello\n"])

    def test_serial_bridge_handles_voice_sideband(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.writes: list[bytes] = []

            def write(self, payload: bytes) -> int:
                self.writes.append(payload)
                return len(payload)

            def flush(self) -> None:
                return

        bridge = SerialAgentBridge(
            port="/dev/cu.usbmodem1101",
            baudrate=115200,
            serial_timeout_s=0.05,
            response_timeout_s=0.2,
            idle_timeout_s=0.02,
            log_serial=False,
            voice_transcriber=lambda pcm, sr: f"len={len(pcm)} sr={sr}",
        )
        fake = FakeSerial()
        bridge._serial = fake

        chunk = base64.b64encode(b"\x01\x02\x03\x04").decode("ascii")
        self.assertTrue(bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:begin:16000"))
        self.assertTrue(bridge._handle_voice_sideband_line(f"__zclaw_voice_stt_req__:chunk:{chunk}"))
        self.assertTrue(bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:end"))

        outbound = b"".join(fake.writes).decode("utf-8")
        self.assertIn(f"{VOICE_STT_RESP_PREFIX}ok:", outbound)
        self.assertIn("len=4 sr=16000", outbound)

    def test_serial_bridge_rejects_voice_chunk_without_begin(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.writes: list[bytes] = []

            def write(self, payload: bytes) -> int:
                self.writes.append(payload)
                return len(payload)

            def flush(self) -> None:
                return

        bridge = SerialAgentBridge(
            port="/dev/cu.usbmodem1101",
            baudrate=115200,
            serial_timeout_s=0.05,
            response_timeout_s=0.2,
            idle_timeout_s=0.02,
            log_serial=False,
            voice_transcriber=lambda pcm, sr: "unused",
        )
        fake = FakeSerial()
        bridge._serial = fake

        bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:chunk:AAAA")
        outbound = b"".join(fake.writes).decode("utf-8")
        self.assertIn(f"{VOICE_STT_RESP_PREFIX}err:", outbound)
        self.assertIn("chunk without begin", outbound)

    def test_serial_bridge_rejects_voice_without_wake_phrase(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.writes: list[bytes] = []

            def write(self, payload: bytes) -> int:
                self.writes.append(payload)
                return len(payload)

            def flush(self) -> None:
                return

        bridge = SerialAgentBridge(
            port="/dev/cu.usbmodem1101",
            baudrate=115200,
            serial_timeout_s=0.05,
            response_timeout_s=0.2,
            idle_timeout_s=0.02,
            log_serial=False,
            voice_transcriber=lambda pcm, sr: "turn on gpio five",
            voice_wake_phrase="hey zclaw",
        )
        fake = FakeSerial()
        bridge._serial = fake

        chunk = base64.b64encode(b"\x01\x02\x03\x04").decode("ascii")
        self.assertTrue(bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:begin:16000"))
        self.assertTrue(bridge._handle_voice_sideband_line(f"__zclaw_voice_stt_req__:chunk:{chunk}"))
        self.assertTrue(bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:end"))

        outbound = b"".join(fake.writes).decode("utf-8")
        self.assertIn(f"{VOICE_STT_RESP_PREFIX}err:", outbound)
        self.assertIn("wake phrase not detected", outbound)

    def test_serial_bridge_accepts_voice_with_wake_phrase(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.writes: list[bytes] = []

            def write(self, payload: bytes) -> int:
                self.writes.append(payload)
                return len(payload)

            def flush(self) -> None:
                return

        bridge = SerialAgentBridge(
            port="/dev/cu.usbmodem1101",
            baudrate=115200,
            serial_timeout_s=0.05,
            response_timeout_s=0.2,
            idle_timeout_s=0.02,
            log_serial=False,
            voice_transcriber=lambda pcm, sr: "Hey, zclaw turn on gpio five",
            voice_wake_phrase="hey zclaw",
        )
        fake = FakeSerial()
        bridge._serial = fake

        chunk = base64.b64encode(b"\x01\x02\x03\x04").decode("ascii")
        self.assertTrue(bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:begin:16000"))
        self.assertTrue(bridge._handle_voice_sideband_line(f"__zclaw_voice_stt_req__:chunk:{chunk}"))
        self.assertTrue(bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:end"))

        outbound = b"".join(fake.writes).decode("utf-8")
        self.assertIn(f"{VOICE_STT_RESP_PREFIX}ok:", outbound)
        self.assertIn("Hey, zclaw turn on gpio five", outbound)

    def test_serial_bridge_accepts_split_word_wake_phrase(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.writes: list[bytes] = []

            def write(self, payload: bytes) -> int:
                self.writes.append(payload)
                return len(payload)

            def flush(self) -> None:
                return

        bridge = SerialAgentBridge(
            port="/dev/cu.usbmodem1101",
            baudrate=115200,
            serial_timeout_s=0.05,
            response_timeout_s=0.2,
            idle_timeout_s=0.02,
            log_serial=False,
            voice_transcriber=lambda pcm, sr: "z claw turn on gpio five",
            voice_wake_phrase="zclaw",
        )
        fake = FakeSerial()
        bridge._serial = fake

        chunk = base64.b64encode(b"\x01\x02\x03\x04").decode("ascii")
        self.assertTrue(bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:begin:16000"))
        self.assertTrue(bridge._handle_voice_sideband_line(f"__zclaw_voice_stt_req__:chunk:{chunk}"))
        self.assertTrue(bridge._handle_voice_sideband_line("__zclaw_voice_stt_req__:end"))

        outbound = b"".join(fake.writes).decode("utf-8")
        self.assertIn(f"{VOICE_STT_RESP_PREFIX}ok:", outbound)
        self.assertIn("z claw turn on gpio five", outbound)

    def test_transcribe_voice_pcm_wake_phrase_miss(self) -> None:
        with self.assertRaises(RuntimeError) as ctx:
            transcribe_voice_pcm(
                pcm_bytes=b"\x01\x02\x03\x04",
                sample_rate_hz=16000,
                voice_transcriber=lambda pcm, sr: "turn on gpio five",
                voice_wake_phrase="zclaw",
            )
        self.assertIn("wake phrase not detected", str(ctx.exception))

    def test_transcribe_voice_pcm_accepts_split_phrase(self) -> None:
        transcript = transcribe_voice_pcm(
            pcm_bytes=b"\x01\x02\x03\x04",
            sample_rate_hz=16000,
            voice_transcriber=lambda pcm, sr: "z claw turn on gpio five",
            voice_wake_phrase="zclaw",
        )
        self.assertEqual(transcript, "z claw turn on gpio five")

    def test_voice_stt_http_endpoint_success(self) -> None:
        state = AppState(
            bridge=MockAgentBridge(latency_s=0.0),
            bridge_target="mock-agent",
            api_key=None,
            cors_origin=None,
            voice_stt_enabled=True,
            voice_stt_handler=lambda pcm, sr: f"len={len(pcm)} sr={sr}",
        )
        status, body = self._post_voice_stt(
            state,
            pcm=b"\x01\x02\x03\x04",
            headers={"X-Zclaw-Sample-Rate": "16000"},
        )
        self.assertEqual(status, 200)
        self.assertEqual(body, "len=4 sr=16000")

    def test_voice_stt_http_endpoint_requires_sample_rate(self) -> None:
        state = AppState(
            bridge=MockAgentBridge(latency_s=0.0),
            bridge_target="mock-agent",
            api_key=None,
            cors_origin=None,
            voice_stt_enabled=True,
            voice_stt_handler=lambda pcm, sr: "ok",
        )
        status, body = self._post_voice_stt(state, pcm=b"\x01\x02", headers={})
        self.assertEqual(status, 400)
        self.assertIn("sample rate", body.lower())

    def test_voice_stt_http_endpoint_requires_auth_when_configured(self) -> None:
        state = AppState(
            bridge=MockAgentBridge(latency_s=0.0),
            bridge_target="mock-agent",
            api_key="secret",
            cors_origin=None,
            voice_stt_enabled=True,
            voice_stt_handler=lambda pcm, sr: "ok",
        )
        status, body = self._post_voice_stt(
            state,
            pcm=b"\x01\x02",
            headers={"X-Zclaw-Sample-Rate": "16000"},
        )
        self.assertEqual(status, 401)
        self.assertIn("unauthorized", body.lower())

    def test_mock_bridge_commands(self) -> None:
        bridge = MockAgentBridge(latency_s=0.0)
        self.assertEqual(bridge.ask("ping"), "pong")
        status = bridge.ask("status")
        self.assertIn("mock-agent online", status)

    def test_create_agent_bridge_mock(self) -> None:
        class Args:
            mock_agent = True
            mock_latency = 0.0
            serial_port = None
            baud = 115200
            serial_timeout = 0.15
            response_timeout = 90.0
            idle_timeout = 1.2
            log_serial = False

        bridge, target = create_agent_bridge(Args())
        self.assertIsInstance(bridge, MockAgentBridge)
        self.assertEqual(target, "mock-agent")

    def test_resolve_serial_port_returns_explicit(self) -> None:
        self.assertEqual(resolve_serial_port("/dev/ttyTEST0"), "/dev/ttyTEST0")

    def _post_voice_stt(
        self,
        state: AppState,
        pcm: bytes,
        headers: dict[str, str],
    ) -> tuple[int, str]:
        from http.server import ThreadingHTTPServer

        try:
            httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(state))
        except PermissionError as exc:
            raise unittest.SkipTest("sandbox disallows binding local test server") from exc
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            host, port = httpd.server_address
            conn = http.client.HTTPConnection(host, port, timeout=3.0)
            req_headers = {
                "Content-Length": str(len(pcm)),
                **headers,
            }
            conn.request("POST", "/api/voice/stt", body=pcm, headers=req_headers)
            resp = conn.getresponse()
            body = resp.read().decode("utf-8", errors="replace")
            conn.close()
            return resp.status, body
        finally:
            httpd.shutdown()
            httpd.server_close()
            thread.join(timeout=1.0)


if __name__ == "__main__":
    unittest.main()
