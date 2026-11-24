#include "test_env.h"

bool RunSuite_PluginSize(TestEnv& env) {
    auto* const img = env.img;
    const auto  base = env.base;
    const auto  end  = env.end;
    auto* const thunkMem = env.thunkMem;
    const auto& buildDir = env.buildDir;
    const auto& plugDir  = env.plugDir;
    auto&       cfg      = env.cfg;
    const auto  fnNrand  = env.fnNrand;
    const auto  fnSphere = env.fnSphere;
    auto* const sInst    = env.sInst;
    const auto  callSizeWithMonster = env.callSizeWithMonster;
    const auto  callEmThinkUpdate   = env.callEmThinkUpdate;
    (void)img; (void)base; (void)end; (void)thunkMem; (void)buildDir; (void)plugDir;
    (void)cfg; (void)fnNrand; (void)fnSphere; (void)sInst;
    (void)callSizeWithMonster; (void)callEmThinkUpdate;

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
        return false;
    }
    if (szFurious != 90U) {
        std::printf("FAIL [Bug #1/#2]: `$rng size furious_rajang min` on Golden Trophy table expected legal min 90%%, got %u%% (88%% = illegal 0-weight row!)\n", szFurious);
        return false;
    }
    if (szBarroth < 89U || szBarroth > 124U) {
        std::printf("FAIL: Unlocked Barroth should roll normal size, got %u%%\n", szBarroth);
        return false;
    }
    std::printf("[OK] Bug #1 & #2 verified: Rajang(#91)=%u%%, GoldenTrophy FuriousRajang(#92)=%u%% (0-weight 88%%/89%% skipped!), Unlocked Barroth(#21)=%u%%!\n",
                szRajang, szFurious, szBarroth);

    // 5. Verify `$rng size 0` AND `rng_inspector.dll`'s `$rng fix size min` / `$rng fix size 0` on Golden Trophy table!
    plugin_host::DispatchPluginSubcommand("size", {"clear"}, "clear");
    plugin_host::DispatchPluginSubcommand("size", {"0"}, "0");
    if (spawnMonster(monFuriousRajang) != 90U) {
        std::printf("FAIL [Bug #2]: `$rng size 0` hit illegal 0-weight row!\n");
        return false;
    }
    plugin_host::DispatchPluginSubcommand("size", {"clear"}, "clear");
    plugin_host::DispatchPluginSubcommand("fix", {"size", "min"}, "size min");
    if (spawnMonster(monFuriousRajang) != 90U) {
        std::printf("FAIL [Bug #2]: `$rng fix size min` hit illegal 0-weight row!\n");
        return false;
    }
    plugin_host::DispatchPluginSubcommand("fix", {"size", "0"}, "size 0");
    if (spawnMonster(monFuriousRajang) != 90U) {
        std::printf("FAIL [Bug #2]: `$rng fix size 0` hit illegal 0-weight row!\n");
        return false;
    }
    plugin_host::DispatchPluginSubcommand("clear", {"size"}, "size");
    std::printf("[OK] Both `$rng size 0` and `$rng fix size min/0` verified immune to `0.0f <= 0.0f` illegal size bug!\n");

    // 6. Verify Wildcard Family `rajang*` + `exact 116` live `cSizeTable` inverse solver!
    plugin_host::DispatchPluginSubcommand("size", {"rajang*", "exact", "116"}, "rajang* exact 116");
    const std::uint32_t exR1 = spawnMonster(monRajang);
    const std::uint32_t exR2 = spawnMonster(monFuriousRajang);
    if (exR1 != 116U || exR2 != 116U) {
        std::printf("FAIL: `rajang* exact 116` expected 116/116, got %u/%u\n", exR1, exR2);
        return false;
    }
    plugin_host::DispatchPluginSubcommand("size", {"history"}, "history");
    std::printf("[OK] Wildcard `rajang* exact 116` matched both #91 (%u%%) & #92 (%u%%)!\n", exR1, exR2);

    *reinterpret_cast<void**>(base + 0x5012908U) = nullptr;
    return true;
}

bool RunSuite_PluginThk(TestEnv& env) {
    auto* const img = env.img;
    const auto  base = env.base;
    const auto  end  = env.end;
    auto* const thunkMem = env.thunkMem;
    const auto& buildDir = env.buildDir;
    const auto& plugDir  = env.plugDir;
    auto&       cfg      = env.cfg;
    const auto  fnNrand  = env.fnNrand;
    const auto  fnSphere = env.fnSphere;
    auto* const sInst    = env.sInst;
    const auto  callSizeWithMonster = env.callSizeWithMonster;
    const auto  callEmThinkUpdate   = env.callEmThinkUpdate;
    (void)img; (void)base; (void)end; (void)thunkMem; (void)buildDir; (void)plugDir;
    (void)cfg; (void)fnNrand; (void)fnSphere; (void)sInst;
    (void)callSizeWithMonster; (void)callEmThinkUpdate;

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
                return false;
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
            return false;
        }

        // Verify 50/50 split on `node110` front (`55:217` at seg 3, `55:277` at seg 4, zero bites `1/2`):
        int cnt217 = 0, cnt277 = 0;
        for (int k = 0; k < 100; ++k) {
            const std::int32_t s = runThkNode(true, 0, 0, -1, 1, node110);
            if (s == 3) ++cnt217;
            else if (s == 4) ++cnt277;
            else {
                std::printf("FAIL [fatalis]: P1 quad 500~1500 front got unexpected seg %d (expected 3 or 4)!\n", s);
                return false;
            }
        }
        if (cnt217 < 30 || cnt217 > 70 || cnt277 < 30 || cnt277 > 70) {
            std::printf("FAIL [fatalis]: P1 quad 500~1500 50/50 balance out of range (217=%d, 277=%d)!\n",
                        cnt217, cnt277);
            return false;
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
            return false;
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
            return false;
        }
        int enr217 = 0, enr277 = 0;
        for (int k = 0; k < 60; ++k) {
            const std::int32_t s = runThkNode(true, 0, 4, -1, 1, node110);
            if (s == 3) ++enr217;
            else if (s == 4) ++enr277;
            else {
                std::printf("FAIL [fatalis]: Enraged (`f=4`) quad 50/50 got unexpected seg %d!\n", s);
                return false;
            }
        }
        if (enr217 < 15 || enr217 > 45 || enr277 < 15 || enr277 > 45) {
            std::printf("FAIL [fatalis]: Enraged (`f=4`) quad 50/50 balance out of range (%d vs %d)!\n", enr217, enr277);
            return false;
        }
        bool sawMountVarA = false, sawMountVarB = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t sm = runThkNode(true, 0, 11, -1, 1, node75);
            if (sm == 5) sawMountVarA = true;
            else sawMountVarB = true;
        }
        if (!sawMountVarA || !sawMountVarB) {
            std::printf("FAIL [fatalis]: Unrelated slot `thkFileIdx=11` should stay unmodified!\n");
            return false;
        }

        plugin_host::DispatchPluginSubcommand("fatalis", {"far", "multi"}, "far multi");
        if (runThkNode(true, 0, 0, -1, 0, node79) != 1 ||
            runThkNode(true, 0, 0, -1, 1, node107) != 2 ||
            runThkNode(true, 0, 0, -1, 0, node108) != 1) {
            std::printf("FAIL [fatalis]: `far multi` expected 55:220 on node79 & 55:221 on node107/108!\n");
            return false;
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
            return false;
        }
        *reinterpret_cast<std::int32_t*>(mgr + 0x340U)  = 2;
        *reinterpret_cast<const void**>(mgr + 0x200U)   = node75;
        if (runThkNode(true, 1, 55, 2881, 9, node55_87) != 10) {
            std::printf("FAIL [fatalis]: Frame 0 return stack unwind from wrapper 2881 -> node 75 expected ENDR (seg 10)!\n");
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
                return false;
            }
            if (s == 1) saw172 = true;
            else sawOther91 = true;
        }
        if (!saw172 || !sawOther91) {
            std::printf("FAIL [thk probe]: probe mode distorted natural Rajang (MonsterID=91) distribution!\n");
            return false;
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
                return false;
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
                return false;
            }
        }

        plugin_host::DispatchPluginSubcommand("thk", {"rule", "clear"}, "rule clear");
        plugin_host::DispatchPluginSubcommand("thk", {"reset"}, "reset");
        plugin_host::DispatchPluginSubcommand("thk", {"seq", "91", "12", "1", "55:185,vanilla,55:297", "once"},
                                             "seq 91 12 1 55:185,vanilla,55:297 once");
        if (runThkNode(true, 0, 0, -1, 1, node12) != 2) {
            std::printf("FAIL [thk seq once]: step 0 expected seg 2 (55:185)!\n");
            return false;
        }
        const std::int32_t vanillaMid = runThkNode(true, 0, 0, -1, 1, node12);
        if (vanillaMid < 1 || vanillaMid > 3) {
            std::printf("FAIL [thk seq once]: step 1 (vanilla) got invalid seg %d!\n", vanillaMid);
            return false;
        }
        if (runThkNode(true, 0, 0, -1, 1, node12) != 3) {
            std::printf("FAIL [thk seq once]: step 2 expected seg 3 (55:297)!\n");
            return false;
        }
        bool sawPostOnceVarA = false, sawPostOnceVarB = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t s = runThkNode(true, 0, 0, -1, 1, node12);
            if (s == 1) sawPostOnceVarA = true;
            else sawPostOnceVarB = true;
        }
        if (!sawPostOnceVarA || !sawPostOnceVarB) {
            std::printf("FAIL [thk seq once]: expected vanilla restoration after `once` sequence!\n");
            return false;
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
            return false;
        }
        bool sawAfterGateA = false, sawAfterGateB = false;
        for (int k = 0; k < 24; ++k) {
            const std::int32_t s = runThkNode(true, 0, 0, -1, 1, node12);
            if (s == 1 || s == 2) sawAfterGateA = true;
            if (s == 3) sawAfterGateB = true;
        }
        if (!sawAfterGateA || !sawAfterGateB) {
            std::printf("FAIL [thk gate]: hits >3 should return to natural vanilla variation!\n");
            return false;
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
            return false;
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
            return false;
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
            return false;
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
                return false;
            }
        }
        if (raj172 < 15 || raj172 > 45 || raj297 < 15 || raj297 > 45) {
            std::printf("FAIL [thk weighted]: 50/50 split skewed (%d vs %d)!\n", raj172, raj297);
            return false;
        }
        *reinterpret_cast<std::uint32_t*>(monFatalis + 0x12280U) = 101U;

        plugin_host::DispatchPluginSubcommand("thk", {"clear"}, "clear");
        std::printf("[OK] Generic THK probe/counter/sequence/weighted/preset/auto-reset engine verified on real 0x14132B838 machine code!\n");
    }

    return true;
}

bool RunSuite_PluginSteam(TestEnv& env) {
    auto* const img = env.img;
    const auto  base = env.base;
    const auto  end  = env.end;
    auto* const thunkMem = env.thunkMem;
    const auto& buildDir = env.buildDir;
    const auto& plugDir  = env.plugDir;
    auto&       cfg      = env.cfg;
    const auto  fnNrand  = env.fnNrand;
    const auto  fnSphere = env.fnSphere;
    auto* const sInst    = env.sInst;
    const auto  callSizeWithMonster = env.callSizeWithMonster;
    const auto  callEmThinkUpdate   = env.callEmThinkUpdate;
    (void)img; (void)base; (void)end; (void)thunkMem; (void)buildDir; (void)plugDir;
    (void)cfg; (void)fnNrand; (void)fnSphere; (void)sInst;
    (void)callSizeWithMonster; (void)callEmThinkUpdate;

    const MhwRngHostApi* host = MhwRng_GetHostApi();
        // V3-9: Verify Official Plugin #5 (`rng_steam.dll`) on Real MHW 15.23 `cSteamControl::start` & `FUN_1405ca060`
        {
            host->SetEngineMode(kMhwRngModeTakeover);
            if (img[0x0666795U] != 0xE8 || img[0x06667B6U] != 0xE8 || img[0x05CA09DU] != 0xE8) {
                std::printf("FAIL [V3-9]: Expected E8 calls at 0x0666795 / 0x06667B6 / 0x05CA09D in text_1523.bin!\n");
                return false;
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
                    return false;
                }
                const std::uint8_t* ord = fakeSteamCtrl + 8;
                const std::string expKb = formatExpectedKb(ord);
                const std::string& msg  = g_capturedAsyncChatLines.back();
                if (msg.find(expKb) == std::string::npos) {
                    img[0x06667D6U] = origKeygenEnd;
                    img[0x05CA0B6U] = origPityCmp;
                    std::printf("FAIL [V3-9a]: Observed hint `%s` mismatched real order `%s`!\n",
                                msg.c_str(), expKb.c_str());
                    return false;
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
                return false;
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
                    return false;
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
                return false;
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
                return false;
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
                return false;
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
                return false;
            }
            std::printf("[OK] V3-9: `rng_steam.dll` ObserveOnly hints (6/6 perms + red), lock (all 6 perms), pity & reload verified on real MHW code!\n");
        }

    return true;
}

