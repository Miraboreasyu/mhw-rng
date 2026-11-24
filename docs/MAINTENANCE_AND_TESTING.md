# `mhw_rng` Maintenance, Console & Testing Manual

Operational reference for AI harnesses and maintainers modifying the **MIRA microkernel (`mhw_rng.dll`)**, its **in-game DX11/DX12 overlay terminal & IME pipeline**, **SharpPluginLoader compatibility**, **the 6 official plugins**, or the **offline test & release suite**.

---

> [!TIP]
> **Reverse-Engineering Database Prerequisite**: Microkernel maintenance, hook verification, struct/offset updates, and offline machine-code tests (`MHW_TEXT_BIN`) are significantly faster and safer when paired with a **queryable reverse-engineering database** (such as an indexed SQLite/MCP symbol & xref database, a Ghidra project, and an unpacked `15.23` `.text` segment dump). Before manually disassembling or scanning binaries for new RVAs/layouts, **ask the user upfront** whether local RE query tools, databases, or unpacked binary dumps are already available in their workspace.

## 1. Microkernel File Map (`src/` & `include/`)

| File | Responsibility & Key Symbols |
| :--- | :--- |
| [`include/mhw_rng_api.h`](../include/mhw_rng_api.h) | SDK v3.5 (`kMhwRngApiVersion = 3`), `MhwRngDrawContext`, `MhwRngCallerRegs` (`0x80` bytes incl. `xmm6/7`), `MhwRngHostApi` (plus main-thread ticks, event bus, and hot-reload-safe service registry / `MhwRngServiceGuard`). Must stay ABI backwards-compatible (`structSize` checks). |
| [`include/mhw_rng_math.h`](../include/mhw_rng_math.h) / [`mhw_1523_world.h`](../include/mhw_1523_world.h) / [`mhw_world_api.h`](../include/mhw_world_api.h) / [`mhw_catalog_api.h`](../include/mhw_catalog_api.h) | Header-only Companion SDK providing shared SEH readers, IEEE-754 interval helpers, verified 15.23 offsets, and `"mhw.world"` / `"mhw.catalog"` service ABIs. |
| [`world/world_plugin.cpp`](../world/world_plugin.cpp) | Built-in core service provider `mhw_world.dll`: polls 15.23 world state via `RegisterMainThreadTick`, serves `"mhw.world"` v1, emits `"mhw.world.*"` events, drains HUD popups via `DisplayPopup`, and exposes `$rng world`. |
| [`src/mhw_addresses.h`](../src/mhw_addresses.h) | Verified MHW `15.23` RVAs (`ImageBase = 0x140000000`): 10 PRNG entry RVAs & exact expected prologue byte arrays (`kPrologueMtRandomNrand`, etc.), `MtRandom::sInstance` (`0x3FDEBD8`), `sChat::DispatchChatEntry` (`0x1A51A40`) / `sChat::move` (`0x1A511F0`), `cD3DRender::present`, `sKeyboard::move` (`0x22E74F0`), `Global::WndProc` (`0x047FD50`), `sAppExt` (`0x51C5320`), and 31 `IsSaveSeedProtectedRva` save-persisted seed RVAs. |
| [`src/hook.h`](../src/hook.h) / [`src/hook.cpp`](../src/hook.cpp) | Strict W^X 64 KB (`16 x 4 KB`) trampoline arena within `+-2 GB` of `MonsterHunterWorld.exe`. `InitArena`, `PrepareDetour`, `SealAndActivateAll`, `InstallStandaloneDetour5`, `WasUnchainedOverwrite5`, `RemoveAllHooks`. Automatically relocates displaced `E8 rel32` instructions and chains onto existing `E9` hooks (`mhw_hfi.dll`, etc.). |
| [`src/rng_trampolines.asm`](../src/rng_trampolines.asm) | MASM x64 thunks (`MhwRngAsm_*`) preserving caller non-volatile GPRs + `XMM6/7` + `_AddressOfReturnAddress()` before calling C++ `rng::*` handlers. **Do NOT reorder `MhwRngCallerRegs` without updating this file.** |
| [`src/rng_engine.h`](../src/rng_engine.h) / [`src/rng_engine.cpp`](../src/rng_engine.cpp) | 16,384-slot open-addressing hash table (`Slot[(condId << 32) | rva]`), `SplitMix64` + `StepMhwXorshift128`, 8 built-in transparent wrappers + custom `RegisterTransparentWrapper`, immutable `RvaHookChain` (max 8 hooks/RVA) + `g_inFlightHookCalls` RCU drain, SEH `.pdata` `CaptureCallerRvas`, `'MRSS'` CRC32 binary state snapshots, 512-entry `PrivateInstanceEntry` table. |
| [`src/plugin_host.h`](../src/plugin_host.h) / [`src/plugin_host.cpp`](../src/plugin_host.cpp) | Shadow-copy loader (`.hotload\<name>.<gen>.<pid>.dll`), 2-level lock hierarchy (`g_lifecycleMx` recursive -> `g_registryMx` leaf), command/completer/meta/i18n/tick/event/service registries, atomic service lease drain on unload, async `sChat::move` queue. |
| [`src/command_router.h`](../src/command_router.h) / [`src/command_router.cpp`](../src/command_router.cpp) | `$rng` / `$rng_<subcmd>` parser, built-in commands (`status`, `help`, `selective`, `on`, `observe`, `off`, `seed`, `reset`, `save`/`restore`/`saves`, `plugins`/`load`/`unload`/`reload`, `lang`, `chat`, `exec`, `raw*`, `clearall`), script executor (`#`/`;`/`//` comments, recursion limit). |
| [`src/console_core.h`](../src/console_core.h) / [`src/console_core.cpp`](../src/console_core.cpp) | Pure-logic UTF-8 terminal core: scrollback ring (`OutputLine`), readline editing (`Ctrl+A/E/B/F/W/U/K/Y/L/C/V/R`), command history, dynamic `Tab`/`Shift+Tab` completion across core + plugin completers, UTF-16 IME composition & candidate page state (`ConsoleSnapshot`). Independently unit-tested offline! |
| [`src/console_overlay.h`](../src/console_overlay.h) / [`src/console_overlay.cpp`](../src/console_overlay.cpp) | DirectX 11 & DirectX 12 runtime overlay renderer (SDR / HDR10 ST.2084 PQ / scRGB), GDI/DirectWrite glyph atlas with full CJK & IME candidate strip rendering, mouse drag/double-click/triple-click selection & clipboard copy. Zero blocking calls during `OnPresent`. |
| [`src/log_config.h`](../src/log_config.h) / [`src/log_config.cpp`](../src/log_config.cpp) | INI parser (`mhw_rng.ini`), `mhw_rng.log` thread-safe logger, `[SlotOverrides]` parser. |
| [`src/dllmain.cpp`](../src/dllmain.cpp) | 3-phase `InitWorker`, `cD3DRender::present`, `sChat::DispatchChatEntry` / `sChat::move`, `sKeyboard::move` (including `DispatchMainThreadTicks()`), and `ConsoleWndProc` (`GWLP_WNDPROC` subclassing + IME `WM_IME_*` + `ImmGetCompositionStringW` / `ImmGetCandidateListW` handling). |

---

## 2. In-Game Terminal, IME & SharpPluginLoader Coexistence

When maintaining `dllmain.cpp`, `console_core.cpp`, or `console_overlay.cpp`, preserve these critical behaviors:

1. **Hotkey & Input Isolation**:
   - Console toggles via `` ` `` / `~` (`KeyToggleConsole` in `mhw_rng.ini`). Opening clears `sKeyboard` state to avoid stray game inputs; when open, `ConsoleWndProc` consumes keyboard, `WM_CHAR`, `WM_IME_*`, and mouse events inside the overlay rectangle.
2. **Inline CJK IME Support**:
   - `ConsoleWndProc` intercepts `WM_IME_STARTCOMPOSITION`, `WM_IME_COMPOSITION` (`GCS_COMPSTR`, `GCS_CURSORPOS`, `GCS_RESULTSTR`), `WM_IME_ENDCOMPOSITION`, and `WM_IME_NOTIFY` (`IMN_OPENCANDIDATE`, `IMN_CHANGECANDIDATE`, `IMN_CLOSECANDIDATE`, `IMN_SETOPENSTATUS`).
   - Do **not** let `WM_IME_SETCONTEXT` suppress candidate state needed by custom composition, and always update `ImmSetCompositionWindow` via `console_overlay::GetInputCaretClientPos` so the OS IME anchors near the terminal prompt.
3. **Why [`patches/spl/winmm.dll`](../patches/spl/winmm.dll) Is Mandatory**:
   - Unpatched SharpPluginLoader `winmm.dll` re-subclasses `GWLP_WNDPROC` during DXGI `ResizeBuffers` (e.g. Return-to-Title or fullscreen switch) whenever another mod (`mhw_rng.dll`, overlay mods) sits ahead of it in the chain, creating a circular `m_origWndProc` loop that freezes both vanilla chat IME and terminal keyboard/IME input.
   - Our patched `winmm.dll` (`SHA256: 64DAAE7CC00EC2ADA0113EDEE1B95A551383968212E4C2E85EFFFFB8402CED34`) guards `SetWindowLongPtrW` so `m_origWndProc` is only set once. See [`patches/spl/README.md`](../patches/spl/README.md).

---

## 3. Official Plugins Maintenance & Git Workflow

Each official plugin under `plugins/` is an independent Git submodule:

| ShortName | Submodule Path | Upstream Repo | Notes |
| :--- | :--- | :--- | :--- |
| `size` | `plugins/size` | `Miraboreasyu/mhw-rng-size` | 71 large monsters, safe open float interval `[1, 0xFFFFFF00]`, live `cSizeTable` inverse solver (`exact`). |
| `inspector` | `plugins/inspector` | `Miraboreasyu/mhw-rng-inspector` | Static dictionary (`rng_catalog_data.inl`), `"mhw.catalog"` service, `$rng top`/`dump`/`find`/`fix`/`trace`. Safe to unload via `$rng unload inspector`. |
| `endemic` | `plugins/endemic` | `Miraboreasyu/mhw-rng-endemic` | Size/crown control + rare endemic life, Great-size fish & GL rare Moly RNG + bilingual Capcom GMD species names + `"mhw.world"` live status & popup notices. |
| `thk` | `plugins/thk` | `Miraboreasyu/mhw-rng-thk` | Generic THK node probe/counter/sequence/preset engine + bundled `thk_presets\fatalis_p1_nasty.txt` + quest/title auto tracker reset. |
| `steam` | `plugins/steam` | `Miraboreasyu/mhw-rng-steam` | Steamworks 3-key/red/pity decoder & optional `lock`; pure direct-microkernel showcase (`mhw_rng_api.h` only). |
| `crit` | `plugins/crit` | `Miraboreasyu/mhw-rng-crit` | Pure RNG seed at `0x01F606D6` + crit evaluator detour at `0x1F51380` (`auto`/`rate`, `random`/`ratio`/`seq`/`mv_bank`/`mv_tier`/`bag`/`prd`, `autoreset [on|off]`). |

### Editing an Official Plugin
1. If you changed [`include/mhw_rng_api.h`](../include/mhw_rng_api.h) or Companion Headers, run `sync_plugin_sdks.bat` first.
2. Edit code inside `plugins/<name>/`, run root `build.bat` (builds all 8 targets) and `run_tests.bat` (must pass Tests 1–25!).
3. **Commit order**:
   ```bash
   git -C plugins/<name> status
   git -C plugins/<name> add ... && git -C plugins/<name> commit -m "..." && git -C plugins/<name> push
   # Then in root repo:
   git add plugins/<name> ... && git commit -m "..." && git push
   ```
   *(Verify local git author identity first in every submodule and root repo.)*

---

## 4. Offline Regression Harness (`tests/test_rng_harness.cpp`)

All **Test 1–25** run **offline** against the real MHW 15.23 `.text` bytes (`text_1523.bin`, 48,304,640 B):

```bat
set MHW_TEXT_BIN=<path-to-text_1523.bin>
run_tests.bat
```

### Coverage Summary
- **Kernel & Hook Mechanics (Tests 1–19 & 24)**: 10 W^X detours, 8-thread contention + `Epoch` rewind, W^X unchained-overwrite recovery, 64 KB trampoline pool (`InstallStandaloneDetour5`), multi-plugin `priority` chain, `FilterVanilla` / `FilterSlot`, `ObserveOnly`, `'MRSS'` binary snapshot restore, Tombstone condition slot reclamation, custom transparent wrapper unwrapping, `CaptureCallerRvas` `.pdata` unwind, private `MtRandom` instance policies, `Selective` + 31 `ProtectSaveSeeds` RVAs, shadow-copy unload/reload & drain, SDK v3.5 main-thread ticks, wildcard event bus + recursion cap, and 4-thread `MhwRngServiceGuard` hot-reload stress (**Test 24**).
- **World Service & Official Plugin Real-Machine-Code Tests**:
  - **Test 25 (`mhw_world.dll` + Companion SDK)**: synthetic 15.23 Title -> Hub -> Guiding Lands -> weather/hour -> quest restart -> Title state transitions, popup queue delivery, `"mhw.catalog"` service resolution, `crit` `autoreset on/off`, live status integration across `size`/`endemic`/`thk`/`crit`, graceful unload degradation, and `steam` minimal showcase isolation.
  - `rng_size`: Rajang / Furious Rajang (`cSizeTable` zero-weight row skip + crown control).
  - `rng_steam` (`V3-9`): real Steamworks machine code decoding & lock.
  - `rng_crit` (**Test 22**): real `0x141F51380` + `0x141F605F0` machine code (coop passthrough, `rate`, `random`, `ratio`, `seq`, `mv_bank`, `mv_tier`, `bag`, `prd`, hot reload).
  - `rng_endemic` (**Test 23**): real rare endemic spawn, Great fish, Guiding Lands Moly, mutual-exclusion tie-breaker, async logs, Simplified Chinese / English species selectors.
  - `rng_thk`: Fatalis P1 preset & THK VM execution.
- **i18n Zero-CJK Sweep (Test 20)**: Executes `$rng lang en`, sweeps every core & official plugin command plus `$rng dump`, and fails immediately if **any** UTF-8 CJK codepoint appears in English output.
- **Console Core & Multi-Level Tab Completion (Test 21)**: Tests UTF-8 readline editing, IME composition/candidate state, history search, and exact Tab completion outputs across core & all 6 official plugins.

> Whenever adding or editing commands/strings in `src/` or any official plugin, always extend **Test 20** (`enCmds`) and **Test 21** (`checkTabComplete`) and re-run `run_tests.bat`!

---

## 5. Release Packaging Checklist (`tools/package_release.py`)

Before cutting a release zip or pushing changes:
1. Run `sync_plugin_sdks.bat && build.bat` -> verify `ALL BUILDS SUCCEEDED`.
2. Run `run_tests.bat` -> confirm `Shadow-copy loaded 7 official modules` and `ALL MICROKERNEL v3.5 & OFFICIAL PLUGIN AUDIT TESTS PASSED`.
3. Run `python tools/package_release.py --check` -> verify `11/11 release payload entries` AND `patches/spl/winmm.dll` SHA256 (`64DAAE7CC00EC2ADA0113EDEE1B95A551383968212E4C2E85EFFFFB8402CED34`).
4. Scan tracked docs and scripts to ensure **0** local file-scheme URIs or drive-letter paths.
5. Run `python tools/package_release.py --out dist\mhw_rng-15.23.zip` -> confirm the zip contains **only** `winmm.dll`, `nativePC/plugins/mhw_rng.dll`, `nativePC/plugins/mhw_rng.ini`, `nativePC/plugins/mhw_rng/mhw_world.dll`, the 6 `nativePC/plugins/mhw_rng/rng_*.dll` binaries, and `thk_presets/fatalis_p1_nasty.txt`.
