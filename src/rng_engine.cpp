#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#include "rng_engine.h"
#include "mhw_addresses.h"

#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma intrinsic(_ReturnAddress)
#pragma intrinsic(_AddressOfReturnAddress)

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
//  Composite-Key Open-Addressed Global Slot Table + Quiescent Hook Table
// ============================================================================

constexpr std::uint32_t kSlotTableSize  = 8192U;
constexpr std::uint32_t kSlotTableMask  = kSlotTableSize - 1U;
constexpr std::uint32_t kInlineSeqCap   = 8U;
constexpr std::uint32_t kSeqPoolCap     = 65536U;
constexpr std::uint16_t kInvalidHookIdx = 0xFFFFU;
constexpr std::uint32_t kMaxSlotHooks   = 128U;

struct SlotHookEntry {
    std::atomic<bool>           active{false};
    MhwRngPluginId              ownerId = 0U;
    std::uint32_t               rva     = 0U;
    std::atomic<MhwRngSlotHookFn> fn{nullptr};
    std::atomic<void*>          userData{nullptr};
};

#pragma warning(push)
#pragma warning(disable : 4324)
struct alignas(64) RngSlot {
    std::atomic<std::uint64_t>  key{0};          // (condId << 32) | callSiteRva; 0 = empty
    std::atomic_flag            lock = ATOMIC_FLAG_INIT;
    std::uint8_t                mode             = kMhwRngSlotMasterDerived;
    std::uint8_t                sourceKind       = kMhwRngSrcUnknown;
    std::uint8_t                seqEndPolicy     = kMhwRngSeqLoop;
    std::uint8_t                useInlineSeq     = 1;
    MhwRngPluginId              ownerPluginId    = 0U;
    std::atomic<std::uint16_t>  hookIdx{kInvalidHookIdx}; // Only used on base slot (condId == 0)
    std::atomic<std::uint32_t>  activeCond{0U};           // Push-mode condition on base slot (condId == 0)

    std::uint32_t               state[4]{};
    std::uint64_t               seededEpoch      = 0;
    std::uint64_t               customSeed       = 0;
    std::uint32_t               fixedValue       = 0;
    std::uint32_t               lastValue        = 0;

    std::uint64_t               totalCalls       = 0;
    std::uint64_t               epochCalls       = 0;

    std::uint32_t               seqOffset        = 0;
    std::uint32_t               seqLength        = 0;
    std::uint32_t               seqCursor        = 0;
    std::uint32_t               inlineSeq[kInlineSeqCap]{};
};
#pragma warning(pop)

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
std::atomic<std::uint32_t> g_activeSlotCount{0};

SlotHookEntry              g_hookEntries[kMaxSlotHooks]{};
std::mutex                 g_hookRegMx;
std::atomic<std::uint32_t> g_inFlightHookCalls{0};

std::uint32_t              g_seqPool[kSeqPoolCap]{};
std::uint32_t              g_seqPoolUsed = 0;
std::mutex                 g_seqAllocMx;

std::uintptr_t             g_exeBase       = 0;
std::uintptr_t             g_exeEnd        = 0;
std::uintptr_t             g_sInstanceAddr = 0;

std::atomic<bool>          g_takeoverEnabled{true};
std::atomic<std::uint64_t> g_masterSeed{12345ULL};
std::atomic<std::uint64_t> g_seedEpoch{1ULL};

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

struct OverriddenInstanceTag {
    std::atomic<std::uintptr_t> ptr{0};
    std::atomic<std::uint64_t>  epoch{0};
};
constexpr std::uint32_t    kTagRingSize = 64U;
OverriddenInstanceTag      g_overriddenInstances[kTagRingSize]{};
std::atomic<std::uint32_t> g_tagRingCursor{0};

void TagExplicitlySeededInstance(std::uintptr_t instancePtr, std::uint64_t epoch) noexcept {
    if (!instancePtr || instancePtr == g_sInstanceAddr) return;
    for (std::uint32_t i = 0; i < kTagRingSize; ++i) {
        if (g_overriddenInstances[i].ptr.load(std::memory_order_relaxed) == instancePtr) {
            g_overriddenInstances[i].epoch.store(epoch, std::memory_order_release);
            return;
        }
    }
    const std::uint32_t idx = g_tagRingCursor.fetch_add(1U, std::memory_order_relaxed) & (kTagRingSize - 1U);
    g_overriddenInstances[idx].ptr.store(instancePtr, std::memory_order_relaxed);
    g_overriddenInstances[idx].epoch.store(epoch, std::memory_order_release);
}

bool IsExplicitlySeededInCurrentEpoch(std::uintptr_t instancePtr, std::uint64_t curEpoch) noexcept {
    if (!instancePtr || instancePtr == g_sInstanceAddr) return false;
    for (std::uint32_t i = 0; i < kTagRingSize; ++i) {
        if (g_overriddenInstances[i].ptr.load(std::memory_order_relaxed) == instancePtr &&
            g_overriddenInstances[i].epoch.load(std::memory_order_acquire) == curEpoch) {
            return true;
        }
    }
    return false;
}

inline std::uint32_t ComputeCallSiteRva(const void* retSlotAddr) noexcept {
    const auto* slotPtr = static_cast<const std::uintptr_t*>(retSlotAddr);
    const std::uintptr_t ret  = *slotPtr;
    const std::uintptr_t base = g_exeBase;
    const std::uintptr_t end  = g_exeEnd;
    if (ret >= base && ret < end) {
        auto rva = static_cast<std::uint32_t>(ret - base);
        // Unwrap 1-frame thin engine RNG wrappers (+0x28 frame + 8B retaddr = +6 QWORDs)
        if (rva == 0x04F71BFU || rva == 0x2396F02U || rva == 0x2396F2DU ||
            rva == 0x197D6A2U || rva == 0x197D6B5U) {
            const std::uintptr_t outerRet = slotPtr[6];
            if (outerRet >= base && outerRet < end) {
                rva = static_cast<std::uint32_t>(outerRet - base);
            }
        }
        return rva != 0U ? rva : 1U;
    }
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

RngSlot* FindOrInsertCompositeSlot(std::uint64_t compKey) noexcept {
    if (compKey == 0ULL) return nullptr;
    std::uint32_t idx = HashCompositeKey(compKey) & kSlotTableMask;
    for (std::uint32_t probe = 0; probe < kSlotTableSize; ++probe) {
        RngSlot& slot = g_slots[idx];
        const std::uint64_t curKey = slot.key.load(std::memory_order_acquire);
        if (curKey == compKey) {
            return &slot;
        }
        if (curKey == 0ULL) {
            std::uint64_t expected = 0ULL;
            if (slot.key.compare_exchange_strong(
                    expected, compKey, std::memory_order_acq_rel, std::memory_order_acquire)) {
                g_activeSlotCount.fetch_add(1U, std::memory_order_relaxed);
                return &slot;
            }
            if (expected == compKey) {
                return &slot;
            }
        }
        idx = (idx + 1U) & kSlotTableMask;
    }
    return nullptr;
}

RngSlot* FindExistingCompositeSlot(std::uint64_t compKey) noexcept {
    if (compKey == 0ULL) return nullptr;
    std::uint32_t idx = HashCompositeKey(compKey) & kSlotTableMask;
    for (std::uint32_t probe = 0; probe < kSlotTableSize; ++probe) {
        RngSlot& slot = g_slots[idx];
        const std::uint64_t curKey = slot.key.load(std::memory_order_acquire);
        if (curKey == compKey) return &slot;
        if (curKey == 0ULL)    return nullptr;
        idx = (idx + 1U) & kSlotTableMask;
    }
    return nullptr;
}

std::uint32_t __fastcall DrawDeterministicHelper(std::uint32_t callSiteRva, std::uint32_t condId) {
    const std::uint64_t compKey = MakeCompositeKey(callSiteRva, condId);
    RngSlot* slot = FindOrInsertCompositeSlot(compKey);
    if (!slot) return 0U;

    const std::uint64_t curEpoch   = g_seedEpoch.load(std::memory_order_acquire);
    const std::uint64_t masterSeed = g_masterSeed.load(std::memory_order_relaxed);

    SlotSpinGuard g(slot->lock);
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
        decision->action = kMhwRngActionUseCondition;
        decision->condId = ctx->activeCondId;
    }
}

struct SlotDrawResult {
    bool          bypass;
    std::uint8_t  mode;
    std::uint32_t val0;
    std::uint32_t val1;
};

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
            if (len == 0U) {
                out = StepMhwXorshift128(slot.state);
                break;
            }
            const std::uint32_t* seqBase = slot.useInlineSeq
                                               ? slot.inlineSeq
                                               : (g_seqPool + slot.seqOffset);
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

SlotDrawResult DrawSlotValues(std::uint32_t callSiteRva,
                              MhwRngSourceKind srcKind,
                              std::uintptr_t rngInstance,
                              const void* retSlotAddr,
                              bool needTwoValues) noexcept {
    const std::uint64_t baseCompKey = MakeCompositeKey(callSiteRva, 0U);
    RngSlot* baseSlot = FindOrInsertCompositeSlot(baseCompKey);
    if (!baseSlot) {
        return SlotDrawResult{true, kMhwRngSlotBypass, 0U, 0U};
    }

    const std::uint64_t curEpoch   = g_seedEpoch.load(std::memory_order_acquire);
    const std::uint64_t masterSeed = g_masterSeed.load(std::memory_order_relaxed);
    std::uint32_t targetCondId     = baseSlot->activeCond.load(std::memory_order_acquire);

    // Optional Pull-mode Plugin Callback (0 overhead on the 99.9% of slots where hookIdx == kInvalidHookIdx!)
    const std::uint16_t hIdx = baseSlot->hookIdx.load(std::memory_order_acquire);
    if (hIdx < kMaxSlotHooks) {
        g_inFlightHookCalls.fetch_add(1U, std::memory_order_acq_rel);
        SlotHookEntry& he = g_hookEntries[hIdx];
        MhwRngHookDecision decision{kMhwRngActionUseCondition, targetCondId, 0U, 0U};
        bool invoked = false;
        if (he.active.load(std::memory_order_acquire)) {
            MhwRngSlotHookFn fn = he.fn.load(std::memory_order_acquire);
            void* ud            = he.userData.load(std::memory_order_acquire);
            if (fn) {
                MhwRngDrawContext ctx{};
                ctx.callSiteRva       = callSiteRva;
                ctx.sourceKind        = static_cast<std::uint8_t>(srcKind);
                ctx.needTwoValues     = needTwoValues ? 1U : 0U;
                ctx.exeBase           = g_exeBase;
                ctx.exeEnd            = g_exeEnd;
                ctx.rngInstance       = rngInstance;
                ctx.returnAddrSlot    = retSlotAddr;
                ctx.masterSeed        = masterSeed;
                ctx.epoch             = curEpoch;
                ctx.activeCondId      = targetCondId;
                ctx.DrawDeterministic = &DrawDeterministicHelper;
                InvokePluginSlotHookSEH(fn, &ctx, &decision, ud);
                invoked = true;
            }
        }
        g_inFlightHookCalls.fetch_sub(1U, std::memory_order_acq_rel);

        if (invoked) {
            if (decision.action == kMhwRngActionBypass) {
                SlotSpinGuard g(baseSlot->lock);
                if (baseSlot->seededEpoch != curEpoch) {
                    baseSlot->epochCalls  = 0ULL;
                    baseSlot->seededEpoch = curEpoch;
                }
                ++baseSlot->totalCalls;
                ++baseSlot->epochCalls;
                return SlotDrawResult{true, kMhwRngSlotBypass, 0U, 0U};
            }
            if (decision.action == kMhwRngActionReturnValue) {
                const std::uint64_t recKey = MakeCompositeKey(callSiteRva, decision.condId);
                RngSlot* recSlot = (decision.condId == 0U) ? baseSlot : FindOrInsertCompositeSlot(recKey);
                if (recSlot) {
                    SlotSpinGuard g(recSlot->lock);
                    recSlot->sourceKind = static_cast<std::uint8_t>(srcKind);
                    if (recSlot->seededEpoch != curEpoch) {
                        recSlot->epochCalls  = 0ULL;
                        recSlot->seededEpoch = curEpoch;
                    }
                    ++recSlot->totalCalls;
                    ++recSlot->epochCalls;
                    recSlot->lastValue = decision.value0;
                }
                return SlotDrawResult{false, kMhwRngSlotFixedValue, decision.value0, decision.value1};
            }
            targetCondId = decision.condId;
        }
    }

    const std::uint64_t targetCompKey = MakeCompositeKey(callSiteRva, targetCondId);
    RngSlot* targetSlot = (targetCondId == 0U) ? baseSlot : FindOrInsertCompositeSlot(targetCompKey);
    if (!targetSlot) {
        return SlotDrawResult{true, kMhwRngSlotBypass, 0U, 0U};
    }

    SlotSpinGuard guard(targetSlot->lock);
    if (targetSlot->sourceKind == kMhwRngSrcUnknown || srcKind != kMhwRngSrcUnknown) {
        targetSlot->sourceKind = static_cast<std::uint8_t>(srcKind);
    }
    if (targetSlot->mode == kMhwRngSlotBypass) {
        if (targetSlot->seededEpoch != curEpoch) {
            targetSlot->epochCalls  = 0ULL;
            targetSlot->seededEpoch = curEpoch;
        }
        ++targetSlot->totalCalls;
        ++targetSlot->epochCalls;
        return SlotDrawResult{true, kMhwRngSlotBypass, 0U, 0U};
    }

    const std::uint32_t v0 = StepSlotLocked(*targetSlot, targetCompKey, curEpoch, masterSeed);
    const std::uint32_t v1 = needTwoValues ? StepSlotLocked(*targetSlot, targetCompKey, curEpoch, masterSeed) : 0U;
    return SlotDrawResult{false, targetSlot->mode, v0, v1};
}

void FillSnapshotLocked(const RngSlot& src,
                        std::uint64_t compKey,
                        MhwRngSlotSnapshot& dst) noexcept {
    const auto rva    = static_cast<std::uint32_t>(compKey & 0xFFFFFFFFULL);
    const auto condId = static_cast<std::uint32_t>(compKey >> 32);
    std::uint32_t baseActiveCond = 0U;
    std::uint8_t  hasHook        = 0U;
    if (condId == 0U) {
        baseActiveCond = src.activeCond.load(std::memory_order_relaxed);
        hasHook        = (src.hookIdx.load(std::memory_order_relaxed) < kMaxSlotHooks) ? 1U : 0U;
    } else if (const RngSlot* base = FindExistingCompositeSlot(MakeCompositeKey(rva, 0U))) {
        baseActiveCond = base->activeCond.load(std::memory_order_relaxed);
        hasHook        = (base->hookIdx.load(std::memory_order_relaxed) < kMaxSlotHooks) ? 1U : 0U;
    }

    dst.callSiteRva   = rva;
    dst.condId        = condId;
    dst.mode          = src.mode;
    dst.sourceKind    = src.sourceKind;
    dst.seqEndPolicy  = src.seqEndPolicy;
    dst.hasPluginHook = hasHook;
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

} // namespace

// ============================================================================
//  Kernel Lifecycle & Composite Slot API
// ============================================================================

void Init(std::uintptr_t exeBase, std::uintptr_t exeEnd, const app::Config& cfg) {
    g_exeBase       = exeBase;
    g_exeEnd        = exeEnd;
    g_sInstanceAddr = exeBase + mhw::kRvaMtRandomInstance;

    g_masterSeed.store(cfg.masterSeed, std::memory_order_relaxed);
    g_takeoverEnabled.store(cfg.takeoverOnBoot, std::memory_order_release);
    g_seedEpoch.store(1ULL, std::memory_order_release);

    ApplyConfigRules(cfg, false);
}

std::uintptr_t ExeBase() noexcept { return g_exeBase; }
std::uintptr_t ExeEnd() noexcept  { return g_exeEnd; }

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
    SlotSpinGuard g(slot->lock);
    slot->mode          = kMhwRngSlotMasterDerived;
    slot->ownerPluginId = ownerId;
    slot->seededEpoch   = 0ULL;
    return true;
}

bool SetCondCustomSeed(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId, std::uint64_t seed) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    RngSlot* slot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, condId));
    if (!slot) return false;
    SlotSpinGuard g(slot->lock);
    slot->mode          = kMhwRngSlotCustomSeed;
    slot->ownerPluginId = ownerId;
    slot->customSeed    = seed;
    slot->seededEpoch   = 0ULL;
    return true;
}

bool SetCondFixedValue(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId, std::uint32_t rawU32) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    RngSlot* slot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, condId));
    if (!slot) return false;
    SlotSpinGuard g(slot->lock);
    slot->mode          = kMhwRngSlotFixedValue;
    slot->ownerPluginId = ownerId;
    slot->fixedValue    = rawU32;
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

    std::uint32_t poolOffset = 0U;
    const bool useInline = (count <= kInlineSeqCap);
    if (!useInline) {
        std::lock_guard<std::mutex> lk(g_seqAllocMx);
        if (g_seqPoolUsed + count > kSeqPoolCap) return false;
        poolOffset = g_seqPoolUsed;
        std::memcpy(g_seqPool + poolOffset, values, count * sizeof(std::uint32_t));
        g_seqPoolUsed += count;
    }

    SlotSpinGuard g(slot->lock);
    if (useInline) {
        std::memcpy(slot->inlineSeq, values, count * sizeof(std::uint32_t));
        slot->useInlineSeq = 1U;
        slot->seqOffset    = 0U;
    } else {
        slot->useInlineSeq = 0U;
        slot->seqOffset    = poolOffset;
    }
    slot->seqLength     = count;
    slot->seqCursor     = 0U;
    slot->seqEndPolicy  = (endPolicy <= kMhwRngSeqFallbackDerived) ? endPolicy : kMhwRngSeqLoop;
    slot->mode          = kMhwRngSlotSequence;
    slot->ownerPluginId = ownerId;
    slot->seededEpoch   = g_seedEpoch.load(std::memory_order_acquire);
    DeriveXorshift128State(g_masterSeed.load(std::memory_order_relaxed), compKey, slot->state);
    return true;
}

bool SetCondBypass(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    RngSlot* slot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, condId));
    if (!slot) return false;
    SlotSpinGuard g(slot->lock);
    slot->mode          = kMhwRngSlotBypass;
    slot->ownerPluginId = ownerId;
    return true;
}

void ClearSlotAllConditions(std::uint32_t rva) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    if (norm == 0U) return;
    for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
        const std::uint64_t k = g_slots[i].key.load(std::memory_order_acquire);
        if (k == 0ULL || static_cast<std::uint32_t>(k & 0xFFFFFFFFULL) != norm) continue;
        g_slots[i].activeCond.store(0U, std::memory_order_release);
        SlotSpinGuard g(g_slots[i].lock);
        const std::uint16_t hIdx = ((k >> 32) == 0ULL) ? g_slots[i].hookIdx.load(std::memory_order_acquire) : kInvalidHookIdx;
        g_slots[i].mode          = kMhwRngSlotMasterDerived;
        g_slots[i].ownerPluginId = (hIdx < kMaxSlotHooks && g_hookEntries[hIdx].active.load(std::memory_order_acquire))
                                       ? g_hookEntries[hIdx].ownerId
                                       : kMhwRngPluginIdKernel;
        g_slots[i].seqLength     = 0U;
        g_slots[i].seqCursor     = 0U;
        g_slots[i].seededEpoch   = 0ULL;
    }
}

void ClearRulesByOwner(MhwRngPluginId ownerId) noexcept {
    for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
        const std::uint64_t k = g_slots[i].key.load(std::memory_order_acquire);
        if (k == 0ULL) continue;
        SlotSpinGuard g(g_slots[i].lock);
        if (g_slots[i].ownerPluginId == ownerId) {
            if ((k >> 32) == 0ULL) {
                g_slots[i].activeCond.store(0U, std::memory_order_release);
            }
            g_slots[i].mode          = kMhwRngSlotMasterDerived;
            g_slots[i].ownerPluginId = kMhwRngPluginIdKernel;
            g_slots[i].seqLength     = 0U;
            g_slots[i].seqCursor     = 0U;
            g_slots[i].seededEpoch   = 0ULL;
        }
    }
}

bool GetCondSlotInfo(std::uint32_t rva, std::uint32_t condId, MhwRngSlotSnapshot* outSnap) noexcept {
    if (!outSnap) return false;
    const std::uint32_t norm    = NormalizeUserSiteAddress(rva);
    const std::uint64_t compKey = MakeCompositeKey(norm, condId);
    RngSlot* slot = FindExistingCompositeSlot(compKey);
    if (!slot) return false;
    SlotSpinGuard g(slot->lock);
    FillSnapshotLocked(*slot, compKey, *outSnap);
    return true;
}

bool RegisterSlotHook(MhwRngPluginId ownerId,
                      std::uint32_t rva,
                      MhwRngSlotHookFn fn,
                      void* userData) noexcept {
    if (!fn) return false;
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    if (norm == 0U) return false;
    RngSlot* baseSlot = FindOrInsertCompositeSlot(MakeCompositeKey(norm, 0U));
    if (!baseSlot) return false;

    std::lock_guard<std::mutex> lk(g_hookRegMx);
    // Reuse existing entry for this RVA if present, or allocate a free one
    std::uint16_t chosen = baseSlot->hookIdx.load(std::memory_order_acquire);
    if (chosen >= kMaxSlotHooks) {
        for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
            if (!g_hookEntries[i].active.load(std::memory_order_acquire)) {
                chosen = i;
                break;
            }
        }
    }
    if (chosen >= kMaxSlotHooks) return false;

    SlotHookEntry& he = g_hookEntries[chosen];
    he.active.store(false, std::memory_order_release);
    he.ownerId = ownerId;
    he.rva     = norm;
    he.userData.store(userData, std::memory_order_release);
    he.fn.store(fn, std::memory_order_release);
    he.active.store(true, std::memory_order_release);
    baseSlot->hookIdx.store(chosen, std::memory_order_release);
    {
        SlotSpinGuard sg(baseSlot->lock);
        if (baseSlot->ownerPluginId == kMhwRngPluginIdKernel) {
            baseSlot->ownerPluginId = ownerId;
        }
    }
    return true;
}

void UnregisterSlotHook(MhwRngPluginId ownerId, std::uint32_t rva) noexcept {
    const std::uint32_t norm = NormalizeUserSiteAddress(rva);
    if (norm == 0U) return;
    std::lock_guard<std::mutex> lk(g_hookRegMx);
    RngSlot* baseSlot = FindExistingCompositeSlot(MakeCompositeKey(norm, 0U));
    if (!baseSlot) return;
    const std::uint16_t hIdx = baseSlot->hookIdx.load(std::memory_order_acquire);
    if (hIdx < kMaxSlotHooks && (ownerId == 0U || g_hookEntries[hIdx].ownerId == ownerId)) {
        const MhwRngPluginId prevOwner = g_hookEntries[hIdx].ownerId;
        baseSlot->hookIdx.store(kInvalidHookIdx, std::memory_order_release);
        g_hookEntries[hIdx].active.store(false, std::memory_order_release);
        while (g_inFlightHookCalls.load(std::memory_order_acquire) != 0U) {
            _mm_pause();
        }
        g_hookEntries[hIdx].fn.store(nullptr, std::memory_order_release);
        g_hookEntries[hIdx].userData.store(nullptr, std::memory_order_release);
        g_hookEntries[hIdx].ownerId = 0U;
        SlotSpinGuard sg(baseSlot->lock);
        if (baseSlot->mode == kMhwRngSlotMasterDerived && baseSlot->ownerPluginId == prevOwner) {
            baseSlot->ownerPluginId = kMhwRngPluginIdKernel;
        }
    }
}

void DetachAllHooksForPluginAndWait(MhwRngPluginId ownerId) noexcept {
    std::lock_guard<std::mutex> lk(g_hookRegMx);
    bool anyDetached = false;
    for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
        if (g_hookEntries[i].active.load(std::memory_order_acquire) &&
            g_hookEntries[i].ownerId == ownerId) {
            if (RngSlot* base = FindExistingCompositeSlot(MakeCompositeKey(g_hookEntries[i].rva, 0U))) {
                if (base->hookIdx.load(std::memory_order_acquire) == i) {
                    base->hookIdx.store(kInvalidHookIdx, std::memory_order_release);
                }
            }
            g_hookEntries[i].active.store(false, std::memory_order_release);
            anyDetached = true;
        }
    }
    if (anyDetached) {
        // Quiescence barrier: wait until all concurrent worker threads have exited any callback!
        while (g_inFlightHookCalls.load(std::memory_order_acquire) != 0U) {
            _mm_pause();
        }
        for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
            if (!g_hookEntries[i].active.load(std::memory_order_acquire) &&
                g_hookEntries[i].ownerId == ownerId) {
                g_hookEntries[i].fn.store(nullptr, std::memory_order_release);
                g_hookEntries[i].userData.store(nullptr, std::memory_order_release);
                g_hookEntries[i].ownerId = 0U;
            }
        }
    }
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

// ============================================================================
//  10 Detour Implementations (100% Coverage of MHW 15.23 RNG Surface)
// ============================================================================

std::uint32_t* __fastcall Hook_MtRandomCtorSeed(std::uint32_t* self, std::uint32_t seed) {
    if (!self) return nullptr;
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        return g_origMtCtorSeed(self, seed);
    }
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);
    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcMtCtorSeed, reinterpret_cast<std::uintptr_t>(self), retSlot, false);
    if (dr.bypass) return g_origMtCtorSeed(self, seed);

    InitMhwMtRandomWords(self, dr.val0);
    if (dr.mode != kMhwRngSlotMasterDerived) {
        TagExplicitlySeededInstance(reinterpret_cast<std::uintptr_t>(self), g_seedEpoch.load(std::memory_order_relaxed));
    }
    return self;
}

std::uint32_t* __fastcall Hook_MtRandomCtorDefault(std::uint32_t* self) {
    if (!self) return nullptr;
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        return g_origMtCtorDef(self);
    }
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);
    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcMtCtorDefault, reinterpret_cast<std::uintptr_t>(self), retSlot, false);
    if (dr.bypass) return g_origMtCtorDef(self);

    InitMhwMtRandomWords(self, dr.val0);
    if (dr.mode != kMhwRngSlotMasterDerived) {
        TagExplicitlySeededInstance(reinterpret_cast<std::uintptr_t>(self), g_seedEpoch.load(std::memory_order_relaxed));
    }
    return self;
}

void __fastcall Hook_MtRandomSrand(std::uint32_t* self, std::uint32_t seed) {
    if (!self) return;
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        g_origMtSrand(self, seed);
        return;
    }
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);
    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcMtSrand, reinterpret_cast<std::uintptr_t>(self), retSlot, false);
    if (dr.bypass) {
        g_origMtSrand(self, seed);
        return;
    }
    if (reinterpret_cast<std::uintptr_t>(self) != g_sInstanceAddr) {
        InitMhwMtRandomWords(self, dr.val0);
        if (dr.mode != kMhwRngSlotMasterDerived) {
            TagExplicitlySeededInstance(reinterpret_cast<std::uintptr_t>(self), g_seedEpoch.load(std::memory_order_relaxed));
        }
    }
}

std::uint32_t __fastcall Hook_MtRandomNrand(std::uint32_t* self) {
    if (!self) return 0U;
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        return g_origMtNrand(self);
    }
    const auto selfAddr     = reinterpret_cast<std::uintptr_t>(self);
    const bool isGlobalInst = (selfAddr == g_sInstanceAddr);
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);

    const SlotDrawResult dr = DrawSlotValues(
        rva,
        isGlobalInst ? kMhwRngSrcMtNrandGlobal : kMhwRngSrcMtNrandPrivate,
        selfAddr,
        retSlot,
        false);
    if (dr.bypass) return g_origMtNrand(self);
    if (isGlobalInst) return dr.val0;

    if (dr.mode == kMhwRngSlotMasterDerived &&
        IsExplicitlySeededInCurrentEpoch(selfAddr, g_seedEpoch.load(std::memory_order_relaxed))) {
        return StepMhwXorshift128(self);
    }

    self[0] = self[1];
    self[1] = self[2];
    self[2] = self[3];
    self[3] = dr.val0 != 0U ? dr.val0 : 0x6D2B79F5U;
    return dr.val0;
}

float* __fastcall Hook_MtRandomSphere(std::uint32_t* self, float* outVec3) {
    if (!self || !outVec3) return outVec3;
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        return g_origMtSphere(self, outVec3);
    }
    const auto selfAddr     = reinterpret_cast<std::uintptr_t>(self);
    const bool isGlobalInst = (selfAddr == g_sInstanceAddr);
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);

    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcMtSphere, selfAddr, retSlot, true);
    if (dr.bypass) return g_origMtSphere(self, outVec3);

    const std::uint32_t u1 = dr.val0;
    const std::uint32_t u2 = dr.val1;
    if (!isGlobalInst) {
        self[0] = self[2];
        self[1] = self[3];
        self[2] = u1 != 0U ? u1 : 0x075BCD15U;
        self[3] = u2 != 0U ? u2 : 0x159A55E5U;
    }

    const float u1f   = static_cast<float>(static_cast<std::int64_t>(static_cast<std::uint64_t>(u1)));
    const float u2f   = static_cast<float>(static_cast<std::int64_t>(static_cast<std::uint64_t>(u2)));
    const float z     = (u1f * kFloatInv2Pow32 + u1f * kFloatInv2Pow32) - 1.0f;
    const float rSq   = 1.0f - z * z;
    const float rXY   = (rSq > 0.0f) ? std::sqrt(rSq) : 0.0f;
    const float angle = u2f * kFloatTwoPiOver2P32;

    outVec3[1] = z;
    outVec3[0] = std::cos(angle) * rXY;
    outVec3[2] = std::sin(angle) * rXY;
    return outVec3;
}

std::uint32_t __cdecl Hook_StdRandomDevice() {
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        return g_origStdRandDev();
    }
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);
    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcStdRandomDevice, 0U, retSlot, false);
    return dr.bypass ? g_origStdRandDev() : dr.val0;
}

float __fastcall Hook_Mt19937Range(void* ctx, float minVal, float maxVal) {
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        return g_origMt19937Range(ctx, minVal, maxVal);
    }
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);
    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcMt19937Range, reinterpret_cast<std::uintptr_t>(ctx), retSlot, false);
    if (dr.bypass) return g_origMt19937Range(ctx, minVal, maxVal);

    const float uf = static_cast<float>(static_cast<std::int64_t>(static_cast<std::uint64_t>(dr.val0)));
    return (uf * kFloatInv2Pow32 + kFloatHalfStep2P32) * (maxVal - minVal) + minVal;
}

void __fastcall Hook_Mt19937HeapInit(void* state, std::uint32_t seed) {
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        g_origMt19937Init(state, seed);
        return;
    }
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);
    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcMt19937HeapInit, reinterpret_cast<std::uintptr_t>(state), retSlot, false);
    g_origMt19937Init(state, dr.bypass ? seed : dr.val0);
}

std::uint32_t __fastcall Hook_Mt19937HeapDraw(void* state) {
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        return g_origMt19937Draw(state);
    }
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);
    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcMt19937HeapDraw, reinterpret_cast<std::uintptr_t>(state), retSlot, false);
    return dr.bypass ? g_origMt19937Draw(state) : dr.val0;
}

int __cdecl Hook_CrtRand() {
    if (!g_takeoverEnabled.load(std::memory_order_relaxed)) {
        return g_origCrtRand();
    }
    const void* retSlot     = _AddressOfReturnAddress();
    const std::uint32_t rva = ComputeCallSiteRva(retSlot);
    const SlotDrawResult dr = DrawSlotValues(rva, kMhwRngSrcCrtRand, 0U, retSlot, false);
    if (dr.bypass) return g_origCrtRand();

    if ((dr.mode == kMhwRngSlotFixedValue || dr.mode == kMhwRngSlotSequence) && dr.val0 <= 0x7FFFU) {
        return static_cast<int>(dr.val0);
    }
    return static_cast<int>((dr.val0 >> 16) & 0x7FFFU);
}

std::uint32_t CountOverriddenSlots() noexcept {
    std::uint32_t cnt = 0U;
    for (std::uint32_t i = 0; i < kSlotTableSize; ++i) {
        if (g_slots[i].key.load(std::memory_order_acquire) == 0ULL) continue;
        SlotSpinGuard g(g_slots[i].lock);
        if (g_slots[i].mode != kMhwRngSlotMasterDerived ||
            g_slots[i].hookIdx.load(std::memory_order_relaxed) < kMaxSlotHooks) {
            ++cnt;
        }
    }
    return cnt;
}

std::uint32_t CountHookedSlots() noexcept {
    std::uint32_t cnt = 0U;
    for (std::uint16_t i = 0; i < kMaxSlotHooks; ++i) {
        if (g_hookEntries[i].active.load(std::memory_order_acquire)) ++cnt;
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
//  Exported C API Implementation (mhw_rng_api.h)
// ============================================================================

MHW_RNG_API std::uint32_t MhwRng_GetApiVersion() {
    return kMhwRngApiVersion;
}

MHW_RNG_API void MhwRng_SetTakeoverEnabled(bool enabled) {
    rng::g_takeoverEnabled.store(enabled, std::memory_order_release);
}

MHW_RNG_API bool MhwRng_IsTakeoverEnabled() {
    return rng::g_takeoverEnabled.load(std::memory_order_acquire);
}

MHW_RNG_API void MhwRng_SetMasterSeed(std::uint64_t seed, bool resetEpoch) {
    rng::g_masterSeed.store(seed, std::memory_order_release);
    if (resetEpoch) {
        rng::g_seedEpoch.fetch_add(1ULL, std::memory_order_acq_rel);
    }
}

MHW_RNG_API std::uint64_t MhwRng_GetMasterSeed() {
    return rng::g_masterSeed.load(std::memory_order_acquire);
}

MHW_RNG_API std::uint64_t MhwRng_ResetEpoch() {
    return rng::g_seedEpoch.fetch_add(1ULL, std::memory_order_acq_rel) + 1ULL;
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
    for (std::uint32_t i = 0; i < rng::kSlotTableSize; ++i) {
        if (rng::g_slots[i].key.load(std::memory_order_acquire) == 0ULL) continue;
        rng::g_slots[i].activeCond.store(0U, std::memory_order_release);
        rng::SlotSpinGuard g(rng::g_slots[i].lock);
        if (rng::g_slots[i].mode != kMhwRngSlotMasterDerived) {
            rng::g_slots[i].mode          = kMhwRngSlotMasterDerived;
            rng::g_slots[i].ownerPluginId = kMhwRngPluginIdKernel;
            rng::g_slots[i].seqLength     = 0U;
            rng::g_slots[i].seqCursor     = 0U;
            rng::g_slots[i].seededEpoch   = 0ULL;
        }
    }
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
        if (compKey == 0ULL) continue;
        MhwRngSlotSnapshot& dst = outArray[written++];
        rng::SlotSpinGuard g(rng::g_slots[i].lock);
        rng::FillSnapshotLocked(rng::g_slots[i], compKey, dst);
    }
    return written;
}
