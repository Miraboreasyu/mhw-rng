#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "hook.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace hook {
namespace {

constexpr std::size_t   kCodePageSize      = 0x1000;  // 4 KB PAGE_EXECUTE_READ page (strict W^X)
constexpr std::size_t   kArenaReservePages = 16;      // 64 KB single OS reservation: Page 0 core, Pages 1..15 standalone pool
constexpr std::size_t   kArenaReserveSize  = kCodePageSize * kArenaReservePages;
constexpr std::size_t   kMaxDetours        = 24;
constexpr std::size_t   kMaxStandalone     = kArenaReservePages - 1U;

struct DetourEntry {
    bool           valid             = false;
    bool           active            = false;
    bool           externallyChained = false;
    bool           fromReservedPool  = false;
    std::uintptr_t site              = 0;
    std::uintptr_t relay             = 0;
    std::uint8_t   orig5[5]{};
    std::uint8_t   jmp5[5]{};
};

std::uintptr_t g_moduleLow        = 0;
std::uintptr_t g_moduleHigh       = 0;
std::uintptr_t g_arenaReserveBase = 0;
std::uint32_t  g_reservedPageMask = 0; // Bit 0 = Page 0 core arena; bits 1..15 = standalone detour pages
std::uintptr_t g_codeArena        = 0;
std::size_t    g_codeUsed         = 0;
bool           g_sealed           = false;
DetourEntry    g_detours[kMaxDetours]{};
std::size_t    g_detourCount      = 0;

DetourEntry    g_standalone[kMaxStandalone]{};
std::size_t    g_standaloneCount  = 0;

std::mutex     g_mx;

bool InRel32(std::uintptr_t from, std::uintptr_t to) noexcept {
    const std::int64_t d = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    return d >= INT32_MIN && d <= INT32_MAX;
}

bool TryReadPtrSEH(std::uintptr_t addr, std::uintptr_t& outVal) noexcept {
    __try {
        outVal = *reinterpret_cast<const std::uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void EmitAbsJmp14(std::uint8_t* dst, std::uintptr_t target) noexcept {
    dst[0] = 0xFF;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    const auto t64 = static_cast<std::uint64_t>(target);
    std::memcpy(dst + 6U, &t64, 8);
}

std::uintptr_t TryAllocRegionAt(std::uintptr_t addr,
                                std::size_t size,
                                DWORD allocType,
                                DWORD prot) noexcept {
    void* p = ::VirtualAlloc(reinterpret_cast<void*>(addr), size, allocType, prot);
    return reinterpret_cast<std::uintptr_t>(p);
}

std::uintptr_t ReserveNearRegion(std::uintptr_t minAddr,
                                 std::uintptr_t maxAddr,
                                 std::uintptr_t anchor,
                                 std::size_t allocSize,
                                 bool commitImmediately) noexcept {
    if (allocSize == 0 || maxAddr <= minAddr + allocSize) return 0;

    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const std::uintptr_t gran  = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000ULL;
    const std::uintptr_t sysLo = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const std::uintptr_t sysHi = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    minAddr = (std::max<std::uintptr_t>)(minAddr, (std::max<std::uintptr_t>)(gran, sysLo));
    maxAddr = (std::min<std::uintptr_t>)(maxAddr, sysHi);
    if (maxAddr <= minAddr + allocSize) return 0;
    if (anchor < minAddr) anchor = minAddr;
    if (anchor > maxAddr) anchor = maxAddr;

    const DWORD allocType = commitImmediately ? (MEM_RESERVE | MEM_COMMIT) : MEM_RESERVE;
    const DWORD prot      = commitImmediately ? PAGE_READWRITE : PAGE_NOACCESS;

    // 1. Scan downward from anchor using VirtualQuery (no arbitrary low iter cap under heavy VAD fragmentation)
    std::uintptr_t cur = anchor;
    for (int iter = 0; iter < 1000000 && cur > minAddr; ++iter) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(cur - 1U), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const auto regBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regEnd  = regBase + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const std::uintptr_t lo = (std::max)(minAddr, regBase);
            const std::uintptr_t hi = (std::min)(cur, regEnd);
            if (hi >= lo + allocSize) {
                std::uintptr_t cand = (hi - allocSize) & ~(gran - 1ULL);
                for (int tries = 0; tries < 4096 && cand >= lo; ++tries) {
                    if (cand + allocSize <= hi) {
                        if (std::uintptr_t p = TryAllocRegionAt(cand, allocSize, allocType, prot)) {
                            return p;
                        }
                    }
                    if (cand < gran) break;
                    cand -= gran;
                }
            }
        }
        if (regBase <= minAddr || regBase >= cur) break;
        cur = regBase;
    }

    // 2. Scan upward from anchor using VirtualQuery
    cur = anchor;
    for (int iter = 0; iter < 1000000 && cur + allocSize <= maxAddr; ++iter) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const auto regBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regEnd  = regBase + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const std::uintptr_t lo = (std::max)(minAddr, regBase);
            const std::uintptr_t hi = (std::min)(maxAddr, regEnd);
            std::uintptr_t cand = (lo + gran - 1ULL) & ~(gran - 1ULL);
            for (int tries = 0; tries < 4096 && cand + allocSize <= hi; ++tries) {
                if (std::uintptr_t p = TryAllocRegionAt(cand, allocSize, allocType, prot)) {
                    return p;
                }
                cand += gran;
            }
        }
        if (regEnd <= cur || regEnd >= maxAddr) break;
        cur = regEnd;
    }

    // 3. Fallback: let the OS pick unaligned/sub-granularity free slots and validate [minAddr, maxAddr)
    void* rejected[32]{};
    int rejectedCount = 0;
    std::uintptr_t chosen = 0;
    for (int probe = 0; probe < 32; ++probe) {
        void* p = ::VirtualAlloc(nullptr, allocSize, allocType, prot);
        if (!p) break;
        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        if (addr >= minAddr && addr + allocSize <= maxAddr) {
            chosen = addr;
            break;
        }
        rejected[rejectedCount++] = p;
    }
    for (int i = 0; i < rejectedCount; ++i) {
        ::VirtualFree(rejected[i], 0, MEM_RELEASE);
    }
    return chosen;
}

std::uintptr_t AllocStandalonePageFromPool(std::uintptr_t site) noexcept {
    if (!g_arenaReserveBase) return 0;
    for (std::size_t idx = 1U; idx < kArenaReservePages; ++idx) {
        if ((g_reservedPageMask & (1U << idx)) != 0U) continue;
        const std::uintptr_t page = g_arenaReserveBase + idx * kCodePageSize;
        if (!InRel32(site + 5U, page)) continue;
        void* p = ::VirtualAlloc(reinterpret_cast<void*>(page), kCodePageSize,
                                 MEM_COMMIT, PAGE_READWRITE);
        if (!p) continue;
        g_reservedPageMask |= (1U << idx);
        return page;
    }
    return 0;
}

void ReleaseStandalonePage(std::uintptr_t page, bool fromReservedPool) noexcept {
    if (!page) return;
    if (fromReservedPool) {
        if (g_arenaReserveBase && page >= g_arenaReserveBase + kCodePageSize &&
            page < g_arenaReserveBase + kArenaReserveSize) {
            const std::size_t idx = (page - g_arenaReserveBase) / kCodePageSize;
            ::VirtualFree(reinterpret_cast<void*>(page), kCodePageSize, MEM_DECOMMIT);
            g_reservedPageMask &= ~(1U << idx);
        }
        return;
    }
    ::VirtualFree(reinterpret_cast<void*>(page), 0, MEM_RELEASE);
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
    const bool isAlreadyRelJmp = (cur[0] == 0xE9);
    const bool isAlreadyIndJmp = (prologueLen == 5U && cur[0] == 0xFF && cur[1] == 0x25);
    if (!matchesExpected && !isAlreadyRelJmp && !isAlreadyIndJmp) {
        return false;
    }

    if (!InRel32(site + 5U, slotBase)) return false;

    auto* out = reinterpret_cast<std::uint8_t*>(slotBase);
    // [0..13] Relay: FF 25 00 00 00 00 <u64 detourFn>
    EmitAbsJmp14(out, reinterpret_cast<std::uintptr_t>(detourFn));

    // [16..] Trampoline
    const std::uintptr_t trampAddr = slotBase + 16U;
    auto* tramp = out + 16U;

    bool chained = false;
    std::uintptr_t prevTarget = 0;

    if (isAlreadyRelJmp && !matchesExpected) {
        chained = true;
        std::int32_t prevRel = 0;
        std::memcpy(&prevRel, cur + 1U, 4);
        prevTarget = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(site + 5U) + prevRel);
        // Use 14B abs jump so chaining works regardless of prevTarget distance from `slotBase`
        EmitAbsJmp14(tramp, prevTarget);
        outBytesUsed = 32U;
    } else if (isAlreadyIndJmp && !matchesExpected) {
        chained = true;
        std::int32_t disp = 0;
        std::memcpy(&disp, cur + 2U, 4);
        const std::uintptr_t ptrAddr = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(site + 6U) + disp);
        if (!TryReadPtrSEH(ptrAddr, prevTarget) ||
            prevTarget < 0x10000ULL || prevTarget > 0x00007FFFFFFF0000ULL) {
            return false;
        }
        EmitAbsJmp14(tramp, prevTarget);
        outBytesUsed = 32U;
    } else {
        if (!CopyAndRelocatePrologue(site, expectedPrologue, prologueLen, trampAddr, tramp)) {
            return false;
        }
        // Use 14B abs jump back to `site + prologueLen` (16 + <=16 + 14 <= 46B < 64B slot stride)
        EmitAbsJmp14(tramp + prologueLen, site + prologueLen);
        outBytesUsed = 16U + prologueLen + 14U;
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
    if (!lo || hi <= lo) return false;

    const std::uintptr_t minAddr = (hi > 0x7E000000ULL) ? (hi - 0x7E000000ULL) : 0x10000ULL;
    const std::uintptr_t maxAddr = lo + 0x7E000000ULL;
    const std::uintptr_t reserve = ReserveNearRegion(
        minAddr, maxAddr, lo, kArenaReserveSize, /*commitImmediately=*/false);
    if (!reserve) return false;

    void* page0 = ::VirtualAlloc(reinterpret_cast<void*>(reserve), kCodePageSize,
                                 MEM_COMMIT, PAGE_READWRITE);
    if (!page0) {
        ::VirtualFree(reinterpret_cast<void*>(reserve), 0, MEM_RELEASE);
        return false;
    }

    g_arenaReserveBase = reserve;
    g_codeArena        = reserve;
    g_reservedPageMask = 1U;
    g_codeUsed         = 0;
    g_sealed           = false;
    g_detourCount      = 0;
    std::memset(reinterpret_cast<void*>(g_codeArena), 0xCC, kCodePageSize);
    return true;
}

std::uintptr_t ArenaBase() { return g_codeArena; }

void ResetUnsealed() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_codeArena || g_sealed) return;
    g_codeUsed    = 0;
    g_detourCount = 0;
    std::memset(g_detours, 0, sizeof(g_detours));
    std::memset(reinterpret_cast<void*>(g_codeArena), 0xCC, kCodePageSize);
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
    if (alignedOff + 64U > kCodePageSize) return false;

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
    if (!::VirtualProtect(reinterpret_cast<void*>(g_codeArena), kCodePageSize,
                          PAGE_EXECUTE_READ, &oldProt)) {
        return false;
    }
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(g_codeArena), kCodePageSize);
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
            if (::VirtualProtect(reinterpret_cast<void*>(g_codeArena), kCodePageSize, PAGE_READWRITE, &tmp)) {
                g_sealed      = false;
                g_codeUsed    = 0;
                g_detourCount = 0;
                std::memset(g_detours, 0, sizeof(g_detours));
                std::memset(reinterpret_cast<void*>(g_codeArena), 0xCC, kCodePageSize);
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
                              std::uintptr_t* outRelay,
                              char* outErrBuf,
                              std::size_t errBufCap) {
    auto setErr = [&](const char* stage) {
        if (!outErrBuf || errBufCap == 0U) return;
        std::uint8_t b[8]{};
        __try {
            if (site) std::memcpy(b, reinterpret_cast<const void*>(site), 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        std::snprintf(outErrBuf, errBufCap,
                      "%s (gle=%lu, cur=%02X %02X %02X %02X %02X %02X)",
                      stage, ::GetLastError(), b[0], b[1], b[2], b[3], b[4], b[5]);
    };

    std::lock_guard<std::mutex> lk(g_mx);
    if (!site || !detourFn || !outOrigTrampoline || !expected5 ||
        g_standaloneCount >= kMaxStandalone || !g_moduleLow || g_moduleHigh <= g_moduleLow) {
        setErr("invalid-args");
        return false;
    }

    // Prefer a pre-reserved page in the 64 KB boot pool (0 extra VA, immune to late +-2GB exhaustion).
    bool fromPool = false;
    std::uintptr_t page = AllocStandalonePageFromPool(site);
    if (page != 0) {
        fromPool = true;
    } else {
        const std::uintptr_t minAddr = (site > 0x7E000000ULL) ? (site - 0x7E000000ULL) : 0x10000ULL;
        const std::uintptr_t maxAddr = site + 0x7E000000ULL;
        page = ReserveNearRegion(minAddr, maxAddr, site, kCodePageSize, /*commitImmediately=*/true);
    }
    if (!page) {
        setErr("alloc-near-failed");
        return false;
    }
    std::memset(reinterpret_cast<void*>(page), 0xCC, kCodePageSize);

    std::size_t used = 0;
    DetourEntry entry{};
    void* trampPtr = nullptr;
    bool chained = false;
    std::uintptr_t prevTarget = 0;

    if (!EmitDetourSlot(site, 5U, detourFn, expected5, page, used, entry,
                        &trampPtr, &chained, &prevTarget)) {
        setErr("emit-mismatch");
        ReleaseStandalonePage(page, fromPool);
        return false;
    }

    DWORD oldProt = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(page), kCodePageSize, PAGE_EXECUTE_READ, &oldProt)) {
        setErr("protect-page-failed");
        ReleaseStandalonePage(page, fromPool);
        return false;
    }
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(page), kCodePageSize);

    // Publish trampoline pointer atomically BEFORE patching `site` so any thread entering `detourFn`
    // immediately sees the valid trampoline; restore previous value if CAS detects a concurrent patch.
    void* const prevPublished = *outOrigTrampoline;
    _InterlockedExchangePointer(reinterpret_cast<void* volatile*>(outOrigTrampoline), trampPtr);

    if (!CompareAndPatch5BytesSafe(site, entry.orig5, entry.jmp5)) {
        setErr("cas-patch-failed");
        _InterlockedExchangePointer(reinterpret_cast<void* volatile*>(outOrigTrampoline), prevPublished);
        if (prevPublished == nullptr) {
            ReleaseStandalonePage(page, fromPool);
        }
        return false;
    }

    entry.active           = true;
    entry.fromReservedPool = fromPool;
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
