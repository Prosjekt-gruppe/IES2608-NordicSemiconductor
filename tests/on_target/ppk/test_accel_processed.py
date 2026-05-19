from pathlib import Path

import pandas as pd
import matplotlib.pyplot as plt


ACCEL_PATH = Path("data/processed/accel.csv")


def main() -> None:
    if not ACCEL_PATH.exists():
        raise FileNotFoundError(f"Missing file: {ACCEL_PATH}")

    df = pd.read_csv(ACCEL_PATH)

    print("Loaded accel.csv")
    print(f"Rows: {len(df)}")
    print(f"Columns: {list(df.columns)}")

    required_columns = [
        "t_sample_s",
        "x",
        "y",
        "z",
        "accel",
        "delta",
        "speed",
        "motion_state",
        "message",
    ]

    missing = [col for col in required_columns if col not in df.columns]
    if missing:
        raise ValueError(f"Missing expected columns: {missing}")

    numeric_columns = [
        "t_sample_s",
        "x",
        "y",
        "z",
        "accel",
        "delta",
        "speed",
    ]

    for col in numeric_columns:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    # Basic sanity checks
    if df["t_sample_s"].notna().sum() == 0:
        raise ValueError("No valid timestamps found in t_sample_s")

    if df["accel"].notna().sum() == 0 and df["speed"].notna().sum() == 0:
        raise ValueError("No valid accel or speed data found")

    print("\nNon-null values:")
    print(df[numeric_columns + ["motion_state"]].notna().sum())

    print("\nFirst rows:")
    print(df[["t_sample_s", "x", "y", "z", "accel", "delta", "speed", "motion_state"]].head(10))

    # Clean rows useful for plotting
    plot_df = df.dropna(subset=["t_sample_s"]).sort_values("t_sample_s")

    # Plot speed if available
    if plot_df["speed"].notna().any():
        plt.figure()
        plt.plot(plot_df["t_sample_s"], plot_df["speed"], marker="o", linewidth=1)
        plt.xlabel("Time [s]")
        plt.ylabel("Speed [mm/s]")
        plt.title("Accelerometer-derived speed")
        plt.tight_layout()
        plt.show()

    # Plot acceleration if available
    if plot_df["accel"].notna().any():
        plt.figure()
        plt.plot(plot_df["t_sample_s"], plot_df["accel"], marker="o", linewidth=1)
        plt.xlabel("Time [s]")
        plt.ylabel("Acceleration [mg]")
        plt.title("Accelerometer activity")
        plt.tight_layout()
        plt.show()

    print("\nAccel processed data looks usable.")


if __name__ == "__main__":
    main()