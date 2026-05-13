# Tests

Run module unit tests with Twister from a configured nRF Connect SDK shell:

```sh
west twister -T tests/modules/rsrp_service -p unit_testing
```

The RSRP suite covers CESQ parsing, LTE fallback signal decisions, unavailable
sample handling, and LTE probe averaging.

Run the same suite with project-focused coverage:

```sh
west twister --coverage -T tests/modules/rsrp_service -p unit_testing --coverage-basedir .
```

`--coverage-basedir .` is important when running from the repository root. Without
it, Twister defaults the coverage root to Zephyr and the report mostly measures
Zephyr's ztest framework files instead of this project's firmware code.









