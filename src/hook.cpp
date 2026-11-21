#include "hook.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <mutex>

namespace hook {
namespace {

constexpr std::size_t kCodeArenaSize = 0x1000; // Single 4 KB PAGE_EXECUTE_READ arena
constexpr std::size_t kMaxDetours    = 24;

struct DetourEntry {
    bool           valid             = false;
    bool           active            = false;
    bool           externallyChained = false;
    std::uintptr_t site              = 0;
    std::uintptr_t relay             = 0;
    std::uint8_t   orig5[5]{};
    std::uint8_t   jmp5[5]{};
};

std::uintptr_t g_codeArena   = 0;
std::size_t    g_codeUsed    = 0;
bool           g_sealed      = false;
DetourEntry    g_detours[kMaxDetours]{};
std::size_t    g_detourCount = 0;
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

bool Patch5BytesSafe(std::uintptr_t site, const std::uint8_t fiveBytes[5]) noexcept {
    DWORD oldProt = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(site), 8, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    if ((site & 0x7ULL) == 0) {
        auto* qwordPtr = reinterpret_cast<volatile LONG64*>(site);
        LONG64 cur = *qwordPtr;
        for (;;) {
            LONG64 desired = cur;
            std::memcpy(&desired, fiveBytes, 5);
            const LONG64 prev = _InterlockedCompareExchange64(qwordPtr, desired, cur);
            if (prev == cur) break;
            cur = prev;
        }
    } else {
        // Unaligned site (e.g. 0x142C58034): patched once during startup/detach under VirtualProtect
        std::memcpy(reinterpret_cast<void*>(site), fiveBytes, 5);
    }
    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(site), 8, oldProt, &tmp);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(site), 8);
    return true;
}

// Relocates any displaced E8 rel32 CALL within `expectedPrologue[0..prologueLen)` (e.g. CRT_rand +4)
bool CopyAndRelocatePrologue(std::uintptr_t site,
                             const std::uint8_t* srcBytes,
                             std::size_t len,
                             std::uintptr_t trampAddr,
                             std::uint8_t* dstTramp) noexcept {
    std::memcpy(dstTramp, srcBytes, len);
    // Check if an E8 rel32 instruction ends at `len` (specifically CRT_rand: 4B sub rsp + 5B call rel32)
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

} // namespace

bool InitArena(std::uintptr_t lo, std::uintptr_t hi) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_codeArena) return true;

    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const std::uintptr_t span = 0x7F000000ULL;

    for (std::uintptr_t a = (lo - gran) & ~(gran - 1); a > gran && lo - a < span; a -= gran) {
        if (!InRel32(a, hi) || !InRel32(a + kCodeArenaSize, lo)) continue;
        if (std::uintptr_t p = TryAllocReadWriteAt(a, kCodeArenaSize)) {
            g_codeArena = p;
            break;
        }
    }
    if (!g_codeArena) {
        for (std::uintptr_t a = (hi + gran) & ~(gran - 1); a - hi < span; a += gran) {
            if (!InRel32(lo, a + kCodeArenaSize)) break;
            if (std::uintptr_t p = TryAllocReadWriteAt(a, kCodeArenaSize)) {
                g_codeArena = p;
                break;
            }
        }
    }
    if (!g_codeArena) {
        for (int i = 0; i < 64 && !g_codeArena; ++i) {
            void* p = ::VirtualAlloc(nullptr, kCodeArenaSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!p) break;
            const auto a = reinterpret_cast<std::uintptr_t>(p);
            if (InRel32(a, hi) && InRel32(a + kCodeArenaSize, lo)) g_codeArena = a;
            else ::VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    if (!g_codeArena) return false;

    g_codeUsed    = 0;
    g_sealed      = false;
    g_detourCount = 0;
    std::memset(reinterpret_cast<void*>(g_codeArena), 0xCC, kCodeArenaSize);
    return true;
}

std::uintptr_t ArenaBase() { return g_codeArena; }

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

    const auto* cur = reinterpret_cast<const std::uint8_t*>(site);
    const bool matchesExpected = (std::memcmp(cur, expectedPrologue, prologueLen) == 0);
    const bool isAlreadyJmp    = (cur[0] == 0xE9);
    if (!matchesExpected && !isAlreadyJmp) {
        return false;
    }

    const std::size_t alignedOff = (g_codeUsed + 63U) & ~std::size_t(63U);
    if (alignedOff + 64U > kCodeArenaSize) return false;

    const std::uintptr_t base = g_codeArena + alignedOff;
    if (!InRel32(site + 5U, base)) return false;

    auto* out = reinterpret_cast<std::uint8_t*>(base);

    // [0..13] Relay: FF 25 00 00 00 00 <u64 detourFn>
    out[0] = 0xFF; out[1] = 0x25;
    out[2] = 0x00; out[3] = 0x00; out[4] = 0x00; out[5] = 0x00;
    const auto fnAddr = reinterpret_cast<std::uint64_t>(detourFn);
    std::memcpy(out + 6U, &fnAddr, 8);

    // [16..] Trampoline
    const std::uintptr_t trampAddr = base + 16U;
    auto* tramp = out + 16U;

    if (isAlreadyJmp && !matchesExpected) {
        // Chain cleanly onto existing plugin's E9 rel32 hook
        std::int32_t prevRel = 0;
        std::memcpy(&prevRel, cur + 1U, 4);
        const auto prevTarget = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(site + 5U) + prevRel);
        if (!InRel32(trampAddr + 5U, prevTarget)) return false;

        tramp[0] = 0xE9;
        const auto chainRel = static_cast<std::int32_t>(
            static_cast<std::int64_t>(prevTarget) - static_cast<std::int64_t>(trampAddr + 5U));
        std::memcpy(tramp + 1U, &chainRel, 4);
        g_codeUsed = alignedOff + 24U;
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
        g_codeUsed = alignedOff + 16U + prologueLen + 5U;
    }

    DetourEntry& d = g_detours[g_detourCount++];
    d.valid             = true;
    d.active            = false;
    d.externallyChained = false;
    d.site              = site;
    d.relay             = base;
    std::memcpy(d.orig5, cur, 5);

    d.jmp5[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(
        static_cast<std::int64_t>(base) - static_cast<std::int64_t>(site + 5U));
    std::memcpy(d.jmp5 + 1U, &rel, 4);

    *outOrigTrampoline = reinterpret_cast<void*>(trampAddr);
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
        if (!Patch5BytesSafe(d.site, d.jmp5)) {
            for (std::size_t r = i; r > 0; --r) {
                DetourEntry& prev = g_detours[r - 1U];
                if (prev.active && prev.site) {
                    Patch5BytesSafe(prev.site, prev.orig5);
                    prev.active = false;
                }
            }
            return false;
        }
        d.active = true;
    }
    return true;
}

void RemoveAllHooks() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (std::size_t i = g_detourCount; i > 0; --i) {
        DetourEntry& d = g_detours[i - 1U];
        if (!d.valid || !d.active || !d.site) continue;
        const auto* cur = reinterpret_cast<const std::uint8_t*>(d.site);
        if (std::memcmp(cur, d.jmp5, 5) == 0) {
            Patch5BytesSafe(d.site, d.orig5);
        } else {
            d.externallyChained = true;
        }
        d.active = false;
    }
}

} // namespace hook
