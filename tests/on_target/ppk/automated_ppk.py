# Adapted from Nordic Semiconductor / Asset-Tracker-Template
# Source: https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/tests/on_target/tests/test_ppk/test_power.py
# License: LicenseRef-Nordic-5-Clause (see upstream LICENSE)
# Changes: refactored for our test setup

import os
import queue
import threading
import time
from argparse import ArgumentParser
from dataclasses import dataclass, field
from threading import Lock

import numpy as np
import serial
from ppk2_api.ppk2_api import PPK2_API, PPK2_Command

class PatchedPPK2(PPK2_API):
    def _read_metadata(self, timeout_s=2.0):
        """Read metadata robustly until END is seen or timeout occurs."""
        deadline = time.time() + timeout_s
        buf = b""

        while time.time() < deadline:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)

                try:
                    decoded = buf.decode("utf-8")
                except UnicodeDecodeError:
                    time.sleep(0.05)
                    continue

                if "END" in decoded:
                    return decoded

            time.sleep(0.05)

        return None

    def get_modifiers(self):
        """Flush input, request metadata, and only accept valid parse."""
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        time.sleep(0.1)

        self._write_serial((PPK2_Command.GET_META_DATA,))
        metadata = self._read_metadata()

        if metadata is None:
            return False

        ret = self._parse_metadata(metadata)
        if not ret:
            return False

        # Simple sanity check: if these are still None, metadata was not really loaded
        if self.modifiers["Calibrated"] is None and self.modifiers["HW"] is None:
            return False

        return True

    def start_measuring(self):
        """Reset parser/filter state before each measurement session."""
        self.remainder = {"sequence": b"", "len": 0}
        self.rolling_avg = None
        self.rolling_avg4 = None
        self.prev_range = None
        self.consecutive_range_samples = 0
        self.after_spike = 0
        super().start_measuring()

@dataclass
class RecordingState:
    t0_ns: int
    sample_rate_hz: int
    base_sample_idx: int = 0
    total_samples_written: int = 0
    lock: Lock = field(default_factory=Lock)

    def reset(self, base_sample_idx: int = 0):
        with self.lock:
            self.t0_ns = time.monotonic_ns()
            self.base_sample_idx = base_sample_idx
            self.total_samples_written = 0

    def mark_samples_written(self, n: int):
        with self.lock:
            self.total_samples_written += n

    def snapshot(self):
        with self.lock:
            t_ns = time.monotonic_ns() - self.t0_ns
            sample_idx = self.base_sample_idx + self.total_samples_written
        return t_ns, sample_idx
    

rec_state = RecordingState(
    t0_ns=time.monotonic_ns(),
    sample_rate_hz=100_000,
)


DATA_DIR = "data/raw"
PPK2_PORT = "/dev/serial/by-id/usb-Nordic_Semiconductor_PPK2_CB6017A1DC4B-if01"
UART_PORT = "/dev/serial/by-id/usb-SEGGER_J-Link_001052041270-if00"
UART_BAUDRATE = 115200
SOURCE_VOLTAGE_MV = 3300
MEASUREMENT_MODE = "ampere"


raw_file = None
event_file = None


uart_queue = queue.Queue()
stop_event = threading.Event()
measurement_started_event = threading.Event()


def parse_args():
    parser = ArgumentParser(description="Record PPK2 current and UART events.")
    parser.add_argument(
        "--ppk2-port",
        default=PPK2_PORT,
        help="Serial port for the PPK2.",
    )
    parser.add_argument(
        "--uart-port",
        default=UART_PORT,
        help="Serial port for DUT UART logs.",
    )
    parser.add_argument(
        "--uart-baudrate",
        type=int,
        default=UART_BAUDRATE,
        help="UART baudrate for DUT logs.",
    )
    parser.add_argument(
        "--mode",
        choices=("ampere", "source"),
        default=MEASUREMENT_MODE,
        help=(
            "PPK2 measurement mode. Use 'ampere' when the PPK2 is inserted "
            "in series with an already-powered rail. Use 'source' when the "
            "PPK2 should power the DUT."
        ),
    )
    parser.add_argument(
        "--disable-output-switch",
        action="store_true",
        help=(
            "Do not enable the PPK2 DUT output switch. In ampere mode this "
            "normally must stay enabled because it closes the VIN-to-VOUT "
            "measurement path."
        ),
    )
    parser.add_argument(
        "--no-power-cycle",
        action="store_true",
        help=(
            "Leave the PPK2 DUT output switch in its current state before "
            "recording. By default the script turns it off first, starts "
            "recording, then turns it on so boot/state events align with "
            "sample indices."
        ),
    )
    parser.add_argument(
        "--source-voltage-mv",
        type=int,
        default=SOURCE_VOLTAGE_MV,
        help=(
            "Voltage in mV used by PPK2 source mode. The ppk2-api also uses "
            "this value for sample conversion in ampere mode."
        ),
    )
    parser.add_argument(
        "--data-dir",
        default=DATA_DIR,
        help="Directory for current_uA.bin and ppk_events.csv.",
    )
    parser.add_argument(
        "--append",
        action="store_true",
        help=(
            "Append to existing output files. By default a run overwrites old "
            "PPK data so event sample indices start at the beginning of the "
            "plotted capture."
        ),
    )
    return parser.parse_args()


def open_output_files(data_dir: str, append: bool) -> int:
    global raw_file, event_file

    os.makedirs(data_dir, exist_ok=True)

    raw_path = os.path.join(data_dir, "current_uA.bin")
    event_path = os.path.join(data_dir, "ppk_events.csv")
    base_sample_idx = 0

    if append and os.path.exists(raw_path):
        base_sample_idx = os.path.getsize(raw_path) // np.dtype(np.float32).itemsize

    raw_file = open(raw_path, "ab" if append else "wb")
    event_file = open(event_path, "a" if append else "w", newline="")

    if (not append) or event_file.tell() == 0:
        event_file.write("t_ns,sample_idx,source,message\n")
        event_file.flush()

    return base_sample_idx


def configure_ppk2(port: str, mode: str, source_voltage_mv: int) -> PPK2_API:
    print("Using patched ppk2")
    ppk2 = PatchedPPK2(port)

    time.sleep(0.1)
    ppk2.ser.reset_input_buffer()
    ppk2.ser.reset_output_buffer()
    time.sleep(0.1)

    if not ppk2.get_modifiers():
        raise RuntimeError("Failed to read PPK2 calibration metadata")

    ppk2.set_source_voltage(source_voltage_mv)

    if mode == "source":
        ppk2.use_source_meter()
    else:
        ppk2.use_ampere_meter()

    return ppk2


def ppk_worker(ppk2: PPK2_API) -> None:
    ppk2.start_measuring()
    measurement_started_event.set()
    print("PPK2 measuring started")

    window_duration = 3.0
    window_start = time.perf_counter()
    window_samples = []

    try:
        while not stop_event.is_set():
            read_data = ppk2.get_data()
            if read_data:
                samples, flags = ppk2.get_samples(read_data)
                if samples:
                    arr = np.asarray(samples, dtype=np.float32)

                    # write raw values to binary
                    arr.tofile(raw_file)
                    raw_file.flush()

                    rec_state.mark_samples_written(len(arr))
                    window_samples.extend(samples)


            now = time.perf_counter()

            if now - window_start >= window_duration:
                if window_samples:
                    avg = sum(window_samples) / len(window_samples)
                    max_val = max(window_samples)

                    print(
                        f"[PPK][3s] avg={avg:.2f} uA | "
                        f"max={max_val:.2f} uA | "
                        f"samples={len(window_samples)}"
                    )
                else:
                    print("[PPK][3s] no samples")

                # reset vindu
                window_samples.clear()
                window_start = now

            time.sleep(0.01)  # litt raskere enn før for bedre sampling

    finally:
        ppk2.stop_measuring()
        ppk2.ser.reset_input_buffer()
        ppk2.ser.reset_output_buffer()
        print("Closed ppk2 thread ok")


def uart_worker(port: str, baudrate: int) -> None:
    ser = serial.Serial(port, baudrate=baudrate, timeout=0.2)

    # Leave DTR/RTS untouched for now since plain reading worked.
    time.sleep(0.1)
    ser.reset_input_buffer()

    print(f"UART opened on {ser.port}")

    try:
        idle_count = 0

        while not stop_event.is_set():
            line = ser.readline()
            if line:
                idle_count = 0
                ts = time.time()
                msg = line.decode(errors="replace").rstrip()
                uart_queue.put((ts, msg))
                t_ns, sample_idx = rec_state.snapshot()

                event_file.write(
                    f'{t_ns},{sample_idx},uart,"{msg}"\n'
                )
                event_file.flush()
            else:
                idle_count += 1
                if idle_count % 50 == 0:
                    print("UART still alive, no data")

    finally:
        ser.close()
        print("Closed uart port")


def main() -> None:
    args = parse_args()
    base_sample_idx = open_output_files(args.data_dir, args.append)
    rec_state.reset(base_sample_idx)
    stop_event.clear()
    measurement_started_event.clear()

    ppk2 = configure_ppk2(args.ppk2_port, args.mode, args.source_voltage_mv)
    print(f"PPK2 configured in {args.mode} mode, starting baseline recording...")
    print("Modifiers:", ppk2.modifiers)
    print("adc_mult:", ppk2.adc_mult)

    if not args.disable_output_switch and not args.no_power_cycle:
        ppk2.toggle_DUT_power("OFF")
        print("PPK2 DUT output switch forced off before recording")
        time.sleep(0.5)

    ppk_thread = threading.Thread(target=ppk_worker, args=(ppk2,))
    uart_thread = threading.Thread(target=uart_worker, args=(args.uart_port, args.uart_baudrate))

    try:
        # start uart logger
        uart_thread.start()
        
        # start ppk logger
        ppk_thread.start()
        if not measurement_started_event.wait(timeout=3.0):
            raise RuntimeError("PPK2 measurement did not start")
        time.sleep(1.0)

        t_ns, sample_idx = rec_state.snapshot()
        if args.disable_output_switch:
            event_file.write(f"{t_ns},{sample_idx},system,DUT_OUTPUT_SWITCH_DISABLED\n")
        elif args.mode == "source":
            ppk2.toggle_DUT_power("ON")
            event_file.write(f"{t_ns},{sample_idx},system,DUT_POWER_ON\n")
        else:
            ppk2.toggle_DUT_power("ON")
            event_file.write(f"{t_ns},{sample_idx},system,AMPERE_MODE_PASS_THROUGH_ON\n")
        event_file.flush()

        while True:
            while not uart_queue.empty():
                ts, msg = uart_queue.get()
                print(f"[UART] {ts:.3f} {msg}")

            time.sleep(0.05)

    except KeyboardInterrupt:
        print("\nStopping...")

    finally:
        stop_event.set()
        if ppk_thread.is_alive():
            ppk_thread.join()
        if uart_thread.is_alive():
            uart_thread.join()
        if raw_file is not None:
            raw_file.close()
        if event_file is not None:
            event_file.close()
        print("Clean shutdown")


if __name__ == "__main__":
    main()
