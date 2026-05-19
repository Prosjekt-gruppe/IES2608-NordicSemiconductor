from __future__ import annotations

import argparse
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd


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

IMPORTANT_PLOT_LABELS = {
    "Measurement start",
    "LTE-M connecting",
    "LTE-M fallback",
    "Starting NTN",
    "Switched to NTN",
    "LTE probe",
    "LTE-M recovered",
    "Stay on NTN",
    "Cloud connected",
    "Cloud disconnected",
    "Cloud connecting",
    "GNSS acquire",
    "GNSS fix",
    "PDN up",
    "PDN down",
}

SUMMARY_IGNORED_STATES = {
    "STATE_CONNECTED",
    "STATE_DISCONNECTED",
    "STATE_RUNNING",
}

DEFAULT_SAMPLE_RATE_HZ = 100_000.0
DEFAULT_VOLTAGE_MV = 3300.0
MARKER_DEDUPE_WINDOW_S = 2.0


@dataclass
class Marker:
    t_s: float
    label: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze PPK2 current with processed event markers."
    )
    parser.add_argument(
        "--data-dir",
        default="data/raw",
        help="Directory containing current_uA.bin.",
    )
    parser.add_argument(
        "--processed-dir",
        default="data/processed",
        help="Directory containing markers.csv and state_transitions.csv.",
    )
    parser.add_argument(
        "--sample-rate-hz",
        type=float,
        default=DEFAULT_SAMPLE_RATE_HZ,
        help="Sample rate in Hz.",
    )
    parser.add_argument(
        "--voltage-mv",
        type=float,
        default=DEFAULT_VOLTAGE_MV,
        help="Voltage for energy estimates (mV).",
    )
    parser.add_argument(
        "--plot-output",
        default=None,
        help="Save plot as PNG to this path.",
    )
    parser.add_argument(
        "--from-s",
        type=float,
        default=None,
        help="Start time in seconds for plotting/analysis window.",
    )
    parser.add_argument(
        "--to-s",
        type=float,
        default=None,
        help="End time in seconds for plotting/analysis window.",
    )
    parser.add_argument(
        "--max-points",
        type=int,
        default=200_000,
        help="Maximum number of plotted points after downsampling.",
    )
    parser.add_argument(
        "--summary-output",
        default=None,
        help="Write summary report to this path.",
    )
    parser.add_argument(
        "--summary-format",
        choices=("csv", "md"),
        default="md",
        help="Summary output format when --summary-output is set.",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="Do not show interactive plot window.",
    )
    parser.add_argument(
        "--summary-min-duration-s",
        type=float,
        default=0.0,
        help="Exclude state intervals shorter than this duration from state summary.",
    )
    parser.add_argument(
        "--plot-important-only",
        action="store_true",
        help="Only show high-level important markers in the plot.",
    )
    return parser.parse_args()


def state_label(state: str) -> str:
    return STATE_LABELS.get(state, state.replace("STATE_", "").replace("_", " ").title())


def load_samples(data_dir: Path) -> np.ndarray:
    sample_path = data_dir / "current_uA.bin"
    if not sample_path.exists():
        raise FileNotFoundError(f"Missing {sample_path}")
    return np.fromfile(sample_path, dtype=np.float32)


def read_csv_safe(path: Path) -> pd.DataFrame:
    try:
        return pd.read_csv(path)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()


def ensure_timestamp(df: pd.DataFrame, sample_rate: float) -> tuple[pd.DataFrame, str]:
    df = df.copy()
    if df.empty:
        df["t_s"] = []
        return df, "t_s"

    if "t_sample_s" in df.columns:
        t_col = "t_sample_s"
    elif "t_s" in df.columns:
        t_col = "t_s"
    elif "sample_idx" in df.columns:
        df["t_s"] = pd.to_numeric(df["sample_idx"], errors="coerce") / sample_rate
        t_col = "t_s"
    else:
        df["t_s"] = float("nan")
        t_col = "t_s"

    df[t_col] = pd.to_numeric(df[t_col], errors="coerce")
    return df, t_col


def clamp_time_range(
    total_duration_s: float,
    from_s: Optional[float],
    to_s: Optional[float],
) -> tuple[float, float]:
    start_s = 0.0 if from_s is None else float(from_s)
    end_s = total_duration_s if to_s is None else float(to_s)

    if not math.isfinite(start_s):
        start_s = 0.0
    if not math.isfinite(end_s):
        end_s = total_duration_s

    start_s = max(0.0, min(start_s, total_duration_s))
    end_s = max(0.0, min(end_s, total_duration_s))
    if end_s < start_s:
        end_s = start_s

    return start_s, end_s


def format_seconds(value: float) -> str:
    if not math.isfinite(value):
        return "n/a"
    text = f"{value:.2f}"
    return text.rstrip("0").rstrip(".")


def normalize_state(value: object) -> Optional[str]:
    if value is None:
        return None
    if isinstance(value, float) and math.isnan(value):
        return None
    text = str(value).strip()
    if not text or text.lower() == "nan":
        return None
    return text


def build_markers(df: pd.DataFrame, t_col: str, important_only: bool) -> list[Marker]:
    if df.empty:
        return []

    label_col = None
    for col in ("label", "key", "message"):
        if col in df.columns:
            label_col = col
            break

    markers: list[Marker] = []
    for _, row in df.iterrows():
        t_value = row.get(t_col)
        if not isinstance(t_value, (float, int)) or not math.isfinite(float(t_value)):
            continue
        label = None
        if label_col is not None:
            label = row.get(label_col)
        if label is None or (isinstance(label, float) and math.isnan(label)):
            label = "Marker"
        label = str(label).strip()
        if not label:
            continue
        if important_only and label not in IMPORTANT_PLOT_LABELS:
            continue
        markers.append(Marker(t_s=float(t_value), label=label))

    return markers


def filter_markers_for_range(
    markers: list[Marker],
    start_s: float,
    end_s: float,
    dedupe_window_s: float,
) -> list[Marker]:
    if not markers:
        return []

    in_range = [m for m in markers if start_s <= m.t_s <= end_s]
    in_range.sort(key=lambda item: item.t_s)

    output: list[Marker] = []
    last_label_time: dict[str, float] = {}
    for marker in in_range:
        last_time = last_label_time.get(marker.label)
        if last_time is not None and marker.t_s - last_time <= dedupe_window_s:
            continue
        last_label_time[marker.label] = marker.t_s
        output.append(marker)

    return output


def build_state_intervals(
    df: pd.DataFrame,
    t_col: str,
    ignored_states: set[str],
) -> list[tuple[str, float, float]]:
    if df.empty:
        return []

    transitions: list[tuple[float, str]] = []
    for _, row in df.iterrows():
        t_value = row.get(t_col)
        if not isinstance(t_value, (float, int)) or not math.isfinite(float(t_value)):
            continue
        to_state = normalize_state(row.get("to_state"))
        if to_state is None:
            continue
        transitions.append((float(t_value), to_state))

    transitions.sort(key=lambda item: item[0])

    intervals: list[tuple[str, float, float]] = []
    current_state: Optional[str] = None
    current_start: Optional[float] = None

    for t_s, to_state in transitions:
        if current_state is not None and current_start is not None:
            if t_s > current_start:
                intervals.append((current_state, current_start, t_s))
            current_state = None
            current_start = None

        if to_state in ignored_states:
            continue

        current_state = to_state
        current_start = t_s

    return intervals


def clip_intervals_to_range(
    intervals: list[tuple[str, float, float]],
    start_s: float,
    end_s: float,
) -> list[tuple[str, float, float]]:
    clipped: list[tuple[str, float, float]] = []
    for state, interval_start, interval_end in intervals:
        overlap_start = max(interval_start, start_s)
        overlap_end = min(interval_end, end_s)
        if overlap_end > overlap_start:
            clipped.append((state, overlap_start, overlap_end))
    return clipped


def compute_charge_metrics(samples: np.ndarray, sample_rate: float) -> dict[str, float]:
    if sample_rate <= 0 or len(samples) == 0:
        return {"charge_C": 0.0, "charge_mC": 0.0, "charge_uAh": 0.0}
    total_uA = float(np.nansum(samples))
    charge_c = total_uA * 1e-6 / sample_rate
    return {
        "charge_C": charge_c,
        "charge_mC": charge_c * 1e3,
        "charge_uAh": charge_c * 1e6 / 3600.0,
    }


def compute_state_stats(
    intervals: list[tuple[str, float, float]],
    samples: np.ndarray,
    sample_rate: float,
    voltage_mv: float,
    summary_min_duration_s: float,
) -> pd.DataFrame:
    rows: list[dict[str, float | str]] = []
    for state, start_s, end_s in intervals:
        duration_s = end_s - start_s
        if duration_s <= 0 or duration_s < summary_min_duration_s:
            continue
        start_idx = max(0, int(math.floor(start_s * sample_rate)))
        end_idx = min(len(samples), int(math.floor(end_s * sample_rate)))
        if end_idx <= start_idx:
            continue
        slice_samples = samples[start_idx:end_idx]
        charge = compute_charge_metrics(slice_samples, sample_rate)
        avg_uA = charge["charge_C"] * 1e6 / duration_s
        min_uA = float(np.nanmin(slice_samples)) if len(slice_samples) else float("nan")
        max_uA = float(np.nanmax(slice_samples)) if len(slice_samples) else float("nan")
        rows.append(
            {
                "state": state_label(state),
                "duration_s": duration_s,
                "avg_uA": avg_uA,
                "min_uA": min_uA,
                "max_uA": max_uA,
                "charge_C": charge["charge_C"],
            }
        )

    if not rows:
        return pd.DataFrame(
            columns=[
                "state",
                "duration_s",
                "avg_uA",
                "min_uA",
                "max_uA",
                "charge_C",
                "charge_mC",
                "charge_uAh",
                "energy_uWh",
            ]
        )

    df = pd.DataFrame(rows)
    grouped = df.groupby("state", as_index=False).agg(
        {
            "duration_s": "sum",
            "min_uA": "min",
            "max_uA": "max",
            "charge_C": "sum",
        }
    )
    grouped["avg_uA"] = grouped.apply(
        lambda row: row["charge_C"] * 1e6 / row["duration_s"]
        if row["duration_s"] > 0
        else float("nan"),
        axis=1,
    )
    grouped["charge_mC"] = grouped["charge_C"] * 1e3
    grouped["charge_uAh"] = grouped["charge_C"] * 1e6 / 3600.0
    if voltage_mv and math.isfinite(voltage_mv):
        grouped["energy_uWh"] = grouped["charge_C"] * voltage_mv / 3.6
    else:
        grouped["energy_uWh"] = float("nan")

    ordered_columns = [
        "state",
        "duration_s",
        "avg_uA",
        "min_uA",
        "max_uA",
        "charge_C",
        "charge_mC",
        "charge_uAh",
        "energy_uWh",
    ]
    return grouped[ordered_columns]


def build_transitions_table(
    df: pd.DataFrame,
    t_col: str,
    start_s: Optional[float] = None,
    end_s: Optional[float] = None,
) -> pd.DataFrame:
    if df.empty or "to_state" not in df.columns:
        return pd.DataFrame()

    mask = df["to_state"].astype(str).str.strip().ne("")
    subset = df.loc[mask].copy()
    if subset.empty:
        return pd.DataFrame()

    columns = ["from_state", "to_state", "event_kind", "message"]
    present = [col for col in columns if col in subset.columns]
    subset = subset[[t_col] + present].copy()
    subset = subset.rename(columns={t_col: "t_s"})
    subset["t_s"] = pd.to_numeric(subset["t_s"], errors="coerce")
    subset = subset[np.isfinite(subset["t_s"])]

    if start_s is not None or end_s is not None:
        start_value = -math.inf if start_s is None else float(start_s)
        end_value = math.inf if end_s is None else float(end_s)
        subset = subset[(subset["t_s"] >= start_value) & (subset["t_s"] <= end_value)]

    subset = subset.sort_values("t_s")
    return subset


def write_summary_markdown(
    path: Path,
    overall: dict[str, str],
    state_stats: pd.DataFrame,
    transitions: pd.DataFrame,
) -> None:
    lines = ["# PPK Analysis Summary", "", "## Overall", ""]
    for key, value in overall.items():
        lines.append(f"- {key}: {value}")

    if not state_stats.empty:
        lines.append("")
        lines.append("## State Summary")
        lines.append("")
        lines.append(state_stats.to_markdown(index=False))

    if not transitions.empty:
        lines.append("")
        lines.append("## Transitions")
        lines.append("")
        lines.append(transitions.to_markdown(index=False))

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_summary_csv(
    path: Path,
    overall: dict[str, str],
    state_stats: pd.DataFrame,
    transitions: pd.DataFrame,
) -> None:
    with path.open("w", encoding="utf-8") as handle:
        handle.write("section,key,value\n")
        for key, value in overall.items():
            handle.write(f"overall,{key},{value}\n")

    if not state_stats.empty:
        state_stats.to_csv(path.with_suffix(".states.csv"), index=False)
    if not transitions.empty:
        transitions.to_csv(path.with_suffix(".transitions.csv"), index=False)


def plot_capture(
    t: np.ndarray,
    samples: np.ndarray,
    markers: list[Marker],
    output_path: Optional[Path],
    show_plot: bool,
    start_s: float,
    end_s: float,
) -> None:
    import matplotlib.pyplot as plt

    plt.figure()
    plt.plot(t, samples, linewidth=0.8)

    plot_top = float(np.nanmax(samples)) if len(samples) else 0.0

    for marker in markers:
        plt.axvline(marker.t_s, linestyle="--", linewidth=0.8)
        plt.text(
            marker.t_s,
            plot_top,
            marker.label,
            rotation=90,
            fontsize=8,
            va="top",
            bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.75, "pad": 1.0},
        )

    plt.xlabel("Time [s]")
    plt.ylabel("Current [uA]")
    title_range = f"{format_seconds(start_s)} s to {format_seconds(end_s)} s"
    plt.title(f"PPK current with processed markers ({title_range})")
    plt.tight_layout()

    if output_path:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        plt.savefig(output_path, dpi=200)

    if show_plot:
        plt.show()
    else:
        plt.close()


def main() -> None:
    args = parse_args()

    if args.sample_rate_hz <= 0:
        print("Error: --sample-rate-hz must be positive.", file=sys.stderr)
        sys.exit(1)
    if args.max_points <= 0:
        print("Error: --max-points must be positive.", file=sys.stderr)
        sys.exit(1)

    data_dir = Path(args.data_dir)
    processed_dir = Path(args.processed_dir)
    markers_path = processed_dir / "markers.csv"
    transitions_path = processed_dir / "state_transitions.csv"

    if not markers_path.exists():
        print(f"Error: missing markers.csv at {markers_path}", file=sys.stderr)
        sys.exit(1)
    if not transitions_path.exists():
        print(f"Error: missing state_transitions.csv at {transitions_path}", file=sys.stderr)
        sys.exit(1)

    try:
        samples = load_samples(data_dir)
    except FileNotFoundError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)

    total_duration_s = len(samples) / args.sample_rate_hz if args.sample_rate_hz > 0 else 0.0
    start_s, end_s = clamp_time_range(total_duration_s, args.from_s, args.to_s)
    start_idx = int(start_s * args.sample_rate_hz)
    end_idx = int(end_s * args.sample_rate_hz)
    start_idx = max(0, min(start_idx, len(samples)))
    end_idx = max(start_idx, min(end_idx, len(samples)))
    window_samples = samples[start_idx:end_idx]

    markers_df = read_csv_safe(markers_path)
    transitions_df = read_csv_safe(transitions_path)

    markers_df, markers_t_col = ensure_timestamp(markers_df, args.sample_rate_hz)
    transitions_df, transitions_t_col = ensure_timestamp(transitions_df, args.sample_rate_hz)

    plot_markers = build_markers(
        markers_df,
        markers_t_col,
        important_only=args.plot_important_only,
    )
    plot_markers = filter_markers_for_range(
        plot_markers,
        start_s,
        end_s,
        MARKER_DEDUPE_WINDOW_S,
    )

    intervals = build_state_intervals(
        transitions_df,
        transitions_t_col,
        ignored_states=SUMMARY_IGNORED_STATES,
    )
    intervals = clip_intervals_to_range(intervals, start_s, end_s)

    state_stats = compute_state_stats(
        intervals,
        samples,
        args.sample_rate_hz,
        args.voltage_mv,
        args.summary_min_duration_s,
    )

    transitions_table = build_transitions_table(
        transitions_df,
        transitions_t_col,
        start_s=start_s,
        end_s=end_s,
    )

    duration_s = len(window_samples) / args.sample_rate_hz if args.sample_rate_hz > 0 else 0.0
    charge_overall = compute_charge_metrics(window_samples, args.sample_rate_hz)

    overall = {
        "samples": str(len(window_samples)),
        "duration_s": f"{duration_s:.2f}",
        "window_start_s": format_seconds(start_s),
        "window_end_s": format_seconds(end_s),
        "avg_uA": f"{float(np.nanmean(window_samples)):.2f}" if len(window_samples) else "n/a",
        "min_uA": f"{float(np.nanmin(window_samples)):.2f}" if len(window_samples) else "n/a",
        "max_uA": f"{float(np.nanmax(window_samples)):.2f}" if len(window_samples) else "n/a",
        "sample_rate_hz": f"{args.sample_rate_hz:.1f}",
        "voltage_mv": f"{args.voltage_mv:.1f}",
        "charge_C": f"{charge_overall['charge_C']:.6f}",
        "charge_mC": f"{charge_overall['charge_mC']:.3f}",
        "charge_uAh": f"{charge_overall['charge_uAh']:.3f}",
    }

    print("Summary:")
    print(f"- samples: {overall['samples']}")
    print(f"- duration_s: {overall['duration_s']}")
    print(f"- window_start_s: {overall['window_start_s']}")
    print(f"- window_end_s: {overall['window_end_s']}")
    print(f"- avg_uA: {overall['avg_uA']}")
    print(f"- min_uA: {overall['min_uA']}")
    print(f"- max_uA: {overall['max_uA']}")
    print(f"- state_count: {len(state_stats)}")

    if args.summary_output:
        summary_path = Path(args.summary_output)
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        if args.summary_format == "csv":
            write_summary_csv(summary_path, overall, state_stats, transitions_table)
        else:
            write_summary_markdown(summary_path, overall, state_stats, transitions_table)

    if args.plot_output or not args.no_show:
        t = (
            np.arange(start_idx, end_idx) / args.sample_rate_hz
            if args.sample_rate_hz > 0
            else np.arange(len(window_samples))
        )
        plot_samples = window_samples
        if len(plot_samples) > args.max_points:
            step = int(math.ceil(len(plot_samples) / args.max_points))
            plot_samples = plot_samples[::step]
            t = t[::step]
        plot_capture(
            t,
            plot_samples,
            plot_markers,
            Path(args.plot_output) if args.plot_output else None,
            show_plot=not args.no_show,
            start_s=start_s,
            end_s=end_s,
        )


if __name__ == "__main__":
    main()
