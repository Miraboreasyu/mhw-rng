#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#include "command_router.h"
#include "mhw_addresses.h"
#include "mhw_rng_api.h"
#include "peer_beacon.h"
#include "plugin_host.h"
#include "rng_engine.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace cmd_router {
namespace {

struct SavedRngSnapshot {
    std::string               name;
    std::vector<std::uint8_t> blob;
    std::uint64_t             seed  = 0ULL;
    std::uint64_t             epoch = 0ULL;
};

constexpr std::size_t         kMaxSavedSnaps = 16U;
constexpr int                 kMaxExecDepth  = 4;

app::Config*                  g_cfg          = nullptr;
ChatSinkFn                    g_chatSink     = nullptr;
std::mutex                    g_savedSnapsMx;
std::vector<SavedRngSnapshot> g_savedSnaps;

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

void NotifyChatFmt(const char* fmt, ...) {
    char buf[256]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    app::Log("ui: %s", buf);
    if (g_chatSink) {
        g_chatSink(buf);
    }
}

void FormatKernelModeShort(const MhwRngSlotSnapshot& s, char* outBuf, std::size_t cap) {
    if (plugin_host::FormatSlotCustom(&s, outBuf, cap)) return;
    const bool en = app::IsEnglish();
    switch (s.mode) {
        case kMhwRngSlotFixedValue:
            std::snprintf(outBuf, cap, en ? "Fixed u32=0x%08X" : u8"固定u32=0x%08X", s.fixedValue);
            break;
        case kMhwRngSlotCustomSeed:
            std::snprintf(outBuf, cap, en ? "Seed=%llu" : u8"独立种子=%llu", static_cast<unsigned long long>(s.customSeed));
            break;
        case kMhwRngSlotSequence:
            std::snprintf(outBuf, cap, en ? "Seq[%u/%u]" : u8"序列[%u/%u]", s.seqCursor, s.seqLength);
            break;
        case kMhwRngSlotBypass:
            std::snprintf(outBuf, cap, en ? "Vanilla" : u8"原版直通");
            break;
        default:
            std::snprintf(outBuf, cap,
                          s.hasPluginHook ? (en ? "Plugin hook" : u8"插件动态回调")
                                          : (en ? "Master-derived" : u8"跟随总种子"));
            break;
    }
}

bool FileExistsNormal(const std::string& path) noexcept {
    const DWORD attr = ::GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string ResolveScriptPath(const std::string& rawArg) {
    const std::string cleaned = TrimStr(rawArg);
    if (cleaned.empty()) return {};

    std::vector<std::string> baseCandidates;
    const bool isAbs = (cleaned.size() >= 2U && cleaned[1] == ':') ||
                       (!cleaned.empty() && (cleaned[0] == '\\' || cleaned[0] == '/'));
    if (isAbs) {
        baseCandidates.push_back(cleaned);
    } else {
        baseCandidates.push_back(app::SubPluginDirectory() + "\\" + cleaned);
        baseCandidates.push_back(app::DllDirectory() + "\\" + cleaned);
    }

    for (const std::string& c : baseCandidates) {
        if (FileExistsNormal(c)) return c;
        if (c.find('.') == std::string::npos) {
            const std::string withTxt = c + ".txt";
            if (FileExistsNormal(withTxt)) return withTxt;
        }
    }
    return {};
}

bool HandleCommandLineInternal(const char* rawLine, bool requirePrefix, int execDepth) {
    if (!rawLine || rawLine[0] == '\0') return false;
    const bool en = app::IsEnglish();
    const std::string pref = (g_cfg && !g_cfg->chatPrefix.empty()) ? g_cfg->chatPrefix : "$rng";

    const std::string trimmed = TrimStr(rawLine);
    if (trimmed.empty()) return false;

    const std::string lowMsg  = LowerAscii(trimmed);
    const std::string lowPref = LowerAscii(pref);
    std::string restStr;

    if (lowMsg.rfind(lowPref, 0) == 0) {
        if (trimmed.size() == lowPref.size()) {
            restStr.clear();
        } else {
            const unsigned char sep = static_cast<unsigned char>(trimmed[lowPref.size()]);
            if (std::isspace(sep) || sep == '_') {
                restStr = TrimStr(trimmed.substr(lowPref.size() + 1U));
            } else if (requirePrefix) {
                return false;
            } else {
                restStr = trimmed;
            }
        }
    } else {
        if (requirePrefix) return false;
        restStr = trimmed;
    }

    const std::vector<std::string> toks = SplitTokens(restStr);
    const std::string sub = toks.empty() ? "status" : LowerAscii(toks[0]);

    // 1. Kernel Status (`$rng` / `$rng status`)
    if (sub == "status" || sub == "stat") {
        const std::uint32_t estCap = std::min<std::uint32_t>(16384U, std::max<std::uint32_t>(MhwRng_GetActiveSlotCount() + 64U, 256U));
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

        if (en) {
            NotifyChatFmt("[RNG] Mode:%s | Seed:%llu | Epoch:%llu | Slots:%u | Plugins:%zu | Rules:%zu",
                          EngineModeLabel(MhwRng_GetEngineMode()),
                          static_cast<unsigned long long>(MhwRng_GetMasterSeed()),
                          static_cast<unsigned long long>(MhwRng_GetEpoch()),
                          totalSlots,
                          plugins.size(),
                          activeRules.size());
        } else {
            NotifyChatFmt(u8"[RNG] 模式:%s | 总种子:%llu | 第%llu轮 | 活跃槽:%u | 插件:%zu个 | 激活规则:%zu处",
                          EngineModeLabel(MhwRng_GetEngineMode()),
                          static_cast<unsigned long long>(MhwRng_GetMasterSeed()),
                          static_cast<unsigned long long>(MhwRng_GetEpoch()),
                          totalSlots,
                          plugins.size(),
                          activeRules.size());
        }

        if (activeRules.empty()) {
            NotifyChatFmt(en ? "[RNG] Type `%s help` for commands, or `%s plugins` to inspect loaded plugins"
                             : u8"[RNG] 输入 `%s help` 查看核心与插件指令，或 `%s plugins` 查看热重载插件",
                          pref.c_str(), pref.c_str());
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
                NotifyChatFmt("[RNG] [%s] %s%s (%s) = %s",
                              plugin_host::GetPluginShortName(s.ownerPluginId),
                              meta.alias,
                              condTag,
                              meta.description,
                              modeBuf);
            }
            if (activeRules.size() > showN) {
                NotifyChatFmt(en ? "[RNG] ...and %zu more active rule(s)"
                                 : u8"[RNG] ...另有 %zu 处规则激活",
                              activeRules.size() - showN);
            }
        }
        return true;
    }

    // 2. Kernel + Plugin Unified Help (`$rng help [subcmd|plugin]`)
    if (sub == "help" || sub == "?") {
        const auto plugins = plugin_host::ListLoadedPlugins();
        const auto pcmds   = plugin_host::ListPluginSubcommands();
        if (toks.size() >= 2) {
            const std::string flt = LowerAscii(toks[1]);
            std::size_t hits = 0U;
            for (const auto& p : plugins) {
                if (LowerAscii(p.shortName) == flt) {
                    NotifyChatFmt(" - [%s v%s] : %s", p.shortName.c_str(), p.version.c_str(), p.description.c_str());
                    ++hits;
                }
            }
            for (const auto& c : pcmds) {
                if (LowerAscii(c.subcmd) == flt || LowerAscii(c.pluginShortName) == flt) {
                    NotifyChatFmt(" - %s : %s",
                                  c.usageExample.empty() ? c.subcmd.c_str() : c.usageExample.c_str(),
                                  c.helpCn.c_str());
                    ++hits;
                }
            }
            if (hits == 0U) {
                struct CoreHelpItem {
                    const char* keys;
                    const char* usage;
                    const char* descEn;
                    const char* descZh;
                };
                static const CoreHelpItem kCoreHelp[] = {
                    {"status stat", "$rng [status]", "Show kernel mode, seed, epoch, slots & active rules", u8"\u67e5\u770b\u5185\u6838\u6a21\u5f0f\u3001\u603b\u79cd\u5b50\u3001\u8f6e\u6b21\u3001\u6d3b\u8dc3\u69fd\u4e0e\u6fc0\u6d3b\u89c4\u5219"},
                    {"plugins mods", "$rng plugins", "List loaded DLL plugins and command counts", u8"\u5217\u51fa\u5f53\u524d\u5df2\u52a0\u8f7d\u7684\u5b50\u63d2\u4ef6 DLL \u53ca\u5176\u6307\u4ee4\u6570"},
                    {"reload", "$rng reload [name|ini]", "Hot-reload one plugin, or reload INI & all plugins", u8"\u70ed\u91cd\u8f7d\u5355\u4e2a\u63d2\u4ef6\uff0c\u6216\u91cd\u8bfb mhw_rng.ini \u5e76\u70ed\u91cd\u8f7d\u5168\u90e8\u63d2\u4ef6"},
                    {"load", "$rng load <name>", "Load a plugin DLL from nativePC\\plugins\\mhw_rng\\", u8"\u4ece nativePC\\plugins\\mhw_rng\\ \u52a8\u6001\u52a0\u8f7d\u6307\u5b9a\u5b50\u63d2\u4ef6"},
                    {"unload", "$rng unload <name>", "Unload a loaded plugin safely without restarting", u8"\u5b89\u5168\u5378\u8f7d\u5df2\u52a0\u8f7d\u7684\u5b50\u63d2\u4ef6\uff08\u514d\u91cd\u542f\uff09"},
                    {"selective sel partial", "$rng selective", "Selective mode (control overridden/hooked slots only)", u8"\u6309\u9700\u63a5\u7ba1\u6a21\u5f0f\uff08\u4ec5\u63a7\u5236\u5df2\u8bbe\u89c4\u5219/\u63d2\u4ef6\u6302\u94a9\u70b9\u4f4d\uff09"},
                    {"on enable full takeover", "$rng on", "Full deterministic takeover mode (respects SaveGuard)", u8"\u5168\u5c40\u786e\u5b9a\u6027\u63a5\u7ba1\u6a21\u5f0f\uff08\u81ea\u52a8\u4fdd\u62a4\u5b58\u6863\u79cd\u5b50\u94fe\uff09"},
                    {"off disable", "$rng off", "Disable RNG overrides (100% vanilla RNG)", u8"\u5173\u95ed\u63a5\u7ba1\uff08100% \u539f\u7248\u968f\u673a\u6570\uff09"},
                    {"observe obs", "$rng observe", "Observe-only mode (vanilla RNG + telemetry/hints)", u8"\u4ec5\u89c2\u6d4b\u6a21\u5f0f\uff08100% \u539f\u7248\u968f\u6738 + \u8ba1\u6570\u4e0e\u84b8\u6c7d\u673a\u63d0\u793a\uff09"},
                    {"seed", "$rng seed <u64>", "Set 64-bit MasterSeed and advance epoch", u8"\u8bbe\u7f6e 64 \u4f4d\u603b\u79cd\u5b50\u5e76\u91cd\u7f6e\u8f6e\u6b21"},
                    {"reset", "$rng reset", "Advance epoch & rewind all RNG streams to start", u8"\u9012\u589e\u8f6e\u6b21\u5e76\u5c06\u6240\u6709\u968f\u673a\u6d41\u56de\u6eda\u8d77\u70b9"},
                    {"save snap", "$rng save [name]", "Save in-memory RNG slot table snapshot", u8"\u4fdd\u5b58\u5185\u5b58 RNG \u69fd\u4f4d\u72b6\u6001\u5feb\u7167"},
                    {"restore loadstate", "$rng restore [name]", "Restore a previously saved RNG state snapshot", u8"\u6062\u590d\u5df2\u4fdd\u5b58\u7684 RNG \u72b6\u6001\u5feb\u7167"},
                    {"saves", "$rng saves", "List all saved in-memory RNG state snapshots", u8"\u5217\u51fa\u5185\u5b58\u4e2d\u5df2\u4fdd\u5b58\u7684 RNG \u5feb\u7167"},
                    {"clearall", "$rng clearall", "Clear all slot override rules across all plugins", u8"\u6e05\u7a7a\u5168\u90e8\u69fd\u4f4d\u8986\u76d6\u89c4\u5219"},
                    {"lang language", "$rng lang [zh|en|ja|...]", "Query or switch active display language live", u8"\u67e5\u770b\u6216\u5b9e\u65f6\u5207\u6362\u663e\u793a\u8bed\u8a00"},
                    {"chat chatecho", "$rng chat [on|off]", "Toggle mirroring output to MHW native game chat", u8"\u5f00\u5173\u662f\u5426\u540c\u6b65\u8f93\u51fa\u5230\u539f\u7248\u6e38\u620f\u804a\u5929\u6846"},
                    {"exec run script", "$rng exec <file> [quiet]", "Execute batch commands from a script file", u8"\u4ece\u811a\u672c\u6587\u4ef6\u6279\u91cf\u6267\u884c\u547d\u4ee4"},
                    {"rawfix", "$rng rawfix <0xRVA> <u32> [condId]", "Pin raw return RVA to fixed uint32 value", u8"\u6309\u539f\u59cb RVA \u56fa\u5b9a\u8fd4\u56de uint32 \u6570\u503c"},
                    {"rawseed", "$rng rawseed <0xRVA> <seed> [condId]", "Assign independent 64-bit seed to a raw RVA", u8"\u4e3a\u539f\u59cb RVA \u6307\u5b9a\u72ec\u7acb 64 \u4f4d\u79cd\u5b50"},
                    {"rawseq", "$rng rawseq <0xRVA> <v1,v2,...> [loop|hold|prng]", "Inject integer sequence on raw RVA", u8"\u4e3a\u539f\u59cb RVA \u6ce8\u5165\u5faa\u73af/\u5b9a\u683c/\u56de\u9000\u5e8f\u5217"},
                    {"rawbypass", "$rng rawbypass <0xRVA> [condId]", "Force raw RVA to use vanilla RNG", u8"\u5f3a\u5236\u539f\u59cb RVA \u8d70\u539f\u7248\u968f\u673a\u76f4\u901a"},
                    {"rawclear", "$rng rawclear <0xRVA> [condId]", "Clear custom rule on raw RVA", u8"\u6e05\u9664\u539f\u59cb RVA \u7684\u8986\u76d6\u89c4\u5219"},
                    {"clear cls", "clear", "Clear terminal scrollback buffer (console only)", u8"\u6e05\u7a7a\u7ec8\u7aef\u5c4f\u5e55\u8f93\u51fa\u7f13\u51b2\u533a"}
                };
                for (const CoreHelpItem& ch : kCoreHelp) {
                    const std::vector<std::string> ks = SplitTokens(ch.keys);
                    if (std::find(ks.begin(), ks.end(), flt) != ks.end()) {
                        NotifyChatFmt(" - %s : %s", ch.usage, en ? ch.descEn : ch.descZh);
                        ++hits;
                        break;
                    }
                }
            }
            if (hits == 0U) {
                NotifyChatFmt(en ? "[RNG] No subcommand or plugin matched `%s`"
                                 : u8"[RNG] \u672a\u627e\u5230\u5339\u914d `%s` \u7684\u5b50\u547d\u4ee4\u6216\u63d2\u4ef6",
                              toks[1].c_str());
            }
            return true;
        }

        if (en) {
            NotifyChatFmt("[RNG Core] status | selective/on/off/observe | seed <n> | reset | save/restore/saves | clearall");
            NotifyChatFmt("[RNG Sys]  plugins | reload [name|ini] | load/unload <name> | lang <zh|en|...> | chat [on|off] | exec <file> | rawfix/rawseq");
        } else {
            NotifyChatFmt(u8"[RNG \u6838\u5fc3] `$rng` \u72b6\u6001 | `selective/on/off/observe` \u6a21\u5f0f | `seed <n>` | `reset` | `save/restore/saves` | `clearall`");
            NotifyChatFmt(u8"[RNG \u7ba1\u7406] `plugins` | `reload [\u63d2\u4ef6|ini]` | `load/unload <\u63d2\u4ef6>` | `lang <zh|en|...>` | `chat [on|off]` | `exec <\u6587\u4ef6>` | `rawfix/rawseq`");
        }

        // Group subcommands by loaded plugin shortName so plugin ownership is unmistakable
        std::vector<std::string> pluginGroups;
        for (const auto& p : plugins) {
            std::string grp = "[" + p.shortName + "] ";
            bool firstCmd = true;
            for (const auto& c : pcmds) {
                if (c.ownerId != p.id) continue;
                if (!firstCmd) grp += ", ";
                grp += c.subcmd;
                firstCmd = false;
            }
            if (firstCmd) {
                grp += en ? "(passive)" : u8"(\u65e0\u5b50\u547d\u4ee4)";
            }
            pluginGroups.push_back(std::move(grp));
        }

        const char* pPrefix = en ? "[Plugin Cmds] " : u8"[\u63d2\u4ef6\u547d\u4ee4] ";
        std::string line = pPrefix;
        bool firstOnLine = true;
        for (std::size_t i = 0; i < pluginGroups.size(); ++i) {
            const std::string& grp = pluginGroups[i];
            const std::size_t sepLen = firstOnLine ? 0U : 3U;
            if (!firstOnLine && (line.size() + sepLen + grp.size()) > 96U) {
                NotifyChatFmt("%s", line.c_str());
                line        = pPrefix;
                firstOnLine = true;
            }
            if (!firstOnLine) line += " | ";
            line += grp;
            firstOnLine = false;
        }
        if (!firstOnLine) {
            NotifyChatFmt("%s", line.c_str());
        }
        return true;
    }

    // 3. Language Switch (`$rng lang [zh|en|ja|...]`)
    if (sub == "lang" || sub == "language") {
        if (toks.size() < 2) {
            NotifyChatFmt(en ? "[RNG] Language: %s (usage: `%s lang zh|en`)"
                             : u8"[RNG] 当前语言: %s (用法: `%s lang zh|en`)",
                          app::GetLanguage(), pref.c_str());
            return true;
        }
        char normTag[16]{};
        MhwRngLangId normId = kMhwRngLangOther;
        if (app::NormalizeLanguageTag(toks[1].c_str(), normTag, normId)) {
            app::SetLanguage(normTag);
            if (g_cfg) g_cfg->language = app::GetLanguage();
            if (normId == kMhwRngLangZh) {
                NotifyChatFmt(u8"[RNG] 显示语言已切换为 简体中文 (zh)");
            } else if (normId == kMhwRngLangEn) {
                NotifyChatFmt("[RNG] Language switched to English (en)");
            } else {
                NotifyChatFmt("[RNG] Language switched to %s", app::GetLanguage());
            }
        } else {
            NotifyChatFmt(en ? "[RNG] Invalid language `%s`; choose `zh`, `en`, or a standard language code"
                             : u8"[RNG] 无效语言 `%s`；可选 `zh`、`en` 或标准语言代码",
                          toks[1].c_str());
        }
        return true;
    }

    // 3b. Native Game Chat Echo Toggle (`$rng chat [on|off]`)
    if (sub == "chat" || sub == "chatecho") {
        if (!g_cfg) return true;
        if (toks.size() < 2) {
            NotifyChatFmt(en ? "[RNG] Native game chat echo: %s (usage: `%s chat on|off`)"
                             : u8"[RNG] 原版游戏聊天框回显: %s (用法: `%s chat on|off`)",
                          g_cfg->showChatFeedback ? (en ? "ON" : u8"开启") : (en ? "OFF" : u8"关闭"),
                          pref.c_str());
            return true;
        }
        const std::string opt = LowerAscii(toks[1]);
        if (opt == "on" || opt == "1" || opt == "true" || opt == "enable") {
            g_cfg->showChatFeedback = true;
            NotifyChatFmt(en ? "[RNG] Native game chat echo -> ON" : u8"[RNG] 原版游戏聊天框回显 -> 开启");
        } else if (opt == "off" || opt == "0" || opt == "false" || opt == "disable") {
            g_cfg->showChatFeedback = false;
            NotifyChatFmt(en ? "[RNG] Native game chat echo -> OFF (console only)"
                             : u8"[RNG] 原版游戏聊天框回显 -> 关闭 (仅内置终端显示)");
        } else {
            NotifyChatFmt(en ? "[RNG] Usage: `%s chat on|off`" : u8"[RNG] 用法: `%s chat on|off`", pref.c_str());
        }
        return true;
    }

    // 4. Script File Execution (`$rng exec <file> [quiet]`)
    if (sub == "exec" || sub == "run" || sub == "script") {
        if (toks.size() < 2) {
            NotifyChatFmt(en ? "[RNG] Usage: `%s exec <file> [quiet]`"
                             : u8"[RNG] 用法: `%s exec <脚本文件> [quiet]`",
                          pref.c_str());
            return true;
        }
        const bool quiet = (toks.size() >= 3 && LowerAscii(toks[2]) == "quiet");
        ExecuteScriptFile(toks[1], false, quiet, execDepth);
        return true;
    }

    // 5. Plugin Management & Hot-Reload (`plugins`, `reload`, `load`, `unload`)
    if (sub == "plugins" || sub == "mods") {
        const auto list = plugin_host::ListLoadedPlugins();
        if (list.empty()) {
            NotifyChatFmt(en ? "[RNG] No plugins loaded (dir: nativePC\\plugins\\mhw_rng\\*.dll)"
                             : u8"[RNG] 当前未加载任何子插件 (目录: nativePC\\plugins\\mhw_rng\\*.dll)");
            return true;
        }
        NotifyChatFmt(en ? "[RNG] Loaded %zu plugin(s) (`%s reload <name>` to hot-reload):"
                         : u8"[RNG] 已加载 %zu 个插件 (支持 `%s reload <名字>` 免重启热更新)：",
                      list.size(), pref.c_str());
        for (const auto& p : list) {
            if (en) {
                if (!p.description.empty()) {
                    NotifyChatFmt(" - %s v%s : %s (%zu cmd(s))",
                                  p.shortName.c_str(), p.version.c_str(), p.description.c_str(), p.subcmdCount);
                } else {
                    NotifyChatFmt(" - %s v%s (%zu cmd(s))",
                                  p.shortName.c_str(), p.version.c_str(), p.subcmdCount);
                }
            } else {
                NotifyChatFmt(u8" - %s v%s : %s (%zu条指令)",
                              p.shortName.c_str(), p.version.c_str(), p.description.c_str(), p.subcmdCount);
            }
        }
        return true;
    }

    if (sub == "reload") {
        const std::string arg1 = (toks.size() >= 2) ? LowerAscii(toks[1]) : "";
        if (!arg1.empty() && arg1 != "all" && arg1 != "ini" && arg1 != "force") {
            std::string msg;
            const bool ok = plugin_host::LoadOrReloadSingle(toks[1], msg);
            NotifyChatFmt(en ? "[RNG] Hot-reload `%s` %s: %s" : u8"[RNG] 热重载 `%s` %s: %s",
                          toks[1].c_str(),
                          ok ? (en ? "OK" : u8"成功") : (en ? "FAILED" : u8"失败"),
                          msg.c_str());
            return true;
        }

        const bool forceBootIni = (arg1 == "ini" || arg1 == "force");
        app::Config fresh = app::LoadConfig();
        const bool bootModeChangedInIni = !g_cfg || (fresh.bootMode != g_cfg->bootMode);
        const bool seedChangedInIni     = !g_cfg || (fresh.masterSeed != g_cfg->masterSeed);

        if (g_cfg) {
            g_cfg->consoleEnabled   = fresh.consoleEnabled;
            g_cfg->showChatFeedback = fresh.showChatFeedback;
            g_cfg->chatPrefix       = fresh.chatPrefix;
            g_cfg->bootMode         = fresh.bootMode;
            g_cfg->protectSaveSeeds = fresh.protectSaveSeeds;
            g_cfg->language         = fresh.language;
            g_cfg->autoExec         = fresh.autoExec;
            g_cfg->masterSeed       = fresh.masterSeed;
        }
        app::SetLanguage(fresh.language.c_str());
        rng::SetProtectSaveSeeds(fresh.protectSaveSeeds);
        if (forceBootIni || bootModeChangedInIni) {
            MhwRng_SetEngineMode(fresh.bootMode);
        }
        if (forceBootIni || seedChangedInIni) {
            MhwRng_SetMasterSeed(fresh.masterSeed, true);
        }
        rng::ApplyConfigRules(fresh, true);
        std::string pmsg;
        plugin_host::ReloadAll(pmsg);
        if (!fresh.autoExec.empty()) {
            ExecuteScriptFile(fresh.autoExec, true, true, execDepth);
        }
        const bool nowEn = app::IsEnglish();
        NotifyChatFmt(nowEn ? "[RNG] Reloaded INI (Mode:%s | Seed:%llu | SaveGuard:%s) & %s"
                            : u8"[RNG] 已重载 mhw_rng.ini (模式:%s | 总种子:%llu | 存档保护:%s) & %s",
                      EngineModeLabel(MhwRng_GetEngineMode()),
                      static_cast<unsigned long long>(MhwRng_GetMasterSeed()),
                      rng::IsProtectSaveSeedsEnabled() ? (nowEn ? "ON" : u8"开") : (nowEn ? "OFF" : u8"关"),
                      pmsg.c_str());
        return true;
    }

    if (sub == "load") {
        if (toks.size() < 2) {
            NotifyChatFmt(en ? "[RNG] Usage: `%s load <plugin>`" : u8"[RNG] 用法: `%s load <插件名>`", pref.c_str());
            return true;
        }
        std::string msg;
        const bool ok = plugin_host::LoadOrReloadSingle(toks[1], msg);
        NotifyChatFmt(en ? "[RNG] Load %s: %s" : u8"[RNG] 加载%s: %s",
                      ok ? (en ? "OK" : u8"成功") : (en ? "FAILED" : u8"失败"),
                      msg.c_str());
        return true;
    }

    if (sub == "unload") {
        if (toks.size() < 2) {
            NotifyChatFmt(en ? "[RNG] Usage: `%s unload <plugin>`" : u8"[RNG] 用法: `%s unload <插件名>`", pref.c_str());
            return true;
        }
        std::string msg;
        plugin_host::UnloadSingle(toks[1], msg);
        NotifyChatFmt("[RNG] %s", msg.c_str());
        return true;
    }

    // 6. Global Mode / Seed / Epoch / Snapshots / ClearAll
    if (sub == "selective" || sub == "sel" || sub == "partial") {
        MhwRng_SetEngineMode(kMhwRngModeSelective);
        NotifyChatFmt(en ? "[RNG] Mode -> SELECTIVE (controls overridden/hooked slots only; untouched & save seeds run vanilla)"
                         : u8"[RNG] 已切换为【按需接管模式】 (仅控制已设规则/插件挂钩点位，其余与存档种子保持原版随机)");
        return true;
    }

    if (sub == "on" || sub == "enable" || sub == "full" || sub == "takeover") {
        MhwRng_SetEngineMode(kMhwRngModeTakeover);
        NotifyChatFmt(en ? "[RNG] Mode -> TAKEOVER (Seed:%llu, Epoch:%llu | SaveGuard:%s)"
                         : u8"[RNG] 全局随机数接管已【开启】 (总种子:%llu, 第%llu轮 | 存档种子保护:%s)",
                      static_cast<unsigned long long>(MhwRng_GetMasterSeed()),
                      static_cast<unsigned long long>(MhwRng_GetEpoch()),
                      rng::IsProtectSaveSeedsEnabled() ? (en ? "ON" : u8"开") : (en ? "OFF" : u8"关"));
        return true;
    }

    if (sub == "off" || sub == "disable") {
        MhwRng_SetEngineMode(kMhwRngModeOff);
        NotifyChatFmt(en ? "[RNG] Mode -> OFF (pure vanilla RNG)"
                         : u8"[RNG] 全局随机数接管已【关闭】 (完全原版随机)");
        return true;
    }

    if (sub == "observe" || sub == "obs") {
        MhwRng_SetEngineMode(kMhwRngModeObserveOnly);
        NotifyChatFmt(en ? "[RNG] Mode -> OBSERVE ONLY (100%% vanilla RNG + telemetry & Steamworks hints active)"
                         : u8"[RNG] 已切换为【仅观测模式】 (100%%原版结果 + 计数/全局观测继续采集)");
        return true;
    }

    if (sub == "save" || sub == "snap") {
        const std::string snapName = (toks.size() >= 2) ? LowerAscii(toks[1]) : "default";
        std::vector<std::uint8_t> blob;
        std::uint32_t written = 0U;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const std::uint32_t need = MhwRng_CaptureStateBlob(nullptr, 0U);
            if (need == 0U) break;
            const std::uint32_t cap = need + 64U * static_cast<std::uint32_t>(sizeof(MhwRngSlotState));
            blob.resize(cap);
            written = MhwRng_CaptureStateBlob(blob.data(), cap);
            if (written > 0U) break;
        }
        if (written == 0U) {
            NotifyChatFmt(en ? "[RNG] Snapshot capture failed" : u8"[RNG] 快照捕获失败");
            return true;
        }
        blob.resize(written);
        const std::uint64_t sSeed  = MhwRng_GetMasterSeed();
        const std::uint64_t sEpoch = MhwRng_GetEpoch();
        {
            std::lock_guard<std::mutex> lk(g_savedSnapsMx);
            auto it = std::find_if(g_savedSnaps.begin(), g_savedSnaps.end(),
                                   [&](const SavedRngSnapshot& e) { return e.name == snapName; });
            if (it != g_savedSnaps.end()) {
                it->blob  = std::move(blob);
                it->seed  = sSeed;
                it->epoch = sEpoch;
            } else {
                if (g_savedSnaps.size() >= kMaxSavedSnaps) {
                    g_savedSnaps.erase(g_savedSnaps.begin());
                }
                g_savedSnaps.push_back(SavedRngSnapshot{snapName, std::move(blob), sSeed, sEpoch});
            }
        }
        NotifyChatFmt(en ? "[RNG] Saved snapshot `%s` (%u bytes | Epoch:%llu)"
                         : u8"[RNG] 已保存状态快照 `%s` (%u 字节 | 第%llu轮)",
                      snapName.c_str(), written, static_cast<unsigned long long>(sEpoch));
        return true;
    }

    if (sub == "restore" || sub == "loadstate") {
        const std::string snapName = (toks.size() >= 2) ? LowerAscii(toks[1]) : "default";
        std::vector<std::uint8_t> blobCopy;
        {
            std::lock_guard<std::mutex> lk(g_savedSnapsMx);
            auto it = std::find_if(g_savedSnaps.begin(), g_savedSnaps.end(),
                                   [&](const SavedRngSnapshot& e) { return e.name == snapName; });
            if (it != g_savedSnaps.end()) {
                blobCopy = it->blob;
            }
        }
        if (blobCopy.empty()) {
            NotifyChatFmt(en ? "[RNG] Snapshot `%s` not found (`%s saves` to list)"
                             : u8"[RNG] 找不到名为 `%s` 的快照 (`%s saves` 查看列表)",
                          snapName.c_str(), pref.c_str());
            return true;
        }
        if (!MhwRng_RestoreStateBlob(blobCopy.data(), static_cast<std::uint32_t>(blobCopy.size()))) {
            NotifyChatFmt(en ? "[RNG] Failed to restore snapshot `%s` (invalid CRC)"
                             : u8"[RNG] 恢复快照 `%s` 失败 (校验无效)",
                          snapName.c_str());
            return true;
        }
        NotifyChatFmt(en ? "[RNG] Restored snapshot `%s` (Seed:%llu, Epoch:%llu)"
                         : u8"[RNG] 已恢复状态快照 `%s` (总种子:%llu, 第%llu轮)",
                      snapName.c_str(),
                      static_cast<unsigned long long>(MhwRng_GetMasterSeed()),
                      static_cast<unsigned long long>(MhwRng_GetEpoch()));
        return true;
    }

    if (sub == "saves" || sub == "snaps") {
        std::lock_guard<std::mutex> lk(g_savedSnapsMx);
        if (g_savedSnaps.empty()) {
            NotifyChatFmt(en ? "[RNG] No snapshots saved (use `%s save [name]`)"
                             : u8"[RNG] 当前无已保存快照 (用 `%s save [名称]` 保存)",
                          pref.c_str());
            return true;
        }
        NotifyChatFmt(en ? "[RNG] Saved snapshots (%zu/%zu):" : u8"[RNG] 已存快照 (%zu/%zu)：",
                      g_savedSnaps.size(), kMaxSavedSnaps);
        for (const auto& e : g_savedSnaps) {
            NotifyChatFmt(en ? " - %s (%zuB | Seed:%llu | Epoch:%llu)"
                             : u8" - %s (%zuB | 种子:%llu | 第%llu轮)",
                          e.name.c_str(), e.blob.size(),
                          static_cast<unsigned long long>(e.seed),
                          static_cast<unsigned long long>(e.epoch));
        }
        return true;
    }

    if (sub == "reset") {
        const std::uint64_t ep = MhwRng_ResetEpoch();
        NotifyChatFmt(en ? "[RNG] Reset to Epoch %llu (all derived streams rewound & round counters zeroed)"
                         : u8"[RNG] 已重置到第 %llu 轮 (所有随机流回退起点 & 本轮计数清零)",
                      static_cast<unsigned long long>(ep));
        return true;
    }

    if (sub == "seed") {
        if (toks.size() < 2) {
            NotifyChatFmt(en ? "[RNG] Usage: `%s seed <number>`" : u8"[RNG] 用法: `%s seed <数字>`", pref.c_str());
            return true;
        }
        std::uint64_t newSeed = 0;
        if (!app::ParseU64Auto(toks[1].c_str(), newSeed)) {
            NotifyChatFmt(en ? "[RNG] Invalid seed: %s" : u8"[RNG] 种子格式无效: %s", toks[1].c_str());
            return true;
        }
        const std::uint32_t curMode = MhwRng_GetEngineMode();
        if (curMode == kMhwRngModeOff || curMode == kMhwRngModeObserveOnly) {
            MhwRng_SetEngineMode(kMhwRngModeTakeover);
        }
        MhwRng_SetMasterSeed(newSeed, true);
        NotifyChatFmt(en ? "[RNG] MasterSeed = %llu (0x%llX) | Epoch %llu | Mode:%s"
                         : u8"[RNG] 已切换总种子 = %llu (0x%llX) | 第 %llu 轮 | 模式:%s",
                      static_cast<unsigned long long>(newSeed),
                      static_cast<unsigned long long>(newSeed),
                      static_cast<unsigned long long>(MhwRng_GetEpoch()),
                      EngineModeLabel(MhwRng_GetEngineMode()));
        return true;
    }

    if (sub == "clearall") {
        MhwRng_ClearAllSlotOverrides();
        NotifyChatFmt(en ? "[RNG] Cleared all slot fixed/sequence/seed overrides"
                         : u8"[RNG] 已清空全部槽位的固定值/序列/子种子覆盖");
        return true;
    }

    // 7. Low-Level Address Primitives (`rawfix`, `rawseed`, `rawseq`, `rawbypass`, `rawclear`)
    if (sub == "rawfix" || sub == "rawseed" || sub == "rawseq" || sub == "rawbypass" || sub == "rawclear") {
        const std::size_t minArgs = (sub == "rawfix" || sub == "rawseed" || sub == "rawseq") ? 3U : 2U;
        if (toks.size() < minArgs) {
            NotifyChatFmt(en ? "[RNG] Usage: `%s %s <0xRVA> [value|v1,v2,...] [condId]`"
                             : u8"[RNG] 用法: `%s %s <0xRVA> [value|v1,v2,...] [condId]`",
                          pref.c_str(), sub.c_str());
            return true;
        }
        std::uint64_t rawAddr = 0;
        if (!app::ParseU64Auto(toks[1].c_str(), rawAddr)) {
            NotifyChatFmt(en ? "[RNG] Core `%s` only accepts numeric addresses (0xRVA); use plugins for named sites"
                             : u8"[RNG] 核心 `%s` 只接受数值地址(0xRVA)；名字/换算请用对应插件指令",
                          sub.c_str());
            return true;
        }
        const std::uint32_t rva = rng::NormalizeUserSiteAddress(rawAddr);
        if (rva == 0U) {
            NotifyChatFmt(en ? "[RNG] Invalid address: %s" : u8"[RNG] 无效地址: %s", toks[1].c_str());
            return true;
        }
        if (sub == "rawclear") {
            rng::ClearSlotAllConditions(rva);
            NotifyChatFmt(en ? "[RNG] Cleared all conditions on 0x%07X" : u8"[RNG] 已恢复 0x%07X 全部条件槽 -> 跟随总种子", rva);
            return true;
        }
        if (sub == "rawbypass") {
            std::uint64_t cid = 0;
            if (toks.size() >= 3 && !app::ParseU64Auto(toks[2].c_str(), cid)) {
                NotifyChatFmt(en ? "[RNG] Invalid condId: %s" : u8"[RNG] 条件编号无效: %s", toks[2].c_str());
                return true;
            }
            rng::SetCondBypass(kMhwRngPluginIdKernel, rva, static_cast<std::uint32_t>(cid));
            NotifyChatFmt(en ? "[RNG] 0x%07X [Cond:%llu] -> Vanilla bypass"
                             : u8"[RNG] 0x%07X [Cond:%llu] -> 原版直通",
                          rva, static_cast<unsigned long long>(cid));
            return true;
        }
        if (sub == "rawseq") {
            std::vector<std::uint32_t> seqVals;
            const std::string& csv = toks[2];
            std::size_t pos = 0U;
            while (pos < csv.size()) {
                const std::size_t c = csv.find(',', pos);
                const std::string item = TrimStr(csv.substr(pos, c == std::string::npos ? std::string::npos : (c - pos)));
                if (!item.empty()) {
                    std::uint64_t elem = 0;
                    if (!app::ParseU64Auto(item.c_str(), elem) || elem > 0xFFFFFFFFULL) {
                        NotifyChatFmt(en ? "[RNG] Invalid rawseq element `%s` (must fit u32)"
                                         : u8"[RNG] 序列元素无效 `%s` (需为 32 位无符号整数)",
                                      item.c_str());
                        return true;
                    }
                    seqVals.push_back(static_cast<std::uint32_t>(elem));
                }
                if (c == std::string::npos) break;
                pos = c + 1U;
            }
            if (seqVals.empty()) {
                NotifyChatFmt(en ? "[RNG] rawseq values cannot be empty" : u8"[RNG] 序列不能为空");
                return true;
            }

            std::uint8_t pol = kMhwRngSeqLoop;
            std::uint64_t cid = 0ULL;
            if (toks.size() >= 4) {
                const std::string p = LowerAscii(toks[3]);
                if (p == "loop") pol = kMhwRngSeqLoop;
                else if (p == "hold") pol = kMhwRngSeqHoldLast;
                else if (p == "prng" || p == "derived") pol = kMhwRngSeqFallbackDerived;
                else if (toks.size() == 4 && app::ParseU64Auto(toks[3].c_str(), cid)) {
                    // Shorthand: `$rng rawseq <0xRVA> <v1,v2,...> <condId>`
                } else {
                    NotifyChatFmt(en ? "[RNG] Invalid rawseq end policy `%s` (use loop/hold/prng)"
                                     : u8"[RNG] 无效序列策略 `%s` (支持 loop/hold/prng)",
                                  toks[3].c_str());
                    return true;
                }
            }
            if (toks.size() >= 5 && !app::ParseU64Auto(toks[4].c_str(), cid)) {
                NotifyChatFmt(en ? "[RNG] Invalid condId: %s" : u8"[RNG] 条件编号无效: %s", toks[4].c_str());
                return true;
            }

            const bool applied = rng::SetCondSequence(kMhwRngPluginIdKernel,
                                                      rva,
                                                      static_cast<std::uint32_t>(cid),
                                                      seqVals.data(),
                                                      static_cast<std::uint32_t>(seqVals.size()),
                                                      pol);
            if (!applied) {
                NotifyChatFmt(en ? "[RNG] Failed setting rawseq on 0x%07X" : u8"[RNG] 设置 0x%07X 序列失败", rva);
                return true;
            }
            const char* polLbl = (pol == kMhwRngSeqLoop)
                                     ? (en ? "loop" : u8"循环")
                                     : ((pol == kMhwRngSeqHoldLast) ? (en ? "hold" : u8"停在末项")
                                                                    : (en ? "prng" : u8"播完转随机"));
            NotifyChatFmt(en ? "[RNG] 0x%07X [Cond:%llu] -> Seq (%zu steps, %s)"
                             : u8"[RNG] 0x%07X [Cond:%llu] -> 序列 (%zu步, %s)",
                          rva, static_cast<unsigned long long>(cid), seqVals.size(), polLbl);
            return true;
        }

        std::uint64_t val = 0, cid = 0;
        if (!app::ParseU64Auto(toks[2].c_str(), val)) {
            NotifyChatFmt(en ? "[RNG] Invalid value: %s" : u8"[RNG] 数值无效: %s", toks[2].c_str());
            return true;
        }
        if (toks.size() >= 4 && !app::ParseU64Auto(toks[3].c_str(), cid)) {
            NotifyChatFmt(en ? "[RNG] Invalid condId: %s" : u8"[RNG] 条件编号无效: %s", toks[3].c_str());
            return true;
        }
        if (sub == "rawfix") {
            rng::SetCondFixedValue(kMhwRngPluginIdKernel, rva, static_cast<std::uint32_t>(cid), static_cast<std::uint32_t>(val));
            NotifyChatFmt(en ? "[RNG] 0x%07X [Cond:%llu] -> Fixed u32 0x%08X (%u)"
                             : u8"[RNG] 0x%07X [Cond:%llu] -> 原始u32 0x%08X (%u)",
                          rva, static_cast<unsigned long long>(cid),
                          static_cast<std::uint32_t>(val), static_cast<std::uint32_t>(val));
        } else {
            rng::SetCondCustomSeed(kMhwRngPluginIdKernel, rva, static_cast<std::uint32_t>(cid), val);
            NotifyChatFmt(en ? "[RNG] 0x%07X [Cond:%llu] -> CustomSeed %llu"
                             : u8"[RNG] 0x%07X [Cond:%llu] -> 独立种子 %llu",
                          rva, static_cast<unsigned long long>(cid), static_cast<unsigned long long>(val));
        }
        return true;
    }

    // Hidden toggle commands (intentionally omitted from help & tab-completion)
    if (sub == "check" || sub == "self_check") {
        const bool isSelf = (sub == "self_check");
        if (isSelf && toks.size() >= 2U && LowerAscii(toks[1]) == "vis") {
            bool curVis = peer_beacon::IsVideoMarkDebugVisible();
            if (toks.size() >= 3U) {
                const std::string opt = LowerAscii(toks[2]);
                bool next = curVis;
                if (opt == "status" || opt == "stat") {
                    // query only
                } else if (opt == "on" || opt == "1" || opt == "true" || opt == "enable") {
                    next = true;
                } else if (opt == "off" || opt == "0" || opt == "false" || opt == "disable") {
                    next = false;
                } else if (opt == "toggle") {
                    next = !curVis;
                } else {
                    NotifyChatFmt(en ? "[RNG] Invalid argument `%s` (use on/off/status)"
                                     : u8"[RNG] 无效参数 `%s` (可用 on/off/status)",
                                  toks[2].c_str());
                    return true;
                }
                peer_beacon::SetVideoMarkDebugVisible(next);
                curVis = next;
            }
            NotifyChatFmt(en ? "[RNG] self_check vis: %s" : u8"[RNG] self_check vis: %s",
                          curVis ? (en ? "ON" : u8"开启") : (en ? "OFF" : u8"关闭"));
            return true;
        }
        bool cur = isSelf ? peer_beacon::IsSelfCheckEnabled() : peer_beacon::IsCheckEnabled();
        bool isQuery = (toks.size() < 2U);
        if (toks.size() >= 2U) {
            const std::string opt = LowerAscii(toks[1]);
            bool next = cur;
            if (opt == "status" || opt == "stat") {
                isQuery = true;
            } else if (opt == "on" || opt == "1" || opt == "true" || opt == "enable") {
                next = true;
            } else if (opt == "off" || opt == "0" || opt == "false" || opt == "disable") {
                next = false;
            } else if (opt == "toggle") {
                next = !cur;
            } else {
                NotifyChatFmt(en ? "[RNG] Invalid argument `%s` (use on/off/status)"
                                 : u8"[RNG] 无效参数 `%s` (可用 on/off/status)",
                              toks[1].c_str());
                return true;
            }
            if (!isQuery) {
                if (isSelf) {
                    if (next != cur) peer_beacon::ResetDecoders();
                    peer_beacon::SetSelfCheckEnabled(next);
                    if (g_cfg) g_cfg->selfCheckEnabled = next;
                } else {
                    if (next != cur) peer_beacon::ResetDecoders();
                    peer_beacon::SetCheckEnabled(next);
                    if (g_cfg) g_cfg->checkEnabled = next;
                }
                cur = next;
            }
        }
        if (isSelf && isQuery) {
            const peer_beacon::DiagSnapshot diag = peer_beacon::GetDiagSnapshot();
            if (en) {
                NotifyChatFmt("[RNG] self_check: %s | Taint: %s | TX:%u RX:%u | Local:%s",
                              cur ? "ON" : "OFF",
                              diag.activeTaint.empty() ? "none (no RNG override fired yet)" : diag.activeTaint.c_str(),
                              diag.txFrames,
                              diag.rxLocalFrames,
                              diag.localFound ? "OK" : "NONE");
            } else {
                NotifyChatFmt(u8"[RNG] self_check: %s | 活跃污点: %s | 发帧:%u 收帧:%u | 本地实体:%s",
                              cur ? u8"开启" : u8"关闭",
                              diag.activeTaint.empty() ? u8"无(尚无插件触发RNG改写)" : diag.activeTaint.c_str(),
                              diag.txFrames,
                              diag.rxLocalFrames,
                              diag.localFound ? u8"正常" : u8"无");
            }
        } else {
            NotifyChatFmt(en ? "[RNG] %s: %s" : u8"[RNG] %s: %s",
                          sub.c_str(),
                          cur ? (en ? "ON" : u8"开启") : (en ? "OFF" : u8"关闭"));
        }
        return true;
    }

    // 8. Delegate to Registered Plugin Subcommands (`size`, `top`, `dump`, `find`, `fix`, etc.)!
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
        return true;
    }

    NotifyChatFmt(en ? "[RNG] Unknown command `%s`; type `%s help` or `%s plugins`"
                     : u8"[RNG] 未知指令 `%s`；输入 `%s help` 查看可用指令或 `%s plugins` 查看已加载插件",
                  toks[0].c_str(), pref.c_str(), pref.c_str());
    return false;
}

bool HandleCommandLineCpp(const char* rawLine, bool requirePrefix, int execDepth) noexcept {
    try {
        return HandleCommandLineInternal(rawLine, requirePrefix, execDepth);
    } catch (...) {
        app::Log("ui: caught C++ exception executing command");
        return false;
    }
}

bool HandleCommandLineSEH(const char* rawLine, bool requirePrefix, int execDepth) noexcept {
    __try {
        return HandleCommandLineCpp(rawLine, requirePrefix, execDepth);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        app::Log("ui: caught SEH exception executing command");
        return false;
    }
}

} // namespace

void Init(app::Config* cfg, ChatSinkFn chatSink) noexcept {
    g_cfg      = cfg;
    g_chatSink = chatSink;
    if (cfg) {
        app::SetLanguage(cfg->language.c_str());
    }
}

void SetChatSink(ChatSinkFn chatSink) noexcept {
    g_chatSink = chatSink;
}

const char* EngineModeLabel(std::uint32_t mode) noexcept {
    const bool en = app::IsEnglish();
    switch (mode) {
        case kMhwRngModeTakeover:    return en ? "TAKEOVER"  : u8"全局接管";
        case kMhwRngModeSelective:   return en ? "SELECTIVE" : u8"按需接管";
        case kMhwRngModeObserveOnly: return en ? "OBSERVE"   : u8"仅观测";
        case kMhwRngModeOff:
        default:                     return en ? "OFF"       : u8"关闭";
    }
}

const char* EngineModeLabelAscii(std::uint32_t mode) noexcept {
    switch (mode) {
        case kMhwRngModeTakeover:    return "TAKEOVER";
        case kMhwRngModeSelective:   return "SELECTIVE";
        case kMhwRngModeObserveOnly: return "OBSERVE";
        case kMhwRngModeOff:
        default:                     return "OFF";
    }
}

bool HandleCommandLine(const char* rawLine, bool requirePrefix) {
    return HandleCommandLineInternal(rawLine, requirePrefix, 0);
}

bool ExecuteScriptFile(const std::string& fileArg, bool isAutoExec, bool quiet, int depth) {
    const bool en = app::IsEnglish();
    if (depth >= kMaxExecDepth) {
        if (!isAutoExec && !quiet) {
            NotifyChatFmt(en ? "[RNG] exec aborted: nested depth limit (%d) exceeded"
                             : u8"[RNG] exec 中止: 脚本嵌套层数超过上限 (%d)",
                          kMaxExecDepth);
        }
        app::Log("exec: aborted '%s' (depth=%d >= %d)", fileArg.c_str(), depth, kMaxExecDepth);
        return false;
    }

    const std::string resolved = ResolveScriptPath(fileArg);
    if (resolved.empty()) {
        if (!isAutoExec && !quiet) {
            NotifyChatFmt(en ? "[RNG] exec failed: script `%s` not found"
                             : u8"[RNG] exec 失败: 找不到脚本文件 `%s`",
                          fileArg.c_str());
        }
        app::Log("exec: file not found '%s'", fileArg.c_str());
        return false;
    }

    FILE* f = nullptr;
    fopen_s(&f, resolved.c_str(), "r");
    if (!f) {
        if (!isAutoExec && !quiet) {
            NotifyChatFmt(en ? "[RNG] exec failed: cannot open `%s`"
                             : u8"[RNG] exec 失败: 无法打开 `%s`",
                          resolved.c_str());
        }
        return false;
    }

    std::size_t executed = 0U;
    std::size_t lineNo   = 0U;
    char buf[1024]{};
    const ChatSinkFn prevSink = g_chatSink;
    if (isAutoExec || quiet) {
        g_chatSink = nullptr;
    }

    while (std::fgets(buf, sizeof(buf), f)) {
        ++lineNo;
        std::string line(buf);
        if (lineNo == 1U && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        line = TrimStr(line);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';' || (line.size() >= 2U && line[0] == '/' && line[1] == '/')) {
            continue;
        }
        HandleCommandLineSEH(line.c_str(), false, depth + 1);
        ++executed;
    }
    std::fclose(f);
    g_chatSink = prevSink;

    app::Log("%s: executed %zu command(s) from '%s'",
             isAutoExec ? "autoexec" : "exec",
             executed,
             resolved.c_str());
    if (!isAutoExec && !quiet) {
        if (en) {
            NotifyChatFmt("[RNG] Executed %zu command(s) from `%s`", executed, fileArg.c_str());
        } else {
            NotifyChatFmt(u8"[RNG] 已从 `%s` 批量执行 %zu 条指令", fileArg.c_str(), executed);
        }
    }
    return true;
}

std::vector<std::string> ListSavedSnapshotNames() {
    std::lock_guard<std::mutex> lk(g_savedSnapsMx);
    std::vector<std::string> names;
    names.reserve(g_savedSnaps.size());
    for (const auto& s : g_savedSnaps) {
        names.push_back(s.name);
    }
    return names;
}

void ResetSnapshots() {
    std::lock_guard<std::mutex> lk(g_savedSnapsMx);
    g_savedSnaps.clear();
}

} // namespace cmd_router

MHW_RNG_API bool MhwRng_ExecuteCommand(const char* commandLine) {
    if (!commandLine || commandLine[0] == '\0') return false;
    return cmd_router::HandleCommandLineSEH(commandLine, false, 0);
}
