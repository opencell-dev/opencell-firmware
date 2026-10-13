# OpenVoxNB in OpenCell: integration findings and decisions

What the four OpenCell repositories look like today, what that means for a
new narrowband speech codec, and the decisions this plan takes. Read with
`PLAN.md` (the codec design and the task list). Inspected 2026-10-13 at
`opencell-firmware` c2dd3a3, `opencell-core` 54044f4, `opencell-app` 7e67aa6,
`opencell-portal` 88bbdd8.

## 0. Decisions at a glance

| Question | Finding | Decision |
|---|---|---|
| Language | OpenCell is **C11**, not C++. 250 C/H files; the one `.cpp` is RadioLib glue (`oc_radio.cpp`). The app is Kotlin with C JNI. | The codec is a **C11 library, `oc_vox`**, like `oc_sig`, `oc_air`, `oc_phy`. A **header-only C++ wrapper** `opencell/voice/openvox_nb.hpp` gives C++ consumers `opencell::voice::OpenVoxNB` at no cost to the C builds. |
| Home | Portable libraries live in **`opencell-firmware/firmware/components/`** and reach the core by git submodule and the app by a vendoring script. | **`firmware/components/oc_vox/`**, dual CMake (ESP-IDF component / host static library), host tests in `host-tests/`. |
| Build | CMake everywhere (ESP-IDF's `idf_component_register` on the board, plain `add_library` on the host); Gradle + NDK CMake in the app. | No new build system. One `CMakeLists.txt` with the `if(ESP_PLATFORM)` split every component uses. |
| Flags | `-Wall -Wextra -Werror`, `CMAKE_C_STANDARD 11`. | Same. No warning is silenced. |
| Memory | Portable libraries never `malloc`; state is a caller-owned struct with fixed arrays; tables are `const`. | Caller-owned `oc_vox_enc_t` / `oc_vox_dec_t`, no heap, no VLAs, no recursion, no `float` in the production path. |
| Errors | `0` / `-1` ints, small `OC_*_E_*` enums, out-parameters; no exceptions anywhere. | Same. |
| Logging | Portable libraries take an optional `io.log` callback or are silent; daemons use `oc_log`; firmware uses `ESP_LOG`. | The codec is silent. It returns counters (`oc_vox_dec_stats_t`); tools and roles log them. |
| Tests | Unity 2.6.1 by `FetchContent`, one executable per `host-tests/test_<name>.c`, `oc_test(name lib)`, vectors in `host-tests/vectors/`, golden-byte tests pin wire formats, ASan/UBSan runs before merge. | `test_vox_*.c` Unity tests, `host-tests/vectors/vox/`, golden bitstreams and golden fixed-point PCM hashes. |
| CI | None in any repository: `ctest` locally, by hand, before merging a branch. | Same; a `ctest` label for the slow vector tests. |
| Library form | Static, in-tree. The app builds vendored C into its own `.so` for JNI. | In-tree static library; the app gets an `:openvox` module via `tools/openvox/vendor.sh` (follow-on). |
| Radio frame | **120 ms** (`OC_FRAME_US`), one app data frame of at most **18 bytes** (`OC_SIG_APP_MAX`) each way per frame; the core's MEDIA carries the same 18 bytes. | The codec's unit on the wire is a **120 ms block of six 20 ms frames**. OVX-600...OVX-1200 (9, 12, 15, 18 bytes) fit today's frame. OVX-1400...OVX-3200 (21-48 bytes) need the follow-on voice spec v2 (bigger app data frame). |
| Codec ids | `codec_caps` is an 8-bit bitmap (bit id-1), `codec` one byte; 1 = Codec2 1200, 2 reserved. | Proposed: 3 = OVX-600, 4 = OVX-800, 5 = OVX-1000, 6 = OVX-1200; 7, 8 reserved; the higher modes get ids when caps widen (voice spec v2). |

## 1. Build system

CMake, in three flavours that the codec must satisfy with one `CMakeLists.txt`:

- **ESP-IDF 6.0.1** (`firmware/CMakeLists.txt`, `project(opencell_w12)`): every component under `firmware/components/<name>/` registers with `idf_component_register(SRCS ... INCLUDE_DIRS "include" REQUIRES ...)`. Target ESP32-S3 (`sdkconfig.defaults`), 240 MHz, FreeRTOS at 1 kHz tick. The whole project is `-Werror` (ESP-IDF's default), with `-Wno-error=cpp` for RadioLib's `#warning`.
- **Host** (`host-tests/CMakeLists.txt`, `project(oc_host_tests C)`, CMake >= 3.16): `add_subdirectory(../firmware/components/<name> <name>)`; each component's `else()` branch is `add_library(<name> STATIC ...)` + `target_include_directories(... PUBLIC include)` + `target_link_libraries`. `tools/ocbench/CMakeLists.txt` does the same standalone.
- **Consumers**: `opencell-core/CMakeLists.txt` adds `oc_phy`, `oc_link`, `oc_sig` from the submodule `third_party/opencell-firmware` and builds them under its own flags (`project(opencell_core C)`, C11, `-Wall -Wextra -Werror`). `opencell-app/codec2/src/main/cpp/CMakeLists.txt` (C99, NDK r27, CMake 3.22) builds vendored C sources into `libcodec2.so` plus a JNI `.so`, for `arm64-v8a` and `x86_64`, and the same CMake runs on the host for the JVM unit tests (`hostCodec2` Gradle task).

So the codec's CMake is the component pattern:

```cmake
set(OC_VOX_SRCS oc_vox_modes.c oc_vox_bits.c oc_vox_fec.c oc_vox_quant.c oc_vox_conceal.c
                oc_vox_cb.c fx/oc_vox_fx_dsp.c fx/oc_vox_fx_analysis.c fx/oc_vox_fx_synth.c oc_vox.c)
if(ESP_PLATFORM)
  idf_component_register(SRCS ${OC_VOX_SRCS} INCLUDE_DIRS "include")
else()
  add_library(oc_vox STATIC ${OC_VOX_SRCS} ref/oc_vox_ref_dsp.c ref/oc_vox_ref_analysis.c ref/oc_vox_ref_synth.c oc_vox_ref.c)
  target_include_directories(oc_vox PUBLIC include)
  target_link_libraries(oc_vox PUBLIC m)
endif()
```

The floating-point reference (`ref/`, `oc_vox_ref_*` symbols) is host-only; the board gets the fixed-point production code only. The component has **no `REQUIRES`**: it depends on nothing in OpenCell, so the core and the app can take it alone.

## 2. C standard and compiler flags

C11 (`CMAKE_C_STANDARD 11`, `CMAKE_C_STANDARD_REQUIRED ON`) with `-Wall -Wextra -Werror` on the host and in the core; ESP-IDF's Xtensa GCC toolchain with its own `-Werror` set on the board; the app's codec CMake says C99 but is built by clang (NDK) with no `-Werror`. The codec is written to the strictest of these: C11, no GNU extensions beyond `__attribute__` already used in `oc_log.h`, no `_Generic`, `_Static_assert` where sizes matter (used in `oc_hop.c`, `oc_term_sig.c`). Fixed-width types from `<stdint.h>`, `size_t` for byte counts, `uint8_t` payload lengths as in `oc_air` and `oc_sig`.

The request asked for a C++20 API "unless the repository uses an older standard". It uses C11, so the API is C11. The wrapper header is C++17-compatible (and therefore C++20-compatible) and is only compiled by a C++ consumer; the host-tests project gains `CXX` as a language for one test that compiles it (`test_vox_cpp.cpp`), the only convention change this plan makes.

## 3. Namespace, module and layout

Naming in OpenCell is **prefix-based C**: component `oc_<name>`, symbols `oc_<name>_*`, types `oc_<name>_<thing>_t`, constants `OC_<NAME>_*` with unsigned literals (`18u`), header guards `OC_<NAME>_H`, one public header per concern in `include/`. Comments are `/* */` prose, often citing a spec section or a review finding (`review M3`). Internal headers are `oc_<name>_int.h`.

OpenVoxNB therefore is **component `oc_vox`**, symbols `oc_vox_*`, and the user-facing name "OpenVoxNB" appears in docs, the C++ class and the codec-id labels. Layout (the request's `voice/` tree, folded into the component convention: flat where components are flat, two subdirectories for the two arithmetic implementations):

```
firmware/components/oc_vox/
  CMakeLists.txt
  include/oc_vox.h                      the API: modes, block encode/decode, stats
  include/oc_vox_modes.h                mode table, codec ids, caps and negotiation
  include/oc_vox_bits.h                 bit writer/reader, per-mode field layout
  include/oc_vox_fec.h                  Golay(24,12), Hamming(12,8), CRC, interleaver
  include/opencell/voice/openvox_nb.hpp header-only C++ wrapper (opencell::voice::OpenVoxNB)
  oc_vox_int.h                          shared internal types: the parameter frame
  oc_vox_modes.c oc_vox_bits.c oc_vox_fec.c oc_vox_quant.c oc_vox_cb.c oc_vox_conceal.c
  oc_vox.c                              production (fixed-point) encoder/decoder glue
  fx/oc_vox_fx_dsp.c fx/oc_vox_fx_analysis.c fx/oc_vox_fx_synth.c
  oc_vox_ref.c                          reference (float) glue, host only
  ref/oc_vox_ref_dsp.c ref/oc_vox_ref_analysis.c ref/oc_vox_ref_synth.c
host-tests/test_vox_*.c, host-tests/test_vox_cpp.cpp, host-tests/vectors/vox/
tools/vox/oc-vox (C CLI), tools/vox/*.py (metrics, codebook training, vector generation)
docs/openvox/ (this file, PLAN.md, later BITSTREAM.md and MODES.md)
```

## 4. Existing interfaces the codec meets

The voice path end to end, as the code has it today (Codec2 1200 only):

1. **Phone app** (`opencell-app`, Kotlin): `VoiceSession` captures 8 kHz mono PCM, `BlockCodec` encodes three 40 ms Codec2 frames into one **18-byte block per 120 ms**, writes it to the terminal's BLE `UP` characteristic (`GattContract.MAX_PAYLOAD = 18`, `FRAME_MILLIS = 120`), drops the block on ATT 0x80 rather than retrying. Downlink: `DOWN` notifications of the same size go into `JitterBuffer` (target 2 blocks, max 6, at most 2 concealed in a row, concealment = replay the last block at -6 dB then -12 dB; **no sequence numbers reach the app**, losses are inferred from arrival times). `VoiceCodec` is the codec interface (`frameSamples`, `frameBytes`, `encode`, `decode`, `close`); `VoiceCodecFactory.create(codecId)` returns null for an unsupported id and the session shows `VoiceState.Unsupported`. `CodecId` mirrors the signalling's codec byte.
2. **Terminal firmware** (`oc_term`, `oc_sig_term`): wraps the app data frame as a link payload (`OC_SIG_LINK_MAX = 20`: kind/seq header + 18 bytes), encrypts it with the call's `K_voice` in Part 15 mode (`oc_sig_term_data_out`), sends one UL payload per granted slot per 120 ms frame (`oc_air` `OC_AIR_DATA`, payload opaque, "RADE/Codec2 frames" in its comment). The terminal never touches audio.
3. **Cell** (`oc_sig_net` in the firmware, run by `oc-cell` from `opencell-pi`): decrypts (`oc_sig_net_data_in`), re-encrypts for the other leg or forwards to the core as `MEDIA` (`oc_core_msg.h`: `ref 4, seq 2, data <= 18`).
4. **Core** (`opencell-core`): switches MEDIA between legs, relays it over OCSS to another core, and runs the **playback service**: a clip of `OC_CORE_PLAY_BYTES = 18` byte payloads, one per `OC_CORE_PLAY_US = 120000`, produced by `tools/clip/oc-clip-gen` (flite text to speech at 8 kHz, then `c2enc 1200`). `oc_core_clip_ok` accepts only whole 18-byte payloads.
5. **Signalling** (`oc_sig_msg.h`): `CALL_SETUP.codec_caps` and `SETUP_IND.codec_caps` are 8-bit bitmaps, `CONNECT.codec` one byte; every sender hard-codes caps 1 and codec 1 (`oc_sig_term.c:367,378`, `oc_sig_net.c:249,421,729,757`). The terminal reports the codec to the app in the BLE `CONNECTED` event (`ev[5]`).

What that means for OpenVoxNB:

- **The natural unit is the 120 ms radio frame**: six 20 ms codec frames per block, one block per app data frame. Every mode's block is a whole number of bytes (72...384 bits), and every mode's block size is distinct, so the mode is implied by the payload length where the transport preserves it (GATT, `oc_air`, MEDIA all do).
- **Only OVX-600...OVX-1200 fit today's 18-byte frame.** OVX-1400 and above need `OC_SIG_APP_MAX`, `OC_SIG_LINK_MAX`, `GattContract.MAX_PAYLOAD`, `OC_CORE_PLAY_BYTES` and the MEDIA body to grow (the air slot itself allows 240 bytes). That is a protocol version change across three repositories: a follow-on plan (voice spec v2), not this one. The codec library implements all nine modes now so the higher ones are ready when the frame grows and usable at once over IP (OCSS between cores, future gateways).
- **The decoder must take "block lost"** (`bits == NULL`) and do parameter-domain concealment, which is far better than the app's replay-at-lower-gain; the app's `JitterBuffer`/`VoiceSession` should call it (follow-on app task). The block's 1-bit **toggle** field lets a receiver without sequence numbers notice a single lost block.
- **Negotiation**: the codec ships `oc_vox_caps_t` (bit per mode), `oc_vox_negotiate()`, `oc_vox_codec_id()` / `oc_vox_mode_of_codec_id()`. Wiring caps into `oc_sig_term` (the app must tell the terminal which codecs it has: a GATT contract v5 COMMAND, or firmware-pinned caps), the pick in `oc_sig_net`/`oc_core`, and the operator's preference are the voice spec v2 plan.

## 5. Representations

| Thing | OpenCell today | OpenVoxNB |
|---|---|---|
| PCM | `int16_t` / `short` 8 kHz mono, little-endian raw files (`.raw`, `s16le`), `ShortArray` in Kotlin, 960 samples per 120 ms block, 320 per Codec2 frame | `int16_t` (Q15), `OC_VOX_FRAME_SAMPLES = 160u` (20 ms), `OC_VOX_BLOCK_SAMPLES = 960u` |
| Encoded frames | Headerless concatenated codec frames, `uint8_t` arrays, length = `frameBytes`; `.bit` files for clips | Headerless block of `oc_vox_block_bytes(mode)` bytes (9...48); `.ovx` file = 1-byte header (version, mode) + blocks, for tools only |
| Packets | `oc_air` `OC_AIR_DATA` (payload_len + opaque payload), link payload 20 B, MEDIA `data <= 18` | Unchanged: the block is the opaque payload |
| Errors | `int` 0 / -1 / `-2`, small negative enums (`OC_CORE_E_*`), `size_t` 0 for "didn't fit" | `int` 0 / -1 for init (bad mode), `size_t` 0 for a buffer too small, `oc_vox_dec_stats_t` counters for FEC outcomes |
| Modes | `CodecId` ints 1, 2; Codec2's `CODEC2_MODE_*` | `oc_vox_mode_t` enum 0...8 (`OC_VOX_600` ... `OC_VOX_3200`), `oc_vox_mode_bps()` |

## 6. Fixed-point, SIMD, embedded and real-time constraints

- **No `float` or `double` anywhere in the portable components today** (grep: none), and the ESP32-S3 has only a single-precision FPU. The production codec is integer-only: Q15 samples, `int32_t` accumulators, `int64_t` only outside inner loops, table-driven log/exp/sine, reciprocal tables instead of division in loops. The reference uses `float` and is never built for the board.
- **No SIMD** anywhere; none is planned (the ESP32-S3's PIE and ARM NEON are optional later wins behind the same `fx/` kernels).
- **Real-time**: the bs-radio's RX turnaround runs from IRAM with a hard ~1 ms budget (`tools/check_iram.py`, `turnaround.lf`); the codec must never be on that path and must not grow IRAM (its code lives in flash; only `const` tables and state). The terminal's BLE host task has an 8 KB stack; the codec's stack use is bounded and documented (target <= 2 KB per call), its state structs are sized by `_Static_assert` (targets: encoder <= 8 KB, decoder <= 4 KB), its codebooks <= 48 KB of flash.
- **Complexity target**: encoder + decoder of OVX-1200 within 25% of one ESP32-S3 core at 240 MHz (about 60 MIPS); measured on the host by the `oc-vox bench` tool, on the board by a follow-on bench step (there is no ESP-IDF toolchain in this container; the maintainer runs `idf.py build` and the bench).
- **Threading**: one encoder and one decoder, each used from one thread, no globals except `const` tables (the `oc_sig_term` / app model).

## 7. Logging, testing, benchmarking and CI

- **Logging**: daemons use `oc_log(prio, fmt, ...)` with syslog priorities (`oc/include/oc_log.h`); portable libraries take `void (*log)(void *ctx, const char *line)` in their `io` struct or are silent; the firmware uses `ESP_LOG`. The codec has no log callback: nothing in a 20 ms loop should format text. It exposes counters, which `oc-vox` prints and the app's developer "voice counters" line can show.
- **Testing**: Unity, `oc_test(name lib)` in `host-tests/CMakeLists.txt`, `setUp`/`tearDown`, `TEST_ASSERT_EQUAL_HEX8_ARRAY` golden bytes (`test_air.c`, `test_hop.c`), vector files passed by compile definition (`OC_NUMBER_VECTORS`). Red-then-green commits ("(red)"). ASan/UBSan configure flags are written down in the core's plans and run before merge. The codec follows all of it and adds: random-byte fuzzing of the decoder, BER and loss sweeps, equivalence tests between `oc_vox_*` and `oc_vox_ref_*`, golden hashes of fixed-point output (bit-exact across hosts and the board).
- **Benchmarking**: `tools/ocbench` is a radio bench tool, not a CPU benchmark; there is no benchmark harness. `tools/vox/oc-vox bench` is new and small.
- **CI**: none; `ctest` by hand. Nothing to integrate with; the plan's final task runs the full suite clean and under the sanitizers, as the existing plans do.

## 8. Library form

An **in-tree static library / ESP-IDF component**, like every other `oc_*`. Not shared: the app's `libcodec2.so` is a separate shared object only because Codec2 is LGPL-2.1 and must stay replaceable; OpenVoxNB is Apache-2.0 like the rest of OpenCell, so the app can compile it into one `libopencell_openvox.so` with its JNI glue (mirroring `:codec2`'s module, without the licence assets). Not header-only: it has tables and a lot of code, and OpenCell has no header-only precedent.

## 9. Touchpoints by repository

| Repository | This plan (branch `claude/openvoxnb-speech-codec-8hcrv3`) | Follow-on plans |
|---|---|---|
| `opencell-firmware` | The `oc_vox` component, its host tests and vectors, `tools/vox`, these docs. The ESP-IDF build compiles it (components under `components/` are built; nothing REQUIRES it yet). | Voice spec v2 in `oc_sig` (caps from the app, the network's pick, GATT contract v5), an on-board bench step. |
| `opencell-core` | Nothing (the submodule pin moves when the codec merges to firmware `main`). | Submodule bump; `oc-clip-gen` emits OpenVoxNB clips with `oc-vox enc`; the playback service learns the codec per call (today 18 B Codec2 1200 fixed); MEDIA size for higher modes. |
| `opencell-app` | Nothing. | `tools/openvox/vendor.sh` (pinned firmware commit, like `tools/codec2/vendor.sh`), `:openvox` module (JNI + `OpenVox : VoiceCodec`), `CodecId` 3-6, a composite `VoiceCodecFactory`, `BlockCodec` generalised to `960 / frameSamples` frames, `JitterBuffer` passing losses to the decoder. |
| `opencell-portal` | Nothing. | The NOC may show a call's codec once the core reports it; no codec code. |

## 10. Deviations from the request, and why

1. **C11 library, not a C++20 class hierarchy.** The repository is C11 and its consumers (firmware, core, JNI) are C. A header-only `opencell::voice::OpenVoxNB` wrapper satisfies the named API for C++ users without making the project's C builds depend on a C++ compiler.
2. **Layout folded into the component convention** (`firmware/components/oc_vox/` with `include/`, flat sources, `ref/` and `fx/`), not a top-level `voice/` tree. Tests go to `host-tests/`, tools to `tools/vox/`, docs to `docs/openvox/`, as the repository places them.
3. **`docs/openvox/INTEGRATION.md`** rather than `docs/INTEGRATION.md`: the repository's docs are grouped by topic (`docs/bench/`).
4. **The 120 ms block is the wire unit**, with the 20 ms frame as the analysis frame: it is the radio frame, every mode's block is whole bytes, and the lowest modes need the block for interpolation and for an FEC word that is not tiny. Bitrates are exact at the block (and therefore at the frame).
5. **Modes above 1200 bit/s do not fit today's air interface.** They are implemented and tested, usable over IP, and wait for voice spec v2 for the LoRa path. Saying so here is the explicit documentation the request asks for.
6. **Trained codebooks need speech.** This container has no speech corpus and no network dataset access is assumed. The plan builds structured (training-free) quantizers first, with a training tool and a bitstream-freeze task once the maintainer supplies a few minutes of 8 kHz speech (own recordings or a permissively licensed corpus). The bit allocation does not change when codebooks are trained, only their contents and the version constant.

## 11. Open questions for the maintainer

1. Confirm **C11 + wrapper** over a C++ codec (decision 1 above).
2. Confirm **codec ids 3-6** for OVX-600...OVX-1200 in the existing 8-bit caps (the voice design spec in the `opencell` docs repository, which is not in this checkout, owns that table).
3. Where **test speech** comes from: a few minutes of 8 kHz mono recordings in `host-tests/vectors/vox/` (kept small, or generated by a script from a corpus the maintainer has).
4. Whether the app should get OpenVoxNB **before** voice spec v2 (possible: the network can be told to pick codec 6 when both ends advertise it) or together with it.
