#include "peer_beacon.h"

#include "log_config.h"
#include "mhw_addresses.h"
#include "plugin_host.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace peer_beacon {
namespace {

constexpr std::size_t   kTaintWords        = 8U;   // 512 plugin ID bits
constexpr std::size_t   kMaxPayloadLen     = 63U;  // pos 0..62 chars, pos 63 = end
constexpr std::size_t   kMaxTrackedPlayers = 24U;
constexpr std::uint64_t kStaleTickLimit    = 60ULL;

constexpr std::uint32_t kMaskKey = 0x7A4D9B35U;
constexpr std::uint32_t kMacKey  = 0xC2B2AE3DU;

struct PlayerDecoderSlot {
    bool           active        = false;
    std::uintptr_t entityPtr     = 0;
    bool           isLocal       = false;
    std::uint64_t  lastSeenTick  = 0ULL;
    std::uint32_t  lastSeed27    = 0U;
    std::uint8_t   activeGen2    = 0xFFU;
    std::uint8_t   endPos        = 0xFFU;
    std::uint64_t  seenMask      = 0ULL;
    std::uint32_t  validFrames   = 0U;
    std::uint32_t  invalidStreak = 0U;
    char           buf[64]{};
    std::string    lastReportedList;

    void ResetAssembly() noexcept {
        activeGen2  = 0xFFU;
        endPos      = 0xFFU;
        seenMask    = 0ULL;
        validFrames = 0U;
        std::memset(buf, 0, sizeof(buf));
    }

    void ClearSlot() noexcept {
        active        = false;
        entityPtr     = 0;
        isLocal       = false;
        lastSeenTick  = 0ULL;
        lastSeed27    = 0U;
        invalidStreak = 0U;
        ResetAssembly();
        lastReportedList.clear();
    }
};

struct ObservedPlayer {
    std::uintptr_t entityPtr  = 0;
    bool           isLocal    = false;
    std::uint32_t  seed27     = 0U;
    std::int32_t   questIdx   = -1;
    std::int32_t   lobbyIdx   = -1;
};

struct PendingAlert {
    bool        isLocal = false;
    std::string playerName;
    std::string pluginList;
};

std::atomic<bool>          g_checkEnabled{true};
std::atomic<bool>          g_selfCheckEnabled{false};
std::atomic<bool>          g_inScene{false};
std::atomic<bool>          g_wmDebugVisible{false};
std::atomic<AlertSinkFn>   g_alertSink{nullptr};
std::atomic<std::uint64_t> g_taintWords[kTaintWords]{};
std::atomic<std::uint32_t> g_txFrameCount{0U};
std::atomic<std::uint32_t> g_rxLocalFrameCount{0U};

std::once_flag             g_qpcOnce;
std::int64_t               g_qpcOrigin = 0;
std::int64_t               g_qpcFreq   = 0;

static_assert(std::is_trivially_copyable_v<AudioMarkPacket>,
              "AudioMarkPacket must be trivially copyable");
constexpr std::size_t kAudioSnapWords =
    (sizeof(AudioMarkPacket) + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t);

std::mutex                 g_audioPubMx;
std::atomic<std::uint32_t> g_audioSnapSeq{0U};
std::atomic<std::uint64_t> g_audioSnapWords[kAudioSnapWords]{};
std::atomic<bool>          g_audioSnapDirty{true};
std::atomic<bool>          g_audioSnapBuiltActive{false};

std::mutex                 g_txMx;
std::uint64_t              g_cachedTaintWords[kTaintWords]{};
bool                       g_txCacheInit = false;
std::string                g_txPayload;
std::uint8_t               g_txGen2   = 0U;
std::size_t                g_txCursor = 0U;
std::uint32_t              g_txSeq    = 0U;

std::mutex                 g_rxMx;
PlayerDecoderSlot          g_rxSlots[kMaxTrackedPlayers]{};
std::uintptr_t             g_lastLocalPlayerPtr = 0;
std::uint64_t              g_rxTickSeq          = 0ULL;
bool                       g_lastQuestValid     = false;
bool                       g_lastInQuest        = false;
std::uint32_t              g_lastQuestId        = 0U;
std::int32_t               g_lastQuestElapsed   = 0;

inline bool IsValidPtr(std::uintptr_t p) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & 0x7ULL) == 0ULL;
}

inline std::uint32_t Mix32(std::uint32_t x, std::uint32_t key) noexcept {
    std::uint32_t h = x ^ key;
    h ^= h >> 16;
    h *= 0x85EBCA6BU;
    h ^= h >> 13;
    h *= 0xC2B2AE35U;
    h ^= h >> 16;
    return h;
}

std::uint32_t PackFrameWithNonce(std::uint8_t nonce5,
                                 std::uint8_t gen2,
                                 std::uint8_t pos6,
                                 std::uint8_t sym6) noexcept {
    const std::uint32_t n = static_cast<std::uint32_t>(nonce5 & 0x1FU);
    const std::uint32_t plain14 =
        (static_cast<std::uint32_t>(gen2 & 0x03U) << 12) |
        (static_cast<std::uint32_t>(pos6 & 0x3FU) << 6) |
        static_cast<std::uint32_t>(sym6 & 0x3FU);
    const std::uint32_t mask14   = Mix32(n, kMaskKey) & 0x3FFFU;
    const std::uint32_t cipher14 = plain14 ^ mask14;
    const std::uint32_t mac8     = Mix32((n << 14) | cipher14, kMacKey) & 0xFFU;
    return (n << 22) | (cipher14 << 8) | mac8;
}

bool ValidateDecodedList(const std::string& listStr) noexcept {
    if (listStr.empty() || listStr.size() > kMaxPayloadLen) return false;
    std::size_t tokStart = 0U;
    while (tokStart <= listStr.size()) {
        const std::size_t comma = listStr.find(',', tokStart);
        const std::size_t tokEnd = (comma == std::string::npos) ? listStr.size() : comma;
        if (tokEnd <= tokStart) return false; // empty token (leading/trailing/consecutive comma)
        const char first = listStr[tokStart];
        if (!((first >= 'a' && first <= 'z') || first == '_')) return false;
        for (std::size_t i = tokStart; i < tokEnd; ++i) {
            const char c = listStr[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c == '_') || (c >= '0' && c <= '9');
            if (!ok) return false;
        }
        if (comma == std::string::npos) break;
        tokStart = comma + 1U;
    }
    return true;
}

std::string BuildTaintListFromWords(const std::uint64_t words[kTaintWords]) {
    std::vector<std::string> names;
    for (std::size_t w = 0U; w < kTaintWords; ++w) {
        std::uint64_t bits = words[w];
        while (bits != 0ULL) {
            unsigned long bit = 0;
            for (unsigned long b = 0; b < 64U; ++b) {
                if ((bits & (1ULL << b)) != 0ULL) {
                    bit = b;
                    break;
                }
            }
            bits &= ~(1ULL << bit);
            const auto pluginId = static_cast<MhwRngPluginId>(w * 64U + bit);
            const char* rawName = plugin_host::GetPluginShortName(pluginId);
            if (!rawName || rawName[0] == '\0') continue;
            std::string normalized;
            for (std::size_t i = 0U; rawName[i] != '\0'; ++i) {
                char c = rawName[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
                std::uint8_t dummySym = 0U;
                if (c != ',' && EncodeSymbolChar(c, dummySym)) {
                    normalized.push_back(c);
                }
            }
            if (!normalized.empty()) {
                names.push_back(std::move(normalized));
            }
        }
    }
    if (names.empty()) return {};

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    std::string out;
    for (const std::string& nm : names) {
        const std::size_t extra = out.empty() ? nm.size() : (1U + nm.size());
        if (out.size() + extra > kMaxPayloadLen) {
            if (out.empty()) {
                out = nm.substr(0U, kMaxPayloadLen);
            }
            break;
        }
        if (!out.empty()) out.push_back(',');
        out += nm;
    }
    return out;
}

void RebuildTxPayloadLocked(const std::uint64_t words[kTaintWords]) {
    for (std::size_t i = 0U; i < kTaintWords; ++i) {
        g_cachedTaintWords[i] = words[i];
    }
    g_txCacheInit = true;
    g_txCursor    = 0U;
    g_txPayload   = BuildTaintListFromWords(words);
    g_txGen2      = g_txPayload.empty()
                        ? 0U
                        : ComputeListGen2(g_txPayload.c_str(), g_txPayload.size());
}

void SyncTxPayloadFromAtomicsLocked() {
    std::uint64_t words[kTaintWords]{};
    for (std::size_t i = 0U; i < kTaintWords; ++i) {
        words[i] = g_taintWords[i].load(std::memory_order_acquire);
    }
    bool changed = !g_txCacheInit;
    for (std::size_t i = 0U; i < kTaintWords && !changed; ++i) {
        if (words[i] != g_cachedTaintWords[i]) changed = true;
    }
    if (changed) {
        RebuildTxPayloadLocked(words);
    }
}

bool IsOnlineSessionInternal(std::uintptr_t exeBase) noexcept {
    if (!exeBase) return false;
    const std::uintptr_t net = *reinterpret_cast<const std::uintptr_t*>(
        exeBase + mhw::kRvaSMhNetworkInstance);
    if (!IsValidPtr(net)) return false;

    const std::int32_t mode = *reinterpret_cast<const std::int32_t*>(
        net + mhw::kOffNetActiveMode);
    if (mode < 0 || mode >= 2) return false;

    const std::uintptr_t obj0 = *reinterpret_cast<const std::uintptr_t*>(net + mhw::kOffNetSessionObj0);
    const std::uintptr_t obj1 = *reinterpret_cast<const std::uintptr_t*>(net + mhw::kOffNetSessionObj1);
    const std::uintptr_t inner0 = IsValidPtr(obj0)
                                      ? *reinterpret_cast<const std::uintptr_t*>(obj0 + mhw::kOffNetSessionInner)
                                      : 0ULL;
    const std::uintptr_t inner1 = IsValidPtr(obj1)
                                      ? *reinterpret_cast<const std::uintptr_t*>(obj1 + mhw::kOffNetSessionInner)
                                      : 0ULL;
    if (!IsValidPtr(inner1)) return false;
    const std::uint8_t flag1 = *reinterpret_cast<const std::uint8_t*>(inner1 + mhw::kOffNetSessionOnlineByte);
    if (mode == 1) {
        return (flag1 & 1U) != 0U;
    }
    if (!IsValidPtr(inner0)) return false;
    const std::uint8_t flag0 = *reinterpret_cast<const std::uint8_t*>(inner0 + mhw::kOffNetSessionOnlineByte);
    return (flag0 & 1U) != 0U && (flag1 & 1U) != 0U;
}

bool TryCopyPrintableNameSEH(const char* rawPtr, char outBuf[64], std::size_t& outLen) noexcept {
    outLen = 0U;
    if (!rawPtr) return false;
    __try {
        std::size_t n = 0U;
        while (n < mhw::kMaxNetMemberNameBytes && rawPtr[n] != '\0') {
            const unsigned char ch = static_cast<unsigned char>(rawPtr[n]);
            if (ch < 0x20U || ch == 0x7FU) return false;
            outBuf[n] = rawPtr[n];
            ++n;
        }
        if (n == 0U || rawPtr[n] != '\0') return false;
        outBuf[n] = '\0';
        outLen = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outLen = 0U;
        outBuf[0] = '\0';
        return false;
    }
}

bool TryReadNetNameSEH(std::uintptr_t exeBase,
                       std::int32_t questIdx,
                       std::int32_t lobbyIdx,
                       char outBuf[64],
                       std::size_t& outLen) noexcept {
    outLen = 0U;
    if (!exeBase) return false;
    __try {
        const std::uintptr_t net = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSMhNetworkInstance);
        if (!IsValidPtr(net)) return false;
        if (questIdx >= 0 && questIdx < 4) {
            const auto* p = reinterpret_cast<const char*>(
                net + mhw::kOffNetQuestNames + static_cast<std::size_t>(questIdx) * mhw::kStrideNetMemberName);
            if (TryCopyPrintableNameSEH(p, outBuf, outLen)) return true;
        }
        if (lobbyIdx >= 0 && lobbyIdx < 16) {
            const auto* p = reinterpret_cast<const char*>(
                net + mhw::kOffNetLobbyNames + static_cast<std::size_t>(lobbyIdx) * mhw::kStrideNetMemberName);
            if (TryCopyPrintableNameSEH(p, outBuf, outLen)) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outLen = 0U;
        return false;
    }
}

std::string ResolvePlayerName(std::uintptr_t exeBase,
                              bool isLocal,
                              std::int32_t questIdx,
                              std::int32_t lobbyIdx,
                              int fallbackSlot) {
    char nameBuf[64]{};
    std::size_t nameLen = 0U;
    if (TryReadNetNameSEH(exeBase, questIdx, lobbyIdx, nameBuf, nameLen) && nameLen > 0U) {
        return std::string(nameBuf, nameLen);
    }
    if (isLocal) {
        return app::IsEnglish() ? "Self" : u8"自己";
    }
    char buf[24]{};
    std::snprintf(buf, sizeof(buf), "P%d", fallbackSlot);
    return buf;
}

bool TryReadPtrSEH(std::uintptr_t addr, std::uintptr_t& outPtr) noexcept {
    outPtr = 0ULL;
    if (addr < 0x100000ULL) return false;
    __try {
        outPtr = *reinterpret_cast<const std::uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outPtr = 0ULL;
        return false;
    }
}

bool TryReadU32SEH(std::uintptr_t addr, std::uint32_t& outVal) noexcept {
    outVal = 0U;
    if (addr < 0x100000ULL) return false;
    __try {
        outVal = *reinterpret_cast<const std::uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outVal = 0U;
        return false;
    }
}

bool TryReadPlayerInfoSlotSEH(std::uintptr_t sPlayer,
                              std::size_t idx,
                              std::uintptr_t& outEnt,
                              std::int32_t& outQIdx,
                              std::int32_t& outLIdx) noexcept {
    outEnt  = 0ULL;
    outQIdx = -1;
    outLIdx = -1;
    __try {
        const std::uintptr_t info = sPlayer + mhw::kOffSPlayerInfoArray + idx * mhw::kStridePlayerInfo;
        const std::int32_t qIdx   = *reinterpret_cast<const std::int32_t*>(info + mhw::kOffPlayerInfoQuestIdx);
        const std::int32_t lIdx   = *reinterpret_cast<const std::int32_t*>(info + mhw::kOffPlayerInfoLobbyIdx);
        if (qIdx < 0 && lIdx < 0) return false;
        const std::uintptr_t ent  = *reinterpret_cast<const std::uintptr_t*>(info + mhw::kOffPlayerInfoEntity);
        if (!IsValidPtr(ent)) return false;
        outQIdx = qIdx;
        outLIdx = lIdx;
        outEnt  = ent;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outEnt  = 0ULL;
        outQIdx = -1;
        outLIdx = -1;
        return false;
    }
}

bool TryReadUPlayerSEH(std::uintptr_t uPlayer, bool& outIsLocal, std::uint32_t& outSeed27) noexcept {
    outIsLocal = false;
    outSeed27  = 0U;
    if (!IsValidPtr(uPlayer)) return false;
    __try {
        const std::uint32_t uFlags = *reinterpret_cast<const std::uint32_t*>(
            uPlayer + mhw::kOffUPlayerFlags);
        if ((uFlags & 0x0EU) == 0U) return false;

        outIsLocal = ((*reinterpret_cast<const std::uint8_t*>(
            uPlayer + mhw::kOffUPlayerLocalFlags)) & 1U) != 0U;
        outSeed27 = (*reinterpret_cast<const std::uint32_t*>(
            uPlayer + mhw::kOffUPlayerSeed27)) & 0x07FFFFFFU;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outIsLocal = false;
        outSeed27  = 0U;
        return false;
    }
}

bool CollectPlayers(std::uintptr_t exeBase,
                    ObservedPlayer outPlayers[kMaxTrackedPlayers],
                    std::size_t& outCount,
                    std::uintptr_t& outLocalEntity) noexcept {
    outCount       = 0U;
    outLocalEntity = 0ULL;
    if (!exeBase) return false;

    auto pushEntity = [&](std::uintptr_t uPlayer, std::int32_t qIdx, std::int32_t lIdx) noexcept {
        if (!IsValidPtr(uPlayer) || outCount >= kMaxTrackedPlayers) return;
        for (std::size_t k = 0U; k < outCount; ++k) {
            if (outPlayers[k].entityPtr == uPlayer) return;
        }
        bool isLocal = false;
        std::uint32_t seed27 = 0U;
        if (!TryReadUPlayerSEH(uPlayer, isLocal, seed27)) return;

        if (isLocal && outLocalEntity == 0ULL) {
            outLocalEntity = uPlayer;
        }
        outPlayers[outCount++] = ObservedPlayer{uPlayer, isLocal, seed27, qIdx, lIdx};
    };

    std::uintptr_t sPlayer = 0ULL;
    if (TryReadPtrSEH(exeBase + mhw::kRvaSPlayerInstance, sPlayer) && IsValidPtr(sPlayer)) {
        for (std::size_t i = 0U; i < mhw::kMaxPlayerInfoSlots; ++i) {
            std::uintptr_t ent = 0ULL;
            std::int32_t qIdx = -1, lIdx = -1;
            if (TryReadPlayerInfoSlotSEH(sPlayer, i, ent, qIdx, lIdx)) {
                pushEntity(ent, qIdx, lIdx);
            }
        }

        std::uint32_t gCount = 0U;
        if (TryReadU32SEH(sPlayer + mhw::kOffSPlayerGlobalCount, gCount)) {
            const std::uint32_t capped = (gCount > 16U) ? 16U : gCount;
            for (std::uint32_t i = 0U; i < capped; ++i) {
                std::uintptr_t ent = 0ULL;
                if (TryReadPtrSEH(sPlayer + mhw::kOffSPlayerGlobalArray +
                                      static_cast<std::size_t>(i) * sizeof(std::uintptr_t),
                                  ent) &&
                    IsValidPtr(ent)) {
                    pushEntity(ent, -1, -1);
                }
            }
        }
    }

    if (outLocalEntity == 0ULL) {
        std::uintptr_t root = 0ULL;
        if (TryReadPtrSEH(exeBase + mhw::kRvaSPlayerRoot, root) && IsValidPtr(root)) {
            std::uintptr_t ent = 0ULL;
            if (TryReadPtrSEH(root + mhw::kOffPlayerRootEntity, ent) && IsValidPtr(ent)) {
                pushEntity(ent, 0, -1);
            }
        }
    }

    if (outLocalEntity == 0ULL) {
        std::uintptr_t spFallback = 0ULL;
        if (TryReadPtrSEH(exeBase + mhw::kRvaSPlayerFallback, spFallback) && IsValidPtr(spFallback)) {
            std::uintptr_t ent = 0ULL;
            if (TryReadPtrSEH(spFallback + mhw::kOffSPlayerFallbackLocal, ent) && IsValidPtr(ent)) {
                pushEntity(ent, 0, -1);
            }
        }
    }

    return (outCount > 0U) || IsValidPtr(sPlayer);
}

bool TryReadQuestStateSEH(std::uintptr_t exeBase,
                          bool& outInQuest,
                          std::uint32_t& outQuestId,
                          std::int32_t& outElapsedTicks) noexcept {
    outInQuest      = false;
    outQuestId      = 0U;
    outElapsedTicks = 0;
    if (!exeBase) return false;
    __try {
        const std::uintptr_t sq = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSQuestInstance);
        if (!IsValidPtr(sq)) return false;

        const std::uint32_t st  = *reinterpret_cast<const std::uint32_t*>(sq + mhw::kOffQuestState);
        const std::uint32_t qid = *reinterpret_cast<const std::uint32_t*>(sq + mhw::kOffQuestId);
        const std::uint32_t cat = *reinterpret_cast<const std::uint32_t*>(sq + mhw::kOffQuestCategory);
        const std::int32_t  rem = *reinterpret_cast<const std::int32_t*>(sq + mhw::kOffQuestRemTicks);
        const std::int32_t  unl = *reinterpret_cast<const std::int32_t*>(sq + mhw::kOffQuestUnlimTicks);
        const std::int32_t  ini = *reinterpret_cast<const std::int32_t*>(sq + mhw::kOffQuestInitRemTicks);

        std::int32_t elapsed = (ini > 0 && ini >= rem) ? (ini - rem) : unl;
        if (elapsed < 0) elapsed = 0;

        outInQuest      = ((st - 2U) <= 10U && cat >= 2U);
        outQuestId      = qid;
        outElapsedTicks = elapsed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outInQuest      = false;
        outQuestId      = 0U;
        outElapsedTicks = 0;
        return false;
    }
}

struct WmCellSpec {
    std::uint8_t ch   = 0U;
    float        chip = 1.0f;
};

struct WmGridTable {
    WmCellSpec cells[kWmCellCount]{};
};

const WmGridTable& GetWmGridTable() noexcept {
    static const WmGridTable s_table = []() {
        static constexpr std::uint8_t kChMap[8] = { 0U, 1U, 1U, 2U, 0U, 2U, 2U, 1U };
        WmGridTable t{};
        for (std::size_t r = 0U; r < kWmGridRows; ++r) {
            for (std::size_t c = 0U; c < kWmGridCols; ++c) {
                const std::size_t cid = r * kWmGridCols + c;
                const std::uint32_t h = Mix32(
                    static_cast<std::uint32_t>(cid) * 2654435761U + 0x9E3779B9U, 0x517CC1B7U);
                t.cells[cid].ch   = kChMap[(r * 3U + c) & 7U];
                t.cells[cid].chip = (((h >> 16U) & 1U) != 0U) ? 1.0f : -1.0f;
            }
        }
        return t;
    }();
    return s_table;
}

double GetQpcElapsedSeconds() noexcept {
    std::call_once(g_qpcOnce, []() {
        LARGE_INTEGER freq{}, now{};
        ::QueryPerformanceFrequency(&freq);
        ::QueryPerformanceCounter(&now);
        g_qpcFreq   = (freq.QuadPart > 0) ? freq.QuadPart : 10000000LL;
        g_qpcOrigin = now.QuadPart;
    });
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_qpcOrigin) / static_cast<double>(g_qpcFreq);
}

PlayerDecoderSlot* FindOrAllocSlotLocked(std::uintptr_t entityPtr, bool isLocal, std::uint64_t tickSeq) {
    int freeIdx = -1;
    int oldestIdx = -1;
    std::uint64_t oldestTick = tickSeq;
    for (int i = 0; i < static_cast<int>(kMaxTrackedPlayers); ++i) {
        if (g_rxSlots[i].active) {
            if (g_rxSlots[i].entityPtr == entityPtr) {
                g_rxSlots[i].isLocal      = isLocal;
                g_rxSlots[i].lastSeenTick = tickSeq;
                return &g_rxSlots[i];
            }
            if (g_rxSlots[i].lastSeenTick <= oldestTick) {
                oldestTick = g_rxSlots[i].lastSeenTick;
                oldestIdx  = i;
            }
        } else if (freeIdx < 0) {
            freeIdx = i;
        }
    }
    const int chosen = (freeIdx >= 0) ? freeIdx : oldestIdx;
    if (chosen < 0) return nullptr;
    g_rxSlots[chosen].ClearSlot();
    g_rxSlots[chosen].active       = true;
    g_rxSlots[chosen].entityPtr    = entityPtr;
    g_rxSlots[chosen].isLocal      = isLocal;
    g_rxSlots[chosen].lastSeenTick = tickSeq;
    return &g_rxSlots[chosen];
}

void ProcessFrameOnSlotLocked(PlayerDecoderSlot& slot,
                              std::uint32_t seed27,
                              const std::string& playerName,
                              std::vector<PendingAlert>& outAlerts) {
    if (seed27 == 0U || seed27 == slot.lastSeed27) return;
    slot.lastSeed27 = seed27;

    std::uint8_t gen2 = 0U, pos6 = 0U, sym6 = 0U;
    if (!UnpackFrame27(seed27, gen2, pos6, sym6)) {
        ++slot.invalidStreak;
        if (slot.invalidStreak >= 3U) {
            slot.ResetAssembly();
        }
        return;
    }

    slot.invalidStreak = 0U;
    if (slot.activeGen2 != gen2) {
        slot.ResetAssembly();
        slot.activeGen2 = gen2;
    }

    if (sym6 == kSymEnd) {
        if (pos6 == 0U || pos6 > kMaxPayloadLen) {
            slot.ResetAssembly();
            return;
        }
        slot.endPos = pos6;
    } else {
        if (pos6 >= kMaxPayloadLen) {
            slot.ResetAssembly();
            return;
        }
        char ch = '\0';
        if (!DecodeSymbolChar(sym6, ch)) {
            slot.ResetAssembly();
            return;
        }
        slot.buf[pos6] = ch;
    }

    slot.seenMask |= (1ULL << pos6);
    ++slot.validFrames;
    if (slot.isLocal) {
        g_rxLocalFrameCount.fetch_add(1U, std::memory_order_relaxed);
    }

    if (slot.endPos != 0xFFU && slot.endPos >= 1U && slot.endPos <= kMaxPayloadLen && slot.validFrames >= 4U) {
        const std::uint64_t needMask = (slot.endPos == 63U)
                                           ? ~0ULL
                                           : ((1ULL << (slot.endPos + 1U)) - 1ULL);
        if ((slot.seenMask & needMask) != needMask) return;

        std::string decoded(slot.buf, slot.endPos);
        if (!ValidateDecodedList(decoded) ||
            ComputeListGen2(decoded.data(), decoded.size()) != slot.activeGen2) {
            slot.ResetAssembly();
            return;
        }

        if (decoded != slot.lastReportedList) {
            slot.lastReportedList = decoded;
            outAlerts.push_back(PendingAlert{slot.isLocal, playerName, decoded});
        }
    }
}

} // namespace

void Init(bool checkEnabled, bool selfCheckEnabled, AlertSinkFn alertSink) noexcept {
    g_checkEnabled.store(checkEnabled, std::memory_order_release);
    g_selfCheckEnabled.store(selfCheckEnabled, std::memory_order_release);
    g_inScene.store(false, std::memory_order_release);
    g_wmDebugVisible.store(false, std::memory_order_release);
    g_alertSink.store(alertSink, std::memory_order_release);
    g_txFrameCount.store(0U, std::memory_order_relaxed);
    g_audioSnapDirty.store(true, std::memory_order_release);
    ClearAllTaint();
    ResetDecoders();
    RefreshAudioMarkSnapshot();
}

void SetAlertSink(AlertSinkFn alertSink) noexcept {
    g_alertSink.store(alertSink, std::memory_order_release);
}

void SetCheckEnabled(bool enabled) noexcept {
    g_checkEnabled.store(enabled, std::memory_order_release);
}

bool IsCheckEnabled() noexcept {
    return g_checkEnabled.load(std::memory_order_acquire);
}

void SetSelfCheckEnabled(bool enabled) noexcept {
    g_selfCheckEnabled.store(enabled, std::memory_order_release);
}

bool IsSelfCheckEnabled() noexcept {
    return g_selfCheckEnabled.load(std::memory_order_acquire);
}

void MarkTainted(MhwRngPluginId ownerId) noexcept {
    const std::size_t bitIdx = static_cast<std::size_t>(ownerId) % 512U;
    const std::size_t word   = bitIdx >> 6U;
    const std::uint64_t mask = 1ULL << (bitIdx & 63U);
    if ((g_taintWords[word].load(std::memory_order_relaxed) & mask) == 0ULL) {
        g_taintWords[word].fetch_or(mask, std::memory_order_release);
        g_audioSnapDirty.store(true, std::memory_order_release);
    }
}

void ClearTainted(MhwRngPluginId ownerId) noexcept {
    const std::size_t bitIdx = static_cast<std::size_t>(ownerId) % 512U;
    const std::size_t word   = bitIdx >> 6U;
    const std::uint64_t mask = 1ULL << (bitIdx & 63U);
    if ((g_taintWords[word].load(std::memory_order_relaxed) & mask) != 0ULL) {
        g_taintWords[word].fetch_and(~mask, std::memory_order_release);
        g_audioSnapDirty.store(true, std::memory_order_release);
    }
}

void ClearAllTaint() noexcept {
    for (auto& w : g_taintWords) {
        w.store(0ULL, std::memory_order_release);
    }
    g_audioSnapDirty.store(true, std::memory_order_release);
}

bool HasAnyTaint() noexcept {
    std::uint64_t acc = 0ULL;
    for (const auto& w : g_taintWords) {
        acc |= w.load(std::memory_order_relaxed);
    }
    return acc != 0ULL;
}

bool IsInScene() noexcept {
    return g_inScene.load(std::memory_order_acquire);
}

bool IsVideoMarkActive() noexcept {
    return IsInScene() && HasAnyTaint();
}

void SetVideoMarkDebugVisible(bool visible) noexcept {
    g_wmDebugVisible.store(visible, std::memory_order_release);
}

bool IsVideoMarkDebugVisible() noexcept {
    return g_wmDebugVisible.load(std::memory_order_acquire);
}

void ResetDecoders() noexcept {
    g_rxLocalFrameCount.store(0U, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_rxMx);
    for (auto& s : g_rxSlots) {
        s.ClearSlot();
    }
    g_lastQuestValid   = false;
    g_lastInQuest      = false;
    g_lastQuestId      = 0U;
    g_lastQuestElapsed = 0;
}

bool EncodeSymbolChar(char c, std::uint8_t& outSym) noexcept {
    if (c >= 'a' && c <= 'z') {
        outSym = static_cast<std::uint8_t>(c - 'a');
        return true;
    }
    if (c == '_') {
        outSym = 26U;
        return true;
    }
    if (c >= '0' && c <= '9') {
        outSym = static_cast<std::uint8_t>(27 + (c - '0'));
        return true;
    }
    if (c == ',') {
        outSym = 37U;
        return true;
    }
    return false;
}

bool DecodeSymbolChar(std::uint8_t sym, char& outChar) noexcept {
    if (sym <= 25U) {
        outChar = static_cast<char>('a' + sym);
        return true;
    }
    if (sym == 26U) {
        outChar = '_';
        return true;
    }
    if (sym >= 27U && sym <= 36U) {
        outChar = static_cast<char>('0' + (sym - 27U));
        return true;
    }
    if (sym == 37U) {
        outChar = ',';
        return true;
    }
    return false;
}

std::uint8_t ComputeListGen2(const char* listStr, std::size_t len) noexcept {
    std::uint32_t h = 2166136261U;
    if (listStr) {
        for (std::size_t i = 0U; i < len; ++i) {
            h ^= static_cast<unsigned char>(listStr[i]);
            h *= 16777619U;
        }
    }
    h ^= h >> 16;
    return static_cast<std::uint8_t>((h ^ (h >> 8) ^ (h >> 2)) & 0x03U);
}

std::uint32_t PackFrame27(std::uint8_t nonce5,
                          std::uint8_t gen2,
                          std::uint8_t pos6,
                          std::uint8_t sym6) noexcept {
    std::uint32_t s = PackFrameWithNonce(nonce5, gen2, pos6, sym6);
    if (s == 0U) {
        s = PackFrameWithNonce(static_cast<std::uint8_t>(nonce5 ^ 1U), gen2, pos6, sym6);
    }
    return s;
}

bool UnpackFrame27(std::uint32_t seed27,
                   std::uint8_t& outGen2,
                   std::uint8_t& outPos6,
                   std::uint8_t& outSym6) noexcept {
    if (seed27 == 0U || seed27 > 0x07FFFFFFU) return false;
    const std::uint32_t n        = (seed27 >> 22) & 0x1FU;
    const std::uint32_t cipher14 = (seed27 >> 8) & 0x3FFFU;
    const std::uint32_t mac8     = seed27 & 0xFFU;
    const std::uint32_t expMac8  = Mix32((n << 14) | cipher14, kMacKey) & 0xFFU;
    if (mac8 != expMac8) return false;

    const std::uint32_t mask14  = Mix32(n, kMaskKey) & 0x3FFFU;
    const std::uint32_t plain14 = cipher14 ^ mask14;
    const auto sym6             = static_cast<std::uint8_t>(plain14 & 0x3FU);
    if (sym6 > kSymEnd) return false;

    outGen2 = static_cast<std::uint8_t>((plain14 >> 12) & 0x03U);
    outPos6 = static_cast<std::uint8_t>((plain14 >> 6) & 0x3FU);
    outSym6 = sym6;
    return true;
}

bool IsOnlineSessionSEH(std::uintptr_t exeBase) noexcept {
    if (!exeBase) return false;
    __try {
        return IsOnlineSessionInternal(exeBase);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t MaybeEncodeCritSeed(std::uintptr_t exeBase,
                                  std::uint32_t callSiteRva,
                                  std::uint32_t rawValue) noexcept {
    if (callSiteRva != mhw::kRvaCritSeedSite || !HasAnyTaint()) {
        return rawValue;
    }
    const bool emitAllowed = g_selfCheckEnabled.load(std::memory_order_relaxed) || IsOnlineSessionSEH(exeBase);
    if (!emitAllowed) {
        return rawValue;
    }

    std::lock_guard<std::mutex> lk(g_txMx);
    SyncTxPayloadFromAtomicsLocked();
    if (g_txPayload.empty()) {
        return rawValue;
    }

    const std::size_t frameLen = g_txPayload.size() + 1U;
    const std::size_t pos      = g_txCursor % frameLen;
    g_txCursor                 = (pos + 1U) % frameLen;

    std::uint8_t sym = kSymEnd;
    if (pos < g_txPayload.size()) {
        if (!EncodeSymbolChar(g_txPayload[pos], sym)) {
            return rawValue;
        }
    }
    const auto nonce5 = static_cast<std::uint8_t>(
        (rawValue ^ (rawValue >> 11) ^ (rawValue >> 21) ^ (g_txSeq++ * 13U)) & 0x1FU);
    const std::uint32_t s27 = PackFrame27(nonce5, g_txGen2, static_cast<std::uint8_t>(pos), sym);
    g_txFrameCount.fetch_add(1U, std::memory_order_relaxed);
    return (rawValue & 0xF8000000U) | s27;
}

std::uint16_t ComputeVideoMarkCrc12(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint32_t crc = 0xFFFU;
    if (data) {
        for (std::size_t i = 0U; i < len; ++i) {
            crc ^= (static_cast<std::uint32_t>(data[i]) << 4U);
            for (int b = 0; b < 8; ++b) {
                if ((crc & 0x800U) != 0U) {
                    crc = ((crc << 1U) ^ 0x80FU) & 0xFFFU;
                } else {
                    crc = (crc << 1U) & 0xFFFU;
                }
            }
        }
    }
    return static_cast<std::uint16_t>(crc & 0xFFFU);
}

bool BuildVideoMarkFrameAt(double tSec,
                           const std::string& payload,
                           bool debugVisible,
                           VideoMarkFrame& out) noexcept {
    out = VideoMarkFrame{};
    if (payload.empty()) return false;

    const std::size_t len = (payload.size() > kMaxPayloadLen) ? kMaxPayloadLen : payload.size();
    std::uint8_t crcBuf[kMaxPayloadLen + 1U]{};
    crcBuf[0] = static_cast<std::uint8_t>(len);

    std::uint8_t syms[kMaxPayloadLen + 5U]{};
    syms[0] = kWmSync0;
    syms[1] = kWmSync1;
    syms[2] = static_cast<std::uint8_t>(len);
    for (std::size_t i = 0U; i < len; ++i) {
        std::uint8_t sym = 0U;
        if (!EncodeSymbolChar(payload[i], sym)) return false;
        crcBuf[1U + i] = sym;
        syms[3U + i]   = sym;
    }
    const std::uint16_t crc12 = ComputeVideoMarkCrc12(crcBuf, 1U + len);
    syms[3U + len] = static_cast<std::uint8_t>((crc12 >> 6U) & 0x3FU);
    syms[4U + len] = static_cast<std::uint8_t>(crc12 & 0x3FU);
    const std::size_t totalSyms = len + 5U;

    const double phase  = tSec * kWmFreqHz;
    const double slotFl = std::floor(phase);
    const auto   slot   = static_cast<std::int64_t>(slotFl);
    double u = phase - slotFl;
    if (u < 0.0) u = 0.0;
    if (u >= 1.0) u = 0.999999;

    const double carr = std::sin(6.283185307179586 * u);
    if (std::fabs(carr) < 1e-4) {
        return false;
    }

    const std::int64_t totalSlots = static_cast<std::int64_t>(totalSyms) * 3LL;
    const std::int64_t ts         = ((slot % totalSlots) + totalSlots) % totalSlots;
    const std::uint8_t sym6       = syms[static_cast<std::size_t>(ts / 3LL)];
    const unsigned     sub        = static_cast<unsigned>(ts % 3LL);
    const std::uint8_t bHi        = (sym6 >> (5U - 2U * sub)) & 1U;
    const std::uint8_t bLo        = (sym6 >> (4U - 2U * sub)) & 1U;

    const float chBit[3] = {
        1.0f,
        (bHi != 0U) ? 1.0f : -1.0f,
        (bLo != 0U) ? 1.0f : -1.0f
    };

    const float fCarr = static_cast<float>(carr);
    const WmGridTable& grid = GetWmGridTable();
    for (std::size_t cid = 0U; cid < kWmCellCount; ++cid) {
        const WmCellSpec& sp = grid.cells[cid];
        out.cellWeights[cid] = fCarr * sp.chip * chBit[sp.ch];
    }

    out.active       = true;
    out.debugVisible = debugVisible;
    return true;
}

bool GetCurrentVideoMarkFrame(VideoMarkFrame& out) noexcept {
    out = VideoMarkFrame{};
    if (!IsVideoMarkActive()) return false;

    std::string payload;
    {
        std::lock_guard<std::mutex> lk(g_txMx);
        SyncTxPayloadFromAtomicsLocked();
        payload = g_txPayload;
    }
    if (payload.empty()) return false;

    const double tSec = GetQpcElapsedSeconds();
    return BuildVideoMarkFrameAt(tSec, payload, IsVideoMarkDebugVisible(), out);
}

bool BuildAudioMarkPacket(const std::string& payload, AudioMarkPacket& out) noexcept {
    out = AudioMarkPacket{};
    if (payload.empty()) return false;

    const std::size_t len = (payload.size() > kMaxPayloadLen) ? kMaxPayloadLen : payload.size();
    std::uint8_t crcBuf[kMaxPayloadLen + 1U]{};
    crcBuf[0] = static_cast<std::uint8_t>(len);

    std::uint8_t syms[kMaxPayloadLen + 5U]{};
    syms[0] = kWmSync0;
    syms[1] = kWmSync1;
    syms[2] = static_cast<std::uint8_t>(len);
    for (std::size_t i = 0U; i < len; ++i) {
        std::uint8_t sym = 0U;
        if (!EncodeSymbolChar(payload[i], sym)) return false;
        crcBuf[1U + i] = sym;
        syms[3U + i]   = sym;
    }
    const std::uint16_t crc12 = ComputeVideoMarkCrc12(crcBuf, 1U + len);
    syms[3U + len] = static_cast<std::uint8_t>((crc12 >> 6U) & 0x3FU);
    syms[4U + len] = static_cast<std::uint8_t>(crc12 & 0x3FU);

    const std::size_t totalSyms  = len + 5U;
    const std::size_t totalSlots = totalSyms * 3U;
    if (totalSlots > kMaxAudioMarkSlots) return false;

    for (std::size_t si = 0U; si < totalSyms; ++si) {
        const std::uint8_t sym6 = syms[si];
        for (unsigned sub = 0U; sub < 3U; ++sub) {
            const std::size_t slot = si * 3U + sub;
            const std::uint8_t bHi = (sym6 >> (5U - 2U * sub)) & 1U;
            const std::uint8_t bLo = (sym6 >> (4U - 2U * sub)) & 1U;
            out.b1[slot] = (bHi != 0U) ? 1 : -1;
            out.b2[slot] = (bLo != 0U) ? 1 : -1;
        }
    }
    out.slotCount = static_cast<std::uint32_t>(totalSlots);
    out.active    = true;
    return true;
}

void RefreshAudioMarkSnapshot() noexcept {
    const bool shouldBeActive = IsVideoMarkActive();
    if (!g_audioSnapDirty.load(std::memory_order_acquire) &&
        shouldBeActive == g_audioSnapBuiltActive.load(std::memory_order_acquire)) {
        return;
    }

    AudioMarkPacket next{};
    if (shouldBeActive) {
        std::string payload;
        {
            std::lock_guard<std::mutex> lk(g_txMx);
            SyncTxPayloadFromAtomicsLocked();
            payload = g_txPayload;
        }
        if (!payload.empty()) {
            (void)BuildAudioMarkPacket(payload, next);
        }
    }

    std::uint64_t tmp[kAudioSnapWords]{};
    std::memcpy(tmp, &next, sizeof(AudioMarkPacket));

    std::lock_guard<std::mutex> pubLk(g_audioPubMx);
    const std::uint32_t seq = g_audioSnapSeq.load(std::memory_order_relaxed);
    g_audioSnapSeq.store(seq + 1U, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0U; i < kAudioSnapWords; ++i) {
        g_audioSnapWords[i].store(tmp[i], std::memory_order_relaxed);
    }
    g_audioSnapBuiltActive.store(shouldBeActive, std::memory_order_release);
    g_audioSnapDirty.store(false, std::memory_order_release);
    g_audioSnapSeq.store(seq + 2U, std::memory_order_release);
}

bool GetCurrentAudioMarkPacket(AudioMarkPacket& out, bool* outBusy) noexcept {
    out = AudioMarkPacket{};
    if (outBusy) *outBusy = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint32_t s1 = g_audioSnapSeq.load(std::memory_order_acquire);
        if ((s1 & 1U) != 0U) {
            ::YieldProcessor();
            continue;
        }
        std::uint64_t tmp[kAudioSnapWords]{};
        for (std::size_t i = 0U; i < kAudioSnapWords; ++i) {
            tmp[i] = g_audioSnapWords[i].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t s2 = g_audioSnapSeq.load(std::memory_order_acquire);
        if (s1 == s2) {
            std::memcpy(&out, tmp, sizeof(AudioMarkPacket));
            return out.active && out.slotCount > 0U;
        }
        ::YieldProcessor();
    }
    if (outBusy) *outBusy = true;
    return false;
}

DiagSnapshot GetDiagSnapshot() noexcept {
    std::uint64_t words[kTaintWords]{};
    for (std::size_t i = 0U; i < kTaintWords; ++i) {
        words[i] = g_taintWords[i].load(std::memory_order_acquire);
    }
    DiagSnapshot snap{};
    snap.activeTaint   = BuildTaintListFromWords(words);
    snap.txFrames      = g_txFrameCount.load(std::memory_order_relaxed);
    snap.rxLocalFrames = g_rxLocalFrameCount.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_rxMx);
        snap.localFound = (g_lastLocalPlayerPtr != 0ULL);
    }
    return snap;
}

void TickOnMainThread(std::uintptr_t exeBase) noexcept {
    if (!exeBase) return;

    ObservedPlayer players[kMaxTrackedPlayers]{};
    std::size_t playerCount = 0U;
    std::uintptr_t curLocalEntity = 0;
    const bool okRead = CollectPlayers(exeBase, players, playerCount, curLocalEntity);
    g_inScene.store(okRead && curLocalEntity != 0ULL, std::memory_order_release);

    bool curInQuest = false;
    std::uint32_t curQuestId = 0U;
    std::int32_t curQuestElapsed = 0;
    const bool questReadOk = TryReadQuestStateSEH(exeBase, curInQuest, curQuestId, curQuestElapsed);

    const bool checkOn     = g_checkEnabled.load(std::memory_order_acquire);
    const bool selfCheckOn = g_selfCheckEnabled.load(std::memory_order_acquire);

    std::vector<PendingAlert> alerts;
    {
        std::lock_guard<std::mutex> lk(g_rxMx);
        const std::uint64_t tickSeq = ++g_rxTickSeq;

        if (!okRead || curLocalEntity != g_lastLocalPlayerPtr) {
            const std::uintptr_t prevLocal = g_lastLocalPlayerPtr;
            g_lastLocalPlayerPtr = okRead ? curLocalEntity : 0;
            if (prevLocal != 0 && (!okRead || curLocalEntity == 0)) {
                ClearAllTaint();
            }
            for (auto& s : g_rxSlots) {
                s.ClearSlot();
            }
        }

        if (questReadOk) {
            if (g_lastQuestValid &&
                (curInQuest != g_lastInQuest ||
                 (curInQuest && curQuestId != g_lastQuestId) ||
                 (curInQuest && g_lastQuestElapsed > 60 && curQuestElapsed + 60 < g_lastQuestElapsed))) {
                for (auto& s : g_rxSlots) {
                    s.ClearSlot();
                }
            }
            g_lastQuestValid   = true;
            g_lastInQuest      = curInQuest;
            g_lastQuestId      = curQuestId;
            g_lastQuestElapsed = curQuestElapsed;
        } else {
            g_lastQuestValid   = false;
            g_lastInQuest      = false;
            g_lastQuestId      = 0U;
            g_lastQuestElapsed = 0;
        }

        if (okRead && (checkOn || selfCheckOn)) {
            int remoteCounter = 1;
            for (std::size_t idx = 0U; idx < playerCount; ++idx) {
                const ObservedPlayer& p = players[idx];
                if (p.isLocal) {
                    if (!selfCheckOn) continue;
                } else {
                    if (!checkOn) continue;
                }
                const int fallbackSlot = p.isLocal ? 0 : (remoteCounter++);
                PlayerDecoderSlot* slot = FindOrAllocSlotLocked(p.entityPtr, p.isLocal, tickSeq);
                if (!slot) continue;

                if (p.seed27 != 0U && p.seed27 != slot->lastSeed27) {
                    const std::string playerName = ResolvePlayerName(
                        exeBase, p.isLocal, p.questIdx, p.lobbyIdx, fallbackSlot);
                    ProcessFrameOnSlotLocked(*slot, p.seed27, playerName, alerts);
                }
            }

            for (auto& s : g_rxSlots) {
                if (s.active && (tickSeq - s.lastSeenTick) > kStaleTickLimit) {
                    s.ClearSlot();
                }
            }
        }
    }

    RefreshAudioMarkSnapshot();

    if (alerts.empty()) return;
    const AlertSinkFn sink = g_alertSink.load(std::memory_order_acquire);
    if (!sink) return;
    const bool en = app::IsEnglish();
    for (const auto& a : alerts) {
        char buf[256]{};
        if (en) {
            std::snprintf(buf, sizeof(buf),
                          a.isLocal ? "[RNG] [SelfCheck] Detected %s using plugin(s): %s"
                                    : "[RNG] Detected %s using plugin(s): %s",
                          a.playerName.c_str(), a.pluginList.c_str());
        } else {
            std::snprintf(buf, sizeof(buf),
                          a.isLocal ? u8"[RNG] [自检] 检测到 %s 使用了插件: %s"
                                    : u8"[RNG] 检测到 %s 使用了插件: %s",
                          a.playerName.c_str(), a.pluginList.c_str());
        }
        sink(buf);
    }
}

} // namespace peer_beacon
