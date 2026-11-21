#pragma once
#include "mhw_rng_api.h"
#include "log_config.h"

#include <cstdint>

namespace rng {

void Init(std::uintptr_t exeBase, std::uintptr_t exeEnd, const app::Config& cfg);
void ApplyConfigRules(const app::Config& cfg, bool clearKernelRulesOnly);

std::uintptr_t ExeBase() noexcept;
std::uintptr_t ExeEnd() noexcept;

std::uint32_t NormalizeUserSiteAddress(std::uint64_t rawAddr, bool* outWasCallInsn = nullptr) noexcept;

// Composite `(rva, condId)` & Push/Pull Condition Operations (used by Kernel & Plugin Host)
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

// Quiescent Pull-mode Per-Slot Plugin Hook Management (100% safe across hot-unload!)
bool          RegisterSlotHook(MhwRngPluginId ownerId, std::uint32_t rva, MhwRngSlotHookFn fn, void* userData) noexcept;
void          UnregisterSlotHook(MhwRngPluginId ownerId, std::uint32_t rva) noexcept;
void          DetachAllHooksForPluginAndWait(MhwRngPluginId ownerId) noexcept;

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

// Detour callbacks for all 10 RNG entry points in MHW 15.23
std::uint32_t* __fastcall Hook_MtRandomCtorSeed(std::uint32_t* self, std::uint32_t seed);
std::uint32_t* __fastcall Hook_MtRandomCtorDefault(std::uint32_t* self);
void           __fastcall Hook_MtRandomSrand(std::uint32_t* self, std::uint32_t seed);
std::uint32_t  __fastcall Hook_MtRandomNrand(std::uint32_t* self);
float*         __fastcall Hook_MtRandomSphere(std::uint32_t* self, float* outVec3);
std::uint32_t  __cdecl    Hook_StdRandomDevice();
float          __fastcall Hook_Mt19937Range(void* ctx, float minVal, float maxVal);
void           __fastcall Hook_Mt19937HeapInit(void* state, std::uint32_t seed);
std::uint32_t  __fastcall Hook_Mt19937HeapDraw(void* state);
int            __cdecl    Hook_CrtRand();

std::uint32_t CountOverriddenSlots() noexcept;
std::uint32_t CountHookedSlots() noexcept;
const char*   ModeName(std::uint8_t mode) noexcept;
const char*   SourceKindName(std::uint8_t src) noexcept;

} // namespace rng
