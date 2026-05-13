# Tests

Run module unit tests with Twister from a configured nRF Connect SDK shell:

```sh
west twister -T tests/modules/rsrp_service -p unit_testing
```

The RSRP suite covers CESQ parsing, LTE fallback signal decisions, unavailable
sample handling, and LTE probe averaging.









