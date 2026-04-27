import re
from argparse import ArgumentParser
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
TRANSITION_RE = re.compile(r"TRANSITION:\s*(STATE_[A-Z0-9_]+)\s*->\s*(STATE_[A-Z0-9_]+)")
STATE_CHANGE_RE = re.compile(r"STATE_CHANGE:\s*(STATE_[A-Z0-9_]+)\s*->\s*(STATE_[A-Z0-9_]+)")
FIELD_LOG_STATE_RE = re.compile(
    r"Field log state #\d+:\s*(STATE_[A-Z0-9_]+)\s*->\s*(STATE_[A-Z0-9_]+)"
)

STATE_LABELS = {
    "STATE_BOOT": "Boot",
    "STATE_IDLE": "Idle",
    "STATE_GNSS_ACQUIRE": "GNSS acquire",
    "STATE_NTN_CONNECTING": "NTN connecting",
    "STATE_NTN_CONNECTED": "NTN connected",
    "STATE_LTEM_CONNECTING": "LTE-M connecting",
    "STATE_LTEM_CONNECTED": "LTE-M connected",
    "STATE_CLOUD_CONNECTING": "Cloud connecting",
    "STATE_LTE_LOCATION": "LTE location",
    "STATE_LTE_PROBE": "LTE probe",
    "STATE_BACKOFF": "Backoff",
}

def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


def parse_args():
    parser = ArgumentParser(description="Plot PPK current with firmware transition markers.")
    parser.add_argument(
        "--data-dir",
        default="data/raw",
        help="Directory containing current_uA.bin and ppk_events.csv.",
    )
    return parser.parse_args()


def state_label(state: str) -> str:
    return STATE_LABELS.get(state, state.replace("STATE_", "").replace("_", " ").title())


def transition_label(from_state: str, to_state: str) -> str:
    if to_state == "STATE_NTN_CONNECTED":
        return "Switched to NTN"
    if to_state == "STATE_NTN_CONNECTING":
        return "Starting NTN"
    if from_state == "STATE_LTEM_CONNECTED" and to_state == "STATE_BACKOFF":
        return "LTE-M fallback"
    if to_state == "STATE_LTEM_CONNECTED":
        return "LTE-M connected"
    if to_state == "STATE_GNSS_ACQUIRE":
        return "GNSS before NTN"
    if to_state == "STATE_LTE_PROBE":
        return "LTE probe"

    return f"{state_label(from_state)} -> {state_label(to_state)}"


def event_marker(message: str):
    match = (
        STATE_CHANGE_RE.search(message)
        or TRANSITION_RE.search(message)
        or FIELD_LOG_STATE_RE.search(message)
    )
    if match:
        from_state = match.group(1)
        to_state = match.group(2)
        return transition_label(from_state, to_state), f"{from_state}->{to_state}"

    if "Trying to connect NTN" in message:
        return "Starting NTN", "trying-ntn"
    if "ntn registered ok" in message:
        return "Switched to NTN", "ntn-registered"
    if "switch: sending XSYSTEMMODE NTN" in message:
        return "Modem mode NTN", "modem-mode-ntn"
    if "switch: sending XSYSTEMMODE TN" in message:
        return "Modem mode LTE-M", "modem-mode-tn"
    if "LTE probe: TN good" in message:
        return "LTE-M recovered", "lte-recovered"
    if "LTE probe: TN still bad" in message:
        return "Stay on NTN", "stay-ntn"
    if "DUT_POWER_ON" in message:
        return "DUT power on", "dut-power-on"
    if "AMPERE_MODE_PASS_THROUGH_ON" in message:
        return "Measurement start", "measurement-start"
    if "DUT_OUTPUT_SWITCH_DISABLED" in message:
        return "Output switch disabled", "output-switch-disabled"

    return None, None


SAMPLE_RATE_HZ = 100_000
args = parse_args()
DATA_DIR = Path(args.data_dir)

samples = np.fromfile(DATA_DIR / "current_uA.bin", dtype=np.float32)
events = pd.read_csv(DATA_DIR / "ppk_events.csv")

events["message"] = events["message"].astype(str).map(strip_ansi)
events["t_sample_s"] = events["sample_idx"] / SAMPLE_RATE_HZ
events["t_host_s"] = events["t_ns"] / 1e9

print(events[["t_sample_s", "source", "message"]].head(20))
print(f"samples: {len(samples)}")
print(f"duration: {len(samples) / SAMPLE_RATE_HZ:.2f} s")

t = np.arange(len(samples)) / SAMPLE_RATE_HZ

plt.figure()
plt.plot(t, samples, linewidth=0.8)

plot_top = np.nanmax(samples) if len(samples) else 0
markers = []
last_marker_by_key = {}

for _, row in events.iterrows():
    msg = row["message"]
    label, key = event_marker(msg)
    if label:
        x = row["t_sample_s"]
        previous_x = last_marker_by_key.get(key)

        if previous_x is not None and abs(x - previous_x) <= 1.0:
            continue

        last_marker_by_key[key] = x
        markers.append((x, label, msg))
        plt.axvline(x, linestyle="--", linewidth=0.8)
        plt.text(
            x,
            plot_top,
            label,
            rotation=90,
            fontsize=8,
            va="top",
            bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.75, "pad": 1.0},
        )

print("\nmarkers:")
for x, label, msg in markers:
    print(f"{x:9.3f}s  {label:24s}  {msg}")

plt.xlabel("Time [s]")
plt.ylabel("Current [uA]")
plt.title("PPK current with UART events")
plt.tight_layout()
plt.show()
