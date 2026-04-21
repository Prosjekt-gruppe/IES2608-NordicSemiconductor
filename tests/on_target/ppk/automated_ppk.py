# Adapted from Nordic Semiconductor / Asset-Tracker-Template
# Source: https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/tests/on_target/tests/test_ppk/test_power.py
# License: LicenseRef-Nordic-5-Clause (see upstream LICENSE)
# Changes: refactored for our test setup

import queue
import threading
import time

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

PPK2_PORT = "/dev/serial/by-id/usb-Nordic_Semiconductor_PPK2_CB6017A1DC4B-if01"
UART_PORT = "/dev/serial/by-id/usb-SEGGER_J-Link_001052041270-if00"
UART_BAUDRATE = 115200
SOURCE_VOLTAGE_MV = 3300


uart_queue = queue.Queue()
stop_event = threading.Event()


def configure_ppk2(port: str) -> PPK2_API:
    print("Using patched ppk2")
    ppk2 = PatchedPPK2(port)

    time.sleep(0.1)
    ppk2.ser.reset_input_buffer()
    ppk2.ser.reset_output_buffer()
    time.sleep(0.1)

    ppk2.get_modifiers()
    ppk2.use_source_meter()
    ppk2.set_source_voltage(SOURCE_VOLTAGE_MV)
    ppk2.toggle_DUT_power("ON")

    return ppk2


def ppk_worker(ppk2: PPK2_API) -> None:
    ppk2.start_measuring()
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


def uart_worker(port: str) -> None:
    ser = serial.Serial(port, baudrate=UART_BAUDRATE, timeout=0.2)

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
            else:
                idle_count += 1
                if idle_count % 50 == 0:
                    print("UART still alive, no data")

    finally:
        ser.close()
        print("Closed uart port")


def main() -> None:
    ppk2 = configure_ppk2(PPK2_PORT)
    print("PPK2 configured, waiting for DUT boot...")
    print("Modifiers:", ppk2.modifiers)
    print("adc_mult:", ppk2.adc_mult)
    time.sleep(2.0)

    ppk_thread = threading.Thread(target=ppk_worker, args=(ppk2,))
    uart_thread = threading.Thread(target=uart_worker, args=(UART_PORT,))

    ppk_thread.start()
    uart_thread.start()

    try:
        while True:
            while not uart_queue.empty():
                ts, msg = uart_queue.get()
                print(f"[UART] {ts:.3f} {msg}")

            time.sleep(0.05)

    except KeyboardInterrupt:
        print("\nStopping...")

    finally:
        stop_event.set()
        ppk_thread.join()
        uart_thread.join()
        print("Clean shutdown")


if __name__ == "__main__":
    main()