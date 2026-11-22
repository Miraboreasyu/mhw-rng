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

    // 2. Load Official Plugins (`rng_inspector.dll` + `rng_size.dll` + `rng_endemic.dll` + `rng_thk.dll` + `rng_steam.dll`) via Shadow-Copy Host
    const std::string plugDir = buildDir + "\\mhw_rng";

    plugin_host::Init(base, end, plugDir, &CaptureChatSink, &CaptureAsyncChatSink);
    const std::size_t loadedCnt = plugin_host::LoadAllFromDirectory();
    if (loadedCnt < 5U) {
        std::printf("FAIL: Expected >= 5 plugins in %s, loaded %zu\n", plugDir.c_str(), loadedCnt);
        return 1;
    }
    std::printf("[OK] Shadow-copy loaded %zu official plugins (`inspector` + `size` + `endemic` + `thk` + `steam`)!\n", loadedCnt);

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
                    g_capturedAsyncChatLines.empty() ||
                    g_capturedAsyncChatLines.back().find(u8"[锁序]") == std::string::npos) {
                    img[0x06667D6U] = origKeygenEnd;
                    img[0x05CA0B6U] = origPityCmp;
                    std::printf("FAIL [V3-9b]: `$rng steam lock %s` produced [%u,%u,%u]!\n",
                                lc.arg, fakeSteamCtrl[8], fakeSteamCtrl[9], fakeSteamCtrl[10]);
                    return 1;
                }
            }

            // Verify bare `$rng steam lock` (no args) defaults to `A - W - D` ({0, 1, 2})
            plugin_host::DispatchPluginSubcommand("steam", {"lock", "off"}, "lock off");
            plugin_host::DispatchPluginSubcommand("steam", {"lock"}, "lock");
            g_capturedAsyncChatLines.clear();
            callWithRbx(fakeSteamCtrl, img + 0x0666785U);
            if (fakeSteamCtrl[8] != 0 || fakeSteamCtrl[9] != 1 || fakeSteamCtrl[10] != 2 ||
                g_capturedAsyncChatLines.empty() ||
                g_capturedAsyncChatLines.back().find(u8"[锁序]") == std::string::npos) {
                img[0x06667D6U] = origKeygenEnd;
                img[0x05CA0B6U] = origPityCmp;
                std::printf("FAIL [V3-9b]: bare `$rng steam lock` did not default to [0,1,2] (got [%u,%u,%u])!\n",
                            fakeSteamCtrl[8], fakeSteamCtrl[9], fakeSteamCtrl[10]);
                return 1;
            }

            // (c) Hidden guaranteed-win pity alert (`0x05CA0A2`)
            g_capturedAsyncChatLines.clear();
            *reinterpret_cast<std::uint32_t*>(fakeSteamCtrl + 0x4A4U) = 100U;
            callWithRbx(fakeSteamCtrl + 0x4A4U, img + 0x05CA096U);
            const bool sawPity100 = (!g_capturedAsyncChatLines.empty() &&
                                     g_capturedAsyncChatLines.back().find(u8"隐藏必中保底") != std::string::npos);
            g_capturedAsyncChatLines.clear();
            *reinterpret_cast<std::uint32_t*>(fakeSteamCtrl + 0x4A4U) = 0U;
            callWithRbx(fakeSteamCtrl + 0x4A4U, img + 0x05CA096U);
            const bool cleanPity0 = g_capturedAsyncChatLines.empty();
            if (!sawPity100 || !cleanPity0) {
                img[0x06667D6U] = origKeygenEnd;
                img[0x05CA0B6U] = origPityCmp;
                std::printf("FAIL [V3-9c]: auto-win check failed (pity100=%d clean0=%d)\n", sawPity100, cleanPity0);
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

    // 12. Verify `randUnitSphere` & clean shutdown
    float v3[3]{};
    CallSphereSite(fnSphere, sInst, v3);
    const float norm = std::sqrt(v3[0] * v3[0] + v3[1] * v3[1] + v3[2] * v3[2]);
    if (std::fabs(norm - 1.0f) > 1e-4f) return 1;

    plugin_host::ShutdownAll();
    hook::RemoveAllHooks();
    rng::Shutdown();
    if (std::memcmp(img + mhw::kRvaMtRandomNrand, mhw::kPrologueMtRandomNrand, sizeof(mhw::kPrologueMtRandomNrand)) != 0) {
        std::printf("FAIL: RemoveAllHooks failed\n");
        return 1;
    }
    ::VirtualFree(thunkMem, 0, MEM_RELEASE);
    ::VirtualFree(img, 0, MEM_RELEASE);
    std::printf("=== ALL MICROKERNEL v3.0 & OFFICIAL PLUGIN AUDIT TESTS PASSED ===\n");
    return 0;
}
