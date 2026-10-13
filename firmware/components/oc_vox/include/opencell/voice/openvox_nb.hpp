// opencell::voice::OpenVoxNB: a header-only C++ view of oc_vox.h for C++
// consumers (PLAN.md §1.8). One object holds an encoder and a decoder of
// one mode; no heap, no exceptions, no RTTI. C++17 or later; with C++20's
// <span> the span overloads appear. The C library stays the API of record.
#ifndef OPENCELL_VOICE_OPENVOX_NB_HPP
#define OPENCELL_VOICE_OPENVOX_NB_HPP

#include <cstddef>
#include <cstdint>

#include "oc_vox.h"

#if defined(__cpp_lib_span) || (__cplusplus >= 202002L && __has_include(<span>))
#include <span>
#define OPENCELL_VOICE_HAS_SPAN 1
#endif

namespace opencell::voice {

enum class Mode : int {
    Ovx600 = OC_VOX_600, Ovx800 = OC_VOX_800, Ovx1000 = OC_VOX_1000, Ovx1200 = OC_VOX_1200, Ovx1400 = OC_VOX_1400,
    Ovx1600 = OC_VOX_1600, Ovx1800 = OC_VOX_1800, Ovx2400 = OC_VOX_2400, Ovx3200 = OC_VOX_3200
};

inline std::uint16_t bitrate(Mode m) noexcept { return oc_vox_mode_bps(static_cast<oc_vox_mode_t>(m)); }
inline std::size_t blockBytes(Mode m) noexcept { return oc_vox_block_bytes(static_cast<oc_vox_mode_t>(m)); }

class OpenVoxNB {
public:
    static constexpr std::size_t kRateHz = OC_VOX_RATE_HZ;
    static constexpr std::size_t kBlockSamples = OC_VOX_BLOCK_SAMPLES;
    static constexpr std::size_t kBlockMaxBytes = OC_VOX_BLOCK_MAX_BYTES;

    explicit OpenVoxNB(Mode m) noexcept
        : ok_(oc_vox_enc_init(&enc_, static_cast<oc_vox_mode_t>(m)) == 0 && oc_vox_dec_init(&dec_, static_cast<oc_vox_mode_t>(m)) == 0),
          mode_(m) {}

    OpenVoxNB(const OpenVoxNB &) = delete;
    OpenVoxNB &operator=(const OpenVoxNB &) = delete;

    bool ok() const noexcept { return ok_; }
    Mode mode() const noexcept { return mode_; }
    std::size_t blockBytes() const noexcept { return voice::blockBytes(mode_); }

    // 960 samples in, blockBytes() bytes out (out must have room).
    void encode(const std::int16_t *pcm, std::uint8_t *out) noexcept { oc_vox_encode(&enc_, pcm, out); }
    // One block in (nullptr or a length other than blockBytes(): lost, concealed), 960 samples out.
    void decode(const std::uint8_t *block, std::size_t len, std::int16_t *pcm) noexcept
    {
        oc_vox_decode(&dec_, (block != nullptr && len == blockBytes()) ? block : nullptr, pcm);
    }

#ifdef OPENCELL_VOICE_HAS_SPAN
    void encode(std::span<const std::int16_t, kBlockSamples> pcm, std::span<std::uint8_t> out) noexcept
    {
        if (out.size() >= blockBytes()) encode(pcm.data(), out.data());
    }
    void decode(std::span<const std::uint8_t> block, std::span<std::int16_t, kBlockSamples> pcm) noexcept
    {
        decode(block.empty() ? nullptr : block.data(), block.size(), pcm.data());
    }
#endif

    const oc_vox_dec_stats_t &stats() const noexcept { return *oc_vox_dec_stats(&dec_); }

private:
    oc_vox_enc_t enc_;
    oc_vox_dec_t dec_;
    bool ok_;
    Mode mode_;
};

} // namespace opencell::voice

#endif
