#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

plt.style.use("default")
plt.rcParams.update({
    "font.size": 11,
    "axes.titlesize": 13,
    "axes.labelsize": 11,
    "legend.fontsize": 10,
})

FIGSIZE = (10, 5)
LINEWIDTH = 2
ALPHA = 0.9
MARKERSIZE = 5


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze extracted LTE connection evaluation time series."
    )
    parser.add_argument(
        "--input",
        default="output/conneval_timeseries.csv",
        help="Input conneval CSV from extract_conneval.py.",
    )
    parser.add_argument(
        "--output-dir",
        default="output",
        help="Directory for plots and summary output.",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="Do not show interactive plot windows.",
    )
    parser.add_argument(
        "--drop-after-gap-s",
        type=float,
        default=None,
        help="Drop conneval rows that occur after a gap larger than this many seconds.",
    )
    parser.add_argument(
        "--break-lines-after-gap-s",
        type=float,
        default=None,
        help="Insert NaN separators so plot lines break across gaps larger than this many seconds.",
    )
    parser.add_argument(
        "--dark-mode",
        action="store_true",
        help="Use a dark background style for plots.",
    )
    return parser.parse_args()


def load_conneval(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise FileNotFoundError(f"Missing input CSV: {path}")

    df = pd.read_csv(path)

    required = {"t_s", "rsrp_dbm", "tx_pwr"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns: {sorted(missing)}")

    df = df.sort_values("t_s").reset_index(drop=True)

    # Make relative time easier to read in plots.
    df["t_rel_s"] = df["t_s"] - df["t_s"].iloc[0]

    return df


def add_event_markers(ax: plt.Axes, df: pd.DataFrame) -> None:
    event_columns = {
        "ntn_switch": "NTN switch",
        "lte_fallback": "LTE fallback",
        "lte_recovery": "LTE recovery",
    }

    for column, label in event_columns.items():
        if column not in df.columns:
            continue

        mask = df[column].fillna(0).astype(bool)
        if not mask.any():
            continue

        first_label = True
        for t_rel_s in df.loc[mask, "t_rel_s"].dropna().unique():
            ax.axvline(
                t_rel_s,
                linestyle=":",
                color="tab:gray",
                alpha=0.6,
                linewidth=1.5,
                label=label if first_label else None,
            )
            first_label = False


def insert_gap_breaks(df: pd.DataFrame, gap_s: float) -> pd.DataFrame:
    if gap_s <= 0:
        return df

    dt_s = df["t_rel_s"].diff()
    gap_indices = dt_s[dt_s > gap_s].index.tolist()
    if not gap_indices:
        return df

    frames = []
    start_idx = 0
    for gap_idx in gap_indices:
        frames.append(df.iloc[start_idx:gap_idx])
        separator = {col: float("nan") for col in df.columns}
        separator["t_rel_s"] = df.iloc[gap_idx - 1]["t_rel_s"]
        frames.append(pd.DataFrame([separator], columns=df.columns))
        start_idx = gap_idx

    frames.append(df.iloc[start_idx:])
    return pd.concat(frames, ignore_index=True)


def plot_rsrp(df: pd.DataFrame, output_dir: Path, show: bool) -> None:
    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.plot(
        df["t_rel_s"],
        df["rsrp_dbm"],
        marker="o",
        markersize=MARKERSIZE,
        linewidth=LINEWIDTH,
        alpha=ALPHA,
        color="tab:red",
        label="RSRP [dBm]",
    )
    ax.axhline(
        -110,
        linestyle="--",
        color="gray",
        linewidth=1.5,
        alpha=0.8,
        label="Fallback threshold",
    )
    add_event_markers(ax, df)
    ax.set_xlabel("Time since first conneval [s]")
    ax.set_ylabel("RSRP [dBm]")
    ax.set_title("LTE-M signal quality during field test")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend(loc="best")
    fig.tight_layout()
    fig.savefig(
        output_dir / "conneval_rsrp_over_time.png",
        dpi=250,
        bbox_inches="tight",
    )

    if show:
        plt.show()
    else:
        plt.close(fig)


def plot_tx_power(df: pd.DataFrame, output_dir: Path, show: bool) -> None:
    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.plot(
        df["t_rel_s"],
        df["tx_pwr"],
        marker="s",
        markersize=MARKERSIZE,
        linewidth=LINEWIDTH,
        alpha=ALPHA,
        color="tab:blue",
        label="TX power",
    )
    add_event_markers(ax, df)
    ax.set_xlabel("Time since first conneval [s]")
    ax.set_ylabel("TX power")
    ax.set_title("LTE-M signal quality during field test")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend(loc="best")
    fig.tight_layout()
    fig.savefig(
        output_dir / "conneval_tx_power_over_time.png",
        dpi=250,
        bbox_inches="tight",
    )

    if show:
        plt.show()
    else:
        plt.close(fig)


def plot_rsrp_and_tx_power(df: pd.DataFrame, output_dir: Path, show: bool) -> None:
    fig, ax1 = plt.subplots(figsize=FIGSIZE)

    ax1.plot(
        df["t_rel_s"],
        df["rsrp_dbm"],
        marker="o",
        markersize=MARKERSIZE,
        linewidth=LINEWIDTH,
        alpha=ALPHA,
        color="tab:red",
        label="RSRP [dBm]",
    )
    ax1.set_xlabel("Time since first conneval [s]")
    ax1.set_ylabel("RSRP [dBm]")
    ax1.grid(True, linestyle="--", alpha=0.4)

    ax2 = ax1.twinx()
    ax2.plot(
        df["t_rel_s"],
        df["tx_pwr"],
        marker="s",
        markersize=MARKERSIZE,
        linewidth=LINEWIDTH,
        alpha=ALPHA,
        color="tab:blue",
        label="TX power",
    )
    ax2.set_ylabel("TX power")

    add_event_markers(ax1, df)

    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(
        lines1 + lines2,
        labels1 + labels2,
        loc="best",
    )

    plt.title("LTE-M signal quality during field test")
    fig.tight_layout()
    fig.savefig(
        output_dir / "conneval_rsrp_tx_power.png",
        dpi=250,
        bbox_inches="tight",
    )

    if show:
        plt.show()
    else:
        plt.close(fig)


def plot_energy(df: pd.DataFrame, output_dir: Path, show: bool) -> None:
    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.plot(
        df["t_rel_s"],
        df["energy"],
        marker="o",
        markersize=MARKERSIZE,
        linewidth=LINEWIDTH,
        alpha=ALPHA,
        color="tab:green",
        label="Conneval energy metric",
    )
    add_event_markers(ax, df)
    ax.set_xlabel("Time since first conneval [s]")
    ax.set_ylabel("Energy metric")
    ax.set_title("LTE-M connection evaluation energy metric")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend(loc="best")
    fig.tight_layout()
    fig.savefig(
        output_dir / "conneval_energy_over_time.png",
        dpi=250,
        bbox_inches="tight",
    )

    if show:
        plt.show()
    else:
        plt.close(fig)


def plot_rsrp_and_energy(df: pd.DataFrame, output_dir: Path, show: bool) -> None:
    fig, ax1 = plt.subplots(figsize=FIGSIZE)

    ax1.plot(
        df["t_rel_s"],
        df["rsrp_dbm"],
        marker="o",
        markersize=MARKERSIZE,
        linewidth=LINEWIDTH,
        alpha=ALPHA,
        color="tab:red",
        label="RSRP [dBm]",
    )
    ax1.set_xlabel("Time since first conneval [s]")
    ax1.set_ylabel("RSRP [dBm]")
    ax1.grid(True, linestyle="--", alpha=0.4)

    ax2 = ax1.twinx()
    ax2.plot(
        df["t_rel_s"],
        df["energy"],
        marker="o",
        markersize=MARKERSIZE,
        linewidth=LINEWIDTH,
        alpha=ALPHA,
        color="tab:green",
        label="Energy metric",
    )
    ax2.set_ylabel("Energy metric")

    add_event_markers(ax1, df)

    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(
        lines1 + lines2,
        labels1 + labels2,
        loc="best",
    )

    plt.title("LTE-M RSRP and connection energy metric")
    fig.tight_layout()
    fig.savefig(
        output_dir / "conneval_rsrp_energy.png",
        dpi=250,
        bbox_inches="tight",
    )

    if show:
        plt.show()
    else:
        plt.close(fig)


def write_summary(
    df: pd.DataFrame,
    output_dir: Path,
    dropped_rows: int = 0,
    gap_s: float | None = None,
) -> None:
    summary_path = output_dir / "conneval_summary.md"

    lines = [
        "# Connection Evaluation Summary",
        "",
        "## Overall",
        "",
        f"- Samples: {len(df)}",
        f"- Time span: {df['t_rel_s'].min():.2f} s to {df['t_rel_s'].max():.2f} s",
        f"- Mean RSRP: {df['rsrp_dbm'].mean():.2f} dBm",
        f"- Min RSRP: {df['rsrp_dbm'].min():.2f} dBm",
        f"- Max RSRP: {df['rsrp_dbm'].max():.2f} dBm",
        f"- Mean TX power: {df['tx_pwr'].mean():.2f}",
        f"- Max TX power: {df['tx_pwr'].max():.2f}",
        "",
    ]

    if dropped_rows and gap_s is not None:
        lines += [
            f"- Dropped after gaps > {gap_s:.2f} s: {dropped_rows}",
            "",
        ]

    if "rsrp_dbm_derived" in df.columns:
        derived_count = int(df["rsrp_dbm_derived"].fillna(0).sum())
        lines += [
            "## RSRP Source",
            "",
            f"- Direct RSRP dBm values: {len(df) - derived_count}",
            f"- Derived RSRP dBm values: {derived_count}",
            "",
        ]

    if "rrc" in df.columns:
        lines += [
            "## RRC Counts",
            "",
            df["rrc"].value_counts(dropna=False).rename_axis("rrc").reset_index(name="count").to_markdown(index=False),
            "",
        ]

    if "energy" in df.columns:
        lines += [
            "## Energy Metric",
            "",
            f"- Mean energy: {df['energy'].mean():.2f}",
            f"- Min energy: {df['energy'].min():.2f}",
            f"- Max energy: {df['energy'].max():.2f}",
            "",
            df["energy"].value_counts(dropna=False).rename_axis("energy").reset_index(name="count").to_markdown(index=False),
            "",
        ]

    numeric_cols = [
        col for col in [
            "rsrp_dbm",
            "rsrq",
            "snr",
            "dl_pl",
            "tx_pwr",
            "tx_rep",
            "rx_rep",
            "energy",
            "ce",
        ]
        if col in df.columns
    ]

    if numeric_cols:
        lines += [
            "## Numeric Summary",
            "",
            df[numeric_cols].describe().to_markdown(),
            "",
        ]

    summary_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    args = parse_args()

    if args.dark_mode:
        plt.style.use("dark_background")

    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    df = load_conneval(input_path)
    df["dt_s"] = df["t_rel_s"].diff()

    filtered_df = df
    dropped_rows = 0
    if args.drop_after_gap_s is not None:
        keep_mask = df["dt_s"].isna() | (df["dt_s"] <= args.drop_after_gap_s)
        dropped_rows = int((~keep_mask).sum())
        filtered_df = df.loc[keep_mask].reset_index(drop=True)
        print(
            f"Dropped {dropped_rows} conneval rows after gaps > {args.drop_after_gap_s:g} s"
        )

    plot_df = filtered_df
    if args.break_lines_after_gap_s is not None:
        plot_df = insert_gap_breaks(filtered_df, args.break_lines_after_gap_s)

    plot_rsrp(plot_df, output_dir, show=not args.no_show)
    plot_tx_power(plot_df, output_dir, show=not args.no_show)
    plot_rsrp_and_tx_power(plot_df, output_dir, show=not args.no_show)
    if "energy" in plot_df.columns:
        plot_energy(plot_df, output_dir, show=not args.no_show)
        plot_rsrp_and_energy(plot_df, output_dir, show=not args.no_show)
    write_summary(
        filtered_df,
        output_dir,
        dropped_rows=dropped_rows,
        gap_s=args.drop_after_gap_s,
    )

    print(f"Loaded {len(df)} conneval samples from {input_path}")
    print(f"Wrote outputs to {output_dir}")


if __name__ == "__main__":
    main()