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

// int CRT_rand(void) @ 0x142C58034 (4B-aligned; +4 is E8 rel32 call to __acrt_getptd 0x142C64908)
//   [+0, 4B] 48 83 EC 28        sub rsp, 0x28
//   [+4, 5B] E8 CB C8 00 00     call 0x142C64908
constexpr std::uintptr_t kRvaCrtRand = 0x2C58034;
constexpr std::uint8_t kPrologueCrtRand[9] = {
    0x48, 0x83, 0xEC, 0x28,
    0xE8, 0xCB, 0xC8, 0x00, 0x00
};

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

// ---- Input Focus Check (for optional hotkeys) ----
constexpr std::uintptr_t kRvaSMhInputTextInstance = 0x4CB8498;
constexpr std::size_t    kOffInputTextEditor      = 0x1D9E8;
constexpr std::size_t    kOffMhInputTextUnfocused = 0x1DA00;
constexpr std::size_t    kOffInputEditorState     = 0x188;

// cD3DRender::present @ 0x1423AA290 (ONLY hooked if user configures non-zero VK hotkeys in INI!)
constexpr std::uintptr_t kRvaD3DPresent = 0x23AA290;
constexpr std::uint8_t kPrologueD3DPresent[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x18
};

} // namespace mhw
