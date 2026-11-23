#pragma once
#include "mhw_rng_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

struct RawSlotRuleConfig {
    std::uint64_t              rawAddress = 0; // Numeric RVA or 0x14... VA
    std::uint32_t              condId     = 0; // Optional `:condId` suffix in key (`0x169BA15:1 = fixed:0`)
    MhwRngSlotMode             mode       = kMhwRngSlotMasterDerived;
    MhwRngSeqEndPolicy         endPolicy  = kMhwRngSeqLoop;
    std::uint32_t              fixedValue = 0;
    std::uint64_t              customSeed = 0;
    std::vector<std::uint32_t> sequence;
};

struct Config {
    bool                           enabled          = true;
    bool                           takeoverOnBoot   = true;
    std::uint32_t                  bootMode         = kMhwRngModeSelective;
    bool                           protectSaveSeeds = true;
    bool                           autoLoadPlugins  = true;
    std::uint64_t                  masterSeed       = 12345ULL;
    bool                           consoleEnabled   = true;
    bool                           showChatFeedback = false;
    std::string                    chatPrefix       = "$rng";
    std::string                    language         = "zh";
    std::string                    autoExec;
    int                            vkToggleConsole  = 0xC0; // VK_OEM_3 ('`' / '~')
    int                            vkToggleTakeover = 0;
    int                            vkResetEpoch     = 0;
    std::vector<RawSlotRuleConfig> rawSlotRules;
};

const std::string& DllDirectory();
const std::string& SubPluginDirectory();
std::string        IniPath();

void        InitLog();
void        CloseLog();
void        Log(const char* fmt, ...);
void        LogPlugin(const char* pluginTag, const char* msg);

using LanguageChangedSinkFn = void(*)(std::uint32_t prevLangId, std::uint32_t newLangId) noexcept;

bool          NormalizeLanguageTag(const char* raw, char outTag[16], MhwRngLangId& outId) noexcept;
void          SetLanguageChangedSink(LanguageChangedSinkFn fn) noexcept;
void          SetLanguage(const char* lang) noexcept;
const char*   GetLanguage() noexcept;
std::uint32_t GetLanguageId() noexcept;
bool          IsEnglish() noexcept;

Config      LoadConfig();
bool   ParseU64Auto(const char* str, std::uint64_t& outVal) noexcept;

} // namespace app
