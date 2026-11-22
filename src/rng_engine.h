#pragma once
#include "mhw_rng_api.h"
#include "log_config.h"

#include <cstdint>

// MASM x64 Entry Thunks (`src/rng_trampolines.asm`)
extern "C" {
std::uint32_t* __fastcall MhwRngAsm_MtRandomCtorSeed(std::uint32_t* self, std::uint32_t seed);
std::uint32_t* __fastcall MhwRngAsm_MtRandomCtorDefault(std::uint32_t* self);
void           __fastcall MhwRngAsm_MtRandomSrand(std::uint32_t* self, std::uint32_t seed);
std::uint32_t  __fastcall MhwRngAsm_MtRandomNrand(std::uint32_t* self);
float*         __fastcall MhwRngAsm_MtRandomSphere(std::uint32_t* self, float* outVec3);
std::uint32_t  __cdecl    MhwRngAsm_StdRandomDevice();
float          __fastcall MhwRngAsm_Mt19937Range(void* ctx, float minVal, float maxVal);
void           __fastcall MhwRngAsm_Mt19937HeapInit(void* state, std::uint32_t seed);
std::uint32_t  __fastcall MhwRngAsm_Mt19937HeapDraw(void* state);
int            __cdecl    MhwRngAsm_CrtRand();
}

namespace rng {

using LifecycleEventSinkFn = void(*)(const MhwRngEventInfo* info) noexcept;

void Init(std::uintptr_t exeBase, std::uintptr_t exeEnd, const app::Config& cfg);
void Shutdown();
void ApplyConfigRules(const app::Config& cfg, bool clearKernelRulesOnly);
void SetLifecycleEventSink(LifecycleEventSinkFn sink) noexcept;

std::uintptr_t ExeBase() noexcept;
std::uintptr_t ExeEnd() noexcept;

std::uint32_t NormalizeUserSiteAddress(std::uint64_t rawAddr, bool* outWasCallInsn = nullptr) noexcept;

// Composite `(rva, condId)` & Push Condition Operations
bool          SetActiveCondition(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept;
std::uint32_t GetActiveCondition(std::uint32_t rva) noexcept;

bool          SetCondMasterDerived(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept;
bool          SetCondCustomSeed(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId, std::uint64_t seed) noexcept;
bool          SetCondFixedValue(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId, std::uint32_t rawU32) noexcept;
bool          SetCondSequence(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId,
                              const std::uint32_t* values, std::uint32_t count, std::uint8_t endPolicy);
bool          SetCondBypass(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept;

void          ClearSlotAllConditions(std::uint32_t rva) noexcept;
void          ClearRulesByOwner(MhwRngPluginId ownerId) noexcept;
bool          GetCondSlotInfo(std::uint32_t rva, std::uint32_t condId, MhwRngSlotSnapshot* outSnap) noexcept;
bool          ReleaseCondSlot(MhwRngPluginId ownerId, std::uint32_t rva, std::uint32_t condId) noexcept;

// Quiescent Chained Pull-Mode Hooks, Global Observers & Transparent Wrappers (v3)
bool             RegisterSlotHook(MhwRngPluginId ownerId, std::uint32_t rva, MhwRngSlotHookFn fn, void* userData) noexcept;
MhwRngHookHandle RegisterSlotHookEx(MhwRngPluginId ownerId, const MhwRngHookDesc* desc) noexcept;
void             UnregisterSlotHook(MhwRngPluginId ownerId, std::uint32_t rva) noexcept;
void             UnregisterSlotHookHandle(MhwRngPluginId ownerId, MhwRngHookHandle handle) noexcept;

MhwRngHookHandle RegisterGlobalObserver(MhwRngPluginId ownerId,
                                        std::uint32_t rvaFilterOrZero,
                                        std::uint32_t sourceKindMask,
                                        MhwRngObserverFn fn,
                                        void* userData) noexcept;
void             UnregisterGlobalObserver(MhwRngPluginId ownerId, MhwRngHookHandle handle) noexcept;

bool             RegisterTransparentWrapper(MhwRngPluginId ownerId,
                                            std::uint32_t wrapperRetRva,
                                            std::uint16_t frameQwords) noexcept;
void             UnregisterTransparentWrapper(MhwRngPluginId ownerId, std::uint32_t wrapperRetRva) noexcept;

void             DetachAllHooksForPluginAndWait(MhwRngPluginId ownerId) noexcept;

// Snapshot & Restore + Private Instance Policies (v3)
bool          RestoreSlotState(const MhwRngSlotState* state) noexcept;
std::uint32_t CaptureStateBlob(void* outBuf, std::uint32_t bufCap) noexcept;
bool          RestoreStateBlob(const void* buf, std::uint32_t bufSize) noexcept;

bool          SetInstancePolicy(std::uintptr_t instancePtr, std::uint32_t policy) noexcept;
std::uint32_t GetInstancePolicy(std::uintptr_t instancePtr) noexcept;

// Original trampoline setters
void SetOrigMtRandomCtorSeed(void* fn) noexcept;
void SetOrigMtRandomCtorDefault(void* fn) noexcept;
void SetOrigMtRandomSrand(void* fn) noexcept;
void SetOrigMtRandomNrand(void* fn) noexcept;
void SetOrigMtRandomSphere(void* fn) noexcept;
void SetOrigStdRandomDevice(void* fn) noexcept;
void SetOrigMt19937Range(void* fn) noexcept;
void SetOrigMt19937HeapInit(void* fn) noexcept;
void SetOrigMt19937HeapDraw(void* fn) noexcept;
void SetOrigCrtRand(void* fn) noexcept;

std::uint32_t CountOverriddenSlots() noexcept;
std::uint32_t CountHookedSlots() noexcept;
const char*   ModeName(std::uint8_t mode) noexcept;
const char*   SourceKindName(std::uint8_t src) noexcept;

} // namespace rng
