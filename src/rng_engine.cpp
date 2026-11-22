#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#include "rng_engine.h"
#include "mhw_addresses.h"

#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

static_assert(sizeof(MhwRngCallerRegs) == 88, "MhwRngCallerRegs size mismatch with rng_trampolines.asm");
static_assert(offsetof(MhwRngCallerRegs, rbx)       == 0x00, "rbx offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, rbp)       == 0x08, "rbp offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, rsi)       == 0x10, "rsi offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, rdi)       == 0x18, "rdi offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, r12)       == 0x20, "r12 offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, r13)       == 0x28, "r13 offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, r14)       == 0x30, "r14 offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, r15)       == 0x38, "r15 offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, callerRsp) == 0x40, "callerRsp offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, callerRip) == 0x48, "callerRip offset mismatch");
static_assert(offsetof(MhwRngCallerRegs, reserved)  == 0x50, "reserved offset mismatch");

namespace rng {
namespace {

using MtCtorSeedFn   = std::uint32_t*(__fastcall*)(std::uint32_t*, std::uint32_t);
using MtCtorDefFn    = std::uint32_t*(__fastcall*)(std::uint32_t*);
using MtSrandFn      = void(__fastcall*)(std::uint32_t*, std::uint32_t);
using MtNrandFn      = std::uint32_t(__fastcall*)(std::uint32_t*);
using MtSphereFn     = float*(__fastcall*)(std::uint32_t*, float*);
using StdRandDevFn   = std::uint32_t(__cdecl*)();
using Mt19937RangeFn = float(__fastcall*)(void*, float, float);
using Mt19937InitFn  = void(__fastcall*)(void*, std::uint32_t);
using Mt19937DrawFn  = std::uint32_t(__fastcall*)(void*);
using CrtRandFn      = int(__cdecl*)();

constexpr float kFloatInv2Pow32     = 1.0f / 4294967296.0f;                  // 2^-32
constexpr float kFloatHalfStep2P32  = 0.5f / 4294967296.0f;                  // 2^-33
constexpr float kFloatTwoPiOver2P32 = 6.28318530717958647692f / 4294967296.0f; // 2*pi * 2^-32

inline std::uint32_t FloatToBits(float f) noexcept {
    std::uint32_t u = 0U;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline float BitsToFloat(std::uint32_t u) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline std::uint32_t Crc32Compute(const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            const std::uint32_t mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

// ============================================================================
//  Deterministic Sub-Stream Primitives (64-bit Composite `(condId, callSiteRva)`)
// ============================================================================

inline std::uint64_t MakeCompositeKey(std::uint32_t rva, std::uint32_t condId) noexcept {
    return (static_cast<std::uint64_t>(condId) << 32) | static_cast<std::uint64_t>(rva);
}

inline std::uint64_t SplitMix64(std::uint64_t& z) noexcept {
    z += 0x9E3779B97F4A7C15ULL;
    std::uint64_t x = z;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

inline void DeriveXorshift128State(std::uint64_t seed,
                                   std::uint64_t compositeKey,
                                   std::uint32_t outState[4]) noexcept {
    const std::uint64_t rvaPart  = compositeKey & 0xFFFFFFFFULL;
    const std::uint64_t condPart = compositeKey >> 32;
    std::uint64_t z = seed ^ (rvaPart * 0xD6E8FEB86659FD93ULL) ^ (condPart * 0xA0761D6478BD642FULL);
    const std::uint64_t a = SplitMix64(z);
    const std::uint64_t b = SplitMix64(z);
    outState[0] = static_cast<std::uint32_t>(a);
    outState[1] = static_cast<std::uint32_t>(a >> 32);
    outState[2] = static_cast<std::uint32_t>(b);
    outState[3] = static_cast<std::uint32_t>(b >> 32);
    if ((outState[0] | outState[1] | outState[2] | outState[3]) == 0U) {
        outState[0] = 0x075BCD15U;
        outState[1] = 0x159A55E5U;
        outState[2] = 0x1F123BB5U;
        outState[3] = 0x05491333U;
    }
}

inline void InitMhwMtRandomWords(std::uint32_t dst[4], std::uint32_t seed) noexcept {
    std::uint32_t u = seed ^ 0x665926CAU;
    if (u == 0U) u = 0x665937CFU;
    dst[0] = 0x075BCD15U * u;
    dst[1] = 0x159A55E5U * u;
    dst[2] = 0x1F123BB5U * u;
    dst[3] = 0x05491333U * u;
}

inline std::uint32_t StepMhwXorshift128(std::uint32_t s[4]) noexcept {
    std::uint32_t t = (s[0] << 15) ^ s[0];
    s[0] = s[1];
    s[1] = s[2];
    const std::uint32_t w = s[3];
    s[2] = w;
    t = ((w >> 17) ^ t) >> 4 ^ w ^ t;
    s[3] = t;
    return t;
}

// ============================================================================
//  Composite Slot Table v3 (16K Tombstone-Capable + Heap Reclaimable Seq)
// ============================================================================

constexpr std::uint32_t kSlotTableSize      = 16384U;
constexpr std::uint32_t kSlotTableMask      = kSlotTableSize - 1U;
constexpr std::uint64_t kSlotKeyEmpty       = 0ULL;
constexpr std::uint64_t kSlotKeyTombstone   = 0xFFFFFFFFFFFFFFFFULL;
constexpr std::uint32_t kInlineSeqCap       = 8U;

constexpr std::uint16_t kInvalidChainIdx    = 0xFFFFU;
constexpr std::uint32_t kMaxSlotHooks       = 256U;
constexpr std::uint32_t kMaxHookChains      = 1024U;
constexpr std::uint32_t kMaxHooksPerRva     = 8U;
constexpr std::uint32_t kMaxGlobalObservers = 16U;
constexpr std::uint32_t kMaxWrappers        = 64U;
constexpr std::uint32_t kMaxInstanceTags    = 512U;

struct SlotHookEntry {
    bool             active   = false;
    MhwRngHookHandle handle   = 0U;
    MhwRngPluginId   ownerId  = 0U;
    std::uint32_t    rva      = 0U;
    std::int32_t     priority = 0;
    std::uint32_t    flags    = 0U;
    std::uint64_t    regSeq   = 0ULL;
    MhwRngSlotHookFn preFn    = nullptr;
    MhwRngSlotHookFn postFn   = nullptr;
    void*            userData = nullptr;
};

struct RvaHookChain {
    bool           inUse        = false;
    MhwRngPluginId firstOwnerId = kMhwRngPluginIdKernel;
    std::uint32_t  rva          = 0U;
    std::uint32_t  count        = 0U;
    bool           hasPre       = false;
    bool           hasPost      = false;
    bool           hasFilter    = false;
    std::uint16_t  entryIdx[kMaxHooksPerRva]{};
};

struct GlobalObserverEntry {
    bool             active     = false;
    MhwRngHookHandle handle     = 0U;
    MhwRngPluginId   ownerId    = 0U;
    std::uint32_t    rvaFilter  = 0U;
    std::uint32_t    sourceMask = 0U;
    MhwRngObserverFn fn         = nullptr;
    void*            userData   = nullptr;
};

struct GlobalObserverSnapshot {
    std::uint32_t       count = 0U;
    GlobalObserverEntry entries[kMaxGlobalObservers]{};
};

#pragma warning(push)
#pragma warning(disable : 4324)
// Keep ALL fields zero-initialized by default so `g_slots[16384]` (~2MB) lives in `.bss`, NOT `.data` on disk!
struct alignas(64) RngSlot {
    std::atomic<std::uint64_t>  key{kSlotKeyEmpty}; // (condId << 32) | rva; 0=empty; ~0=tombstone
    std::atomic_flag            lock = ATOMIC_FLAG_INIT;
    std::uint8_t                mode             = 0; // kMhwRngSlotMasterDerived
    std::uint8_t                sourceKind       = 0; // kMhwRngSrcUnknown
    std::uint8_t                seqEndPolicy     = 0; // kMhwRngSeqLoop
    std::uint8_t                useHeapSeq       = 0; // 0 = inlineSeq, 1 = seqHeap
    MhwRngPluginId              ownerPluginId    = 0;
    std::atomic<std::uint16_t>  hookChainIdxPlus1{0}; // 0 = none, >0 => chain index + 1 (base slot only)
    std::atomic<std::uint32_t>  activeCond{0};        // Base slot (condId == 0)

    std::uint32_t               state[4]{};
    std::uint64_t               seededEpoch      = 0;
    std::uint64_t               customSeed       = 0;
    std::uint32_t               fixedValue       = 0;
    std::uint32_t               lastValue        = 0;

    std::uint64_t               totalCalls       = 0;
    std::uint64_t               epochCalls       = 0;

    std::uint32_t               seqLength        = 0;
    std::uint32_t               seqCursor        = 0;
    std::uint32_t*              seqHeap          = nullptr; // Reclaimable heap sequence for count > 8
    std::uint32_t               inlineSeq[kInlineSeqCap]{};
};
#pragma warning(pop)

inline std::uint16_t DecodeChainIdx(std::uint16_t rawPlus1) noexcept {
    return (rawPlus1 > 0U && rawPlus1 <= kMaxHookChains)
               ? static_cast<std::uint16_t>(rawPlus1 - 1U)
               : kInvalidChainIdx;
}

struct SlotSpinGuard {
    std::atomic_flag& flag;
    explicit SlotSpinGuard(std::atomic_flag& f) noexcept : flag(f) {
        while (flag.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }
    }
    ~SlotSpinGuard() noexcept {
        flag.clear(std::memory_order_release);
    }
};

RngSlot                    g_slots[kSlotTableSize]{};
std::mutex                 g_slotTableMutMx; // Serializes cold insertion & tombstone deletion
std::atomic<std::uint32_t> g_activeSlotCount{0};

SlotHookEntry              g_hookEntries[kMaxSlotHooks]{};
RvaHookChain               g_hookChains[kMaxHookChains]{};
std::mutex                 g_hookRegMx;
std::atomic<std::uint32_t> g_inFlightHookCalls{0};
std::uint32_t              g_nextHandleGen = 1U;
std::uint64_t              g_nextRegSeq    = 1ULL;

GlobalObserverEntry        g_obsStorage[kMaxGlobalObservers]{};
GlobalObserverSnapshot     g_obsSnaps[2]{};
std::atomic<std::uint32_t> g_obsActiveIdx{0U};
std::atomic<std::uint32_t> g_obsCount{0U};

// Dynamic transparent wrapper table (packed atomic u64: rva | frameQwords<<32 | ownerId<<48)
std::atomic<std::uint64_t> g_wrappers[kMaxWrappers]{};
std::atomic<std::uint32_t> g_wrapperHighWater{0U};
std::mutex                 g_wrapperMx;

std::uintptr_t             g_exeBase       = 0;
std::uintptr_t             g_exeEnd        = 0;
std::uintptr_t             g_sInstanceAddr = 0;

std::atomic<std::uint32_t> g_engineMode{kMhwRngModeSelective};
std::atomic<bool>          g_protectSaveSeeds{true};
std::atomic<std::uint64_t> g_masterSeed{12345ULL};
std::atomic<std::uint64_t> g_seedEpoch{1ULL};

LifecycleEventSinkFn       g_lifecycleSink = nullptr;

MtCtorSeedFn               g_origMtCtorSeed   = nullptr;
MtCtorDefFn                g_origMtCtorDef    = nullptr;
MtSrandFn                  g_origMtSrand      = nullptr;
MtNrandFn                  g_origMtNrand      = nullptr;
MtSphereFn                 g_origMtSphere     = nullptr;
StdRandDevFn               g_origStdRandDev   = nullptr;
Mt19937RangeFn             g_origMt19937Range = nullptr;
Mt19937InitFn              g_origMt19937Init  = nullptr;
Mt19937DrawFn              g_origMt19937Draw  = nullptr;
CrtRandFn                  g_origCrtRand      = nullptr;

// 512-entry Private MtRandom instance tag & policy table
struct PrivateInstanceEntry {
    std::atomic<std::uintptr_t> ptr{0};
    std::atomic<std::uint64_t>  epoch{0};
    std::atomic<std::uint32_t>  policy{kMhwRngInstAuto};
};
PrivateInstanceEntry       g_instanceTags[kMaxInstanceTags]{};
std::atomic<std::uint32_t> g_tagRingCursor{0U};
std::mutex                 g_instPolicyMx;

void EmitLifecycleEvent(std::uint32_t eventKind, std::uint64_t prevVal) noexcept {
    LifecycleEventSinkFn sink = g_lifecycleSink;
    if (!sink) return;
    MhwRngEventInfo info{};
    info.eventKind  = eventKind;
    info.engineMode = g_engineMode.load(std::memory_order_acquire);
    info.masterSeed = g_masterSeed.load(std::memory_order_acquire);
    info.epoch      = g_seedEpoch.load(std::memory_order_acquire);
    info.prevValue  = prevVal;
    sink(&info);
}

void TagExplicitlySeededInstance(std::uintptr_t instancePtr, std::uint64_t epoch) noexcept {
    if (!instancePtr || instancePtr == g_sInstanceAddr) return;
    for (std::uint32_t i = 0; i < kMaxInstanceTags; ++i) {
        if (g_instanceTags[i].ptr.load(std::memory_order_relaxed) == instancePtr) {
            g_instanceTags[i].epoch.store(epoch, std::memory_order_release);
            return;
        }
    }
    std::lock_guard<std::mutex> lk(g_instPolicyMx);
    for (std::uint32_t i = 0; i < kMaxInstanceTags; ++i) {
        if (g_instanceTags[i].ptr.load(std::memory_order_relaxed) == instancePtr) {
            g_instanceTags[i].epoch.store(epoch, std::memory_order_release);
            return;
        }
    }
    std::uint32_t idx = g_tagRingCursor.fetch_add(1U, std::memory_order_relaxed) & (kMaxInstanceTags - 1U);
    for (std::uint32_t p = 0; p < kMaxInstanceTags; ++p) {
        const std::uint32_t cand = (idx + p) & (kMaxInstanceTags - 1U);
        if (g_instanceTags[cand].policy.load(std::memory_order_relaxed) == kMhwRngInstAuto) {
            idx = cand;
            break;
        }
    }
    g_instanceTags[idx].policy.store(kMhwRngInstAuto, std::memory_order_relaxed);
    g_instanceTags[idx].ptr.store(instancePtr, std::memory_order_relaxed);
    g_instanceTags[idx].epoch.store(epoch, std::memory_order_release);
}

void QueryInstanceTagInfo(std::uintptr_t instancePtr,
                          std::uint64_t curEpoch,
                          bool& outExplicitSeeded,
                          std::uint32_t& outPolicy) noexcept {
    outExplicitSeeded = false;
    outPolicy         = kMhwRngInstAuto;
    if (!instancePtr || instancePtr == g_sInstanceAddr) return;
    for (std::uint32_t i = 0; i < kMaxInstanceTags; ++i) {
        if (g_instanceTags[i].ptr.load(std::memory_order_relaxed) == instancePtr) {
            outPolicy         = g_instanceTags[i].policy.load(std::memory_order_acquire);
            outExplicitSeeded = (g_instanceTags[i].epoch.load(std::memory_order_acquire) == curEpoch);
            return;
        }
    }
}

bool TryReadStackSlotSEH(const std::uintptr_t* slotPtr, std::size_t idx, std::uintptr_t& outVal) noexcept {
    __try {
        outVal = slotPtr[idx];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline std::uint64_t PackWrapper(MhwRngPluginId ownerId, std::uint16_t frameQwords, std::uint32_t rva) noexcept {
    return (static_cast<std::uint64_t>(ownerId) << 48) |
           (static_cast<std::uint64_t>(frameQwords) << 32) |
           static_cast<std::uint64_t>(rva);
}

bool RegisterWrapperInternal(MhwRngPluginId ownerId, std::uint32_t normRva, std::uint16_t frameQwords) noexcept {
    if (normRva == 0U || frameQwords == 0U || frameQwords > 64U) return false;
    std::lock_guard<std::mutex> lk(g_wrapperMx);
    std::uint32_t freeIdx = kMaxWrappers;
    for (std::uint32_t i = 0; i < kMaxWrappers; ++i) {
        const std::uint64_t p = g_wrappers[i].load(std::memory_order_relaxed);
        if (p == 0ULL) {
            if (freeIdx == kMaxWrappers) freeIdx = i;
            continue;
        }
        if (static_cast<std::uint32_t>(p) == normRva) {
            g_wrappers[i].store(PackWrapper(ownerId, frameQwords, normRva), std::memory_order_release);
            return true;
        }
    }
    if (freeIdx >= kMaxWrappers) return false;
    g_wrappers[freeIdx].store(PackWrapper(ownerId, frameQwords, normRva), std::memory_order_release);
    const std::uint32_t curHi = g_wrapperHighWater.load(std::memory_order_relaxed);
    if (freeIdx + 1U > curHi) {
        g_wrapperHighWater.store(freeIdx + 1U, std::memory_order_release);
    }
    return true;
}

void RegisterBuiltinWrappers() noexcept {
    static constexpr std::uint32_t kBuiltinRvas[5] = {
        0x04F71BFU, 0x2396F02U, 0x2396F2DU, 0x197D6A2U, 0x197D6B5U
    };
    for (std::uint32_t r : kBuiltinRvas) {
        RegisterWrapperInternal(kMhwRngPluginIdKernel, r, 6U);
    }
}

inline std::uint32_t ComputeCallSiteRva(const void* retSlotAddr, std::uint32_t* outOrigRetRva) noexcept {
    const auto* slotPtr = static_cast<const std::uintptr_t*>(retSlotAddr);
    const std::uintptr_t ret  = *slotPtr;
    const std::uintptr_t base = g_exeBase;
    const std::uintptr_t end  = g_exeEnd;
    if (ret >= base && ret < end) {
        auto rva = static_cast<std::uint32_t>(ret - base);
        if (outOrigRetRva) *outOrigRetRva = rva;

        std::size_t stackOffset = 0U;
        const std::uint32_t hi  = g_wrapperHighWater.load(std::memory_order_acquire);
        for (int hop = 0; hop < 2; ++hop) {
            std::uint16_t matchedFq = 0U;
            for (std::uint32_t i = 0; i < hi; ++i) {
                const std::uint64_t p = g_wrappers[i].load(std::memory_order_relaxed);
                if (p != 0ULL && static_cast<std::uint32_t>(p) == rva) {
                    matchedFq = static_cast<std::uint16_t>((p >> 32) & 0xFFFFU);
                    break;
                }
            }
            if (matchedFq == 0U) break;
            stackOffset += matchedFq;
            std::uintptr_t outerRet = 0;
            if (!TryReadStackSlotSEH(slotPtr, stackOffset, outerRet) ||
                outerRet < base || outerRet >= end) {
                break;
            }
            rva = static_cast<std::uint32_t>(outerRet - base);
        }
        return rva != 0U ? rva : 1U;
    }
    if (outOrigRetRva) *outOrigRetRva = 0U;
    std::uint64_t h = ret * 0x9E3779B97F4A7C15ULL;
    h ^= (h >> 32);
    return 0x80000000U | (static_cast<std::uint32_t>(h) & 0x7FFFFFFFU) | 1U;
}

inline std::uint32_t HashCompositeKey(std::uint64_t key) noexcept {
    std::uint64_t x = key;
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return static_cast<std::uint32_t>(x);
}

RngSlot* FindExistingCompositeSlot(std::uint64_t compKey) noexcept {
    if (compKey == kSlotKeyEmpty || compKey == kSlotKeyTombstone) return nullptr;
    std::uint32_t idx = HashCompositeKey(compKey) & kSlotTableMask;
    for (std::uint32_t probe = 0; probe < kSlotTableSize; ++probe) {
        RngSlot& slot = g_slots[idx];
        const std::uint64_t curKey = slot.key.load(std::memory_order_acquire);
        if (curKey == compKey)      return &slot;
        if (curKey == kSlotKeyEmpty) return nullptr;
        idx = (idx + 1U) & kSlotTableMask;
    }
    return nullptr;
}

RngSlot* FindOrInsertCompositeSlot(std::uint64_t compKey) noexcept {
    if (compKey == kSlotKeyEmpty || compKey == kSlotKeyTombstone) return nullptr;
    if (RngSlot* existing = FindExistingCompositeSlot(compKey)) {
        return existing;
    }

    std::lock_guard<std::mutex> lk(g_slotTableMutMx);
    std::uint32_t idx            = HashCompositeKey(compKey) & kSlotTableMask;
    std::uint32_t firstTombstone = kSlotTableSize;
    std::uint32_t chosen         = kSlotTableSize;

    for (std::uint32_t probe = 0; probe < kSlotTableSize; ++probe) {
        const std::uint64_t curKey = g_slots[idx].key.load(std::memory_order_acquire);
        if (curKey == compKey) {
            return &g_slots[idx];
        }
        if (curKey == kSlotKeyTombstone) {
            if (firstTombstone == kSlotTableSize) firstTombstone = idx;
        } else if (curKey == kSlotKeyEmpty) {
            chosen = (firstTombstone < kSlotTableSize) ? firstTombstone : idx;
            break;
        }
        idx = (idx + 1U) & kSlotTableMask;
    }
    if (chosen >= kSlotTableSize && firstTombstone < kSlotTableSize) {
        chosen = firstTombstone;
    }
    if (chosen >= kSlotTableSize) return nullptr;

    RngSlot& s = g_slots[chosen];
    {
        SlotSpinGuard g(s.lock);
        s.mode          = kMhwRngSlotMasterDerived;
        s.sourceKind    = kMhwRngSrcUnknown;
        s.seqEndPolicy  = kMhwRngSeqLoop;
        s.useHeapSeq    = 0U;
        s.ownerPluginId = kMhwRngPluginIdKernel;
        s.hookChainIdxPlus1.store(0U, std::memory_order_relaxed);
        s.activeCond.store(0U, std::memory_order_relaxed);
        std::memset(s.state, 0, sizeof(s.state));
        s.seededEpoch   = 0ULL;
        s.customSeed    = 0ULL;
        s.fixedValue    = 0U;
        s.lastValue     = 0U;
        s.totalCalls    = 0ULL;
        s.epochCalls    = 0ULL;
        s.seqLength     = 0U;
        s.seqCursor     = 0U;
        s.seqHeap       = nullptr;
        std::memset(s.inlineSeq, 0, sizeof(s.inlineSeq));
        s.key.store(compKey, std::memory_order_release);
    }
    g_activeSlotCount.fetch_add(1U, std::memory_order_relaxed);
    return &s;
}

std::uint32_t __fastcall DrawDeterministicHelper(std::uint32_t callSiteRva, std::uint32_t condId) {
    const std::uint64_t compKey = MakeCompositeKey(callSiteRva, condId);
    RngSlot* slot = FindOrInsertCompositeSlot(compKey);
    if (!slot) return 0U;

    const std::uint64_t curEpoch   = g_seedEpoch.load(std::memory_order_acquire);
    const std::uint64_t masterSeed = g_masterSeed.load(std::memory_order_relaxed);

    SlotSpinGuard g(slot->lock);
    if (slot->key.load(std::memory_order_relaxed) != compKey) return 0U;
    if (slot->seededEpoch != curEpoch) {
        const std::uint64_t baseSeed = (slot->mode == kMhwRngSlotCustomSeed) ? slot->customSeed : masterSeed;
        DeriveXorshift128State(baseSeed, compKey, slot->state);
        slot->seqCursor   = 0U;
        slot->epochCalls  = 0ULL;
        slot->seededEpoch = curEpoch;
    }
    return StepMhwXorshift128(slot->state);
}

void InvokePluginSlotHookSEH(MhwRngSlotHookFn fn,
                             const MhwRngDrawContext* ctx,
                             MhwRngHookDecision* decision,
                             void* userData) noexcept {
    __try {
        fn(ctx, decision, userData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        decision->action = kMhwRngActionContinue;
        decision->condId = ctx->activeCondId;
    }
}

void InvokeGlobalObserverSEH(MhwRngObserverFn fn,
                             const MhwRngDrawContext* ctx,
                             void* userData) noexcept {
    __try {
        fn(ctx, userData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

inline std::uint32_t StepSlotLocked(RngSlot& slot,
                                    std::uint64_t compKey,
                                    std::uint64_t curEpoch,
                                    std::uint64_t masterSeed) noexcept {
    if (slot.seededEpoch != curEpoch) {
        if (slot.mode == kMhwRngSlotCustomSeed) {
            DeriveXorshift128State(slot.customSeed, compKey, slot.state);
        } else {
            DeriveXorshift128State(masterSeed, compKey, slot.state);
        }
        slot.seqCursor   = 0U;
        slot.epochCalls  = 0ULL;
        slot.seededEpoch = curEpoch;
    }

    ++slot.totalCalls;
    ++slot.epochCalls;

    std::uint32_t out = 0U;
    switch (slot.mode) {
        case kMhwRngSlotFixedValue:
            out = slot.fixedValue;
            break;

        case kMhwRngSlotSequence: {
            const std::uint32_t len = slot.seqLength;
            const std::uint32_t* seqBase = (slot.useHeapSeq == 0U) ? slot.inlineSeq : slot.seqHeap;
            if (len == 0U || !seqBase) {
                out = StepMhwXorshift128(slot.state);
                break;
            }
            if (slot.seqCursor < len) {
                out = seqBase[slot.seqCursor++];
                if (slot.seqCursor >= len && slot.seqEndPolicy == kMhwRngSeqLoop) {
                    slot.seqCursor = 0U;
                }
            } else if (slot.seqEndPolicy == kMhwRngSeqHoldLast) {
                out = seqBase[len - 1U];
            } else if (slot.seqEndPolicy == kMhwRngSeqLoop) {
                slot.seqCursor = 0U;
                out = seqBase[slot.seqCursor++];
                if (slot.seqCursor >= len) slot.seqCursor = 0U;
            } else {
                out = StepMhwXorshift128(slot.state);
            }
            break;
        }

        case kMhwRngSlotCustomSeed:
        case kMhwRngSlotMasterDerived:
        default:
            out = StepMhwXorshift128(slot.state);
            break;
    }

    slot.lastValue = out;
    return out;
}

inline void RecordSlotTelemetry(RngSlot* slot,
                                std::uint64_t expectedKey,
                                MhwRngSourceKind srcKind,
                                std::uint64_t curEpoch,
                                std::uint32_t lastVal) noexcept {
    if (!slot) return;
    SlotSpinGuard g(slot->lock);
    if (slot->key.load(std::memory_order_relaxed) != expectedKey) return;
    if (slot->sourceKind == kMhwRngSrcUnknown || srcKind != kMhwRngSrcUnknown) {
        slot->sourceKind = static_cast<std::uint8_t>(srcKind);
    }
    if (slot->seededEpoch != curEpoch) {
        slot->epochCalls  = 0ULL;
        slot->seededEpoch = curEpoch;
    }
    ++slot->totalCalls;
    ++slot->epochCalls;
    slot->lastValue = lastVal;
}

// ============================================================================
//  Unified RNG Draw Execution Engine (v3)
// ============================================================================

struct DrawExecResult {
    bool          overridden      = false;
    bool          bypassed        = false;
    bool          hadVanilla      = false;
    bool          returnFloatBits = false;
    std::uint8_t  mode            = kMhwRngSlotMasterDerived;
    std::uint32_t val0            = 0U;
    std::uint32_t val1            = 0U;
    float         finalFloat      = 0.0f;
};

void FirePostHooksAndObservers(const RvaHookChain* chain,
                               const SlotHookEntry* skipEntry,
                               bool isObserveOnlyMode,
                               std::uint32_t rva,
                               MhwRngSourceKind srcKind,
                               MhwRngDrawContext& ctx,
                               std::uint32_t obsCount) noexcept {
    ctx.callFlags &= ~kMhwRngDrawFlagCanModify;
    ctx.callFlags |= kMhwRngDrawFlagPostDraw;

    if (chain && chain->hasPost) {
        for (std::uint32_t i = 0; i < chain->count; ++i) {
            const SlotHookEntry& he = g_hookEntries[chain->entryIdx[i]];
            if (!he.active || !he.postFn || &he == skipEntry) continue;
            if ((he.flags & kMhwRngHookPost) == 0U) continue;
            if (isObserveOnlyMode && (he.flags & kMhwRngHookObserveOnly) == 0U) continue;

            MhwRngHookDecision postDec{kMhwRngActionContinue, ctx.activeCondId, ctx.finalValue0, ctx.finalValue1};
            InvokePluginSlotHookSEH(he.postFn, &ctx, &postDec, he.userData);
        }
    }

    if (obsCount != 0U) {
        const GlobalObserverSnapshot& snap = g_obsSnaps[g_obsActiveIdx.load(std::memory_order_acquire)];
        const std::uint32_t srcBit = (static_cast<std::uint32_t>(srcKind) < 32U)
                                         ? (1U << static_cast<std::uint32_t>(srcKind))
                                         : 0U;
        for (std::uint32_t i = 0; i < snap.count; ++i) {
            const GlobalObserverEntry& ob = snap.entries[i];
            if (!ob.active || !ob.fn) continue;
            if (ob.rvaFilter != 0U && ob.rvaFilter != rva) continue;
            if (ob.sourceMask != 0U && (ob.sourceMask & srcBit) == 0U) continue;
            InvokeGlobalObserverSEH(ob.fn, &ctx, ob.userData);
        }
    }
}

template <typename VanillaFn>
DrawExecResult ExecuteRngDraw(MhwRngSourceKind srcKind,
                              std::uintptr_t rngInstance,
                              bool needTwoValues,
                              std::uint64_t origArg1,
                              std::uint64_t origArg2,
                              float origMinVal,
                              float origMaxVal,
                              std::uint32_t baseCallFlags,
                              const MhwRngCallerRegs* regs,
                              VanillaFn&& callVanilla,
                              std::uint32_t* privateSelfWords = nullptr) noexcept {
    DrawExecResult out{};
    const std::uint32_t engineMode = g_engineMode.load(std::memory_order_relaxed);
    const void* retSlot = regs ? reinterpret_cast<const void*>(regs->callerRsp) : nullptr;

    std::uint32_t origRetRva = 0U;
    const std::uint32_t rva  = ComputeCallSiteRva(retSlot, &origRetRva);
    const std::uint64_t baseCompKey = MakeCompositeKey(rva, 0U);
    RngSlot* baseSlot = FindOrInsertCompositeSlot(baseCompKey);

    const std::uint64_t curEpoch   = g_seedEpoch.load(std::memory_order_acquire);
    const std::uint64_t masterSeed = g_masterSeed.load(std::memory_order_relaxed);

    if (!baseSlot) {
        std::uint32_t van0 = 0U, van1 = 0U;
        float vanF = 0.0f;
        callVanilla(van0, van1, vanF);
        out.bypassed   = true;
        out.mode       = kMhwRngSlotBypass;
        out.val0       = van0;
        out.val1       = van1;
        out.finalFloat = vanF;
        return out;
    }

    // Check private instance tags if requested
    bool instExplicit = false;
    std::uint32_t instPolicy = kMhwRngInstAuto;
    if (privateSelfWords) {
        QueryInstanceTagInfo(rngInstance, curEpoch, instExplicit, instPolicy);
        baseCallFlags |= kMhwRngDrawFlagIsPrivateInst;
        if (instExplicit) baseCallFlags |= kMhwRngDrawFlagInstExplicitSeeded;
    }

    // Lock-free fast probe before taking an RMW on `g_inFlightHookCalls`
    std::uint16_t chainIdx = DecodeChainIdx(baseSlot->hookChainIdxPlus1.load(std::memory_order_acquire));
    std::uint32_t obsCount = g_obsCount.load(std::memory_order_acquire);
    const bool enteredInFlight = (chainIdx < kMaxHookChains || obsCount != 0U);
    if (enteredInFlight) {
        // Full seq_cst Dekker barrier pairs with writer's seq_cst publish + g_inFlightHookCalls check.
        g_inFlightHookCalls.fetch_add(1U, std::memory_order_seq_cst);
        chainIdx = DecodeChainIdx(baseSlot->hookChainIdxPlus1.load(std::memory_order_seq_cst));
        obsCount = g_obsCount.load(std::memory_order_seq_cst);
    }

    const RvaHookChain* chain = (chainIdx < kMaxHookChains && g_hookChains[chainIdx].inUse)
                                    ? &g_hookChains[chainIdx]
                                    : nullptr;
    std::uint32_t targetCondId = baseSlot->activeCond.load(std::memory_order_acquire);

    MhwRngDrawContext ctx{};
    ctx.callSiteRva       = rva;
    ctx.sourceKind        = static_cast<std::uint8_t>(srcKind);
    ctx.needTwoValues     = needTwoValues ? 1U : 0U;
    ctx.exeBase           = g_exeBase;
    ctx.exeEnd            = g_exeEnd;
    ctx.rngInstance       = rngInstance;
    ctx.returnAddrSlot    = retSlot;
    ctx.masterSeed        = masterSeed;
    ctx.epoch             = curEpoch;
    ctx.activeCondId      = targetCondId;
    ctx.structSize        = sizeof(MhwRngDrawContext);
    ctx.DrawDeterministic = &DrawDeterministicHelper;
    ctx.callerRegs        = regs;
    ctx.origArg1          = origArg1;
    ctx.origArg2          = origArg2;
    ctx.origMinVal        = origMinVal;
    ctx.origMaxVal        = origMaxVal;
    ctx.callFlags         = baseCallFlags | (regs ? kMhwRngDrawFlagHasRegs : 0U);
    ctx.origReturnRva     = origRetRva;

    // 1. Observe-Only Mode: NEVER alter game RNG, just execute vanilla + update telemetry + notify observers
    if (engineMode == kMhwRngModeObserveOnly) {
        std::uint32_t van0 = 0U, van1 = 0U;
        float vanF = 0.0f;
        callVanilla(van0, van1, vanF);

        RecordSlotTelemetry(baseSlot, baseCompKey, srcKind, curEpoch, van0);

        ctx.callFlags    |= kMhwRngDrawFlagObserveOnly | kMhwRngDrawFlagHasVanilla;
        ctx.vanillaValue0 = van0;
        ctx.vanillaValue1 = van1;
        ctx.vanillaFloat  = vanF;
        ctx.finalValue0   = van0;
        ctx.finalValue1   = van1;
        ctx.finalFloat    = vanF;

        if (chain || obsCount != 0U) {
            FirePostHooksAndObservers(chain, nullptr, true, rva, srcKind, ctx, obsCount);
        }
        if (enteredInFlight) g_inFlightHookCalls.fetch_sub(1U, std::memory_order_acq_rel);

        out.bypassed   = true;
        out.mode       = kMhwRngSlotBypass;
        out.val0       = van0;
        out.val1       = van1;
        out.finalFloat = vanF;
        return out;
    }

    // 2. Full Takeover Mode: Run Pre-Hook Chain
    enum class PreOutcome : std::uint8_t {
        UseSlot,
        DirectValue,
        DirectFloatBits,
        BypassVanilla,
        FilterVanilla
    };
    PreOutcome outcome = PreOutcome::UseSlot;
    const SlotHookEntry* filterEntry = nullptr;
    std::uint32_t preVal0 = 0U, preVal1 = 0U;

    if (chain && (chain->hasPre || chain->hasFilter)) {
        for (std::uint32_t i = 0; i < chain->count; ++i) {
            const SlotHookEntry& he = g_hookEntries[chain->entryIdx[i]];
            if (!he.active) continue;

            if ((he.flags & kMhwRngHookPre) != 0U && he.preFn != nullptr) {
                MhwRngHookDecision dec{kMhwRngActionUseCondition, targetCondId, 0U, 0U};
                InvokePluginSlotHookSEH(he.preFn, &ctx, &dec, he.userData);

                if (dec.action == kMhwRngActionContinue) {
                    continue;
                }
                if (dec.action == kMhwRngActionUseCondition) {
                    targetCondId     = dec.condId;
                    ctx.activeCondId = targetCondId;
                    continue; // Allow downstream hooks in chain to inspect/continue/override
                }
                if (dec.action == kMhwRngActionClaimCondition) {
                    targetCondId     = dec.condId;
                    ctx.activeCondId = targetCondId;
                    break;
                }
                if (dec.action == kMhwRngActionReturnValue) {
                    targetCondId     = dec.condId;
                    ctx.activeCondId = targetCondId;
                    preVal0          = dec.value0;
                    preVal1          = dec.value1;
                    outcome          = PreOutcome::DirectValue;
                    break;
                }
                if (dec.action == kMhwRngActionReturnFloatBits) {
                    targetCondId     = dec.condId;
                    ctx.activeCondId = targetCondId;
                    preVal0          = dec.value0;
                    outcome          = PreOutcome::DirectFloatBits;
                    break;
                }
                if (dec.action == kMhwRngActionBypass) {
                    targetCondId     = dec.condId;
                    ctx.activeCondId = targetCondId;
                    outcome          = PreOutcome::BypassVanilla;
                    break;
                }
                if (dec.action == kMhwRngActionFilterVanilla) {
                    targetCondId     = dec.condId;
                    ctx.activeCondId = targetCondId;
                    filterEntry      = &he;
                    outcome          = PreOutcome::FilterVanilla;
                    break;
                }
            } else if ((he.flags & kMhwRngHookFilterVanilla) != 0U && he.postFn != nullptr) {
                filterEntry = &he;
                outcome     = PreOutcome::FilterVanilla;
                break;
            }
        }
    }

    const std::uint64_t targetCompKey = MakeCompositeKey(rva, targetCondId);
    RngSlot* targetSlot = (targetCondId == 0U) ? baseSlot : FindOrInsertCompositeSlot(targetCompKey);
    if (!targetSlot) {
        targetSlot = baseSlot;
    }
    const std::uint64_t recKey = (targetSlot == baseSlot) ? baseCompKey : targetCompKey;

    if (outcome == PreOutcome::DirectValue || outcome == PreOutcome::DirectFloatBits) {
        if (privateSelfWords) {
            privateSelfWords[0] = privateSelfWords[1];
            privateSelfWords[1] = privateSelfWords[2];
            privateSelfWords[2] = privateSelfWords[3];
            privateSelfWords[3] = (preVal0 != 0U) ? preVal0 : 0x6D2B79F5U;
        }
        RecordSlotTelemetry(targetSlot, recKey, srcKind, curEpoch, preVal0);
        out.overridden      = true;
        out.returnFloatBits = (outcome == PreOutcome::DirectFloatBits);
        out.mode            = kMhwRngSlotFixedValue;
        out.val0            = preVal0;
        out.val1            = preVal1;
        if (out.returnFloatBits) {
            out.finalFloat = BitsToFloat(preVal0);
        } else if (srcKind == kMhwRngSrcMt19937Range) {
            const float uf = static_cast<float>(static_cast<std::int64_t>(static_cast<std::uint64_t>(preVal0)));
            out.finalFloat = (uf * kFloatInv2Pow32 + kFloatHalfStep2P32) * (origMaxVal - origMinVal) + origMinVal;
        }
        ctx.callFlags   |= kMhwRngDrawFlagOverridden;
        ctx.finalValue0  = out.val0;
        ctx.finalValue1  = out.val1;
        ctx.finalFloat   = out.finalFloat;
    } else if (outcome == PreOutcome::BypassVanilla) {
        std::uint32_t van0 = 0U, van1 = 0U;
        float vanF = 0.0f;
        callVanilla(van0, van1, vanF);
        RecordSlotTelemetry(targetSlot, recKey, srcKind, curEpoch, van0);

        out.bypassed      = true;
        out.mode          = kMhwRngSlotBypass;
        out.val0          = van0;
        out.val1          = van1;
        out.finalFloat    = vanF;
        ctx.callFlags    |= kMhwRngDrawFlagHasVanilla | kMhwRngDrawFlagBypassed;
        ctx.vanillaValue0 = van0;
        ctx.vanillaValue1 = van1;
        ctx.vanillaFloat  = vanF;
        ctx.finalValue0   = van0;
        ctx.finalValue1   = van1;
        ctx.finalFloat    = vanF;
    } else if (outcome == PreOutcome::FilterVanilla) {
        std::uint32_t van0 = 0U, van1 = 0U;
        float vanF = 0.0f;
        callVanilla(van0, van1, vanF);

        ctx.callFlags    |= kMhwRngDrawFlagPostDraw | kMhwRngDrawFlagHasVanilla | kMhwRngDrawFlagCanModify;
        ctx.vanillaValue0 = van0;
        ctx.vanillaValue1 = van1;
        ctx.vanillaFloat  = vanF;
        ctx.finalValue0   = van0;
        ctx.finalValue1   = van1;
        ctx.finalFloat    = vanF;

        MhwRngHookDecision fdec{kMhwRngActionContinue, targetCondId, van0, van1};
        if (filterEntry && filterEntry->postFn) {
            InvokePluginSlotHookSEH(filterEntry->postFn, &ctx, &fdec, filterEntry->userData);
        }

        if (fdec.action == kMhwRngActionReturnValue) {
            out.overridden  = true;
            out.hadVanilla  = true;
            out.mode        = kMhwRngSlotFixedValue;
            out.val0        = fdec.value0;
            out.val1        = fdec.value1;
            if (privateSelfWords) {
                privateSelfWords[3] = (fdec.value0 != 0U) ? fdec.value0 : 0x6D2B79F5U;
            }
            if (srcKind == kMhwRngSrcMt19937Range) {
                const float uf = static_cast<float>(static_cast<std::int64_t>(static_cast<std::uint64_t>(fdec.value0)));
                out.finalFloat = (uf * kFloatInv2Pow32 + kFloatHalfStep2P32) * (origMaxVal - origMinVal) + origMinVal;
            }
            ctx.callFlags  |= kMhwRngDrawFlagOverridden;
            ctx.finalValue0 = out.val0;
            ctx.finalValue1 = out.val1;
            ctx.finalFloat  = out.finalFloat;
        } else if (fdec.action == kMhwRngActionReturnFloatBits) {
            out.overridden      = true;
            out.hadVanilla      = true;
            out.returnFloatBits = true;
            out.mode            = kMhwRngSlotFixedValue;
            out.val0            = fdec.value0;
            out.finalFloat      = BitsToFloat(fdec.value0);
            ctx.callFlags      |= kMhwRngDrawFlagOverridden;
            ctx.finalValue0     = out.val0;
            ctx.finalFloat      = out.finalFloat;
        } else {
            out.bypassed    = true;
            out.mode        = kMhwRngSlotBypass;
            out.val0        = van0;
            out.val1        = van1;
            out.finalFloat  = vanF;
            ctx.callFlags  |= kMhwRngDrawFlagBypassed;
        }
        RecordSlotTelemetry(targetSlot, recKey, srcKind, curEpoch, out.val0);
    } else {
        // PreOutcome::UseSlot
        std::uint8_t slotMode = kMhwRngSlotMasterDerived;
        bool slotBypass       = false;
        std::uint32_t v0 = 0U, v1 = 0U;
        {
            SlotSpinGuard guard(targetSlot->lock);
            if (targetSlot->key.load(std::memory_order_relaxed) == recKey) {
                if (targetSlot->sourceKind == kMhwRngSrcUnknown || srcKind != kMhwRngSrcUnknown) {
                    targetSlot->sourceKind = static_cast<std::uint8_t>(srcKind);
                }
                slotMode = targetSlot->mode;
                if (slotMode == kMhwRngSlotBypass ||
                    (slotMode == kMhwRngSlotMasterDerived &&
                     (engineMode == kMhwRngModeSelective ||
                      (g_protectSaveSeeds.load(std::memory_order_relaxed) &&
                       (mhw::IsSaveSeedProtectedRva(rva) || mhw::IsSaveSeedProtectedRva(origRetRva)))))) {
                    slotBypass = true;
                    if (targetSlot->seededEpoch != curEpoch) {
                        targetSlot->epochCalls  = 0ULL;
                        targetSlot->seededEpoch = curEpoch;
                    }
                    ++targetSlot->totalCalls;
                    ++targetSlot->epochCalls;
                } else {
                    v0 = StepSlotLocked(*targetSlot, recKey, curEpoch, masterSeed);
                    v1 = needTwoValues ? StepSlotLocked(*targetSlot, recKey, curEpoch, masterSeed) : 0U;
                }
            } else {
                slotBypass = true;
            }
        }

        if (slotBypass) {
            std::uint32_t van0 = 0U, van1 = 0U;
            float vanF = 0.0f;
            callVanilla(van0, van1, vanF);
            {
                SlotSpinGuard guard(targetSlot->lock);
                if (targetSlot->key.load(std::memory_order_relaxed) == recKey) {
                    targetSlot->lastValue = van0;
                }
            }
            out.bypassed      = true;
            out.mode          = kMhwRngSlotBypass;
            out.val0          = van0;
            out.val1          = van1;
            out.finalFloat    = vanF;
            ctx.callFlags    |= kMhwRngDrawFlagHasVanilla | kMhwRngDrawFlagBypassed;
            ctx.vanillaValue0 = van0;
            ctx.vanillaValue1 = van1;
            ctx.vanillaFloat  = vanF;
            ctx.finalValue0   = van0;
            ctx.finalValue1   = van1;
            ctx.finalFloat    = vanF;
        } else {
            // Handle private MtRandom instance state policy when slot is MasterDerived
            if (privateSelfWords && slotMode == kMhwRngSlotMasterDerived &&
                (instPolicy == kMhwRngInstFollowOwnState ||
                 (instPolicy == kMhwRngInstAuto && instExplicit))) {
                v0 = StepMhwXorshift128(privateSelfWords);
                SlotSpinGuard guard(targetSlot->lock);
                if (targetSlot->key.load(std::memory_order_relaxed) == recKey) {
                    targetSlot->lastValue = v0;
                }
            } else if (privateSelfWords) {
                privateSelfWords[0] = privateSelfWords[1];
                privateSelfWords[1] = privateSelfWords[2];
                privateSelfWords[2] = privateSelfWords[3];
                privateSelfWords[3] = (v0 != 0U) ? v0 : 0x6D2B79F5U;
            }

            out.mode = slotMode;
            out.val0 = v0;
            out.val1 = v1;
            if (slotMode != kMhwRngSlotMasterDerived) {
                out.overridden = true;
                ctx.callFlags |= kMhwRngDrawFlagOverridden;
            }
            if (srcKind == kMhwRngSrcMt19937Range) {
                const float uf = static_cast<float>(static_cast<std::int64_t>(static_cast<std::uint64_t>(v0)));
                out.finalFloat = (uf * kFloatInv2Pow32 + kFloatHalfStep2P32) * (origMaxVal - origMinVal) + origMinVal;
            }
            ctx.finalValue0 = v0;
            ctx.finalValue1 = v1;
            ctx.finalFloat  = out.finalFloat;
        }
    }

    if (chain || obsCount != 0U) {
        FirePostHooksAndObservers(chain, filterEntry, false, rva, srcKind, ctx, obsCount);
    }
    if (enteredInFlight) {
        g_inFlightHookCalls.fetch_sub(1U, std::memory_order_acq_rel);
    }
    return out;
}

void FillSnapshotLocked(const RngSlot& src,
                        std::uint64_t compKey,
                        MhwRngSlotSnapshot& dst) noexcept {
    const auto rva    = static_cast<std::uint32_t>(compKey & 0xFFFFFFFFULL);
    const auto condId = static_cast<std::uint32_t>(compKey >> 32);
    std::uint32_t baseActiveCond = 0U;
    std::uint8_t  hookCount      = 0U;

    auto countChain = [](const RngSlot& slot) -> std::uint8_t {
        std::uint16_t cIdx = DecodeChainIdx(slot.hookChainIdxPlus1.load(std::memory_order_acquire));
        if (cIdx >= kMaxHookChains) return 0U;
        g_inFlightHookCalls.fetch_add(1U, std::memory_order_seq_cst);
        cIdx = DecodeChainIdx(slot.hookChainIdxPlus1.load(std::memory_order_seq_cst));
        std::uint8_t cnt = 0U;
        if (cIdx < kMaxHookChains && g_hookChains[cIdx].inUse) {
            cnt = static_cast<std::uint8_t>(g_hookChains[cIdx].count);
        }
        g_inFlightHookCalls.fetch_sub(1U, std::memory_order_acq_rel);
        return cnt;
    };

    if (condId == 0U) {
        baseActiveCond = src.activeCond.load(std::memory_order_relaxed);
        hookCount      = countChain(src);
    } else if (const RngSlot* base = FindExistingCompositeSlot(MakeCompositeKey(rva, 0U))) {
        baseActiveCond = base->activeCond.load(std::memory_order_relaxed);
        hookCount      = countChain(*base);
    }

    dst.callSiteRva   = rva;
    dst.condId        = condId;
    dst.mode          = src.mode;
    dst.sourceKind    = src.sourceKind;
    dst.seqEndPolicy  = src.seqEndPolicy;
    dst.hasPluginHook = hookCount;
    dst.ownerPluginId = src.ownerPluginId;
    dst.reserved0     = 0U;
    dst.activeCondId  = baseActiveCond;
    dst.fixedValue    = src.fixedValue;
    dst.lastValue     = src.lastValue;
    dst.reserved1     = 0U;
    dst.customSeed    = src.customSeed;
    dst.totalCalls    = src.totalCalls;
    dst.epochCalls    = src.epochCalls;
    dst.seqLength     = src.seqLength;
    dst.seqCursor     = src.seqCursor;
    std::memcpy(dst.state, src.state, sizeof(dst.state));
}

// Requires `g_hookRegMx` held
bool RebuildChainForRvaLocked(std::uint32_t normRva, RngSlot* baseSlot) {
    if (!baseSlot) return false;

    std::uint16_t matched[kMaxHooksPerRva]{};
    std::uint32_t mCount = 0U;
    for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
        if (g_hookEntries[i].active && g_hookEntries[i].rva == normRva) {
            if (mCount < kMaxHooksPerRva) {
                matched[mCount++] = i;
            }
        }
    }

    std::sort(matched, matched + mCount, [](std::uint16_t a, std::uint16_t b) {
        if (g_hookEntries[a].priority != g_hookEntries[b].priority) {
            return g_hookEntries[a].priority < g_hookEntries[b].priority;
        }
        return g_hookEntries[a].regSeq < g_hookEntries[b].regSeq;
    });

    const std::uint16_t oldChainIdx = DecodeChainIdx(baseSlot->hookChainIdxPlus1.load(std::memory_order_acquire));
    if (mCount == 0U) {
        baseSlot->hookChainIdxPlus1.store(0U, std::memory_order_seq_cst);
        while (g_inFlightHookCalls.load(std::memory_order_seq_cst) != 0U) {
            _mm_pause();
        }
        if (oldChainIdx < kMaxHookChains) {
            g_hookChains[oldChainIdx] = RvaHookChain{};
        }
        SlotSpinGuard sg(baseSlot->lock);
        if (baseSlot->mode == kMhwRngSlotMasterDerived) {
            baseSlot->ownerPluginId = kMhwRngPluginIdKernel;
        }
        return true;
    }

    std::uint16_t newChainIdx = kInvalidChainIdx;
    for (std::uint16_t c = 0; c < kMaxHookChains; ++c) {
        if (!g_hookChains[c].inUse) {
            newChainIdx = c;
            break;
        }
    }
    if (newChainIdx >= kMaxHookChains) return false;

    RvaHookChain& nc = g_hookChains[newChainIdx];
    nc              = RvaHookChain{};
    nc.inUse        = true;
    nc.firstOwnerId = g_hookEntries[matched[0]].ownerId;
    nc.rva          = normRva;
    nc.count        = mCount;
    for (std::uint32_t i = 0; i < mCount; ++i) {
        const SlotHookEntry& he = g_hookEntries[matched[i]];
        nc.entryIdx[i] = matched[i];
        if ((he.flags & kMhwRngHookPre) != 0U && he.preFn)          nc.hasPre    = true;
        if ((he.flags & kMhwRngHookPost) != 0U && he.postFn)        nc.hasPost   = true;
        if ((he.flags & kMhwRngHookFilterVanilla) != 0U)            nc.hasFilter = true;
    }

    baseSlot->hookChainIdxPlus1.store(static_cast<std::uint16_t>(newChainIdx + 1U), std::memory_order_seq_cst);
    while (g_inFlightHookCalls.load(std::memory_order_seq_cst) != 0U) {
        _mm_pause();
    }
    if (oldChainIdx < kMaxHookChains) {
        g_hookChains[oldChainIdx] = RvaHookChain{};
    }
    {
        SlotSpinGuard sg(baseSlot->lock);
        if (baseSlot->mode == kMhwRngSlotMasterDerived) {
            baseSlot->ownerPluginId = nc.firstOwnerId;
        }
    }
    return true;
}

// Requires `g_hookRegMx` held
void PublishObserverSnapshotLocked() {
    const std::uint32_t nextIdx = g_obsActiveIdx.load(std::memory_order_relaxed) ^ 1U;
    GlobalObserverSnapshot& dst = g_obsSnaps[nextIdx];
    dst.count = 0U;
    for (std::uint32_t i = 0; i < kMaxGlobalObservers; ++i) {
        if (g_obsStorage[i].active && g_obsStorage[i].fn) {
            dst.entries[dst.count++] = g_obsStorage[i];
        }
    }
    g_obsActiveIdx.store(nextIdx, std::memory_order_seq_cst);
    g_obsCount.store(dst.count, std::memory_order_seq_cst);
    while (g_inFlightHookCalls.load(std::memory_order_seq_cst) != 0U) {
        _mm_pause();
    }
}

MhwRngHookHandle MakeHookHandle(std::uint32_t slotIdx) noexcept {
    const std::uint32_t gen = (g_nextHandleGen++ & 0xFFFFU);
    if (gen == 0U) g_nextHandleGen = 2U;
    return ((gen != 0U ? gen : 1U) << 16) | (slotIdx + 1U);
}

#pragma pack(push, 1)
struct StateBlobHeader {
    std::uint32_t magic;        // 'MRSS' = 0x5353524DU
    std::uint32_t version;      // 1U
    std::uint64_t masterSeed;
    std::uint64_t epoch;
    std::uint32_t slotCount;
    std::uint32_t payloadCrc32;
};
#pragma pack(pop)
constexpr std::uint32_t kStateBlobMagic   = 0x5353524DU;
constexpr std::uint32_t kStateBlobVersion = 1U;

bool RestoreSingleSlotStateInternal(const MhwRngSlotState& st, std::uint64_t targetEpoch) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(st.callSiteRva);
    if (norm == 0U) return false;
    const std::uint64_t compKey = MakeCompositeKey(norm, st.condId);
    RngSlot* slot = FindOrInsertCompositeSlot(compKey);
    if (!slot) return false;

    const std::uint32_t flags = (st.flags == 0U) ? kMhwRngRestoreAll : st.flags;
    if ((flags & kMhwRngRestoreActiveCond) != 0U && st.condId == 0U) {
        slot->activeCond.store(st.activeCondId, std::memory_order_release);
    }

    SlotSpinGuard g(slot->lock);
    if (slot->key.load(std::memory_order_relaxed) != compKey) return false;

    if ((flags & kMhwRngRestoreState) != 0U) {
        std::memcpy(slot->state, st.state, sizeof(slot->state));
        if ((slot->state[0] | slot->state[1] | slot->state[2] | slot->state[3]) == 0U) {
            slot->state[0] = 0x075BCD15U;
            slot->state[1] = 0x159A55E5U;
            slot->state[2] = 0x1F123BB5U;
            slot->state[3] = 0x05491333U;
        }
        slot->seededEpoch = targetEpoch;
    }
    if ((flags & kMhwRngRestoreSeqCursor) != 0U) {
        slot->seqCursor = st.seqCursor;
    }
    if ((flags & kMhwRngRestoreCounters) != 0U) {
        slot->epochCalls = st.epochCalls;
        slot->lastValue  = st.lastValue;
    }
    return true;
}

} // namespace

// ============================================================================
//  Kernel Lifecycle & Composite Slot API
// ============================================================================

void Init(std::uintptr_t exeBase, std::uintptr_t exeEnd, const app::Config& cfg) {
    g_exeBase       = exeBase;
    g_exeEnd        = exeEnd;
    g_sInstanceAddr = exeBase + mhw::kRvaMtRandomInstance;

    RegisterBuiltinWrappers();

    g_masterSeed.store(cfg.masterSeed, std::memory_order_relaxed);
    const std::uint32_t bootMode = (cfg.bootMode <= kMhwRngModeSelective) ? cfg.bootMode : kMhwRngModeSelective;
    g_engineMode.store(bootMode, std::memory_order_release);
    g_protectSaveSeeds.store(cfg.protectSaveSeeds, std::memory_order_release);
    g_seedEpoch.store(1ULL, std::memory_order_release);

    ApplyConfigRules(cfg, false);
}

void Shutdown() {
    std::lock_guard<std::mutex> tlk(g_slotTableMutMx);
    for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
        std::uint32_t* toFree = nullptr;
        {
            SlotSpinGuard g(g_slots[i].lock);
            toFree = g_slots[i].seqHeap;
            g_slots[i].seqHeap    = nullptr;
            g_slots[i].useHeapSeq = 0U;
            g_slots[i].seqLength  = 0U;
            g_slots[i].seqCursor  = 0U;
            g_slots[i].hookChainIdxPlus1.store(0U, std::memory_order_relaxed);
            g_slots[i].key.store(kSlotKeyEmpty, std::memory_order_relaxed);
        }
        delete[] toFree;
    }
    g_activeSlotCount.store(0U, std::memory_order_relaxed);
}

void SetLifecycleEventSink(LifecycleEventSinkFn sink) noexcept {
    g_lifecycleSink = sink;
}

std::uintptr_t ExeBase() noexcept { return g_exeBase; }
std::uintptr_t ExeEnd() noexcept  { return g_exeEnd; }

void SetProtectSaveSeeds(bool enabled) noexcept {
    g_protectSaveSeeds.store(enabled, std::memory_order_release);
}

bool IsProtectSaveSeedsEnabled() noexcept {
    return g_protectSaveSeeds.load(std::memory_order_acquire);
}

std::uint32_t NormalizeUserSiteAddress(std::uint64_t rawAddr, bool* outWasCallInsn) noexcept {
    if (outWasCallInsn) *outWasCallInsn = false;
    if (rawAddr == 0ULL) return 0U;

    if (g_exeBase != 0 && rawAddr >= g_exeBase && rawAddr < g_exeEnd) {
        rawAddr -= g_exeBase;
    } else if (rawAddr >= 0x140000000ULL && rawAddr < 0x146000000ULL) {
        rawAddr -= 0x140000000ULL;
    }
    if (rawAddr == 0ULL || rawAddr > 0xFFFFFFFFULL) return 0U;

    auto rva = static_cast<std::uint32_t>(rawAddr);
    if (g_exeBase != 0 && (g_exeBase + rva + 5U) < g_exeEnd) {
        __try {
            const auto* code = reinterpret_cast<const std::uint8_t*>(g_exeBase + rva);
            if (code[0] == 0xE8) {
                std::int32_t disp = 0;
                std::memcpy(&disp, code + 1U, 4);
                const auto targetRva = static_cast<std::uintptr_t>(
                    static_cast<std::int64_t>(rva + 5U) + disp);
                if (targetRva == mhw::kRvaMtRandomNrand       ||
                    targetRva == mhw::kRvaMtRandomCtorSeed    ||
                    targetRva == mhw::kRvaMtRandomCtorDefault ||
                    targetRva == mhw::kRvaMtRandomSrand       ||
                    targetRva == mhw::kRvaMtRandomSphere      ||
                    targetRva == mhw::kRvaStdRandomDevice     ||
                    targetRva == mhw::kRvaMt19937Range        ||
                    targetRva == mhw::kRvaMt19937HeapInit     ||
                    targetRva == mhw::kRvaMt19937HeapDraw     ||
                    targetRva == mhw::kRvaCrtRand) {
                    rva += 5U;
                    if (outWasCallInsn) *outWasCallInsn = true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return rva;
}

void ApplyConfigRules(const app::Config& cfg, bool clearKernelRulesOnly) {
    if (clearKernelRulesOnly) {
        ClearRulesByOwner(kMhwRngPluginIdKernel);
    }
    for (const app::RawSlotRuleConfig& r : cfg.rawSlotRules) {
        const std::uint32_t rva = NormalizeUserSiteAddress(r.rawAddress);
        if (rva == 0U) continue;
        switch (r.mode) {
            case kMhwRngSlotFixedValue:
                SetCondFixedValue(kMhwRngPluginIdKernel, rva, r.condId, r.fixedValue);
                break;
            case kMhwRngSlotCustomSeed:
                SetCondCustomSeed(kMhwRngPluginIdKernel, rva, r.condId, r.customSeed);
                break;
            case kMhwRngSlotSequence:
                if (!r.sequence.empty()) {
                    SetCondSequence(kMhwRngPluginIdKernel,
                                    rva,
                                    r.condId,
                                    r.sequence.data(),
                                    static_cast<std::uint32_t>(r.sequence.size()),
                                    static_cast<std::uint8_t>(r.endPolicy));
                }
                break;
            case kMhwRngSlotBypass:
                SetCondBypass(kMhwRngPluginIdKernel, rva, r.condId);
                break;
            case kMhwRngSlotMasterDerived:
            default:
                SetCondMasterDerived(kMhwRngPluginIdKernel, rva, r.condId);
                break;
        }
    }
}

bool SetActiveCondition(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    RngSlot* base = FindOrInsertCompositeSlot(MakeCompositeKey(norm, 0U));
    if (!base) return false;
    base->activeCond.store(condId, std::memory_order_release);
    if (condId != 0U) {
        SlotSpinGuard g(base->lock);
        base->ownerPluginId = ownerId;
    }
    return true;
}

std::uint32_t GetActiveCondition(std::uint32_t rva) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    const RngSlot* base = FindExistingCompositeSlot(MakeCompositeKey(norm, 0U));
    return base ? base->activeCond.load(std::memory_order_acquire) : 0U;
}

bool SetCondMasterDerived(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    RngSlot* slot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, condId));
    if (!slot) return false;
    std::uint32_t* toFree = nullptr;
    {
        SlotSpinGuard g(slot->lock);
        toFree              = slot->seqHeap;
        slot->seqHeap       = nullptr;
        slot->useHeapSeq    = 0U;
        slot->seqLength     = 0U;
        slot->seqCursor     = 0U;
        slot->mode          = kMhwRngSlotMasterDerived;
        slot->ownerPluginId = ownerId;
        slot->seededEpoch   = 0ULL;
    }
    delete[] toFree;
    return true;
}

bool SetCondCustomSeed(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId, std::uint64_t seed) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    RngSlot* slot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, condId));
    if (!slot) return false;
    std::uint32_t* toFree = nullptr;
    {
        SlotSpinGuard g(slot->lock);
        toFree              = slot->seqHeap;
        slot->seqHeap       = nullptr;
        slot->useHeapSeq    = 0U;
        slot->seqLength     = 0U;
        slot->seqCursor     = 0U;
        slot->mode          = kMhwRngSlotCustomSeed;
        slot->ownerPluginId = ownerId;
        slot->customSeed    = seed;
        slot->seededEpoch   = 0ULL;
    }
    delete[] toFree;
    return true;
}

bool SetCondFixedValue(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId, std::uint32_t rawU32) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    RngSlot* slot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, condId));
    if (!slot) return false;
    std::uint32_t* toFree = nullptr;
    {
        SlotSpinGuard g(slot->lock);
        toFree              = slot->seqHeap;
        slot->seqHeap       = nullptr;
        slot->useHeapSeq    = 0U;
        slot->seqLength     = 0U;
        slot->seqCursor     = 0U;
        slot->mode          = kMhwRngSlotFixedValue;
        slot->ownerPluginId = ownerId;
        slot->fixedValue    = rawU32;
    }
    delete[] toFree;
    return true;
}

bool SetCondSequence(MhwRngPluginId ownerId,
                     std::uint32_t rva,
                     std::uint32_t condId,
                     const std::uint32_t* values,
                     std::uint32_t count,
                     std::uint8_t endPolicy) {
    if (!values || count == 0U) return false;
    const std::uint32_t norm    = NormalizeUserSiteAddress(rva);
    const std::uint64_t compKey = MakeCompositeKey(norm, condId);
    RngSlot* slot = FindOrInsertCompositeSlot(compKey);
    if (!slot) return false;

    const bool useInline = (count <= kInlineSeqCap);
    std::uint32_t* newHeap = nullptr;
    if (!useInline) {
        newHeap = new std::uint32_t[count];
        std::memcpy(newHeap, values, count * sizeof(std::uint32_t));
    }

    std::uint32_t* oldHeap = nullptr;
    bool applied           = false;
    {
        SlotSpinGuard g(slot->lock);
        if (slot->key.load(std::memory_order_relaxed) == compKey) {
            oldHeap = slot->seqHeap;
            if (useInline) {
                std::memcpy(slot->inlineSeq, values, count * sizeof(std::uint32_t));
                slot->useHeapSeq = 0U;
                slot->seqHeap    = nullptr;
            } else {
                slot->useHeapSeq = 1U;
                slot->seqHeap    = newHeap;
            }
            slot->seqLength     = count;
            slot->seqCursor     = 0U;
            slot->seqEndPolicy  = (endPolicy <= kMhwRngSeqFallbackDerived) ? endPolicy : kMhwRngSeqLoop;
            slot->mode          = kMhwRngSlotSequence;
            slot->ownerPluginId = ownerId;
            slot->seededEpoch   = g_seedEpoch.load(std::memory_order_acquire);
            DeriveXorshift128State(g_masterSeed.load(std::memory_order_relaxed), compKey, slot->state);
            applied = true;
        }
    }
    if (!applied) {
        delete[] newHeap;
        return false;
    }
    delete[] oldHeap;
    return true;
}

bool SetCondBypass(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    RngSlot* slot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, condId));
    if (!slot) return false;
    std::uint32_t* toFree = nullptr;
    {
        SlotSpinGuard g(slot->lock);
        toFree              = slot->seqHeap;
        slot->seqHeap       = nullptr;
        slot->useHeapSeq    = 0U;
        slot->seqLength     = 0U;
        slot->seqCursor     = 0U;
        slot->mode          = kMhwRngSlotBypass;
        slot->ownerPluginId = ownerId;
    }
    delete[] toFree;
    return true;
}

void ClearSlotAllConditions(std::uint32_t rva) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    if (norm == 0U) return;
    std::lock_guard<std::mutex> tlk(g_slotTableMutMx);
    for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
        const std::uint64_t k = g_slots[i].key.load(std::memory_order_acquire);
        if (k == kSlotKeyEmpty || k == kSlotKeyTombstone) continue;
        if (static_cast<std::uint32_t>(k & 0xFFFFFFFFULL) != norm) continue;

        if ((k >> 32) == 0ULL) {
            g_slots[i].activeCond.store(0U, std::memory_order_release);
        }
        MhwRngPluginId keepOwner = kMhwRngPluginIdKernel;
        if ((k >> 32) == 0ULL) {
            std::uint16_t cIdx = DecodeChainIdx(g_slots[i].hookChainIdxPlus1.load(std::memory_order_acquire));
            if (cIdx < kMaxHookChains) {
                g_inFlightHookCalls.fetch_add(1U, std::memory_order_acq_rel);
                cIdx = DecodeChainIdx(g_slots[i].hookChainIdxPlus1.load(std::memory_order_acquire));
                if (cIdx < kMaxHookChains && g_hookChains[cIdx].inUse && g_hookChains[cIdx].count > 0U) {
                    keepOwner = g_hookChains[cIdx].firstOwnerId;
                }
                g_inFlightHookCalls.fetch_sub(1U, std::memory_order_acq_rel);
            }
        }
        std::uint32_t* toFree = nullptr;
        {
            SlotSpinGuard g(g_slots[i].lock);
            toFree = g_slots[i].seqHeap;
            g_slots[i].seqHeap       = nullptr;
            g_slots[i].useHeapSeq    = 0U;
            g_slots[i].mode          = kMhwRngSlotMasterDerived;
            g_slots[i].ownerPluginId = keepOwner;
            g_slots[i].seqLength     = 0U;
            g_slots[i].seqCursor     = 0U;
            g_slots[i].seededEpoch   = 0ULL;
        }
        delete[] toFree;
    }
}

void ClearRulesByOwner(MhwRngPluginId ownerId) noexcept {
    std::lock_guard<std::mutex> tlk(g_slotTableMutMx);
    for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
        const std::uint64_t k = g_slots[i].key.load(std::memory_order_acquire);
        if (k == kSlotKeyEmpty || k == kSlotKeyTombstone) continue;

        std::uint32_t* toFree = nullptr;
        bool reclaimTombstone = false;
        {
            SlotSpinGuard g(g_slots[i].lock);
            if (g_slots[i].ownerPluginId == ownerId) {
                toFree = g_slots[i].seqHeap;
                g_slots[i].seqHeap      = nullptr;
                g_slots[i].useHeapSeq   = 0U;
                g_slots[i].mode         = kMhwRngSlotMasterDerived;
                g_slots[i].ownerPluginId= kMhwRngPluginIdKernel;
                g_slots[i].seqLength    = 0U;
                g_slots[i].seqCursor    = 0U;
                g_slots[i].seededEpoch  = 0ULL;
                if ((k >> 32) == 0ULL) {
                    g_slots[i].activeCond.store(0U, std::memory_order_release);
                } else {
                    g_slots[i].key.store(kSlotKeyTombstone, std::memory_order_release);
                    reclaimTombstone = true;
                }
            }
        }
        if (reclaimTombstone) {
            g_activeSlotCount.fetch_sub(1U, std::memory_order_relaxed);
        }
        delete[] toFree;
    }
}

bool ReleaseCondSlot(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept {
    if (condId == 0U) return false;
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    if (norm == 0U) return false;
    const std::uint64_t compKey = MakeCompositeKey(norm, condId);

    std::lock_guard<std::mutex> tlk(g_slotTableMutMx);
    RngSlot* slot = FindExistingCompositeSlot(compKey);
    if (!slot) return false;

    std::uint32_t* toFree = nullptr;
    {
        SlotSpinGuard g(slot->lock);
        if (slot->key.load(std::memory_order_relaxed) != compKey) return false;
        if (ownerId != kMhwRngPluginIdKernel && slot->ownerPluginId != ownerId) return false;
        toFree              = slot->seqHeap;
        slot->seqHeap       = nullptr;
        slot->useHeapSeq    = 0U;
        slot->seqLength     = 0U;
        slot->seqCursor     = 0U;
        slot->mode          = kMhwRngSlotMasterDerived;
        slot->ownerPluginId = kMhwRngPluginIdKernel;
        slot->seededEpoch   = 0ULL;
        slot->key.store(kSlotKeyTombstone, std::memory_order_release);
    }
    g_activeSlotCount.fetch_sub(1U, std::memory_order_relaxed);
    delete[] toFree;
    return true;
}

bool GetCondSlotInfo(std::uint32_t rva, std::uint32_t condId, MhwRngSlotSnapshot* outSnap) noexcept {
    if (!outSnap) return false;
    const std::uint32_t norm    = NormalizeUserSiteAddress(rva);
    const std::uint64_t compKey = MakeCompositeKey(norm, condId);
    RngSlot* slot = FindExistingCompositeSlot(compKey);
    if (!slot) return false;
    SlotSpinGuard g(slot->lock);
    if (slot->key.load(std::memory_order_relaxed) != compKey) return false;
    FillSnapshotLocked(*slot, compKey, *outSnap);
    return true;
}

MhwRngHookHandle RegisterSlotHookEx(MhwRngPluginId ownerId, const MhwRngHookDesc* desc) noexcept {
    if (!desc || desc->structSize != sizeof(MhwRngHookDesc)) return kMhwRngInvalidHandle;
    if (!desc->preFn && !desc->postFn) return kMhwRngInvalidHandle;

    const std::uint32_t norm = NormalizeUserSiteAddress(desc->rva);
    if (norm == 0U) return kMhwRngInvalidHandle;
    RngSlot* baseSlot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, 0U));
    if (!baseSlot) return kMhwRngInvalidHandle;

    std::uint32_t flags = desc->flags;
    if (flags == 0U) {
        if (desc->preFn)  flags |= kMhwRngHookPre;
        if (desc->postFn) flags |= kMhwRngHookPost;
    }
    if ((flags & kMhwRngHookFilterVanilla) != 0U && desc->postFn != nullptr) {
        flags |= kMhwRngHookPost;
    }

    std::lock_guard<std::mutex> lk(g_hookRegMx);

    std::uint32_t existingCount = 0U;
    std::uint16_t freeSlot      = kMaxSlotHooks;
    for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
        if (g_hookEntries[i].active) {
            if (g_hookEntries[i].rva == norm) ++existingCount;
        } else if (freeSlot == kMaxSlotHooks) {
            freeSlot = i;
        }
    }
    if (freeSlot >= kMaxSlotHooks || existingCount >= kMaxHooksPerRva) {
        return kMhwRngInvalidHandle;
    }

    const MhwRngHookHandle handle = MakeHookHandle(freeSlot);
    SlotHookEntry& he = g_hookEntries[freeSlot];
    he.active   = true;
    he.handle   = handle;
    he.ownerId  = ownerId;
    he.rva      = norm;
    he.priority = desc->priority;
    he.flags    = flags;
    he.regSeq   = g_nextRegSeq++;
    he.preFn    = desc->preFn;
    he.postFn   = desc->postFn;
    he.userData = desc->userData;

    if (!RebuildChainForRvaLocked(norm, baseSlot)) {
        he = SlotHookEntry{};
        return kMhwRngInvalidHandle;
    }
    return handle;
}

bool RegisterSlotHook(MhwRngPluginId ownerId,
                      std::uint32_t rva,
                      MhwRngSlotHookFn fn,
                      void* userData) noexcept {
    if (!fn) return false;
    MhwRngHookDesc desc{};
    desc.structSize = sizeof(desc);
    desc.rva        = rva;
    desc.priority   = 0;
    desc.flags      = kMhwRngHookPre;
    desc.preFn      = fn;
    desc.postFn     = nullptr;
    desc.userData   = userData;
    return RegisterSlotHookEx(ownerId, &desc) != kMhwRngInvalidHandle;
}

void UnregisterSlotHookHandle(MhwRngPluginId ownerId, MhwRngHookHandle handle) noexcept {
    if (handle == kMhwRngInvalidHandle) return;
    std::lock_guard<std::mutex> lk(g_hookRegMx);
    for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
        if (g_hookEntries[i].active && g_hookEntries[i].handle == handle &&
            (ownerId == 0U || g_hookEntries[i].ownerId == ownerId)) {
            const std::uint32_t normRva = g_hookEntries[i].rva;
            g_hookEntries[i].active = false;
            if (RngSlot* baseSlot = FindExistingCompositeSlot(MakeCompositeKey(normRva, 0U))) {
                RebuildChainForRvaLocked(normRva, baseSlot);
            } else {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                while (g_inFlightHookCalls.load(std::memory_order_seq_cst) != 0U) _mm_pause();
            }
            g_hookEntries[i] = SlotHookEntry{};
            return;
        }
    }
}

void UnregisterSlotHook(MhwRngPluginId ownerId, std::uint32_t rva) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    if (norm == 0U) return;
    std::lock_guard<std::mutex> lk(g_hookRegMx);

    std::vector<std::uint16_t> removed;
    for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
        if (g_hookEntries[i].active && g_hookEntries[i].rva == norm &&
            (ownerId == 0U || g_hookEntries[i].ownerId == ownerId)) {
            g_hookEntries[i].active = false;
            removed.push_back(i);
        }
    }
    if (removed.empty()) return;

    if (RngSlot* baseSlot = FindExistingCompositeSlot(MakeCompositeKey(norm, 0U))) {
        RebuildChainForRvaLocked(norm, baseSlot);
    } else {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (g_inFlightHookCalls.load(std::memory_order_seq_cst) != 0U) _mm_pause();
    }
    for (std::uint16_t idx : removed) {
        g_hookEntries[idx] = SlotHookEntry{};
    }
}

MhwRngHookHandle RegisterGlobalObserver(MhwRngPluginId ownerId,
                                        std::uint32_t rvaFilterOrZero,
                                        std::uint32_t sourceKindMask,
                                        MhwRngObserverFn fn,
                                        void* userData) noexcept {
    if (!fn) return kMhwRngInvalidHandle;
    const std::uint32_t normFilter = (rvaFilterOrZero != 0U) ? NormalizeUserSiteAddress(rvaFilterOrZero) : 0U;

    std::lock_guard<std::mutex> lk(g_hookRegMx);
    for (std::uint32_t i = 0; i < kMaxGlobalObservers; ++i) {
        if (!g_obsStorage[i].active) {
            const MhwRngHookHandle handle = MakeHookHandle(kMaxSlotHooks + i);
            g_obsStorage[i].active     = true;
            g_obsStorage[i].handle     = handle;
            g_obsStorage[i].ownerId    = ownerId;
            g_obsStorage[i].rvaFilter  = normFilter;
            g_obsStorage[i].sourceMask = sourceKindMask;
            g_obsStorage[i].fn         = fn;
            g_obsStorage[i].userData   = userData;
            PublishObserverSnapshotLocked();
            return handle;
        }
    }
    return kMhwRngInvalidHandle;
}

void UnregisterGlobalObserver(MhwRngPluginId ownerId, MhwRngHookHandle handle) noexcept {
    if (handle == kMhwRngInvalidHandle) return;
    std::lock_guard<std::mutex> lk(g_hookRegMx);
    for (std::uint32_t i = 0; i < kMaxGlobalObservers; ++i) {
        if (g_obsStorage[i].active && g_obsStorage[i].handle == handle &&
            (ownerId == 0U || g_obsStorage[i].ownerId == ownerId)) {
            g_obsStorage[i] = GlobalObserverEntry{};
            PublishObserverSnapshotLocked();
            return;
        }
    }
}

bool RegisterTransparentWrapper(MhwRngPluginId ownerId,
                                std::uint32_t wrapperRetRva,
                                std::uint16_t frameQwords) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(wrapperRetRva);
    return RegisterWrapperInternal(ownerId, norm, frameQwords);
}

void UnregisterTransparentWrapper(MhwRngPluginId ownerId, std::uint32_t wrapperRetRva) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(wrapperRetRva);
    if (norm == 0U) return;
    std::lock_guard<std::mutex> lk(g_wrapperMx);
    for (std::uint32_t i = 0; i < kMaxWrappers; ++i) {
        const std::uint64_t p = g_wrappers[i].load(std::memory_order_relaxed);
        if (p == 0ULL) continue;
        const auto pOwner = static_cast<MhwRngPluginId>((p >> 48) & 0xFFFFU);
        if (static_cast<std::uint32_t>(p) == norm &&
            (ownerId == 0U || pOwner == ownerId)) {
            g_wrappers[i].store(0ULL, std::memory_order_release);
        }
    }
}

void DetachAllHooksForPluginAndWait(MhwRngPluginId ownerId) noexcept {
    {
        std::lock_guard<std::mutex> lk(g_hookRegMx);
        std::vector<std::uint32_t> affectedRvas;
        std::vector<std::uint16_t> removedEntries;
        for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
            if (g_hookEntries[i].active && g_hookEntries[i].ownerId == ownerId) {
                g_hookEntries[i].active = false;
                removedEntries.push_back(i);
                if (std::find(affectedRvas.begin(), affectedRvas.end(), g_hookEntries[i].rva) == affectedRvas.end()) {
                    affectedRvas.push_back(g_hookEntries[i].rva);
                }
            }
        }
        for (std::uint32_t rva : affectedRvas) {
            if (RngSlot* base = FindExistingCompositeSlot(MakeCompositeKey(rva, 0U))) {
                RebuildChainForRvaLocked(rva, base);
            }
        }

        bool obsChanged = false;
        for (std::uint32_t i = 0; i < kMaxGlobalObservers; ++i) {
            if (g_obsStorage[i].active && g_obsStorage[i].ownerId == ownerId) {
                g_obsStorage[i] = GlobalObserverEntry{};
                obsChanged = true;
            }
        }
        if (obsChanged) {
            PublishObserverSnapshotLocked();
        } else if (!removedEntries.empty()) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            while (g_inFlightHookCalls.load(std::memory_order_seq_cst) != 0U) {
                _mm_pause();
            }
        }
        for (std::uint16_t idx : removedEntries) {
            g_hookEntries[idx] = SlotHookEntry{};
        }
    }

    {
        std::lock_guard<std::mutex> wlk(g_wrapperMx);
        for (std::uint32_t i = 0; i < kMaxWrappers; ++i) {
            const std::uint64_t p = g_wrappers[i].load(std::memory_order_relaxed);
            if (p != 0ULL && static_cast<MhwRngPluginId>((p >> 48) & 0xFFFFU) == ownerId) {
                g_wrappers[i].store(0ULL, std::memory_order_release);
            }
        }
    }
}

bool RestoreSlotState(const MhwRngSlotState* state) noexcept {
    if (!state) return false;
    return RestoreSingleSlotStateInternal(*state, g_seedEpoch.load(std::memory_order_acquire));
}

std::uint32_t CaptureStateBlob(void* outBuf, std::uint32_t bufCap) noexcept {
    std::vector<MhwRngSlotState> states;
    std::uint64_t capSeed  = 0ULL;
    std::uint64_t capEpoch = 0ULL;
    {
        std::lock_guard<std::mutex> tlk(g_slotTableMutMx);
        capSeed  = g_masterSeed.load(std::memory_order_acquire);
        capEpoch = g_seedEpoch.load(std::memory_order_acquire);
        states.reserve(g_activeSlotCount.load(std::memory_order_relaxed));
        for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
            const std::uint64_t k = g_slots[i].key.load(std::memory_order_acquire);
            if (k == kSlotKeyEmpty || k == kSlotKeyTombstone) continue;
            MhwRngSlotState st{};
            st.callSiteRva = static_cast<std::uint32_t>(k & 0xFFFFFFFFULL);
            st.condId      = static_cast<std::uint32_t>(k >> 32);
            {
                SlotSpinGuard g(g_slots[i].lock);
                std::memcpy(st.state, g_slots[i].state, sizeof(st.state));
                st.seqCursor    = g_slots[i].seqCursor;
                st.activeCondId = (st.condId == 0U) ? g_slots[i].activeCond.load(std::memory_order_relaxed) : 0U;
                st.epochCalls   = g_slots[i].epochCalls;
                st.lastValue    = g_slots[i].lastValue;
                st.flags        = (g_slots[i].seededEpoch == capEpoch)
                                      ? kMhwRngRestoreAll
                                      : (kMhwRngRestoreSeqCursor | kMhwRngRestoreActiveCond | kMhwRngRestoreCounters);
            }
            states.push_back(st);
        }
    }

    const std::size_t payloadBytes = states.size() * sizeof(MhwRngSlotState);
    const auto needed = static_cast<std::uint32_t>(sizeof(StateBlobHeader) + payloadBytes);
    if (!outBuf || bufCap == 0U) {
        return needed;
    }
    if (bufCap < needed) {
        return 0U;
    }

    StateBlobHeader hdr{};
    hdr.magic        = kStateBlobMagic;
    hdr.version      = kStateBlobVersion;
    hdr.masterSeed   = capSeed;
    hdr.epoch        = capEpoch;
    hdr.slotCount    = static_cast<std::uint32_t>(states.size());
    hdr.payloadCrc32 = payloadBytes > 0U ? Crc32Compute(states.data(), payloadBytes) : 0U;

    auto* dst = static_cast<std::uint8_t*>(outBuf);
    std::memcpy(dst, &hdr, sizeof(hdr));
    if (payloadBytes > 0U) {
        std::memcpy(dst + sizeof(hdr), states.data(), payloadBytes);
    }
    return needed;
}

bool RestoreStateBlob(const void* buf, std::uint32_t bufSize) noexcept {
    if (!buf || bufSize < sizeof(StateBlobHeader)) return false;
    StateBlobHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != kStateBlobMagic || hdr.version != kStateBlobVersion || hdr.slotCount > kSlotTableSize) {
        return false;
    }
    const std::size_t payloadBytes = static_cast<std::size_t>(hdr.slotCount) * sizeof(MhwRngSlotState);
    if (bufSize < sizeof(StateBlobHeader) + payloadBytes) {
        return false;
    }
    const auto* payload = static_cast<const std::uint8_t*>(buf) + sizeof(StateBlobHeader);
    const std::uint32_t crc = payloadBytes > 0U ? Crc32Compute(payload, payloadBytes) : 0U;
    if (crc != hdr.payloadCrc32) {
        return false;
    }

    const std::uint64_t prevEpoch = g_seedEpoch.load(std::memory_order_acquire);
    g_masterSeed.store(hdr.masterSeed, std::memory_order_release);
    g_seedEpoch.store(hdr.epoch, std::memory_order_release);

    {
        std::lock_guard<std::mutex> tlk(g_slotTableMutMx);
        for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
            const std::uint64_t k = g_slots[i].key.load(std::memory_order_acquire);
            if (k == kSlotKeyEmpty || k == kSlotKeyTombstone) continue;
            if ((k >> 32) == 0ULL) {
                g_slots[i].activeCond.store(0U, std::memory_order_release);
            }
            SlotSpinGuard g(g_slots[i].lock);
            g_slots[i].seededEpoch = 0ULL;
            g_slots[i].seqCursor   = 0U;
            g_slots[i].epochCalls  = 0ULL;
        }
    }

    const auto* recs = reinterpret_cast<const MhwRngSlotState*>(payload);
    for (std::uint32_t i = 0; i < hdr.slotCount; ++i) {
        RestoreSingleSlotStateInternal(recs[i], hdr.epoch);
    }

    EmitLifecycleEvent(kMhwRngEventStateRestored, prevEpoch);
    return true;
}

bool SetInstancePolicy(std::uintptr_t instancePtr, std::uint32_t policy) noexcept {
    if (!instancePtr || instancePtr == g_sInstanceAddr || policy > kMhwRngInstForceSlotControl) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_instPolicyMx);
    for (std::uint32_t i = 0; i < kMaxInstanceTags; ++i) {
        if (g_instanceTags[i].ptr.load(std::memory_order_relaxed) == instancePtr) {
            g_instanceTags[i].policy.store(policy, std::memory_order_release);
            return true;
        }
    }
    const std::uint32_t idx = g_tagRingCursor.fetch_add(1U, std::memory_order_relaxed) & (kMaxInstanceTags - 1U);
    g_instanceTags[idx].epoch.store(0ULL, std::memory_order_relaxed);
    g_instanceTags[idx].ptr.store(instancePtr, std::memory_order_relaxed);
    g_instanceTags[idx].policy.store(policy, std::memory_order_release);
    return true;
}

std::uint32_t GetInstancePolicy(std::uintptr_t instancePtr) noexcept {
    if (!instancePtr || instancePtr == g_sInstanceAddr) return kMhwRngInstAuto;
    for (std::uint32_t i = 0; i < kMaxInstanceTags; ++i) {
        if (g_instanceTags[i].ptr.load(std::memory_order_relaxed) == instancePtr) {
            return g_instanceTags[i].policy.load(std::memory_order_acquire);
        }
    }
    return kMhwRngInstAuto;
}

void SetOrigMtRandomCtorSeed(void* fn) noexcept    { g_origMtCtorSeed   = reinterpret_cast<MtCtorSeedFn>(fn); }
void SetOrigMtRandomCtorDefault(void* fn) noexcept { g_origMtCtorDef    = reinterpret_cast<MtCtorDefFn>(fn); }
void SetOrigMtRandomSrand(void* fn) noexcept       { g_origMtSrand      = reinterpret_cast<MtSrandFn>(fn); }
void SetOrigMtRandomNrand(void* fn) noexcept       { g_origMtNrand      = reinterpret_cast<MtNrandFn>(fn); }
void SetOrigMtRandomSphere(void* fn) noexcept      { g_origMtSphere     = reinterpret_cast<MtSphereFn>(fn); }
void SetOrigStdRandomDevice(void* fn) noexcept     { g_origStdRandDev   = reinterpret_cast<StdRandDevFn>(fn); }
void SetOrigMt19937Range(void* fn) noexcept        { g_origMt19937Range = reinterpret_cast<Mt19937RangeFn>(fn); }
void SetOrigMt19937HeapInit(void* fn) noexcept     { g_origMt19937Init  = reinterpret_cast<Mt19937InitFn>(fn); }
void SetOrigMt19937HeapDraw(void* fn) noexcept     { g_origMt19937Draw  = reinterpret_cast<Mt19937DrawFn>(fn); }
void SetOrigCrtRand(void* fn) noexcept             { g_origCrtRand      = reinterpret_cast<CrtRandFn>(fn); }

std::uint32_t CountOverriddenSlots() noexcept {
    std::uint32_t cnt = 0U;
    for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
        const std::uint64_t k = g_slots[i].key.load(std::memory_order_acquire);
        if (k == kSlotKeyEmpty || k == kSlotKeyTombstone) continue;
        SlotSpinGuard g(g_slots[i].lock);
        if (g_slots[i].mode != kMhwRngSlotMasterDerived ||
            DecodeChainIdx(g_slots[i].hookChainIdxPlus1.load(std::memory_order_relaxed)) < kMaxHookChains) {
            ++cnt;
        }
    }
    return cnt;
}

std::uint32_t CountHookedSlots() noexcept {
    std::uint32_t cnt = 0U;
    std::lock_guard<std::mutex> lk(g_hookRegMx);
    for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
        if (g_hookEntries[i].active) ++cnt;
    }
    return cnt;
}

const char* ModeName(std::uint8_t mode) noexcept {
    switch (mode) {
        case kMhwRngSlotMasterDerived: return "MasterDerived";
        case kMhwRngSlotCustomSeed:    return "CustomSeed";
        case kMhwRngSlotFixedValue:    return "FixedValue";
        case kMhwRngSlotSequence:      return "Sequence";
        case kMhwRngSlotBypass:        return "Bypass";
        default:                       return "Unknown";
    }
}

const char* SourceKindName(std::uint8_t src) noexcept {
    switch (src) {
        case kMhwRngSrcMtNrandGlobal:   return "MtNrandGlobal";
        case kMhwRngSrcMtNrandPrivate:  return "MtNrandPriv";
        case kMhwRngSrcMtCtorSeed:      return "MtCtorSeed";
        case kMhwRngSrcMtCtorDefault:   return "MtCtorDefault";
        case kMhwRngSrcMtSrand:         return "MtSrand";
        case kMhwRngSrcMtSphere:        return "MtSphereVec";
        case kMhwRngSrcStdRandomDevice: return "StdRandDevice";
        case kMhwRngSrcMt19937Range:    return "Mt19937Range";
        case kMhwRngSrcMt19937HeapInit: return "Mt19937Init";
        case kMhwRngSrcMt19937HeapDraw: return "Mt19937Draw";
        case kMhwRngSrcCrtRand:         return "CrtRand";
        default:                        return "Pending";
    }
}

} // namespace rng

// ============================================================================
//  10 C++ Detour Targets Called by MASM Thunks (`src/rng_trampolines.asm`)
// ============================================================================

extern "C" {

std::uint32_t* __fastcall HookImpl_MtRandomCtorSeed(std::uint32_t* self,
                                                    std::uint32_t seed,
                                                    const MhwRngCallerRegs* regs) {
    if (!self) return nullptr;
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        return rng::g_origMtCtorSeed(self, seed);
    }
    const auto selfAddr = reinterpret_cast<std::uintptr_t>(self);
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcMtCtorSeed,
        selfAddr,
        false,
        static_cast<std::uint64_t>(seed),
        0ULL,
        0.0f,
        0.0f,
        kMhwRngDrawFlagHasOrigSeed,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float&) {
            rng::g_origMtCtorSeed(self, seed);
            v0 = self[3];
        });
    if (dr.bypassed) return self;

    rng::InitMhwMtRandomWords(self, dr.val0);
    if (dr.mode != kMhwRngSlotMasterDerived || dr.overridden) {
        rng::TagExplicitlySeededInstance(selfAddr, rng::g_seedEpoch.load(std::memory_order_relaxed));
    }
    return self;
}

std::uint32_t* __fastcall HookImpl_MtRandomCtorDefault(std::uint32_t* self,
                                                       const MhwRngCallerRegs* regs) {
    if (!self) return nullptr;
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        return rng::g_origMtCtorDef(self);
    }
    const auto selfAddr = reinterpret_cast<std::uintptr_t>(self);
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcMtCtorDefault,
        selfAddr,
        false,
        0ULL,
        0ULL,
        0.0f,
        0.0f,
        0U,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float&) {
            rng::g_origMtCtorDef(self);
            v0 = self[3];
        });
    if (dr.bypassed) return self;

    rng::InitMhwMtRandomWords(self, dr.val0);
    if (dr.mode != kMhwRngSlotMasterDerived || dr.overridden) {
        rng::TagExplicitlySeededInstance(selfAddr, rng::g_seedEpoch.load(std::memory_order_relaxed));
    }
    return self;
}

void __fastcall HookImpl_MtRandomSrand(std::uint32_t* self,
                                       std::uint32_t seed,
                                       const MhwRngCallerRegs* regs) {
    if (!self) return;
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        rng::g_origMtSrand(self, seed);
        return;
    }
    const auto selfAddr = reinterpret_cast<std::uintptr_t>(self);
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcMtSrand,
        selfAddr,
        false,
        static_cast<std::uint64_t>(seed),
        0ULL,
        0.0f,
        0.0f,
        kMhwRngDrawFlagHasOrigSeed,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float&) {
            rng::g_origMtSrand(self, seed);
            v0 = self[3];
        });
    if (dr.bypassed) return;

    if (selfAddr != rng::g_sInstanceAddr) {
        rng::InitMhwMtRandomWords(self, dr.val0);
        if (dr.mode != kMhwRngSlotMasterDerived || dr.overridden) {
            rng::TagExplicitlySeededInstance(selfAddr, rng::g_seedEpoch.load(std::memory_order_relaxed));
        }
    }
}

std::uint32_t __fastcall HookImpl_MtRandomNrand(std::uint32_t* self,
                                                const MhwRngCallerRegs* regs) {
    if (!self) return 0U;
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        return rng::g_origMtNrand(self);
    }
    const auto selfAddr     = reinterpret_cast<std::uintptr_t>(self);
    const bool isGlobalInst = (selfAddr == rng::g_sInstanceAddr);
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        isGlobalInst ? kMhwRngSrcMtNrandGlobal : kMhwRngSrcMtNrandPrivate,
        selfAddr,
        false,
        0ULL,
        0ULL,
        0.0f,
        0.0f,
        0U,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float&) {
            v0 = rng::g_origMtNrand(self);
        },
        isGlobalInst ? nullptr : self);
    return dr.val0;
}

float* __fastcall HookImpl_MtRandomSphere(std::uint32_t* self,
                                          float* outVec3,
                                          const MhwRngCallerRegs* regs) {
    if (!self || !outVec3) return outVec3;
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        return rng::g_origMtSphere(self, outVec3);
    }
    const auto selfAddr     = reinterpret_cast<std::uintptr_t>(self);
    const bool isGlobalInst = (selfAddr == rng::g_sInstanceAddr);
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcMtSphere,
        selfAddr,
        true,
        reinterpret_cast<std::uintptr_t>(outVec3),
        0ULL,
        0.0f,
        0.0f,
        0U,
        regs,
        [&](std::uint32_t& v0, std::uint32_t& v1, float&) {
            rng::g_origMtSphere(self, outVec3);
            v0 = self[2];
            v1 = self[3];
        });
    if (dr.bypassed) return outVec3;

    const std::uint32_t u1 = dr.val0;
    const std::uint32_t u2 = dr.val1;
    if (!isGlobalInst) {
        if (dr.hadVanilla) {
            // Vanilla `randUnitSphere` already shifted `self[0..3]` once; only sync final emitted words
            self[2] = u1 != 0U ? u1 : 0x075BCD15U;
            self[3] = u2 != 0U ? u2 : 0x159A55E5U;
        } else {
            self[0] = self[2];
            self[1] = self[3];
            self[2] = u1 != 0U ? u1 : 0x075BCD15U;
            self[3] = u2 != 0U ? u2 : 0x159A55E5U;
        }
    }

    const float u1f   = static_cast<float>(static_cast<std::int64_t>(static_cast<std::uint64_t>(u1)));
    const float u2f   = static_cast<float>(static_cast<std::int64_t>(static_cast<std::uint64_t>(u2)));
    const float z     = (u1f * rng::kFloatInv2Pow32 + u1f * rng::kFloatInv2Pow32) - 1.0f;
    const float rSq   = 1.0f - z * z;
    const float rXY   = (rSq > 0.0f) ? std::sqrt(rSq) : 0.0f;
    const float angle = u2f * rng::kFloatTwoPiOver2P32;

    outVec3[1] = z;
    outVec3[0] = std::cos(angle) * rXY;
    outVec3[2] = std::sin(angle) * rXY;
    return outVec3;
}

std::uint32_t __cdecl HookImpl_StdRandomDevice(const MhwRngCallerRegs* regs) {
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        return rng::g_origStdRandDev();
    }
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcStdRandomDevice,
        0ULL,
        false,
        0ULL,
        0ULL,
        0.0f,
        0.0f,
        0U,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float&) {
            v0 = rng::g_origStdRandDev();
        });
    return dr.val0;
}

float __fastcall HookImpl_Mt19937Range(void* ctx,
                                       float minVal,
                                       float maxVal,
                                       const MhwRngCallerRegs* regs) {
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        return rng::g_origMt19937Range(ctx, minVal, maxVal);
    }
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcMt19937Range,
        reinterpret_cast<std::uintptr_t>(ctx),
        false,
        rng::FloatToBits(minVal),
        rng::FloatToBits(maxVal),
        minVal,
        maxVal,
        kMhwRngDrawFlagHasRange,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float& vf) {
            vf = rng::g_origMt19937Range(ctx, minVal, maxVal);
            v0 = rng::FloatToBits(vf);
        });
    return dr.finalFloat;
}

void __fastcall HookImpl_Mt19937HeapInit(void* state,
                                         std::uint32_t seed,
                                         const MhwRngCallerRegs* regs) {
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        rng::g_origMt19937Init(state, seed);
        return;
    }
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcMt19937HeapInit,
        reinterpret_cast<std::uintptr_t>(state),
        false,
        static_cast<std::uint64_t>(seed),
        0ULL,
        0.0f,
        0.0f,
        kMhwRngDrawFlagHasOrigSeed,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float&) {
            rng::g_origMt19937Init(state, seed);
            v0 = seed;
        });
    if (!dr.bypassed) {
        rng::g_origMt19937Init(state, dr.val0);
    }
}

std::uint32_t __fastcall HookImpl_Mt19937HeapDraw(void* state,
                                                  const MhwRngCallerRegs* regs) {
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        return rng::g_origMt19937Draw(state);
    }
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcMt19937HeapDraw,
        reinterpret_cast<std::uintptr_t>(state),
        false,
        0ULL,
        0ULL,
        0.0f,
        0.0f,
        0U,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float&) {
            v0 = rng::g_origMt19937Draw(state);
        });
    return dr.val0;
}

int __cdecl HookImpl_CrtRand(const MhwRngCallerRegs* regs) {
    if (rng::g_engineMode.load(std::memory_order_relaxed) == kMhwRngModeOff) {
        return rng::g_origCrtRand();
    }
    const rng::DrawExecResult dr = rng::ExecuteRngDraw(
        kMhwRngSrcCrtRand,
        0ULL,
        false,
        0ULL,
        0ULL,
        0.0f,
        0.0f,
        0U,
        regs,
        [&](std::uint32_t& v0, std::uint32_t&, float&) {
            v0 = static_cast<std::uint32_t>(rng::g_origCrtRand());
        });
    if (dr.bypassed) {
        return static_cast<int>(dr.val0);
    }
    if ((dr.mode == kMhwRngSlotFixedValue || dr.mode == kMhwRngSlotSequence) && dr.val0 <= 0x7FFFU) {
        return static_cast<int>(dr.val0);
    }
    return static_cast<int>((dr.val0 >> 16) & 0x7FFFU);
}

} // extern "C"

// ============================================================================
//  Exported C API Implementation (mhw_rng_api.h v3.0)
// ============================================================================

MHW_RNG_API std::uint32_t MhwRng_GetApiVersion() {
    return kMhwRngApiVersion;
}

MHW_RNG_API void MhwRng_SetEngineMode(std::uint32_t mode) {
    const std::uint32_t clamped = (mode <= kMhwRngModeSelective) ? mode : kMhwRngModeSelective;
    const std::uint32_t prev    = rng::g_engineMode.exchange(clamped, std::memory_order_acq_rel);
    rng::EmitLifecycleEvent(kMhwRngEventEngineMode, prev);
}

MHW_RNG_API std::uint32_t MhwRng_GetEngineMode() {
    return rng::g_engineMode.load(std::memory_order_acquire);
}

MHW_RNG_API void MhwRng_SetTakeoverEnabled(bool enabled) {
    if (!enabled) {
        MhwRng_SetEngineMode(kMhwRngModeOff);
        return;
    }
    const std::uint32_t cur = MhwRng_GetEngineMode();
    if (cur == kMhwRngModeTakeover || cur == kMhwRngModeSelective) {
        return;
    }
    MhwRng_SetEngineMode(kMhwRngModeSelective);
}

MHW_RNG_API bool MhwRng_IsTakeoverEnabled() {
    const std::uint32_t m = rng::g_engineMode.load(std::memory_order_acquire);
    return m == kMhwRngModeTakeover || m == kMhwRngModeSelective;
}

MHW_RNG_API void MhwRng_SetMasterSeed(std::uint64_t seed, bool resetEpoch) {
    const std::uint64_t prev = rng::g_masterSeed.exchange(seed, std::memory_order_acq_rel);
    if (resetEpoch) {
        rng::g_seedEpoch.fetch_add(1ULL, std::memory_order_acq_rel);
    }
    rng::EmitLifecycleEvent(kMhwRngEventMasterSeed, prev);
}

MHW_RNG_API std::uint64_t MhwRng_GetMasterSeed() {
    return rng::g_masterSeed.load(std::memory_order_acquire);
}

MHW_RNG_API std::uint64_t MhwRng_ResetEpoch() {
    const std::uint64_t prev = rng::g_seedEpoch.fetch_add(1ULL, std::memory_order_acq_rel);
    rng::EmitLifecycleEvent(kMhwRngEventEpochReset, prev);
    return prev + 1ULL;
}

MHW_RNG_API std::uint64_t MhwRng_GetEpoch() {
    return rng::g_seedEpoch.load(std::memory_order_acquire);
}

MHW_RNG_API bool MhwRng_SetSlotMasterDerived(std::uint32_t callSiteRva) {
    return rng::SetCondMasterDerived(kMhwRngPluginIdKernel, callSiteRva, 0U);
}

MHW_RNG_API bool MhwRng_SetSlotCustomSeed(std::uint32_t callSiteRva, std::uint64_t seed) {
    return rng::SetCondCustomSeed(kMhwRngPluginIdKernel, callSiteRva, 0U, seed);
}

MHW_RNG_API bool MhwRng_SetSlotFixedValue(std::uint32_t callSiteRva, std::uint32_t value) {
    return rng::SetCondFixedValue(kMhwRngPluginIdKernel, callSiteRva, 0U, value);
}

MHW_RNG_API bool MhwRng_SetSlotSequence(std::uint32_t callSiteRva,
                                        const std::uint32_t* values,
                                        std::uint32_t count,
                                        std::uint8_t endPolicy) {
    return rng::SetCondSequence(kMhwRngPluginIdKernel, callSiteRva, 0U, values, count, endPolicy);
}

MHW_RNG_API bool MhwRng_SetSlotBypass(std::uint32_t callSiteRva) {
    return rng::SetCondBypass(kMhwRngPluginIdKernel, callSiteRva, 0U);
}

MHW_RNG_API void MhwRng_ClearAllSlotOverrides() {
    {
        std::lock_guard<std::mutex> tlk(rng::g_slotTableMutMx);
        for (std::uint32_t i = 0; i < rng::kSlotTableSize; ++i) {
            const std::uint64_t k = rng::g_slots[i].key.load(std::memory_order_acquire);
            if (k == rng::kSlotKeyEmpty || k == rng::kSlotKeyTombstone) continue;
            if ((k >> 32) == 0ULL) {
                rng::g_slots[i].activeCond.store(0U, std::memory_order_release);
            }
            std::uint32_t* toFree = nullptr;
            {
                rng::SlotSpinGuard g(rng::g_slots[i].lock);
                if (rng::g_slots[i].mode != kMhwRngSlotMasterDerived) {
                    toFree                       = rng::g_slots[i].seqHeap;
                    rng::g_slots[i].seqHeap      = nullptr;
                    rng::g_slots[i].useHeapSeq   = 0U;
                    rng::g_slots[i].mode         = kMhwRngSlotMasterDerived;
                    rng::g_slots[i].ownerPluginId= kMhwRngPluginIdKernel;
                    rng::g_slots[i].seqLength    = 0U;
                    rng::g_slots[i].seqCursor    = 0U;
                    rng::g_slots[i].seededEpoch  = 0ULL;
                }
            }
            delete[] toFree;
        }
    }
    rng::EmitLifecycleEvent(kMhwRngEventClearAllRules, 0ULL);
}

MHW_RNG_API bool MhwRng_GetSlotInfo(std::uint32_t callSiteRva, MhwRngSlotSnapshot* outSnapshot) {
    return rng::GetCondSlotInfo(callSiteRva, 0U, outSnapshot);
}

MHW_RNG_API std::uint32_t MhwRng_GetActiveSlotCount() {
    return rng::g_activeSlotCount.load(std::memory_order_acquire);
}

MHW_RNG_API std::uint32_t MhwRng_SnapshotActiveSlots(MhwRngSlotSnapshot* outArray, std::uint32_t maxCount) {
    if (!outArray || maxCount == 0U) {
        return MhwRng_GetActiveSlotCount();
    }
    std::uint32_t written = 0U;
    for (std::uint32_t i = 0; i < rng::kSlotTableSize && written < maxCount; ++i) {
        const std::uint64_t compKey = rng::g_slots[i].key.load(std::memory_order_acquire);
        if (compKey == rng::kSlotKeyEmpty || compKey == rng::kSlotKeyTombstone) continue;
        MhwRngSlotSnapshot& dst = outArray[written++];
        rng::SlotSpinGuard g(rng::g_slots[i].lock);
        if (rng::g_slots[i].key.load(std::memory_order_relaxed) != compKey) {
            --written;
            continue;
        }
        rng::FillSnapshotLocked(rng::g_slots[i], compKey, dst);
    }
    return written;
}

MHW_RNG_API bool MhwRng_RestoreSlotState(const MhwRngSlotState* state) {
    return rng::RestoreSlotState(state);
}

MHW_RNG_API std::uint32_t MhwRng_CaptureStateBlob(void* outBuf, std::uint32_t bufCap) {
    return rng::CaptureStateBlob(outBuf, bufCap);
}

MHW_RNG_API bool MhwRng_RestoreStateBlob(const void* buf, std::uint32_t bufSize) {
    return rng::RestoreStateBlob(buf, bufSize);
}

MHW_RNG_API bool MhwRng_SetInstancePolicy(std::uintptr_t instancePtr, std::uint32_t policy) {
    return rng::SetInstancePolicy(instancePtr, policy);
}

MHW_RNG_API std::uint32_t MhwRng_GetInstancePolicy(std::uintptr_t instancePtr) {
    return rng::GetInstancePolicy(instancePtr);
}
