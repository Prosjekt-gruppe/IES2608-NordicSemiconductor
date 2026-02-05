# NTN Prototype Deployment Guide

## Quick Start

This guide provides step-by-step instructions for deploying the NTN prototype to your Thingy:91 X device for remote testing.

## Prerequisites Checklist

Before deployment, ensure you have:

- [ ] **Thingy:91 X device** with updated modem firmware
- [ ] **nRF Connect SDK** (v2.5.0+) installed
- [ ] **West build tool** configured
- [ ] **SIM card** with satellite/NTN provisioning
- [ ] **USB cable** for initial flashing
- [ ] **Serial terminal** software (screen, minicom, or PuTTY)

## Step 1: Prepare Development Environment

### Install nRF Connect SDK

If not already installed:

```bash
# Follow Nordic's official installation guide:
# https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/installation.html

# Verify installation
west --version
```

### Clone Repository

```bash
cd ~/projects
git clone https://github.com/Prosjekt-gruppe/IES2608-NordicSemiconductor.git
cd IES2608-NordicSemiconductor
```

## Step 2: Build the NTN Prototype

### Option A: Using West (Recommended)

```bash
# Navigate to nRF Connect SDK
cd <ncs-installation-path>

# Build for Thingy:91 X
west build -b thingy91x/nrf9160/ns ~/projects/IES2608-NordicSemiconductor/app

# Build output will be in build/zephyr/
```

### Option B: From App Directory

```bash
cd app
west build -b thingy91x/nrf9160/ns .
```

### Build Verification

Look for successful build completion:
```
Memory region         Used Size  Region Size  %age Used
           FLASH:       XXXXX B       1 MB     XX.XX%
             RAM:       XXXXX B      256 KB    XX.XX%
        IDT_LIST:          0 GB         2 KB      0.00%
[XXX/XXX] Linking C executable zephyr/zephyr.elf
```

## Step 3: Flash the Device

### Connect Device

1. Connect Thingy:91 X via USB
2. Device should appear as `/dev/ttyACM0` (Linux) or `COMX` (Windows)

### Flash Firmware

```bash
# Flash using West
west flash

# Or manually specify port
west flash --dev-id /dev/ttyACM0
```

### Flash Verification

Device will reboot automatically after flashing. You should see LED activity.

## Step 4: Initial Testing

### Connect Serial Console

**Linux/macOS**:
```bash
# Using screen
screen /dev/ttyACM0 115200

# Using minicom
minicom -D /dev/ttyACM0 -b 115200

# Using pyserial-miniterm
python -m serial.tools.miniterm /dev/ttyACM0 115200
```

**Windows**:
```powershell
# Using PuTTY or TeraTerm
# Configure: 115200 baud, 8-N-1, no flow control
```

### Verify Boot Sequence

You should see:
```
*** Booting Zephyr OS build vX.X.X ***
[00:00:00.001,000] <inf> ntn_prototype: === NTN Prototype Starting ===
[00:00:00.001,000] <inf> ntn_prototype: Device: Thingy:91 X
[00:00:00.001,000] <inf> ntn_prototype: Mode: Non-Terrestrial Network (Satellite)
```

## Step 5: Test NTN Connectivity (Lab Test)

Before remote deployment, test in lab:

### Run Automated Test

```bash
# Activate test environment
cd tests/on_target
conda activate ies2608

# Run connectivity test
python ntn_test.py --timeout 600
```

### Manual Verification

Monitor serial output for:
- ✓ Modem initialization
- ✓ NTN mode configuration
- ✓ Connection attempt
- ⚠ May take 5-15 minutes to connect indoors

**Note**: Indoor testing may fail due to lack of satellite visibility. This is expected.

## Step 6: Remote Deployment

### Pre-Deployment Configuration

1. **SIM Card**:
   - Install SIM with satellite provisioning
   - Verify SIM is activated for NTN

2. **Power**:
   - Fully charge battery
   - Verify power LED is on

3. **Testing Location**:
   - Choose location with clear sky view
   - Avoid indoor or obstructed areas
   - Higher elevation is better

### Deployment Procedure

1. **Initial Power-On**:
   ```bash
   # Before deployment, verify boot via serial
   screen /dev/ttyACM0 115200
   # Observe startup logs
   # Disconnect after verification
   ```

2. **Deploy Device**:
   - Place device in remote location
   - Ensure clear view of sky (satellites need line of sight)
   - Secure device if necessary

3. **Connection Timeline**:
   - **0-2 min**: Device boots and initializes modem
   - **2-5 min**: Modem searches for satellite network
   - **5-15 min**: Initial NTN connection (typical)
   - **15+ min**: Connection stabilizes

### Remote Monitoring

If possible, leave device connected to serial logger:

```bash
# Log to file for later analysis
screen -L -Logfile ntn_test.log /dev/ttyACM0 115200
```

Or use automated test:

```bash
# Run extended test with logging
python ntn_test.py --timeout 900 --monitor 300 2>&1 | tee ntn_deployment.log
```

## Step 7: Verify Remote Operation

### Connection Indicators

**Success Indicators**:
- Serial logs show "NTN network connected!"
- Status updates show "NTN Status: CONNECTED"
- LED patterns indicate active connection (device-specific)

**Troubleshooting**:
- **No connection after 15 min**: Check sky visibility
- **Connection drops**: Device may have moved to obstructed area
- **High power drain**: Verify PSM is enabled

### Test Data Transmission

Once connected, device is ready for:
- Position tracking over satellite
- Remote data transmission
- Power consumption profiling

## Step 8: Power Profiling (Optional)

For battery life testing:

```bash
# Connect PPK2 (Power Profiler Kit 2)
cd tests/on_target/ppk
python automated_ppk.py
```

Monitor:
- Connection phase current
- Idle current with PSM
- Transmission current spikes

## Troubleshooting Guide

### Build Issues

**Error: "west not found"**
```bash
# Install west
pip3 install west
```

**Error: "Board not found"**
```bash
# Update to latest nRF Connect SDK
west update
```

### Flash Issues

**Error: "No device found"**
- Check USB connection
- Verify device is in bootloader mode
- Try different USB port/cable

**Error: "Permission denied" (Linux)**
```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER
# Log out and back in
```

### Connection Issues

**Indoor testing fails**:
- Expected - satellites require outdoor line of sight
- Move to outdoor location for testing

**Long connection time**:
- Normal for NTN - can take 10-15 minutes
- Be patient, especially for first connection

**Connection drops**:
- Check for obstructions (buildings, trees)
- Verify device hasn't moved
- Check SIM card status

### Debug Commands

Use modem commands for debugging:

```bash
# Run diagnostic
python tests/on_target/ntn_modem_commands.py --mode diagnostic

# Interactive mode for manual commands
python tests/on_target/ntn_modem_commands.py --mode interactive
```

## Appendix A: Hardware Setup

### Thingy:91 X Pinout

For custom testing setup:
- **USB**: Power and serial console
- **External antenna**: For better satellite reception
- **Battery**: For remote operation

### Recommended Accessories

- **External LTE/GNSS antenna**: Better signal in challenging locations
- **Battery pack**: Extended remote operation
- **Weatherproof enclosure**: Outdoor deployment
- **USB logger**: Continuous serial logging

## Appendix B: Network Configuration

### Supported Networks

NTN prototype supports:
- **LTE-M NTN**: Satellite LTE-M (where available)
- **NB-IoT NTN**: Satellite NB-IoT (region dependent)

### Carrier Requirements

Check with your carrier for:
- NTN/satellite coverage in your area
- SIM provisioning requirements
- Data plan compatibility
- Roaming agreements

## Appendix C: Safety and Compliance

### Regulatory Compliance

- Verify satellite communication is legal in deployment region
- Some countries restrict satellite device usage
- Obtain necessary permits if required

### Safety Guidelines

- Do not deploy in hazardous locations
- Ensure proper RF exposure distances
- Follow battery safety guidelines
- Secure device to prevent theft/loss

## Next Steps

After successful deployment:

1. **Monitor Operation**: Check logs regularly
2. **Collect Data**: Gather connection statistics
3. **Optimize**: Tune PSM/eDRX for your use case
4. **Expand**: Add position tracking features
5. **Integrate**: Connect to cloud services

## Support

For issues or questions:
- **Documentation**: `/app/README.md`, `/tests/on_target/NTN_TESTING.md`
- **Nordic Support**: [developer.nordicsemi.com](https://developer.nordicsemi.com/)
- **Project Issues**: GitHub repository issues

## Revision History

- **v1.0** (2026-02-05): Initial NTN prototype deployment guide
