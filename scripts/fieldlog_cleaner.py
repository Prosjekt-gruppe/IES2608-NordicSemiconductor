#!/usr/bin/env python3
import argparse
import re
from pathlib import Path



# Removes injected AT command fragments like:
# conn> AT%XDATAPRFL?
# > AT+CESQ
# LTE-> AT%XTIME=1
AT_FRAGMENT_RE = re.compile(r"(?:\w+)?\s*>\s*AT[^\r\n,]*")

HEADER_PREFIXES = (
    "# fieldlog csv",
    "type,seq,uptime_s",
    "acy_m,",
    "itchbacks_interval,",
    "tx_power,",
)
VALID_PREFIXES = ("state,", "conneval,", "summary,")

EXPECTED_MIN_FIELDS = {
    "state": 6,
    "conneval": 4,
    "summary": 4,
}

CSV_HEADER = (
    "type,seq,uptime_s,from_state,to_state,reason,active_rat,next_rat,"
    "location_source,latitude,longitude,accuracy_m,last_rsrp_dbm,"
    "interval_s,power_interval_uwh,power_total_uwh,"
    "lte_losses_interval,lte_losses_total,"
    "switchbacks_interval,switchbacks_total,flags,dropped_messages,"
    "rrc_state,ce_level,rsrp,rsrq,snr,dl_pathloss,tx_power,tx_rep,rx_rep"
)

def is_valid_record(line: str) -> bool:
    if ">" in line or line.startswith("AT"):
        return False
    

    if not line.strip(","):
        return False

    fields = line.split(",")
    record_type = fields[0]

    if record_type == "state":
        return (
            len(fields) >= 13
            and fields[3].startswith("STATE_")
            and fields[4].startswith("STATE_")
            and fields[5].startswith("EVT_")
        )

    if record_type == "conneval":
        return len(fields) >= 4

    if record_type == "summary":
        return len(fields) >= 4

    return False


def clean_line(line: str) -> str | None:
    line = line.strip()

    if not line:
        return None
    
    #if line.startswith(HEADER_PREFIXES):
    #    return line

    line = AT_FRAGMENT_RE.sub("", line).strip()

    if line.startswith("eval,"):
        line = "conneval," + line[len("eval,"):]

    if not is_valid_record(line):
        return None

    return line

def clean_file(input_path: Path, output_path: Path) -> None:
    kept = 0
    dropped = 0
    repaired = 0

    with input_path.open("r", encoding="utf-8", errors="replace") as f_in, \
         output_path.open("w", encoding="utf-8", newline="") as f_out:

        f_out.write(CSV_HEADER + "\n")

        for raw in f_in:
            before = raw.strip()
            cleaned = clean_line(raw)

            if cleaned is None:
                dropped += 1
                print("DROPPED:", before)
                continue

            if cleaned != before:
                repaired += 1

            f_out.write(cleaned + "\n")
            kept += 1

    print(f"Input:    {input_path}")
    print(f"Output:   {output_path}")
    print(f"Kept:     {kept}")
    print(f"Dropped:  {dropped}")
    print(f"Repaired: {repaired}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Clean embedded CSV logs by removing injected AT command fragments."
    )
    parser.add_argument("input", type=Path)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output CSV path. Default: <input>_clean.csv",
    )

    args = parser.parse_args()

    output = args.output
    if output is None:
        output = args.input.with_name(args.input.stem + "_clean.csv")

    clean_file(args.input, output)


if __name__ == "__main__":
    main()