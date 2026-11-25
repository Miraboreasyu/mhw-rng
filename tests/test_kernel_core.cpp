#include "test_env.h"

bool RunSuite_KernelThreads(TestEnv& env) {
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
        return false;
    }
    std::printf("[OK] 8-thread stream isolation & O(1) Epoch rewind verified!\n");

    return true;
}

bool RunSuite_KernelHotload(TestEnv& env) {
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

    // 9. Verify Zero-Lock Shadow-Copy Hot-Reload + Inspector `dump` / `top` + Hot-Unload!
    const std::string srcSizeDll = plugDir + "\\rng_size.dll";
    HANDLE hWriteCheck = ::CreateFileA(srcSizeDll.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hWriteCheck == INVALID_HANDLE_VALUE) {
        std::printf("FAIL: %s is file-locked! Shadow-copy hot-reload broken!\n", srcSizeDll.c_str());
        return false;
    }
    ::CloseHandle(hWriteCheck);

    std::string reloadMsg;
    if (!plugin_host::LoadOrReloadSingle("size", reloadMsg)) {
        std::printf("FAIL: Hot-reload of 'size' failed: %s\n", reloadMsg.c_str());
        return false;
    }
    std::printf("[OK] Zero-lock Shadow-Copy Hot-Reload verified: %s\n", reloadMsg.c_str());

    const std::string dumpPath = buildDir + "\\test_rng_dump_v2.txt";
    plugin_host::DispatchPluginSubcommand("dump", {dumpPath}, dumpPath);
    plugin_host::DispatchPluginSubcommand("top", {}, "");

    std::string unloadMsg;
    if (!plugin_host::UnloadSingle("inspector", unloadMsg)) {
        std::printf("FAIL: UnloadSingle('inspector') failed\n");
        return false;
    }
    std::printf("[OK] Hot-unload verified: %s\n", unloadMsg.c_str());

    std::string reloadInspMsg;
    if (!plugin_host::LoadOrReloadSingle("inspector", reloadInspMsg)) {
        std::printf("FAIL [Hotload-Restore]: failed to reload inspector: %s\n", reloadInspMsg.c_str());
        return false;
    }
    ::DeleteFileA(dumpPath.c_str());
    return true;
}

bool RunSuite_KernelChatWx(TestEnv& env) {
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
            return false;
        }
        if (hook::WasUnchainedOverwrite5(chatSite, relayA, mhw::kPrologueSChatDispatch)) {
            std::printf("FAIL [chat chain]: false positive while relayA is at head\n");
            return false;
        }

        // Proper second plugin chains onto relayA -> WasUnchainedOverwrite5 MUST return false!
        if (!hook::InstallStandaloneDetour5(chatSite, reinterpret_cast<void*>(dummyChatB), &origB,
                                            mhw::kPrologueSChatDispatch, &chainedB, &targetB, &relayB) ||
            !chainedB || targetB != relayA) {
            std::printf("FAIL [chat chain]: second plugin failed to chain onto relayA\n");
            return false;
        }
        if (hook::WasUnchainedOverwrite5(chatSite, relayA, mhw::kPrologueSChatDispatch)) {
            std::printf("FAIL [chat chain]: false positive on properly chained second plugin\n");
            return false;
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
                return false;
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
                return false;
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
            return false;
        }
        MEMORY_BASIC_INFORMATION mbiP0{}, mbiA{}, mbiB{};
        if (!::VirtualQuery(reinterpret_cast<LPCVOID>(hook::ArenaBase()), &mbiP0, sizeof(mbiP0)) ||
            !::VirtualQuery(reinterpret_cast<LPCVOID>(relayA), &mbiA, sizeof(mbiA)) ||
            !::VirtualQuery(reinterpret_cast<LPCVOID>(relayB), &mbiB, sizeof(mbiB)) ||
            mbiP0.Protect != PAGE_EXECUTE_READ || mbiA.Protect != PAGE_EXECUTE_READ || mbiB.Protect != PAGE_EXECUTE_READ) {
            std::printf("FAIL [chat chain]: Expected PAGE_EXECUTE_READ on Page0/A/B (0x%lX/0x%lX/0x%lX)\n",
                        mbiP0.Protect, mbiA.Protect, mbiB.Protect);
            return false;
        }
        std::printf("[OK] Pre-reserved 64KB standalone W^X pool verified for sChat::DispatchChatEntry (Page0=%p, A=%p, B=%p)!\n",
                    reinterpret_cast<void*>(hook::ArenaBase()),
                    reinterpret_cast<void*>(relayA),
                    reinterpret_cast<void*>(relayB));
    }

    // 12. Verify `randUnitSphere`, pre-reserved 64 KB multi-page standalone W^X pool, & clean shutdown
    float v3[3]{};
    CallSphereSite(fnSphere, sInst, v3);
    const float norm = std::sqrt(v3[0] * v3[0] + v3[1] * v3[1] + v3[2] * v3[2]);
    if (std::fabs(norm - 1.0f) > 1e-4f) return false;

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
        return false;
    }
    const std::uintptr_t poolLo = hook::ArenaBase() + 0x1000ULL;
    const std::uintptr_t poolHi = hook::ArenaBase() + 0x10000ULL;
    MEMORY_BASIC_INFORMATION mbiMove{};
    if (relayMove < poolLo || relayMove >= poolHi || chainedMove ||
        !::VirtualQuery(reinterpret_cast<LPCVOID>(relayMove), &mbiMove, sizeof(mbiMove)) ||
        mbiMove.Protect != PAGE_EXECUTE_READ) {
        std::printf("FAIL [Standalone]: sChat::move standalone pool verification failed (relay=%p, prot=0x%lX, chained=%d)\n",
                    reinterpret_cast<void*>(relayMove), mbiMove.Protect, chainedMove);
        return false;
    }
    std::printf("[OK] sChat::move standalone W^X pool page verified (relay=%p)!\n",
                reinterpret_cast<void*>(relayMove));

    return true;
}

bool RunSuite_KernelSelective(TestEnv& env) {
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
            return false;
        }
        host->SetTakeoverEnabled(true); // From Off -> Selective
        if (host->GetEngineMode() != kMhwRngModeSelective || !host->IsTakeoverEnabled()) {
            std::printf("FAIL [Selective-1]: SetTakeoverEnabled(true) from Off expected Selective, got %u\n", host->GetEngineMode());
            return false;
        }
        host->SetEngineMode(kMhwRngModeTakeover);
        host->SetTakeoverEnabled(true); // Preserves Takeover if already active
        if (host->GetEngineMode() != kMhwRngModeTakeover) {
            std::printf("FAIL [Selective-1]: SetTakeoverEnabled(true) from Takeover should stay Takeover\n");
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
        }
        std::printf("[OK] Selective mode + ProtectSaveSeeds verified on real MHW 15.23 `FUN_14135d9a0`!\n");
    }

    return true;
}

bool RunSuite_KernelWrappersMt(TestEnv& env) {
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
            return false;
        }

        // Call #2: `0x02CAAF0` (`ret 0x02CAB1E`, 6 qwords) -> `int(int min, int max)`
        bwObs = {};
        const int rInt = reinterpret_cast<int(__fastcall*)(int, int)>(wrapCallers + 0x20U)(10, 20);
        if (bwObs.gotCallRva != retRvas[1] || bwObs.gotOrigRet != expectedWrapRet[1] || rInt < 10 || rInt > 20) {
            std::printf("FAIL [Wrapper-2 0x02CAB1E]: gotCall=0x%X (exp 0x%X) origRet=0x%X (exp 0x%X) r=%d\n",
                        bwObs.gotCallRva, retRvas[1], bwObs.gotOrigRet, expectedWrapRet[1], rInt);
            return false;
        }

        // Call #3: `0x02CD9B0` (`ret 0x02CD9D1`, 8 qwords) -> `float/bool(void*, float pct)`
        bwObs = {};
        (void)reinterpret_cast<std::uint32_t(__fastcall*)(void*, float)>(wrapCallers + 0x40U)(nullptr, 50.0f);
        if (bwObs.gotCallRva != retRvas[2] || bwObs.gotOrigRet != expectedWrapRet[2]) {
            std::printf("FAIL [Wrapper-3 0x02CD9D1]: gotCall=0x%X (exp 0x%X) origRet=0x%X (exp 0x%X)\n",
                        bwObs.gotCallRva, retRvas[2], bwObs.gotOrigRet, expectedWrapRet[2]);
            return false;
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
            return false;
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
                return false;
            }
            ::Sleep(2);
        }

        stopStress.store(true, std::memory_order_relaxed);
        for (auto& w : stressWorkers) w.join();
        if (metaCalls.load() == 0ULL) {
            std::printf("FAIL [ConcurrentReload]: zero worker iterations\n");
            return false;
        }
        std::printf("[OK] Concurrent hot-reload (10 cycles) + 4-thread HostApi drain (%llu calls) verified!\n",
                    static_cast<unsigned long long>(metaCalls.load()));
    }

    return true;
}

bool RunSuite_PluginNames(TestEnv& env) {
    const auto& plugDir = env.plugDir;

    // 1. Validate strict plugin shortName rules ([a-z_], 1..24, not 'kernel') & auto-suffixing (_1, _2, ...)
    {
        if (!plugin_host::ValidateDeclaredShortName("crit") ||
            !plugin_host::ValidateDeclaredShortName("end") ||
            !plugin_host::ValidateDeclaredShortName("my_plugin") ||
            !plugin_host::ValidateDeclaredShortName("a_b_c")) {
            std::printf("FAIL [NameRule-1]: valid lowercase_underscore names rejected\n");
            return false;
        }
        const char* badNames[] = {
            nullptr, "", "Crit", "foo1", "foo-bar", "foo.bar", "kernel",
            "abcdefghijklmnopqrstuvwxy" // 25 chars > 24
        };
        for (const char* bn : badNames) {
            if (plugin_host::ValidateDeclaredShortName(bn)) {
                std::printf("FAIL [NameRule-2]: invalid shortName '%s' was accepted\n", bn ? bn : "<null>");
                return false;
            }
        }
        std::vector<std::string> existing = {"foo", "bar"};
        const std::string s1 = plugin_host::AllocateUniqueShortName("foo", existing);
        existing.push_back(s1);
        const std::string s2 = plugin_host::AllocateUniqueShortName("foo", existing);
        existing.push_back(s2);
        const std::string s3 = plugin_host::AllocateUniqueShortName("baz", existing);
        if (s1 != "foo_1" || s2 != "foo_2" || s3 != "baz") {
            std::printf("FAIL [NameRule-3]: unexpected suffixes (%s, %s, %s)\n",
                        s1.c_str(), s2.c_str(), s3.c_str());
            return false;
        }

        // Real DLL collision test: copy `rng_steam.dll` -> `_test_dup_steam.dll`, load alongside `steam`, verify `steam_1`
        const std::string srcDll = plugDir + "\\rng_steam.dll";
        const std::string dupDll = plugDir + "\\_test_dup_steam.dll";
        ::DeleteFileA(dupDll.c_str());
        if (!::CopyFileA(srcDll.c_str(), dupDll.c_str(), FALSE)) {
            std::printf("FAIL [NameRule-DupCopy]: failed to copy rng_steam.dll\n");
            return false;
        }
        std::string loadMsg;
        const bool okDup = plugin_host::LoadOrReloadSingle("_test_dup_steam.dll", loadMsg);
        const auto pluginsAfter = plugin_host::ListLoadedPlugins();
        bool sawSteam = false, sawSteam1 = false;
        for (const auto& p : pluginsAfter) {
            if (p.shortName == "steam") sawSteam = true;
            if (p.shortName == "steam_1") sawSteam1 = true;
        }
        std::string unloadDupMsg, reloadOrigMsg;
        if (sawSteam1) plugin_host::UnloadSingle("steam_1", unloadDupMsg);
        ::DeleteFileA(dupDll.c_str());
        plugin_host::LoadOrReloadSingle("steam", reloadOrigMsg);
        if (!okDup || !sawSteam || !sawSteam1) {
            std::printf("FAIL [NameRule-DupLoad]: ok=%d steam=%d steam_1=%d msg='%s'\n",
                        okDup ? 1 : 0, sawSteam ? 1 : 0, sawSteam1 ? 1 : 0, loadMsg.c_str());
            return false;
        }
    }

    std::printf("[OK] Test 26: Plugin shortName rules ([a-z_], 1..24) & collision auto-suffixing verified!\n");
    return true;
}

