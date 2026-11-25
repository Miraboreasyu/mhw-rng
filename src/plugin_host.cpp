#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#include "plugin_host.h"
#include "log_config.h"
#include "peer_beacon.h"
#include "rng_engine.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace plugin_host {
namespace {

struct LocaleTextOwned {
    std::string lang;
    std::string text;
};

struct RegisteredSubcmd {
    MhwRngPluginId               ownerId = 0;
    std::string                  subcmdLower;
    std::string                  usageExample;
    std::string                  helpCn;
    std::vector<LocaleTextOwned> helpI18n;
    MhwRngSubcommandFn           fn                = nullptr;
    void*                        userData          = nullptr;
    MhwRngSubcommandCompleteFn   completer         = nullptr;
    void*                        completerUserData = nullptr;
};

struct RegisteredSiteMeta {
    MhwRngPluginId               ownerId = 0;
    std::uint32_t                rva     = 0;
    std::string                  alias;
    std::string                  descUtf8;
    std::vector<LocaleTextOwned> descI18n;
    MhwRngValueFormatterFn       formatter = nullptr;
    void*                        userData  = nullptr;
};

struct FallbackResolver {
    MhwRngPluginId       ownerId  = 0;
    MhwRngMetaResolverFn fn       = nullptr;
    void*                userData = nullptr;
};

struct RegisteredLifecycleHook {
    MhwRngPluginId    ownerId  = 0;
    MhwRngHookHandle  handle   = 0;
    MhwRngLifecycleFn fn       = nullptr;
    void*             userData = nullptr;
};

struct RegisteredMainThreadTick {
    MhwRngPluginId         ownerId  = 0;
    MhwRngHookHandle       handle   = 0;
    MhwRngMainThreadTickFn fn       = nullptr;
    void*                  userData = nullptr;
};

struct RegisteredEventSub {
    MhwRngPluginId      ownerId        = 0;
    MhwRngHookHandle    handle         = 0;
    bool                wildcardAll    = false;
    bool                prefixWildcard = false;
    std::string         patternLower;
    std::string         prefixLower;
    MhwRngCustomEventFn fn             = nullptr;
    void*               userData       = nullptr;
};

constexpr std::size_t kMaxServices = 64U;

struct ServiceSlot {
    bool                       occupied = false;
    bool                       draining = false;
    std::atomic<std::uint32_t> generation{1U};
    std::uint32_t              version  = 0U;
    MhwRngPluginId             ownerId  = 0U;
    char                       name[64]{};
    const void*                apiTable = nullptr;
    std::atomic<std::uint32_t> inFlightRefs{0U};
};

struct LoadedPlugin {
    MhwRngPluginId               id = 0;
    std::string                  shortName;
    std::string                  shortNameLower;
    std::string                  displayName;
    std::vector<LocaleTextOwned> displayNamesI18n;
    std::string                  version;
    std::string                  description;
    std::vector<LocaleTextOwned> descriptionsI18n;
    std::string                  sourcePath;
    std::string                  fileName;
    std::string                  shadowPath;
    HMODULE                      module     = nullptr;
    MhwRngPluginShutdownFn       shutdownFn = nullptr;
};

// Two-tier lock hierarchy (prevents quiescence deadlocks even if a plugin calls Host APIs inside `OnSlotDraw`):
//   1. `g_lifecycleMx`: Serializes Load / Unload / ReloadAll operations.
//   2. `g_registryMx` : Leaf lock protecting in-memory metadata & command vectors (`g_plugins`, `g_subcmds`,
//                       `g_siteMetas`, `g_fallbackResolvers`). NEVER held across `DetachAllHooksForPluginAndWait`,
//                       plugin callbacks, `LoadLibrary`, or `FreeLibrary`!
std::recursive_mutex             g_lifecycleMx;
std::mutex                       g_registryMx;
std::atomic<std::uint32_t>       g_inFlightHostCallbacks{0U};

struct InFlightHostGuard {
    bool armed = false;
    void Arm() noexcept {
        if (!armed) {
            g_inFlightHostCallbacks.fetch_add(1U, std::memory_order_seq_cst);
            armed = true;
        }
    }
    ~InFlightHostGuard() noexcept {
        if (armed) {
            g_inFlightHostCallbacks.fetch_sub(1U, std::memory_order_acq_rel);
        }
    }
};

std::string                      g_subPluginDir;
std::string                      g_hotloadDir;
ChatPrintSinkFn                  g_chatSink      = nullptr;
AsyncChatSinkFn                  g_asyncChatSink = nullptr;
MhwRngHostApi                    g_hostApi{};
MhwRngPluginId                   g_nextPluginId     = 1U;
std::uint32_t                    g_nextLifecycleHdl = 1U;
std::uint32_t                    g_nextTickHdl      = 1U;
std::uint32_t                    g_nextEventSubHdl  = 1U;
std::uint32_t                    g_shadowGen        = 1U;
std::atomic<std::uint64_t>       g_mainTickSeq{0ULL};
std::atomic<std::uint64_t>       g_customEventSeq{0ULL};

// Static lock-free ring of plugin shortNames so `GetPluginShortName()` never returns dangling heap pointers:
constexpr std::size_t            kShortNameSlotCount = 512U;
char                             g_shortNameCache[kShortNameSlotCount][32]{};

std::vector<LoadedPlugin>             g_plugins;
std::vector<RegisteredSubcmd>         g_subcmds;
std::vector<RegisteredSiteMeta>       g_siteMetas;
std::vector<FallbackResolver>         g_fallbackResolvers;
std::vector<RegisteredLifecycleHook>  g_lifecycleHooks;
std::vector<RegisteredMainThreadTick> g_mainTicks;
std::vector<RegisteredEventSub>       g_eventSubs;
ServiceSlot                           g_services[kMaxServices]{};

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::vector<LocaleTextOwned> CopyLocaleTable(const MhwRngLocaleText* entries, std::size_t count) {
    std::vector<LocaleTextOwned> out;
    if (!entries || count == 0U) return out;
    out.reserve(count);
    for (std::size_t i = 0U; i < count; ++i) {
        if (!entries[i].lang || entries[i].lang[0] == '\0' || !entries[i].text || entries[i].text[0] == '\0') {
            continue;
        }
        char primary[12]{};
        MhwRng_ExtractCanonicalPrimary(entries[i].lang, primary);
        if (primary[0] != '\0') {
            out.push_back(LocaleTextOwned{primary, entries[i].text});
        }
    }
    return out;
}

std::string ResolveOwnedLocale(const std::vector<LocaleTextOwned>& entries, const std::string& fallback) {
    if (!entries.empty()) {
        const char* cur = app::GetLanguage();
        for (const auto& e : entries) {
            if (!e.text.empty() && MhwRng_LangMatchesPrimary(e.lang.c_str(), cur)) {
                return e.text;
            }
        }
        if (!MhwRng_LangMatchesPrimary(cur, "en")) {
            for (const auto& e : entries) {
                if (!e.text.empty() && MhwRng_LangMatchesPrimary(e.lang.c_str(), "en")) {
                    return e.text;
                }
            }
        }
        if (!MhwRng_LangMatchesPrimary(cur, "zh")) {
            for (const auto& e : entries) {
                if (!e.text.empty() && MhwRng_LangMatchesPrimary(e.lang.c_str(), "zh")) {
                    return e.text;
                }
            }
        }
        for (const auto& e : entries) {
            if (!e.text.empty()) {
                return e.text;
            }
        }
    }
    return fallback;
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

std::string DeriveShortNameFromFilename(const std::string& fileName) {
    std::string s = LowerAscii(fileName);
    if (s.size() > 4U && s.substr(s.size() - 4U) == ".dll") {
        s.resize(s.size() - 4U);
    }
    if (s.rfind("mhw_rng_", 0) == 0 && s.size() > 8U) {
        s = s.substr(8U);
    } else if (s.rfind("rng_", 0) == 0 && s.size() > 4U) {
        s = s.substr(4U);
    } else if (s.rfind("mhw_", 0) == 0 && s.size() > 4U) {
        s = s.substr(4U);
    }
    return s;
}

void CleanupStaleShadowFiles() {
    if (g_hotloadDir.empty()) return;
    const std::string pattern = g_hotloadDir + "\\*.dll";
    WIN32_FIND_DATAA fd{};
    HANDLE hFind = ::FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            const std::string full = g_hotloadDir + "\\" + fd.cFileName;
            ::DeleteFileA(full.c_str()); // Silently succeeds if unmapped, ignored if still mapped
        }
    } while (::FindNextFileA(hFind, &fd));
    ::FindClose(hFind);
}

void InvokePluginShutdownSEH(MhwRngPluginShutdownFn fn) noexcept {
    if (!fn) return;
    __try {
        fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool InvokePluginInitSEH(MhwRngPluginInitFn fn, const MhwRngHostApi* api, MhwRngPluginId id) noexcept {
    __try {
        return fn(api, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const MhwRngPluginDescriptor* InvokePluginGetInfoSEH(MhwRngPluginGetInfoFn fn) noexcept {
    __try {
        return fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool InvokeSubcommandSEH(MhwRngSubcommandFn fn,
                         int argc,
                         const char* const* argv,
                         const char* rawArgs,
                         void* userData) noexcept {
    __try {
        fn(argc, argv, rawArgs, userData);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeCompleterSEH(MhwRngSubcommandCompleteFn fn,
                        const char* subcmd,
                        int argc,
                        const char* const* argv,
                        const char* prefix,
                        MhwRngCompletionSinkFn addCand,
                        void* sinkCtx,
                        void* userData) noexcept {
    __try {
        fn(subcmd, argc, argv, prefix, addCand, sinkCtx, userData);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeFormatterSEH(MhwRngValueFormatterFn fn,
                        const MhwRngSlotSnapshot* snap,
                        char* outBuf,
                        std::size_t bufCap,
                        void* userData) noexcept {
    __try {
        return fn(snap, outBuf, bufCap, userData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeResolverSEH(MhwRngMetaResolverFn fn,
                       std::uint32_t rva,
                       MhwRngSiteMeta* outMeta,
                       void* userData) noexcept {
    __try {
        return fn(rva, outMeta, userData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void InvokeLifecycleSEH(MhwRngLifecycleFn fn,
                        const MhwRngEventInfo* info,
                        void* userData) noexcept {
    __try {
        fn(info, userData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void InvokeMainTickSEH(MhwRngMainThreadTickFn fn,
                       std::uint64_t tickSeq,
                       void* userData) noexcept {
    __try {
        fn(tickSeq, userData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void InvokeCustomEventSEH(MhwRngCustomEventFn fn,
                          const MhwRngCustomEvent* ev,
                          void* userData) noexcept {
    __try {
        fn(ev, userData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::uint32_t EmitEventInternal(MhwRngPluginId selfId,
                                const char* topic,
                                const void* payload,
                                std::uint32_t payloadSize) noexcept {
    if (!topic || topic[0] == '\0') return 0U;
    thread_local int tls_emitDepth = 0;
    if (tls_emitDepth >= 8) return 0U;
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& v) noexcept : d(v) { ++d; }
        ~DepthGuard() noexcept { --d; }
    } depthGuard(tls_emitDepth);

    const std::string topicLow = LowerAscii(topic);
    InFlightHostGuard guard;
    std::vector<RegisteredEventSub> matched;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& s : g_eventSubs) {
            bool hit = false;
            if (s.wildcardAll) {
                hit = true;
            } else if (s.prefixWildcard) {
                hit = (topicLow.rfind(s.prefixLower, 0) == 0);
            } else {
                hit = (topicLow == s.patternLower);
            }
            if (hit && s.fn) {
                matched.push_back(s);
            }
        }
        if (!matched.empty()) {
            guard.Arm();
        }
    }
    if (matched.empty()) return 0U;

    MhwRngCustomEvent ev{};
    ev.structSize  = sizeof(MhwRngCustomEvent);
    ev.payloadSize = payloadSize;
    ev.topic       = topicLow.c_str();
    ev.senderId    = selfId;
    ev.reserved    = 0U;
    ev.seq         = g_customEventSeq.fetch_add(1U, std::memory_order_relaxed) + 1ULL;
    ev.payload     = payload;

    std::uint32_t delivered = 0U;
    for (const auto& s : matched) {
        InvokeCustomEventSEH(s.fn, &ev, s.userData);
        ++delivered;
    }
    return delivered;
}

void DrainAndClearServicesForPlugin(MhwRngPluginId targetId, const char* singleNameLowerOrNull) {
    struct DrainedEntry {
        std::size_t               idx = 0U;
        MhwRngServiceEventPayload evPayload{};
    };
    std::vector<DrainedEntry> toDrain;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (std::size_t i = 0U; i < kMaxServices; ++i) {
            ServiceSlot& s = g_services[i];
            if (!s.occupied || s.draining) continue;
            if (s.ownerId != targetId) continue;
            if (singleNameLowerOrNull && std::strcmp(s.name, singleNameLowerOrNull) != 0) continue;
            s.draining = true;
            DrainedEntry de{};
            de.idx                   = i;
            de.evPayload.structSize  = sizeof(MhwRngServiceEventPayload);
            de.evPayload.version     = s.version;
            de.evPayload.ownerId     = s.ownerId;
            CopyTrunc(de.evPayload.name, sizeof(de.evPayload.name), s.name);
            toDrain.push_back(de);
        }
    }
    if (toDrain.empty()) return;

    // Spin-wait outside `g_registryMx` until all active `MhwRngServiceGuard` / `AcquireService` leases finish!
    for (const auto& de : toDrain) {
        while (g_services[de.idx].inFlightRefs.load(std::memory_order_seq_cst) != 0U) {
            _mm_pause();
        }
    }

    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& de : toDrain) {
            ServiceSlot& s = g_services[de.idx];
            s.occupied = false;
            s.draining = false;
            s.version  = 0U;
            s.ownerId  = 0U;
            s.apiTable = nullptr;
            s.name[0]  = '\0';
        }
    }

    for (const auto& de : toDrain) {
        (void)EmitEventInternal(targetId,
                                "kernel.service.unregistered",
                                &de.evPayload,
                                static_cast<std::uint32_t>(sizeof(de.evPayload)));
    }
}

void BroadcastLifecycleEvent(const MhwRngEventInfo* info) noexcept {
    if (!info) return;
    InFlightHostGuard guard;
    std::vector<RegisteredLifecycleHook> snapshot;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        if (g_lifecycleHooks.empty()) return;
        snapshot = g_lifecycleHooks;
        guard.Arm();
    }
    for (const auto& h : snapshot) {
        if (h.fn) {
            InvokeLifecycleSEH(h.fn, info, h.userData);
        }
    }
}

void BroadcastLanguageChanged(std::uint32_t prevLangId, std::uint32_t) noexcept {
    MhwRngEventInfo info{};
    info.eventKind  = kMhwRngEventLanguageChanged;
    info.engineMode = MhwRng_GetEngineMode();
    info.masterSeed = MhwRng_GetMasterSeed();
    info.epoch      = MhwRng_GetEpoch();
    info.prevValue  = prevLangId;
    BroadcastLifecycleEvent(&info);
}

// Requires `g_lifecycleMx` held; NEVER holds `g_registryMx` across `DetachAllHooksForPluginAndWait`!
void UnloadByIdLifecycleLocked(MhwRngPluginId targetId) {
    LoadedPlugin p{};
    bool found = false;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (std::size_t i = 0; i < g_plugins.size(); ++i) {
            if (g_plugins[i].id == targetId) {
                p = std::move(g_plugins[i]);
                g_plugins.erase(g_plugins.begin() + static_cast<std::ptrdiff_t>(i));
                found = true;
                break;
            }
        }
    }
    if (!found) return;

    // 1. Detach all synchronous RNG slot callbacks and wait for in-flight calls to drain (WITHOUT `g_registryMx`!)
    rng::DetachAllHooksForPluginAndWait(p.id);

    // 2. Drain any active service leases (`AcquireService` / `MhwRngServiceGuard`) before removing the DLL
    DrainAndClearServicesForPlugin(p.id, nullptr);

    // 3. Remove registered subcommands, metadata, fallback resolvers, lifecycle hooks, main-thread ticks, and event subs
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        g_subcmds.erase(
            std::remove_if(g_subcmds.begin(), g_subcmds.end(),
                           [&](const RegisteredSubcmd& c) { return c.ownerId == p.id; }),
            g_subcmds.end());
        g_siteMetas.erase(
            std::remove_if(g_siteMetas.begin(), g_siteMetas.end(),
                           [&](const RegisteredSiteMeta& m) { return m.ownerId == p.id; }),
            g_siteMetas.end());
        g_fallbackResolvers.erase(
            std::remove_if(g_fallbackResolvers.begin(), g_fallbackResolvers.end(),
                           [&](const FallbackResolver& r) { return r.ownerId == p.id; }),
            g_fallbackResolvers.end());
        g_lifecycleHooks.erase(
            std::remove_if(g_lifecycleHooks.begin(), g_lifecycleHooks.end(),
                           [&](const RegisteredLifecycleHook& l) { return l.ownerId == p.id; }),
            g_lifecycleHooks.end());
        g_mainTicks.erase(
            std::remove_if(g_mainTicks.begin(), g_mainTicks.end(),
                           [&](const RegisteredMainThreadTick& t) { return t.ownerId == p.id; }),
            g_mainTicks.end());
        g_eventSubs.erase(
            std::remove_if(g_eventSubs.begin(), g_eventSubs.end(),
                           [&](const RegisteredEventSub& s) { return s.ownerId == p.id; }),
            g_eventSubs.end());
    }
    // Wait for any in-flight non-hot-path callbacks (Lifecycle / MainThreadTick / CustomEvent / QuerySiteMeta / FormatSlotCustom)
    // before invoking Shutdown or FreeLibrary on the shadow DLL.
    while (g_inFlightHostCallbacks.load(std::memory_order_seq_cst) != 0U) {
        _mm_pause();
    }
    rng::ClearRulesByOwner(p.id);
    peer_beacon::ClearTainted(p.id);

    // 3. Notify plugin & unmap shadow DLL
    InvokePluginShutdownSEH(p.shutdownFn);
    if (p.module) {
        ::FreeLibrary(p.module);
    }
    if (!p.shadowPath.empty()) {
        ::DeleteFileA(p.shadowPath.c_str());
    }
    app::Log("plugin_host: unloaded plugin '%s' (id=%u)", p.shortName.c_str(), p.id);
}

bool LoadDllPathLifecycleLocked(const std::string& fullDllPath,
                                const std::string& fileName,
                                std::string& outMsg) {
    ::CreateDirectoryA(g_subPluginDir.c_str(), nullptr);
    ::CreateDirectoryA(g_hotloadDir.c_str(), nullptr);
    ::SetFileAttributesA(g_hotloadDir.c_str(), FILE_ATTRIBUTE_HIDDEN);
    CleanupStaleShadowFiles();

    const bool en = app::IsEnglish();
    const std::string baseShort = DeriveShortNameFromFilename(fileName);
    char shadowName[128]{};
    std::snprintf(shadowName, sizeof(shadowName), "%s.%u.%lu.dll",
                  baseShort.c_str(), g_shadowGen++, ::GetCurrentProcessId());
    const std::string shadowPath = g_hotloadDir + "\\" + shadowName;

    if (!::CopyFileA(fullDllPath.c_str(), shadowPath.c_str(), FALSE)) {
        outMsg = (en ? "Failed to read plugin file: " : u8"无法读取插件文件: ") + fileName;
        return false;
    }

    HMODULE mod = ::LoadLibraryA(shadowPath.c_str());
    if (!mod) {
        ::DeleteFileA(shadowPath.c_str());
        outMsg = (en ? "LoadLibrary failed: " : u8"LoadLibrary 失败: ") + fileName;
        return false;
    }

    auto fnGetInfo  = reinterpret_cast<MhwRngPluginGetInfoFn>(::GetProcAddress(mod, "MhwRngPlugin_GetInfo"));
    auto fnInit     = reinterpret_cast<MhwRngPluginInitFn>(::GetProcAddress(mod, "MhwRngPlugin_Init"));
    auto fnShutdown = reinterpret_cast<MhwRngPluginShutdownFn>(::GetProcAddress(mod, "MhwRngPlugin_Shutdown"));

    if (!fnGetInfo || !fnInit) {
        ::FreeLibrary(mod);
        ::DeleteFileA(shadowPath.c_str());
        outMsg = (en ? "Not a valid RNG plugin (missing exports): " : u8"非标准 RNG 插件(缺少导出入口): ") + fileName;
        return false;
    }

    const MhwRngPluginDescriptor* desc = InvokePluginGetInfoSEH(fnGetInfo);
    if (!desc || desc->targetApiVersion != kMhwRngApiVersion || !desc->shortName || desc->shortName[0] == '\0') {
        ::FreeLibrary(mod);
        ::DeleteFileA(shadowPath.c_str());
        outMsg = (en ? "Plugin API version mismatch: " : u8"插件 API 版本不匹配: ") + fileName;
        return false;
    }

    if (!ValidateDeclaredShortName(desc->shortName)) {
        ::FreeLibrary(mod);
        ::DeleteFileA(shadowPath.c_str());
        outMsg = (en ? "Invalid plugin shortName (must be 1..24 lowercase [a-z_], not 'kernel'): "
                     : u8"插件 shortName 无效 (仅允许 1..24 位小写字母和下划线，且不能为 kernel): ") +
                 std::string(desc->shortName);
        return false;
    }

    const std::string fullLow = LowerAscii(fullDllPath);
    const std::string fileLow = LowerAscii(fileName);
    MhwRngPluginId sameFileId = 0U;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& p : g_plugins) {
            if (LowerAscii(p.sourcePath) == fullLow || LowerAscii(p.fileName) == fileLow) {
                sameFileId = p.id;
                break;
            }
        }
    }
    if (sameFileId != 0U) {
        UnloadByIdLifecycleLocked(sameFileId);
    }

    std::string assignedShort;
    MhwRngPluginId newId = 0U;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        std::vector<std::string> existingLower;
        existingLower.reserve(g_plugins.size());
        for (const auto& p : g_plugins) {
            existingLower.push_back(p.shortNameLower);
        }
        assignedShort = AllocateUniqueShortName(desc->shortName, existingLower);
        newId = g_nextPluginId++;
        CopyTrunc(g_shortNameCache[newId % kShortNameSlotCount],
                  sizeof(g_shortNameCache[0]),
                  assignedShort.c_str());

        LoadedPlugin lp{};
        lp.id             = newId;
        lp.shortName      = assignedShort;
        lp.shortNameLower = assignedShort;
        lp.displayName    = desc->displayName ? desc->displayName : assignedShort;
        lp.version        = desc->version ? desc->version : "1.0";
        lp.description    = desc->description ? desc->description : "";
        lp.sourcePath     = fullDllPath;
        lp.fileName       = fileName;
        lp.shadowPath     = shadowPath;
        lp.module         = mod;
        lp.shutdownFn     = fnShutdown;
        g_plugins.push_back(std::move(lp));
    }

    if (!InvokePluginInitSEH(fnInit, &g_hostApi, newId)) {
        UnloadByIdLifecycleLocked(newId);
        outMsg = (en ? "Plugin Init returned false: " : u8"插件初始化返回失败: ") + assignedShort;
        return false;
    }

    std::size_t cmdCnt = 0;
    std::string dispName = assignedShort;
    std::string versionStr = "1.0";
    std::string finalShort = assignedShort;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& c : g_subcmds) {
            if (c.ownerId == newId) ++cmdCnt;
        }
        for (const auto& p : g_plugins) {
            if (p.id == newId) {
                dispName   = ResolveOwnedLocale(p.displayNamesI18n, p.displayName);
                versionStr = p.version;
                finalShort = p.shortName;
                break;
            }
        }
    }
    app::Log("plugin_host: loaded '%s' v%s (%s, id=%u, cmds=%zu)",
             finalShort.c_str(), versionStr.c_str(), fileName.c_str(), newId, cmdCnt);

    char okBuf[192]{};
    if (en) {
        std::snprintf(okBuf, sizeof(okBuf), "%s v%s (%s, %zu cmds)",
                      finalShort.c_str(), versionStr.c_str(), dispName.c_str(), cmdCnt);
    } else {
        std::snprintf(okBuf, sizeof(okBuf), u8"%s v%s (%s, 注册%zu条指令)",
                      finalShort.c_str(), versionStr.c_str(), dispName.c_str(), cmdCnt);
    }
    outMsg = okBuf;
    return true;
}

// ============================================================================
//  Host API Thunks Exposed to Plugins
// ============================================================================

const char* __fastcall Host_GetRootPluginDir() {
    return app::DllDirectory().c_str();
}

const char* __fastcall Host_GetSubPluginDir() {
    return g_subPluginDir.c_str();
}

void __cdecl Host_Log(MhwRngPluginId selfId, const char* fmt, ...) {
    if (!fmt) return;
    char buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    app::LogPlugin(GetPluginShortName(selfId), buf);
}

void __fastcall Host_ChatPrint(const char* utf8Text) {
    if (!utf8Text || utf8Text[0] == '\0') return;
    app::Log("ui: %s", utf8Text);
    if (g_chatSink) g_chatSink(utf8Text);
}

void __cdecl Host_ChatPrintf(const char* fmt, ...) {
    if (!fmt) return;
    char buf[256]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Host_ChatPrint(buf);
}

void __fastcall Host_ChatPrintAsync(const char* utf8Text) {
    if (!utf8Text || utf8Text[0] == '\0') return;
    app::Log("ui-async: %s", utf8Text);
    if (g_asyncChatSink) {
        g_asyncChatSink(utf8Text);
    } else if (g_chatSink) {
        g_chatSink(utf8Text);
    }
}

void __cdecl Host_ChatPrintfAsync(const char* fmt, ...) {
    if (!fmt) return;
    char buf[256]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Host_ChatPrintAsync(buf);
}

MhwRngHookHandle __fastcall Host_RegisterLifecycleHook(MhwRngPluginId selfId,
                                                       MhwRngLifecycleFn fn,
                                                       void* userData) {
    if (!fn) return kMhwRngInvalidHandle;
    std::lock_guard<std::mutex> rlk(g_registryMx);
    const MhwRngHookHandle h = g_nextLifecycleHdl++;
    if (g_nextLifecycleHdl == 0U) g_nextLifecycleHdl = 1U;
    g_lifecycleHooks.push_back(RegisteredLifecycleHook{selfId, h, fn, userData});
    return h;
}

void __fastcall Host_UnregisterLifecycleHook(MhwRngPluginId selfId, MhwRngHookHandle handle) {
    if (handle == kMhwRngInvalidHandle) return;
    std::lock_guard<std::mutex> rlk(g_registryMx);
    g_lifecycleHooks.erase(
        std::remove_if(g_lifecycleHooks.begin(), g_lifecycleHooks.end(),
                       [&](const RegisteredLifecycleHook& l) {
                           return l.handle == handle && (selfId == 0U || l.ownerId == selfId);
                       }),
        g_lifecycleHooks.end());
}

bool __fastcall Host_RegisterSubcommand(MhwRngPluginId selfId,
                                        const char* subcmd,
                                        const char* usageExample,
                                        const char* helpCn,
                                        MhwRngSubcommandFn fn,
                                        void* userData) {
    if (!subcmd || subcmd[0] == '\0' || !fn) return false;
    const std::string low = LowerAscii(subcmd);
    std::lock_guard<std::mutex> rlk(g_registryMx);
    for (auto& existing : g_subcmds) {
        if (existing.subcmdLower == low) {
            existing.ownerId           = selfId;
            existing.usageExample      = usageExample ? usageExample : "";
            existing.helpCn            = helpCn ? helpCn : "";
            existing.helpI18n.clear();
            existing.fn                = fn;
            existing.userData          = userData;
            existing.completer         = nullptr;
            existing.completerUserData = nullptr;
            return true;
        }
    }
    g_subcmds.push_back(RegisteredSubcmd{
        selfId,
        low,
        usageExample ? usageExample : "",
        helpCn ? helpCn : "",
        {},
        fn,
        userData,
        nullptr,
        nullptr
    });
    return true;
}

bool __fastcall Host_RegisterSiteMeta(MhwRngPluginId selfId,
                                      std::uint32_t rva,
                                      const char* shortAlias,
                                      const char* descriptionUtf8,
                                      MhwRngValueFormatterFn formatterOrNull,
                                      void* userData) {
    const std::uint32_t norm = rng::NormalizeUserSiteAddress(rva);
    if (norm == 0U) return false;
    std::lock_guard<std::mutex> rlk(g_registryMx);
    for (auto& m : g_siteMetas) {
        if (m.rva == norm) {
            m.ownerId   = selfId;
            m.alias     = shortAlias ? shortAlias : "";
            m.descUtf8  = descriptionUtf8 ? descriptionUtf8 : "";
            m.descI18n.clear();
            m.formatter = formatterOrNull;
            m.userData  = userData;
            return true;
        }
    }
    g_siteMetas.push_back(RegisteredSiteMeta{
        selfId,
        norm,
        shortAlias ? shortAlias : "",
        descriptionUtf8 ? descriptionUtf8 : "",
        {},
        formatterOrNull,
        userData
    });
    return true;
}

const char* __fastcall Host_SelectText(const MhwRngLocaleText* entries,
                                       std::size_t count,
                                       const char* fallbackText) {
    return MhwRng_SelectTextFallback(app::GetLanguage(), entries, count, fallbackText);
}

bool __fastcall Host_RegisterPluginInfoI18n(MhwRngPluginId selfId,
                                            const MhwRngLocaleText* displayNames,
                                            std::size_t displayCount,
                                            const MhwRngLocaleText* descriptions,
                                            std::size_t descCount) {
    if (selfId == 0U) return false;
    auto namesVec = CopyLocaleTable(displayNames, displayCount);
    auto descVec  = CopyLocaleTable(descriptions, descCount);
    std::lock_guard<std::mutex> rlk(g_registryMx);
    for (auto& p : g_plugins) {
        if (p.id == selfId) {
            if (!namesVec.empty()) p.displayNamesI18n = std::move(namesVec);
            if (!descVec.empty())  p.descriptionsI18n = std::move(descVec);
            return true;
        }
    }
    return false;
}

bool __fastcall Host_RegisterSubcommandI18n(MhwRngPluginId selfId,
                                            const char* subcmd,
                                            const char* usageExample,
                                            const MhwRngLocaleText* helpTexts,
                                            std::size_t helpCount,
                                            MhwRngSubcommandFn fn,
                                            void* userData) {
    if (!subcmd || subcmd[0] == '\0' || !fn) return false;
    const std::string low = LowerAscii(subcmd);
    auto table = CopyLocaleTable(helpTexts, helpCount);
    const std::string fallbackHelp = table.empty() ? std::string() : ResolveOwnedLocale(table, "");
    std::lock_guard<std::mutex> rlk(g_registryMx);
    for (auto& existing : g_subcmds) {
        if (existing.subcmdLower == low) {
            existing.ownerId           = selfId;
            existing.usageExample      = usageExample ? usageExample : "";
            existing.helpCn            = fallbackHelp;
            existing.helpI18n          = std::move(table);
            existing.fn                = fn;
            existing.userData          = userData;
            existing.completer         = nullptr;
            existing.completerUserData = nullptr;
            return true;
        }
    }
    g_subcmds.push_back(RegisteredSubcmd{
        selfId,
        low,
        usageExample ? usageExample : "",
        fallbackHelp,
        std::move(table),
        fn,
        userData,
        nullptr,
        nullptr
    });
    return true;
}

bool __fastcall Host_RegisterSubcommandCompleter(MhwRngPluginId selfId,
                                                 const char* subcmd,
                                                 MhwRngSubcommandCompleteFn completer,
                                                 void* userData) {
    if (!subcmd || subcmd[0] == '\0' || !completer) return false;
    const std::string low = LowerAscii(subcmd);
    std::lock_guard<std::mutex> rlk(g_registryMx);
    for (auto& existing : g_subcmds) {
        if (existing.subcmdLower == low && (selfId == 0U || existing.ownerId == selfId)) {
            existing.completer         = completer;
            existing.completerUserData = userData;
            return true;
        }
    }
    return false;
}

bool __fastcall Host_RegisterSiteMetaI18n(MhwRngPluginId selfId,
                                          std::uint32_t rva,
                                          const char* shortAlias,
                                          const MhwRngLocaleText* descriptions,
                                          std::size_t descCount,
                                          MhwRngValueFormatterFn formatterOrNull,
                                          void* userData) {
    const std::uint32_t norm = rng::NormalizeUserSiteAddress(rva);
    if (norm == 0U) return false;
    auto table = CopyLocaleTable(descriptions, descCount);
    const std::string fallbackDesc = table.empty() ? std::string() : ResolveOwnedLocale(table, "");
    std::lock_guard<std::mutex> rlk(g_registryMx);
    for (auto& m : g_siteMetas) {
        if (m.rva == norm) {
            m.ownerId   = selfId;
            m.alias     = shortAlias ? shortAlias : "";
            m.descUtf8  = fallbackDesc;
            m.descI18n  = std::move(table);
            m.formatter = formatterOrNull;
            m.userData  = userData;
            return true;
        }
    }
    g_siteMetas.push_back(RegisteredSiteMeta{
        selfId,
        norm,
        shortAlias ? shortAlias : "",
        fallbackDesc,
        std::move(table),
        formatterOrNull,
        userData
    });
    return true;
}

bool __fastcall Host_SetFallbackMetaResolver(MhwRngPluginId selfId,
                                             MhwRngMetaResolverFn fn,
                                             void* userData) {
    if (!fn) return false;
    std::lock_guard<std::mutex> rlk(g_registryMx);
    for (auto& r : g_fallbackResolvers) {
        if (r.ownerId == selfId) {
            r.fn       = fn;
            r.userData = userData;
            return true;
        }
    }
    g_fallbackResolvers.push_back(FallbackResolver{selfId, fn, userData});
    return true;
}

MhwRngHookHandle __fastcall Host_RegisterMainThreadTick(MhwRngPluginId selfId,
                                                        MhwRngMainThreadTickFn fn,
                                                        void* userData) {
    if (!fn) return kMhwRngInvalidHandle;
    std::lock_guard<std::mutex> rlk(g_registryMx);
    const MhwRngHookHandle h = g_nextTickHdl++;
    if (g_nextTickHdl == 0U) g_nextTickHdl = 1U;
    g_mainTicks.push_back(RegisteredMainThreadTick{selfId, h, fn, userData});
    return h;
}

void __fastcall Host_UnregisterMainThreadTick(MhwRngPluginId selfId, MhwRngHookHandle handle) {
    if (handle == kMhwRngInvalidHandle) return;
    std::lock_guard<std::mutex> rlk(g_registryMx);
    g_mainTicks.erase(
        std::remove_if(g_mainTicks.begin(), g_mainTicks.end(),
                       [&](const RegisteredMainThreadTick& t) {
                           return t.handle == handle && (selfId == 0U || t.ownerId == selfId);
                       }),
        g_mainTicks.end());
}

MhwRngHookHandle __fastcall Host_SubscribeEvent(MhwRngPluginId selfId,
                                                const char* topicPattern,
                                                MhwRngCustomEventFn fn,
                                                void* userData) {
    if (!topicPattern || topicPattern[0] == '\0' || !fn) return kMhwRngInvalidHandle;
    std::string pat = LowerAscii(topicPattern);
    RegisteredEventSub sub{};
    sub.ownerId      = selfId;
    sub.patternLower = pat;
    sub.fn           = fn;
    sub.userData     = userData;
    if (pat == "*") {
        sub.wildcardAll = true;
    } else if (pat.size() >= 2U && pat[pat.size() - 1U] == '*' && pat[pat.size() - 2U] == '.') {
        sub.prefixWildcard = true;
        sub.prefixLower    = pat.substr(0U, pat.size() - 1U);
    }

    std::lock_guard<std::mutex> rlk(g_registryMx);
    const MhwRngHookHandle h = g_nextEventSubHdl++;
    if (g_nextEventSubHdl == 0U) g_nextEventSubHdl = 1U;
    sub.handle = h;
    g_eventSubs.push_back(std::move(sub));
    return h;
}

void __fastcall Host_UnsubscribeEvent(MhwRngPluginId selfId, MhwRngHookHandle handle) {
    if (handle == kMhwRngInvalidHandle) return;
    std::lock_guard<std::mutex> rlk(g_registryMx);
    g_eventSubs.erase(
        std::remove_if(g_eventSubs.begin(), g_eventSubs.end(),
                       [&](const RegisteredEventSub& s) {
                           return s.handle == handle && (selfId == 0U || s.ownerId == selfId);
                       }),
        g_eventSubs.end());
}

std::uint32_t __fastcall Host_EmitEvent(MhwRngPluginId selfId,
                                        const char* topic,
                                        const void* payload,
                                        std::uint32_t payloadSize) {
    return EmitEventInternal(selfId, topic, payload, payloadSize);
}

bool __fastcall Host_RegisterService(MhwRngPluginId selfId,
                                     const char* serviceName,
                                     std::uint32_t version,
                                     const void* apiTable) {
    if (!serviceName || serviceName[0] == '\0' || version == 0U || !apiTable) return false;
    const std::string low = LowerAscii(serviceName);
    if (low.size() >= sizeof(ServiceSlot::name)) return false;

    MhwRngServiceEventPayload evPayload{};
    evPayload.structSize = sizeof(MhwRngServiceEventPayload);
    evPayload.version    = version;
    evPayload.ownerId    = selfId;
    CopyTrunc(evPayload.name, sizeof(evPayload.name), low.c_str());

    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        std::size_t freeIdx = kMaxServices;
        for (std::size_t i = 0U; i < kMaxServices; ++i) {
            ServiceSlot& s = g_services[i];
            if (!s.occupied) {
                if (freeIdx == kMaxServices) freeIdx = i;
                continue;
            }
            if (std::strcmp(s.name, low.c_str()) == 0) {
                if (s.draining || s.ownerId != selfId) {
                    return false;
                }
                s.version  = version;
                s.apiTable = apiTable;
                goto registered_ok;
            }
        }
        if (freeIdx >= kMaxServices) return false;
        {
            ServiceSlot& s = g_services[freeIdx];
            std::uint32_t nextGen = s.generation.load(std::memory_order_relaxed) + 1U;
            if (nextGen == 0U) nextGen = 1U;
            s.generation.store(nextGen, std::memory_order_relaxed);
            s.occupied = true;
            s.draining = false;
            s.version  = version;
            s.ownerId  = selfId;
            CopyTrunc(s.name, sizeof(s.name), low.c_str());
            s.apiTable = apiTable;
            s.inFlightRefs.store(0U, std::memory_order_release);
        }
    }

registered_ok:
    (void)EmitEventInternal(selfId,
                            "kernel.service.registered",
                            &evPayload,
                            static_cast<std::uint32_t>(sizeof(evPayload)));
    return true;
}

void __fastcall Host_UnregisterService(MhwRngPluginId selfId, const char* serviceName) {
    if (!serviceName || serviceName[0] == '\0') return;
    const std::string low = LowerAscii(serviceName);
    DrainAndClearServicesForPlugin(selfId, low.c_str());
}

MhwRngServiceLease __fastcall Host_AcquireService(const char* serviceName,
                                                  std::uint32_t minVersion,
                                                  MhwRngServiceInfo* outInfo) {
    if (!serviceName || serviceName[0] == '\0' || !outInfo) return kMhwRngInvalidLease;
    if (outInfo->structSize != 0U && outInfo->structSize < sizeof(MhwRngServiceInfo)) return kMhwRngInvalidLease;
    const std::string low = LowerAscii(serviceName);

    std::lock_guard<std::mutex> rlk(g_registryMx);
    for (std::size_t i = 0U; i < kMaxServices; ++i) {
        ServiceSlot& s = g_services[i];
        if (!s.occupied || s.draining) continue;
        if (std::strcmp(s.name, low.c_str()) != 0) continue;
        if (s.version < minVersion || !s.apiTable) return kMhwRngInvalidLease;

        s.inFlightRefs.fetch_add(1U, std::memory_order_seq_cst);
        outInfo->structSize = sizeof(MhwRngServiceInfo);
        outInfo->version    = s.version;
        outInfo->ownerId    = s.ownerId;
        outInfo->reserved   = 0U;
        outInfo->name       = s.name;
        outInfo->apiTable   = s.apiTable;
        const std::uint32_t gen = s.generation.load(std::memory_order_acquire);
        return (static_cast<std::uint64_t>(gen) << 32) | static_cast<std::uint64_t>(i + 1U);
    }
    return kMhwRngInvalidLease;
}

void __fastcall Host_ReleaseService(MhwRngServiceLease lease) {
    if (lease == kMhwRngInvalidLease) return;
    const std::uint32_t idxPlus1 = static_cast<std::uint32_t>(lease & 0xFFFFFFFFULL);
    const std::uint32_t gen      = static_cast<std::uint32_t>(lease >> 32);
    if (idxPlus1 == 0U || idxPlus1 > kMaxServices) return;

    ServiceSlot& s = g_services[idxPlus1 - 1U];
    if (s.generation.load(std::memory_order_acquire) != gen) return;
    std::uint32_t cur = s.inFlightRefs.load(std::memory_order_acquire);
    while (cur > 0U && !s.inFlightRefs.compare_exchange_weak(
                           cur, cur - 1U, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
}

} // namespace

// ============================================================================
//  Public Plugin Host Implementation
// ============================================================================

void Init(std::uintptr_t exeBase,
          std::uintptr_t exeEnd,
          const std::string& subPluginDir,
          ChatPrintSinkFn chatSink,
          AsyncChatSinkFn asyncChatSink) {
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    g_subPluginDir  = subPluginDir;
    g_hotloadDir    = subPluginDir + "\\.hotload";
    g_chatSink      = chatSink;
    g_asyncChatSink = asyncChatSink;

    rng::SetLifecycleEventSink(&BroadcastLifecycleEvent);
    app::SetLanguageChangedSink(&BroadcastLanguageChanged);

    g_hostApi.apiVersion               = kMhwRngApiVersion;
    g_hostApi.structSize               = sizeof(MhwRngHostApi);
    g_hostApi.exeBase                  = exeBase;
    g_hostApi.exeEnd                   = exeEnd;

    g_hostApi.GetRootPluginDir         = &Host_GetRootPluginDir;
    g_hostApi.GetSubPluginDir          = &Host_GetSubPluginDir;
    g_hostApi.Log                      = &Host_Log;
    g_hostApi.ChatPrint                = &Host_ChatPrint;
    g_hostApi.ChatPrintf               = &Host_ChatPrintf;

    g_hostApi.SetTakeoverEnabled       = &MhwRng_SetTakeoverEnabled;
    g_hostApi.IsTakeoverEnabled        = &MhwRng_IsTakeoverEnabled;
    g_hostApi.SetMasterSeed            = &MhwRng_SetMasterSeed;
    g_hostApi.GetMasterSeed            = &MhwRng_GetMasterSeed;
    g_hostApi.ResetEpoch               = &MhwRng_ResetEpoch;
    g_hostApi.GetEpoch                 = &MhwRng_GetEpoch;
    g_hostApi.NormalizeSiteAddress     = &rng::NormalizeUserSiteAddress;

    g_hostApi.SetActiveCondition       = &rng::SetActiveCondition;
    g_hostApi.GetActiveCondition       = &rng::GetActiveCondition;
    g_hostApi.SetCondMasterDerived     = &rng::SetCondMasterDerived;
    g_hostApi.SetCondCustomSeed        = &rng::SetCondCustomSeed;
    g_hostApi.SetCondFixedValue        = &rng::SetCondFixedValue;
    g_hostApi.SetCondSequence          = &rng::SetCondSequence;
    g_hostApi.SetCondBypass            = &rng::SetCondBypass;
    g_hostApi.ClearSlotAllConditions   = &rng::ClearSlotAllConditions;
    g_hostApi.ClearPluginRules         = &rng::ClearRulesByOwner;
    g_hostApi.ClearAllRules            = &MhwRng_ClearAllSlotOverrides;

    g_hostApi.RegisterSlotHook         = &rng::RegisterSlotHook;
    g_hostApi.UnregisterSlotHook       = &rng::UnregisterSlotHook;

    g_hostApi.RegisterSubcommand       = &Host_RegisterSubcommand;
    g_hostApi.RegisterSiteMeta         = &Host_RegisterSiteMeta;
    g_hostApi.SetFallbackMetaResolver  = &Host_SetFallbackMetaResolver;
    g_hostApi.QuerySiteMeta            = &QuerySiteMeta;
    g_hostApi.FormatSlotCustom         = &FormatSlotCustom;
    g_hostApi.GetPluginShortName       = &GetPluginShortName;

    g_hostApi.GetCondSlotInfo          = &rng::GetCondSlotInfo;
    g_hostApi.GetActiveSlotCount       = &MhwRng_GetActiveSlotCount;
    g_hostApi.SnapshotActiveSlots      = &MhwRng_SnapshotActiveSlots;

    // v3.0 capabilities
    g_hostApi.ChatPrintAsync               = &Host_ChatPrintAsync;
    g_hostApi.ChatPrintfAsync              = &Host_ChatPrintfAsync;
    g_hostApi.SetEngineMode                = &MhwRng_SetEngineMode;
    g_hostApi.GetEngineMode                = &MhwRng_GetEngineMode;
    g_hostApi.RegisterSlotHookEx           = &rng::RegisterSlotHookEx;
    g_hostApi.UnregisterSlotHookHandle     = &rng::UnregisterSlotHookHandle;
    g_hostApi.RegisterGlobalObserver       = &rng::RegisterGlobalObserver;
    g_hostApi.UnregisterGlobalObserver     = &rng::UnregisterGlobalObserver;
    g_hostApi.RegisterLifecycleHook        = &Host_RegisterLifecycleHook;
    g_hostApi.UnregisterLifecycleHook      = &Host_UnregisterLifecycleHook;
    g_hostApi.RestoreSlotState             = &rng::RestoreSlotState;
    g_hostApi.CaptureStateBlob             = &rng::CaptureStateBlob;
    g_hostApi.RestoreStateBlob             = &rng::RestoreStateBlob;
    g_hostApi.ReleaseCondSlot              = &rng::ReleaseCondSlot;
    g_hostApi.SetInstancePolicy            = &rng::SetInstancePolicy;
    g_hostApi.GetInstancePolicy            = &rng::GetInstancePolicy;
    g_hostApi.RegisterTransparentWrapper   = &rng::RegisterTransparentWrapper;
    g_hostApi.UnregisterTransparentWrapper = &rng::UnregisterTransparentWrapper;

    // v3.1 extensions
    g_hostApi.GetLanguage                  = &MhwRng_GetLanguage;
    g_hostApi.ExecuteCommand               = &MhwRng_ExecuteCommand;

    // v3.2 extensions
    g_hostApi.CaptureCallerRvas            = &rng::CaptureCallerRvas;

    // v3.3 standardized i18n extensions
    g_hostApi.SelectText                   = &Host_SelectText;
    g_hostApi.RegisterPluginInfoI18n       = &Host_RegisterPluginInfoI18n;
    g_hostApi.RegisterSubcommandI18n       = &Host_RegisterSubcommandI18n;
    g_hostApi.RegisterSiteMetaI18n         = &Host_RegisterSiteMetaI18n;

    // v3.4 subcommand Tab-completion extension
    g_hostApi.RegisterSubcommandCompleter  = &Host_RegisterSubcommandCompleter;

    // v3.5 main-thread tick, event bus & hot-reload-safe service registry
    g_hostApi.RegisterMainThreadTick       = &Host_RegisterMainThreadTick;
    g_hostApi.UnregisterMainThreadTick     = &Host_UnregisterMainThreadTick;
    g_hostApi.SubscribeEvent               = &Host_SubscribeEvent;
    g_hostApi.UnsubscribeEvent             = &Host_UnsubscribeEvent;
    g_hostApi.EmitEvent                    = &Host_EmitEvent;
    g_hostApi.RegisterService              = &Host_RegisterService;
    g_hostApi.UnregisterService            = &Host_UnregisterService;
    g_hostApi.AcquireService               = &Host_AcquireService;
    g_hostApi.ReleaseService               = &Host_ReleaseService;
}

void ShutdownAll() {
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    for (;;) {
        MhwRngPluginId tailId = 0U;
        {
            std::lock_guard<std::mutex> rlk(g_registryMx);
            if (g_plugins.empty()) break;
            tailId = g_plugins.back().id;
        }
        UnloadByIdLifecycleLocked(tailId);
    }
    CleanupStaleShadowFiles();
}

std::size_t LoadAllFromDirectory() {
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    if (g_subPluginDir.empty()) return 0U;

    const std::string pattern = g_subPluginDir + "\\*.dll";
    WIN32_FIND_DATAA fd{};
    HANDLE hFind = ::FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0U;

    std::vector<std::string> dllNames;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            dllNames.emplace_back(fd.cFileName);
        }
    } while (::FindNextFileA(hFind, &fd));
    ::FindClose(hFind);

    std::sort(dllNames.begin(), dllNames.end());

    std::size_t loadedCount = 0U;
    for (const std::string& fn : dllNames) {
        std::string msg;
        if (LoadDllPathLifecycleLocked(g_subPluginDir + "\\" + fn, fn, msg)) {
            ++loadedCount;
        } else {
            app::Log("plugin_host: failed loading '%s': %s", fn.c_str(), msg.c_str());
        }
    }
    return loadedCount;
}

bool LoadOrReloadSingle(const std::string& nameOrFile, std::string& outMsg) {
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    const std::string q = LowerAscii(nameOrFile);

    std::string existingSource, existingFile;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& p : g_plugins) {
            if (p.shortNameLower == q || LowerAscii(p.fileName) == q) {
                existingSource = p.sourcePath;
                existingFile   = p.fileName;
                break;
            }
        }
    }
    if (!existingSource.empty()) {
        return LoadDllPathLifecycleLocked(existingSource, existingFile, outMsg);
    }

    const std::string cands[] = {
        nameOrFile,
        nameOrFile + ".dll",
        "rng_" + nameOrFile + ".dll",
        "mhw_rng_" + nameOrFile + ".dll",
        "mhw_" + nameOrFile + ".dll"
    };
    for (const std::string& fn : cands) {
        const std::string full = g_subPluginDir + "\\" + fn;
        const DWORD attr = ::GetFileAttributesA(full.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return LoadDllPathLifecycleLocked(full, fn, outMsg);
        }
    }

    outMsg = app::IsEnglish()
                 ? ("Plugin `" + nameOrFile + "` not found under nativePC\\plugins\\mhw_rng\\")
                 : (u8"在 nativePC\\plugins\\mhw_rng\\ 下找不到插件 `" + nameOrFile + "`");
    return false;
}

bool UnloadSingle(const std::string& nameOrFile, std::string& outMsg) {
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    const std::string q = LowerAscii(nameOrFile);
    MhwRngPluginId targetId = 0U;
    std::string    sname;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& p : g_plugins) {
            if (p.shortNameLower == q ||
                LowerAscii(p.fileName) == q ||
                DeriveShortNameFromFilename(p.fileName) == q) {
                targetId = p.id;
                sname    = p.shortName;
                break;
            }
        }
    }
    const bool en = app::IsEnglish();
    if (targetId != 0U) {
        UnloadByIdLifecycleLocked(targetId);
        outMsg = en ? ("Unloaded plugin `" + sname + "` and cleared its hooks/rules")
                    : (u8"已卸载插件 `" + sname + u8"` 并清理其全部钩子与锁定");
        return true;
    }
    outMsg = en ? ("Loaded plugin `" + nameOrFile + "` not found")
                : (u8"未找到已加载插件 `" + nameOrFile + "`");
    return false;
}

std::size_t ReloadAll(std::string& outMsg) {
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    ShutdownAll();
    const std::size_t n = LoadAllFromDirectory();
    char buf[128]{};
    if (app::IsEnglish()) {
        std::snprintf(buf, sizeof(buf), "Hot-reloaded %zu plugin(s)", n);
    } else {
        std::snprintf(buf, sizeof(buf), u8"已热重载全部 %zu 个插件", n);
    }
    outMsg = buf;
    return n;
}

void DispatchMainThreadTicks() noexcept {
    InFlightHostGuard guard;
    std::vector<RegisteredMainThreadTick> snapshot;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        if (g_mainTicks.empty()) return;
        snapshot = g_mainTicks;
        guard.Arm();
    }
    const std::uint64_t seq = g_mainTickSeq.fetch_add(1U, std::memory_order_relaxed) + 1ULL;
    for (const auto& t : snapshot) {
        if (t.fn) {
            InvokeMainTickSEH(t.fn, seq, t.userData);
        }
    }
}

bool DispatchPluginSubcommand(const std::string& subcmdLower,
                              const std::vector<std::string>& argsAfterSubcmd,
                              const std::string& rawArgsAfterSubcmd) {
    // Hold `g_lifecycleMx` during subcommand execution so a plugin cannot be unloaded mid-command,
    // while keeping `g_registryMx` released so the plugin can freely call any `g_host->...` API!
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    MhwRngSubcommandFn fn = nullptr;
    void* ud              = nullptr;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& c : g_subcmds) {
            if (c.subcmdLower == subcmdLower) {
                fn = c.fn;
                ud = c.userData;
                break;
            }
        }
    }
    if (!fn) return false;

    std::vector<const char*> ptrs;
    ptrs.reserve(argsAfterSubcmd.size());
    for (const std::string& s : argsAfterSubcmd) ptrs.push_back(s.c_str());

    if (!InvokeSubcommandSEH(fn,
                             static_cast<int>(ptrs.size()),
                             ptrs.empty() ? nullptr : ptrs.data(),
                             rawArgsAfterSubcmd.c_str(),
                             ud)) {
        Host_ChatPrintf(app::IsEnglish()
                            ? "[RNG] Plugin threw an exception running `%s` (safely caught)"
                            : u8"[RNG] 插件执行指令 `%s` 时发生异常，已安全拦截",
                        subcmdLower.c_str());
    }
    return true;
}

bool CompletePluginSubcommand(const std::string& subcmdLower,
                              const std::vector<std::string>& priorArgsAfterSubcmd,
                              const std::string& currentPrefix,
                              std::vector<std::string>& outCandidates) {
    outCandidates.clear();
    InFlightHostGuard guard;
    MhwRngSubcommandCompleteFn comp = nullptr;
    void* ud                        = nullptr;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& c : g_subcmds) {
            if (c.subcmdLower == subcmdLower && c.completer != nullptr) {
                comp = c.completer;
                ud   = c.completerUserData;
                guard.Arm();
                break;
            }
        }
    }
    if (!comp) return false;

    std::vector<const char*> ptrs;
    ptrs.reserve(priorArgsAfterSubcmd.size());
    for (const std::string& s : priorArgsAfterSubcmd) ptrs.push_back(s.c_str());

    auto sink = +[](const char* cand, void* ctx) {
        if (!cand || cand[0] == '\0' || !ctx) return;
        auto* vec = static_cast<std::vector<std::string>*>(ctx);
        if (vec->size() < 512U) {
            vec->emplace_back(cand);
        }
    };

    InvokeCompleterSEH(comp,
                       subcmdLower.c_str(),
                       static_cast<int>(ptrs.size()),
                       ptrs.empty() ? nullptr : ptrs.data(),
                       currentPrefix.c_str(),
                       sink,
                       &outCandidates,
                       ud);
    return true;
}

bool QuerySiteMeta(std::uint32_t rva, MhwRngSiteMeta* outMeta) {
    if (!outMeta) return false;
    std::memset(outMeta, 0, sizeof(*outMeta));
    outMeta->rva = rva;

    InFlightHostGuard guard;
    bool hasExplicit = false;
    std::string expAlias, expDesc;
    std::vector<FallbackResolver> resolvers;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& m : g_siteMetas) {
            if (m.rva == rva) {
                hasExplicit = true;
                expAlias    = m.alias;
                expDesc     = ResolveOwnedLocale(m.descI18n, m.descUtf8);
                break;
            }
        }
        resolvers = g_fallbackResolvers;
        if (!resolvers.empty()) {
            guard.Arm();
        }
    }

    if (hasExplicit) {
        outMeta->found          = 1U;
        outMeta->isCoreGameplay = 1U;
        CopyTrunc(outMeta->alias, sizeof(outMeta->alias), expAlias.c_str());
        CopyTrunc(outMeta->description, sizeof(outMeta->description), expDesc.c_str());
        for (const auto& fb : resolvers) {
            MhwRngSiteMeta tmp{};
            if (fb.fn && InvokeResolverSEH(fb.fn, rva, &tmp, fb.userData)) {
                outMeta->category  = tmp.category;
                outMeta->scaleKind = tmp.scaleKind;
                CopyTrunc(outMeta->techSymbol, sizeof(outMeta->techSymbol), tmp.techSymbol);
                if (outMeta->alias[0] == '\0') CopyTrunc(outMeta->alias, sizeof(outMeta->alias), tmp.alias);
                if (outMeta->description[0] == '\0') CopyTrunc(outMeta->description, sizeof(outMeta->description), tmp.description);
                break;
            }
        }
        return true;
    }

    for (const auto& fb : resolvers) {
        if (fb.fn && InvokeResolverSEH(fb.fn, rva, outMeta, fb.userData)) {
            outMeta->found = 1U;
            return true;
        }
    }

    std::snprintf(outMeta->alias, sizeof(outMeta->alias), "0x%07X", rva);
    CopyTrunc(outMeta->description,
              sizeof(outMeta->description),
              app::IsEnglish() ? "Raw call site" : u8"原始调用点");
    return false;
}

bool FormatSlotCustom(const MhwRngSlotSnapshot* snap, char* outBuf, std::size_t bufCap) {
    if (!snap || !outBuf || bufCap == 0U) return false;
    InFlightHostGuard guard;
    MhwRngValueFormatterFn fn = nullptr;
    void* ud                  = nullptr;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& m : g_siteMetas) {
            if (m.rva == snap->callSiteRva && m.formatter != nullptr) {
                fn = m.formatter;
                ud = m.userData;
                guard.Arm();
                break;
            }
        }
    }
    return fn ? InvokeFormatterSEH(fn, snap, outBuf, bufCap, ud) : false;
}

const char* GetPluginShortName(MhwRngPluginId id) noexcept {
    if (id == kMhwRngPluginIdKernel) return "kernel";
    const char* cached = g_shortNameCache[id % kShortNameSlotCount];
    return (cached[0] != '\0') ? cached : "plugin";
}

bool ValidateDeclaredShortName(const char* rawName) noexcept {
    if (!rawName || rawName[0] == '\0') return false;
    std::size_t len = 0U;
    while (rawName[len] != '\0') {
        const char c = rawName[len];
        if (!((c >= 'a' && c <= 'z') || c == '_')) {
            return false;
        }
        ++len;
        if (len > 24U) return false;
    }
    if (std::strcmp(rawName, "kernel") == 0) {
        return false;
    }
    return true;
}

std::string AllocateUniqueShortName(const std::string& baseName,
                                    const std::vector<std::string>& existingLower) {
    auto exists = [&](const std::string& cand) -> bool {
        const std::string low = LowerAscii(cand);
        for (const auto& ex : existingLower) {
            if (LowerAscii(ex) == low) return true;
        }
        return false;
    };
    if (!exists(baseName)) {
        return baseName;
    }
    for (std::uint32_t n = 1U; n < 10000U; ++n) {
        const std::string suffix = "_" + std::to_string(n);
        const std::size_t maxPrefix = (suffix.size() < 31U) ? (31U - suffix.size()) : 0U;
        const std::string cand = baseName.substr(0U, maxPrefix) + suffix;
        if (!exists(cand)) {
            return cand;
        }
    }
    return baseName;
}

std::vector<LoadedPluginSummary> ListLoadedPlugins() {
    std::lock_guard<std::mutex> rlk(g_registryMx);
    std::vector<LoadedPluginSummary> out;
    out.reserve(g_plugins.size());
    for (const auto& p : g_plugins) {
        std::size_t cmdCnt = 0;
        for (const auto& c : g_subcmds) {
            if (c.ownerId == p.id) ++cmdCnt;
        }
        out.push_back(LoadedPluginSummary{
            p.id,
            p.shortName,
            ResolveOwnedLocale(p.displayNamesI18n, p.displayName),
            p.version,
            ResolveOwnedLocale(p.descriptionsI18n, p.description),
            p.fileName,
            cmdCnt
        });
    }
    return out;
}

std::vector<SubcommandHelpEntry> ListPluginSubcommands() {
    std::lock_guard<std::mutex> rlk(g_registryMx);
    std::vector<SubcommandHelpEntry> out;
    out.reserve(g_subcmds.size());
    for (const auto& c : g_subcmds) {
        out.push_back(SubcommandHelpEntry{
            c.ownerId,
            GetPluginShortName(c.ownerId),
            c.subcmdLower,
            c.usageExample,
            ResolveOwnedLocale(c.helpI18n, c.helpCn)
        });
    }
    return out;
}

const MhwRngHostApi* GetHostApiTable() noexcept {
    return &g_hostApi;
}

} // namespace plugin_host

MHW_RNG_API const MhwRngHostApi* MhwRng_GetHostApi() {
    return plugin_host::GetHostApiTable();
}

MHW_RNG_API void MhwRng_PumpMainThreadTicks() {
    plugin_host::DispatchMainThreadTicks();
}
