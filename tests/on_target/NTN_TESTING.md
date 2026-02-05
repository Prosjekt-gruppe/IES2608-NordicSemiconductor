# NTN Testing Documentation

## Overview

This document describes the testing procedures for the NTN (Non-Terrestrial Network) prototype on the Thingy:91 X device.

## Test Environment Setup

### Prerequisites

1. **Hardware**:
   - Thingy:91 X device with NTN-capable modem firmware
   - USB cable for connection
   - SIM card with satellite network provisioning
   - Clear sky view for satellite visibility

2. **Software**:
   - Python test environment (conda)
   - Serial terminal access
   - nRF Connect SDK (for building/flashing)

### Environment Setup

```bash
# Navigate to test directory
cd tests/on_target

# Activate conda environment
conda activate ies2608

# Verify Python dependencies
python -c "import serial; import pytest; print('✓ Dependencies OK')"
```

## Test Scripts

### 1. NTN Connectivity Test (`ntn_test.py`)

Automated test for NTN satellite connectivity.

**Usage**:
```bash
# Basic test (default port, 5min timeout)
python ntn_test.py

# Custom port and timeout
python ntn_test.py --port /dev/ttyACM1 --timeout 600

# Extended monitoring
python ntn_test.py --timeout 300 --monitor 120
```

**What it tests**:
- Serial communication with device
- NTN connection establishment
- Connection stability monitoring
- Status reporting

**Expected results**:
- ✓ Connected to serial port
- ✓ Device boots and initializes
- ✓ NTN network connection established
- ✓ Stable connection for monitoring period

**Troubleshooting**:
- If timeout occurs, check satellite visibility
- Verify SIM card has satellite provisioning
- Check modem firmware supports NTN

### 2. NTN Modem Commands (`ntn_modem_commands.py`)

AT command interface for modem diagnostics and configuration.

**Diagnostic Mode**:
```bash
python ntn_modem_commands.py --mode diagnostic
```

Shows:
- Modem UUID and version
- System mode configuration
- Network registration status
- Signal quality
- PSM/eDRX settings

**Configuration Mode**:
```bash
python ntn_modem_commands.py --mode configure
```

Configures:
- System mode for NTN/LTE-M
- Power Saving Mode (PSM)
- Extended DRX (eDRX)

**Interactive Mode**:
```bash
python ntn_modem_commands.py --mode interactive
```

Allows manual AT command entry for advanced testing.

**Common AT Commands**:
```
AT%XSYSTEMMODE?      # Query system mode
AT+CEREG?            # Check network registration
AT+CSQ               # Signal quality
AT%XMODEMUUID        # Modem UUID
AT+CFUN?             # Functional mode
```

## Manual Testing Procedures

### Test 1: Basic NTN Connection

1. Flash the NTN prototype to device:
   ```bash
   cd app
   west build -b thingy91x/nrf9160/ns .
   west flash
   ```

2. Connect to serial console:
   ```bash
   screen /dev/ttyACM0 115200
   ```

3. Observe boot sequence:
   - Device should print "NTN Prototype Starting"
   - Modem initialization messages
   - "Connecting to NTN network..."

4. Wait for connection (2-10 minutes typical):
   - Look for "NTN network connected!" message
   - Status updates show "CONNECTED"

5. Verify stability:
   - Connection should remain stable
   - Periodic status updates continue
   - No error messages

**Pass Criteria**: Device connects to NTN network and maintains stable connection

### Test 2: Power Consumption Verification

1. Connect PPK2 (Power Profiler Kit 2)

2. Run power profiling:
   ```bash
   python tests/on_target/ppk/automated_ppk.py
   ```

3. Compare power consumption:
   - NTN connection phase: Higher current
   - Connected idle state: PSM should reduce power
   - Periodic updates: Brief current spikes

**Pass Criteria**: Power consumption matches expected NTN profile

### Test 3: Location Services Integration

1. Ensure outdoor location with clear sky view

2. Monitor GNSS acquisition in logs:
   - GNSS initialization messages
   - Satellite acquisition progress
   - Position fix obtained

3. Verify position reporting over NTN

**Pass Criteria**: Device acquires GPS fix and can report position

## Remote Testing Procedures

For remote deployment scenarios:

### Pre-Deployment Checklist

- [ ] Device flashed with NTN prototype
- [ ] SIM card installed and activated
- [ ] Battery fully charged
- [ ] Test connectivity in lab before deployment
- [ ] Log collection configured

### Deployment Steps

1. **Initial Setup**:
   - Power on device
   - Verify initial boot via USB before deployment
   - Confirm device enters connection mode

2. **Remote Monitoring**:
   - Device will attempt NTN connection automatically
   - Connection may take 5-15 minutes initially
   - Monitor via serial logs if possible

3. **Validation**:
   - Check for "NTN network connected" in logs
   - Verify periodic status updates
   - Confirm data transmission if applicable

### Remote Troubleshooting

**Issue**: Device doesn't connect
- **Check**: Satellite visibility (must have clear sky view)
- **Check**: SIM activation status
- **Action**: Wait up to 15 minutes for initial connection

**Issue**: Connection drops
- **Check**: Device moved to location with obstructed view
- **Action**: Relocate to better position

**Issue**: High power consumption
- **Check**: PSM/eDRX settings in logs
- **Action**: Verify modem configuration

## Test Results Documentation

Document test results using this template:

```
Test Date: YYYY-MM-DD
Device ID: [Thingy91X-XXXXX]
Location: [Indoor/Outdoor/Remote]
Firmware: [Version]

Test: NTN Connectivity
Result: PASS/FAIL
Connection Time: XX minutes
Notes: [Any observations]

Test: Power Consumption  
Result: PASS/FAIL
Average Current: XX mA
Notes: [Any observations]

Test: Location Services
Result: PASS/FAIL  
GNSS Fix Time: XX seconds
Notes: [Any observations]
```

## Continuous Integration

For automated testing in CI/CD:

```bash
# Run in CI environment
pytest tests/on_target/ntn_test.py --junit-xml=test-results.xml
```

## Safety and Regulations

⚠️ **Important Notes**:
- Comply with local regulations for satellite communication
- Some regions may restrict satellite device usage
- Verify SIM card authorization for satellite networks
- Follow proper RF exposure guidelines

## Support and Resources

- **Nordic Documentation**: [developer.nordicsemi.com](https://developer.nordicsemi.com/)
- **Thingy:91 X**: Hardware reference
- **nRF Connect SDK**: SDK documentation
- **Project README**: `/app/README.md`

## Appendix: Expected Log Output

### Successful Connection
```
[00:00:00.001,000] <inf> ntn_prototype: === NTN Prototype Starting ===
[00:00:00.001,000] <inf> ntn_prototype: Device: Thingy:91 X
[00:00:00.001,000] <inf> ntn_prototype: Mode: Non-Terrestrial Network (Satellite)
[00:00:00.010,000] <inf> ntn_prototype: Initializing modem for NTN mode...
[00:00:01.000,000] <inf> ntn_prototype: Modem initialized for NTN mode
[00:00:01.010,000] <inf> ntn_prototype: Connecting to NTN network...
[00:02:30.000,000] <inf> ntn_prototype: Network registration status: 1
[00:02:30.001,000] <inf> ntn_prototype: NTN network connected!
[00:02:40.002,000] <inf> ntn_prototype: NTN Status: CONNECTED
```

### Connection Issues
```
[00:00:00.010,000] <inf> ntn_prototype: Initializing modem for NTN mode...
[00:00:01.000,000] <err> ntn_prototype: Failed to initialize modem library, error: -X
```

Check modem firmware and configuration if errors occur.
