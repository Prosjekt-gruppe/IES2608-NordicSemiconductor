# Tests

Run module unit tests with Twister from a configured nRF Connect SDK shell:

```sh
west twister -T tests/modules -p unit_testing
```

The module suites cover deterministic firmware logic that can be tested without
modem, GNSS, cloud, or sensor hardware:

- RSRP: CESQ parsing, LTE fallback signal decisions, unavailable sample
  handling, and LTE probe averaging.
- GNSS: satellite counting, A-GNSS processed-data detection, and assisted-start
  timeout extension decisions.
- LTE-M: network registration event mapping, probe-ready transitions, and LTE
  mode names.
- NTN: firmware token detection, AT response cleanup, and GNSS-fix prechecks.
- Modem: TN/NTN system-mode command selection and UDP payload boundaries.
- Sensors: accelerometer motion math and battery level/charger-state
  classification.

Run the same suite with project-focused coverage:

```sh
west twister --coverage -T tests/modules -p unit_testing --coverage-basedir .
```

`--coverage-basedir .` is important when running from the repository root. Without
it, Twister defaults the coverage root to Zephyr and the report mostly measures
Zephyr's ztest framework files instead of this project's firmware code.









