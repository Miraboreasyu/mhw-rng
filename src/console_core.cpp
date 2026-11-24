#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "console_core.h"
#include "command_router.h"
#include "mhw_rng_api.h"
#include "plugin_host.h"
#include "rng_engine.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace console {
namespace {

constexpr std::size_t kMaxScrollbackLines = 512U;
constexpr std::size_t kMaxHistoryEntries  = 128U;
constexpr std::size_t kMaxInputBytes      = 512U;

struct TabSession {
    bool                     active     = false;
    std::size_t              tokStart   = 0U;
    std::size_t              tokEnd     = 0U;
    std::size_t              cycleIdx   = 0U;
    std::vector<std::string> candidates;
};

struct CompletionContext {
    std::uint64_t            genSnapshot = 0ULL;
    std::size_t              tokStart    = 0U;
    std::size_t              tokEnd      = 0U;
    std::string              prefix;
    std::vector<std::string> priorTokens;
    std::string              chatPrefix;
    bool                     valid       = false;
};

app::Config*               g_cfg = nullptr;
std::atomic<bool>          g_open{false};
std::atomic<std::uint64_t> g_gen{1ULL};
std::mutex                 g_mx;

std::deque<OutputLine>     g_lines;
std::size_t                g_scrollOffset = 0U;
std::string                g_input;
std::size_t                g_cursor = 0U;
std::string                g_killBuf;

std::deque<std::string>    g_history;
std::size_t                g_historyCursor = 0U; // g_history.size() means draft line
std::string                g_historyDraft;

bool                       g_reverseSearch    = false;
std::string                g_reverseQuery;
std::string                g_reverseSavedLine;
std::size_t                g_reverseSavedCursor = 0U;
std::size_t                g_reverseMatchIdx    = 0U;
bool                       g_reverseHasMatch    = false;

TabSession                 g_tab;
std::deque<std::string>    g_pendingCmds;

std::atomic<bool>          g_hasImeComp{false};
std::string                g_imeComp;
std::size_t                g_imeCompCursor    = 0U;
std::vector<std::string>   g_imeCandidates;
std::size_t                g_imeCandSelected  = 0U;
std::size_t                g_imeCandPageStart = 0U;
std::size_t                g_imeCandTotal     = 0U;

inline void BumpGenLocked() noexcept {
    g_gen.fetch_add(1ULL, std::memory_order_release);
}

void ClearImeStateLocked(bool& outChanged) noexcept {
    outChanged = !g_imeComp.empty() || !g_imeCandidates.empty() ||
                 g_imeCompCursor != 0U || g_imeCandSelected != 0U ||
                 g_imeCandPageStart != 0U || g_imeCandTotal != 0U ||
                 g_hasImeComp.load(std::memory_order_relaxed);
    g_imeComp.clear();
    g_imeCompCursor = 0U;
    g_imeCandidates.clear();
    g_imeCandSelected  = 0U;
    g_imeCandPageStart = 0U;
    g_imeCandTotal     = 0U;
    g_hasImeComp.store(false, std::memory_order_release);
}

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::string TrimStr(const std::string& s) {
    std::size_t b = 0U;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1U]))) --e;
    return s.substr(b, e - b);
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const std::string h = LowerAscii(haystack);
    const std::string n = LowerAscii(needle);
    return h.find(n) != std::string::npos;
}

bool StartsWithCaseInsensitive(const std::string& str, const std::string& prefix) {
    if (prefix.size() > str.size()) return false;
    for (std::size_t i = 0U; i < prefix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(str[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

std::size_t PrevUtf8Boundary(const std::string& s, std::size_t pos) noexcept {
    if (pos == 0U || s.empty()) return 0U;
    if (pos > s.size()) pos = s.size();
    --pos;
    while (pos > 0U && (static_cast<unsigned char>(s[pos]) & 0xC0U) == 0x80U) {
        --pos;
    }
    return pos;
}

std::size_t NextUtf8Boundary(const std::string& s, std::size_t pos) noexcept {
    if (pos >= s.size()) return s.size();
    ++pos;
    while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0U) == 0x80U) {
        ++pos;
    }
    return pos;
}

std::size_t PrevWordBoundary(const std::string& s, std::size_t pos) noexcept {
    if (pos == 0U || s.empty()) return 0U;
    if (pos > s.size()) pos = s.size();
    while (pos > 0U && std::isspace(static_cast<unsigned char>(s[pos - 1U]))) {
        --pos;
    }
    while (pos > 0U && !std::isspace(static_cast<unsigned char>(s[pos - 1U]))) {
        pos = PrevUtf8Boundary(s, pos);
    }
    return pos;
}

std::size_t NextWordBoundary(const std::string& s, std::size_t pos) noexcept {
    const std::size_t n = s.size();
    if (pos >= n) return n;
    while (pos < n && !std::isspace(static_cast<unsigned char>(s[pos]))) {
        pos = NextUtf8Boundary(s, pos);
    }
    while (pos < n && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    return pos;
}

std::string Utf16ToUtf8(const wchar_t* wstr, int len) {
    if (!wstr || len == 0) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wstr, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wstr, len, &out[0], need, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::wstring Utf8ToUtf16(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], need);
    return out;
}

std::uint8_t ClassifyLineColor(const std::string& line) {
    if (line.empty()) return kColorNormal;
    if (line.size() >= 2U && line[0] == '>' && line[1] == ' ') return kColorCommand;
    if (ContainsCaseInsensitive(line, "error") ||
        ContainsCaseInsensitive(line, "failed") ||
        ContainsCaseInsensitive(line, "warning") ||
        ContainsCaseInsensitive(line, "unknown") ||
        ContainsCaseInsensitive(line, "invalid") ||
        line.find(u8"\u8B66\u544A") != std::string::npos || // 警告
        line.find(u8"\u5931\u8D25") != std::string::npos || // 失败
        line.find(u8"\u672A\u77E5") != std::string::npos || // 未知
        line.find(u8"\u65E0\u6548") != std::string::npos) { // 无效
        return kColorError;
    }
    if (line.rfind("[RNG", 0) == 0 || line.rfind("[Plugins]", 0) == 0 ||
        line.rfind("[Plugin Cmds]", 0) == 0 ||
        line.rfind(u8"[\u63D2\u4EF6", 0) == 0 ||
        line.rfind(u8"[\u5DF2\u8F7D\u5165", 0) == 0) {
        return kColorKernel;
    }
    if (line[0] == '[') return kColorPlugin;
    return kColorNormal;
}

void PushLineInternalLocked(std::string line, std::uint8_t kind) {
    if (g_lines.size() >= kMaxScrollbackLines) {
        g_lines.pop_front();
    }
    g_lines.push_back(OutputLine{std::move(line), kind});
    if (g_scrollOffset > 0U && g_scrollOffset < g_lines.size()) {
        // Keep viewport steady when user scrolled up to read earlier output
        ++g_scrollOffset;
    }
    BumpGenLocked();
}

void ResetTabLocked() noexcept {
    if (g_tab.active || !g_tab.candidates.empty()) {
        g_tab.active   = false;
        g_tab.tokStart = 0U;
        g_tab.tokEnd   = 0U;
        g_tab.cycleIdx = 0U;
        g_tab.candidates.clear();
    }
}

void UpdateReverseSearchLocked(bool startFromCurrentIdx) {
    if (g_history.empty()) {
        g_reverseHasMatch = false;
        return;
    }
    std::size_t idx = startFromCurrentIdx
                          ? std::min(g_reverseMatchIdx, g_history.size())
                          : g_history.size();
    while (idx > 0U) {
        --idx;
        if (ContainsCaseInsensitive(g_history[idx], g_reverseQuery)) {
            g_reverseMatchIdx = idx;
            g_reverseHasMatch = true;
            g_input           = g_history[idx];
            g_cursor          = g_input.size();
            return;
        }
    }
    g_reverseHasMatch = false;
}

void InsertUtf8Locked(const std::string& utf8) {
    if (utf8.empty()) return;
    ResetTabLocked();
    if (g_reverseSearch) {
        for (std::size_t i = 0U; i < utf8.size();) {
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
            if (c == '\r' || c == '\n' || c == '\t' || c < 0x20U || c == 0x7FU) {
                ++i;
                continue;
            }
            std::size_t seqLen = 1U;
            if ((c & 0xE0U) == 0xC0U) seqLen = 2U;
            else if ((c & 0xF0U) == 0xE0U) seqLen = 3U;
            else if ((c & 0xF8U) == 0xF0U) seqLen = 4U;
            if (i + seqLen > utf8.size()) seqLen = utf8.size() - i;
            if (g_reverseQuery.size() + seqLen > 128U) break;
            g_reverseQuery.append(utf8, i, seqLen);
            i += seqLen;
        }
        UpdateReverseSearchLocked(false);
        BumpGenLocked();
        return;
    }

    std::string clean;
    clean.reserve(utf8.size());
    for (std::size_t i = 0U; i < utf8.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c == '\r' || c == '\n' || c == '\t') {
            clean.push_back(' ');
        } else if (c >= 0x20U && c != 0x7FU) {
            clean.push_back(static_cast<char>(c));
        }
    }
    if (clean.empty()) return;

    const std::size_t avail = (g_input.size() < kMaxInputBytes) ? (kMaxInputBytes - g_input.size()) : 0U;
    if (avail == 0U) return;
    if (clean.size() > avail) {
        std::size_t cut = avail;
        while (cut > 0U && (static_cast<unsigned char>(clean[cut]) & 0xC0U) == 0x80U) --cut;
        clean.resize(cut);
    }
    if (clean.empty()) return;
    if (g_cursor > g_input.size()) g_cursor = g_input.size();
    g_input.insert(g_cursor, clean);
    g_cursor += clean.size();
    BumpGenLocked();
}

void AddCandidateUnique(std::vector<std::string>& out, const std::string& cand) {
    if (cand.empty()) return;
    for (const auto& existing : out) {
        if (LowerAscii(existing) == LowerAscii(cand)) return;
    }
    out.push_back(cand);
}

void ScanDirectoryFiles(const std::string& dir, const char* pattern, bool stripExt,
                        bool stripRngPrefix, std::vector<std::string>& out) {
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd{};
    const std::string search = dir + "\\" + pattern;
    HANDLE h = ::FindFirstFileA(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        std::string name(fd.cFileName);
        if (stripExt) {
            const std::size_t dot = name.find_last_of('.');
            if (dot != std::string::npos) name.resize(dot);
        }
        if (stripRngPrefix) {
            const std::string low = LowerAscii(name);
            if (low.rfind("mhw_rng_", 0) == 0 && name.size() > 8U) {
                AddCandidateUnique(out, name.substr(8U));
                continue;
            }
            if (low.rfind("rng_", 0) == 0 && name.size() > 4U) {
                AddCandidateUnique(out, name.substr(4U));
                continue;
            }
        }
        AddCandidateUnique(out, name);
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
}

void ExtractUsageKeywords(const std::string& usage, const std::string& subcmdLower,
                          std::vector<std::string>& out) {
    if (usage.empty()) return;
    std::string cur;
    bool inAngle = false;
    auto flushTok = [&]() {
        if (cur.empty()) return;
        const std::string low = LowerAscii(cur);
        cur.clear();
        if (low == "$rng" || low == subcmdLower) return;
        bool allDigits = true;
        bool hasAlpha  = false;
        for (unsigned char c : low) {
            if (!std::isdigit(c)) allDigits = false;
            if (std::isalpha(c)) hasAlpha = true;
        }
        if (allDigits || !hasAlpha) return;
        AddCandidateUnique(out, low);
    };

    for (unsigned char c : usage) {
        if (c == '<') {
            flushTok();
            inAngle = true;
            continue;
        }
        if (c == '>') {
            inAngle = false;
            continue;
        }
        if (inAngle) continue;
        if (std::isalnum(c) || c == '_' || c == '.' || c == '-') {
            cur.push_back(static_cast<char>(c));
        } else {
            flushTok();
        }
    }
    flushTok();
}

std::vector<std::string> BuildCompletionCandidates(const CompletionContext& ctx) {
    std::vector<std::string> pool;
    const auto loadedPlugins = plugin_host::ListLoadedPlugins();
    const auto pluginSubcmds = plugin_host::ListPluginSubcommands();

    std::size_t skip = 0U;
    const std::string chatPrefLow = LowerAscii(ctx.chatPrefix.empty() ? "$rng" : ctx.chatPrefix);
    if (!ctx.priorTokens.empty() && LowerAscii(ctx.priorTokens[0]) == chatPrefLow) {
        skip = 1U;
    }

    const std::size_t logicalPos = ctx.priorTokens.size() - skip;
    static const char* const kCoreCmds[] = {
        "status", "help", "plugins", "reload", "load", "unload",
        "selective", "on", "off", "observe", "lang", "chat", "exec",
        "seed", "reset", "save", "restore", "saves", "clearall",
        "rawfix", "rawseed", "rawseq", "rawbypass", "rawclear", "clear"
    };

    if (logicalPos == 0U) {
        for (const char* c : kCoreCmds) AddCandidateUnique(pool, c);
        for (const auto& sc : pluginSubcmds) AddCandidateUnique(pool, sc.subcmd);
    } else {
        const std::string cmd = LowerAscii(ctx.priorTokens[skip]);
        const std::size_t argIdx = logicalPos; // 1 = first arg after cmd

        if (cmd == "help" || cmd == "?") {
            if (argIdx == 1U) {
                for (const char* c : kCoreCmds) AddCandidateUnique(pool, c);
                for (const auto& sc : pluginSubcmds) AddCandidateUnique(pool, sc.subcmd);
                for (const auto& lp : loadedPlugins) AddCandidateUnique(pool, lp.shortName);
            }
        } else if (cmd == "reload") {
            if (argIdx == 1U) {
                AddCandidateUnique(pool, "all");
                AddCandidateUnique(pool, "ini");
                for (const auto& lp : loadedPlugins) AddCandidateUnique(pool, lp.shortName);
            }
        } else if (cmd == "unload") {
            if (argIdx == 1U) {
                for (const auto& lp : loadedPlugins) AddCandidateUnique(pool, lp.shortName);
            }
        } else if (cmd == "load") {
            if (argIdx == 1U) {
                ScanDirectoryFiles(app::SubPluginDirectory(), "*.dll", true, true, pool);
            }
        } else if (cmd == "lang" || cmd == "language") {
            if (argIdx == 1U) {
                AddCandidateUnique(pool, "zh");
                AddCandidateUnique(pool, "en");
                AddCandidateUnique(pool, "ja");
                AddCandidateUnique(pool, "ko");
            }
        } else if (cmd == "chat" || cmd == "chatecho") {
            if (argIdx == 1U) {
                AddCandidateUnique(pool, "on");
                AddCandidateUnique(pool, "off");
            }
        } else if (cmd == "save" || cmd == "snap" || cmd == "restore" || cmd == "loadstate") {
            if (argIdx == 1U) {
                for (const auto& s : cmd_router::ListSavedSnapshotNames()) {
                    AddCandidateUnique(pool, s);
                }
            }
        } else if (cmd == "exec" || cmd == "run" || cmd == "script") {
            if (argIdx == 1U) {
                ScanDirectoryFiles(app::SubPluginDirectory(), "*.txt", false, false, pool);
                ScanDirectoryFiles(app::DllDirectory(), "*.txt", false, false, pool);
            } else if (argIdx == 2U) {
                AddCandidateUnique(pool, "quiet");
            }
        } else if (cmd == "rawseq") {
            if (argIdx == 3U) {
                AddCandidateUnique(pool, "loop");
                AddCandidateUnique(pool, "hold");
                AddCandidateUnique(pool, "prng");
            }
        } else {
            std::vector<std::string> priorArgs;
            if (ctx.priorTokens.size() > (skip + 1U)) {
                priorArgs.assign(ctx.priorTokens.begin() + static_cast<std::ptrdiff_t>(skip + 1U),
                                 ctx.priorTokens.end());
            }
            std::vector<std::string> plugCands;
            if (plugin_host::CompletePluginSubcommand(cmd, priorArgs, ctx.prefix, plugCands)) {
                for (const auto& c : plugCands) {
                    AddCandidateUnique(pool, c);
                }
            } else if (argIdx == 1U) {
                // Fallback for 3rd-party plugins without a registered completer (1st argument only)
                for (const auto& sc : pluginSubcmds) {
                    if (LowerAscii(sc.subcmd) == cmd) {
                        ExtractUsageKeywords(sc.usageExample, cmd, pool);
                    }
                }
            }
        }
    }

    std::vector<std::string> matched;
    for (const auto& cand : pool) {
        if (StartsWithCaseInsensitive(cand, ctx.prefix)) {
            matched.push_back(cand);
        }
    }
    std::sort(matched.begin(), matched.end(), [](const std::string& a, const std::string& b) {
        return LowerAscii(a) < LowerAscii(b);
    });
    return matched;
}

std::string LongestCommonPrefixCI(const std::vector<std::string>& list) {
    if (list.empty()) return {};
    std::string lcp = list[0];
    for (std::size_t i = 1U; i < list.size(); ++i) {
        std::size_t j = 0U;
        while (j < lcp.size() && j < list[i].size() &&
               std::tolower(static_cast<unsigned char>(lcp[j])) ==
                   std::tolower(static_cast<unsigned char>(list[i][j]))) {
            ++j;
        }
        lcp.resize(j);
        if (lcp.empty()) break;
    }
    return lcp;
}

std::string FormatTabHintBar(const std::vector<std::string>& cands, std::size_t activeIdx, bool showActive) {
    if (cands.empty()) return {};
    std::string out;
    if (showActive) {
        char head[32]{};
        std::snprintf(head, sizeof(head), "[%zu/%zu] ", activeIdx + 1U, cands.size());
        out += head;
    } else {
        char head[32]{};
        std::snprintf(head, sizeof(head), "[%zu] ", cands.size());
        out += head;
    }

    const std::size_t maxShow = 8U;
    std::size_t start = 0U;
    if (showActive && cands.size() > maxShow && activeIdx >= maxShow / 2U) {
        start = std::min(activeIdx - maxShow / 2U, cands.size() - maxShow);
    }
    const std::size_t end = std::min(cands.size(), start + maxShow);
    if (start > 0U) out += "... ";
    for (std::size_t i = start; i < end; ++i) {
        if (i > start) out += "  ";
        if (showActive && i == activeIdx) {
            out += "[" + cands[i] + "]";
        } else {
            out += cands[i];
        }
    }
    if (end < cands.size()) out += " ...";
    return out;
}

void HandleTabCompletion(bool reverseCycle) {
    CompletionContext ctx{};
    bool cycleExisting = false;
    std::size_t nextIdx = 0U;

    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_reverseSearch) return;
        if (g_tab.active && g_tab.candidates.size() > 1U &&
            g_tab.tokStart <= g_input.size() && g_tab.tokEnd <= g_input.size() &&
            g_tab.tokStart <= g_tab.tokEnd) {
            cycleExisting = true;
            const std::size_t n = g_tab.candidates.size();
            nextIdx = reverseCycle ? ((g_tab.cycleIdx + n - 1U) % n) : ((g_tab.cycleIdx + 1U) % n);
            g_tab.cycleIdx = nextIdx;
            const std::string& rep = g_tab.candidates[nextIdx];
            g_input.replace(g_tab.tokStart, g_tab.tokEnd - g_tab.tokStart, rep);
            g_tab.tokEnd = g_tab.tokStart + rep.size();
            g_cursor     = g_tab.tokEnd;
            BumpGenLocked();
            return;
        }

        const std::size_t cur = std::min(g_cursor, g_input.size());
        std::size_t tokStart = cur;
        while (tokStart > 0U && !std::isspace(static_cast<unsigned char>(g_input[tokStart - 1U]))) {
            --tokStart;
        }
        std::size_t tokEnd = cur;
        while (tokEnd < g_input.size() && !std::isspace(static_cast<unsigned char>(g_input[tokEnd]))) {
            ++tokEnd;
        }

        // Tokenize prior text [0, tokStart)
        std::size_t pos = 0U;
        while (pos < tokStart) {
            while (pos < tokStart && std::isspace(static_cast<unsigned char>(g_input[pos]))) ++pos;
            if (pos >= tokStart) break;
            std::size_t q = pos;
            while (q < tokStart && !std::isspace(static_cast<unsigned char>(g_input[q]))) ++q;
            ctx.priorTokens.push_back(g_input.substr(pos, q - pos));
            pos = q;
        }

        ctx.genSnapshot = g_gen.load(std::memory_order_relaxed);
        ctx.tokStart    = tokStart;
        ctx.tokEnd      = tokEnd;
        ctx.prefix      = g_input.substr(tokStart, cur - tokStart);
        ctx.chatPrefix  = (g_cfg && !g_cfg->chatPrefix.empty()) ? g_cfg->chatPrefix : "$rng";
        ctx.valid       = true;
    }

    if (cycleExisting || !ctx.valid) return;
    std::vector<std::string> matches = BuildCompletionCandidates(ctx);
    if (matches.empty()) return;

    std::lock_guard<std::mutex> lk(g_mx);
    // Abort if another thread modified input while unlocked
    if (g_gen.load(std::memory_order_relaxed) != ctx.genSnapshot) return;

    if (matches.size() == 1U) {
        std::string rep = matches[0];
        const bool endsWithDelim = (!rep.empty() && (rep.back() == ':' || rep.back() == '='));
        const bool appendSpace = !endsWithDelim &&
                                 ((ctx.tokEnd >= g_input.size()) ||
                                  !std::isspace(static_cast<unsigned char>(g_input[ctx.tokEnd])));
        if (appendSpace) rep.push_back(' ');
        g_input.replace(ctx.tokStart, ctx.tokEnd - ctx.tokStart, rep);
        g_cursor = ctx.tokStart + rep.size();
        ResetTabLocked();
        BumpGenLocked();
        return;
    }

    const std::string lcp = LongestCommonPrefixCI(matches);
    if (lcp.size() > ctx.prefix.size()) {
        g_input.replace(ctx.tokStart, ctx.tokEnd - ctx.tokStart, lcp);
        g_tab.active     = true;
        g_tab.tokStart   = ctx.tokStart;
        g_tab.tokEnd     = ctx.tokStart + lcp.size();
        g_tab.cycleIdx   = static_cast<std::size_t>(-1);
        g_tab.candidates = std::move(matches);
        g_cursor         = g_tab.tokEnd;
    } else {
        const std::size_t idx = reverseCycle ? (matches.size() - 1U) : 0U;
        const std::string& rep = matches[idx];
        g_input.replace(ctx.tokStart, ctx.tokEnd - ctx.tokStart, rep);
        g_tab.active     = true;
        g_tab.tokStart   = ctx.tokStart;
        g_tab.tokEnd     = ctx.tokStart + rep.size();
        g_tab.cycleIdx   = idx;
        g_tab.candidates = std::move(matches);
        g_cursor         = g_tab.tokEnd;
    }
    BumpGenLocked();
}

void CopyLastOutputToClipboardLocked() {
    if (g_lines.empty()) return;
    const std::size_t take = std::min<std::size_t>(g_lines.size(), 16U);
    std::string combined;
    for (std::size_t i = g_lines.size() - take; i < g_lines.size(); ++i) {
        combined += g_lines[i].text;
        combined += "\r\n";
    }
    const std::wstring w = Utf8ToUtf16(combined);
    if (w.empty()) return;
    if (!::OpenClipboard(nullptr)) return;
    ::EmptyClipboard();
    const SIZE_T bytes = (w.size() + 1U) * sizeof(wchar_t);
    HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        void* dst = ::GlobalLock(hg);
        if (dst) {
            std::memcpy(dst, w.c_str(), bytes);
            ::GlobalUnlock(hg);
        }
        ::SetClipboardData(CF_UNICODETEXT, hg);
    }
    ::CloseClipboard();
}

void PasteClipboardLocked() {
    if (!::OpenClipboard(nullptr)) return;
    HANDLE hData = ::GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const auto* wptr = static_cast<const wchar_t*>(::GlobalLock(hData));
        if (wptr) {
            const std::string u8 = Utf16ToUtf8(wptr, -1);
            ::GlobalUnlock(hData);
            ::CloseClipboard();
            InsertUtf8Locked(u8);
            return;
        }
    }
    ::CloseClipboard();
}

void SubmitCurrentInputLocked() {
    ResetTabLocked();
    {
        bool unused = false;
        ClearImeStateLocked(unused);
    }
    if (g_reverseSearch) {
        g_reverseSearch = false;
        g_reverseQuery.clear();
        g_reverseSavedLine.clear();
    }

    const std::string trimmed = TrimStr(g_input);
    g_input.clear();
    g_cursor        = 0U;
    g_scrollOffset  = 0U;
    g_historyDraft.clear();
    if (trimmed.empty()) {
        g_historyCursor = g_history.size();
        BumpGenLocked();
        return;
    }

    PushLineInternalLocked("> " + trimmed, kColorCommand);
    if (g_history.empty() || g_history.back() != trimmed) {
        if (g_history.size() >= kMaxHistoryEntries) g_history.pop_front();
        g_history.push_back(trimmed);
    }
    g_historyCursor = g_history.size();

    std::string cmdCheck = LowerAscii(trimmed);
    const std::string prefLow = LowerAscii((g_cfg && !g_cfg->chatPrefix.empty()) ? g_cfg->chatPrefix : "$rng");
    if (cmdCheck.rfind(prefLow + " ", 0) == 0) {
        cmdCheck = TrimStr(cmdCheck.substr(prefLow.size() + 1U));
    }
    if (cmdCheck == "clear" || cmdCheck == "cls") {
        g_lines.clear();
        g_scrollOffset = 0U;
        BumpGenLocked();
        return;
    }

    g_pendingCmds.push_back(trimmed);
    BumpGenLocked();
}

} // namespace

void Init(app::Config* cfg) noexcept {
    std::lock_guard<std::mutex> lk(g_mx);
    g_cfg = cfg;
}

void Reset() noexcept {
    g_open.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_mx);
    g_lines.clear();
    g_scrollOffset = 0U;
    g_input.clear();
    g_cursor = 0U;
    g_killBuf.clear();
    g_history.clear();
    g_historyCursor = 0U;
    g_historyDraft.clear();
    g_reverseSearch = false;
    g_reverseQuery.clear();
    g_reverseSavedLine.clear();
    g_reverseSavedCursor = 0U;
    g_reverseMatchIdx    = 0U;
    g_reverseHasMatch    = false;
    ResetTabLocked();
    {
        bool unused = false;
        ClearImeStateLocked(unused);
    }
    g_pendingCmds.clear();
    BumpGenLocked();
}

bool IsOpen() noexcept {
    return g_open.load(std::memory_order_acquire);
}

void SetOpen(bool open) noexcept {
    const bool prev = g_open.exchange(open, std::memory_order_acq_rel);
    if (prev != open) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!open) {
            ResetTabLocked();
            bool unused = false;
            ClearImeStateLocked(unused);
            if (g_reverseSearch) {
                g_reverseSearch = false;
                g_reverseQuery.clear();
            }
        }
        BumpGenLocked();
    }
}

bool ToggleOpen() noexcept {
    std::lock_guard<std::mutex> lk(g_mx);
    const bool next = !g_open.load(std::memory_order_acquire);
    g_open.store(next, std::memory_order_release);
    if (!next) {
        ResetTabLocked();
        bool unused = false;
        ClearImeStateLocked(unused);
        if (g_reverseSearch) {
            g_reverseSearch = false;
            g_reverseQuery.clear();
        }
    }
    BumpGenLocked();
    return next;
}

std::uint64_t Generation() noexcept {
    return g_gen.load(std::memory_order_acquire);
}

void PushOutputLine(const char* utf8Text) noexcept {
    if (!utf8Text || utf8Text[0] == '\0') return;
    std::lock_guard<std::mutex> lk(g_mx);
    std::string chunk;
    for (const char* p = utf8Text; *p != '\0'; ++p) {
        if (*p == '\r') continue;
        if (*p == '\n') {
            if (!chunk.empty()) {
                const std::uint8_t k = ClassifyLineColor(chunk);
                PushLineInternalLocked(std::move(chunk), k);
                chunk.clear();
            }
        } else {
            chunk.push_back(*p);
        }
    }
    if (!chunk.empty()) {
        const std::uint8_t k = ClassifyLineColor(chunk);
        PushLineInternalLocked(std::move(chunk), k);
    }
}

void ClearScrollback() noexcept {
    std::lock_guard<std::mutex> lk(g_mx);
    g_lines.clear();
    g_scrollOffset = 0U;
    BumpGenLocked();
}

void ScrollBy(int deltaLines) noexcept {
    if (deltaLines == 0) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_lines.empty()) {
        g_scrollOffset = 0U;
        return;
    }
    const std::ptrdiff_t maxCoarse = static_cast<std::ptrdiff_t>(g_lines.size() * 8U);
    std::ptrdiff_t next = static_cast<std::ptrdiff_t>(g_scrollOffset) + static_cast<std::ptrdiff_t>(deltaLines);
    if (next < 0) next = 0;
    if (next > maxCoarse) next = maxCoarse;
    if (static_cast<std::size_t>(next) != g_scrollOffset) {
        g_scrollOffset = static_cast<std::size_t>(next);
        BumpGenLocked();
    }
}

void ClampScrollOffset(std::size_t maxScrollOffset) noexcept {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_scrollOffset > maxScrollOffset) {
        g_scrollOffset = maxScrollOffset;
        BumpGenLocked();
    }
}

bool OnKeyDown(int vk, bool ctrl, bool shift, bool alt) noexcept {
    if (vk == VK_TAB && !ctrl && !alt) {
        HandleTabCompletion(shift);
        return true;
    }

    std::lock_guard<std::mutex> lk(g_mx);

    // Clear active Tab bar on any key other than Tab / modifier keys
    if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
        vk != VK_LSHIFT && vk != VK_RSHIFT && vk != VK_LCONTROL && vk != VK_RCONTROL) {
        if (vk != VK_ESCAPE) {
            ResetTabLocked();
        }
    }

    if (ctrl && !alt && vk == 'R') {
        if (!g_reverseSearch) {
            g_reverseSearch      = true;
            g_reverseSavedLine   = g_input;
            g_reverseSavedCursor = g_cursor;
            g_reverseQuery.clear();
            g_reverseMatchIdx = g_history.size();
            UpdateReverseSearchLocked(false);
        } else {
            UpdateReverseSearchLocked(true);
        }
        BumpGenLocked();
        return true;
    }

    if (vk == VK_ESCAPE || (ctrl && !alt && vk == 'G')) {
        if (g_reverseSearch) {
            g_reverseSearch = false;
            g_reverseQuery.clear();
            g_input  = std::move(g_reverseSavedLine);
            g_cursor = std::min(g_reverseSavedCursor, g_input.size());
            BumpGenLocked();
            return true;
        }
        if (g_tab.active) {
            ResetTabLocked();
            BumpGenLocked();
            return true;
        }
        if (vk == VK_ESCAPE) {
            g_open.store(false, std::memory_order_release);
            bool unused = false;
            ClearImeStateLocked(unused);
            BumpGenLocked();
        }
        return true;
    }

    if (vk == VK_RETURN) {
        SubmitCurrentInputLocked();
        return true;
    }

    if (vk == VK_PRIOR) {
        if (!g_lines.empty()) {
            const std::size_t maxCoarse = g_lines.size() * 8U;
            g_scrollOffset = std::min(maxCoarse, g_scrollOffset + 10U);
            BumpGenLocked();
        }
        return true;
    }
    if (vk == VK_NEXT) {
        g_scrollOffset = (g_scrollOffset > 10U) ? (g_scrollOffset - 10U) : 0U;
        BumpGenLocked();
        return true;
    }

    // Backspace / Ctrl+H / Ctrl+Backspace
    if ((vk == VK_BACK && !alt) || (ctrl && !alt && vk == 'H')) {
        if (g_reverseSearch) {
            if (!g_reverseQuery.empty()) {
                g_reverseQuery.resize(PrevUtf8Boundary(g_reverseQuery, g_reverseQuery.size()));
                UpdateReverseSearchLocked(false);
                BumpGenLocked();
            }
            return true;
        }
        if (ctrl && vk == VK_BACK) {
            if (g_cursor > 0U) {
                const std::size_t wb = PrevWordBoundary(g_input, g_cursor);
                g_killBuf = g_input.substr(wb, g_cursor - wb);
                g_input.erase(wb, g_cursor - wb);
                g_cursor = wb;
                BumpGenLocked();
            }
            return true;
        }
        if (g_cursor > 0U) {
            const std::size_t prev = PrevUtf8Boundary(g_input, g_cursor);
            g_input.erase(prev, g_cursor - prev);
            g_cursor = prev;
            BumpGenLocked();
        }
        return true;
    }

    // Exit reverse-i-search into normal editing before cursor motion/line editing
    auto acceptReverseSearch = [&]() {
        if (g_reverseSearch) {
            g_reverseSearch = false;
            g_reverseQuery.clear();
            g_reverseSavedLine.clear();
            BumpGenLocked();
        }
    };

    if (ctrl && !alt && vk == 'L') {
        acceptReverseSearch();
        g_lines.clear();
        g_scrollOffset = 0U;
        BumpGenLocked();
        return true;
    }

    if (ctrl && !alt && vk == 'C') {
        if (shift && g_input.empty()) {
            CopyLastOutputToClipboardLocked();
            return true;
        }
        if (g_reverseSearch || !g_input.empty()) {
            acceptReverseSearch();
            PushLineInternalLocked("> " + g_input + "^C", kColorError);
            g_input.clear();
            g_cursor        = 0U;
            g_historyCursor = g_history.size();
            g_historyDraft.clear();
            BumpGenLocked();
        }
        return true;
    }

    if ((ctrl && !alt && vk == 'V') || (shift && !ctrl && !alt && vk == VK_INSERT)) {
        acceptReverseSearch();
        PasteClipboardLocked();
        return true;
    }

    if (vk == VK_LEFT || (ctrl && !alt && vk == 'B')) {
        acceptReverseSearch();
        g_cursor = (ctrl && vk == VK_LEFT) ? PrevWordBoundary(g_input, g_cursor)
                                           : PrevUtf8Boundary(g_input, g_cursor);
        BumpGenLocked();
        return true;
    }
    if (alt && !ctrl && vk == 'B') {
        acceptReverseSearch();
        g_cursor = PrevWordBoundary(g_input, g_cursor);
        BumpGenLocked();
        return true;
    }

    if (vk == VK_RIGHT || (ctrl && !alt && vk == 'F')) {
        acceptReverseSearch();
        g_cursor = (ctrl && vk == VK_RIGHT) ? NextWordBoundary(g_input, g_cursor)
                                            : NextUtf8Boundary(g_input, g_cursor);
        BumpGenLocked();
        return true;
    }
    if (alt && !ctrl && vk == 'F') {
        acceptReverseSearch();
        g_cursor = NextWordBoundary(g_input, g_cursor);
        BumpGenLocked();
        return true;
    }

    if (vk == VK_HOME || (ctrl && !alt && vk == 'A')) {
        acceptReverseSearch();
        g_cursor = 0U;
        BumpGenLocked();
        return true;
    }
    if (vk == VK_END || (ctrl && !alt && vk == 'E')) {
        acceptReverseSearch();
        g_cursor = g_input.size();
        BumpGenLocked();
        return true;
    }

    if ((vk == VK_DELETE && !ctrl && !alt) || (ctrl && !alt && vk == 'D')) {
        if (ctrl && vk == 'D' && g_input.empty() && !g_reverseSearch) {
            g_open.store(false, std::memory_order_release);
            bool unused = false;
            ClearImeStateLocked(unused);
            BumpGenLocked();
            return true;
        }
        acceptReverseSearch();
        if (g_cursor < g_input.size()) {
            const std::size_t next = NextUtf8Boundary(g_input, g_cursor);
            g_input.erase(g_cursor, next - g_cursor);
            BumpGenLocked();
        }
        return true;
    }

    if (ctrl && !alt && vk == 'W') {
        acceptReverseSearch();
        if (g_cursor > 0U) {
            const std::size_t wb = PrevWordBoundary(g_input, g_cursor);
            g_killBuf = g_input.substr(wb, g_cursor - wb);
            g_input.erase(wb, g_cursor - wb);
            g_cursor = wb;
            BumpGenLocked();
        }
        return true;
    }
    if (ctrl && !alt && vk == 'U') {
        acceptReverseSearch();
        if (g_cursor > 0U) {
            g_killBuf = g_input.substr(0U, g_cursor);
            g_input.erase(0U, g_cursor);
            g_cursor = 0U;
            BumpGenLocked();
        }
        return true;
    }
    if (ctrl && !alt && vk == 'K') {
        acceptReverseSearch();
        if (g_cursor < g_input.size()) {
            g_killBuf = g_input.substr(g_cursor);
            g_input.erase(g_cursor);
            BumpGenLocked();
        }
        return true;
    }
    if (ctrl && !alt && vk == 'Y') {
        acceptReverseSearch();
        InsertUtf8Locked(g_killBuf);
        return true;
    }

    if (vk == VK_UP || (ctrl && !alt && vk == 'P')) {
        acceptReverseSearch();
        if (!g_history.empty() && g_historyCursor > 0U) {
            if (g_historyCursor == g_history.size()) {
                g_historyDraft = g_input;
            }
            --g_historyCursor;
            g_input  = g_history[g_historyCursor];
            g_cursor = g_input.size();
            BumpGenLocked();
        }
        return true;
    }
    if (vk == VK_DOWN || (ctrl && !alt && vk == 'N')) {
        acceptReverseSearch();
        if (g_historyCursor < g_history.size()) {
            ++g_historyCursor;
            g_input  = (g_historyCursor == g_history.size()) ? g_historyDraft : g_history[g_historyCursor];
            g_cursor = g_input.size();
            BumpGenLocked();
        }
        return true;
    }

    return false;
}

void InsertUtf8(const char* utf8Text) noexcept {
    if (!utf8Text || utf8Text[0] == '\0') return;
    std::lock_guard<std::mutex> lk(g_mx);
    InsertUtf8Locked(utf8Text);
}

void InsertUtf16(const wchar_t* wstr, int len) noexcept {
    if (!wstr || len <= 0) return;
    const std::string u8 = Utf16ToUtf8(wstr, len);
    if (u8.empty()) return;
    std::lock_guard<std::mutex> lk(g_mx);
    InsertUtf8Locked(u8);
}

bool HasImeComposition() noexcept {
    return g_hasImeComp.load(std::memory_order_acquire);
}

void SetImeCompositionUtf16(const wchar_t* compStr, int compLen, int cursorUtf16Idx) noexcept {
    std::string fullU8;
    std::size_t cursorU8 = 0U;
    if (compStr && compLen > 0) {
        const int curIdx = std::max(0, std::min(cursorUtf16Idx, compLen));
        const std::string headU8 = (curIdx > 0) ? Utf16ToUtf8(compStr, curIdx) : std::string{};
        const std::string tailU8 = (curIdx < compLen) ? Utf16ToUtf8(compStr + curIdx, compLen - curIdx) : std::string{};
        cursorU8 = headU8.size();
        fullU8   = headU8 + tailU8;
    }

    std::lock_guard<std::mutex> lk(g_mx);
    if (fullU8.empty()) {
        const bool changed = !g_imeComp.empty() || g_imeCompCursor != 0U;
        g_imeComp.clear();
        g_imeCompCursor = 0U;
        g_hasImeComp.store(!g_imeCandidates.empty(), std::memory_order_release);
        if (changed) BumpGenLocked();
        return;
    }
    ResetTabLocked();
    g_imeComp       = std::move(fullU8);
    g_imeCompCursor = cursorU8;
    g_hasImeComp.store(true, std::memory_order_release);
    BumpGenLocked();
}

void SetImeCandidatesUtf16(const std::vector<std::wstring>& pageItems,
                           std::size_t selectedInPage,
                           std::size_t pageStart,
                           std::size_t totalCount) noexcept {
    std::vector<std::string> itemsU8;
    itemsU8.reserve(pageItems.size());
    for (const auto& ws : pageItems) {
        if (ws.empty()) {
            itemsU8.emplace_back();
        } else {
            itemsU8.push_back(Utf16ToUtf8(ws.c_str(), static_cast<int>(ws.size())));
        }
    }

    std::lock_guard<std::mutex> lk(g_mx);
    g_imeCandidates    = std::move(itemsU8);
    g_imeCandSelected  = g_imeCandidates.empty() ? 0U : std::min(selectedInPage, g_imeCandidates.size() - 1U);
    g_imeCandPageStart = g_imeCandidates.empty() ? 0U : pageStart;
    g_imeCandTotal     = g_imeCandidates.empty() ? 0U : std::max(totalCount, g_imeCandidates.size());
    g_hasImeComp.store(!g_imeComp.empty() || !g_imeCandidates.empty(), std::memory_order_release);
    BumpGenLocked();
}

void ClearImeCandidates() noexcept {
    std::lock_guard<std::mutex> lk(g_mx);
    const bool changed = !g_imeCandidates.empty() || g_imeCandSelected != 0U ||
                         g_imeCandPageStart != 0U || g_imeCandTotal != 0U;
    g_imeCandidates.clear();
    g_imeCandSelected  = 0U;
    g_imeCandPageStart = 0U;
    g_imeCandTotal     = 0U;
    g_hasImeComp.store(!g_imeComp.empty(), std::memory_order_release);
    if (changed) BumpGenLocked();
}

void ClearImeComposition() noexcept {
    std::lock_guard<std::mutex> lk(g_mx);
    bool changed = false;
    ClearImeStateLocked(changed);
    if (changed) BumpGenLocked();
}

bool PopPendingCommand(std::string& outCmd) noexcept {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_pendingCmds.empty()) return false;
    outCmd = std::move(g_pendingCmds.front());
    g_pendingCmds.pop_front();
    return true;
}

void CaptureSnapshot(std::size_t maxLines, ConsoleSnapshot& out) {
    out.lines.clear();
    std::lock_guard<std::mutex> lk(g_mx);
    out.totalLines        = g_lines.size();
    out.scrollOffset      = g_scrollOffset;
    out.inputLine         = g_input;
    out.cursorByte        = g_cursor;
    out.imeCompText       = g_imeComp;
    out.imeCompCursorByte = g_imeCompCursor;
    out.imeCandidates     = g_imeCandidates;
    out.imeCandSelected   = g_imeCandSelected;
    out.imeCandPageStart  = g_imeCandPageStart;
    out.imeCandTotal      = g_imeCandTotal;

    if (g_reverseSearch) {
        out.searchPrompt = std::string(g_reverseHasMatch ? "(reverse-i-search)`" : "(failed reverse-i-search)`") +
                           g_reverseQuery + "': ";
    } else {
        out.searchPrompt.clear();
    }

    if (g_tab.active && g_tab.candidates.size() > 1U) {
        const bool showActive = (g_tab.cycleIdx < g_tab.candidates.size());
        out.completionHint = FormatTabHintBar(g_tab.candidates, showActive ? g_tab.cycleIdx : 0U, showActive);
    } else {
        out.completionHint.clear();
    }

    if (g_lines.empty()) return;
    std::size_t endIdx = g_lines.size();
    std::size_t startIdx = 0U;
    if (maxLines > 0U) {
        const std::size_t maxOff = (g_lines.size() > maxLines) ? (g_lines.size() - maxLines) : 0U;
        const std::size_t effOff = std::min(g_scrollOffset, maxOff);
        endIdx   = g_lines.size() - effOff;
        startIdx = (endIdx > maxLines) ? (endIdx - maxLines) : 0U;
    }
    out.lines.reserve(endIdx - startIdx);
    for (std::size_t i = startIdx; i < endIdx; ++i) {
        out.lines.push_back(g_lines[i]);
    }
}

} // namespace console
