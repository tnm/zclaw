#!/usr/bin/env python3
"""Unit tests for web_relay helpers."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "scripts"))

from web_relay import (  # noqa: E402
    MockAgentBridge,
    SerialAgentBridge,
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
    normalize_api_key,
    normalize_origin,
    resolve_serial_port,
    validate_bind_security,
)


class WebRelayTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
