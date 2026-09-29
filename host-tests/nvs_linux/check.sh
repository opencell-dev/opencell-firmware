#!/usr/bin/env bash
# lc2oc: keep-file (this file names the legacy NVS namespaces on purpose)
# Runs the NVS migration (ESP-IDF's real NVS code, linux target) on a copy of
# a dumped NVS partition, twice, and checks: every key of ours kept its value
# under its new namespace, nothing is left under an old one, the second boot
# moves nothing. Needs the ESP-IDF environment and build/nvs_linux.elf
# (idf.py build in this directory). Usage: check.sh DUMP.bin | NVS.csv
# (a .csv is first made into a partition by ESP-IDF's nvs_partition_gen.py).
#
# check.sh DUMP SCENARIO first boots with a fault injected (main/main.c,
# $OC_NVS_FAULT) into one pair's move, or all four's, then cleanly, twice.
# SCENARIO is KIND:PAIR, PAIR one of lc, lc_id, lc_scan, lc_ble, all:
#   failed  the move fails before writing: FAILED
#   unsure  the marker can't be read: UNSURE
#   cut     the marker can't be written after the copy, nor the copy erased:
#           FAILED, then a boot that can't read the marker: UNSURE, over an
#           unmarked copy in the new namespace
# It checks that a FAILED or UNSURE boot says so, writes nothing in that pair
# (with PAIR all, nothing at all), keeps the identity readable and unchanged,
# and that the next clean boot converges (moves the pair, and every key of
# ours kept its value), the boot after it moving nothing.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
fp="$here/../../tools/rename/nvs_fp.py"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
dump=$1
scen=${2:-}
if [ "${dump%.csv}" != "$dump" ]; then
    python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" generate "$dump" "$work/csv.bin" 0x6000 >/dev/null
    dump="$work/csv.bin"
fi
python3 -c "import sys; sys.stdout.buffer.write(b'\xff' * 0x620000)" >"$work/flash.img"
dd if="$here/build/partition_table/partition-table.bin" of="$work/flash.img" bs=1 seek=$((0x8000)) conv=notrunc status=none
dd if="$dump" of="$work/flash.img" bs=1 seek=$((0x9000)) conv=notrunc status=none

boot() { # boot LOG [FAULT]
    if [ -n "${2:-}" ]; then
        OC_FLASH="$work/flash.img" OC_NVS_FAULT="$2" timeout 20 "$here/build/nvs_linux.elf" >"$work/$1.log" 2>&1
    else
        OC_FLASH="$work/flash.img" timeout 20 "$here/build/nvs_linux.elf" >"$work/$1.log" 2>&1
    fi
}
part() { dd if="$work/flash.img" of="$1" bs=1 skip=$((0x9000)) count=$((0x6000)) status=none; }
show() { grep -E 'oc_nvs|use:' "$work/$1.log" | sed 's/^[WE] ([0-9]*) //'; }
fail() { echo "FAIL: $*"; exit 1; }
ident() { awk '/^ident: / && $2 != "none" { print $3, $4 }' "$work/$1.log"; } # length and hash, never the value

if [ -z "$scen" ]; then
    boot run1
    boot run2
    part "$work/after.bin"
    show run1
    python3 "$fp" "$dump" >"$work/before.fp"
    python3 "$fp" "$work/after.bin" | grep -v ' oc_moved ' >"$work/after.fp"
    diff "$work/before.fp" "$work/after.fp" || fail "values changed"
    if python3 "$fp" --raw "$work/after.bin" | grep -q '^lc'; then fail "keys left under an old namespace"; fi
    [ "$(python3 "$fp" --raw "$work/after.bin" | grep -c ' oc_moved ')" = 4 ] || fail "markers"
    if grep -q 'oc_nvs:' "$work/run2.log"; then fail "the second boot moved again"; fi
    grep -q 'use: oc oc_id oc_scan oc_ble' "$work/run2.log" || fail "second boot"
    echo "PASS $(basename "$1"): $(wc -l <"$work/before.fp") key(s) kept"
    exit 0
fi

kind=${scen%%:*}
pair=${scen#*:}
case "$kind" in failed | unsure | cut) ;; *) fail "scenario $scen: KIND is failed, unsure or cut" ;; esac
case "$pair" in
all) pairs="lc lc_id lc_scan lc_ble" ;;
lc | lc_id | lc_scan | lc_ble) pairs=$pair ;;
*) fail "scenario $scen: PAIR is lc, lc_id, lc_scan, lc_ble or all" ;;
esac
col() { case "$1" in lc) echo 1 ;; lc_id) echo 2 ;; lc_scan) echo 3 ;; lc_ble) echo 4 ;; esac; }
faults() { local f="" p; for p in $pairs; do f="$f${f:+,}$1:o${p#l}"; done; echo "$f"; }
rawns() { python3 "$fp" --raw "$1" | awk -v ns="$2" '$1 == ns { print $2, $3, $4 }'; } # the keys of ns, no names

python3 "$fp" "$dump" >"$work/before.fp"
part "$work/p0.bin"

# The faulted boot: FAILED (failed, cut) or UNSURE (unsure) for each pair of the scenario.
first=$kind
[ "$kind" = unsure ] || first=failed
boot runf "$(faults "$kind")"
part "$work/pf.bin"
show runf
checkbad() { # checkbad LOG RESULT BEFORE.bin AFTER.bin: the pairs' lines, no writes allowed, their namespaces
    local p o w
    for p in $pairs; do
        o=o${p#l}
        if [ "$2" = FAILED ]; then w="reading $p, no writes this boot"; else w="reading $o then $p, no writes this boot"; fi
        grep -q "oc_nvs: NVS $p -> $o $2: $w" "$work/$1.log" || fail "$1: no '$p -> $o $2' line"
        [ "$(awk '/^write: / { print $('"$(col "$p")"' + 1) }' "$work/$1.log")" = 0 ] || fail "$1: $p may be written"
        [ "$(rawns "$4" "$p")" = "$(rawns "$3" "$p")" ] || fail "$1: $p changed"
        if [ "$kind" = cut ]; then
            [ "$(rawns "$4" "$o")" = "$(rawns "$3" "$p")" ] || fail "$1: $o is not the unmarked copy of $p"
        else
            [ -z "$(rawns "$4" "$o")" ] || fail "$1: $o was written"
        fi
    done
}
checkbad runf "${first^^}" "$work/p0.bin" "$work/pf.bin"
if [ "$pair" = all ] && [ "$kind" != cut ]; then
    cmp -s "$work/p0.bin" "$work/pf.bin" || fail "runf: the partition was written"
fi
id_bad=$(ident runf)
if [ "$kind" = cut ]; then
    # A boot that can't read the marker, over the unmarked copy: UNSURE, and
    # not one byte written (the other pairs, if any, are DONE and silent).
    boot runu "$(faults unsure)"
    part "$work/pu.bin"
    show runu
    checkbad runu UNSURE "$work/pf.bin" "$work/pu.bin"
    cmp -s "$work/pf.bin" "$work/pu.bin" || fail "runu: the partition was written"
    [ "$(ident runu)" = "$id_bad" ] || fail "runu: another identity"
    case " $pairs " in *" lc_id "*)
        [ -z "$id_bad" ] || grep -q '^ident: oc_id ' "$work/runu.log" || fail "runu: identity not read from oc_id first" ;;
    esac
fi

# The next clean boot converges, and the one after it moves nothing.
boot runc
show runc
if grep -qE 'FAILED|UNSURE' "$work/runc.log"; then fail "runc: not converged"; fi
for p in $pairs; do
    grep -q "oc_nvs: NVS $p -> o${p#l}: [0-9]* key(s) moved" "$work/runc.log" || fail "runc: $p not moved"
done
boot rund
part "$work/after.bin"
python3 "$fp" "$work/after.bin" | grep -v ' oc_moved ' >"$work/after.fp"
diff "$work/before.fp" "$work/after.fp" || fail "values changed"
if python3 "$fp" --raw "$work/after.bin" | grep -q '^lc'; then fail "keys left under an old namespace"; fi
[ "$(python3 "$fp" --raw "$work/after.bin" | grep -c ' oc_moved ')" = 4 ] || fail "markers"
if grep -q 'oc_nvs:' "$work/rund.log"; then fail "the boot after the clean one moved again"; fi
grep -q 'use: oc oc_id oc_scan oc_ble' "$work/rund.log" || fail "the boot after the clean one"
grep -q 'write: 1 1 1 1' "$work/rund.log" || fail "the boot after the clean one can't write"
[ "$(ident rund)" = "$id_bad" ] || fail "the identity read on the $first boot is not the one kept"
echo "PASS $(basename "$1") $scen: $(wc -l <"$work/before.fp") key(s) kept, identity $([ -n "$id_bad" ] && echo intact || echo absent), converged"
