#!/usr/bin/env python3
"""Считает mem_busy по CSV-трассам spike_dram.

mem_busy = access_time + turnaround_time:
  * access_time     — сумма задержек доступа к памяти;
  * turnaround_time — штрафы за смену направления read <-> write.

Для async-трассы задержки берутся из колонки latency_ns (там лежат вклады
ACT/PRE/RD/WR). Для sync-трассы latency_ns нет, поэтому задержка доступа
считается из числа чтений/записей и фиксированных --read-ns/--write-ns.
Turnaround в обеих моделях восстанавливается по сменам RD/WR в трассе.

Примеры:
    python3 tools/trace_stats.py dram_trace_async.csv
    python3 tools/trace_stats.py dram_trace_async.csv dram_trace_sync.csv
    python3 tools/trace_stats.py dram_trace_sync.csv --read-ns 30 --write-ns 20
"""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path

ACCESS_COMMANDS = {"RD", "WR"}


@dataclass
class TraceStats:
    path: Path
    mode: str
    accesses: int
    reads: int
    writes: int
    direction_changes: int
    access_time_ns: float
    turnaround_time_ns: float

    @property
    def mem_busy_ns(self) -> float:
        return self.access_time_ns + self.turnaround_time_ns

    @property
    def avg_access_ns(self) -> float:
        return self.access_time_ns / self.accesses if self.accesses else 0.0

    @property
    def avg_mem_busy_ns(self) -> float:
        return self.mem_busy_ns / self.accesses if self.accesses else 0.0


def detect_mode(fieldnames: list[str]) -> str:
    fields = set(fieldnames)
    if "row_access" in fields and "latency_ns" in fields:
        return "async"
    if {"command", "type", "addr", "len"} <= fields:
        return "sync"
    raise ValueError(f"неизвестный формат CSV: {sorted(fields)}")


def load_stats(path: Path, args: argparse.Namespace) -> TraceStats:
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        fieldnames = reader.fieldnames or []
        mode = detect_mode(fieldnames) if args.mode == "auto" else args.mode
        rows = list(reader)

    if "command" not in fieldnames:
        raise ValueError(f"{path}: нет колонки command")

    access_rows = [row for row in rows if row.get("command") in ACCESS_COMMANDS]
    reads = sum(1 for row in access_rows if row["command"] == "RD")
    writes = len(access_rows) - reads

    direction_changes = sum(
        1 for prev, cur in zip(access_rows, access_rows[1:]) if prev["command"] != cur["command"]
    )

    if mode == "async":
        if "latency_ns" not in fieldnames:
            raise ValueError(f"{path}: для async нужна колонка latency_ns")
        access_time_ns = sum(float(row["latency_ns"]) for row in rows)
    elif mode == "sync":
        if "latency_ns" in fieldnames:
            access_time_ns = sum(float(row["latency_ns"]) for row in access_rows)
        else:
            access_time_ns = reads * args.read_ns + writes * args.write_ns
    else:
        raise ValueError(f"неизвестный режим: {mode}")

    return TraceStats(
        path=path,
        mode=mode,
        accesses=len(access_rows),
        reads=reads,
        writes=writes,
        direction_changes=direction_changes,
        access_time_ns=access_time_ns,
        turnaround_time_ns=direction_changes * args.switch_ns,
    )


def print_table(stats: list[TraceStats]) -> None:
    header = (
        f"{'file':<28} {'mode':<6} {'acc':>8} {'rd':>8} {'wr':>8} {'turns':>6} "
        f"{'access_ns':>12} {'turn_ns':>10} {'mem_busy_ns':>13} {'avg_mem_ns':>11}"
    )
    print(header)
    print("-" * len(header))
    for s in stats:
        print(
            f"{s.path.name:<28} {s.mode:<6} {s.accesses:>8} {s.reads:>8} {s.writes:>8} "
            f"{s.direction_changes:>6} {s.access_time_ns:>12.1f} {s.turnaround_time_ns:>10.1f} "
            f"{s.mem_busy_ns:>13.1f} {s.avg_mem_busy_ns:>11.3f}"
        )


def print_comparison(reference: TraceStats, compared: TraceStats) -> None:
    print()
    print(f"reference: {reference.path} ({reference.mode})")
    print(f"compared:  {compared.path} ({compared.mode})")
    print()
    print(f"{'metric':<16} {'reference':>14} {'compared':>14} {'delta':>14} {'abs_error':>10}")
    print("-" * 72)
    for name, ref_value, cmp_value in (
        ("mem_busy_ns", reference.mem_busy_ns, compared.mem_busy_ns),
        ("access_ns", reference.access_time_ns, compared.access_time_ns),
        ("avg_mem_ns", reference.avg_mem_busy_ns, compared.avg_mem_busy_ns),
    ):
        delta = cmp_value - ref_value
        error = abs(delta) / ref_value * 100.0 if ref_value else float("nan")
        print(f"{name:<16} {ref_value:>14.3f} {cmp_value:>14.3f} {delta:>14.3f} {error:>9.2f}%")


def write_csv(path: Path, stats: list[TraceStats]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "file",
                "mode",
                "accesses",
                "reads",
                "writes",
                "direction_changes",
                "access_ns",
                "turnaround_ns",
                "mem_busy_ns",
                "avg_access_ns",
                "avg_mem_busy_ns",
            ]
        )
        for s in stats:
            writer.writerow(
                [
                    s.path,
                    s.mode,
                    s.accesses,
                    s.reads,
                    s.writes,
                    s.direction_changes,
                    f"{s.access_time_ns:.6f}",
                    f"{s.turnaround_time_ns:.6f}",
                    f"{s.mem_busy_ns:.6f}",
                    f"{s.avg_access_ns:.6f}",
                    f"{s.avg_mem_busy_ns:.6f}",
                ]
            )


def main() -> int:
    parser = argparse.ArgumentParser(description="Посчитать mem_busy по CSV-трассам spike_dram")
    parser.add_argument("traces", nargs="+", type=Path, help="CSV-трассы (async и/или sync)")
    parser.add_argument(
        "--mode", choices=("auto", "async", "sync"), default="auto", help="формат трассы (по умолчанию auto)"
    )
    parser.add_argument(
        "--read-ns", type=float, default=30.0, help="sync: задержка чтения в нс (по умолчанию 30)"
    )
    parser.add_argument(
        "--write-ns", type=float, default=20.0, help="sync: задержка записи в нс (по умолчанию 20)"
    )
    parser.add_argument(
        "--switch-ns",
        type=float,
        default=20.0,
        help="штраф за смену направления чтение/запись в нс (по умолчанию 20)",
    )
    parser.add_argument("--csv-out", type=Path, help="записать метрики в CSV")
    args = parser.parse_args()

    try:
        stats = [load_stats(path, args) for path in args.traces]
    except (OSError, ValueError, KeyError) as error:
        print(f"trace_stats: {error}", file=sys.stderr)
        return 1

    print_table(stats)

    if len(stats) == 2:
        reference, compared = stats
        if reference.mode == "sync" and compared.mode == "async":
            reference, compared = compared, reference
        print_comparison(reference, compared)

    if args.csv_out:
        write_csv(args.csv_out, stats)
        print(f"\nmetrics -> {args.csv_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
