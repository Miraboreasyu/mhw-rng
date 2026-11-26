#pragma once
#include "mhw_rng_api.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace peer_beacon {

using AlertSinkFn = void(*)(const char* utf8Text) noexcept;

struct DiagSnapshot {
    bool          localFound    = false;
    std::uint32_t txFrames      = 0U;
    std::uint32_t rxLocalFrames = 0U;
    std::string   activeTaint;
};

constexpr std::uint8_t kSymEnd            = 38U;
constexpr std::size_t  kWmGridCols        = 16U;
constexpr std::size_t  kWmGridRows        = 12U;
constexpr std::size_t  kWmCellCount       = kWmGridCols * kWmGridRows; // 192
constexpr std::uint8_t kWmSync0           = 61U; // 0b111101
constexpr std::uint8_t kWmSync1           = 42U; // 0b101010
constexpr double       kWmFreqHz          = 4.0;
constexpr std::size_t  kMaxAudioMarkSlots = 256U;

struct VideoMarkFrame {
    bool  active       = false;
    bool  debugVisible = false;
    float cellWeights[kWmCellCount]{};
};

struct AudioMarkPacket {
    bool          active    = false;
    std::uint32_t slotCount = 0U;
    std::int8_t   b1[kMaxAudioMarkSlots]{};
    std::int8_t   b2[kMaxAudioMarkSlots]{};
};

void Init(bool checkEnabled, bool selfCheckEnabled, AlertSinkFn alertSink) noexcept;
void SetAlertSink(AlertSinkFn alertSink) noexcept;

void SetCheckEnabled(bool enabled) noexcept;
bool IsCheckEnabled() noexcept;

void SetSelfCheckEnabled(bool enabled) noexcept;
bool IsSelfCheckEnabled() noexcept;

void MarkTainted(MhwRngPluginId ownerId) noexcept;
void ClearTainted(MhwRngPluginId ownerId) noexcept;
void ClearAllTaint() noexcept;
bool HasAnyTaint() noexcept;
bool IsInScene() noexcept;
bool IsVideoMarkActive() noexcept;
void SetVideoMarkDebugVisible(bool visible) noexcept;
bool IsVideoMarkDebugVisible() noexcept;
void ResetDecoders() noexcept;

bool IsOnlineSessionSEH(std::uintptr_t exeBase) noexcept;
std::uint32_t MaybeEncodeCritSeed(std::uintptr_t exeBase,
                                  std::uint32_t callSiteRva,
                                  std::uint32_t rawValue) noexcept;
void TickOnMainThread(std::uintptr_t exeBase) noexcept;

bool EncodeSymbolChar(char c, std::uint8_t& outSym) noexcept;
bool DecodeSymbolChar(std::uint8_t sym, char& outChar) noexcept;
std::uint32_t PackFrame27(std::uint8_t nonce5,
                          std::uint8_t gen2,
                          std::uint8_t pos6,
                          std::uint8_t sym6) noexcept;
bool UnpackFrame27(std::uint32_t seed27,
                   std::uint8_t& outGen2,
                   std::uint8_t& outPos6,
                   std::uint8_t& outSym6) noexcept;
std::uint8_t ComputeListGen2(const char* listStr, std::size_t len) noexcept;
std::uint16_t ComputeVideoMarkCrc12(const std::uint8_t* data, std::size_t len) noexcept;
bool BuildVideoMarkFrameAt(double tSec,
                           const std::string& payload,
                           bool debugVisible,
                           VideoMarkFrame& out) noexcept;
bool GetCurrentVideoMarkFrame(VideoMarkFrame& out) noexcept;
bool BuildAudioMarkPacket(const std::string& payload, AudioMarkPacket& out) noexcept;
void RefreshAudioMarkSnapshot() noexcept;
bool GetCurrentAudioMarkPacket(AudioMarkPacket& out, bool* outBusy = nullptr) noexcept;
DiagSnapshot GetDiagSnapshot() noexcept;

} // namespace peer_beacon
