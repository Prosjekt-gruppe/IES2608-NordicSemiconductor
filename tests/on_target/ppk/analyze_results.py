from __future__ import annotations

import argparse
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

import numpy as np
import pandas as pd


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
TRANSITION_RE = re.compile(r"TRANSITION:\s*(STATE_[A-Z0-9_]+)\s*->\s*(STATE_[A-Z0-9_]+)")
FIELD_LOG_STATE_RE = re.compile(
    r"Field log state #\d+:\s*(STATE_[A-Z0-9_]+)\s*->\s*(STATE_[A-Z0-9_]+)"
)
ENTER_STATE_RE = re.compile(r"ENTER:\s*(STATE_[A-Z0-9_]+)")
EXIT_STATE_RE = re.compile(r"EXIT:\s*(STATE_[A-Z0-9_]+)")

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

PLOT_IGNORED_STATES = {
    "STATE_DISCONNECTED",
    "STATE_CONNECTED",
    "STATE_RUNNING",
}

SUMMARY_IGNORED_STATES = {
    "STATE_CONNECTED",
    "STATE_DISCONNECTED",
    "STATE_RUNNING",
}

DEFAULT_SAMPLE_RATE_HZ = 100_000.0
DEFAULT_VOLTAGE_MV = 3300.0


@dataclass
class EventMarker:
    t_s: float
    label: str
    key: str
    message: str
    from_state: Optional[str] = None
    to_state: Optional[str] = None


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze PPK2 current with UART/field-log event markers."
    )
    parser.add_argument(
        "--data-dir",
        default="data/raw",
        help="Directory containing current_uA.bin and ppk_events.csv.",
    )
    parser.add_argument(
        "--sample-rate-hz",
        type=float,
        default=None,
        help="Override sample rate in Hz (auto-detected if omitted).",
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
        "--marker-dedupe-window-s",
        type=float,
        default=2.0,
        help="Suppress repeated visual markers with the same label within this time window.",
    )
    parser.add_argument(
        "--plot-important-only",
        action="store_true",
        help="Only show high-level important markers in the plot.",
    )
    parser.add_argument(
        "--summary-min-duration-s",
        type=float,
        default=0.0,
        help="Exclude state intervals shorter than this duration from state summary.",
    )
    return parser.parse_args()


def state_label(state: str) -> str:
    return STATE_LABELS.get(state, state.replace("STATE_", "").replace("_", " ").title())


def transition_label(from_state: Optional[str], to_state: Optional[str]) -> str:
    if to_state == "STATE_NTN_CONNECTED":
        return "Switched to NTN"
    if to_state == "STATE_NTN_CONNECTING":
        return "Starting NTN"
    if from_state == "STATE_LTEM_CONNECTED" and to_state == "STATE_BACKOFF":
        return "LTE-M fallback"
    if to_state == "STATE_LTEM_CONNECTED":
        return "LTE-M connected"
    if to_state == "STATE_GNSS_ACQUIRE":
        return "GNSS acquire"
    if to_state == "STATE_LTE_PROBE":
        return "LTE probe"
    if to_state == "STATE_CLOUD_CONNECTING":
        return "Cloud connecting"
    if to_state and from_state:
        return f"{state_label(from_state)} -> {state_label(to_state)}"
    if to_state:
        return state_label(to_state)
    return "State transition"


def detect_sample_rate(events: pd.DataFrame) -> float:
    if "sample_idx" not in events.columns or "t_ns" not in events.columns:
        return DEFAULT_SAMPLE_RATE_HZ

    idx = pd.to_numeric(events["sample_idx"], errors="coerce")
    t_ns = pd.to_numeric(events["t_ns"], errors="coerce")
    deltas = idx.diff() / (t_ns.diff() / 1e9)
    valid = deltas[(deltas > 1.0) & np.isfinite(deltas)]
    if len(valid) < 5:
        return DEFAULT_SAMPLE_RATE_HZ
    return float(np.median(valid))


def load_samples(data_dir: Path) -> np.ndarray:
    sample_path = data_dir / "current_uA.bin"
    if not sample_path.exists():
        raise FileNotFoundError(f"Missing {sample_path}")
    return np.fromfile(sample_path, dtype=np.float32)


def load_events(data_dir: Path) -> pd.DataFrame:
    event_path = data_dir / "ppk_events.csv"
    if not event_path.exists():
        raise FileNotFoundError(f"Missing {event_path}")
    events = pd.read_csv(event_path)
    if "message" in events.columns:
        events["message"] = events["message"].astype(str).map(strip_ansi)
    return events


def parse_state_transition(message: str) -> tuple[Optional[str], Optional[str]]:
    match = FIELD_LOG_STATE_RE.search(message) or TRANSITION_RE.search(message)
    if match:
        return match.group(1), match.group(2)

    enter_match = ENTER_STATE_RE.search(message)
    if enter_match:
        return None, enter_match.group(1)

    exit_match = EXIT_STATE_RE.search(message)
    if exit_match:
        return exit_match.group(1), None

    return None, None


def event_marker(message: str) -> tuple[Optional[str], Optional[str], Optional[str], Optional[str]]:
    from_state, to_state = parse_state_transition(message)
    if from_state or to_state:
        key = f"{from_state or 'UNKNOWN'}->{to_state or 'UNKNOWN'}"
        return transition_label(from_state, to_state), key, from_state, to_state

    msg = message.lower()
    rules = [
        ("trying to connect ntn", "Starting NTN", "trying-ntn"),
        ("ntn connect started", "Starting NTN", "ntn-connect"),
        ("ntn registered on network", "Switched to NTN", "ntn-registered"),
        ("ntn registered ok", "Switched to NTN", "ntn-registered"),
        ("lte registered on network", "LTE-M connected", "lte-registered"),
        ("lte poor", "LTE-M poor", "lte-poor"),
        ("entering backoff", "Backoff", "backoff"),
        ("retry lte connect", "Retry LTE-M", "retry-lte"),
        ("no gnss fix", "GNSS before NTN", "gnss-needed"),
        ("gnss acquire started", "GNSS acquire", "gnss-start"),
        ("gnss fix ok", "GNSS fix", "gnss-fix"),
        ("cloud ready", "Cloud connected", "cloud-ready"),
        ("cloud transport disconnected", "Cloud disconnected", "cloud-down"),
        ("cloud connection failed", "Cloud connect failed", "cloud-fail"),
        ("cloud connecting", "Cloud connecting", "cloud-connecting"),
        ("cloud connection started", "Cloud connecting", "cloud-connecting"),
        ("switch: sending xsystemmode ntn", "Modem mode NTN", "modem-mode-ntn"),
        ("switch: sending xsystemmode tn", "Modem mode LTE-M", "modem-mode-tn"),
        ("lte probe: tn good", "LTE-M recovered", "lte-recovered"),
        ("lte probe: tn still bad", "Stay on NTN", "stay-ntn"),
        ("lte probe lost pdn", "LTE probe failed", "probe-fail"),
        ("lte probe: tn good -> udp test", "LTE-M recovered", "lte-recovered"),
        ("lte probe: tn still bad -> returning to ntn", "Stay on NTN", "stay-ntn"),
        ("dut_power_on", "DUT power on", "dut-power-on"),
        ("ampere_mode_pass_through_on", "Measurement start", "measurement-start"),
        ("dut_output_switch_disabled", "Output switch disabled", "output-switch-disabled"),
        ("pdn up", "PDN up", "pdn-up"),
        ("pdn down", "PDN down", "pdn-down"),
    ]

    for needle, label, key in rules:
        if needle in msg:
            return label, key, None, None

    return None, None, None, None


def build_markers(
    events: pd.DataFrame,
    t_column: str,
) -> list[EventMarker]:
    markers: list[EventMarker] = []

    for _, row in events.iterrows():
        msg = row.get("message", "")
        label, key, from_state, to_state = event_marker(str(msg))
        if not label or not key:
            continue
        if from_state is not None and to_state is None:
            continue
        if (from_state in SUMMARY_IGNORED_STATES) or (to_state in SUMMARY_IGNORED_STATES):
            continue
        x = float(row[t_column])
        if not math.isfinite(x):
            continue
        markers.append(EventMarker(t_s=x, label=label, key=key, message=str(msg),
                                   from_state=from_state, to_state=to_state))

    return markers


def build_plot_markers(
    markers: Iterable[EventMarker],
    important_only: bool,
    dedupe_window_s: float,
) -> list[EventMarker]:
    filtered = [
        marker
        for marker in markers
        if not ((marker.from_state in PLOT_IGNORED_STATES) or (marker.to_state in PLOT_IGNORED_STATES))
    ]
    if important_only:
        filtered = [marker for marker in filtered if marker.label in IMPORTANT_PLOT_LABELS]

    output: list[EventMarker] = []
    last_marker_by_label: dict[str, float] = {}
    last_to_state: Optional[str] = None

    for marker in filtered:
        if marker.to_state is not None and marker.to_state == last_to_state:
            continue
        previous_x = last_marker_by_label.get(marker.label)
        if previous_x is not None and abs(marker.t_s - previous_x) <= dedupe_window_s:
            continue

        if marker.to_state is not None:
            last_to_state = marker.to_state
        last_marker_by_label[marker.label] = marker.t_s
        output.append(marker)

    return output


def build_state_intervals(
    markers: Iterable[EventMarker],
    ignored_states: Optional[set[str]] = None,
) -> list[tuple[str, float, float]]:
    transitions = [m for m in markers if m.to_state]
    transitions.sort(key=lambda item: item.t_s)

    if not transitions:
        return []

    intervals: list[tuple[str, float, float]] = []
    ignored = ignored_states or set()
    current_state: Optional[str] = None
    current_start: float = 0.0

    for marker in transitions:
        if marker.to_state is None or marker.to_state in ignored:
            continue

        if current_state is None:
            current_state = marker.to_state
            current_start = marker.t_s
            continue

        if marker.t_s < current_start:
            continue

        if current_state != marker.to_state:
            intervals.append((current_state, current_start, marker.t_s))
            current_state = marker.to_state
            current_start = marker.t_s

    return intervals


def compute_state_stats(
    intervals: list[tuple[str, float, float]],
    samples: np.ndarray,
    sample_rate: float,
    voltage_mv: float,
    include_clipped: bool,
    summary_min_duration_s: float,
) -> pd.DataFrame:
    rows = []
    for state, start_s, end_s in intervals:
        if state in SUMMARY_IGNORED_STATES:
            continue
        if end_s <= start_s:
            continue
        start_idx = max(0, int(math.floor(start_s * sample_rate)))
        end_idx = min(len(samples), int(math.floor(end_s * sample_rate)))
        if end_idx <= start_idx:
            continue
        duration_s = end_s - start_s
        if duration_s < summary_min_duration_s:
            continue
        slice_samples = samples[start_idx:end_idx]
        charge = compute_charge_metrics(slice_samples, sample_rate, include_clipped)
        avg_uA = (
            charge["charge_C"] * 1e6 / duration_s
            if duration_s > 0
            else float("nan")
        )
        min_uA = float(np.nanmin(slice_samples)) if len(slice_samples) else float("nan")
        max_uA = float(np.nanmax(slice_samples)) if len(slice_samples) else float("nan")
        energy_uwh = None
        if voltage_mv and not math.isnan(avg_uA):
            energy_uwh = avg_uA * voltage_mv * duration_s / 3_600_000.0

        rows.append(
            {
                "state": state_label(state),
                "duration_s": duration_s,
                "avg_uA": avg_uA,
                "min_uA": min_uA,
                "max_uA": max_uA,
                "energy_uWh": energy_uwh,
                "charge_C": charge["charge_C"],
                "charge_C_clipped": charge.get("charge_C_clipped"),
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
                "energy_uWh",
                "charge_C",
                "charge_mC",
                "charge_uAh",
                "charge_C_clipped",
            ]
        )

    df = pd.DataFrame(rows)
    grouped = df.groupby("state", as_index=False).agg(
        {
            "duration_s": "sum",
            "min_uA": "min",
            "max_uA": "max",
            "energy_uWh": "sum",
            "charge_C": "sum",
            "charge_C_clipped": "sum",
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
    ordered_columns = [
        "state",
        "duration_s",
        "avg_uA",
        "min_uA",
        "max_uA",
        "energy_uWh",
        "charge_C",
        "charge_mC",
        "charge_uAh",
        "charge_C_clipped",
    ]
    return grouped[[col for col in ordered_columns if col in grouped.columns]]


def state_stats_consistency_warnings(state_stats: pd.DataFrame) -> list[str]:
    warnings: list[str] = []
    if state_stats.empty:
        return warnings

    for _, row in state_stats.iterrows():
        duration_s = float(row.get("duration_s", 0.0))
        avg_uA = float(row.get("avg_uA", float("nan")))
        charge_C = float(row.get("charge_C", float("nan")))
        if duration_s <= 0 or not math.isfinite(avg_uA) or not math.isfinite(charge_C):
            continue
        expected = avg_uA * 1e-6 * duration_s
        tolerance = max(1e-6, abs(charge_C) * 1e-3)
        if abs(expected - charge_C) > tolerance:
            state = row.get("state", "<unknown>")
            warnings.append(
                f"State summary mismatch for {state}: charge_C={charge_C:.6f}, "
                f"expected={expected:.6f} (duration_s={duration_s:.3f}, avg_uA={avg_uA:.2f})."
            )

    return warnings


def compute_charge_metrics(
    samples: np.ndarray,
    sample_rate: float,
    include_clipped: bool,
) -> dict[str, float]:
    if sample_rate <= 0 or len(samples) == 0:
        output = {
            "charge_C": 0.0,
            "charge_mC": 0.0,
            "charge_uAh": 0.0,
        }
        if include_clipped:
            output["charge_C_clipped"] = 0.0
        return output

    total_uA = float(np.nansum(samples))
    charge_c = total_uA * 1e-6 / sample_rate
    output = {
        "charge_C": charge_c,
        "charge_mC": charge_c * 1e3,
        "charge_uAh": charge_c * 1e6 / 3600.0,
    }

    if include_clipped:
        clipped_uA = float(np.nansum(np.clip(samples, 0.0, None)))
        output["charge_C_clipped"] = clipped_uA * 1e-6 / sample_rate

    return output


def alignment_warnings(events: pd.DataFrame, samples_len: int, sample_rate: float) -> list[str]:
    warnings: list[str] = []

    if "sample_idx" in events.columns:
        sample_idx = pd.to_numeric(events["sample_idx"], errors="coerce")
        if sample_idx.min() < 0:
            warnings.append("Found negative sample_idx values.")
        if sample_idx.max() >= samples_len:
            warnings.append("Some event sample_idx values exceed sample count.")

    if "t_ns" in events.columns and "sample_idx" in events.columns:
        t_ns = pd.to_numeric(events["t_ns"], errors="coerce")
        sample_idx = pd.to_numeric(events["sample_idx"], errors="coerce")
        t_sample = sample_idx / sample_rate
        t_host = t_ns / 1e9
        delta = t_sample - t_host
        valid = delta[np.isfinite(delta)]
        if len(valid) >= 10:
            spread = float(np.nanstd(valid))
            if spread > 0.5:
                warnings.append(
                    f"Event time alignment jitter is high (std={spread:.2f} s)."
                )

    return warnings


def write_summary_markdown(
    path: Path,
    overall: dict[str, float | str],
    state_stats: pd.DataFrame,
    transitions: pd.DataFrame,
    warnings: list[str],
) -> None:
    lines = ["# PPK Analysis Summary", "", "## Overall", ""]
    for key, value in overall.items():
        lines.append(f"- {key}: {value}")

    if not state_stats.empty:
        lines.append("\n## State Summary\n")
        lines.append(state_stats.to_markdown(index=False))

    if not transitions.empty:
        lines.append("\n## Transition Timestamps\n")
        lines.append(transitions.to_markdown(index=False))

    if warnings:
        lines.append("\n## Warnings\n")
        for warning in warnings:
            lines.append(f"- {warning}")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_summary_csv(
    path: Path,
    state_stats: pd.DataFrame,
    transitions: pd.DataFrame,
    overall: dict[str, float | str],
    warnings: list[str],
) -> None:
    with path.open("w", encoding="utf-8") as handle:
        handle.write("section,key,value\n")
        for key, value in overall.items():
            handle.write(f"overall,{key},{value}\n")
        for warning in warnings:
            handle.write(f"warning,warning,{warning}\n")

    if not state_stats.empty:
        state_stats.to_csv(path.with_suffix(".states.csv"), index=False)
    if not transitions.empty:
        transitions.to_csv(path.with_suffix(".transitions.csv"), index=False)


def plot_capture(
    samples: np.ndarray,
    sample_rate: float,
    markers: list[EventMarker],
    output_path: Optional[Path],
    show_plot: bool,
) -> None:
    import matplotlib.pyplot as plt

    t = np.arange(len(samples)) / sample_rate
    plt.figure()
    plt.plot(t, samples, linewidth=0.8)

    plot_top = np.nanmax(samples) if len(samples) else 0.0

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
    plt.title("PPK current with UART events")
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
    data_dir = Path(args.data_dir)
    samples = load_samples(data_dir)
    events = load_events(data_dir)

    sample_rate = args.sample_rate_hz or detect_sample_rate(events)

    if "sample_idx" in events.columns:
        events["t_sample_s"] = pd.to_numeric(events["sample_idx"], errors="coerce") / sample_rate
    if "t_ns" in events.columns:
        events["t_host_s"] = pd.to_numeric(events["t_ns"], errors="coerce") / 1e9

    if "t_sample_s" in events.columns:
        events["t_s"] = events["t_sample_s"]
    elif "t_host_s" in events.columns:
        events["t_s"] = events["t_host_s"]
    else:
        events["t_s"] = float("nan")

    all_markers = build_markers(events, "t_s")
    plot_markers = build_plot_markers(
        all_markers,
        important_only=args.plot_important_only,
        dedupe_window_s=args.marker_dedupe_window_s,
    )
    intervals = build_state_intervals(all_markers, ignored_states=SUMMARY_IGNORED_STATES)
    has_negative_samples = bool(np.any(samples < 0)) if len(samples) else False
    state_stats = compute_state_stats(
        intervals,
        samples,
        sample_rate,
        args.voltage_mv,
        include_clipped=has_negative_samples,
        summary_min_duration_s=args.summary_min_duration_s,
    )
    if not has_negative_samples:
        state_stats = state_stats.drop(columns=["charge_C_clipped"], errors="ignore")

    transitions = pd.DataFrame(
        [
            {
                "t_s": marker.t_s,
                "label": marker.label,
                "from": marker.from_state,
                "to": marker.to_state,
                "message": marker.message,
            }
            for marker in all_markers
            if marker.from_state or marker.to_state
        ]
    )

    duration_s = len(samples) / sample_rate if sample_rate > 0 else 0.0
    charge_overall = compute_charge_metrics(samples, sample_rate, has_negative_samples)
    overall = {
        "samples": len(samples),
        "duration_s": f"{duration_s:.2f}",
        "avg_uA": f"{float(np.nanmean(samples)):.2f}" if len(samples) else "n/a",
        "min_uA": f"{float(np.nanmin(samples)):.2f}" if len(samples) else "n/a",
        "max_uA": f"{float(np.nanmax(samples)):.2f}" if len(samples) else "n/a",
        "sample_rate_hz": f"{sample_rate:.1f}",
        "charge_C": f"{charge_overall['charge_C']:.6f}",
        "charge_mC": f"{charge_overall['charge_mC']:.3f}",
        "charge_uAh": f"{charge_overall['charge_uAh']:.3f}",
    }
    if has_negative_samples:
        overall["charge_C_clipped"] = f"{charge_overall['charge_C_clipped']:.6f}"

    warnings = alignment_warnings(events, len(samples), sample_rate)
    if has_negative_samples:
        warnings.append("Negative current samples detected; clipped charge reported.")
    warnings.extend(state_stats_consistency_warnings(state_stats))

    print("Summary:")
    for key, value in overall.items():
        print(f"- {key}: {value}")
    if warnings:
        print("Warnings:")
        for warning in warnings:
            print(f"- {warning}")

    if args.summary_output:
        summary_path = Path(args.summary_output)
        if args.summary_format == "csv":
            write_summary_csv(summary_path, state_stats, transitions, overall, warnings)
        else:
            write_summary_markdown(summary_path, overall, state_stats, transitions, warnings)

    plot_capture(
        samples,
        sample_rate,
        plot_markers,
        Path(args.plot_output) if args.plot_output else None,
        show_plot=not args.no_show,
    )


if __name__ == "__main__":
    main()
