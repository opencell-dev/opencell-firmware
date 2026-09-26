# W12 Terminal Bring-up Record (plan 3)

Fill in as the bench tasks are run. Setup and equipment are as in `docs/bench/w12-bringup.md` (plan 2), plus one W12 as the terminal (W12-T) and an Android phone with nRF Connect.

## Setup

| Item | Value |
|---|---|
| W12-T serial / TMID | MAC 44:B1:76:AD:04:88 / TMID 76AD0488 |
| bs-radio 915 (W12-1) tty | |
| bs-radio 2.4 (W12-2) tty | |
| 915 path attenuation (dB) | |
| 2.4 path attenuation (dB) | |
| Firmware commit | terminal branch, lc_radio calibration-timeout fix (2026-09-25) |

## Task 8 — first boot, role switch, OLED, BLE

| Check | Pass criterion | Result |
|---|---|---|
| Role switch to terminal | log `switching role to terminal`, then `role: terminal` | PASS: BOOT down at 0.49 s → switch 3 s later → `role: terminal`. First boot as terminal gave `radio_err -705` (20 ms BUSY timeout vs dual-band calibration) — fixed; now `radio_err 0` |
| OLED | `OPENCELL SEARCHING` / `NO SERVICE` / TMID | PASS (user: shows SEARCHING and the TMID) |
| BLE advertising | nRF Connect lists `OpenCell-<TMID>` with service 6c630001-… | PASS: connected; service 6c630001-7e2a-4b8e-9f2d-3c1a5e7b0d10 |
| STATUS read | 20 bytes, byte 0 = 00, bytes 8–11 = TMID (LE) | PASS: `00-00-02-00-00-00-00-00-88-04-AD-76-00-…-00` (SEARCH, 915, EDGE, TMID 76AD0488) |
| UP write while searching | fails with ATT error 0x80 | PASS: board logged `UP write len=1 state=0 rc=-1 -> att 128` for the phone's write; laptop (bleak) write-with-response returned `Application-specific Error 0x80`. (nRF Connect didn't visibly show the error.) GATT table confirmed: UP write/write-no-rsp, DOWN notify, STATUS read/notify |
| Role switch back | `role` flips back to bs-radio after another 3 s hold | PASS: `switching role to bs-radio` → `bs-radio up … radio_err=0`, then `switching role to terminal` → `terminal up: tmid 76ad0488 radio_err 0` |

## Task 9 — 915 MHz attach, grants, loopback, paging

| Test | Pass criterion | Result |
|---|---|---|
| Attach, EDGE | GRANTED within 15 s; `attach 1`, `grants ≥ 2` | |
| UL continuity, 60 s, EDGE | `ul` grows ≥ 495 per 60 s (≥ 99 % of 500 frames) | |
| UL continuity, 60 s, MID | as above | |
| UL continuity, 60 s, NEAR (FLRC 260 kb/s) | as above | |
| Loopback | 20 UP writes → ≥ 19 identical DOWN notifications, each < 0.5 s | |
| Paging | `--idle --page-after 30`: IDLE, then GRANTED ≤ 3 s after `paging`; `page_reply 1` | |
| Time to GRANTED from power-up, 10 runs | every run ≤ 12 s (list them) | |

## Task 10 — 2.4 GHz, cross-band, fallback, coexistence

| Test | Pass criterion | Result |
|---|---|---|
| Cross-band NEAR (DL 915 / UL 2.4) | GRANTED; `ul` ≥ 99 % over 60 s; loopback works | |
| 2.4 TDD MID | GRANTED with STATUS band = 1; `ul` ≥ 99 % over 60 s | |
| 2.4 fallback | 2.4 pigtail removed → re-attach on 915 within 3 s (`attach 2`), STATUS band = 0, tier MID | |
| BLE coexistence (2.4 TDD MID) | `ul` loss with the phone connected and notifying ≤ loss without + 1 % | |
| Band switch time used | `LC_TERM_BAND_SWITCH_US` = plan 2 item 4 measurement + ≥ 100 µs, and equal to plan 4 `band_switch_us` | |

## Over-the-air, low-power link (2026-09-25, no attenuators)

Setup: boards A (44:B1:76:AE:1A:E8) and T (44:B1:76:AD:04:88), both in `bench` role (internal 1 Hz PPS), ~1 m apart with 915 MHz antennas; firmware built with `idf.py -DLC_BENCH_LOW_POWER=1` (GC1109 PA bypassed via CPS low, chip drive −9 dBm ≈ −10 dBm at the port, below the Part 15.249 limit). Host link over each board's USB-Serial-JTAG (no USB-UART adapter).

| Test | Result |
|---|---|
| `lcbench config … bench 915` over USB | `ack 0`; board restarts, STATUS every 1 s, clock LOCKED on internal PPS, 0 link errors with console text on the same port |
| `lcbench link A T 915250000 edge 100 --internal`, stock `LC_EXEC_CONFIG_LEAD_US` = 400 µs | **FAIL**: 0/100. Diagnostics: every slot configured + staged, 0 launched — all skipped as late (configure + stage > 400 µs lead) |
| Same, lead raised to 5000 µs (diagnostic build) | **91/100 received, all CRC OK**, RSSI −33.5 dBm (−34…−33), SNR 14.9 dB. 9 lost to the unaligned internal clocks (RX window 100 ms of 120) |
| Radio op timing, worst case (8 MHz SPI, 160 MHz CPU) | configure 379 µs, stage TX 921 µs, **stage RX 1836 µs**, launch 390 µs |
| TX-done on board A | seen on only 2 of 96 TX slots (94 overruns) although T received 91 — hypothesis: TX (ramp + airtime) ends just after the slot's 200 µs guard, so the executor stops polling first |

Consequences (feed plan 2 Task 12 / plan 3 Task 9):
- `LC_EXEC_CONFIG_LEAD_US` 400 µs is unreachable today; back-to-back slots (beacon → AG with ~200 µs guard) can't be prepared in time. Speed up configure/stage (skip RadioLib's per-stage `getPacketType`/`calculateRxTimeout` reads, SPI 16 MHz, CPU 240 MHz) and re-measure before the terminal attach test.
- Size TX slots / guard from measured TX-done latency, then re-check overruns.
- Found and fixed on the way: `lcbench` `link`/`guard`/`cw` read the wrong argv index (could never run); `lcbench` reset USB-attached boards on exit (HUPCL); the bs-radio STATUS heartbeat now goes out both ports.

## GNSS modules on both boards (2026-09-25)

- 9600-baud sniff on the header: NMEA arrives on **GPIO39** (not GPIO38 as the published map says) — firmware `W12_PIN_HDR_RX` is now 39, `W12_PIN_HDR_TX` 38; the Pi link uses the same header pins, so the spec wiring is corrected too.
- GNSS supply gate GPIO48 is active-low (verified: 1732 bytes/6 s with it low, draining only with it high); board init now drives GPIO48 low, GPIO42 (reset) high, GPIO40 (force-on) high.
- Indoors: `$GNGGA,,,,,,0,00,25.5…` — no fix, 0 satellites, so no PPS (0 edges on GPIO41). PPS needs sky view.

## GPS-locked over-the-air link (2026-09-25, both boards on GNSS PPS, low-power build)

Both boards in `bs` role on their GNSS module's PPS (GPIO41), clocks LOCKED; ~1 m apart at a window.

| Setup (timing-diagnostic build, `LC_EXEC_CONFIG_LEAD_US` temporarily 5000 µs) | Result |
|---|---|
| 160 MHz CPU, 8 MHz SPI (previous run) | worst: configure 379, stage TX 921, stage RX 1836, launch 390 µs |
| **240 MHz CPU, 16 MHz SPI** | avg/max: configure 271–290/350, stage TX 653/690, **stage RX 1342/1393**, launch 254–286/350 µs |
| `lcbench link … edge 100` (frames from the laptop clock) | 24–39/100 — SCHEDULE refused MALFORMED ×55: laptop clock (systemd-timesyncd, offset −62 ms, jitter 66 ms) put some schedules 3 frames ahead |
| `lcbench link … edge 200 --internal --rx-window-us 20000` (frames from each board's STATUS) | **167/200, all CRC OK**, RSSI −49.5 dBm, SNR 12.0 dB; losses are LATE schedules at start (stale STATUS in the USB buffer seeds the frame estimate) |

Next (plan 2 Task 12): stage RX/TX still ~2–3× the 400 µs lead. RadioLib's `stageMode` re-reads the packet type and re-sends packet params, IRQ config and IRQ clear every slot; a fast path that skips unchanged settings is needed before back-to-back slots (and the terminal attach test) can work. For bench runs, sync the laptop clock better (chrony) or use `--internal`.

## Staging fast path + measured guard (2026-09-25, GPS-locked, low-power build)

`lc_radio` stages TX/RX itself instead of RadioLib's `stageMode`, sending only what changed (packet type tracked, packet params / IRQ mapping / RX path cached) plus a TX/RX FIFO clear each slot (without it, stale FIFO bytes produced bad payloads).

| µs avg/max | RadioLib `stageMode` | fast path |
|---|---|---|
| stage TX | 653 / 690 | 369 / 652 |
| stage RX | 1342 / 1393 | 261 / 1339 (first slot only) |

With the stock 400 µs lead the fast path alone still launched few slots, so from the measurements: **`LC_EXEC_CONFIG_LEAD_US` = 1200 µs, `LC_GUARD_US` = 1200 µs** (back-to-back slots need guard ≥ lead). Clean build, no diagnostics, `lcbench link … 300 --internal --rx-window-us 20000`:

| Tier | Modulation | A→T received | CRC fail / bad payload | RSSI | SNR |
|---|---|---|---|---|---|
| edge | LoRa SF7/500 kHz | 228/300 (T→A 234/300) | 0 / 0 | −32 dBm | 9.5 dB |
| mid | LoRa SF5/500 kHz | 234/300 | 0 / 0 | −37 dBm | 8.4 dB |
| near | FLRC 260 kb/s | 235/300 | 0 / 0 | −38 dBm | — (no SNR for FLRC) |

Remaining losses are SCHEDULE parts refused LATE (~35–55 per board per 300 frames) — an `lcbench` scheduling issue, not the radio (ignoring STATUS for 1.5 s after opening didn't change it; still open).

LR-FHSS: not testable board-to-board — the LR2021 (and RadioLib) can only transmit LR-FHSS; receiving needs an SX1302/1303 gateway.

## LATE schedules fixed (2026-09-25)

Cause: `lcbench` estimates each board's frame from its last STATUS, which doesn't carry the phase within the frame, so the estimate can lag by one; "2 ahead" was then sometimes 1 ahead with the frame boundary inside the W12's 1.5 ms setup window → LATE. Fix: `lcbench` schedules 3 ahead of its estimate and the W12 accepts up to 3 ahead (`LC_EXEC_MAX_AHEAD` 3, `LC_EXEC_FRAMES` 4). Edge tier, 300 frames: **A→T 275/300, T→A 278/300**, no LATE schedules, 0 CRC/payload errors. ~7–8 % still lost (not yet explained).
