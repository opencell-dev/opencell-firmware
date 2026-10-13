# OpenVoxNB (`oc_vox`): Implementation Plan

> Tasks use checkbox (`- [ ]`) syntax for tracking. Each task is one commit, red test first where the repository's habit is red-then-green. Read `INTEGRATION.md` first: it says why the codec is a C11 component here and what the rest of OpenCell expects of it.

**Goal:** An original, Apache-2.0, multi-band-excitation-inspired narrowband speech codec, **OpenVoxNB**, as the OpenCell component `oc_vox`: nine modes from 600 to 3200 bit/s, 20 ms analysis frames at 8 kHz, a 120 ms block on the wire (the radio frame), unequal error protection, parameter-domain concealment, a floating-point reference and a fixed-point production implementation proven equivalent on shared vectors, a negotiation layer that maps onto OpenCell's `codec_caps` / `codec` bytes, and tools to encode, decode, measure and train.

**Architecture:**
- **One repository, one component.** `opencell-firmware/firmware/components/oc_vox/`, host tests in `host-tests/`, tools in `tools/vox/`, docs in `docs/openvox/`. The core (submodule) and the app (vendor script) take it later; those are follow-on plans (§Follow-on).
- **Two arithmetic implementations, one bitstream.** `oc_vox_ref_*` (float, host only) is the reference: it defines what the parameters mean and generates the vectors. `oc_vox_*` (fixed point, Q15/Q31) is production, the only thing the board and the app ship. They share the mode tables, bit layout, FEC, quantizer tables, interpolation and concealment, which all work in an **integer parameter domain**; they differ only in analysis and synthesis DSP.
- **Block = 6 frames = 120 ms.** Parameters are estimated every 20 ms; low modes transmit some of them every 40 ms and interpolate; FEC words and the interleaver span the block, so one LoRa frame's burst errors are spread. All nine block sizes are whole bytes and distinct.
- **Not derived from anything.** Implementers do not open `opencell-app/codec2/` (LGPL, and the request forbids derivation) or any AMBE/IMBE/MELPe source or spec while writing `oc_vox`. General, textbook techniques only (sinusoidal/harmonic model, normalized cross-correlation pitch, band energies, DCT, Golay/Hamming codes, DP tracking), combined in this plan's own way. A freedom-to-operate review before a public release is the maintainer's call; this plan cannot promise patent clearance.

**Tech stack:** C11, `-Wall -Wextra -Werror`, Unity 2.6.1 host tests, CMake (ESP-IDF component + host static library), Python 3 with numpy for tools (no runtime dependency), clang for the ARM cross-compile check.

**Where to work:** `opencell-firmware` on branch `claude/openvoxnb-speech-codec-8hcrv3`. Check with `git branch --show-current` before the first edit. Never commit to `main`.

**Validated in this environment:** the firmware host suite configures, builds and passes here (38 tests, gcc 13, CMake 3.28, Ninja). Not available here: ESP-IDF (no `idf.py` build, no board), scipy, speech recordings. Tasks that need them say so and give the maintainer's step.

## Rulings to confirm

1. **C11 library plus a header-only C++ wrapper** (`opencell::voice::OpenVoxNB`), not a C++ codec. (INTEGRATION.md §2, §10.)
2. **Codec ids 3, 4, 5, 6** = OVX-600, 800, 1000, 1200 in the existing 8-bit `codec_caps`; 7 and 8 stay reserved; higher modes get ids in voice spec v2.
3. **The bitstream is frozen only at Task 13** (`OC_VOX_VERSION 1`), after codebooks are trained on real speech. Until then `OC_VOX_VERSION 0` and vectors are regenerated when the allocation or the codebooks change.
4. **Structured quantizers first, trained codebooks second**, same bit counts, so the layout and the FEC classes never move.

## 1. Codec design

### 1.1 Frames, blocks, delay

- Input: 8 kHz mono `int16_t`. Frame `OC_VOX_FRAME_SAMPLES = 160u` (20 ms). Block `OC_VOX_BLOCK_FRAMES = 6u`, `OC_VOX_BLOCK_SAMPLES = 960u` (120 ms), `OC_VOX_BLOCK_MAX_BYTES = 48u`.
- Analysis window: 256 samples (32 ms) Hann, centred on the frame; 256-point real FFT (31.25 Hz bins). The last frame of a block looks 48 samples past the block, so the encoder holds 48 samples of history and the coded block covers `[960n - 48, 960n + 912)`: a constant 6 ms offset. Decoder: 20 ms overlap-add synthesis with 10 ms crossfade. Algorithmic delay about 136 ms, dominated by the 120 ms block the radio imposes anyway.
- Bitrate per mode is exact: block bits = bit/s x 0.12. Frame bits (12...64) are a bookkeeping figure; the wire unit is the block.

### 1.2 Parameters (the integer parameter domain, `oc_vox_pframe_t`)

| Parameter | Range and step | Estimation (reference) |
|---|---|---|
| `e` energy | 6 bits: frame RMS in dBFS, 1.5 dB steps, 0 = -96 dBFS ... 63 = -1.5 dBFS (7 bits = 0.75 dB in OVX-3200) | RMS of the Hann-windowed frame |
| `p` pitch | 7 bits: `f0 = 50 Hz x 2^(p/42)`, 50...406 Hz, 1.66 % steps (8 bits = 0.83 % in OVX-3200) | Coarse: normalized cross-correlation on the 2 kHz-decimated, DC-free signal, lags 5...40. Refine +-1 lag at 8 kHz. Octave check: harmonic-sum score on the FFT magnitude (sum of log magnitude at the first 8 harmonics minus midway between them) for `f0`, `f0/2`, `2 f0`. Then **block-level dynamic programming** over the 6 frames: cost = local score + `|delta log2 f0|` x weight + octave-jump penalty, which the block structure makes free. |
| `v` voicing | 4 bands: 0-600, 600-1200, 1200-2400, 2400-4000 Hz. Full form: 4-bit band mask. Low form: 2-bit class (0 unvoiced, 1 band 0, 2 bands 0-1, 3 all), expanded to a mask | Per band, periodicity = energy at harmonic bins / band energy, from the FFT at the tracked `f0`; a band is voiced above a threshold with hysteresis across frames |
| `vmix` aperiodicity | 2 bits (3 in OVX-3200): how much the voiced bands' harmonics are phase-dispersed | Mean periodicity of the voiced bands, quantized |
| `s[12]` envelope | 12 warped bands, edges 100, 300, 500, 700, 900, 1150, 1450, 1800, 2200, 2650, 3150, 3700, 4000 Hz; log band energy minus the frame mean (which `e` carries), 0.75 dB units | Voiced bands: mean magnitude at the harmonic bins in the band; unvoiced: band RMS. Smoothed across 2 frames |
| `t` toggle | 1 bit per block, alternates every block | For receivers without sequence numbers (the app's `DOWN` path): a repeated value means a block was lost (odd count) |

Envelope quantization, v0 (structured, no training): the 12 log band energies, mean removed, go through a 12-point DCT; **stage 1** (10 bits) codes DCT coefficients 1-4 with a per-coefficient scalar step table (3, 3, 2, 2 bits); **stage 2** (4-8 bits) codes coefficients 5-8; **stage 3** (5-8 bits, high modes) codes the residual of the top 4 bands after reconstruction. Task 13 replaces stages 1 and 2 by trained **two-stage VQ** with the same bit counts (`oc_vox_cb.c`, generated by `tools/vox/vox_train_cb.py`), with no change of layout.

Energy and pitch are coded **absolute on the block's first frame and as deltas on later transmitted frames** (delta tables are non-uniform, e.g. energy 3 bits = {-12, -6, -3, -1.5, 0, +1.5, +3, +6} dB). Frames a mode does not transmit (the 40 ms schedule) are **interpolated** in the parameter domain by the decoder (linear in `e`, `p`, `s`; voicing takes the nearer transmitted frame). The joint voicing/pitch trick (reuse pitch bits in unvoiced frames) is a Task 13 option, not v0.

### 1.3 Bit allocation per 120 ms block (v0)

Bits per block; `E` energy, `P` pitch, `V` voicing (+mix), `S` envelope, `T` toggle. Schedules: "40 ms" = transmitted on frames 0, 2, 4; "20 ms" = all six. FEC: `G` = Golay(24,12) extended (12 parity per 12 class-A bits; corrects 3 errors, detects 4); `H` = Hamming(12,8) (4 parity per 8 class-B bits; corrects 1); `CRC` over classes B and C. Every row sums to the block exactly (checked by script while writing; `test_vox_bits` pins it).

| Mode | Block | E | P | V | S | T | FEC | A / B / C bits |
|---|---:|---|---|---|---|--:|---|---|
| OVX-600 | 72 b, 9 B | 40 ms: 6+2+2 = 10 | 40 ms: 7+3+3 = 13 | 40 ms class: 2x3 = 6 | 40 ms: 10x3 = 30 | 1 | 1 G = 12 | 12 / 0 / 48 |
| OVX-800 | 96 b, 12 B | 40 ms: 6+3+3 = 12 | 40 ms: 7+4+4 = 15 | 40 ms class: 6 | 40 ms: (10+6)+10+(10+6) = 42 | 1 | 1 G + 2 H = 20 | 12 / 16 / 48 |
| OVX-1000 | 120 b, 15 B | 20 ms: 6+5x3 = 21 | 20 ms: 7+5x3 = 22 | 40 ms class: 6 | 40 ms: (10+6)+(10+4)+(10+6) = 46 | 1 | 1 G + 3 H = 24 | 12 / 24 / 60 |
| OVX-1200 | 144 b, 18 B | 20 ms: 21 | 20 ms: 7+5x4 = 27 | 40 ms mask: 4+4+4, mix 2 on f0 = 14 | 40 ms: (10+6)+(10+7)+(10+6) = 49 | 1 | 2 G + 2 H = 32 | 24 / 16 / 72 |
| OVX-1400 | 168 b, 21 B | 20 ms: 21 | 20 ms: 27 | mask 4 on f0,2,4; 2-bit refine on f1,3,5 = 18 | stage 1 20 ms 10x6 = 60; stage 2 40 ms 3x3 = 9 | 1 | 2 G + 2 H = 32 | 24 / 16 / 96 |
| OVX-1600 | 192 b, 24 B | 20 ms: 21 | 20 ms: 27 | 18 | stage 1 60; stage 2 20 ms 6,5,6,5,6,5 = 33 | 1 | 2 G + 2 H = 32 | 24 / 16 / 120 |
| OVX-1800 | 216 b, 27 B | 20 ms: 6+5x4 = 26 | 20 ms: 27 | mask 4x6, mix 2 on f0,2,4 = 30 | 20 ms: 6x(10+6) = 96 | 1 | 2 G + 3 H = 36 | 24 / 24 / 132 |
| OVX-2400 | 288 b, 36 B | 20 ms: 26 | 20 ms: 7+5x5 = 32 | mask+mix 6x(4+2) = 36 | 20 ms: 6x(10+8) + stage 3 6,5,6,5,6,5 = 141 | 1 | 3 G + 2 H + CRC-8 = 52 | 36 / 16 / 184 |
| OVX-3200 | 384 b, 48 B | 20 ms: 7+5x4, plus mid-frame 6x3 = 44 | 20 ms: 8+5x5 = 33 | 6x(4+2), mix 3 on f0 = 37 | 20 ms: 6x(10+8+8) + stage 4 (harmonic residual) 6x8 = 204 | 1 | 3 G + 4 H + CRC-12 = 64 | 36 / 32 / 252 |

Class A (Golay) always holds, in this order of filling: `T`, the first frame's `E` absolute, `P` absolute MSBs (3 of 7 in the one-word modes, all 7 otherwise), the first frame's `V`, then `S` stage-1 MSBs of frame 0. Class B (Hamming) holds `E`/`P` deltas and the other frames' `V`. Class C is the rest of `S` and the fine deltas. A **bit interleaver** writes the protected block row-wise into a `width x depth` matrix (width 8 for OVX-600, 12 for 800...1200, 24 above) and reads it column-wise, so a burst of up to `width` bits lands on distinct code words.

Decoder policy: class A uncorrectable (Golay detects 4 or the toggle contradicts the previous block's trajectory) -> the block is treated as **lost**; class B uncorrectable (Hamming cannot know, so this is "a corrected word plus any CRC failure") -> keep class A, interpolate the rest from the previous block; CRC failure (2400, 3200) -> use class A and B, drop class C (envelope detail and fine deltas fall back to stage 1 and the interpolated trajectory).

### 1.4 Concealment

Parameter domain, shared by both implementations, state in the decoder:

- **Lost block** (`bits == NULL` or class A failed): extrapolate per frame. Energy: hold for the first lost frame, then -3 dB per 20 ms; after 6 lost frames (one block) -6 dB per frame; silence (energy index 0) after 12. Pitch: hold, with a 0.3 % per frame random walk toward the long-term median to avoid a frozen buzz. Voicing: unchanged for 3 frames, then toward unvoiced one band per frame from the top. Envelope: hold, tilting -1 dB per frame above 2 kHz. `vmix`: toward 3 (fully dispersed).
- **Recovery**: the first good block after a loss is crossfaded at the parameter level over 2 frames (energy ramp capped at +6 dB per frame) so a late burst does not click.
- **Bit errors**: as §1.3's decoder policy. Counters in `oc_vox_dec_stats_t`: `golay_fixed`, `golay_failed`, `hamming_fixed`, `crc_failed`, `lost_blocks`, `concealed_frames`, `toggle_gaps`.
- The app's current replay-at-lower-gain concealment stays as a fallback for a decoder that was given nothing; the follow-on app task feeds losses to `oc_vox_decode(d, NULL, pcm)` instead.

### 1.5 Mode negotiation and bitstream container

- `oc_vox_mode_t` { `OC_VOX_600` = 0 ... `OC_VOX_3200` = 8 }, `OC_VOX_MODE_COUNT`. `oc_vox_mode_bps()`, `oc_vox_block_bytes()`, `oc_vox_mode_of_block_bytes()` (distinct sizes: 9, 12, 15, 18, 21, 24, 27, 36, 48).
- `oc_vox_caps_t` = `uint16_t`, bit per mode. `oc_vox_negotiate(local, remote, max_block_bytes, prefer)`: the highest mode both have whose block fits `max_block_bytes` (18 today), or the lowest if `prefer` says robustness; `OC_VOX_MODE_NONE` (-1) if none.
- OpenCell codec ids: `oc_vox_codec_id(mode)` -> 3...6 for the four modes that fit the 18-byte frame, 0 for the others until voice spec v2; `oc_vox_mode_of_codec_id(id)`; `oc_vox_caps_to_sig(caps)` -> the 8-bit `codec_caps` bits for ids 3-6; `oc_vox_caps_of_sig(byte)`.
- Container for files and non-OpenCell links (`.ovx`): one header byte `version (2 bits) | mode (4 bits) | reserved (2)`, then headerless blocks. On OpenCell links there is no header: the call's `codec` byte fixed the mode, and the payload length confirms it.
- `OC_VOX_VERSION` (0 until the freeze): decoders refuse a container of another version; the OpenCell link carries no version, the codec id implies it (a future incompatible change takes a new id).

### 1.6 Fixed point versus reference

- Reference (`ref/`, `oc_vox_ref_*`): `float`, straightforward, readable; the vectors' source of truth.
- Production (`fx/`, `oc_vox_*`): Q15 samples, Q15/Q31 intermediates, `int32_t` MACs, `int64_t` only outside inner loops; 256-point radix-2 real FFT with block floating point; `log2`/`exp2`/`sqrt`/reciprocal by 256-entry tables with linear interpolation; sine LUT (1024 + interpolation) oscillator bank for synthesis; LCG noise with a per-decoder seed (deterministic output, so golden hashes hold on every platform). No `float`/`double` token in `fx/` or the shared files (`test_vox_nofloat.sh` greps).
- **Equivalence criteria** (the "shared test vectors" the request asks for; `test_vox_equiv`):
  1. Shared layers (bits, FEC, quant tables, interpolation, concealment) are the same code: bit-exact by construction, pinned by golden bytes.
  2. Encoders: on every vector and mode, >= 97 % of frames give identical parameter indices; the rest differ by at most one step in `e`, `p` or any `s[i]`, and never in `v`'s band 0.
  3. Decoders, same bitstream in: per-frame log-spectral distance (12 bands) between `oc_vox_ref_decode` and `oc_vox_decode` output <= 1.0 dB mean, <= 3.0 dB at the 99th percentile; frame RMS within 1.0 dB; cross-correlation of the two outputs at lag 0 over each voiced frame >= 0.8 (same phase model, same seed policy).
  4. Fixed-point decoder output: a per-mode **golden hash** (FNV-1a 64 over the PCM) pinned in the test, like `test_hop`'s golden vectors: any change is deliberate and reviewed.
- **Perceptual acceptance** (tools, not pass/fail in `ctest` until speech vectors exist): log-spectral distance of the decoded envelope versus the input's own analysis <= 2.5 dB (OVX-1200), <= 3.5 dB (OVX-600); pitch error on synthetic vowels <= 2 %; STOI (via `pystoi` when installed) reported per mode by `vox_metrics.py`.

### 1.7 Complexity and memory budgets

| Budget | Target | How checked |
|---|---|---|
| Encoder state | <= 8 KB | `_Static_assert(sizeof(oc_vox_enc_t) <= 8192u)` |
| Decoder state | <= 4 KB | `_Static_assert` |
| Stack per call | <= 2 KB | no VLAs, no recursion; `-fstack-usage` read once in Task 12 |
| Tables (flash) | <= 48 KB | `size` of `oc_vox_cb.o` + `oc_vox_quant.o` in Task 12 |
| CPU, OVX-1200 enc+dec | <= 25 % of one ESP32-S3 core at 240 MHz | host `oc-vox bench` (cycles via `clock_gettime`, reported as x realtime); on the board a follow-on |
| Heap | none | grep for `malloc`/`calloc`/`free`/`alloca` in the component is empty (`test_vox_nofloat.sh` checks this too) |

### 1.8 API (C11, `include/oc_vox.h`)

```c
#define OC_VOX_RATE_HZ        8000u
#define OC_VOX_FRAME_SAMPLES  160u   /* 20 ms */
#define OC_VOX_BLOCK_FRAMES   6u     /* 120 ms: one radio frame (OC_FRAME_US) */
#define OC_VOX_BLOCK_SAMPLES  960u
#define OC_VOX_BLOCK_MAX_BYTES 48u   /* OVX-3200 */
#define OC_VOX_VERSION        0u     /* 1 at the bitstream freeze (Task 13) */

typedef enum { OC_VOX_600 = 0, OC_VOX_800, OC_VOX_1000, OC_VOX_1200, OC_VOX_1400, OC_VOX_1600,
               OC_VOX_1800, OC_VOX_2400, OC_VOX_3200, OC_VOX_MODE_COUNT } oc_vox_mode_t;
#define OC_VOX_MODE_NONE (-1)

uint16_t oc_vox_mode_bps(oc_vox_mode_t m);          /* 600 ... 3200; 0 for a bad mode */
uint8_t  oc_vox_block_bytes(oc_vox_mode_t m);       /* 9 ... 48; 0 for a bad mode */
int      oc_vox_mode_of_block_bytes(uint8_t n);     /* the mode, or OC_VOX_MODE_NONE */

typedef struct oc_vox_enc oc_vox_enc_t;   /* complete types in the header: caller-owned, no heap */
typedef struct oc_vox_dec oc_vox_dec_t;

int  oc_vox_enc_init(oc_vox_enc_t *e, oc_vox_mode_t mode);   /* 0, or -1 for a bad mode */
int  oc_vox_dec_init(oc_vox_dec_t *d, oc_vox_mode_t mode);
/* One block: 960 samples in, oc_vox_block_bytes(mode) bytes out (out must have room). */
void oc_vox_encode(oc_vox_enc_t *e, const int16_t pcm[OC_VOX_BLOCK_SAMPLES], uint8_t *out);
/* One block in (NULL: lost, conceal), 960 samples out. Always produces audio. */
void oc_vox_decode(oc_vox_dec_t *d, const uint8_t *block, int16_t pcm[OC_VOX_BLOCK_SAMPLES]);

typedef struct {
    uint32_t blocks, lost_blocks, concealed_frames, toggle_gaps;
    uint32_t golay_fixed, golay_failed, hamming_fixed, crc_failed;
} oc_vox_dec_stats_t;
const oc_vox_dec_stats_t *oc_vox_dec_stats(const oc_vox_dec_t *d);

/* The reference, host only (oc_vox_ref.h): the same shape with the oc_vox_ref_ prefix. */
```

`include/opencell/voice/openvox_nb.hpp` (header-only, C++17 or later):

```cpp
namespace opencell::voice {
enum class Mode : int { Ovx600 = OC_VOX_600, /* ... */ Ovx3200 = OC_VOX_3200 };
class OpenVoxNB {   // one encoder and one decoder of one mode; no heap, no exceptions, no RTTI
public:
    static constexpr std::size_t kBlockSamples = OC_VOX_BLOCK_SAMPLES;
    explicit OpenVoxNB(Mode m) noexcept;              // ok() false for a bad mode
    bool ok() const noexcept;
    std::size_t blockBytes() const noexcept;
    void encode(std::span<const int16_t, kBlockSamples> pcm, std::span<uint8_t> out) noexcept;
    void decode(std::span<const uint8_t> block /* empty: lost */, std::span<int16_t, kBlockSamples> pcm) noexcept;
    const oc_vox_dec_stats_t &stats() const noexcept;
private:
    oc_vox_enc_t enc_; oc_vox_dec_t dec_; bool ok_;
};
}
```
(`std::span` needs C++20; the header falls back to pointer + length overloads under C++17.)

## 2. Test strategy

| Test (Unity unless noted) | Library | Pins |
|---|---|---|
| `test_vox_modes` | `oc_vox` | mode table, block sizes, mode-of-length, codec ids, caps conversion, `oc_vox_negotiate` |
| `test_vox_bits` | `oc_vox` | field widths sum to the block in every mode; writer/reader round trip; golden bytes of one fixed parameter set per mode |
| `test_vox_fec` | `oc_vox` | Golay: every 1-, 2-, 3-bit error pattern on 64 random words is corrected, 4-bit patterns detected; Hamming(12,8) single errors; CRC known answers; interleaver is a permutation; class map covers every bit once |
| `test_vox_quant` | `oc_vox` | quantize/dequantize error bounds for `e`, `p`, `s`; delta tables; interpolation of a 40 ms schedule; concealment trajectories (decay rates, mute point, recovery ramp) |
| `test_vox_ref_dsp` | `oc_vox` | reference FFT vs naive DFT (1e-4), NCC on periodic input, window energy |
| `test_vox_ref` | `oc_vox` | reference analysis on synthetic vowels, noise, chirps, silence; full round trip in all 9 modes: energy, pitch, LSD bounds, determinism |
| `test_vox_conceal` | `oc_vox` | lost blocks, BER sweeps 1e-3...5e-2 with a seeded xorshift, toggle gaps, CRC drop path, stats counters |
| `test_vox_fx_dsp` | `oc_vox` | fixed kernels vs float kernels within tolerances |
| `test_vox_equiv` | `oc_vox` | §1.6 criteria 2-4 on the vectors (`OC_VOX_VECTORS` compile definition, as `OC_NUMBER_VECTORS`) |
| `test_vox_fuzz` | `oc_vox` | 10 000 random blocks of random lengths per mode decode without UB (under ASan/UBSan) and within +-32767 |
| `test_vox_cpp.cpp` | `oc_vox` | the wrapper compiles as C++17 and C++20 and round-trips a block |
| `test_vox_nofloat.sh` | script | no `float`/`double`/`malloc`/`alloca` tokens in production sources; `clang --target=armv7m-none-eabi -mcpu=cortex-m4 -ffreestanding -fsyntax-only` of every production source |
| `test_vox_tool.sh` | script | `oc-vox enc`/`dec`/`info` on a vector; the `.ovx` header; exit codes |

Vectors (`host-tests/vectors/vox/`, generated by `tools/vox/vox_vectors.py`, deterministic, committed, kept under 2 MB in all): `synth_vowels.raw` (a formant-synthesized vowel sequence with a pitch contour, 6 s), `synth_mixed.raw` (vowels, fricative noise bursts, silences, 6 s), `chirp.raw`, `silence.raw`, and from Task 13 the maintainer's `speech_*.raw`. Per vector and mode: `<name>.ovx<mode>` from the reference encoder and `<name>.fx<mode>.fnv` (the fixed decoder's golden hash).

## 3. Tasks

### Task 0: Branch, skeleton, mode table

**Files:** `firmware/components/oc_vox/CMakeLists.txt`, `include/oc_vox.h`, `include/oc_vox_modes.h`, `oc_vox_modes.c`, `oc_vox.c` (init/encode/decode stubs returning silence until Task 9), `host-tests/CMakeLists.txt` (+ `add_subdirectory(../firmware/components/oc_vox oc_vox)`, `oc_test(test_vox_modes oc_vox)`), `host-tests/test_vox_modes.c`, `README.md` (one line in the components list).

- [ ] Step 1: `git branch --show-current` is `claude/openvoxnb-speech-codec-8hcrv3`.
- [ ] Step 2 (red): `test_vox_modes`: 9 modes, `oc_vox_mode_bps` 600...3200, `oc_vox_block_bytes` 9, 12, 15, 18, 21, 24, 27, 36, 48, `oc_vox_mode_of_block_bytes` inverse and `OC_VOX_MODE_NONE` for 0, 10, 49; `oc_vox_codec_id` 3...6 for the first four, 0 above; `oc_vox_mode_of_codec_id(1)` is NONE; `oc_vox_caps_to_sig(all)` is `0x3C`; `oc_vox_negotiate` picks OVX-1200 for full caps at 18 bytes, OVX-3200 at 48, NONE for disjoint caps.
- [ ] Step 3: the component and the table; `_Static_assert` that every block is whole bytes.
- [ ] Step 4: Run: `cmake -S host-tests -B host-tests/build && cmake --build host-tests/build -j 2>&1 | grep -E "warning|error"; (cd host-tests/build && ctest -R vox | tail -3)` Expected: no output from grep; `100% tests passed, 0 tests failed out of 1`.
- [ ] Step 5: Commit: `oc_vox: the OpenVoxNB component skeleton and mode table (codec ids 3-6)`.

### Task 1: Bit layout and bit I/O

**Files:** `include/oc_vox_bits.h`, `oc_vox_bits.c`, `oc_vox_int.h` (`oc_vox_pframe_t`, `oc_vox_block_params_t` = 6 pframes + toggle), `host-tests/test_vox_bits.c`.

- [ ] Step 1 (red): widths per field per mode sum to the block (§1.3 table as data); writer/reader round trip of random values; golden bytes for a fixed parameter set in OVX-600 and OVX-1200 (write the expected bytes by hand from the layout, so the layout, not the code, is the reference).
- [ ] Step 2: MSB-first `oc_vox_bw_t`/`oc_vox_br_t`; a table `oc_vox_layout[mode]` of `{field, frame, bits, class}` rows; `oc_vox_bits_pack(mode, params, out_bits)` / `unpack` operating on the **unprotected** bit string (FEC in Task 2 wraps it). A reader past the end returns 0 and sets a flag, never reads out of bounds.
- [ ] Step 3: Run the Task 0 command. Expected: 2 tests pass, grep silent.
- [ ] Step 4: Commit: `oc_vox: bit layout tables and bit I/O for the nine modes (golden bytes)`.

### Task 2: FEC, classes, interleaver

**Files:** `include/oc_vox_fec.h`, `oc_vox_fec.c`, `host-tests/test_vox_fec.c`.

- [ ] Step 1 (red): Golay(24,12) extended: generator from the standard polynomial `0xC75` (own implementation: syndrome decoding with the 24x12 parity matrix, not a lookup copied from anywhere); exhaustive 1-3 bit patterns corrected on 64 random words (2024 x 64 three-bit patterns run in well under a second); 4-bit patterns flagged, never "corrected" into a wrong word silently (the API returns the error count or -1). Hamming(12,8): all single errors. CRC-8 (`0x2F`) and CRC-12 (`0x80F`) known answers. Interleaver: a bijection for each mode's width. Class map: each of the block's bits appears exactly once in A, B or C.
- [ ] Step 2: `oc_vox_fec_protect(mode, bits_in, block_out)` and `oc_vox_fec_recover(mode, block_in, bits_out, report)`; the class order of §1.3.
- [ ] Step 3: Run; Expected 3 tests pass.
- [ ] Step 4: Commit: `oc_vox: Golay(24,12) and Hamming(12,8) unequal error protection, CRC, block interleaver`.

### Task 3: Quantizers, schedules, interpolation, concealment

**Files:** `oc_vox_quant.c` (+ `oc_vox_quant.h` internal), `oc_vox_conceal.c`, `host-tests/test_vox_quant.c`.

- [ ] Step 1 (red): `e` 1.5 dB grid, `p` log grid (`oc_vox_pitch_hz_x16(p)` fixed point, and the float helper for the reference), delta tables, voicing class <-> mask, the DCT envelope quantizer (stages 1-3) with error bounds on random smooth envelopes; the 40 ms schedule interpolates frames 1, 3, 5 linearly; concealment: a lost block decays energy as §1.4, mutes at frame 12, recovery ramp caps +6 dB per frame.
- [ ] Step 2: implement; everything integer (`int32_t` Q-formats documented in `oc_vox_int.h`), DCT by a 12x12 `int16_t` Q14 matrix.
- [ ] Step 3: Run; Expected 4 tests pass.
- [ ] Step 4: Commit: `oc_vox: parameter quantizers, update schedules, interpolation and parameter-domain concealment`.

### Task 4: Reference DSP kernels

**Files:** `ref/oc_vox_ref_dsp.c`, `oc_vox_ref_int.h`, `host-tests/test_vox_ref_dsp.c`; the host branch of `CMakeLists.txt` adds `ref/` and `m`.

- [ ] Step 1 (red): own radix-2 256-point real FFT against a naive DFT (max abs error < 1e-4 on random input), Hann window sums, NCC peak at the period of a 100 Hz pulse train, decimation by 4 with a 7-tap half-band pair.
- [ ] Step 2: implement in `float` only.
- [ ] Step 3: Run; Expected 5 tests pass.
- [ ] Step 4: Commit: `oc_vox: floating-point reference DSP kernels (FFT, window, NCC, decimation)`.

### Task 5: Reference analysis

**Files:** `ref/oc_vox_ref_analysis.c`, `host-tests/test_vox_ref.c` (first half), `tools/vox/vox_vectors.py` (synthetic signal generator; numpy only), `host-tests/vectors/vox/synth_*.raw`.

- [ ] Step 1: `vox_vectors.py` writes the synthetic vectors (formant synthesis: an impulse train at a contour 90-250 Hz through 3 resonators per vowel; noise bursts shaped by a high-pass; silences; a chirp); deterministic seed; committed.
- [ ] Step 2 (red): on `synth_vowels.raw` the tracked pitch is within 2 % of the generator's contour on >= 95 % of voiced frames with no octave errors across the block DP; noise bursts are unvoiced in bands 1-3; silence hits energy index <= 2; the envelope's band 0-3 ordering follows the vowel's formants.
- [ ] Step 3: implement §1.2's estimation: energy, coarse and refined pitch, harmonic-sum octave check, block DP, band periodicity with hysteresis, warped-band envelope.
- [ ] Step 4: Run; Expected 6 tests pass.
- [ ] Step 5: Commit: `oc_vox: reference analysis: energy, block-tracked pitch, band voicing, warped-band envelope`.

### Task 6: Reference synthesis and the reference codec

**Files:** `ref/oc_vox_ref_synth.c`, `oc_vox_ref.c`, `include/oc_vox_ref.h`, `host-tests/test_vox_ref.c` (second half).

- [ ] Step 1 (red): full round trip `oc_vox_ref_encode` -> `oc_vox_ref_decode` in all 9 modes on `synth_vowels.raw` and `synth_mixed.raw`: after 2 blocks of start-up, block RMS within 2 dB of the input; re-analysed pitch within 3 % on voiced frames; 12-band LSD <= 3.5 dB (600), <= 2.5 dB (1200), <= 2.0 dB (3200); silence decodes below -70 dBFS; the same input gives the same bytes twice; a block's byte count is `oc_vox_block_bytes`.
- [ ] Step 2: synthesis: harmonic oscillator bank at the interpolated `f0`, amplitudes from the envelope sampled at harmonic frequencies and scaled by `e`; voiced bands keep continuous phase plus a fixed per-harmonic dispersion table scaled by `vmix`; unvoiced bands re-randomize phase every frame and jitter frequency by up to a quarter harmonic spacing; 20 ms overlap-add with a 10 ms raised-cosine crossfade; a light post-filter (band gains to the power 1.15 around the smoothed envelope, renormalized to `e`). The reference encoder/decoder glue uses the shared bits/FEC/quant/conceal.
- [ ] Step 3: Run; Expected 6 tests pass (the file grew).
- [ ] Step 4: Commit: `oc_vox: reference synthesis and the complete floating-point reference codec (all nine modes)`.

### Task 7: Concealment and bit-error behaviour in the reference decoder

**Files:** `oc_vox_ref.c`, `oc_vox_conceal.c`, `host-tests/test_vox_conceal.c`.

- [ ] Step 1 (red): every 5th block lost (`NULL`) still gives block RMS within 6 dB of the clean decode and no sample jumps > 8000 between adjacent frames; 12 lost blocks fade to silence; BER 1e-3 in OVX-1200 gives `golay_failed` = 0 over 500 blocks and output within 3 dB RMS of clean; BER 2e-2 keeps the output bounded and `lost_blocks` > 0; toggle repeated -> `toggle_gaps` increments and a concealed block is inserted; in OVX-2400 a flipped class-C bit sets `crc_failed` and keeps RMS within 2 dB.
- [ ] Step 2: wire §1.3's decoder policy and §1.4's trajectories; stats.
- [ ] Step 3: Run; Expected 7 tests pass.
- [ ] Step 4: Commit: `oc_vox: lost-block and bit-error concealment, decoder statistics`.

### Task 8: Fixed-point kernels

**Files:** `fx/oc_vox_fx_dsp.c`, `oc_vox_fx_int.h`, `host-tests/test_vox_fx_dsp.c`.

- [ ] Step 1 (red): Q15 256-point real FFT with block floating point vs the float FFT: magnitude error <= 0.1 dB above -60 dBFS; `log2`/`exp2` tables within 0.02 of the float value in Q8; `sqrt` and reciprocal within 1 LSB of Q15; sine LUT within 1e-4 of `sinf`; NCC on Q15 input within 0.01 of the float NCC; the LCG's first 8 outputs pinned.
- [ ] Step 2: implement; no `float` token; `int64_t` only in the block-floating-point scaling decision.
- [ ] Step 3: Run; Expected 8 tests pass; `grep -nE "\b(float|double)\b" firmware/components/oc_vox/fx/*.c` prints nothing.
- [ ] Step 4: Commit: `oc_vox: fixed-point DSP kernels (Q15 FFT, log/exp/sqrt tables, oscillator LUT)`.

### Task 9: Fixed-point analysis, synthesis and the production codec

**Files:** `fx/oc_vox_fx_analysis.c`, `fx/oc_vox_fx_synth.c`, `oc_vox.c` (replacing the stubs), `host-tests/test_vox_equiv.c`, `host-tests/vectors/vox/*.ovx*`, `*.fnv`, `host-tests/CMakeLists.txt` (`OC_VOX_VECTORS` definition).

- [ ] Step 1 (red): §1.6 criteria 2 (index agreement), 3 (LSD, RMS, correlation) and 4 (golden FNV-1a of the fixed decoder's output per vector and mode: write the test with the hash computed by the new `oc-vox` after Step 2, then pin).
- [ ] Step 2: port analysis and synthesis to fixed point against the reference, one function at a time, checking the equivalence test after each.
- [ ] Step 3: Run; Expected 9 tests pass. Also: `_Static_assert` sizes hold (§1.7).
- [ ] Step 4: Commit: `oc_vox: fixed-point production encoder and decoder, equivalent to the reference on the shared vectors`.

### Task 10: Tools

**Files:** `tools/vox/oc-vox.c`, `tools/vox/CMakeLists.txt` (standalone and from host-tests, like `tools/ocbench`), `tools/vox/vox_metrics.py`, `host-tests/test_vox_tool.sh`, `host-tests/CMakeLists.txt` (`add_test(NAME test_vox_tool COMMAND bash ...)`).

- [ ] Step 1: `oc-vox enc MODE in.raw out.ovx [--ref]`, `oc-vox dec in.ovx out.raw [--ref] [--lose N] [--ber P --seed S]`, `oc-vox info in.ovx`, `oc-vox bench MODE [seconds]` (x realtime for enc and dec, host), `oc-vox hash in.raw`. Modes are given as `600`...`3200`. Errors to stderr, exit 1; `.raw` is s16le 8 kHz as `oc-clip-gen` writes it.
- [ ] Step 2: `vox_metrics.py in.raw out.raw` prints block RMS difference, 12-band LSD, pitch agreement, and STOI when `pystoi` is importable (otherwise says so and skips).
- [ ] Step 3 (red then green): `test_vox_tool.sh` encodes and decodes a vector in two modes, checks the header byte, the block count, a refused mode, and that `--lose` changes the hash but not the length.
- [ ] Step 4: Run the full host suite; Expected all pass.
- [ ] Step 5: Commit: `tools/vox: oc-vox (enc, dec, info, bench, hash) and vox_metrics.py`.

### Task 11: Negotiation helpers, the C++ wrapper, format docs

**Files:** `oc_vox_modes.c` (negotiation already in Task 0: this task completes `oc_vox_caps_*` edge cases), `include/opencell/voice/openvox_nb.hpp`, `host-tests/test_vox_cpp.cpp`, `host-tests/CMakeLists.txt` (`project(oc_host_tests C CXX)`, `set(CMAKE_CXX_STANDARD 20)`, the one C++ test), `docs/openvox/BITSTREAM.md`, `docs/openvox/MODES.md`.

- [ ] Step 1 (red): `test_vox_cpp.cpp` constructs `opencell::voice::OpenVoxNB{Mode::Ovx1200}`, encodes and decodes a block of a sine, checks `blockBytes() == 18`, decodes an empty span as a loss and sees `stats().lost_blocks == 1`; compiled with `-fno-exceptions -fno-rtti`.
- [ ] Step 2: the header (C++17 pointer overloads, C++20 `std::span` overloads under `__cpp_lib_span`).
- [ ] Step 3: `BITSTREAM.md`: §1.3's tables as the normative layout, the class order, the interleaver, the container byte, the version rule. `MODES.md`: per mode the use case, the link it fits (LoRa 18 B frame: 600-1200; IP/OCSS and future wider frames: 1400-3200), the quality to expect and its limits (600: intelligible, synthetic timbre, no speaker identity, music is noise; 800-1000: better onsets and pitch; 1200: the daily driver; 1400-1800: natural voicing, mild speaker identity; 2400-3200: near-communications quality, tonal music still poor), and the error behaviour (BER at which class A starts failing, loss handling).
- [ ] Step 4: Run; Expected all tests pass, including the C++ one.
- [ ] Step 5: Commit: `oc_vox: C++ wrapper opencell::voice::OpenVoxNB, negotiation edge cases, BITSTREAM.md and MODES.md`.

### Task 12: Robustness, portability, budgets

**Files:** `host-tests/test_vox_fuzz.c`, `host-tests/test_vox_nofloat.sh`, `host-tests/CMakeLists.txt`.

- [ ] Step 1 (red): fuzz: for each mode, 10 000 blocks of random bytes (xorshift, fixed seed) and random lengths 0...64 passed through `oc_vox_mode_of_block_bytes` and `oc_vox_decode` (lengths that are not the mode's are treated as lost): never crashes, every sample within range, stats counters never exceed `blocks`.
- [ ] Step 2: `test_vox_nofloat.sh`: greps production sources for `float`, `double`, `malloc`, `calloc`, `realloc`, `free(`, `alloca`, `va_`; runs `clang --target=armv7m-none-eabi -mcpu=cortex-m4 -mthumb -ffreestanding -std=c11 -Wall -Wextra -Werror -fsyntax-only` on each production source (skipped with exit 77 when clang is absent, like `test_oc_clip_gen`).
- [ ] Step 3: the sanitizer run: `cmake -S host-tests -B host-tests/build/asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build host-tests/build/asan -j && (cd host-tests/build/asan && ctest -R vox)` Expected: all pass, no sanitizer report.
- [ ] Step 4: budgets: `-fstack-usage` once on `fx/` and `oc_vox.c` (max frame <= 2 KB), `size` of the table objects (<= 48 KB), `oc-vox bench 1200` noted in `MODES.md` for the host as a reference point.
- [ ] Step 5: Commit: `oc_vox: decoder fuzzing, no-float/no-heap and ARM cross-compile checks, budgets`.

### Task 13: Tuning, trained codebooks, bitstream freeze (needs speech)

**Files:** `tools/vox/vox_train_cb.py`, `oc_vox_cb.c` (generated, with a header line naming the script, the input set and its SHA-256, as `README.opencell` does for Codec2), `oc_vox_quant.c`, `host-tests/vectors/vox/speech_*.raw` (the maintainer's), regenerated `.ovx*` / `.fnv`, `include/oc_vox.h` (`OC_VOX_VERSION 1`), `docs/openvox/MODES.md`.

- [ ] Step 1 (maintainer): a few minutes of 8 kHz mono s16le speech, several speakers, in `host-tests/vectors/vox/speech_*.raw` (own recordings, or a permissively licensed corpus resampled with `vox_vectors.py --import`). Keep the committed total under 2 MB; a larger training set can live outside the repository and be named by hash in `oc_vox_cb.c`.
- [ ] Step 2: `vox_train_cb.py`: runs the reference analysis (through `oc-vox analyse`, a tool subcommand added here that dumps parameter frames) over the training set, trains the stage-1 (1024 entries) and stage-2 (16-256 entries by mode) envelope codebooks by k-means with a fixed seed, writes `oc_vox_cb.c`. The structured DCT quantizer stays as the fallback when `OC_VOX_CB_TRAINED` is 0, so the build never depends on the training set.
- [ ] Step 3: listening and metrics on the speech vectors in every mode (`oc-vox enc/dec`, `vox_metrics.py`); adjust thresholds (voicing hysteresis, DP weights, post-filter power, concealment decay) and, if a mode needs it, move bits between fields **within the same class totals** (so the FEC and the layout tables change together, pinned by `test_vox_bits`). Record the decisions and the metrics table in `MODES.md`.
- [ ] Step 4: `OC_VOX_VERSION 1`; regenerate vectors and hashes; `test_vox_bits` golden bytes updated deliberately with a note in the commit.
- [ ] Step 5: the full suite clean and under the sanitizers (Task 12 Step 3's commands, all tests); `README.md`'s component list names `oc_vox`.
- [ ] Step 6: Commit: `oc_vox: trained envelope codebooks, tuning on speech, bitstream v1 freeze`.
- [ ] Step 7 (maintainer, on the bench machine): `idf.py -C firmware build` compiles the component for the ESP32-S3; note the `.text`/`.rodata` delta in `MODES.md`.

## 4. Follow-on plans (separate branches, after `oc_vox` merges to firmware `main`)

| Plan | Repository | What |
|---|---|---|
| **F1 voice spec v2** | `opencell-firmware` (`oc_sig`, `oc_term`), the `opencell` docs repository | The app tells the terminal its codec caps (GATT contract v5: `COMMAND 0x08 SET_CODEC_CAPS` + 1 byte, default `0x01` for old apps); `oc_sig_term` sends them in `CALL_SETUP.codec_caps`; `oc_sig_net` / the core pick `CONNECT.codec` as the highest common id the operator allows (cell/core config `voice_codec_max`), Codec2 1200 otherwise; the CONNECTED event already carries the codec. Later in the same spec: a wider app data frame (`OC_SIG_APP_MAX` 18 -> 48, `OC_SIG_LINK_MAX`, MEDIA, `OC_CORE_PLAY_BYTES`, GATT `MAX_PAYLOAD`) for OVX-1400...3200 and their ids. |
| **F2 app** | `opencell-app` | `tools/openvox/vendor.sh` (pinned firmware commit, copies `oc_vox` production sources only), `:openvox` module (NDK CMake building `libopencell_openvox.so` = codec + JNI), `OpenVox : VoiceCodec` with `frameSamples = 960`, `frameBytes = oc_vox_block_bytes`, `CodecId` 3-6 and labels, a composite factory (`Codec2` + `OpenVox`), `BlockCodec` frames = `960 / frameSamples`, `JitterBuffer`/`VoiceSession` hand a `Conceal` to `decode(null)` for codecs that conceal, developer-options show `oc_vox_dec_stats_t`. JVM tests through the host build like `Codec2Test`. |
| **F3 core** | `opencell-core` | Submodule pin; `oc-clip-gen` gains `--codec ovx1200` (calls `oc-vox enc`); `oc_core_clip_ok` and the playback service take the clip's codec from its config and offer it in the service legs' `CONNECT`; `oc-core admin status` shows it. |
| **F4 bench** | `opencell-firmware` | An `ocbench` or bs-radio console step that times `oc_vox_encode`/`decode` on the W12 with `esp_timer` and prints cycles; the IRAM check must stay green (the codec is not on the turnaround path). |

## 5. Risks and how the plan meets them

| Risk | Mitigation |
|---|---|
| OVX-600 is not intelligible enough with structured quantizers | Task 13 trains codebooks within the same bit counts; `MODES.md` states the quality honestly; the 40 ms schedule and the block DP for pitch are chosen for robustness over fidelity |
| Pitch octave errors (the usual failure of harmonic codecs) | Harmonic-sum check plus block-level DP with an octave penalty; measured against the generator's contour in Task 5 |
| Fixed-point drift from the reference | Task 9 ports function by function against `test_vox_equiv`; golden hashes pin the result |
| No ESP-IDF toolchain here | The clang ARM syntax check in Task 12; the maintainer's `idf.py build` in Task 13 Step 7; the component has no IDF-specific code |
| No speech corpus here | Synthetic vectors carry Tasks 5-12; Task 13 is explicitly gated on the maintainer's speech |
| Patents in the MBE family | Original parameterization and coding; no DVSI material consulted; the maintainer decides on a freedom-to-operate review before release |
| Higher modes unusable over LoRa today | Stated in `INTEGRATION.md` and `MODES.md`; voice spec v2 (F1) is the path; the codec is complete now |
