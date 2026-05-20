#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_SAMPLE_RATE_HZ = 100_000.0
DEFAULT_VOLTAGE_V = 3.7
DEFAULT_MAX_POINTS = 160_000
FALLBACK_ANALYSIS_START_S = 120.0
FALLBACK_ANALYSIS_END_S = 500.0

STATE_LABELS = {
    "STATE_BOOT": "Boot",
    "STATE_RUNNING": "Running",
    "STATE_DISCONNECTED": "Disconnected",
    "STATE_BACKOFF": "Backoff",
    "STATE_LTEM_CONNECTING": "LTE-M connecting",
    "STATE_NTN_CONNECTING": "NTN connecting",
    "STATE_CONNECTED": "Connected",
    "STATE_LTEM_CONNECTED": "LTE-M connected",
    "STATE_CLOUD_CONNECTING": "Cloud connecting",
    "STATE_LTE_LOCATION": "LTE location",
    "STATE_GNSS_ACQUIRE": "GNSS acquire",
    "STATE_NTN_CONNECTED": "NTN connected",
    "STATE_LTE_PROBE": "LTE probe",
    "STATE_IDLE": "Idle",
}

SUMMARY_IGNORED_STATES = {
    "STATE_CONNECTED",
    "STATE_DISCONNECTED",
    "STATE_RUNNING",
}

IMPORTANT_MARKERS = {
    "LTE-M fallback",
    "LTE-M poor",
    "Starting NTN",
    "Switched to NTN",
    "LTE probe",
    "LTE probe failed",
    "LTE probe -> LTE-M connecting",
    "LTE-M recovered",
    "Retry LTE-M",
    "Stay on NTN",
}

FIELD_LOG_RE = re.compile(
    r"reason=(?P<reason>\S+).*?rat=(?P<rat>\S+).*?rsrp=(?P<rsrp>-?\d+)\s*dBm"
)


@dataclass(frozen=True)
class Transition:
    t_s: float
    from_state: str
    to_state: str
    event_kind: str
    message: str
    reason: str = ""
    rat: str = ""
    rsrp_dbm: Optional[int] = None


@dataclass(frozen=True)
class Interval:
    state: str
    start_s: float
    end_s: float
    transition: Optional[Transition] = None

    @property
    def duration_s(self) -> float:
        return self.end_s - self.start_s


@dataclass(frozen=True)
class Marker:
    t_s: float
    label: str
    message: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate compact report figures/tables for LTE-M to NTN fallback results."
    )
    parser.add_argument(
        "--processed-dir",
        default=str(SCRIPT_DIR / "data" / "processed"),
        help="Directory containing processed CSV files.",
    )
    parser.add_argument(
        "--data-dir",
        default=str(SCRIPT_DIR / "data" / "raw"),
        help="Directory containing current_uA.bin.",
    )
    parser.add_argument(
        "--output-dir",
        default=str(SCRIPT_DIR / "output" / "report_figures"),
        help="Directory for generated report outputs.",
    )
    parser.add_argument(
        "--sample-rate-hz",
        type=float,
        default=DEFAULT_SAMPLE_RATE_HZ,
        help="PPK current sample rate.",
    )
    parser.add_argument(
        "--voltage-v",
        type=float,
        default=DEFAULT_VOLTAGE_V,
        help="Voltage used for energy estimates.",
    )
    parser.add_argument(
        "--max-points",
        type=int,
        default=DEFAULT_MAX_POINTS,
        help="Maximum current points plotted after downsampling.",
    )
    parser.add_argument(
        "--min-label-duration-s",
        type=float,
        default=10.0,
        help="Minimum state interval duration for labels in state_timeline.pdf.",
    )
    parser.add_argument(
        "--current-log-scale",
        action="store_true",
        help="Use logarithmic current axis in combined_result_overview.",
    )
    parser.add_argument(
        "--rsrp-gap-threshold-s",
        type=float,
        default=40.0,
        help="Split LTE-M RSRP line in combined_result_overview when sample gaps exceed this duration.",
    )
    parser.add_argument(
        "--ntn-rsrp-gap-threshold-s",
        type=float,
        default=60.0,
        help="Split NTN estimated RSRP line when sample gaps exceed this duration.",
    )
    parser.add_argument(
        "--analysis-from-s",
        type=float,
        default=None,
        help="Start time for report tables. Defaults to inferred main fallback period.",
    )
    parser.add_argument(
        "--analysis-to-s",
        type=float,
        default=None,
        help="End time for report tables. Defaults to inferred main fallback period.",
    )
    parser.add_argument(
        "--overview-from-s",
        type=float,
        default=None,
        help="Optional start time for combined overview.",
    )
    parser.add_argument(
        "--overview-to-s",
        type=float,
        default=None,
        help="Optional end time for combined overview.",
    )
    parser.add_argument(
        "--probe-from-s",
        type=float,
        default=None,
        help="Optional start time for LTE probe zoom.",
    )
    parser.add_argument(
        "--probe-to-s",
        type=float,
        default=None,
        help="Optional end time for LTE probe zoom.",
    )
    return parser.parse_args()


def state_label(state: str) -> str:
    return STATE_LABELS.get(state, state.replace("STATE_", "").replace("_", " ").title())


def format_seconds(value: float) -> str:
    if not math.isfinite(value):
        return "n/a"
    text = f"{value:.2f}"
    return text.rstrip("0").rstrip(".")


def parse_float(value: object) -> Optional[float]:
    if value is None:
        return None
    text = str(value).strip()
    if not text or text.lower() == "nan":
        return None
    try:
        parsed = float(text)
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) else None


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        print(f"Warning: missing {path}")
        return []
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        return list(csv.DictReader(handle))


def row_time_s(row: dict[str, str], sample_rate_hz: float) -> Optional[float]:
    for key in ("t_sample_s", "t_s"):
        value = parse_float(row.get(key))
        if value is not None:
            return value
    sample_idx = parse_float(row.get("sample_idx"))
    if sample_idx is not None and sample_rate_hz > 0:
        return sample_idx / sample_rate_hz
    return None


def parse_transition(row: dict[str, str], sample_rate_hz: float) -> Optional[Transition]:
    t_s = row_time_s(row, sample_rate_hz)
    to_state = (row.get("to_state") or "").strip()
    if t_s is None or not to_state:
        return None

    message = row.get("message") or ""
    reason = ""
    rat = ""
    rsrp_dbm: Optional[int] = None
    match = FIELD_LOG_RE.search(message)
    if match:
        reason = match.group("reason")
        rat = match.group("rat")
        try:
            rsrp_dbm = int(match.group("rsrp"))
        except ValueError:
            rsrp_dbm = None

    return Transition(
        t_s=t_s,
        from_state=(row.get("from_state") or "").strip(),
        to_state=to_state,
        event_kind=(row.get("event_kind") or "").strip(),
        message=message,
        reason=reason,
        rat=rat,
        rsrp_dbm=rsrp_dbm,
    )


def load_transitions(path: Path, sample_rate_hz: float) -> list[Transition]:
    transitions = [
        transition
        for row in read_csv_rows(path)
        if (transition := parse_transition(row, sample_rate_hz)) is not None
    ]
    transitions.sort(key=lambda item: item.t_s)
    return transitions


def choose_clean_transitions(transitions: list[Transition]) -> tuple[list[Transition], str]:
    field_log = [
        item
        for item in transitions
        if item.event_kind == "field_log" and item.to_state not in SUMMARY_IGNORED_STATES
    ]
    if field_log:
        return field_log, "field_log"
    fallback = [item for item in transitions if item.to_state not in SUMMARY_IGNORED_STATES]
    return fallback, "all transition rows"


def load_markers(path: Path, sample_rate_hz: float) -> list[Marker]:
    markers: list[Marker] = []
    for row in read_csv_rows(path):
        t_s = row_time_s(row, sample_rate_hz)
        label = (row.get("label") or row.get("key") or "").strip()
        if t_s is None or not label:
            continue
        markers.append(Marker(t_s=t_s, label=label, message=row.get("message") or ""))
    markers.sort(key=lambda item: item.t_s)
    return markers


def build_intervals(
    transitions: list[Transition],
    end_s: Optional[float],
    min_duration_s: float = 0.0,
) -> list[Interval]:
    intervals: list[Interval] = []
    clean = [item for item in transitions if item.to_state not in SUMMARY_IGNORED_STATES]
    clean.sort(key=lambda item: item.t_s)
    for idx, transition in enumerate(clean):
        next_t = end_s
        for candidate in clean[idx + 1 :]:
            if candidate.t_s > transition.t_s:
                next_t = candidate.t_s
                break
        if next_t is None or next_t <= transition.t_s:
            continue
        interval = Interval(
            state=transition.to_state,
            start_s=transition.t_s,
            end_s=next_t,
            transition=transition,
        )
        if interval.duration_s >= min_duration_s:
            intervals.append(interval)
    return intervals


def infer_analysis_window(
    clean_transitions: list[Transition],
    explicit_start_s: Optional[float],
    explicit_end_s: Optional[float],
) -> tuple[float, float, str]:
    fallback_start = next(
        (
            item
            for item in clean_transitions
            if item.from_state == "STATE_LTEM_CONNECTED"
            and item.to_state == "STATE_BACKOFF"
            and item.reason == "EVT_LTE_POOR"
        ),
        None,
    )
    inferred_start = FALLBACK_ANALYSIS_START_S
    inferred_end = FALLBACK_ANALYSIS_END_S
    source = "fallback constants"

    if fallback_start is not None:
        later = [item for item in clean_transitions if item.t_s > fallback_start.t_s]
        probe = next((item for item in later if item.to_state == "STATE_LTE_PROBE"), None)
        recovery_attempt = None
        if probe is not None:
            recovery_attempt = next(
                (
                    item
                    for item in later
                    if item.to_state == "STATE_LTEM_CONNECTING" and item.t_s >= probe.t_s
                ),
                None,
            )
        inferred_start = max(0.0, fallback_start.t_s - 45.0)
        inferred_end = (
            recovery_attempt.t_s + 60.0
            if recovery_attempt is not None
            else fallback_start.t_s + 380.0
        )
        source = "inferred from first EVT_LTE_POOR fallback cycle"

    start_s = inferred_start if explicit_start_s is None else explicit_start_s
    end_s = inferred_end if explicit_end_s is None else explicit_end_s
    if end_s < start_s:
        end_s = start_s
    if explicit_start_s is not None or explicit_end_s is not None:
        source = "command line override"
    return start_s, end_s, source


def load_current_memmap(data_dir: Path) -> Optional[np.memmap]:
    path = data_dir / "current_uA.bin"
    if not path.exists():
        print(f"Warning: missing current samples at {path}")
        return None
    if path.stat().st_size == 0:
        print(f"Warning: empty current samples at {path}")
        return None
    return np.memmap(path, dtype=np.float32, mode="r")


def clamp_time_range(
    total_duration_s: float, start_s: Optional[float], end_s: Optional[float]
) -> tuple[float, float]:
    start = 0.0 if start_s is None else float(start_s)
    end = total_duration_s if end_s is None else float(end_s)
    start = max(0.0, min(start, total_duration_s))
    end = max(start, min(end, total_duration_s))
    return start, end


def current_slice(
    samples: np.memmap,
    sample_rate_hz: float,
    start_s: float,
    end_s: float,
) -> np.ndarray:
    start_idx = max(0, int(math.floor(start_s * sample_rate_hz)))
    end_idx = min(len(samples), int(math.ceil(end_s * sample_rate_hz)))
    if end_idx <= start_idx:
        return np.array([], dtype=np.float32)
    return np.asarray(samples[start_idx:end_idx], dtype=np.float64)


def compute_power_rows(
    intervals: list[Interval],
    samples: Optional[np.memmap],
    sample_rate_hz: float,
    voltage_v: float,
    analysis_start_s: Optional[float] = None,
    analysis_end_s: Optional[float] = None,
    analysis_window_source: str = "",
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    if samples is None or sample_rate_hz <= 0:
        return rows

    total_duration_s = len(samples) / sample_rate_hz
    for interval in intervals:
        start_s, end_s = clamp_time_range(total_duration_s, interval.start_s, interval.end_s)
        duration_s = end_s - start_s
        values_uA = current_slice(samples, sample_rate_hz, start_s, end_s)
        if duration_s <= 0 or values_uA.size == 0:
            continue

        clipped_uA = np.clip(values_uA, 0.0, None)
        charge_c = float(np.nansum(clipped_uA) * 1e-6 / sample_rate_hz)
        rows.append(
            {
                "state": interval.state,
                "state_label": state_label(interval.state),
                "start_s": start_s,
                "end_s": end_s,
                "duration_s": duration_s,
                "mean_current_mA": float(np.nanmean(values_uA) / 1000.0),
                "median_current_mA": float(np.nanmedian(values_uA) / 1000.0),
                "charge_mC": charge_c * 1000.0,
                "energy_mJ": charge_c * voltage_v * 1000.0,
                "reason": interval.transition.reason if interval.transition else "",
                "rat": interval.transition.rat if interval.transition else "",
                "rsrp_dbm": interval.transition.rsrp_dbm
                if interval.transition and interval.transition.rsrp_dbm is not None
                else "",
                "analysis_start_s": analysis_start_s if analysis_start_s is not None else "",
                "analysis_end_s": analysis_end_s if analysis_end_s is not None else "",
                "analysis_window_source": analysis_window_source,
            }
        )
    return rows


def power_summary_fields() -> list[str]:
    return [
        "state",
        "state_label",
        "start_s",
        "end_s",
        "duration_s",
        "mean_current_mA",
        "median_current_mA",
        "charge_mC",
        "energy_mJ",
        "reason",
        "rat",
        "rsrp_dbm",
        "analysis_start_s",
        "analysis_end_s",
        "analysis_window_source",
    ]


def write_power_summary_csv(path: Path, rows: list[dict[str, object]]) -> None:
    fields = power_summary_fields()
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def write_grouped_power_summary_csv(path: Path, rows: list[dict[str, object]]) -> None:
    fields = [
        "state",
        "state_label",
        "interval_count",
        "duration_s",
        "mean_current_mA",
        "median_current_mA_interval_weighted",
        "charge_mC",
        "energy_mJ",
        "analysis_start_s",
        "analysis_end_s",
        "analysis_window_source",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def markdown_table(headers: list[str], rows: Iterable[Iterable[object]]) -> str:
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(str(value) for value in row) + " |")
    return "\n".join(lines)


def write_interval_power_summary_md(
    path: Path,
    rows: list[dict[str, object]],
    voltage_v: float,
    interval_source: str,
    analysis_start_s: float,
    analysis_end_s: float,
    analysis_window_source: str,
) -> None:
    table_rows = []
    for row in rows:
        table_rows.append(
            [
                row["state_label"],
                format_seconds(float(row["start_s"])),
                format_seconds(float(row["end_s"])),
                f"{float(row['duration_s']):.2f}",
                f"{float(row['mean_current_mA']):.3f}",
                f"{float(row['median_current_mA']):.3f}",
                f"{float(row['charge_mC']):.3f}",
                f"{float(row['energy_mJ']):.3f}",
            ]
        )

    lines = [
        "# Per-Interval Power Summary",
        "",
        f"- Analysis window: {format_seconds(analysis_start_s)} s to {format_seconds(analysis_end_s)} s.",
        f"- Window source: {analysis_window_source}.",
        f"- State interval source: `{interval_source}` rows from `state_transitions.csv`.",
        "- Each row is one contiguous state interval clipped to the analysis window.",
        "- Repeated occurrences of the same state are not mixed in this table.",
        f"- Energy assumes {voltage_v:.2f} V.",
        "- Negative current samples are clipped to zero for charge and energy calculations.",
        "- Mean and median current are computed from the raw current samples.",
        "",
        markdown_table(
            [
                "State",
                "Start [s]",
                "End [s]",
                "Duration [s]",
                "Mean [mA]",
                "Median [mA]",
                "Charge [mC]",
                "Energy [mJ]",
            ],
            table_rows,
        )
        if table_rows
        else "No per-interval power rows could be computed for this window.",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def weighted_median(values: list[tuple[float, float]]) -> float:
    clean = sorted((value, weight) for value, weight in values if weight > 0 and math.isfinite(value))
    if not clean:
        return float("nan")
    total_weight = sum(weight for _, weight in clean)
    midpoint = total_weight / 2.0
    running = 0.0
    for value, weight in clean:
        running += weight
        if running >= midpoint:
            return value
    return clean[-1][0]


def group_power_rows(
    rows: list[dict[str, object]],
    analysis_start_s: float,
    analysis_end_s: float,
    analysis_window_source: str,
) -> list[dict[str, object]]:
    grouped: dict[str, list[dict[str, object]]] = {}
    for row in rows:
        grouped.setdefault(str(row["state"]), []).append(row)

    output: list[dict[str, object]] = []
    for state, state_rows in grouped.items():
        duration_s = sum(float(row["duration_s"]) for row in state_rows)
        charge_mC = sum(float(row["charge_mC"]) for row in state_rows)
        energy_mJ = sum(float(row["energy_mJ"]) for row in state_rows)
        mean_current_mA = (
            sum(float(row["mean_current_mA"]) * float(row["duration_s"]) for row in state_rows)
            / duration_s
            if duration_s > 0
            else float("nan")
        )
        median_current_mA = weighted_median(
            [
                (float(row["median_current_mA"]), float(row["duration_s"]))
                for row in state_rows
            ]
        )
        output.append(
            {
                "state": state,
                "state_label": state_label(state),
                "interval_count": len(state_rows),
                "duration_s": duration_s,
                "mean_current_mA": mean_current_mA,
                "median_current_mA_interval_weighted": median_current_mA,
                "charge_mC": charge_mC,
                "energy_mJ": energy_mJ,
                "analysis_start_s": analysis_start_s,
                "analysis_end_s": analysis_end_s,
                "analysis_window_source": analysis_window_source,
            }
        )
    output.sort(key=lambda row: str(row["state_label"]))
    return output


def write_grouped_power_summary_md(
    path: Path,
    rows: list[dict[str, object]],
    voltage_v: float,
    analysis_start_s: float,
    analysis_end_s: float,
    analysis_window_source: str,
) -> None:
    table_rows = [
        [
            row["state_label"],
            row["interval_count"],
            f"{float(row['duration_s']):.2f}",
            f"{float(row['mean_current_mA']):.3f}",
            f"{float(row['median_current_mA_interval_weighted']):.3f}",
            f"{float(row['charge_mC']):.3f}",
            f"{float(row['energy_mJ']):.3f}",
        ]
        for row in rows
    ]
    lines = [
        "# Grouped Power Summary for Selected Window",
        "",
        f"- Analysis window: {format_seconds(analysis_start_s)} s to {format_seconds(analysis_end_s)} s.",
        f"- Window source: {analysis_window_source}.",
        f"- Energy assumes {voltage_v:.2f} V.",
        "- This table intentionally groups repeated occurrences of the same state within the selected window.",
        "- Negative current samples are clipped to zero for charge and energy calculations.",
        "- Mean current is duration-weighted from interval means.",
        "- Median current is the duration-weighted median of the per-interval medians, not a sample-level median across all occurrences.",
        "",
        markdown_table(
            [
                "State",
                "Intervals",
                "Duration [s]",
                "Mean [mA]",
                "Weighted median [mA]",
                "Charge [mC]",
                "Energy [mJ]",
            ],
            table_rows,
        )
        if table_rows
        else "No grouped power rows could be computed for this window.",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def first_main_fallback_bounds(clean_transitions: list[Transition]) -> Optional[tuple[float, float]]:
    fallback_start = next(
        (
            item
            for item in clean_transitions
            if item.from_state == "STATE_LTEM_CONNECTED"
            and item.to_state == "STATE_BACKOFF"
            and item.reason == "EVT_LTE_POOR"
        ),
        None,
    )
    if fallback_start is None:
        return None

    later = [item for item in clean_transitions if item.t_s > fallback_start.t_s]
    probe = next((item for item in later if item.to_state == "STATE_LTE_PROBE"), None)
    if probe is None:
        ntn_connected = next(
            (item for item in later if item.to_state == "STATE_NTN_CONNECTED"),
            None,
        )
        if ntn_connected is None:
            return None
        return fallback_start.t_s, ntn_connected.t_s

    recovery_attempt = next(
        (
            item
            for item in later
            if item.to_state == "STATE_LTEM_CONNECTING" and item.t_s >= probe.t_s
        ),
        None,
    )
    if recovery_attempt is None:
        return fallback_start.t_s, probe.t_s
    return fallback_start.t_s, recovery_attempt.t_s


def rows_for_window(
    rows: list[dict[str, object]], start_s: float, end_s: float
) -> list[dict[str, object]]:
    return [
        row
        for row in rows
        if float(row["end_s"]) > start_s and float(row["start_s"]) < end_s
    ]


def try_import_pandas():
    try:
        import pandas as pd
    except ImportError as exc:
        print(f"Warning: pandas unavailable, skipping LaTeX tables: {exc}")
        return None
    return pd


def format_tex_float(value: object, decimals: int) -> str:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return ""
    if not math.isfinite(number):
        return ""
    return f"{number:.{decimals}f}"


def write_latex_table(path: Path, dataframe, caption: str, label: str) -> None:
    latex = dataframe.to_latex(
        index=False,
        escape=True,
        column_format="l" + "r" * (len(dataframe.columns) - 1),
        caption=caption,
        label=label,
        bold_rows=False,
        longtable=False,
    )
    path.write_text(latex, encoding="utf-8")


def export_latex_tables(
    output_dir: Path,
    interval_rows: list[dict[str, object]],
    grouped_rows: list[dict[str, object]],
    event_rows: list[dict[str, object]],
    clean_transitions: list[Transition],
    analysis_start_s: float,
    analysis_end_s: float,
) -> list[Path]:
    pd = try_import_pandas()
    if pd is None:
        return []

    tables_dir = output_dir / "tables"
    tables_dir.mkdir(parents=True, exist_ok=True)
    generated: list[Path] = []

    full_interval_df = pd.DataFrame(
        [
            {
                "State": row["state_label"],
                "Start [s]": format_tex_float(row["start_s"], 2),
                "End [s]": format_tex_float(row["end_s"], 2),
                "Duration [s]": format_tex_float(row["duration_s"], 2),
                "Mean [mA]": format_tex_float(row["mean_current_mA"], 2),
                "Median [mA]": format_tex_float(row["median_current_mA"], 2),
                "Charge [mC]": format_tex_float(row["charge_mC"], 1),
                "Energy [mJ]": format_tex_float(row["energy_mJ"], 1),
            }
            for row in interval_rows
        ]
    )
    if not full_interval_df.empty:
        path = tables_dir / "per_interval_power_summary.tex"
        write_latex_table(
            path,
            full_interval_df,
            (
                "Per-interval power summary for the selected analysis window "
                f"({analysis_start_s:.2f}--{analysis_end_s:.2f} s)."
            ),
            "tab:per-interval-power-summary",
        )
        generated.append(path)

    main_bounds = first_main_fallback_bounds(clean_transitions)
    if main_bounds is None:
        compact_rows = interval_rows
        compact_start_s = analysis_start_s
        compact_end_s = analysis_end_s
    else:
        compact_start_s, compact_end_s = main_bounds
        compact_rows = rows_for_window(interval_rows, compact_start_s, compact_end_s)
    compact_interval_df = pd.DataFrame(
        [
            {
                "State": row["state_label"],
                "Duration [s]": format_tex_float(row["duration_s"], 2),
                "Mean [mA]": format_tex_float(row["mean_current_mA"], 2),
                "Charge [mC]": format_tex_float(row["charge_mC"], 1),
            }
            for row in compact_rows
        ]
    )
    if not compact_interval_df.empty:
        path = tables_dir / "per_interval_power_summary_compact.tex"
        write_latex_table(
            path,
            compact_interval_df,
            (
                "Compact per-interval power summary for the main fallback interval "
                f"({compact_start_s:.2f}--{compact_end_s:.2f} s)."
            ),
            "tab:per-interval-power-summary-compact",
        )
        generated.append(path)

    grouped_df = pd.DataFrame(
        [
            {
                "State": row["state_label"],
                "Duration [s]": format_tex_float(row["duration_s"], 2),
                "Mean [mA]": format_tex_float(row["mean_current_mA"], 2),
                "Charge [mC]": format_tex_float(row["charge_mC"], 1),
            }
            for row in grouped_rows
        ]
    )
    if not grouped_df.empty:
        path = tables_dir / "grouped_power_summary_selected_window.tex"
        write_latex_table(
            path,
            grouped_df,
            (
                "Grouped power summary for the selected analysis window "
                f"({analysis_start_s:.2f}--{analysis_end_s:.2f} s)."
            ),
            "tab:grouped-power-summary-selected-window",
        )
        generated.append(path)

    event_df = pd.DataFrame(
        [
            {
                "Event span": row["metric"],
                "Start [s]": format_tex_float(row["start_s"], 2),
                "End [s]": format_tex_float(row["end_s"], 2),
                "Duration [s]": format_tex_float(row["duration_s"], 2),
            }
            for row in event_rows
        ]
    )
    if not event_df.empty:
        path = tables_dir / "event_duration_summary.tex"
        write_latex_table(
            path,
            event_df,
            (
                "Important fallback event durations for the selected analysis window "
                f"({analysis_start_s:.2f}--{analysis_end_s:.2f} s)."
            ),
            "tab:event-duration-summary",
        )
        generated.append(path)

    return generated


def find_marker(markers: list[Marker], labels: set[str], start_s: float, end_s: float) -> Optional[Marker]:
    for marker in markers:
        if start_s <= marker.t_s <= end_s and marker.label in labels:
            return marker
    return None


def build_event_duration_rows(
    clean_transitions: list[Transition], markers: list[Marker]
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []

    for idx, transition in enumerate(clean_transitions):
        if not (
            transition.to_state == "STATE_BACKOFF"
            and transition.from_state == "STATE_LTEM_CONNECTED"
            and transition.reason == "EVT_LTE_POOR"
        ):
            continue

        next_items = clean_transitions[idx + 1 :]
        ntn_connect = next(
            (item for item in next_items if item.to_state == "STATE_NTN_CONNECTING"),
            None,
        )
        ntn_connected = next(
            (item for item in next_items if item.to_state == "STATE_NTN_CONNECTED"),
            None,
        )
        lte_probe = next(
            (item for item in next_items if item.to_state == "STATE_LTE_PROBE"),
            None,
        )
        ltem_connect = next(
            (
                item
                for item in next_items
                if item.to_state == "STATE_LTEM_CONNECTING"
                and (lte_probe is None or item.t_s >= lte_probe.t_s)
            ),
            None,
        )

        poor_marker = find_marker(
            markers,
            {"LTE-M poor", "LTE-M fallback"},
            transition.t_s - 5.0,
            transition.t_s + 5.0,
        )
        degradation_t = poor_marker.t_s if poor_marker else transition.t_s
        degradation_note = (
            f"Used marker `{poor_marker.label}` near Backoff."
            if poor_marker
            else "No separate degradation marker; using field-log transition into Backoff."
        )

        rows.append(
            {
                "cycle_start_s": transition.t_s,
                "metric": "LTE-M poor/degradation -> Backoff",
                "start_s": degradation_t,
                "end_s": transition.t_s,
                "duration_s": max(0.0, transition.t_s - degradation_t),
                "note": degradation_note,
            }
        )

        if ntn_connect:
            rows.append(
                {
                    "cycle_start_s": transition.t_s,
                    "metric": "Backoff -> NTN connecting",
                    "start_s": transition.t_s,
                    "end_s": ntn_connect.t_s,
                    "duration_s": ntn_connect.t_s - transition.t_s,
                    "note": "Field-log state timestamps.",
                }
            )
        if ntn_connect and ntn_connected:
            rows.append(
                {
                    "cycle_start_s": transition.t_s,
                    "metric": "NTN connecting -> NTN connected",
                    "start_s": ntn_connect.t_s,
                    "end_s": ntn_connected.t_s,
                    "duration_s": ntn_connected.t_s - ntn_connect.t_s,
                    "note": "Ends at field-log EVT_PDN_UP / NTN connected.",
                }
            )
        if ntn_connected and lte_probe:
            rows.append(
                {
                    "cycle_start_s": transition.t_s,
                    "metric": "NTN connected -> LTE probe",
                    "start_s": ntn_connected.t_s,
                    "end_s": lte_probe.t_s,
                    "duration_s": lte_probe.t_s - ntn_connected.t_s,
                    "note": "Field-log state timestamps.",
                }
            )
        if lte_probe and ltem_connect:
            rows.append(
                {
                    "cycle_start_s": transition.t_s,
                    "metric": "LTE probe -> LTE-M connecting / recovery attempt",
                    "start_s": lte_probe.t_s,
                    "end_s": ltem_connect.t_s,
                    "duration_s": ltem_connect.t_s - lte_probe.t_s,
                    "note": "Recovery attempt inferred from transition into LTE-M connecting.",
                }
            )

    # Remove duplicate rows caused by Backoff retries inside one cycle.
    deduped: list[dict[str, object]] = []
    seen: set[tuple[float, str]] = set()
    for row in rows:
        key = (round(float(row["cycle_start_s"]), 3), str(row["metric"]))
        if key in seen:
            continue
        seen.add(key)
        deduped.append(row)
    return deduped


def filter_event_rows_to_window(
    rows: list[dict[str, object]], start_s: float, end_s: float
) -> list[dict[str, object]]:
    return [
        row
        for row in rows
        if float(row["end_s"]) >= start_s and float(row["start_s"]) <= end_s
    ]


def write_event_duration_summary(
    path: Path,
    rows: list[dict[str, object]],
    analysis_start_s: float,
    analysis_end_s: float,
    analysis_window_source: str,
) -> None:
    table_rows = [
        [
            format_seconds(float(row["cycle_start_s"])),
            row["metric"],
            format_seconds(float(row["start_s"])),
            format_seconds(float(row["end_s"])),
            f"{float(row['duration_s']):.2f}",
            row["note"],
        ]
        for row in rows
    ]
    lines = [
        "# Event Duration Summary",
        "",
        f"- Analysis window: {format_seconds(analysis_start_s)} s to {format_seconds(analysis_end_s)} s.",
        f"- Window source: {analysis_window_source}.",
        "Durations are derived from `event_kind == field_log` transitions where possible.",
        "When a separate marker is not available, the field-log state transition timestamp is used as the event time.",
        "",
        markdown_table(
            ["Cycle start [s]", "Event span", "Start [s]", "End [s]", "Duration [s]", "Note"],
            table_rows,
        )
        if table_rows
        else "No fallback event durations could be inferred.",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def extract_numeric_series(
    rows: list[dict[str, str]],
    sample_rate_hz: float,
    value_col: str,
    transform=lambda x: x,
) -> tuple[np.ndarray, np.ndarray]:
    points: list[tuple[float, float]] = []
    for row in rows:
        t_s = row_time_s(row, sample_rate_hz)
        value = parse_float(row.get(value_col))
        if t_s is None or value is None:
            continue
        points.append((t_s, transform(value)))
    points.sort(key=lambda item: item[0])
    if not points:
        return np.array([]), np.array([])
    return np.array([item[0] for item in points]), np.array([item[1] for item in points])


def filter_series(
    t: np.ndarray, y: np.ndarray, start_s: float, end_s: float
) -> tuple[np.ndarray, np.ndarray]:
    if t.size == 0:
        return t, y
    mask = (t >= start_s) & (t <= end_s) & np.isfinite(y)
    return t[mask], y[mask]


def downsample_current_for_plot(
    samples: Optional[np.memmap],
    sample_rate_hz: float,
    start_s: float,
    end_s: float,
    max_points: int,
) -> tuple[np.ndarray, np.ndarray]:
    if samples is None or sample_rate_hz <= 0:
        return np.array([]), np.array([])
    total_duration_s = len(samples) / sample_rate_hz
    start_s, end_s = clamp_time_range(total_duration_s, start_s, end_s)
    start_idx = max(0, int(math.floor(start_s * sample_rate_hz)))
    end_idx = min(len(samples), int(math.ceil(end_s * sample_rate_hz)))
    if end_idx <= start_idx:
        return np.array([]), np.array([])
    point_count = end_idx - start_idx
    step = max(1, int(math.ceil(point_count / max(1, max_points))))
    indices = np.arange(start_idx, end_idx, step)
    return indices / sample_rate_hz, np.asarray(samples[indices], dtype=np.float64) / 1000.0


def intervals_in_range(
    intervals: list[Interval], start_s: float, end_s: float
) -> list[Interval]:
    clipped: list[Interval] = []
    for interval in intervals:
        clipped_start = max(start_s, interval.start_s)
        clipped_end = min(end_s, interval.end_s)
        if clipped_end > clipped_start:
            clipped.append(Interval(interval.state, clipped_start, clipped_end, interval.transition))
    return clipped


def setup_matplotlib():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as exc:
        print(f"Warning: matplotlib unavailable, skipping figures: {exc}")
        return None

    plt.style.use("default")
    plt.rcParams.update(
        {
            "font.size": 10,
            "axes.titlesize": 12,
            "axes.labelsize": 10,
            "legend.fontsize": 8,
        }
    )
    return plt


def state_colors(states: Iterable[str]):
    palette = [
        "#4C78A8",
        "#F58518",
        "#54A24B",
        "#E45756",
        "#72B7B2",
        "#B279A2",
        "#FF9DA6",
        "#9D755D",
        "#BAB0AC",
    ]
    return {state: palette[idx % len(palette)] for idx, state in enumerate(sorted(set(states)))}


def add_state_shading(
    ax,
    intervals: list[Interval],
    colors: dict[str, str],
    alpha: float = 0.12,
    show_labels: bool = True,
):
    used: set[str] = set()
    for interval in intervals:
        label = state_label(interval.state)
        ax.axvspan(
            interval.start_s,
            interval.end_s,
            color=colors.get(interval.state, "#BAB0AC"),
            alpha=alpha,
            label=label if show_labels and label not in used else None,
            linewidth=0,
        )
        used.add(label)


def add_marker_lines(ax, markers: list[Marker], start_s: float, end_s: float):
    used: set[str] = set()
    for marker in markers:
        if not (start_s <= marker.t_s <= end_s) or marker.label not in IMPORTANT_MARKERS:
            continue
        label = marker.label if marker.label not in used else None
        ax.axvline(marker.t_s, color="0.35", linestyle="--", linewidth=0.9, alpha=0.7, label=label)
        used.add(marker.label)


def insert_gap_breaks(t: np.ndarray, y: np.ndarray, gap_threshold_s: float) -> tuple[np.ndarray, np.ndarray]:
    if t.size <= 1:
        return t, y
    out_t: list[float] = []
    out_y: list[float] = []
    for idx, (t_value, y_value) in enumerate(zip(t, y)):
        if idx > 0 and t_value - t[idx - 1] > gap_threshold_s:
            out_t.append(t_value)
            out_y.append(float("nan"))
        out_t.append(t_value)
        out_y.append(y_value)
    return np.array(out_t), np.array(out_y)


def important_overview_events(
    intervals: list[Interval], markers: list[Marker], start_s: float, end_s: float
) -> list[tuple[float, str, str]]:
    events: list[tuple[float, str, str]] = []

    backoff = next(
        (
            interval
            for interval in intervals
            if interval.state == "STATE_BACKOFF"
            and interval.transition is not None
            and interval.transition.from_state == "STATE_LTEM_CONNECTED"
        ),
        None,
    )
    ntn_connecting = next((interval for interval in intervals if interval.state == "STATE_NTN_CONNECTING"), None)
    ntn_connected = next((interval for interval in intervals if interval.state == "STATE_NTN_CONNECTED"), None)
    lte_probe = next((interval for interval in intervals if interval.state == "STATE_LTE_PROBE"), None)
    ltem_connecting = next(
        (
            interval
            for interval in intervals
            if interval.state == "STATE_LTEM_CONNECTING"
            and lte_probe is not None
            and interval.start_s >= lte_probe.start_s
        ),
        None,
    )

    if backoff is not None:
        events.append((backoff.start_s, "Backoff", "STATE_BACKOFF"))
    if ntn_connecting is not None:
        events.append((ntn_connecting.start_s, "NTN conn.", "STATE_NTN_CONNECTING"))
    if ntn_connected is not None:
        events.append((ntn_connected.start_s, "NTN up", "STATE_NTN_CONNECTED"))
    if lte_probe is not None:
        events.append((lte_probe.start_s, "LTE probe", "STATE_LTE_PROBE"))
    if ltem_connecting is not None:
        events.append((ltem_connecting.start_s, "LTE retry", "STATE_LTEM_CONNECTING"))

    deduped: list[tuple[float, str, str]] = []
    for t_s, label, state in events:
        if not (start_s <= t_s <= end_s):
            continue
        if any(abs(t_s - existing_t) <= 0.5 and state == existing_state for existing_t, _, existing_state in deduped):
            continue
        deduped.append((t_s, label, state))
    return deduped


def add_overview_event_markers(
    ax,
    events: list[tuple[float, str, str]],
    colors: dict[str, str],
    annotate: bool,
    label_top_fraction: float = 0.10,
) -> None:
    y_min, y_max = ax.get_ylim()
    y_span = y_max - y_min
    label_y = y_max - y_span * label_top_fraction
    for t_s, label, _state in events:
        ax.axvline(
            t_s,
            color="0.25",
            linestyle="--",
            linewidth=0.9,
            alpha=0.45,
            zorder=1,
        )
        if annotate:
            label_x = t_s
            if label == "Backoff":
                label_x = t_s - 12.0
            elif label == "NTN conn.":
                label_x = t_s + 12.0
            ax.text(
                label_x,
                label_y,
                label,
                ha="center",
                va="top",
                fontsize=7,
                color="0.2",
                bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.65, "pad": 0.8},
                clip_on=False,
            )


def plot_state_timeline(
    plt,
    path: Path,
    intervals: list[Interval],
    min_label_duration_s: float,
) -> None:
    if not intervals:
        return
    states = []
    for interval in intervals:
        if interval.state not in states:
            states.append(interval.state)
    y_map = {state: idx for idx, state in enumerate(states)}
    colors = state_colors(states)

    fig_height = max(3.2, 0.42 * len(states) + 1.4)
    fig, ax = plt.subplots(figsize=(12, fig_height))
    for interval in intervals:
        y = y_map[interval.state]
        ax.barh(
            y,
            interval.duration_s,
            left=interval.start_s,
            height=0.55,
            color=colors[interval.state],
            edgecolor="white",
            linewidth=0.7,
        )
        if interval.duration_s >= min_label_duration_s:
            ax.text(
                interval.start_s + interval.duration_s / 2.0,
                y,
                f"{interval.duration_s:.1f}s",
                ha="center",
                va="center",
                fontsize=7.5,
                color="black",
            )
    ax.set_yticks(list(y_map.values()), [state_label(state) for state in states])
    ax.set_xlabel("Time [s]")
    ax.set_title("Overview over field trip fallback result")
    ax.grid(True, axis="x", linestyle="--", alpha=0.35)
    ax.invert_yaxis()
    fig.tight_layout()
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)


def plot_state_axis(ax, intervals: list[Interval], colors: dict[str, str]) -> None:
    states = []
    for interval in intervals:
        if interval.state not in states:
            states.append(interval.state)
    y_map = {state: idx for idx, state in enumerate(states)}
    for interval in intervals:
        ax.barh(
            y_map[interval.state],
            interval.duration_s,
            left=interval.start_s,
            height=0.62,
            color=colors.get(interval.state, "#BAB0AC"),
            edgecolor="white",
            linewidth=0.5,
        )
    ax.set_yticks(list(y_map.values()), [state_label(state) for state in states])
    ax.set_xlabel("Time [s]")
    ax.grid(True, axis="x", linestyle="--", alpha=0.3)
    ax.invert_yaxis()


def plot_rsrp_panel(
    ax,
    intervals: list[Interval],
    colors: dict[str, str],
    events: list[tuple[float, str, str]],
    conneval_rows: list[dict[str, str]],
    ntn_rows: list[dict[str, str]],
    sample_rate_hz: float,
    start_s: float,
    end_s: float,
    rsrp_gap_threshold_s: float,
    ntn_rsrp_gap_threshold_s: float,
    title: Optional[str] = None,
    annotate_events: bool = True,
    show_state_shading: bool = True,
    event_label_top_fraction: float = 0.10,
) -> None:
    lte_t, lte_rsrp = extract_numeric_series(conneval_rows, sample_rate_hz, "rsrp_dbm")
    lte_t, lte_rsrp = filter_series(lte_t, lte_rsrp, start_s, end_s)
    lte_t, lte_rsrp = insert_gap_breaks(
        lte_t,
        lte_rsrp,
        gap_threshold_s=rsrp_gap_threshold_s,
    )
    ntn_t, ntn_rsrp = extract_numeric_series(
        ntn_rows, sample_rate_hz, "rsrp_raw", transform=lambda value: value - 141.0
    )
    ntn_t, ntn_rsrp = filter_series(ntn_t, ntn_rsrp, start_s, end_s)
    ntn_t, ntn_rsrp = insert_gap_breaks(
        ntn_t,
        ntn_rsrp,
        gap_threshold_s=ntn_rsrp_gap_threshold_s,
    )

    if show_state_shading:
        add_state_shading(ax, intervals, colors, alpha=0.06, show_labels=False)
    if lte_t.size:
        ax.plot(
            lte_t,
            lte_rsrp,
            marker="o",
            markersize=3.0,
            linewidth=1.2,
            color="#E45756",
            label="LTE-M RSRP [dBm]",
        )
    if ntn_t.size:
        ax.plot(
            ntn_t,
            ntn_rsrp,
            marker="s",
            markersize=3.0,
            linewidth=1.2,
            color="#B279A2",
            label="NTN RSRP estimated [dBm]",
        )
    ax.axhline(-110, color="black", linestyle=":", linewidth=1.1, label="Fallback threshold")
    ax.axhline(-120, color="0.35", linestyle="--", linewidth=1.0, label="-120 dBm reference")
    ax.set_xlim(start_s, end_s)
    ax.set_ylabel("RSRP [dBm]")
    if title:
        ax.set_title(title)
    ax.grid(True, linestyle="--", alpha=0.3)
    add_overview_event_markers(
        ax,
        events,
        colors,
        annotate=annotate_events,
        label_top_fraction=event_label_top_fraction,
    )
    ax.legend(loc="upper right", ncol=2)


def plot_current_panel(
    ax,
    intervals: list[Interval],
    colors: dict[str, str],
    events: list[tuple[float, str, str]],
    samples: Optional[np.memmap],
    sample_rate_hz: float,
    max_points: int,
    start_s: float,
    end_s: float,
    current_log_scale: bool,
    title: Optional[str] = None,
    show_state_shading: bool = True,
    annotate_events: bool = False,
) -> None:
    current_t, current_mA = downsample_current_for_plot(
        samples, sample_rate_hz, start_s, end_s, max_points
    )
    if current_t.size:
        if current_log_scale:
            current_plot_mA = np.clip(current_mA, 1e-3, None)
            ax.plot(current_t, current_plot_mA, color="#4C78A8", linewidth=0.7, label="Current")
            ax.set_yscale("log")
        else:
            ax.plot(current_t, current_mA, color="#4C78A8", linewidth=0.7, label="Current")
    if show_state_shading:
        add_state_shading(ax, intervals, colors, alpha=0.05, show_labels=False)
    add_overview_event_markers(ax, events, colors, annotate=annotate_events)
    ax.set_xlim(start_s, end_s)
    ax.set_ylabel("Current [mA]")
    if title:
        ax.set_title(title)
    ax.grid(True, linestyle="--", alpha=0.3)


def plot_state_timeline_panel(
    ax,
    intervals: list[Interval],
    colors: dict[str, str],
    events: list[tuple[float, str, str]],
    start_s: float,
    end_s: float,
    title: Optional[str] = None,
    show_event_markers: bool = True,
) -> None:
    plot_state_axis(ax, intervals, colors)
    if show_event_markers:
        add_overview_event_markers(ax, events, colors, annotate=False)
    ax.set_xlim(start_s, end_s)
    if title:
        ax.set_title(title)


def plot_combined_overview(
    plt,
    output_dir: Path,
    intervals: list[Interval],
    markers: list[Marker],
    samples: Optional[np.memmap],
    sample_rate_hz: float,
    max_points: int,
    conneval_rows: list[dict[str, str]],
    ntn_rows: list[dict[str, str]],
    start_s: float,
    end_s: float,
    current_log_scale: bool,
    rsrp_gap_threshold_s: float,
    ntn_rsrp_gap_threshold_s: float,
) -> None:
    interval_window = intervals_in_range(intervals, start_s, end_s)
    colors = state_colors(interval.state for interval in interval_window)
    overview_events = important_overview_events(interval_window, markers, start_s, end_s)

    fig, axes = plt.subplots(
        3,
        1,
        figsize=(12, 8.2),
        sharex=True,
        gridspec_kw={"height_ratios": [1.35, 1.05, 1.35]},
    )

    plot_rsrp_panel(
        axes[0],
        interval_window,
        colors,
        overview_events,
        conneval_rows,
        ntn_rows,
        sample_rate_hz,
        start_s,
        end_s,
        rsrp_gap_threshold_s,
        ntn_rsrp_gap_threshold_s,
        title="Fallback result overview",
    )
    plot_current_panel(
        axes[1],
        interval_window,
        colors,
        overview_events,
        samples,
        sample_rate_hz,
        max_points,
        start_s,
        end_s,
        current_log_scale,
    )
    plot_state_timeline_panel(
        axes[2],
        interval_window,
        colors,
        overview_events,
        start_s,
        end_s,
    )

    fig.tight_layout()
    fig.savefig(output_dir / "combined_result_overview.pdf", bbox_inches="tight")
    fig.savefig(output_dir / "combined_result_overview.png", dpi=180, bbox_inches="tight")
    plt.close(fig)

    plot_standalone_overview_panels(
        plt,
        output_dir,
        interval_window,
        colors,
        overview_events,
        samples,
        sample_rate_hz,
        max_points,
        conneval_rows,
        ntn_rows,
        start_s,
        end_s,
        current_log_scale,
        rsrp_gap_threshold_s,
        ntn_rsrp_gap_threshold_s,
    )


def save_pdf_png(fig, output_dir: Path, stem: str) -> None:
    fig.savefig(output_dir / f"{stem}.pdf", bbox_inches="tight")
    fig.savefig(output_dir / f"{stem}.png", dpi=180, bbox_inches="tight")


def plot_standalone_overview_panels(
    plt,
    output_dir: Path,
    interval_window: list[Interval],
    colors: dict[str, str],
    overview_events: list[tuple[float, str, str]],
    samples: Optional[np.memmap],
    sample_rate_hz: float,
    max_points: int,
    conneval_rows: list[dict[str, str]],
    ntn_rows: list[dict[str, str]],
    start_s: float,
    end_s: float,
    current_log_scale: bool,
    rsrp_gap_threshold_s: float,
    ntn_rsrp_gap_threshold_s: float,
) -> None:
    fig, ax = plt.subplots(figsize=(10, 4.8))
    plot_rsrp_panel(
        ax,
        interval_window,
        colors,
        overview_events,
        conneval_rows,
        ntn_rows,
        sample_rate_hz,
        start_s,
        end_s,
        rsrp_gap_threshold_s,
        ntn_rsrp_gap_threshold_s,
        title="RSRP overview during fallback",
        event_label_top_fraction=0.17,
    )
    ax.set_xlabel("Time [s]")
    fig.tight_layout()
    save_pdf_png(fig, output_dir, "result_rsrp_overview")
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(10, 4.4))
    plot_current_panel(
        ax,
        interval_window,
        colors,
        overview_events,
        samples,
        sample_rate_hz,
        max_points,
        start_s,
        end_s,
        current_log_scale,
        title="Current consumption during fallback",
        annotate_events=True,
    )
    ax.set_xlabel("Time [s]")
    fig.tight_layout()
    save_pdf_png(fig, output_dir, "result_current_overview")
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(10, 4.2))
    plot_state_timeline_panel(
        ax,
        interval_window,
        colors,
        overview_events,
        start_s,
        end_s,
        title="State timeline during fallback",
        show_event_markers=False,
    )
    fig.tight_layout()
    save_pdf_png(fig, output_dir, "result_state_timeline_overview")
    plt.close(fig)


def infer_probe_window(intervals: list[Interval], explicit_start: Optional[float], explicit_end: Optional[float]) -> tuple[float, float]:
    if explicit_start is not None and explicit_end is not None:
        return explicit_start, explicit_end
    probe = next((interval for interval in intervals if interval.state == "STATE_LTE_PROBE"), None)
    if probe is None:
        return (
            350.0 if explicit_start is None else explicit_start,
            480.0 if explicit_end is None else explicit_end,
        )
    recovery = next(
        (
            interval
            for interval in intervals
            if interval.state == "STATE_LTEM_CONNECTING" and interval.start_s >= probe.start_s
        ),
        None,
    )
    start = max(0.0, probe.start_s - 20.0) if explicit_start is None else explicit_start
    if explicit_end is not None:
        end = explicit_end
    elif recovery is not None:
        end = max(recovery.end_s + 20.0, recovery.start_s + 32.0)
    else:
        end = probe.end_s + 32.0
    return start, end


def add_probe_event_markers(ax, intervals: list[Interval], markers: list[Marker], start_s: float, end_s: float) -> None:
    events: list[tuple[float, str]] = []
    probe = next((item for item in intervals if item.state == "STATE_LTE_PROBE"), None)
    recovery = next(
        (
            item
            for item in intervals
            if item.state == "STATE_LTEM_CONNECTING"
            and item.start_s >= (probe.start_s if probe else start_s)
        ),
        None,
    )
    failed_marker = next(
        (
            marker
            for marker in markers
            if start_s <= marker.t_s <= end_s
            and marker.label in {"LTE probe failed", "LTE probe -> LTE-M connecting"}
        ),
        None,
    )

    if probe is not None and start_s <= probe.start_s <= end_s:
        events.append((probe.start_s, "LTE probe start"))
    if recovery is not None and start_s <= recovery.start_s <= end_s:
        recovery_label = "LTE-M connecting start"
        if failed_marker is not None and abs(failed_marker.t_s - recovery.start_s) <= 0.5:
            recovery_label = "LTE probe failed / LTE-M connecting"
        events.append((recovery.start_s, recovery_label))
    elif failed_marker is not None:
        events.append((failed_marker.t_s, failed_marker.label))

    used: set[str] = set()
    for t_s, label in events:
        if label in used:
            continue
        ax.axvline(
            t_s,
            color="0.25",
            linestyle="--",
            linewidth=1.0,
            alpha=0.75,
            label=label,
            zorder=1,
        )
        used.add(label)


def plot_lte_probe_zoom(
    plt,
    path: Path,
    intervals: list[Interval],
    markers: list[Marker],
    conneval_rows: list[dict[str, str]],
    ntn_rows: list[dict[str, str]],
    sample_rate_hz: float,
    start_s: float,
    end_s: float,
) -> None:
    lte_t, lte_rsrp = extract_numeric_series(conneval_rows, sample_rate_hz, "rsrp_dbm")
    lte_t, lte_rsrp = filter_series(lte_t, lte_rsrp, start_s, end_s)
    ntn_t, ntn_rsrp = extract_numeric_series(
        ntn_rows, sample_rate_hz, "rsrp_raw", transform=lambda value: value - 141.0
    )
    ntn_t, ntn_rsrp = filter_series(ntn_t, ntn_rsrp, start_s, end_s)
    interval_window = intervals_in_range(intervals, start_s, end_s)
    colors = state_colors(interval.state for interval in interval_window)

    fig, ax = plt.subplots(figsize=(10, 5.0))
    add_state_shading(ax, interval_window, colors, alpha=0.13)

    if lte_t.size:
        gap_threshold_s = 30.0
        lte_plot = lte_rsrp.copy()
        gaps = np.where(np.diff(lte_t) > gap_threshold_s)[0]
        for idx in gaps:
            lte_plot[idx + 1] = np.nan
        ax.plot(
            lte_t,
            lte_plot,
            marker="o",
            markersize=4,
            linewidth=1.5,
            alpha=0.95,
            color="#E45756",
            label="LTE-M RSRP [dBm]",
            zorder=3,
        )
    if ntn_t.size:
        ax.plot(
            ntn_t,
            ntn_rsrp,
            marker="s",
            markersize=4,
            linewidth=1.5,
            alpha=0.9,
            color="#B279A2",
            label="NTN RSRP estimated [dBm]",
            zorder=3,
        )

    ax.axhline(-110, color="black", linestyle=":", linewidth=1.1, label="Fallback threshold")
    ax.axhline(-120, color="0.35", linestyle="--", linewidth=1.0, label="-120 dBm reference")
    add_probe_event_markers(ax, interval_window, markers, start_s, end_s)
    ax.set_xlim(start_s, end_s)
    ax.set_xlabel("Time [s]")
    ax.set_ylabel("RSRP [dBm]")
    ax.set_title("LTE probe and recovery attempt with NTN context")
    ax.grid(True, linestyle="--", alpha=0.3)
    ax.legend(loc="upper left", bbox_to_anchor=(1.02, 1.0), borderaxespad=0)
    fig.tight_layout()
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    args = parse_args()
    if args.sample_rate_hz <= 0:
        raise SystemExit("--sample-rate-hz must be positive")
    if args.max_points <= 0:
        raise SystemExit("--max-points must be positive")

    processed_dir = Path(args.processed_dir)
    data_dir = Path(args.data_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    transitions = load_transitions(processed_dir / "state_transitions.csv", args.sample_rate_hz)
    clean_transitions, interval_source = choose_clean_transitions(transitions)
    markers = load_markers(processed_dir / "markers.csv", args.sample_rate_hz)
    conneval_rows = read_csv_rows(processed_dir / "conneval.csv")
    ntn_rows = read_csv_rows(processed_dir / "ntn_monitor.csv")

    samples = load_current_memmap(data_dir)
    current_duration_s = len(samples) / args.sample_rate_hz if samples is not None else None
    event_end_s = max([item.t_s for item in clean_transitions], default=0.0)
    timeline_end_s = current_duration_s if current_duration_s is not None else event_end_s

    intervals = build_intervals(clean_transitions, timeline_end_s)
    analysis_start_s, analysis_end_s, analysis_window_source = infer_analysis_window(
        clean_transitions,
        args.analysis_from_s,
        args.analysis_to_s,
    )
    if current_duration_s is not None:
        analysis_start_s, analysis_end_s = clamp_time_range(
            current_duration_s, analysis_start_s, analysis_end_s
        )
    selected_intervals = intervals_in_range(intervals, analysis_start_s, analysis_end_s)

    interval_power_rows = compute_power_rows(
        selected_intervals,
        samples,
        args.sample_rate_hz,
        args.voltage_v,
        analysis_start_s,
        analysis_end_s,
        analysis_window_source,
    )
    grouped_power_rows = group_power_rows(
        interval_power_rows,
        analysis_start_s,
        analysis_end_s,
        analysis_window_source,
    )
    write_power_summary_csv(output_dir / "per_interval_power_summary.csv", interval_power_rows)
    write_interval_power_summary_md(
        output_dir / "per_interval_power_summary.md",
        interval_power_rows,
        args.voltage_v,
        interval_source,
        analysis_start_s,
        analysis_end_s,
        analysis_window_source,
    )
    write_grouped_power_summary_csv(
        output_dir / "grouped_power_summary_selected_window.csv",
        grouped_power_rows,
    )
    write_grouped_power_summary_md(
        output_dir / "grouped_power_summary_selected_window.md",
        grouped_power_rows,
        args.voltage_v,
        analysis_start_s,
        analysis_end_s,
        analysis_window_source,
    )

    event_rows = filter_event_rows_to_window(
        build_event_duration_rows(clean_transitions, markers),
        analysis_start_s,
        analysis_end_s,
    )
    write_event_duration_summary(
        output_dir / "event_duration_summary.md",
        event_rows,
        analysis_start_s,
        analysis_end_s,
        analysis_window_source,
    )

    generated = [
        output_dir / "per_interval_power_summary.csv",
        output_dir / "per_interval_power_summary.md",
        output_dir / "grouped_power_summary_selected_window.csv",
        output_dir / "grouped_power_summary_selected_window.md",
        output_dir / "event_duration_summary.md",
    ]
    generated.extend(
        export_latex_tables(
            output_dir,
            interval_power_rows,
            grouped_power_rows,
            event_rows,
            clean_transitions,
            analysis_start_s,
            analysis_end_s,
        )
    )

    plt = setup_matplotlib()
    if plt is not None:
        plot_state_timeline(
            plt,
            output_dir / "state_timeline.pdf",
            intervals,
            args.min_label_duration_s,
        )
        generated.append(output_dir / "state_timeline.pdf")

        if current_duration_s is not None:
            overview_from_s = (
                args.overview_from_s
                if args.overview_from_s is not None
                else analysis_start_s
            )
            overview_to_s = (
                args.overview_to_s if args.overview_to_s is not None else analysis_end_s
            )
            overview_start, overview_end = clamp_time_range(
                current_duration_s, overview_from_s, overview_to_s
            )
        else:
            overview_start = (
                args.overview_from_s
                if args.overview_from_s is not None
                else analysis_start_s
            )
            overview_end = (
                args.overview_to_s if args.overview_to_s is not None else analysis_end_s
            )
        plot_combined_overview(
            plt,
            output_dir,
            intervals,
            markers,
            samples,
            args.sample_rate_hz,
            args.max_points,
            conneval_rows,
            ntn_rows,
            overview_start,
            overview_end,
            args.current_log_scale,
            args.rsrp_gap_threshold_s,
            args.ntn_rsrp_gap_threshold_s,
        )
        generated.extend(
            [
                output_dir / "combined_result_overview.pdf",
                output_dir / "combined_result_overview.png",
                output_dir / "result_rsrp_overview.pdf",
                output_dir / "result_rsrp_overview.png",
                output_dir / "result_current_overview.pdf",
                output_dir / "result_current_overview.png",
                output_dir / "result_state_timeline_overview.pdf",
                output_dir / "result_state_timeline_overview.png",
            ]
        )

        probe_start, probe_end = infer_probe_window(intervals, args.probe_from_s, args.probe_to_s)
        plot_lte_probe_zoom(
            plt,
            output_dir / "lte_probe_zoom.pdf",
            intervals,
            markers,
            conneval_rows,
            ntn_rows,
            args.sample_rate_hz,
            probe_start,
            probe_end,
        )
        generated.append(output_dir / "lte_probe_zoom.pdf")

    print("Generated result summary outputs:")
    for path in generated:
        print(f"- {path}")
    print(f"State intervals: {len(intervals)} from {interval_source}")
    print(
        "Analysis window: "
        f"{format_seconds(analysis_start_s)} s to {format_seconds(analysis_end_s)} s "
        f"({analysis_window_source})"
    )
    print(f"Per-interval power rows: {len(interval_power_rows)}")
    print(f"Grouped power rows: {len(grouped_power_rows)}")
    print(f"Event duration rows: {len(event_rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
