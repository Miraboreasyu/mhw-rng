#include "test_env.h"

bool RunSuite_SdkV3Core(TestEnv& env) {
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
    if (!host || host->apiVersion != 3U || host->structSize != sizeof(MhwRngHostApi)) {
        std::printf("FAIL [v3 host]: invalid HostApi version/size\n");
        return false;
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
                return false;
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
                return false;
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
                return false;
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
                return false;
            }

            tr.bMode = 0;
            const std::uint32_t vCont = CallNrandV3Chain(fnNrand, sInst);
            if (vCont != 0xAAAAAAAAU || tr.bPos != 0 || tr.aPos != 1) {
                std::printf("FAIL [V3-2]: priority/Continue mismatch (v=0x%X bPos=%d aPos=%d)\n", vCont, tr.bPos, tr.aPos);
                return false;
            }

            tr.orderIdx = 0; tr.bPos = tr.aPos = -1; tr.bMode = 1;
            const std::uint32_t vShort = CallNrandV3Chain(fnNrand, sInst);
            if (vShort != 0xBBBBBBBBU || tr.bPos != 0 || tr.aPos != -1) {
                std::printf("FAIL [V3-2]: ReturnValue short-circuit failed\n");
                return false;
            }

            host->SetCondFixedValue(kTestPluginId, chainRva, 7U, 0x77777777U);
            tr.orderIdx = 0; tr.bPos = tr.aPos = -1; tr.bMode = 2;
            const std::uint32_t vCond = CallNrandV3Chain(fnNrand, sInst);
            if (vCond != 0x77777777U || tr.aSawCond != 7U) {
                std::printf("FAIL [V3-2]: UseCondition propagation failed (v=0x%X aSaw=%u)\n", vCond, tr.aSawCond);
                return false;
            }

            // Unregister B and ensure A remains completely intact
            host->UnregisterSlotHookHandle(kTestPluginId, hB);
            tr.bMode = 0;
            if (CallNrandV3Chain(fnNrand, sInst) != 0xAAAAAAAAU) {
                std::printf("FAIL [V3-2]: hook A lost after unregistering hook B\n");
                return false;
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
                return false;
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
                return false;
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
                return false;
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
                return false;
            }
            // Verify unseeded slot captured with seededEpoch==0 was NOT corrupted to fallback state:
            MhwRngSlotSnapshot unseededSnap{};
            if (!host->GetCondSlotInfo(kUnseededRva, 0U, &unseededSnap) ||
                (unseededSnap.state[0] == 0x075BCD15U && unseededSnap.state[1] == 0x159A55E5U)) {
                std::printf("FAIL [V3-5]: unseeded slot state corrupted on Capture/RestoreStateBlob!\n");
                return false;
            }
            std::uint32_t gotAfterRestore[16]{};
            for (int i = 0; i < 16; ++i) gotAfterRestore[i] = CallNrandSiteA(fnNrand, sInst);

            if (std::memcmp(gotAfterRestore, expAfterSave, sizeof(expAfterSave)) != 0) {
                std::printf("FAIL [V3-5]: Restored RNG stream diverged from captured snapshot!\n");
                return false;
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
                return false;
            }
            for (int i = 0; i < 8; ++i) {
                if (CallNrandSiteB(fnNrand, sInst) != expB[i]) {
                    std::printf("FAIL [V3-5]: Single-slot RestoreSlotState diverged at step %d\n", i);
                    return false;
                }
            }

            host->UnregisterLifecycleHook(kTestPluginId, hLife);
            if (lst.evEpoch < 1 || lst.evSeed < 1 || lst.evMode < 2 || lst.evClear < 1 || lst.evRestore < 1) {
                std::printf("FAIL [V3-6]: Lifecycle events missing (%d/%d/%d/%d/%d)\n",
                            lst.evEpoch, lst.evSeed, lst.evMode, lst.evClear, lst.evRestore);
                return false;
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
                return false;
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
                    return false;
                }
            }
            host->ReleaseCondSlot(kTestPluginId, 0x05000100U, 1U);

            // (b) Dynamic condId allocation + tombstone reclamation
            const std::uint32_t baselineSlots = host->GetActiveSlotCount();
            for (std::uint32_t cid = 100U; cid < 600U; ++cid) {
                if (!host->SetCondFixedValue(kTestPluginId, 0x05000120U, cid, cid)) {
                    std::printf("FAIL [V3-8b]: SetCondFixedValue failed at cid %u\n", cid);
                    return false;
                }
            }
            for (std::uint32_t cid = 100U; cid < 600U; ++cid) {
                if (!host->ReleaseCondSlot(kTestPluginId, 0x05000120U, cid)) {
                    std::printf("FAIL [V3-8b]: ReleaseCondSlot failed at cid %u\n", cid);
                    return false;
                }
            }
            if (host->GetActiveSlotCount() != baselineSlots) {
                std::printf("FAIL [V3-8b]: active slot count not reclaimed (%u vs %u)\n",
                            host->GetActiveSlotCount(), baselineSlots);
                return false;
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
                return false;
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
                return false;
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
                return false;
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
                return false;
            }
            host->SetInstancePolicy(reinterpret_cast<std::uintptr_t>(privPol), kMhwRngInstForceSlotControl);
            const std::uint32_t forceGot = CallNrandV3Priv(fnNrand, privPol);
            const std::uint32_t ownRef2  = stepWordRef(copyWords);
            if (forceGot == ownRef2) {
                std::printf("FAIL [V3-8d]: ForceSlotControl still stepped own state instead of slot!\n");
                return false;
            }
            std::printf("[OK] V3-8: Reclaimable seq heap, tombstone slots, custom wrapper unwrap & instance policy verified!\n");
        }

    return true;
}

bool RunSuite_SdkV3Filters(TestEnv& env) {
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
                return false;
            }
        }
        host->UnregisterSlotHookHandle(kTestPluginId, hFvA);
        host->UnregisterSlotHookHandle(kTestPluginId, hFvB);
        host->UnregisterSlotHookHandle(kTestPluginId, hFvC);
        if (!fvSt.flagsOk || fvSt.aCalls != 3 || fvSt.bCalls != 3 || fvSt.cCalls != 3 || fvSt.cFinal != 123U) {
            std::printf("FAIL [FilterVanillaChain]: a=%d b=%d c=%d ok=%d cFinal=%u\n",
                        fvSt.aCalls, fvSt.bCalls, fvSt.cCalls, fvSt.flagsOk, fvSt.cFinal);
            return false;
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
                return false;
            }
        }

        // Now verify FilterSlot in Selective mode over vanilla RNG draws
        host->ClearSlotAllConditions(kRvaFilterSlot);
        host->SetEngineMode(kMhwRngModeSelective);
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t got = fnStubSlot(sInst);
            if (got < 50U || got > 300U) {
                std::printf("FAIL [FilterSlot-Selective]: got %u outside [50, 300]\n", got);
                return false;
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
                return false;
            }
        }
        host->UnregisterSlotHookHandle(kTestPluginId, hPreFilt);
        host->ClearSlotAllConditions(kRvaFilterSlot);
        std::printf("[OK] Multi-plugin chained FilterVanilla + FilterSlot (Takeover/Selective/PreCombo) verified!\n");
    }

    return true;
}

bool RunSuite_SdkV3Trace(TestEnv& env) {
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
            return false;
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
            return false;
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
            return false;
        }

        // (c) Verify `$rng trace` in `rng_inspector.dll` (trace on -> 3 calls -> log -> dump -> off -> clear)
        cmd_router::Init(&cfg, &CaptureChatSink);
        g_capturedChatLines.clear();
        if (!cmd_router::HandleCommandLine("$rng trace 0x5000909", true)) {
            std::printf("FAIL [InspectorTrace-1]: `$rng trace 0x5000909` command failed\n");
            ::RtlDeleteFunctionTable(synPdata);
            return false;
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
            return false;
        }
        std::printf("[OK] XMM6/XMM7 capture + 4-hop `.pdata` CaptureCallerRvas + `$rng trace` verified!\n");
    }

    return true;
}

bool RunSuite_SdkV35(TestEnv& env) {
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
    // Test 24: Verify Kernel SDK v3.5 Primitives (MainThreadTick, EventBus & Service Registry)
    // ------------------------------------------------------------------------
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        static constexpr MhwRngPluginId kSdkTestId = 0x7024U;

        // (a) MainThreadTick registration, pumping, monotonic seq & unregister
        struct TickState {
            std::atomic<int> hits{0};
            std::uint64_t    lastSeq = 0ULL;
        } tickState;
        const MhwRngHookHandle hTick = MhwRng_HostRegisterMainThreadTick(
            host, kSdkTestId,
            +[](std::uint64_t seq, void* ud) {
                auto* ts = static_cast<TickState*>(ud);
                ts->hits.fetch_add(1, std::memory_order_relaxed);
                ts->lastSeq = seq;
            },
            &tickState);
        if (hTick == kMhwRngInvalidHandle) {
            std::printf("FAIL [sdk35-tick]: RegisterMainThreadTick returned invalid handle\n");
            return false;
        }
        MhwRng_PumpMainThreadTicks();
        const std::uint64_t s1 = tickState.lastSeq;
        MhwRng_PumpMainThreadTicks();
        MhwRng_PumpMainThreadTicks();
        if (tickState.hits.load() != 3 || tickState.lastSeq <= s1) {
            std::printf("FAIL [sdk35-tick]: expected 3 monotonic hits, got %d (s1=%llu, s3=%llu)\n",
                        tickState.hits.load(),
                        static_cast<unsigned long long>(s1),
                        static_cast<unsigned long long>(tickState.lastSeq));
            MhwRng_HostUnregisterMainThreadTick(host, kSdkTestId, hTick);
            return false;
        }
        MhwRng_HostUnregisterMainThreadTick(host, kSdkTestId, hTick);
        MhwRng_PumpMainThreadTicks();
        if (tickState.hits.load() != 3) {
            std::printf("FAIL [sdk35-tick-unreg]: tick fired after unregister\n");
            return false;
        }

        // (b) Event Bus: exact match, prefix wildcard, global `*`, recursion guard (<= 8) & unsubscribe
        struct BusState {
            int           exactHits  = 0;
            int           prefixHits = 0;
            int           allHits    = 0;
            int           recHits    = 0;
            std::uint32_t lastMagic  = 0U;
        } bus;
        const MhwRngHookHandle hEx = MhwRng_HostSubscribeEvent(
            host, kSdkTestId, "test.alpha.hit",
            +[](const MhwRngCustomEvent* ev, void* ud) {
                auto* b = static_cast<BusState*>(ud);
                ++b->exactHits;
                if (ev && ev->payload && ev->payloadSize == sizeof(std::uint32_t)) {
                    b->lastMagic = *static_cast<const std::uint32_t*>(ev->payload);
                }
            },
            &bus);
        const MhwRngHookHandle hPf = MhwRng_HostSubscribeEvent(
            host, kSdkTestId, "test.alpha.*",
            +[](const MhwRngCustomEvent*, void* ud) { ++static_cast<BusState*>(ud)->prefixHits; },
            &bus);
        const MhwRngHookHandle hAll = MhwRng_HostSubscribeEvent(
            host, kSdkTestId, "*",
            +[](const MhwRngCustomEvent*, void* ud) { ++static_cast<BusState*>(ud)->allHits; },
            &bus);
        const MhwRngHookHandle hRec = MhwRng_HostSubscribeEvent(
            host, kSdkTestId, "test.recurse",
            +[](const MhwRngCustomEvent*, void* ud) {
                auto* b = static_cast<BusState*>(ud);
                ++b->recHits;
                MhwRng_HostEmitEvent(MhwRng_GetHostApi(), kSdkTestId, "test.recurse");
            },
            &bus);

        const std::uint32_t magic = 0xDEADBEEFU;
        MhwRng_HostEmitEvent(host, kSdkTestId, "test.alpha.hit", &magic, sizeof(magic));
        MhwRng_HostEmitEvent(host, kSdkTestId, "test.alpha.clear");
        MhwRng_HostEmitEvent(host, kSdkTestId, "test.recurse");
        const int allBeforeUnsub = bus.allHits;
        MhwRng_HostUnsubscribeEvent(host, kSdkTestId, hEx);
        MhwRng_HostUnsubscribeEvent(host, kSdkTestId, hPf);
        MhwRng_HostUnsubscribeEvent(host, kSdkTestId, hRec);
        MhwRng_HostUnsubscribeEvent(host, kSdkTestId, hAll);
        MhwRng_HostEmitEvent(host, kSdkTestId, "test.alpha.hit", &magic, sizeof(magic));

        if (bus.exactHits != 1 || bus.prefixHits != 2 || allBeforeUnsub < 10 ||
            bus.recHits != 8 || bus.lastMagic != magic || bus.allHits != allBeforeUnsub) {
            std::printf("FAIL [sdk35-bus]: exact=%d prefix=%d allBefore=%d allAfter=%d rec=%d magic=0x%X\n",
                        bus.exactHits, bus.prefixHits, allBeforeUnsub, bus.allHits, bus.recHits, bus.lastMagic);
            return false;
        }

        // (c) Concurrent 4-thread ServiceGuard readers racing 4x hot-reload of `mhw_world.dll`
        std::atomic<bool>          stopReaders{false};
        std::atomic<std::uint64_t> okServiceReads{0ULL};
        std::thread                readers[4];
        for (int ti = 0; ti < 4; ++ti) {
            readers[ti] = std::thread([&]() {
                while (!stopReaders.load(std::memory_order_acquire)) {
                    {
                        MhwRngServiceGuard<MhwWorldApi> world(host, kMhwWorldServiceName, kMhwWorldServiceVersion);
                        if (world) {
                            MhwWorldState st{};
                            char summary[128]{};
                            if (world->GetState && world->GetState(&st) &&
                                world->FormatWorldSummary && world->FormatWorldSummary(summary, sizeof(summary), true)) {
                                (void)world->GetSessionGen();
                                okServiceReads.fetch_add(1U, std::memory_order_relaxed);
                            }
                        }
                    }
                    std::this_thread::yield();
                }
            });
        }
        std::string worldReloadMsg;
        for (int iter = 0; iter < 4; ++iter) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!plugin_host::LoadOrReloadSingle("world", worldReloadMsg)) {
                stopReaders.store(true, std::memory_order_release);
                for (auto& th : readers) th.join();
                std::printf("FAIL [sdk35-service-reload]: iter=%d msg=%s\n", iter, worldReloadMsg.c_str());
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stopReaders.store(true, std::memory_order_release);
        for (auto& th : readers) th.join();

        MhwRng_PumpMainThreadTicks();
        MhwWorldState stAfter{};
        if (okServiceReads.load() == 0ULL || !MhwWorld_TryGetState(host, &stAfter) || stAfter.tickCount == 0ULL) {
            std::printf("FAIL [sdk35-service-stress]: okReads=%llu tickCount=%llu\n",
                        static_cast<unsigned long long>(okServiceReads.load()),
                        static_cast<unsigned long long>(stAfter.tickCount));
            return false;
        }
        std::printf("[OK] Test 24: Kernel SDK v3.5 (MainThreadTick, EventBus & concurrent ServiceGuard vs 4x `world` hot-reload, %llu safe leases) verified!\n",
                    static_cast<unsigned long long>(okServiceReads.load()));
    }

    return true;
}

