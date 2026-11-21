#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#include "plugin_host.h"
#include "log_config.h"
#include "rng_engine.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace plugin_host {
namespace {

struct RegisteredSubcmd {
    MhwRngPluginId     ownerId = 0;
    std::string        subcmdLower;
    std::string        usageExample;
    std::string        helpCn;
    MhwRngSubcommandFn fn       = nullptr;
    void*              userData = nullptr;
};

struct RegisteredSiteMeta {
    MhwRngPluginId         ownerId = 0;
    std::uint32_t          rva     = 0;
    std::string            alias;
    std::string            descUtf8;
    MhwRngValueFormatterFn formatter = nullptr;
    void*                  userData  = nullptr;
};

struct FallbackResolver {
    MhwRngPluginId       ownerId  = 0;
    MhwRngMetaResolverFn fn       = nullptr;
    void*                userData = nullptr;
};

struct LoadedPlugin {
    MhwRngPluginId         id = 0;
    std::string            shortName;
    std::string            shortNameLower;
    std::string            displayName;
    std::string            version;
    std::string            description;
    std::string            sourcePath;
    std::string            fileName;
    std::string            shadowPath;
    HMODULE                module     = nullptr;
    MhwRngPluginShutdownFn shutdownFn = nullptr;
};

// Two-tier lock hierarchy (prevents quiescence deadlocks even if a plugin calls Host APIs inside `OnSlotDraw`):
//   1. `g_lifecycleMx`: Serializes Load / Unload / ReloadAll operations.
//   2. `g_registryMx` : Leaf lock protecting in-memory metadata & command vectors (`g_plugins`, `g_subcmds`,
//                       `g_siteMetas`, `g_fallbackResolvers`). NEVER held across `DetachAllHooksForPluginAndWait`,
//                       plugin callbacks, `LoadLibrary`, or `FreeLibrary`!
std::recursive_mutex             g_lifecycleMx;
std::mutex                       g_registryMx;

std::string                      g_subPluginDir;
std::string                      g_hotloadDir;
ChatPrintSinkFn                  g_chatSink = nullptr;
MhwRngHostApi                    g_hostApi{};
MhwRngPluginId                   g_nextPluginId = 1U;
std::uint32_t                    g_shadowGen    = 1U;

// Static lock-free ring of plugin shortNames so `GetPluginShortName()` never returns dangling heap pointers:
constexpr std::size_t            kShortNameSlotCount = 512U;
char                             g_shortNameCache[kShortNameSlotCount][32]{};

std::vector<LoadedPlugin>        g_plugins;
std::vector<RegisteredSubcmd>    g_subcmds;
std::vector<RegisteredSiteMeta>  g_siteMetas;
std::vector<FallbackResolver>    g_fallbackResolvers;

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

std::string DeriveShortNameFromFilename(const std::string& fileName) {
    std::string s = LowerAscii(fileName);
    if (s.size() > 4U && s.substr(s.size() - 4U) == ".dll") {
        s.resize(s.size() - 4U);
    }
    if (s.rfind("mhw_rng_", 0) == 0 && s.size() > 8U) {
        s = s.substr(8U);
    } else if (s.rfind("rng_", 0) == 0 && s.size() > 4U) {
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

    // 2. Remove registered subcommands, metadata, fallback resolvers, and slot overrides
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
    }
    rng::ClearRulesByOwner(p.id);

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
                                std::string& outMsgCn) {
    ::CreateDirectoryA(g_subPluginDir.c_str(), nullptr);
    ::CreateDirectoryA(g_hotloadDir.c_str(), nullptr);
    ::SetFileAttributesA(g_hotloadDir.c_str(), FILE_ATTRIBUTE_HIDDEN);
    CleanupStaleShadowFiles();

    const std::string baseShort = DeriveShortNameFromFilename(fileName);
    char shadowName[128]{};
    std::snprintf(shadowName, sizeof(shadowName), "%s.%u.%lu.dll",
                  baseShort.c_str(), g_shadowGen++, ::GetCurrentProcessId());
    const std::string shadowPath = g_hotloadDir + "\\" + shadowName;

    if (!::CopyFileA(fullDllPath.c_str(), shadowPath.c_str(), FALSE)) {
        outMsgCn = u8"无法读取插件文件: " + fileName;
        return false;
    }

    HMODULE mod = ::LoadLibraryA(shadowPath.c_str());
    if (!mod) {
        ::DeleteFileA(shadowPath.c_str());
        outMsgCn = u8"LoadLibrary 失败: " + fileName;
        return false;
    }

    auto fnGetInfo  = reinterpret_cast<MhwRngPluginGetInfoFn>(::GetProcAddress(mod, "MhwRngPlugin_GetInfo"));
    auto fnInit     = reinterpret_cast<MhwRngPluginInitFn>(::GetProcAddress(mod, "MhwRngPlugin_Init"));
    auto fnShutdown = reinterpret_cast<MhwRngPluginShutdownFn>(::GetProcAddress(mod, "MhwRngPlugin_Shutdown"));

    if (!fnGetInfo || !fnInit) {
        ::FreeLibrary(mod);
        ::DeleteFileA(shadowPath.c_str());
        outMsgCn = u8"非标准 RNG 插件(缺少导出入口): " + fileName;
        return false;
    }

    const MhwRngPluginDescriptor* desc = InvokePluginGetInfoSEH(fnGetInfo);
    if (!desc || desc->targetApiVersion != kMhwRngApiVersion || !desc->shortName || desc->shortName[0] == '\0') {
        ::FreeLibrary(mod);
        ::DeleteFileA(shadowPath.c_str());
        outMsgCn = u8"插件 API 版本不匹配: " + fileName;
        return false;
    }

    const std::string shortLow = LowerAscii(desc->shortName);
    MhwRngPluginId existingId = 0U;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& p : g_plugins) {
            if (p.shortNameLower == shortLow) {
                existingId = p.id;
                break;
            }
        }
    }
    if (existingId != 0U) {
        UnloadByIdLifecycleLocked(existingId);
    }

    const MhwRngPluginId newId = g_nextPluginId++;
    CopyTrunc(g_shortNameCache[newId % kShortNameSlotCount],
              sizeof(g_shortNameCache[0]),
              desc->shortName);

    LoadedPlugin lp{};
    lp.id             = newId;
    lp.shortName      = desc->shortName;
    lp.shortNameLower = shortLow;
    lp.displayName    = desc->displayName ? desc->displayName : desc->shortName;
    lp.version        = desc->version ? desc->version : "1.0";
    lp.description    = desc->description ? desc->description : "";
    lp.sourcePath     = fullDllPath;
    lp.fileName       = fileName;
    lp.shadowPath     = shadowPath;
    lp.module         = mod;
    lp.shutdownFn     = fnShutdown;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        g_plugins.push_back(lp);
    }

    if (!InvokePluginInitSEH(fnInit, &g_hostApi, newId)) {
        UnloadByIdLifecycleLocked(newId);
        outMsgCn = u8"插件初始化返回失败: " + std::string(desc->shortName);
        return false;
    }

    std::size_t cmdCnt = 0;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& c : g_subcmds) {
            if (c.ownerId == newId) ++cmdCnt;
        }
    }
    app::Log("plugin_host: loaded '%s' v%s (%s, id=%u, cmds=%zu)",
             lp.shortName.c_str(), lp.version.c_str(), fileName.c_str(), newId, cmdCnt);

    char okBuf[192]{};
    std::snprintf(okBuf, sizeof(okBuf), u8"%s v%s (%s, 注册%zu条指令)",
                  lp.shortName.c_str(), lp.version.c_str(), lp.displayName.c_str(), cmdCnt);
    outMsgCn = okBuf;
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
            existing.ownerId      = selfId;
            existing.usageExample = usageExample ? usageExample : "";
            existing.helpCn       = helpCn ? helpCn : "";
            existing.fn           = fn;
            existing.userData     = userData;
            return true;
        }
    }
    g_subcmds.push_back(RegisteredSubcmd{
        selfId,
        low,
        usageExample ? usageExample : "",
        helpCn ? helpCn : "",
        fn,
        userData
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

} // namespace

// ============================================================================
//  Public Plugin Host Implementation
// ============================================================================

void Init(std::uintptr_t exeBase,
          std::uintptr_t exeEnd,
          const std::string& subPluginDir,
          ChatPrintSinkFn chatSink) {
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    g_subPluginDir = subPluginDir;
    g_hotloadDir   = subPluginDir + "\\.hotload";
    g_chatSink     = chatSink;

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

bool LoadOrReloadSingle(const std::string& nameOrFile, std::string& outMsgCn) {
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
        return LoadDllPathLifecycleLocked(existingSource, existingFile, outMsgCn);
    }

    const std::string cands[] = {
        nameOrFile,
        nameOrFile + ".dll",
        "rng_" + nameOrFile + ".dll",
        "mhw_rng_" + nameOrFile + ".dll"
    };
    for (const std::string& fn : cands) {
        const std::string full = g_subPluginDir + "\\" + fn;
        const DWORD attr = ::GetFileAttributesA(full.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return LoadDllPathLifecycleLocked(full, fn, outMsgCn);
        }
    }

    outMsgCn = u8"在 nativePC\\plugins\\mhw_rng\\ 下找不到插件 `" + nameOrFile + "`";
    return false;
}

bool UnloadSingle(const std::string& nameOrFile, std::string& outMsgCn) {
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
    if (targetId != 0U) {
        UnloadByIdLifecycleLocked(targetId);
        outMsgCn = u8"已卸载插件 `" + sname + u8"` 并清理其全部钩子与锁定";
        return true;
    }
    outMsgCn = u8"未找到已加载插件 `" + nameOrFile + "`";
    return false;
}

std::size_t ReloadAll(std::string& outMsgCn) {
    std::lock_guard<std::recursive_mutex> lk(g_lifecycleMx);
    ShutdownAll();
    const std::size_t n = LoadAllFromDirectory();
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), u8"已热重载全部 %zu 个插件", n);
    outMsgCn = buf;
    return n;
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
        Host_ChatPrintf(u8"[RNG] 插件执行指令 `%s` 时发生异常，已安全拦截", subcmdLower.c_str());
    }
    return true;
}

bool QuerySiteMeta(std::uint32_t rva, MhwRngSiteMeta* outMeta) {
    if (!outMeta) return false;
    std::memset(outMeta, 0, sizeof(*outMeta));
    outMeta->rva = rva;

    bool hasExplicit = false;
    std::string expAlias, expDesc;
    std::vector<FallbackResolver> resolvers;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& m : g_siteMetas) {
            if (m.rva == rva) {
                hasExplicit = true;
                expAlias    = m.alias;
                expDesc     = m.descUtf8;
                break;
            }
        }
        resolvers = g_fallbackResolvers;
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
    CopyTrunc(outMeta->description, sizeof(outMeta->description), u8"原始调用点");
    return false;
}

bool FormatSlotCustom(const MhwRngSlotSnapshot* snap, char* outBuf, std::size_t bufCap) {
    if (!snap || !outBuf || bufCap == 0U) return false;
    MhwRngValueFormatterFn fn = nullptr;
    void* ud                  = nullptr;
    {
        std::lock_guard<std::mutex> rlk(g_registryMx);
        for (const auto& m : g_siteMetas) {
            if (m.rva == snap->callSiteRva && m.formatter != nullptr) {
                fn = m.formatter;
                ud = m.userData;
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
            p.id, p.shortName, p.displayName, p.version, p.description, p.fileName, cmdCnt
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
            c.ownerId, GetPluginShortName(c.ownerId), c.subcmdLower, c.usageExample, c.helpCn
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
