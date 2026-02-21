#!/usr/bin/env python3
"""Benchmark zclaw latency through relay HTTP or direct serial."""

from __future__ import annotations

import argparse
import glob
import json
import os
import platform
import re
import statistics
import sys
import time
from dataclasses import dataclass
from typing import Any
from urllib import error, request


ESP_LOG_PREFIXES = ("I (", "W (", "E (", "D (", "V (")
BOOT_LOG_PREFIXES = ("ets ", "rst:", "boot:", "SPIWP:", "mode:", "load:", "entry ")
LOG_LINE_RE = re.compile(r"^[IWEVD] \((?P<ts>\d+)\) (?P<tag>[^:]+): (?P<msg>.*)$")
METRIC_KV_RE = re.compile(r"([a-z_]+)=([^ ]+)")


@dataclass
class RequestSample:
    index: int
    host_total_ms: float
    relay_elapsed_ms: int | None
    first_response_ms: float | None
    device_total_ms: int | None
    device_llm_ms: int | None
    device_tool_ms: int | None
    device_rounds: int | None
    device_outcome: str | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark zclaw latency")
    parser.add_argument(
        "--mode",
        choices=("relay", "serial", "both"),
        default="relay",
        help="Benchmark mode (default: relay)",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=10,
        help="Measured request count per mode (default: 10)",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="Warm-up request count before measurements (default: 1)",
    )
    parser.add_argument(
        "--message",
        default="ping",
        help="Prompt to send (default: ping)",
    )
    parser.add_argument(
        "--interval-ms",
        type=int,
        default=250,
        help="Delay between measured requests in milliseconds (default: 250)",
    )

    parser.add_argument(
        "--url",
        default="http://127.0.0.1:8787/api/chat",
        help="Relay chat endpoint (default: http://127.0.0.1:8787/api/chat)",
    )
    parser.add_argument(
        "--api-key",
        default=None,
        help="Optional relay API key (or use ZCLAW_WEB_API_KEY env)",
    )
    parser.add_argument(
        "--http-timeout",
        type=float,
        default=120.0,
        help="HTTP request timeout in seconds (default: 120)",
    )

    parser.add_argument("--serial-port", default=None, help="Serial port for serial mode")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud (default: 115200)")
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
        help="Max wait for response in seconds (default: 90)",
    )
    parser.add_argument(
        "--idle-timeout",
        type=float,
        default=1.2,
        help="Stop collecting response after this idle gap in seconds (default: 1.2)",
    )
    parser.add_argument(
        "--log-lines",
        action="store_true",
        help="Print captured response lines for each measured request",
    )
    return parser.parse_args()


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
        raise RuntimeError("No serial port detected. Pass --serial-port explicitly.")
    if len(ports) > 1:
        raise RuntimeError(
            "Multiple serial ports detected. Pass --serial-port explicitly: " + ", ".join(ports)
        )
    return ports[0]


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]

    sorted_values = sorted(values)
    rank = (len(sorted_values) - 1) * pct
    lower = int(rank)
    upper = lower + 1

    if upper >= len(sorted_values):
        return sorted_values[-1]

    weight = rank - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def print_summary(title: str, values: list[float]) -> None:
    if not values:
        print(f"  {title}: n/a")
        return

    mean = statistics.fmean(values)
    stdev = statistics.pstdev(values) if len(values) > 1 else 0.0
    print(
        f"  {title}: n={len(values)} "
        f"min={min(values):.1f}ms p50={percentile(values, 0.50):.1f}ms "
        f"p90={percentile(values, 0.90):.1f}ms p95={percentile(values, 0.95):.1f}ms "
        f"max={max(values):.1f}ms mean={mean:.1f}ms stdev={stdev:.1f}ms"
    )


def pct(part: float, whole: float) -> float:
    if whole <= 0.0:
        return 0.0
    return (part / whole) * 100.0


def try_parse_int(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def parse_agent_metric(tag: str, msg: str) -> dict[str, str] | None:
    if tag.strip() != "agent":
        return None
    if not msg.startswith("METRIC request "):
        return None

    payload = msg[len("METRIC request ") :]
    parsed: dict[str, str] = {}
    for match in METRIC_KV_RE.finditer(payload):
        parsed[match.group(1)] = match.group(2)
    return parsed if parsed else None


def drain_serial_input(ser: Any) -> None:
    try:
        ser.reset_input_buffer()
    except Exception:
        while True:
            data = ser.read(1024)
            if not data:
                return


def run_relay_request(
    url: str,
    message: str,
    timeout_s: float,
    api_key: str | None,
    index: int,
) -> RequestSample:
    payload = json.dumps({"message": message}).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["X-Zclaw-Key"] = api_key

    req = request.Request(url, data=payload, headers=headers, method="POST")

    started = time.monotonic()
    try:
        with request.urlopen(req, timeout=timeout_s) as resp:
            body = resp.read()
    except error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc
    except error.URLError as exc:
        raise RuntimeError(f"Relay request failed: {exc}") from exc

    host_total_ms = (time.monotonic() - started) * 1000.0

    try:
        parsed = json.loads(body.decode("utf-8"))
    except Exception as exc:
        raise RuntimeError(f"Invalid JSON response: {body[:200]!r}") from exc

    if not isinstance(parsed, dict):
        raise RuntimeError("Relay returned non-object JSON payload")

    relay_elapsed = parsed.get("elapsed_ms")
    if relay_elapsed is not None and not isinstance(relay_elapsed, int):
        relay_elapsed = None

    return RequestSample(
        index=index,
        host_total_ms=host_total_ms,
        relay_elapsed_ms=relay_elapsed,
        first_response_ms=None,
        device_total_ms=None,
        device_llm_ms=None,
        device_tool_ms=None,
        device_rounds=None,
        device_outcome=None,
    )


def run_serial_request(
    ser: Any,
    message: str,
    response_timeout_s: float,
    idle_timeout_s: float,
    index: int,
) -> tuple[RequestSample, list[str]]:
    sent_prompt = message.strip()
    if not sent_prompt:
        raise RuntimeError("Message must not be empty")

    drain_serial_input(ser)

    started = time.monotonic()
    ser.write((sent_prompt + "\n").encode("utf-8"))
    ser.flush()

    deadline = started + response_timeout_s
    idle_deadline = started + idle_timeout_s
    saw_echo = False
    first_response_ms: float | None = None
    response_lines: list[str] = []
    latest_metric: dict[str, str] | None = None

    while time.monotonic() < deadline:
        raw_line = ser.readline()
        now = time.monotonic()

        if not raw_line:
            if response_lines and now >= idle_deadline:
                break
            continue

        line = raw_line.decode("utf-8", errors="replace").strip("\r\n")
        if not line:
            if response_lines and response_lines[-1] != "":
                response_lines.append("")
            continue

        log_match = LOG_LINE_RE.match(line)
        if log_match:
            metric = parse_agent_metric(log_match.group("tag"), log_match.group("msg"))
            if metric:
                latest_metric = metric
            continue

        if not saw_echo and line.strip() == sent_prompt:
            saw_echo = True
            continue

        if line.startswith(ESP_LOG_PREFIXES) or line.startswith(BOOT_LOG_PREFIXES):
            continue

        response_lines.append(line)
        if first_response_ms is None:
            first_response_ms = (now - started) * 1000.0
        idle_deadline = now + idle_timeout_s

    if not response_lines:
        raise RuntimeError(f"No agent response within {response_timeout_s:.1f}s")

    host_total_ms = (time.monotonic() - started) * 1000.0

    sample = RequestSample(
        index=index,
        host_total_ms=host_total_ms,
        relay_elapsed_ms=None,
        first_response_ms=first_response_ms,
        device_total_ms=try_parse_int((latest_metric or {}).get("total_ms")),
        device_llm_ms=try_parse_int((latest_metric or {}).get("llm_ms")),
        device_tool_ms=try_parse_int((latest_metric or {}).get("tool_ms")),
        device_rounds=try_parse_int((latest_metric or {}).get("rounds")),
        device_outcome=(latest_metric or {}).get("outcome"),
    )
    return sample, response_lines


def run_relay_benchmark(args: argparse.Namespace) -> list[RequestSample]:
    api_key = args.api_key
    if api_key is None:
        api_key = os.environ.get("ZCLAW_WEB_API_KEY") or None

    total = args.warmup + args.count
    samples: list[RequestSample] = []

    print(f"Relay benchmark -> {args.url}")
    print(f"Message: {args.message!r}")

    for i in range(total):
        measured = i >= args.warmup
        phase = "measure" if measured else "warmup"
        ordinal = i - args.warmup + 1 if measured else i + 1
        sample = run_relay_request(args.url, args.message, args.http_timeout, api_key, ordinal)

        if measured:
            samples.append(sample)
            relay_str = f" relay={sample.relay_elapsed_ms}ms" if sample.relay_elapsed_ms is not None else ""
            print(
                f"  [{len(samples)}/{args.count}] {phase} host={sample.host_total_ms:.1f}ms{relay_str}"
            )
            if len(samples) < args.count:
                time.sleep(max(0.0, args.interval_ms / 1000.0))
        else:
            print(f"  [warmup {ordinal}/{args.warmup}] host={sample.host_total_ms:.1f}ms")

    return samples


def run_serial_benchmark(args: argparse.Namespace) -> list[RequestSample]:
    try:
        import serial  # type: ignore
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "pyserial is required for serial mode. Install with: "
            "uv run --with-requirements scripts/requirements-web-relay.txt "
            "scripts/benchmark_latency.py --mode serial --serial-port <port>"
        ) from exc

    port = resolve_serial_port(args.serial_port)
    total = args.warmup + args.count
    samples: list[RequestSample] = []

    print(f"Serial benchmark -> {port} @ {args.baud}")
    print(f"Message: {args.message!r}")

    ser = serial.Serial(port, args.baud, timeout=args.serial_timeout)
    try:
        time.sleep(0.2)

        for i in range(total):
            measured = i >= args.warmup
            phase = "measure" if measured else "warmup"
            ordinal = i - args.warmup + 1 if measured else i + 1

            sample, response_lines = run_serial_request(
                ser,
                args.message,
                args.response_timeout,
                args.idle_timeout,
                ordinal,
            )

            if measured:
                samples.append(sample)
                first_str = (
                    f" first={sample.first_response_ms:.1f}ms"
                    if sample.first_response_ms is not None
                    else ""
                )
                device_str = (
                    f" device_total={sample.device_total_ms}ms"
                    if sample.device_total_ms is not None
                    else ""
                )
                outcome_str = f" outcome={sample.device_outcome}" if sample.device_outcome else ""
                print(
                    f"  [{len(samples)}/{args.count}] {phase} host={sample.host_total_ms:.1f}ms"
                    f"{first_str}{device_str}{outcome_str}"
                )

                if args.log_lines:
                    for line in response_lines:
                        print(f"    {line}")

                if len(samples) < args.count:
                    time.sleep(max(0.0, args.interval_ms / 1000.0))
            else:
                print(f"  [warmup {ordinal}/{args.warmup}] host={sample.host_total_ms:.1f}ms")
    finally:
        try:
            ser.close()
        except Exception:
            pass

    return samples


def print_benchmark_summary(mode: str, samples: list[RequestSample]) -> None:
    if not samples:
        print("No samples collected.")
        return

    print(f"\nSummary ({mode})")
    print_summary("Host total", [s.host_total_ms for s in samples])

    relay_values = [float(s.relay_elapsed_ms) for s in samples if s.relay_elapsed_ms is not None]
    if relay_values:
        print_summary("Relay elapsed", relay_values)

    first_values = [s.first_response_ms for s in samples if s.first_response_ms is not None]
    if first_values:
        print_summary("First response", [float(v) for v in first_values])

    device_total_values = [float(s.device_total_ms) for s in samples if s.device_total_ms is not None]
    if device_total_values:
        print_summary("Device total", device_total_values)

    device_llm_values = [float(s.device_llm_ms) for s in samples if s.device_llm_ms is not None]
    if device_llm_values:
        print_summary("Device LLM", device_llm_values)

    device_tool_values = [float(s.device_tool_ms) for s in samples if s.device_tool_ms is not None]
    if device_tool_values:
        print_summary("Device tools", device_tool_values)

    # Explicitly attribute on-device latency when full stage metrics are available.
    stage_pairs: list[tuple[float, float, float]] = []
    for sample in samples:
        if sample.device_total_ms is None or sample.device_llm_ms is None or sample.device_tool_ms is None:
            continue
        total_ms = float(sample.device_total_ms)
        llm_ms = float(sample.device_llm_ms)
        tool_ms = float(sample.device_tool_ms)
        other_ms = max(0.0, total_ms - llm_ms - tool_ms)
        stage_pairs.append((total_ms, llm_ms, tool_ms + other_ms))

    if stage_pairs:
        totals = [v[0] for v in stage_pairs]
        llms = [v[1] for v in stage_pairs]
        us = [v[2] for v in stage_pairs]

        mean_total = statistics.fmean(totals)
        mean_llm = statistics.fmean(llms)
        mean_us = statistics.fmean(us)

        p50_total = percentile(totals, 0.50)
        p50_llm = percentile(llms, 0.50)
        p50_us = percentile(us, 0.50)

        print(
            "  Device attribution (LLM vs us): "
            f"mean llm={mean_llm:.1f}ms ({pct(mean_llm, mean_total):.1f}%) "
            f"us={mean_us:.1f}ms ({pct(mean_us, mean_total):.1f}%)"
        )
        print(
            "  Device attribution (LLM vs us): "
            f"p50 llm={p50_llm:.1f}ms ({pct(p50_llm, p50_total):.1f}%) "
            f"us={p50_us:.1f}ms ({pct(p50_us, p50_total):.1f}%)"
        )

    outcomes: dict[str, int] = {}
    for sample in samples:
        if not sample.device_outcome:
            continue
        outcomes[sample.device_outcome] = outcomes.get(sample.device_outcome, 0) + 1

    if outcomes:
        outcome_items = ", ".join(f"{k}={v}" for k, v in sorted(outcomes.items()))
        print(f"  Device outcomes: {outcome_items}")


def main() -> int:
    args = parse_args()

    if args.count <= 0:
        print("--count must be > 0", file=sys.stderr)
        return 2
    if args.warmup < 0:
        print("--warmup must be >= 0", file=sys.stderr)
        return 2

    try:
        if args.mode in ("relay", "both"):
            relay_samples = run_relay_benchmark(args)
            print_benchmark_summary("relay", relay_samples)

        if args.mode in ("serial", "both"):
            serial_samples = run_serial_benchmark(args)
            print_benchmark_summary("serial", serial_samples)
    except KeyboardInterrupt:
        print("Interrupted.", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
