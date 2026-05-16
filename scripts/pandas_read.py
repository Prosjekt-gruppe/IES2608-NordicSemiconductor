import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv("./data/field-log-csv-and-uart_clean.csv")

print(df)

df["uptime_s"] = pd.to_numeric(df["uptime_s"], errors="coerce")
df["rsrp"] = pd.to_numeric(df["rsrp"], errors="coerce")

# only conneval rows
df = df[df["type"] == "conneval"].copy()

# detect reboot when uptime goes backwards
df["session"] = (df["uptime_s"].diff() < 0).cumsum()

plt.figure(figsize=(12, 6))

for session_id, group in df.groupby("session"):
    plt.plot(group["uptime_s"], group["rsrp"], marker="o")

plt.xlabel("Time [s]")
plt.ylabel("RSRP [dBm]")
plt.title("LTE RSRP over time")
plt.grid(True)

plt.show()