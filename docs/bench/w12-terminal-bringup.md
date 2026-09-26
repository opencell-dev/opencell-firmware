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
