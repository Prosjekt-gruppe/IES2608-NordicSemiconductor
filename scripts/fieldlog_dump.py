#!/usr/bin/env python3

import os
import time
import serial
from argparse import ArgumentParser

DEFAULT_UART_PORT = "/dev/serial/by-id/usb-Nordic_Semiconductor_Thingy:91_X_UART_THINGY91X_135CE6BA227-if01"
DEFAULT_BAUDRATE = 115200
DEFAULT_OUT = "data/fieldlog_dump.txt"


def dump_fieldlog(port: str, baudrate: int, output_path: str) -> None:
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

    try:
        with serial.Serial(port, baudrate=baudrate, timeout=0.5) as ser:
            time.sleep(0.3)

            # Wake shell / clear stale prompt noise
            ser.write(b"\n")
            ser.flush()
            time.sleep(0.2)
            ser.reset_input_buffer()

            ser.write(b"fieldlog dump\n")
            ser.flush()

            deadline = time.time() + 30.0

            with open(output_path, "w", encoding="utf-8") as f:
                while time.time() < deadline:
                    line = ser.readline()
                    if not line:
                        continue

                    text = line.decode(errors="replace")
                    print(text, end="")
                    f.write(text)

                    if "# records=" in text:
                        print(f"\nFieldlog dumped to {output_path}")
                        return

        print(f"\nFieldlog dump timed out; partial dump saved to {output_path}")

    except Exception as e:
        print(f"Fieldlog dump failed: {e}")


def main() -> None:
    parser = ArgumentParser(description="Dump Zephyr fieldlog over UART shell.")
    parser.add_argument("--port", default=DEFAULT_UART_PORT)
    parser.add_argument("--baudrate", type=int, default=DEFAULT_BAUDRATE)
    parser.add_argument("--output", default=DEFAULT_OUT)
    args = parser.parse_args()

    dump_fieldlog(args.port, args.baudrate, args.output)


if __name__ == "__main__":
    main()