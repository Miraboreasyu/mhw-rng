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

using TestNrandFn  = std::uint32_t(__fastcall*)(std::uint32_t*);
using TestSphereFn = float*(__fastcall*)(std::uint32_t*, float*);
// Executes `fn_r9(holder_rcx, tblIdx_rdx)` with non-volatile `RBX = monster_r8`, reproducing `FUN_141ccfb70`!
using CallWithMonsterRbxFn = std::uint32_t(__fastcall*)(void* holder, std::uint32_t* tblIdxPtr, void* monster, void* targetFn);
using EndemicCaptureRollFn = std::uint32_t(__fastcall*)(std::int32_t animalId, std::int32_t animalType, float* outScale);
using SharedFloatRangeFn   = float(__fastcall*)(float minVal, float maxVal);

volatile std::uint32_t g_antiTailCallSink = 0;
std::vector<std::string> g_capturedChatLines;

void CaptureChatSink(const char* utf8Text) noexcept {
    if (utf8Text) {
        g_capturedChatLines.emplace_back(utf8Text);
        std::printf("  [chat] %s\n", utf8Text);
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
    std::printf("=== MHW RNG Microkernel v2.1 + Official Plugin Audit Suite ===\n");

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
    auto* thunkMem = static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!thunkMem) return 1;
    std::memcpy(thunkMem, kRbxCallerThunk, sizeof(kRbxCallerThunk));
    const auto callSizeWithMonster = reinterpret_cast<CallWithMonsterRbxFn>(thunkMem);

    if (!hook::InitArena(base, end)) return 1;

    app::Config cfg{};
    cfg.masterSeed     = 0xCAFEBABE12345678ULL;
    cfg.takeoverOnBoot = true;
    rng::Init(base, end, cfg);

    void *origNrand = nullptr, *origCtor = nullptr, *origDef = nullptr, *origSrand = nullptr, *origSph = nullptr;
    void *origRdev  = nullptr, *origMtRng = nullptr, *origMtIni = nullptr, *origMtDrw = nullptr, *origCrt = nullptr;

    const bool okHooks =
        hook::PrepareDetour(base + mhw::kRvaMtRandomNrand,       sizeof(mhw::kPrologueMtRandomNrand),       reinterpret_cast<void*>(&rng::Hook_MtRandomNrand),       &origNrand, mhw::kPrologueMtRandomNrand) &&
        hook::PrepareDetour(base + mhw::kRvaMtRandomCtorSeed,    sizeof(mhw::kPrologueMtRandomCtorSeed),    reinterpret_cast<void*>(&rng::Hook_MtRandomCtorSeed),    &origCtor,  mhw::kPrologueMtRandomCtorSeed) &&
        hook::PrepareDetour(base + mhw::kRvaMtRandomCtorDefault, sizeof(mhw::kPrologueMtRandomCtorDefault), reinterpret_cast<void*>(&rng::Hook_MtRandomCtorDefault), &origDef,   mhw::kPrologueMtRandomCtorDefault) &&
        hook::PrepareDetour(base + mhw::kRvaMtRandomSrand,       sizeof(mhw::kPrologueMtRandomSrand),       reinterpret_cast<void*>(&rng::Hook_MtRandomSrand),       &origSrand, mhw::kPrologueMtRandomSrand) &&
        hook::PrepareDetour(base + mhw::kRvaMtRandomSphere,      sizeof(mhw::kPrologueMtRandomSphere),      reinterpret_cast<void*>(&rng::Hook_MtRandomSphere),      &origSph,   mhw::kPrologueMtRandomSphere) &&
        hook::PrepareDetour(base + mhw::kRvaStdRandomDevice,     sizeof(mhw::kPrologueStdRandomDevice),     reinterpret_cast<void*>(&rng::Hook_StdRandomDevice),     &origRdev,  mhw::kPrologueStdRandomDevice) &&
        hook::PrepareDetour(base + mhw::kRvaMt19937Range,        sizeof(mhw::kPrologueMt19937Range),        reinterpret_cast<void*>(&rng::Hook_Mt19937Range),        &origMtRng, mhw::kPrologueMt19937Range) &&
        hook::PrepareDetour(base + mhw::kRvaMt19937HeapInit,     sizeof(mhw::kPrologueMt19937HeapInit),     reinterpret_cast<void*>(&rng::Hook_Mt19937HeapInit),     &origMtIni, mhw::kPrologueMt19937HeapInit) &&
        hook::PrepareDetour(base + mhw::kRvaMt19937HeapDraw,     sizeof(mhw::kPrologueMt19937HeapDraw),     reinterpret_cast<void*>(&rng::Hook_Mt19937HeapDraw),     &origMtDrw, mhw::kPrologueMt19937HeapDraw) &&
        hook::PrepareDetour(base + mhw::kRvaCrtRand,             sizeof(mhw::kPrologueCrtRand),             reinterpret_cast<void*>(&rng::Hook_CrtRand),             &origCrt,   mhw::kPrologueCrtRand);
    if (!okHooks) return 1;

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

    // 2. Load Official Plugins (`rng_inspector.dll` + `rng_size.dll` + `rng_endemic.dll`) via Shadow-Copy Host
    const std::string plugDir = buildDir + "\\mhw_rng";

    plugin_host::Init(base, end, plugDir, &CaptureChatSink);
    const std::size_t loadedCnt = plugin_host::LoadAllFromDirectory();
    if (loadedCnt < 3U) {
        std::printf("FAIL: Expected >= 3 plugins in %s, loaded %zu\n", plugDir.c_str(), loadedCnt);
        return 1;
    }
    std::printf("[OK] Shadow-copy loaded %zu official plugins (`inspector` + `size` + `endemic`)!\n", loadedCnt);

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

    // 8. Verify Zero-Lock Shadow-Copy Hot-Reload + Inspector `dump` / `top` + Hot-Unload!
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

    // 9. Verify `sChat::DispatchChatEntry` Standalone Chaining + Unchained-Overwrite Detection & Self-Healing!
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

    // 10. Verify `randUnitSphere` & clean shutdown
    float v3[3]{};
    CallSphereSite(fnSphere, sInst, v3);
    const float norm = std::sqrt(v3[0] * v3[0] + v3[1] * v3[1] + v3[2] * v3[2]);
    if (std::fabs(norm - 1.0f) > 1e-4f) return 1;

    plugin_host::ShutdownAll();
    hook::RemoveAllHooks();
    if (std::memcmp(img + mhw::kRvaMtRandomNrand, mhw::kPrologueMtRandomNrand, sizeof(mhw::kPrologueMtRandomNrand)) != 0) {
        std::printf("FAIL: RemoveAllHooks failed\n");
        return 1;
    }
    ::VirtualFree(thunkMem, 0, MEM_RELEASE);
    ::VirtualFree(img, 0, MEM_RELEASE);
    std::printf("=== ALL MICROKERNEL v2.1 & OFFICIAL PLUGIN AUDIT TESTS PASSED ===\n");
    return 0;
}
