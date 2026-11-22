#include "hook.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <mutex>

namespace hook {
namespace {

constexpr std::size_t kCodeArenaSize = 0x1000; // 4 KB PAGE_EXECUTE_READ arena (strict W^X)
constexpr std::size_t kMaxDetours    = 24;
constexpr std::size_t kMaxStandalone = 8;

struct DetourEntry {
    bool           valid             = false;
    bool           active            = false;
    bool           externallyChained = false;
    std::uintptr_t site              = 0;
    std::uintptr_t relay             = 0;
    std::uint8_t   orig5[5]{};
    std::uint8_t   jmp5[5]{};
};

std::uintptr_t g_moduleLow       = 0;
std::uintptr_t g_moduleHigh      = 0;
std::uintptr_t g_codeArena       = 0;
std::size_t    g_codeUsed        = 0;
bool           g_sealed          = false;
DetourEntry    g_detours[kMaxDetours]{};
std::size_t    g_detourCount     = 0;

DetourEntry    g_standalone[kMaxStandalone]{};
std::size_t    g_standaloneCount = 0;

std::mutex     g_mx;

bool InRel32(std::uintptr_t from, std::uintptr_t to) noexcept {
    const std::int64_t d = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    return d >= INT32_MIN && d <= INT32_MAX;
}

std::uintptr_t TryAllocReadWriteAt(std::uintptr_t addr, std::size_t size) noexcept {
    void* p = ::VirtualAlloc(reinterpret_cast<void*>(addr), size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return reinterpret_cast<std::uintptr_t>(p);
}

std::uintptr_t AllocNearPageReadWrite(std::uintptr_t lo, std::uintptr_t hi) noexcept {
    if (!lo || hi <= lo) return 0;
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const std::uintptr_t span = 0x7F000000ULL;

    for (std::uintptr_t a = (lo - gran) & ~(gran - 1); a > gran && lo - a < span; a -= gran) {
        if (!InRel32(a, hi) || !InRel32(a + kCodeArenaSize, lo)) continue;
        if (std::uintptr_t p = TryAllocReadWriteAt(a, kCodeArenaSize)) {
            return p;
        }
    }
    for (std::uintptr_t a = (hi + gran) & ~(gran - 1); a - hi < span; a += gran) {
        if (!InRel32(lo, a + kCodeArenaSize)) break;
        if (std::uintptr_t p = TryAllocReadWriteAt(a, kCodeArenaSize)) {
            return p;
        }
    }
    for (int i = 0; i < 64; ++i) {
        void* p = ::VirtualAlloc(nullptr, kCodeArenaSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) break;
        const auto a = reinterpret_cast<std::uintptr_t>(p);
        if (InRel32(a, hi) && InRel32(a + kCodeArenaSize, lo)) return a;
        ::VirtualFree(p, 0, MEM_RELEASE);
    }
    return 0;
}

// Atomically replaces 5 bytes ONLY if the current 5 bytes still equal `expected5`.
// Prevents TOCTOU overwrites when two plugins concurrently hook the same address (e.g. sChat::DispatchChatEntry).
bool CompareAndPatch5BytesSafe(std::uintptr_t site,
                               const std::uint8_t expected5[5],
                               const std::uint8_t new5[5]) noexcept {
    DWORD oldProt = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(site), 8, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }

    bool patched = false;
    if ((site & 0x7ULL) == 0) {
        auto* qwordPtr = reinterpret_cast<volatile LONG64*>(site);
        LONG64 cur = *qwordPtr;
        for (;;) {
            if (std::memcmp(&cur, expected5, 5) != 0) {
                patched = false;
                break;
            }
            LONG64 desired = cur;
            std::memcpy(&desired, new5, 5);
            const LONG64 prev = _InterlockedCompareExchange64(qwordPtr, desired, cur);
            if (prev == cur) {
                patched = true;
                break;
            }
            cur = prev;
        }
    } else {
        // Unaligned site (e.g. 0x142C58034 CRT_rand): verify bytes before patching under VirtualProtect
        if (std::memcmp(reinterpret_cast<const void*>(site), expected5, 5) == 0) {
            std::memcpy(reinterpret_cast<void*>(site), new5, 5);
            patched = true;
        }
    }

    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(site), 8, oldProt, &tmp);
    if (patched) {
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(site), 8);
    }
    return patched;
}

// Relocates any displaced E8 rel32 CALL within `expectedPrologue[0..prologueLen)` (e.g. CRT_rand +4)
bool CopyAndRelocatePrologue(std::uintptr_t site,
                             const std::uint8_t* srcBytes,
                             std::size_t len,
                             std::uintptr_t trampAddr,
                             std::uint8_t* dstTramp) noexcept {
    std::memcpy(dstTramp, srcBytes, len);
    if (len >= 5U && srcBytes[len - 5U] == 0xE8) {
        const std::size_t callOff = len - 5U;
        std::int32_t origRel = 0;
        std::memcpy(&origRel, srcBytes + callOff + 1U, 4);
        const std::uintptr_t callTarget = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(site + callOff + 5U) + origRel);
        const std::uintptr_t trampCallEnd = trampAddr + callOff + 5U;
        if (!InRel32(trampCallEnd, callTarget)) {
            return false;
        }
        const auto newRel = static_cast<std::int32_t>(
            static_cast<std::int64_t>(callTarget) - static_cast<std::int64_t>(trampCallEnd));
        std::memcpy(dstTramp + callOff + 1U, &newRel, 4);
    }
    return true;
}

bool EmitDetourSlot(std::uintptr_t site,
                    std::size_t prologueLen,
                    void* detourFn,
                    const std::uint8_t* expectedPrologue,
                    std::uintptr_t slotBase,
                    std::size_t& outBytesUsed,
                    DetourEntry& outEntry,
                    void** outOrigTrampoline,
                    bool* outWasChained,
                    std::uintptr_t* outChainedTarget) noexcept {
    const auto* cur = reinterpret_cast<const std::uint8_t*>(site);
    const bool matchesExpected = (std::memcmp(cur, expectedPrologue, prologueLen) == 0);
    const bool isAlreadyJmp    = (cur[0] == 0xE9);
    if (!matchesExpected && !isAlreadyJmp) {
        return false;
    }

    if (!InRel32(site + 5U, slotBase)) return false;

    auto* out = reinterpret_cast<std::uint8_t*>(slotBase);
    // [0..13] Relay: FF 25 00 00 00 00 <u64 detourFn>
    out[0] = 0xFF; out[1] = 0x25;
    out[2] = 0x00; out[3] = 0x00; out[4] = 0x00; out[5] = 0x00;
    const auto fnAddr = reinterpret_cast<std::uint64_t>(detourFn);
    std::memcpy(out + 6U, &fnAddr, 8);

    // [16..] Trampoline
    const std::uintptr_t trampAddr = slotBase + 16U;
    auto* tramp = out + 16U;

    bool chained = false;
    std::uintptr_t prevTarget = 0;

    if (isAlreadyJmp && !matchesExpected) {
        chained = true;
        std::int32_t prevRel = 0;
        std::memcpy(&prevRel, cur + 1U, 4);
        prevTarget = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(site + 5U) + prevRel);
        if (!InRel32(trampAddr + 5U, prevTarget)) return false;

        tramp[0] = 0xE9;
        const auto chainRel = static_cast<std::int32_t>(
            static_cast<std::int64_t>(prevTarget) - static_cast<std::int64_t>(trampAddr + 5U));
        std::memcpy(tramp + 1U, &chainRel, 4);
        outBytesUsed = 24U;
    } else {
        const std::uintptr_t backNext = trampAddr + prologueLen + 5U;
        if (!InRel32(backNext, site + prologueLen)) return false;
        if (!CopyAndRelocatePrologue(site, expectedPrologue, prologueLen, trampAddr, tramp)) {
            return false;
        }
        tramp[prologueLen] = 0xE9;
        const auto backRel = static_cast<std::int32_t>(
            static_cast<std::int64_t>(site + prologueLen) - static_cast<std::int64_t>(backNext));
        std::memcpy(tramp + prologueLen + 1U, &backRel, 4);
        outBytesUsed = 16U + prologueLen + 5U;
    }

    outEntry                   = DetourEntry{};
    outEntry.valid             = true;
    outEntry.active            = false;
    outEntry.externallyChained = false;
    outEntry.site              = site;
    outEntry.relay             = slotBase;
    std::memcpy(outEntry.orig5, cur, 5);

    outEntry.jmp5[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(
        static_cast<std::int64_t>(slotBase) - static_cast<std::int64_t>(site + 5U));
    std::memcpy(outEntry.jmp5 + 1U, &rel, 4);

    if (outOrigTrampoline) *outOrigTrampoline = reinterpret_cast<void*>(trampAddr);
    if (outWasChained)     *outWasChained     = chained;
    if (outChainedTarget)  *outChainedTarget  = prevTarget;
    return true;
}

} // namespace

bool InitArena(std::uintptr_t lo, std::uintptr_t hi) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_moduleLow  = lo;
    g_moduleHigh = hi;
    if (g_codeArena) return true;

    g_codeArena = AllocNearPageReadWrite(lo, hi);
    if (!g_codeArena) return false;

    g_codeUsed    = 0;
    g_sealed      = false;
    g_detourCount = 0;
    std::memset(reinterpret_cast<void*>(g_codeArena), 0xCC, kCodeArenaSize);
    return true;
}

std::uintptr_t ArenaBase() { return g_codeArena; }

void ResetUnsealed() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_codeArena || g_sealed) return;
    g_codeUsed    = 0;
    g_detourCount = 0;
    std::memset(g_detours, 0, sizeof(g_detours));
    std::memset(reinterpret_cast<void*>(g_codeArena), 0xCC, kCodeArenaSize);
}

bool PrepareDetour(std::uintptr_t site,
                   std::size_t prologueLen,
                   void* detourFn,
                   void** outOrigTrampoline,
                   const std::uint8_t* expectedPrologue) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_codeArena || g_sealed || g_detourCount >= kMaxDetours ||
        !site || !detourFn || !outOrigTrampoline || !expectedPrologue ||
        prologueLen < 5U || prologueLen > 16U) {
        return false;
    }

    const std::size_t alignedOff = (g_codeUsed + 63U) & ~std::size_t(63U);
    if (alignedOff + 64U > kCodeArenaSize) return false;

    std::size_t used = 0;
    DetourEntry entry{};
    if (!EmitDetourSlot(site, prologueLen, detourFn, expectedPrologue,
                        g_codeArena + alignedOff, used, entry,
                        outOrigTrampoline, nullptr, nullptr)) {
        return false;
    }

    g_codeUsed = alignedOff + used;
    g_detours[g_detourCount++] = entry;
    return true;
}

bool SealAndActivateAll() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_codeArena || g_sealed || g_detourCount == 0) return false;

    DWORD oldProt = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(g_codeArena), kCodeArenaSize,
                          PAGE_EXECUTE_READ, &oldProt)) {
        return false;
    }
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(g_codeArena), kCodeArenaSize);
    g_sealed = true;

    for (std::size_t i = 0; i < g_detourCount; ++i) {
        DetourEntry& d = g_detours[i];
        if (!d.valid || !d.site) continue;
        if (!CompareAndPatch5BytesSafe(d.site, d.orig5, d.jmp5)) {
            // Another plugin concurrently patched `d.site` between Prepare and Seal — roll back cleanly!
            for (std::size_t r = i; r > 0; --r) {
                DetourEntry& prev = g_detours[r - 1U];
                if (prev.active && prev.site) {
                    CompareAndPatch5BytesSafe(prev.site, prev.jmp5, prev.orig5);
                    prev.active = false;
                }
            }
            // Unseal & reset arena so next retry re-prepares cleanly against the updated bytes
            DWORD tmp = 0;
            if (::VirtualProtect(reinterpret_cast<void*>(g_codeArena), kCodeArenaSize, PAGE_READWRITE, &tmp)) {
                g_sealed      = false;
                g_codeUsed    = 0;
                g_detourCount = 0;
                std::memset(g_detours, 0, sizeof(g_detours));
                std::memset(reinterpret_cast<void*>(g_codeArena), 0xCC, kCodeArenaSize);
            }
            return false;
        }
        d.active = true;
    }
    return true;
}

bool InstallStandaloneDetour5(std::uintptr_t site,
                              void* detourFn,
                              void** outOrigTrampoline,
                              const std::uint8_t expected5[5],
                              bool* outWasChained,
                              std::uintptr_t* outChainedTarget,
                              std::uintptr_t* outRelay) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!site || !detourFn || !outOrigTrampoline || !expected5 ||
        g_standaloneCount >= kMaxStandalone || !g_moduleLow || g_moduleHigh <= g_moduleLow) {
        return false;
    }

    // Allocate a dedicated W^X page for this standalone detour (never flips an already-executing page back to RW).
    const std::uintptr_t page = AllocNearPageReadWrite(g_moduleLow, g_moduleHigh);
    if (!page) return false;
    std::memset(reinterpret_cast<void*>(page), 0xCC, kCodeArenaSize);

    std::size_t used = 0;
    DetourEntry entry{};
    void* trampPtr = nullptr;
    bool chained = false;
    std::uintptr_t prevTarget = 0;

    if (!EmitDetourSlot(site, 5U, detourFn, expected5, page, used, entry,
                        &trampPtr, &chained, &prevTarget)) {
        ::VirtualFree(reinterpret_cast<void*>(page), 0, MEM_RELEASE);
        return false;
    }

    DWORD oldProt = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(page), kCodeArenaSize, PAGE_EXECUTE_READ, &oldProt)) {
        ::VirtualFree(reinterpret_cast<void*>(page), 0, MEM_RELEASE);
        return false;
    }
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(page), kCodeArenaSize);

    // Publish trampoline pointer atomically BEFORE patching `site` so any thread entering `detourFn`
    // immediately sees the valid trampoline; restore previous value if CAS detects a concurrent patch.
    void* const prevPublished = *outOrigTrampoline;
    _InterlockedExchangePointer(reinterpret_cast<void* volatile*>(outOrigTrampoline), trampPtr);

    if (!CompareAndPatch5BytesSafe(site, entry.orig5, entry.jmp5)) {
        _InterlockedExchangePointer(reinterpret_cast<void* volatile*>(outOrigTrampoline), prevPublished);
        if (prevPublished == nullptr) {
            ::VirtualFree(reinterpret_cast<void*>(page), 0, MEM_RELEASE);
        }
        return false;
    }

    entry.active = true;
    g_standalone[g_standaloneCount++] = entry;

    if (outWasChained)    *outWasChained    = chained;
    if (outChainedTarget) *outChainedTarget = prevTarget;
    if (outRelay)         *outRelay         = entry.relay;
    return true;
}

bool WasUnchainedOverwrite5(std::uintptr_t site,
                            std::uintptr_t ourRelay,
                            const std::uint8_t expected5[5]) noexcept {
    if (!site || !ourRelay || !expected5) return false;
    __try {
        const auto* cur = reinterpret_cast<const std::uint8_t*>(site);
        if (std::memcmp(cur, expected5, 5) == 0) {
            return true; // Restored back to vanilla bytes
        }
        if (cur[0] != 0xE9) {
            return false; // Unknown non-E9 patch; don't disturb
        }
        std::int32_t rel = 0;
        std::memcpy(&rel, cur + 1U, 4);
        const auto target = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(site + 5U) + rel);
        if (target == ourRelay) {
            return false; // Our hook is still at the head of `site`
        }
        // Check if the newer hooker uses the relay+16 trampoline layout (e.g. mhw_hfi / sibling hookers):
        // If [target+16 .. target+21) equals `expected5`, it built a vanilla trampoline without chaining to us!
        const auto* trampBytes = reinterpret_cast<const std::uint8_t*>(target + 16U);
        if (std::memcmp(trampBytes, expected5, 5) == 0) {
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RemoveAllHooks() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (std::size_t i = g_standaloneCount; i > 0; --i) {
        DetourEntry& d = g_standalone[i - 1U];
        if (!d.valid || !d.active || !d.site) continue;
        if (!CompareAndPatch5BytesSafe(d.site, d.jmp5, d.orig5)) {
            d.externallyChained = true;
        }
        d.active = false;
    }
    for (std::size_t i = g_detourCount; i > 0; --i) {
        DetourEntry& d = g_detours[i - 1U];
        if (!d.valid || !d.active || !d.site) continue;
        if (!CompareAndPatch5BytesSafe(d.site, d.jmp5, d.orig5)) {
            d.externallyChained = true;
        }
        d.active = false;
    }
}

} // namespace hook
