# Project architecture

- Zephyr + nRF Connect SDK
- Event-driven architecture using zbus + SMF
- app_sm.c is orchestration only
- Business logic belongs in services
- Prefer small incremental refactors
- Preserve transition_to_state() logging behavior
- Do not rewrite large sections unnecessarily
- Prefer moving duplicated logic into helpers
- Avoid changing AT command sequences unless required
- LTE-M is preferred TN profile
- NTN currently uses NB-IoT-based NTN flow
- HSM ancestor support enabled via CONFIG_SMF_ANCESTOR_SUPPORT=y