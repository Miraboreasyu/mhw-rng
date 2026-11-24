// Standalone Hardware/Binary Verification Suite for MHW RNG Microkernel v2 + Official Plugins:
//   1. Maps real 48.3 MB MHW 15.23 `.text` (`text_1523.bin`) & seals all 10 W^X detours.
//   2. Verifies 8-thread stream isolation & O(1) Epoch rewind.
//   3. Verifies 64-bit Composite `(condId, callSiteRva)` Push & Pull Condition Routing.
//   4. Verifies Bug #1 Fix: Real x64 `RBX = Monster*` stack-frame conditional routing for
//      `Rajang` (`MonsterID = 91`) vs `FuriousRajang` (`MonsterID = 92`) vs `Barroth` (`MonsterID = 21`)!
//   5. Verifies Bug #2 Fix: Restricted "Mew are Number One!" (`黄金奖杯`) `cSizeTable` where leading rows
//      `88%..99%` have `weight = 0` — proves `$rng size min`, `$rng size 0`, `$rng fix size min`, and
//      `$rng fix size 0` NEVER return illegal `88%` (`0.0f <= 0.0f`) and always land on `100%` (first legal row)!
//   6. Verifies Official Plugin #3 (`rng_endemic.dll`): Real MHW 15.23 `0x141669AC0 -> 0x14166CCC0`
//      crown tier + intra-tier interpolation, per-species `ec059` / `ec059_02` / `ec051_00` routing,
//      live `rAnimalSize` `exact` scale/cm solver, and shared `0x140672F90` non-endemic caller isolation!
//   7. Verifies Zero-Lock Shadow-Copy Hot-Reload (`ReloadSingle` + `UnloadSingle`)!
#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#include "command_router.h"
#include "console_core.h"
#include "hook.h"
#include "log_config.h"
#include "mhw_addresses.h"
#include "mhw_rng_api.h"
#include "plugin_host.h"
#include "rng_engine.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using TestNrandFn       = std::uint32_t(__fastcall*)(std::uint32_t*);
using TestCtorSeedFn    = std::uint32_t*(__fastcall*)(std::uint32_t*, std::uint32_t);
using TestSphereFn      = float*(__fastcall*)(std::uint32_t*, float*);
using TestMtRangeFn     = float(__fastcall*)(void*, float, float);
// Executes `fn_r9(holder_rcx, tblIdx_rdx)` with non-volatile `RBX = monster_r8`, reproducing `FUN_141ccfb70`!
using CallWithMonsterRbxFn = std::uint32_t(__fastcall*)(void* holder, std::uint32_t* tblIdxPtr, void* monster, void* targetFn);
using EndemicCaptureRollFn = std::uint32_t(__fastcall*)(std::int32_t animalId, std::int32_t animalType, float* outScale);
using SharedFloatRangeFn   = float(__fastcall*)(float minVal, float maxVal);

volatile std::uint32_t g_antiTailCallSink = 0;
std::vector<std::string> g_capturedChatLines;
std::vector<std::string> g_capturedAsyncChatLines;

void CaptureChatSink(const char* utf8Text) noexcept {
    if (utf8Text) {
        g_capturedChatLines.emplace_back(utf8Text);
        std::printf("  [chat] %s\n", utf8Text);
    }
}

void CaptureAsyncChatSink(const char* utf8Text) noexcept {
    if (utf8Text) {
        g_capturedAsyncChatLines.emplace_back(utf8Text);
    }
}

__declspec(noinline) std::uint32_t CallNrandSiteA(TestNrandFn fn, std::uint32_t* sInst) {
    const std::uint32_t r = fn(sInst);
    g_antiTailCallSink = r ^ 1U;
    return r;
}
__declspec(noinline) std::uint32_t CallNrandSiteB(TestNrandFn fn, std::uint32_t* sInst) {
    const std::uint32_t r = fn(sInst);
    g_antiTailCallSink = r ^ 2U;
    return r;
}
__declspec(noinline) std::uint32_t CallNrandV3Chain(TestNrandFn fn, std::uint32_t* sInst) {
    const std::uint32_t r = fn(sInst);
    g_antiTailCallSink = r ^ 3U;
    return r;
}
__declspec(noinline) std::uint32_t CallNrandV3Filter(TestNrandFn fn, std::uint32_t* sInst) {
    const std::uint32_t r = fn(sInst);
    g_antiTailCallSink = r ^ 4U;
    return r;
}
__declspec(noinline) std::uint32_t CallNrandV3Observe(TestNrandFn fn, std::uint32_t* sInst) {
    const std::uint32_t r = fn(sInst);
    g_antiTailCallSink = r ^ 5U;
    return r;
}
__declspec(noinline) std::uint32_t CallNrandV3Priv(TestNrandFn fn, std::uint32_t* privInst) {
    const std::uint32_t r = fn(privInst);
    g_antiTailCallSink = r ^ 6U;
    return r;
}
__declspec(noinline) std::uint32_t* CallCtorSeedV3Priv(TestCtorSeedFn fn, std::uint32_t* privInst, std::uint32_t seed) {
    std::uint32_t* p = fn(privInst, seed);
    g_antiTailCallSink = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p));
    return p;
}
__declspec(noinline) float CallMtRangeV3(TestMtRangeFn fn, void* ctx, float lo, float hi) {
    const float r = fn(ctx, lo, hi);
    _ReadWriteBarrier();
    return r;
}
__declspec(noinline) std::uint32_t CallNoiseWorker(TestNrandFn fn, std::uint32_t* sInst) {
    const std::uint32_t r = fn(sInst);
    _ReadWriteBarrier();
    return r;
}
__declspec(noinline) void CallSphereSite(TestSphereFn fn, std::uint32_t* sInst, float* out3) {
    fn(sInst, out3);
    _ReadWriteBarrier();
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== MHW RNG Microkernel v3.0 + Official Plugin Audit Suite ===\n");

    constexpr std::size_t kImageSize = 0x06000000ULL;
    auto* img = static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!img) return 1;

    char modPath[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, modPath, MAX_PATH);
    std::string buildDir(modPath);
    buildDir = buildDir.substr(0, buildDir.find_last_of("\\/"));

    FILE* tf = nullptr;
    std::vector<std::string> textBinCands;
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') textBinCands.emplace_back(argv[1]);
    char envBuf[MAX_PATH]{};
    if (::GetEnvironmentVariableA("MHW_TEXT_BIN", envBuf, MAX_PATH) > 0) {
        textBinCands.emplace_back(envBuf);
    }
    textBinCands.emplace_back("text_1523.bin");
    textBinCands.emplace_back(buildDir + "\\text_1523.bin");
    textBinCands.emplace_back(buildDir + "\\..\\..\\..\\work\\gzf\\text_1523.bin");
    for (const std::string& cand : textBinCands) {
        if (fopen_s(&tf, cand.c_str(), "rb") == 0 && tf != nullptr) break;
        tf = nullptr;
    }
    if (!tf) {
        std::printf("FAIL: could not open text_1523.bin (pass path via argv[1] or MHW_TEXT_BIN env var)\n");
        return 1;
    }
    const std::size_t nRead = std::fread(img + 0x1000, 1, 0x03000000ULL, tf);
    std::fclose(tf);
    std::printf("[OK] Mapped text_1523.bin (%zu bytes) at %p\n", nRead, img);

    const auto base = reinterpret_cast<std::uintptr_t>(img);
    const auto end  = base + kImageSize;

    // Populate `.rdata` float constants used by `cSizeTable::getSizeMultiplier`,
    // `FUN_14166ccc0`, `0x141669AC0`, and `FUN_140672f90`:
    *reinterpret_cast<float*>(base + 0x2F173C4U) = 100.0f / 4294967296.0f;
    *reinterpret_cast<float*>(base + 0x2F12798U) = 1.0f / 4294967296.0f;
    *reinterpret_cast<float*>(base + 0x2E4FE48U) = 1.192092896e-7f; // FLT_EPSILON
    *reinterpret_cast<float*>(base + 0x2E4FE58U) = 1.0f;            // __0f
    *reinterpret_cast<float*>(base + 0x2F127C4U) = 1000.0f;
    *reinterpret_cast<float*>(base + 0x2F17850U) = -1000.0f;
    *reinterpret_cast<float*>(base + 0x2F0F354U) = 0.01f;
    *reinterpret_cast<float*>(base + 0x2F16EC4U) = 100.0f;

    // Emit 15-byte x64 ABI thunk reproducing `FUN_141ccfb70` (`RBX = Monster*` -> `call FUN_140f5f760`):
    //   53             push rbx
    //   48 83 EC 20    sub  rsp, 20h
    //   49 8B D8       mov  rbx, r8
    //   41 FF D1       call r9
    //   48 83 C4 20    add  rsp, 20h
    //   5B             pop  rbx
    //   C3             ret
    static constexpr std::uint8_t kRbxCallerThunk[] = {
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xD8,
        0x41, 0xFF, 0xD1, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3
    };
    // Emits 26-byte ABI thunk matching `cThinkEm::update` (0x14132B7B0) stack frame before `0x14132B82C`:
    //   void CallEmThinkUpdate(void* thinkEm_rcx, void* monster_rdx, void* target_r8 /* base + 0x132B82C */)
    static constexpr std::uint8_t kEmThinkUpdateThunk[] = {
        0x53,                         // push rbx
        0x56,                         // push rsi
        0x57,                         // push rdi
        0x48, 0x83, 0xEC, 0x40,       // sub  rsp, 40h
        0x48, 0x89, 0x6C, 0x24, 0x68, // mov  [rsp+68h], rbp
        0x4C, 0x89, 0x74, 0x24, 0x70, // mov  [rsp+70h], r14
        0x48, 0x8B, 0xF1,             // mov  rsi, rcx
        0x48, 0x8B, 0xFA,             // mov  rdi, rdx
        0x41, 0xFF, 0xE0              // jmp  r8
    };
    auto* thunkMem = static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!thunkMem) return 1;
    std::memcpy(thunkMem, kRbxCallerThunk, sizeof(kRbxCallerThunk));
    std::memcpy(thunkMem + 0x100U, kEmThinkUpdateThunk, sizeof(kEmThinkUpdateThunk));
    const auto callSizeWithMonster = reinterpret_cast<CallWithMonsterRbxFn>(thunkMem);
    using EmThinkUpdateThunkFn = void(__fastcall*)(void* thinkEm, void* monster, void* targetAddr);
    const auto callEmThinkUpdate = reinterpret_cast<EmThinkUpdateThunkFn>(thunkMem + 0x100U);

    if (!hook::InitArena(base, end)) return 1;

    app::Config cfg{};
    cfg.masterSeed     = 0xCAFEBABE12345678ULL;
    cfg.takeoverOnBoot = true;
    cfg.bootMode       = kMhwRngModeTakeover;
    rng::Init(base, end, cfg);

    void *origNrand = nullptr, *origCtor = nullptr, *origDef = nullptr, *origSrand = nullptr, *origSph = nullptr;
    void *origRdev  = nullptr, *origMtRng = nullptr, *origMtIni = nullptr, *origMtDrw = nullptr, *origCrt = nullptr;

    const bool okHooks =
        hook::PrepareDetour(base + mhw::kRvaMtRandomNrand,       sizeof(mhw::kPrologueMtRandomNrand),       reinterpret_cast<void*>(&MhwRngAsm_MtRandomNrand),       &origNrand, mhw::kPrologueMtRandomNrand) &&
        hook::PrepareDetour(base + mhw::kRvaMtRandomCtorSeed,    sizeof(mhw::kPrologueMtRandomCtorSeed),    reinterpret_cast<void*>(&MhwRngAsm_MtRandomCtorSeed),    &origCtor,  mhw::kPrologueMtRandomCtorSeed) &&
        hook::PrepareDetour(base + mhw::kRvaMtRandomCtorDefault, sizeof(mhw::kPrologueMtRandomCtorDefault), reinterpret_cast<void*>(&MhwRngAsm_MtRandomCtorDefault), &origDef,   mhw::kPrologueMtRandomCtorDefault) &&
        hook::PrepareDetour(base + mhw::kRvaMtRandomSrand,       sizeof(mhw::kPrologueMtRandomSrand),       reinterpret_cast<void*>(&MhwRngAsm_MtRandomSrand),       &origSrand, mhw::kPrologueMtRandomSrand) &&
        hook::PrepareDetour(base + mhw::kRvaMtRandomSphere,      sizeof(mhw::kPrologueMtRandomSphere),      reinterpret_cast<void*>(&MhwRngAsm_MtRandomSphere),      &origSph,   mhw::kPrologueMtRandomSphere) &&
        hook::PrepareDetour(base + mhw::kRvaStdRandomDevice,     sizeof(mhw::kPrologueStdRandomDevice),     reinterpret_cast<void*>(&MhwRngAsm_StdRandomDevice),     &origRdev,  mhw::kPrologueStdRandomDevice) &&
        hook::PrepareDetour(base + mhw::kRvaMt19937Range,        sizeof(mhw::kPrologueMt19937Range),        reinterpret_cast<void*>(&MhwRngAsm_Mt19937Range),        &origMtRng, mhw::kPrologueMt19937Range) &&
        hook::PrepareDetour(base + mhw::kRvaMt19937HeapInit,     sizeof(mhw::kPrologueMt19937HeapInit),     reinterpret_cast<void*>(&MhwRngAsm_Mt19937HeapInit),     &origMtIni, mhw::kPrologueMt19937HeapInit) &&
        hook::PrepareDetour(base + mhw::kRvaMt19937HeapDraw,     sizeof(mhw::kPrologueMt19937HeapDraw),     reinterpret_cast<void*>(&MhwRngAsm_Mt19937HeapDraw),     &origMtDrw, mhw::kPrologueMt19937HeapDraw) &&
        hook::PrepareDetour(base + mhw::kRvaCrtRand,             sizeof(mhw::kPrologueCrtRand),             reinterpret_cast<void*>(&MhwRngAsm_CrtRand),             &origCrt,   mhw::kPrologueCrtRand);
    if (!okHooks) return 1;
    if (std::memcmp(img + mhw::kRvaSChatMove, mhw::kPrologueSChatMove, sizeof(mhw::kPrologueSChatMove)) != 0) {
        std::printf("FAIL: sChat::move prologue mismatch at 0x%llX\n", static_cast<unsigned long long>(mhw::kRvaSChatMove));
        return 1;
    }

    rng::SetOrigMtRandomNrand(origNrand);
    rng::SetOrigMtRandomCtorSeed(origCtor);
    rng::SetOrigMtRandomCtorDefault(origDef);
    rng::SetOrigMtRandomSrand(origSrand);
    rng::SetOrigMtRandomSphere(origSph);
    rng::SetOrigStdRandomDevice(origRdev);
    rng::SetOrigMt19937Range(origMtRng);
    rng::SetOrigMt19937HeapInit(origMtIni);
    rng::SetOrigMt19937HeapDraw(origMtDrw);
    rng::SetOrigCrtRand(origCrt);

    if (!hook::SealAndActivateAll()) return 1;
    std::printf("[OK] All 10 W^X detours sealed & activated!\n");

    auto fnNrand  = reinterpret_cast<TestNrandFn>(base + mhw::kRvaMtRandomNrand);
    auto fnSphere = reinterpret_cast<TestSphereFn>(base + mhw::kRvaMtRandomSphere);
    auto* sInst   = reinterpret_cast<std::uint32_t*>(base + mhw::kRvaMtRandomInstance);

    // 1. Test 8-thread contention + Epoch rewind
    std::atomic<bool> stopNoise{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&]() {
            while (!stopNoise.load(std::memory_order_relaxed)) {
                CallNoiseWorker(fnNrand, sInst);
            }
        });
    }
    MhwRng_SetMasterSeed(20261122ULL, true);
    std::uint32_t seqA1[32]{}, seqB1[32]{};
    for (int i = 0; i < 32; ++i) {
        seqA1[i] = CallNrandSiteA(fnNrand, sInst);
        seqB1[i] = CallNrandSiteB(fnNrand, sInst);
    }
    ::Sleep(20);
    MhwRng_ResetEpoch();
    std::uint32_t seqA2[32]{}, seqB2[32]{};
    for (int i = 0; i < 32; ++i) {
        seqB2[i] = CallNrandSiteB(fnNrand, sInst);
        seqA2[i] = CallNrandSiteA(fnNrand, sInst);
    }
    stopNoise.store(true, std::memory_order_release);
    for (auto& th : workers) th.join();

    if (std::memcmp(seqA1, seqA2, sizeof(seqA1)) != 0 || std::memcmp(seqB1, seqB2, sizeof(seqB1)) != 0) {
        std::printf("FAIL: Stream isolation mismatch\n");
        return 1;
    }
    std::printf("[OK] 8-thread stream isolation & O(1) Epoch rewind verified!\n");

    // 2. Load Official Plugins (`rng_inspector.dll` + `rng_size.dll` + `rng_endemic.dll` + `rng_thk.dll` + `rng_steam.dll` + `rng_crit.dll`) via Shadow-Copy Host
    const std::string plugDir = buildDir + "\\mhw_rng";

    plugin_host::Init(base, end, plugDir, &CaptureChatSink, &CaptureAsyncChatSink);
    const std::size_t loadedCnt = plugin_host::LoadAllFromDirectory();
    if (loadedCnt != 6U) {
        std::printf("FAIL: Expected == 6 plugins in %s, loaded %zu\n", plugDir.c_str(), loadedCnt);
        return 1;
    }
    std::printf("[OK] Shadow-copy loaded %zu official plugins (`inspector` + `size` + `endemic` + `thk` + `steam` + `crit`)!\n", loadedCnt);

    // 3. Build TWO real-world 38-row `cSizeTable`s:
    //    - Table 0 (`fullCrownTbl`): Standard Crown table (`88% .. 125%`, all 38 rows have `weight > 0`)
    //    - Table 1 (`goldenTrophyTbl`): Real "Mew are Number One!" (`黄金奖杯` Furious Rajang) range `90% .. 118%`,
    //      where rows `0..1` (`88%, 89%`) and `31..37` (`119%..125%`) have `weight = 0`!
    struct SyntheticSizeTable {
        std::uint8_t pad0[0x20]{};
        struct { std::int32_t sizePct; std::int32_t weight; std::int32_t p0; std::int32_t p1; } rows[38]{};
    } fullCrownTbl{}, goldenTrophyTbl{};

    for (int i = 0; i < 38; ++i) {
        fullCrownTbl.rows[i].sizePct    = 88 + i;
        fullCrownTbl.rows[i].weight     = (i == 0 || i == 37) ? 3 : ((i == 12) ? 24 : 2); // 2*3 + 24 + 35*2 = 100
        goldenTrophyTbl.rows[i].sizePct = 88 + i;
        // Rows 0..1 (`88%, 89%`) & 31..37 (`119%..125%`) have weight = 0! Legal range = `90%..118%` (29 rows)!
        goldenTrophyTbl.rows[i].weight  = (i >= 2 && i <= 30) ? 3 : 0; // 29*3 = 87
    }
    goldenTrophyTbl.rows[12].weight += 13; // Exact sum = 100 (87 + 13 = 100)

    void* tablePtrArray[2] = { &fullCrownTbl, &goldenTrophyTbl };
    std::uint8_t synHolder[0x110]{};
    *reinterpret_cast<std::uint32_t*>(synHolder + 0xE8U) = 2U;
    *reinterpret_cast<void***>(synHolder + 0xF8U)        = tablePtrArray;
    *reinterpret_cast<void**>(base + 0x5012908U)         = synHolder;

    // Allocate 3 synthetic 8-byte-aligned `Monster` instances (`sizeof >= 0x12300`):
    //   - `monRajang`        : `MonsterID = 91` (`rajang`),         `tableIdx = 0` (`88%..125%`)
    //   - `monFuriousRajang` : `MonsterID = 92` (`furious_rajang`), `tableIdx = 1` (`90%..118%` "黄金奖杯"!)
    //   - `monBarroth`       : `MonsterID = 21` (`barroth`),        `tableIdx = 0` (`88%..125%`)
    alignas(16) static std::uint8_t monRajang[0x12300]{};
    alignas(16) static std::uint8_t monFuriousRajang[0x12300]{};
    alignas(16) static std::uint8_t monBarroth[0x12300]{};

    *reinterpret_cast<std::uint32_t*>(monRajang        + 0x12280U) = 91U; // Rajang
    *reinterpret_cast<std::uint32_t*>(monRajang        + 0x773CU)  = 0U;
    *reinterpret_cast<std::uint32_t*>(monFuriousRajang + 0x12280U) = 92U; // FuriousRajang
    *reinterpret_cast<std::uint32_t*>(monFuriousRajang + 0x773CU)  = 1U;
    *reinterpret_cast<std::uint32_t*>(monBarroth       + 0x12280U) = 21U; // Barroth
    *reinterpret_cast<std::uint32_t*>(monBarroth       + 0x773CU)  = 0U;

    void* const fnFun0F5F760 = reinterpret_cast<void*>(base + 0x0F5F760U);
    auto spawnMonster = [&](std::uint8_t* mon) -> std::uint32_t {
        auto* tblIdxField = reinterpret_cast<std::uint32_t*>(mon + 0x773CU);
        return callSizeWithMonster(synHolder, tblIdxField, mon, fnFun0F5F760);
    };

    // 4. Verify Bug #1 + Bug #2 Simultaneously:
    //    - Lock `rajang` (`#91`) to `max` (`125%`)
    //    - Lock `furious_rajang` (`#92`) to `min` on "黄金奖杯" table (MUST be `90%`, NEVER illegal `88%`!)
    //    - Leave `barroth` (`#21`) unlocked (normal deterministic roll, neither forced 88% nor 125%)!
    plugin_host::DispatchPluginSubcommand("size", {"rajang", "max"}, "rajang max");
    plugin_host::DispatchPluginSubcommand("size", {"furious_rajang", "min"}, "furious_rajang min");

    const std::uint32_t szRajang  = spawnMonster(monRajang);
    const std::uint32_t szFurious = spawnMonster(monFuriousRajang);
    const std::uint32_t szBarroth = spawnMonster(monBarroth);

    if (szRajang != 125U) {
        std::printf("FAIL [Bug #1]: `$rng size rajang max` expected 125%%, got %u%%\n", szRajang);
        return 1;
    }
    if (szFurious != 90U) {
        std::printf("FAIL [Bug #1/#2]: `$rng size furious_rajang min` on Golden Trophy table expected legal min 90%%, got %u%% (88%% = illegal 0-weight row!)\n", szFurious);
        return 1;
    }
    if (szBarroth < 89U || szBarroth > 124U) {
        std::printf("FAIL: Unlocked Barroth should roll normal size, got %u%%\n", szBarroth);
        return 1;
    }
    std::printf("[OK] Bug #1 & #2 verified: Rajang(#91)=%u%%, GoldenTrophy FuriousRajang(#92)=%u%% (0-weight 88%%/89%% skipped!), Unlocked Barroth(#21)=%u%%!\n",
                szRajang, szFurious, szBarroth);

    // 5. Verify `$rng size 0` AND `rng_inspector.dll`'s `$rng fix size min` / `$rng fix size 0` on Golden Trophy table!
    plugin_host::DispatchPluginSubcommand("size", {"clear"}, "clear");
    plugin_host::DispatchPluginSubcommand("size", {"0"}, "0");
    if (spawnMonster(monFuriousRajang) != 90U) {
        std::printf("FAIL [Bug #2]: `$rng size 0` hit illegal 0-weight row!\n");
        return 1;
    }
    plugin_host::DispatchPluginSubcommand("size", {"clear"}, "clear");
    plugin_host::DispatchPluginSubcommand("fix", {"size", "min"}, "size min");
    if (spawnMonster(monFuriousRajang) != 90U) {
        std::printf("FAIL [Bug #2]: `$rng fix size min` hit illegal 0-weight row!\n");
        return 1;
    }
    plugin_host::DispatchPluginSubcommand("fix", {"size", "0"}, "size 0");
    if (spawnMonster(monFuriousRajang) != 90U) {
        std::printf("FAIL [Bug #2]: `$rng fix size 0` hit illegal 0-weight row!\n");
        return 1;
    }
    plugin_host::DispatchPluginSubcommand("clear", {"size"}, "size");
    std::printf("[OK] Both `$rng size 0` and `$rng fix size min/0` verified immune to `0.0f <= 0.0f` illegal size bug!\n");

    // 6. Verify Wildcard Family `rajang*` + `exact 116` live `cSizeTable` inverse solver!
    plugin_host::DispatchPluginSubcommand("size", {"rajang*", "exact", "116"}, "rajang* exact 116");
    const std::uint32_t exR1 = spawnMonster(monRajang);
    const std::uint32_t exR2 = spawnMonster(monFuriousRajang);
    if (exR1 != 116U || exR2 != 116U) {
        std::printf("FAIL: `rajang* exact 116` expected 116/116, got %u/%u\n", exR1, exR2);
        return 1;
    }
    plugin_host::DispatchPluginSubcommand("size", {"history"}, "history");
    std::printf("[OK] Wildcard `rajang* exact 116` matched both #91 (%u%%) & #92 (%u%%)!\n", exR1, exR2);

    // 7. Verify Official Plugin #3 (`rng_endemic.dll`) on Real MHW 15.23 Endemic Size Machine Code:
    //    `0x141669AC0 -> 0x14166CCC0 -> 0x142174E30` + `0x140672F90` shared-caller isolation!
    alignas(16) static std::uint8_t synPlayer[0xB000]{};
    alignas(16) static std::uint8_t synFoodSkills[0x1000]{};
    alignas(16) static std::uint8_t synAnimalMgr[0xC00]{};
    *reinterpret_cast<void**>(synPlayer + 0xAE40U) = synFoodSkills;
    *reinterpret_cast<void**>(base + 0x500ECA0U)   = synPlayer;
    *reinterpret_cast<void**>(base + 0x5012938U)   = synAnimalMgr;

    struct SyntheticAnimalRow {
        std::uint8_t raw[0x60]{};
        void Init(std::int32_t id, std::int32_t tp, float baseCm,
                  std::int32_t probMode, std::int32_t sizeMode,
                  float minS, float maxS,
                  float rSmallHi, float rNormLo, float rNormHi, float rLargeLo,
                  float smallProb, float largeProb) {
            *reinterpret_cast<std::int32_t*>(raw + 0x08U) = id;
            *reinterpret_cast<std::int32_t*>(raw + 0x0CU) = tp;
            *reinterpret_cast<float*>(raw + 0x24U)        = baseCm;
            *reinterpret_cast<std::int32_t*>(raw + 0x28U) = probMode;
            *reinterpret_cast<std::int32_t*>(raw + 0x2CU) = sizeMode;
            *reinterpret_cast<float*>(raw + 0x30U)        = minS;
            *reinterpret_cast<float*>(raw + 0x34U)        = maxS;
            *reinterpret_cast<float*>(raw + 0x38U)        = rSmallHi;
            *reinterpret_cast<float*>(raw + 0x3CU)        = rNormLo;
            *reinterpret_cast<float*>(raw + 0x40U)        = rNormHi;
            *reinterpret_cast<float*>(raw + 0x44U)        = rLargeLo;
            *reinterpret_cast<float*>(raw + 0x48U)        = smallProb;
            *reinterpret_cast<float*>(raw + 0x4CU)        = largeProb;
        }
    };
    alignas(16) static SyntheticAnimalRow animalRows[4]{};
    // Range 0.80..1.25 (span=0.45): Mini=[0.80, 0.90], Normal=[0.91, 1.14], Giant=[1.15, 1.25]
    animalRows[0].Init(0,  0, 20.0f, 1, 1, 0.80f, 1.25f, 0.222222f, 0.244444f, 0.755556f, 0.777778f, 0.03f, 0.03f);
    animalRows[1].Init(59, 0, 20.0f, 1, 1, 0.80f, 1.25f, 0.222222f, 0.244444f, 0.755556f, 0.777778f, 0.03f, 0.03f);
    animalRows[2].Init(59, 2, 20.0f, 1, 1, 0.80f, 1.25f, 0.222222f, 0.244444f, 0.755556f, 0.777778f, 0.03f, 0.03f);
    animalRows[3].Init(51, 0, 20.0f, 1, 1, 0.80f, 1.25f, 0.222222f, 0.244444f, 0.755556f, 0.777778f, 0.03f, 0.03f);

    void* animalRowPtrs[4] = { &animalRows[0], &animalRows[1], &animalRows[2], &animalRows[3] };
    alignas(16) static std::uint8_t synAnimalSize[0x100]{};
    *reinterpret_cast<std::uint32_t*>(synAnimalSize + 0xB8U) = 4U;
    *reinterpret_cast<void***>(synAnimalSize + 0xC8U)        = animalRowPtrs;
    *reinterpret_cast<void**>(base + 0x5012740U)             = synAnimalSize;

    const auto fnEndemicCapture = reinterpret_cast<EndemicCaptureRollFn>(base + 0x1669AC0U);
    const auto fnSharedRange    = reinterpret_cast<SharedFloatRangeFn>(base + 0x0672F90U);

    // 7a. Global Mini / Giant / Normal crown locks
    float outScale = 0.0f;
    plugin_host::DispatchPluginSubcommand("endemic", {"mini"}, "mini");
    const std::uint32_t crMini = fnEndemicCapture(59, 0, &outScale);
    if (crMini != 2U || std::fabs(outScale - 0.80f) > 0.015f) {
        std::printf("FAIL [endemic mini]: expected crown=2 scale=0.80, got crown=%u scale=%.3f\n", crMini, outScale);
        return 1;
    }

    plugin_host::DispatchPluginSubcommand("endemic", {"giant"}, "giant");
    const std::uint32_t crGiant = fnEndemicCapture(59, 0, &outScale);
    if (crGiant != 1U || std::fabs(outScale - 1.25f) > 0.015f) {
        std::printf("FAIL [endemic giant]: expected crown=1 scale=1.25, got crown=%u scale=%.3f\n", crGiant, outScale);
        return 1;
    }

    // 7b. Verify shared `FUN_140672f90` non-endemic caller guard while `$rng endemic giant` is active!
    const float nonEndemicVal = fnSharedRange(10.0f, 20.0f);
    if (nonEndemicVal <= 10.01f || nonEndemicVal >= 19.99f) {
        std::printf("FAIL [endemic guard]: non-endemic caller of 0x140672F90 was skewed to %.4f!\n", nonEndemicVal);
        return 1;
    }

    plugin_host::DispatchPluginSubcommand("endemic", {"normal"}, "normal");
    const std::uint32_t crNorm = fnEndemicCapture(59, 0, &outScale);
    if (crNorm != 0U || outScale < 0.90f || outScale > 1.15f) {
        std::printf("FAIL [endemic normal]: expected crown=0 scale in [0.91,1.14], got crown=%u scale=%.3f\n", crNorm, outScale);
        return 1;
    }

    // 7c. Per-species conditional routing (`ec059` family = giant, `ec059_02` override = mini, `ec051_00` unlocked)
    plugin_host::DispatchPluginSubcommand("endemic", {"clear"}, "clear");
    plugin_host::DispatchPluginSubcommand("endemic", {"ec059", "giant"}, "ec059 giant");
    plugin_host::DispatchPluginSubcommand("endemic", {"ec059_02", "mini"}, "ec059_02 mini");

    float sc59_00 = 0.0f, sc59_02 = 0.0f, sc51_00 = 0.0f;
    const std::uint32_t c59_00 = fnEndemicCapture(59, 0, &sc59_00);
    const std::uint32_t c59_02 = fnEndemicCapture(59, 2, &sc59_02);
    const std::uint32_t c51_00 = fnEndemicCapture(51, 0, &sc51_00);
    if (c59_00 != 1U || std::fabs(sc59_00 - 1.25f) > 0.015f ||
        c59_02 != 2U || std::fabs(sc59_02 - 0.80f) > 0.015f ||
        c51_00 != 0U) {
        std::printf("FAIL [endemic species]: ec059_00=(%u,%.2f) ec059_02=(%u,%.2f) ec051_00=(%u,%.2f)\n",
                    c59_00, sc59_00, c59_02, sc59_02, c51_00, sc51_00);
        return 1;
    }

    // 7d. Live `rAnimalSize` exact scale (`1.20x`) and exact cm (`23.6cm` on 20cm base -> `1.18x`)
    plugin_host::DispatchPluginSubcommand("endemic", {"clear"}, "clear");
    plugin_host::DispatchPluginSubcommand("endemic", {"exact", "1.20x"}, "exact 1.20x");
    const std::uint32_t cExScale = fnEndemicCapture(59, 0, &outScale);
    if (cExScale != 1U || std::fabs(outScale - 1.20f) > 0.015f) {
        std::printf("FAIL [endemic exact 1.20x]: got crown=%u scale=%.3f\n", cExScale, outScale);
        return 1;
    }
    plugin_host::DispatchPluginSubcommand("endemic", {"exact", "23.6cm"}, "exact 23.6cm");
    const std::uint32_t cExCm = fnEndemicCapture(59, 0, &outScale);
    if (cExCm != 1U || std::fabs(outScale - 1.18f) > 0.015f) {
        std::printf("FAIL [endemic exact 23.6cm]: got crown=%u scale=%.3f\n", cExCm, outScale);
        return 1;
    }
    plugin_host::DispatchPluginSubcommand("endemic", {"history"}, "history");
    plugin_host::DispatchPluginSubcommand("endemic", {"clear"}, "clear");
    std::printf("[OK] Endemic Life plugin verified on real 0x141669AC0/0x14166CCC0/0x140672F90 machine code (guard=%.3f)!\n",
                nonEndemicVal);

    // 8. Verify Official Plugin #4 (`rng_thk.dll`: Fatalis Preset Facade + Generic THK Prober/Counter/Sequence Engine)
    //    on Real MHW 15.23 `0x14132B838` & `0x14133A850 -> cThinkMgr::updateThk (0x141339640)` Machine Code!
    {
        struct SynThkSegment {
            std::uint8_t raw[0x80]{};
            void InitRandom(std::uint32_t ctrlKind, std::int32_t wt, std::int32_t targetFile, std::int32_t targetNode,
                            const void* localTarget = nullptr) {
                std::memset(raw, 0, sizeof(raw));
                *reinterpret_cast<std::uint32_t*>(raw + 0x00U) = ctrlKind;
                *reinterpret_cast<std::int32_t*>(raw + 0x0CU)  = wt;
                *reinterpret_cast<std::int32_t*>(raw + 0x28U)  = targetFile;
                *reinterpret_cast<std::int32_t*>(raw + 0x2CU)  = targetNode;
                *reinterpret_cast<std::uintptr_t*>(raw + 0x30U) =
                    localTarget ? reinterpret_cast<std::uintptr_t>(localTarget) : static_cast<std::uintptr_t>(-1LL);
            }
            void InitEndSentinel(std::int32_t nodeArrIdx) {
                std::memset(raw, 0, sizeof(raw));
                *reinterpret_cast<std::uint32_t*>(raw + 0x00U) = 0x01U;
                *reinterpret_cast<std::int32_t*>(raw + 0x24U)  = 10000 + nodeArrIdx;
                *reinterpret_cast<std::int32_t*>(raw + 0x28U)  = -1;
                *reinterpret_cast<std::int32_t*>(raw + 0x2CU)  = -1;
            }
        };

        static const std::uint8_t* g_lastChosenSeg = nullptr;
        auto preSegCapture = +[](const std::uint8_t* segPtr, std::uint8_t* mgrPtr, void*) -> std::uint8_t {
            g_lastChosenSeg = segPtr;
            *(mgrPtr + 0x560U) = 1U; // Causes `ProcessTHKSegment` to return `-4` cleanly!
            return 1U;
        };

        alignas(16) static std::uint8_t synThinkEm[0x800]{};
        alignas(16) static std::uint8_t monFatalis[0x12300]{};
        std::memset(synThinkEm, 0, sizeof(synThinkEm));
        std::memset(monFatalis, 0, sizeof(monFatalis));

        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 101U; // Fatalis (`em013`)
        std::uint8_t* mgr = synThinkEm + 0x20U;
        *reinterpret_cast<std::uintptr_t*>(synThinkEm + 0x00U) = base + 0x324F320U; // `cThinkEm::vft`
        *reinterpret_cast<void**>(synThinkEm + 0x08U)          = monFatalis;
        *reinterpret_cast<std::uintptr_t*>(mgr + 0x00U)        = base + 0x324F438U; // `cThinkMgr::vft`
        *reinterpret_cast<void**>(mgr + 0x508U)                = monFatalis;
        *reinterpret_cast<void**>(mgr + 0x510U) = reinterpret_cast<void*>(preSegCapture);

        using ThkPrefetchTickFn = std::uint8_t(__fastcall*)(void* mgrPtr);
        const auto fnThkTick      = reinterpret_cast<ThkPrefetchTickFn>(base + 0x133A850U);
        void* const fnEmTickMid   = reinterpret_cast<void*>(base + 0x132B82CU);

        auto runThkNode = [&](bool usePrimaryEmTick,
                              std::uint32_t depth,
                              std::int32_t thkFileIdx,
                              std::int32_t callerNodeArrIdx,
                              std::int32_t initialSegIdx,
                              const SynThkSegment* nodeSegs) -> std::int32_t {
            *(mgr + 0x38U)  = 0U;
            *(mgr + 0x580U) = 0U;
            *reinterpret_cast<std::uint32_t*>(mgr + 0x4F8U) = depth;
            if (depth == 1U) {
                *reinterpret_cast<std::int32_t*>(mgr + 0x364U) = callerNodeArrIdx;
            }
            std::uint8_t* frameBase = mgr + static_cast<std::uintptr_t>(depth) * 0x180U;
            *reinterpret_cast<const void**>(frameBase + 0x350U)  = nodeSegs;
            *reinterpret_cast<std::int32_t*>(frameBase + 0x358U) = initialSegIdx;
            *reinterpret_cast<std::int32_t*>(frameBase + 0x35CU) = thkFileIdx;
            g_lastChosenSeg = nullptr;
            if (usePrimaryEmTick) {
                // `synThinkEm + 0x38..0x44` must be 0 so `cThinkEm::update` post-switch cleanly skips.
                std::memset(synThinkEm + 0x38U, 0, 0x10U);
                callEmThinkUpdate(synThinkEm, monFatalis, fnEmTickMid);
            } else {
                fnThkTick(mgr);
            }
            if (!g_lastChosenSeg) return -1;
            const auto byteDiff = g_lastChosenSeg - reinterpret_cast<const std::uint8_t*>(nodeSegs);
            return static_cast<std::int32_t>(byteDiff / 0x80);
        };

        // Construct authentic vanilla segment tables matching `em013_00.thk` & `em013_55.thk`
        alignas(16) static SynThkSegment node75[64]{};
        alignas(16) static SynThkSegment node76[64]{};
        alignas(16) static SynThkSegment node77[64]{};
        alignas(16) static SynThkSegment node78[64]{};
        alignas(16) static SynThkSegment node79[8]{};
        alignas(16) static SynThkSegment node18[28]{};
        alignas(16) static SynThkSegment node14[70]{};
        alignas(16) static SynThkSegment node46[78]{};
        alignas(16) static SynThkSegment node55_87[18]{};

        // Node 75 & 76 (<900): two sectors tested (seg 1 and seg 36), target 55:297 at +4 from start
        auto fillCloseNode = [](SynThkSegment* arr, std::int32_t arrIdx) {
            // Sector 0 (seg 1..9)
            arr[1].InitRandom(0x40, 30, 55, 172);
            arr[2].InitRandom(0xC0, 15, 55, 298);
            arr[3].InitRandom(0xC0, 0,  55, 286);
            arr[4].InitRandom(0xC0, 25, 55, 185);
            arr[5].InitRandom(0xC0, 10, 55, 297); // Target: 孕吐
            arr[8].InitRandom(0x80, 20, 55, 274);
            // Sector 3 (seg 36..44)
            arr[36].InitRandom(0x40, 20, 55, 172);
            arr[37].InitRandom(0xC0, 15, 55, 298);
            arr[39].InitRandom(0xC0, 15, 55, 185);
            arr[40].InitRandom(0xC0, 10, 55, 297); // Target: 孕吐
            arr[42].InitRandom(0xC0, 30, 55, 175);
            arr[44].InitRandom(0x80, 10, 55, 274);
            arr[62].InitEndSentinel(arrIdx);
        };
        fillCloseNode(node75, 75);
        fillCloseNode(node76, 76);

        // Node 77 (900..2000): front(seg 1..9 -> 55:184 at 4, 55:282 cone at 5), back(seg 13..22 -> 55:175 at 20), side(seg 26..34 & 50..57 -> 55:188 at 29 / 53)
        node77[1].InitRandom(0x40, 23, 55, 179);
        node77[2].InitRandom(0xC0, 17, 55, 220);
        node77[4].InitRandom(0xC0, 25, 55, 184); // Front target (NO cone)
        node77[5].InitRandom(0xC0, 10, 55, 282);
        node77[7].InitRandom(0xC0, 20, 55, 193);
        node77[9].InitRandom(0x80, 5,  55, 274);
        node77[13].InitRandom(0x40, 20, 55, 179);
        node77[14].InitRandom(0xC0, 15, 55, 220);
        node77[16].InitRandom(0xC0, 20, 55, 184);
        node77[20].InitRandom(0xC0, 25, 55, 175); // Back target (甩尾)
        node77[22].InitRandom(0x80, 5,  55, 274);
        node77[26].InitRandom(0x40, 20, 55, 179);
        node77[27].InitRandom(0xC0, 15, 55, 220);
        node77[29].InitRandom(0xC0, 20, 55, 188); // Side1 target (侧地毯)
        node77[32].InitRandom(0xC0, 25, 55, 175);
        node77[34].InitRandom(0x80, 5,  55, 274);
        node77[50].InitRandom(0x40, 25, 55, 179);
        node77[51].InitRandom(0xC0, 20, 55, 220);
        node77[53].InitRandom(0xC0, 35, 55, 188); // Side2 target (侧地毯)
        node77[55].InitRandom(0xC0, 15, 55, 193);
        node77[57].InitRandom(0x80, 5,  55, 274);
        node77[62].InitEndSentinel(77);

        // Node 78 (2000..4500): front(seg 1..9 -> 55:184 at 4), side(seg 25..32 -> 55:188 at 28)
        node78[1].InitRandom(0x40, 23, 55, 179);
        node78[2].InitRandom(0xC0, 17, 55, 220);
        node78[4].InitRandom(0xC0, 25, 55, 184); // Target (NO cone)
        node78[5].InitRandom(0xC0, 10, 55, 282);
        node78[7].InitRandom(0xC0, 20, 55, 193);
        node78[9].InitRandom(0x80, 5,  55, 274);
        node78[25].InitRandom(0x40, 25, 55, 179);
        node78[26].InitRandom(0xC0, 20, 55, 220);
        node78[28].InitRandom(0xC0, 35, 55, 188); // Side target
        node78[30].InitRandom(0xC0, 15, 55, 193);
        node78[32].InitRandom(0x80, 5,  55, 274);
        node78[37].InitEndSentinel(78);

        // Node 79 (>4500): seg 0 -> 55:179 (wt=35), seg 1 -> 55:220 (wt=30), seg 2 -> 55:215 (wt=35, ENDR=0x80)
        node79[0].InitRandom(0x40, 35, 55, 179);
        node79[1].InitRandom(0xC0, 30, 55, 220);
        node79[2].InitRandom(0x80, 35, 55, 215);
        node79[6].InitEndSentinel(79);

        // Node 55[87] (under-breath follow-up): seg 9 RANDOM(35 -> LOC=88), seg 10 ENDR(65 -> no cone)
        node55_87[9].InitRandom(0x40,  35, -1, -1);
        node55_87[10].InitRandom(0x80, 65, -1, -1);
        node55_87[16].InitEndSentinel(87);

        // Node 18 (P1 15% extra charge): seg 7 RANDOM(15), seg 12 ENDR(85)
        node18[7].InitRandom(0x40,  15, -1, -1);
        node18[12].InitRandom(0x80, 85, -1, -1);
        node18[26].InitEndSentinel(18);

        // Node 14 (P1 stance switch): bipedal stay seg 22 RANDOM(10)/31 ENDR(90); L4 stand seg 49 RANDOM(10)/58 ENDR(90)
        node14[22].InitRandom(0x40, 10, -1, -1);
        node14[31].InitRandom(0x80, 90, -1, -1);
        node14[49].InitRandom(0x40, 10, -1, -1);
        node14[58].InitRandom(0x80, 90, -1, -1);
        node14[69].InitEndSentinel(14);

        // Node 46 (occluded): seg 3..7 = Phase 2 (must ignore), seg 53..56 = Phase 1 front (`55:184` at 55)
        node46[3].InitRandom(0x40, 30, 55, 179);
        node46[5].InitRandom(0xC0, 10, 55, 184);
        node46[7].InitRandom(0x80, 60, 55, 270);
        node46[53].InitRandom(0x40, 30, 55, 179);
        node46[54].InitRandom(0xC0, 10, 55, 220);
        node46[55].InitRandom(0xC0, 15, 55, 184);
        node46[56].InitRandom(0x80, 45, 55, 215);
        node46[76].InitEndSentinel(46);

        plugin_host::DispatchPluginSubcommand("fatalis", {"on"}, "on");
        plugin_host::DispatchPluginSubcommand("thk", {"preset", "tag", "bipedal_lock", "on"}, "preset tag bipedal_lock on");
        // Test BOTH primary caller (`0x132B83D`, `cThinkEm::update`) AND prefetch caller (`0x133A876`, `FUN_14133a850`)!
        for (bool usePrimary : {true, false}) {
            if (runThkNode(usePrimary, 0, 0, -1, 1,  node75) != 5  ||
                runThkNode(usePrimary, 0, 0, -1, 36, node76) != 40 ||
                runThkNode(usePrimary, 1, 55, 75, 9, node55_87) != 10 ||
                runThkNode(usePrimary, 0, 0, -1, 1,  node77) != 4  ||
                runThkNode(usePrimary, 0, 0, -1, 13, node77) != 20 ||
                runThkNode(usePrimary, 0, 0, -1, 26, node77) != 29 ||
                runThkNode(usePrimary, 0, 0, -1, 50, node77) != 53 ||
                runThkNode(usePrimary, 0, 0, -1, 1,  node78) != 4  ||
                runThkNode(usePrimary, 0, 0, -1, 25, node78) != 28 ||
                runThkNode(usePrimary, 0, 0, -1, 0,  node79) != 0  ||
                runThkNode(usePrimary, 0, 0, -1, 7,  node18) != 12 ||
                runThkNode(usePrimary, 0, 0, -1, 22, node14) != 31 ||
                runThkNode(usePrimary, 0, 0, -1, 49, node14) != 49 ||
                runThkNode(usePrimary, 0, 0, -1, 53, node46) != 55) {
                std::printf("FAIL [fatalis]: Phase 1 bipedal move rule assertion failed (primary=%d)!\n", usePrimary ? 1 : 0);
                return 1;
            }
        }
        plugin_host::DispatchPluginSubcommand("thk", {"preset", "tag", "bipedal_lock", "off"}, "preset tag bipedal_lock off");

        // Verify ALL Phase 1 Quadruped (`四足`) rules + 50/50 probabilistic split + Phase 2 isolation!
        alignas(16) static SynThkSegment node104[26]{};
        alignas(16) static SynThkSegment node109[22]{};
        alignas(16) static SynThkSegment node105[54]{};
        alignas(16) static SynThkSegment node110[49]{};
        alignas(16) static SynThkSegment node107[26]{};
        alignas(16) static SynThkSegment node108[8]{};
        alignas(16) static SynThkSegment node47[36]{};
        alignas(16) static SynThkSegment node115_p2[26]{};

        // `node104` (0,500 未出蛇形咬): seg 1..5 -> 55:216 at seg 3
        node104[1].InitRandom(0x40, 10, 55, 171);
        node104[2].InitRandom(0xC0, 10, 55, 192);
        node104[3].InitRandom(0xC0, 40, 55, 216); // Target: 蛇形咬
        node104[4].InitRandom(0x80, 40, 55, 277);
        node104[24].InitEndSentinel(104);

        // `node109` (0,500 已出蛇形咬): seg 1..4 -> 55:277 at seg 3
        node109[1].InitRandom(0x40, 10, 55, 171);
        node109[2].InitRandom(0xC0, 45, 55, 192);
        node109[3].InitRandom(0x80, 45, 55, 277); // Target: 后撤地毯火
        node109[20].InitEndSentinel(109);

        // `node105` (500,1000 未出蛇形咬): front(seg 1..5 -> 55:216 at 3), back(seg 31..35 -> 55:176 at 33)
        node105[1].InitRandom(0x40, 35, 55, 177);
        node105[2].InitRandom(0xC0, 20, 55, 217);
        node105[3].InitRandom(0xC0, 10, 55, 216); // Front target: 蛇形咬
        node105[4].InitRandom(0x80, 35, 55, 178);
        node105[31].InitRandom(0x40, 10, 55, 177);
        node105[32].InitRandom(0xC0, 20, 55, 217);
        node105[33].InitRandom(0xC0, 20, 55, 176); // Back target: 尾砸
        node105[34].InitRandom(0xC0, 30, 55, 216);
        node105[35].InitRandom(0x80, 20, 55, 277);
        node105[52].InitEndSentinel(105);

        // `node110` (500,1000 已出蛇形咬): front(seg 1..4 -> 55:217 at 3 & 55:277 at 4), back(seg 28..31 -> 55:176 at 30)
        node110[1].InitRandom(0x40, 40, 55, 177);
        node110[2].InitRandom(0xC0, 35, 55, 178);
        node110[3].InitRandom(0xC0, 20, 55, 217); // Front 50%: 蓄力火
        node110[4].InitRandom(0x80, 5,  55, 277); // Front 50%: 后撤地毯火
        node110[28].InitRandom(0x40, 45, 55, 177);
        node110[29].InitRandom(0xC0, 10, 55, 217);
        node110[30].InitRandom(0xC0, 20, 55, 176); // Back target: 尾砸
        node110[31].InitRandom(0x80, 25, 55, 277);
        node110[47].InitEndSentinel(110);

        // `node107` (1500,5000): front(seg 1..4 -> 55:180 at 1, 55:221 at 2), back(seg 9..12 -> 55:180 at 9, 55:221 at 10)
        node107[1].InitRandom(0x40, 25, 55, 180);
        node107[2].InitRandom(0xC0, 20, 55, 221);
        node107[3].InitRandom(0xC0, 35, 55, 192);
        node107[4].InitRandom(0x80, 20, 55, 216);
        node107[9].InitRandom(0x40,  25, 55, 180);
        node107[10].InitRandom(0xC0, 20, 55, 221);
        node107[11].InitRandom(0xC0, 35, 55, 192);
        node107[12].InitRandom(0x80, 20, 55, 216);
        node107[24].InitEndSentinel(107);

        // `node108` (>5000): seg 0..3
        node108[0].InitRandom(0x40, 35, 55, 180);
        node108[1].InitRandom(0xC0, 25, 55, 221);
        node108[2].InitRandom(0x80, 40, 55, 216);
        node108[6].InitEndSentinel(108);

        // `node47` occluded quadruped: P1 seg 2..5 (locked), P2 seg 26..29 (MUST stay vanilla)
        node47[2].InitRandom(0x40,  30, 55, 180);
        node47[3].InitRandom(0xC0,  10, 55, 221);
        node47[4].InitRandom(0x80,  60, 55, 216);
        node47[26].InitRandom(0x40, 30, 55, 180);
        node47[27].InitRandom(0xC0, 30, 55, 221);
        node47[28].InitRandom(0x80, 40, 55, 216);
        node47[34].InitEndSentinel(47);

        // `node115_p2` Phase 2 Quadruped (arrIdx=115, MUST NOT be controlled)
        node115_p2[1].InitRandom(0x40, 10, 55, 171);
        node115_p2[2].InitRandom(0xC0, 10, 55, 192);
        node115_p2[3].InitRandom(0xC0, 40, 55, 216);
        node115_p2[4].InitRandom(0x80, 40, 55, 277);
        node115_p2[24].InitEndSentinel(115);

        if (runThkNode(true, 0, 0, -1, 1,  node104) != 3  ||
            runThkNode(true, 0, 0, -1, 1,  node109) != 3  ||
            runThkNode(true, 0, 0, -1, 1,  node105) != 3  ||
            runThkNode(true, 0, 0, -1, 31, node105) != 33 ||
            runThkNode(true, 0, 0, -1, 28, node110) != 30 ||
            runThkNode(true, 0, 0, -1, 1,  node107) != 1  ||
            runThkNode(true, 0, 0, -1, 9,  node107) != 9  ||
            runThkNode(true, 0, 0, -1, 0,  node108) != 0  ||
            runThkNode(true, 0, 0, -1, 2,  node47)  != 2) {
            std::printf("FAIL [fatalis]: Phase 1 quadruped deterministic rule assertion failed!\n");
            return 1;
        }

        // Verify 50/50 split on `node110` front (`55:217` at seg 3, `55:277` at seg 4, zero bites `1/2`):
        int cnt217 = 0, cnt277 = 0;
        for (int k = 0; k < 100; ++k) {
            const std::int32_t s = runThkNode(true, 0, 0, -1, 1, node110);
            if (s == 3) ++cnt217;
            else if (s == 4) ++cnt277;
            else {
                std::printf("FAIL [fatalis]: P1 quad 500~1500 front got unexpected seg %d (expected 3 or 4)!\n", s);
                return 1;
            }
        }
        if (cnt217 < 30 || cnt217 > 70 || cnt277 < 30 || cnt277 > 70) {
            std::printf("FAIL [fatalis]: P1 quad 500~1500 50/50 balance out of range (217=%d, 277=%d)!\n",
                        cnt217, cnt277);
            return 1;
        }

        // Verify `node47` Phase 2 segment (`seg0=26`) & `node115` Phase 2 stay natural vanilla!
        bool sawP2QuadVarA = false, sawP2QuadVarB = false;
        bool sawOcclP2A = false, sawOcclP2B = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t s115 = runThkNode(true, 0, 0, -1, 1, node115_p2);
            if (s115 == 3) sawP2QuadVarA = true;
            else sawP2QuadVarB = true;
            const std::int32_t s47 = runThkNode(true, 0, 0, -1, 26, node47);
            if (s47 == 26) sawOcclP2A = true;
            else sawOcclP2B = true;
        }
        if (!sawP2QuadVarA || !sawP2QuadVarB || !sawOcclP2A || !sawOcclP2B) {
            std::printf("FAIL [fatalis]: Phase 2 quadruped isolation check failed!\n");
            return 1;
        }

        // Verify Enraged Combat (`thkFileIdx = 4`, `THK_04`) covers BOTH P1 Bipedal & P1 Quadruped identically to `f=0`,
        // while unrelated slot (`thkFileIdx = 11`, Mount) remains untouched vanilla!
        if (runThkNode(true, 0, 4, -1, 1,  node75) != 5  ||
            runThkNode(true, 0, 4, -1, 1,  node77) != 4  ||
            runThkNode(true, 0, 4, -1, 13, node77) != 20 ||
            runThkNode(true, 0, 4, -1, 26, node77) != 29 ||
            runThkNode(true, 0, 4, -1, 1,  node78) != 4  ||
            runThkNode(true, 0, 4, -1, 0,  node79) != 0  ||
            runThkNode(true, 0, 4, -1, 1,  node104) != 3  ||
            runThkNode(true, 0, 4, -1, 1,  node109) != 3  ||
            runThkNode(true, 0, 4, -1, 1,  node105) != 3  ||
            runThkNode(true, 0, 4, -1, 31, node105) != 33 ||
            runThkNode(true, 0, 4, -1, 28, node110) != 30 ||
            runThkNode(true, 0, 4, -1, 1,  node107) != 1  ||
            runThkNode(true, 0, 4, -1, 0,  node108) != 0  ||
            runThkNode(true, 0, 4, -1, 2,  node47)  != 2) {
            std::printf("FAIL [fatalis]: Enraged (`thkFileIdx=4`) P1 rule assertion failed!\n");
            return 1;
        }
        int enr217 = 0, enr277 = 0;
        for (int k = 0; k < 60; ++k) {
            const std::int32_t s = runThkNode(true, 0, 4, -1, 1, node110);
            if (s == 3) ++enr217;
            else if (s == 4) ++enr277;
            else {
                std::printf("FAIL [fatalis]: Enraged (`f=4`) quad 50/50 got unexpected seg %d!\n", s);
                return 1;
            }
        }
        if (enr217 < 15 || enr217 > 45 || enr277 < 15 || enr277 > 45) {
            std::printf("FAIL [fatalis]: Enraged (`f=4`) quad 50/50 balance out of range (%d vs %d)!\n", enr217, enr277);
            return 1;
        }
        bool sawMountVarA = false, sawMountVarB = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t sm = runThkNode(true, 0, 11, -1, 1, node75);
            if (sm == 5) sawMountVarA = true;
            else sawMountVarB = true;
        }
        if (!sawMountVarA || !sawMountVarB) {
            std::printf("FAIL [fatalis]: Unrelated slot `thkFileIdx=11` should stay unmodified!\n");
            return 1;
        }

        plugin_host::DispatchPluginSubcommand("fatalis", {"far", "multi"}, "far multi");
        if (runThkNode(true, 0, 0, -1, 0, node79) != 1 ||
            runThkNode(true, 0, 0, -1, 1, node107) != 2 ||
            runThkNode(true, 0, 0, -1, 0, node108) != 1) {
            std::printf("FAIL [fatalis]: `far multi` expected 55:220 on node79 & 55:221 on node107/108!\n");
            return 1;
        }
        plugin_host::DispatchPluginSubcommand("fatalis", {"far", "single"}, "far single");

        // Verify 1-hop local wrapper resolution & Frame 0 return stack unwind (FatalisAdaptive instrumented THK):
        alignas(16) static SynThkSegment wrap179[4]{}, wrap220[4]{}, wrap184[4]{}, wrap282[4]{};
        alignas(16) static SynThkSegment node77Wrapped[64]{}, node81[4]{};
        auto fillWrapper = [](SynThkSegment* w, std::int32_t targetNodeId, std::int32_t wrapArrIdx) {
            w[0].InitRandom(0x00U, 0, -1, -1); // Telemetry marker
            w[1].InitRandom(0x00U, 0, 55, targetNodeId);
            w[2].InitRandom(0x800U, 0, -1, -1);
            w[3].InitEndSentinel(wrapArrIdx);
        };
        fillWrapper(wrap179, 179, 2938);
        fillWrapper(wrap220, 220, 2939);
        fillWrapper(wrap184, 184, 2940);
        fillWrapper(wrap282, 282, 2941);
        node77Wrapped[1].InitRandom(0x40, 31, -1, -1, wrap179);
        node77Wrapped[2].InitRandom(0xC0, 16, -1, -1, wrap220);
        node77Wrapped[4].InitRandom(0xC0, 29, -1, -1, wrap184); // Target via 1-hop wrapper
        node77Wrapped[5].InitRandom(0xC0, 7,  -1, -1, wrap282);
        node77Wrapped[9].InitRandom(0x80, 17, 55, 274);
        node77Wrapped[62].InitEndSentinel(77);
        node81[0].InitEndSentinel(81);

        if (runThkNode(true, 0, 0, -1, 1, node77Wrapped) != 4) {
            std::printf("FAIL [fatalis]: 1-hop wrapper resolution in node77Wrapped expected seg 4 (55:184)!\n");
            return 1;
        }
        *reinterpret_cast<std::int32_t*>(mgr + 0x340U)  = 2;
        *reinterpret_cast<const void**>(mgr + 0x200U)   = node75;
        if (runThkNode(true, 1, 55, 2881, 9, node55_87) != 10) {
            std::printf("FAIL [fatalis]: Frame 0 return stack unwind from wrapper 2881 -> node 75 expected ENDR (seg 10)!\n");
            return 1;
        }
        *reinterpret_cast<const void**>(mgr + 0x200U)   = node81;
        bool sawWrappedP2Cone = false, sawWrappedP2NoCone = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t s = runThkNode(true, 1, 55, 2881, 9, node55_87);
            if (s == 9) sawWrappedP2Cone = true;
            if (s == 10) sawWrappedP2NoCone = true;
        }
        *reinterpret_cast<std::int32_t*>(mgr + 0x340U)  = 0;
        *reinterpret_cast<const void**>(mgr + 0x200U)   = nullptr;
        if (!sawWrappedP2Cone || !sawWrappedP2NoCone) {
            std::printf("FAIL [fatalis]: Wrapper unwind for Phase 2 node 81 should stay unmodified!\n");
            return 1;
        }

        // Phase 2 isolation on `55[87]` (caller=81) and `idx=46` (seg=3): MUST produce natural mix of outcomes!
        bool saw55Cone = false, saw55NoCone = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t s = runThkNode(true, 1, 55, 81, 9, node55_87);
            if (s == 9) saw55Cone = true;
            if (s == 10) saw55NoCone = true;
        }
        if (!saw55Cone || !saw55NoCone) {
            std::printf("FAIL [fatalis]: Phase 2 `55[87]` caller=81 should not be locked!\n");
            return 1;
        }

        // Non-Fatalis isolation (`MonsterID = 91`): MUST produce natural mix of outcomes on `node75`!
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 91U;
        bool sawNonTarget = false;
        for (int k = 0; k < 24; ++k) {
            if (runThkNode(true, 0, 0, -1, 1, node75) != 5) sawNonTarget = true;
        }
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 101U;
        if (!sawNonTarget) {
            std::printf("FAIL [fatalis]: Non-Fatalis (MonsterID=91) was unexpectedly locked!\n");
            return 1;
        }

        plugin_host::DispatchPluginSubcommand("fatalis", {"history"}, "history");
        plugin_host::DispatchPluginSubcommand("fatalis", {"diag"}, "diag");
        plugin_host::DispatchPluginSubcommand("fatalis", {"status"}, "status");
        plugin_host::DispatchPluginSubcommand("fatalis", {"off"}, "off");
        bool sawUnlockedFatalis = false;
        for (int k = 0; k < 24; ++k) {
            if (runThkNode(true, 0, 0, -1, 1, node75) != 5) sawUnlockedFatalis = true;
        }
        if (!sawUnlockedFatalis) {
            std::printf("FAIL [fatalis]: `$rng fatalis off` failed to restore vanilla rolls!\n");
            return 1;
        }
        std::printf("[OK] Fatalis Phase 1 preset via `$rng fatalis` verified on real 0x14132B838 & 0x14133A850 -> 0x141339640 machine code!\n");

        // 8B. Verify Generic THK Engine Capabilities on Non-Fatalis Monster (MonsterID = 91, Rajang) & External Preset:
        alignas(16) static SynThkSegment node12[16]{};
        node12[1].InitRandom(0x40, 30, 55, 172); // seg 1 -> 55:172
        node12[2].InitRandom(0xC0, 30, 55, 185); // seg 2 -> 55:185
        node12[3].InitRandom(0x80, 40, 55, 297); // seg 3 -> 55:297
        node12[8].InitEndSentinel(12);

        // (1) Arbitrary Node Probing & Move Counting (`MonsterID = 91`, Rajang)
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 91U;
        plugin_host::DispatchPluginSubcommand("thk", {"probe", "on", "91"}, "probe on 91");
        bool saw172 = false, sawOther91 = false;
        for (int k = 0; k < 18; ++k) {
            const std::int32_t s = runThkNode(true, 0, 0, -1, 1, node12);
            if (s < 1 || s > 3) {
                std::printf("FAIL [thk probe]: invalid candidate %d on node12!\n", s);
                return 1;
            }
            if (s == 1) saw172 = true;
            else sawOther91 = true;
        }
        if (!saw172 || !sawOther91) {
            std::printf("FAIL [thk probe]: probe mode distorted natural Rajang (MonsterID=91) distribution!\n");
            return 1;
        }
        plugin_host::DispatchPluginSubcommand("thk", {"nodes", "91"}, "nodes 91");
        plugin_host::DispatchPluginSubcommand("thk", {"stats", "91"}, "stats 91");
        plugin_host::DispatchPluginSubcommand("thk", {"probe", "log"}, "probe log");
        plugin_host::DispatchPluginSubcommand("thk", {"probe", "off"}, "probe off");

        // (2) Ordered Move Sequences (`loop`, `hold`, `once` with `vanilla` step)
        plugin_host::DispatchPluginSubcommand("thk", {"reset"}, "reset");
        plugin_host::DispatchPluginSubcommand("thk", {"seq", "91", "12", "1", "55:172,55:185,55:297", "loop"},
                                             "seq 91 12 1 55:172,55:185,55:297 loop");
        const std::int32_t expectedLoop[5] = {1, 2, 3, 1, 2};
        for (int k = 0; k < 5; ++k) {
            const std::int32_t got = runThkNode(true, 0, 0, -1, 1, node12);
            if (got != expectedLoop[k]) {
                std::printf("FAIL [thk seq loop]: step %d got seg %d, expected %d!\n", k, got, expectedLoop[k]);
                return 1;
            }
        }

        plugin_host::DispatchPluginSubcommand("thk", {"rule", "clear"}, "rule clear");
        plugin_host::DispatchPluginSubcommand("thk", {"reset"}, "reset");
        plugin_host::DispatchPluginSubcommand("thk", {"seq", "91", "12", "1", "55:297,55:172", "hold"},
                                             "seq 91 12 1 55:297,55:172 hold");
        const std::int32_t expectedHold[4] = {3, 1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            const std::int32_t got = runThkNode(true, 0, 0, -1, 1, node12);
            if (got != expectedHold[k]) {
                std::printf("FAIL [thk seq hold]: step %d got seg %d, expected %d!\n", k, got, expectedHold[k]);
                return 1;
            }
        }

        plugin_host::DispatchPluginSubcommand("thk", {"rule", "clear"}, "rule clear");
        plugin_host::DispatchPluginSubcommand("thk", {"reset"}, "reset");
        plugin_host::DispatchPluginSubcommand("thk", {"seq", "91", "12", "1", "55:185,vanilla,55:297", "once"},
                                             "seq 91 12 1 55:185,vanilla,55:297 once");
        if (runThkNode(true, 0, 0, -1, 1, node12) != 2) {
            std::printf("FAIL [thk seq once]: step 0 expected seg 2 (55:185)!\n");
            return 1;
        }
        const std::int32_t vanillaMid = runThkNode(true, 0, 0, -1, 1, node12);
        if (vanillaMid < 1 || vanillaMid > 3) {
            std::printf("FAIL [thk seq once]: step 1 (vanilla) got invalid seg %d!\n", vanillaMid);
            return 1;
        }
        if (runThkNode(true, 0, 0, -1, 1, node12) != 3) {
            std::printf("FAIL [thk seq once]: step 2 expected seg 3 (55:297)!\n");
            return 1;
        }
        bool sawPostOnceVarA = false, sawPostOnceVarB = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t s = runThkNode(true, 0, 0, -1, 1, node12);
            if (s == 1) sawPostOnceVarA = true;
            else sawPostOnceVarB = true;
        }
        if (!sawPostOnceVarA || !sawPostOnceVarB) {
            std::printf("FAIL [thk seq once]: expected vanilla restoration after `once` sequence!\n");
            return 1;
        }

        // (3) Count-Gated Rule (`after=2 until=3`)
        plugin_host::DispatchPluginSubcommand("thk", {"rule", "clear"}, "rule clear");
        plugin_host::DispatchPluginSubcommand("thk", {"reset"}, "reset");
        plugin_host::DispatchPluginSubcommand("thk", {"rule", "add", "91", "12", "1", "55:297", "after=2", "until=3"},
                                             "rule add 91 12 1 55:297 after=2 until=3");
        (void)runThkNode(true, 0, 0, -1, 1, node12); // hit #1 (uncontrolled)
        if (runThkNode(true, 0, 0, -1, 1, node12) != 3 ||
            runThkNode(true, 0, 0, -1, 1, node12) != 3) {
            std::printf("FAIL [thk gate]: after=2 until=3 did not lock hits #2 and #3 to 55:297!\n");
            return 1;
        }
        bool sawAfterGateA = false, sawAfterGateB = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t s = runThkNode(true, 0, 0, -1, 1, node12);
            if (s == 1 || s == 2) sawAfterGateA = true;
            if (s == 3) sawAfterGateB = true;
        }
        if (!sawAfterGateA || !sawAfterGateB) {
            std::printf("FAIL [thk gate]: hits >3 should return to natural vanilla variation!\n");
            return 1;
        }

        // (4) External Plain-Text Preset Loading (`thk_presets\fatalis_p1_nasty.txt`)
        plugin_host::DispatchPluginSubcommand("thk", {"clear"}, "clear");
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 101U;
        plugin_host::DispatchPluginSubcommand("thk", {"preset", "load", "fatalis_p1_nasty"},
                                             "preset load fatalis_p1_nasty");
        if (runThkNode(true, 0, 0, -1, 1,  node75) != 5  ||
            runThkNode(true, 0, 0, -1, 1,  node77) != 4  ||
            runThkNode(true, 0, 0, -1, 13, node77) != 20 ||
            runThkNode(true, 0, 0, -1, 26, node77) != 29 ||
            runThkNode(true, 1, 55, 75, 9, node55_87) != 10) {
            std::printf("FAIL [thk preset]: external fatalis_p1_nasty preset did not match expected P1 targets!\n");
            return 1;
        }

        // (5) Per-Monster-Instance Auto-Reset When Switching `Monster*` Pointer
        alignas(16) static std::uint8_t monRajang2[0x12300]{};
        std::memset(monRajang2, 0, sizeof(monRajang2));
        *reinterpret_cast<std::uint32_t*>(monRajang2 + 0x12280U) = 91U;

        plugin_host::DispatchPluginSubcommand("thk", {"clear"}, "clear");
        plugin_host::DispatchPluginSubcommand("thk", {"seq", "91", "12", "1", "55:172,55:297", "hold"},
                                             "seq 91 12 1 55:172,55:297 hold");
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 91U;
        if (runThkNode(true, 0, 0, -1, 1, node12) != 1 ||
            runThkNode(true, 0, 0, -1, 1, node12) != 3) {
            std::printf("FAIL [thk instance]: first Rajang instance did not play [1, 3]!\n");
            return 1;
        }
        *reinterpret_cast<void**>(synThinkEm + 0x08U) = monRajang2;
        *reinterpret_cast<void**>(mgr + 0x508U)       = monRajang2;
        *(mgr + 0x38U)  = 0U;
        *(mgr + 0x580U) = 0U;
        *reinterpret_cast<std::uint32_t*>(mgr + 0x4F8U) = 0U;
        *reinterpret_cast<const void**>(mgr + 0x350U)   = node12;
        *reinterpret_cast<std::int32_t*>(mgr + 0x358U)  = 1;
        *reinterpret_cast<std::int32_t*>(mgr + 0x35CU)  = 0;
        std::memset(synThinkEm + 0x38U, 0, 0x10U);
        g_lastChosenSeg = nullptr;
        callEmThinkUpdate(synThinkEm, monRajang2, fnEmTickMid);
        const std::int32_t raj2Step0 = g_lastChosenSeg
            ? static_cast<std::int32_t>((g_lastChosenSeg - reinterpret_cast<const std::uint8_t*>(node12)) / 0x80)
            : -1;
        *reinterpret_cast<void**>(synThinkEm + 0x08U) = monFatalis;
        *reinterpret_cast<void**>(mgr + 0x508U)       = monFatalis;
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 101U;
        if (raj2Step0 != 1) {
            std::printf("FAIL [thk instance]: new Monster* instance expected step 0 (seg 1), got %d!\n", raj2Step0);
            return 1;
        }

        // (6) Weighted Random Choice (`A(w1)+B(w2)`) on Rajang (MonsterID=91)
        plugin_host::DispatchPluginSubcommand("thk", {"clear"}, "clear");
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 91U;
        plugin_host::DispatchPluginSubcommand("thk", {"rule", "add", "91", "12", "1", "55:172(50)+55:297(50)"},
                                             "rule add 91 12 1 55:172(50)+55:297(50)");
        int raj172 = 0, raj297 = 0;
        for (int k = 0; k < 60; ++k) {
            const std::int32_t s = runThkNode(true, 0, 0, -1, 1, node12);
            if (s == 1) ++raj172;
            else if (s == 3) ++raj297;
            else {
                std::printf("FAIL [thk weighted]: unexpected seg %d on 55:172(50)+55:297(50)!\n", s);
                return 1;
            }
        }
        if (raj172 < 15 || raj172 > 45 || raj297 < 15 || raj297 > 45) {
            std::printf("FAIL [thk weighted]: 50/50 split skewed (%d vs %d)!\n", raj172, raj297);
            return 1;
        }
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 101U;

        plugin_host::DispatchPluginSubcommand("thk", {"clear"}, "clear");
        std::printf("[OK] Generic THK probe/counter/sequence/weighted/preset/auto-reset engine verified on real 0x14132B838 machine code!\n");
    }

    // 9. Verify Zero-Lock Shadow-Copy Hot-Reload + Inspector `dump` / `top` + Hot-Unload!
    const std::string srcSizeDll = plugDir + "\\rng_size.dll";
    HANDLE hWriteCheck = ::CreateFileA(srcSizeDll.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hWriteCheck == INVALID_HANDLE_VALUE) {
        std::printf("FAIL: %s is file-locked! Shadow-copy hot-reload broken!\n", srcSizeDll.c_str());
        return 1;
    }
    ::CloseHandle(hWriteCheck);

    std::string reloadMsg;
    if (!plugin_host::LoadOrReloadSingle("size", reloadMsg)) {
        std::printf("FAIL: Hot-reload of 'size' failed: %s\n", reloadMsg.c_str());
        return 1;
    }
    std::printf("[OK] Zero-lock Shadow-Copy Hot-Reload verified: %s\n", reloadMsg.c_str());

    const std::string dumpPath = buildDir + "\\test_rng_dump_v2.txt";
    plugin_host::DispatchPluginSubcommand("dump", {dumpPath}, dumpPath);
    plugin_host::DispatchPluginSubcommand("top", {}, "");

    std::string unloadMsg;
    if (!plugin_host::UnloadSingle("inspector", unloadMsg)) {
        std::printf("FAIL: UnloadSingle('inspector') failed\n");
        return 1;
    }
    std::printf("[OK] Hot-unload verified: %s\n", unloadMsg.c_str());

    // 10. Verify `sChat::DispatchChatEntry` Standalone Chaining + Unchained-Overwrite Detection & Self-Healing!
    {
        const std::uintptr_t chatSite = base + mhw::kRvaSChatDispatchEntry;
        void* origA = nullptr;
        void* origB = nullptr;
        bool chainedA = false, chainedB = false;
        std::uintptr_t targetB = 0, relayA = 0, relayB = 0;

        auto dummyChatA = +[](std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint8_t) {};
        auto dummyChatB = +[](std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint8_t) {};

        if (!hook::InstallStandaloneDetour5(chatSite, reinterpret_cast<void*>(dummyChatA), &origA,
                                            mhw::kPrologueSChatDispatch, &chainedA, nullptr, &relayA) ||
            chainedA) {
            std::printf("FAIL [chat chain]: initial vanilla chat hook failed\n");
            return 1;
        }
        if (hook::WasUnchainedOverwrite5(chatSite, relayA, mhw::kPrologueSChatDispatch)) {
            std::printf("FAIL [chat chain]: false positive while relayA is at head\n");
            return 1;
        }

        // Proper second plugin chains onto relayA -> WasUnchainedOverwrite5 MUST return false!
        if (!hook::InstallStandaloneDetour5(chatSite, reinterpret_cast<void*>(dummyChatB), &origB,
                                            mhw::kPrologueSChatDispatch, &chainedB, &targetB, &relayB) ||
            !chainedB || targetB != relayA) {
            std::printf("FAIL [chat chain]: second plugin failed to chain onto relayA\n");
            return 1;
        }
        if (hook::WasUnchainedOverwrite5(chatSite, relayA, mhw::kPrologueSChatDispatch)) {
            std::printf("FAIL [chat chain]: false positive on properly chained second plugin\n");
            return 1;
        }

        // Simulate a racing unchained hook overwriting chatSite with a vanilla-trampoline relay
        std::uint8_t fakeRelayBuf[64]{};
        auto* fakePage = static_cast<std::uint8_t*>(
            ::VirtualAlloc(reinterpret_cast<void*>((base - 0x20000ULL) & ~0xFFFFULL), 4096,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (fakePage) {
            std::memcpy(fakePage + 16U, mhw::kPrologueSChatDispatch, 5); // Vanilla trampoline (unchained!)
            const auto fakeRel = static_cast<std::int32_t>(
                static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(fakePage)) -
                static_cast<std::int64_t>(chatSite + 5U));
            fakeRelayBuf[0] = 0xE9;
            std::memcpy(fakeRelayBuf + 1, &fakeRel, 4);
            std::memcpy(reinterpret_cast<void*>(chatSite), fakeRelayBuf, 5);
            if (!hook::WasUnchainedOverwrite5(chatSite, relayB, mhw::kPrologueSChatDispatch)) {
                std::printf("FAIL [chat chain]: failed to detect unchained overwrite!\n");
                return 1;
            }
            // Self-heal by re-chaining onto the unchained hook
            void* origHeal = nullptr;
            bool chainedHeal = false;
            std::uintptr_t relayHeal = 0;
            if (!hook::InstallStandaloneDetour5(chatSite, reinterpret_cast<void*>(dummyChatB), &origHeal,
                                                mhw::kPrologueSChatDispatch, &chainedHeal, nullptr, &relayHeal) ||
                !chainedHeal ||
                hook::WasUnchainedOverwrite5(chatSite, relayHeal, mhw::kPrologueSChatDispatch)) {
                std::printf("FAIL [chat chain]: self-heal re-chain failed!\n");
                return 1;
            }
            ::VirtualFree(fakePage, 0, MEM_RELEASE);
        }
        std::printf("[OK] sChat::DispatchChatEntry chaining + unchained-overwrite detection & self-heal verified!\n");
        const std::uintptr_t poolLo10 = hook::ArenaBase() + 0x1000ULL;
        const std::uintptr_t poolHi10 = hook::ArenaBase() + 0x10000ULL;
        if (relayA < poolLo10 || relayA >= poolHi10 ||
            relayB < poolLo10 || relayB >= poolHi10 ||
            relayA == relayB) {
            std::printf("FAIL [chat chain]: standalone pool placement invalid (base=%p, relayA=%p, relayB=%p)\n",
                        reinterpret_cast<void*>(hook::ArenaBase()),
                        reinterpret_cast<void*>(relayA),
                        reinterpret_cast<void*>(relayB));
            return 1;
        }
        MEMORY_BASIC_INFORMATION mbiP0{}, mbiA{}, mbiB{};
        if (!::VirtualQuery(reinterpret_cast<LPCVOID>(hook::ArenaBase()), &mbiP0, sizeof(mbiP0)) ||
            !::VirtualQuery(reinterpret_cast<LPCVOID>(relayA), &mbiA, sizeof(mbiA)) ||
            !::VirtualQuery(reinterpret_cast<LPCVOID>(relayB), &mbiB, sizeof(mbiB)) ||
            mbiP0.Protect != PAGE_EXECUTE_READ || mbiA.Protect != PAGE_EXECUTE_READ || mbiB.Protect != PAGE_EXECUTE_READ) {
            std::printf("FAIL [chat chain]: Expected PAGE_EXECUTE_READ on Page0/A/B (0x%lX/0x%lX/0x%lX)\n",
                        mbiP0.Protect, mbiA.Protect, mbiB.Protect);
            return 1;
        }
        std::printf("[OK] Pre-reserved 64KB standalone W^X pool verified for sChat::DispatchChatEntry (Page0=%p, A=%p, B=%p)!\n",
                    reinterpret_cast<void*>(hook::ArenaBase()),
                    reinterpret_cast<void*>(relayA),
                    reinterpret_cast<void*>(relayB));
    }

    // 11. Verify SDK v3.0 Microkernel Capabilities (Real `text_1523.bin` + MHW x64 Thunks)
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        if (!host || host->apiVersion != 3U || host->structSize != sizeof(MhwRngHostApi)) {
            std::printf("FAIL [v3 host]: invalid HostApi version/size\n");
            return 1;
        }
        constexpr MhwRngPluginId kTestPluginId = 777U;

        // V3-1: Caller Non-Volatile Register Snapshot + Original Argument Forwarding
        {
            // Thunk placed inside [base, end) at `base + 0x05000000`:
            //   fn(void* rcx, std::uint64_t rdx_or_xmm1, std::uint64_t r8_or_xmm2, void* target_r9)
            std::uint8_t* regThunk = img + 0x05000000ULL;
            std::size_t off = 0;
            auto emitBytes = [&](std::initializer_list<std::uint8_t> bs) {
                for (std::uint8_t b : bs) regThunk[off++] = b;
            };
            auto emitU64 = [&](std::uint64_t v) {
                std::memcpy(regThunk + off, &v, 8);
                off += 8;
            };
            emitBytes({0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}); // push rbx..r15
            emitBytes({0x48, 0x83, 0xEC, 0x28});                                                 // sub rsp, 28h
            emitBytes({0x49, 0x8B, 0xC1});                                                       // mov rax, r9
            emitBytes({0x48, 0xBB}); emitU64(0x1111222233334441ULL);
            emitBytes({0x48, 0xBD}); emitU64(0x1111222233334442ULL);
            emitBytes({0x48, 0xBE}); emitU64(0x1111222233334443ULL);
            emitBytes({0x48, 0xBF}); emitU64(0x1111222233334444ULL);
            emitBytes({0x49, 0xBC}); emitU64(0x1111222233334445ULL);
            emitBytes({0x49, 0xBD}); emitU64(0x1111222233334446ULL);
            emitBytes({0x49, 0xBE}); emitU64(0x1111222233334447ULL);
            emitBytes({0x49, 0xBF}); emitU64(0x1111222233334448ULL);
            emitBytes({0xFF, 0xD0});                                                             // call rax
            const std::uint32_t thunkRetRva = static_cast<std::uint32_t>(0x05000000ULL + off);
            emitBytes({0x48, 0x83, 0xC4, 0x28});                                                 // add rsp, 28h
            emitBytes({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B, 0xC3});
            ::FlushInstructionCache(::GetCurrentProcess(), regThunk, off);

            struct RegCheckState {
                bool seenNrand = false, seenCtor = false, seenRange = false;
                bool regsExact = false;
                std::uint64_t gotSeed = 0;
                float gotMin = 0.0f, gotMax = 0.0f;
            } regSt{};

            MhwRngHookDesc hd{};
            hd.structSize = sizeof(hd);
            hd.rva        = thunkRetRva;
            hd.priority   = 0;
            hd.flags      = kMhwRngHookPre;
            hd.userData   = &regSt;
            hd.preFn      = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void* ud) {
                auto* st = static_cast<RegCheckState*>(ud);
                if (!ctx->callerRegs || (ctx->callFlags & kMhwRngDrawFlagHasRegs) == 0U) return;
                const bool match =
                    ctx->callerRegs->rbx == 0x1111222233334441ULL &&
                    ctx->callerRegs->rbp == 0x1111222233334442ULL &&
                    ctx->callerRegs->rsi == 0x1111222233334443ULL &&
                    ctx->callerRegs->rdi == 0x1111222233334444ULL &&
                    ctx->callerRegs->r12 == 0x1111222233334445ULL &&
                    ctx->callerRegs->r13 == 0x1111222233334446ULL &&
                    ctx->callerRegs->r14 == 0x1111222233334447ULL &&
                    ctx->callerRegs->r15 == 0x1111222233334448ULL &&
                    ctx->callerRegs->callerRsp == reinterpret_cast<std::uintptr_t>(ctx->returnAddrSlot);

                if (ctx->sourceKind == kMhwRngSrcMtNrandGlobal) {
                    st->seenNrand = true;
                    st->regsExact = match;
                } else if (ctx->sourceKind == kMhwRngSrcMtCtorSeed) {
                    st->seenCtor = match && ((ctx->callFlags & kMhwRngDrawFlagHasOrigSeed) != 0U);
                    st->gotSeed  = ctx->origArg1;
                } else if (ctx->sourceKind == kMhwRngSrcMt19937Range) {
                    st->seenRange = match && ((ctx->callFlags & kMhwRngDrawFlagHasRange) != 0U);
                    st->gotMin    = ctx->origMinVal;
                    st->gotMax    = ctx->origMaxVal;
                    float retF = 42.5f;
                    std::uint32_t bits = 0U;
                    std::memcpy(&bits, &retF, sizeof(bits));
                    dec->action = kMhwRngActionReturnFloatBits;
                    dec->value0 = bits;
                }
            };
            const MhwRngHookHandle hReg = host->RegisterSlotHookEx(kTestPluginId, &hd);
            if (!hReg) {
                std::printf("FAIL [V3-1]: RegisterSlotHookEx failed\n");
                return 1;
            }

            using ThunkNrandFn = std::uint32_t(__fastcall*)(void*, std::uint64_t, std::uint64_t, void*);
            using ThunkCtorFn  = std::uint32_t*(__fastcall*)(void*, std::uint32_t, std::uint64_t, void*);
            using ThunkRangeFn = float(__fastcall*)(void*, float, float, void*);
            alignas(16) std::uint32_t tmpMt[4]{};

            reinterpret_cast<ThunkNrandFn>(regThunk)(sInst, 0ULL, 0ULL, reinterpret_cast<void*>(fnNrand));
            reinterpret_cast<ThunkCtorFn>(regThunk)(tmpMt, 0x13579BDFU, 0ULL, reinterpret_cast<void*>(base + mhw::kRvaMtRandomCtorSeed));
            const float fGot = reinterpret_cast<ThunkRangeFn>(regThunk)(nullptr, -12.5f, 37.25f, reinterpret_cast<void*>(base + mhw::kRvaMt19937Range));

            host->UnregisterSlotHookHandle(kTestPluginId, hReg);

            if (!regSt.seenNrand || !regSt.regsExact || !regSt.seenCtor || regSt.gotSeed != 0x13579BDFULL ||
                !regSt.seenRange || std::fabs(regSt.gotMin - (-12.5f)) > 1e-6f ||
                std::fabs(regSt.gotMax - 37.25f) > 1e-6f || std::fabs(fGot - 42.5f) > 1e-6f) {
                std::printf("FAIL [V3-1]: reg/arg capture failed (nrand=%d regs=%d ctor=%d seed=0x%llX range=%d f=%.2f)\n",
                            regSt.seenNrand, regSt.regsExact, regSt.seenCtor,
                            static_cast<unsigned long long>(regSt.gotSeed), regSt.seenRange, fGot);
                return 1;
            }
            std::printf("[OK] V3-1: MASM non-volatile GPR snapshot (all 8 regs) + origSeed/minVal/maxVal verified!\n");
        }

        // V3-2: Multi-Plugin Chained Priority Hooks + Unregister Independence
        {
            (void)CallNrandV3Chain(fnNrand, sInst);
            MhwRngSlotSnapshot snap{};
            // Discover the exact RVA of `CallNrandV3Chain`'s inner call by inspecting active slots or via GlobalObserver
            std::uint32_t chainRva = 0U;
            const MhwRngHookHandle hObs = host->RegisterGlobalObserver(
                kTestPluginId, 0U, (1U << kMhwRngSrcMtNrandGlobal),
                +[](const MhwRngDrawContext* ctx, void* ud) {
                    *static_cast<std::uint32_t*>(ud) = ctx->callSiteRva;
                },
                &chainRva);
            (void)CallNrandV3Chain(fnNrand, sInst);
            host->UnregisterGlobalObserver(kTestPluginId, hObs);
            if (chainRva == 0U || !host->GetCondSlotInfo(chainRva, 0U, &snap)) {
                std::printf("FAIL [V3-2]: failed to capture chainRva via GlobalObserver\n");
                return 1;
            }

            struct ChainTrace {
                int  orderIdx = 0;
                int  bPos = -1, aPos = -1;
                int  bMode = 0; // 0 = Continue, 1 = ReturnValue, 2 = UseCondition(7)
                std::uint32_t aSawCond = 0U;
            } tr{};

            MhwRngHookDesc hdA{};
            hdA.structSize = sizeof(hdA);
            hdA.rva        = chainRva;
            hdA.priority   = 10;
            hdA.flags      = kMhwRngHookPre;
            hdA.userData   = &tr;
            hdA.preFn      = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void* ud) {
                auto* t = static_cast<ChainTrace*>(ud);
                t->aPos = t->orderIdx++;
                t->aSawCond = ctx->activeCondId;
                if (t->bMode == 2) {
                    dec->action = kMhwRngActionClaimCondition;
                    dec->condId = ctx->activeCondId;
                } else {
                    dec->action = kMhwRngActionReturnValue;
                    dec->value0 = 0xAAAAAAAAU;
                }
            };

            MhwRngHookDesc hdB{};
            hdB.structSize = sizeof(hdB);
            hdB.rva        = chainRva;
            hdB.priority   = -10;
            hdB.flags      = kMhwRngHookPre;
            hdB.userData   = &tr;
            hdB.preFn      = +[](const MhwRngDrawContext*, MhwRngHookDecision* dec, void* ud) {
                auto* t = static_cast<ChainTrace*>(ud);
                t->bPos = t->orderIdx++;
                if (t->bMode == 1) {
                    dec->action = kMhwRngActionReturnValue;
                    dec->value0 = 0xBBBBBBBBU;
                } else if (t->bMode == 2) {
                    dec->action = kMhwRngActionUseCondition;
                    dec->condId = 7U;
                } else {
                    dec->action = kMhwRngActionContinue;
                }
            };

            const MhwRngHookHandle hA = host->RegisterSlotHookEx(kTestPluginId, &hdA);
            const MhwRngHookHandle hB = host->RegisterSlotHookEx(kTestPluginId, &hdB);
            if (!hA || !hB) {
                std::printf("FAIL [V3-2]: RegisterSlotHookEx failed\n");
                return 1;
            }

            tr.bMode = 0;
            const std::uint32_t vCont = CallNrandV3Chain(fnNrand, sInst);
            if (vCont != 0xAAAAAAAAU || tr.bPos != 0 || tr.aPos != 1) {
                std::printf("FAIL [V3-2]: priority/Continue mismatch (v=0x%X bPos=%d aPos=%d)\n", vCont, tr.bPos, tr.aPos);
                return 1;
            }

            tr.orderIdx = 0; tr.bPos = tr.aPos = -1; tr.bMode = 1;
            const std::uint32_t vShort = CallNrandV3Chain(fnNrand, sInst);
            if (vShort != 0xBBBBBBBBU || tr.bPos != 0 || tr.aPos != -1) {
                std::printf("FAIL [V3-2]: ReturnValue short-circuit failed\n");
                return 1;
            }

            host->SetCondFixedValue(kTestPluginId, chainRva, 7U, 0x77777777U);
            tr.orderIdx = 0; tr.bPos = tr.aPos = -1; tr.bMode = 2;
            const std::uint32_t vCond = CallNrandV3Chain(fnNrand, sInst);
            if (vCond != 0x77777777U || tr.aSawCond != 7U) {
                std::printf("FAIL [V3-2]: UseCondition propagation failed (v=0x%X aSaw=%u)\n", vCond, tr.aSawCond);
                return 1;
            }

            // Unregister B and ensure A remains completely intact
            host->UnregisterSlotHookHandle(kTestPluginId, hB);
            tr.bMode = 0;
            if (CallNrandV3Chain(fnNrand, sInst) != 0xAAAAAAAAU) {
                std::printf("FAIL [V3-2]: hook A lost after unregistering hook B\n");
                return 1;
            }
            host->UnregisterSlotHookHandle(kTestPluginId, hA);
            host->ClearSlotAllConditions(chainRva);
            std::printf("[OK] V3-2: Multi-plugin priority hook chain & independent handle unregistration verified!\n");
        }

        // V3-3: Observe-Only Mode + Global Wildcard Observer
        {
            alignas(16) std::uint32_t vanInst[4]{0x12345678U, 0x23456789U, 0x3456789AU, 0x456789ABU};
            auto stepLocalVanilla = [](std::uint32_t s[4]) -> std::uint32_t {
                std::uint32_t t = (s[0] << 15) ^ s[0];
                s[0] = s[1]; s[1] = s[2];
                const std::uint32_t w = s[3];
                s[2] = w;
                t = ((w >> 17) ^ t) >> 4 ^ w ^ t;
                s[3] = t;
                return t;
            };
            std::uint32_t refInst[4]{0x12345678U, 0x23456789U, 0x3456789AU, 0x456789ABU};
            std::uint32_t expectedVan[8]{};
            for (int i = 0; i < 8; ++i) expectedVan[i] = stepLocalVanilla(refInst);

            struct ObsStats {
                int count = 0;
                int postCount = 0;
                std::uint32_t rva = 0U;
                std::uint32_t seenVals[8]{};
                bool flagsOk = true;
            } obs{};

            const MhwRngHookHandle hObs = host->RegisterGlobalObserver(
                kTestPluginId, 0U, (1U << kMhwRngSrcMtNrandPrivate),
                +[](const MhwRngDrawContext* ctx, void* ud) {
                    auto* st = static_cast<ObsStats*>(ud);
                    if ((ctx->callFlags & (kMhwRngDrawFlagObserveOnly | kMhwRngDrawFlagPostDraw | kMhwRngDrawFlagHasVanilla)) !=
                        (kMhwRngDrawFlagObserveOnly | kMhwRngDrawFlagPostDraw | kMhwRngDrawFlagHasVanilla)) {
                        st->flagsOk = false;
                    }
                    if (ctx->vanillaValue0 != ctx->finalValue0) st->flagsOk = false;
                    if (st->count < 8) st->seenVals[st->count] = ctx->finalValue0;
                    st->rva = ctx->callSiteRva;
                    ++st->count;
                },
                &obs);

            host->SetEngineMode(kMhwRngModeObserveOnly);
            std::uint32_t gotVan[8]{};
            for (int i = 0; i < 8; ++i) gotVan[i] = CallNrandV3Observe(fnNrand, vanInst);

            MhwRngHookDesc postOnly{};
            postOnly.structSize = sizeof(postOnly);
            postOnly.rva        = obs.rva;
            postOnly.flags      = kMhwRngHookPost | kMhwRngHookObserveOnly;
            postOnly.postFn     = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void* ud) {
                auto* st = static_cast<ObsStats*>(ud);
                ++st->postCount;
                // Attempted return override MUST be ignored in ObserveOnly mode!
                dec->action = kMhwRngActionReturnValue;
                dec->value0 = 0xDEADBEEFU;
                (void)ctx;
            };
            postOnly.userData   = &obs;
            const MhwRngHookHandle hPostObs = host->RegisterSlotHookEx(kTestPluginId, &postOnly);

            const std::uint32_t extraExpected = stepLocalVanilla(refInst);
            const std::uint32_t extraGot      = CallNrandV3Observe(fnNrand, vanInst);

            host->SetEngineMode(kMhwRngModeTakeover);
            host->UnregisterSlotHookHandle(kTestPluginId, hPostObs);
            host->UnregisterGlobalObserver(kTestPluginId, hObs);

            MhwRngSlotSnapshot obsSnap{};
            if (std::memcmp(gotVan, expectedVan, sizeof(expectedVan)) != 0 ||
                std::memcmp(obs.seenVals, expectedVan, sizeof(expectedVan)) != 0 ||
                extraGot != extraExpected || obs.postCount != 1 || obs.count != 9 || !obs.flagsOk ||
                !host->GetCondSlotInfo(obs.rva, 0U, &obsSnap) || obsSnap.lastValue != extraExpected || obsSnap.totalCalls < 9ULL) {
                std::printf("FAIL [V3-3]: ObserveOnly/GlobalObserver mismatch (cnt=%d post=%d flagsOk=%d)\n",
                            obs.count, obs.postCount, obs.flagsOk);
                return 1;
            }
            std::printf("[OK] V3-3: ObserveOnly mode + Global Wildcard Observer verified against raw MHW Xorshift128!\n");
        }

        // V3-4: `FilterVanilla` Post-Draw Conditional Floor/Clamp
        {
            std::uint32_t filterRva = 0U;
            const MhwRngHookHandle hFind = host->RegisterGlobalObserver(
                kTestPluginId, 0U, (1U << kMhwRngSrcMtNrandPrivate),
                +[](const MhwRngDrawContext* ctx, void* ud) { *static_cast<std::uint32_t*>(ud) = ctx->callSiteRva; },
                &filterRva);
            alignas(16) std::uint32_t privF[4]{0x87654321U, 0x13572468U, 0x99887766U, 0x55443322U};
            (void)CallNrandV3Filter(fnNrand, privF);
            host->UnregisterGlobalObserver(kTestPluginId, hFind);

            struct FilterStats {
                int kept = 0, clamped = 0;
                bool canModifyOk = true;
            } fst{};

            MhwRngHookDesc fd{};
            fd.structSize = sizeof(fd);
            fd.rva        = filterRva;
            fd.flags      = kMhwRngHookFilterVanilla;
            fd.userData   = &fst;
            fd.postFn     = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void* ud) {
                auto* st = static_cast<FilterStats*>(ud);
                if ((ctx->callFlags & (kMhwRngDrawFlagCanModify | kMhwRngDrawFlagPostDraw | kMhwRngDrawFlagHasVanilla)) !=
                    (kMhwRngDrawFlagCanModify | kMhwRngDrawFlagPostDraw | kMhwRngDrawFlagHasVanilla)) {
                    st->canModifyOk = false;
                }
                if (ctx->vanillaValue0 < 0x80000000U) {
                    dec->action = kMhwRngActionReturnValue;
                    dec->value0 = 0xF00F0001U;
                    ++st->clamped;
                } else {
                    dec->action = kMhwRngActionContinue;
                    ++st->kept;
                }
            };
            const MhwRngHookHandle hF = host->RegisterSlotHookEx(kTestPluginId, &fd);
            int gotKept = 0, gotClamped = 0;
            for (int i = 0; i < 32; ++i) {
                const std::uint32_t r = CallNrandV3Filter(fnNrand, privF);
                if (r == 0xF00F0001U) ++gotClamped;
                else if (r >= 0x80000000U) ++gotKept;
            }
            host->UnregisterSlotHookHandle(kTestPluginId, hF);
            if (!fst.canModifyOk || fst.kept == 0 || fst.clamped == 0 || gotKept != fst.kept || gotClamped != fst.clamped) {
                std::printf("FAIL [V3-4]: FilterVanilla failed (kept=%d clamped=%d ok=%d)\n",
                            fst.kept, fst.clamped, fst.canModifyOk);
                return 1;
            }
            std::printf("[OK] V3-4: FilterVanilla post-draw pity clamp (%d kept vanilla, %d clamped) verified!\n",
                        gotKept, gotClamped);
        }

        // V3-5 & V3-6: Binary State Snapshot/Restore + Single-Slot Restore + Lifecycle Events
        {
            struct LifeStats {
                int evEpoch = 0, evSeed = 0, evMode = 0, evClear = 0, evRestore = 0;
            } lst{};
            const MhwRngHookHandle hLife = host->RegisterLifecycleHook(
                kTestPluginId,
                +[](const MhwRngEventInfo* info, void* ud) {
                    auto* s = static_cast<LifeStats*>(ud);
                    switch (info->eventKind) {
                        case kMhwRngEventEpochReset:    ++s->evEpoch; break;
                        case kMhwRngEventMasterSeed:    ++s->evSeed; break;
                        case kMhwRngEventEngineMode:    ++s->evMode; break;
                        case kMhwRngEventClearAllRules: ++s->evClear; break;
                        case kMhwRngEventStateRestored: ++s->evRestore; break;
                        default: break;
                    }
                },
                &lst);

            for (int i = 0; i < 20; ++i) (void)CallNrandSiteA(fnNrand, sInst);

            // Also test unseeded slot + activeCond restoration behavior:
            constexpr std::uint32_t kUnseededRva = 0x05000400U;
            host->SetCondMasterDerived(kTestPluginId, kUnseededRva, 0U);
            host->SetActiveCondition(kTestPluginId, kUnseededRva, 0U);

            const std::uint32_t blobNeed = host->CaptureStateBlob(nullptr, 0U);
            std::vector<std::uint8_t> blob(blobNeed);
            const std::uint32_t blobGot = host->CaptureStateBlob(blob.data(), blobNeed);
            if (blobNeed == 0U || blobGot != blobNeed) {
                std::printf("FAIL [V3-5]: CaptureStateBlob size mismatch (%u vs %u)\n", blobGot, blobNeed);
                return 1;
            }
            const std::uint64_t savedSeed  = host->GetMasterSeed();
            const std::uint64_t savedEpoch = host->GetEpoch();

            std::uint32_t expAfterSave[16]{};
            for (int i = 0; i < 16; ++i) expAfterSave[i] = CallNrandSiteA(fnNrand, sInst);

            // Mutate heavily (including modifying activeCond on kUnseededRva!)
            host->SetActiveCondition(kTestPluginId, kUnseededRva, 99U);
            for (int i = 0; i < 50; ++i) (void)CallNrandSiteA(fnNrand, sInst);
            host->SetMasterSeed(0x9999888877776666ULL, true);
            host->ResetEpoch();
            host->SetEngineMode(kMhwRngModeObserveOnly);
            host->SetEngineMode(kMhwRngModeTakeover);
            host->ClearAllRules();

            if (!host->RestoreStateBlob(blob.data(), static_cast<std::uint32_t>(blob.size())) ||
                host->GetMasterSeed() != savedSeed || host->GetEpoch() != savedEpoch ||
                host->GetActiveCondition(kUnseededRva) != 0U) {
                std::printf("FAIL [V3-5]: RestoreStateBlob failed to restore seed/epoch/activeCond\n");
                return 1;
            }
            // Verify unseeded slot captured with seededEpoch==0 was NOT corrupted to fallback state:
            MhwRngSlotSnapshot unseededSnap{};
            if (!host->GetCondSlotInfo(kUnseededRva, 0U, &unseededSnap) ||
                (unseededSnap.state[0] == 0x075BCD15U && unseededSnap.state[1] == 0x159A55E5U)) {
                std::printf("FAIL [V3-5]: unseeded slot state corrupted on Capture/RestoreStateBlob!\n");
                return 1;
            }
            std::uint32_t gotAfterRestore[16]{};
            for (int i = 0; i < 16; ++i) gotAfterRestore[i] = CallNrandSiteA(fnNrand, sInst);

            if (std::memcmp(gotAfterRestore, expAfterSave, sizeof(expAfterSave)) != 0) {
                std::printf("FAIL [V3-5]: Restored RNG stream diverged from captured snapshot!\n");
                return 1;
            }

            // Single-slot cursor/state restore check
            for (int i = 0; i < 10; ++i) (void)CallNrandSiteB(fnNrand, sInst);
            std::uint32_t siteBRva = 0U;
            const MhwRngHookHandle hBobs = host->RegisterGlobalObserver(
                kTestPluginId, 0U, (1U << kMhwRngSrcMtNrandGlobal),
                +[](const MhwRngDrawContext* ctx, void* ud) { *static_cast<std::uint32_t*>(ud) = ctx->callSiteRva; },
                &siteBRva);
            (void)CallNrandSiteB(fnNrand, sInst);
            host->UnregisterGlobalObserver(kTestPluginId, hBobs);

            MhwRngSlotSnapshot snapB{};
            host->GetCondSlotInfo(siteBRva, 0U, &snapB);
            MhwRngSlotState midSt{};
            midSt.callSiteRva  = siteBRva;
            midSt.condId       = 0U;
            std::memcpy(midSt.state, snapB.state, sizeof(midSt.state));
            midSt.seqCursor    = snapB.seqCursor;
            midSt.activeCondId = snapB.activeCondId;
            midSt.epochCalls   = snapB.epochCalls;
            midSt.lastValue    = snapB.lastValue;
            midSt.flags        = kMhwRngRestoreAll;

            std::uint32_t expB[8]{};
            for (int i = 0; i < 8; ++i) expB[i] = CallNrandSiteB(fnNrand, sInst);
            for (int i = 0; i < 20; ++i) (void)CallNrandSiteB(fnNrand, sInst);
            if (!host->RestoreSlotState(&midSt)) {
                std::printf("FAIL [V3-5]: RestoreSlotState failed\n");
                return 1;
            }
            for (int i = 0; i < 8; ++i) {
                if (CallNrandSiteB(fnNrand, sInst) != expB[i]) {
                    std::printf("FAIL [V3-5]: Single-slot RestoreSlotState diverged at step %d\n", i);
                    return 1;
                }
            }

            host->UnregisterLifecycleHook(kTestPluginId, hLife);
            if (lst.evEpoch < 1 || lst.evSeed < 1 || lst.evMode < 2 || lst.evClear < 1 || lst.evRestore < 1) {
                std::printf("FAIL [V3-6]: Lifecycle events missing (%d/%d/%d/%d/%d)\n",
                            lst.evEpoch, lst.evSeed, lst.evMode, lst.evClear, lst.evRestore);
                return 1;
            }
            std::printf("[OK] V3-5 & V3-6: Full binary SaveState/Restore + single-slot Restore + 5 lifecycle events verified!\n");
        }

        // V3-7: Concurrent Async Chat Queue
        {
            g_capturedAsyncChatLines.clear();
            std::vector<std::thread> chatThreads;
            for (int t = 0; t < 4; ++t) {
                chatThreads.emplace_back([&, t]() {
                    for (int k = 0; k < 10; ++k) {
                        host->ChatPrintfAsync("[async-t%d] msg #%d", t, k);
                    }
                });
            }
            for (auto& th : chatThreads) th.join();
            if (g_capturedAsyncChatLines.size() != 40U) {
                std::printf("FAIL [V3-7]: expected 40 async chat messages, got %zu\n", g_capturedAsyncChatLines.size());
                return 1;
            }
            std::printf("[OK] V3-7: Concurrent ChatPrintfAsync (4 threads x 10 msgs) verified!\n");
        }

        // V3-8: Resource Ceiling Removal (Reclaimable Heap Seq, Tombstone CondSlots, Dynamic Wrapper, Instance Policy)
        {
            // (a) 2,000 updates of a 128-element sequence (= 256,000 elements > old 65,536 fixed pool)
            std::vector<std::uint32_t> longSeq(128U, 0xABCD0001U);
            for (std::uint32_t iter = 0; iter < 2000U; ++iter) {
                longSeq[0] = iter;
                if (!host->SetCondSequence(kTestPluginId, 0x05000100U, 1U, longSeq.data(), 128U, kMhwRngSeqLoop)) {
                    std::printf("FAIL [V3-8a]: SetCondSequence exhausted memory at iter %u\n", iter);
                    return 1;
                }
            }
            host->ReleaseCondSlot(kTestPluginId, 0x05000100U, 1U);

            // (b) Dynamic condId allocation + tombstone reclamation
            const std::uint32_t baselineSlots = host->GetActiveSlotCount();
            for (std::uint32_t cid = 100U; cid < 600U; ++cid) {
                if (!host->SetCondFixedValue(kTestPluginId, 0x05000120U, cid, cid)) {
                    std::printf("FAIL [V3-8b]: SetCondFixedValue failed at cid %u\n", cid);
                    return 1;
                }
            }
            for (std::uint32_t cid = 100U; cid < 600U; ++cid) {
                if (!host->ReleaseCondSlot(kTestPluginId, 0x05000120U, cid)) {
                    std::printf("FAIL [V3-8b]: ReleaseCondSlot failed at cid %u\n", cid);
                    return 1;
                }
            }
            if (host->GetActiveSlotCount() != baselineSlots) {
                std::printf("FAIL [V3-8b]: active slot count not reclaimed (%u vs %u)\n",
                            host->GetActiveSlotCount(), baselineSlots);
                return 1;
            }

            // (c) Custom transparent wrapper registration (frameQwords = 8 -> push rbx/rsi/rdi + sub rsp,20h + call rdx)
            std::uint8_t* customWrap  = img + 0x05000200ULL;
            std::uint8_t* outerCaller = img + 0x05000300ULL;
            // Wrapper: 53 56 57 48 83 EC 20 FF D2 48 83 C4 20 5F 5E 5B C3
            const std::uint8_t kWrapCode[] = {
                0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20,
                0xFF, 0xD2,
                0x48, 0x83, 0xC4, 0x20, 0x5F, 0x5E, 0x5B, 0xC3
            };
            std::memcpy(customWrap, kWrapCode, sizeof(kWrapCode));
            const std::uint32_t wrapRetRva = 0x05000209U; // right after `call rdx`
            // Outer: 48 83 EC 28 41 FF D0 48 83 C4 28 C3  (calls r8=wrapper, passes rcx=sInst, rdx=fnNrand)
            const std::uint8_t kOuterCode[] = {
                0x48, 0x83, 0xEC, 0x28,
                0x41, 0xFF, 0xD0,
                0x48, 0x83, 0xC4, 0x28, 0xC3
            };
            std::memcpy(outerCaller, kOuterCode, sizeof(kOuterCode));
            const std::uint32_t outerRetRva = 0x05000307U; // right after `call r8`
            ::FlushInstructionCache(::GetCurrentProcess(), customWrap, 0x200U);

            if (!host->RegisterTransparentWrapper(kTestPluginId, wrapRetRva, 8U)) {
                std::printf("FAIL [V3-8c]: RegisterTransparentWrapper failed\n");
                return 1;
            }
            struct WrapObs {
                std::uint32_t resolvedRva = 0U, origRetRva = 0U;
            } wobs{};
            const MhwRngHookHandle hWobs = host->RegisterGlobalObserver(
                kTestPluginId, outerRetRva, (1U << kMhwRngSrcMtNrandGlobal),
                +[](const MhwRngDrawContext* ctx, void* ud) {
                    auto* w = static_cast<WrapObs*>(ud);
                    w->resolvedRva = ctx->callSiteRva;
                    w->origRetRva  = ctx->origReturnRva;
                },
                &wobs);
            using OuterFn = std::uint32_t(__fastcall*)(void* sInst, void* fnNrand, void* wrapPtr);
            (void)reinterpret_cast<OuterFn>(outerCaller)(sInst, reinterpret_cast<void*>(fnNrand), customWrap);
            host->UnregisterGlobalObserver(kTestPluginId, hWobs);
            host->UnregisterTransparentWrapper(kTestPluginId, wrapRetRva);
            if (wobs.resolvedRva != outerRetRva || wobs.origRetRva != wrapRetRva) {
                std::printf("FAIL [V3-8c]: wrapper unwrap failed (resolved=0x%X orig=0x%X)\n",
                            wobs.resolvedRva, wobs.origRetRva);
                return 1;
            }

            // (d) Private MtRandom instance policy (FollowOwnState vs ForceSlotControl)
            alignas(16) std::uint32_t privPol[4]{};
            const auto fnCtorSeed = reinterpret_cast<TestCtorSeedFn>(base + mhw::kRvaMtRandomCtorSeed);
            CallCtorSeedV3Priv(fnCtorSeed, privPol, 0x55667788U);
            // Initially MasterDerived on ctor -> not explicitly seeded in Auto mode, returns slot derived value.
            // Explicitly set FollowOwnState -> steps privPol directly:
            if (!host->SetInstancePolicy(reinterpret_cast<std::uintptr_t>(privPol), kMhwRngInstFollowOwnState) ||
                host->GetInstancePolicy(reinterpret_cast<std::uintptr_t>(privPol)) != kMhwRngInstFollowOwnState) {
                std::printf("FAIL [V3-8d]: SetInstancePolicy failed\n");
                return 1;
            }
            std::uint32_t copyWords[4]{privPol[0], privPol[1], privPol[2], privPol[3]};
            auto stepWordRef = [](std::uint32_t s[4]) -> std::uint32_t {
                std::uint32_t t = (s[0] << 15) ^ s[0];
                s[0] = s[1]; s[1] = s[2];
                const std::uint32_t w = s[3]; s[2] = w;
                t = ((w >> 17) ^ t) >> 4 ^ w ^ t; s[3] = t;
                return t;
            };
            const std::uint32_t ownGot = CallNrandV3Priv(fnNrand, privPol);
            const std::uint32_t ownExp = stepWordRef(copyWords);
            if (ownGot != ownExp) {
                std::printf("FAIL [V3-8d]: FollowOwnState did not step instance state (0x%X vs 0x%X)\n", ownGot, ownExp);
                return 1;
            }
            host->SetInstancePolicy(reinterpret_cast<std::uintptr_t>(privPol), kMhwRngInstForceSlotControl);
            const std::uint32_t forceGot = CallNrandV3Priv(fnNrand, privPol);
            const std::uint32_t ownRef2  = stepWordRef(copyWords);
            if (forceGot == ownRef2) {
                std::printf("FAIL [V3-8d]: ForceSlotControl still stepped own state instead of slot!\n");
                return 1;
            }
            std::printf("[OK] V3-8: Reclaimable seq heap, tombstone slots, custom wrapper unwrap & instance policy verified!\n");
        }

        // V3-9: Verify Official Plugin #5 (`rng_steam.dll`) on Real MHW 15.23 `cSteamControl::start` & `FUN_1405ca060`
        {
            host->SetEngineMode(kMhwRngModeTakeover);
            if (img[0x0666795U] != 0xE8 || img[0x06667B6U] != 0xE8 || img[0x05CA09DU] != 0xE8) {
                std::printf("FAIL [V3-9]: Expected E8 calls at 0x0666795 / 0x06667B6 / 0x05CA09D in text_1523.bin!\n");
                return 1;
            }

            // CallWithRbx(void* rbxVal_rcx, void* target_rdx)
            // Target offsets `0x0666785` and `0x05CA096` are mid-function bodies (`RSP % 16 == 0` at entry),
            // so `sub rsp, 20h` after two pushes ensures `RSP % 16 == 8` right before `call rdx`.
            static constexpr std::uint8_t kSteamCallThunk[] = {
                0x53,                         // push rbx
                0x57,                         // push rdi
                0x48, 0x83, 0xEC, 0x20,       // sub  rsp, 20h
                0x48, 0x8B, 0xD9,             // mov  rbx, rcx
                0xFF, 0xD2,                   // call rdx
                0x48, 0x83, 0xC4, 0x20,       // add  rsp, 20h
                0x5F,                         // pop  rdi
                0x5B,                         // pop  rbx
                0xC3                          // ret
            };
            std::memcpy(thunkMem + 0x200U, kSteamCallThunk, sizeof(kSteamCallThunk));
            ::FlushInstructionCache(::GetCurrentProcess(), thunkMem + 0x200U, sizeof(kSteamCallThunk));
            using CallWithRbxFn = void(__fastcall*)(void* rbxVal, void* targetAddr);
            const auto callWithRbx = reinterpret_cast<CallWithRbxFn>(thunkMem + 0x200U);

            const std::uint8_t origKeygenEnd = img[0x06667D6U];
            const std::uint8_t origPityCmp   = img[0x05CA0B6U];
            img[0x06667D6U] = 0xC3; // RET right after writing `order[0..2]` at `[rbx+8..0xA]`
            img[0x05CA0B6U] = 0xC3; // RET right after `(r % 100)` inside `FUN_1405ca060`
            ::FlushInstructionCache(::GetCurrentProcess(), img + 0x06667D6U, 1);
            ::FlushInstructionCache(::GetCurrentProcess(), img + 0x05CA0B6U, 1);

            alignas(16) static std::uint8_t fakeSteamCtrl[0x500]{};
            std::memset(fakeSteamCtrl, 0, sizeof(fakeSteamCtrl));

            // (a) Pure `ObserveOnly` hint accuracy across all 6 permutations + Red slot marker
            // Seed synthetic `sInstance` in `img + 0x3FDEBD8` (zeros by default since only `.text` is loaded)
            // so vanilla `MtRandom::nrand(sInst)` produces a real varying Xorshift128 stream.
            const std::uint32_t savedSInst[4] = { sInst[0], sInst[1], sInst[2], sInst[3] };
            sInst[0] = 0x12345678U;
            sInst[1] = 0x9ABCDEF0U;
            sInst[2] = 0x13579BDFU;
            sInst[3] = 0x2468ACE0U;
            host->SetEngineMode(kMhwRngModeObserveOnly);
            g_capturedAsyncChatLines.clear();
            auto formatExpectedKb = [](const std::uint8_t ord[3]) -> std::string {
                static constexpr const char* kKb[3] = { "A", "W", "D" };
                return std::string(kKb[ord[0]]) + " - " + kKb[ord[1]] + " - " + kKb[ord[2]];
            };
            bool seenPerms[6]{};
            int seenPermCnt = 0;
            bool sawRedMsg = false;
            for (int r = 0; r < 200 && seenPermCnt < 6; ++r) {
                fakeSteamCtrl[0x4A9U] = (r == 0) ? 1U : 0U;
                const std::size_t prevN = g_capturedAsyncChatLines.size();
                callWithRbx(fakeSteamCtrl, img + 0x0666785U);
                if (g_capturedAsyncChatLines.size() != prevN + 1U) {
                    img[0x06667D6U] = origKeygenEnd;
                    img[0x05CA0B6U] = origPityCmp;
                    std::printf("FAIL [V3-9a]: Expected 1 async chat hint per ObserveOnly round!\n");
                    return 1;
                }
                const std::uint8_t* ord = fakeSteamCtrl + 8;
                const std::string expKb = formatExpectedKb(ord);
                const std::string& msg  = g_capturedAsyncChatLines.back();
                if (msg.find(expKb) == std::string::npos) {
                    img[0x06667D6U] = origKeygenEnd;
                    img[0x05CA0B6U] = origPityCmp;
                    std::printf("FAIL [V3-9a]: Observed hint `%s` mismatched real order `%s`!\n",
                                msg.c_str(), expKb.c_str());
                    return 1;
                }
                if (r == 0) {
                    sawRedMsg = (msg.find(u8"[红槽!]") != std::string::npos);
                }
                static constexpr std::uint8_t kPermTbl[6][3] = {
                    {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
                };
                for (int p = 0; p < 6; ++p) {
                    if (!seenPerms[p] &&
                        ord[0] == kPermTbl[p][0] && ord[1] == kPermTbl[p][1] && ord[2] == kPermTbl[p][2]) {
                        seenPerms[p] = true;
                        ++seenPermCnt;
                    }
                }
            }
            if (seenPermCnt != 6 || !sawRedMsg) {
                img[0x06667D6U] = origKeygenEnd;
                img[0x05CA0B6U] = origPityCmp;
                std::printf("FAIL [V3-9a]: ObserveOnly saw %d/6 perms, red=%d\n", seenPermCnt, sawRedMsg);
                return 1;
            }

            // (b) `Takeover` mode sequence lock for all 6 permutations + `r2` alias
            //     With default `verbose=false`, locked rounds stay silent while still setting `[8..10]`.
            host->SetEngineMode(kMhwRngModeTakeover);
            struct LockCase { const char* arg; std::uint8_t b0, b1, b2; };
            static constexpr LockCase kLockCases[] = {
                { "awd", 0, 1, 2 }, { "adw", 0, 2, 1 },
                { "wad", 1, 0, 2 }, { "wda", 1, 2, 0 },
                { "daw", 2, 0, 1 }, { "dwa", 2, 1, 0 },
                { "r2",  0, 1, 2 }
            };
            fakeSteamCtrl[0x4A9U] = 0U;
            for (const auto& lc : kLockCases) {
                plugin_host::DispatchPluginSubcommand("steam", {"lock", lc.arg}, std::string("lock ") + lc.arg);
                g_capturedAsyncChatLines.clear();
                callWithRbx(fakeSteamCtrl, img + 0x0666785U);
                if (fakeSteamCtrl[8] != lc.b0 || fakeSteamCtrl[9] != lc.b1 || fakeSteamCtrl[10] != lc.b2 ||
                    !g_capturedAsyncChatLines.empty()) {
                    img[0x06667D6U] = origKeygenEnd;
                    img[0x05CA0B6U] = origPityCmp;
                    std::printf("FAIL [V3-9b]: `$rng steam lock %s` produced [%u,%u,%u] async=%zu!\n",
                                lc.arg, fakeSteamCtrl[8], fakeSteamCtrl[9], fakeSteamCtrl[10],
                                g_capturedAsyncChatLines.size());
                    return 1;
                }
            }

            // Enable `verbose on` to verify `[锁序]` async notifications emit when verbose is enabled
            plugin_host::DispatchPluginSubcommand("steam", {"verbose", "on"}, "verbose on");
            plugin_host::DispatchPluginSubcommand("steam", {"lock", "awd"}, "lock awd verbose");
            g_capturedAsyncChatLines.clear();
            callWithRbx(fakeSteamCtrl, img + 0x0666785U);
            if (fakeSteamCtrl[8] != 0 || fakeSteamCtrl[9] != 1 || fakeSteamCtrl[10] != 2 ||
                g_capturedAsyncChatLines.empty() ||
                g_capturedAsyncChatLines.back().find(u8"[锁序]") == std::string::npos) {
                img[0x06667D6U] = origKeygenEnd;
                img[0x05CA0B6U] = origPityCmp;
                std::printf("FAIL [V3-9b]: `verbose on` + `$rng steam lock awd` did not emit [锁序]!\n");
                return 1;
            }
            plugin_host::DispatchPluginSubcommand("steam", {"verbose", "off"}, "verbose off");

            // Verify bare `$rng steam lock` (no args) defaults to `A - W - D` ({0, 1, 2}) and stays silent when verbose=false
            plugin_host::DispatchPluginSubcommand("steam", {"lock", "off"}, "lock off");
            plugin_host::DispatchPluginSubcommand("steam", {"lock"}, "lock");
            g_capturedAsyncChatLines.clear();
            callWithRbx(fakeSteamCtrl, img + 0x0666785U);
            if (fakeSteamCtrl[8] != 0 || fakeSteamCtrl[9] != 1 || fakeSteamCtrl[10] != 2 ||
                !g_capturedAsyncChatLines.empty()) {
                img[0x06667D6U] = origKeygenEnd;
                img[0x05CA0B6U] = origPityCmp;
                std::printf("FAIL [V3-9b]: bare `$rng steam lock` did not default to [0,1,2] quietly (got [%u,%u,%u], async=%zu)!\n",
                            fakeSteamCtrl[8], fakeSteamCtrl[9], fakeSteamCtrl[10], g_capturedAsyncChatLines.size());
                return 1;
            }

            // (c) Hidden guaranteed-win pity alert (`0x05CA0A2`): silent by default (`verbose=false`), alerts when `verbose=true`
            g_capturedAsyncChatLines.clear();
            *reinterpret_cast<std::uint32_t*>(fakeSteamCtrl + 0x4A4U) = 100U;
            callWithRbx(fakeSteamCtrl + 0x4A4U, img + 0x05CA096U);
            const bool quietPityDefault = g_capturedAsyncChatLines.empty();

            plugin_host::DispatchPluginSubcommand("steam", {"verbose", "on"}, "verbose on pity");
            g_capturedAsyncChatLines.clear();
            *reinterpret_cast<std::uint32_t*>(fakeSteamCtrl + 0x4A4U) = 100U;
            callWithRbx(fakeSteamCtrl + 0x4A4U, img + 0x05CA096U);
            const bool sawPity100 = (!g_capturedAsyncChatLines.empty() &&
                                     g_capturedAsyncChatLines.back().find(u8"隐藏必中保底") != std::string::npos);
            g_capturedAsyncChatLines.clear();
            *reinterpret_cast<std::uint32_t*>(fakeSteamCtrl + 0x4A4U) = 0U;
            callWithRbx(fakeSteamCtrl + 0x4A4U, img + 0x05CA096U);
            const bool cleanPity0 = g_capturedAsyncChatLines.empty();
            plugin_host::DispatchPluginSubcommand("steam", {"verbose", "off"}, "verbose off pity");
            if (!quietPityDefault || !sawPity100 || !cleanPity0) {
                img[0x06667D6U] = origKeygenEnd;
                img[0x05CA0B6U] = origPityCmp;
                std::printf("FAIL [V3-9c]: auto-win check failed (quietDef=%d pity100=%d clean0=%d)\n",
                            quietPityDefault, sawPity100, cleanPity0);
                return 1;
            }

            // (d) Lifecycle ClearAllRules clears lock, style switch, on/off, and hot-reload
            host->ClearAllRules();
            plugin_host::DispatchPluginSubcommand("steam", {"style", "cn"}, "style cn");
            g_capturedAsyncChatLines.clear();
            callWithRbx(fakeSteamCtrl, img + 0x0666785U);
            const bool cnOk = (!g_capturedAsyncChatLines.empty() &&
                               g_capturedAsyncChatLines.back().find(u8"[锁序]") == std::string::npos &&
                               (g_capturedAsyncChatLines.back().find(u8"左 - ") != std::string::npos ||
                                g_capturedAsyncChatLines.back().find(u8"中 - ") != std::string::npos ||
                                g_capturedAsyncChatLines.back().find(u8"右 - ") != std::string::npos));

            plugin_host::DispatchPluginSubcommand("steam", {"off"}, "off");
            g_capturedAsyncChatLines.clear();
            callWithRbx(fakeSteamCtrl, img + 0x0666785U);
            const bool offOk = g_capturedAsyncChatLines.empty();

            plugin_host::DispatchPluginSubcommand("steam", {"on"}, "on");
            std::string steamReloadMsg;
            const bool steamReloadOk = plugin_host::LoadOrReloadSingle("steam", steamReloadMsg);

            img[0x06667D6U] = origKeygenEnd;
            img[0x05CA0B6U] = origPityCmp;
            ::FlushInstructionCache(::GetCurrentProcess(), img + 0x06667D6U, 1);
            ::FlushInstructionCache(::GetCurrentProcess(), img + 0x05CA0B6U, 1);
            std::memcpy(sInst, savedSInst, sizeof(savedSInst));

            if (!cnOk || !offOk || !steamReloadOk) {
                std::printf("FAIL [V3-9d]: cn=%d off=%d reload=%d (%s)\n",
                            cnOk, offOk, steamReloadOk, steamReloadMsg.c_str());
                return 1;
            }
            std::printf("[OK] V3-9: `rng_steam.dll` ObserveOnly hints (6/6 perms + red), lock (all 6 perms), pity & reload verified on real MHW code!\n");
        }
    }

    // 12. Verify `randUnitSphere`, pre-reserved 64 KB multi-page standalone W^X pool, & clean shutdown
    float v3[3]{};
    CallSphereSite(fnSphere, sInst, v3);
    const float norm = std::sqrt(v3[0] * v3[0] + v3[1] * v3[1] + v3[2] * v3[2]);
    if (std::fabs(norm - 1.0f) > 1e-4f) return 1;

    // Verify InstallStandaloneDetour5 commits a page from the pre-reserved 64 KB boot pool for sChat::move
    void* origMove = nullptr;
    std::uintptr_t relayMove = 0;
    bool chainedMove = true;
    char errStandalone[128]{};
    auto dummyMoveDetour = +[](std::uintptr_t) {};
    if (!hook::InstallStandaloneDetour5(base + mhw::kRvaSChatMove,
                                        reinterpret_cast<void*>(dummyMoveDetour),
                                        &origMove,
                                        mhw::kPrologueSChatMove,
                                        &chainedMove, nullptr, &relayMove,
                                        errStandalone, sizeof(errStandalone))) {
        std::printf("FAIL [Standalone]: InstallStandaloneDetour5(sChat::move) failed (%s)\n", errStandalone);
        return 1;
    }
    const std::uintptr_t poolLo = hook::ArenaBase() + 0x1000ULL;
    const std::uintptr_t poolHi = hook::ArenaBase() + 0x10000ULL;
    MEMORY_BASIC_INFORMATION mbiMove{};
    if (relayMove < poolLo || relayMove >= poolHi || chainedMove ||
        !::VirtualQuery(reinterpret_cast<LPCVOID>(relayMove), &mbiMove, sizeof(mbiMove)) ||
        mbiMove.Protect != PAGE_EXECUTE_READ) {
        std::printf("FAIL [Standalone]: sChat::move standalone pool verification failed (relay=%p, prot=0x%lX, chained=%d)\n",
                    reinterpret_cast<void*>(relayMove), mbiMove.Protect, chainedMove);
        return 1;
    }
    std::printf("[OK] sChat::move standalone W^X pool page verified (relay=%p)!\n",
                reinterpret_cast<void*>(relayMove));

    // 13. Verify `kMhwRngModeSelective` + `ProtectSaveSeeds` (using real MHW 15.23 `FUN_14135d9a0`)
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        constexpr MhwRngPluginId kTestPluginId = 999U;
        host->ClearAllRules();
        host->ResetEpoch();

        // Populate IAT entries used by `FUN_14135d9a0` (`0x135D9B8` -> 0x2E13000, `0x135D9E6` -> 0x2E13008)
        auto noopCritSec = +[](void*) {};
        *reinterpret_cast<std::uintptr_t*>(base + 0x2E13000U) = reinterpret_cast<std::uintptr_t>(noopCritSec);
        *reinterpret_cast<std::uintptr_t*>(base + 0x2E13008U) = reinterpret_cast<std::uintptr_t>(noopCritSec);

        using SaveSeedStepFn = void(__fastcall*)(void* cUserData, std::uint32_t idx);
        const auto fnSaveStepper = reinterpret_cast<SaveSeedStepFn>(base + 0x135D9A0U);
        std::vector<std::uint8_t> fakeUserData(0x103000U, 0U);
        auto* saveSeedPtr = reinterpret_cast<std::uint32_t*>(fakeUserData.data() + 0x102758U + 2U * 4U);
        constexpr std::uint32_t kOrigSaveSeed = 0x3C4B2B0DU;

        // Measure pure vanilla output from `kMhwRngModeOff`
        host->SetEngineMode(kMhwRngModeOff);
        *saveSeedPtr = kOrigSaveSeed;
        fnSaveStepper(fakeUserData.data(), 2U);
        const std::uint32_t expSaveVanilla1 = *saveSeedPtr;
        fnSaveStepper(fakeUserData.data(), 2U);
        const std::uint32_t expSaveVanilla2 = *saveSeedPtr;

        // Also measure pure vanilla MtRandom::nrand(&sInst) sequence for SiteA / SiteB
        std::uint32_t refInst[4]{0x11223344U, 0x55667788U, 0x99AABBCCU, 0xDDEEFF00U};
        std::memcpy(sInst, refInst, sizeof(refInst));
        const std::uint32_t expSiteAVan1 = CallNrandSiteA(fnNrand, sInst);
        const std::uint32_t expSiteAVan2 = CallNrandSiteA(fnNrand, sInst);
        std::uint32_t expSiteAVan3_fromStep = 0U;
        {
            const std::uint32_t expB_DummyVan = CallNrandSiteB(fnNrand, sInst);
            (void)expB_DummyVan;
            expSiteAVan3_fromStep = CallNrandSiteA(fnNrand, sInst);
        }

        // Test `SetTakeoverEnabled` / `IsTakeoverEnabled` state machine
        if (host->IsTakeoverEnabled()) {
            std::printf("FAIL [Selective-1]: IsTakeoverEnabled should be false in Off mode\n");
            return 1;
        }
        host->SetTakeoverEnabled(true); // From Off -> Selective
        if (host->GetEngineMode() != kMhwRngModeSelective || !host->IsTakeoverEnabled()) {
            std::printf("FAIL [Selective-1]: SetTakeoverEnabled(true) from Off expected Selective, got %u\n", host->GetEngineMode());
            return 1;
        }
        host->SetEngineMode(kMhwRngModeTakeover);
        host->SetTakeoverEnabled(true); // Preserves Takeover if already active
        if (host->GetEngineMode() != kMhwRngModeTakeover) {
            std::printf("FAIL [Selective-1]: SetTakeoverEnabled(true) from Takeover should stay Takeover\n");
            return 1;
        }
        host->SetEngineMode(kMhwRngModeSelective);

        // Identify SiteA and SiteB RVAs and attach a GlobalObserver to verify telemetry
        struct SelObsState { std::uint32_t count = 0U; std::uint32_t lastRva = 0U; std::uint32_t lastFlags = 0U; } obsSt{};
        const MhwRngHookHandle hSelObs = host->RegisterGlobalObserver(
            kTestPluginId, 0U, (1U << kMhwRngSrcMtNrandGlobal),
            +[](const MhwRngDrawContext* c, void* ud) {
                auto* s = static_cast<SelObsState*>(ud);
                ++s->count;
                s->lastRva   = c->callSiteRva;
                s->lastFlags = c->callFlags;
            },
            &obsSt);

        // Reset sInst and draw SiteA unoverridden + observe SiteB RVA
        std::memcpy(sInst, refInst, sizeof(refInst));
        const std::uint32_t gotSelA1 = CallNrandSiteA(fnNrand, sInst);
        const std::uint32_t selARva  = obsSt.lastRva;
        const std::uint32_t selAFlags= obsSt.lastFlags;
        const std::uint32_t gotSelA2 = CallNrandSiteA(fnNrand, sInst);
        (void)CallNrandSiteB(fnNrand, sInst);
        const std::uint32_t selBRva  = obsSt.lastRva;

        // Now override ONLY SiteB with FixedValue = 0x12345678 and verify SiteA stays pure vanilla
        host->SetCondFixedValue(kTestPluginId, selBRva, 0U, 0x12345678U);
        const std::uint32_t gotSelB_ovr = CallNrandSiteB(fnNrand, sInst);
        const std::uint32_t gotSelA3    = CallNrandSiteA(fnNrand, sInst);
        host->UnregisterGlobalObserver(kTestPluginId, hSelObs);

        // Note: SiteB consumed a step on sInst before being overridden, so re-verify SiteA without touching SiteB on sInst
        std::memcpy(sInst, refInst, sizeof(refInst));
        (void)CallNrandSiteA(fnNrand, sInst);
        (void)CallNrandSiteA(fnNrand, sInst);
        const std::uint32_t gotSelB_ovr2 = CallNrandSiteB(fnNrand, sInst);
        const std::uint32_t gotSelA_afterOvr = CallNrandSiteA(fnNrand, sInst);
        (void)expSiteAVan3_fromStep;
        if (gotSelA1 != expSiteAVan1 || gotSelA2 != expSiteAVan2 ||
            (selAFlags & kMhwRngDrawFlagBypassed) == 0U ||
            gotSelB_ovr != 0x12345678U || gotSelB_ovr2 != 0x12345678U ||
            obsSt.count != 5U) {
            std::printf("FAIL [Selective-2]: A1=%X(%X) A2=%X(%X) B_ovr=%X obs=%u flags=0x%X\n",
                        gotSelA1, expSiteAVan1, gotSelA2, expSiteAVan2, gotSelB_ovr, obsSt.count, selAFlags);
            return 1;
        }
        // Notice after B is overridden in Selective mode, sInst wasn't stepped by vanilla B, so A now matches step#3 of sInst!
        std::memcpy(sInst, refInst, sizeof(refInst));
        (void)CallNrandSiteA(fnNrand, sInst);
        (void)CallNrandSiteA(fnNrand, sInst);
        const std::uint32_t pureStep3 = CallNrandSiteA(fnNrand, sInst);
        (void)gotSelA3; (void)selARva; (void)gotSelA_afterOvr;
        if (gotSelA_afterOvr != pureStep3) {
            std::printf("FAIL [Selective-2b]: Overridden SiteB unexpectedly stepped vanilla sInst (%X vs %X)!\n",
                        gotSelA_afterOvr, pureStep3);
            return 1;
        }

        // Test SlotHook in Selective mode (`ReturnValue` overrides; `UseCondition(0)` falls back to pure vanilla + still invokes postFn)
        struct SelHookState { bool shouldOverride = false; std::uint32_t lastPostFlags = 0U; std::uint32_t lastPostVal = 0U; } hkSt{};
        MhwRngHookDesc selHkDesc{};
        selHkDesc.structSize = sizeof(MhwRngHookDesc);
        selHkDesc.rva = selARva;
        selHkDesc.flags = kMhwRngHookPre | kMhwRngHookPost;
        selHkDesc.preFn = +[](const MhwRngDrawContext*, MhwRngHookDecision* d, void* ud) {
            auto* st = static_cast<SelHookState*>(ud);
            if (st->shouldOverride) {
                d->action = kMhwRngActionReturnValue;
                d->value0 = 0xCAFEBABEU;
            } else {
                d->action = kMhwRngActionUseCondition;
                d->condId = 0U;
            }
        };
        selHkDesc.postFn = +[](const MhwRngDrawContext* c, MhwRngHookDecision*, void* ud) {
            auto* st = static_cast<SelHookState*>(ud);
            st->lastPostFlags = c->callFlags;
            st->lastPostVal   = c->finalValue0;
        };
        selHkDesc.userData = &hkSt;
        const MhwRngHookHandle hSelHook = host->RegisterSlotHookEx(kTestPluginId, &selHkDesc);

        std::memcpy(sInst, refInst, sizeof(refInst));
        hkSt.shouldOverride = false;
        const std::uint32_t hookPassA = CallNrandSiteA(fnNrand, sInst);
        const bool passFlagsOk = (hkSt.lastPostFlags & (kMhwRngDrawFlagBypassed | kMhwRngDrawFlagHasVanilla)) ==
                                     (kMhwRngDrawFlagBypassed | kMhwRngDrawFlagHasVanilla) &&
                                 hkSt.lastPostVal == expSiteAVan1;
        hkSt.shouldOverride = true;
        const std::uint32_t hookOvrA = CallNrandSiteA(fnNrand, sInst);
        const bool ovrFlagsOk = (hkSt.lastPostFlags & kMhwRngDrawFlagOverridden) != 0U && hkSt.lastPostVal == 0xCAFEBABEU;
        host->UnregisterSlotHookHandle(kTestPluginId, hSelHook);

        if (hookPassA != expSiteAVan1 || !passFlagsOk || hookOvrA != 0xCAFEBABEU || !ovrFlagsOk) {
            std::printf("FAIL [Selective-3]: hookPass=%X(%X) passFlags=%d hookOvr=%X ovrFlags=%d\n",
                        hookPassA, expSiteAVan1, passFlagsOk, hookOvrA, ovrFlagsOk);
            return 1;
        }

        // Test `ProtectSaveSeeds` on real MHW 15.23 `FUN_14135d9a0`:
        //   (a) In Selective mode -> pure vanilla
        //   (b) In full Takeover mode with ProtectSaveSeeds=true -> STILL pure vanilla!
        //   (c) In full Takeover mode with ProtectSaveSeeds=false -> MIRA deterministic stream (!= vanilla)
        //   (d) In full Takeover mode with ProtectSaveSeeds=true + explicit FixedValue override on 0x135D9DC -> FixedValue!
        host->ClearAllRules();
        host->SetEngineMode(kMhwRngModeSelective);
        rng::SetProtectSaveSeeds(true);
        *saveSeedPtr = kOrigSaveSeed;
        fnSaveStepper(fakeUserData.data(), 2U);
        const std::uint32_t saveSel1 = *saveSeedPtr;
        fnSaveStepper(fakeUserData.data(), 2U);
        const std::uint32_t saveSel2 = *saveSeedPtr;

        host->SetEngineMode(kMhwRngModeTakeover);
        *saveSeedPtr = kOrigSaveSeed;
        fnSaveStepper(fakeUserData.data(), 2U);
        const std::uint32_t saveTakProt1 = *saveSeedPtr;
        fnSaveStepper(fakeUserData.data(), 2U);
        const std::uint32_t saveTakProt2 = *saveSeedPtr;

        rng::SetProtectSaveSeeds(false);
        *saveSeedPtr = kOrigSaveSeed;
        fnSaveStepper(fakeUserData.data(), 2U);
        const std::uint32_t saveTakUnprot = *saveSeedPtr;

        rng::SetProtectSaveSeeds(true);
        host->SetCondFixedValue(kTestPluginId, 0x135D9DCU, 0U, 0x11112222U);
        *saveSeedPtr = kOrigSaveSeed;
        fnSaveStepper(fakeUserData.data(), 2U);
        const std::uint32_t saveTakExplicit = *saveSeedPtr;
        host->ClearAllRules();

        if (saveSel1 != expSaveVanilla1 || saveSel2 != expSaveVanilla2 ||
            saveTakProt1 != expSaveVanilla1 || saveTakProt2 != expSaveVanilla2 ||
            saveTakUnprot == expSaveVanilla1 || saveTakExplicit != 0x11112222U) {
            std::printf("FAIL [ProtectSaveSeeds]: van=[%X,%X] sel=[%X,%X] takProt=[%X,%X] unprot=%X exp=%X\n",
                        expSaveVanilla1, expSaveVanilla2, saveSel1, saveSel2,
                        saveTakProt1, saveTakProt2, saveTakUnprot, saveTakExplicit);
            return 1;
        }
        std::printf("[OK] Selective mode + ProtectSaveSeeds verified on real MHW 15.23 `FUN_14135d9a0`!\n");
    }

    // 14. Verify `cmd_router`: `lang zh|en` across core + all 5 plugins, `rawseq`, `reload` state preservation & INI delta detection, and `exec` + recursion guard
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        std::string tmpMsg;
        if (!plugin_host::LoadOrReloadSingle("inspector", tmpMsg)) {
            std::printf("FAIL [CmdRouter-1]: failed to reload inspector: %s\n", tmpMsg.c_str());
            return 1;
        }
        host->SetEngineMode(kMhwRngModeTakeover);
        host->ClearAllRules();
        g_capturedChatLines.clear();

        cmd_router::Init(&cfg, &CaptureChatSink);

        // (a) Language toggle `en` <-> `zh` across core + all 5 plugins (+ direct C export check)
        if (!MhwRng_ExecuteCommand("lang en") ||
            std::strcmp(MhwRng_GetLanguage(), "en") != 0 || !app::IsEnglish()) {
            std::printf("FAIL [CmdRouter-2a]: `MhwRng_ExecuteCommand(\"lang en\")` failed!\n");
            return 1;
        }
        g_capturedChatLines.clear();
        cmd_router::HandleCommandLine("$rng status", true);
        const bool hasEngStatus = !g_capturedChatLines.empty() &&
                                  g_capturedChatLines[0].find("Mode:") != std::string::npos;
        cmd_router::HandleCommandLine("$rng size help", true);
        cmd_router::HandleCommandLine("$rng endemic help", true);
        cmd_router::HandleCommandLine("$rng steam help", true);
        cmd_router::HandleCommandLine("$rng thk help", true);
        cmd_router::HandleCommandLine("$rng names", true);
        const bool hasEngNames = !g_capturedChatLines.empty() &&
                                 g_capturedChatLines.back().find("Boss AI:") != std::string::npos;
        if (!hasEngStatus || !hasEngNames) {
            std::printf("FAIL [CmdRouter-2b]: English output missing after `$rng lang en`!\n");
            return 1;
        }

        if (!MhwRng_ExecuteCommand("lang zh") ||
            std::strcmp(MhwRng_GetLanguage(), "zh") != 0 || app::IsEnglish()) {
            std::printf("FAIL [CmdRouter-2c]: `MhwRng_ExecuteCommand(\"lang zh\")` failed!\n");
            return 1;
        }

        // (b) `$rng rawseq` + `rawclear`
        if (!cmd_router::HandleCommandLine("$rng rawseq 0x05000600 10,20,30 hold", true)) {
            std::printf("FAIL [CmdRouter-3a]: `$rng rawseq` failed!\n");
            return 1;
        }
        MhwRngSlotSnapshot rsSnap{};
        if (!host->GetCondSlotInfo(0x05000600U, 0U, &rsSnap) ||
            rsSnap.mode != kMhwRngSlotSequence || rsSnap.seqLength != 3U ||
            rsSnap.seqEndPolicy != kMhwRngSeqHoldLast) {
            std::printf("FAIL [CmdRouter-3b]: rawseq slot mismatch (mode=%u len=%u pol=%u)\n",
                        rsSnap.mode, rsSnap.seqLength, rsSnap.seqEndPolicy);
            return 1;
        }
        if (!cmd_router::HandleCommandLine("$rng rawclear 0x05000600", true) ||
            !host->GetCondSlotInfo(0x05000600U, 0U, &rsSnap) ||
            rsSnap.mode != kMhwRngSlotMasterDerived) {
            std::printf("FAIL [CmdRouter-3c]: `$rng rawclear` failed!\n");
            return 1;
        }

        // (c) `$rng reload` runtime preservation + AutoExec + INI delta override + `$rng exec` (>200B line & recursion limit)
        const std::string testIniPath  = app::IniPath();
        const std::string autoExecFile = plugDir + "\\_test_autoexec.txt";
        const std::string longExecFile = plugDir + "\\_test_exec_long.txt";
        const std::string subExecFile  = plugDir + "\\_test_exec_sub.txt";
        const std::string loopExecFile = plugDir + "\\_test_exec_loop.txt";

        bool hadOrigIni = false;
        std::string origIniContent;
        {
            FILE* fOld = nullptr;
            if (fopen_s(&fOld, testIniPath.c_str(), "rb") == 0 && fOld) {
                hadOrigIni = true;
                char buf[4096]{};
                std::size_t n = 0;
                while ((n = std::fread(buf, 1, sizeof(buf), fOld)) > 0) origIniContent.append(buf, n);
                std::fclose(fOld);
            }
        }

        auto writeTextFile = [](const std::string& p, const std::string& body) -> bool {
            FILE* f = nullptr;
            if (fopen_s(&f, p.c_str(), "wb") != 0 || !f) return false;
            std::fwrite(body.data(), 1, body.size(), f);
            std::fclose(f);
            return true;
        };

        if (!writeTextFile(autoExecFile,
                           "# autoexec comment\r\n"
                           "rawseq 0x05000610 42 hold\r\n")) {
            std::printf("FAIL [CmdRouter-4]: could not write temp autoexec file\n");
            return 1;
        }
        if (!writeTextFile(testIniPath,
                           "[General]\r\n"
                           "Enabled=1\r\n"
                           "BootMode=Selective\r\n"
                           "MasterSeed=12345\r\n"
                           "Language=zh\r\n"
                           "AutoExec=_test_autoexec.txt\r\n")) {
            std::printf("FAIL [CmdRouter-4]: could not write temp ini file\n");
            return 1;
        }
        cfg = app::LoadConfig();
        host->SetEngineMode(kMhwRngModeObserveOnly);
        MhwRng_SetMasterSeed(777888999ULL, false);

        // Reload with UNCHANGED INI: must preserve ObserveOnly + 777888999 and run AutoExec!
        cmd_router::HandleCommandLine("$rng reload", true);
        MhwRngSlotSnapshot aeSnap{};
        if (host->GetEngineMode() != kMhwRngModeObserveOnly ||
            host->GetMasterSeed() != 777888999ULL ||
            !host->GetCondSlotInfo(0x05000610U, 0U, &aeSnap) ||
            aeSnap.mode != kMhwRngSlotSequence || aeSnap.seqLength != 1U) {
            std::printf("FAIL [CmdRouter-4b]: `$rng reload` did not preserve runtime mode/seed or failed AutoExec (mode=%u seed=%llu)\n",
                        host->GetEngineMode(), static_cast<unsigned long long>(host->GetMasterSeed()));
            return 1;
        }

        // Edit INI BootMode & MasterSeed -> `$rng reload` must pick up the delta!
        writeTextFile(testIniPath,
                      "[General]\r\n"
                      "Enabled=1\r\n"
                      "BootMode=Takeover\r\n"
                      "MasterSeed=555666777\r\n"
                      "Language=zh\r\n"
                      "AutoExec=\r\n");
        cmd_router::HandleCommandLine("$rng reload", true);
        if (host->GetEngineMode() != kMhwRngModeTakeover ||
            host->GetMasterSeed() != 555666777ULL) {
            std::printf("FAIL [CmdRouter-4c]: `$rng reload` did not apply INI delta (mode=%u seed=%llu)\n",
                        host->GetEngineMode(), static_cast<unsigned long long>(host->GetMasterSeed()));
            return 1;
        }
        // Verify `$rng reload ini` forces re-applying INI even over changed runtime mode/seed
        host->SetEngineMode(kMhwRngModeObserveOnly);
        MhwRng_SetMasterSeed(111222333ULL, false);
        cmd_router::HandleCommandLine("$rng reload ini", true);
        if (host->GetEngineMode() != kMhwRngModeTakeover ||
            host->GetMasterSeed() != 555666777ULL) {
            std::printf("FAIL [CmdRouter-4d]: `$rng reload ini` did not force INI values (mode=%u seed=%llu)\n",
                        host->GetEngineMode(), static_cast<unsigned long long>(host->GetMasterSeed()));
            return 1;
        }

        // Long (>200B) line + BOM + `.txt` extension fallback + recursion guard (>4 depth)
        std::string longSeqCmd = "rawseq 0x05000620 ";
        for (int i = 1; i <= 60; ++i) {
            if (i > 1) longSeqCmd += ",";
            longSeqCmd += std::to_string(i);
        }
        longSeqCmd += " loop";
        std::string longScript("\xEF\xBB\xBF", 3);
        longScript += "; semicolon comment\r\n// slash comment\r\n# hash comment\r\n";
        longScript += longSeqCmd + "\r\n";
        longScript += "$rng exec _test_exec_sub\r\n";
        writeTextFile(longExecFile, longScript);
        writeTextFile(subExecFile, "exec _test_exec_loop\r\n");
        writeTextFile(loopExecFile, "exec _test_exec_loop quiet\r\n");

        if (!cmd_router::HandleCommandLine("$rng exec _test_exec_long", true)) {
            std::printf("FAIL [CmdRouter-5a]: `$rng exec` failed on valid script!\n");
            return 1;
        }
        MhwRngSlotSnapshot longSnap{};
        if (!host->GetCondSlotInfo(0x05000620U, 0U, &longSnap) ||
            longSnap.mode != kMhwRngSlotSequence || longSnap.seqLength != 60U) {
            std::printf("FAIL [CmdRouter-5b]: >200B script line truncated or failed (len=%u)!\n", longSnap.seqLength);
            return 1;
        }

        ::DeleteFileA(autoExecFile.c_str());
        ::DeleteFileA(longExecFile.c_str());
        ::DeleteFileA(subExecFile.c_str());
        ::DeleteFileA(loopExecFile.c_str());
        ::DeleteFileA(dumpPath.c_str());
        if (hadOrigIni) writeTextFile(testIniPath, origIniContent);
        else ::DeleteFileA(testIniPath.c_str());
        cmd_router::ResetSnapshots();
        host->ClearAllRules();
        MhwRng_SetMasterSeed(20261122ULL, true);
        std::printf("[OK] cmd_router (`lang zh|en`, `rawseq`, `reload` preservation + delta, `exec` >200B & recursion guard) verified!\n");
    }

    // 15. Verify Builtin Transparent Wrapper Unwrapping (`0x0672FBC` [10qw], `0x02CAB1E` [6qw], `0x02CD9D1` [8qw])
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        constexpr MhwRngPluginId kTestPluginId = 999U;
        host->SetEngineMode(kMhwRngModeTakeover);
        host->ClearAllRules();

        // Emit three 14-byte outer call sites (`sub rsp,28h; call rel32; add rsp,28h; ret`) at `base + 0x05000700/20/40`
        std::uint8_t* wrapCallers = img + 0x05000700ULL;
        std::uint32_t retRvas[3]{};
        const std::uint32_t targetWrapRvas[3] = {0x0672F90U, 0x02CAAF0U, 0x02CD9B0U};
        const std::uint32_t expectedWrapRet[3] = {0x0672FBCU, 0x02CAB1EU, 0x02CD9D1U};
        for (int i = 0; i < 3; ++i) {
            std::uint8_t* p = wrapCallers + i * 0x20U;
            // 48 83 EC 28 | E8 rel32 | 48 83 C4 28 C3 (`call` at +4 -> ret at +9)
            p[0] = 0x48; p[1] = 0x83; p[2] = 0xEC; p[3] = 0x28;
            p[4] = 0xE8;
            const std::uintptr_t nextPc = reinterpret_cast<std::uintptr_t>(p + 9);
            const std::uintptr_t dstPc  = base + targetWrapRvas[i];
            const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::intptr_t>(dstPc) - static_cast<std::intptr_t>(nextPc));
            std::memcpy(p + 5, &rel, sizeof(rel));
            p[9] = 0x48; p[10] = 0x83; p[11] = 0xC4; p[12] = 0x28; p[13] = 0xC3;
            retRvas[i] = static_cast<std::uint32_t>(0x05000700U + i * 0x20U + 9U);
        }
        ::FlushInstructionCache(::GetCurrentProcess(), wrapCallers, 0x80U);

        struct BuiltinWrapObs {
            std::uint32_t gotCallRva = 0U;
            std::uint32_t gotOrigRet = 0U;
        } bwObs{};
        const MhwRngHookHandle hBw = host->RegisterGlobalObserver(
            kTestPluginId, 0U, (1U << kMhwRngSrcMtNrandGlobal),
            +[](const MhwRngDrawContext* ctx, void* ud) {
                auto* o = static_cast<BuiltinWrapObs*>(ud);
                o->gotCallRva = ctx->callSiteRva;
                o->gotOrigRet = ctx->origReturnRva;
            },
            &bwObs);

        // Call #1: `0x0672F90` (`ret 0x0672FBC`, 10 qwords) -> `float(float min, float max)`
        bwObs = {};
        (void)reinterpret_cast<float(__fastcall*)(float, float)>(wrapCallers + 0x00U)(10.0f, 20.0f);
        if (bwObs.gotCallRva != retRvas[0] || bwObs.gotOrigRet != expectedWrapRet[0]) {
            std::printf("FAIL [Wrapper-1 0x0672FBC]: gotCall=0x%X (exp 0x%X) origRet=0x%X (exp 0x%X)\n",
                        bwObs.gotCallRva, retRvas[0], bwObs.gotOrigRet, expectedWrapRet[0]);
            return 1;
        }

        // Call #2: `0x02CAAF0` (`ret 0x02CAB1E`, 6 qwords) -> `int(int min, int max)`
        bwObs = {};
        const int rInt = reinterpret_cast<int(__fastcall*)(int, int)>(wrapCallers + 0x20U)(10, 20);
        if (bwObs.gotCallRva != retRvas[1] || bwObs.gotOrigRet != expectedWrapRet[1] || rInt < 10 || rInt > 20) {
            std::printf("FAIL [Wrapper-2 0x02CAB1E]: gotCall=0x%X (exp 0x%X) origRet=0x%X (exp 0x%X) r=%d\n",
                        bwObs.gotCallRva, retRvas[1], bwObs.gotOrigRet, expectedWrapRet[1], rInt);
            return 1;
        }

        // Call #3: `0x02CD9B0` (`ret 0x02CD9D1`, 8 qwords) -> `float/bool(void*, float pct)`
        bwObs = {};
        (void)reinterpret_cast<std::uint32_t(__fastcall*)(void*, float)>(wrapCallers + 0x40U)(nullptr, 50.0f);
        if (bwObs.gotCallRva != retRvas[2] || bwObs.gotOrigRet != expectedWrapRet[2]) {
            std::printf("FAIL [Wrapper-3 0x02CD9D1]: gotCall=0x%X (exp 0x%X) origRet=0x%X (exp 0x%X)\n",
                        bwObs.gotCallRva, retRvas[2], bwObs.gotOrigRet, expectedWrapRet[2]);
            return 1;
        }

        host->UnregisterGlobalObserver(kTestPluginId, hBw);
        std::printf("[OK] Builtin transparent wrappers (0x0672FBC/10qw, 0x02CAB1E/6qw, 0x02CD9D1/8qw) verified!\n");
    }

    // 16. Verify `SetInstancePolicy` on Real `0x142A74400` (`mt19937_heap_init`) & `0x142A744B0` (`mt19937_heap_draw`)
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        constexpr MhwRngPluginId kTestPluginId = 999U;
        using HeapInitFn = void(__fastcall*)(void* state, std::uint32_t seed);
        using HeapDrawFn = std::uint32_t(__fastcall*)(void* state);

        // Emit two 14-byte in-module call stubs (`sub rsp,28h; call rel32; add rsp,28h; ret`)
        // at `base + 0x05000780` (calling heap_init, ret RVA `0x05000789`)
        // and `base + 0x050007A0` (calling heap_draw, ret RVA `0x050007A9`).
        auto emitCallStub = [&](std::uint32_t stubRva, std::uintptr_t targetRva) -> std::uintptr_t {
            std::uint8_t* p = img + stubRva;
            p[0] = 0x48; p[1] = 0x83; p[2] = 0xEC; p[3] = 0x28;
            p[4] = 0xE8;
            const std::uintptr_t nextPc = reinterpret_cast<std::uintptr_t>(p + 9);
            const std::uintptr_t dstPc  = base + targetRva;
            const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::intptr_t>(dstPc) - static_cast<std::intptr_t>(nextPc));
            std::memcpy(p + 5, &rel, sizeof(rel));
            p[9] = 0x48; p[10] = 0x83; p[11] = 0xC4; p[12] = 0x28; p[13] = 0xC3;
            ::FlushInstructionCache(::GetCurrentProcess(), p, 14U);
            return reinterpret_cast<std::uintptr_t>(p);
        };
        const auto fnHeapInitStub = reinterpret_cast<HeapInitFn>(emitCallStub(0x05000780U, mhw::kRvaMt19937HeapInit));
        const auto fnHeapDrawStub = reinterpret_cast<HeapDrawFn>(emitCallStub(0x050007A0U, mhw::kRvaMt19937HeapDraw));
        constexpr std::uint32_t kHeapInitRetRva = 0x05000789U;

        alignas(16) std::vector<std::uint32_t> bufRef(624U, 0U);
        alignas(16) std::uint8_t stateRef[0x20]{};
        *reinterpret_cast<void**>(stateRef + 8) = bufRef.data();

        alignas(16) std::vector<std::uint32_t> bufTest(624U, 0U);
        alignas(16) std::uint8_t stateTest[0x20]{};
        *reinterpret_cast<void**>(stateTest + 8) = bufTest.data();

        // Capture pure vanilla MT19937 draws in `Off` mode with seed 0x1234ABCDU
        host->SetEngineMode(kMhwRngModeOff);
        fnHeapInitStub(stateRef, 0x1234ABCDU);
        std::uint32_t expHeap[6]{};
        for (int i = 0; i < 6; ++i) expHeap[i] = fnHeapDrawStub(stateRef);

        // Now switch to `Takeover` and explicitly override `kHeapInitRetRva` -> `0x1234ABCDU`
        // so `HookImpl_Mt19937HeapInit` tags `stateTest` via `TagExplicitlySeededInstance`!
        host->SetEngineMode(kMhwRngModeTakeover);
        host->SetCondFixedValue(kTestPluginId, kHeapInitRetRva, 0U, 0x1234ABCDU);
        fnHeapInitStub(stateTest, 0x99999999U);

        // Auto mode on explicitly seeded MT19937 heap must follow its own state (matches vanilla 0, 1)
        const std::uint32_t gotAuto0 = fnHeapDrawStub(stateTest);
        const std::uint32_t gotAuto1 = fnHeapDrawStub(stateTest);

        // ForceSlotControl switches `stateTest` to MIRA slot-controlled output (differs from expHeap[2])
        host->SetInstancePolicy(reinterpret_cast<std::uintptr_t>(stateTest), kMhwRngInstForceSlotControl);
        const std::uint32_t gotForced = fnHeapDrawStub(stateTest);

        // Restore FollowOwnState / Auto: note vanilla index did NOT advance during `ForceSlotControl`, so next own draw is `expHeap[2]`!
        host->SetInstancePolicy(reinterpret_cast<std::uintptr_t>(stateTest), kMhwRngInstFollowOwnState);
        const std::uint32_t gotOwn2 = fnHeapDrawStub(stateTest);
        host->SetInstancePolicy(reinterpret_cast<std::uintptr_t>(stateTest), kMhwRngInstAuto);
        const std::uint32_t gotOwn3 = fnHeapDrawStub(stateTest);

        host->ClearSlotAllConditions(kHeapInitRetRva);

        if (gotAuto0 != expHeap[0] || gotAuto1 != expHeap[1] ||
            gotForced == expHeap[2] || gotOwn2 != expHeap[2] || gotOwn3 != expHeap[3]) {
            std::printf("FAIL [HeapMT-Policy]: auto=[%X,%X](exp %X,%X) forced=%X(vs %X) own=[%X,%X](exp %X,%X)\n",
                        gotAuto0, gotAuto1, expHeap[0], expHeap[1],
                        gotForced, expHeap[2], gotOwn2, gotOwn3, expHeap[2], expHeap[3]);
            return 1;
        }
        std::printf("[OK] MT19937 heap `SetInstancePolicy` (Auto / ForceSlotControl / FollowOwnState) verified on 0x142A74400/B0!\n");
    }

    // 17. Verify Concurrent Hot-Reload + `SetMasterSeed` / `QuerySiteMeta` / `FormatSlotCustom` Drain Safety
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        std::atomic<bool> stopStress{false};
        std::atomic<std::uint64_t> metaCalls{0};
        std::vector<std::thread> stressWorkers;
        for (int t = 0; t < 4; ++t) {
            stressWorkers.emplace_back([&, t]() {
                std::uint64_t iter = 0;
                while (!stopStress.load(std::memory_order_relaxed)) {
                    MhwRngSiteMeta meta{};
                    host->QuerySiteMeta(0x0133A876U, &meta);
                    MhwRngSlotSnapshot snap{};
                    snap.callSiteRva   = 0x00666795U;
                    snap.mode          = kMhwRngSlotMasterDerived;
                    snap.hasPluginHook = 1U;
                    char fmtBuf[128]{};
                    host->FormatSlotCustom(&snap, fmtBuf, sizeof(fmtBuf));
                    if (t == 0 && (iter % 16ULL) == 0ULL) {
                        MhwRng_SetMasterSeed(20261122ULL + iter, false);
                    }
                    ++iter;
                    metaCalls.fetch_add(1ULL, std::memory_order_relaxed);
                }
            });
        }

        for (int r = 0; r < 10; ++r) {
            std::string rmsg;
            if (!plugin_host::LoadOrReloadSingle((r % 2 == 0) ? "steam" : "inspector", rmsg)) {
                stopStress.store(true, std::memory_order_relaxed);
                for (auto& w : stressWorkers) w.join();
                std::printf("FAIL [ConcurrentReload]: reload failed at iter %d: %s\n", r, rmsg.c_str());
                return 1;
            }
            ::Sleep(2);
        }

        stopStress.store(true, std::memory_order_relaxed);
        for (auto& w : stressWorkers) w.join();
        if (metaCalls.load() == 0ULL) {
            std::printf("FAIL [ConcurrentReload]: zero worker iterations\n");
            return 1;
        }
        std::printf("[OK] Concurrent hot-reload (10 cycles) + 4-thread HostApi drain (%llu calls) verified!\n",
                    static_cast<unsigned long long>(metaCalls.load()));
    }

    // 18. Verify Multi-Plugin Chained `FilterVanilla` & `FilterSlot` (`Takeover` / `Sequence` / `Selective` / `Pre+FilterSlot`)
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        constexpr MhwRngPluginId kTestPluginId = 999U;
        host->SetEngineMode(kMhwRngModeTakeover);
        host->ClearAllRules();

        auto emitCallStub18 = [&](std::uint32_t stubRva, std::uintptr_t targetRva) -> std::uintptr_t {
            std::uint8_t* p = img + stubRva;
            p[0] = 0x48; p[1] = 0x83; p[2] = 0xEC; p[3] = 0x28;
            p[4] = 0xE8;
            const std::uintptr_t nextPc = reinterpret_cast<std::uintptr_t>(p + 9);
            const std::uintptr_t dstPc  = base + targetRva;
            const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::intptr_t>(dstPc) - static_cast<std::intptr_t>(nextPc));
            std::memcpy(p + 5, &rel, sizeof(rel));
            p[9] = 0x48; p[10] = 0x83; p[11] = 0xC4; p[12] = 0x28; p[13] = 0xC3;
            ::FlushInstructionCache(::GetCurrentProcess(), p, 14U);
            return reinterpret_cast<std::uintptr_t>(p);
        };

        const auto fnStubVanilla = reinterpret_cast<TestNrandFn>(emitCallStub18(0x05000800U, mhw::kRvaMtRandomNrand));
        const auto fnStubSlot    = reinterpret_cast<TestNrandFn>(emitCallStub18(0x05000820U, mhw::kRvaMtRandomNrand));
        constexpr std::uint32_t kRvaFilterVanilla = 0x05000809U;
        constexpr std::uint32_t kRvaFilterSlot    = 0x05000829U;

        // (a) Chained FilterVanilla: priority -10 sets 100U, priority +10 adds +23U -> 123U; read-only Post sees 123U!
        struct FvChainState {
            int aCalls = 0, bCalls = 0, cCalls = 0;
            bool flagsOk = true;
            std::uint32_t cFinal = 0U;
        } fvSt{};

        MhwRngHookDesc fvA{};
        fvA.structSize = sizeof(fvA);
        fvA.rva        = kRvaFilterVanilla;
        fvA.priority   = -10;
        fvA.flags      = kMhwRngHookFilterVanilla;
        fvA.userData   = &fvSt;
        fvA.postFn     = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void* ud) {
            auto* s = static_cast<FvChainState*>(ud);
            ++s->aCalls;
            if ((ctx->callFlags & (kMhwRngDrawFlagPostDraw | kMhwRngDrawFlagCanModify | kMhwRngDrawFlagHasVanilla)) !=
                (kMhwRngDrawFlagPostDraw | kMhwRngDrawFlagCanModify | kMhwRngDrawFlagHasVanilla)) {
                s->flagsOk = false;
            }
            dec->action = kMhwRngActionReturnValue;
            dec->value0 = 100U;
        };

        MhwRngHookDesc fvB{};
        fvB.structSize = sizeof(fvB);
        fvB.rva        = kRvaFilterVanilla;
        fvB.priority   = 10;
        fvB.flags      = kMhwRngHookFilterVanilla;
        fvB.userData   = &fvSt;
        fvB.postFn     = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void* ud) {
            auto* s = static_cast<FvChainState*>(ud);
            ++s->bCalls;
            if ((ctx->callFlags & kMhwRngDrawFlagCanModify) == 0U || ctx->finalValue0 != 100U) {
                s->flagsOk = false;
            }
            dec->action = kMhwRngActionReturnValue;
            dec->value0 = ctx->finalValue0 + 23U;
        };

        MhwRngHookDesc fvPost{};
        fvPost.structSize = sizeof(fvPost);
        fvPost.rva        = kRvaFilterVanilla;
        fvPost.priority   = 20;
        fvPost.flags      = kMhwRngHookPost;
        fvPost.userData   = &fvSt;
        fvPost.postFn     = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision*, void* ud) {
            auto* s = static_cast<FvChainState*>(ud);
            ++s->cCalls;
            if ((ctx->callFlags & kMhwRngDrawFlagCanModify) != 0U) s->flagsOk = false;
            s->cFinal = ctx->finalValue0;
        };

        const MhwRngHookHandle hFvA = host->RegisterSlotHookEx(kTestPluginId, &fvA);
        const MhwRngHookHandle hFvB = host->RegisterSlotHookEx(kTestPluginId, &fvB);
        const MhwRngHookHandle hFvC = host->RegisterSlotHookEx(kTestPluginId, &fvPost);
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t v = fnStubVanilla(sInst);
            if (v != 123U) {
                std::printf("FAIL [FilterVanillaChain]: iter %d expected 123, got %u\n", i, v);
                return 1;
            }
        }
        host->UnregisterSlotHookHandle(kTestPluginId, hFvA);
        host->UnregisterSlotHookHandle(kTestPluginId, hFvB);
        host->UnregisterSlotHookHandle(kTestPluginId, hFvC);
        if (!fvSt.flagsOk || fvSt.aCalls != 3 || fvSt.bCalls != 3 || fvSt.cCalls != 3 || fvSt.cFinal != 123U) {
            std::printf("FAIL [FilterVanillaChain]: a=%d b=%d c=%d ok=%d cFinal=%u\n",
                        fvSt.aCalls, fvSt.bCalls, fvSt.cCalls, fvSt.flagsOk, fvSt.cFinal);
            return 1;
        }

        // (b) Chained FilterSlot on Sequence + Selective + Pre|FilterSlot
        MhwRngHookDesc fsLo{};
        fsLo.structSize = sizeof(fsLo);
        fsLo.rva        = kRvaFilterSlot;
        fsLo.priority   = -10;
        fsLo.flags      = kMhwRngHookFilterSlot;
        fsLo.postFn     = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void*) {
            if (ctx->finalValue0 < 50U) {
                dec->action = kMhwRngActionReturnValue;
                dec->value0 = 50U;
            }
        };

        MhwRngHookDesc fsHi{};
        fsHi.structSize = sizeof(fsHi);
        fsHi.rva        = kRvaFilterSlot;
        fsHi.priority   = 10;
        fsHi.flags      = kMhwRngHookFilterSlot;
        fsHi.postFn     = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void*) {
            if (ctx->finalValue0 > 300U) {
                dec->action = kMhwRngActionReturnValue;
                dec->value0 = 300U;
            }
        };

        const MhwRngHookHandle hFsLo = host->RegisterSlotHookEx(kTestPluginId, &fsLo);
        const MhwRngHookHandle hFsHi = host->RegisterSlotHookEx(kTestPluginId, &fsHi);
        const std::uint32_t rawSeqVals[4] = {10U, 200U, 30U, 450U};
        const std::uint32_t expSeqVals[4] = {50U, 200U, 50U, 300U};
        host->SetCondSequence(kTestPluginId, kRvaFilterSlot, 0U, rawSeqVals, 4U, kMhwRngSeqLoop);
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t got = fnStubSlot(sInst);
            if (got != expSeqVals[i]) {
                std::printf("FAIL [FilterSlot-Seq]: step %d got %u, expected %u\n", i, got, expSeqVals[i]);
                return 1;
            }
        }

        // Now verify FilterSlot in Selective mode over vanilla RNG draws
        host->ClearSlotAllConditions(kRvaFilterSlot);
        host->SetEngineMode(kMhwRngModeSelective);
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t got = fnStubSlot(sInst);
            if (got < 50U || got > 300U) {
                std::printf("FAIL [FilterSlot-Selective]: got %u outside [50, 300]\n", got);
                return 1;
            }
        }
        host->UnregisterSlotHookHandle(kTestPluginId, hFsLo);
        host->UnregisterSlotHookHandle(kTestPluginId, hFsHi);

        // Pre + FilterSlot combo with condition routing (`condId = 3`)
        host->SetEngineMode(kMhwRngModeTakeover);
        const std::uint32_t cond3Vals[3] = {1U, 2U, 3U};
        host->SetCondSequence(kTestPluginId, kRvaFilterSlot, 3U, cond3Vals, 3U, kMhwRngSeqLoop);
        MhwRngHookDesc preFilt{};
        preFilt.structSize = sizeof(preFilt);
        preFilt.rva        = kRvaFilterSlot;
        preFilt.priority   = 0;
        preFilt.flags      = kMhwRngHookPre | kMhwRngHookFilterSlot;
        preFilt.preFn      = +[](const MhwRngDrawContext*, MhwRngHookDecision* dec, void*) {
            dec->action = kMhwRngActionUseCondition;
            dec->condId = 3U;
        };
        preFilt.postFn     = +[](const MhwRngDrawContext* ctx, MhwRngHookDecision* dec, void*) {
            dec->action = kMhwRngActionReturnValue;
            dec->value0 = ctx->finalValue0 + 1000U;
        };
        const MhwRngHookHandle hPreFilt = host->RegisterSlotHookEx(kTestPluginId, &preFilt);
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t got = fnStubSlot(sInst);
            if (got != static_cast<std::uint32_t>(1001 + i)) {
                std::printf("FAIL [FilterSlot-PreCombo]: iter %d got %u, expected %d\n", i, got, 1001 + i);
                return 1;
            }
        }
        host->UnregisterSlotHookHandle(kTestPluginId, hPreFilt);
        host->ClearSlotAllConditions(kRvaFilterSlot);
        std::printf("[OK] Multi-plugin chained FilterVanilla + FilterSlot (Takeover/Selective/PreCombo) verified!\n");
    }

    // 19. Verify `XMM6/XMM7` Capture + 4-Hop `.pdata` `CaptureCallerRvas` + Standardized `$rng trace`
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        constexpr MhwRngPluginId kTestPluginId = 999U;
        host->SetEngineMode(kMhwRngModeTakeover);
        host->ClearAllRules();

        // (a) Non-volatile XMM6 / XMM7 capture thunk at `base + 0x05000980`
        //   void CallWithXmm67(void* sInst_rcx, const float* xmm6_rdx, const float* xmm7_r8)
        std::uint8_t* xmmThunk = img + 0x05000980U;
        std::uint32_t xmmRetRva = 0U;
        {
            std::size_t o = 0U;
            auto put = [&](std::initializer_list<std::uint8_t> b) {
                for (std::uint8_t x : b) xmmThunk[o++] = x;
            };
            put({0x48, 0x83, 0xEC, 0x48});                   // sub rsp, 48h
            put({0x0F, 0x29, 0x74, 0x24, 0x20});             // movaps [rsp+20h], xmm6
            put({0x0F, 0x29, 0x7C, 0x24, 0x30});             // movaps [rsp+30h], xmm7
            put({0x0F, 0x10, 0x32});                         // movups xmm6, [rdx]
            put({0x41, 0x0F, 0x10, 0x38});                   // movups xmm7, [r8]
            put({0xE8});                                     // call fnNrand
            const std::uintptr_t nextPc = reinterpret_cast<std::uintptr_t>(xmmThunk + o + 4U);
            const std::uintptr_t dstPc  = base + mhw::kRvaMtRandomNrand;
            const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::intptr_t>(dstPc) - static_cast<std::intptr_t>(nextPc));
            std::memcpy(xmmThunk + o, &rel, sizeof(rel));
            o += 4U;
            xmmRetRva = static_cast<std::uint32_t>(0x05000980U + o);
            put({0x0F, 0x28, 0x74, 0x24, 0x20});             // movaps xmm6, [rsp+20h]
            put({0x0F, 0x28, 0x7C, 0x24, 0x30});             // movaps xmm7, [rsp+30h]
            put({0x48, 0x83, 0xC4, 0x48, 0xC3});             // add rsp, 48h; ret
            ::FlushInstructionCache(::GetCurrentProcess(), xmmThunk, o);
        }

        struct XmmObs {
            bool hasFlag = false;
            float xmm6   = 0.0f;
            float xmm7   = 0.0f;
        } xmmObs{};
        const MhwRngHookHandle hXmm = host->RegisterGlobalObserver(
            kTestPluginId, xmmRetRva, (1U << kMhwRngSrcMtNrandGlobal),
            +[](const MhwRngDrawContext* ctx, void* ud) {
                auto* x = static_cast<XmmObs*>(ud);
                x->hasFlag = ((ctx->callFlags & kMhwRngDrawFlagHasXmmRegs) != 0U) && (ctx->callerRegs != nullptr);
                if (x->hasFlag) {
                    x->xmm6 = ctx->callerRegs->xmm6.f32[0];
                    x->xmm7 = ctx->callerRegs->xmm7.f32[0];
                }
            },
            &xmmObs);
        alignas(16) const float inXmm6[4] = {123.5f, 1.0f, 2.0f, 3.0f};
        alignas(16) const float inXmm7[4] = {-456.25f, 4.0f, 5.0f, 6.0f};
        using CallXmmFn = std::uint32_t(__fastcall*)(void*, const float*, const float*);
        (void)reinterpret_cast<CallXmmFn>(xmmThunk)(sInst, inXmm6, inXmm7);
        host->UnregisterGlobalObserver(kTestPluginId, hXmm);
        if (!xmmObs.hasFlag || std::fabs(xmmObs.xmm6 - 123.5f) > 1e-5f ||
            std::fabs(xmmObs.xmm7 - (-456.25f)) > 1e-5f) {
            std::printf("FAIL [XMM6/7]: has=%d xmm6=%.3f xmm7=%.3f\n",
                        xmmObs.hasFlag, xmmObs.xmm6, xmmObs.xmm7);
            return 1;
        }

        // (b) 4-hop nested `.pdata` functions (`Hop3 -> Hop2 -> Hop1 -> Hop0 -> nrand`) via RtlAddFunctionTable
        constexpr std::uint32_t kHop0Start = 0x05000900U;
        constexpr std::uint32_t kHop1Start = 0x05000920U;
        constexpr std::uint32_t kHop2Start = 0x05000940U;
        constexpr std::uint32_t kHop3Start = 0x05000960U;

        auto emitStdHop = [&](std::uint32_t startRva, std::uintptr_t targetRva) -> std::uint32_t {
            std::uint8_t* p = img + startRva;
            p[0] = 0x48; p[1] = 0x83; p[2] = 0xEC; p[3] = 0x28;
            p[4] = 0xE8;
            const std::uintptr_t nextPc = reinterpret_cast<std::uintptr_t>(p + 9);
            const std::uintptr_t dstPc  = base + targetRva;
            const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::intptr_t>(dstPc) - static_cast<std::intptr_t>(nextPc));
            std::memcpy(p + 5, &rel, sizeof(rel));
            p[9] = 0x48; p[10] = 0x83; p[11] = 0xC4; p[12] = 0x28; p[13] = 0xC3;
            ::FlushInstructionCache(::GetCurrentProcess(), p, 14U);
            return startRva + 9U;
        };

        const std::uint32_t kHop0Ret = emitStdHop(kHop0Start, mhw::kRvaMtRandomNrand);
        const std::uint32_t kHop1Ret = emitStdHop(kHop1Start, kHop0Start);
        // Hop2 uses `push rbx; sub rsp, 30h; call Hop1; add rsp, 30h; pop rbx; ret`
        const std::uint32_t kHop2Ret = kHop2Start + 10U;
        {
            std::uint8_t* p = img + kHop2Start;
            p[0] = 0x53;                                     // push rbx
            p[1] = 0x48; p[2] = 0x83; p[3] = 0xEC; p[4] = 0x30; // sub rsp, 30h
            p[5] = 0xE8;                                     // call Hop1
            const std::uintptr_t nextPc = reinterpret_cast<std::uintptr_t>(p + 10);
            const std::uintptr_t dstPc  = base + kHop1Start;
            const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::intptr_t>(dstPc) - static_cast<std::intptr_t>(nextPc));
            std::memcpy(p + 6, &rel, sizeof(rel));
            p[10] = 0x48; p[11] = 0x83; p[12] = 0xC4; p[13] = 0x30; // add rsp, 30h
            p[14] = 0x5B; p[15] = 0xC3;                      // pop rbx; ret
            ::FlushInstructionCache(::GetCurrentProcess(), p, 16U);
        }
        const std::uint32_t kHop3Ret = emitStdHop(kHop3Start, kHop2Start);

        // Build aligned UNWIND_INFO structures in `img + 0x05000A00..0x05000A30`
        const std::uint32_t kUw0 = 0x05000A00U;
        const std::uint32_t kUw1 = 0x05000A10U;
        const std::uint32_t kUw2 = 0x05000A20U;
        const std::uint32_t kUw3 = 0x05000A30U;
        auto writeUnwindSmall28 = [&](std::uint32_t rva) {
            std::uint8_t* u = img + rva;
            u[0] = 0x01; // Version=1, Flags=0
            u[1] = 0x04; // SizeOfProlog=4
            u[2] = 0x01; // CountOfCodes=1
            u[3] = 0x00; // FrameReg/Off=0
            u[4] = 0x04; // CodeOffset=4
            u[5] = 0x42; // UWOP_ALLOC_SMALL (2) | OpInfo=4 ((4+1)*8 = 0x28)
            u[6] = 0x00; u[7] = 0x00;
        };
        writeUnwindSmall28(kUw0);
        writeUnwindSmall28(kUw1);
        writeUnwindSmall28(kUw3);
        {
            std::uint8_t* u = img + kUw2;
            u[0] = 0x01; // Version=1, Flags=0
            u[1] = 0x05; // SizeOfProlog=5
            u[2] = 0x02; // CountOfCodes=2
            u[3] = 0x00; // FrameReg/Off=0
            u[4] = 0x05; // CodeOffset=5 (`sub rsp, 30h`)
            u[5] = 0x52; // UWOP_ALLOC_SMALL (2) | OpInfo=5 ((5+1)*8 = 0x30)
            u[6] = 0x01; // CodeOffset=1 (`push rbx`)
            u[7] = 0x30; // UWOP_PUSH_NONVOL (0) | OpInfo=3 (RBX)
        }

        alignas(16) static RUNTIME_FUNCTION synPdata[4]{};
        synPdata[0] = RUNTIME_FUNCTION{kHop0Start, kHop0Start + 14U, kUw0};
        synPdata[1] = RUNTIME_FUNCTION{kHop1Start, kHop1Start + 14U, kUw1};
        synPdata[2] = RUNTIME_FUNCTION{kHop2Start, kHop2Start + 16U, kUw2};
        synPdata[3] = RUNTIME_FUNCTION{kHop3Start, kHop3Start + 14U, kUw3};
        if (!::RtlAddFunctionTable(synPdata, 4U, base)) {
            std::printf("FAIL [PdataUnwind]: RtlAddFunctionTable failed\n");
            return 1;
        }

        struct UnwindCheckState {
            const MhwRngHostApi* host = nullptr;
            std::uint32_t count = 0U;
            std::uint32_t rvas[8]{};
        } uwSt{host, 0U, {}};
        const MhwRngHookHandle hUw = host->RegisterGlobalObserver(
            kTestPluginId, kHop0Ret, (1U << kMhwRngSrcMtNrandGlobal),
            +[](const MhwRngDrawContext* ctx, void* ud) {
                auto* s = static_cast<UnwindCheckState*>(ud);
                s->count = MhwRng_HostCaptureCallerRvas(s->host, ctx, s->rvas, 8U);
            },
            &uwSt);
        (void)reinterpret_cast<TestNrandFn>(base + kHop3Start)(sInst);
        host->UnregisterGlobalObserver(kTestPluginId, hUw);

        if (uwSt.count < 4U ||
            uwSt.rvas[0] != kHop0Ret || uwSt.rvas[1] != kHop1Ret ||
            uwSt.rvas[2] != kHop2Ret || uwSt.rvas[3] != kHop3Ret) {
            std::printf("FAIL [PdataUnwind]: count=%u got=[%X, %X, %X, %X] exp=[%X, %X, %X, %X]\n",
                        uwSt.count, uwSt.rvas[0], uwSt.rvas[1], uwSt.rvas[2], uwSt.rvas[3],
                        kHop0Ret, kHop1Ret, kHop2Ret, kHop3Ret);
            ::RtlDeleteFunctionTable(synPdata);
            return 1;
        }

        // (c) Verify `$rng trace` in `rng_inspector.dll` (trace on -> 3 calls -> log -> dump -> off -> clear)
        cmd_router::Init(&cfg, &CaptureChatSink);
        g_capturedChatLines.clear();
        if (!cmd_router::HandleCommandLine("$rng trace 0x5000909", true)) {
            std::printf("FAIL [InspectorTrace-1]: `$rng trace 0x5000909` command failed\n");
            ::RtlDeleteFunctionTable(synPdata);
            return 1;
        }
        for (int i = 0; i < 3; ++i) {
            (void)reinterpret_cast<TestNrandFn>(base + kHop3Start)(sInst);
        }
        g_capturedChatLines.clear();
        cmd_router::HandleCommandLine("$rng trace log", true);
        const std::string expChainSub = "0x5000909 <- 0x5000929 <- 0x500094A <- 0x5000969";
        bool sawChainInChat = false;
        for (const std::string& line : g_capturedChatLines) {
            if (line.find(expChainSub) != std::string::npos && line.find("3 hits") != std::string::npos) {
                sawChainInChat = true;
                break;
            }
        }
        const std::string traceDumpPath = buildDir + "\\_test_mhw_rng_trace.txt";
        const std::string traceDumpCmd  = "$rng trace dump " + traceDumpPath;
        cmd_router::HandleCommandLine(traceDumpCmd.c_str(), true);
        bool sawChainInDump = false;
        {
            FILE* tfp = nullptr;
            if (fopen_s(&tfp, traceDumpPath.c_str(), "rb") == 0 && tfp) {
                std::string body;
                char buf[1024]{};
                std::size_t nr = 0;
                while ((nr = std::fread(buf, 1, sizeof(buf), tfp)) > 0) body.append(buf, nr);
                std::fclose(tfp);
                sawChainInDump = (body.find(expChainSub) != std::string::npos);
            }
        }
        ::DeleteFileA(traceDumpPath.c_str());
        cmd_router::HandleCommandLine("$rng trace off", true);
        cmd_router::HandleCommandLine("$rng trace clear", true);
        ::RtlDeleteFunctionTable(synPdata);

        if (!sawChainInChat || !sawChainInDump) {
            std::printf("FAIL [InspectorTrace-2]: chat=%d dump=%d\n", sawChainInChat, sawChainInDump);
            return 1;
        }
        std::printf("[OK] XMM6/XMM7 capture + 4-hop `.pdata` CaptureCallerRvas + `$rng trace` verified!\n");
    }

    // 20. Verify Standardized i18n API (SDK v3.3), Fallback Chain & Zero-CJK Sweep in `$rng lang en`
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        constexpr MhwRngPluginId kTestPluginId = 999U;

        auto containsCjkUtf8 = [](const std::string& s) -> bool {
            const std::size_t n = s.size();
            for (std::size_t i = 0U; i < n;) {
                const auto b0 = static_cast<unsigned char>(s[i]);
                if (b0 < 0x80U) {
                    ++i;
                    continue;
                }
                std::uint32_t cp = 0U;
                std::size_t len = 1U;
                if ((b0 & 0xE0U) == 0xC0U && i + 1U < n) {
                    cp = ((b0 & 0x1FU) << 6) | (static_cast<unsigned char>(s[i + 1U]) & 0x3FU);
                    len = 2U;
                } else if ((b0 & 0xF0U) == 0xE0U && i + 2U < n) {
                    cp = ((b0 & 0x0FU) << 12) |
                         ((static_cast<unsigned char>(s[i + 1U]) & 0x3FU) << 6) |
                         (static_cast<unsigned char>(s[i + 2U]) & 0x3FU);
                    len = 3U;
                } else if ((b0 & 0xF8U) == 0xF0U && i + 3U < n) {
                    cp = ((b0 & 0x07U) << 18) |
                         ((static_cast<unsigned char>(s[i + 1U]) & 0x3FU) << 12) |
                         ((static_cast<unsigned char>(s[i + 2U]) & 0x3FU) << 6) |
                         (static_cast<unsigned char>(s[i + 3U]) & 0x3FU);
                    len = 4U;
                }
                if ((cp >= 0x3000U && cp <= 0x303FU) ||
                    (cp >= 0x4E00U && cp <= 0x9FFFU) ||
                    (cp >= 0xFF00U && cp <= 0xFFEFU)) {
                    return true;
                }
                i += len;
            }
            return false;
        };

        // (a) Verify `SelectText` / `MhwRng_TrTable` fallback chain & `kMhwRngEventLanguageChanged`
        struct LangEvState {
            int calls = 0;
            std::uint64_t lastPrev = 999ULL;
            std::uint32_t lastMode = 0U;
        } langEv{};
        const MhwRngHookHandle hLangLife = host->RegisterLifecycleHook(
            kTestPluginId,
            +[](const MhwRngEventInfo* info, void* ud) {
                if (!info || info->eventKind != kMhwRngEventLanguageChanged) return;
                auto* st = static_cast<LangEvState*>(ud);
                ++st->calls;
                st->lastPrev = info->prevValue;
                st->lastMode = info->engineMode;
            },
            &langEv);

        const MhwRngLocaleText tblZhEnJa[3] = {
            {"zh", u8"中文文案"},
            {"en", "English text"},
            {"ja", u8"日本語テキスト"}
        };
        const MhwRngLocaleText tblZhEn[2] = {
            {"zh", u8"仅中英-中"},
            {"en", "En-Fallback"}
        };
        const MhwRngLocaleText tblZhOnly[1] = {
            {"zh", u8"仅中文兜底"}
        };
        const MhwRngLocaleText tblFrOnly[1] = {
            {"fr", "Texte FR"}
        };

        MhwRng_SetLanguage("zh");
        if (std::strcmp(MhwRng_GetLanguage(), "zh") != 0 ||
            MhwRng_GetLanguageId() != kMhwRngLangZh ||
            std::strcmp(MhwRng_TrTable(host, tblZhEnJa, 3U, "fb"), u8"中文文案") != 0) {
            std::printf("FAIL [i18n-zh]: lang=%s id=%u\n", MhwRng_GetLanguage(), MhwRng_GetLanguageId());
            host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
            return 1;
        }

        MhwRng_SetLanguage("ja-JP");
        if (std::strcmp(MhwRng_GetLanguage(), "ja") != 0 ||
            MhwRng_GetLanguageId() != kMhwRngLangJa ||
            langEv.calls != 1 || langEv.lastPrev != kMhwRngLangZh ||
            std::strcmp(MhwRng_TrTable(host, tblZhEnJa, 3U, "fb"), u8"日本語テキスト") != 0 ||
            std::strcmp(MhwRng_TrTable(host, tblZhEn,   2U, "fb"), "En-Fallback") != 0 ||
            std::strcmp(MhwRng_TrTable(host, tblZhOnly, 1U, "fb"), u8"仅中文兜底") != 0 ||
            std::strcmp(MhwRng_TrTable(host, tblFrOnly, 1U, "fb"), "Texte FR") != 0 ||
            std::strcmp(MhwRng_TrTable(host, nullptr,   0U, "fb"), "fb") != 0) {
            std::printf("FAIL [i18n-ja]: lang=%s id=%u ev=%d prev=%llu\n",
                        MhwRng_GetLanguage(), MhwRng_GetLanguageId(), langEv.calls,
                        static_cast<unsigned long long>(langEv.lastPrev));
            host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
            return 1;
        }

        MhwRng_SetLanguage("fr");
        if (std::strcmp(MhwRng_GetLanguage(), "fr") != 0 ||
            MhwRng_GetLanguageId() != kMhwRngLangOther ||
            std::strcmp(MhwRng_Tr(host, u8"中", "EN"), "EN") != 0) {
            std::printf("FAIL [i18n-fr]: lang=%s id=%u\n", MhwRng_GetLanguage(), MhwRng_GetLanguageId());
            host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
            return 1;
        }

        // (b) Switch to `en` via chat command and sweep all core + official plugin commands for zero CJK leaks
        cmd_router::Init(&cfg, &CaptureChatSink);
        g_capturedChatLines.clear();
        const std::string dumpEnPath = buildDir + "\\_test_dump_en.txt";
        const std::vector<std::string> enCmds = {
            "$rng lang en",
            "$rng status",
            "$rng plugins",
            "$rng help",
            "$rng help steam",
            "$rng top all",
            "$rng list",
            "$rng find meal",
            "$rng find fatalis",
            "$rng info em.size",
            "$rng trace em.size",
            "$rng size rajang max",
            "$rng size status",
            "$rng size monsters rajang",
            "$rng endemic ec059 max",
            "$rng endemic status",
            "$rng endemic rare on",
            "$rng endemic rare status",
            "$rng endemic rare history",
            "$rng endemic rare help",
            "$rng endemic rare ec058 0%",
            "$rng endemic rare wintermoon_nettle on",
            "$rng endemic rare moly* 0%",
            "$rng endemic rare log on",
            "$rng endemic rare log off",
            "$rng endemic rare off",
            "$rng steam status",
            "$rng steam help",
            "$rng crit status",
            "$rng crit help",
            "$rng crit rate 100",
            "$rng crit random",
            "$rng crit mv_bank 50",
            "$rng crit mv_tier 50 125 200",
            "$rng crit mv_tier 25 60 delta=-10,0,+25",
            "$rng crit rate auto",
            "$rng crit off",
            "$rng fatalis on",
            "$rng fatalis status",
            "$rng fatalis history",
            "$rng thk status",
            "$rng dump " + dumpEnPath
        };
        for (const std::string& cmd : enCmds) {
            if (!cmd_router::HandleCommandLine(cmd.c_str(), true)) {
                std::printf("FAIL [i18n-cmd]: command failed: %s\n", cmd.c_str());
                ::DeleteFileA(dumpEnPath.c_str());
                host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
                return 1;
            }
        }
        cmd_router::HandleCommandLine("$rng trace off", true);
        cmd_router::HandleCommandLine("$rng trace clear", true);

        for (const std::string& line : g_capturedChatLines) {
            if (containsCjkUtf8(line)) {
                std::printf("FAIL [i18n-NoCJK-Chat]: unexpected CJK text in en mode: %s\n", line.c_str());
                ::DeleteFileA(dumpEnPath.c_str());
                host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
                return 1;
            }
        }
        {
            FILE* dfp = nullptr;
            if (fopen_s(&dfp, dumpEnPath.c_str(), "rb") != 0 || !dfp) {
                std::printf("FAIL [i18n-DumpEn]: failed to open %s\n", dumpEnPath.c_str());
                host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
                return 1;
            }
            std::string body;
            char dbuf[1024]{};
            std::size_t nr = 0U;
            while ((nr = std::fread(dbuf, 1, sizeof(dbuf), dfp)) > 0U) body.append(dbuf, nr);
            std::fclose(dfp);
            ::DeleteFileA(dumpEnPath.c_str());
            if (body.find("Monster crown/size multiplier") == std::string::npos || containsCjkUtf8(body)) {
                std::printf("FAIL [i18n-NoCJK-Dump]: CJK leaked or missing English em.size in dump\n");
                host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
                return 1;
            }
        }

        // Verify dynamic QuerySiteMeta in `en` and immediate restoration in `zh` WITHOUT reloading plugins
        const std::uint32_t probeRvas[8] = {
            0x0169BA15U, // size: em.size
            0x005CA0A2U, // steam: steam.autowin
            0x01D2A3CCU, // endemic: endemic.special_size
            0x01A4ACEDU, // endemic: endemic.rare
            0x00271F25U, // endemic: endemic.rare_moly
            0x01339C8CU, // thk: ai.thk_rand
            0x01F606D6U, // crit: pl.crit_seed
            0x00F99246U  // inspector fallback: meal.trigger
        };
        for (std::uint32_t rva : probeRvas) {
            MhwRngSiteMeta m{};
            host->QuerySiteMeta(rva, &m);
            if (m.description[0] == '\0' || containsCjkUtf8(m.description)) {
                std::printf("FAIL [i18n-MetaEn]: rva=0x%07X desc=%s\n", rva, m.description);
                host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
                return 1;
            }
        }

        cmd_router::HandleCommandLine("$rng lang zh", true);
        for (std::uint32_t rva : probeRvas) {
            MhwRngSiteMeta m{};
            host->QuerySiteMeta(rva, &m);
            if (!containsCjkUtf8(m.description)) {
                std::printf("FAIL [i18n-MetaZh]: rva=0x%07X did not restore Chinese without reload (%s)\n",
                            rva, m.description);
                host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
                return 1;
            }
        }

        cmd_router::HandleCommandLine("$rng fatalis off", true);
        cmd_router::HandleCommandLine("$rng size clear", true);
        cmd_router::HandleCommandLine("$rng endemic clear", true);
        host->ClearAllRules();
        host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
        std::printf("[OK] Standardized i18n API (SDK v3.3), fallback chain, zero-CJK en sweep & hot lang switch verified!\n");
    }

    // ------------------------------------------------------------------------
    // Test 21: In-Game Overlay Terminal Core (Readline UTF-8, History, Dynamic Tab Complete)
    // ------------------------------------------------------------------------
    {
        console::Reset();
        console::Init(&cfg);
        auto combinedSink = +[](const char* utf8Text) noexcept {
            CaptureChatSink(utf8Text);
            console::PushOutputLine(utf8Text);
        };
        cmd_router::Init(&cfg, combinedSink);

        // Toggle open & check UTF-8 codepoint-safe editing
        if (console::IsOpen() || !console::ToggleOpen() || !console::IsOpen()) {
            std::printf("FAIL [console-toggle]: ToggleOpen failed\n");
            return 1;
        }
        console::InsertUtf8(u8"size \u91D1\u72EE\u5B50 max"); // "size 金狮子 max"
        for (int i = 0; i < 4; ++i) console::OnKeyDown(VK_LEFT, false, false, false);
        for (int i = 0; i < 3; ++i) console::OnKeyDown(VK_BACK, false, false, false);
        console::InsertUtf8("rajang");
        console::ConsoleSnapshot snap{};
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "size rajang max") {
            std::printf("FAIL [console-utf8]: got '%s'\n", snap.inputLine.c_str());
            return 1;
        }

        // Test readline Ctrl+U / Ctrl+Y / Ctrl+A / Ctrl+K
        console::OnKeyDown(VK_END, false, false, false);
        console::OnKeyDown('U', true, false, false); // kill whole line
        console::CaptureSnapshot(0U, snap);
        if (!snap.inputLine.empty()) {
            std::printf("FAIL [console-ctrlu]: expected empty, got '%s'\n", snap.inputLine.c_str());
            return 1;
        }
        console::OnKeyDown('Y', true, false, false); // yank back
        console::OnKeyDown('A', true, false, false); // Home
        console::OnKeyDown(VK_RIGHT, true, false, false); // word right -> before "rajang"
        console::OnKeyDown('K', true, false, false); // kill to end -> "size "
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "size ") {
            std::printf("FAIL [console-ctrlk]: got '%s'\n", snap.inputLine.c_str());
            return 1;
        }

        // Dynamic Tab completion: plugin usage keywords + plugin names + monsters + multi-match cycle
        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("steam l");
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "steam lock ") {
            std::printf("FAIL [console-tab-steam]: expected 'steam lock ', got '%s'\n", snap.inputLine.c_str());
            return 1;
        }

        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("reload ins");
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "reload inspector ") {
            std::printf("FAIL [console-tab-reload]: expected 'reload inspector ', got '%s'\n", snap.inputLine.c_str());
            return 1;
        }

        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("size furious");
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "size furious_rajang ") {
            std::printf("FAIL [console-tab-size]: expected 'size furious_rajang ', got '%s'\n", snap.inputLine.c_str());
            return 1;
        }

        // Multi-level sub-sub-command completion across all 5 plugins (SDK v3.4 completer API)
        auto checkTabComplete = [&](const char* input, const char* expectedLine, const char* tag) -> bool {
            console::OnKeyDown('U', true, false, false);
            console::InsertUtf8(input);
            console::OnKeyDown(VK_TAB, false, false, false);
            console::CaptureSnapshot(0U, snap);
            if (snap.inputLine != expectedLine) {
                std::printf("FAIL [console-tab-%s]: input='%s' expected='%s' got='%s'\n",
                            tag, input, expectedLine, snap.inputLine.c_str());
                return false;
            }
            return true;
        };
        if (!checkTabComplete("steam style x", "steam style xbox ", "steam-style") ||
            !checkTabComplete("steam lock wd", "steam lock wda ", "steam-lock") ||
            !checkTabComplete("steam status x", "steam status x", "steam-status-noop") ||
            !checkTabComplete("crit rate a", "crit rate auto ", "crit-rate-auto") ||
            !checkTabComplete("crit ratio 3:2 s", "crit ratio 3:2 spread ", "crit-ratio-spread") ||
            !checkTabComplete("crit mode mv_b", "crit mode mv_bank ", "crit-mode-mvbank") ||
            !checkTabComplete("crit mv_bank 50 1", "crit mv_bank 50 1.0 ", "crit-mvbank-res") ||
            !checkTabComplete("crit verbose o", "crit verbose off", "crit-verbose-cycle1") ||
            !checkTabComplete("fatalis far m", "fatalis far multi ", "fatalis-far") ||
            !checkTabComplete("thk probe l", "thk probe log ", "thk-probe") ||
            !checkTabComplete("thk rule a", "thk rule add ", "thk-rule") ||
            !checkTabComplete("thk preset load f", "thk preset load fatalis_p1_nasty ", "thk-preset-load") ||
            !checkTabComplete("size rajang e", "size rajang exact ", "size-sub") ||
            !checkTabComplete("size min e", "size min e", "size-min-noop") ||
            !checkTabComplete("endemic ec059 n", "endemic ec059 normal ", "endemic-sub") ||
            !checkTabComplete("endemic min n", "endemic min n", "endemic-min-noop") ||
            !checkTabComplete("endemic r", "endemic rare ", "endemic-rare") ||
            !checkTabComplete("endemic rare l", "endemic rare log ", "endemic-rare-log") ||
            !checkTabComplete("endemic rare ec058 b", "endemic rare ec058 block ", "endemic-rare-sp-block") ||
            !checkTabComplete("endemic ec058 r", "endemic ec058 rare ", "endemic-sp-rare") ||
            !checkTabComplete("endemic ec058 rare b", "endemic ec058 rare block ", "endemic-sp-rare-block") ||
            !checkTabComplete(u8"endemic rare \u6708\u7FBD", u8"endemic rare \u6708\u7FBD\u5929\u6BCD ", "endemic-rare-zhname") ||
            !checkTabComplete("endemic rare wintermoon", "endemic rare wintermoon_nettle ", "endemic-rare-slug") ||
            !checkTabComplete("endemic rocky_m", "endemic rocky_moly ", "endemic-size-slug") ||
            !checkTabComplete("fix em.size r", "fix em.size raw:", "fix-raw") ||
            !checkTabComplete("seq em.size 10,20 h", "seq em.size 10,20 hold ", "seq-pol") ||
            !checkTabComplete("trace dum", "trace dump ", "trace-dump") ||
            !checkTabComplete("trace log d", "trace log d", "trace-log-noop")) {
            return 1;
        }
        // `endemic rare <Tab>`, `endemic <Tab>`, and `endemic clear <Tab>` with empty prefixes MUST list species names
        {
            std::vector<std::string> rareCands;
            plugin_host::CompletePluginSubcommand("endemic", {"rare"}, "", rareCands);
            std::size_t nameCnt = 0U;
            bool sawWintermoon = false, sawGreatFish = false, sawCommon = false, sawSlug = false;
            for (const auto& c : rareCands) {
                if (c == u8"\u6708\u7FBD\u5929\u6BCD") sawWintermoon = true;             // 月羽天母 (ec058_00, rare)
                if (c == u8"\u7C89\u8272\u5F02\u5F62\u9C7C") sawGreatFish = true;        // 粉色异形鱼 (ec011_00, Great-size fish)
                if (c == u8"\u7ED5\u884C\u5154") sawCommon = true;                       // 绕行兔 (ec001_00, common)
                if (c == "wintermoon_nettle") sawSlug = true;
                if (!c.empty() && static_cast<unsigned char>(c[0]) >= 0x80U) ++nameCnt;
            }
            if (!sawWintermoon || !sawGreatFish || sawCommon || sawSlug || nameCnt < 30U) {
                std::printf("FAIL [console-tab-endemic-rare-list]: names=%zu wintermoon=%d greatFish=%d common=%d slug=%d\n",
                            nameCnt, sawWintermoon, sawGreatFish, sawCommon, sawSlug);
                return 1;
            }

            std::vector<std::string> allCands;
            plugin_host::CompletePluginSubcommand("endemic", {}, "", allCands);
            std::size_t allNameCnt = 0U;
            bool allRare = false, allCommon = false, allSlug = false;
            for (const auto& c : allCands) {
                if (c == u8"\u6708\u7FBD\u5929\u6BCD") allRare = true;
                if (c == u8"\u7ED5\u884C\u5154") allCommon = true;
                if (c == "wintermoon_nettle") allSlug = true;
                if (!c.empty() && static_cast<unsigned char>(c[0]) >= 0x80U) ++allNameCnt;
            }
            if (!allRare || !allCommon || allSlug || allNameCnt < 90U) {
                std::printf("FAIL [console-tab-endemic-all-list]: names=%zu rare=%d common=%d slug=%d\n",
                            allNameCnt, allRare, allCommon, allSlug);
                return 1;
            }

            std::vector<std::string> clearCands;
            plugin_host::CompletePluginSubcommand("endemic", {"clear"}, "", clearCands);
            std::size_t clrNameCnt = 0U;
            bool clrRare = false, clrCommon = false;
            for (const auto& c : clearCands) {
                if (c == u8"\u6708\u7FBD\u5929\u6BCD") clrRare = true;
                if (c == u8"\u7ED5\u884C\u5154") clrCommon = true;
                if (!c.empty() && static_cast<unsigned char>(c[0]) >= 0x80U) ++clrNameCnt;
            }
            if (!clrRare || !clrCommon || clrNameCnt < 90U) {
                std::printf("FAIL [console-tab-endemic-clear-list]: names=%zu rare=%d common=%d\n",
                            clrNameCnt, clrRare, clrCommon);
                return 1;
            }
        }
        // Ambiguous sub-sub-command: `thk preset tag bipedal_lock ` -> LCP `'o'`, then cycles `off` -> `on`
        if (!checkTabComplete("thk preset tag bipedal_lock ", "thk preset tag bipedal_lock o", "thk-tag-lcp")) {
            return 1;
        }
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "thk preset tag bipedal_lock off") {
            std::printf("FAIL [console-tab-thk-tag-off]: expected '... off', got '%s'\n", snap.inputLine.c_str());
            return 1;
        }
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "thk preset tag bipedal_lock on") {
            std::printf("FAIL [console-tab-thk-tag-cycle]: expected '... on', got '%s'\n", snap.inputLine.c_str());
            return 1;
        }

        // Ambiguous prefix "st" -> "status" <-> "steam" cycling via Tab / Shift+Tab
        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("st");
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        const std::string firstCand = snap.inputLine;
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        const std::string secondCand = snap.inputLine;
        console::OnKeyDown(VK_TAB, false, true, false); // Shift+Tab cycles back
        console::CaptureSnapshot(0U, snap);
        if ((firstCand != "status" && firstCand != "steam") ||
            secondCand == firstCand ||
            snap.inputLine != firstCand ||
            snap.completionHint.empty()) {
            std::printf("FAIL [console-tab-cycle]: c1='%s' c2='%s' back='%s' hint='%s'\n",
                        firstCand.c_str(), secondCand.c_str(), snap.inputLine.c_str(), snap.completionHint.c_str());
            return 1;
        }

        // Command submission (prefixless) -> pendingCmds -> HandleCommandLine(..., false) -> scrollback output
        auto runConsoleLine = [&](const char* cmdText) -> bool {
            console::OnKeyDown('U', true, false, false);
            console::InsertUtf8(cmdText);
            console::OnKeyDown(VK_RETURN, false, false, false);
            std::string pending;
            if (!console::PopPendingCommand(pending)) return false;
            return cmd_router::HandleCommandLine(pending.c_str(), false);
        };
        if (!runConsoleLine("chat on") || !cfg.showChatFeedback ||
            !runConsoleLine("chat off") || cfg.showChatFeedback ||
            !runConsoleLine("status")) {
            std::printf("FAIL [console-exec]: command execution via console failed\n");
            return 1;
        }

        // History Up/Down preserving draft & Ctrl+R reverse search
        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("draft_text");
        console::OnKeyDown(VK_UP, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "status") {
            std::printf("FAIL [console-hist-up]: expected 'status', got '%s'\n", snap.inputLine.c_str());
            return 1;
        }
        console::OnKeyDown(VK_DOWN, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "draft_text") {
            std::printf("FAIL [console-hist-draft]: expected 'draft_text', got '%s'\n", snap.inputLine.c_str());
            return 1;
        }
        console::OnKeyDown('R', true, false, false); // Ctrl+R
        console::InsertUtf8("chat o");
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "chat off" || snap.searchPrompt.empty()) {
            std::printf("FAIL [console-ctrlr]: got '%s' prompt='%s'\n",
                        snap.inputLine.c_str(), snap.searchPrompt.c_str());
            return 1;
        }
        if (snap.lines.size() < 4U) {
            std::printf("FAIL [console-scrollback]: expected >=4 output lines, got %zu\n", snap.lines.size());
            return 1;
        }
        console::OnKeyDown(VK_ESCAPE, false, false, false); // exit Ctrl+R
        if (!runConsoleLine("?") || !runConsoleLine("help plugins")) {
            std::printf("FAIL [console-help]: help execution via console failed\n");
            return 1;
        }
        {
            console::ConsoleSnapshot helpSnap{};
            console::CaptureSnapshot(32U, helpSnap);
            bool sawInspectorGroup = false;
            bool sawThkGroup       = false;
            bool sawPluginsHelp    = false;
            for (const auto& l : helpSnap.lines) {
                if (l.text.find("[inspector]") != std::string::npos) sawInspectorGroup = true;
                if (l.text.find("[thk]") != std::string::npos) sawThkGroup = true;
                if (l.text.find("$rng plugins") != std::string::npos) sawPluginsHelp = true;
            }
            if (!sawInspectorGroup || !sawThkGroup || !sawPluginsHelp) {
                std::printf("FAIL [console-help]: insp=%d thk=%d plugins=%d\n",
                            sawInspectorGroup ? 1 : 0, sawThkGroup ? 1 : 0, sawPluginsHelp ? 1 : 0);
                return 1;
            }
        }
        // Verify scroll-to-top never hides bottom lines and ClampScrollOffset keeps ScrollBy(-N) responsive
        console::ScrollBy(500);
        {
            console::ConsoleSnapshot topSnap{};
            console::CaptureSnapshot(6U, topSnap);
            if (topSnap.totalLines >= 6U && topSnap.lines.size() != 6U) {
                std::printf("FAIL [console-scroll-top]: expected 6 visible lines, got %zu (total=%zu)\n",
                            topSnap.lines.size(), topSnap.totalLines);
                return 1;
            }
            console::ClampScrollOffset(topSnap.totalLines - 6U);
            console::ScrollBy(-2);
            console::ConsoleSnapshot downSnap{};
            console::CaptureSnapshot(6U, downSnap);
            if (downSnap.scrollOffset != topSnap.totalLines - 8U || downSnap.lines.size() != 6U) {
                std::printf("FAIL [console-scroll-clamp]: offset=%zu lines=%zu\n",
                            downSnap.scrollOffset, downSnap.lines.size());
                return 1;
            }
        }
        // Console IME composition, candidate list, UTF-16 surrogate pair, and CJK Ctrl+R reverse search
        {
            console::OnKeyDown('U', true, false, false);
            console::SetImeCompositionUtf16(L"yueyu", 5, 3);
            console::CaptureSnapshot(0U, snap);
            if (!console::HasImeComposition() || snap.imeCompText != "yueyu" || snap.imeCompCursorByte != 3U) {
                std::printf("FAIL [console-ime-comp-ascii]: has=%d text='%s' cur=%zu\n",
                            console::HasImeComposition() ? 1 : 0, snap.imeCompText.c_str(), snap.imeCompCursorByte);
                return 1;
            }

            console::SetImeCompositionUtf16(L"\u6708\u7FBD\u5929\u6BCD", 4, 2);
            console::CaptureSnapshot(0U, snap);
            if (snap.imeCompText != u8"\u6708\u7FBD\u5929\u6BCD" || snap.imeCompCursorByte != 6U) {
                std::printf("FAIL [console-ime-comp-cjk]: text='%s' cur=%zu\n",
                            snap.imeCompText.c_str(), snap.imeCompCursorByte);
                return 1;
            }

            console::SetImeCandidatesUtf16({L"\u6708\u7FBD\u5929\u6BCD", L"\u6708\u5149\u58C1\u864E"}, 1U, 0U, 14U);
            console::CaptureSnapshot(0U, snap);
            if (!console::HasImeComposition() || snap.imeCandidates.size() != 2U ||
                snap.imeCandSelected != 1U || snap.imeCandTotal != 14U) {
                std::printf("FAIL [console-ime-cands]: has=%d n=%zu sel=%zu tot=%zu\n",
                            console::HasImeComposition() ? 1 : 0, snap.imeCandidates.size(),
                            snap.imeCandSelected, snap.imeCandTotal);
                return 1;
            }

            console::ClearImeCandidates();
            console::CaptureSnapshot(0U, snap);
            if (!console::HasImeComposition() || !snap.imeCandidates.empty() || snap.imeCompText.empty()) {
                std::printf("FAIL [console-ime-clear-cands]: has=%d cands=%zu comp='%s'\n",
                            console::HasImeComposition() ? 1 : 0, snap.imeCandidates.size(), snap.imeCompText.c_str());
                return 1;
            }

            console::ClearImeComposition();
            console::CaptureSnapshot(0U, snap);
            if (console::HasImeComposition() || !snap.imeCompText.empty() || !snap.imeCandidates.empty()) {
                std::printf("FAIL [console-ime-clear-all]: has=%d\n", console::HasImeComposition() ? 1 : 0);
                return 1;
            }

            // Closing console clears active IME state automatically
            console::SetImeCompositionUtf16(L"test", 4, 2);
            console::SetImeCandidatesUtf16({L"\u6D4B\u8BD5"}, 0U, 0U, 1U);
            console::SetOpen(false);
            console::CaptureSnapshot(0U, snap);
            if (console::HasImeComposition() || !snap.imeCompText.empty() || !snap.imeCandidates.empty()) {
                std::printf("FAIL [console-ime-close-clear]: IME state survived SetOpen(false)\n");
                return 1;
            }
            console::SetOpen(true);

            // Surrogate pair UTF-16 insertion (`U+1F409` dragon emoji = `\xF0\x9F\x90\x89`)
            console::OnKeyDown('U', true, false, false);
            const wchar_t kDragonSurrogate[2] = { static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDC09) };
            console::InsertUtf16(kDragonSurrogate, 2);
            console::CaptureSnapshot(0U, snap);
            if (snap.inputLine != "\xF0\x9F\x90\x89") {
                std::printf("FAIL [console-ime-surrogate]: unexpected UTF-8 output (len=%zu)\n", snap.inputLine.size());
                return 1;
            }

            // Reverse-i-search with CJK UTF-8 history & CJK search query
            if (!runConsoleLine(u8"endemic rare \u6708\u7FBD\u5929\u6BCD status")) {
                std::printf("FAIL [console-ime-cjk-history-exec]\n");
                return 1;
            }
            console::OnKeyDown('U', true, false, false);
            console::OnKeyDown('R', true, false, false);
            console::InsertUtf8(u8"\u6708\u7FBD");
            console::CaptureSnapshot(0U, snap);
            if (snap.inputLine != u8"endemic rare \u6708\u7FBD\u5929\u6BCD status" || snap.searchPrompt.empty()) {
                std::printf("FAIL [console-ime-cjk-ctrlr]: got '%s'\n", snap.inputLine.c_str());
                return 1;
            }
            console::OnKeyDown(VK_ESCAPE, false, false, false); // exit Ctrl+R
        }

        console::OnKeyDown(VK_ESCAPE, false, false, false); // close console
        if (console::IsOpen()) {
            std::printf("FAIL [console-esc]: Esc did not close console\n");
            return 1;
        }
        cmd_router::Init(&cfg, &CaptureChatSink);
        std::printf("[OK] Terminal core (UTF-8/IME readline, history, Ctrl+R, dynamic plugin Tab completion, chat echo toggle) verified!\n");
    }

    // ------------------------------------------------------------------------
    // Test 22: Verify Official Plugin #6 (`rng_crit.dll`) on Real MHW 15.23 `0x141f51380` & `0x141f605f0`
    // ------------------------------------------------------------------------
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        static constexpr std::uint8_t kCritOrig5[5] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
        if (img[0x01F51380U] != 0xE9U) {
            std::printf("FAIL [crit-hook]: Expected 0xE9 detour at 0x01F51380, got 0x%02X\n", img[0x01F51380U]);
            return 1;
        }

        // Preserve & populate synthetic global pointers referenced by `0x1411c0940` / `0x1411c0620`
        const std::uint32_t savedDefFloatBits = *reinterpret_cast<const std::uint32_t*>(base + 0x2E4FE58U);
        void* const savedCritMultTblPtr       = *reinterpret_cast<void* const*>(base + 0x5011F58U);
        const std::uint32_t savedSInst[4]     = { sInst[0], sInst[1], sInst[2], sInst[3] };

        alignas(16) static std::uint8_t fakeCritTbl[0x800]{};
        std::memset(fakeCritTbl, 0, sizeof(fakeCritTbl));
        *reinterpret_cast<float*>(fakeCritTbl + 0x7A8U) = 1.25f; // Positive crit base mult
        *reinterpret_cast<float*>(fakeCritTbl + 0x7ACU) = 0.75f; // Negative crit base mult
        *reinterpret_cast<float*>(base + 0x2E4FE58U)    = 1.0f;  // Default float arg for getSkillParam<float>
        *reinterpret_cast<void**>(base + 0x5011F58U)    = fakeCritTbl;
        sInst[0] = 0x12345678U;
        sInst[1] = 0x9ABCDEF0U;
        sInst[2] = 0x13579BDFU;
        sInst[3] = 0x2468ACE1U;

        alignas(16) static std::uint8_t fakeLocalPlayer[0xE200]{};
        alignas(16) static std::uint8_t fakeRemotePlayer[0xE200]{};
        alignas(16) static std::uint8_t fakeStatusMgr[0xA00]{};
        std::memset(fakeLocalPlayer,  0, sizeof(fakeLocalPlayer));
        std::memset(fakeRemotePlayer, 0, sizeof(fakeRemotePlayer));
        std::memset(fakeStatusMgr,    0, sizeof(fakeStatusMgr));

        *reinterpret_cast<void**>(fakeLocalPlayer + 0x7D20U)        = fakeStatusMgr;
        *reinterpret_cast<std::uint8_t*>(fakeLocalPlayer + 0x8938U) = 1U; // Local player bit
        *reinterpret_cast<void**>(fakeRemotePlayer + 0x7D20U)       = fakeStatusMgr;
        *reinterpret_cast<std::uint8_t*>(fakeRemotePlayer + 0x8938U)= 0U; // Remote coop replica
        *reinterpret_cast<void**>(fakeStatusMgr + 0x08U)            = fakeLocalPlayer;
        *reinterpret_cast<std::uint8_t*>(fakeStatusMgr + 0x9BAU)    = 1U; // Fast-path live affinity calc

        alignas(16) std::uint8_t fakeOrigAtkParam[0x280]{};
        alignas(16) std::uint8_t fakeAtkNode[0x100]{};
        alignas(16) std::uint8_t fakeHitInfo[0x100]{};
        std::memset(fakeOrigAtkParam, 0, sizeof(fakeOrigAtkParam));
        std::memset(fakeAtkNode,      0, sizeof(fakeAtkNode));
        std::memset(fakeHitInfo,      0, sizeof(fakeHitInfo));
        *reinterpret_cast<void**>(fakeAtkNode + 0xA8U) = fakeOrigAtkParam;
        *reinterpret_cast<void**>(fakeHitInfo + 0x38U) = fakeAtkNode;

        void* nullHitInfo = nullptr;
        void* localHitInfo = fakeHitInfo;
        struct { void** hitInfoPtr; void* player; } ctxLocal{ &localHitInfo, fakeLocalPlayer };
        struct { void** hitInfoPtr; void* player; } ctxRemote{ &nullHitInfo, fakeRemotePlayer };
        alignas(16) std::uint8_t fakeAtkParam[0x200]{};

        using ApplyCritFn = void(__fastcall*)(void* critCtx, void* atkParam,
                                              std::uint8_t isWeakPoint, std::uint8_t force100Crit,
                                              std::uint8_t isTenderized, std::uint8_t extraFlag);
        const auto fnApplyCrit = reinterpret_cast<ApplyCritFn>(base + 0x01F51380U);

        // Realistic separation: `origAtkParam+0x20` holds true MV (e.g. 22 / 264), while mutable
        // `atkParam+0x20` holds pre-crit scaled raw damage (e.g. 4.0x larger = 88 / 1056)!
        auto runHit = [&](void* critCtx, float trueMv, std::uint8_t force100, std::int8_t& outRes, float& outScaledDmg) {
            std::memset(fakeAtkParam, 0, sizeof(fakeAtkParam));
            *reinterpret_cast<float*>(fakeOrigAtkParam + 0x20U) = trueMv;
            *reinterpret_cast<float*>(fakeAtkParam + 0x20U)     = trueMv * 4.0f;
            fnApplyCrit(critCtx, fakeAtkParam, 0U, force100, 0U, 0U);
            outRes       = *reinterpret_cast<const std::int8_t*>(fakeAtkParam + 0x160U);
            outScaledDmg = *reinterpret_cast<const float*>(fakeAtkParam + 0x20U);
        };

        auto restoreCritTestMem = [&]() {
            *reinterpret_cast<std::uint32_t*>(base + 0x2E4FE58U) = savedDefFloatBits;
            *reinterpret_cast<void**>(base + 0x5011F58U)         = savedCritMultTblPtr;
            std::memcpy(sInst, savedSInst, sizeof(savedSInst));
        };

        host->SetEngineMode(kMhwRngModeSelective);
        host->SetMasterSeed(777ULL, true);
        plugin_host::DispatchPluginSubcommand("crit", {"reset"}, "reset");
        plugin_host::DispatchPluginSubcommand("crit", {"random"}, "random");
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "0"}, "rate 0");

        // (a) Remote coop replica MUST bypass `rng_crit` and run pure vanilla (`force100=1` -> +1 crit, MV x1.25)
        std::int8_t remRes = 0;
        float       remMv  = 0.0f;
        runHit(&ctxRemote, 100.0f, 1U, remRes, remMv);
        g_capturedChatLines.clear();
        plugin_host::DispatchPluginSubcommand("crit", {"status"}, "status");
        const bool remoteIgnored = (!g_capturedChatLines.empty() &&
                                    g_capturedChatLines.back().find("MV=") == std::string::npos); // Only verify via stats line:
        bool statsZeroHits = false;
        for (const auto& l : g_capturedChatLines) {
            if (l.find(u8"总命中=0") != std::string::npos || l.find("hits=0") != std::string::npos) {
                statsZeroHits = true;
            }
        }
        (void)remoteIgnored;
        if (remRes != 1 || std::fabs(remMv - 500.0f) > 0.01f || !statsZeroHits) {
            restoreCritTestMem();
            std::printf("FAIL [crit-remote]: res=%d mv=%.2f statsZero=%d\n", remRes, remMv, statsZeroHits ? 1 : 0);
            return 1;
        }

        // (b) Action-start seed hook (`RVA 0x01F606D6`): captures `DrawDeterministic` and packs (S<<27)|S
        {
            static constexpr std::uint8_t kCritSeedThunk[] = {
                0x53,                   // push rbx
                0x48, 0x83, 0xEC, 0x28, // sub rsp, 28h
                0x48, 0x8B, 0xD9,       // mov rbx, rcx
                0xFF, 0xD2,             // call rdx
                0x48, 0x83, 0xC4, 0x28, // add rsp, 28h
                0x5B,                   // pop rbx
                0xC3                    // ret
            };
            std::memcpy(thunkMem + 0x240U, kCritSeedThunk, sizeof(kCritSeedThunk));
            ::FlushInstructionCache(::GetCurrentProcess(), thunkMem + 0x240U, sizeof(kCritSeedThunk));
            using SeedCallFn = void(__fastcall*)(void* player, void* addr);
            const auto fnSeedCall = reinterpret_cast<SeedCallFn>(thunkMem + 0x240U);

            const std::uint8_t origSeedEnd = img[0x01F606EFU];
            img[0x01F606EFU] = 0xC3;
            ::FlushInstructionCache(::GetCurrentProcess(), img + 0x01F606EFU, 1);
            fnSeedCall(fakeLocalPlayer, img + 0x01F606C1U);
            img[0x01F606EFU] = origSeedEnd;
            ::FlushInstructionCache(::GetCurrentProcess(), img + 0x01F606EFU, 1);

            const std::uint32_t s27 = *reinterpret_cast<const std::uint32_t*>(fakeLocalPlayer + 0x8818U);
            const std::uint32_t s32 = *reinterpret_cast<const std::uint32_t*>(fakeLocalPlayer + 0x881CU);
            if (((s27 << 27) | (s27 & 0x07FFFFFFU)) != s32) {
                restoreCritTestMem();
                std::printf("FAIL [crit-seed27]: s27=0x%X s32=0x%X\n", s27, s32);
                return 1;
            }
        }

        // (c) `rate +100 / 0 / -100` overrides + `rate auto` (`force100Crit=1`)
        //     `trueMv=100 -> preCritDmg=400`: verifies mutable dmg gets multiplied (`500/400/300`) while `origAtkParam+0x20` stays `100`!
        std::int8_t res = 0;
        float       mv  = 0.0f;
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "100"}, "rate 100");
        runHit(&ctxLocal, 100.0f, 0U, res, mv);
        if (res != 1 || std::fabs(mv - 500.0f) > 0.01f ||
            std::fabs(*reinterpret_cast<const float*>(fakeOrigAtkParam + 0x20U) - 100.0f) > 0.01f) {
            restoreCritTestMem();
            std::printf("FAIL [crit-rate100]: res=%d mv=%.2f\n", res, mv);
            return 1;
        }
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "0"}, "rate 0");
        runHit(&ctxLocal, 100.0f, 1U, res, mv); // Override beats force100!
        if (res != 0 || std::fabs(mv - 400.0f) > 0.01f) {
            restoreCritTestMem();
            std::printf("FAIL [crit-rate0]: res=%d mv=%.2f\n", res, mv);
            return 1;
        }
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "-100"}, "rate -100");
        runHit(&ctxLocal, 100.0f, 0U, res, mv);
        if (res != -1 || std::fabs(mv - 300.0f) > 0.01f) {
            restoreCritTestMem();
            std::printf("FAIL [crit-rate-100]: res=%d mv=%.2f\n", res, mv);
            return 1;
        }
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "auto"}, "rate auto");
        runHit(&ctxLocal, 100.0f, 1U, res, mv);
        if (res != 1 || std::fabs(mv - 500.0f) > 0.01f) {
            restoreCritTestMem();
            std::printf("FAIL [crit-auto]: res=%d mv=%.2f\n", res, mv);
            return 1;
        }

        // (d) `random` determinism across `ResetEpoch()` + active override reminder check
        g_capturedChatLines.clear();
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "100"}, "rate 100 reminder");
        plugin_host::DispatchPluginSubcommand("crit", {"random"}, "random reminder");
        bool sawRateReminder = false;
        for (const auto& l : g_capturedChatLines) {
            if (l.find("rate auto") != std::string::npos) sawRateReminder = true;
        }
        if (!sawRateReminder) {
            restoreCritTestMem();
            std::printf("FAIL [crit-rate-reminder]: expected rate auto reminder on mode switch while rate override active\n");
            return 1;
        }
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "50"}, "rate 50");
        host->ResetEpoch();
        std::int8_t randSeq1[64]{}, randSeq2[64]{};
        int randCrits = 0;
        for (int i = 0; i < 64; ++i) {
            runHit(&ctxLocal, 40.0f, 0U, randSeq1[i], mv);
            if (randSeq1[i] > 0) ++randCrits;
        }
        host->ResetEpoch();
        for (int i = 0; i < 64; ++i) {
            runHit(&ctxLocal, 40.0f, 0U, randSeq2[i], mv);
        }
        if (std::memcmp(randSeq1, randSeq2, sizeof(randSeq1)) != 0 || randCrits < 15 || randCrits > 49) {
            restoreCritTestMem();
            std::printf("FAIL [crit-random]: match=%d crits=%d/64\n",
                        std::memcmp(randSeq1, randSeq2, sizeof(randSeq1)) == 0 ? 1 : 0, randCrits);
            return 1;
        }

        // (e) `ratio 3:2 block`, `ratio 3:2 spread`, and custom `seq 10-1` (plus step-format verbose check)
        plugin_host::DispatchPluginSubcommand("crit", {"ratio", "3:2", "block"}, "ratio 3:2 block");
        static constexpr std::int8_t kExpBlock[10]  = { 1, 1, 1, 0, 0, 1, 1, 1, 0, 0 };
        static constexpr std::int8_t kExpSpread[10] = { 0, 1, 0, 1, 1, 0, 1, 0, 1, 1 };
        static constexpr std::int8_t kExpCustom[8]  = { 1, 0, -1, 1, 1, 0, -1, 1 };
        for (int i = 0; i < 10; ++i) {
            runHit(&ctxLocal, 50.0f, 0U, res, mv);
            if (res != kExpBlock[i]) {
                restoreCritTestMem();
                std::printf("FAIL [crit-ratio-block]: idx=%d exp=%d got=%d\n", i, kExpBlock[i], res);
                return 1;
            }
        }
        plugin_host::DispatchPluginSubcommand("crit", {"ratio", "3:2", "spread"}, "ratio 3:2 spread");
        for (int i = 0; i < 10; ++i) {
            runHit(&ctxLocal, 50.0f, 0U, res, mv);
            if (res != kExpSpread[i]) {
                restoreCritTestMem();
                std::printf("FAIL [crit-ratio-spread]: idx=%d exp=%d got=%d\n", i, kExpSpread[i], res);
                return 1;
            }
        }
        plugin_host::DispatchPluginSubcommand("crit", {"verbose", "on"}, "verbose on seq");
        g_capturedAsyncChatLines.clear();
        plugin_host::DispatchPluginSubcommand("crit", {"seq", "10-1"}, "seq 10-1");
        for (int i = 0; i < 8; ++i) {
            runHit(&ctxLocal, 50.0f, 0U, res, mv);
            if (res != kExpCustom[i]) {
                restoreCritTestMem();
                std::printf("FAIL [crit-seq]: idx=%d exp=%d got=%d\n", i, kExpCustom[i], res);
                return 1;
            }
        }
        plugin_host::DispatchPluginSubcommand("crit", {"verbose", "off"}, "verbose off seq");
        {
            const bool seqCntOk = (g_capturedAsyncChatLines.size() == 8U);
            const bool seqTagOk = !g_capturedAsyncChatLines.empty() &&
                                  (g_capturedAsyncChatLines[0].find(u8"[序列 1/4]") != std::string::npos ||
                                   g_capturedAsyncChatLines[0].find("[Seq 1/4]") != std::string::npos);
            bool noAffInSeq = true;
            for (const auto& l : g_capturedAsyncChatLines) {
                if (l.find(u8"会心=") != std::string::npos || l.find("Aff=") != std::string::npos) {
                    noAffInSeq = false;
                }
            }
            if (!seqCntOk || !seqTagOk || !noAffInSeq) {
                restoreCritTestMem();
                std::printf("FAIL [crit-seq-verbose]: cnt=%zu tagOk=%d noAff=%d first='%s'\n",
                            g_capturedAsyncChatLines.size(), seqTagOk ? 1 : 0, noAffInSeq ? 1 : 0,
                            g_capturedAsyncChatLines.empty() ? "" : g_capturedAsyncChatLines[0].c_str());
                return 1;
            }
        }

        // (f) `mv_bank 50 1.0` at 50%: TCS pairs `(22.0, 264.0)` -> 100% high-MV crit & 50% overall conservation
        g_capturedChatLines.clear();
        plugin_host::DispatchPluginSubcommand("crit", {"mv_bank", "50", "1.0"}, "mv_bank 50 1.0");
        {
            bool sawOverflowThresh = false;
            for (const auto& l : g_capturedChatLines) {
                if (l.find("2.00") != std::string::npos) sawOverflowThresh = true;
            }
            if (!sawOverflowThresh) {
                restoreCritTestMem();
                std::printf("FAIL [crit-mvbank-copy]: expected 2.00 overflow threshold in mv_bank confirmation\n");
                return 1;
            }
        }
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "50"}, "rate 50");
        int highCrits = 0, lowCrits = 0;
        for (int pair = 0; pair < 40; ++pair) {
            runHit(&ctxLocal, 22.0f, 0U, res, mv);
            if (res > 0) ++lowCrits;
            runHit(&ctxLocal, 264.0f, 0U, res, mv);
            if (res > 0) ++highCrits;
        }
        if (highCrits != 40 || lowCrits != 0) {
            restoreCritTestMem();
            std::printf("FAIL [crit-mvbank-tcs]: high=%d/40 low=%d/40\n", highCrits, lowCrits);
            return 1;
        }
        plugin_host::DispatchPluginSubcommand("crit", {"reset"}, "reset bank");
        int lowOnlyCrits = 0;
        for (int i = 0; i < 100; ++i) {
            runHit(&ctxLocal, 15.0f, 0U, res, mv);
            if (res > 0) ++lowOnlyCrits;
        }
        if (lowOnlyCrits < 48 || lowOnlyCrits > 51) {
            restoreCritTestMem();
            std::printf("FAIL [crit-mvbank-low]: crits=%d/100\n", lowOnlyCrits);
            return 1;
        }

        // (g) `mv_tier` multi-cutoff (zero hidden delta), strict validation (no silent clamp!), `delta=`, and `rate=`
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "50"}, "rate 50 tier");
        plugin_host::DispatchPluginSubcommand("crit", {"mv_tier", "50", "125", "200"}, "mv_tier 50 125 200");
        // Verify invalid `delta=0,0,0,200` and count mismatch fail cleanly WITHOUT mutating active tier streams
        plugin_host::DispatchPluginSubcommand("crit", {"mv_tier", "50", "125", "200", "delta=0,0,0,200"}, "mv_tier bad range");
        plugin_host::DispatchPluginSubcommand("crit", {"mv_tier", "50", "125", "200", "delta=-10,10"}, "mv_tier bad count");
        int t4Low = 0, t4High = 0;
        for (int i = 0; i < 30; ++i) {
            runHit(&ctxLocal, 22.0f,  0U, res, mv); if (res > 0) ++t4Low;
            runHit(&ctxLocal, 264.0f, 0U, res, mv); if (res > 0) ++t4High;
        }
        if (t4Low == 0 || t4Low == 30 || t4High == 0 || t4High == 30) {
            restoreCritTestMem();
            std::printf("FAIL [crit-mvtier-4cuts]: unexpected forced crit/miss (lo=%d/30 hi=%d/30)\n", t4Low, t4High);
            return 1;
        }

        plugin_host::DispatchPluginSubcommand("crit", {"mv_tier", "25", "60", "delta=-50,0,50"}, "mv_tier delta");
        int tierLow = 0, tierMid = 0, tierHigh = 0;
        for (int i = 0; i < 20; ++i) {
            runHit(&ctxLocal, 15.0f, 0U, res, mv); if (res > 0) ++tierLow;
            runHit(&ctxLocal, 80.0f, 0U, res, mv); if (res > 0) ++tierHigh;
            runHit(&ctxLocal, 40.0f, 0U, res, mv); if (res > 0) ++tierMid;
        }
        if (tierLow != 0 || tierHigh != 20 || tierMid < 4 || tierMid > 16) {
            restoreCritTestMem();
            std::printf("FAIL [crit-mvtier]: lo=%d mid=%d hi=%d\n", tierLow, tierMid, tierHigh);
            return 1;
        }

        plugin_host::DispatchPluginSubcommand("crit", {"mv_tier", "50", "125", "200", "rate=0,100,0,100"}, "mv_tier rate");
        int r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        for (int i = 0; i < 10; ++i) {
            runHit(&ctxLocal, 22.0f,  0U, res, mv); if (res > 0) ++r0;
            runHit(&ctxLocal, 82.0f,  0U, res, mv); if (res > 0) ++r1;
            runHit(&ctxLocal, 150.0f, 0U, res, mv); if (res > 0) ++r2;
            runHit(&ctxLocal, 264.0f, 0U, res, mv); if (res > 0) ++r3;
        }
        if (r0 != 0 || r1 != 10 || r2 != 0 || r3 != 10) {
            restoreCritTestMem();
            std::printf("FAIL [crit-mvtier-rate]: r=%d,%d,%d,%d\n", r0, r1, r2, r3);
            return 1;
        }

        // (h) `bag 10` at 35% over 100 hits -> exact 35 crits (fractional carry conservation)
        plugin_host::DispatchPluginSubcommand("crit", {"bag", "10"}, "bag 10");
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "35"}, "rate 35");
        int bagCrits = 0;
        for (int i = 0; i < 100; ++i) {
            runHit(&ctxLocal, 40.0f, 0U, res, mv);
            if (res > 0) ++bagCrits;
        }
        if (bagCrits != 35) {
            restoreCritTestMem();
            std::printf("FAIL [crit-bag]: crits=%d/100 (expected exact 35)\n", bagCrits);
            return 1;
        }

        // (i) `prd` at 25% over 1000 hits -> ~25% mean AND bounded max miss streak (<= 12)
        plugin_host::DispatchPluginSubcommand("crit", {"prd"}, "prd");
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "25"}, "rate 25");
        int prdCrits = 0, curMiss = 0, maxMiss = 0;
        for (int i = 0; i < 1000; ++i) {
            runHit(&ctxLocal, 40.0f, 0U, res, mv);
            if (res > 0) {
                ++prdCrits;
                curMiss = 0;
            } else {
                ++curMiss;
                if (curMiss > maxMiss) maxMiss = curMiss;
            }
        }
        if (prdCrits < 210 || prdCrits > 290 || maxMiss > 12) {
            restoreCritTestMem();
            std::printf("FAIL [crit-prd]: crits=%d/1000 maxMiss=%d\n", prdCrits, maxMiss);
            return 1;
        }

        // (j) Hot-unload restores exact 5-byte prologue & hot-reload re-hooks cleanly
        std::string critMsg;
        if (!plugin_host::UnloadSingle("crit", critMsg) ||
            std::memcmp(img + 0x01F51380U, kCritOrig5, 5) != 0) {
            restoreCritTestMem();
            std::printf("FAIL [crit-unload]: prologue at 0x01F51380 not restored (%s)\n", critMsg.c_str());
            return 1;
        }
        if (!plugin_host::LoadOrReloadSingle("crit", critMsg) || img[0x01F51380U] != 0xE9U) {
            restoreCritTestMem();
            std::printf("FAIL [crit-reload]: failed to re-hook 0x01F51380 (%s)\n", critMsg.c_str());
            return 1;
        }

        restoreCritTestMem();
        host->ClearAllRules();
        std::printf("[OK] Test 22: `rng_crit.dll` (remote passthrough, rate override, random/ratio/seq/mv_bank/mv_tier/bag/prd & hot unload/reload) verified!\n");
    }

    // ------------------------------------------------------------------------
    // Test 23: Verify Rare Endemic Life Spawn Controller (`rng_endemic.dll` v1.3.0)
    //          on Real MHW 15.23 `FUN_141a4a9b0`, `0x140271ce0`, `0x140271f90 -> 0x14166d070`,
    //          and `FUN_141a44f50` (Mutual-Exclusion Tie-Breaker) Machine Code!
    // ------------------------------------------------------------------------
    {
        *reinterpret_cast<float*>(base + 0x30C8AC4U) = 0.015f;

        alignas(16) static std::uint8_t synTime[0x100]{};
        alignas(16) static std::uint8_t synZeroBuf[0x100]{};
        alignas(16) static std::uint8_t synQuest[0x13000]{};
        *reinterpret_cast<float*>(synTime + 0x38U)        = 12.0f; // Noon (in [0, 24) window)
        *reinterpret_cast<std::uint32_t*>(synTime + 0x8CU) = 0U;   // Normal clear weather
        *reinterpret_cast<std::int32_t*>(synQuest + 0x12FF0U) = -1;

        void* const prevTimePtr  = *reinterpret_cast<void**>(base + 0x5013830U);
        void* const prevZeroPtr  = *reinterpret_cast<void**>(base + 0x51C46B8U);
        void* const prevQuestPtr = *reinterpret_cast<void**>(base + 0x500ED30U);
        void* const prevBlockPtr = *reinterpret_cast<void**>(base + 0x51C42B8U); // `FUN_14166c870` quest-block flag holder
        *reinterpret_cast<void**>(base + 0x5013830U) = synTime;
        *reinterpret_cast<void**>(base + 0x51C46B8U) = synZeroBuf;
        *reinterpret_cast<void**>(base + 0x500ED30U) = synQuest;
        *reinterpret_cast<void**>(base + 0x51C42B8U) = synQuest; // `[ptr + 0xD2EB] == 0` -> not blocked

        std::uint8_t origB417C0[8]{}, origB02720[3]{}, origA486F0[12]{};
        std::uint8_t origB545D0[6]{}, orig272450[1]{}, origA4A830[12]{};
        std::memcpy(origB417C0, img + 0x1B417C0U, sizeof(origB417C0));
        std::memcpy(origB02720, img + 0x1B02720U, sizeof(origB02720));
        std::memcpy(origA486F0, img + 0x1A486F0U, sizeof(origA486F0));
        std::memcpy(origB545D0, img + 0x1B545D0U, sizeof(origB545D0));
        std::memcpy(orig272450, img + 0x0272450U, sizeof(orig272450));
        std::memcpy(origA4A830, img + 0x1A4A830U, sizeof(origA4A830));

        auto restoreRareTestMem = [&]() {
            cmd_router::HandleCommandLine("$rng endemic clear", true);
            cmd_router::HandleCommandLine("$rng endemic rare log off", true);
            std::memcpy(img + 0x1B417C0U, origB417C0, sizeof(origB417C0));
            std::memcpy(img + 0x1B02720U, origB02720, sizeof(origB02720));
            std::memcpy(img + 0x1A486F0U, origA486F0, sizeof(origA486F0));
            std::memcpy(img + 0x1B545D0U, origB545D0, sizeof(origB545D0));
            std::memcpy(img + 0x0272450U, orig272450, sizeof(orig272450));
            std::memcpy(img + 0x1A4A830U, origA4A830, sizeof(origA4A830));
            *reinterpret_cast<void**>(base + 0x5013830U) = prevTimePtr;
            *reinterpret_cast<void**>(base + 0x51C46B8U) = prevZeroPtr;
            *reinterpret_cast<void**>(base + 0x500ED30U) = prevQuestPtr;
            *reinterpret_cast<void**>(base + 0x51C42B8U) = prevBlockPtr;
            ::FlushInstructionCache(::GetCurrentProcess(), img, kImageSize);
        };

        auto writeAbsJump12 = [&](std::uint32_t rva, void* dst) {
            std::uint8_t* p = img + rva;
            p[0] = 0x48; p[1] = 0xB8;
            const auto addr = reinterpret_cast<std::uintptr_t>(dst);
            std::memcpy(p + 2, &addr, 8);
            p[10] = 0xFF; p[11] = 0xE0;
        };

        // Patch tiny deterministic helper stubs into MHW code buffer
        // `0x141b417c0`: mov rax, -1; ret (48 C7 C0 FF FF FF FF C3)
        static constexpr std::uint8_t kRetNeg1[8] = { 0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3 };
        // `0x141b02720`: xor eax, eax; ret (31 C0 C3)
        static constexpr std::uint8_t kRetZero[3] = { 0x31, 0xC0, 0xC3 };
        // `0x141b545d0`: mov eax, 7; ret (B8 07 00 00 00 C3) -> GL region level 7
        static constexpr std::uint8_t kRetLvl7[6] = { 0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3 };
        static constexpr std::uint8_t kRetVoid[1] = { 0xC3 };

        std::memcpy(img + 0x1B417C0U, kRetNeg1, sizeof(kRetNeg1));
        std::memcpy(img + 0x1B02720U, kRetZero, sizeof(kRetZero));
        std::memcpy(img + 0x1B545D0U, kRetLvl7, sizeof(kRetLvl7));
        std::memcpy(img + 0x0272450U, kRetVoid, sizeof(kRetVoid));

        // Synthetic RareSetData & Ec059RareSetData
        alignas(16) static std::uint8_t synRareMaster[0x30]{};
        alignas(16) static std::uint8_t synRareMoly[0x30]{};
        *reinterpret_cast<std::int32_t*>(synRareMaster + 0x08U) = 0;     // any weather
        *reinterpret_cast<float*>(synRareMaster + 0x0CU)        = 0.01f; // 1%
        synRareMaster[0x10U] = 0U;
        synRareMaster[0x11U] = 24U;

        *reinterpret_cast<std::int32_t*>(synRareMoly + 0x08U)  = 0;     // any weather
        *reinterpret_cast<float*>(synRareMoly + 0x0CU)         = 0.05f; // 5%
        synRareMoly[0x10U] = 0U;
        synRareMoly[0x11U] = 24U;
        *reinterpret_cast<std::uint32_t*>(synRareMoly + 0x18U) = 5U;    // minPopLv
        *reinterpret_cast<std::uint32_t*>(synRareMoly + 0x1CU) = 7U;    // maxPopLv

        static void* synRareMasterPtrs[1] = { synRareMaster };
        static void* synRareMolyPtrs[1]   = { synRareMoly };
        alignas(16) static std::uint8_t synRareArrHolder[0x28]{};
        *reinterpret_cast<std::uint32_t*>(synRareArrHolder + 0x08U) = 1U;
        *reinterpret_cast<void***>(synRareArrHolder + 0x18U)        = synRareMasterPtrs;

        alignas(16) static std::uint8_t synMolyArr[0x28]{};
        *reinterpret_cast<std::uint32_t*>(synMolyArr + 0x08U) = 1U;
        *reinterpret_cast<void***>(synMolyArr + 0x18U)        = synRareMolyPtrs;

        // C++ helper via thunk for `0x141a486f0(rcx, rdx_tmpMtArr, r8d_animalType)`
        auto* getMolyArrFn = +[](void*, std::uint8_t* tmpArr, std::uint32_t) noexcept -> void* {
            tmpArr[0x10U] = 0U;
            *reinterpret_cast<std::uintptr_t*>(tmpArr + 0x18U) = 0U;
            return synMolyArr;
        };
        writeAbsJump12(0x1A486F0U, reinterpret_cast<void*>(getMolyArrFn));

        static std::uint32_t g_despawnMask = 0U;
        auto* despawnSpyFn = +[](std::uint8_t* sAnimPtr, std::uint32_t slotIdx, std::uint8_t) noexcept {
            if (slotIdx < 32U) {
                g_despawnMask |= (1U << slotIdx);
                sAnimPtr[0x70A8U + static_cast<std::size_t>(slotIdx) * 0xD0U + 0xC9U] = 0xFFU;
            }
        };
        writeAbsJump12(0x1A4A830U, reinterpret_cast<void*>(despawnSpyFn));
        ::FlushInstructionCache(::GetCurrentProcess(), img, kImageSize);

        // Synthetic sAnimal + 3 uAnimals (slot 0, 1, 2)
        alignas(16) static std::uint8_t synSAnimal[0x10000]{};
        alignas(16) static std::uint8_t synUAnimals[3][0x2000]{};
        auto setupSlot = [&](std::uint32_t slotIdx, std::uint32_t aid, std::uint32_t atp, std::int8_t mutexId) {
            std::uint8_t* u = synUAnimals[slotIdx];
            std::memset(u, 0, 0x2000U);
            *reinterpret_cast<std::uint32_t*>(u + 0x1AF0U) = aid;
            *reinterpret_cast<std::uint32_t*>(u + 0x1AF4U) = atp;
            std::uint8_t* sl = synSAnimal + 0x70A8U + static_cast<std::size_t>(slotIdx) * 0xD0U;
            std::memset(sl, 0, 0xD0U);
            *reinterpret_cast<void**>(sl + 0x00U)       = u;
            sl[0xC9U]                                   = 0xFFU; // inactive
            sl[0xCAU]                                   = static_cast<std::uint8_t>(mutexId);
            sl[0xCBU]                                   = 0U;
        };

        using MasterRareRollFn = void(__fastcall*)(void* sAnim, std::uint8_t isInit, std::uint32_t aid,
                                                   std::uint32_t atp, std::uint32_t regIdx,
                                                   void* arrHolder, std::uint32_t slotIdx,
                                                   float prevTime, float curTime, std::uint8_t isSpecW);
        using MolyRollFn       = void(__fastcall*)(void* molyCtrl);
        using GroupRareFn      = void(__fastcall*)(void* grpCtrl);
        using MutexTieFn       = void(__fastcall*)(void* param);

        const auto fnMaster = reinterpret_cast<MasterRareRollFn>(base + 0x1A4A9B0U);
        const auto fnMoly   = reinterpret_cast<MolyRollFn>(base + 0x0271CE0U);
        const auto fnGroup  = reinterpret_cast<GroupRareFn>(base + 0x0271F90U);
        const auto fnMutex  = reinterpret_cast<MutexTieFn>(base + 0x1A44F50U);

        // (a) Master rare roll (`FUN_141a4a9b0` @ `RVA 0x01A4ACED`)
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "on"}, "rare on");
        for (int i = 0; i < 10; ++i) {
            setupSlot(0U, 58U, 0U, 0);
            *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.01f;
            fnMaster(synSAnimal, 1U, 58U, 0U, 0U, synRareArrHolder, 0U, 12.0f, 12.0f, 0U);
            const std::uint8_t* sl = synSAnimal + 0x70A8U;
            if (sl[0xC9U] != 24U || sl[0xCBU] != 1U) {
                restoreRareTestMem();
                std::printf("FAIL [rare-master-on]: trial %d endTime=%u justRolled=%u\n",
                            i, sl[0xC9U], sl[0xCBU]);
                return 1;
            }
        }
        // Disabled 0.0f-prob row MUST NOT be falsely forced!
        setupSlot(0U, 58U, 0U, 0);
        *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.0f;
        fnMaster(synSAnimal, 1U, 58U, 0U, 0U, synRareArrHolder, 0U, 12.0f, 12.0f, 0U);
        if (synSAnimal[0x70A8U + 0xC9U] != 0xFFU) {
            restoreRareTestMem();
            std::printf("FAIL [rare-zero-prob]: disabled 0.0f row should stay unspawned!\n");
            return 1;
        }
        // Global `0%` blocks even 95%-prob entry
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "0%"}, "rare 0%");
        for (int i = 0; i < 10; ++i) {
            setupSlot(0U, 58U, 0U, 0);
            *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.95f;
            fnMaster(synSAnimal, 1U, 58U, 0U, 0U, synRareArrHolder, 0U, 12.0f, 12.0f, 0U);
            if (synSAnimal[0x70A8U + 0xC9U] != 0xFFU) {
                restoreRareTestMem();
                std::printf("FAIL [rare-master-block]: trial %d spawned under 0%%!\n", i);
                return 1;
            }
        }
        // Per-species: `ec058` on (1%), `ec025` blocked (95%)
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "off"}, "rare off");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "ec058", "on"}, "rare ec058 on");
        plugin_host::DispatchPluginSubcommand("endemic", {"ec025", "rare", "0%"}, "ec025 rare 0%");
        for (int i = 0; i < 10; ++i) {
            setupSlot(0U, 58U, 0U, 0);
            *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.01f;
            fnMaster(synSAnimal, 1U, 58U, 0U, 0U, synRareArrHolder, 0U, 12.0f, 12.0f, 0U);
            if (synSAnimal[0x70A8U + 0xC9U] != 24U) {
                restoreRareTestMem();
                std::printf("FAIL [rare-sp-on]: ec058 did not spawn at trial %d\n", i);
                return 1;
            }
            setupSlot(1U, 25U, 0U, -1);
            *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.95f;
            fnMaster(synSAnimal, 1U, 25U, 0U, 0U, synRareArrHolder, 1U, 12.0f, 12.0f, 0U);
            if (synSAnimal[0x70A8U + 0xD0U + 0xC9U] != 0xFFU) {
                restoreRareTestMem();
                std::printf("FAIL [rare-sp-block]: ec025 spawned despite 0%% at trial %d\n", i);
                return 1;
            }
        }

        // (b) Guiding Lands Rare Regional Moly (`0x140271ce0` @ `RVA 0x00271F25`)
        alignas(16) static std::uint8_t synMolyCtrl[0x500]{};
        *reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x328U) = 59U;
        *reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x32CU) = 2U;
        *reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x330U) = 2U; // Coral region
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "off"}, "rare off");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "ec059", "on"}, "rare ec059 on");
        for (int i = 0; i < 10; ++i) {
            *reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x40CU) = 8U;
            fnMoly(synMolyCtrl);
            if (*reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x40CU) != 7U) {
                restoreRareTestMem();
                std::printf("FAIL [rare-moly-on]: expected state=7, got %u\n",
                            *reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x40CU));
                return 1;
            }
        }
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "ec059", "0%"}, "rare ec059 0%");
        *reinterpret_cast<float*>(synRareMoly + 0x0CU) = 0.95f;
        for (int i = 0; i < 10; ++i) {
            *reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x40CU) = 7U;
            fnMoly(synMolyCtrl);
            if (*reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x40CU) != 8U) {
                restoreRareTestMem();
                std::printf("FAIL [rare-moly-block]: expected state=8, got %u\n",
                            *reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x40CU));
                return 1;
            }
        }

        // (c) Group-level rare roll (`0x140271f90 -> 0x14166d070` @ `RVA 0x0166D12E`)
        alignas(16) static std::uint8_t synGroupCtrl[0x400]{};
        *reinterpret_cast<std::uint32_t*>(synGroupCtrl + 0x328U) = 25U;
        *reinterpret_cast<std::uint32_t*>(synGroupCtrl + 0x32CU) = 0U;
        *reinterpret_cast<std::uint32_t*>(synGroupCtrl + 0x388U) = 4U; // non-zero without weather/time bits
        *reinterpret_cast<std::uint32_t*>(synGroupCtrl + 0x348U + 0x08U) = 1U;
        *reinterpret_cast<void***>(synGroupCtrl + 0x348U + 0x18U)        = synRareMasterPtrs;
        *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.02f;
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "on"}, "rare on");
        fnGroup(synGroupCtrl);
        if (synGroupCtrl[0x334U] != 1U) {
            restoreRareTestMem();
            std::printf("FAIL [rare-group-on]: expected ctrl[0x334]=1, got %u\n", synGroupCtrl[0x334U]);
            return 1;
        }
        *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.95f;
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "ec025", "0%"}, "rare ec025 0%");
        fnGroup(synGroupCtrl);
        if (synGroupCtrl[0x334U] != 0U) {
            restoreRareTestMem();
            std::printf("FAIL [rare-group-block]: expected ctrl[0x334]=0, got %u\n", synGroupCtrl[0x334U]);
            return 1;
        }

        // (d) Mutual-exclusion tie-breaker (`FUN_141a44f50` @ `RVA 0x01A44FCC`)
        alignas(16) static std::uint32_t mutexSlotList[0x90]{};
        mutexSlotList[0] = 0U; // Inactive sibling
        mutexSlotList[1] = 1U; // ec015_00 active
        mutexSlotList[2] = 2U; // ec058_00 active
        *reinterpret_cast<std::uint64_t*>(mutexSlotList + 0x80U) = 3ULL; // count at +0x200
        static std::uint8_t synMutexFlag = 0U;
        struct MutexParam { void* list; void* sAnim; void* flagPtr; } mutexParam{
            mutexSlotList, synSAnimal, &synMutexFlag
        };

        auto resetMutexSlots = [&]() {
            g_despawnMask = 0U;
            setupSlot(0U, 10U, 0U, 0); // inactive (0xC9=0xFF, 0xCB=0)
            setupSlot(1U, 15U, 0U, 0);
            synSAnimal[0x70A8U + 1U * 0xD0U + 0xC9U] = 5U;
            synSAnimal[0x70A8U + 1U * 0xD0U + 0xCBU] = 1U;
            setupSlot(2U, 58U, 0U, 0);
            synSAnimal[0x70A8U + 2U * 0xD0U + 0xC9U] = 5U;
            synSAnimal[0x70A8U + 2U * 0xD0U + 0xCBU] = 1U;
        };

        // Case d1: `rare on` globally -> inactive slot 0 must NEVER wipe both active slots
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "off"}, "rare off");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "on"}, "rare on");
        for (int i = 0; i < 30; ++i) {
            resetMutexSlots();
            fnMutex(&mutexParam);
            const bool alive1 = (synSAnimal[0x70A8U + 1U * 0xD0U + 0xC9U] == 5U);
            const bool alive2 = (synSAnimal[0x70A8U + 2U * 0xD0U + 0xC9U] == 5U);
            if (!(alive1 ^ alive2)) {
                restoreRareTestMem();
                std::printf("FAIL [rare-mutex-global]: trial %d alive1=%d alive2=%d\n", i, alive1, alive2);
                return 1;
            }
        }
        // Case d2: per-species `ec058` forced -> slot 2 (`ec058`) survives 30/30!
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "off"}, "rare off");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "ec058", "on"}, "rare ec058 on");
        for (int i = 0; i < 30; ++i) {
            resetMutexSlots();
            fnMutex(&mutexParam);
            const bool alive1 = (synSAnimal[0x70A8U + 1U * 0xD0U + 0xC9U] == 5U);
            const bool alive2 = (synSAnimal[0x70A8U + 2U * 0xD0U + 0xC9U] == 5U);
            if (alive1 || !alive2) {
                restoreRareTestMem();
                std::printf("FAIL [rare-mutex-sp]: trial %d expected slot2 win, alive1=%d alive2=%d\n",
                            i, alive1, alive2);
                return 1;
            }
        }
        // Case d3: global `rare on` + `ec058 0%` -> slot 1 (`ec015`) survives 30/30!
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "on"}, "rare on");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "ec058", "0%"}, "rare ec058 0%");
        for (int i = 0; i < 30; ++i) {
            resetMutexSlots();
            fnMutex(&mutexParam);
            const bool alive1 = (synSAnimal[0x70A8U + 1U * 0xD0U + 0xC9U] == 5U);
            const bool alive2 = (synSAnimal[0x70A8U + 2U * 0xD0U + 0xC9U] == 5U);
            if (!alive1 || alive2) {
                restoreRareTestMem();
                std::printf("FAIL [rare-mutex-deny]: trial %d expected slot1 win, alive1=%d alive2=%d\n",
                            i, alive1, alive2);
                return 1;
            }
        }

        // (e) Telemetry (`rare log on` async delivery & `rare history`)
        g_capturedAsyncChatLines.clear();
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "log", "on"}, "rare log on");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "ec058", "on"}, "rare ec058 on");
        setupSlot(0U, 58U, 0U, 0);
        *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.02f;
        fnMaster(synSAnimal, 1U, 58U, 0U, 0U, synRareArrHolder, 0U, 12.0f, 12.0f, 0U);
        if (g_capturedAsyncChatLines.empty()) {
            restoreRareTestMem();
            std::printf("FAIL [rare-log-async]: expected async rare spawn log message\n");
            return 1;
        }
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "history"}, "rare history");

        // (f) Game-data name selectors (Simplified Chinese name / English slug / family `name*`)
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "log", "off"}, "rare log off");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "off"}, "rare off");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", u8"\u6708\u7FBD\u5929\u6BCD", "on"}, "rare zh-name on");
        for (int i = 0; i < 5; ++i) {
            setupSlot(0U, 58U, 0U, 0);
            *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.01f;
            fnMaster(synSAnimal, 1U, 58U, 0U, 0U, synRareArrHolder, 0U, 12.0f, 12.0f, 0U);
            if (synSAnimal[0x70A8U + 0xC9U] != 24U) {
                restoreRareTestMem();
                std::printf("FAIL [rare-name-zh]: zh name selector did not force ec058_00\n");
                return 1;
            }
        }
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "wintermoon_nettle", "0%"}, "rare slug 0%");
        for (int i = 0; i < 5; ++i) {
            setupSlot(0U, 58U, 0U, 0);
            *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.95f;
            fnMaster(synSAnimal, 1U, 58U, 0U, 0U, synRareArrHolder, 0U, 12.0f, 12.0f, 0U);
            if (synSAnimal[0x70A8U + 0xC9U] != 0xFFU) {
                restoreRareTestMem();
                std::printf("FAIL [rare-name-slug]: slug selector did not block ec058_00\n");
                return 1;
            }
        }
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "off"}, "rare off");
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "rocky_moly", "0%"}, "rare rocky_moly 0%");
        *reinterpret_cast<float*>(synRareMoly + 0x0CU) = 0.95f;
        for (int i = 0; i < 5; ++i) {
            *reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x40CU) = 7U;
            fnMoly(synMolyCtrl); // ctrl animalType = 2 (Rocky Moly)
            if (*reinterpret_cast<std::uint32_t*>(synMolyCtrl + 0x40CU) != 8U) {
                restoreRareTestMem();
                std::printf("FAIL [rare-name-moly]: rocky_moly 0%% did not block ec059_02\n");
                return 1;
            }
        }
        g_capturedChatLines.clear();
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "status"}, "rare status names");
        {
            bool sawName = false;
            for (const auto& l : g_capturedChatLines) {
                if (l.find(u8"ec059_02(\u6398\u571F\u6BDB\u5409)") != std::string::npos ||
                    l.find("ec059_02(Rocky Moly)") != std::string::npos) {
                    sawName = true;
                }
            }
            if (!sawName) {
                restoreRareTestMem();
                std::printf("FAIL [rare-name-label]: status did not show `ec059_02(<name>)` label\n");
                return 1;
            }
        }

        restoreRareTestMem();
        MhwRng_GetHostApi()->ClearAllRules();
        std::printf("[OK] Test 23: `rng_endemic.dll` v1.3.0 (master rare, GL rare Moly, group rare, mutex tie-breaker & async log) verified!\n");
    }

    plugin_host::ShutdownAll();
    hook::RemoveAllHooks();
    rng::Shutdown();
    if (std::memcmp(img + mhw::kRvaMtRandomNrand, mhw::kPrologueMtRandomNrand, sizeof(mhw::kPrologueMtRandomNrand)) != 0 ||
        std::memcmp(img + mhw::kRvaSChatMove, mhw::kPrologueSChatMove, sizeof(mhw::kPrologueSChatMove)) != 0) {
        std::printf("FAIL: RemoveAllHooks failed\n");
        return 1;
    }
    ::VirtualFree(thunkMem, 0, MEM_RELEASE);
    ::VirtualFree(img, 0, MEM_RELEASE);
    std::printf("=== ALL MICROKERNEL v3.0 & OFFICIAL PLUGIN AUDIT TESTS PASSED ===\n");
    return 0;
}
