// ============================================================================
//  MHW Universal RNG Microkernel (mhw_rng.dll)
//  Pure Interception + Composite Condition Slot Table + Hot-Reload Plugin Bus
// ============================================================================
#include "hook.h"
#include "log_config.h"
#include "mhw_addresses.h"
#include "mhw_rng_api.h"
#include "plugin_host.h"
#include "rng_engine.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using ChatDispatchFn     = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint8_t);
using SChatShowMessageFn = void(__fastcall*)(std::uintptr_t, const char*, float, std::uint32_t, std::uint8_t);
using D3DPresentFn       = std::uint64_t(__fastcall*)(std::uintptr_t, std::uint64_t, std::uint64_t, std::uint64_t);

HMODULE                    g_selfModule       = nullptr;
std::uintptr_t             g_exeBase          = 0;
std::uintptr_t             g_exeEnd           = 0;
app::Config                g_cfg{};

void*                       g_origChatDispatchRaw = nullptr;
void*                       g_origPresentRaw      = nullptr;
std::uintptr_t              g_chatRelay           = 0;
HHOOK                       g_kbHook           = nullptr;
std::atomic<bool>           g_kbHookInstalled{false};
std::atomic<bool>           g_detaching{false};
thread_local std::uintptr_t tls_activeSChat    = 0;
thread_local bool           tls_inChatCmd      = false;

std::string TrimStr(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::vector<std::string> SplitTokens(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        std::size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

inline bool IsValidUserPtr(std::uintptr_t p) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & 0x7ULL) == 0ULL;
}

void PostChatSystemNoticeSEH(const char* utf8Text) noexcept {
    if (!g_cfg.showChatFeedback || !g_exeBase || !utf8Text || utf8Text[0] == '\0') return;
    __try {
        std::uintptr_t sChat = *reinterpret_cast<const std::uintptr_t*>(g_exeBase + mhw::kRvaSChatInstance);
        if (!IsValidUserPtr(sChat)) {
            sChat = tls_activeSChat;
        }
        if (!IsValidUserPtr(sChat)) return;

        // Verify sChat vftable and sMhGUI::Instance before entering sChat::ShowMessage
        // (sChat::ShowMessage locks sChat+0x08 then calls CreateSystemMessage which dereferences sMhGUI::Instance).
        const std::uintptr_t sChatVft = *reinterpret_cast<const std::uintptr_t*>(sChat);
        const std::uintptr_t sMhGui   = *reinterpret_cast<const std::uintptr_t*>(g_exeBase + mhw::kRvaSMhGuiInstance);
        if (!IsValidUserPtr(sChatVft) || !IsValidUserPtr(sMhGui)) return;

        const auto fnShow = reinterpret_cast<SChatShowMessageFn>(g_exeBase + mhw::kRvaSChatShowMessage);
        fnShow(sChat, utf8Text, 0.0f, 0xFFFFFFFFU, 0U);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void NotifyChatFmt(const char* fmt, ...) {
    char buf[256]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    app::Log("ui: %s", buf);
    PostChatSystemNoticeSEH(buf);
}

bool IsGameTextInputActive(std::uintptr_t exeBase) noexcept {
    if (!exeBase) return false;
    __try {
        const std::uintptr_t mhInputText = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSMhInputTextInstance);
        if (mhInputText < 0x100000ULL || mhInputText > 0x00007FFFFFFF0000ULL || (mhInputText & 0x7ULL) != 0) {
            return false;
        }
        if (*reinterpret_cast<const std::uint8_t*>(mhInputText + mhw::kOffMhInputTextUnfocused) != 0) {
            return false;
        }
        const std::uintptr_t editor = *reinterpret_cast<const std::uintptr_t*>(
            mhInputText + mhw::kOffInputTextEditor);
        if (editor < 0x100000ULL || editor > 0x00007FFFFFFF0000ULL || (editor & 0x7ULL) != 0) {
            return false;
        }
        return (*reinterpret_cast<const std::int32_t*>(editor + mhw::kOffInputEditorState) == 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadLocalChatMessageSEH(std::uintptr_t netMsg, char outText[129]) noexcept {
    __try {
        if (netMsg < 0x100000ULL || netMsg > 0x00007FFFFFFF0000ULL) return false;
        const auto msgType = static_cast<int>(*reinterpret_cast<const std::int8_t*>(netMsg + mhw::kOffChatNetMsgType));
        if (msgType != 1 && msgType != 2 && msgType != 3) return false;

        const auto* src = reinterpret_cast<const char*>(netMsg + mhw::kOffChatNetMsgText);
        std::size_t n = 0;
        while (n < 128U && src[n] != '\0') {
            outText[n] = src[n];
            ++n;
        }
        outText[n] = '\0';
        return n > 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FormatKernelModeShort(const MhwRngSlotSnapshot& s, char* outBuf, std::size_t cap) {
    if (plugin_host::FormatSlotCustom(&s, outBuf, cap)) return;
    switch (s.mode) {
        case kMhwRngSlotFixedValue:
            std::snprintf(outBuf, cap, u8"固定u32=0x%08X", s.fixedValue);
            break;
        case kMhwRngSlotCustomSeed:
            std::snprintf(outBuf, cap, u8"独立种子=%llu", static_cast<unsigned long long>(s.customSeed));
            break;
        case kMhwRngSlotSequence:
            std::snprintf(outBuf, cap, u8"序列[%u/%u]", s.seqCursor, s.seqLength);
            break;
        case kMhwRngSlotBypass:
            std::snprintf(outBuf, cap, u8"原版直通");
            break;
        default:
            std::snprintf(outBuf, cap, s.hasPluginHook ? u8"插件动态回调" : u8"跟随总种子");
            break;
    }
}

void HandleRngChatCommand(const std::string& rawMsg) {
    const std::string trimmed = TrimStr(rawMsg);
    const std::string lowMsg  = LowerAscii(trimmed);
    const std::string lowPref = LowerAscii(g_cfg.chatPrefix);
    if (lowPref.empty() || lowMsg.rfind(lowPref, 0) != 0) return;

    // Unified `$rng <subcmd>` or `$rng_<subcmd>` namespace!
    std::string restStr;
    if (trimmed.size() > lowPref.size()) {
        const unsigned char sep = static_cast<unsigned char>(trimmed[lowPref.size()]);
        if (std::isspace(sep) || sep == '_') {
            restStr = TrimStr(trimmed.substr(lowPref.size() + 1U));
        } else {
            return;
        }
    }

    const std::vector<std::string> toks = SplitTokens(restStr);
    const std::string sub = toks.empty() ? "status" : LowerAscii(toks[0]);

    // 1. Kernel Status (`$rng` / `$rng status`)
    if (sub == "status" || sub == "stat") {
        const std::uint32_t estCap = std::min<std::uint32_t>(8192U, std::max<std::uint32_t>(MhwRng_GetActiveSlotCount() + 64U, 256U));
        std::vector<MhwRngSlotSnapshot> snaps(estCap);
        const std::uint32_t totalSlots = MhwRng_SnapshotActiveSlots(snaps.data(), estCap);
        snaps.resize(totalSlots);

        std::vector<MhwRngSlotSnapshot> activeRules;
        for (const auto& s : snaps) {
            if (s.mode != kMhwRngSlotMasterDerived || (s.condId == 0U && s.hasPluginHook != 0U)) {
                activeRules.push_back(s);
            }
        }
        const auto plugins = plugin_host::ListLoadedPlugins();

        NotifyChatFmt(u8"[RNG] 接管:%s | 总种子:%llu | 第%llu轮 | 活跃槽:%u | 插件:%zu个 | 激活规则:%zu处",
                      MhwRng_IsTakeoverEnabled() ? u8"开启" : u8"关闭",
                      static_cast<unsigned long long>(MhwRng_GetMasterSeed()),
                      static_cast<unsigned long long>(MhwRng_GetEpoch()),
                      totalSlots,
                      plugins.size(),
                      activeRules.size());

        if (activeRules.empty()) {
            NotifyChatFmt(u8"[RNG] 输入 `$rng help` 查看核心与插件指令，或 `$rng plugins` 查看热重载插件");
        } else {
            const std::size_t showN = std::min<std::size_t>(activeRules.size(), 3U);
            for (std::size_t i = 0; i < showN; ++i) {
                const auto& s = activeRules[i];
                MhwRngSiteMeta meta{};
                plugin_host::QuerySiteMeta(s.callSiteRva, &meta);
                char modeBuf[80]{};
                FormatKernelModeShort(s, modeBuf, sizeof(modeBuf));
                char condTag[24]{};
                if (s.condId != 0U) std::snprintf(condTag, sizeof(condTag), "[Cond:%u]", s.condId);
                NotifyChatFmt(u8"[RNG] [%s] %s%s (%s) = %s",
                              plugin_host::GetPluginShortName(s.ownerPluginId),
                              meta.alias,
                              condTag,
                              meta.description,
                              modeBuf);
            }
            if (activeRules.size() > showN) {
                NotifyChatFmt(u8"[RNG] ...另有 %zu 处规则激活", activeRules.size() - showN);
            }
        }
        return;
    }

    // 2. Kernel + Plugin Unified Help (`$rng help`)
    if (sub == "help" || sub == "?") {
        NotifyChatFmt(u8"[RNG 核心] `$rng` 状态 | `on/off` 开关 | `seed <n>` 总种子 | `reset` 重置轮次 | `clearall` 全清");
        NotifyChatFmt(u8"[RNG 插件管理] `plugins` 列表 | `reload [插件名]` 热重载DLL | `load/unload <插件名>`");
        const auto pcmds = plugin_host::ListPluginSubcommands();
        if (!pcmds.empty()) {
            std::string line = u8"[已载入插件指令] ";
            bool firstOnLine = true;
            for (std::size_t i = 0; i < pcmds.size(); ++i) {
                if (!firstOnLine) line += " | ";
                line += pcmds[i].subcmd;
                firstOnLine = false;
                if (line.size() > 95U && i + 1U < pcmds.size()) {
                    NotifyChatFmt("%s", line.c_str());
                    line        = u8"[已载入插件指令] ";
                    firstOnLine = true;
                }
            }
            NotifyChatFmt("%s", line.c_str());
        }
        return;
    }

    // 3. Plugin Management & Hot-Reload (`plugins`, `reload`, `load`, `unload`)
    if (sub == "plugins" || sub == "mods") {
        const auto list = plugin_host::ListLoadedPlugins();
        if (list.empty()) {
            NotifyChatFmt(u8"[RNG] 当前未加载任何子插件 (目录: nativePC\\plugins\\mhw_rng\\*.dll)");
            return;
        }
        NotifyChatFmt(u8"[RNG] 已加载 %zu 个插件 (支持 `$rng reload <名字>` 免重启热更新)：", list.size());
        for (const auto& p : list) {
            NotifyChatFmt(u8" - %s v%s : %s (%zu条指令)",
                          p.shortName.c_str(), p.version.c_str(), p.description.c_str(), p.subcmdCount);
        }
        return;
    }

    if (sub == "reload") {
        if (toks.size() >= 2 && LowerAscii(toks[1]) != "all") {
            std::string msg;
            const bool ok = plugin_host::LoadOrReloadSingle(toks[1], msg);
            NotifyChatFmt(u8"[RNG] 热重载 `%s` %s: %s", toks[1].c_str(), ok ? u8"成功" : u8"失败", msg.c_str());
            return;
        }
        app::Config fresh      = app::LoadConfig();
        g_cfg.showChatFeedback = fresh.showChatFeedback;
        g_cfg.chatPrefix       = fresh.chatPrefix;
        MhwRng_SetTakeoverEnabled(fresh.takeoverOnBoot);
        MhwRng_SetMasterSeed(fresh.masterSeed, true);
        rng::ApplyConfigRules(fresh, true);
        std::string pmsg;
        plugin_host::ReloadAll(pmsg);
        NotifyChatFmt(u8"[RNG] 已重载 mhw_rng.ini (总种子:%llu) & %s",
                      static_cast<unsigned long long>(fresh.masterSeed), pmsg.c_str());
        return;
    }

    if (sub == "load") {
        if (toks.size() < 2) {
            NotifyChatFmt(u8"[RNG] 用法: `%s load <插件名>`", g_cfg.chatPrefix.c_str());
            return;
        }
        std::string msg;
        const bool ok = plugin_host::LoadOrReloadSingle(toks[1], msg);
        NotifyChatFmt(u8"[RNG] 加载%s: %s", ok ? u8"成功" : u8"失败", msg.c_str());
        return;
    }

    if (sub == "unload") {
        if (toks.size() < 2) {
            NotifyChatFmt(u8"[RNG] 用法: `%s unload <插件名>`", g_cfg.chatPrefix.c_str());
            return;
        }
        std::string msg;
        plugin_host::UnloadSingle(toks[1], msg);
        NotifyChatFmt(u8"[RNG] %s", msg.c_str());
        return;
    }

    // 4. Global Takeover / Seed / Epoch / ClearAll
    if (sub == "on" || sub == "enable") {
        MhwRng_SetTakeoverEnabled(true);
        NotifyChatFmt(u8"[RNG] 全局随机数接管已【开启】 (总种子:%llu, 第%llu轮)",
                      static_cast<unsigned long long>(MhwRng_GetMasterSeed()),
                      static_cast<unsigned long long>(MhwRng_GetEpoch()));
        return;
    }

    if (sub == "off" || sub == "disable") {
        MhwRng_SetTakeoverEnabled(false);
        NotifyChatFmt(u8"[RNG] 全局随机数接管已【关闭】 (完全原版随机)");
        return;
    }

    if (sub == "reset") {
        const std::uint64_t ep = MhwRng_ResetEpoch();
        NotifyChatFmt(u8"[RNG] 已重置到第 %llu 轮 (所有随机流回退起点 & 本轮计数清零)",
                      static_cast<unsigned long long>(ep));
        return;
    }

    if (sub == "seed") {
        if (toks.size() < 2) {
            NotifyChatFmt(u8"[RNG] 用法: `%s seed <数字>`", g_cfg.chatPrefix.c_str());
            return;
        }
        std::uint64_t newSeed = 0;
        if (!app::ParseU64Auto(toks[1].c_str(), newSeed)) {
            NotifyChatFmt(u8"[RNG] 种子格式无效: %s", toks[1].c_str());
            return;
        }
        MhwRng_SetTakeoverEnabled(true);
        MhwRng_SetMasterSeed(newSeed, true);
        NotifyChatFmt(u8"[RNG] 已切换总种子 = %llu (0x%llX) | 第 %llu 轮",
                      static_cast<unsigned long long>(newSeed),
                      static_cast<unsigned long long>(newSeed),
                      static_cast<unsigned long long>(MhwRng_GetEpoch()));
        return;
    }

    if (sub == "clearall") {
        MhwRng_ClearAllSlotOverrides();
        NotifyChatFmt(u8"[RNG] 已清空全部槽位的固定值/序列/子种子覆盖");
        return;
    }

    // 5. Low-Level Address Primitives (`rawfix`, `rawseed`, `rawseq`, `rawbypass`, `rawclear`)
    if (sub == "rawfix" || sub == "rawseed" || sub == "rawbypass" || sub == "rawclear") {
        if (toks.size() < (sub == "rawfix" || sub == "rawseed" ? 3U : 2U)) {
            NotifyChatFmt(u8"[RNG] 用法: `%s %s <0xRVA> [value] [condId]`", g_cfg.chatPrefix.c_str(), sub.c_str());
            return;
        }
        std::uint64_t rawAddr = 0;
        if (!app::ParseU64Auto(toks[1].c_str(), rawAddr)) {
            NotifyChatFmt(u8"[RNG] 核心 `%s` 只接受数值地址(0xRVA)；名字/换算请用对应插件指令", sub.c_str());
            return;
        }
        const std::uint32_t rva = rng::NormalizeUserSiteAddress(rawAddr);
        if (rva == 0U) {
            NotifyChatFmt(u8"[RNG] 无效地址: %s", toks[1].c_str());
            return;
        }
        if (sub == "rawclear") {
            rng::ClearSlotAllConditions(rva);
            NotifyChatFmt(u8"[RNG] 已恢复 0x%07X 全部条件槽 -> 跟随总种子", rva);
            return;
        }
        if (sub == "rawbypass") {
            std::uint64_t cid = 0;
            if (toks.size() >= 3) app::ParseU64Auto(toks[2].c_str(), cid);
            rng::SetCondBypass(kMhwRngPluginIdKernel, rva, static_cast<std::uint32_t>(cid));
            NotifyChatFmt(u8"[RNG] 0x%07X [Cond:%llu] -> 原版直通", rva, static_cast<unsigned long long>(cid));
            return;
        }
        std::uint64_t val = 0, cid = 0;
        if (!app::ParseU64Auto(toks[2].c_str(), val)) {
            NotifyChatFmt(u8"[RNG] 数值无效: %s", toks[2].c_str());
            return;
        }
        if (toks.size() >= 4) app::ParseU64Auto(toks[3].c_str(), cid);
        if (sub == "rawfix") {
            rng::SetCondFixedValue(kMhwRngPluginIdKernel, rva, static_cast<std::uint32_t>(cid), static_cast<std::uint32_t>(val));
            NotifyChatFmt(u8"[RNG] 0x%07X [Cond:%llu] -> 原始u32 0x%08X (%u)",
                          rva, static_cast<unsigned long long>(cid),
                          static_cast<std::uint32_t>(val), static_cast<std::uint32_t>(val));
        } else {
            rng::SetCondCustomSeed(kMhwRngPluginIdKernel, rva, static_cast<std::uint32_t>(cid), val);
            NotifyChatFmt(u8"[RNG] 0x%07X [Cond:%llu] -> 独立种子 %llu",
                          rva, static_cast<unsigned long long>(cid), static_cast<unsigned long long>(val));
        }
        return;
    }

    // 6. Delegate to Registered Plugin Subcommands (`size`, `top`, `dump`, `find`, `fix`, etc.)!
    std::vector<std::string> subArgs;
    if (toks.size() > 1U) {
        subArgs.assign(toks.begin() + 1, toks.end());
    }
    std::string rawAfterSub;
    if (!toks.empty()) {
        const std::size_t p = restStr.find(toks[0]);
        if (p != std::string::npos) {
            rawAfterSub = TrimStr(restStr.substr(p + toks[0].size()));
        }
    }
    if (plugin_host::DispatchPluginSubcommand(sub, subArgs, rawAfterSub)) {
        return;
    }

    NotifyChatFmt(u8"[RNG] 未知指令 `%s`；输入 `$rng help` 查看可用指令或 `$rng plugins` 查看已加载插件",
                  toks[0].c_str());
}

void RunRngChatCommandSafe(const char* rawMsg) noexcept {
    try {
        HandleRngChatCommand(rawMsg);
    } catch (...) {
        app::Log("ui: caught C++ exception while executing chat command");
    }
}

bool InvokeChatCommandSEH(const char* rawMsg) noexcept {
    __try {
        RunRngChatCommandSafe(rawMsg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall Hook_ChatDispatch(std::uintptr_t sChat,
                                  std::uintptr_t netMsg,
                                  std::uint32_t targetChannel,
                                  std::uint8_t isLocal) {
    char msgBuf[129]{};
    const bool hasLocalText = (isLocal != 0 && netMsg != 0) && ReadLocalChatMessageSEH(netMsg, msgBuf);

    if (const auto orig = reinterpret_cast<ChatDispatchFn>(
            _InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_origChatDispatchRaw), nullptr, nullptr))) {
        orig(sChat, netMsg, targetChannel, isLocal);
    }

    if (hasLocalText && !tls_inChatCmd) {
        tls_inChatCmd = true;
        const std::uintptr_t prevSChat = tls_activeSChat;
        tls_activeSChat = sChat;
        const bool ok = InvokeChatCommandSEH(msgBuf);
        tls_activeSChat = prevSChat;
        tls_inChatCmd = false;
        if (!ok) {
            app::Log("ui: caught SEH exception while executing chat command '%s'", msgBuf);
        }
    }
}

LRESULT CALLBACK RngKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const bool isKeyDown   = ((lParam & (1LL << 31)) == 0);
        const bool wasDownPrev = ((lParam & (1LL << 30)) != 0);
        if (isKeyDown && !wasDownPrev && !IsGameTextInputActive(g_exeBase)) {
            const int vk = static_cast<int>(wParam);
            if (vk != 0) {
                if (vk == g_cfg.vkToggleTakeover) {
                    const bool next = !MhwRng_IsTakeoverEnabled();
                    MhwRng_SetTakeoverEnabled(next);
                    NotifyChatFmt(u8"[RNG] 全局随机数接管 = %s", next ? u8"开启" : u8"关闭");
                } else if (vk == g_cfg.vkResetEpoch) {
                    const std::uint64_t ep = MhwRng_ResetEpoch();
                    NotifyChatFmt(u8"[RNG] 已重置到第 %llu 轮", static_cast<unsigned long long>(ep));
                }
            }
        }
    }
    return ::CallNextHookEx(g_kbHook, code, wParam, lParam);
}

std::uint64_t __fastcall Hook_PresentForKbInit(std::uintptr_t d3dRender,
                                               std::uint64_t a2,
                                               std::uint64_t a3,
                                               std::uint64_t a4) {
    if (!g_kbHookInstalled.load(std::memory_order_relaxed)) {
        g_kbHookInstalled.store(true, std::memory_order_release);
        g_kbHook = ::SetWindowsHookExW(WH_KEYBOARD, RngKeyboardProc, nullptr, ::GetCurrentThreadId());
        app::Log("init: optional hotkey WH_KEYBOARD installed on window thread (%p)", g_kbHook);
    }
    const auto orig = reinterpret_cast<D3DPresentFn>(
        _InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_origPresentRaw), nullptr, nullptr));
    return orig ? orig(d3dRender, a2, a3, a4) : 0ULL;
}

bool GetModuleSpan(HMODULE mod, std::uintptr_t& outBase, std::uintptr_t& outEnd) noexcept {
    if (!mod) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    outBase = base;
    outEnd  = base + nt->OptionalHeader.SizeOfImage;
    return true;
}

bool IsSiteReady(std::uintptr_t site, const std::uint8_t* expected, std::size_t len) noexcept {
    __try {
        const auto* p = reinterpret_cast<const std::uint8_t*>(site);
        if (std::memcmp(p, expected, len) == 0) return true;
        // If another plugin already installed a 5-byte E9 jump, verify any untouched tail bytes [5..len)
        // so encrypted SteamStub garbage bytes starting with 0xE9 cannot produce a false positive.
        if (p[0] == 0xE9) {
            return (len <= 5U) || (std::memcmp(p + 5U, expected + 5U, len - 5U) == 0);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsSChatInstanceReadySEH(std::uintptr_t exeBase) noexcept {
    if (!exeBase) return false;
    __try {
        const std::uintptr_t p = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSChatInstance);
        return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & 0x7ULL) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryInstallCoreRngHooks(HMODULE exe, std::uintptr_t& outBase, std::uintptr_t& outEnd) {
    std::uintptr_t base = 0, end = 0;
    if (!GetModuleSpan(exe, base, end)) return false;

    // Require all 5 core MtRandom sites + sChat::DispatchChatEntry to be unpacked before touching .text
    if (!IsSiteReady(base + mhw::kRvaMtRandomNrand,       mhw::kPrologueMtRandomNrand,       sizeof(mhw::kPrologueMtRandomNrand)) ||
        !IsSiteReady(base + mhw::kRvaMtRandomCtorSeed,    mhw::kPrologueMtRandomCtorSeed,    sizeof(mhw::kPrologueMtRandomCtorSeed)) ||
        !IsSiteReady(base + mhw::kRvaMtRandomCtorDefault, mhw::kPrologueMtRandomCtorDefault, sizeof(mhw::kPrologueMtRandomCtorDefault)) ||
        !IsSiteReady(base + mhw::kRvaMtRandomSrand,       mhw::kPrologueMtRandomSrand,       sizeof(mhw::kPrologueMtRandomSrand)) ||
        !IsSiteReady(base + mhw::kRvaMtRandomSphere,      mhw::kPrologueMtRandomSphere,      sizeof(mhw::kPrologueMtRandomSphere)) ||
        !IsSiteReady(base + mhw::kRvaSChatDispatchEntry,  mhw::kPrologueSChatDispatch,       sizeof(mhw::kPrologueSChatDispatch))) {
        return false;
    }

    if (!hook::InitArena(base, end)) {
        app::Log("init: error - failed to allocate W^X trampoline arena near %p", reinterpret_cast<void*>(base));
        return false;
    }

    rng::Init(base, end, g_cfg);

    struct HookSpec {
        const char*         name;
        std::uintptr_t      rva;
        const std::uint8_t* prologue;
        std::size_t         prologueLen;
        void*               detour;
        void (*setOrig)(void*) noexcept;
        bool                required;
    };

    const HookSpec specs[] = {
        {"MtRandom::nrand",        mhw::kRvaMtRandomNrand,       mhw::kPrologueMtRandomNrand,       sizeof(mhw::kPrologueMtRandomNrand),       reinterpret_cast<void*>(&rng::Hook_MtRandomNrand),       &rng::SetOrigMtRandomNrand,       true},
        {"MtRandom::ctor(seed)",   mhw::kRvaMtRandomCtorSeed,    mhw::kPrologueMtRandomCtorSeed,    sizeof(mhw::kPrologueMtRandomCtorSeed),    reinterpret_cast<void*>(&rng::Hook_MtRandomCtorSeed),    &rng::SetOrigMtRandomCtorSeed,    true},
        {"MtRandom::ctor_default", mhw::kRvaMtRandomCtorDefault, mhw::kPrologueMtRandomCtorDefault, sizeof(mhw::kPrologueMtRandomCtorDefault), reinterpret_cast<void*>(&rng::Hook_MtRandomCtorDefault), &rng::SetOrigMtRandomCtorDefault, true},
        {"MtRandom::srand",        mhw::kRvaMtRandomSrand,       mhw::kPrologueMtRandomSrand,       sizeof(mhw::kPrologueMtRandomSrand),       reinterpret_cast<void*>(&rng::Hook_MtRandomSrand),       &rng::SetOrigMtRandomSrand,       true},
        {"MtRandom::randSphere",   mhw::kRvaMtRandomSphere,      mhw::kPrologueMtRandomSphere,      sizeof(mhw::kPrologueMtRandomSphere),      reinterpret_cast<void*>(&rng::Hook_MtRandomSphere),      &rng::SetOrigMtRandomSphere,      true},
        {"std::_Random_device",    mhw::kRvaStdRandomDevice,     mhw::kPrologueStdRandomDevice,     sizeof(mhw::kPrologueStdRandomDevice),     reinterpret_cast<void*>(&rng::Hook_StdRandomDevice),     &rng::SetOrigStdRandomDevice,     false},
        {"mt19937_envelope_range", mhw::kRvaMt19937Range,        mhw::kPrologueMt19937Range,        sizeof(mhw::kPrologueMt19937Range),        reinterpret_cast<void*>(&rng::Hook_Mt19937Range),        &rng::SetOrigMt19937Range,        false},
        {"mt19937_heap_init",      mhw::kRvaMt19937HeapInit,     mhw::kPrologueMt19937HeapInit,     sizeof(mhw::kPrologueMt19937HeapInit),     reinterpret_cast<void*>(&rng::Hook_Mt19937HeapInit),     &rng::SetOrigMt19937HeapInit,     false},
        {"mt19937_heap_draw",      mhw::kRvaMt19937HeapDraw,     mhw::kPrologueMt19937HeapDraw,     sizeof(mhw::kPrologueMt19937HeapDraw),     reinterpret_cast<void*>(&rng::Hook_Mt19937HeapDraw),     &rng::SetOrigMt19937HeapDraw,     false},
        {"CRT_rand",               mhw::kRvaCrtRand,             mhw::kPrologueCrtRand,             sizeof(mhw::kPrologueCrtRand),             reinterpret_cast<void*>(&rng::Hook_CrtRand),             &rng::SetOrigCrtRand,             false},
    };

    std::size_t hookedCount = 0;
    for (const HookSpec& sp : specs) {
        void* orig = nullptr;
        const std::uintptr_t site = base + sp.rva;
        if (hook::PrepareDetour(site, sp.prologueLen, sp.detour, &orig, sp.prologue)) {
            sp.setOrig(orig);
            ++hookedCount;
            app::Log("init: prepared %-24s @ %p -> tramp %p", sp.name, reinterpret_cast<void*>(site), orig);
        } else {
            app::Log("init: %s - failed to prepare %s @ %p",
                     sp.required ? "ERROR" : "warning",
                     sp.name,
                     reinterpret_cast<void*>(site));
            if (sp.required) {
                hook::ResetUnsealed();
                return false;
            }
        }
    }

    g_exeBase = base;
    g_exeEnd  = end;

    if (!hook::SealAndActivateAll()) {
        app::Log("init: warning - SealAndActivateAll detected concurrent patch or protect failure, will retry");
        g_exeBase = 0;
        g_exeEnd  = 0;
        return false;
    }

    // Initialize Plugin Host & auto-load sub-plugins BEFORE activating sChat::DispatchChatEntry
    plugin_host::Init(base, end, app::SubPluginDirectory(), &PostChatSystemNoticeSEH);
    std::size_t loadedPlugins = 0U;
    if (g_cfg.autoLoadPlugins) {
        loadedPlugins = plugin_host::LoadAllFromDirectory();
    }

    app::Log("init: activated %zu/10 RNG hooks + %zu sub-plugins (arena=%p, takeover=%s, masterSeed=0x%llX)",
             hookedCount,
             loadedPlugins,
             reinterpret_cast<void*>(hook::ArenaBase()),
             MhwRng_IsTakeoverEnabled() ? "ON" : "OFF",
             static_cast<unsigned long long>(MhwRng_GetMasterSeed()));
    outBase = base;
    outEnd  = end;
    return true;
}

bool InstallChatHookWithRetry(std::uintptr_t base, const char* tag) {
    const std::uintptr_t chatSite = base + mhw::kRvaSChatDispatchEntry;
    for (int retry = 0; retry < 20 && !g_detaching.load(std::memory_order_acquire); ++retry) {
        bool wasChained = false;
        std::uintptr_t chainedTarget = 0;
        std::uintptr_t relay = 0;
        if (hook::InstallStandaloneDetour5(chatSite,
                                           reinterpret_cast<void*>(&Hook_ChatDispatch),
                                           &g_origChatDispatchRaw,
                                           mhw::kPrologueSChatDispatch,
                                           &wasChained,
                                           &chainedTarget,
                                           &relay)) {
            g_chatRelay = relay;
            if (wasChained) {
                app::Log("%s: hooked sChat::DispatchChatEntry @ %p (chained -> %p, tramp=%p)",
                         tag,
                         reinterpret_cast<void*>(chatSite),
                         reinterpret_cast<void*>(chainedTarget),
                         g_origChatDispatchRaw);
            } else {
                app::Log("%s: hooked sChat::DispatchChatEntry @ %p (vanilla, tramp=%p)",
                         tag,
                         reinterpret_cast<void*>(chatSite),
                         g_origChatDispatchRaw);
            }
            return true;
        }
        ::Sleep(25);
    }
    app::Log("%s: ERROR - failed to hook sChat::DispatchChatEntry @ %p after retries",
             tag, reinterpret_cast<void*>(chatSite));
    return false;
}

void InstallOptionalPresentHookWithRetry(std::uintptr_t base) {
    if (g_cfg.vkToggleTakeover == 0 && g_cfg.vkResetEpoch == 0) return;
    const std::uintptr_t presSite = base + mhw::kRvaD3DPresent;
    for (int retry = 0; retry < 20 && !g_detaching.load(std::memory_order_acquire); ++retry) {
        bool wasChained = false;
        std::uintptr_t chainedTarget = 0;
        if (hook::InstallStandaloneDetour5(presSite,
                                           reinterpret_cast<void*>(&Hook_PresentForKbInit),
                                           &g_origPresentRaw,
                                           mhw::kPrologueD3DPresent,
                                           &wasChained,
                                           &chainedTarget,
                                           nullptr)) {
            app::Log("init: hooked optional D3DPresent @ %p (%s)",
                     reinterpret_cast<void*>(presSite), wasChained ? "chained" : "vanilla");
            return;
        }
        ::Sleep(25);
    }
}

DWORD WINAPI InitWorker(LPVOID) {
    app::InitLog();
    g_cfg = app::LoadConfig();
    if (!g_cfg.enabled) {
        app::Log("init: plugin disabled via mhw_rng.ini");
        return 0;
    }

    HMODULE exe = ::GetModuleHandleW(L"MonsterHunterWorld.exe");
    if (!exe) exe = ::GetModuleHandleW(nullptr);
    app::Log("init: MonsterHunterWorld.exe base = %p", exe);
    if (!exe) return 1;

    // Phase 1: Install all 10 core RNG hooks + auto-load sub-plugins immediately on unpack
    std::uintptr_t base = 0, end = 0;
    bool coreReady = false;
    for (int attempt = 0; attempt < 2400 && !g_detaching.load(std::memory_order_acquire); ++attempt) {
        if (TryInstallCoreRngHooks(exe, base, end)) {
            coreReady = true;
            break;
        }
        ::Sleep(50);
    }
    if (!coreReady) {
        app::Log("init: timed out waiting for MonsterHunterWorld.exe unpack");
        return 1;
    }

    // Phase 2: Wait briefly for concurrent startup plugins (e.g. mhw_hfi.dll) to finish their
    // 1-2ms attach-time hook installs on shared UI sites (sChat::DispatchChatEntry / D3DPresent),
    // avoiding cross-DLL TOCTOU overwrites while still activating well before title/hub chat is reachable.
    const DWORD settleStart = ::GetTickCount();
    while (!g_detaching.load(std::memory_order_acquire)) {
        const DWORD elapsed = ::GetTickCount() - settleStart;
        if (elapsed >= 800U && (IsSChatInstanceReadySEH(base) || elapsed >= 3000U)) {
            break;
        }
        ::Sleep(50);
    }
    if (g_detaching.load(std::memory_order_acquire)) return 0;

    if (InstallChatHookWithRetry(base, "init")) {
        app::Log("init: microkernel ready! Type '%s' in game chat.", g_cfg.chatPrefix.c_str());
    }
    InstallOptionalPresentHookWithRetry(base);

    // Phase 3: Two lightweight post-install chain verifications (+1.5s and +4.0s) to self-heal
    // in case a late-loading mod overwrote sChat::DispatchChatEntry without chaining.
    const DWORD checkDelaysMs[2] = {1500U, 2500U};
    const std::uintptr_t chatSite = base + mhw::kRvaSChatDispatchEntry;
    for (DWORD delayMs : checkDelaysMs) {
        for (DWORD slept = 0; slept < delayMs && !g_detaching.load(std::memory_order_acquire); slept += 100U) {
            ::Sleep(100);
        }
        if (g_detaching.load(std::memory_order_acquire)) return 0;
        if (g_chatRelay != 0 && hook::WasUnchainedOverwrite5(chatSite, g_chatRelay, mhw::kPrologueSChatDispatch)) {
            app::Log("chat: detected unchained overwrite at %p, re-chaining...",
                     reinterpret_cast<void*>(chatSite));
            InstallChatHookWithRetry(base, "chat-rechain");
        }
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = module;
        g_detaching.store(false, std::memory_order_release);
        ::DisableThreadLibraryCalls(module);
        HANDLE th = ::CreateThread(nullptr, 0, InitWorker, nullptr, 0, nullptr);
        if (th) ::CloseHandle(th);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        g_detaching.store(true, std::memory_order_release);
        if (g_kbHook) {
            ::UnhookWindowsHookEx(g_kbHook);
            g_kbHook = nullptr;
        }
        plugin_host::ShutdownAll();
        hook::RemoveAllHooks();
        app::CloseLog();
    }
    return TRUE;
}
