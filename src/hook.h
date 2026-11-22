#pragma once
#include <cstddef>
#include <cstdint>

namespace hook {

bool InitArena(std::uintptr_t moduleLow, std::uintptr_t moduleHigh);
std::uintptr_t ArenaBase();

// Resets all prepared (unsealed) detours in the primary arena so a failed startup attempt can retry cleanly.
void ResetUnsealed();

// Prepares a 5-byte E9 rel32 detour into the primary W^X trampoline arena.
// - Supports instruction-boundary prologues of 5..16 bytes.
// - Automatically relocates any displaced E8 rel32 CALL instruction inside the prologue.
// - Automatically chains if another plugin (e.g. mhw_hfi.dll) already placed an E9 jmp at `site`.
bool PrepareDetour(std::uintptr_t site,
                   std::size_t prologueLen,
                   void* detourFn,
                   void** outOrigTrampoline,
                   const std::uint8_t* expectedPrologue);

// Seals the primary trampoline arena as PAGE_EXECUTE_READ (strict W^X) and atomically activates all prepared detours.
// Uses expected-bytes CAS: if another plugin concurrently modified any site between Prepare and Seal, rolls back
// cleanly and returns false so the caller can retry and chain properly.
bool SealAndActivateAll();

// Installs a single 5-byte E9 rel32 detour in its own strict-W^X (PAGE_EXECUTE_READ) trampoline page using
// expected-bytes CAS. Publishes `*origSlotToPublish` BEFORE patching `site` (restoring prior value if CAS fails).
bool InstallStandaloneDetour5(std::uintptr_t site,
                              void* detourFn,
                              void** origSlotToPublish,
                              const std::uint8_t expected5[5],
                              bool* outWasChained = nullptr,
                              std::uintptr_t* outChainedTarget = nullptr,
                              std::uintptr_t* outRelay = nullptr,
                              char* outErrBuf = nullptr,
                              std::size_t errBufCap = 0);

// Detects whether `site` was overwritten by vanilla bytes or an unchained E9 hook (whose trampoline at target+16
// holds `expected5` instead of chaining back to `ourRelay`). Returns false if `ourRelay` is still active or chained.
bool WasUnchainedOverwrite5(std::uintptr_t site,
                            std::uintptr_t ourRelay,
                            const std::uint8_t expected5[5]) noexcept;

// Restores any detours that were not chained over by a later-loaded plugin.
void RemoveAllHooks();

} // namespace hook
