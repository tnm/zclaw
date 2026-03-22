#!/usr/bin/env python3
"""Unit tests for benchmark_latency helpers."""

from __future__ import annotations

import argparse
import sys
import types
import unittest
from pathlib import Path
from unittest import mock


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "scripts"))

import benchmark_latency
from benchmark_latency import RequestSample, build_request_message


def make_sample(index: int) -> RequestSample:
    return RequestSample(
        index=index,
        host_total_ms=1.0,
        relay_elapsed_ms=1,
        first_response_ms=1.0,
        device_total_ms=1,
        device_llm_ms=1,
        device_tool_ms=0,
        device_rounds=1,
        device_outcome="success",
    )


class BenchmarkLatencyTests(unittest.TestCase):
    def test_build_request_message_leaves_prompt_unchanged_by_default(self) -> None:
        self.assertEqual(build_request_message("ping", 3, False), "ping")

    def test_build_request_message_appends_counter_when_enabled(self) -> None:
        self.assertEqual(build_request_message("ping", 3, True), "ping 3")

    def test_run_relay_benchmark_uses_monotonic_request_suffixes(self) -> None:
        args = argparse.Namespace(
            api_key=None,
            url="http://relay",
            warmup=1,
            count=2,
            message="ping",
            append_counter=True,
            http_timeout=1.0,
            interval_ms=0,
        )
        seen_messages: list[str] = []

        def fake_run_relay_request(
            url: str,
            message: str,
            timeout_s: float,
            api_key: str | None,
            index: int,
        ) -> RequestSample:
            self.assertEqual(url, args.url)
            self.assertEqual(timeout_s, args.http_timeout)
            self.assertIsNone(api_key)
            seen_messages.append(message)
            return make_sample(index)

        with (
            mock.patch.object(benchmark_latency, "run_relay_request", side_effect=fake_run_relay_request),
            mock.patch.object(benchmark_latency.time, "sleep"),
        ):
            samples = benchmark_latency.run_relay_benchmark(args)

        self.assertEqual(seen_messages, ["ping 1", "ping 2", "ping 3"])
        self.assertEqual([sample.index for sample in samples], [1, 2])

    def test_run_serial_benchmark_uses_monotonic_request_suffixes(self) -> None:
        args = argparse.Namespace(
            serial_port="/dev/fake",
            baud=115200,
            serial_timeout=0.1,
            warmup=1,
            count=2,
            message="ping",
            append_counter=True,
            response_timeout=1.0,
            idle_timeout=0.1,
            interval_ms=0,
            log_lines=False,
        )
        fake_serial = mock.Mock()
        seen_messages: list[str] = []

        def fake_run_serial_request(
            ser: object,
            message: str,
            response_timeout_s: float,
            idle_timeout_s: float,
            index: int,
        ) -> tuple[RequestSample, list[str]]:
            self.assertIs(ser, fake_serial)
            self.assertEqual(response_timeout_s, args.response_timeout)
            self.assertEqual(idle_timeout_s, args.idle_timeout)
            seen_messages.append(message)
            return make_sample(index), ["ok"]

        fake_serial_module = types.SimpleNamespace(Serial=mock.Mock(return_value=fake_serial))

        with (
            mock.patch.dict(sys.modules, {"serial": fake_serial_module}),
            mock.patch.object(benchmark_latency, "run_serial_request", side_effect=fake_run_serial_request),
            mock.patch.object(benchmark_latency.time, "sleep"),
        ):
            samples = benchmark_latency.run_serial_benchmark(args)

        fake_serial_module.Serial.assert_called_once_with(
            args.serial_port, args.baud, timeout=args.serial_timeout
        )
        fake_serial.close.assert_called_once_with()
        self.assertEqual(seen_messages, ["ping 1", "ping 2", "ping 3"])
        self.assertEqual([sample.index for sample in samples], [1, 2])


if __name__ == "__main__":
    unittest.main()
