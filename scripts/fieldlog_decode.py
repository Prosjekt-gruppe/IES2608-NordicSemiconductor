#!/usr/bin/env python3
"""
Decode `fieldlog dump` output into a more readable table or expanded CSV/JSON.

Supported formats:
- v2 mixed field log rows: state changes + 10-minute summaries
- v1 legacy battery-only summary rows

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
from pathlib import Path
from typing import Iterable, List, Sequence


LEGACY_ROW_PATTERN = re.compile(
    r"(?P<row>"
    r"\d+,\d+,\d+,\d+,"
    r"-?\d+,-?\d+,-?\d+,"
    r"\d+,\d+,\d+,"
    r"-?\d+,-?\d+,"
    r"\d+,\d+,\d+,"
    r"0x[0-9a-fA-F]+"
    r")"
)

V2_PREFIXES = ("state,", "summary,")

FLAG_NO_BATTERY_SAMPLES = 1 << 0
FLAG_VBUS_SEEN_V2 = 1 << 1
FLAG_STORAGE_DISABLED_V2 = 1 << 2
FLAG_STORAGE_DISABLED_V1 = 1 << 1
FLAG_VBUS_SEEN_V1 = 1 << 2


def seconds_to_hms(total_seconds: int) -> str:
    hours, rem = divmod(total_seconds, 3600)
    minutes, seconds = divmod(rem, 60)
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}"


def parse_optional_int(value: str) -> int | None:
    value = value.strip()
    if not value:
        return None
    return int(value)


def decode_v2_flags(flags_raw: int) -> str:
    names: List[str] = []
    if flags_raw & FLAG_NO_BATTERY_SAMPLES:
        names.append("no_battery_samples")
    if flags_raw & FLAG_VBUS_SEEN_V2:
        names.append("vbus_seen")
    if flags_raw & FLAG_STORAGE_DISABLED_V2:
        names.append("storage_disabled")
    return "|".join(names) if names else "-"


def decode_v1_flags(flags_raw: int) -> str:
    names: List[str] = []
    if flags_raw & FLAG_NO_BATTERY_SAMPLES:
        names.append("no_samples")
    if flags_raw & FLAG_STORAGE_DISABLED_V1:
        names.append("storage_disabled")
    if flags_raw & FLAG_VBUS_SEEN_V1:
        names.append("vbus_seen")
    return "|".join(names) if names else "-"


def parse_legacy_row(row_text: str) -> dict:
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

    return {
        "type": "battery_v1",
        "seq": seq,
        "uptime_s": uptime_s,
        "uptime_hms": seconds_to_hms(uptime_s),
        "interval_s": interval_s,
        "interval_hms": seconds_to_hms(interval_s),
        "samples": samples,
        "avg_current_ma": avg_current_ma,
        "min_current_ma": min_current_ma,
        "max_current_ma": max_current_ma,
        "avg_voltage_mv": avg_voltage_mv,
        "min_voltage_mv": min_voltage_mv,
        "last_voltage_mv": last_voltage_mv,
        "avg_temp_c": avg_temp_deci_c / 10.0,
        "net_energy_interval_uwh": net_energy_interval_uwh,
        "discharge_energy_interval_uwh": discharge_energy_interval_uwh,
        "discharge_energy_total_uwh": discharge_energy_total_uwh,
        "vbus_samples": vbus_samples,
        "flags_raw": flags_raw,
        "flags_hex": f"0x{flags_raw:02X}",
        "flags_text": decode_v1_flags(flags_raw),
    }


def parse_v2_row(row_text: str) -> dict:
    parts = next(csv.reader([row_text]))
    if len(parts) == 21 and parts and parts[0].strip() == "state":
        parts.append("")
    if len(parts) != 22:
        raise ValueError(f"expected 22 columns, got {len(parts)}: {row_text!r}")

    row_type = parts[0].strip()
    seq = int(parts[1])
    uptime_s = int(parts[2])

    if row_type == "state":
        accuracy_m = parse_optional_int(parts[11])
        return {
            "type": "state",
            "seq": seq,
            "uptime_s": uptime_s,
            "uptime_hms": seconds_to_hms(uptime_s),
            "from_state": parts[3].strip(),
            "to_state": parts[4].strip(),
            "reason": parts[5].strip(),
            "active_rat": parts[6].strip(),
            "next_rat": parts[7].strip(),
            "location_source": parts[8].strip(),
            "latitude": parts[9].strip(),
            "longitude": parts[10].strip(),
            "accuracy_m": accuracy_m,
            "last_rsrp_dbm": int(parts[12]) if parts[12].strip() else None,
        }

    if row_type == "summary":
        flags_text = decode_v2_flags(int(parts[20], 16))
        return {
            "type": "summary",
            "seq": seq,
            "uptime_s": uptime_s,
            "uptime_hms": seconds_to_hms(uptime_s),
            "interval_s": int(parts[13]),
            "interval_hms": seconds_to_hms(int(parts[13])),
            "power_interval_uwh": int(parts[14]),
            "power_total_uwh": int(parts[15]),
            "lte_losses_interval": int(parts[16]),
            "lte_losses_total": int(parts[17]),
            "switchbacks_interval": int(parts[18]),
            "switchbacks_total": int(parts[19]),
            "flags_raw": int(parts[20], 16),
            "flags_hex": parts[20].strip(),
            "flags_text": flags_text,
            "dropped_messages": int(parts[21]),
        }

    raise ValueError(f"unknown row type: {row_type!r}")


def extract_row_text(line: str) -> str | None:
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        return None
    if stripped.startswith("type,"):
        return None

    for prefix in V2_PREFIXES:
        idx = line.find(prefix)
        if idx >= 0:
            return line[idx:].strip()

    match = LEGACY_ROW_PATTERN.search(line)
    if match:
        return match.group("row")

    return None


def parse_lines(lines: Iterable[str]) -> List[dict]:
    rows: List[dict] = []

    for line in lines:
        row_text = extract_row_text(line)
        if not row_text:
            continue

        if row_text.startswith(V2_PREFIXES):
            rows.append(parse_v2_row(row_text))
        else:
            rows.append(parse_legacy_row(row_text))

    rows.sort(key=lambda row: row["seq"])
    return rows


def read_input(path_str: str) -> List[str]:
    if path_str == "-":
        return sys.stdin.read().splitlines()

    return Path(path_str).read_text(encoding="utf-8").splitlines()


def build_summary(rows: Sequence[dict]) -> List[tuple[str, str]]:
    if not rows:
        return [("records", "0")]

    state_rows = [row for row in rows if row["type"] == "state"]
    summary_rows = [row for row in rows if row["type"] == "summary"]
    legacy_rows = [row for row in rows if row["type"] == "battery_v1"]

    summary: List[tuple[str, str]] = [
        ("records", str(len(rows))),
        ("first_seq", str(rows[0]["seq"])),
        ("last_seq", str(rows[-1]["seq"])),
        ("uptime_start", rows[0]["uptime_hms"]),
        ("uptime_end", rows[-1]["uptime_hms"]),
    ]

    if state_rows or summary_rows:
        summary.extend(
            [
                ("state_changes", str(len(state_rows))),
                ("summaries", str(len(summary_rows))),
            ]
        )
        if summary_rows:
            latest = summary_rows[-1]
            summary.extend(
                [
                    (
                        "total_power",
                        f"{latest['power_total_uwh']} uWh ({latest['power_total_uwh'] / 1000.0:.3f} mWh)",
                    ),
                    ("lte_losses_total", str(latest["lte_losses_total"])),
                    ("switchbacks_total", str(latest["switchbacks_total"])),
                ]
            )
    elif legacy_rows:
        total_interval_s = sum(row["interval_s"] for row in legacy_rows)
        summary.extend(
            [
                ("legacy_rows", str(len(legacy_rows))),
                ("logged_duration", seconds_to_hms(total_interval_s)),
                (
                    "total_discharge_energy",
                    f"{legacy_rows[-1]['discharge_energy_total_uwh']} uWh "
                    f"({legacy_rows[-1]['discharge_energy_total_uwh'] / 1000.0:.3f} mWh)",
                ),
            ]
        )

    return summary


def render_summary(summary: Sequence[tuple[str, str]]) -> str:
    key_width = max(len(key) for key, _ in summary)
    return "\n".join(f"{key:<{key_width}} : {value}" for key, value in summary)


def render_table(rows: Sequence[dict]) -> str:
    headers = ["seq", "uptime", "type", "details"]
    table_rows: List[List[str]] = []

    for row in rows:
        if row["type"] == "state":
            location = row["location_source"]
            if row["location_source"] != "none" and row["latitude"] and row["longitude"]:
                if row["accuracy_m"] is None:
                    location = f"{row['location_source']} {row['latitude']},{row['longitude']} acc=?"
                else:
                    location = (
                        f"{row['location_source']} {row['latitude']},{row['longitude']} "
                        f"acc={row['accuracy_m']}m"
                    )
            details = (
                f"{row['from_state']} -> {row['to_state']} | {row['reason']} | "
                f"rat={row['active_rat']}/{row['next_rat']} | loc={location}"
            )
            if row["last_rsrp_dbm"] is not None and row["last_rsrp_dbm"] > -30000:
                details += f" | rsrp={row['last_rsrp_dbm']} dBm"
        elif row["type"] == "summary":
            details = (
                f"interval={row['interval_hms']} | power={row['power_interval_uwh']}/"
                f"{row['power_total_uwh']} uWh | lte_losses={row['lte_losses_interval']}/"
                f"{row['lte_losses_total']} | switchbacks={row['switchbacks_interval']}/"
                f"{row['switchbacks_total']} | dropped={row['dropped_messages']} | "
                f"flags={row['flags_text']}"
            )
        else:
            details = (
                f"interval={row['interval_hms']} | current={row['avg_current_ma']}/"
                f"{row['min_current_ma']}/{row['max_current_ma']} mA | "
                f"voltage={row['avg_voltage_mv']}/{row['min_voltage_mv']}/"
                f"{row['last_voltage_mv']} mV | discharge_total="
                f"{row['discharge_energy_total_uwh']} uWh | flags={row['flags_text']}"
            )

        table_rows.append(
            [
                str(row["seq"]),
                row["uptime_hms"],
                row["type"],
                details,
            ]
        )

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


def render_expanded_csv(rows: Sequence[dict]) -> str:
    if not rows:
        return ""

    fieldnames = sorted({key for row in rows for key in row.keys()})
    output_lines: List[str] = []

    class ListWriter:
        def write(self, text: str) -> int:
            output_lines.append(text)
            return len(text)

    writer = csv.DictWriter(ListWriter(), fieldnames=fieldnames, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow(row)

    return "".join(output_lines)


def render_json(rows: Sequence[dict]) -> str:
    return json.dumps(list(rows), indent=2)


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
