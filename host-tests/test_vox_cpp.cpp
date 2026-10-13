// The C++ wrapper compiles (C++20, no exceptions, no RTTI) and round-trips.
#include "unity.h"

#include <array>
#include <cmath>
#include <cstring>

#include "opencell/voice/openvox_nb.hpp"

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

using opencell::voice::Mode;
using opencell::voice::OpenVoxNB;

static_assert(OpenVoxNB::kBlockSamples == 960);

static void test_wrapper_round_trips_a_block_and_counts_a_loss()
{
    OpenVoxNB codec{Mode::Ovx1200};
    TEST_ASSERT_TRUE(codec.ok());
    TEST_ASSERT_EQUAL_size_t(18, codec.blockBytes());
    TEST_ASSERT_EQUAL_size_t(18, opencell::voice::blockBytes(Mode::Ovx1200));
    TEST_ASSERT_EQUAL_UINT16(3200, opencell::voice::bitrate(Mode::Ovx3200));
    std::array<std::int16_t, OpenVoxNB::kBlockSamples> pcm{}, out{};
    std::array<std::uint8_t, OpenVoxNB::kBlockMaxBytes> block{};
    for (std::size_t i = 0; i < pcm.size(); i++) pcm[i] = static_cast<std::int16_t>(8000.0 * std::sin(2.0 * 3.14159265 * 150.0 * static_cast<double>(i) / 8000.0));
    double in = 0, o = 0;
    for (int b = 0; b < 4; b++) {
#ifdef OPENCELL_VOICE_HAS_SPAN
        codec.encode(std::span<const std::int16_t, 960>{pcm}, std::span<std::uint8_t>{block});
        codec.decode(std::span<const std::uint8_t>{block.data(), 18}, std::span<std::int16_t, 960>{out});
#else
        codec.encode(pcm.data(), block.data());
        codec.decode(block.data(), 18, out.data());
#endif
    }
    for (std::size_t i = 0; i < pcm.size(); i++) { in += double(pcm[i]) * pcm[i]; o += double(out[i]) * out[i]; }
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 0.0f, static_cast<float>(10.0 * std::log10(o / in)));
    TEST_ASSERT_EQUAL_UINT32(0, codec.stats().lost_blocks);
    codec.decode(nullptr, 0, out.data());
    codec.decode(block.data(), 17, out.data()); // the wrong length is a loss too
    TEST_ASSERT_EQUAL_UINT32(2, codec.stats().lost_blocks);
    TEST_ASSERT_EQUAL_UINT32(6, codec.stats().blocks);
}

static void test_a_bad_mode_is_not_ok()
{
    OpenVoxNB codec{static_cast<Mode>(9)};
    TEST_ASSERT_FALSE(codec.ok());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_wrapper_round_trips_a_block_and_counts_a_loss);
    RUN_TEST(test_a_bad_mode_is_not_ok);
    return UNITY_END();
}
