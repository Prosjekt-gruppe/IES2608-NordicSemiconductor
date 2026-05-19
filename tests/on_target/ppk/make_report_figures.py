#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

plt.style.use("default")
plt.rcParams.update(
    {
        "font.size": 10,
        "axes.titlesize": 12,
        "axes.labelsize": 10,
        "legend.fontsize": 9,
    }
)

DEFAULT_SAMPLE_RATE_HZ = 100_000.0
DEFAULT_MAX_POINTS = 200_000

FIGSIZE = (10, 5)
COMBINED_FIGSIZE = (11, 10)

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


@dataclass
class Marker:
    t_s: float
    label: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate report-ready figures from processed PPK/UART data."
    )
    parser.add_argument("--data-dir", default="data/raw", help="Raw data directory.")
    parser.add_argument(
        "--processed-dir",
        default="data/processed",
        help="Processed CSV directory.",
    )
    parser.add_argument(
        "--output-dir",
        default="output/report_figures",
        help="Output directory for figures.",
    )
    parser.add_argument(
        "--sample-rate-hz",
        type=float,
        default=DEFAULT_SAMPLE_RATE_HZ,
        help="Sample rate in Hz.",
    )
    parser.add_argument("--from-s", type=float, default=None, help="Start time.")
    parser.add_argument("--to-s", type=float, default=None, help="End time.")
    parser.add_argument(
        "--max-points",
        type=int,
        default=DEFAULT_MAX_POINTS,
        help="Maximum plotted current samples.",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="Do not show interactive figures.",
    )
    return parser.parse_args()


def state_label(state: str) -> str:
    return STATE_LABELS.get(state, state.replace("STATE_", "").replace("_", " ").title())


def format_seconds(value: float) -> str:
    if not math.isfinite(value):
        return "n/a"
    text = f"{value:.2f}"
    return text.rstrip("0").rstrip(".")


def read_csv_safe(path: Path, label: str) -> pd.DataFrame:
    if not path.exists():
        print(f"Warning: missing {label} at {path}")
        return pd.DataFrame()
    try:
        return pd.read_csv(path)
    except pd.errors.EmptyDataError:
        print(f"Warning: empty {label} at {path}")
        return pd.DataFrame()


def load_current_samples(data_dir: Path) -> Optional[np.ndarray]:
    path = data_dir / "current_uA.bin"
    if not path.exists():
        print(f"Warning: missing current samples at {path}")
        return None
    samples = np.fromfile(path, dtype=np.float32)
    if samples.size == 0:
        print(f"Warning: empty current samples at {path}")
        return None
    return samples


def ensure_timestamp(df: pd.DataFrame, sample_rate: float) -> tuple[pd.DataFrame, str]:
    df = df.copy()
    if df.empty:
        df["t_sample_s"] = []
        return df, "t_sample_s"

    if "t_sample_s" in df.columns:
        df["t_sample_s"] = pd.to_numeric(df["t_sample_s"], errors="coerce")
        if df["t_sample_s"].notna().any():
            return df, "t_sample_s"

    if "t_s" in df.columns:
        df["t_s"] = pd.to_numeric(df["t_s"], errors="coerce")
        if df["t_s"].notna().any():
            return df, "t_s"

    if "sample_idx" in df.columns and sample_rate > 0:
        df["t_s"] = pd.to_numeric(df["sample_idx"], errors="coerce") / float(
            sample_rate
        )
        return df, "t_s"

    df["t_s"] = float("nan")
    return df, "t_s"


def filter_time_window(
    df: pd.DataFrame,
    t_col: str,
    start_s: Optional[float],
    end_s: Optional[float],
) -> pd.DataFrame:
    if df.empty or t_col not in df.columns:
        return df
    times = pd.to_numeric(df[t_col], errors="coerce")
    mask = np.isfinite(times)
    if start_s is not None:
        mask &= times >= float(start_s)
    if end_s is not None:
        mask &= times <= float(end_s)
    filtered = df.loc[mask].copy()
    if filtered.empty:
        return filtered
    filtered[t_col] = pd.to_numeric(filtered[t_col], errors="coerce")
    return filtered.sort_values(t_col)


def extract_series(
    df: pd.DataFrame, t_col: str, value_col: str
) -> tuple[np.ndarray, np.ndarray]:
    if df.empty or t_col not in df.columns or value_col not in df.columns:
        return np.array([]), np.array([])
    t_values = pd.to_numeric(df[t_col], errors="coerce")
    y_values = pd.to_numeric(df[value_col], errors="coerce")
    mask = np.isfinite(t_values) & np.isfinite(y_values)
    if not mask.any():
        return np.array([]), np.array([])
    t = t_values[mask].to_numpy()
    y = y_values[mask].to_numpy()
    order = np.argsort(t)
    return t[order], y[order]


def clamp_time_range(
    total_duration_s: float, from_s: Optional[float], to_s: Optional[float]
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


def derive_time_window(
    samples: Optional[np.ndarray],
    sample_rate: float,
    from_s: Optional[float],
    to_s: Optional[float],
    frames: Iterable[tuple[pd.DataFrame, str]],
) -> tuple[float, float]:
    if samples is not None and sample_rate > 0:
        total_duration_s = len(samples) / sample_rate
        return clamp_time_range(total_duration_s, from_s, to_s)

    start_s = from_s
    end_s = to_s

    if start_s is None or end_s is None:
        min_t: Optional[float] = None
        max_t: Optional[float] = None
        for frame, t_col in frames:
            if frame.empty or t_col not in frame.columns:
                continue
            times = pd.to_numeric(frame[t_col], errors="coerce")
            times = times[np.isfinite(times)]
            if times.empty:
                continue
            frame_min = float(times.min())
            frame_max = float(times.max())
            min_t = frame_min if min_t is None else min(min_t, frame_min)
            max_t = frame_max if max_t is None else max(max_t, frame_max)
        if start_s is None:
            start_s = min_t
        if end_s is None:
            end_s = max_t

    if start_s is None:
        start_s = 0.0
    if end_s is None:
        end_s = start_s
    if end_s < start_s:
        end_s = start_s

    return float(start_s), float(end_s)


def build_markers(
    df: pd.DataFrame, t_col: str, important_only: bool = True
) -> list[Marker]:
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
        label = row.get(label_col) if label_col else None
        if label is None or (isinstance(label, float) and math.isnan(label)):
            label = "Marker"
        label = str(label).strip()
        if not label:
            continue
        if important_only and label not in IMPORTANT_MARKERS:
            continue
        markers.append(Marker(t_s=float(t_value), label=label))

    return markers


def dedupe_markers(
    markers: list[Marker], start_s: float, end_s: float, window_s: float = 2.0
) -> list[Marker]:
    in_range = [m for m in markers if start_s <= m.t_s <= end_s]
    in_range.sort(key=lambda item: item.t_s)

    output: list[Marker] = []
    last_label_time: dict[str, float] = {}
    for marker in in_range:
        last_time = last_label_time.get(marker.label)
        if last_time is not None and marker.t_s - last_time <= window_s:
            continue
        last_label_time[marker.label] = marker.t_s
        output.append(marker)

    return output


def build_state_intervals(
    df: pd.DataFrame,
    t_col: str,
    ignored_states: set[str],
    end_s: Optional[float],
) -> list[tuple[str, float, float]]:
    if df.empty or "to_state" not in df.columns:
        return []

    transitions: list[tuple[float, str]] = []
    for _, row in df.iterrows():
        t_value = row.get(t_col)
        if not isinstance(t_value, (float, int)) or not math.isfinite(float(t_value)):
            continue
        to_state = row.get("to_state")
        if to_state is None or (isinstance(to_state, float) and math.isnan(to_state)):
            continue
        to_state = str(to_state).strip()
        if not to_state:
            continue
        transitions.append((float(t_value), to_state))

    transitions.sort(key=lambda item: item[0])
    intervals: list[tuple[str, float, float]] = []

    for idx, (t_s, state) in enumerate(transitions):
        if state in ignored_states:
            continue
        next_t = None
        for next_idx in range(idx + 1, len(transitions)):
            next_t_candidate = transitions[next_idx][0]
            if next_t_candidate > t_s:
                next_t = next_t_candidate
                break
        if next_t is None:
            next_t = end_s
        if next_t is None or not math.isfinite(next_t) or next_t <= t_s:
            continue
        intervals.append((state, t_s, float(next_t)))

    return intervals


def add_state_shading(
    ax: plt.Axes,
    intervals: list[tuple[str, float, float]],
    alpha: float,
    show_labels: bool = True,
) -> None:
    if not intervals:
        return

    cmap = plt.get_cmap("tab20")
    state_colors: dict[str, tuple[float, float, float, float]] = {}
    used_labels: set[str] = set()

    for idx, (state, start_s, end_s) in enumerate(intervals):
        color = state_colors.get(state)
        if color is None:
            color = cmap(idx % 20)
            state_colors[state] = color
        label = state_label(state)
        legend_label = label if show_labels and label not in used_labels else None
        ax.axvspan(
            start_s,
            end_s,
            color=color,
            alpha=alpha,
            label=legend_label,
            zorder=0,
        )
        used_labels.add(label)


def add_marker_lines(ax: plt.Axes, markers: list[Marker]) -> None:
    used_labels: set[str] = set()
    for marker in markers:
        label = marker.label if marker.label not in used_labels else None
        ax.axvline(
            marker.t_s,
            linestyle="--",
            color="tab:gray",
            alpha=0.7,
            linewidth=1.2,
            label=label,
        )
        used_labels.add(marker.label)


def downsample_series(
    t: np.ndarray, y: np.ndarray, max_points: int
) -> tuple[np.ndarray, np.ndarray]:
    if max_points <= 0 or len(y) <= max_points:
        return t, y
    step = int(math.ceil(len(y) / max_points))
    return t[::step], y[::step]


def plot_placeholder(ax: plt.Axes, label: str) -> None:
    ax.text(
        0.5,
        0.5,
        label,
        transform=ax.transAxes,
        ha="center",
        va="center",
        fontsize=11,
        color="tab:gray",
    )
    ax.set_axis_off()


def plot_current_with_states(
    output_dir: Path,
    t: np.ndarray,
    samples: np.ndarray,
    intervals: list[tuple[str, float, float]],
    markers: list[Marker],
    show_plot: bool,
) -> None:
    fig, ax = plt.subplots(figsize=FIGSIZE)

    if len(samples):
        ax.plot(t, samples, linewidth=0.8, color="tab:blue", label="Current [uA]")
        ax.set_ylabel("Current [uA]")
        ax.set_xlabel("Time [s]")
        ax.set_title("Current with state intervals")
        ax.grid(True, linestyle="--", alpha=0.4)
        add_state_shading(ax, intervals, alpha=0.06)
        add_marker_lines(ax, markers)
        ax.legend(
            loc="upper left",
            bbox_to_anchor=(1.02, 1),
            borderaxespad=0,
            ncol=1,
        )
    else:
        plot_placeholder(ax, "No current data available")

    fig.tight_layout()
    fig.savefig(output_dir / "current_with_states.pdf", dpi=250, bbox_inches="tight")

    if show_plot:
        plt.show()
    else:
        plt.close(fig)


def plot_lte_rsrp_with_states(
    output_dir: Path,
    df: pd.DataFrame,
    t_col: str,
    intervals: list[tuple[str, float, float]],
    markers: list[Marker],
    show_plot: bool,
) -> int:
    fig, ax = plt.subplots(figsize=FIGSIZE)

    plotted_points = 0
    if not df.empty and "rsrp_dbm" in df.columns:
        t_all = pd.to_numeric(df[t_col], errors="coerce")
        rsrp = pd.to_numeric(df["rsrp_dbm"], errors="coerce")
        mask = np.isfinite(t_all) & np.isfinite(rsrp)
        if mask.any():
            df_plot = df.loc[mask].copy()
            df_plot[t_col] = pd.to_numeric(df_plot[t_col], errors="coerce")
            df_plot["rsrp_dbm"] = pd.to_numeric(df_plot["rsrp_dbm"], errors="coerce")
            df_plot = df_plot.sort_values(t_col)

            derived_mask = None
            if "rsrp_dbm_derived" in df_plot.columns:
                derived_mask = df_plot["rsrp_dbm_derived"].fillna(0).astype(int) == 1
            if "source_type" in df_plot.columns:
                direct_mask = df_plot["source_type"].fillna("") == "lte_rsrp_dbm"
            else:
                direct_mask = None

            if derived_mask is not None:
                direct = df_plot.loc[~derived_mask]
                derived = df_plot.loc[derived_mask]
            elif direct_mask is not None:
                direct = df_plot.loc[direct_mask]
                derived = df_plot.loc[~direct_mask]
            else:
                direct = df_plot
                derived = pd.DataFrame()

            if not direct.empty:
                ax.plot(
                    direct[t_col],
                    direct["rsrp_dbm"],
                    marker="o",
                    markersize=3,
                    linewidth=1.2,
                    alpha=0.9,
                    color="tab:red",
                    label="RSRP [dBm]",
                )
                plotted_points += len(direct)

            if not derived.empty:
                ax.plot(
                    derived[t_col],
                    derived["rsrp_dbm"],
                    marker="s",
                    markersize=3,
                    linewidth=1.2,
                    alpha=0.8,
                    color="tab:orange",
                    label="RSRP derived [dBm]",
                )
                plotted_points += len(derived)

            ax.axhline(
                -110,
                linestyle="--",
                color="gray",
                linewidth=1.4,
                alpha=0.8,
                label="Fallback threshold",
            )
            ax.axhline(
                -120,
                linestyle=":",
                color="black",
                linewidth=1.1,
                alpha=0.55,
                label="-120 dBm reference",
                zorder=2,
            )

            ax.set_ylabel("RSRP [dBm]")
            ax.set_xlabel("Time [s]")
            ax.set_title("LTE RSRP with state intervals")
            ax.grid(True, linestyle="--", alpha=0.4)
            add_state_shading(ax, intervals, alpha=0.12)
            add_marker_lines(ax, markers)
            ax.legend(
                loc="upper left",
                bbox_to_anchor=(1.02, 1),
                borderaxespad=0,
                ncol=1,
            )
        else:
            plot_placeholder(ax, "No LTE RSRP data available")
    else:
        plot_placeholder(ax, "No LTE RSRP data available")

    fig.tight_layout()
    fig.savefig(output_dir / "lte_rsrp_with_states.pdf", dpi=250, bbox_inches="tight")

    if show_plot:
        plt.show()
    else:
        plt.close(fig)

    return plotted_points


def plot_ntn_monitor_with_states(
    output_dir: Path,
    df: pd.DataFrame,
    t_col: str,
    intervals: list[tuple[str, float, float]],
    markers: list[Marker],
    show_plot: bool,
) -> int:
    fig, ax1 = plt.subplots(figsize=FIGSIZE)

    rsrp_t, rsrp = extract_series(df, t_col, "rsrp_raw")
    rsrp_dbm = rsrp - 141
    snr_t, snr = extract_series(df, t_col, "snr")
    plotted_points = 0

    if len(rsrp_t) or len(snr_t):
        if len(rsrp_t):
            ax1.plot(
                rsrp_t,
                rsrp_dbm,
                marker="o",
                markersize=3,
                linewidth=1.2,
                alpha=0.9,
                color="tab:purple",
                label="NTN RSRP estimated [dBm]",
                zorder=2,
            )
            plotted_points = max(plotted_points, len(rsrp_t))
        ax1.set_ylabel("NTN RSRP estimated [dBm]")
        ax1.set_xlabel("Time [s]")
        ax1.grid(True, linestyle="--", alpha=0.4)

        if len(snr_t):
            ax2 = ax1.twinx()
            ax2.plot(
                snr_t,
                snr,
                marker="s",
                markersize=3,
                linewidth=1.2,
                alpha=0.9,
                color="tab:green",
                label="NTN SNR",
            )
            ax2.set_ylabel("NTN SNR")
        else:
            ax2 = None

        add_state_shading(ax1, intervals, alpha=0.12)
        add_marker_lines(ax1, markers)

        lines1, labels1 = ax1.get_legend_handles_labels()
        lines2, labels2 = (ax2.get_legend_handles_labels() if ax2 else ([], []))
        ax1.legend(lines1 + lines2, labels1 + labels2, loc="best", ncol=2)
        ax1.set_title("NTN monitor with state intervals")
    else:
        plot_placeholder(ax1, "No NTN monitor data available")

    fig.tight_layout()
    fig.savefig(
        output_dir / "ntn_monitor_with_states.pdf", dpi=250, bbox_inches="tight"
    )

    if show_plot:
        plt.show()
    else:
        plt.close(fig)

    return plotted_points


def plot_accel_with_states(
    output_dir: Path,
    df: pd.DataFrame,
    t_col: str,
    intervals: list[tuple[str, float, float]],
    markers: list[Marker],
    show_plot: bool,
) -> int:
    fig, ax1 = plt.subplots(figsize=FIGSIZE)

    speed_t, speed = extract_series(df, t_col, "speed")
    accel_t, accel = extract_series(df, t_col, "accel")
    plotted_points = 0

    if len(speed_t) or len(accel_t):
        if len(speed_t):
            ax1.plot(
                speed_t,
                speed,
                marker="o",
                markersize=3,
                linewidth=1.2,
                alpha=0.9,
                color="tab:blue",
                label="Speed",
            )
            ax1.set_ylabel("Speed")
            plotted_points = max(plotted_points, len(speed_t))
        else:
            ax1.set_ylabel("Speed")

        ax1.set_xlabel("Time [s]")
        ax1.grid(True, linestyle="--", alpha=0.4)

        ax2 = None
        if len(accel_t):
            ax2 = ax1.twinx()
            ax2.plot(
                accel_t,
                accel,
                marker="s",
                markersize=3,
                linewidth=1.2,
                alpha=0.9,
                color="tab:orange",
                label="Accel",
            )
            ax2.set_ylabel("Accel")
            plotted_points = max(plotted_points, len(accel_t))

        add_state_shading(ax1, intervals, alpha=0.12)
        add_marker_lines(ax1, markers)

        lines1, labels1 = ax1.get_legend_handles_labels()
        lines2, labels2 = (ax2.get_legend_handles_labels() if ax2 else ([], []))
        ax1.legend(lines1 + lines2, labels1 + labels2, loc="best", ncol=2)
        ax1.set_title("Motion/accel with state intervals")
    else:
        plot_placeholder(ax1, "No accel or speed data available")

    fig.tight_layout()
    fig.savefig(
        output_dir / "accel_motion_with_states.pdf", dpi=250, bbox_inches="tight"
    )

    if show_plot:
        plt.show()
    else:
        plt.close(fig)

    return plotted_points


def plot_combined_overview(
    output_dir: Path,
    current_t: np.ndarray,
    current_samples: np.ndarray,
    conneval_df: pd.DataFrame,
    conneval_t_col: str,
    ntn_df: pd.DataFrame,
    ntn_t_col: str,
    accel_df: pd.DataFrame,
    accel_t_col: str,
    intervals: list[tuple[str, float, float]],
    show_plot: bool,
) -> None:
    fig, axes = plt.subplots(4, 1, figsize=COMBINED_FIGSIZE, sharex=True)

    # Panel 1: current
    ax = axes[0]
    if len(current_samples):
        ax.plot(current_t, current_samples, linewidth=0.8, color="tab:blue")
        ax.set_ylabel("Current [uA]")
        ax.grid(True, linestyle="--", alpha=0.3)
        add_state_shading(ax, intervals, alpha=0.08, show_labels=False)
    else:
        plot_placeholder(ax, "No current data")

    # Panel 2: LTE RSRP
    ax = axes[1]
    rsrp_t, rsrp = extract_series(conneval_df, conneval_t_col, "rsrp_dbm")
    if len(rsrp_t):
        ax.plot(rsrp_t, rsrp, linewidth=1.2, color="tab:red")
        ax.axhline(-110, linestyle="--", color="gray", linewidth=1.2, alpha=0.7)
        ax.set_ylabel("RSRP [dBm]")
        ax.grid(True, linestyle="--", alpha=0.3)
        add_state_shading(ax, intervals, alpha=0.08, show_labels=False)
    else:
        plot_placeholder(ax, "No LTE RSRP data")

    # Panel 3: NTN RSRP/SNR
    ax = axes[2]
    rsrp_t, rsrp = extract_series(ntn_df, ntn_t_col, "rsrp_raw")
    rsrp_dbm = rsrp - 141
    snr_t, snr = extract_series(ntn_df, ntn_t_col, "snr")
    if len(rsrp_t) or len(snr_t):
        if len(rsrp_t):
            ax.plot(rsrp_t, rsrp_dbm, linewidth=1.2, color="tab:purple")
            ax.set_ylabel("NTN RSRP [dbm]")
        ax.grid(True, linestyle="--", alpha=0.3)
        if len(snr_t):
            ax2 = ax.twinx()
            ax2.plot(snr_t, snr, linewidth=1.2, color="tab:green")
            ax2.set_ylabel("NTN SNR")
        add_state_shading(ax, intervals, alpha=0.08, show_labels=False)
    else:
        plot_placeholder(ax, "No NTN monitor data")

    # Panel 4: speed or accel
    ax = axes[3]
    speed_t, speed = extract_series(accel_df, accel_t_col, "speed")
    accel_t, accel = extract_series(accel_df, accel_t_col, "accel")
    if len(speed_t):
        ax.plot(speed_t, speed, linewidth=1.2, color="tab:blue")
        ax.set_ylabel("Speed")
        ax.grid(True, linestyle="--", alpha=0.3)
        add_state_shading(ax, intervals, alpha=0.08, show_labels=False)
    elif len(accel_t):
        ax.plot(accel_t, accel, linewidth=1.2, color="tab:orange")
        ax.set_ylabel("Accel")
        ax.grid(True, linestyle="--", alpha=0.3)
        add_state_shading(ax, intervals, alpha=0.08, show_labels=False)
    else:
        plot_placeholder(ax, "No accel/speed data")

    axes[-1].set_xlabel("Time [s]")
    fig.suptitle("PPK/UART overview", y=0.98)
    fig.tight_layout()
    fig.savefig(output_dir / "combined_overview.pdf", dpi=250, bbox_inches="tight")

    if show_plot:
        plt.show()
    else:
        plt.close(fig)

def plot_fallback_decision_rsrp(
    output_dir: Path,
    conneval_df: pd.DataFrame,
    conneval_t_col: str,
    ntn_df: pd.DataFrame,
    ntn_t_col: str,
    intervals: list[tuple[str, float, float]],
    markers: list[Marker],
    show_plot: bool,
) -> int:
    """Focused figure showing LTE-M degradation, fallback threshold and NTN monitoring."""
    fig, ax = plt.subplots(figsize=(10, 5))

    plotted_points = 0

    # LTE-M connection-evaluation rows already store RSRP in dBm.
    lte_t, lte_rsrp = extract_series(conneval_df, conneval_t_col, "rsrp_dbm")

    # NTN monitor RSRP is reported as modem RSRP index.
    # Convert to lower dBm boundary using rsrp_dbm = rsrp_raw - 141.
    ntn_t, ntn_rsrp_raw = extract_series(ntn_df, ntn_t_col, "rsrp_raw")
    ntn_rsrp_dbm = ntn_rsrp_raw - 141 if len(ntn_rsrp_raw) else np.array([])

    has_data = len(lte_t) or len(ntn_t)

    if has_data:
        add_state_shading(ax, intervals, alpha=0.12)

        if len(lte_t):
            gap_threshold_s = 30.0

            lte_t_plot = lte_t.copy()
            lte_rsrp_plot = lte_rsrp.copy()

            time_diff = np.diff(lte_t_plot)

            gap_indices = np.where(time_diff > gap_threshold_s)[0]

            for idx in gap_indices:
                lte_rsrp_plot[idx + 1] = np.nan
            ax.plot(
                lte_t_plot,
                lte_rsrp_plot,
                marker="o",
                markersize=4,
                linewidth=1.5,
                alpha=0.95,
                color="tab:red",
                label="LTE-M RSRP [dBm]",
                zorder=3,
            )
            plotted_points += len(lte_t)

        if len(ntn_t):
            ax.plot(
                ntn_t,
                ntn_rsrp_dbm,
                marker="s",
                markersize=4,
                linewidth=1.5,
                alpha=0.9,
                color="tab:purple",
                label="NTN RSRP estimated [dBm]",
                zorder=3,
            )
            plotted_points += len(ntn_t)

        ax.axhline(
            -110,
            linestyle=":",
            color="black",
            linewidth=1.2,
            alpha=0.75,
            label="Fallback threshold",
            zorder=2,
        )

        # Lower reference line for very weak LTE-M samples.
        ax.axhline(
            -120,
            linestyle="--",
            color="black",
            linewidth=1.1,
            alpha=0.55,
            label="-120 dBm reference",
            zorder=2,
        )


        relevant_marker_labels = {
            "LTE-M fallback",
            "Starting NTN",
            "Switched to NTN",
            "LTE probe",
            "LTE-M recovered",
            "Stay on NTN",
        }
        focused_markers = [
            marker for marker in markers if marker.label in relevant_marker_labels
        ]
        for marker in focused_markers:
            ax.axvline(
                marker.t_s,
                linestyle="--",
                color="tab:gray",
                alpha=0.5,
                linewidth=1.0,
                zorder=1,
            )
        ax.set_xlabel("Time [s]")
        ax.set_ylabel("RSRP [dBm]")
        ax.set_title("Fallback decision from LTE-M degradation to NTN operation")
        ax.grid(True, linestyle="--", alpha=0.3)

        ax.legend(
            loc="upper left",
            bbox_to_anchor=(1.02, 1.0),
            borderaxespad=0,
            ncol=1,
        )
    else:
        plot_placeholder(ax, "No LTE/NTN RSRP data available")

    fig.tight_layout()
    fig.savefig(
        output_dir / "fallback_decision_rsrp.pdf",
        dpi=250,
        bbox_inches="tight",
    )

    if show_plot:
        plt.show()
    else:
        plt.close(fig)

    return plotted_points


def write_summary(
    output_dir: Path,
    start_s: float,
    end_s: float,
    current_points: int,
    lte_points: int,
    ntn_points: int,
    accel_points: int,
    state_intervals: list[tuple[str, float, float]],
    figure_files: list[str],
) -> None:
    summary_path = output_dir / "figure_summary.md"
    unique_states = sorted({state_label(state) for state, _, _ in state_intervals})

    lines = [
        "# Figure Summary",
        "",
        "## Time Window",
        "",
        f"- start_s: {format_seconds(start_s)}",
        f"- end_s: {format_seconds(end_s)}",
        "",
        "## Counts",
        "",
        f"- current_samples_plotted: {current_points}",
        f"- lte_rsrp_points: {lte_points}",
        f"- ntn_monitor_points: {ntn_points}",
        f"- accel_points: {accel_points}",
        f"- state_intervals: {len(state_intervals)}",
    ]

    if unique_states:
        lines.append(f"- states_included: {', '.join(unique_states)}")

    lines += ["", "## Figures", ""]
    for name in figure_files:
        lines.append(f"- {name}")

    summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()

    if args.sample_rate_hz <= 0:
        print("Error: --sample-rate-hz must be positive.")
        return 1
    if args.max_points <= 0:
        print("Error: --max-points must be positive.")
        return 1

    data_dir = Path(args.data_dir)
    processed_dir = Path(args.processed_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    samples = load_current_samples(data_dir)

    conneval_df = read_csv_safe(processed_dir / "conneval.csv", "conneval.csv")
    ntn_df = read_csv_safe(processed_dir / "ntn_monitor.csv", "ntn_monitor.csv")
    accel_df = read_csv_safe(processed_dir / "accel.csv", "accel.csv")
    transitions_df = read_csv_safe(
        processed_dir / "state_transitions.csv", "state_transitions.csv"
    )
    markers_df = read_csv_safe(processed_dir / "markers.csv", "markers.csv")

    conneval_df, conneval_t_col = ensure_timestamp(conneval_df, args.sample_rate_hz)
    ntn_df, ntn_t_col = ensure_timestamp(ntn_df, args.sample_rate_hz)
    accel_df, accel_t_col = ensure_timestamp(accel_df, args.sample_rate_hz)
    transitions_df, transitions_t_col = ensure_timestamp(
        transitions_df, args.sample_rate_hz
    )
    markers_df, markers_t_col = ensure_timestamp(markers_df, args.sample_rate_hz)

    start_s, end_s = derive_time_window(
        samples,
        args.sample_rate_hz,
        args.from_s,
        args.to_s,
        [
            (conneval_df, conneval_t_col),
            (ntn_df, ntn_t_col),
            (accel_df, accel_t_col),
            (transitions_df, transitions_t_col),
            (markers_df, markers_t_col),
        ],
    )

    conneval_df = filter_time_window(conneval_df, conneval_t_col, start_s, end_s)
    ntn_df = filter_time_window(ntn_df, ntn_t_col, start_s, end_s)
    accel_df = filter_time_window(accel_df, accel_t_col, start_s, end_s)
    transitions_df = filter_time_window(
        transitions_df, transitions_t_col, start_s, end_s
    )
    markers_df = filter_time_window(markers_df, markers_t_col, start_s, end_s)

    intervals = build_state_intervals(
        transitions_df, transitions_t_col, SUMMARY_IGNORED_STATES, end_s
    )

    markers = build_markers(markers_df, markers_t_col, important_only=True)
    markers = dedupe_markers(markers, start_s, end_s, window_s=2.0)

    # Current samples are stored separately from UART events, so build this series here.
    current_t = np.array([])
    current_samples = np.array([])
    current_points = 0
    if samples is not None:
        total_duration_s = len(samples) / args.sample_rate_hz
        start_s, end_s = clamp_time_range(total_duration_s, start_s, end_s)
        start_idx = max(0, int(math.floor(start_s * args.sample_rate_hz)))
        end_idx = max(start_idx, int(math.ceil(end_s * args.sample_rate_hz)))
        end_idx = min(end_idx, len(samples))
        window_samples = samples[start_idx:end_idx]
        if len(window_samples):
            t = np.arange(start_idx, end_idx) / args.sample_rate_hz
            t, window_samples = downsample_series(t, window_samples, args.max_points)
            current_t = t
            current_samples = window_samples
            current_points = len(window_samples)

    figure_files = [
        "current_with_states.pdf",
        "lte_rsrp_with_states.pdf",
        "ntn_monitor_with_states.pdf",
        "accel_motion_with_states.pdf",
        "combined_overview.pdf",
        "fallback_decision_rsrp.pdf",
    ]

    plot_current_with_states(
        output_dir, current_t, current_samples, intervals, markers, not args.no_show
    )
    lte_points = plot_lte_rsrp_with_states(
        output_dir,
        conneval_df,
        conneval_t_col,
        intervals,
        markers,
        not args.no_show,
    )
    ntn_points = plot_ntn_monitor_with_states(
        output_dir,
        ntn_df,
        ntn_t_col,
        intervals,
        markers,
        not args.no_show,
    )
    accel_points = plot_accel_with_states(
        output_dir,
        accel_df,
        accel_t_col,
        intervals,
        markers,
        not args.no_show,
    )

    plot_combined_overview(
        output_dir,
        current_t,
        current_samples,
        conneval_df,
        conneval_t_col,
        ntn_df,
        ntn_t_col,
        accel_df,
        accel_t_col,
        intervals,
        not args.no_show,
    )
    fallback_points = plot_fallback_decision_rsrp(
        output_dir,
        conneval_df,
        conneval_t_col,
        ntn_df,
        ntn_t_col,
        intervals,
        markers,
        not args.no_show,
    )
    _ = fallback_points

    write_summary(
        output_dir,
        start_s,
        end_s,
        current_points,
        lte_points,
        ntn_points,
        accel_points,
        intervals,
        figure_files,
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
