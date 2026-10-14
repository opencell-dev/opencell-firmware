#!/usr/bin/env python3
"""vox_train_cb.py --oc-vox OC_VOX [--out DIR] [--size 1024] [--min-e 20] FILE.raw...

Trains OpenVoxNB's stage-1 envelope codebook (PLAN.md Task 13) and writes
oc_vox_cb.h and oc_vox_cb.c into DIR (default: the component directory
next to this script's repository). Each FILE is 8 kHz mono s16le speech;
`oc-vox analyse 1200 FILE` gives the reference analysis's frames, of
which those with energy index >= --min-e (default 20, -81 dBFS) train.
The 12 band values (0.75 dB units, mean removed) are clustered by
k-means with LBG splitting from a fixed seed, so the same input gives the
same codebook. The header records the training set as the SHA-256 of
the concatenated analysis frames, so a codebook can be traced.

Needs numpy. A few minutes of several speakers is a sensible minimum;
the committed vectors (synthetic vowels) are not speech and make a
codebook that overfits them: train on recordings.

After writing: rebuild, run the host tests (test_vox_quant checks the
codebook; test_vox_equiv's golden hashes change deliberately: regenerate
them from its INFO lines), listen, and bump OC_VOX_VERSION at the freeze.
"""
import hashlib
import os
import subprocess
import sys

import numpy as np


def frames_of(oc_vox, path):
    out = subprocess.run([oc_vox, "analyse", "1200", path], check=True, capture_output=True, text=True).stdout
    rows = [list(map(int, line.split())) for line in out.splitlines() if line and not line.startswith("#")]
    return np.array(rows, dtype=np.int32)


def kmeans(x, k, rng, iters=25):
    """LBG: split from one centroid to k, k-means between splits."""
    cents = x.mean(axis=0, keepdims=True)
    while len(cents) < k:
        cents = np.concatenate([cents * 1.0 + rng.normal(0, 0.5, cents.shape), cents - rng.normal(0, 0.5, cents.shape)])
        cents = cents[:k]
        for _ in range(iters):
            d = ((x[:, None, :] - cents[None, :, :]) ** 2).sum(axis=2)
            lab = d.argmin(axis=1)
            for i in range(len(cents)):
                m = lab == i
                if m.any():
                    cents[i] = x[m].mean(axis=0)
                else:  # an empty cell: re-seed it on the worst-fitted vector
                    cents[i] = x[d.min(axis=1).argmax()]
    return cents


def main():
    args = sys.argv[1:]
    oc_vox, out_dir, size, min_e, files = None, None, 1024, 20, []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--oc-vox":
            oc_vox = args[i + 1]; i += 2
        elif a == "--out":
            out_dir = args[i + 1]; i += 2
        elif a == "--size":
            size = int(args[i + 1]); i += 2
        elif a == "--min-e":
            min_e = int(args[i + 1]); i += 2
        else:
            files.append(a); i += 1
    if oc_vox is None or not files:
        print(__doc__)
        sys.exit(1)
    if out_dir is None:
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        out_dir = os.path.join(root, "firmware", "components", "oc_vox")
    allf = np.concatenate([frames_of(oc_vox, f) for f in files])
    digest = hashlib.sha256(allf.tobytes()).hexdigest()[:16]
    s = allf[allf[:, 0] >= min_e][:, 5:17].astype(float)
    if len(s) < size * 4:
        print(f"only {len(s)} frames above the energy floor for {size} entries: train on more audio", file=sys.stderr)
    rng = np.random.default_rng(20261013)
    cents = kmeans(s, size, rng)
    cents = np.clip(np.round(cents - cents.mean(axis=1, keepdims=True)), -127, 127).astype(int)
    # every entry distinct (a small training set leaves empty cells on the same vector):
    # a duplicate is nudged one unit on successive bands until it is unique
    seen = set()
    for i in range(len(cents)):
        b = 0
        while tuple(cents[i]) in seen:  # +a on one band, -a on another: the mean stays, and a
            a = 1 + b // 12               # growing amplitude keeps the walk from circling back
            cents[i][b % 12] += a
            cents[i][(b + 6) % 12] -= a
            b += 1
        seen.add(tuple(cents[i]))
    # deterministic order: by the first band, then the rest
    cents = cents[np.lexsort(cents.T[::-1])]
    dist = ((s[:, None, :] - cents[None, :, :]) ** 2).sum(axis=2)
    d = dist.min(axis=1)
    print(f"{len(s)} training frames from {len(files)} files, {size} entries, rms error {np.sqrt(d.mean()):.2f} units "
          f"({np.sqrt(d.mean()) * 0.75:.2f} dB); training set {digest}")
    # the residual's DCT per coefficient: what stages 2-4 quantize once stage 1 is this codebook.
    # STEP_AT_2BITS in oc_vox_quant.c (c5..c11) should be about 2.5 x these rms values for a 2-bit
    # mid-rise quantizer to cover them; print them so the retuning (PLAN.md Task 13 step 3) has its numbers
    res = s - cents[dist.argmin(axis=1)]
    n = np.arange(12)
    dct = np.array([[np.cos(np.pi * (2 * j + 1) * k / 24) for j in n] for k in n])
    coef = res @ dct.T
    print("residual DCT rms per coefficient c1..c11 (coefficient units):", " ".join(f"{v:.0f}" for v in np.sqrt((coef ** 2).mean(axis=0))[1:]))
    names = ", ".join(os.path.basename(f) for f in files)
    with open(os.path.join(out_dir, "oc_vox_cb.h"), "w") as fh:
        fh.write(f"""/* The trained envelope codebook (PLAN.md Task 13), written by
 * tools/vox/vox_train_cb.py: do not edit, re-run the script. Stage 1 of the
 * envelope is a {size}-entry vector quantizer of the 12 band values (10 bits,
 * the same width as the structured quantizer it replaces); stages 2-4
 * refine the DCT of the residual, so the layout and the FEC classes do
 * not move. Training set: {names} ({digest}). */
#ifndef OC_VOX_CB_H
#define OC_VOX_CB_H

#include <stdint.h>

#define OC_VOX_CB_TRAINED 1
#define OC_VOX_CB_S1_SIZE {size}u
#define OC_VOX_CB_TRAINING_SET "{digest}"

#if OC_VOX_CB_TRAINED
extern const int8_t oc_vox_cb_s1[OC_VOX_CB_S1_SIZE][12]; /* 0.75 dB units, mean removed */
#endif

#endif
""")
    with open(os.path.join(out_dir, "oc_vox_cb.c"), "w") as fh:
        fh.write(f"/* Written by tools/vox/vox_train_cb.py (training set {digest}): do not edit. */\n")
        fh.write('#include "oc_vox_cb.h"\n\n')
        fh.write(f"const int8_t oc_vox_cb_s1[OC_VOX_CB_S1_SIZE][12] = {{\n")
        for c in cents:
            fh.write("    { " + ", ".join(f"{v:4d}" for v in c) + " },\n")
        fh.write("};\n")
    print("wrote oc_vox_cb.h and oc_vox_cb.c in", out_dir)


if __name__ == "__main__":
    main()
