# IES2608 Asset Tracker

Firmware project for exploring dynamic switching between LTE-M and NTN on Nordic Semiconductor hardware, with the Thingy:91 X as the current target platform.

The project is built around an asset-tracker use case: stay on LTE-M when coverage is good, detect when the asset is in a higher-risk situation, and fall back toward NTN when terrestrial coverage becomes unreliable.

## Repository status

`app/` is the active application and the main place to look for current code.

Most of the other top-level folders are earlier experiments, prototypes, or reference material kept around for comparison while the project is still taking shape.

## Current application scope

The firmware in `app/` currently includes:

- an event-driven application state machine
- LTE-M bring-up and registration handling
- LTE RSRP monitoring
- NTN connection flow experiments
- early GNSS integration hooks
- basic Thingy:91 X sensor demos

The sensor work is still prototype-level. Right now the goal is to understand the hardware and data flow before integrating the sensors into the final tracking and network-selection logic.

## Sensor demos

Two simple sensor modules are currently wired into the app:

- Accelerometer demo: polls the Thingy:91 X BMI270 and prints to serial when movement is detected, including the movement delta and current XYZ acceleration in mg.
- Battery demo: reads the nPM1300 charger and prints battery voltage, charge state, USB presence, current, and temperature.

These are intentionally basic examples, not finished production modules.

## Building

### Prerequisites

- nRF Connect SDK / Zephyr development environment
- `west`
- a Nordic programmer/debug probe
- a serial log viewer

### Thingy:91 X

Build the app for the Thingy:91 X nRF9151 non-secure target:

```sh
west build -b thingy91x_nrf9151_ns -d app/build app --pristine
```

Then flash it:

```sh
west flash -d app/build
```

The Thingy:91 X board overlay enables the onboard high-performance accelerometer used by the movement demo.

### Alternate development target

There is also board-specific configuration for `nrf9151dk_nrf9151_ns` in `app/boards/` for bring-up and development on the nRF9151 DK, but the current sensor-focused direction is the Thingy:91 X.

## Running

After boot, open the serial log output. The app starts the state machine and prints log messages from the currently enabled services and sensor demos.

Example outputs include:

- LTE registration and signal-strength updates
- accelerometer movement detections
- periodic battery status readings

## Project layout

```text
app/        Active Zephyr/NCS application
docs/       Supporting notes and documentation
Firmware/   Earlier firmware/reference material
Prototype/  Prototypes and experiments
tests/      Test and scratch work
Zbus/       Messaging-related experiments/reference code
```

## Acknowledgements

This project uses the nRF Connect Asset Tracker Template as a reference:

[https://github.com/nrfconnect/Asset-Tracker-Template/tree/main](https://github.com/nrfconnect/Asset-Tracker-Template/tree/main)
