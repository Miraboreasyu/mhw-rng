#include "test_env.h"

bool RunSuite_PluginEndemicSize(TestEnv& env) {
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
        return false;
    }

    plugin_host::DispatchPluginSubcommand("endemic", {"giant"}, "giant");
    const std::uint32_t crGiant = fnEndemicCapture(59, 0, &outScale);
    if (crGiant != 1U || std::fabs(outScale - 1.25f) > 0.015f) {
        std::printf("FAIL [endemic giant]: expected crown=1 scale=1.25, got crown=%u scale=%.3f\n", crGiant, outScale);
        return false;
    }

    // 7b. Verify shared `FUN_140672f90` non-endemic caller guard while `$rng endemic giant` is active!
    const float nonEndemicVal = fnSharedRange(10.0f, 20.0f);
    if (nonEndemicVal <= 10.01f || nonEndemicVal >= 19.99f) {
        std::printf("FAIL [endemic guard]: non-endemic caller of 0x140672F90 was skewed to %.4f!\n", nonEndemicVal);
        return false;
    }

    plugin_host::DispatchPluginSubcommand("endemic", {"normal"}, "normal");
    const std::uint32_t crNorm = fnEndemicCapture(59, 0, &outScale);
    if (crNorm != 0U || outScale < 0.90f || outScale > 1.15f) {
        std::printf("FAIL [endemic normal]: expected crown=0 scale in [0.91,1.14], got crown=%u scale=%.3f\n", crNorm, outScale);
        return false;
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
        return false;
    }

    // 7d. Live `rAnimalSize` exact scale (`1.20x`) and exact cm (`23.6cm` on 20cm base -> `1.18x`)
    plugin_host::DispatchPluginSubcommand("endemic", {"clear"}, "clear");
    plugin_host::DispatchPluginSubcommand("endemic", {"exact", "1.20x"}, "exact 1.20x");
    const std::uint32_t cExScale = fnEndemicCapture(59, 0, &outScale);
    if (cExScale != 1U || std::fabs(outScale - 1.20f) > 0.015f) {
        std::printf("FAIL [endemic exact 1.20x]: got crown=%u scale=%.3f\n", cExScale, outScale);
        return false;
    }
    plugin_host::DispatchPluginSubcommand("endemic", {"exact", "23.6cm"}, "exact 23.6cm");
    const std::uint32_t cExCm = fnEndemicCapture(59, 0, &outScale);
    if (cExCm != 1U || std::fabs(outScale - 1.18f) > 0.015f) {
        std::printf("FAIL [endemic exact 23.6cm]: got crown=%u scale=%.3f\n", cExCm, outScale);
        return false;
    }
    plugin_host::DispatchPluginSubcommand("endemic", {"history"}, "history");
    plugin_host::DispatchPluginSubcommand("endemic", {"clear"}, "clear");
    std::printf("[OK] Endemic Life plugin verified on real 0x141669AC0/0x14166CCC0/0x140672F90 machine code (guard=%.3f)!\n",
                nonEndemicVal);

    return true;
}

bool RunSuite_PluginCrit(TestEnv& env) {
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

    // ------------------------------------------------------------------------
    // Test 22: Verify Official Plugin #6 (`rng_crit.dll`) on Real MHW 15.23 `0x141f51380` & `0x141f605f0`
    // ------------------------------------------------------------------------
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        static constexpr std::uint8_t kCritOrig5[5] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
        if (img[0x01F51380U] != 0xE9U) {
            std::printf("FAIL [crit-hook]: Expected 0xE9 detour at 0x01F51380, got 0x%02X\n", img[0x01F51380U]);
            return false;
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
            return false;
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
                return false;
            }

            // (b2) `kVanilla` (`crit off`) & `kObserve` (`crit observe`) under `kMhwRngModeTakeover` MUST
            //      return `kMhwRngActionContinue` (deterministic MasterDerived seed, immune to vanilla sInst),
            //      while `kMhwRngModeSelective` stays `kMhwRngActionBypass` (advances vanilla sInst).
            img[0x01F606EFU] = 0xC3;
            ::FlushInstructionCache(::GetCurrentProcess(), img + 0x01F606EFU, 1);

            const std::uint32_t prevMode = host->GetEngineMode();
            host->SetEngineMode(kMhwRngModeTakeover);
            for (const char* subMode : {"off", "observe"}) {
                plugin_host::DispatchPluginSubcommand("crit", {subMode}, subMode);
                host->ResetEpoch();
                fnSeedCall(fakeLocalPlayer, img + 0x01F606C1U);
                const std::uint32_t a1 = *reinterpret_cast<const std::uint32_t*>(fakeLocalPlayer + 0x8818U);
                fnSeedCall(fakeLocalPlayer, img + 0x01F606C1U);
                const std::uint32_t a2 = *reinterpret_cast<const std::uint32_t*>(fakeLocalPlayer + 0x8818U);

                // Perturb vanilla `MtRandom::sInstance` to prove TAKEOVER does not bypass to vanilla!
                sInst[0] ^= 0x13579BDFU;
                sInst[1] ^= 0x2468ACE0U;
                host->ResetEpoch();
                fnSeedCall(fakeLocalPlayer, img + 0x01F606C1U);
                const std::uint32_t b1 = *reinterpret_cast<const std::uint32_t*>(fakeLocalPlayer + 0x8818U);
                fnSeedCall(fakeLocalPlayer, img + 0x01F606C1U);
                const std::uint32_t b2 = *reinterpret_cast<const std::uint32_t*>(fakeLocalPlayer + 0x8818U);
                if (a1 != b1 || a2 != b2) {
                    img[0x01F606EFU] = origSeedEnd;
                    ::FlushInstructionCache(::GetCurrentProcess(), img + 0x01F606EFU, 1);
                    restoreCritTestMem();
                    std::printf("FAIL [crit-takeover-%s]: non-deterministic seed (0x%X,0x%X vs 0x%X,0x%X)\n",
                                subMode, a1, a2, b1, b2);
                    return false;
                }
            }

            // In SELECTIVE mode with `crit off`, `OnCritSeedPre` MUST still bypass to vanilla `MtRandom::sInstance`
            host->SetEngineMode(kMhwRngModeSelective);
            plugin_host::DispatchPluginSubcommand("crit", {"off"}, "off");
            const std::uint32_t sBefore[4] = {sInst[0], sInst[1], sInst[2], sInst[3]};
            fnSeedCall(fakeLocalPlayer, img + 0x01F606C1U);
            const bool vanillaStepped = (sInst[0] != sBefore[0] || sInst[1] != sBefore[1] ||
                                         sInst[2] != sBefore[2] || sInst[3] != sBefore[3]);

            img[0x01F606EFU] = origSeedEnd;
            ::FlushInstructionCache(::GetCurrentProcess(), img + 0x01F606EFU, 1);
            host->SetEngineMode(prevMode);
            plugin_host::DispatchPluginSubcommand("crit", {"random"}, "random");
            if (!vanillaStepped) {
                restoreCritTestMem();
                std::printf("FAIL [crit-selective-bypass]: expected vanilla sInst advance in SELECTIVE + crit off\n");
                return false;
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
            return false;
        }
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "0"}, "rate 0");
        runHit(&ctxLocal, 100.0f, 1U, res, mv); // Override beats force100!
        if (res != 0 || std::fabs(mv - 400.0f) > 0.01f) {
            restoreCritTestMem();
            std::printf("FAIL [crit-rate0]: res=%d mv=%.2f\n", res, mv);
            return false;
        }
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "-100"}, "rate -100");
        runHit(&ctxLocal, 100.0f, 0U, res, mv);
        if (res != -1 || std::fabs(mv - 300.0f) > 0.01f) {
            restoreCritTestMem();
            std::printf("FAIL [crit-rate-100]: res=%d mv=%.2f\n", res, mv);
            return false;
        }
        plugin_host::DispatchPluginSubcommand("crit", {"rate", "auto"}, "rate auto");
        runHit(&ctxLocal, 100.0f, 1U, res, mv);
        if (res != 1 || std::fabs(mv - 500.0f) > 0.01f) {
            restoreCritTestMem();
            std::printf("FAIL [crit-auto]: res=%d mv=%.2f\n", res, mv);
            return false;
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
            return false;
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
            return false;
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
                return false;
            }
        }
        plugin_host::DispatchPluginSubcommand("crit", {"ratio", "3:2", "spread"}, "ratio 3:2 spread");
        for (int i = 0; i < 10; ++i) {
            runHit(&ctxLocal, 50.0f, 0U, res, mv);
            if (res != kExpSpread[i]) {
                restoreCritTestMem();
                std::printf("FAIL [crit-ratio-spread]: idx=%d exp=%d got=%d\n", i, kExpSpread[i], res);
                return false;
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
                return false;
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
                return false;
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
                return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
        }

        // (j) Hot-unload restores exact 5-byte prologue & hot-reload re-hooks cleanly
        std::string critMsg;
        if (!plugin_host::UnloadSingle("crit", critMsg) ||
            std::memcmp(img + 0x01F51380U, kCritOrig5, 5) != 0) {
            restoreCritTestMem();
            std::printf("FAIL [crit-unload]: prologue at 0x01F51380 not restored (%s)\n", critMsg.c_str());
            return false;
        }
        if (!plugin_host::LoadOrReloadSingle("crit", critMsg) || img[0x01F51380U] != 0xE9U) {
            restoreCritTestMem();
            std::printf("FAIL [crit-reload]: failed to re-hook 0x01F51380 (%s)\n", critMsg.c_str());
            return false;
        }

        restoreCritTestMem();
        host->ClearAllRules();
        std::printf("[OK] Test 22: `rng_crit.dll` (remote passthrough, rate override, random/ratio/seq/mv_bank/mv_tier/bag/prd & hot unload/reload) verified!\n");
    }

    return true;
}

bool RunSuite_PluginEndemicRare(TestEnv& env) {
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
                return false;
            }
        }
        // Disabled 0.0f-prob row MUST NOT be falsely forced!
        setupSlot(0U, 58U, 0U, 0);
        *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.0f;
        fnMaster(synSAnimal, 1U, 58U, 0U, 0U, synRareArrHolder, 0U, 12.0f, 12.0f, 0U);
        if (synSAnimal[0x70A8U + 0xC9U] != 0xFFU) {
            restoreRareTestMem();
            std::printf("FAIL [rare-zero-prob]: disabled 0.0f row should stay unspawned!\n");
            return false;
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
                return false;
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
                return false;
            }
            setupSlot(1U, 25U, 0U, -1);
            *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.95f;
            fnMaster(synSAnimal, 1U, 25U, 0U, 0U, synRareArrHolder, 1U, 12.0f, 12.0f, 0U);
            if (synSAnimal[0x70A8U + 0xD0U + 0xC9U] != 0xFFU) {
                restoreRareTestMem();
                std::printf("FAIL [rare-sp-block]: ec025 spawned despite 0%% at trial %d\n", i);
                return false;
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
                return false;
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
                return false;
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
            return false;
        }
        *reinterpret_cast<float*>(synRareMaster + 0x0CU) = 0.95f;
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "ec025", "0%"}, "rare ec025 0%");
        fnGroup(synGroupCtrl);
        if (synGroupCtrl[0x334U] != 0U) {
            restoreRareTestMem();
            std::printf("FAIL [rare-group-block]: expected ctrl[0x334]=0, got %u\n", synGroupCtrl[0x334U]);
            return false;
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
                return false;
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
                return false;
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
                return false;
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
            return false;
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
                return false;
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
                return false;
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
                return false;
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
                return false;
            }
        }

        restoreRareTestMem();
        MhwRng_GetHostApi()->ClearAllRules();
        std::printf("[OK] Test 23: `rng_endemic.dll` v1.3.0 (master rare, GL rare Moly, group rare, mutex tie-breaker & async log) verified!\n");
    }

    return true;
}

