#!/usr/bin/env python3
"""check_iram.py ELF [NM]: the bs-radio's RX turnaround path must not run from flash.

Bench 2026-09-30 (rx-turnaround.md §14): in the worst tight turnaround every
step from the RX-done readout to the launch ran 1.4-2x slower than average,
with no lock wait, no gap in core 1's spin loops and no prepare running. Core
1 was not taken away, it ran slowly. The ESP32-S3's 16 KB instruction cache is
shared by both cores and fed from DIO flash, and core 0 is busy during every
turnaround (the report task encodes and writes the RX report). So the code
core 1 runs between an RX done and the next launch is placed in internal RAM
(firmware/main/turnaround.lf and sdkconfig.defaults), and this check, run after
every firmware build, fails the build if any of it lands in flash again.

Exit 0 when every listed function is outside the flash text range, 1 otherwise
(or when one is missing: a rename must update this list)."""
import re
import subprocess
import sys

FLASH_TEXT = (0x42000000, 0x44000000)

# (pattern on the mangled name, required). Not required: only in some builds.
TURNAROUND = [
    # exec task loop and executor (core 1)
    (r"^exec_task$", True),
    (r"^oc_exec_step$", True),
    (r"^oc_exec_staged$", True),
    (r"^oc_bsr_tick$", True),
    (r"^oc_bsr_make_rx_report$", True),
    (r"^oc_clock_frame_at$", True),
    (r"^oc_clock_frame_start_us$", True),
    (r"^oc_clock_tick$", True),
    (r"^oc_clock_on_pps$", True),
    (r"^app_link_on_rx$", True),
    # oc_radio's per-slot operations and the lean LR2021 commands
    (r"op_pollEPvP16oc_radio_event_t$", True),
    (r"op_configureEPvmPK9oc_mode_t$", True),
    (r"op_configure_EmPK9oc_mode_t$", True),
    (r"op_stage_rxEPvm$", True),
    (r"op_stage_txEPvPKhh$", True),
    (r"op_launchEPvy$", True),
    (r"fire_set_modeEtmyiPx$", True),
    (r"wait_busy_low_forEPxm$", True),
    (r"bus_xferEPvPhS1_j$", True),
    (r"lr_set_frequencyEm$", True),
    (r"W12Hal11spiTransferEPhjS1_$", True),
    (r"^oc_lr_(get_irq|irq_of|event|read_rx|read|write|rx_len|rx_quality|clear_irq|clear_rx_fifo|"
     r"clear_tx_fifo|write_tx_fifo|set_fs|set_rf_frequency)$", True),
    # RadioLib and the drivers it ends in
    (r"^_ZN6EspHal11digitalReadEm$", True),
    (r"^_ZN6EspHal12digitalWriteEmm$", True),
    (r"^_ZN6Module16setRfSwitchStateEh$", True),
    (r"^_ZNK6Module16findRfSwitchModeEh$", True),
    (r"^spi_device_polling_transmit$", True),
    (r"^spi_device_polling_start$", True),
    (r"^spi_device_polling_end$", True),
    (r"^gpio_get_level$", True),
    (r"^gpio_set_level$", True),
    (r"^esp_timer_get_time$", True),
    (r"^xQueueGenericSend$", True),
    (r"^xQueueReceive$", True),
    (r"^xQueueSemaphoreTake$", True),
    # trace builds only
    (r"^oc_rxt_(mark|begin|gap|launch_at|end_launched)$", False),
]


def symbols(elf, nm):
    out = subprocess.run([nm, elf], check=True, capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            syms.setdefault(p[2], int(p[0], 16))
    return syms


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    elf = sys.argv[1]
    nm = sys.argv[2] if len(sys.argv) > 2 else "xtensa-esp32s3-elf-nm"
    syms = symbols(elf, nm)
    bad, missing, ok = [], [], 0
    for pat, required in TURNAROUND:
        rx = re.compile(pat)
        hits = [(n, a) for n, a in syms.items() if rx.search(n)]
        if not hits:
            if required:
                missing.append(pat)
            continue
        for n, a in hits:
            if FLASH_TEXT[0] <= a < FLASH_TEXT[1]:
                bad.append((n, a))
            else:
                ok += 1
    for n, a in sorted(bad, key=lambda x: x[1]):
        print(f"check_iram: in flash: 0x{a:08x} {n}")
    for p in missing:
        print(f"check_iram: no symbol matches {p} (renamed? update tools/check_iram.py)")
    if bad or missing:
        print(f"check_iram: FAIL: {len(bad)} turnaround functions in flash, {len(missing)} missing ({ok} in RAM)")
        return 1
    print(f"check_iram: ok, {ok} turnaround functions in internal RAM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
