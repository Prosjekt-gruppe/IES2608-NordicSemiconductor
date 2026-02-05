# Adapted from Nordic Semiconductor / Asset-Tracker-Template
# Source: https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/tests/on_target/tests/test_ppk/test_power.py
# License: LicenseRef-Nordic-5-Clause (see upstream LICENSE)
# Changes: refactored for our test setup


import time as t
from ppk2_api.ppk2_api import PPK2_API


ppk2test = PPK2_API("/dev/ttyACM0")
ppk2test.get_modifiers()
