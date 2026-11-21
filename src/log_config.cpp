#include "log_config.h"

#include <windows.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace app {
namespace {

FILE*       g_logFile = nullptr;
std::mutex  g_logMx;
std::string g_dllDir;
std::string g_subPluginDir;
std::once_flag g_dirOnce;

extern "C" IMAGE_DOS_HEADER __ImageBase;

void EnsureDirsInitialized() {
    std::call_once(g_dirOnce, []() {
        char buf[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            g_dllDir = ".";
        } else {
            std::string p(buf, n);
            const std::size_t slash = p.find_last_of("\\/");
            g_dllDir = (slash == std::string::npos) ? "." : p.substr(0, slash);
        }
        g_subPluginDir = g_dllDir + "\\mhw_rng";
    });
}

std::string Trim(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

bool ParseBool(const std::string& raw, bool defVal) {
    const std::string v = ToLowerAscii(Trim(raw));
    if (v == "1" || v == "true" || v == "on" || v == "yes")  return true;
    if (v == "0" || v == "false" || v == "off" || v == "no") return false;
    return defVal;
}

int ParseVk(const std::string& raw, int defVal) {
    const std::string v = Trim(raw);
    if (v.empty()) return defVal;
    const std::string low = ToLowerAscii(v);
    if (low == "none" || low == "off" || low == "0") return 0;
    if (v.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(v[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    }
    if (low.size() >= 2 && low[0] == 'f' && std::isdigit(static_cast<unsigned char>(low[1]))) {
        const int fn = std::atoi(low.c_str() + 1);
        if (fn >= 1 && fn <= 24) return VK_F1 + (fn - 1);
    }
    std::uint64_t num = 0;
    if (ParseU64Auto(v.c_str(), num) && num < 256ULL) {
        return static_cast<int>(num);
    }
    return defVal;
}

bool ParseRawSlotRuleValue(const std::string& rhsRaw, RawSlotRuleConfig& ioRule) {
    const std::string rhs = Trim(rhsRaw);
    const std::string low = ToLowerAscii(rhs);
    if (low == "bypass" || low == "vanilla" || low == "passthrough") {
        ioRule.mode = kMhwRngSlotBypass;
        return true;
    }
    if (low == "derived" || low == "master" || low == "default") {
        ioRule.mode = kMhwRngSlotMasterDerived;
        return true;
    }
    if (low.rfind("fixed:", 0) == 0 || low.rfind("const:", 0) == 0) {
        const std::string valStr = Trim(rhs.substr(rhs.find(':') + 1U));
        std::uint64_t v = 0;
        if (!ParseU64Auto(valStr.c_str(), v)) return false;
        ioRule.mode       = kMhwRngSlotFixedValue;
        ioRule.fixedValue = static_cast<std::uint32_t>(v);
        return true;
    }
    if (low.rfind("seed:", 0) == 0) {
        const std::string valStr = Trim(rhs.substr(5U));
        std::uint64_t s = 0;
        if (!ParseU64Auto(valStr.c_str(), s)) return false;
        ioRule.mode       = kMhwRngSlotCustomSeed;
        ioRule.customSeed = s;
        return true;
    }
    if (low.rfind("seq:", 0) == 0) {
        std::string rest = Trim(rhs.substr(4U));
        ioRule.mode      = kMhwRngSlotSequence;
        ioRule.endPolicy = kMhwRngSeqLoop;

        const std::size_t colon2 = rest.find(':');
        if (colon2 != std::string::npos) {
            const std::string pol = ToLowerAscii(Trim(rest.substr(0, colon2)));
            if (pol == "loop")      ioRule.endPolicy = kMhwRngSeqLoop;
            else if (pol == "hold") ioRule.endPolicy = kMhwRngSeqHoldLast;
            else if (pol == "prng" || pol == "derived") ioRule.endPolicy = kMhwRngSeqFallbackDerived;
            rest = Trim(rest.substr(colon2 + 1U));
        }

        ioRule.sequence.clear();
        std::size_t pos = 0;
        while (pos < rest.size()) {
            const std::size_t comma = rest.find(',', pos);
            const std::string tok = Trim(rest.substr(pos, comma == std::string::npos ? std::string::npos : (comma - pos)));
            if (!tok.empty()) {
                std::uint64_t elem = 0;
                if (!ParseU64Auto(tok.c_str(), elem)) return false;
                ioRule.sequence.push_back(static_cast<std::uint32_t>(elem));
            }
            if (comma == std::string::npos) break;
            pos = comma + 1U;
        }
        return !ioRule.sequence.empty();
    }
    return false;
}

} // namespace

const std::string& DllDirectory() {
    EnsureDirsInitialized();
    return g_dllDir;
}

const std::string& SubPluginDirectory() {
    EnsureDirsInitialized();
    return g_subPluginDir;
}

std::string IniPath() {
    return DllDirectory() + "\\mhw_rng.ini";
}

void InitLog() {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logFile) return;
    const std::string path     = DllDirectory() + "\\mhw_rng.log";
    const std::string prevPath = DllDirectory() + "\\mhw_rng.prev.log";
    ::MoveFileExA(path.c_str(), prevPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    fopen_s(&g_logFile, path.c_str(), "w");
    if (g_logFile) {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        std::fprintf(g_logFile,
                     "=== MHW RNG Microkernel v2 (mhw_rng.dll) started %04d-%02d-%02d %02d:%02d:%02d ===\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::fflush(g_logFile);
    }
}

void CloseLog() {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_logFile) return;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    std::fprintf(g_logFile, "[%02d:%02d:%02d.%03d] [kernel] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_logFile);
    std::fflush(g_logFile);
}

void LogPlugin(const char* pluginTag, const char* msg) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_logFile) return;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    std::fprintf(g_logFile, "[%02d:%02d:%02d.%03d] [plugin:%s] %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 pluginTag ? pluginTag : "?", msg ? msg : "");
    std::fflush(g_logFile);
}

bool ParseU64Auto(const char* str, std::uint64_t& outVal) noexcept {
    if (!str) return false;
    while (*str && std::isspace(static_cast<unsigned char>(*str))) ++str;
    if (*str == '\0' || *str == '-') return false;
    char* endPtr = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(str, &endPtr, 0);
    if (endPtr == str || errno == ERANGE) return false;
    while (*endPtr && std::isspace(static_cast<unsigned char>(*endPtr))) ++endPtr;
    if (*endPtr != '\0') return false;
    outVal = static_cast<std::uint64_t>(v);
    return true;
}

Config LoadConfig() {
    Config cfg{};
    FILE* f = nullptr;
    const std::string path = IniPath();
    fopen_s(&f, path.c_str(), "r");
    if (!f) {
        Log("config: %s not found, using defaults (seed=0x%llX, takeover=%d)",
            path.c_str(),
            static_cast<unsigned long long>(cfg.masterSeed),
            cfg.takeoverOnBoot ? 1 : 0);
        return cfg;
    }

    std::string section;
    char lineBuf[1024]{};
    while (std::fgets(lineBuf, sizeof(lineBuf), f)) {
        std::string line(lineBuf);
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == ';') {
                line.resize(i);
                break;
            }
        }
        line = Trim(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = ToLowerAscii(Trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key  = Trim(line.substr(0, eq));
        const std::string val  = Trim(line.substr(eq + 1U));
        const std::string klow = ToLowerAscii(key);

        if (section == "general") {
            if (klow == "enabled")               cfg.enabled          = ParseBool(val, cfg.enabled);
            else if (klow == "takeoveronboot")   cfg.takeoverOnBoot   = ParseBool(val, cfg.takeoverOnBoot);
            else if (klow == "autoloadplugins")  cfg.autoLoadPlugins  = ParseBool(val, cfg.autoLoadPlugins);
            else if (klow == "showchatfeedback") cfg.showChatFeedback = ParseBool(val, cfg.showChatFeedback);
            else if (klow == "chatprefix" && !val.empty()) cfg.chatPrefix = val;
            else if (klow == "masterseed") {
                std::uint64_t s = 0;
                if (ParseU64Auto(val.c_str(), s)) cfg.masterSeed = s;
            }
        } else if (section == "hotkeys") {
            if (klow == "keytoggletakeover") cfg.vkToggleTakeover = ParseVk(val, 0);
            else if (klow == "keyresetepoch") cfg.vkResetEpoch    = ParseVk(val, 0);
        } else if (section == "slotoverrides") {
            // Kernel [SlotOverrides] accepts numeric RVA/VA with optional `:condId`, e.g. `0x169BA15 = fixed:0` or `0x169BA15:1 = fixed:0`
            std::string addrPart = key;
            std::uint64_t cond = 0;
            const std::size_t colonPos = key.find(':');
            if (colonPos != std::string::npos) {
                addrPart = Trim(key.substr(0, colonPos));
                const std::string condPart = Trim(key.substr(colonPos + 1U));
                if (!ParseU64Auto(condPart.c_str(), cond)) continue;
            }
            std::uint64_t addr = 0;
            if (ParseU64Auto(addrPart.c_str(), addr)) {
                RawSlotRuleConfig rule{};
                rule.rawAddress = addr;
                rule.condId     = static_cast<std::uint32_t>(cond);
                if (ParseRawSlotRuleValue(val, rule)) {
                    cfg.rawSlotRules.push_back(std::move(rule));
                } else {
                    Log("config: invalid [SlotOverrides] rule '%s = %s'", key.c_str(), val.c_str());
                }
            } else {
                Log("config: skipped non-numeric [SlotOverrides] '%s' (domain names belong in plugins)", key.c_str());
            }
        }
    }

    std::fclose(f);
    Log("config: loaded %s (enabled=%d, takeover=%d, autoLoadPlugins=%d, masterSeed=0x%llX, rawRules=%zu)",
        path.c_str(),
        cfg.enabled ? 1 : 0,
        cfg.takeoverOnBoot ? 1 : 0,
        cfg.autoLoadPlugins ? 1 : 0,
        static_cast<unsigned long long>(cfg.masterSeed),
        cfg.rawSlotRules.size());
    return cfg;
}

} // namespace app
