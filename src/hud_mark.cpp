#include "hud_mark.h"

#include "mhw_addresses.h"
#include "peer_beacon.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hud_mark {
namespace {

using GetInstanceFromNameFn = std::uintptr_t(__fastcall*)(std::uintptr_t uGui, const char* name);

constexpr std::size_t kMaxHudEntries = 16U;
HudShiftEntry g_entries[kMaxHudEntries]{};
std::uint64_t g_useSeq = 0ULL;

inline bool IsValidPtr(std::uintptr_t p) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & 0x7ULL) == 0ULL;
}

HudWidgetKind ClassifyHudWidget(std::uintptr_t exeBase, std::uintptr_t vft) noexcept {
    if (!exeBase || !vft) return HudWidgetKind::Unknown;
    if (vft == (exeBase + mhw::kRvaVftGuiHudItemWindow))     return HudWidgetKind::ItemWindow;
    if (vft == (exeBase + mhw::kRvaVftGuiHudItemRingShortcut))   return HudWidgetKind::RingShortcut;
    if (vft == (exeBase + mhw::kRvaVftGuiHudItemRingShortcutPC)) return HudWidgetKind::RingShortcutPC;
    return HudWidgetKind::Unknown;
}

void ReadDisplayLayoutSEH(std::uintptr_t exeBase,
                          float& outAspect,
                          float& outWideMarginPx) noexcept {
    outAspect       = 16.0f / 9.0f;
    outWideMarginPx = 0.0f;
    if (!exeBase) return;
    __try {
        const auto sMhRender = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSMhRenderInstance);
        if (IsValidPtr(sMhRender)) {
            const float a = *reinterpret_cast<const float*>(
                sMhRender + mhw::kOffRenderAspectRatio);
            if (std::isfinite(a) && a >= 1.0f && a <= 5.0f) {
                outAspect = a;
            }
        }
        const auto sMhGui = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSMhGuiInstance);
        if (IsValidPtr(sMhGui)) {
            const float m = *reinterpret_cast<const float*>(
                sMhGui + mhw::kOffSMhGuiWideUiMarginPx);
            if (std::isfinite(m) && m >= 0.0f && m < 10000.0f) {
                outWideMarginPx = m;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool IsPlausibleTransform(float posX, float posY, float scX, float scY) noexcept {
    return std::isfinite(posX) && std::isfinite(posY) &&
           std::isfinite(scX) && std::isfinite(scY) &&
           std::fabs(posX) < 10000.0f &&
           std::fabs(posY) < 10000.0f &&
           scX > 0.1f && scX < 10.0f &&
           scY > 0.1f && scY < 10.0f;
}

bool ResolveRootAnimSEH(std::uintptr_t exeBase,
                        std::uintptr_t uMhGui,
                        std::uintptr_t& outRes,
                        std::uintptr_t& outRoot,
                        bool& outShouldMarkFailed) noexcept {
    outRes              = 0ULL;
    outRoot             = 0ULL;
    outShouldMarkFailed = false;
    __try {
        outRes  = *reinterpret_cast<const std::uintptr_t*>(uMhGui + mhw::kOffUGuiResource);
        outRoot = *reinterpret_cast<const std::uintptr_t*>(uMhGui + mhw::kOffUMhGuiRootAnim);
        if (IsValidPtr(outRoot)) {
            return true;
        }
        const char* nullName = *reinterpret_cast<const char* const*>(uMhGui + mhw::kOffUMhGuiRootNullName);
        if (IsValidPtr(reinterpret_cast<std::uintptr_t>(nullName)) && nullName[0] != '\0') {
            const auto fnGetInst = reinterpret_cast<GetInstanceFromNameFn>(
                exeBase + mhw::kRvaUGuiGetInstanceFromName);
            outRoot = fnGetInst(uMhGui, nullName);
            if (!IsValidPtr(outRoot)) {
                outRoot = 0ULL;
                outShouldMarkFailed = true;
                return false;
            }
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outRes              = 0ULL;
        outRoot             = 0ULL;
        outShouldMarkFailed = true;
        return false;
    }
}

void ApplyToRootSEH(std::uintptr_t root,
                    bool wantShift,
                    float targetDeltaX,
                    HudShiftEntry& entry) noexcept {
    if (!IsValidPtr(root)) return;
    __try {
        float posX = *reinterpret_cast<const float*>(root + mhw::kOffGuiInstLocalPosX);
        float posY = *reinterpret_cast<const float*>(root + mhw::kOffGuiInstLocalPosY);
        float scX  = *reinterpret_cast<const float*>(root + mhw::kOffGuiInstLocalScaleX);
        float scY  = *reinterpret_cast<const float*>(root + mhw::kOffGuiInstLocalScaleY);
        if (!IsPlausibleTransform(posX, posY, scX, scY)) {
            return;
        }
        std::uint32_t flags = *reinterpret_cast<const std::uint32_t*>(root + mhw::kOffGuiInstFlags);
        if (ApplyHudShiftState(wantShift,
                               targetDeltaX,
                               entry.applied,
                               entry.appliedDeltaX,
                               entry.lastWrittenPosX,
                               entry.lastWrittenPosY,
                               entry.lastWrittenScX,
                               entry.lastWrittenScY,
                               posX,
                               posY,
                               scX,
                               scY,
                               flags)) {
            *reinterpret_cast<float*>(root + mhw::kOffGuiInstLocalPosX)     = posX;
            *reinterpret_cast<float*>(root + mhw::kOffGuiInstLocalPosY)     = posY;
            *reinterpret_cast<float*>(root + mhw::kOffGuiInstLocalScaleX)   = scX;
            *reinterpret_cast<float*>(root + mhw::kOffGuiInstLocalScaleY)   = scY;
            *reinterpret_cast<std::uint32_t*>(root + mhw::kOffGuiInstFlags) = flags;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        entry.applied       = false;
        entry.appliedDeltaX = 0.0f;
    }
}

} // namespace

float ComputeHudShiftDeltaX(HudWidgetKind kind,
                            float aspectRatio,
                            float wideUiMarginPx) noexcept {
    const float aspect = (std::isfinite(aspectRatio) && aspectRatio >= 1.0f && aspectRatio <= 5.0f)
        ? aspectRatio
        : (16.0f / 9.0f);
    const float extraW = (std::max)(0.0f, aspect * kHudDesignHeight - kHudDesignWidth);
    const float rawMargin = std::isfinite(wideUiMarginPx) ? wideUiMarginPx : 0.0f;
    const float clampedMargin = std::clamp(rawMargin, 0.0f, 0.5f * extraW);

    float pivotX = kHudDesignCenterX;
    if (kind == HudWidgetKind::RingShortcut) {
        pivotX = kHudDesignCenterX + 0.5f * extraW;
    } else if (kind == HudWidgetKind::RingShortcutPC) {
        pivotX = kHudDesignCenterX + (extraW - clampedMargin);
    }
    return (1.0f - kHudShiftScale) * pivotX;
}

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
                        std::uint32_t& ioFlags) noexcept {
    constexpr std::uint32_t kDirtyFlag = 0x10000U;
    constexpr float         kEps       = 1e-3f;

    if (wantShift) {
        if (!ioApplied) {
            ioPosX   += targetDeltaX;
            ioPosY   += kHudShiftDeltaY;
            ioScaleX *= kHudShiftScale;
            ioScaleY *= kHudShiftScale;
            ioLastWrittenPosX = ioPosX;
            ioLastWrittenPosY = ioPosY;
            ioLastWrittenScX  = ioScaleX;
            ioLastWrittenScY  = ioScaleY;
            ioAppliedDeltaX   = targetDeltaX;
            ioApplied         = true;
            ioFlags          |= kDirtyFlag;
            return true;
        }

        bool dirty = false;
        if (std::fabs(ioPosX - ioLastWrittenPosX) > kEps) {
            ioPosX            += targetDeltaX;
            ioLastWrittenPosX  = ioPosX;
            ioAppliedDeltaX    = targetDeltaX;
            dirty              = true;
        } else if (std::fabs(targetDeltaX - ioAppliedDeltaX) > kEps) {
            ioPosX            += (targetDeltaX - ioAppliedDeltaX);
            ioLastWrittenPosX  = ioPosX;
            ioAppliedDeltaX    = targetDeltaX;
            dirty              = true;
        }
        if (std::fabs(ioPosY - ioLastWrittenPosY) > kEps) {
            ioPosY            += kHudShiftDeltaY;
            ioLastWrittenPosY  = ioPosY;
            dirty              = true;
        }
        if (std::fabs(ioScaleX - ioLastWrittenScX) > kEps) {
            ioScaleX         *= kHudShiftScale;
            ioLastWrittenScX  = ioScaleX;
            dirty             = true;
        }
        if (std::fabs(ioScaleY - ioLastWrittenScY) > kEps) {
            ioScaleY         *= kHudShiftScale;
            ioLastWrittenScY  = ioScaleY;
            dirty             = true;
        }
        if (dirty) {
            ioFlags |= kDirtyFlag;
        }
        return dirty;
    }

    if (!ioApplied) {
        return false;
    }

    bool restoredAny = false;
    if (std::fabs(ioPosX - ioLastWrittenPosX) <= kEps) {
        ioPosX     -= ioAppliedDeltaX;
        restoredAny = true;
    }
    if (std::fabs(ioPosY - ioLastWrittenPosY) <= kEps) {
        ioPosY     -= kHudShiftDeltaY;
        restoredAny = true;
    }
    if (std::fabs(ioScaleX - ioLastWrittenScX) <= kEps) {
        ioScaleX   /= kHudShiftScale;
        restoredAny = true;
    }
    if (std::fabs(ioScaleY - ioLastWrittenScY) <= kEps) {
        ioScaleY   /= kHudShiftScale;
        restoredAny = true;
    }
    ioApplied       = false;
    ioAppliedDeltaX = 0.0f;
    if (restoredAny) {
        ioFlags |= kDirtyFlag;
    }
    return restoredAny;
}

void OnUMhGuiMoveAfter(std::uintptr_t exeBase, std::uintptr_t uMhGui) noexcept {
    if (!exeBase || !IsValidPtr(uMhGui)) return;

    std::uintptr_t vft = 0ULL;
    __try {
        vft = *reinterpret_cast<const std::uintptr_t*>(uMhGui);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const HudWidgetKind kind = ClassifyHudWidget(exeBase, vft);
    if (kind == HudWidgetKind::Unknown) return;

    HudShiftEntry* entry = nullptr;
    int emptyIdx  = -1;
    int oldestIdx = 0;
    std::uint64_t oldestSeq = ~0ULL;
    for (std::size_t i = 0U; i < kMaxHudEntries; ++i) {
        if (g_entries[i].uMhGui == uMhGui) {
            entry = &g_entries[i];
            entry->lastUseSeq = ++g_useSeq;
            break;
        }
        if (g_entries[i].uMhGui == 0ULL) {
            if (emptyIdx < 0) emptyIdx = static_cast<int>(i);
        } else if (g_entries[i].lastUseSeq < oldestSeq) {
            oldestSeq = g_entries[i].lastUseSeq;
            oldestIdx = static_cast<int>(i);
        }
    }

    const bool wantShift = peer_beacon::IsVideoMarkActive();
    if (!entry) {
        if (!wantShift) return;
        const int chosen = (emptyIdx >= 0) ? emptyIdx : oldestIdx;
        entry = &g_entries[chosen];
        *entry = HudShiftEntry{};
        entry->uMhGui     = uMhGui;
        entry->lastUseSeq = ++g_useSeq;
    }

    std::uintptr_t curRes  = 0ULL;
    std::uintptr_t curRoot = 0ULL;
    __try {
        curRes  = *reinterpret_cast<const std::uintptr_t*>(uMhGui + mhw::kOffUGuiResource);
        curRoot = *reinterpret_cast<const std::uintptr_t*>(uMhGui + mhw::kOffUMhGuiRootAnim);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (curRes != entry->mpResource) {
        entry->mpResource    = curRes;
        entry->rootAnim      = 0ULL;
        entry->applied       = false;
        entry->appliedDeltaX = 0.0f;
        entry->failedResolve = false;
    } else if (IsValidPtr(curRoot) && entry->rootAnim != curRoot) {
        entry->rootAnim      = curRoot;
        entry->applied       = false;
        entry->appliedDeltaX = 0.0f;
        entry->failedResolve = false;
    }

    if (entry->failedResolve && curRes == entry->mpResource) {
        return;
    }

    if (!IsValidPtr(entry->rootAnim)) {
        if (!wantShift) return;
        std::uintptr_t resolvedRes = 0ULL;
        std::uintptr_t resolvedRoot = 0ULL;
        bool markFailed = false;
        if (!ResolveRootAnimSEH(exeBase, uMhGui, resolvedRes, resolvedRoot, markFailed)) {
            entry->mpResource    = resolvedRes;
            entry->failedResolve = markFailed;
            return;
        }
        entry->mpResource    = resolvedRes;
        entry->rootAnim      = resolvedRoot;
        entry->applied       = false;
        entry->appliedDeltaX = 0.0f;
    }

    float targetDeltaX = kHudShiftDeltaX;
    if (wantShift) {
        float aspect = 16.0f / 9.0f;
        float marginPx = 0.0f;
        ReadDisplayLayoutSEH(exeBase, aspect, marginPx);
        targetDeltaX = ComputeHudShiftDeltaX(kind, aspect, marginPx);
    }

    ApplyToRootSEH(entry->rootAnim, wantShift, targetDeltaX, *entry);
}

void ResetState() noexcept {
    std::memset(g_entries, 0, sizeof(g_entries));
    g_useSeq = 0ULL;
}

} // namespace hud_mark
