# Tests

This directory contains test infrastructure for the NTN prototype project.

## Test Organization

### `/on_target` - Device Testing
Tests that run on or communicate with the actual Thingy:91 X hardware:
- **NTN connectivity tests** (`ntn_test.py`) - Automated satellite connection testing
- **Modem diagnostics** (`ntn_modem_commands.py`) - AT command interface
- **Power profiling** (`ppk/`) - Power consumption analysis using PPK2
- **Testing documentation** (`NTN_TESTING.md`) - Comprehensive test procedures

### `/modules` - Unit Tests
Unit tests for individual code modules (future expansion).

## Quick Start

### Setup Test Environment

```bash
cd tests/on_target

# Create conda environment
conda env create -f environment.yml

# Activate environment
conda activate ies2608
```

### Run NTN Tests

```bash
# Test NTN connectivity
python ntn_test.py --timeout 300

# Run modem diagnostics
python ntn_modem_commands.py --mode diagnostic

# Interactive AT commands
python ntn_modem_commands.py --mode interactive
```

## Documentation

- **[NTN Testing Guide](on_target/NTN_TESTING.md)** - Complete testing procedures
- **[Deployment Guide](../docs/NTN_DEPLOYMENT_GUIDE.md)** - Device deployment instructions

## Test Requirements

- Python 3.10+
- pyserial, pytest, pandas, plotly (installed via conda)
- Thingy:91 X device connected via USB
- Serial port access (`/dev/ttyACM0` or similar)

## Contributing

When adding tests:
1. Follow existing test patterns
2. Document test procedures
3. Update this README
4. Ensure tests can run in CI/CD environment









