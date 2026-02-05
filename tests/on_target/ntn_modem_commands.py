#!/usr/bin/env python3
"""
NTN Modem Command Script for Thingy:91 X

Provides AT command interface for NTN modem configuration and testing.
Can be used for manual testing and debugging of satellite connectivity.

Usage:
    python ntn_modem_commands.py [--port /dev/ttyACM0]
"""

import argparse
import time
import serial
import sys


class NTNModemCommander:
    """AT command interface for NTN modem operations"""
    
    AT_COMMANDS = {
        "system_mode": "AT%XSYSTEMMODE?",
        "functional_mode": "AT+CFUN?",
        "network_reg": "AT+CEREG?",
        "signal_quality": "AT+CSQ",
        "modem_uuid": "AT%XMODEMUUID",
        "psm_status": "AT+CPSMS?",
        "edrx_status": "AT+CEDRXS?",
        "set_ntn_mode": "AT%XSYSTEMMODE=1,0,1,0",  # LTE-M with GPS
        "enable_psm": "AT+CPSMS=1",
        "enable_edrx": "AT+CEDRXS=1",
    }
    
    def __init__(self, port: str = "/dev/ttyACM0", baudrate: int = 115200):
        """Initialize modem commander"""
        self.port = port
        self.baudrate = baudrate
        self.ser = None
    
    def connect(self) -> bool:
        """Connect to modem"""
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=2.0
            )
            print(f"✓ Connected to modem on {self.port}")
            time.sleep(0.5)  # Allow modem to settle
            return True
        except serial.SerialException as e:
            print(f"✗ Failed to connect: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from modem"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("✓ Disconnected")
    
    def send_at_command(self, command: str, timeout: float = 2.0) -> str:
        """Send AT command and get response"""
        if not self.ser or not self.ser.is_open:
            return "Error: Not connected"
        
        # Clear buffers
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        
        # Send command
        cmd_bytes = (command + "\r\n").encode('ascii')
        self.ser.write(cmd_bytes)
        print(f"→ {command}")
        
        # Read response
        time.sleep(0.1)
        start_time = time.time()
        response_lines = []
        
        while (time.time() - start_time) < timeout:
            if self.ser.in_waiting:
                line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    response_lines.append(line)
                    print(f"← {line}")
                    
                    # Check for command completion
                    if line in ["OK", "ERROR"]:
                        break
        
        return "\n".join(response_lines)
    
    def run_diagnostic(self):
        """Run complete NTN diagnostic sequence"""
        print("=" * 60)
        print("NTN MODEM DIAGNOSTIC")
        print("=" * 60)
        print()
        
        if not self.connect():
            return False
        
        try:
            # Test basic connectivity
            print("\n--- Basic Modem Info ---")
            self.send_at_command("AT")
            self.send_at_command(self.AT_COMMANDS["modem_uuid"])
            
            print("\n--- System Configuration ---")
            self.send_at_command(self.AT_COMMANDS["system_mode"])
            self.send_at_command(self.AT_COMMANDS["functional_mode"])
            
            print("\n--- Network Status ---")
            self.send_at_command(self.AT_COMMANDS["network_reg"])
            self.send_at_command(self.AT_COMMANDS["signal_quality"])
            
            print("\n--- Power Management ---")
            self.send_at_command(self.AT_COMMANDS["psm_status"])
            self.send_at_command(self.AT_COMMANDS["edrx_status"])
            
            print("\n" + "=" * 60)
            print("DIAGNOSTIC COMPLETE")
            print("=" * 60)
            return True
            
        finally:
            self.disconnect()
    
    def configure_ntn(self):
        """Configure modem for NTN operation"""
        print("=" * 60)
        print("CONFIGURING MODEM FOR NTN")
        print("=" * 60)
        print()
        
        if not self.connect():
            return False
        
        try:
            print("Setting system mode for NTN...")
            self.send_at_command(self.AT_COMMANDS["set_ntn_mode"])
            
            print("\nEnabling Power Saving Mode...")
            self.send_at_command(self.AT_COMMANDS["enable_psm"])
            
            print("\nEnabling eDRX...")
            self.send_at_command(self.AT_COMMANDS["enable_edrx"])
            
            print("\n" + "=" * 60)
            print("NTN CONFIGURATION COMPLETE")
            print("Reboot device for changes to take effect")
            print("=" * 60)
            return True
            
        finally:
            self.disconnect()
    
    def interactive_mode(self):
        """Interactive AT command mode"""
        print("=" * 60)
        print("INTERACTIVE AT COMMAND MODE")
        print("Enter AT commands (or 'quit' to exit)")
        print("=" * 60)
        print()
        
        if not self.connect():
            return False
        
        try:
            while True:
                cmd = input("\nAT> ").strip()
                
                if cmd.lower() in ['quit', 'exit', 'q']:
                    break
                
                if not cmd:
                    continue
                
                if not cmd.startswith("AT"):
                    print("Commands must start with 'AT'")
                    continue
                
                self.send_at_command(cmd)
        
        except KeyboardInterrupt:
            print("\n\nExiting...")
        
        finally:
            self.disconnect()


def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(
        description="NTN modem AT command interface"
    )
    parser.add_argument(
        "--port",
        default="/dev/ttyACM0",
        help="Serial port (default: /dev/ttyACM0)"
    )
    parser.add_argument(
        "--mode",
        choices=["diagnostic", "configure", "interactive"],
        default="diagnostic",
        help="Operation mode (default: diagnostic)"
    )
    
    args = parser.parse_args()
    
    commander = NTNModemCommander(port=args.port)
    
    if args.mode == "diagnostic":
        commander.run_diagnostic()
    elif args.mode == "configure":
        commander.configure_ntn()
    elif args.mode == "interactive":
        commander.interactive_mode()
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
