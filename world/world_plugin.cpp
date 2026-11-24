// ============================================================================
//  MIRA Built-in Core Service Provider: World Context & Events (`mhw_world.dll`)
//  Version : 1.0.0 (SDK v3.5)
//  Service : "mhw.world" (v1, `MhwWorldApi`)
//  Events  : "mhw.world.*" (scene_enter, title_return, quest_enter, quest_leave,
//            stage_changed, player_changed, hour_changed, weather_changed)
//
//  Strict Microkernel Discipline:
//    - Zero RNG detours, zero game memory writes.
//    - Polls verified 15.23 world singletons via `RegisterMainThreadTick` with
//      SEH protection (`mhw_1523_world.h`), emits Edge-Triggered Event Bus
//      topics, and drains main-thread HUD popups (`sMhGUI::DisplayPopup`).
//    - Strict leaf mutex (`g_stateMx`): NEVER call `g_host->...` while locked.
// ============================================================================
#include "mhw_rng_api.h"
#include "mhw_rng_math.h"
#include "mhw_1523_world.h"
#include "mhw_world_api.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using DisplayPopupFn = void(__fastcall*)(void* sMhGui,
                                         const char* msg,
                                         float durationSec,
                                         float delaySec,
                                         std::uint8_t flags,
                                         float unk1,
                                         float unk2);

struct PopupEntry {
    char         msg[256]{};
    float        durationSec = 3.0f;
    float        delaySec    = 0.0f;
    std::uint8_t flags       = 0U;
};

constexpr std::size_t kPopupQueueCap = 16U;

const MhwRngHostApi*        g_host   = nullptr;
MhwRngPluginId              g_selfId = 0U;
MhwRngHookHandle            g_tickHdl = kMhwRngInvalidHandle;
std::atomic<bool>           g_shuttingDown{false};
std::atomic<std::uint64_t>  g_popupsDelivered{0ULL};

// Strict leaf mutex: NEVER call `g_host->...` or game functions while holding `g_stateMx`!
std::mutex                  g_stateMx;
bool                        g_hasBaseline = false;
bool                        g_logEvents   = false;
std::uint64_t               g_sessionGen  = 0ULL;
std::uint64_t               g_tickCount   = 0ULL;
MhwWorldState               g_state{};

PopupEntry                  g_popupQueue[kPopupQueueCap]{};
std::size_t                 g_popupHead  = 0U;
std::size_t                 g_popupTail  = 0U;
std::size_t                 g_popupCount = 0U;

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

void CopyTrunc(char* dst, std::size_t cap, const char* src) noexcept {
    if (!dst || cap == 0U) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::size_t i = 0U;
    while (i + 1U < cap && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

MhwWorldState SnapshotToWorldState(const mhw1523::WorldStateSnapshot& s) noexcept {
    MhwWorldState w{};
    w.structSize        = sizeof(MhwWorldState);
    w.inScene           = s.inScene ? 1U : 0U;
    w.inQuest           = s.inQuest ? 1U : 0U;
    w.isVillage         = s.isVillage ? 1U : 0U;
    w.isGuidingLands    = s.isGuidingLands ? 1U : 0U;
    w.isSpecialWeather  = s.isSpecialWeather ? 1U : 0U;
    w.hasTime           = s.hasTime ? 1U : 0U;
    w.hasQuest          = s.hasQuest ? 1U : 0U;
    w.hasArea           = s.hasArea ? 1U : 0U;
    w.questId           = s.questId;
    w.questState        = s.questState;
    w.stageId           = s.stageId;
    w.weatherId         = s.weatherId;
    w.questElapsedTicks = s.questElapsedTicks;
    w.gameHour          = s.gameHour;
    w.gameHourInt       = s.gameHourInt;
    std::memcpy(w.glRegionLevels, s.glRegionLevels, sizeof(w.glRegionLevels));
    w.localPlayerPtr    = s.localPlayerPtr;
    return w;
}

bool TryCallDisplayPopupSEH(std::uintptr_t exeBase,
                            const char* utf8Msg,
                            float durationSec,
                            float delaySec,
                            std::uint8_t flags) noexcept {
    if (!exeBase || !utf8Msg || utf8Msg[0] == '\0') return false;
    __try {
        const std::uintptr_t sMhGui = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw1523::kRvaSMhGui);
        if (!mhw_sdk::IsValidUserPtr(sMhGui)) return false;
        const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(sMhGui);
        if (!mhw_sdk::IsValidUserPtr(vft)) return false;
        const auto fn = reinterpret_cast<DisplayPopupFn>(exeBase + mhw1523::kRvaDisplayPopup);
        fn(reinterpret_cast<void*>(sMhGui), utf8Msg, durationSec, delaySec, flags, 0.0f, 0.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FormatWorldSummaryText(const MhwWorldState& st, char* outBuf, std::size_t bufCap, bool en) noexcept {
    if (!outBuf || bufCap == 0U) return;
    if (!st.inScene && !st.hasArea && !st.hasQuest && !st.hasTime) {
        std::snprintf(outBuf, bufCap, en ? "Title / Loading" : u8"标题画面 / 加载中");
        return;
    }
    const char* sceneLbl = !st.inScene
                               ? (en ? "Title" : u8"标题")
                               : (st.inQuest ? (st.isGuidingLands ? (en ? "GuidingLands" : u8"聚魔之地")
                                                                  : (en ? "Quest" : u8"任务中"))
                                             : (st.isVillage ? (en ? "Hub" : u8"据点")
                                                             : (en ? "Field" : u8"探索中")));
    const char* weatherLbl = !st.hasTime
                                 ? "-"
                                 : (st.isSpecialWeather ? (en ? "Special" : u8"特殊天气")
                                                        : (en ? "Clear" : u8"晴天"));
    if (st.inQuest) {
        std::snprintf(outBuf, bufCap,
                      en ? "%s | Q:%u Stg:%u | Hr:%.1f(%s)"
                         : u8"%s | 任务:%u 地图:%u | 时间:%.1f(%s)",
                      sceneLbl, st.questId, st.stageId, static_cast<double>(st.gameHour), weatherLbl);
    } else {
        std::snprintf(outBuf, bufCap,
                      en ? "%s | Stg:%u | Hr:%.1f(%s)"
                         : u8"%s | 地图:%u | 时间:%.1f(%s)",
                      sceneLbl, st.stageId, static_cast<double>(st.gameHour), weatherLbl);
    }
}

// ============================================================================
//  MhwWorldApi Implementation
// ============================================================================

bool __fastcall World_GetState(MhwWorldState* outState) {
    if (!outState) return false;
    std::lock_guard<std::mutex> lk(g_stateMx);
    *outState            = g_state;
    outState->structSize = sizeof(MhwWorldState);
    outState->sessionGen = g_sessionGen;
    outState->tickCount  = g_tickCount;
    return true;
}

bool __fastcall World_RefreshStateNow(MhwWorldState* outState) {
    if (!outState || !g_host || !g_host->exeBase) return false;
    mhw1523::WorldStateSnapshot snap{};
    mhw1523::ReadWorldStateSEH(g_host->exeBase, snap);
    MhwWorldState curr = SnapshotToWorldState(snap);

    std::lock_guard<std::mutex> lk(g_stateMx);
    curr.sessionGen = g_sessionGen;
    curr.tickCount  = g_tickCount;
    *outState       = curr;
    return true;
}

std::uint64_t __fastcall World_GetSessionGen() {
    std::lock_guard<std::mutex> lk(g_stateMx);
    return g_sessionGen;
}

bool __fastcall World_DisplayPopupAsync(const char* utf8Msg,
                                        float durationSec,
                                        float delaySec,
                                        std::uint8_t flags) {
    if (!utf8Msg || utf8Msg[0] == '\0' || g_shuttingDown.load(std::memory_order_acquire)) return false;
    PopupEntry entry{};
    CopyTrunc(entry.msg, sizeof(entry.msg), utf8Msg);
    entry.durationSec = (durationSec > 0.1f && durationSec <= 30.0f) ? durationSec : 3.0f;
    entry.delaySec    = (delaySec >= 0.0f && delaySec <= 10.0f) ? delaySec : 0.0f;
    entry.flags       = flags;

    std::lock_guard<std::mutex> lk(g_stateMx);
    if (g_popupCount >= kPopupQueueCap) {
        g_popupHead = (g_popupHead + 1U) % kPopupQueueCap;
        --g_popupCount;
    }
    g_popupQueue[g_popupTail] = entry;
    g_popupTail = (g_popupTail + 1U) % kPopupQueueCap;
    ++g_popupCount;
    return true;
}

bool __fastcall World_GetGuidingLandsLevel(std::uint32_t regionIdx, std::uint8_t* outLevel) {
    if (!outLevel || regionIdx >= 6U) return false;
    std::lock_guard<std::mutex> lk(g_stateMx);
    if (!g_state.hasQuest) {
        *outLevel = 0U;
        return false;
    }
    *outLevel = g_state.glRegionLevels[regionIdx];
    return true;
}

bool __fastcall World_FormatWorldSummary(char* outBuf, std::size_t bufCap, bool english) {
    if (!outBuf || bufCap == 0U) return false;
    MhwWorldState st{};
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        st = g_state;
    }
    FormatWorldSummaryText(st, outBuf, bufCap, english);
    return true;
}

const MhwWorldApi g_worldApi = {
    kMhwWorldServiceVersion,
    static_cast<std::uint32_t>(sizeof(MhwWorldApi)),
    &World_GetState,
    &World_RefreshStateNow,
    &World_GetSessionGen,
    &World_DisplayPopupAsync,
    &World_GetGuidingLandsLevel,
    &World_FormatWorldSummary
};

// ============================================================================
//  Main-Thread Tick Polling & Edge-Triggered Event Bus Dispatch
// ============================================================================

void __fastcall OnMainThreadTick(std::uint64_t, void*) {
    if (g_shuttingDown.load(std::memory_order_acquire) || !g_host || !g_host->exeBase) return;

    mhw1523::WorldStateSnapshot snap{};
    mhw1523::ReadWorldStateSEH(g_host->exeBase, snap);
    MhwWorldState curr = SnapshotToWorldState(snap);

    MhwWorldEventPayload evPayload{};
    evPayload.structSize = sizeof(MhwWorldEventPayload);
    bool logEvents       = false;

    PopupEntry popped[2]{};
    std::size_t poppedCount = 0U;

    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        curr.tickCount = ++g_tickCount;
        logEvents      = g_logEvents;

        if (!g_hasBaseline) {
            g_hasBaseline = true;
            if (curr.inScene || curr.inQuest) {
                ++g_sessionGen;
            }
            curr.sessionGen = g_sessionGen;
            g_state         = curr;
        } else {
            const MhwWorldState prev = g_state;
            std::uint32_t trans = 0U;

            if (curr.inScene && !prev.inScene) trans |= kMhwWorldTransSceneEnter;
            if (!curr.inScene && prev.inScene) trans |= kMhwWorldTransTitleReturn;

            const bool questRestarted =
                curr.inQuest && prev.inQuest &&
                ((curr.questId != prev.questId && curr.questId != 0U) ||
                 (curr.localPlayerPtr != 0ULL && prev.localPlayerPtr != 0ULL &&
                  curr.localPlayerPtr != prev.localPlayerPtr) ||
                 (curr.questElapsedTicks >= 0 && prev.questElapsedTicks > curr.questElapsedTicks + 60));

            if ((curr.inQuest && !prev.inQuest) || questRestarted) {
                trans |= kMhwWorldTransQuestEnter;
            }
            if (!curr.inQuest && prev.inQuest) {
                trans |= kMhwWorldTransQuestLeave;
            }
            if (curr.hasArea && prev.hasArea && curr.stageId != prev.stageId) {
                trans |= kMhwWorldTransStageChanged;
            }
            if (curr.localPlayerPtr != prev.localPlayerPtr) {
                trans |= kMhwWorldTransPlayerChanged;
            }
            if (curr.hasTime && prev.hasTime && curr.gameHourInt != prev.gameHourInt) {
                trans |= kMhwWorldTransHourChanged;
            }
            if (curr.hasTime && prev.hasTime &&
                (curr.isSpecialWeather != prev.isSpecialWeather || curr.weatherId != prev.weatherId)) {
                trans |= kMhwWorldTransWeatherChange;
            }

            if ((trans & (kMhwWorldTransSceneEnter | kMhwWorldTransTitleReturn |
                          kMhwWorldTransQuestEnter | kMhwWorldTransQuestLeave)) != 0U) {
                ++g_sessionGen;
            }
            curr.sessionGen = g_sessionGen;
            g_state         = curr;

            evPayload.transitionFlags = trans;
            evPayload.sessionGen      = g_sessionGen;
            evPayload.prev            = prev;
            evPayload.curr            = curr;
        }

        if (curr.inScene) {
            while (g_popupCount > 0U && poppedCount < 2U) {
                popped[poppedCount++] = g_popupQueue[g_popupHead];
                g_popupHead = (g_popupHead + 1U) % kPopupQueueCap;
                --g_popupCount;
            }
        }
    }

    // Emit all triggered topics strictly OUTSIDE `g_stateMx`!
    const std::uint32_t t = evPayload.transitionFlags;
    if (t != 0U) {
        struct TopicBit {
            std::uint32_t flag;
            const char*   topic;
        };
        static constexpr TopicBit kTopicBits[] = {
            { kMhwWorldTransTitleReturn,   kMhwWorldEvTitleReturn    },
            { kMhwWorldTransQuestLeave,    kMhwWorldEvQuestLeave     },
            { kMhwWorldTransSceneEnter,    kMhwWorldEvSceneEnter     },
            { kMhwWorldTransQuestEnter,    kMhwWorldEvQuestEnter     },
            { kMhwWorldTransStageChanged,  kMhwWorldEvStageChanged   },
            { kMhwWorldTransPlayerChanged, kMhwWorldEvPlayerChanged  },
            { kMhwWorldTransHourChanged,   kMhwWorldEvHourChanged    },
            { kMhwWorldTransWeatherChange, kMhwWorldEvWeatherChanged },
        };
        for (const auto& tb : kTopicBits) {
            if ((t & tb.flag) != 0U) {
                MhwRng_HostEmitEvent(g_host, g_selfId, tb.topic,
                                     &evPayload, static_cast<std::uint32_t>(sizeof(evPayload)));
            }
        }
        if (logEvents) {
            const bool en = MhwRng_HostIsEnglish(g_host);
            char summary[160]{};
            FormatWorldSummaryText(curr, summary, sizeof(summary), en);
            g_host->ChatPrintfAsync(en ? "[world] Gen #%llu (flags=0x%X): %s"
                                       : u8"[world] 轮次 #%llu (变更=0x%X): %s",
                                    static_cast<unsigned long long>(curr.sessionGen),
                                    t,
                                    summary);
        }
    }

    for (std::size_t i = 0U; i < poppedCount; ++i) {
        if (TryCallDisplayPopupSEH(g_host->exeBase, popped[i].msg,
                                   popped[i].durationSec, popped[i].delaySec, popped[i].flags)) {
            g_popupsDelivered.fetch_add(1U, std::memory_order_relaxed);
        }
    }
}

// ============================================================================
//  Chat Subcommand (`$rng world ...`) & Tab Completer
// ============================================================================

void PrintWorldHelp(bool en) {
    if (en) {
        g_host->ChatPrint("[world] Usage:");
        g_host->ChatPrint("  $rng world [status]      : Live scene/quest/stage/time/weather/GL summary");
        g_host->ChatPrint("  $rng world events on|off : Toggle live transition logging in console/chat");
        g_host->ChatPrint("  $rng world popup <text>  : Queue a right-side HUD popup on the main thread");
    } else {
        g_host->ChatPrint(u8"[world] 用法:");
        g_host->ChatPrint(u8"  $rng world [status]      : 查看当前场景/任务/地图/游戏时间/天气与聚魔等级");
        g_host->ChatPrint(u8"  $rng world events on|off : 开关进图/回标题/天气变更日志");
        g_host->ChatPrint(u8"  $rng world popup <文本>  : 主线程安全投递游戏右侧弹窗通知");
    }
}

void PrintWorldStatus(bool en) {
    MhwWorldState st{};
    bool logEv = false;
    std::size_t queued = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        st     = g_state;
        logEv  = g_logEvents;
        queued = g_popupCount;
    }
    char summary[160]{};
    FormatWorldSummaryText(st, summary, sizeof(summary), en);
    const auto delivered = static_cast<unsigned long long>(g_popupsDelivered.load(std::memory_order_relaxed));
    g_host->ChatPrintf(en ? "[world] %s | Session:%llu Ticks:%llu | Events:%s | Popups:%llu (queued:%zu)"
                          : u8"[world] %s | 会话:%llu 帧:%llu | 事件日志:%s | 弹窗:%llu (待发:%zu)",
                       summary,
                       static_cast<unsigned long long>(st.sessionGen),
                       static_cast<unsigned long long>(st.tickCount),
                       logEv ? (en ? "ON" : u8"开") : (en ? "OFF" : u8"关"),
                       delivered,
                       queued);
    if (st.hasQuest && st.isGuidingLands) {
        g_host->ChatPrintf(en ? "[world] GL Regions: Forest:%u Wildspire:%u Coral:%u Vale:%u Recess:%u Tundra:%u"
                              : u8"[world] 聚魔地区等级: 森林:%u 荒地:%u 珊瑚:%u 瘴气:%u 龙结晶:%u 永霜:%u",
                           st.glRegionLevels[0], st.glRegionLevels[1], st.glRegionLevels[2],
                           st.glRegionLevels[3], st.glRegionLevels[4], st.glRegionLevels[5]);
    }
}

void __fastcall Cmd_World(int argc, const char* const* argv, const char* rawArgStr, void*) {
    if (!g_host) return;
    const bool en = MhwRng_HostIsEnglish(g_host);
    if (argc <= 0) {
        PrintWorldStatus(en);
        return;
    }
    const std::string sub = LowerAscii(argv[0]);
    if (sub == "status" || sub == "stat") {
        PrintWorldStatus(en);
        return;
    }
    if (sub == "help" || sub == "?") {
        PrintWorldHelp(en);
        return;
    }
    if (sub == "events" || sub == "log") {
        if (argc < 2) {
            bool cur = false;
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                cur = g_logEvents;
            }
            g_host->ChatPrintf(en ? "[world] Event logging is %s (use `events on|off`)"
                                  : u8"[world] 事件日志当前: %s (用法 `events on|off`)",
                               cur ? (en ? "ON" : u8"开") : (en ? "OFF" : u8"关"));
            return;
        }
        const std::string v = LowerAscii(argv[1]);
        const bool on = (v == "on" || v == "1" || v == "true");
        const bool off = (v == "off" || v == "0" || v == "false");
        if (!on && !off) {
            PrintWorldHelp(en);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_logEvents = on;
        }
        g_host->ChatPrintf(en ? "[world] Event logging -> %s" : u8"[world] 事件日志 -> %s",
                           on ? (en ? "ON" : u8"开") : (en ? "OFF" : u8"关"));
        return;
    }
    if (sub == "popup") {
        const char* text = nullptr;
        if (rawArgStr && rawArgStr[0] != '\0') {
            const char* p = rawArgStr;
            while (*p && !std::isspace(static_cast<unsigned char>(*p))) ++p;
            while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
            if (*p != '\0') text = p;
        }
        if (!text && argc >= 2) text = argv[1];
        if (!text || text[0] == '\0') {
            g_host->ChatPrint(en ? "[world] Usage: `$rng world popup <text>`"
                                 : u8"[world] 用法: `$rng world popup <文本>`");
            return;
        }
        (void)World_DisplayPopupAsync(text, 3.0f, 0.0f, 0U);
        g_host->ChatPrintf(en ? "[world] Queued HUD popup: %s" : u8"[world] 已入队游戏右侧弹窗: %s", text);
        return;
    }

    PrintWorldHelp(en);
}

void __fastcall Complete_World(const char*,
                               int argc,
                               const char* const* argv,
                               const char*,
                               MhwRngCompletionSinkFn addCand,
                               void* sinkCtx,
                               void*) {
    if (!addCand) return;
    if (argc == 0) {
        addCand("status", sinkCtx);
        addCand("events", sinkCtx);
        addCand("popup", sinkCtx);
        addCand("help", sinkCtx);
        return;
    }
    if (argc == 1 && argv && argv[0]) {
        const std::string sub = LowerAscii(argv[0]);
        if (sub == "events" || sub == "log") {
            addCand("on", sinkCtx);
            addCand("off", sinkCtx);
        }
    }
}

const MhwRngPluginDescriptor kDesc = {
    kMhwRngApiVersion,
    "world",
    u8"世界上下文与事件服务",
    "1.0.0",
    u8"提供共享游戏场景/任务/时间/天气状态、事件总线与主线程弹窗服务"
};

} // namespace

MHW_RNG_PLUGIN_EXPORT const MhwRngPluginDescriptor* MhwRngPlugin_GetInfo() {
    return &kDesc;
}

MHW_RNG_PLUGIN_EXPORT bool MhwRngPlugin_Init(const MhwRngHostApi* host, MhwRngPluginId selfId) {
    if (!host || host->apiVersion != kMhwRngApiVersion || !host->exeBase) return false;
    g_host   = host;
    g_selfId = selfId;
    g_shuttingDown.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        g_hasBaseline = false;
        g_logEvents   = false;
        g_sessionGen  = 0ULL;
        g_tickCount   = 0ULL;
        g_state       = MhwWorldState{};
        g_state.structSize = sizeof(MhwWorldState);
        g_popupHead   = 0U;
        g_popupTail   = 0U;
        g_popupCount  = 0U;
    }
    g_popupsDelivered.store(0ULL, std::memory_order_relaxed);

    MhwRng_RegisterPluginInfoBi(
        host, selfId,
        u8"世界上下文与事件服务",
        "World Context & Event Service",
        u8"提供共享场景/任务/时间/天气状态、事件总线与主线程弹窗服务",
        "Shared scene/quest/stage/time/weather state, event bus & HUD popup service");

    g_tickHdl = MhwRng_HostRegisterMainThreadTick(host, selfId, &OnMainThreadTick, nullptr);
    if (g_tickHdl == kMhwRngInvalidHandle) {
        g_host   = nullptr;
        g_selfId = 0U;
        return false;
    }

    if (!MhwRng_HostRegisterService(host, selfId, kMhwWorldServiceName, kMhwWorldServiceVersion, &g_worldApi)) {
        MhwRng_HostUnregisterMainThreadTick(host, selfId, g_tickHdl);
        g_tickHdl = kMhwRngInvalidHandle;
        g_host    = nullptr;
        g_selfId  = 0U;
        return false;
    }

    MhwRng_RegisterSubcommandBi(
        host, selfId, "world",
        "$rng world [status|events on|off|popup <text>|help]",
        u8"查看实时世界状态、开关事件日志或发送游戏右侧弹窗",
        "Inspect live world state, toggle transition logs, or show HUD popup",
        &Cmd_World, nullptr);
    MhwRng_RegisterSubcommandCompleter(host, selfId, "world", &Complete_World, nullptr);
    return true;
}

MHW_RNG_PLUGIN_EXPORT void MhwRngPlugin_Shutdown() {
    g_shuttingDown.store(true, std::memory_order_release);
    if (g_host) {
        MhwRng_HostUnregisterService(g_host, g_selfId, kMhwWorldServiceName);
        MhwRng_HostUnregisterMainThreadTick(g_host, g_selfId, g_tickHdl);
    }
    g_tickHdl = kMhwRngInvalidHandle;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        g_hasBaseline = false;
        g_popupHead   = 0U;
        g_popupTail   = 0U;
        g_popupCount  = 0U;
    }
    g_host   = nullptr;
    g_selfId = 0U;
}
