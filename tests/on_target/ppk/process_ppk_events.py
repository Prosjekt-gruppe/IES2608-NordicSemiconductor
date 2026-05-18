#!/usr/bin/env python3
"""This script converts the raw PPK/UART event log into analysis-friendly CSV tables.
The raw log is kept unchanged; all processed files are generated reproducibly from it.
"""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path
from typing import Any, Optional

import pandas as pd


DEFAULT_INPUT = Path("data/raw/ppk_events.csv")
DEFAULT_OUTPUT_DIR = Path("data/processed")
DEFAULT_COLUMNS = ("t_ns", "sample_idx", "source", "message")

ANSI_RE = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
PROMPT_RE = re.compile(r"(?:^|\s)uart:~\$\s*")
ZEPHYR_LOG_RE = re.compile(
    r"^\[?(?P<time>\d{2}:\d{2}:\d{2}\.\d{3},\d{3})\]?\s*"
    r"<(?P<level>[A-Za-z]+)>\s*(?P<module>[^:]+):\s*(?P<body>.*)$"
)
ZEPHYR_NO_TIME_RE = re.compile(
    r"^<(?P<level>[A-Za-z]+)>\s*(?P<module>[^:]+):\s*(?P<body>.*)$"
)
SHORT_LOG_RE = re.compile(r"^(?P<level>[DIWE])\s*:\s*(?P<body>.*)$")

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

SUMMARY_IGNORED_STATES = {
    "STATE_CONNECTED",
    "STATE_DISCONNECTED",
    "STATE_RUNNING",
}

CONN_EVAL_RE = re.compile(r"conn eval:\s*(.*)", re.IGNORECASE)
FIELD_LOG_CONNEVAL_RE = re.compile(r"Field log conneval #\d+:\s*(.*)", re.IGNORECASE)
NTN_MONITOR_RE = re.compile(r"NTN monitor:\s*(.*)", re.IGNORECASE)
XMONITOR_RE = re.compile(r"%?XMONITOR", re.IGNORECASE)
KEYVAL_RE = re.compile(r"\b([A-Za-z_]+)\s*=\s*(-?\d+(?:\.\d+)?)")
MONITOR_KEYVAL_RE = re.compile(r"\b([A-Za-z_]+)\s*=\s*([0-9A-Fa-f]+)\b")
LTE_M_RSRP_RE = re.compile(r"LTE-M RSRP:\s*(-?\d+)\s*dBm", re.IGNORECASE)
RRC_RE = re.compile(r"\brrc(?:_state)?\s*[:=]\s*([A-Za-z0-9_-]+)", re.IGNORECASE)
BAND_RE = re.compile(r"\bband\s*[:=]\s*(\d+)", re.IGNORECASE)
EARFCN_RE = re.compile(r"\bearfcn\s*[:=]\s*(\d+)", re.IGNORECASE)
CELL_ID_RE = re.compile(r"\bcell(?:_id|id)?\s*[:=]\s*(0x[0-9A-Fa-f]+|\d+)", re.IGNORECASE)
TX_POWER_RE = re.compile(r"\btx(?:_pwr|_power|power)?\s*[:=]\s*(-?\d+)\b", re.IGNORECASE)
CE_LEVEL_RE = re.compile(r"\bce(?:_level)?\s*[:=]\s*(-?\d+)\b", re.IGNORECASE)
ENERGY_RE = re.compile(r"\benergy(?:_estimate)?\s*[:=]\s*(-?\d+(?:\.\d+)?)", re.IGNORECASE)

XYZ_RE = re.compile(r"xyz=\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*\)")
ACCEL_RE = re.compile(r"\baccel(?:eration)?\s*[:=]\s*(-?\d+(?:\.\d+)?)", re.IGNORECASE)
DELTA_RE = re.compile(r"\bdelta\s*[:=]\s*(-?\d+(?:\.\d+)?)", re.IGNORECASE)
SPEED_RE = re.compile(r"\bspeed\s*[:=]\s*(-?\d+(?:\.\d+)?)", re.IGNORECASE)
MOTION_RE = re.compile(r"\bmotion(?:_state)?\s*[:=]\s*([A-Za-z0-9_-]+)", re.IGNORECASE)


def strip_ansi(text: str) -> str:
    if not text:
        return ""
    return ANSI_RE.sub("", text)


def parse_number(value: Optional[str]) -> Optional[float]:
    if value is None:
        return None
    text = value.strip()
    if not text:
        return None
    try:
        if text.lower().startswith("0x"):
            return float(int(text, 16))
        if "." in text:
            return float(text)
        return float(int(text))
    except ValueError:
        return None


def detect_header(fields: list[str]) -> bool:
    lowered = [field.strip().lower() for field in fields]
    return any(name in lowered for name in DEFAULT_COLUMNS)


def parse_csv_line(line: str) -> tuple[list[str], bool]:
    try:
        row = next(csv.reader([line]))
        return row, False
    except csv.Error:
        return line.rstrip("\n").split(","), True


def robust_read_events(path: Path) -> tuple[pd.DataFrame, int, int]:
    rows: list[dict[str, Any]] = []
    malformed_rows = 0
    skipped_rows = 0

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        first_line = handle.readline()
        if not first_line:
            return pd.DataFrame(columns=["t_ns", "sample_idx", "source", "raw_message", "row_status"]), 0, 0

        header_fields, header_fallback = parse_csv_line(first_line)
        has_header = (not header_fallback) and detect_header(header_fields)

        def process_line(raw_line: str, used_fallback: bool) -> None:
            nonlocal malformed_rows, skipped_rows
            if not raw_line.strip():
                skipped_rows += 1
                return

            row_fields, row_fallback = parse_csv_line(raw_line)
            used_fallback = used_fallback or row_fallback

            if len(row_fields) >= 4:
                t_ns = row_fields[0]
                sample_idx = row_fields[1]
                source = row_fields[2]
                message = ",".join(row_fields[3:])
                row_status = "malformed" if used_fallback else "ok"
            elif len(row_fields) >= 3:
                t_ns = row_fields[0]
                sample_idx = row_fields[1]
                source = row_fields[2]
                message = ""
                row_status = "malformed"
            elif len(row_fields) == 2:
                t_ns = row_fields[0]
                sample_idx = row_fields[1]
                source = ""
                message = raw_line.strip()
                row_status = "malformed"
            elif len(row_fields) == 1:
                t_ns = row_fields[0]
                sample_idx = ""
                source = ""
                message = raw_line.strip()
                row_status = "malformed"
            else:
                t_ns = ""
                sample_idx = ""
                source = ""
                message = raw_line.strip()
                row_status = "malformed"

            if row_status == "malformed":
                malformed_rows += 1

            rows.append(
                {
                    "t_ns": t_ns,
                    "sample_idx": sample_idx,
                    "source": source,
                    "raw_message": message,
                    "row_status": row_status,
                }
            )

        if has_header:
            for line in handle:
                process_line(line, used_fallback=False)
        else:
            process_line(first_line, used_fallback=header_fallback)
            for line in handle:
                process_line(line, used_fallback=False)

    return pd.DataFrame(rows), malformed_rows, skipped_rows


def normalize_message(message: str) -> str:
    cleaned = strip_ansi(message).replace("\r", "").strip()
    cleaned = PROMPT_RE.sub("", cleaned).strip()
    return cleaned


def parse_zephyr_log(message: str) -> Optional[dict[str, str]]:
    if not message:
        return None
    msg = normalize_message(message)
    if not msg:
        return None

    match = ZEPHYR_LOG_RE.match(msg)
    if match:
        return {
            "zephyr_time": match.group("time"),
            "log_level": match.group("level").lower(),
            "module": match.group("module").strip(),
            "body": match.group("body").strip(),
        }

    match = ZEPHYR_NO_TIME_RE.match(msg)
    if match:
        return {
            "zephyr_time": "",
            "log_level": match.group("level").lower(),
            "module": match.group("module").strip(),
            "body": match.group("body").strip(),
        }

    match = SHORT_LOG_RE.match(msg)
    if match:
        level_map = {"I": "inf", "W": "wrn", "E": "err", "D": "dbg"}
        return {
            "zephyr_time": "",
            "log_level": level_map.get(match.group("level"), match.group("level").lower()),
            "module": "",
            "body": match.group("body").strip(),
        }

    return None


def parse_state_transition(message: str) -> tuple[Optional[str], Optional[str], Optional[str]]:
    match = FIELD_LOG_STATE_RE.search(message)
    if match:
        return match.group(1), match.group(2), "field_log"

    match = TRANSITION_RE.search(message)
    if match:
        return match.group(1), match.group(2), "transition"

    enter_match = ENTER_STATE_RE.search(message)
    if enter_match:
        return None, enter_match.group(1), "enter"

    exit_match = EXIT_STATE_RE.search(message)
    if exit_match:
        return exit_match.group(1), None, "exit"

    return None, None, None


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


def event_marker(message: str) -> tuple[Optional[str], Optional[str], Optional[str], Optional[str]]:
    from_state, to_state, _ = parse_state_transition(message)
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


def parse_conneval(message: str) -> Optional[dict[str, Any]]:
    if not message:
        return None
    lowered = message.lower()
    if "ntn monitor" in lowered or XMONITOR_RE.search(message):
        return None

    data: dict[str, Any] = {}

    field_match = FIELD_LOG_CONNEVAL_RE.search(message)
    conn_match = CONN_EVAL_RE.search(message)
    lte_rsrp_match = LTE_M_RSRP_RE.search(message)

    if field_match:
        payload = field_match.group(1)
        source_type = "field_log_conneval"
    elif conn_match:
        payload = conn_match.group(1)
        source_type = "rsrp_service_conn_eval"
    elif lte_rsrp_match:
        rsrp_dbm = parse_number(lte_rsrp_match.group(1))
        if rsrp_dbm is None:
            return None
        data["source_type"] = "lte_rsrp_dbm"
        data["rsrp_dbm"] = int(rsrp_dbm)
        data["rsrp_dbm_derived"] = 0
        return data
    else:
        return None

    data["source_type"] = source_type

    key_map = {
        "rsrp": "rsrp_raw",
        "rsrq": "rsrq",
        "snr": "snr",
        "band": "band",
        "earfcn": "earfcn",
        "cell_id": "cell_id",
        "cellid": "cell_id",
        "tx_pwr": "tx_power",
        "tx_power": "tx_power",
        "txpwr": "tx_power",
        "ce": "ce_level",
        "ce_level": "ce_level",
        "energy": "energy_estimate",
        "energy_estimate": "energy_estimate",
        "rrc": "rrc_state",
        "rrc_state": "rrc_state",
    }

    for key, value in KEYVAL_RE.findall(payload):
        target = key_map.get(key.lower())
        if not target:
            continue
        parsed = parse_number(value)
        if parsed is not None:
            data[target] = int(parsed) if parsed.is_integer() else parsed

    rrc_match = RRC_RE.search(message)
    if rrc_match:
        rrc_value = rrc_match.group(1)
        parsed_rrc = parse_number(rrc_value)
        data.setdefault("rrc_state", int(parsed_rrc) if parsed_rrc is not None else rrc_value)

    band_match = BAND_RE.search(message)
    if band_match:
        data.setdefault("band", int(band_match.group(1)))

    earfcn_match = EARFCN_RE.search(message)
    if earfcn_match:
        data.setdefault("earfcn", int(earfcn_match.group(1)))

    cell_id_match = CELL_ID_RE.search(message)
    if cell_id_match:
        cell_value = cell_id_match.group(1)
        parsed_cell = parse_number(cell_value)
        if parsed_cell is not None:
            data.setdefault("cell_id", int(parsed_cell))

    tx_match = TX_POWER_RE.search(message)
    if tx_match:
        data.setdefault("tx_power", int(tx_match.group(1)))

    ce_match = CE_LEVEL_RE.search(message)
    if ce_match:
        data.setdefault("ce_level", int(ce_match.group(1)))

    energy_match = ENERGY_RE.search(message)
    if energy_match:
        data.setdefault("energy_estimate", parse_number(energy_match.group(1)))

    rsrp_raw = data.get("rsrp_raw")
    if rsrp_raw is not None:
        rsrp_raw_int = int(rsrp_raw)
        data["rsrp_raw"] = rsrp_raw_int
        data["rsrp_dbm"] = rsrp_raw_int - 141
        data["rsrp_dbm_derived"] = 1

    return data or None


def parse_ntn_monitor(message: str) -> Optional[dict[str, Any]]:
    if not message:
        return None
    if XMONITOR_RE.search(message):
        return None

    match = NTN_MONITOR_RE.search(message)
    if not match:
        return None

    payload = match.group(1)
    data: dict[str, Any] = {}

    key_map = {
        "reg": "reg",
        "act": "act",
        "plmn": "plmn",
        "tac": "tac",
        "band": "band",
        "cell": "cell",
        "pci": "pci",
        "earfcn": "earfcn",
        "rsrp": "rsrp_raw",
        "snr": "snr",
    }
    string_keys = {"plmn", "tac", "cell"}

    for key, value in MONITOR_KEYVAL_RE.findall(payload):
        target = key_map.get(key.lower())
        if not target:
            continue
        if key.lower() in string_keys:
            data[target] = value
            continue
        parsed = parse_number(value)
        if parsed is not None:
            data[target] = int(parsed) if parsed.is_integer() else parsed

    if not data:
        return None

    return data


def parse_accel(message: str) -> Optional[dict[str, Any]]:
    if not message:
        return None

    lowered = message.lower()
    if "accel" not in lowered and "motion" not in lowered:
        return None

    data: dict[str, Any] = {}

    xyz_match = XYZ_RE.search(message)
    if xyz_match:
        data["x"] = parse_number(xyz_match.group(1))
        data["y"] = parse_number(xyz_match.group(2))
        data["z"] = parse_number(xyz_match.group(3))

    accel_match = ACCEL_RE.search(message)
    if accel_match:
        data["accel"] = parse_number(accel_match.group(1))

    delta_match = DELTA_RE.search(message)
    if delta_match:
        data["delta"] = parse_number(delta_match.group(1))

    speed_match = SPEED_RE.search(message)
    if speed_match:
        data["speed"] = parse_number(speed_match.group(1))

    motion_match = MOTION_RE.search(message)
    if motion_match:
        data["motion_state"] = motion_match.group(1)

    return data or None


def build_markers(events: pd.DataFrame) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []

    for _, row in events.iterrows():
        message = row.get("message", "")
        label, key, from_state, to_state = event_marker(str(message))
        if not label or not key:
            continue
        if from_state is not None and to_state is None:
            continue
        if (from_state in SUMMARY_IGNORED_STATES) or (to_state in SUMMARY_IGNORED_STATES):
            continue

        rows.append(
            {
                "t_ns": row.get("t_ns"),
                "sample_idx": row.get("sample_idx"),
                "t_sample_s": row.get("t_sample_s"),
                "label": label,
                "key": key,
                "from_state": from_state,
                "to_state": to_state,
                "message": message,
            }
        )

    return pd.DataFrame(rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert raw PPK/UART events into processed CSV tables."
    )
    parser.add_argument(
        "--input",
        default=str(DEFAULT_INPUT),
        help="Input ppk_events.csv path (default: data/raw/ppk_events.csv).",
    )
    parser.add_argument(
        "--output-dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help="Output directory for processed CSVs (default: data/processed).",
    )
    parser.add_argument(
        "--sample-rate-hz",
        type=float,
        default=None,
        help="Sample rate in Hz for computing t_sample_s.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    sample_rate_hz = args.sample_rate_hz

    if not input_path.exists():
        print(f"Input file not found: {input_path}")
        return 1

    output_dir.mkdir(parents=True, exist_ok=True)

    raw_events, malformed_rows, skipped_rows = robust_read_events(input_path)
    raw_rows_loaded = len(raw_events)

    raw_events["t_ns"] = pd.to_numeric(raw_events["t_ns"], errors="coerce")
    raw_events["sample_idx"] = pd.to_numeric(raw_events["sample_idx"], errors="coerce")
    raw_events["source"] = raw_events["source"].fillna("")
    raw_events["raw_message"] = raw_events["raw_message"].fillna("")

    clean_rows: list[dict[str, Any]] = []
    for _, row in raw_events.iterrows():
        raw_message = str(row.get("raw_message", ""))
        message = normalize_message(raw_message)
        zephyr = parse_zephyr_log(message)
        parse_status = "zephyr" if zephyr else "raw"
        if row.get("row_status") == "malformed":
            parse_status = "malformed"

        clean_rows.append(
            {
                "t_ns": row.get("t_ns"),
                "sample_idx": row.get("sample_idx"),
                "source": row.get("source", ""),
                "raw_message": raw_message,
                "message": message,
                "zephyr_time": zephyr.get("zephyr_time") if zephyr else "",
                "log_level": zephyr.get("log_level") if zephyr else "",
                "module": zephyr.get("module") if zephyr else "",
                "body": zephyr.get("body") if zephyr else "",
                "parse_status": parse_status,
            }
        )

    events_clean = pd.DataFrame(clean_rows)
    if sample_rate_hz:
        events_clean["t_sample_s"] = events_clean["sample_idx"] / float(sample_rate_hz)
    else:
        events_clean["t_sample_s"] = pd.NA

    events_columns = [
        "t_ns",
        "sample_idx",
        "t_sample_s",
        "source",
        "raw_message",
        "message",
        "zephyr_time",
        "log_level",
        "module",
        "body",
        "parse_status",
    ]
    events_clean = events_clean[events_columns]
    events_clean.to_csv(output_dir / "events_clean.csv", index=False)

    transition_rows: list[dict[str, Any]] = []
    for _, row in events_clean.iterrows():
        message = str(row.get("message", ""))
        from_state, to_state, event_kind = parse_state_transition(message)
        if not from_state and not to_state:
            continue
        transition_rows.append(
            {
                "t_ns": row.get("t_ns"),
                "sample_idx": row.get("sample_idx"),
                "t_sample_s": row.get("t_sample_s"),
                "from_state": from_state,
                "to_state": to_state,
                "event_kind": event_kind,
                "message": message,
            }
        )

    transitions_df = pd.DataFrame(transition_rows)
    transitions_df = transitions_df[
        [
            "t_ns",
            "sample_idx",
            "t_sample_s",
            "from_state",
            "to_state",
            "event_kind",
            "message",
        ]
    ]
    transitions_df.to_csv(output_dir / "state_transitions.csv", index=False)

    conneval_rows: list[dict[str, Any]] = []
    for _, row in events_clean.iterrows():
        message = str(row.get("message", ""))
        parsed = parse_conneval(message)
        if not parsed:
            continue
        conneval_rows.append(
            {
                "t_ns": row.get("t_ns"),
                "sample_idx": row.get("sample_idx"),
                "t_sample_s": row.get("t_sample_s"),
                "source": row.get("source"),
                "source_type": parsed.get("source_type"),
                "rsrp_raw": parsed.get("rsrp_raw"),
                "rsrp_dbm": parsed.get("rsrp_dbm"),
                "rsrp_dbm_derived": parsed.get("rsrp_dbm_derived"),
                "rsrq": parsed.get("rsrq"),
                "snr": parsed.get("snr"),
                "band": parsed.get("band"),
                "earfcn": parsed.get("earfcn"),
                "cell_id": parsed.get("cell_id"),
                "tx_power": parsed.get("tx_power"),
                "ce_level": parsed.get("ce_level"),
                "energy_estimate": parsed.get("energy_estimate"),
                "rrc_state": parsed.get("rrc_state"),
                "raw_message": row.get("raw_message"),
                "message": message,
            }
        )

    conneval_df = pd.DataFrame(conneval_rows).reindex(
        columns=[
            "t_ns",
            "sample_idx",
            "t_sample_s",
            "source",
            "source_type",
            "rsrp_raw",
            "rsrp_dbm",
            "rsrp_dbm_derived",
            "rsrq",
            "snr",
            "band",
            "earfcn",
            "cell_id",
            "tx_power",
            "ce_level",
            "energy_estimate",
            "rrc_state",
            "raw_message",
            "message",
        ]
    )
    conneval_df.to_csv(output_dir / "conneval.csv", index=False)

    ntn_rows: list[dict[str, Any]] = []
    for _, row in events_clean.iterrows():
        message = str(row.get("message", ""))
        parsed = parse_ntn_monitor(message)
        if not parsed:
            continue
        ntn_rows.append(
            {
                "t_ns": row.get("t_ns"),
                "sample_idx": row.get("sample_idx"),
                "t_sample_s": row.get("t_sample_s"),
                "source": row.get("source"),
                "reg": parsed.get("reg"),
                "act": parsed.get("act"),
                "plmn": parsed.get("plmn"),
                "tac": parsed.get("tac"),
                "band": parsed.get("band"),
                "cell": parsed.get("cell"),
                "pci": parsed.get("pci"),
                "earfcn": parsed.get("earfcn"),
                "rsrp_raw": parsed.get("rsrp_raw"),
                "snr": parsed.get("snr"),
                "raw_message": row.get("raw_message"),
                "message": message,
            }
        )

    ntn_df = pd.DataFrame(ntn_rows).reindex(
        columns=[
            "t_ns",
            "sample_idx",
            "t_sample_s",
            "source",
            "reg",
            "act",
            "plmn",
            "tac",
            "band",
            "cell",
            "pci",
            "earfcn",
            "rsrp_raw",
            "snr",
            "raw_message",
            "message",
        ]
    )
    ntn_df.to_csv(output_dir / "ntn_monitor.csv", index=False)

    accel_rows: list[dict[str, Any]] = []
    for _, row in events_clean.iterrows():
        message = str(row.get("message", ""))
        parsed = parse_accel(message)
        if not parsed:
            continue
        accel_rows.append(
            {
                "t_ns": row.get("t_ns"),
                "sample_idx": row.get("sample_idx"),
                "t_sample_s": row.get("t_sample_s"),
                "source": row.get("source"),
                "x": parsed.get("x"),
                "y": parsed.get("y"),
                "z": parsed.get("z"),
                "accel": parsed.get("accel"),
                "delta": parsed.get("delta"),
                "speed": parsed.get("speed"),
                "motion_state": parsed.get("motion_state"),
                "raw_message": row.get("raw_message"),
                "message": message,
            }
        )

    accel_df = pd.DataFrame(accel_rows)
    accel_df = accel_df[
        [
            "t_ns",
            "sample_idx",
            "t_sample_s",
            "source",
            "x",
            "y",
            "z",
            "accel",
            "delta",
            "speed",
            "motion_state",
            "raw_message",
            "message",
        ]
    ]
    accel_df.to_csv(output_dir / "accel.csv", index=False)

    markers_df = build_markers(events_clean)
    markers_df = markers_df[
        [
            "t_ns",
            "sample_idx",
            "t_sample_s",
            "label",
            "key",
            "from_state",
            "to_state",
            "message",
        ]
    ]
    markers_df.to_csv(output_dir / "markers.csv", index=False)

    malformed_total = malformed_rows + skipped_rows
    print(
        "\n".join(
            [
                f"Raw rows loaded: {raw_rows_loaded}",
                f"Cleaned events: {len(events_clean)}",
                f"State transitions extracted: {len(transitions_df)}",
                f"Conneval rows extracted: {len(conneval_df)}",
                f"NTN monitor rows extracted: {len(ntn_df)}",
                f"Accel rows extracted: {len(accel_df)}",
                f"Markers extracted: {len(markers_df)}",
                f"Malformed/skipped rows: {malformed_total}",
            ]
        )
    )
    print(f"Output directory: {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
