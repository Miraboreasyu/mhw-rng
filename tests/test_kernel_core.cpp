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

bool RunSuite_KernelBeacon(TestEnv& env) {
    const auto base = env.base;
    auto&      cfg  = env.cfg;

    // 2. Codec round-trip across full alphabet, uniform 27-bit distribution, & 100k noise false-positive rejection
    {
        const std::string alpha = "abcdefghijklmnopqrstuvwxyz_0123456789,";
        for (std::size_t i = 0; i < alpha.size(); ++i) {
            std::uint8_t sym = 0U;
            if (!peer_beacon::EncodeSymbolChar(alpha[i], sym)) {
                std::printf("FAIL [Codec-Alpha]: encode failed for '%c'\n", alpha[i]);
                return false;
            }
            char back = '\0';
            if (!peer_beacon::DecodeSymbolChar(sym, back) || back != alpha[i]) {
                std::printf("FAIL [Codec-Alpha]: decode mismatch for '%c'\n", alpha[i]);
                return false;
            }
            const std::uint32_t s27 = peer_beacon::PackFrame27(static_cast<std::uint8_t>(i & 31U),
                                                               static_cast<std::uint8_t>(i & 3U),
                                                               static_cast<std::uint8_t>(i),
                                                               sym);
            std::uint8_t g = 0U, p = 0U, s = 0U;
            if (s27 == 0U || s27 > 0x07FFFFFFU || !peer_beacon::UnpackFrame27(s27, g, p, s) ||
                g != (i & 3U) || p != i || s != sym) {
                std::printf("FAIL [Codec-RoundTrip]: idx=%zu s27=0x%X\n", i, s27);
                return false;
            }
        }
        // kSymEnd round-trip
        {
            const std::uint32_t endFrame = peer_beacon::PackFrame27(17U, 2U, 15U, peer_beacon::kSymEnd);
            std::uint8_t g = 0U, p = 0U, s = 0U;
            if (!peer_beacon::UnpackFrame27(endFrame, g, p, s) || g != 2U || p != 15U || s != peer_beacon::kSymEnd) {
                std::printf("FAIL [Codec-End]: end frame round-trip failed\n");
                return false;
            }
        }
        // Uniformity check on all 27 bits across 10k SplitMix samples
        std::uint32_t bitOnes[27]{};
        std::uint64_t rngState = 0x123456789ABCDEF0ULL;
        for (int i = 0; i < 10000; ++i) {
            rngState += 0x9E3779B97F4A7C15ULL;
            std::uint64_t z = rngState;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z ^= z >> 31;
            const auto nonce = static_cast<std::uint8_t>((z >> 11) & 0x1FU);
            const auto gen   = static_cast<std::uint8_t>(z & 0x03U);
            const auto pos   = static_cast<std::uint8_t>((z >> 2) & 0x3FU);
            const auto sym   = static_cast<std::uint8_t>((z >> 16) % (peer_beacon::kSymEnd + 1U));
            const std::uint32_t f = peer_beacon::PackFrame27(nonce, gen, pos, sym);
            for (int b = 0; b < 27; ++b) {
                if ((f & (1U << b)) != 0U) ++bitOnes[b];
            }
        }
        for (int b = 0; b < 27; ++b) {
            if (bitOnes[b] < 4500U || bitOnes[b] > 5500U) {
                std::printf("FAIL [Codec-Bias]: bit %d ones=%u/10000\n", b, bitOnes[b]);
                return false;
            }
        }
    }

    // 3. End-to-end synthetic sPlayer/sMhNetwork + hidden check/self_check + deduplication + stealth tests
    static std::vector<std::string> g_beaconAlerts;
    g_beaconAlerts.clear();
    auto beaconAlertSink = +[](const char* msg) noexcept {
        if (msg) {
            g_beaconAlerts.emplace_back(msg);
            console::PushOutputLine(msg);
        }
    };

    // Synthetic sMhNetwork + sPlayer layout inside mapped env.img
    std::vector<std::uint8_t> fakeNet(0x60000U, 0U);
    std::vector<std::uint8_t> fakeSessionObj0(0x20U, 0U);
    std::vector<std::uint8_t> fakeSessionInner(0xC020U, 0U);
    fakeSessionInner[mhw::kOffNetSessionOnlineByte] = 1U;
    *reinterpret_cast<std::uintptr_t*>(fakeSessionObj0.data() + mhw::kOffNetSessionInner) =
        reinterpret_cast<std::uintptr_t>(fakeSessionInner.data());
    *reinterpret_cast<std::int32_t*>(fakeNet.data() + mhw::kOffNetActiveMode) = 1;
    *reinterpret_cast<std::uintptr_t*>(fakeNet.data() + mhw::kOffNetSessionObj0) =
        reinterpret_cast<std::uintptr_t>(fakeSessionObj0.data());
    *reinterpret_cast<std::uintptr_t*>(fakeNet.data() + mhw::kOffNetSessionObj1) =
        reinterpret_cast<std::uintptr_t>(fakeSessionObj0.data());
    std::snprintf(reinterpret_cast<char*>(fakeNet.data() + mhw::kOffNetQuestNames + 0 * mhw::kStrideNetMemberName),
                  36, "LocalTester");
    std::snprintf(reinterpret_cast<char*>(fakeNet.data() + mhw::kOffNetQuestNames + 1 * mhw::kStrideNetMemberName),
                  36, "TeammateAlice");

    std::vector<std::uint8_t> fakeSPlayer(0xB000U, 0U);
    std::vector<std::uint8_t> fakeLocalUPlayer(0x9000U, 0U);
    std::vector<std::uint8_t> fakeRemoteUPlayer(0x9000U, 0U);

    *reinterpret_cast<std::uint32_t*>(fakeLocalUPlayer.data() + mhw::kOffUPlayerFlags)      = 2U;
    fakeLocalUPlayer[mhw::kOffUPlayerLocalFlags]                                           = 1U;
    *reinterpret_cast<std::uint32_t*>(fakeRemoteUPlayer.data() + mhw::kOffUPlayerFlags)     = 2U;
    fakeRemoteUPlayer[mhw::kOffUPlayerLocalFlags]                                          = 0U;

    const auto setPlayerSlot = [&](std::size_t idx, std::uintptr_t ent, std::int32_t qIdx) {
        std::uint8_t* pInfo = fakeSPlayer.data() + mhw::kOffSPlayerInfoArray + idx * mhw::kStridePlayerInfo;
        *reinterpret_cast<std::int32_t*>(pInfo + mhw::kOffPlayerInfoQuestIdx) = qIdx;
        *reinterpret_cast<std::int32_t*>(pInfo + mhw::kOffPlayerInfoLobbyIdx) = -1;
        *reinterpret_cast<std::uintptr_t*>(pInfo + mhw::kOffPlayerInfoEntity) = ent;
    };
    setPlayerSlot(0U, reinterpret_cast<std::uintptr_t>(fakeLocalUPlayer.data()), 0);
    setPlayerSlot(1U, reinterpret_cast<std::uintptr_t>(fakeRemoteUPlayer.data()), 1);
    // Inject a dangling invalid entity pointer into inactive slot 2 (qIdx=-1, lIdx=-1)
    // to prove CollectPlayers per-slot SEH guard never aborts slots 0/1!
    {
        std::uint8_t* badSlot = fakeSPlayer.data() + mhw::kOffSPlayerInfoArray + 2U * mhw::kStridePlayerInfo;
        *reinterpret_cast<std::int32_t*>(badSlot + mhw::kOffPlayerInfoQuestIdx) = -1;
        *reinterpret_cast<std::int32_t*>(badSlot + mhw::kOffPlayerInfoLobbyIdx) = -1;
        *reinterpret_cast<std::uintptr_t*>(badSlot + mhw::kOffPlayerInfoEntity) = 0x0000000012345670ULL;
    }

    auto cleanupSynth = [&]() {
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerInstance)    = 0ULL;
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerRoot)        = 0ULL;
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerFallback)    = 0ULL;
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSQuestInstance)     = 0ULL;
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSMhNetworkInstance) = 0ULL;
        peer_beacon::SetVideoMarkDebugVisible(false);
        peer_beacon::Init(true, false, nullptr);
        cfg.checkEnabled     = true;
        cfg.selfCheckEnabled = false;
        MhwRng_SetLanguage("zh");
        cmd_router::Init(&cfg, &CaptureChatSink);
    };

    *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerInstance) =
        reinterpret_cast<std::uintptr_t>(fakeSPlayer.data());
    *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSMhNetworkInstance) =
        reinterpret_cast<std::uintptr_t>(fakeNet.data());

    // Default state: check = ON, self_check = OFF
    peer_beacon::Init(true, false, beaconAlertSink);
    if (!peer_beacon::IsCheckEnabled() || peer_beacon::IsSelfCheckEnabled()) {
        cleanupSynth();
        std::printf("FAIL [Beacon-Defaults]: unexpected default flags\n");
        return false;
    }

    // 100,000 random noise 27-bit values must produce ZERO false detections
    {
        std::uint64_t nst = 0xDEADBEEFCAFE1234ULL;
        for (int i = 0; i < 100000; ++i) {
            nst = nst * 6364136223846793005ULL + 1442695040888963407ULL;
            const std::uint32_t r27 = static_cast<std::uint32_t>((nst >> 37) & 0x07FFFFFFU);
            *reinterpret_cast<std::uint32_t*>(fakeRemoteUPlayer.data() + mhw::kOffUPlayerSeed27) =
                (r27 == 0U) ? 1U : r27;
            peer_beacon::TickOnMainThread(base);
        }
        if (!g_beaconAlerts.empty()) {
            cleanupSynth();
            std::printf("FAIL [Beacon-Noise]: false positive count=%zu ('%s')\n",
                        g_beaconAlerts.size(), g_beaconAlerts[0].c_str());
            return false;
        }
    }

    // Test hidden `self_check` + live emission from RVA 0x01F606D6 + end-tag deduplication
    {
        console::Reset();
        console::Init(&cfg);
        cmd_router::Init(&cfg, &CaptureChatSink);
        g_capturedChatLines.clear();
        if (!cmd_router::HandleCommandLine("$rng lang en", true) ||
            !cmd_router::HandleCommandLine("$rng self_check on", true) ||
            !peer_beacon::IsSelfCheckEnabled()) {
            cleanupSynth();
            std::printf("FAIL [Beacon-SelfCmd]: `$rng self_check on` failed\n");
            return false;
        }

        // Offline solo test: mark net offline and confirm self_check STILL emits & detects!
        fakeSessionInner[mhw::kOffNetSessionOnlineByte] = 0U;
        peer_beacon::TickOnMainThread(base); // establish baseline local player ptr

        // Taint two plugins (`crit` via `$rng crit random` claiming 0x01F606D6, and `size` directly)
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        MhwRngPluginId sizeId = 0U;
        for (const auto& p : plugin_host::ListLoadedPlugins()) {
            if (p.shortName == "size") sizeId = p.id;
        }
        host->SetEngineMode(kMhwRngModeSelective);
        plugin_host::DispatchPluginSubcommand("crit", {"random"}, "random");
        peer_beacon::MarkTainted(sizeId);

        static constexpr std::uint8_t kSeedThunk[] = {
            0x53, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xD9, 0xFF, 0xD2, 0x48, 0x83, 0xC4, 0x28, 0x5B, 0xC3
        };
        std::memcpy(env.thunkMem + 0x280U, kSeedThunk, sizeof(kSeedThunk));
        ::FlushInstructionCache(::GetCurrentProcess(), env.thunkMem + 0x280U, sizeof(kSeedThunk));
        using SeedCallFn = void(__fastcall*)(void* player, void* addr);
        const auto fnSeedCall = reinterpret_cast<SeedCallFn>(env.thunkMem + 0x280U);
        const std::uint8_t origEnd = env.img[0x01F606EFU];
        env.img[0x01F606EFU] = 0xC3;
        ::FlushInstructionCache(::GetCurrentProcess(), env.img + 0x01F606EFU, 1);

        g_beaconAlerts.clear();
        for (int step = 0; step < 60; ++step) {
            fnSeedCall(fakeLocalUPlayer.data(), env.img + 0x01F606C1U);
            peer_beacon::TickOnMainThread(base);
        }

        env.img[0x01F606EFU] = origEnd;
        ::FlushInstructionCache(::GetCurrentProcess(), env.img + 0x01F606EFU, 1);
        plugin_host::DispatchPluginSubcommand("crit", {"off"}, "off");

        if (g_beaconAlerts.size() != 1U ||
            g_beaconAlerts[0].find("LocalTester") == std::string::npos ||
            g_beaconAlerts[0].find("crit,size") == std::string::npos ||
            g_beaconAlerts[0].find("SelfCheck") == std::string::npos) {
            cleanupSynth();
            std::printf("FAIL [Beacon-SelfEmit]: alerts=%zu first='%s'\n",
                        g_beaconAlerts.size(), g_beaconAlerts.empty() ? "" : g_beaconAlerts[0].c_str());
            return false;
        }

        // Verify `self_check status` and `check status` in EN (no CJK) & ZH
        g_capturedChatLines.clear();
        if (!cmd_router::HandleCommandLine("$rng self_check status", true) ||
            !cmd_router::HandleCommandLine("$rng check status", true) ||
            g_capturedChatLines.size() < 2U ||
            g_capturedChatLines[0].find("TX:") == std::string::npos ||
            g_capturedChatLines[0].find("crit,size") == std::string::npos ||
            g_capturedChatLines[0].find("Local:OK") == std::string::npos) {
            cleanupSynth();
            std::printf("FAIL [Beacon-StatusDiagEn]: lines=%zu first='%s'\n",
                        g_capturedChatLines.size(), g_capturedChatLines.empty() ? "" : g_capturedChatLines[0].c_str());
            return false;
        }
        cmd_router::HandleCommandLine("$rng lang zh", true);
        g_capturedChatLines.clear();
        cmd_router::HandleCommandLine("$rng self_check status", true);
        if (g_capturedChatLines.empty() || g_capturedChatLines[0].find("crit,size") == std::string::npos) {
            cleanupSynth();
            std::printf("FAIL [Beacon-StatusDiagZh]: missing active taint in zh status\n");
            return false;
        }
        cmd_router::HandleCommandLine("$rng lang en", true);

        // Verify local player fallback via `sPlayerRoot` when `sPlayerInstance` is null
        std::vector<std::uint8_t> fakeRoot(0x80U, 0U);
        *reinterpret_cast<std::uintptr_t*>(fakeRoot.data() + mhw::kOffPlayerRootEntity) =
            reinterpret_cast<std::uintptr_t>(fakeLocalUPlayer.data());
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerInstance) = 0ULL;
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerRoot)     =
            reinterpret_cast<std::uintptr_t>(fakeRoot.data());
        peer_beacon::TickOnMainThread(base);
        const peer_beacon::DiagSnapshot rootDiag = peer_beacon::GetDiagSnapshot();
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerRoot)     = 0ULL;
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerInstance) =
            reinterpret_cast<std::uintptr_t>(fakeSPlayer.data());
        if (!rootDiag.localFound) {
            cleanupSynth();
            std::printf("FAIL [Beacon-RootFallback]: sPlayerRoot local fallback failed\n");
            return false;
        }

        host->ClearAllRules();
    }

    // Test teammate detection (`check on` vs `check off`) & out-of-order reassembly + dynamic change
    {
        fakeSessionInner[mhw::kOffNetSessionOnlineByte] = 1U;
        peer_beacon::ResetDecoders();
        g_beaconAlerts.clear();

        const std::string list1 = "crit,thk";
        const std::uint8_t gen1 = peer_beacon::ComputeListGen2(list1.c_str(), list1.size());
        // Emit out of order: end first, then odd positions, then even positions (twice to verify no re-spam!)
        std::vector<std::uint32_t> frames;
        frames.push_back(peer_beacon::PackFrame27(3U, gen1, static_cast<std::uint8_t>(list1.size()), peer_beacon::kSymEnd));
        for (std::size_t i = 1U; i < list1.size(); i += 2U) {
            std::uint8_t sym = 0U;
            peer_beacon::EncodeSymbolChar(list1[i], sym);
            frames.push_back(peer_beacon::PackFrame27(static_cast<std::uint8_t>(i & 31U), gen1, static_cast<std::uint8_t>(i), sym));
        }
        for (std::size_t i = 0U; i < list1.size(); i += 2U) {
            std::uint8_t sym = 0U;
            peer_beacon::EncodeSymbolChar(list1[i], sym);
            frames.push_back(peer_beacon::PackFrame27(static_cast<std::uint8_t>((i + 7U) & 31U), gen1, static_cast<std::uint8_t>(i), sym));
        }
        for (int repeat = 0; repeat < 3; ++repeat) {
            for (std::uint32_t f : frames) {
                *reinterpret_cast<std::uint32_t*>(fakeRemoteUPlayer.data() + mhw::kOffUPlayerSeed27) = f;
                peer_beacon::TickOnMainThread(base);
            }
        }
        if (g_beaconAlerts.size() != 1U ||
            g_beaconAlerts[0].find("TeammateAlice") == std::string::npos ||
            g_beaconAlerts[0].find("crit,thk") == std::string::npos) {
            cleanupSynth();
            std::printf("FAIL [Beacon-Remote]: alerts=%zu first='%s'\n",
                        g_beaconAlerts.size(), g_beaconAlerts.empty() ? "" : g_beaconAlerts[0].c_str());
            return false;
        }

        // Now turn `check off`, change teammate plugin list, and verify NO alert is emitted!
        cmd_router::HandleCommandLine("$rng check off", true);
        const std::string list2 = "endemic,thk";
        const std::uint8_t gen2 = peer_beacon::ComputeListGen2(list2.c_str(), list2.size());
        for (std::size_t i = 0U; i <= list2.size(); ++i) {
            std::uint8_t sym = peer_beacon::kSymEnd;
            if (i < list2.size()) peer_beacon::EncodeSymbolChar(list2[i], sym);
            *reinterpret_cast<std::uint32_t*>(fakeRemoteUPlayer.data() + mhw::kOffUPlayerSeed27) =
                peer_beacon::PackFrame27(static_cast<std::uint8_t>(i & 31U), gen2, static_cast<std::uint8_t>(i), sym);
            peer_beacon::TickOnMainThread(base);
        }
        if (g_beaconAlerts.size() != 1U) {
            cleanupSynth();
            std::printf("FAIL [Beacon-CheckOff]: expected suppressed alert when check off (got %zu)\n", g_beaconAlerts.size());
            return false;
        }
    }

    // 3b. Synthetic sQuest boundary & in-place restart reset (without clearing spawn-time taint!)
    {
        std::vector<std::uint8_t> fakeQuest(0x14000U, 0U);
        auto setFakeQuest = [&](bool inQ, std::uint32_t qid, std::int32_t elapsed) {
            *reinterpret_cast<std::uint32_t*>(fakeQuest.data() + mhw::kOffQuestState)        = inQ ? 2U : 0U;
            *reinterpret_cast<std::uint32_t*>(fakeQuest.data() + mhw::kOffQuestId)           = qid;
            *reinterpret_cast<std::uint32_t*>(fakeQuest.data() + mhw::kOffQuestCategory)     = inQ ? 2U : 0U;
            *reinterpret_cast<std::int32_t*>(fakeQuest.data() + mhw::kOffQuestInitRemTicks)  = 30000;
            *reinterpret_cast<std::int32_t*>(fakeQuest.data() + mhw::kOffQuestRemTicks)      = 30000 - elapsed;
        };

        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSQuestInstance) =
            reinterpret_cast<std::uintptr_t>(fakeQuest.data());
        fakeSessionInner[mhw::kOffNetSessionOnlineByte] = 1U;
        cmd_router::HandleCommandLine("$rng check on", true);
        peer_beacon::ResetDecoders();
        g_beaconAlerts.clear();

        // Spawn-time taint (e.g. `size` marked before quest transitions `false -> true`) MUST survive!
        MhwRngPluginId sizeId = 0U;
        for (const auto& p : plugin_host::ListLoadedPlugins()) {
            if (p.shortName == "size") sizeId = p.id;
        }
        peer_beacon::MarkTainted(sizeId);

        setFakeQuest(false, 0U, 0);
        *reinterpret_cast<std::uint32_t*>(fakeRemoteUPlayer.data() + mhw::kOffUPlayerSeed27) = 0U;
        peer_beacon::TickOnMainThread(base);

        setFakeQuest(true, 101U, 200);
        peer_beacon::TickOnMainThread(base);
        if (!peer_beacon::HasAnyTaint() || !peer_beacon::IsVideoMarkActive()) {
            cleanupSynth();
            std::printf("FAIL [Quest-TaintKeep]: pre-quest spawn taint wiped on quest entry\n");
            return false;
        }

        const std::string qList = "crit,thk";
        const std::uint8_t qGen = peer_beacon::ComputeListGen2(qList.c_str(), qList.size());
        auto sendRemoteListOnce = [&]() {
            for (std::size_t i = 0U; i <= qList.size(); ++i) {
                std::uint8_t sym = peer_beacon::kSymEnd;
                if (i < qList.size()) peer_beacon::EncodeSymbolChar(qList[i], sym);
                *reinterpret_cast<std::uint32_t*>(fakeRemoteUPlayer.data() + mhw::kOffUPlayerSeed27) =
                    peer_beacon::PackFrame27(static_cast<std::uint8_t>((i + 3U) & 31U), qGen, static_cast<std::uint8_t>(i), sym);
                peer_beacon::TickOnMainThread(base);
            }
        };
        sendRemoteListOnce();
        sendRemoteListOnce(); // repeated frames in Quest 1 must NOT re-spam
        if (g_beaconAlerts.size() != 1U) {
            cleanupSynth();
            std::printf("FAIL [Quest-AlertOnce]: expected 1 alert in quest 1, got %zu\n", g_beaconAlerts.size());
            return false;
        }

        // Simulated in-place quest restart (elapsed rewinds 200 -> 10 without changing uPlayer*)
        setFakeQuest(true, 101U, 10);
        sendRemoteListOnce();
        if (g_beaconAlerts.size() != 2U) {
            cleanupSynth();
            std::printf("FAIL [Quest-Restart]: expected alert again after quest restart, got %zu\n", g_beaconAlerts.size());
            return false;
        }
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSQuestInstance) = 0ULL;
        peer_beacon::ClearAllTaint();
    }

    // 3c. Deterministic video watermark grid, sinusoidal inversion, CRC-12, and gating checks
    {
        peer_beacon::VideoMarkFrame f{};
        peer_beacon::VideoMarkFrame fFlip{};
        peer_beacon::VideoMarkFrame fZero{};
        if (!peer_beacon::BuildVideoMarkFrameAt(0.0625, "crit,thk", false, f) || !f.active ||
            !peer_beacon::BuildVideoMarkFrameAt(0.1875, "crit,thk", false, fFlip) || !fFlip.active ||
            peer_beacon::BuildVideoMarkFrameAt(0.125, "crit,thk", false, fZero)) {
            cleanupSynth();
            std::printf("FAIL [Wm-Build]: unexpected frame sinusoidal behavior\n");
            return false;
        }
        int pilotCells = 0, ch1Cells = 0, ch2Cells = 0;
        static constexpr std::uint8_t kChMap[8] = { 0U, 1U, 1U, 2U, 0U, 2U, 2U, 1U };
        for (std::size_t r = 0U; r < peer_beacon::kWmGridRows; ++r) {
            for (std::size_t c = 0U; c < peer_beacon::kWmGridCols; ++c) {
                const std::size_t cid = r * peer_beacon::kWmGridCols + c;
                const std::uint8_t ch = kChMap[(r * 3U + c) & 7U];
                if (ch == 0U) ++pilotCells;
                else if (ch == 1U) ++ch1Cells;
                else ++ch2Cells;

                const float w0 = f.cellWeights[cid];
                const float w1 = fFlip.cellWeights[cid];
                if (std::fabs(std::fabs(w0) - 1.0f) > 1e-4f || std::fabs(w0 + w1) > 1e-4f) {
                    cleanupSynth();
                    std::printf("FAIL [Wm-CellWeight]: cid=%zu w0=%.5f w1=%.5f\n", cid, w0, w1);
                    return false;
                }
            }
        }
        if (pilotCells != 48 || ch1Cells != 72 || ch2Cells != 72) {
            cleanupSynth();
            std::printf("FAIL [Wm-Channels]: %d/%d/%d (expected 48/72/72)\n", pilotCells, ch1Cells, ch2Cells);
            return false;
        }

        // CRC-12 stability & 1-bit error sensitivity across all bits
        std::uint8_t msgBytes[16]{};
        for (std::size_t i = 0U; i < 16U; ++i) msgBytes[i] = static_cast<std::uint8_t>((i * 29U + 7U) & 0xFFU);
        const std::uint16_t baseCrc = peer_beacon::ComputeVideoMarkCrc12(msgBytes, sizeof(msgBytes));
        if (baseCrc != peer_beacon::ComputeVideoMarkCrc12(msgBytes, sizeof(msgBytes))) {
            cleanupSynth();
            std::printf("FAIL [Wm-CrcStable]: CRC-12 unstable\n");
            return false;
        }
        for (std::size_t bit = 0U; bit < 128U; ++bit) {
            msgBytes[bit >> 3U] ^= static_cast<std::uint8_t>(1U << (bit & 7U));
            const std::uint16_t flipCrc = peer_beacon::ComputeVideoMarkCrc12(msgBytes, sizeof(msgBytes));
            msgBytes[bit >> 3U] ^= static_cast<std::uint8_t>(1U << (bit & 7U));
            if (flipCrc == baseCrc) {
                cleanupSynth();
                std::printf("FAIL [Wm-CrcFlip]: single bit flip %zu undetected\n", bit);
                return false;
            }
        }

        // Gate check: off-scene -> inactive, in-scene untainted -> inactive, in-scene tainted -> active, plus hidden `self_check vis`
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerInstance) = 0ULL;
        peer_beacon::TickOnMainThread(base);
        MhwRngPluginId sizeId = 0U;
        for (const auto& p : plugin_host::ListLoadedPlugins()) {
            if (p.shortName == "size") sizeId = p.id;
        }
        peer_beacon::MarkTainted(sizeId);
        if (peer_beacon::IsVideoMarkActive()) {
            cleanupSynth();
            std::printf("FAIL [Wm-OffScene]: active off-scene\n");
            return false;
        }
        *reinterpret_cast<std::uintptr_t*>(base + mhw::kRvaSPlayerInstance) =
            reinterpret_cast<std::uintptr_t>(fakeSPlayer.data());
        peer_beacon::TickOnMainThread(base);
        peer_beacon::MarkTainted(sizeId);
        if (!peer_beacon::IsVideoMarkActive()) {
            cleanupSynth();
            std::printf("FAIL [Wm-InScene]: expected active in-scene when tainted\n");
            return false;
        }
        peer_beacon::ClearAllTaint();
        if (peer_beacon::IsVideoMarkActive()) {
            cleanupSynth();
            std::printf("FAIL [Wm-Untainted]: expected inactive when untainted\n");
            return false;
        }

        if (!cmd_router::HandleCommandLine("$rng self_check vis on", true) || !peer_beacon::IsVideoMarkDebugVisible() ||
            !cmd_router::HandleCommandLine("$rng self_check vis off", true) || peer_beacon::IsVideoMarkDebugVisible()) {
            cleanupSynth();
            std::printf("FAIL [Wm-VisCmd]: self_check vis toggle failed\n");
            return false;
        }
    }

    // 4. Verify stealth guarantees: ZERO Tab completion & ZERO `$rng help` / `$rng status` mentions
    {
        console::Reset();
        console::Init(&cfg);
        console::ToggleOpen();
        const char* prefixes[] = { "c", "ch", "che", "check", "s", "se", "sel", "self", "self_" };
        for (const char* pref : prefixes) {
            console::OnKeyDown('U', true, false, false);
            console::InsertUtf8(pref);
            for (int tab = 0; tab < 6; ++tab) {
                console::OnKeyDown(VK_TAB, false, false, false);
                console::ConsoleSnapshot snap{};
                console::CaptureSnapshot(0U, snap);
                if (snap.inputLine.find("check") != std::string::npos && std::string(pref) != "check") {
                    cleanupSynth();
                    std::printf("FAIL [Beacon-StealthTab]: prefix '%s' tab-completed to '%s'\n",
                                pref, snap.inputLine.c_str());
                    return false;
                }
                if (snap.completionHint.find("check") != std::string::npos) {
                    cleanupSynth();
                    std::printf("FAIL [Beacon-StealthHint]: prefix '%s' hint leaked '%s'\n",
                                pref, snap.completionHint.c_str());
                    return false;
                }
            }
        }
        console::OnKeyDown(VK_ESCAPE, false, false, false);

        g_capturedChatLines.clear();
        cmd_router::HandleCommandLine("$rng help", true);
        cmd_router::HandleCommandLine("$rng help check", true);
        cmd_router::HandleCommandLine("$rng help self_check", true);
        cmd_router::HandleCommandLine("$rng status", true);
        for (const auto& l : g_capturedChatLines) {
            const bool isNotFound = (l.find("No subcommand") != std::string::npos ||
                                     l.find(u8"\u672a\u627e\u5230") != std::string::npos);
            if (!isNotFound && (l.find("self_check") != std::string::npos || l.find("check") != std::string::npos)) {
                cleanupSynth();
                std::printf("FAIL [Beacon-StealthHelp]: leaked in help/status output: '%s'\n", l.c_str());
                return false;
            }
        }
    }

    cleanupSynth();
    std::printf("[OK] Test 27: Hidden check/self_check, seed27 beacon & stealth checks verified!\n");
    return true;
}

