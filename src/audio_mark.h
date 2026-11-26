#pragma once
#include "peer_beacon.h"

#include <cstdint>

namespace audio_mark {

struct EchoStreamProcessor {
    static constexpr std::size_t  kKernelLen   = 12U;
    static constexpr std::size_t  kMaxChannels = 8U;
    static constexpr std::size_t  kRingCap     = 1024U; // power of 2; max lag at 192kHz is 912
    static constexpr std::size_t  kRingMask    = kRingCap - 1U;
    static constexpr float        kTargetAlpha = 0.016f;
    static constexpr double       kCarrierHz   = 4.0;
    static constexpr double       kSlotsPerSec = 2.0;

    std::uint32_t sampleRate    = 48000U;
    std::uint32_t channels      = 2U;
    std::uint64_t sampleCounter = 0ULL;
    std::uint32_t ringPos       = 0U;
    float         currentGain   = 0.0f;
    float         rampStep      = 0.016f / 480.0f;
    std::uint32_t lagsP[kKernelLen]{};
    std::uint32_t lagsD1[kKernelLen]{};
    std::uint32_t lagsD2[kKernelLen]{};
    peer_beacon::AudioMarkPacket lastValidPkt{};
    float         ring[kMaxChannels][kRingCap]{};

    void Configure(std::uint32_t sr, std::uint32_t ch) noexcept;
    void Reset() noexcept;
    void AdvanceSilent(std::uint32_t numFrames,
                       const peer_beacon::AudioMarkPacket& pkt) noexcept;
    void ProcessFloatInterleaved(float* samples,
                                 std::uint32_t numFrames,
                                 const peer_beacon::AudioMarkPacket& pkt) noexcept;
    void ProcessInt16Interleaved(std::int16_t* samples,
                                 std::uint32_t numFrames,
                                 const peer_beacon::AudioMarkPacket& pkt) noexcept;
};

void Init() noexcept;
void Shutdown() noexcept;
bool IsHooked() noexcept;

} // namespace audio_mark
