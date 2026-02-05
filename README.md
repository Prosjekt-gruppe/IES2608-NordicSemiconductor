# IES2608 Nordic Semiconductor

Private repo for the development of dynamic switching between cellular and satellite networks for position tracking using the Thingy:91 X.

## Project Overview

This project implements a **Non-Terrestrial Network (NTN)** prototype for the Nordic Semiconductor Thingy:91 X device, enabling satellite-based connectivity for remote position tracking and testing.

### Key Features

- 🛰️ **NTN/Satellite Connectivity**: Direct satellite network support using nRF9160 modem
- 🔋 **Power Optimized**: PSM and eDRX for efficient satellite operations
- 📍 **Location Services**: GNSS integration for position tracking
- 🧪 **Remote Testing**: Comprehensive test infrastructure for deployment validation
- 🔧 **AT Command Interface**: Full modem control and diagnostics

## Repository Structure

```
├── app/                    # NTN prototype application (Zephyr/C)
│   ├── src/main.c         # Main application code
│   ├── prj.conf           # Configuration
│   ├── CMakeLists.txt     # Build system
│   └── README.md          # Build and deployment instructions
├── docs/                   # Documentation
│   └── NTN_DEPLOYMENT_GUIDE.md  # Complete deployment guide
├── tests/                  # Test infrastructure
│   ├── on_target/         # Device testing
│   │   ├── ntn_test.py              # Automated NTN connectivity tests
│   │   ├── ntn_modem_commands.py    # AT command interface
│   │   ├── NTN_TESTING.md           # Testing documentation
│   │   └── ppk/                     # Power profiling
│   └── modules/           # Unit tests
└── scripts/               # Automation scripts
```

## Quick Start

### 1. Build the Application

```bash
# Using nRF Connect SDK
cd <ncs-path>
west build -b thingy91x/nrf9160/ns /path/to/app
west flash
```

### 2. Run Remote Tests

```bash
# Activate test environment
conda activate ies2608

# Test NTN connectivity
python tests/on_target/ntn_test.py

# Or run modem diagnostics
python tests/on_target/ntn_modem_commands.py --mode diagnostic
```

### 3. Monitor Device

```bash
# Connect to serial console (115200 baud)
screen /dev/ttyACM0 115200

# Wait for "NTN network connected!" message
```

## Documentation

- **[Application README](app/README.md)** - Build instructions and architecture
- **[Deployment Guide](docs/NTN_DEPLOYMENT_GUIDE.md)** - Complete deployment procedure
- **[Testing Guide](tests/on_target/NTN_TESTING.md)** - Test procedures and scripts
- **[AT Commands Reference](docs/Useful%20Commands/commands.txt)** - Modem commands

## Prerequisites

### Hardware
- Thingy:91 X device with NTN-capable modem firmware
- SIM card with satellite network provisioning
- USB cable for flashing and debugging

### Software
- nRF Connect SDK (v2.5.0 or later)
- Python 3.10+ with test dependencies (see `tests/on_target/environment.yml`)
- Serial terminal (screen, minicom, or equivalent)

## Getting Started

For detailed instructions, see:
1. **[NTN Deployment Guide](docs/NTN_DEPLOYMENT_GUIDE.md)** - Step-by-step deployment
2. **[Application README](app/README.md)** - Building and configuration
3. **[Testing Documentation](tests/on_target/NTN_TESTING.md)** - Running tests

## Testing Infrastructure

### Automated Testing

```bash
# Setup test environment
cd tests/on_target
conda env create -f environment.yml
conda activate ies2608

# Run connectivity test
python ntn_test.py --timeout 300 --monitor 60

# Run modem diagnostics
python ntn_modem_commands.py --mode diagnostic
```

### Power Profiling

```bash
# Using Nordic PPK2
python tests/on_target/ppk/automated_ppk.py
```

## Development Status

- ✅ NTN modem integration
- ✅ Power management (PSM/eDRX)
- ✅ GNSS location services
- ✅ Remote testing infrastructure
- ✅ AT command interface
- 🚧 Dynamic cellular/satellite switching (future)
- 🚧 Cloud integration (future)

## Contributing

This is a private development repository for the IES2608 project.

## Acknowledgements

This project uses the nRF Connect Asset Tracker Template as a reference:

[https://github.com/nrfconnect/Asset-Tracker-Template/tree/main](https://github.com/nrfconnect/Asset-Tracker-Template/tree/main)

## License

See [LICENSE](LICENSE) file for details.

