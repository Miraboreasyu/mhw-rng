# AGENTS.md — `mhw_rng` (MIRA) Harness Onboarding Guide

This file is the cold-start manual for any AI harness or contributor maintaining the **MIRA (`mhw_rng`)** microkernel, its 6 official plugins, or developing external standalone MIRA plugins.

- **Target game**: *Monster Hunter: World – Iceborne* (Steam `15.23`, Build `15539686`, `ImageBase = 0x140000000`, unpacked `.text` size `48,304,640` bytes).
- **Primary architecture spec**: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- **Standalone plugin development manual**: [docs/PLUGIN_DEV_GUIDE.md](docs/PLUGIN_DEV_GUIDE.md)
- **Core maintenance, console & QA manual**: [docs/MAINTENANCE_AND_TESTING.md](docs/MAINTENANCE_AND_TESTING.md)
- **Reverse-engineering database prerequisite**: Maintaining the microkernel, locating new RVAs, verifying game structs/offsets, or running offline machine-code tests (`MHW_TEXT_BIN`) works best alongside a **queryable reverse-engineering database** (e.g., an indexed SQLite/MCP semantic symbol & cross-reference database, a Ghidra project, and an unpacked `15.23` `.text` dump). **Ask the user upfront** at the start of a task whether any local RE database, MCP server/CLI, Ghidra project, or unpacked binary dump already exists in their workspace before manually scanning binaries or disassembling from scratch.

---

## 1. Repository Boundaries & Governance

1. **Frozen Official Plugin Roster (6 Submodules Only)**:
   - Official plugins in [`plugins/`](plugins) are maintained exclusively as Git submodules ([.gitmodules](.gitmodules)): `size`, `inspector`, `endemic`, `thk`, `steam`, `crit`.
   - **Do NOT add new official plugins or new submodules** under `plugins/` and do NOT bundle third-party/standalone plugins into `build.bat`, `sync_plugin_sdks.bat`, `tools/package_release.py`, or `tests/test_rng_harness.cpp`.
   - All new gameplay plugins MUST live as **standalone external projects** that consume [`include/mhw_rng_api.h`](include/mhw_rng_api.h) and deploy their DLLs into `nativePC\plugins\mhw_rng\`. See [docs/PLUGIN_DEV_GUIDE.md](docs/PLUGIN_DEV_GUIDE.md).
2. **Git Submodule & Authoring Hygiene**:
   - Never commit `build/`, `dist/`, `.hotload/`, `*.dll` build outputs, or test scratch files (exception: [`patches/spl/winmm.dll`](patches/spl/winmm.dll)).
   - Never put local file-scheme URIs or machine-local drive paths in public repo files or docs. Keep `git config --local user.name` / `user.email` set to the anonymous project identity when committing/pushing.
   - If modifying both a submodule under `plugins/<name>/` and the root repo, commit & push the **submodule first**, then commit the updated gitlink in the root repo.
3. **Mandatory Release Package Rules ([`tools/package_release.py`](tools/package_release.py))**:
   - Release zips contain **only files loaded by the game** — NEVER include `README`, `LICENSE`, screenshots, source files, or changelogs inside a release zip.
   - Every release zip MUST bundle the patched SharpPluginLoader [`patches/spl/winmm.dll`](patches/spl/winmm.dll) (`SHA256: 64DAAE7CC00EC2ADA0113EDEE1B95A551383968212E4C2E85EFFFFB8402CED34`) at zip root alongside `nativePC\plugins\...`. See [patches/spl/README.md](patches/spl/README.md).

---

## 2. Repository Map

| Path | Purpose |
| :--- | :--- |
| [`include/mhw_rng_api.h`](include/mhw_rng_api.h) | Single-header C ABI SDK (`kMhwRngApiVersion == 3`, v3.4 extensions). Authoritative source; propagated via [`sync_plugin_sdks.bat`](sync_plugin_sdks.bat). |
| [`src/`](src) | Microkernel C++/MASM source: entry/3-phase startup (`dllmain.cpp`), W^X pool (`hook.cpp`/`hook.h`), GPR+XMM capture (`rng_trampolines.asm`), 16K slot table & hook chains (`rng_engine.cpp`/`rng_engine.h`), shadow-copy plugin host (`plugin_host.cpp`/`plugin_host.h`), command router (`command_router.cpp`/`command_router.h`), DX11/DX12 overlay terminal & IME (`console_core.*`, `console_overlay.*`), INI/log (`log_config.*`), static 15.23 RVAs (`mhw_addresses.h`). |
| [`plugins/`](plugins) | 6 official submodule plugins (`size`, `inspector`, `endemic`, `thk`, `steam`, `crit`). |
| [`tests/test_rng_harness.cpp`](tests/test_rng_harness.cpp) | Real-machine-code offline regression suite (**Test 1–23**) mapping unpacked `text_1523.bin` via `VirtualAlloc` (`kImageSize = 0x06000000`). |
| [`patches/spl/`](patches/spl/README.md) | Minimal patched `winmm.dll` preventing post-return-to-title DXGI `ResizeBuffers` WndProc re-subclass loop. |
| [`tools/`](tools) | [`package_release.py`](tools/package_release.py) (`--check` / `--out`), [`gen_rng_catalog.py`](tools/gen_rng_catalog.py), [`annotate_rng_dump.py`](tools/annotate_rng_dump.py). |
| [`mhw_rng.ini`](mhw_rng.ini) | Default runtime INI shipped inside `nativePC\plugins\mhw_rng.ini`. |

---

## 3. Build, Test & Packaging Commands

Run from an MSVC x64 developer environment (or `build.bat` / `run_tests.bat` auto-locates VS via `vswhere.exe`):

```bat
:: 1. Sync SDK header to the 6 official plugins & build all 7 binaries + preset
sync_plugin_sdks.bat && build.bat

:: 2. Run all offline machine-code tests (Test 1..23; requires MHW 15.23 .text dump)
set MHW_TEXT_BIN=<path-to-text_1523.bin>
run_tests.bat

:: 3. Validate release payload (10/10 files + patched winmm.dll SHA256 guard)
python tools/package_release.py --check

:: 4. Build clean release zip (only game-loaded binaries/INI/preset)
python tools/package_release.py --out dist\mhw_rng-15.23.zip
```

---

## 4. Non-Negotiable Engineering Invariants

1. **Pure-RNG Discipline**: Prefer `RegisterSlotHookEx` (`Pre` / `Post` / `FilterVanilla` / `FilterSlot`) over business-function detours. Core detours live strictly in the 64 KB `PAGE_EXECUTE_READ` W^X pool ([`hook.cpp`](src/hook.cpp)).
2. **Leaf Mutex Discipline**: Neither the microkernel nor any plugin may hold an internal mutex while calling across the plugin/host boundary (`g_host->...` or plugin hooks/formatters). Snapshot state under `std::lock_guard`, unlock, then call out.
3. **SEH Safety**: Accesses to game memory pointers in callbacks or host unwinding (`CaptureCallerRvas`) must validate user-mode address bounds and run inside `__try / __except` helpers with **zero C++ objects requiring stack unwinding** in the SEH frame.
4. **4 Engine Modes (`Selective` default)**: Never alter untouched game calls or the 31 save-persisted seed steppers in `Selective` or `ObserveOnly` (`Pre` is skipped in `ObserveOnly`; only hooks with `kMhwRngHookObserveOnly` receive read-only `Post`).
5. **Bilingual Zero-CJK Sweep (Test 20)**: Switching `$rng lang en` triggers Test 20's CJK scan over all core & official plugin chat responses and `$rng dump`. Both English and Simplified Chinese strings must be complete and cleanly separated (`MhwRng_HostIsEnglish`).
6. **In-Game Terminal & IME Contract**: [`console_core.cpp`](src/console_core.cpp) + [`console_overlay.cpp`](src/console_overlay.cpp) + [`dllmain.cpp`](src/dllmain.cpp) (`ConsoleWndProc`) handle UTF-8 readline, `WM_IME_COMPOSITION`/`GCS_RESULTSTR`/`WM_IME_NOTIFY` candidate rendering, `Ctrl+R`, mouse selection, and dynamic plugin `Tab` completion (never call blocking game functions from `OnPresent`).
