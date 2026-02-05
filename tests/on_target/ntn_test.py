#!/usr/bin/env python3
"""
NTN Connectivity Test Script for Thingy:91 X

Tests NTN (Non-Terrestrial Network) satellite connectivity and monitors
the device's connection status during remote testing.

Usage:
    python ntn_test.py [--port /dev/ttyACM0] [--timeout 300]
"""

import argparse
import time
import serial
import sys
import re
from typing import Optional


class NTNTester:
    """NTN connectivity tester for Thingy:91 X"""
    
    def __init__(self, port: str = "/dev/ttyACM0", baudrate: int = 115200):
        """Initialize NTN tester with serial connection"""
        self.port = port
        self.baudrate = baudrate
        self.ser: Optional[serial.Serial] = None
        self.connected = False
        
    def connect(self) -> bool:
        """Establish serial connection to device"""
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=1.0
            )
            print(f"✓ Connected to {self.port} at {self.baudrate} baud")
            return True
        except serial.SerialException as e:
            print(f"✗ Failed to connect to {self.port}: {e}")
            return False
    
    def disconnect(self):
        """Close serial connection"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("✓ Disconnected from device")
    
    def read_line(self, timeout: float = 1.0) -> Optional[str]:
        """Read a line from serial output"""
        if not self.ser:
            return None
        
        start_time = time.time()
        line_buffer = b""
        
        while (time.time() - start_time) < timeout:
            if self.ser.in_waiting:
                byte = self.ser.read(1)
                if byte == b'\n':
                    try:
                        line = line_buffer.decode('utf-8', errors='ignore').strip()
                        return line
                    except Exception:
                        return None
                else:
                    line_buffer += byte
        
        return None
    
    def wait_for_ntn_connection(self, timeout: int = 300) -> bool:
        """
        Wait for NTN network connection
        
        Args:
            timeout: Maximum time to wait in seconds (default: 300s = 5min)
        
        Returns:
            True if connected, False if timeout
        """
        print(f"Waiting for NTN connection (timeout: {timeout}s)...")
        print("Note: Satellite connection can take several minutes")
        print("-" * 60)
        
        start_time = time.time()
        
        while (time.time() - start_time) < timeout:
            line = self.read_line(timeout=5.0)
            
            if line:
                # Print all output for debugging
                print(line)
                
                # Check for connection indicators
                if "NTN network connected" in line:
                    self.connected = True
                    print("-" * 60)
                    print("✓ NTN CONNECTED!")
                    return True
                
                if "NTN Status: CONNECTED" in line:
                    self.connected = True
                    print("-" * 60)
                    print("✓ NTN CONNECTION VERIFIED!")
                    return True
                
                # Check for errors
                if "Failed to" in line or "error" in line.lower():
                    print(f"⚠ Warning: {line}")
        
        elapsed = time.time() - start_time
        print("-" * 60)
        print(f"✗ Timeout after {elapsed:.1f}s - NTN connection not established")
        return False
    
    def monitor_connection(self, duration: int = 60):
        """
        Monitor NTN connection status for a specified duration
        
        Args:
            duration: Monitoring duration in seconds
        """
        print(f"\nMonitoring NTN connection for {duration}s...")
        print("-" * 60)
        
        start_time = time.time()
        status_count = 0
        
        while (time.time() - start_time) < duration:
            line = self.read_line(timeout=2.0)
            
            if line:
                print(line)
                
                if "NTN Status:" in line:
                    status_count += 1
                    if "CONNECTED" in line:
                        print(f"  → Connection stable ({status_count} status checks)")
        
        print("-" * 60)
        print(f"✓ Monitoring complete - received {status_count} status updates")
    
    def run_test(self, connection_timeout: int = 300, monitor_duration: int = 60):
        """
        Run complete NTN test sequence
        
        Args:
            connection_timeout: Max time to wait for initial connection
            monitor_duration: Time to monitor stable connection
        """
        print("=" * 60)
        print("NTN CONNECTIVITY TEST")
        print("=" * 60)
        print()
        
        # Connect to device
        if not self.connect():
            return False
        
        try:
            # Wait for NTN connection
            if self.wait_for_ntn_connection(timeout=connection_timeout):
                # Monitor stable connection
                self.monitor_connection(duration=monitor_duration)
                
                print("\n" + "=" * 60)
                print("TEST RESULT: PASSED ✓")
                print("NTN prototype is ready for remote testing")
                print("=" * 60)
                return True
            else:
                print("\n" + "=" * 60)
                print("TEST RESULT: FAILED ✗")
                print("Could not establish NTN connection")
                print("=" * 60)
                return False
        
        finally:
            self.disconnect()


def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(
        description="Test NTN connectivity on Thingy:91 X"
    )
    parser.add_argument(
        "--port",
        default="/dev/ttyACM0",
        help="Serial port (default: /dev/ttyACM0)"
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="Connection timeout in seconds (default: 300)"
    )
    parser.add_argument(
        "--monitor",
        type=int,
        default=60,
        help="Monitoring duration in seconds (default: 60)"
    )
    
    args = parser.parse_args()
    
    # Create tester and run
    tester = NTNTester(port=args.port)
    success = tester.run_test(
        connection_timeout=args.timeout,
        monitor_duration=args.monitor
    )
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
