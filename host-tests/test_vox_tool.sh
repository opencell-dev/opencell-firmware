#!/usr/bin/env bash
# oc-vox on a vector: encode, inspect, decode, lose blocks; the decoded
# audio hashes to the production decoder's golden hash (test_vox_equiv).
#   test_vox_tool.sh OC_VOX VECTORS_DIR WORK_DIR
set -euo pipefail
ocvox=$1; vec=$2; work=$3
mkdir -p "$work"
fail() { echo "test_vox_tool: $*" >&2; exit 1; }

"$ocvox" enc 1200 "$vec/synth_vowels.raw" "$work/v1200.ovx" 2>"$work/enc.log"
grep -q "50 blocks (6.00 s) of OVX-1200" "$work/enc.log" || fail "enc summary: $(cat "$work/enc.log")"
[ "$(stat -c %s "$work/v1200.ovx")" = "$((2 + 50 * 18))" ] || fail "ovx size"
[ "$(head -c 2 "$work/v1200.ovx" | od -An -tx1 | tr -d ' ')" = "c503" ] || fail "header: magic c5, version 0 mode 3 -> 03"

"$ocvox" info "$work/v1200.ovx" > "$work/info.txt"
grep -q "^OVX-1200 1200 bit/s, 18 bytes a block, version 0, 50 blocks, 6.00 s$" "$work/info.txt" || fail "info: $(cat "$work/info.txt")"

"$ocvox" dec "$work/v1200.ovx" "$work/v1200.raw" 2>"$work/dec.log"
[ "$(stat -c %s "$work/v1200.raw")" = "$((50 * 960 * 2))" ] || fail "raw size"
grep -q "lost 0, concealed frames 0, toggle gaps 0, golay fixed 0 failed 0" "$work/dec.log" || fail "dec summary: $(cat "$work/dec.log")"
# the golden hash of synth_vowels through OVX-1200 (test_vox_equiv.c GOLDEN)
[ "$("$ocvox" hash "$work/v1200.raw")" = "a1a16b6508c0ded2" ] || fail "decoded hash $("$ocvox" hash "$work/v1200.raw")"

"$ocvox" dec "$work/v1200.ovx" "$work/lost.raw" --lose 5 2>"$work/lost.log"
grep -q "lost 10, concealed frames 60" "$work/lost.log" || fail "lose: $(cat "$work/lost.log")"
[ "$(stat -c %s "$work/lost.raw")" = "$((50 * 960 * 2))" ] || fail "lost raw size"
[ "$("$ocvox" hash "$work/lost.raw")" != "a1a16b6508c0ded2" ] || fail "losing blocks changed nothing"

"$ocvox" dec "$work/v1200.ovx" "$work/ber.raw" --ber 0.001 --seed 3 2>"$work/ber.log"
grep -Eq "golay fixed [1-9][0-9]* failed 0" "$work/ber.log" || fail "ber: $(cat "$work/ber.log")"

"$ocvox" enc 600 "$vec/synth_mixed.raw" "$work/m600.ovx" --ref 2>/dev/null
"$ocvox" info "$work/m600.ovx" | grep -q "^OVX-600 600 bit/s, 9 bytes a block" || fail "600 info"
"$ocvox" dec "$work/m600.ovx" "$work/m600.raw" --ref 2>/dev/null
[ "$(stat -c %s "$work/m600.raw")" = "$((50 * 960 * 2))" ] || fail "600 raw size"

if "$ocvox" enc 700 "$vec/synth_vowels.raw" "$work/x.ovx" 2>/dev/null; then fail "mode 700 accepted"; fi
if "$ocvox" info "$vec/synth_vowels.raw" 2>/dev/null; then fail "a raw file passed as .ovx"; fi
"$ocvox" analyse 1200 "$vec/chirp.raw" > "$work/chirp.txt"
[ "$(grep -vc '^#' "$work/chirp.txt")" = "96" ] || fail "analyse: $(grep -vc '^#' "$work/chirp.txt") lines"
echo "test_vox_tool: ok"
