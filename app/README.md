# NTN Prototype Application

## Overview

This is a Non-Terrestrial Network (NTN) prototype application for the Thingy:91 X device. It enables satellite network connectivity for remote position tracking and testing over satellite networks.

## Features

- **NTN/Satellite Connectivity**: Connects to satellite networks using the nRF9160 modem's NTN capabilities
- **Power Management**: Implements PSM (Power Saving Mode) and eDRX for efficient satellite operations
- **GNSS Integration**: Supports location services for position tracking
- **Remote Testing Ready**: Designed for remote deployment and testing scenarios

## Architecture

The application consists of:
- `src/main.c` - Main application logic with NTN initialization and connection management
- `prj.conf` - Zephyr project configuration with NTN-specific settings
- `CMakeLists.txt` - Build system configuration
- `thingy91x_nrf9160_ns.overlay` - Board-specific device tree overlay

## Building

### Prerequisites

1. **nRF Connect SDK** (v2.5.0 or later recommended)
2. **Zephyr toolchain** properly installed
3. **West** build tool
4. **Thingy:91 X** device with updated modem firmware supporting NTN

### Build Commands

```bash
# Navigate to the nRF Connect SDK installation
cd <ncs-installation-path>

# Build the application for Thingy:91 X
west build -b thingy91x/nrf9160/ns /path/to/IES2608-NordicSemiconductor/app

# Flash to device (when connected via USB)
west flash

# View serial output
west debug --serial
```

### Alternative Build (from app directory)

```bash
cd app
west build -b thingy91x/nrf9160/ns .
west flash
```

## Configuration

### Key Configuration Options (prj.conf)

- `CONFIG_LTE_NTN=y` - Enables NTN/satellite mode
- `CONFIG_LTE_PSM_REQ=y` - Power Saving Mode for battery efficiency
- `CONFIG_LTE_EDRX_REQ=y` - Extended DRX for improved power consumption
- `CONFIG_NRF_MODEM_LIB_GNSS=y` - GNSS support for location tracking

### Modem Requirements

The device must have modem firmware that supports NTN. Check Nordic Semiconductor's documentation for:
- Minimum modem firmware version
- NTN feature availability in your region
- SIM card with satellite network support

## Testing

### Remote Testing Setup

1. **Flash the application** to your Thingy:91 X device
2. **Power on** the device in a location with satellite visibility
3. **Monitor via UART** (115200 baud):
   ```bash
   # Linux
   screen /dev/ttyACM0 115200
   
   # Or using minicom
   minicom -D /dev/ttyACM0 -b 115200
   ```

4. **Check connection status** in the logs:
   - Look for "NTN network connected!" message
   - Monitor "NTN Status: CONNECTED" periodic updates

### Expected Output

```
*** Booting Zephyr OS build v3.x.x ***
[00:00:00.001,000] <inf> ntn_prototype: === NTN Prototype Starting ===
[00:00:00.001,000] <inf> ntn_prototype: Device: Thingy:91 X
[00:00:00.001,000] <inf> ntn_prototype: Mode: Non-Terrestrial Network (Satellite)
[00:00:00.010,000] <inf> ntn_prototype: Initializing modem for NTN mode...
[00:00:00.500,000] <inf> ntn_prototype: Configuring modem for NTN...
[00:00:01.000,000] <inf> ntn_prototype: Modem initialized for NTN mode
[00:00:01.010,000] <inf> ntn_prototype: Connecting to NTN network...
[00:00:01.020,000] <inf> ntn_prototype: NTN connection initiated, waiting for network...
[00:00:01.030,000] <inf> ntn_prototype: Entering main loop - monitoring NTN connection
...
[00:00:30.000,000] <inf> ntn_prototype: Network registration status: 1
[00:00:30.001,000] <inf> ntn_prototype: NTN network connected!
[00:00:30.002,000] <inf> ntn_prototype: NTN Status: CONNECTED
```

### Test Scripts

Python test scripts for automated NTN testing are available in `/tests/on_target/`:

```bash
# Activate test environment
conda activate ies2608

# Run NTN connectivity tests
python tests/on_target/ntn_test.py
```

## Troubleshooting

### Connection Issues

1. **No satellite visibility**: Ensure clear sky view, NTN requires direct line of sight to satellites
2. **Modem firmware**: Verify NTN support in modem firmware version
3. **SIM card**: Confirm SIM has satellite network provisioning
4. **Region support**: Check if NTN is available in your geographic region

### Debug Commands

Use AT commands via serial console for debugging:

```
AT%XSYSTEMMODE?      # Check current system mode
AT+CFUN?             # Check modem functional mode
AT+CEREG?            # Check network registration
AT%XMODEMUUID        # Get modem UUID
```

## Next Steps

This prototype provides the foundation for:
- **Position tracking** over satellite networks
- **Dynamic network switching** between cellular and satellite
- **Power consumption profiling** using PPK2
- **Remote deployment** scenarios

## References

- [Nordic nRF9160 NTN Documentation](https://developer.nordicsemi.com/)
- [Thingy:91 X Product Page](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)
- [nRF Connect Asset Tracker Template](https://github.com/nrfconnect/Asset-Tracker-Template)

## License

This project uses the Nordic Semiconductor 5-Clause License (LicenseRef-Nordic-5-Clause)