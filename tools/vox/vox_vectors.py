#!/usr/bin/env python3
"""vox_vectors.py [OUT_DIR]: the synthetic test vectors of oc_vox (PLAN.md §2).

Writes, deterministically (fixed seed), 8 kHz mono s16le:
  synth_vowels.raw  6 s of formant-synthesised vowels on a pitch contour
  synth_mixed.raw   6 s of vowels, fricative-like noise bursts and silences
  chirp.raw         2 s sine sweep 100 -> 3000 Hz at -12 dBFS
  silence.raw       1 s of digital silence
and for each a .lab file: one line per 20 ms frame, "f0 kind" with f0 in
Hz (0 when unvoiced) and kind 0 silence, 1 voiced, 2 noise, 3 tone.
Default OUT_DIR: host-tests/vectors/vox next to this script's repository.
Needs numpy only.
"""
import os
import sys

import numpy as np

RATE = 8000
FRAME = 160
VOWELS = {  # F1, F2, F3 (Hz) and bandwidths
    "a": ((730, 1090, 2440), (80, 90, 120)),
    "e": ((530, 1840, 2480), (70, 100, 130)),
    "i": ((270, 2290, 3010), (60, 110, 140)),
    "o": ((570, 840, 2410), (80, 80, 120)),
    "u": ((300, 870, 2240), (60, 80, 120)),
}


def resonator(x, f, bw):
    """Second-order resonator (a formant) applied to x."""
    r = np.exp(-np.pi * bw / RATE)
    a1, a2 = -2 * r * np.cos(2 * np.pi * f / RATE), r * r
    y = np.zeros_like(x)
    y1 = y2 = 0.0
    g = 1 + a1 + a2
    for n in range(len(x)):
        v = g * x[n] - a1 * y1 - a2 * y2
        y[n] = v
        y2, y1 = y1, v
    return y


def glottal_train(f0_per_sample):
    """Pulses of a glottal-ish shape at the running f0 (Hz per sample)."""
    phase = 0.0
    out = np.zeros(len(f0_per_sample))
    pulse = np.exp(-np.arange(24) / 6.0) * np.hanning(48)[24:]  # a decaying pulse
    for n, f in enumerate(f0_per_sample):
        phase += f / RATE
        if phase >= 1.0:
            phase -= 1.0
            end = min(n + len(pulse), len(out))
            out[n:end] += pulse[: end - n]
    return out


def vowel_segment(vowel, f0_curve, rng):
    (formants, bws) = VOWELS[vowel]
    x = glottal_train(f0_curve)
    for f, bw in zip(formants, bws):
        x = resonator(x, f, bw)
    x = resonator(x, 3500, 200) * 0.3 + x  # a little high band
    env = np.ones(len(x))
    att = min(400, len(x) // 4)
    env[:att] = np.linspace(0, 1, att)
    env[-att:] = np.linspace(1, 0, att)
    return x * env


def noise_burst(n, rng):
    x = rng.standard_normal(n)
    # high-pass by differencing twice, then a resonance around 3 kHz
    x = np.diff(x, n=2, prepend=[0, 0])
    x = resonator(x, 3000, 600)
    env = np.ones(n)
    att = min(200, n // 4)
    env[:att] = np.linspace(0, 1, att)
    env[-att:] = np.linspace(1, 0, att)
    return x * env


def normalize(x, peak_db=-12.0):
    m = np.max(np.abs(x))
    return x / m * 32768 * 10 ** (peak_db / 20) if m > 0 else x


def labels(frames_f0, frames_kind):
    return "\n".join(f"{f0:.1f} {k}" for f0, k in zip(frames_f0, frames_kind)) + "\n"


def build(mixed, rng):
    audio, f0s, kinds = [], [], []
    seq = list("aeiouaiueo")
    t_total = 0
    for i, v in enumerate(seq):
        n = 300 * RATE // 1000 if not mixed else 360 * RATE // 1000
        n -= n % FRAME
        # pitch: a glide across the vowel between 90 and 250 Hz, different per vowel
        base = 90 + 160 * (0.5 + 0.5 * np.sin(i * 1.1))
        f0 = base * (1 + 0.12 * np.sin(np.linspace(0, np.pi, n)))
        audio.append(vowel_segment(v, f0, rng))
        for k in range(n // FRAME):
            f0s.append(float(np.mean(f0[k * FRAME:(k + 1) * FRAME])))
            kinds.append(1)
        t_total += n
        if mixed and i % 3 == 1:
            n2 = 160 * RATE // 1000
            audio.append(noise_burst(n2, rng) * 0.5)
            f0s += [0.0] * (n2 // FRAME)
            kinds += [2] * (n2 // FRAME)
        if mixed and i % 3 == 2:
            n3 = 200 * RATE // 1000
            audio.append(np.zeros(n3))
            f0s += [0.0] * (n3 // FRAME)
            kinds += [0] * (n3 // FRAME)
    x = np.concatenate(audio)
    want = 6 * RATE
    if len(x) < want:
        pad = want - len(x)
        x = np.concatenate([x, np.zeros(pad)])
        f0s += [0.0] * (pad // FRAME)
        kinds += [0] * (pad // FRAME)
    x = normalize(x[:want])
    return x, f0s[: want // FRAME], kinds[: want // FRAME]


def write(path, x, f0s, kinds):
    np.clip(np.round(x), -32768, 32767).astype("<i2").tofile(path)
    with open(path[: -len(".raw")] + ".lab", "w") as fh:
        fh.write(labels(f0s, kinds))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "host-tests", "vectors", "vox")
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(20261013)
    x, f, k = build(False, rng)
    write(os.path.join(out, "synth_vowels.raw"), x, f, k)
    x, f, k = build(True, rng)
    write(os.path.join(out, "synth_mixed.raw"), x, f, k)
    n = 2 * RATE
    t = np.arange(n) / RATE
    f_inst = 100 * (30 ** (t / 2.0))  # 100 -> 3000 Hz, exponential
    phase = 2 * np.pi * np.cumsum(f_inst) / RATE
    x = normalize(np.sin(phase))
    frames = n // FRAME
    write(os.path.join(out, "chirp.raw"), x, [0.0] * frames, [3] * frames)
    write(os.path.join(out, "silence.raw"), np.zeros(RATE), [0.0] * (RATE // FRAME), [0] * (RATE // FRAME))
    print("wrote vectors to", out)


if __name__ == "__main__":
    main()
