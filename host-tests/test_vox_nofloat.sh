#!/usr/bin/env bash
# The production sources of oc_vox (everything but ref/ and oc_vox_ref.c)
# use no floating point, no heap and no varargs; and they compile for a
# Cortex-M4 with clang as a portability check (skipped, exit 77, without
# clang). Comments are stripped before the grep.
#   test_vox_nofloat.sh COMPONENT_DIR WORK_DIR
set -euo pipefail
comp=$1; work=$2
mkdir -p "$work/stub"
fail() { echo "test_vox_nofloat: $*" >&2; exit 1; }

prod=( "$comp"/oc_vox_modes.c "$comp"/oc_vox_layout.c "$comp"/oc_vox_bits.c "$comp"/oc_vox_fec.c
       "$comp"/oc_vox_quant.c "$comp"/oc_vox_conceal.c "$comp"/oc_vox_pipe.c "$comp"/oc_vox.c
       "$comp"/fx/*.c )
headers=( "$comp"/include/oc_vox.h "$comp"/include/oc_vox_modes.h "$comp"/include/oc_vox_bits.h "$comp"/include/oc_vox_fec.h
          "$comp"/include/oc_vox_int.h "$comp"/include/oc_vox_layout.h "$comp"/include/oc_vox_conceal.h
          "$comp"/include/oc_vox_pipe.h "$comp"/include/oc_vox_stats.h "$comp"/include/oc_vox_state.h
          "$comp"/oc_vox_quant.h "$comp"/fx/oc_vox_fx_int.h )

for f in "${prod[@]}" "${headers[@]}"; do
  # drop comments, then look for the forbidden words
  gcc -fpreprocessed -dD -E -P "$f" 2>/dev/null > "$work/stripped.c" || cp "$f" "$work/stripped.c"
  if grep -nE '\b(float|double|long double|malloc|calloc|realloc|alloca|va_list|va_start)\b|\bfree[[:space:]]*\(' "$work/stripped.c"; then
    fail "$f uses floating point, the heap or varargs"
  fi
done
echo "test_vox_nofloat: no float, heap or varargs in $((${#prod[@]} + ${#headers[@]})) production files"

if ! command -v clang >/dev/null 2>&1; then
  echo "test_vox_nofloat: clang not found, the ARM cross-compile check is skipped"
  exit 77
fi
# a freestanding target has no libc headers: the few the sources need, declared
cat > "$work/stub/string.h" <<'H'
#include <stddef.h>
void *memset(void *, int, size_t); void *memcpy(void *, const void *, size_t); void *memmove(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
H
for f in "${prod[@]}"; do
  clang --target=armv7m-none-eabi -mcpu=cortex-m4 -mthumb -ffreestanding -nostdinc -std=c11 -Wall -Wextra -Werror \
        -isystem "$work/stub" -isystem "$(clang -print-resource-dir)/include" \
        -I "$comp/include" -I "$comp" -I "$comp/fx" -fsyntax-only "$f" || fail "$f does not compile for Cortex-M4"
done
echo "test_vox_nofloat: ${#prod[@]} production sources compile for Cortex-M4 (clang)"
