#pragma once
// ============================================================================
//  Verified MHW:I 15.23 (Steam build 15539686, ImageBase 0x140000000) RVAs &
//  exact instruction-boundary prologues for mhw_rng.dll.
//
//  Exhaustively verified against unpacked 48.3MB .text (text_1523.bin) & Ghidra:
//    - 0 inline Xorshift128 step bodies exist outside 0x142174E30 & 0x142174E60.
//    - 0 hardware rdrand / rdseed instructions exist in .text.
//    - All 10 RNG/seeder entry points below cover 100% of gameplay & engine PRNGs.
// ============================================================================
#include <cstddef>
#include <cstdint>

namespace mhw {

// ---- Global MtRandom Singleton ----
// MtRandom::sInstance (16B: uint32_t mRandomX, mRandomY, mRandomZ, mRandomW) @ 0x143FDEBD8
constexpr std::uintptr_t kRvaMtRandomInstance = 0x3FDEBD8;

// ---- 1. Core MtRandom Family (0x142174CE0 .. 0x142174E60) ----

// MtRandom* MtRandom::ctor(MtRandom* this, uint32_t seed) @ 0x142174CE0
//   [+0, 6B] 81 F2 CA 26 59 66  xor edx, 0x665926ca
constexpr std::uintptr_t kRvaMtRandomCtorSeed = 0x2174CE0;
constexpr std::uint8_t kPrologueMtRandomCtorSeed[6] = {
    0x81, 0xF2, 0xCA, 0x26, 0x59, 0x66
};

// MtRandom* MtRandom::ctor_default(MtRandom* this) @ 0x142174D50
//   [+0, 2B] 33 C0              xor eax, eax
//   [+2, 6B] C7 01 15 CD 5B 07  mov dword ptr [rcx], 0x075bcd15
constexpr std::uintptr_t kRvaMtRandomCtorDefault = 0x2174D50;
constexpr std::uint8_t kPrologueMtRandomCtorDefault[8] = {
    0x33, 0xC0,
    0xC7, 0x01, 0x15, 0xCD, 0x5B, 0x07
};

// void MtRandom::srand(MtRandom* this, uint32_t seed) [xorshift_init] @ 0x142174DC0
//   [+0, 6B] 81 F2 CA 26 59 66  xor edx, 0x665926ca
constexpr std::uintptr_t kRvaMtRandomSrand = 0x2174DC0;
constexpr std::uint8_t kPrologueMtRandomSrand[6] = {
    0x81, 0xF2, 0xCA, 0x26, 0x59, 0x66
};

// uint32_t MtRandom::nrand(MtRandom* this) @ 0x142174E30 (747 caller functions)
//   [+0, 3B] 8B 41 04           mov eax, dword ptr [rcx+4]
//   [+3, 3B] 4C 8B C1           mov r8, rcx
constexpr std::uintptr_t kRvaMtRandomNrand = 0x2174E30;
constexpr std::uint8_t kPrologueMtRandomNrand[6] = {
    0x8B, 0x41, 0x04,
    0x4C, 0x8B, 0xC1
};

// float* MtRandom::randUnitSphere(MtRandom* this, float* outVec3) @ 0x142174E60
//   [+0, 2B] 40 53              push rbx
//   [+2, 4B] 48 83 EC 40        sub rsp, 0x40
constexpr std::uintptr_t kRvaMtRandomSphere = 0x2174E60;
constexpr std::uint8_t kPrologueMtRandomSphere[6] = {
    0x40, 0x53,
    0x48, 0x83, 0xEC, 0x40
};

// ---- 2. Secondary Engine / Stdlib / CRT PRNG Entry Points ----

// uint32_t std::_Random_device() @ 0x14273D420 (sole caller: 0x141B0DCE0 lobby shuffle)
//   [+0, 4B] 48 83 EC 28        sub rsp, 0x28
//   [+4, 5B] 48 8D 4C 24 30     lea rcx, [rsp+0x30]
constexpr std::uintptr_t kRvaStdRandomDevice = 0x273D420;
constexpr std::uint8_t kPrologueStdRandomDevice[9] = {
    0x48, 0x83, 0xEC, 0x28,
    0x48, 0x8D, 0x4C, 0x24, 0x30
};

// float mt19937_envelope_range(void* unused, float minVal, float maxVal) @ 0x1427D3E60
//   [+0, 4B] 48 83 EC 48                 sub rsp, 0x48
//   [+4, 9B] 65 48 8B 04 25 58 00 00 00  mov rax, gs:[0x58]
constexpr std::uintptr_t kRvaMt19937Range = 0x27D3E60;
constexpr std::uint8_t kPrologueMt19937Range[13] = {
    0x48, 0x83, 0xEC, 0x48,
    0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00
};

// void mt19937_heap_init(void* state, uint32_t seed) @ 0x142A74400
//   [+0, 5B] 48 89 5C 24 08     mov [rsp+8], rbx
constexpr std::uintptr_t kRvaMt19937HeapInit = 0x2A74400;
constexpr std::uint8_t kPrologueMt19937HeapInit[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x08
};

// uint32_t mt19937_heap_draw(void* state) @ 0x142A744B0
//   [+0, 2B] 40 53              push rbx
//   [+2, 4B] 48 83 EC 20        sub rsp, 0x20
constexpr std::uintptr_t kRvaMt19937HeapDraw = 0x2A744B0;
constexpr std::uint8_t kPrologueMt19937HeapDraw[6] = {
    0x40, 0x53,
    0x48, 0x83, 0xEC, 0x20
};

// CRT_rand() @ 0x142C58034
constexpr std::uintptr_t kRvaCrtRand = 0x2C58034;
constexpr std::uint8_t kPrologueCrtRand[9] = {
    0x48, 0x83, 0xEC, 0x28,
    0xE8, 0xCB, 0xC8, 0x00, 0x00
};

// ---- Save-Persisted PRNG Steppers & Seeder RVAs (`ProtectSaveSeeds`) ----
// Return RVAs for game routines that read a seed from cUserData/cPCSaveData/Safi,
// advance it once, and write the new seed back to persistent save memory.
constexpr std::uint32_t kSaveSeedProtectedRvas[] = {
    // cPCSaveData save salt
    0x031B682U, 0x031B69AU, 0x031B803U, 0x031B81AU, 0x031BA5BU, 0x031BA7AU,
    // Safi'jiiva awakening seed stepper (FUN_140b1c480)
    0x0B1C493U, 0x0B1C4A0U, 0x0B1C4AAU,
    // cUserData persistent PRNG steppers (reward / facility / canteen / melder / appraisal / palico)
    0x135D927U, 0x135D931U, // FUN_14135d900 -> cUserData+0x26A524
    0x135D977U, 0x135D981U, // FUN_14135d950 -> cUserData+0x26CA78
    0x135D9D2U, 0x135D9DCU, // FUN_14135d9a0 -> cUserData+0x102758[idx]
    0x135DA34U, 0x135DA3EU, // FUN_14135da00 -> cUserData+0x26A50C[idx]
    0x135DA97U, 0x135DAA1U, // FUN_14135da60 -> cUserData+0x1034A0[idx]
    0x135DB20U, 0x135DB2CU, 0x135DB3AU, 0x135DB53U, // FUN_14135dad0 -> cUserData+0x26A3FC/+0x26A540
    0x135DBA7U, 0x135DBB1U, // FUN_14135db80 -> cUserData+0x10C260
    0x136B30FU, 0x136B31FU, 0x136B32FU, 0x136B33CU, 0x136B3A4U, 0x136B3AEU // cUserData+0x10275C
};

inline bool IsSaveSeedProtectedRva(std::uint32_t rva) noexcept {
    for (std::uint32_t p : kSaveSeedProtectedRvas) {
        if (p == rva) return true;
    }
    return false;
}

// ---- Engine Math & .rdata Constants Used by 0x142174E60 & 0x1427D3E60 ----
// __libm_sse2_sincosf_(float angleInXmm0) -> XMM0 low32 = sin, second32 = cos @ 0x1427422A0
constexpr std::uintptr_t kRvaLibmSincosf       = 0x27422A0;
constexpr std::uintptr_t kRvaFloatOne          = 0x2E4FE58; // 1.0f
constexpr std::uintptr_t kRvaFloatInv2Pow32    = 0x2F12798; // 1.0f / 4294967296.0f (2.3283064e-10f)
constexpr std::uintptr_t kRvaFloatTwoPiOver2P32= 0x2F13EC4; // 2*pi / 4294967296.0f
constexpr std::uintptr_t kRvaFloatHalfStep2P32 = 0x30D2BC8; // 0.5f / 4294967296.0f

// ---- Chat Command & Notification Engine Hooks / Helpers ----
// sChat::move(sChat* this) @ 0x141A511F0 (per-frame main-thread chat update; 5B mov [rsp+10h], rbx)
constexpr std::uintptr_t kRvaSChatMove = 0x1A511F0;
constexpr std::uint8_t kPrologueSChatMove[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x10
};

// sChat::DispatchChatEntry(sChat* this, const cChatNetMsg* msg, uint32_t channel, uint8_t isLocal)
// @ 0x141A51A40 (5B mov [rsp+18h], rbx; chains cleanly with mhw_hfi.dll!)
constexpr std::uintptr_t kRvaSChatDispatchEntry = 0x1A51A40;
constexpr std::uint8_t kPrologueSChatDispatch[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x18
};

// sChat::ShowMessage(sChat* this, const char* msg, float delaySec, uint32_t icon, uint8_t isPurple)
// @ 0x141A540D0
constexpr std::uintptr_t kRvaSChatShowMessage  = 0x1A540D0;
constexpr std::uintptr_t kRvaSChatInstance     = 0x500CE70; // sChat*
constexpr std::uintptr_t kRvaSMhGuiInstance    = 0x51C4640; // sMhGUI* (required non-null by sChat::CreateSystemMessage)
constexpr std::uintptr_t kRvaSPlayerRoot       = 0x50139A0; // sPlayerRoot* (+0x50 = active player entity in scene)
constexpr std::size_t    kOffPlayerRootEntity  = 0x50;

constexpr std::size_t kOffChatNetMsgType       = 0x1B;      // int8_t (1 = typed, 2/3 = manual shoutout)
constexpr std::size_t kOffChatNetMsgText       = 0x44;      // char[128] UTF-8

// ---- Input Focus Check & Keyboard State Hook ----
constexpr std::uintptr_t kRvaSMhInputTextInstance   = 0x4CB8498;
constexpr std::size_t    kOffInputTextEditor        = 0x1D9E8;
constexpr std::size_t    kOffMhInputTextUnfocused   = 0x1DA00;
constexpr std::size_t    kOffInputEditorState       = 0x188;

// sKeyboard::move(sKeyboard* this) @ 0x1422E74F0 (16B-aligned, 5B mov [rsp+8], rbx)
// Clears mOn..holdTimers (+0x138, 0x8C0 bytes) safely after vanilla input processing, avoiding SharpPluginLoader/mhw_hfi updateState collision.
constexpr std::uintptr_t kRvaSKeyboardMove = 0x22E74F0;
constexpr std::uint8_t kPrologueSKeyboardMove[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x08
};
constexpr std::size_t    kOffSKeyboardStateBlock  = 0x138;
constexpr std::size_t    kSizeSKeyboardStateBlock = 0x8C0;
constexpr std::size_t    kOffSKeyboardOn          = 0x138;
constexpr std::size_t    kOffSKeyboardTrg         = 0x178;
constexpr std::size_t    kOffSKeyboardRel         = 0x198;
constexpr std::size_t    kOffSKeyboardChg         = 0x1B8;
constexpr std::size_t    kOffSKeyboardRep         = 0x1D8;

// sAppExt & sMouse Cursor / Overlay Control (verified @ 0x1422E58E0, 0x1422E688A, 0x1422E89A0)
constexpr std::uintptr_t kRvaSAppExtInstance        = 0x51C5320; // sAppExt*
constexpr std::size_t    kOffAppExtCursorDirty      = 0x2C23;    // uint8_t
constexpr std::size_t    kOffAppExtForceShowCursor  = 0x2C26;    // uint8_t (unclips & shows OS cursor)
constexpr std::size_t    kOffAppExtOverlayActive    = 0x2CD9;    // uint8_t (mutes game kb/mouse deltas & clicks)
constexpr std::uintptr_t kRvaSMouseInstance         = 0x51C5370; // sMouse*
constexpr std::size_t    kOffSMouseDiscardNextDelta = 0xE0;      // uint8_t

// ---- D3D11 / D3D12 / HDR Present Overlay Constants ----
// cD3DRender::present(cD3DRender* this, UINT swapInterval, bool param_3) @ 0x1423AA290
constexpr std::uintptr_t kRvaD3DPresent             = 0x23AA290;
constexpr std::uint8_t kPrologueD3DPresent[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x18
};

constexpr std::uintptr_t kRvaSMhRenderInstance      = 0x51C4480; // sMhRender*
constexpr std::uintptr_t kRvaIsD3D12Byte            = 0x51CC080; // uint8_t: 0 = DX11, != 0 = DX12
constexpr std::size_t    kOffRenderHdrOutput        = 0x21A;     // uint8_t
constexpr std::size_t    kOffRenderHdrWhiteNits     = 0x21C;     // float

// cD3DRender struct offsets (15.23)
constexpr std::size_t kOffD3DDevice12               = 0x18;      // ID3D12Device*
constexpr std::size_t kOffD3DCmdQueue12             = 0x20;      // ID3D12CommandQueue*
constexpr std::size_t kOffD3DRtvDescSize12          = 0x1D8;     // uint32_t
constexpr std::size_t kOffD3DBackBuffers12          = 0x240;     // ID3D12Resource*[3]
constexpr std::size_t kOffD3DRtvHeap12              = 0x258;     // ID3D12DescriptorHeap*
constexpr std::size_t kOffD3DRtvStart12             = 0x260;     // SIZE_T
constexpr std::size_t kOffD3DFullscreenLatch        = 0x26C;     // uint8_t
constexpr std::size_t kOffD3DDevice11               = 0x13F8;    // ID3D11Device*
constexpr std::size_t kOffD3DContext11              = 0x1400;    // ID3D11DeviceContext*
constexpr std::size_t kOffD3DSwapHwnd               = 0x1460;    // HWND
constexpr std::size_t kOffD3DSwapChain12            = 0x1470;    // IDXGISwapChain3*
constexpr std::size_t kOffD3DOccludedByte           = 0x1482;    // uint8_t
constexpr std::size_t kOffD3DSwapChain11            = 0x1488;    // IDXGISwapChain*
constexpr std::size_t kOffD3DSwapChainCount         = 0x14A0;    // uint32_t
constexpr std::size_t kOffD3DPresentLock            = 0x1620;    // CRITICAL_SECTION

} // namespace mhw
