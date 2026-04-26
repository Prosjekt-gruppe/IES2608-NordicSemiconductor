import re
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)

SAMPLE_RATE_HZ = 100_000
DATA_DIR = Path("data/raw")

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)

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

for _, row in events.iterrows():
    msg = row["message"]
    if any(key in msg for key in ["ENTER:", "Trying", "failed", "DUT_POWER_ON"]):
        x = row["t_sample_s"]
        plt.axvline(x, linestyle="--", linewidth=0.8)
        plt.text(x, np.nanmax(samples), msg[-50:], rotation=90, fontsize=8)

plt.xlabel("Time [s]")
plt.ylabel("Current [uA]")
plt.title("PPK current with UART events")
plt.tight_layout()
plt.show()
