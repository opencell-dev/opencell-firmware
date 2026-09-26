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

## 2.4 GHz over the air (2026-09-25, GPS-locked, ~+3 dBm: chip −19 dBm + RFX2402E)

| Mode at 2440 MHz | Result |
|---|---|
| FLRC 1.3 Mb/s (near tier), before fix | 0/300 — every RX ended in an error IRQ, chip error `0x0200` RXFREQ_NO_FE_CAL_ERR: a 915 bs-radio only calibrated its own band's front end |
| **fix: every role calibrates both front ends at boot (904, 924 MHz LF + 2440 MHz HF)** | |
| FLRC 1.3 Mb/s (near) | **282/300**, 2 CRC fails, RSSI −53 dBm |
| FLRC 520 kb/s (mid tier) | 281 detected, **all CRC fail** |
| FLRC 650 / 260 kb/s (experiments) | 182/182 detected, all CRC fail (260 kb/s works at 915 MHz) |
| RX offset sweep −80…+80 kHz, FLRC 260 | detected between −20…+40 kHz (centre ≈ +10 kHz), **never** CRC OK → not a crystal-offset problem |
| **LoRa SF7 / 812.5 kHz (bench-only mode)** | **276/300 both directions**, 0 CRC fails, RSSI −51 dBm, SNR 15 dB |

Open: FLRC below 1.3 Mb/s on the 2.4 GHz path decodes nothing (cause unknown without the LR2021 datasheet's FLRC table; possibly a RadioLib HF parameter). The 2.4 GHz mid tier (FLRC 520 kb/s) is therefore unusable as specified; LoRa SF7/812.5 kHz is a working candidate.

## 2.4 GHz FLRC below 1.3 Mb/s — investigation (2026-09-25, with the LR20xx datasheet Rev 2.2)

Datasheet Table 3-13 specifies FLRC at 2.4 GHz down to 260 kb/s (CR 3/4, −108.5 dBm; freq tolerance ±25 kHz at 260, ±50 kHz at 520), so the chip supports it. Hypotheses tested, all **rejected**:

1. Crystal offset: RX offset sweep −80…+80 kHz — packets detected around +10 kHz but never CRC OK at any offset.
2. Missing firmware patch (datasheet §22.3, "highly recommended"; RadioLib never loads it): now loaded after every reset (Semtech PRAM v0x0313, verified `loaded 1`) — **kept**; 915 edge 191/200 and 2.4 FLRC 1.3 Mb/s 178–191/200 unchanged; FLRC 520 at 2.4 still 0/186.
3. RadioLib's DC-DC switcher workaround (retunes the switcher for narrow bandwidths, not in Semtech's driver): undone on the HF path — FLRC 520 still 0/191.

RadioLib's `SetFlrcModulationParams` encoding matches §18.4.1 (bitrate_bw, `cr<<4 | shape`, BT 0.5 = 0x5). Next steps would be to reproduce with Semtech's own driver (Lora-net/usp) to split RadioLib vs chip, or ask RadioLib/Semtech. LoRa SF7/812.5 kHz works at 2.4 GHz (276/300).

## 2.4 GHz mid tier changed to LoRa SF7 / 812.5 kHz (2026-09-25)

Decision (option A): `lc_phy` 2.4 GHz mid = LoRa SF7/812.5 kHz, CR 4/5, preamble 8 (10 280 µs for 28 bytes). With the real tier table: A→T 267/300 (1 CRC fail), T→A 274/300, RSSI −51…−52 dBm, SNR 14.9 dB.

## Periodic 3 s loss fixed: schedule by board frame (2026-09-25)

The remaining ~7–8 % loss came in a 3 s pattern: `lcbench` counted frames on the host clock and skipped a board frame whenever the two drifted across a frame boundary. It now keeps a per-board next-frame counter and schedules every board frame up to its estimate + 3 (TX and RX boards' frames paired by their measured difference, snapped to 0 within ±1). 915 mid: **600/600**.

## Long runs, 10 000 frames per mode (2026-09-25 23:30 – 01:10, A→T, GPS-locked, low-power build)

| Mode | Received | CRC fail | Lost | Notes |
|---|---|---|---|---|
| 915 edge, LoRa SF7/500k | 9 999 | 0 | 1 | the one loss is frame 0 (start-up) |
| 915 mid, LoRa SF5/500k | 9 999 | 0 | 1 | frame 0 |
| 915 near, FLRC 260k | 9 999 | 0 | 1 | frame 0 |
| 2440 near, FLRC 1.3M | 9 905 | 26 | 95 (0.95 %) | spread through the run |
| 2440 mid, LoRa SF7/812.5k | 9 942 | 48 | 58 (0.58 %) | spread through the run |

915 MHz is error-free apart from start-up. 2.4 GHz losses include CRC failures at 15 dB SNR (RSSI −51 dBm), and their spacing is dominated by 44 frames (5.28 s) and 88 frames: a periodic interferer in the band rather than link margin. 2440 MHz is inside Wi-Fi channels 6/7. The next night's check (below) did not bear the Wi-Fi idea out. Occasional `TIME` ACKs come back LATE (the board saw no PPS edge in the 900 ms before the message). Most likely cause, not yet verified: `lcbench` times TIME by the laptop clock, not the boards' PPS phase. Harmless once the clock is labelled.

## Slot timing to the microsecond (2026-09-26, overnight)

**Instrumentation.**

- `RX_REPORT.end_us`: RX_DONE, µs from the receiving board's frame start. The bs-radio timestamps the LR2021 IRQ line (DIO8) in an ISR.
- STATUS adds the latest TX preamble start (BUSY falls after SetTx; datasheet §5.3: "BUSY goes low when the PA has ramped-up and transmission of preamble starts"), the TX done time, late slots, radio errors, and the last failing radio op and code.
- `lcbench` prints mean / SD / min / max of each against the ideal (slot start, or slot start + airtime). `LCB_DUMP=1` prints every sample.

**Where the time went** (915 mid, both directions):

| Step | Preamble start after slot start | SD |
|---|---|---|
| Before (RadioLib `setTx` from standby) | +306 µs (timestamp inflated by RadioLib's status read-back; true ≈ +245) | 11 µs |
| Stage into FS (PLL locked) instead of standby | −60 µs, matching datasheet RC→TX 106 µs vs FS→TX 45 µs | 11 µs |
| One bare SPI transfer for SetTx (no allocation or paranoid read-back) | +168 µs (true BUSY-low stamp) | 11 µs |
| **SetTx clocked in with NSS held low; NSS raised at slot start − learned NSS→BUSY latency, interrupts off for the last 20 µs** | **+0.1 µs** | **0.6–1.2 µs** |

`launch()` now takes the start time. `lc_exec` (and `lc_term`) hand it over `LC_RADIO_ARM_US` (300 µs) early, and `lc_radio` fires itself.

**All modes after the change (500 frames each direction):**

| Mode | Preamble start | TX start SD | RX_DONE after ideal end | RX end SD |
|---|---|---|---|---|
| 915 edge SF7/500k | +0.1…0.3 µs | 0.8–1.3 µs | +223 µs | 3.5–3.7 µs |
| 915 mid SF5/500k | +0.1…0.3 µs | 0.6 µs | +141 µs | 2.9–3.1 µs |
| 915 near FLRC 260k | +0.5 µs | 1.9 µs | +264 µs | 8–14 µs (rare outliers, 47 µs off in 1 of 499 in a later dump; cause not investigated) |
| 2440 near FLRC 1.3M | +0.3…0.5 µs | 0.6–2.5 µs | +158 µs | 2.4 µs (one run 9.7: outlier) |
| 2440 mid SF7/812.5k | +0.4…0.9 µs | 0.7–3.4 µs | +168 µs | 3.6–3.8 µs |

- **Clocks.** A→T vs T→A means differ by < 1 µs, so the two GPS-disciplined frame clocks agree to sub-µs on average. The per-second (PPS) component of the jitter is 1.3 µs SD for both boards combined; within a second it's 2.4 µs, which is receiver-side timestamp and demod noise. That is at `esp_timer`'s 1 µs resolution.
- **Airtime.** TX done − preamble start is constant to ±1 µs per mode, and `lc_airtime_us`'s payload slope is exact. TX_DONE comes ~137 µs after the formula end. FLRC adds ~34 bits the formula leaves out (21-bit timing preamble, tail).
- **RX_DONE lag.** RX_DONE follows TX_DONE by 4 µs (SF5) to 85 µs (SF7/500k) of demodulator latency. `lc_rx_done_lag_us(mode)` (lc_phy) fits the RX_DONE lag to within 12 µs.
- **Terminal fix.** `lc_term` now subtracts that lag when it times the base from a beacon or DL. Before, a terminal's clock would have lagged the base by ~220 µs (edge beacon), and its uplinks with it. The host simulation now delivers RX_DONE that late, and the old code fails it.

## Cross-band duplex on one radio (2026-09-26)

`lcbench duplex`: per frame, A sends DL on one band to T, then T sends UL on the other band to A. Each board switches bands twice per frame, as a single-W12 terminal must.

What it took:

1. **Mode changes were 4.4 ms.** Every RadioLib setter re-reads the packet type and re-runs the DC-DC workaround. A same-packet-type mode change is now one modulation command.
2. **RadioLib's DC-DC workaround is buggy.** It passes `sizeof()` as a *word* count, so every modulation change overran an ESP32 stack variable and wrote 12 bytes of stack into LR2021 RAM after DCDC_FREQ_LF (0x80004C). `lc_radio` snapshots those words after reset and does the workaround itself, including for SetRxPath.
   - It was **not** the 2.4 GHz FLRC problem; see the bisect below. With the stray writes left in place, FLRC 520 at 2440 still passes 199/200.
   - Draft upstream issue: `draft-radiolib-issue-lr2021-flrc-2g4.md`, not posted.
3. **The 2.4 GHz PA was refused.** SetPaConfig for the HF PA returns CMD_INVALID (−706) while the LF RX path is selected after an LF reception. A band change now moves the RX path first.
4. **SetRxPath is slow after a reception.** Moving to the other path right after a reception holds BUSY ~7.5 ms, in either direction. `lc_exec` gives a slot on another band `LC_EXEC_BAND_SWITCH_LEAD_US` = 12 ms; `lcbench duplex` leaves an 11 ms gap across bands.
5. **SPI paranoid mode is off** (`RADIOLIB_SPI_PARANOID=0`). RadioLib still checks the status byte each transfer returns.
6. **LoRa↔FLRC switching.** After each packet type's first full setup, switching back is SetPacketType, sync word and modulation, and the RX path is re-sent at staging (without that, FLRC RX after a same-band LoRa slot heard nothing). It still takes 3.1–3.2 ms from the previous slot's end to launch, measured by a gap sweep. So `LC_EXEC_MOD_SWITCH_LEAD_US` = 4 ms, and `lcbench duplex` leaves a 3 ms gap when the modulations differ.

**Duplex matrix, final firmware (300 frames; A = base side, T = terminal side, both single W12s):**

| DL (A→T) | UL (T→A) | Gap | DL rx | UL rx | DL / UL preamble start |
|---|---|---|---|---|---|
| 915 edge | 2.4 mid | 11 ms | 300 | 300 | +0.8 / +0.2 µs |
| 915 mid | 2.4 mid | 11 ms | 300 | 300 | +0.7 / +0.2 µs |
| 915 near | 2.4 near | 11 ms | 300 | 300 | +0.3 / +0.4 µs |
| 915 mid | 2.4 near | 11 ms | 300 | 300 | +1.3 / +0.5 µs |
| 2.4 mid | 915 mid | 11 ms | 300 | 300 | +0.1 / +0.8 µs |
| 2.4 near | 915 edge | 11 ms | 299 | 300 | +0.1 / +1.0 µs |
| 2.4 near | 915 near | 11 ms | 299 | 300 | +0.4 / +0.4 µs |
| 915 mid | 915 mid | 1.5 ms | 300 | 300 | +0.9 / +16.1 µs |
| 2.4 mid | 2.4 mid | 1.5 ms | 300 | 297 | +0.2 / +19.0 µs |
| 915 near | 915 mid | 3 ms | 300 | 300 | +0.1 / +0.7 µs |
| 915 mid | 915 near | 3 ms | 300 | 300 | +0.7 / +0.4 µs |
| 915 edge | 915 near | 3 ms | 300 | 300 | +0.8 / +0.2 µs |
| 2.4 near | 2.4 mid | 3 ms | 300 | 300 | +0.4 / +0.1 µs |

There are 0 CRC failures in every row. The same-mode TX↔RX turnaround at a 1.5 ms gap launches 16–19 µs late: config plus stage slightly exceeds the 1.2 ms lead minus the 300 µs arm. The guard absorbs it; faster SPI transactions would fix it (each RadioLib transfer costs ~50–80 µs in the ESP-IDF HAL).

**Tight RX windows.** RX window = the TX slot exactly (`--rx-window-us 1`), 300 frames per mode: 915 edge / mid / near 300/300, 2.4 near 299/300, 2.4 mid 300/300. RX-end spans are 9–30 µs. With starts on the microsecond, a slot's own guard is enough for the receiver.

## 2.4 GHz FLRC below 1.3 Mb/s: solved by starting TX from FS (bisected 2026-09-26)

FLRC 520 kb/s at 2440 MHz, 200 packets, `lcbench` with the 2.4 mid tier temporarily set to FLRC 520:

| Firmware | Result |
|---|---|
| `c23f38f` (before tonight's radio changes) | 0 received, 199 CRC errors: bug reproduces |
| `4f69be1` (stage into FS + bare SetTx/SetRx) | **199/200** |
| `4f69be1` without FS staging | 0 received, 196 CRC errors |
| `4f69be1`, FS on the RX side only | 0 received, 199 CRC errors |
| `4f69be1`, FS on the TX side only | **197/200** |
| HEAD with the DC-DC repair disabled (RadioLib's stray writes kept) | 199/200: the DC-DC bug is not the cause |

Cause: an FLRC transmission below 1.3 Mb/s on the HF path started with SetTx **from STDBY_RC** is corrupted, and started **from FS** it is fine. HEAD passes 260 / 520 / 650 kb/s at 200 / 198 / 199 of 200.

The 2.4 GHz mid tier could go back to FLRC 520 kb/s. Decided: keep LoRa SF7/812.5 kHz for its ~11.5 dB more link budget (spec §4.4). The upstream draft (`draft-radiolib-issue-lr2021-flrc-2g4.md`) is rewritten around this, with the DC-DC `sizeof` bug as a separate report. It is not posted.

## Final long runs (2026-09-26 03:06–05:46, HEAD before W12Hal, RX window = TX slot exactly)

| Run, 10 000 frames | Received | CRC fail | Preamble start (SD) | RX end SD |
|---|---|---|---|---|
| link 915 edge | **10 000** | 0 | +0.8 µs (0.59) | 2.6 µs |
| link 915 mid | **10 000** | 0 | +0.8 µs (0.56) | 1.7 µs |
| link 915 near | **10 000** | 0 | +0.3 µs (0.67) | 1.5 µs |
| link 2.4 near | 9 975 | 2 | +0.3 µs (0.59) | 2.5 µs |
| link 2.4 mid | 9 999 | 0 | +0.2 µs (0.47) | 2.5 µs |
| duplex 915 mid DL / 2.4 mid UL | **10 000** / 9 999 | 0 / 0 | +0.8 / +0.2 µs | 1.9 / 2.5 µs |
| duplex 2.4 near DL / 915 edge UL | 9 984 / 9 998 | 4 / 0 | +0.3 / +0.8 µs | 2.6 / 4.9 µs |
| duplex 915 near DL / 915 mid UL | **10 000** / **10 000** | 0 / 0 | +0.3 / +0.9 µs | 1.7 / 2.7 µs |

Compared with the previous night's runs (20 ms RX windows, old launch): 2.4 near loss fell from 0.95 % to 0.25 %, and 2.4 mid from 0.58 % to 0.01 %.

## W12Hal (after the long runs)

EspHal acquires and releases the SPI bus around every transfer and uses DMA. `W12Hal` keeps the bus acquired (the LR2021 is SPI2's only device) and polls the FIFO in 64-byte chunks.

- Same-mode TX↔RX turnaround at a 1.5 ms gap now launches on time: −0.1 µs, was +16 µs.
- LoRa↔FLRC switch plus staging: 2.8 ms, was 3.2 ms.
- Link 915 mid: 199/200, start −0.2 µs.

## 2440 vs 2480 MHz (2026-09-26, HEAD, 2.4 near FLRC 1.3M, 5 000 frames, RX window = slot)

| Frequency | Received | CRC fail | Lost |
|---|---|---|---|
| 2440 MHz (Wi-Fi ch 6/7) | 4 989 | 2 | 11 (0.22 %), spread out, no 44-frame pattern |
| 2480 MHz (above Wi-Fi; BLE advertising channel 39) | 4 909 | 41 | 91 (1.8 %) |

The previous night's 2.4 GHz losses (0.6–1 %, 44-frame spacing) are mostly gone with the new firmware at 2440 MHz, so they weren't simply Wi-Fi. 2480 MHz is worse. BLE advertising on channel 39 is a plausible cause; that's not verified, and the RFX2402E's response near the band edge is another candidate. The hop plan should avoid the BLE advertising channels (2402, 2426, 2480 MHz); worth adding to the 2.4 GHz channel list work.
