<!-- lc2oc: keep-file (this record names the legacy NVS namespaces on purpose) -->
# NVS migration lc -> oc on the bench (2026-09-29)

The first boards to boot the one firmware version that carries `oc_nvs_mig` (see `firmware/components/oc_nvs_mig/include/oc_nvs_mig.h`, the binding contract and the operator recovery steps). Plan: `docs/superpowers/plans/2026-09-29-oc-rename.md` in the docs repository, Task 5.

## Image and boards

- Image: branch `oc-rename`, commit `66c5b46` ("NVS: move lc, lc_id, lc_scan, lc_ble to oc* on the first boot"), built on the laptop with `idf.py -C firmware -DOC_BENCH_LOW_POWER=1 build` (ESP-IDF v6.0.1); the boot banner reads `App version: 66c5b46`. `flash_args` writes the bootloader (0x0), the partition table (0x8000), `ota_data_initial.bin` (0xf000) and `opencell_w12.bin` (0x20000); the NVS (0x9000, 0x6000) is not in it.
- Previous image on the boards: T `f05c414`, A `72d6c54-dirty` (both from before the rename).
- Where: both boards on the Raspberry Pi `opencell-bs1` (USB). The Pi has no ESP-IDF: the image was copied there (sha256 checked) and flashed with esptool 5.3.0 from a venv on the Pi (`~/esptool-venv`; `~/.venvs/opencell` with esptool 5.3.0, pyserial and bleak for `oc_console.py`).
  - Board A, bs-radio: `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_44:B1:76:AE:1A:E8-if00`
  - Terminal T, OpenCell-76AD0488: `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_44:B1:76:AD:04:88-if00`
  - T2 (`44:B1:76:AE:20:64`): not connected. It migrates when it next boots this firmware (the rename plan's Task 12 checks its `oc_nvs` lines and read-back before the migration is removed; restoring its archived dump `t2-76ae2064-nvs-20260928.bin` first just migrates again). Task 12 must not remove the migration before T2 has migrated.
- No other process had either port open (`fuser`), and nothing like `lcbench`/`ocbench`/`oc-cell` was running on the Pi.

## Before: the old image's view

T (console, reset, old image):

```
I (386) lc_main: role: terminal
I (756) lc_scan: scan list: 1 user, 2 network (v5), 4 learned, last 922250 kHz, fallback 2/13
I (1050) lc_ident: identity: activated, key id 1
I (1195) lc_ble: 2 bonded phone(s)
```

A (console, reset, old image):

```
I (728) lc_main: bs-radio up: configured=1 band=0 role=0 radio_err=0
```

## Backups

Each partition read with `esptool --chip esp32s3 -p PORT --after no-reset read-flash 0x9000 0x6000 FILE`, read twice (the two reads identical), after the console logs above. `--after no-reset` left each board in its bootloader, so neither booted its old image again between the backup and the flash: each backup is exactly the partition the migration then started from. In `~/Documents/opencell-archive/nvs-backups/` on the laptop (read-only) and `~/nvs/` on the Pi:

| file | bytes | sha256 |
| --- | --- | --- |
| `t-76ad0488-nvs-20260929-pre-oc.bin` | 24576 | `2355df40f5045bb0cdfd2709e052f71205a2b171440f9bf0d762f28b44d3fbbb` |
| `a-44b176ae1ae8-nvs-20260929-pre-oc.bin` | 24576 | `94cc092b219cb728ee7224766801ea5b68662d05e6c3aeeef0f3ce846cc374ef` |
| `t-76ad0488-nvs-20260929-post-oc.bin` | 24576 | T after the migration and two boots |
| `a-44b176ae1ae8-nvs-20260929-post-oc.bin` | 24576 | A after the migration and two boots |

Board A had no earlier archived dump: `a-44b176ae1ae8-nvs-20260929-pre-oc.bin` is its baseline.

Our keys in the backups (`tools/rename/nvs_fp.py`, old namespaces shown under their new names; the last column is a truncated sha256 of the value, never the value):

```
T:  oc term uint8_t 6b86b273ff34fce1
    oc_ble gatt_ver uint8_t 4b227777d4dd1fc6
    oc_id ident blob_data ca0670257d473b99
    oc_scan list blob_data ece1af9bb7b3670d
A:  oc cfg blob_data e1cd4010110cafee
```

## Rehearsal (linux target, before flashing)

`host-tests/nvs_linux/check.sh DUMP` on a copy of each backup (ESP-IDF's own NVS code, two boots):

```
t-76ad0488-nvs-20260929-pre-oc.bin
oc_nvs: NVS lc -> oc: 1 key(s) moved
oc_nvs: NVS lc_id -> oc_id: 1 key(s) moved
oc_nvs: NVS lc_scan -> oc_scan: 1 key(s) moved
oc_nvs: NVS lc_ble -> oc_ble: 1 key(s) moved
use: oc oc_id oc_scan oc_ble
PASS t-76ad0488-nvs-20260929-pre-oc.bin: 4 key(s) kept

a-44b176ae1ae8-nvs-20260929-pre-oc.bin
oc_nvs: NVS lc -> oc: 1 key(s) moved
oc_nvs: NVS lc_id -> oc_id: 0 key(s) moved
oc_nvs: NVS lc_scan -> oc_scan: 0 key(s) moved
oc_nvs: NVS lc_ble -> oc_ble: 0 key(s) moved
use: oc oc_id oc_scan oc_ble
PASS a-44b176ae1ae8-nvs-20260929-pre-oc.bin: 1 key(s) kept
```

A FAILED first boot, per pair, on both backups (`check.sh DUMP failed:PAIR`, PAIR each of `lc`, `lc_id`, `lc_scan`, `lc_ble`): every run printed `oc_nvs: NVS <from> -> <to> FAILED: reading <from>, no writes this boot`, wrote nothing in that pair, and converged on the next clean boot (`PASS ... failed:PAIR: 4 key(s) kept, identity intact, converged` for T; `1 key(s) kept, identity absent, converged` for A, which has no identity). This was done on the linux target only; no board was made to fail.

## T: flash, first boot, second boot

`esptool --chip esp32s3 -p T --after no-reset write-flash @flash_args`: 4 of 4 `Hash of data verified`. First boot on the new image (started and logged by `oc_console.py --reset`):

```
I (335) app_init: App version:      66c5b46
W (450) oc_nvs: NVS lc -> oc: 1 key(s) moved
W (462) oc_nvs: NVS lc_id -> oc_id: 1 key(s) moved
W (474) oc_nvs: NVS lc_scan -> oc_scan: 1 key(s) moved
W (483) oc_nvs: NVS lc_ble -> oc_ble: 1 key(s) moved
I (484) oc_main: role: terminal
I (858) oc_scan: scan list: 1 user, 2 network (v5), 4 learned, last 922250 kHz, fallback 2/13
I (1152) oc_ident: identity: activated, key id 1
I (1239) oc_ble: 2 bonded phone(s)
```

The same four lines as the rehearsal. Second boot: `0` `oc_nvs` lines, and

```
I (402) oc_main: role: terminal
I (773) oc_scan: scan list: 1 user, 2 network (v5), 4 learned, last 922250 kHz, fallback 2/13
I (1061) oc_ident: identity: activated, key id 1
I (1147) oc_ble: 2 bonded phone(s)
```

## T: NVS read back

`t-76ad0488-nvs-20260929-post-oc.bin` against the backup: `same` (every key of ours has the value it had, the identity `oc_id:ident` included: `ca0670257d473b99` before and after, a 114-byte blob), and `0` keys left under an `lc` name. Stored now: `oc:term`, `oc_id:ident`, `oc_scan:list`, `oc_ble:gatt_ver`, and `oc_moved` in each of the four. NimBLE's bond store (`nimble_bond`, 18 entries) is byte-for-byte the same as before.

## A: flash, first boot

4 of 4 `Hash of data verified`. First boot:

```
I (332) app_init: App version:      66c5b46
W (434) oc_nvs: NVS lc -> oc: 1 key(s) moved
W (436) oc_nvs: NVS lc_id -> oc_id: 0 key(s) moved
W (437) oc_nvs: NVS lc_scan -> oc_scan: 0 key(s) moved
W (439) oc_nvs: NVS lc_ble -> oc_ble: 0 key(s) moved
I (820) oc_main: bs-radio up: configured=1 band=0 role=0 radio_err=0
```

The same four lines as the rehearsal, and `configured=1` as before. Second boot: `0` `oc_nvs` lines, `bs-radio up: configured=1 band=0 role=0 radio_err=0`. Read back: `same`, `0` keys under an `lc` name; stored now: `oc:cfg` and `oc_moved` in each of the four.

## Notes

- Downgrade (Design §3): an older image after this one starts from `lc*`; back on this one, the moved data wins and the old image's changes are dropped. After this migration `lc*` is empty, so an older image sees a fresh board: a terminal comes up as a bs-radio (no `lc:term`), and a terminal switched back makes a new, not activated identity in `lc_id`. Back on this image, the marker in `oc_id` makes that boot DONE and erases `lc_id`: the activated identity in `oc_id` is the one kept.
- ESP-IDF's NVS iterator (`nvs_entry_find()`/`nvs_entry_next()`) skips entries it cannot read rather than returning an error, so a namespace listing can come back short with no error. `oc_nvs_mig`'s copy-and-compare only checks the keys the listing returned. The bench check here does not rely on the iterator alone: `nvs_fp.py` reads the raw partition with `nvs_tool.py`, and the before/after fingerprints matched on both boards.
- An activation on a FAILED (or UNSURE) boot of `lc_id -> oc_id` succeeds only in RAM: the terminal runs on the new identity that boot, but `term_ident.c` does not write it (`oc_ident: identity not written: NVS oc_id not migrated this boot`, then `identity save failed`), and after the next reboot the board is back on the identity it had. The activation has to be done again once the pair has migrated.
- Recovery, if a pair is ever stuck: `oc_nvs_mig.h`, "Recovering a pair stuck UNSURE"; the reference partitions are the `*-20260929-pre-oc.bin` backups above.
