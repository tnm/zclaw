#!/usr/bin/env python3
"""Summarize NETDIAG firmware log lines from zclaw soak runs."""

from __future__ import annotations

import argparse
import math
import re
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

NETDIAG_MARKER = "NETDIAG "


@dataclass
class OpStats:
    total: int = 0
    ok: int = 0
    fail: int = 0
    durations_ms: list[int] = field(default_factory=list)
    statuses: Counter[int] = field(default_factory=Counter)
    errs: Counter[str] = field(default_factory=Counter)
    errnos: Counter[int] = field(default_factory=Counter)
    stale_polls: int = 0
    stale_updates: int = 0
    new_updates: int = 0


def field_str(line: str, key: str) -> str | None:
    if key == "op" and "op=flush getUpdates" in line:
        return "flush_getUpdates"
    match = re.search(rf"\b{re.escape(key)}=([^ ]+)", line)
    return match.group(1) if match else None


def field_int(line: str, key: str) -> int | None:
    match = re.search(rf"\b{re.escape(key)}=(-?\d+)", line)
    if not match:
        return None
    try:
        return int(match.group(1))
    except ValueError:
        return None


def percentile(values: list[int], pct: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = max(0.0, min(1.0, pct)) * (len(ordered) - 1)
    low = int(math.floor(rank))
    high = int(math.ceil(rank))
    if low == high:
        return ordered[low]
    weight = rank - low
    return int(round(ordered[low] * (1.0 - weight) + ordered[high] * weight))


def parse_netdiag_lines(lines: Iterable[str]) -> dict[str, OpStats]:
    stats: dict[str, OpStats] = defaultdict(OpStats)

    for raw_line in lines:
        line = raw_line.strip()
        if NETDIAG_MARKER not in line:
            continue

        op = field_str(line, "op") or "unknown"
        op_stats = stats[op]
        op_stats.total += 1

        ok = field_int(line, "ok")
        if ok == 1:
            op_stats.ok += 1
        else:
            op_stats.fail += 1

        dur_ms = field_int(line, "dur_ms")
        if dur_ms is not None:
            op_stats.durations_ms.append(dur_ms)

        status = field_int(line, "status")
        if status is not None:
            op_stats.statuses[status] += 1

        err = field_str(line, "err")
        if err:
            op_stats.errs[err] += 1

        errno_value = field_int(line, "errno")
        if errno_value is not None:
            op_stats.errnos[errno_value] += 1

        stale = field_int(line, "stale")
        if stale is not None:
            op_stats.stale_updates += stale
            if stale > 0:
                op_stats.stale_polls += 1

        new_updates = field_int(line, "new")
        if new_updates is not None:
            op_stats.new_updates += new_updates

    return dict(stats)


def top_k(counter: Counter, limit: int = 3) -> str:
    if not counter:
        return "-"
    parts: list[str] = []
    for key, count in counter.most_common(limit):
        parts.append(f"{key}:{count}")
    return ", ".join(parts)


def print_summary(stats: dict[str, OpStats]) -> int:
    if not stats:
        print("No NETDIAG lines found.")
        return 1

    print("NETDIAG summary")
    print("=============")

    for op in sorted(stats.keys()):
        op_stats = stats[op]
        fail_rate = (op_stats.fail / op_stats.total * 100.0) if op_stats.total else 0.0

        print(f"op={op}")
        print(
            f"  total={op_stats.total} ok={op_stats.ok} fail={op_stats.fail} "
            f"fail_rate={fail_rate:.1f}%"
        )

        if op_stats.durations_ms:
            p50 = percentile(op_stats.durations_ms, 0.50)
            p95 = percentile(op_stats.durations_ms, 0.95)
            p99 = percentile(op_stats.durations_ms, 0.99)
            mean = statistics.fmean(op_stats.durations_ms)
            print(
                f"  latency_ms mean={mean:.1f} p50={p50} p95={p95} p99={p99} "
                f"max={max(op_stats.durations_ms)}"
            )

        print(f"  status_top={top_k(op_stats.statuses)}")
        print(f"  err_top={top_k(op_stats.errs)}")
        print(f"  errno_top={top_k(op_stats.errnos)}")

        if op == "getUpdates":
            print(
                f"  telegram_stale polls_with_stale={op_stats.stale_polls} "
                f"stale_updates={op_stats.stale_updates} new_updates={op_stats.new_updates}"
            )

    return 0


def read_lines(path: str | None) -> Iterable[str]:
    if path is None:
        return sys.stdin
    return Path(path).read_text(encoding="utf-8", errors="replace").splitlines()


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize zclaw NETDIAG log lines")
    parser.add_argument("logfile", nargs="?", help="Log file path (default: stdin)")
    parser.add_argument(
        "--op",
        action="append",
        default=[],
        help="Only include one or more operations (e.g. --op getUpdates --op llm_request)",
    )
    args = parser.parse_args()

    stats = parse_netdiag_lines(read_lines(args.logfile))

    if args.op:
        selected = set(args.op)
        stats = {k: v for k, v in stats.items() if k in selected}

    return print_summary(stats)


if __name__ == "__main__":
    raise SystemExit(main())
