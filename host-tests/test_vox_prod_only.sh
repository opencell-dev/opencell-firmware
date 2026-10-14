#!/usr/bin/env bash
# oc_vox built as a consumer builds it (OC_VOX_REF OFF, OpenVoxNB spec V7):
# the library holds no oc_vox_ref_ symbol and asks libm for nothing, and a
# program using it links without -lm and runs.
#   test_vox_prod_only.sh SOURCE_DIR WORK_DIR
set -euo pipefail
src=$1; work=$2
fail() { echo "test_vox_prod_only: $*" >&2; exit 1; }
rm -rf "$work"
cmake -S "$src" -B "$work" >/dev/null
cmake --build "$work" >/dev/null
lib=$(find "$work" -name 'liboc_vox.a' | head -1)
[ -n "$lib" ] || fail "no liboc_vox.a"
# nm's output goes to a variable first: under pipefail, grep -q exiting early
# would SIGPIPE nm and make the pipeline's status lie.
defined=$(nm "$lib")
undefined=$(nm -u "$lib")
if grep -q ' oc_vox_ref_' <<<"$defined"; then fail "the floating-point reference is in the library"; fi
if grep -Eq ' U (sin|cos|tan|exp|exp2|log|log2|log10|sqrt|pow|floor|ceil|fabs|round)f?$' <<<"$undefined"; then
  fail "the library needs libm"
fi
"$work/vox_prod_link"
echo "test_vox_prod_only: production only, no libm"
