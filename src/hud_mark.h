#pragma once

#include <cstdint>

namespace hud_mark {

enum class HudWidgetKind : std::uint8_t {
    Unknown        = 0,
    ItemWindow     = 1,
    RingShortcut   = 2,
    RingShortcutPC = 3
};

struct HudShiftEntry {
    std::uintptr_t uMhGui           = 0;
    std::uintptr_t mpResource       = 0;
    std::uintptr_t rootAnim         = 0;
    bool           applied          = false;
    bool           failedResolve    = false;
    float          appliedDeltaX    = 0.0f;
    float          lastWrittenPosX  = 0.0f;
    float          lastWrittenPosY  = 0.0f;
    float          lastWrittenScX   = 1.0f;
    float          lastWrittenScY   = 1.0f;
    std::uint64_t  lastUseSeq       = 0ULL;
};

constexpr float kHudDesignWidth   = 1920.0f;
constexpr float kHudDesignHeight  = 1080.0f;
constexpr float kHudDesignCenterX = 960.0f;
constexpr float kHudShiftScale    = 0.985f;
constexpr float kHudShiftDeltaX   = 14.4f; // 16:9 baseline: (1.0f - kHudShiftScale) * kHudDesignCenterX
constexpr float kHudShiftDeltaY   = 4.0f;

float ComputeHudShiftDeltaX(HudWidgetKind kind,
                            float aspectRatio,
                            float wideUiMarginPx) noexcept;

bool ApplyHudShiftState(bool wantShift,
                        float targetDeltaX,
                        bool& ioApplied,
                        float& ioAppliedDeltaX,
                        float& ioLastWrittenPosX,
                        float& ioLastWrittenPosY,
                        float& ioLastWrittenScX,
                        float& ioLastWrittenScY,
                        float& ioPosX,
                        float& ioPosY,
                        float& ioScaleX,
                        float& ioScaleY,
                        std::uint32_t& ioFlags) noexcept;

void OnUMhGuiMoveAfter(std::uintptr_t exeBase, std::uintptr_t uMhGui) noexcept;
void ResetState() noexcept;

} // namespace hud_mark
