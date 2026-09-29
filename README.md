# OpenCell firmware

Firmware for the Meshnology W12 board (ESP32-S3 + Semtech LR2021) in both of its OpenCell roles, plus the C libraries, tests and bench tools that go with it. Part of [OpenCell](https://github.com/opencell-dev/opencell), a cellular-style voice network over LoRa.

- `firmware/`: the ESP-IDF 6.0.1 project. `main/` holds the terminal and base-station radio (bs-radio) roles, chosen at boot. `components/` holds the shared libraries: `oc_sig` (signalling: activation, MILENAGE, registration, calls), `oc_air`, `oc_phy`, `oc_link`, `oc_term`, `oc_exec`, `oc_radio` and others.
- `host-tests/`: Unity tests of the portable components, built with CMake on the host.
- `tools/ocbench/`: the bench tool (cell, network stand-in `ocbench net`, activation codes `ocbench mkqr`, radio probes).
- `tools/ble/`: laptop BLE client (`oc_ble.py`, pairs with the terminal's rolling passkey) and console logger. `tools/qr/`: activation QR codes.
- `docs/bench/`: bench records.

Build:

```bash
cmake -S host-tests -B host-tests/build && cmake --build host-tests/build -j && (cd host-tests/build && ctest)
source ~/.espressif/tools/activate_idf_v6.0.1.sh && idf.py -C firmware -DOC_BENCH_LOW_POWER=1 build
cmake -S tools/ocbench -B tools/ocbench/build -G Ninja && cmake --build tools/ocbench/build
```

`firmware/sdkconfig` is generated from `sdkconfig.defaults` and not tracked: delete a stale one after pulling (see `docs/bench/w12-terminal-bringup.md`).

Design documents, specs and plans live in [opencell](https://github.com/opencell-dev/opencell). Branches: `main` (terminal and bs-radio, current), `bs-radio` (the base-station radio line before it merged into main).
