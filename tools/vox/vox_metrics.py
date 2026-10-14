#!/usr/bin/env python3
"""vox_metrics.py IN.raw OUT.raw [--delay 80]: how a decoded file compares with its input.

Both files 8 kHz mono s16le. OUT is the codec's output, which lags IN by
the analysis delay (80 samples, --delay). Prints: the block (120 ms)
loudness difference (mean and the share of blocks off by more than 2.5
dB), the 12-band log-spectral distance per 20 ms frame (mean, 95th
percentile) over frames above -50 dBFS, and STOI when the pystoi
package is importable (it is optional; pip install pystoi). Needs numpy.
"""
import sys

import numpy as np

RATE = 8000
EDGES = [100, 300, 500, 700, 900, 1150, 1450, 1800, 2200, 2650, 3150, 3700, 4000]


def band_env(x):
    """12 band log energies (dB), mean removed, per 20 ms frame of x."""
    n = len(x) // 160
    win = np.hanning(256)
    out = np.zeros((n, 12))
    bins = np.fft.rfftfreq(256, 1 / RATE)
    for i in range(n):
        c = i * 160 + 80
        seg = np.zeros(256)
        lo, hi = max(0, c - 128), min(len(x), c + 128)
        seg[lo - (c - 128): lo - (c - 128) + hi - lo] = x[lo:hi]
        p = np.abs(np.fft.rfft(seg * win)) ** 2
        env = np.array([10 * np.log10(p[(bins >= a) & (bins < b)].mean() + 1e-9) for a, b in zip(EDGES[:-1], EDGES[1:])])
        env = np.maximum(env, env.max() - 60)
        out[i] = env - env.mean()
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    delay = 80
    if "--delay" in sys.argv:
        delay = int(sys.argv[sys.argv.index("--delay") + 1])
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)
    x = np.fromfile(args[0], dtype="<i2").astype(float)
    y = np.fromfile(args[1], dtype="<i2").astype(float)
    y = y[delay:]
    n = min(len(x), len(y)) // 960 * 960
    x, y = x[:n], y[:n]
    bx = np.sqrt((x.reshape(-1, 960) ** 2).mean(axis=1)) + 1
    by = np.sqrt((y.reshape(-1, 960) ** 2).mean(axis=1)) + 1
    loud = bx > 32768 * 10 ** (-50 / 20)
    d = 20 * np.log10(by[loud] / bx[loud])
    print(f"blocks: {n // 960}, loud: {loud.sum()}; loudness difference mean {d.mean():+.2f} dB, "
          f"off by more than 2.5 dB: {100 * (np.abs(d) > 2.5).mean():.1f} %")
    ex, ey = band_env(x), band_env(y)
    fx = np.sqrt((x.reshape(-1, 160) ** 2).mean(axis=1))
    keep = fx > 32768 * 10 ** (-50 / 20)
    lsd = np.sqrt(((ex - ey) ** 2).mean(axis=1))[keep]
    print(f"log-spectral distance (12 bands, frames above -50 dBFS): mean {lsd.mean():.2f} dB, 95th percentile {np.percentile(lsd, 95):.2f} dB")
    try:
        from pystoi import stoi
        print(f"STOI: {stoi(x, y, RATE, extended=False):.3f}")
    except ImportError:
        print("STOI: pystoi is not installed (pip install pystoi), skipped")


if __name__ == "__main__":
    main()
