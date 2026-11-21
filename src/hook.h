#pragma once
#include <cstddef>
#include <cstdint>

namespace hook {

bool InitArena(std::uintptr_t moduleLow, std::uintptr_t moduleHigh);
std::uintptr_t ArenaBase();

// Prepares a 5-byte E9 rel32 detour into the W^X trampoline arena.
// - Supports instruction-boundary prologues of 5..16 bytes.
// - Automatically relocates any displaced E8 rel32 CALL instruction inside the prologue.
// - Automatically chains if another plugin (e.g. mhw_hfi.dll) already placed an E9 jmp at `site`.
bool PrepareDetour(std::uintptr_t site,
                   std::size_t prologueLen,
                   void* detourFn,
                   void** outOrigTrampoline,
                   const std::uint8_t* expectedPrologue);

// Seals the trampoline arena as PAGE_EXECUTE_READ (strict W^X) and atomically activates all prepared detours.
bool SealAndActivateAll();

// Restores any detours that were not chained over by a later-loaded plugin.
void RemoveAllHooks();

} // namespace hook
