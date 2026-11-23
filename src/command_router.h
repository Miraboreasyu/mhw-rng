#pragma once
#include "log_config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cmd_router {

using ChatSinkFn = void(*)(const char* utf8Text) noexcept;

void        Init(app::Config* cfg, ChatSinkFn chatSink) noexcept;
void        SetChatSink(ChatSinkFn chatSink) noexcept;

const char* EngineModeLabel(std::uint32_t mode) noexcept;
const char* EngineModeLabelAscii(std::uint32_t mode) noexcept;

// Executes a single command line (up to 1024B). When `requirePrefix` is true, ignores lines not starting with `$rng`.
bool        HandleCommandLine(const char* rawLine, bool requirePrefix);

// Executes a multi-line script file (`#`, `;`, `//` comments ignored). Prefix `$rng` is optional inside scripts.
bool        ExecuteScriptFile(const std::string& fileArg, bool isAutoExec, bool quiet = false, int depth = 0);

// Snapshot inspection & cleanup helpers.
std::vector<std::string> ListSavedSnapshotNames();
void                     ResetSnapshots();

} // namespace cmd_router
