#pragma once
#include "mhw_rng_api.h"

#include <cstddef>
#include <string>
#include <vector>

namespace plugin_host {

struct LoadedPluginSummary {
    MhwRngPluginId id = 0;
    std::string    shortName;
    std::string    displayName;
    std::string    version;
    std::string    description;
    std::string    fileName;
    std::size_t    subcmdCount = 0;
};

struct SubcommandHelpEntry {
    MhwRngPluginId ownerId = 0;
    std::string    pluginShortName;
    std::string    subcmd;
    std::string    usageExample;
    std::string    helpCn;
};

using ChatPrintSinkFn = void(*)(const char* utf8Text) noexcept;
using AsyncChatSinkFn = void(*)(const char* utf8Text) noexcept;

void Init(std::uintptr_t exeBase,
          std::uintptr_t exeEnd,
          const std::string& subPluginDir,
          ChatPrintSinkFn chatSink,
          AsyncChatSinkFn asyncChatSink = nullptr);

void ShutdownAll();

// Shadow-copy hot-loading of `nativePC\plugins\mhw_rng\*.dll`
std::size_t LoadAllFromDirectory();
bool        LoadOrReloadSingle(const std::string& nameOrFile, std::string& outMsgCn);
bool        UnloadSingle(const std::string& nameOrFile, std::string& outMsgCn);
std::size_t ReloadAll(std::string& outMsgCn);

// Dispatches `$rng <subcmd> ...` to a registered plugin subcommand if matched
bool DispatchPluginSubcommand(const std::string& subcmdLower,
                              const std::vector<std::string>& argsAfterSubcmd,
                              const std::string& rawArgsAfterSubcmd);

// Metadata & formatting queries (used by Kernel `$rng` status and by plugins like `inspector`)
bool        QuerySiteMeta(std::uint32_t rva, MhwRngSiteMeta* outMeta);
bool        FormatSlotCustom(const MhwRngSlotSnapshot* snap, char* outBuf, std::size_t bufCap);
const char* GetPluginShortName(MhwRngPluginId id) noexcept;

std::vector<LoadedPluginSummary> ListLoadedPlugins();
std::vector<SubcommandHelpEntry> ListPluginSubcommands();

const MhwRngHostApi* GetHostApiTable() noexcept;

} // namespace plugin_host
