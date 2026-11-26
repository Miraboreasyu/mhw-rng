// ============================================================================
//  MHW Universal RNG Microkernel (mhw_rng.dll)
//  Pure Interception + Composite Condition Slot Table + Hot-Reload Plugin Bus
// ============================================================================
#include "audio_mark.h"
#include "command_router.h"
#include "console_core.h"
#include "console_overlay.h"
#include "hook.h"
#include "hud_mark.h"
#include "log_config.h"
#include "mhw_addresses.h"
#include "mhw_rng_api.h"
#include "peer_beacon.h"
#include "plugin_host.h"
#include "rng_engine.h"

#include <windows.h>
#include <imm.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace {

using ChatDispatchFn     = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint8_t);
using SChatMoveFn        = void(__fastcall*)(std::uintptr_t);
using SChatShowMessageFn = void(__fastcall*)(std::uintptr_t, const char*, float, std::uint32_t, std::uint8_t);
using D3DPresentFn       = std::uint64_t(__fastcall*)(std::uintptr_t, std::uint64_t, std::uint64_t, std::uint64_t);
using SKeyboardMoveFn    = void(__fastcall*)(std::uintptr_t);
using UMhGuiMoveAfterFn  = void(__fastcall*)(std::uintptr_t);

struct ImmApi {
    using FnGetContext            = HIMC(WINAPI*)(HWND);
    using FnReleaseContext        = BOOL(WINAPI*)(HWND, HIMC);
    using FnAssociateContext      = HIMC(WINAPI*)(HWND, HIMC);
    using FnAssociateContextEx    = BOOL(WINAPI*)(HWND, HIMC, DWORD);
    using FnGetCompositionStringW = LONG(WINAPI*)(HIMC, DWORD, LPVOID, DWORD);
    using FnGetCandidateListW     = DWORD(WINAPI*)(HIMC, DWORD, LPCANDIDATELIST, DWORD);
    using FnNotifyIME             = BOOL(WINAPI*)(HIMC, DWORD, DWORD, DWORD);
    using FnSetCompositionWindow  = BOOL(WINAPI*)(HIMC, LPCOMPOSITIONFORM);
    using FnSetCandidateWindow    = BOOL(WINAPI*)(HIMC, LPCANDIDATEFORM);
    using FnGetOpenStatus         = BOOL(WINAPI*)(HIMC);
    using FnSetOpenStatus         = BOOL(WINAPI*)(HIMC, BOOL);
    using FnIsIME                 = BOOL(WINAPI*)(HKL);

    FnGetContext            getContext            = nullptr;
    FnReleaseContext        releaseContext        = nullptr;
    FnAssociateContext      associateContext      = nullptr;
    FnAssociateContextEx    associateContextEx    = nullptr;
    FnGetCompositionStringW getCompositionStringW = nullptr;
    FnGetCandidateListW     getCandidateListW     = nullptr;
    FnNotifyIME             notifyIME             = nullptr;
    FnSetCompositionWindow  setCompositionWindow  = nullptr;
    FnSetCandidateWindow    setCandidateWindow    = nullptr;
    FnGetOpenStatus         getOpenStatus         = nullptr;
    FnSetOpenStatus         setOpenStatus         = nullptr;
    FnIsIME                 isIME                 = nullptr;
    std::once_flag          once{};
    bool                    ready                 = false;

    bool ensure() noexcept {
        std::call_once(once, [this]() {
            HMODULE mod = ::GetModuleHandleW(L"imm32.dll");
            if (!mod) mod = ::LoadLibraryW(L"imm32.dll");
            if (!mod) return;
            getContext            = reinterpret_cast<FnGetContext>(::GetProcAddress(mod, "ImmGetContext"));
            releaseContext        = reinterpret_cast<FnReleaseContext>(::GetProcAddress(mod, "ImmReleaseContext"));
            associateContext      = reinterpret_cast<FnAssociateContext>(::GetProcAddress(mod, "ImmAssociateContext"));
            associateContextEx    = reinterpret_cast<FnAssociateContextEx>(::GetProcAddress(mod, "ImmAssociateContextEx"));
            getCompositionStringW = reinterpret_cast<FnGetCompositionStringW>(::GetProcAddress(mod, "ImmGetCompositionStringW"));
            getCandidateListW     = reinterpret_cast<FnGetCandidateListW>(::GetProcAddress(mod, "ImmGetCandidateListW"));
            notifyIME             = reinterpret_cast<FnNotifyIME>(::GetProcAddress(mod, "ImmNotifyIME"));
            setCompositionWindow  = reinterpret_cast<FnSetCompositionWindow>(::GetProcAddress(mod, "ImmSetCompositionWindow"));
            setCandidateWindow    = reinterpret_cast<FnSetCandidateWindow>(::GetProcAddress(mod, "ImmSetCandidateWindow"));
            getOpenStatus         = reinterpret_cast<FnGetOpenStatus>(::GetProcAddress(mod, "ImmGetOpenStatus"));
            setOpenStatus         = reinterpret_cast<FnSetOpenStatus>(::GetProcAddress(mod, "ImmSetOpenStatus"));
            isIME                 = reinterpret_cast<FnIsIME>(::GetProcAddress(mod, "ImmIsIME"));
            ready = (getContext && releaseContext && associateContext && getCompositionStringW && getCandidateListW);
        });
        return ready;
    }
} g_imm;

constexpr UINT kMsgConsoleImeSync = WM_APP + 0x4D52U;

HMODULE                    g_selfModule       = nullptr;
std::uintptr_t             g_exeBase          = 0;
std::uintptr_t             g_exeEnd           = 0;
app::Config                g_cfg{};

void*                       g_origChatDispatchRaw  = nullptr;
void*                       g_origSChatMoveRaw     = nullptr;
void*                       g_origPresentRaw       = nullptr;
void*                       g_origSKeyboardMoveRaw = nullptr;
void*                       g_origUMhGuiMoveAfterRaw = nullptr;
void SetOrigUMhGuiMoveAfter(void* p) noexcept { g_origUMhGuiMoveAfterRaw = p; }
std::uintptr_t              g_chatRelay            = 0;
std::uintptr_t              g_chatMoveRelay        = 0;
std::uintptr_t              g_presentRelay         = 0;
std::uintptr_t              g_kbMoveRelay          = 0;
HHOOK                       g_kbHook           = nullptr;
HHOOK                       g_getMsgHook       = nullptr;
std::atomic<bool>           g_kbHookInstalled{false};
std::atomic<DWORD>          g_wndTid{0U};
std::atomic<HWND>           g_gameHwnd{nullptr};
std::atomic<std::uint32_t>  g_presentHookFrames{0U};
std::atomic<bool>           g_reassertKbHookPending{false};
std::atomic<bool>           g_imeSyncPending{false};
std::atomic<bool>           g_imeSyncPosted{false};
std::atomic<bool>           g_wndProcCheckPending{false};
std::atomic<std::uint32_t>  g_kbOpenDownCount{0U};
std::atomic<std::uint32_t>  g_wpOpenDownCount{0U};
std::atomic<std::uint8_t>   g_swallowVkMask{0U};
std::atomic<bool>           g_detaching{false};
std::atomic<bool>           g_kbMoveHooked{false};
bool                        g_mouseOverrideApplied = false;
std::uint8_t                g_savedForceCursor     = 0U;
std::uint8_t                g_savedOverlayActive   = 0U;
WNDPROC                     g_origWndProc          = nullptr;
HWND                        g_subclassedHwnd       = nullptr;
bool                        g_wndProcInstalled     = false;
bool                        g_consoleImeSyncedOpen = false;
bool                        g_consoleWeAttachedIme = false;
bool                        g_inImeSync            = false;
bool                        g_changedImeOpen       = false;
BOOL                        g_prevImeOpen          = FALSE;
bool                        g_loggedNoWpDelivery   = false;
wchar_t                     g_pendingHighSurrogate = 0;
thread_local std::uintptr_t tls_activeSChat    = 0;
thread_local bool           tls_inChatCmd      = false;

struct AsyncChatItem {
    char text[256]{};
    bool forceGameChat = false;
};

constexpr std::size_t       kAsyncChatCap      = 64U;
AsyncChatItem               g_asyncChatRing[kAsyncChatCap]{};
std::size_t                 g_asyncChatHead    = 0U;
std::size_t                 g_asyncChatTail    = 0U;
std::size_t                 g_asyncChatCount   = 0U;
std::mutex                  g_asyncChatMx;
std::atomic<std::uint32_t>  g_asyncChatPending{0U};

std::atomic<bool>           g_startupNoticePending{false};
char                        g_startupNoticeBuf[256]{};
std::mutex                  g_startupNoticeMx;

inline bool IsValidUserPtr(std::uintptr_t p) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & 0x7ULL) == 0ULL;
}

bool TryPostChatSystemNoticeSEH(const char* utf8Text, bool forceGameChat = false) noexcept {
    if (!forceGameChat && !g_cfg.showChatFeedback) return true;
    if (!g_exeBase || !utf8Text || utf8Text[0] == '\0') return false;
    __try {
        std::uintptr_t sChat = *reinterpret_cast<const std::uintptr_t*>(g_exeBase + mhw::kRvaSChatInstance);
        if (!IsValidUserPtr(sChat)) {
            sChat = tls_activeSChat;
        }
        if (!IsValidUserPtr(sChat)) return false;

        // Verify sChat vftable and sMhGUI::Instance before entering sChat::ShowMessage
        // (sChat::ShowMessage locks sChat+0x08 then calls CreateSystemMessage which dereferences sMhGUI::Instance).
        const std::uintptr_t sChatVft = *reinterpret_cast<const std::uintptr_t*>(sChat);
        const std::uintptr_t sMhGui   = *reinterpret_cast<const std::uintptr_t*>(g_exeBase + mhw::kRvaSMhGuiInstance);
        if (!IsValidUserPtr(sChatVft) || !IsValidUserPtr(sMhGui)) return false;

        const auto fnShow = reinterpret_cast<SChatShowMessageFn>(g_exeBase + mhw::kRvaSChatShowMessage);
        fnShow(sChat, utf8Text, 0.0f, 0xFFFFFFFFU, 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PostChatSystemNoticeSEH(const char* utf8Text) noexcept {
    console::PushOutputLine(utf8Text);
    (void)TryPostChatSystemNoticeSEH(utf8Text);
}

bool IsPlayerInSceneSEH(std::uintptr_t exeBase) noexcept {
    if (!exeBase) return false;
    __try {
        const std::uintptr_t root = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSPlayerRoot);
        if (!IsValidUserPtr(root)) return false;
        const std::uintptr_t ent = *reinterpret_cast<const std::uintptr_t*>(root + mhw::kOffPlayerRootEntity);
        return IsValidUserPtr(ent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void QueueStartupNoticeFmt(const char* fmt, ...) {
    char buf[256]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    app::Log("ui: %s", buf);
    console::PushOutputLine(buf);
    {
        std::lock_guard<std::mutex> lk(g_startupNoticeMx);
        std::memcpy(g_startupNoticeBuf, buf, sizeof(g_startupNoticeBuf));
        g_startupNoticeBuf[sizeof(g_startupNoticeBuf) - 1U] = '\0';
    }
    g_startupNoticePending.store(true, std::memory_order_release);
}

bool TryFlushStartupNotice(std::uintptr_t sChatHint) noexcept {
    if (!g_startupNoticePending.load(std::memory_order_acquire)) return true;
    if (!g_cfg.showChatFeedback) {
        g_startupNoticePending.store(false, std::memory_order_release);
        return true;
    }
    if (!IsPlayerInSceneSEH(g_exeBase)) return false;

    char msg[256]{};
    {
        std::lock_guard<std::mutex> lk(g_startupNoticeMx);
        std::memcpy(msg, g_startupNoticeBuf, sizeof(msg));
    }
    if (msg[0] == '\0') {
        g_startupNoticePending.store(false, std::memory_order_release);
        return true;
    }

    const std::uintptr_t prevSChat = tls_activeSChat;
    if (sChatHint != 0) tls_activeSChat = sChatHint;
    const bool delivered = TryPostChatSystemNoticeSEH(msg);
    tls_activeSChat = prevSChat;
    if (delivered) {
        g_startupNoticePending.store(false, std::memory_order_release);
    }
    return delivered;
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

void PushAsyncChatRing(const char* utf8Text, bool forceGameChat) noexcept {
    std::lock_guard<std::mutex> lk(g_asyncChatMx);
    if (g_asyncChatCount >= kAsyncChatCap) {
        g_asyncChatHead = (g_asyncChatHead + 1U) % kAsyncChatCap;
        --g_asyncChatCount;
    }
    std::size_t n = 0U;
    while (n < 255U && utf8Text[n] != '\0') {
        g_asyncChatRing[g_asyncChatTail].text[n] = utf8Text[n];
        ++n;
    }
    g_asyncChatRing[g_asyncChatTail].text[n] = '\0';
    g_asyncChatRing[g_asyncChatTail].forceGameChat = forceGameChat;
    g_asyncChatTail = (g_asyncChatTail + 1U) % kAsyncChatCap;
    ++g_asyncChatCount;
    g_asyncChatPending.store(static_cast<std::uint32_t>(g_asyncChatCount), std::memory_order_release);
}

void EnqueueAsyncChat(const char* utf8Text) noexcept {
    if (!utf8Text || utf8Text[0] == '\0') return;
    console::PushOutputLine(utf8Text);
    if (!g_cfg.showChatFeedback) return;
    PushAsyncChatRing(utf8Text, false);
}

void EnqueueForcedAsyncChat(const char* utf8Text) noexcept {
    if (!utf8Text || utf8Text[0] == '\0') return;
    app::Log("ui: %s", utf8Text);
    console::PushOutputLine(utf8Text);
    PushAsyncChatRing(utf8Text, true);
}

void DrainAsyncChatOnMainThread(std::uintptr_t sChat, std::uint32_t maxPerTick) noexcept {
    AsyncChatItem msgs[8]{};
    std::uint32_t popped = 0U;
    const std::uint32_t limit = (maxPerTick > 8U) ? 8U : maxPerTick;
    {
        std::lock_guard<std::mutex> lk(g_asyncChatMx);
        while (g_asyncChatCount > 0U && popped < limit) {
            msgs[popped] = g_asyncChatRing[g_asyncChatHead];
            g_asyncChatHead = (g_asyncChatHead + 1U) % kAsyncChatCap;
            --g_asyncChatCount;
            ++popped;
        }
        g_asyncChatPending.store(static_cast<std::uint32_t>(g_asyncChatCount), std::memory_order_release);
    }
    if (popped == 0U) return;
    const std::uintptr_t prevSChat = tls_activeSChat;
    tls_activeSChat = sChat;
    for (std::uint32_t i = 0U; i < popped; ++i) {
        (void)TryPostChatSystemNoticeSEH(msgs[i].text, msgs[i].forceGameChat);
    }
    tls_activeSChat = prevSChat;
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

bool InvokeChatCommandCpp(const char* rawMsg, bool requirePrefix) noexcept {
    try {
        return cmd_router::HandleCommandLine(rawMsg, requirePrefix);
    } catch (...) {
        app::Log("ui: caught C++ exception while executing chat command");
        return false;
    }
}

bool InvokeChatCommandSEH(const char* rawMsg, bool requirePrefix) noexcept {
    __try {
        return InvokeChatCommandCpp(rawMsg, requirePrefix);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DrainConsoleCommandsOnMainThread(std::uintptr_t sChatHint) noexcept {
    if (!g_cfg.consoleEnabled) return;
    const std::uintptr_t prevSChat = tls_activeSChat;
    if (sChatHint != 0) tls_activeSChat = sChatHint;
    std::string cmd;
    for (int i = 0; i < 4 && console::PopPendingCommand(cmd); ++i) {
        if (!InvokeChatCommandSEH(cmd.c_str(), false)) {
            app::Log("console: command '%s' ignored or failed", cmd.c_str());
        }
    }
    if (sChatHint != 0 && g_asyncChatPending.load(std::memory_order_acquire) != 0U) {
        DrainAsyncChatOnMainThread(sChatHint, 8U);
    }
    tls_activeSChat = prevSChat;
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
        const bool ok = InvokeChatCommandSEH(msgBuf, true);
        tls_activeSChat = prevSChat;
        tls_inChatCmd = false;
        if (!ok) {
            app::Log("ui: caught SEH exception while executing chat command '%s'", msgBuf);
        }
        if (g_asyncChatPending.load(std::memory_order_acquire) != 0U) {
            DrainAsyncChatOnMainThread(sChat, 8U);
        }
    }
}

LRESULT CALLBACK RngKeyboardProc(int code, WPARAM wParam, LPARAM lParam);

void ReassertKbHookOnWindowThread() noexcept {
    const DWORD tid = g_wndTid.load(std::memory_order_acquire);
    if (tid == 0U || ::GetCurrentThreadId() != tid) return;
    if (!g_reassertKbHookPending.exchange(false, std::memory_order_acq_rel)) return;

    HHOOK oldHook = g_kbHook;
    HHOOK newHook = ::SetWindowsHookExW(WH_KEYBOARD, RngKeyboardProc, g_selfModule, tid);
    if (newHook) {
        g_kbHook = newHook;
        if (oldHook) ::UnhookWindowsHookEx(oldHook);
    }
}

void __fastcall Hook_SChatMove(std::uintptr_t sChat) {
    ReassertKbHookOnWindowThread();
    DrainConsoleCommandsOnMainThread(sChat);
    if (!g_kbMoveHooked.load(std::memory_order_acquire) && !g_detaching.load(std::memory_order_relaxed)) {
        plugin_host::DispatchMainThreadTicks();
        peer_beacon::TickOnMainThread(g_exeBase);
    }
    if (g_startupNoticePending.load(std::memory_order_acquire)) {
        TryFlushStartupNotice(sChat);
    }
    if (g_asyncChatPending.load(std::memory_order_acquire) != 0U) {
        DrainAsyncChatOnMainThread(sChat, 4U);
    }
    if (const auto orig = reinterpret_cast<SChatMoveFn>(
            _InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_origSChatMoveRaw), nullptr, nullptr))) {
        orig(sChat);
    }
}

void __fastcall Hook_UMhGuiMoveAfter(std::uintptr_t uMhGui) {
    if (!g_detaching.load(std::memory_order_relaxed)) {
        hud_mark::OnUMhGuiMoveAfter(g_exeBase, uMhGui);
    }
    if (const auto orig = reinterpret_cast<UMhGuiMoveAfterFn>(
            _InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_origUMhGuiMoveAfterRaw), nullptr, nullptr))) {
        orig(uMhGui);
    }
}

void SyncConsoleMouseStateSEH(std::uintptr_t exeBase,
                              bool wantOpen,
                              bool& ioApplied,
                              std::uint8_t& ioSavedCursor,
                              std::uint8_t& ioSavedOverlay) noexcept {
    if (!exeBase) return;
    __try {
        const std::uintptr_t sAppExt = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSAppExtInstance);
        if (!IsValidUserPtr(sAppExt)) return;
        const std::uintptr_t sMouse = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSMouseInstance);

        if (wantOpen) {
            if (!ioApplied) {
                ioSavedCursor  = *reinterpret_cast<const std::uint8_t*>(sAppExt + mhw::kOffAppExtForceShowCursor);
                ioSavedOverlay = *reinterpret_cast<const std::uint8_t*>(sAppExt + mhw::kOffAppExtOverlayActive);
                ioApplied      = true;
            }
            *reinterpret_cast<std::uint8_t*>(sAppExt + mhw::kOffAppExtForceShowCursor) = 1U;
            *reinterpret_cast<std::uint8_t*>(sAppExt + mhw::kOffAppExtOverlayActive)   = 1U;
            if (IsValidUserPtr(sMouse)) {
                *reinterpret_cast<std::uint8_t*>(sMouse + mhw::kOffSMouseDiscardNextDelta) = 1U;
            }
        } else if (ioApplied) {
            *reinterpret_cast<std::uint8_t*>(sAppExt + mhw::kOffAppExtForceShowCursor) = ioSavedCursor;
            *reinterpret_cast<std::uint8_t*>(sAppExt + mhw::kOffAppExtOverlayActive)   = ioSavedOverlay;
            *reinterpret_cast<std::uint8_t*>(sAppExt + mhw::kOffAppExtCursorDirty)     = 1U;
            if (IsValidUserPtr(sMouse)) {
                *reinterpret_cast<std::uint8_t*>(sMouse + mhw::kOffSMouseDiscardNextDelta) = 1U;
            }
            ioApplied = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ApplyConsoleKeyboardBlockSEH(std::uintptr_t sKb,
                                   bool isOpen,
                                   std::uint8_t swallowMask,
                                   int toggleVk,
                                   std::uint8_t& outNewMask) noexcept {
    outNewMask = swallowMask;
    __try {
        if (isOpen) {
            std::memset(reinterpret_cast<void*>(sKb + mhw::kOffSKeyboardStateBlock),
                        0,
                        mhw::kSizeSKeyboardStateBlock);
            outNewMask = 0x07U;
            return;
        }
        if (swallowMask != 0U) {
            auto* mOn  = reinterpret_cast<std::uint32_t*>(sKb + mhw::kOffSKeyboardOn);
            auto* mTrg = reinterpret_cast<std::uint32_t*>(sKb + mhw::kOffSKeyboardTrg);
            auto* mRel = reinterpret_cast<std::uint32_t*>(sKb + mhw::kOffSKeyboardRel);
            auto* mChg = reinterpret_cast<std::uint32_t*>(sKb + mhw::kOffSKeyboardChg);
            auto* mRep = reinterpret_cast<std::uint32_t*>(sKb + mhw::kOffSKeyboardRep);
            const int vks[3] = { VK_ESCAPE, VK_RETURN, toggleVk };
            std::uint8_t nextMask = swallowMask;
            for (int i = 0; i < 3; ++i) {
                const std::uint8_t maskBit = static_cast<std::uint8_t>(1U << i);
                if ((swallowMask & maskBit) == 0U) continue;
                const int vk = vks[i];
                if (vk > 0 && vk < 256) {
                    const std::uint32_t wIdx = static_cast<std::uint32_t>(vk) >> 5U;
                    const std::uint32_t wBit = 1U << (vk & 31);
                    if ((mOn[wIdx] & wBit) != 0U || (mRel[wIdx] & wBit) != 0U) {
                        mOn[wIdx]  &= ~wBit;
                        mTrg[wIdx] &= ~wBit;
                        mRel[wIdx] &= ~wBit;
                        mChg[wIdx] &= ~wBit;
                        mRep[wIdx] &= ~wBit;
                    } else {
                        nextMask &= static_cast<std::uint8_t>(~maskBit);
                    }
                } else {
                    nextMask &= static_cast<std::uint8_t>(~maskBit);
                }
            }
            outNewMask = nextMask;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall Hook_SKeyboardMove(std::uintptr_t sKb) {
    if (const auto orig = reinterpret_cast<SKeyboardMoveFn>(
            _InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_origSKeyboardMoveRaw), nullptr, nullptr))) {
        orig(sKb);
    }
    if (!g_detaching.load(std::memory_order_relaxed)) {
        plugin_host::DispatchMainThreadTicks();
        peer_beacon::TickOnMainThread(g_exeBase);
    }
    if (!g_cfg.consoleEnabled) return;

    const bool isOpen = console::IsOpen();
    SyncConsoleMouseStateSEH(g_exeBase, isOpen, g_mouseOverrideApplied, g_savedForceCursor, g_savedOverlayActive);
    if (!IsValidUserPtr(sKb)) return;

    const std::uint8_t swallowMask = g_swallowVkMask.load(std::memory_order_relaxed);
    if (!isOpen && swallowMask == 0U) return;

    std::uint8_t nextMask = swallowMask;
    ApplyConsoleKeyboardBlockSEH(sKb, isOpen, swallowMask, g_cfg.vkToggleConsole, nextMask);
    if (nextMask != swallowMask) {
        g_swallowVkMask.store(nextMask, std::memory_order_relaxed);
    }
}

void UpdateImeWindowPos(HWND hwnd, HIMC himc) noexcept {
    if (!hwnd || !himc || !g_imm.ensure()) return;
    int cx = 24, cy = 400, lh = 24;
    if (!console_overlay::GetInputCaretClientPos(hwnd, cx, cy, lh)) {
        RECT cr{};
        if (::GetClientRect(hwnd, &cr)) {
            cx = 24;
            cy = std::max(24, static_cast<int>((cr.bottom - cr.top) / 2));
        }
    }
    if (g_imm.setCompositionWindow) {
        COMPOSITIONFORM cf{};
        cf.dwStyle        = CFS_POINT;
        cf.ptCurrentPos.x = cx;
        cf.ptCurrentPos.y = cy;
        g_imm.setCompositionWindow(himc, &cf);
    }
    if (g_imm.setCandidateWindow) {
        CANDIDATEFORM candForm{};
        candForm.dwIndex        = 0U;
        candForm.dwStyle        = CFS_CANDIDATEPOS;
        candForm.ptCurrentPos.x = cx;
        candForm.ptCurrentPos.y = cy + lh;
        g_imm.setCandidateWindow(himc, &candForm);
    }
}

void RefreshImeCandidates(HWND hwnd) noexcept {
    if (!hwnd || !g_imm.ensure()) return;
    HIMC himc = g_imm.getContext(hwnd);
    if (!himc) {
        console::ClearImeCandidates();
        return;
    }

    const DWORD bytes = g_imm.getCandidateListW(himc, 0U, nullptr, 0U);
    if (bytes < sizeof(CANDIDATELIST) || bytes > 65536U) {
        g_imm.releaseContext(hwnd, himc);
        if (bytes == 0U) console::ClearImeCandidates();
        return;
    }

    std::vector<std::uint8_t> buf(bytes, 0U);
    auto* cl = reinterpret_cast<CANDIDATELIST*>(buf.data());
    const DWORD got = g_imm.getCandidateListW(himc, 0U, cl, bytes);
    if (got < sizeof(CANDIDATELIST) || cl->dwCount == 0U ||
        offsetof(CANDIDATELIST, dwOffset) + static_cast<std::size_t>(cl->dwCount) * sizeof(DWORD) > got) {
        g_imm.releaseContext(hwnd, himc);
        console::ClearImeCandidates();
        return;
    }

    const DWORD pageSize = (cl->dwPageSize > 0U && cl->dwPageSize <= 10U)
                               ? cl->dwPageSize
                               : std::min<DWORD>(9U, cl->dwCount);
    const DWORD sel = std::min<DWORD>(cl->dwSelection, cl->dwCount - 1U);
    DWORD pageStart = cl->dwPageStart;
    if (sel < pageStart || sel >= pageStart + pageSize) {
        pageStart = (sel / pageSize) * pageSize;
    }
    const DWORD pageEnd = std::min<DWORD>(cl->dwCount, pageStart + pageSize);

    std::vector<std::wstring> pageItems;
    pageItems.reserve(pageEnd - pageStart);
    for (DWORD i = pageStart; i < pageEnd; ++i) {
        const DWORD off = cl->dwOffset[i];
        if (off >= got) {
            pageItems.emplace_back();
            continue;
        }
        const std::size_t maxWchars = (got - off) / sizeof(wchar_t);
        const auto* ws = reinterpret_cast<const wchar_t*>(buf.data() + off);
        const std::size_t wlen = ::wcsnlen(ws, maxWchars);
        pageItems.emplace_back(ws, wlen);
    }

    console::SetImeCandidatesUtf16(pageItems, static_cast<std::size_t>(sel - pageStart),
                                   static_cast<std::size_t>(pageStart), static_cast<std::size_t>(cl->dwCount));
    UpdateImeWindowPos(hwnd, himc);
    g_imm.releaseContext(hwnd, himc);
}

void FormatModuleOffset(const void* addr, char* out, std::size_t cap) noexcept {
    if (!out || cap == 0U) return;
    out[0] = '\0';
    if (!addr) {
        std::snprintf(out, cap, "0x0");
        return;
    }
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(addr), &mod) ||
        !mod) {
        std::snprintf(out, cap, "%p", addr);
        return;
    }
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* baseName = ::wcsrchr(path, L'\\');
    baseName = baseName ? (baseName + 1) : path;
    char modUtf8[MAX_PATH * 2]{};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, baseName, -1, modUtf8,
                                        static_cast<int>(sizeof(modUtf8)), nullptr, nullptr);
    if (n <= 0) {
        std::snprintf(out, cap, "%p", addr);
        return;
    }
    const auto off = static_cast<unsigned long long>(
        reinterpret_cast<const std::uint8_t*>(addr) -
        reinterpret_cast<const std::uint8_t*>(mod));
    std::snprintf(out, cap, "%s+0x%llX (%p)", modUtf8, off, addr);
}

void RequestConsoleImeSync(HWND hw) noexcept {
    g_imeSyncPending.store(true, std::memory_order_release);
    if (hw && !g_imeSyncPosted.exchange(true, std::memory_order_acq_rel)) {
        if (!::PostMessageW(hw, kMsgConsoleImeSync, 0, 0)) {
            g_imeSyncPosted.store(false, std::memory_order_release);
        }
    }
}

void SyncConsoleImeStateOnWindowThread(bool forceDetach = false) noexcept {
    if (g_inImeSync) return;
    g_inImeSync = true;
    struct SyncGuard {
        bool& flag;
        ~SyncGuard() { flag = false; }
    } syncGuard{g_inImeSync};

    HWND hwnd = g_gameHwnd.load(std::memory_order_acquire);
    if (!hwnd || !::IsWindow(hwnd)) return;
    const DWORD tid = g_wndTid.load(std::memory_order_acquire);
    if (!forceDetach && (tid == 0U || ::GetCurrentThreadId() != tid)) return;
    g_imeSyncPending.store(false, std::memory_order_release);
    g_imeSyncPosted.store(false, std::memory_order_release);
    if (!g_imm.ensure()) return;

    const bool wantIme = !forceDetach && console::IsOpen() && !IsGameTextInputActive(g_exeBase);
    if (wantIme) {
        if (!g_consoleImeSyncedOpen) {
            const HWND fg  = ::GetForegroundWindow();
            const HWND foc = ::GetFocus();
            if (fg == hwnd && foc != hwnd) {
                ::SetFocus(hwnd);
            }

            HIMC preHimc = g_imm.getContext(hwnd);
            g_consoleImeSyncedOpen = true;
            const bool needAttach = (preHimc == nullptr);
            g_consoleWeAttachedIme = needAttach;

            HIMC workHimc = preHimc;
            if (needAttach) {
                if (g_imm.associateContextEx) {
                    g_imm.associateContextEx(hwnd, nullptr, IACE_DEFAULT);
                }
                workHimc = g_imm.getContext(hwnd);
                if (!workHimc) {
                    app::Log("console-ime: failed to attach IME context on hwnd=%p", hwnd);
                }
            }

            const HKL hkl = ::GetKeyboardLayout(0U);
            const BOOL isIme = (g_imm.isIME != nullptr) ? g_imm.isIME(hkl) : FALSE;
            if (workHimc) {
                if (isIme && g_imm.getOpenStatus && g_imm.setOpenStatus) {
                    const BOOL wasOpen = g_imm.getOpenStatus(workHimc);
                    if (!wasOpen) {
                        g_prevImeOpen    = FALSE;
                        g_changedImeOpen = true;
                        g_imm.setOpenStatus(workHimc, TRUE);
                    }
                }
                UpdateImeWindowPos(hwnd, workHimc);
                g_imm.releaseContext(hwnd, workHimc);
            }
        }
    } else {
        g_pendingHighSurrogate = 0;
        if (console::HasImeComposition()) {
            console::ClearImeComposition();
        }
        const bool wasSynced   = g_consoleImeSyncedOpen;
        const bool weAttached  = g_consoleWeAttachedIme;
        const bool restoreOpen = g_changedImeOpen;
        const BOOL prevOpen    = g_prevImeOpen;
        g_consoleImeSyncedOpen = false;
        g_consoleWeAttachedIme = false;
        g_changedImeOpen       = false;

        if (wasSynced || weAttached || forceDetach) {
            HIMC himc = g_imm.getContext(hwnd);
            if (himc) {
                if (g_imm.notifyIME) {
                    g_imm.notifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0U);
                }
                if (restoreOpen && g_imm.setOpenStatus) {
                    g_imm.setOpenStatus(himc, prevOpen);
                }
                g_imm.releaseContext(hwnd, himc);
            }
            if (weAttached && (forceDetach || !IsGameTextInputActive(g_exeBase))) {
                g_imm.associateContext(hwnd, nullptr);
            }
        }
    }
}

struct ConsoleMsgNest;
thread_local ConsoleMsgNest* tls_consoleMsgNest = nullptr;
thread_local int             tls_wndProcDepth   = 0;

struct ConsoleMsgNest {
    HWND            hwnd;
    UINT            msg;
    WPARAM          wParam;
    LPARAM          lParam;
    ConsoleMsgNest* prev;

    ConsoleMsgNest(HWND h, UINT m, WPARAM w, LPARAM l) noexcept
        : hwnd(h), msg(m), wParam(w), lParam(l), prev(tls_consoleMsgNest) {
        tls_consoleMsgNest = this;
        ++tls_wndProcDepth;
    }
    ~ConsoleMsgNest() noexcept {
        tls_consoleMsgNest = prev;
        --tls_wndProcDepth;
    }
    ConsoleMsgNest(const ConsoleMsgNest&)            = delete;
    ConsoleMsgNest& operator=(const ConsoleMsgNest&) = delete;
};

int CountConsoleMsgNest(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    int count = 0;
    for (const ConsoleMsgNest* cur = tls_consoleMsgNest; cur; cur = cur->prev) {
        if (cur->hwnd == hwnd && cur->msg == msg && cur->wParam == wParam && cur->lParam == lParam) {
            ++count;
        }
    }
    return count;
}

LRESULT CALLBACK ConsoleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT ForwardToOrigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, WNDPROC orig, int nest) noexcept {
    if (nest > 3 || tls_wndProcDepth > 12) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (nest > 1 || tls_wndProcDepth > 10) {
        static thread_local ULONGLONG nextWarnTick = 0ULL;
        const ULONGLONG now = ::GetTickCount64();
        if (now >= nextWarnTick) {
            nextWarnTick = now + 5000ULL;
            char origDesc[160]{};
            FormatModuleOffset(reinterpret_cast<const void*>(orig), origDesc, sizeof(origDesc));
            app::Log("console-ime: warning - WndProc cycle/reentrancy detected (msg=0x%04X nest=%d depth=%d orig=%s), falling back to Global::WndProc",
                     static_cast<unsigned>(msg), nest, tls_wndProcDepth, origDesc);
        }
        if (g_exeBase != 0) {
            const auto globalWp = reinterpret_cast<WNDPROC>(g_exeBase + mhw::kRvaGlobalWndProc);
            if (globalWp != &ConsoleWndProc && globalWp != orig) {
                return ::CallWindowProcW(globalWp, hwnd, msg, wParam, lParam);
            }
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (orig && orig != &ConsoleWndProc) {
        return ::CallWindowProcW(orig, hwnd, msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ConsoleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC orig = g_origWndProc;
    ConsoleMsgNest nestGuard(hwnd, msg, wParam, lParam);
    const int nest = CountConsoleMsgNest(hwnd, msg, wParam, lParam);

    if (msg == WM_NCDESTROY) {
        app::Log("console-ime: WM_NCDESTROY on hwnd=%p", hwnd);
        SyncConsoleImeStateOnWindowThread(true);
        if (g_wndProcInstalled && orig &&
            reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == &ConsoleWndProc) {
            ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        }
        g_wndProcInstalled = false;
        g_subclassedHwnd   = nullptr;
        return ForwardToOrigWndProc(hwnd, msg, wParam, lParam, orig, nest);
    }

    if (msg == kMsgConsoleImeSync) {
        SyncConsoleImeStateOnWindowThread();
        return 0;
    }

    if (g_imeSyncPending.load(std::memory_order_acquire) && !g_inImeSync) {
        SyncConsoleImeStateOnWindowThread();
    }

    if (msg == WM_SETFOCUS || msg == WM_ACTIVATEAPP) {
        const LRESULT res = ForwardToOrigWndProc(hwnd, msg, wParam, lParam, orig, nest);
        if (console::IsOpen() && !g_inImeSync && (msg == WM_SETFOCUS || wParam != 0) && g_imm.ensure()) {
            HIMC cur = g_imm.getContext(hwnd);
            if (!cur) {
                g_consoleImeSyncedOpen = false;
                g_consoleWeAttachedIme = false;
                SyncConsoleImeStateOnWindowThread();
            } else {
                UpdateImeWindowPos(hwnd, cur);
                g_imm.releaseContext(hwnd, cur);
            }
        }
        return res;
    }

    if (msg == WM_KILLFOCUS) {
        if (console::IsOpen()) {
            g_pendingHighSurrogate = 0;
            if (g_imm.ensure()) {
                if (HIMC himc = g_imm.getContext(hwnd)) {
                    if (g_imm.notifyIME) g_imm.notifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0U);
                    g_imm.releaseContext(hwnd, himc);
                }
            }
            console::ClearImeComposition();
        }
        return ForwardToOrigWndProc(hwnd, msg, wParam, lParam, orig, nest);
    }

    if (!console::IsOpen()) {
        if (!g_inImeSync && (g_consoleImeSyncedOpen || g_consoleWeAttachedIme)) {
            SyncConsoleImeStateOnWindowThread();
        }
        return ForwardToOrigWndProc(hwnd, msg, wParam, lParam, orig, nest);
    }

    switch (msg) {
        case WM_IME_SETCONTEXT:
            lParam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW | ISC_SHOWUIALLCANDIDATEWINDOW);
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_IME_STARTCOMPOSITION:
            if (g_imm.ensure()) {
                if (HIMC himc = g_imm.getContext(hwnd)) {
                    UpdateImeWindowPos(hwnd, himc);
                    g_imm.releaseContext(hwnd, himc);
                }
            }
            return 0;

        case WM_IME_COMPOSITION: {
            if (g_imm.ensure()) {
                HIMC himc = g_imm.getContext(hwnd);
                if (himc) {
                    if (lParam == 0) {
                        console::ClearImeComposition();
                    }
                    if ((lParam & GCS_RESULTSTR) != 0) {
                        const LONG bytes = g_imm.getCompositionStringW(himc, GCS_RESULTSTR, nullptr, 0U);
                        if (bytes > 0 && bytes <= 4096) {
                            const int wchars = static_cast<int>(bytes / static_cast<LONG>(sizeof(wchar_t)));
                            std::vector<wchar_t> wbuf(static_cast<std::size_t>(wchars) + 1U, L'\0');
                            const LONG got = g_imm.getCompositionStringW(himc, GCS_RESULTSTR, wbuf.data(),
                                                                         static_cast<DWORD>(bytes));
                            if (got > 0) {
                                const int gotChars = static_cast<int>(got / static_cast<LONG>(sizeof(wchar_t)));
                                console::InsertUtf16(wbuf.data(), gotChars);
                            }
                        }
                        if ((lParam & GCS_COMPSTR) == 0) {
                            console::ClearImeComposition();
                        }
                    }
                    if ((lParam & GCS_COMPSTR) != 0) {
                        const LONG bytes = g_imm.getCompositionStringW(himc, GCS_COMPSTR, nullptr, 0U);
                        if (bytes <= 0 || bytes > 4096) {
                            console::ClearImeComposition();
                        } else {
                            const int wchars = static_cast<int>(bytes / static_cast<LONG>(sizeof(wchar_t)));
                            std::vector<wchar_t> wbuf(static_cast<std::size_t>(wchars) + 1U, L'\0');
                            const LONG got = g_imm.getCompositionStringW(himc, GCS_COMPSTR, wbuf.data(),
                                                                         static_cast<DWORD>(bytes));
                            if (got > 0) {
                                const int gotChars = static_cast<int>(got / static_cast<LONG>(sizeof(wchar_t)));
                                int cursorIdx = gotChars;
                                if ((lParam & GCS_CURSORPOS) != 0) {
                                    const LONG cp = g_imm.getCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0U);
                                    if (cp >= 0) {
                                        cursorIdx = std::min(gotChars, static_cast<int>(cp & 0xFFFF));
                                    }
                                }
                                console::SetImeCompositionUtf16(wbuf.data(), gotChars, cursorIdx);
                            } else {
                                console::ClearImeComposition();
                            }
                        }
                        UpdateImeWindowPos(hwnd, himc);
                    }
                    g_imm.releaseContext(hwnd, himc);
                }
            }
            return 0;
        }

        case WM_IME_ENDCOMPOSITION:
            console::ClearImeComposition();
            return 0;

        case WM_IME_NOTIFY:
            if (wParam == IMN_OPENCANDIDATE || wParam == IMN_CHANGECANDIDATE) {
                RefreshImeCandidates(hwnd);
                return 0;
            }
            if (wParam == IMN_CLOSECANDIDATE) {
                console::ClearImeCandidates();
                return 0;
            }
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_IME_CHAR:
            return 0;

        case WM_IME_REQUEST:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_CHAR: {
            const wchar_t ch = static_cast<wchar_t>(wParam);
            if (IS_HIGH_SURROGATE(ch)) {
                g_pendingHighSurrogate = ch;
                return 0;
            }
            if (IS_LOW_SURROGATE(ch)) {
                if (g_pendingHighSurrogate != 0) {
                    const wchar_t pair[2] = {g_pendingHighSurrogate, ch};
                    g_pendingHighSurrogate = 0;
                    console::InsertUtf16(pair, 2);
                }
                return 0;
            }
            g_pendingHighSurrogate = 0;
            if (ch >= 0x20 && ch != 0x7F) {
                console::InsertUtf16(&ch, 1);
            }
            return 0;
        }

        case WM_DEADCHAR:
        case WM_SYSCHAR:
            return 0;

        case WM_KEYDOWN: {
            const int vk = static_cast<int>(wParam);
            g_wpOpenDownCount.fetch_add(1U, std::memory_order_relaxed);
            if (vk == VK_PROCESSKEY) return 0;
            if (console::HasImeComposition()) {
                if (vk == VK_ESCAPE) {
                    if (g_imm.ensure()) {
                        if (HIMC himc = g_imm.getContext(hwnd)) {
                            if (g_imm.notifyIME) g_imm.notifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0U);
                            g_imm.releaseContext(hwnd, himc);
                        }
                    }
                    console::ClearImeComposition();
                }
                return 0;
            }
            const bool ctrl  = ((::GetKeyState(VK_CONTROL) & 0x8000) != 0);
            const bool shift = ((::GetKeyState(VK_SHIFT) & 0x8000) != 0);
            const bool alt   = ((::GetKeyState(VK_MENU) & 0x8000) != 0);
            if (ctrl && !alt && !shift && (vk == 'C' || vk == VK_INSERT)) {
                if (console_overlay::CopySelectionToClipboard(false)) {
                    return 0;
                }
            }
            if (vk == VK_ESCAPE && !ctrl && !alt && console_overlay::HasSelection()) {
                console_overlay::ClearSelection();
                return 0;
            }
            (void)console::OnKeyDown(vk, ctrl, shift, alt);
            if (!console::IsOpen()) {
                console_overlay::ClearSelection();
                g_swallowVkMask.fetch_or(0x01U, std::memory_order_relaxed);
                SyncConsoleImeStateOnWindowThread();
            }
            return 0;
        }

        case WM_KEYUP:
            return 0;

        case WM_SYSKEYDOWN: {
            const int vk = static_cast<int>(wParam);
            if (vk == 'B' || vk == 'F') {
                if (!console::HasImeComposition() && ((::GetKeyState(VK_CONTROL) & 0x8000) == 0)) {
                    const bool shift = ((::GetKeyState(VK_SHIFT) & 0x8000) != 0);
                    (void)console::OnKeyDown(vk, false, shift, true);
                }
                return 0;
            }
            if (vk == VK_F4) {
                return ForwardToOrigWndProc(hwnd, msg, wParam, lParam, orig, nest);
            }
            if (vk == VK_MENU || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                vk == VK_LMENU || vk == VK_RMENU) {
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);
            }
            return 0;
        }

        case WM_SYSKEYUP:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);

        default:
            return ForwardToOrigWndProc(hwnd, msg, wParam, lParam, orig, nest);
    }
}

void EnsureWndProcSubclassOnWindowThread() noexcept {
    if (!g_cfg.consoleEnabled || g_detaching.load(std::memory_order_acquire)) return;
    const DWORD tid = g_wndTid.load(std::memory_order_acquire);
    if (tid == 0U || ::GetCurrentThreadId() != tid) return;
    const bool checkPending = g_wndProcCheckPending.exchange(false, std::memory_order_acq_rel);
    if (g_wndProcInstalled && !checkPending) return;

    HWND hwnd = g_gameHwnd.load(std::memory_order_acquire);
    if (!hwnd || !::IsWindow(hwnd)) return;

    if (g_wndProcInstalled && g_subclassedHwnd == hwnd) {
        const auto curWp = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
        if (curWp != &ConsoleWndProc) {
            char wpDesc[160]{};
            FormatModuleOffset(reinterpret_cast<const void*>(curWp), wpDesc, sizeof(wpDesc));
            app::Log("console-ime: WndProc head changed to %s on hwnd=%p", wpDesc, hwnd);
        }
        return;
    }

    ::SetLastError(0U);
    auto* prev = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                              reinterpret_cast<LONG_PTR>(&ConsoleWndProc)));
    if (prev && prev != &ConsoleWndProc) {
        g_origWndProc      = prev;
        g_subclassedHwnd   = hwnd;
        g_wndProcInstalled = true;
        char origDesc[160]{};
        FormatModuleOffset(reinterpret_cast<const void*>(prev), origDesc, sizeof(origDesc));
        app::Log("init: installed console WndProc subclass on hwnd=%p (orig=%s)", hwnd, origDesc);
    }
}

LRESULT CALLBACK RngGetMsgProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        ReassertKbHookOnWindowThread();
        EnsureWndProcSubclassOnWindowThread();
        if (g_imeSyncPending.load(std::memory_order_acquire) ||
            (g_cfg.consoleEnabled && g_wndProcInstalled && (console::IsOpen() != g_consoleImeSyncedOpen))) {
            RequestConsoleImeSync(g_gameHwnd.load(std::memory_order_acquire));
        }
    }
    if (code == HC_ACTION && wParam == PM_REMOVE && console::IsOpen() && lParam != 0) {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        const int mx = static_cast<int>(static_cast<short>(LOWORD(msg->lParam)));
        const int my = static_cast<int>(static_cast<short>(HIWORD(msg->lParam)));
        if (msg->message == WM_MOUSEWHEEL) {
            const int delta = GET_WHEEL_DELTA_WPARAM(msg->wParam);
            const int lines = (delta >= 0) ? std::max(1, (delta * 3) / WHEEL_DELTA)
                                           : std::min(-1, (delta * 3) / WHEEL_DELTA);
            console::ScrollBy(lines);
            msg->message = WM_NULL;
        } else if (msg->message == WM_LBUTTONDOWN || msg->message == WM_LBUTTONDBLCLK) {
            console_overlay::OnMouseDown(msg->hwnd, mx, my, msg->message == WM_LBUTTONDBLCLK);
            msg->message = WM_NULL;
        } else if (msg->message == WM_MOUSEMOVE) {
            console_overlay::OnMouseMove(msg->hwnd, mx, my, (msg->wParam & MK_LBUTTON) != 0);
        } else if (msg->message == WM_LBUTTONUP) {
            console_overlay::OnMouseUp(msg->hwnd, mx, my);
            msg->message = WM_NULL;
        } else if (msg->message == WM_RBUTTONUP) {
            console_overlay::CopySelectionToClipboard(true);
            msg->message = WM_NULL;
        } else if (msg->message == WM_RBUTTONDOWN || msg->message == WM_RBUTTONDBLCLK ||
                   msg->message == WM_MBUTTONDOWN || msg->message == WM_MBUTTONUP ||
                   msg->message == WM_XBUTTONDOWN || msg->message == WM_XBUTTONUP) {
            msg->message = WM_NULL;
        }
    }
    return ::CallNextHookEx(g_getMsgHook, code, wParam, lParam);
}

LRESULT CALLBACK RngKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const bool isKeyDown   = ((lParam & (1LL << 31)) == 0);
        const bool wasDownPrev = ((lParam & (1LL << 30)) != 0);
        const int  vk          = static_cast<int>(wParam);

        if (g_cfg.consoleEnabled && g_cfg.vkToggleConsole != 0 && vk == g_cfg.vkToggleConsole) {
            if (console::IsOpen() || !IsGameTextInputActive(g_exeBase)) {
                if (isKeyDown && !wasDownPrev) {
                    const bool opened = console::ToggleOpen();
                    if (opened) {
                        g_loggedNoWpDelivery = false;
                        g_kbOpenDownCount.store(0U, std::memory_order_relaxed);
                        g_wpOpenDownCount.store(0U, std::memory_order_relaxed);
                        g_reassertKbHookPending.store(true, std::memory_order_release);
                    } else {
                        console_overlay::ClearSelection();
                        g_swallowVkMask.fetch_or(0x04U, std::memory_order_relaxed);
                    }
                    RequestConsoleImeSync(g_gameHwnd.load(std::memory_order_acquire));
                }
                return 1;
            }
        }

        if (console::IsOpen()) {
            if (g_wndProcInstalled) {
                if (!console::HasImeComposition() &&
                    (vk == g_cfg.vkToggleTakeover || vk == g_cfg.vkResetEpoch ||
                     (vk >= VK_F1 && vk <= VK_F24))) {
                    return 1;
                }
                if (isKeyDown && !wasDownPrev && !(vk >= VK_F1 && vk <= VK_F24)) {
                    const std::uint32_t kd = g_kbOpenDownCount.fetch_add(1U, std::memory_order_relaxed) + 1U;
                    if (kd == 3U && g_wpOpenDownCount.load(std::memory_order_relaxed) == 0U && !g_loggedNoWpDelivery) {
                        g_loggedNoWpDelivery = true;
                        HWND hw = g_gameHwnd.load(std::memory_order_acquire);
                        const void* wp = hw ? reinterpret_cast<const void*>(::GetWindowLongPtrW(hw, GWLP_WNDPROC))
                                            : nullptr;
                        char wpDesc[160]{};
                        FormatModuleOffset(wp, wpDesc, sizeof(wpDesc));
                        app::Log("console: warning - WH_KEYBOARD saw 3 keydowns but ConsoleWndProc saw 0 (hwnd=%p fg=%p focus=%p wp=%s)",
                                 hw, ::GetForegroundWindow(), ::GetFocus(), wpDesc);
                    }
                }
                // Return 0 WITHOUT CallNextHookEx so downstream WH_KEYBOARD hooks (e.g. mhw_hfi)
                // never see keystrokes while TranslateMessage/IMM32/TSF deliver to ConsoleWndProc.
                return 0;
            }
            return 1;
        }

        if (isKeyDown && !wasDownPrev && !IsGameTextInputActive(g_exeBase) && vk != 0) {
            if (vk == g_cfg.vkToggleTakeover) {
                if (MhwRng_IsTakeoverEnabled()) {
                    MhwRng_SetEngineMode(kMhwRngModeOff);
                } else {
                    const std::uint32_t target = (g_cfg.bootMode == kMhwRngModeTakeover)
                                                     ? kMhwRngModeTakeover
                                                     : kMhwRngModeSelective;
                    MhwRng_SetEngineMode(target);
                }
                NotifyChatFmt(app::IsEnglish() ? "[RNG] Mode = %s" : u8"[RNG] 模式 = %s",
                              cmd_router::EngineModeLabel(MhwRng_GetEngineMode()));
            } else if (vk == g_cfg.vkResetEpoch) {
                const std::uint64_t ep = MhwRng_ResetEpoch();
                NotifyChatFmt(app::IsEnglish() ? "[RNG] Reset to Epoch %llu" : u8"[RNG] 已重置到第 %llu 轮",
                              static_cast<unsigned long long>(ep));
            }
        }
    }
    return ::CallNextHookEx(g_kbHook, code, wParam, lParam);
}

HWND GetSwapWindowSEH(std::uintptr_t d3dRender) noexcept {
    if (!IsValidUserPtr(d3dRender)) return nullptr;
    __try {
        return *reinterpret_cast<HWND const*>(d3dRender + mhw::kOffD3DSwapHwnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::uint64_t __fastcall Hook_Present(std::uintptr_t d3dRender,
                                      std::uint64_t a2,
                                      std::uint64_t a3,
                                      std::uint64_t a4) {
    if (!g_detaching.load(std::memory_order_relaxed)) {
        if (g_cfg.consoleEnabled) {
            DrainConsoleCommandsOnMainThread(0);
        }
        console_overlay::OnPresent(g_exeBase, d3dRender);
    }

    const auto orig = reinterpret_cast<D3DPresentFn>(
        _InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_origPresentRaw), nullptr, nullptr));
    const std::uint64_t ret = orig ? orig(d3dRender, a2, a3, a4) : 0ULL;

    // Install WH_KEYBOARD AFTER downstream hooks (e.g. mhw_hfi.dll) on frame 10 so we stay
    // at the head of Windows' LIFO WH_KEYBOARD chain without editing other plugins.
    const bool wantInputHooks = (g_cfg.consoleEnabled || g_cfg.vkToggleTakeover != 0 || g_cfg.vkResetEpoch != 0);
    if (wantInputHooks && !g_kbHookInstalled.load(std::memory_order_acquire)) {
        if (g_presentHookFrames.fetch_add(1U, std::memory_order_relaxed) + 1U >= 10U) {
            const HWND hwnd = GetSwapWindowSEH(d3dRender);
            DWORD wndTid = hwnd ? ::GetWindowThreadProcessId(hwnd, nullptr) : 0U;
            if (wndTid == 0U) wndTid = ::GetCurrentThreadId();
            if (!g_kbHookInstalled.exchange(true, std::memory_order_acq_rel)) {
                if (hwnd) g_gameHwnd.store(hwnd, std::memory_order_release);
                g_wndTid.store(wndTid, std::memory_order_release);
                g_kbHook = ::SetWindowsHookExW(WH_KEYBOARD, RngKeyboardProc, g_selfModule, wndTid);
                if (g_cfg.consoleEnabled) {
                    g_getMsgHook = ::SetWindowsHookExW(WH_GETMESSAGE, RngGetMsgProc, g_selfModule, wndTid);
                }
                app::Log("init: window input hooks installed after orig present (hwnd=%p, tid=%lu, kb=%p, getmsg=%p)",
                         hwnd, wndTid, g_kbHook, g_getMsgHook);
            }
        }
    } else if (g_cfg.consoleEnabled && (g_presentHookFrames.fetch_add(1U, std::memory_order_relaxed) % 60U) == 0U) {
        const HWND curHwnd = GetSwapWindowSEH(d3dRender);
        const HWND prevHwnd = g_gameHwnd.load(std::memory_order_relaxed);
        if (curHwnd && curHwnd != prevHwnd) {
            app::Log("present: swapchain HWND changed (%p -> %p)", prevHwnd, curHwnd);
            g_gameHwnd.store(curHwnd, std::memory_order_release);
            g_wndProcCheckPending.store(true, std::memory_order_release);
        }
    }
    return ret;
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
        {"MtRandom::nrand",        mhw::kRvaMtRandomNrand,       mhw::kPrologueMtRandomNrand,       sizeof(mhw::kPrologueMtRandomNrand),       reinterpret_cast<void*>(&MhwRngAsm_MtRandomNrand),       &rng::SetOrigMtRandomNrand,       true},
        {"MtRandom::ctor(seed)",   mhw::kRvaMtRandomCtorSeed,    mhw::kPrologueMtRandomCtorSeed,    sizeof(mhw::kPrologueMtRandomCtorSeed),    reinterpret_cast<void*>(&MhwRngAsm_MtRandomCtorSeed),    &rng::SetOrigMtRandomCtorSeed,    true},
        {"MtRandom::ctor_default", mhw::kRvaMtRandomCtorDefault, mhw::kPrologueMtRandomCtorDefault, sizeof(mhw::kPrologueMtRandomCtorDefault), reinterpret_cast<void*>(&MhwRngAsm_MtRandomCtorDefault), &rng::SetOrigMtRandomCtorDefault, true},
        {"MtRandom::srand",        mhw::kRvaMtRandomSrand,       mhw::kPrologueMtRandomSrand,       sizeof(mhw::kPrologueMtRandomSrand),       reinterpret_cast<void*>(&MhwRngAsm_MtRandomSrand),       &rng::SetOrigMtRandomSrand,       true},
        {"MtRandom::randSphere",   mhw::kRvaMtRandomSphere,      mhw::kPrologueMtRandomSphere,      sizeof(mhw::kPrologueMtRandomSphere),      reinterpret_cast<void*>(&MhwRngAsm_MtRandomSphere),      &rng::SetOrigMtRandomSphere,      true},
        {"std::_Random_device",    mhw::kRvaStdRandomDevice,     mhw::kPrologueStdRandomDevice,     sizeof(mhw::kPrologueStdRandomDevice),     reinterpret_cast<void*>(&MhwRngAsm_StdRandomDevice),     &rng::SetOrigStdRandomDevice,     false},
        {"mt19937_envelope_range", mhw::kRvaMt19937Range,        mhw::kPrologueMt19937Range,        sizeof(mhw::kPrologueMt19937Range),        reinterpret_cast<void*>(&MhwRngAsm_Mt19937Range),        &rng::SetOrigMt19937Range,        false},
        {"mt19937_heap_init",      mhw::kRvaMt19937HeapInit,     mhw::kPrologueMt19937HeapInit,     sizeof(mhw::kPrologueMt19937HeapInit),     reinterpret_cast<void*>(&MhwRngAsm_Mt19937HeapInit),     &rng::SetOrigMt19937HeapInit,     false},
        {"mt19937_heap_draw",      mhw::kRvaMt19937HeapDraw,     mhw::kPrologueMt19937HeapDraw,     sizeof(mhw::kPrologueMt19937HeapDraw),     reinterpret_cast<void*>(&MhwRngAsm_Mt19937HeapDraw),     &rng::SetOrigMt19937HeapDraw,     false},
        {"CRT_rand",               mhw::kRvaCrtRand,             mhw::kPrologueCrtRand,             sizeof(mhw::kPrologueCrtRand),             reinterpret_cast<void*>(&MhwRngAsm_CrtRand),             &rng::SetOrigCrtRand,             false},
        {"uMhGUI::moveAfter",      mhw::kRvaUMhGuiMoveAfter,      mhw::kPrologueUMhGuiMoveAfter,      sizeof(mhw::kPrologueUMhGuiMoveAfter),      reinterpret_cast<void*>(&Hook_UMhGuiMoveAfter),          &SetOrigUMhGuiMoveAfter,          false},
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

    // Initialize Console Core, Plugin Host & auto-load sub-plugins BEFORE activating UI hooks
    console::Init(&g_cfg);
    peer_beacon::Init(g_cfg.checkEnabled, g_cfg.selfCheckEnabled, &EnqueueForcedAsyncChat);
    audio_mark::Init();
    plugin_host::Init(base, end, app::SubPluginDirectory(), &PostChatSystemNoticeSEH, &EnqueueAsyncChat);
    cmd_router::Init(&g_cfg, &PostChatSystemNoticeSEH);
    std::size_t loadedPlugins = 0U;
    if (g_cfg.autoLoadPlugins) {
        loadedPlugins = plugin_host::LoadAllFromDirectory();
    }
    if (!g_cfg.autoExec.empty()) {
        cmd_router::ExecuteScriptFile(g_cfg.autoExec, true);
    }

    app::Log("init: activated %zu/10 RNG hooks + %zu sub-plugins (arena=%p, mode=%s, protectSaveSeeds=%s, lang=%s, masterSeed=0x%llX)",
             hookedCount,
             loadedPlugins,
             reinterpret_cast<void*>(hook::ArenaBase()),
             cmd_router::EngineModeLabelAscii(MhwRng_GetEngineMode()),
             rng::IsProtectSaveSeedsEnabled() ? "ON" : "OFF",
             app::GetLanguage(),
             static_cast<unsigned long long>(MhwRng_GetMasterSeed()));
    outBase = base;
    outEnd  = end;
    return true;
}

bool InstallChatHookWithRetry(std::uintptr_t base, const char* tag) {
    const std::uintptr_t chatSite = base + mhw::kRvaSChatDispatchEntry;
    char errBuf[192]{};
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
                                           &relay,
                                           errBuf,
                                           sizeof(errBuf))) {
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
    app::Log("%s: ERROR - failed to hook sChat::DispatchChatEntry @ %p after retries (%s)",
             tag, reinterpret_cast<void*>(chatSite), errBuf[0] ? errBuf : "unknown");
    return false;
}

bool InstallSChatMoveHookWithRetry(std::uintptr_t base, const char* tag) {
    const std::uintptr_t moveSite = base + mhw::kRvaSChatMove;
    char errBuf[192]{};
    for (int retry = 0; retry < 20 && !g_detaching.load(std::memory_order_acquire); ++retry) {
        bool wasChained = false;
        std::uintptr_t chainedTarget = 0;
        std::uintptr_t relay = 0;
        if (hook::InstallStandaloneDetour5(moveSite,
                                           reinterpret_cast<void*>(&Hook_SChatMove),
                                           &g_origSChatMoveRaw,
                                           mhw::kPrologueSChatMove,
                                           &wasChained,
                                           &chainedTarget,
                                           &relay,
                                           errBuf,
                                           sizeof(errBuf))) {
            g_chatMoveRelay = relay;
            app::Log("%s: hooked sChat::move @ %p (%s, tramp=%p)",
                     tag, reinterpret_cast<void*>(moveSite),
                     wasChained ? "chained" : "vanilla", g_origSChatMoveRaw);
            return true;
        }
        ::Sleep(25);
    }
    app::Log("%s: warning - failed to hook sChat::move @ %p (%s)",
             tag, reinterpret_cast<void*>(moveSite), errBuf[0] ? errBuf : "unknown");
    return false;
}

void InstallPresentHookWithRetry(std::uintptr_t base, const char* tag) {
    const std::uintptr_t presSite = base + mhw::kRvaD3DPresent;
    char errBuf[192]{};
    for (int retry = 0; retry < 20 && !g_detaching.load(std::memory_order_acquire); ++retry) {
        bool wasChained = false;
        std::uintptr_t chainedTarget = 0;
        std::uintptr_t relay = 0;
        if (hook::InstallStandaloneDetour5(presSite,
                                           reinterpret_cast<void*>(&Hook_Present),
                                           &g_origPresentRaw,
                                           mhw::kPrologueD3DPresent,
                                           &wasChained,
                                           &chainedTarget,
                                           &relay,
                                           errBuf,
                                           sizeof(errBuf))) {
            g_presentRelay = relay;
            app::Log("%s: hooked cD3DRender::present @ %p (%s, tramp=%p)",
                     tag, reinterpret_cast<void*>(presSite),
                     wasChained ? "chained" : "vanilla", g_origPresentRaw);
            return;
        }
        ::Sleep(25);
    }
    app::Log("%s: warning - failed to hook cD3DRender::present @ %p (%s)",
             tag, reinterpret_cast<void*>(presSite), errBuf[0] ? errBuf : "unknown");
}

void InstallSKeyboardMoveHookWithRetry(std::uintptr_t base, const char* tag) {
    const std::uintptr_t kbSite = base + mhw::kRvaSKeyboardMove;
    char errBuf[192]{};
    for (int retry = 0; retry < 20 && !g_detaching.load(std::memory_order_acquire); ++retry) {
        bool wasChained = false;
        std::uintptr_t chainedTarget = 0;
        std::uintptr_t relay = 0;
        if (hook::InstallStandaloneDetour5(kbSite,
                                           reinterpret_cast<void*>(&Hook_SKeyboardMove),
                                           &g_origSKeyboardMoveRaw,
                                           mhw::kPrologueSKeyboardMove,
                                           &wasChained,
                                           &chainedTarget,
                                           &relay,
                                           errBuf,
                                           sizeof(errBuf))) {
            g_kbMoveRelay = relay;
            g_kbMoveHooked.store(true, std::memory_order_release);
            app::Log("%s: hooked sKeyboard::move @ %p (%s, tramp=%p)",
                     tag, reinterpret_cast<void*>(kbSite),
                     wasChained ? "chained" : "vanilla", g_origSKeyboardMoveRaw);
            return;
        }
        ::Sleep(25);
    }
    app::Log("%s: warning - failed to hook sKeyboard::move @ %p (%s)",
             tag, reinterpret_cast<void*>(kbSite), errBuf[0] ? errBuf : "unknown");
}

DWORD WINAPI InitWorker(LPVOID) {
    app::InitLog();
    g_cfg = app::LoadConfig();
    app::SetLanguage(g_cfg.language.c_str());
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

    const bool chatOk = InstallChatHookWithRetry(base, "init");
    const bool en = app::IsEnglish();
    if (chatOk) {
        app::Log("init: microkernel ready! Type '%s' in game chat.", g_cfg.chatPrefix.c_str());
        const auto plugins = plugin_host::ListLoadedPlugins();
        QueueStartupNoticeFmt(en ? "[RNG] mhw_rng v3.5 ready (%zu plugins | Mode:%s | Type %s help)"
                                 : u8"[RNG] mhw_rng v3.5 已就绪（已载入 %zu 个插件 | 模式:%s | 输入 %s help）",
                              plugins.size(),
                              cmd_router::EngineModeLabel(MhwRng_GetEngineMode()),
                              g_cfg.chatPrefix.c_str());
    } else {
        QueueStartupNoticeFmt(en ? "[RNG] Warning: chat command hook failed, see mhw_rng.log"
                                 : u8"[RNG] 警告：聊天指令钩子安装失败，详情见 mhw_rng.log");
    }
    const bool moveOk = InstallSChatMoveHookWithRetry(base, "init");
    InstallSKeyboardMoveHookWithRetry(base, "init");
    InstallPresentHookWithRetry(base, "init");

    // Phase 3: Two lightweight post-install chain verifications (+1.5s and +4.0s) to self-heal
    // in case a late-loading mod overwrote shared UI hooks without chaining.
    const DWORD checkDelaysMs[2] = {1500U, 2500U};
    const std::uintptr_t chatSite = base + mhw::kRvaSChatDispatchEntry;
    const std::uintptr_t moveSite = base + mhw::kRvaSChatMove;
    const std::uintptr_t presSite = base + mhw::kRvaD3DPresent;
    const std::uintptr_t kbSite   = base + mhw::kRvaSKeyboardMove;
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
        if (g_chatMoveRelay != 0 && hook::WasUnchainedOverwrite5(moveSite, g_chatMoveRelay, mhw::kPrologueSChatMove)) {
            app::Log("chat: detected unchained overwrite at sChat::move %p, re-chaining...",
                     reinterpret_cast<void*>(moveSite));
            InstallSChatMoveHookWithRetry(base, "chatmove-rechain");
        }
        if (g_kbMoveRelay != 0 && hook::WasUnchainedOverwrite5(kbSite, g_kbMoveRelay, mhw::kPrologueSKeyboardMove)) {
            app::Log("input: detected unchained overwrite at sKeyboard::move %p, re-chaining...",
                     reinterpret_cast<void*>(kbSite));
            InstallSKeyboardMoveHookWithRetry(base, "kb-rechain");
        }
        if (g_presentRelay != 0 && hook::WasUnchainedOverwrite5(presSite, g_presentRelay, mhw::kPrologueD3DPresent)) {
            app::Log("present: detected unchained overwrite at D3DPresent %p, re-chaining...",
                     reinterpret_cast<void*>(presSite));
            InstallPresentHookWithRetry(base, "present-rechain");
        }
    }

    // Fallback delivery if sChat::move was not hooked: wait until player enters scene and post directly.
    if (!moveOk && g_startupNoticePending.load(std::memory_order_acquire)) {
        for (int i = 0; i < 150 && !g_detaching.load(std::memory_order_acquire); ++i) {
            if (TryFlushStartupNotice(0)) break;
            ::Sleep(200);
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
        SyncConsoleMouseStateSEH(g_exeBase, false, g_mouseOverrideApplied, g_savedForceCursor, g_savedOverlayActive);
        if (g_wndProcInstalled && g_subclassedHwnd && ::IsWindow(g_subclassedHwnd) && g_origWndProc &&
            reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g_subclassedHwnd, GWLP_WNDPROC)) == &ConsoleWndProc) {
            ::SetWindowLongPtrW(g_subclassedHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        }
        g_wndProcInstalled = false;
        g_subclassedHwnd   = nullptr;
        SyncConsoleImeStateOnWindowThread(true);
        if (g_getMsgHook) {
            ::UnhookWindowsHookEx(g_getMsgHook);
            g_getMsgHook = nullptr;
        }
        if (g_kbHook) {
            ::UnhookWindowsHookEx(g_kbHook);
            g_kbHook = nullptr;
        }
        console_overlay::Shutdown();
        audio_mark::Shutdown();
        plugin_host::ShutdownAll();
        hook::RemoveAllHooks();
        hud_mark::ResetState();
        rng::Shutdown();
        app::CloseLog();
    }
    return TRUE;
}
