#pragma once
#include "log_config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace console {

enum ColorKind : std::uint8_t {
    kColorNormal   = 0,
    kColorCommand  = 1,
    kColorKernel   = 2,
    kColorPlugin   = 3,
    kColorError    = 4
};

struct OutputLine {
    std::string  text;
    std::uint8_t colorKind = kColorNormal;
};

struct ConsoleSnapshot {
    std::vector<OutputLine> lines;       // Window slice of lines to display (oldest..newest)
    std::size_t             totalLines   = 0;
    std::size_t             scrollOffset = 0; // Lines scrolled up from bottom
    std::string             inputLine;
    std::size_t             cursorByte   = 0;
    std::string             searchPrompt;   // Non-empty when Ctrl+R reverse-i-search is active
    std::string             completionHint; // Non-empty when multiple Tab candidates exist
};

void          Init(app::Config* cfg) noexcept;
void          Reset() noexcept;

bool          IsOpen() noexcept;
void          SetOpen(bool open) noexcept;
bool          ToggleOpen() noexcept;
std::uint64_t Generation() noexcept;

// Scrollback output (thread-safe; callable from any worker/hook thread)
void          PushOutputLine(const char* utf8Text) noexcept;
void          ClearScrollback() noexcept;
void          ScrollBy(int deltaLines) noexcept;
void          ClampScrollOffset(std::size_t maxScrollOffset) noexcept;

// Readline input handling
bool          OnKeyDown(int vk, bool ctrl, bool shift, bool alt) noexcept;
void          InsertUtf8(const char* utf8Text) noexcept;
void          InsertUtf16(const wchar_t* wstr, int len) noexcept;

// Pending submitted commands (drained on game main thread)
bool          PopPendingCommand(std::string& outCmd) noexcept;

// Thread-safe render snapshot (`maxLines == 0` returns all buffered lines)
void          CaptureSnapshot(std::size_t maxLines, ConsoleSnapshot& out);

} // namespace console
