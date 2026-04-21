#!/usr/bin/env python3
"""
Decode `fieldlog dump` output into a more readable table or expanded CSV/JSON.

The parser is intentionally tolerant:
- It accepts plain `fieldlog dump` CSV lines.
- It ignores shell prompts, comments, and unrelated log lines.
- It works on raw Tera Term capture files.

Examples:
    python scripts/fieldlog_decode.py fieldlog_dump.txt
    python scripts/fieldlog_decode.py fieldlog_dump.txt --format csv --output decoded.csv
    python scripts/fieldlog_decode.py fieldlog_dump.txt --format json
    type fieldlog_dump.txt | python scripts/fieldlog_decode.py -
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, List, Sequence


ROW_PATTERN = re.compile(
    r"(?P<row>"
    r"\d+,\d+,\d+,\d+,"
    r"-?\d+,-?\d+,-?\d+,"
    r"\d+,\d+,\d+,"
    r"-?\d+,-?\d+,"
    r"\d+,\d+,\d+,"
    r"0x[0-9a-fA-F]+"
    r")"
)


FLAG_NO_SAMPLES = 1 << 0
FLAG_STORAGE_DISABLED = 1 << 1
FLAG_VBUS_SEEN = 1 << 2


@dataclass
class FieldLogRow:
    seq: int
    uptime_s: int
    interval_s: int
    samples: int
    avg_current_ma: int
    min_current_ma: int
    max_current_ma: int
    avg_voltage_mv: int
    min_voltage_mv: int
    last_voltage_mv: int
    avg_temp_deci_c: int
    net_energy_interval_uwh: int
    discharge_energy_interval_uwh: int
    discharge_energy_total_uwh: int
    vbus_samples: int
    flags_raw: int

    @property
    def uptime_hms(self) -> str:
        return seconds_to_hms(self.uptime_s)

    @property
    def interval_hms(self) -> str:
        return seconds_to_hms(self.interval_s)

    @property
    def avg_temp_c(self) -> float:
        return self.avg_temp_deci_c / 10.0

    @property
    def net_energy_interval_mwh(self) -> float:
        return self.net_energy_interval_uwh / 1000.0

    @property
    def discharge_energy_interval_mwh(self) -> float:
        return self.discharge_energy_interval_uwh / 1000.0

    @property
    def discharge_energy_total_mwh(self) -> float:
        return self.discharge_energy_total_uwh / 1000.0

    @property
    def no_samples(self) -> bool:
        return bool(self.flags_raw & FLAG_NO_SAMPLES)

    @property
    def storage_disabled(self) -> bool:
        return bool(self.flags_raw & FLAG_STORAGE_DISABLED)

    @property
    def vbus_seen(self) -> bool:
        return bool(self.flags_raw & FLAG_VBUS_SEEN)

    @property
    def flags_text(self) -> str:
        names = []
        if self.no_samples:
            names.append("no_samples")
        if self.storage_disabled:
            names.append("storage_disabled")
        if self.vbus_seen:
            names.append("vbus_seen")
        return "|".join(names) if names else "-"

    @property
    def power_state(self) -> str:
        # Zephyr battery convention: positive current means charging, negative means discharging.
        if self.vbus_seen:
            if self.avg_current_ma > 0:
                return "usb_charging"
            if self.avg_current_ma < 0:
                return "usb_plus_load"
            return "usb_idle"

        if self.avg_current_ma < 0:
            return "battery_discharge"
        if self.avg_current_ma > 0:
            return "battery_charge"
        return "battery_idle"

    @property
    def current_triplet(self) -> str:
        return f"{self.avg_current_ma}/{self.min_current_ma}/{self.max_current_ma}"

    @property
    def voltage_triplet(self) -> str:
        return f"{self.avg_voltage_mv}/{self.min_voltage_mv}/{self.last_voltage_mv}"

    def to_expanded_dict(self) -> dict:
        data = asdict(self)
        data.update(
            {
                "uptime_hms": self.uptime_hms,
                "interval_hms": self.interval_hms,
                "avg_temp_c": round(self.avg_temp_c, 1),
                "net_energy_interval_mwh": round(self.net_energy_interval_mwh, 3),
                "discharge_energy_interval_mwh": round(self.discharge_energy_interval_mwh, 3),
                "discharge_energy_total_mwh": round(self.discharge_energy_total_mwh, 3),
                "vbus_seen": self.vbus_seen,
                "no_samples": self.no_samples,
                "storage_disabled": self.storage_disabled,
                "power_state": self.power_state,
                "flags_hex": f"0x{self.flags_raw:02X}",
                "flags_text": self.flags_text,
            }
        )
        return data


def seconds_to_hms(total_seconds: int) -> str:
    hours, rem = divmod(total_seconds, 3600)
    minutes, seconds = divmod(rem, 60)
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}"


def parse_row(row_text: str) -> FieldLogRow:
    parts = [part.strip() for part in row_text.split(",")]
    if len(parts) != 16:
        raise ValueError(f"expected 16 columns, got {len(parts)}: {row_text!r}")

    seq, uptime_s, interval_s, samples = map(int, parts[0:4])
    avg_current_ma, min_current_ma, max_current_ma = map(int, parts[4:7])
    avg_voltage_mv, min_voltage_mv, last_voltage_mv = map(int, parts[7:10])
    avg_temp_deci_c = int(parts[10])
    net_energy_interval_uwh = int(parts[11])
    discharge_energy_interval_uwh = int(parts[12])
    discharge_energy_total_uwh = int(parts[13])
    vbus_samples = int(parts[14])
    flags_raw = int(parts[15], 16)

    return FieldLogRow(
        seq=seq,
        uptime_s=uptime_s,
        interval_s=interval_s,
        samples=samples,
        avg_current_ma=avg_current_ma,
        min_current_ma=min_current_ma,
        max_current_ma=max_current_ma,
        avg_voltage_mv=avg_voltage_mv,
        min_voltage_mv=min_voltage_mv,
        last_voltage_mv=last_voltage_mv,
        avg_temp_deci_c=avg_temp_deci_c,
        net_energy_interval_uwh=net_energy_interval_uwh,
        discharge_energy_interval_uwh=discharge_energy_interval_uwh,
        discharge_energy_total_uwh=discharge_energy_total_uwh,
        vbus_samples=vbus_samples,
        flags_raw=flags_raw,
    )


def parse_lines(lines: Iterable[str]) -> List[FieldLogRow]:
    rows: List[FieldLogRow] = []

    for line in lines:
        match = ROW_PATTERN.search(line)
        if not match:
            continue
        rows.append(parse_row(match.group("row")))

    rows.sort(key=lambda row: row.seq)
    return rows


def read_input(path_str: str) -> List[str]:
    if path_str == "-":
        return sys.stdin.read().splitlines()

    return Path(path_str).read_text(encoding="utf-8").splitlines()


def build_summary(rows: Sequence[FieldLogRow]) -> List[tuple[str, str]]:
    if not rows:
        return [("records", "0")]

    total_interval_s = sum(row.interval_s for row in rows)
    total_net_uwh = sum(row.net_energy_interval_uwh for row in rows)
    total_discharge_uwh = rows[-1].discharge_energy_total_uwh
    vbus_records = sum(1 for row in rows if row.vbus_seen)
    battery_records = len(rows) - vbus_records
    min_voltage = min(row.min_voltage_mv for row in rows)
    max_voltage = max(row.avg_voltage_mv for row in rows)

    return [
        ("records", str(len(rows))),
        ("first_seq", str(rows[0].seq)),
        ("last_seq", str(rows[-1].seq)),
        ("uptime_start", rows[0].uptime_hms),
        ("uptime_end", rows[-1].uptime_hms),
        ("logged_duration", seconds_to_hms(total_interval_s)),
        ("total_net_energy", f"{total_net_uwh} uWh ({total_net_uwh / 1000.0:.3f} mWh)"),
        (
            "total_discharge_energy",
            f"{total_discharge_uwh} uWh ({total_discharge_uwh / 1000.0:.3f} mWh)",
        ),
        ("records_with_vbus", str(vbus_records)),
        ("records_on_battery", str(battery_records)),
        ("voltage_range_mv", f"{min_voltage}..{max_voltage}"),
    ]


def render_summary(summary: Sequence[tuple[str, str]]) -> str:
    key_width = max(len(key) for key, _ in summary)
    return "\n".join(f"{key:<{key_width}} : {value}" for key, value in summary)


def render_table(rows: Sequence[FieldLogRow]) -> str:
    headers = [
        "seq",
        "uptime",
        "int",
        "samples",
        "current a/min/max",
        "voltage a/min/last",
        "temp C",
        "net uWh",
        "dischg uWh",
        "total uWh",
        "vbus",
        "state",
        "flags",
    ]

    table_rows = [
        [
            str(row.seq),
            row.uptime_hms,
            row.interval_hms,
            str(row.samples),
            row.current_triplet,
            row.voltage_triplet,
            f"{row.avg_temp_c:.1f}",
            str(row.net_energy_interval_uwh),
            str(row.discharge_energy_interval_uwh),
            str(row.discharge_energy_total_uwh),
            "yes" if row.vbus_seen else "no",
            row.power_state,
            row.flags_text,
        ]
        for row in rows
    ]

    widths = [len(header) for header in headers]
    for row in table_rows:
        for idx, cell in enumerate(row):
            widths[idx] = max(widths[idx], len(cell))

    def format_row(cells: Sequence[str]) -> str:
        return " | ".join(cell.ljust(widths[idx]) for idx, cell in enumerate(cells))

    separator = "-+-".join("-" * width for width in widths)

    lines = [format_row(headers), separator]
    lines.extend(format_row(row) for row in table_rows)
    return "\n".join(lines)


def render_expanded_csv(rows: Sequence[FieldLogRow]) -> str:
    if not rows:
        return ""

    fieldnames = list(rows[0].to_expanded_dict().keys())
    output_lines: List[str] = []

    class ListWriter:
        def write(self, text: str) -> int:
            output_lines.append(text)
            return len(text)

    writer = csv.DictWriter(ListWriter(), fieldnames=fieldnames, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow(row.to_expanded_dict())

    return "".join(output_lines)


def render_json(rows: Sequence[FieldLogRow]) -> str:
    return json.dumps([row.to_expanded_dict() for row in rows], indent=2)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input",
        help="Path to a fieldlog dump capture file, or '-' to read stdin.",
    )
    parser.add_argument(
        "--format",
        choices=("table", "csv", "json"),
        default="table",
        help="Output format. Default: table.",
    )
    parser.add_argument(
        "--output",
        help="Optional output file. Defaults to stdout.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    rows = parse_lines(read_input(args.input))

    if not rows:
        print("No fieldlog rows found in input.", file=sys.stderr)
        return 1

    if args.format == "table":
        rendered = (
            "Summary\n"
            f"{render_summary(build_summary(rows))}\n\n"
            "Rows\n"
            f"{render_table(rows)}\n"
        )
    elif args.format == "csv":
        rendered = render_expanded_csv(rows)
    else:
        rendered = render_json(rows) + "\n"

    if args.output:
        Path(args.output).write_text(rendered, encoding="utf-8")
    else:
        sys.stdout.write(rendered)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
