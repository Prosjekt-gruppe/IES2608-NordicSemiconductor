#!/usr/bin/env python3
import argparse
import csv
import os
import re
from pathlib import Path
from typing import Dict, Optional, Tuple


ANSI_ESCAPE_RE = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
CONN_EVAL_RE = re.compile(r"conn eval:\s*(.*)")
LTE_M_RSRP_RE = re.compile(r"LTE-M RSRP:\s*(-?\d+)\s*dBm")
KEYVAL_RE = re.compile(r"(\w+)=(-?\d+)")


OUTPUT_FIELDS = [
	"t_ns",
	"t_s",
	"sample_idx",
	"source",
	"rrc",
	"energy",
	"ce",
	"rsrp_raw",
	"rsrp_dbm",
	"rsrp_dbm_derived",
	"rsrq",
	"snr",
	"dl_pl",
	"tx_pwr",
	"tx_rep",
	"rx_rep",
	"earfcn",
	"band",
	"phy_cid",
	"cell_id",
	"mcc",
	"mnc",
	"message_count",
]


def strip_ansi(text: str) -> str:
	if not text:
		return text
	return ANSI_ESCAPE_RE.sub("", text)


def parse_int(value: Optional[str]) -> Optional[int]:
	if value is None or value == "":
		return None
	try:
		return int(value)
	except (TypeError, ValueError):
		return None


def parse_keyvals(payload: str) -> Dict[str, int]:
	data: Dict[str, int] = {}
	for key, value in KEYVAL_RE.findall(payload):
		parsed = parse_int(value)
		if parsed is not None:
			data[key] = parsed
	return data


def start_group(row: Dict[str, str]) -> Dict[str, Optional[int]]:
	t_ns = parse_int(row.get("t_ns"))
	return {
		"t_ns": t_ns,
		"t_s": None if t_ns is None else t_ns / 1e9,
		"sample_idx": parse_int(row.get("sample_idx")),
		"source": row.get("source") or "",
		"rrc": None,
		"energy": None,
		"ce": None,
		"rsrp_raw": None,
		"rsrp_dbm": None,
		"rsrp_dbm_derived": None,
		"rsrq": None,
		"snr": None,
		"dl_pl": None,
		"tx_pwr": None,
		"tx_rep": None,
		"rx_rep": None,
		"earfcn": None,
		"band": None,
		"phy_cid": None,
		"cell_id": None,
		"mcc": None,
		"mnc": None,
		"message_count": 0,
	}


def apply_keyvals(group: Dict[str, Optional[int]], keyvals: Dict[str, int]) -> None:
	mapping = {
		"rrc": "rrc",
		"energy": "energy",
		"ce": "ce",
		"rsrp": "rsrp_raw",
		"rsrq": "rsrq",
		"snr": "snr",
		"dl_pl": "dl_pl",
		"tx_pwr": "tx_pwr",
		"tx_rep": "tx_rep",
		"rx_rep": "rx_rep",
		"earfcn": "earfcn",
		"band": "band",
		"phy_cid": "phy_cid",
		"cell_id": "cell_id",
		"mcc": "mcc",
		"mnc": "mnc",
	}
	for key, target in mapping.items():
		if key in keyvals:
			group[target] = keyvals[key]


def finalize_group(group: Dict[str, Optional[int]]) -> Dict[str, Optional[int]]:
	if group.get("rsrp_dbm") is None and group.get("rsrp_raw") is not None:
		group["rsrp_dbm"] = group["rsrp_raw"] - 141
		group["rsrp_dbm_derived"] = 1
	elif group.get("rsrp_dbm") is not None:
		group["rsrp_dbm_derived"] = 0
	return group


def write_group(writer: csv.DictWriter, group: Dict[str, Optional[int]]) -> None:
	finalized = finalize_group(group)
	writer.writerow({k: finalized.get(k, "") for k in OUTPUT_FIELDS})


def process_events(events_csv: Path, output_csv: Path) -> int:
	output_csv.parent.mkdir(parents=True, exist_ok=True)
	rows_written = 0
	current_group: Optional[Dict[str, Optional[int]]] = None

	with events_csv.open("r", newline="") as infile, output_csv.open("w", newline="") as outfile:
		reader = csv.DictReader(infile)
		writer = csv.DictWriter(outfile, fieldnames=OUTPUT_FIELDS)
		writer.writeheader()

		for row in reader:
			raw_message = row.get("message", "")
			message = strip_ansi(raw_message)

			conn_match = CONN_EVAL_RE.search(message)
			if conn_match:
				payload = conn_match.group(1)
				keyvals = parse_keyvals(payload)

				if "rrc" in keyvals:
					if current_group is not None:
						write_group(writer, current_group)
						rows_written += 1
					current_group = start_group(row)

				if current_group is None:
					current_group = start_group(row)

				apply_keyvals(current_group, keyvals)
				current_group["message_count"] = (current_group.get("message_count") or 0) + 1
				continue

			rsrp_match = LTE_M_RSRP_RE.search(message)
			if rsrp_match:
				if current_group is None:
					current_group = start_group(row)
				current_group["rsrp_dbm"] = parse_int(rsrp_match.group(1))
				current_group["message_count"] = (current_group.get("message_count") or 0) + 1

		if current_group is not None:
			write_group(writer, current_group)
			rows_written += 1

	return rows_written


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description="Extract LTE conn eval samples into a time-series CSV."
	)
	parser.add_argument(
		"--events-csv",
		default="data/raw/ppk_events.csv",
		help="Input ppk_events.csv path",
	)
	parser.add_argument(
		"--output",
		default="output/conneval_timeseries.csv",
		help="Output time-series CSV path",
	)
	return parser.parse_args()


def main() -> int:
	args = parse_args()
	events_csv = Path(args.events_csv)
	output_csv = Path(args.output)

	if not events_csv.exists():
		print(f"Input file not found: {events_csv}")
		return 1

	rows_written = process_events(events_csv, output_csv)
	print(f"Rows written: {rows_written}")
	print(f"Output: {output_csv}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
