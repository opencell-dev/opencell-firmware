# OpenVoxNB modes: use, fit, quality and limits

What each mode is for, where it fits in OpenCell, what to expect of it and
what it cannot do, with the numbers measured on the synthetic vectors in
`host-tests/vectors/vox` (formant-synthesised vowels; no recorded speech was
available when this was written, see `PLAN.md` Task 13). Bitstream version 0.

## The modes at a glance

| Mode | bit/s | Block | Fits today's 18-byte app data frame | Codec id | Intended use |
|---|---:|---:|---|---:|---|
| OVX-600 | 600 | 9 B | yes | 3 | the last resort: edge-tier LoRa links, a channel that must carry a voice at all |
| OVX-800 | 800 | 12 B | yes | 4 | poor LoRa links; a step up in onsets and pitch |
| OVX-1000 | 1000 | 15 B | yes | 5 | ordinary LoRa links with headroom for FEC in the link layer |
| OVX-1200 | 1200 | 18 B | yes, exactly | 6 | **the daily driver**: the same payload as Codec2 1200, with unequal error protection inside |
| OVX-1400 | 1400 | 21 B | no (voice spec v2) | - | FLRC / near-tier slots once the frame grows |
| OVX-1600 | 1600 | 24 B | no | - | as above, with envelope detail every frame |
| OVX-1800 | 1800 | 27 B | no | - | natural voicing and speaker identity start here |
| OVX-2400 | 2400 | 36 B | no | - | IP and OCSS links between cores, gateways |
| OVX-3200 | 3200 | 48 B | no | - | the best the parametrisation does: communications quality over IP |

Every mode: 8 kHz mono, 20 ms frames, six to a 120 ms block, 136 ms
algorithmic delay (120 ms block + 10 ms analysis lookahead + the decoder's
in-frame interpolation). All nine are implemented, tested and equivalent
between the reference and the fixed-point codec; the LoRa air interface
carries the first four until voice spec v2 widens the app data frame
(`INTEGRATION.md` §4).

## What each mode spends its bits on

| Mode | Energy | Pitch | Voicing | Envelope | Protection |
|---|---|---|---|---|---|
| 600 | 40 ms, 2-bit deltas | 40 ms, 3-bit deltas | 40 ms, 2-bit class | 40 ms, stage 1 (10 bits) | 1 Golay word: 12 bits (toggle, energy, voicing, pitch MSBs) |
| 800 | 40 ms, 3-bit deltas | 40 ms, 4-bit deltas | 40 ms, class | 40 ms, stage 1 + 6 bits on frames 1 and 5 | 1 Golay + 2 Hamming words |
| 1000 | 20 ms | 20 ms, 3-bit deltas | 40 ms, class | 40 ms, stage 1 + 4-6 bits | 1 Golay + 3 Hamming |
| 1200 | 20 ms | 20 ms, 4-bit deltas | 40 ms, 4-bit mask, mix | 40 ms, stage 1 + 6-7 bits | 2 Golay + 2 Hamming |
| 1400 | 20 ms | 20 ms | mask on odd frames, class on even | stage 1 every frame, stage 2 on odd | 2 Golay + 2 Hamming |
| 1600 | 20 ms | 20 ms | as 1400 | stage 1 + stage 2 every frame | 2 Golay + 2 Hamming |
| 1800 | 20 ms, 4-bit deltas | 20 ms | mask every frame, mix on odd | stage 1 + 6 bits every frame | 2 Golay + 3 Hamming |
| 2400 | 20 ms, 4-bit | 20 ms, 5-bit deltas | mask + mix every frame | stages 1-3 every frame | 3 Golay + 2 Hamming + CRC-8 |
| 3200 | 7-bit absolute, half-frame steps | 8-bit absolute, 5-bit deltas | mask + mix every frame | stages 1-4 every frame | 3 Golay + 4 Hamming + CRC-12 |

The exact layouts are in `BITSTREAM.md`.

## Quality to expect

Measured with `test_vox_ref`, `test_vox_equiv` and `tools/vox/oc-vox` on
`synth_vowels` (6 s of vowels on a 90-250 Hz pitch contour). The
log-spectral distance (LSD) is between the input's own 12-band envelope and
the decoded output's, over voiced frames; it is dominated by the envelope
quantizer (a structured DCT quantizer until Task 13 trains codebooks), not
by the synthesis, whose own round trip is 1.7 dB.

| Mode | Blocks off by > 2.5 dB in loudness | Decoded pitch within 4.8 % of the input | Envelope LSD | What it sounds like (expected) |
|---|---:|---:|---:|---|
| 600 | 8 % | 81 % | 5.4 dB | Intelligible, synthetic; vowels and pitch carry, consonants are coarse; no speaker identity; music becomes a hum. Energy and pitch update every 40 ms, so fast onsets inside a block smear. |
| 800 | 10 % | 90 % | 5.2 dB | As 600 with cleaner onsets and steadier pitch. |
| 1000 | 4 % | 94 % | 5.0 dB | Energy and pitch every 20 ms: plosives and stops come through. |
| 1200 | 2 % | 96 % | 4.8 dB | The reference point: clear speech, a recognisable but synthetic voice; the four-band voicing mask gives fricatives their place. |
| 1400 | 6 % | 95 % | 4.6 dB | Envelope every frame: formant transitions follow. |
| 1600 | 4 % | 95 % | 4.5 dB | As 1400 with more envelope detail. |
| 1800 | 2 % | 96 % | 4.4 dB | Voicing mask every frame, mix on half of them: breathy and mixed sounds improve. |
| 2400 | 2 % | 97 % | 4.2 dB | Three envelope stages: some speaker identity; still a vocoder. |
| 3200 | 2 % | 96 % | 3.1 dB | The parametrisation's best: half-frame energy for crisp onsets, four envelope stages, finest pitch. Communications quality, not toll quality. |

Honest caveats: these are synthetic vowels, which have deeper spectral
valleys than speech and no aspiration noise; real speech will score
differently (probably better in LSD, as its envelopes are smoother, and
worse in pitch, as real pitch is less clean). Nothing here was listened to
on real speech yet. The largest single gain available is a trained envelope
codebook within the same bit counts (Task 13): `tools/vox/vox_train_cb.py`
makes one from recordings and the quantizer switches to it at compile time
(`oc_vox_cb.h`). A trial codebook trained on these very vectors (overfit,
not committed) brought the LSD above to 1.5-1.9 dB in every mode, which is
the order of gain to expect once speech trains it, after which the
refinement steps of stages 2-4 are set from the residual the tool reports.

## Error behaviour

- **Bit errors.** Class A (the toggle, the first frame's energy, voicing and
  pitch, the first envelope's MSBs) survives up to 3 errors in each 24-bit
  Golay word. Where a word fails the block is concealed. At BER 1e-3 in
  OVX-1200 no block was lost in 500 (test_vox_conceal); at 5e-2 about one
  block in fifteen is. Class B (deltas, other frames' voicing) corrects one
  error per 12-bit word. Class C is unprotected but, in 2400 and 3200, CRC
  checked: a failure drops only the fine envelope stages. The interleaver
  makes any burst of up to 3 x depth bits harmless to class A: 27 bits in
  OVX-600, 54 in OVX-1200, 144 in OVX-3200.
- **Lost blocks.** The decoder takes `NULL` for a lost block and conceals in
  the parameter domain: energy held one frame then -3 dB a frame, -6 dB
  after a whole block, silence from the 12th lost frame (240 ms); pitch held;
  voicing fades from the top band down after 3 frames; recovery is capped to
  +6 dB a frame. A lost block in five keeps the loudness within 6 dB and never
  clicks (test_vox_conceal).
- **Unannounced losses.** A receiver without sequence numbers (the app's
  BLE DOWN path) sees the block toggle bit repeat and counts a `toggle_gap`;
  the recovery ramp applies.

## Resources (fixed-point production codec)

| Measure | Value | How |
|---|---|---|
| Encoder state | 7.3 KB (`sizeof(oc_vox_enc_t)` = 7428, pinned <= 8 KB) | `_Static_assert` in `oc_vox.c` |
| Decoder state | 2.3 KB (2320, pinned <= 4 KB) | same |
| Stack, deepest call | encode 1.3 KB (the analysis), decode 1.0 KB (the synthesis) at -O2; the FFT and oscillator scratch live in the state | `-fstack-usage`, Task 12 |
| Code + tables | 27.8 KB text at -O2 on x86-64 (tables: 3.5 KB kernels, 1 KB quantizers, 1 KB layout) | `size` |
| Heap | none | `test_vox_nofloat.sh` |
| Host speed, -O2, OVX-1200 | encode 0.28 ms and decode 0.44 ms per 120 ms block (420x and 280x realtime) | `oc-vox bench 1200 20` |
| Host speed, -O0 (the default host-tests build) | encode 0.84 ms, decode 0.85 ms per block | `oc-vox bench 1200 10` |
| ESP32-S3 estimate | 15-25 % of one core at 240 MHz for encode + decode, by scaling the -O2 host figure 25-40x; the synthesis's 144 oscillators x 160 samples are the hot spot | to be measured on the bench (PLAN.md F4) |

## Where it does not fit yet

- OVX-1400 and above need a bigger app data frame (`OC_SIG_APP_MAX` 18 -> 48)
  and codec ids beyond the 8-bit `codec_caps`: voice spec v2 (`PLAN.md` F1).
- The app decodes Codec2 1200 only until its `:openvox` module lands (F2);
  the network sends codec 1 only until `oc_sig` negotiates (F1).
- No DTX / comfort noise: silence costs the full bitrate (energy index 0
  decodes to exact silence, so a transport may drop such blocks and the
  decoder's concealment fades correctly; that is a transport decision).
