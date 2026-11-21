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
    bool                           autoLoadPlugins  = true;
    std::uint64_t                  masterSeed       = 12345ULL;
    bool                           showChatFeedback = true;
    std::string                    chatPrefix       = "$rng";
    int                            vkToggleTakeover = 0;
    int                            vkResetEpoch     = 0;
    std::vector<RawSlotRuleConfig> rawSlotRules;
};

const std::string& DllDirectory();
const std::string& SubPluginDirectory();
std::string        IniPath();

void   InitLog();
void   CloseLog();
void   Log(const char* fmt, ...);
void   LogPlugin(const char* pluginTag, const char* msg);

Config LoadConfig();
bool   ParseU64Auto(const char* str, std::uint64_t& outVal) noexcept;

} // namespace app
