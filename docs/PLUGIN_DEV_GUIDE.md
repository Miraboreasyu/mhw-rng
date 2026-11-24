# MIRA Standalone Plugin Developer Guide (SDK v3.4)

This guide enables any developer or AI harness to build **new standalone MIRA (`mhw_rng`) plugins** cleanly **outside** the core `mhw_rng` repository, without touching `mhw_rng`'s `build.bat`, `.gitmodules`, or `tests/`.

> **Target Platform**: *Monster Hunter: World – Iceborne* Steam `15.23` (`Build 15539686`, `ImageBase = 0x140000000`) + `mhw_rng.dll` SDK v3.4 (`kMhwRngApiVersion == 3`).
> **Full C ABI Reference**: [`include/mhw_rng_api.h`](../include/mhw_rng_api.h) | **Microkernel Design**: [`docs/ARCHITECTURE.md`](ARCHITECTURE.md).

---

## 1. Standalone Project Layout & Tooling

Create a new directory alongside `mhw_rng` (e.g. `mods\<your_plugin_repo>\`):

```text
<your_plugin_repo>/
├── AGENTS.md                 (optional harness quickstart for your repo)
├── README.md
├── .gitignore                (ignore build/, *.obj, *.lib, *.exp, *.pdb)
├── include/
│   └── mhw_rng_api.h         (copied by sync_sdk.bat; single-header SDK, no .lib)
├── sync_sdk.bat              (syncs ..\mhw_rng\include\mhw_rng_api.h)
├── build.bat                 (MSVC x64 /LD build; supports --deploy)
└── <plugin_name>_plugin.cpp  (one or more plugin DLL sources)
```

### `sync_sdk.bat` Pattern
Resolve candidate header in order: explicit CLI arg `%~1` -> `%MHW_RNG_SDK_DIR%` -> `..\mhw_rng\include\mhw_rng_api.h`, copy to `include\mhw_rng_api.h`.

### `build.bat [--deploy]` Compiler Flags
Always match `mhw_rng`'s CRT/ABI compilation flags:
```bat
cl.exe /nologo /std:c++17 /O2 /GL /GR- ^
    /W4 /WX- /EHsc /MT ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "NOMINMAX" ^
    /utf-8 ^
    /I "%ROOT%include" ^
    "%ROOT%my_plugin.cpp" ^
    /Fe:"%OUTDIR%\rng_myplugin.dll" ^
    /Fo:"%OUTDIR%\\" ^
    /LD ^
    /link kernel32.lib /OPT:REF /OPT:ICF ^
    /OUT:"%OUTDIR%\rng_myplugin.dll"
```
When `--deploy` is passed, copy the output DLL(s) into `<MHW_DIR>\nativePC\plugins\mhw_rng\`. While MHW is running, load or update without restarting via `$rng load myplugin` / `$rng reload myplugin` (shadow-copy loading into `.hotload\` keeps disk DLLs unlocked).

---

## 2. SDK v3.4 Lifecycle & Registration APIs

Every plugin DLL exports exactly 3 C functions (`MHW_RNG_PLUGIN_EXPORT`):

1. `const MhwRngPluginDescriptor* MhwRngPlugin_GetInfo()`:
   - `targetApiVersion = kMhwRngApiVersion` (`3`)
   - `shortName`: lowercase unique ID (`"coral"`, `"quest"`, etc. — used by `$rng reload <shortName>`)
2. `bool MhwRngPlugin_Init(const MhwRngHostApi* host, MhwRngPluginId selfId)`:
   - Validate `host && host->apiVersion == kMhwRngApiVersion && host->exeBase`.
   - Seed any local PRNG (see Section 5.4).
   - Register metadata, hooks, lifecycle listeners, transparent wrappers, subcommands, and Tab completers:
     - `MhwRng_RegisterPluginInfoBi(host, selfId, zhName, enName, zhDesc, enDesc)`
     - `MhwRng_RegisterSiteMetaBi(host, selfId, rva, alias, zhDesc, enDesc, formatterOrNull, userData)`
     - `host->RegisterTransparentWrapper(selfId, wrapperRetRva, frameQwords)` *(if needed; see Section 3)*
     - `host->RegisterSlotHookEx(selfId, &hookDesc)` *(save `MhwRngHookHandle`)*
     - `host->RegisterLifecycleHook(selfId, &OnKernelLifecycle, nullptr)` *(save `MhwRngHookHandle`)*
     - `MhwRng_RegisterSubcommandBi(host, selfId, subcmd, usage, zhHelp, enHelp, &Cmd_Fn, nullptr)`
     - `MhwRng_RegisterSubcommandCompleter(host, selfId, subcmd, &Complete_Fn, nullptr)`
3. `void MhwRngPlugin_Shutdown()`:
   - Set an atomic `g_shuttingDown = true`.
   - Note: `plugin_host` automatically detaches hooks, drains in-flight hook calls (`g_inFlightHookCalls == 0`), removes subcommands/wrappers/rules before calling `MhwRngPlugin_Shutdown()`, **and** calling `UnregisterSlotHookHandle` / `UnregisterLifecycleHook` / `UnregisterTransparentWrapper` when `g_host != nullptr` is safe & idempotent. Reset state under `g_stateMx` and null `g_host`.

### Hook Decision Flow (`MhwRngHookDesc` + `MhwRngDrawContext`)
- Set `hk.flags = kMhwRngHookPre | kMhwRngHookPost | kMhwRngHookObserveOnly`.
- **In `Takeover` / `Selective` modes**, `preFn(const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void*)` runs:
  - Set `dec->action = kMhwRngActionReturnValue; dec->value0 = ...;` (or `ReturnFloatBits` for `mt19937_envelope_range`) to force an exact RNG output and advance telemetry.
  - Set `dec->action = kMhwRngActionContinue;` when you do **not** wish to override this roll (lets `Selective` run vanilla RNG, `Takeover` run `MasterDerived`, or next hook in chain run).
  - Or use `kMhwRngActionFilterVanilla` / `kMhwRngHookFilterVanilla` (or `FilterSlot`) to inspect/clamp `ctx->vanillaValue0` inside `postFn` when `(ctx->callFlags & kMhwRngDrawFlagCanModify) != 0`.
- **In `ObserveOnly` mode**, kernel skips `preFn`, runs vanilla RNG, sets `ctx->callFlags |= kMhwRngDrawFlagObserveOnly`, and calls `postFn` for passive telemetry/logging (`kMhwRngHookObserveOnly`).

---

## 3. Finding RNG Sites & Unwrapping Helpers

> [!TIP]
> **Before You Start (Ask About Local RE Assets)**: Finding new RNG call sites, wrapping helpers, and struct layouts works best when paired with a **queryable reverse-engineering database** (such as an indexed SQLite/MCP symbol & cross-reference database, a Ghidra project, and an unpacked `15.23` `.text` dump). Before spending turns scanning binaries or hunting patterns from scratch, **ask the user upfront** whether those local RE query tools or databases already exist in their workspace.

### Method A: Built-in Microkernel Tools
- Check [`plugins/inspector/rng_catalog_data.inl`](../plugins/inspector/rng_catalog_data.inl) (1,697 static sites + 793 outer functions) or use `$rng find <keyword>` / `$rng top all` / `$rng trace <site|*>` inside the game.
- **RVA rule**: Every hook RVA in MIRA is the **return address** `(call_insn_va + 5) - 0x140000000`. Note `NormalizeSiteAddress` also accepts the `CALL` instruction address itself when translating user input.

### Method B: Inspecting Caller Context (`MhwRngCallerRegs` & Stack Unwind)
At the exact moment a game call enters any of the 10 intercepted PRNG primitives, [`src/rng_trampolines.asm`](../src/rng_trampolines.asm) captures:
- All 8 non-volatile GPRs: `ctx->callerRegs->rbx`, `rbp`, `rsi`, `rdi`, `r12`, `r13`, `r14`, `r15`
- Non-volatile floats: `ctx->callerRegs->xmm6`, `xmm7` (`f32[4]`, check `kMhwRngDrawFlagHasXmmRegs`)
- Entry stack pointer: `ctx->callerRegs->callerRsp` (`_AddressOfReturnAddress()`)
- Original arguments: `ctx->origArg1`, `ctx->origArg2`, `ctx->origMinVal`, `ctx->origMaxVal`
- Unwrapped return history: `ctx->origReturnRva` holds the direct return RVA before transparent-wrapper unwrapping; `host->CaptureCallerRvas(ctx, outRvas, maxDepth)` performs SEH-safe `.pdata` multi-hop unwinding (`outRvas[0]` = innermost gameplay caller).

### Registering Custom Transparent Wrappers (`RegisterTransparentWrapper`)
Before registering hooks, check whether your gameplay function calls `MtRandom::nrand` directly or via an intermediate utility wrapper:
- 8 wrappers are **already unwrapped built-in** by `ComputeCallSiteRva` in [`src/rng_engine.cpp`](../src/rng_engine.cpp), including `0x0672FBCU` (`0x140672F90` `randRange(min, max)` float helper). Do **NOT** re-register those 8!
- If your caller goes through a tiny custom wrapper not in the built-in 8 (for example `0x140BCDCC0`: integer `base + (nrand() % count)`), calculate its exact stack frame size in `uint64_t` QWORDs (`returnAddrSlot` increments to the wrapper's own return address!) and call `host->RegisterTransparentWrapper(selfId, wrapperRetRva, frameQwords)`:
  - Count: **each `push rXX` (`+1` qword) + `sub rsp, imm` (`imm / 8` qwords) + `1` qword for the caller's return address**.
  - *Worked Example (`0x140BCDCC0`)*:
    ```asm
    0x140BCDCC0: mov qword ptr [rsp + 8], rbx   ; shadow space only (+0)
    0x140BCDCC5: push rdi                       ; +1 qword
    0x140BCDCC6: sub rsp, 0x20                  ; +4 qwords
    ...
    0x140BCDCE6: call 0x142174e30               ; wrapperRetRva = 0x0BCDCEB
    ; Total frameQwords = 1 (push rdi) + 4 (0x20/8) + 1 (return addr) = 6
    ```
  - Caution: Registering the wrong `frameQwords` will misread the caller return address on the stack; always verify the exact wrapper prologue in disassembly and test it with a unit test!

---

## 4. Exact MHW 15.23 RNG Math Patterns

Always replicate the game's exact machine-code float/integer conversion instead of guessing linear scaling:

| Pattern | Vanilla Game Math | Exact Synthesis / Inverse Strategy |
| :--- | :--- | :--- |
| **Integer modulo roll** (`nrand() % N`) | `eax % count`, compare `< thresh` | Direct modulo value: e.g. for `N=100` and `< 20` choose `10U`; for `>= 20` choose `60U`; for exact ordinal `k` (`< N`) choose `k`. |
| **Float `[0, maxF]` / weight table** | `float x = float(int64(uint64(u32))) * (1.0f/2^32) * maxF` | Use safe open interval **`[0x00000001U, 0xFFFFFF00U]`** (`0U` can hit disabled `weight == 0` rows; `0xFFFFFFFFU` rounds in `float32` to `1.0f * maxF`). |
| **`randRange(a, b)`** (`0x140672F90`) | `a + float(int64(uint64(u32))) * 2.3283064e-10f * (b - a)` | `u32 = clamp((targetFloat - a) / (b - a) * 2^32, 1, 0xFFFFFF00)`. (For `rva = 0x01B34BDF` `randRange(0, 120)`: `25.0f -> 0x35555555`, `75.0f -> 0xA0000000`, `110.0f -> 0xEAAAAAAA`). |
| **Ascending quadratic intervals** (e.g. Bongos `0x141991690`) | `ord` in `[ord^2 * 10, (ord+1)^2 * 10]` across `count` items | Midpoint: `num = 2*ord^2 + 2*ord + 1`, `den = 2*count^2`, `u32 = uint32(num * 2^32 / den)`; verify forward `Eval(u32, count) == ord` with a small `delta` search. |
| **Descending quadratic intervals** (e.g. Horn `0x141991220`) | `d = count - ord`, `[(d-1)^2 * 10, d^2 * 10]` | Midpoint: `num = 2*d^2 - 2*d + 1`, `den = 2*count^2`, `u32 = uint32(num * 2^32 / den)`; verify forward `Eval(u32, count) == ord`. |

---

## 5. Mandatory Safety & Quality Checklist

1. **Leaf Mutex Discipline**:
   - **Never** call `g_host->ChatPrint*`, `g_host->GetLanguage`, `g_host->GetEngineMode`, `g_host->QuerySiteMeta`, or game functions while holding `g_stateMx`.
   - Pattern: `{ std::lock_guard<std::mutex> lk(g_stateMx); /* copy to locals */ }` -> then call `g_host->...` outside.
2. **SEH Without C++ Stack Unwinding (`C2712` Safe)**:
   - Put `__try / __except(EXCEPTION_EXECUTE_HANDLER)` in tiny standalone `noexcept` helper functions that declare **no** `std::string`, `std::vector`, or `std::lock_guard` inside the function.
   - Return `false` on fault and gracefully fall back (`decision->action = kMhwRngActionContinue`).
3. **Never Cache Dynamic Game Pointers Across Loads**:
   - Objects like `uPlayer*` (`sPlayer + 0x80`), `statusMgr` (`uPlayer + 0x7D20`), or quest/UI structs are destroyed on Return-to-Title and map loads. Traverse dynamically via SEH each roll.
   - *Bonus*: Comparing the current `uPlayer*` address against `g_lastPlayerUnit` automatically detects quest/map transitions so cooldown timers can auto-reset cleanly.
4. **`Selective` vs `Takeover` Randomness**:
   - If your plugin rolls among allowed options inside `preFn` (like Coral `heroics` filtering), seed a local `Xorshift128` (`g_localRng`) in `MhwRngPlugin_Init` using `QueryPerformanceCounter ^ GetTickCount64() ^ (PID << 32)`. Only call `ctx->DrawDeterministic(...)` when `g_host->GetEngineMode() == kMhwRngModeTakeover`; in `Selective` mode use `g_localRng.Next()` so filtered rolls don't repeat identically across boots when `MasterSeed` is default.
5. **Multiplayer Safety**:
   - Avoid modifying other players' state. Check local player flags before acting (e.g. `(*reinterpret_cast<const uint32_t*>(uPlayer + 0x0C) & 0x0EU) != 0` or `*(uint32_t*)(uPlayer + 0x8938)` local-player flags), or verify the vanilla caller already filters for local ownership.
6. **Full Bilingual Support & Zero-CJK `en` Mode**:
   - Check `MhwRng_HostIsEnglish(g_host)` for **every** command output, `FormatSlotStatus` formatter, `list` command, and in-game popup!
   - Careful pitfall: Do **not** print `zhName (enName)` when `en == true` inside `list` subcommands — print `enName` only in English mode and `zhName (enName)` in Chinese mode.
7. **Empty-Prefix & Multi-Level Tab Completion**:
   - In `Complete_MyPlugin(..., int argc, const char* const* argv, const char* /*prefix*/, ...)`, handle `argc == 0` by emitting all top-level subcommands (`status`, `help`, ...) so pressing `<Tab>` right after `myplugin ` lists all commands immediately. Emit unfiltered candidates at each `argc`; the host terminal handles prefix filtering, deduplication, and sorting.

---

## 6. Offline Testing Against Real MHW Machine Code (Optional Standalone Harness)

Because standalone plugins should not modify `mods/mhw_rng/tests/test_rng_harness.cpp`, you can validate pure-math helpers or create a small standalone console runner in your repo if needed, OR follow the MIRA harness technique:
1. Map `text_1523.bin` at an `0x06000000`-byte `VirtualAlloc(..., PAGE_EXECUTE_READWRITE)` region (`base = VirtualAlloc(...)`, copy `text_1523.bin` to `base + 0x1000`).
2. Populate `.rdata` floats/tables needed by the target function, write minimal return-zero / 12-byte `mov rax, addr; jmp rax` stubs for unrelated helper calls (saving & restoring original bytes + calling `FlushInstructionCache`), and wire synthetic singletons (with guard sentinel bytes like `0xA5` around write targets to catch over-wide stores!).
3. Test directly against `InvokeSlotHooks` or load `mhw_rng.dll` + your plugin to verify real x64 ABI register states, edge masks, `Selective` + `Takeover`, `ObserveOnly`, and Tab completion.
