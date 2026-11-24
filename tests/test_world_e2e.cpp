#include "test_env.h"

bool RunSuite_WorldE2E(TestEnv& env) {
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
    // Test 25: End-to-End `mhw_world.dll` + 5 Refactored Plugins + `mhw.catalog` + `steam` Showcase
    // ------------------------------------------------------------------------
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        static constexpr MhwRngPluginId kWorldTestId = 0x7025U;

        struct PopupCapture {
            std::atomic<int> hits{0};
            char             lastMsg[256]{};
        };
        static PopupCapture g_popupCap;
        g_popupCap.hits.store(0, std::memory_order_relaxed);
        g_popupCap.lastMsg[0] = '\0';

        auto popupStub = +[](void*, const char* msg, float, float, std::uint8_t, float, float) {
            g_popupCap.hits.fetch_add(1, std::memory_order_relaxed);
            if (msg) {
                std::snprintf(g_popupCap.lastMsg, sizeof(g_popupCap.lastMsg), "%s", msg);
            }
        };

        void* const savedPlayerRoot = *reinterpret_cast<void* const*>(base + mhw1523::kRvaSPlayerRoot);
        void* const savedPlayer     = *reinterpret_cast<void* const*>(base + mhw1523::kRvaSPlayer);
        void* const savedQuest      = *reinterpret_cast<void* const*>(base + mhw1523::kRvaSQuest);
        void* const savedArea       = *reinterpret_cast<void* const*>(base + mhw1523::kRvaSMhArea);
        void* const savedTime       = *reinterpret_cast<void* const*>(base + mhw1523::kRvaSTime);
        void* const savedGui        = *reinterpret_cast<void* const*>(base + mhw1523::kRvaSMhGui);
        const std::uint32_t savedDefFloatBits = *reinterpret_cast<const std::uint32_t*>(base + 0x2E4FE58U);
        void* const savedCritMultTblPtr       = *reinterpret_cast<void* const*>(base + 0x5011F58U);

        std::uint8_t savedDisplayBytes[12]{};
        std::memcpy(savedDisplayBytes, img + mhw1523::kRvaDisplayPopup, sizeof(savedDisplayBytes));
        DWORD oldPopupProt = PAGE_EXECUTE_READ;
        if (!::VirtualProtect(img + mhw1523::kRvaDisplayPopup, 16, PAGE_EXECUTE_READWRITE, &oldPopupProt)) {
            std::printf("FAIL [world-e2e]: VirtualProtect on kRvaDisplayPopup failed\n");
            return false;
        }
        {
            std::uint8_t* pfn = img + mhw1523::kRvaDisplayPopup;
            pfn[0] = 0x48;
            pfn[1] = 0xB8;
            const auto stubPtr = reinterpret_cast<std::uintptr_t>(popupStub);
            std::memcpy(pfn + 2, &stubPtr, 8);
            pfn[10] = 0xFF;
            pfn[11] = 0xE0;
            ::FlushInstructionCache(::GetCurrentProcess(), pfn, 16);
        }

        auto restoreWorldTestMem = [&]() {
            *reinterpret_cast<void**>(base + mhw1523::kRvaSPlayerRoot) = savedPlayerRoot;
            *reinterpret_cast<void**>(base + mhw1523::kRvaSPlayer)     = savedPlayer;
            *reinterpret_cast<void**>(base + mhw1523::kRvaSQuest)      = savedQuest;
            *reinterpret_cast<void**>(base + mhw1523::kRvaSMhArea)     = savedArea;
            *reinterpret_cast<void**>(base + mhw1523::kRvaSTime)       = savedTime;
            *reinterpret_cast<void**>(base + mhw1523::kRvaSMhGui)      = savedGui;
            *reinterpret_cast<std::uint32_t*>(base + 0x2E4FE58U)       = savedDefFloatBits;
            *reinterpret_cast<void**>(base + 0x5011F58U)               = savedCritMultTblPtr;
            DWORD dummyProt = 0;
            ::VirtualProtect(img + mhw1523::kRvaDisplayPopup, 16, PAGE_EXECUTE_READWRITE, &dummyProt);
            std::memcpy(img + mhw1523::kRvaDisplayPopup, savedDisplayBytes, sizeof(savedDisplayBytes));
            ::VirtualProtect(img + mhw1523::kRvaDisplayPopup, 16, oldPopupProt, &dummyProt);
            ::FlushInstructionCache(::GetCurrentProcess(), img + mhw1523::kRvaDisplayPopup, 16);
        };

        std::vector<std::uint8_t> synPlayerRoot(0x100, 0U);
        std::vector<std::uint8_t> synWorldPlayer(0x100, 0U);
        std::vector<std::uint8_t> synUnit1(0xE200, 0U);
        std::vector<std::uint8_t> synUnit2(0xE200, 0U);
        std::vector<std::uint8_t> synStatusMgr(0xA00, 0U);
        std::vector<std::uint8_t> synCritTbl(0x800, 0U);
        std::vector<std::uint8_t> synQuest(0x2B000, 0U);
        std::vector<std::uint8_t> synArea(0xE000, 0U);
        std::vector<std::uint8_t> synTime(0x100, 0U);
        std::vector<std::uint8_t> synGui(0x40, 0U);
        void*                     dummyGuiVft[4]{};

        *reinterpret_cast<std::uint32_t*>(synUnit1.data() + mhw1523::kOffUnitValidMask)     = 0x0EU;
        *reinterpret_cast<std::uint32_t*>(synUnit1.data() + mhw1523::kOffPlayerLocalFlags)  = 1U;
        *reinterpret_cast<void**>(synUnit1.data() + mhw1523::kOffPlayerStatusMgr)           = synStatusMgr.data();
        *reinterpret_cast<std::uint32_t*>(synUnit2.data() + mhw1523::kOffUnitValidMask)     = 0x0EU;
        *reinterpret_cast<std::uint32_t*>(synUnit2.data() + mhw1523::kOffPlayerLocalFlags)  = 1U;
        *reinterpret_cast<void**>(synUnit2.data() + mhw1523::kOffPlayerStatusMgr)           = synStatusMgr.data();
        *reinterpret_cast<void**>(synStatusMgr.data() + 0x08U)                              = synUnit1.data();
        *reinterpret_cast<std::uint8_t*>(synStatusMgr.data() + 0x9BAU)                      = 1U;
        *reinterpret_cast<float*>(synCritTbl.data() + 0x7A8U)                               = 1.25f;
        *reinterpret_cast<float*>(synCritTbl.data() + 0x7ACU)                               = 0.75f;
        *reinterpret_cast<float*>(base + 0x2E4FE58U)                                        = 1.0f;
        *reinterpret_cast<void**>(base + 0x5011F58U)                                        = synCritTbl.data();
        *reinterpret_cast<void**>(synGui.data())                                            = dummyGuiVft;

        *reinterpret_cast<void**>(base + mhw1523::kRvaSPlayerRoot) = nullptr;
        *reinterpret_cast<void**>(base + mhw1523::kRvaSPlayer)     = nullptr;
        *reinterpret_cast<void**>(base + mhw1523::kRvaSQuest)      = synQuest.data();
        *reinterpret_cast<void**>(base + mhw1523::kRvaSMhArea)     = synArea.data();
        *reinterpret_cast<void**>(base + mhw1523::kRvaSTime)       = synTime.data();
        *reinterpret_cast<void**>(base + mhw1523::kRvaSMhGui)      = synGui.data();

        std::vector<std::string> worldTopics;
        const MhwRngHookHandle hWorldSub = MhwRng_HostSubscribeEvent(
            host, kWorldTestId, "mhw.world.*",
            +[](const MhwRngCustomEvent* ev, void* ud) {
                if (ev && ev->topic) static_cast<std::vector<std::string>*>(ud)->emplace_back(ev->topic);
            },
            &worldTopics);

        // Step A: Title baseline -> Enter Hub
        MhwRng_PumpMainThreadTicks();
        MhwWorldState ws{};
        if (!MhwWorld_TryGetState(host, &ws) || ws.inScene != 0U || ws.inQuest != 0U) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [world-title-base]: inScene=%u inQuest=%u\n", ws.inScene, ws.inQuest);
            return false;
        }

        worldTopics.clear();
        *reinterpret_cast<void**>(synPlayerRoot.data() + mhw1523::kOffPlayerRootEntity)  = synUnit1.data();
        *reinterpret_cast<void**>(synWorldPlayer.data() + mhw1523::kOffSPlayerLocalUnit) = synUnit1.data();
        *reinterpret_cast<void**>(base + mhw1523::kRvaSPlayerRoot) = synPlayerRoot.data();
        *reinterpret_cast<void**>(base + mhw1523::kRvaSPlayer)     = synWorldPlayer.data();
        *reinterpret_cast<std::uint8_t*>(synArea.data() + mhw1523::kOffAreaIsVillage) = 1U;
        *reinterpret_cast<std::uint32_t*>(synArea.data() + mhw1523::kOffAreaStageId)  = 301U;
        *reinterpret_cast<float*>(synTime.data() + mhw1523::kOffTimeCurHour)          = 10.5f;
        *reinterpret_cast<std::uint32_t*>(synTime.data() + mhw1523::kOffTimeWeatherId) = 0U;
        MhwRng_PumpMainThreadTicks();

        auto hasTopic = [&](const char* t) {
            return std::find(worldTopics.begin(), worldTopics.end(), t) != worldTopics.end();
        };
        if (!MhwWorld_TryGetState(host, &ws) || ws.inScene == 0U || ws.isVillage == 0U || ws.inQuest != 0U ||
            !hasTopic(kMhwWorldEvSceneEnter)) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [world-hub]: inScene=%u isVillage=%u inQuest=%u\n",
                        ws.inScene, ws.isVillage, ws.inQuest);
            return false;
        }

        // Step B: Enter Guiding Lands Quest + Queue Popup + Weather/Hour Advance
        worldTopics.clear();
        *reinterpret_cast<std::uint32_t*>(synQuest.data() + mhw1523::kOffQuestState)          = 2U;
        *reinterpret_cast<std::uint32_t*>(synQuest.data() + mhw1523::kOffQuestCategory)       = 2U;
        *reinterpret_cast<std::uint32_t*>(synQuest.data() + mhw1523::kOffQuestId)             = 9001U;
        *reinterpret_cast<std::uint8_t*>(synQuest.data() + mhw1523::kOffQuestIsGuidingLands)  = 1U;
        *reinterpret_cast<std::int32_t*>(synQuest.data() + mhw1523::kOffQuestRemTicks)        = 3000;
        *reinterpret_cast<std::int32_t*>(synQuest.data() + mhw1523::kOffQuestInitRemTicks)    = 3000;
        const std::uint8_t glLevels[6] = { 7U, 7U, 7U, 4U, 1U, 7U };
        std::memcpy(synQuest.data() + mhw1523::kOffQuestGlRegionLevels, glLevels, 6);
        *reinterpret_cast<std::uint8_t*>(synArea.data() + mhw1523::kOffAreaIsVillage) = 0U;
        *reinterpret_cast<std::uint32_t*>(synArea.data() + mhw1523::kOffAreaStageId)  = 109U;

        (void)MhwWorld_TryDisplayPopup(host, "Hello MIRA v3.5", 3.0f);
        MhwRng_PumpMainThreadTicks();
        if (!MhwWorld_TryGetState(host, &ws) || ws.inQuest == 0U || ws.isGuidingLands == 0U ||
            ws.glRegionLevels[0] != 7U || !hasTopic(kMhwWorldEvQuestEnter) || !hasTopic(kMhwWorldEvStageChanged) ||
            g_popupCap.hits.load() < 1 || std::strcmp(g_popupCap.lastMsg, "Hello MIRA v3.5") != 0) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [world-gl-quest]: inQuest=%u gl=%u lv0=%u popupHits=%d msg='%s'\n",
                        ws.inQuest, ws.isGuidingLands, ws.glRegionLevels[0],
                        g_popupCap.hits.load(), g_popupCap.lastMsg);
            return false;
        }

        worldTopics.clear();
        *reinterpret_cast<float*>(synTime.data() + mhw1523::kOffTimeCurHour)           = 12.2f;
        *reinterpret_cast<std::uint32_t*>(synTime.data() + mhw1523::kOffTimeWeatherId) = 3U;
        MhwRng_PumpMainThreadTicks();
        if (!MhwWorld_TryGetState(host, &ws) || ws.isSpecialWeather == 0U ||
            !hasTopic(kMhwWorldEvHourChanged) || !hasTopic(kMhwWorldEvWeatherChanged)) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [world-time-weather]: isSpecialWeather=%u hr=%.1f wid=%u\n",
                        ws.isSpecialWeather, static_cast<double>(ws.gameHour), ws.weatherId);
            return false;
        }

        // Step C: Verify refactored plugin statuses + `mhw.catalog` service + `crit autoreset`
        cmd_router::HandleCommandLine("$rng lang en", true);
        auto checkChatContains = [&](const char* needle) -> bool {
            for (const auto& l : g_capturedChatLines) {
                if (l.find(needle) != std::string::npos) return true;
            }
            return false;
        };

        g_capturedChatLines.clear();
        plugin_host::DispatchPluginSubcommand("endemic", {"rare", "status"}, "rare status");
        if (!checkChatContains("Stage 109") || !checkChatContains("7/7/7/4/1/7")) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [endemic-world-status]: missing live GL world info\n");
            return false;
        }

        for (const char* sub : { "size", "thk", "crit" }) {
            g_capturedChatLines.clear();
            plugin_host::DispatchPluginSubcommand(sub, {"status"}, "status");
            if (!checkChatContains("World context:")) {
                restoreWorldTestMem();
                MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
                std::printf("FAIL [%s-world-status]: missing World context line\n", sub);
                return false;
            }
        }

        std::uint32_t catalogRva = 0U;
        MhwRngSiteMeta catalogMeta{};
        {
            MhwRngServiceGuard<MhwCatalogApi> cat(host, kMhwCatalogServiceName, kMhwCatalogServiceVersion);
            if (!MhwCatalog_TryResolveAlias(host, "em.size", &catalogRva) || catalogRva != 0x169BA15U ||
                !cat || !cat->QueryCatalogSite || !cat->QueryCatalogSite(0x169BA15U, &catalogMeta) || catalogMeta.found == 0U) {
                restoreWorldTestMem();
                MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
                std::printf("FAIL [catalog-service]: rva=0x%X found=%u\n", catalogRva, catalogMeta.found);
                return false;
            }
        }

        // Verify `crit autoreset on` vs `off` on quest restart using real 0x01F51380 crit hook
        {
            alignas(16) std::uint8_t origAtk[0x280]{};
            alignas(16) std::uint8_t atkNode[0x100]{};
            alignas(16) std::uint8_t hitInfo[0x100]{};
            alignas(16) std::uint8_t mutAtk[0x200]{};
            *reinterpret_cast<float*>(origAtk + 0x20U) = 50.0f;
            *reinterpret_cast<void**>(atkNode + 0xA8U) = origAtk;
            *reinterpret_cast<void**>(hitInfo + 0x38U) = atkNode;
            void* pHit = hitInfo;
            struct { void** hitInfoPtr; void* player; } critCtx{ &pHit, synUnit1.data() };
            using ApplyCritFn = void(__fastcall*)(void*, void*, std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t);
            const auto fnApplyCrit = reinterpret_cast<ApplyCritFn>(base + 0x01F51380U);

            plugin_host::DispatchPluginSubcommand("crit", {"autoreset", "on"}, "autoreset on");
            plugin_host::DispatchPluginSubcommand("crit", {"random"}, "random");
            plugin_host::DispatchPluginSubcommand("crit", {"rate", "100"}, "rate 100");
            plugin_host::DispatchPluginSubcommand("crit", {"reset"}, "reset");

            std::memset(mutAtk, 0, sizeof(mutAtk));
            *reinterpret_cast<float*>(mutAtk + 0x20U) = 200.0f;
            fnApplyCrit(&critCtx, mutAtk, 0U, 0U, 0U, 0U);
            g_capturedChatLines.clear();
            plugin_host::DispatchPluginSubcommand("crit", {"status"}, "status");
            if (!checkChatContains("hits=1")) {
                restoreWorldTestMem();
                MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
                std::printf("FAIL [crit-autoreset-pre]: expected hits=1 before quest restart\n");
                return false;
            }

            // Trigger in-quest restart (questId changes 9001 -> 9002) while `autoreset on`
            *reinterpret_cast<std::uint32_t*>(synQuest.data() + mhw1523::kOffQuestId) = 9002U;
            MhwRng_PumpMainThreadTicks();
            g_capturedChatLines.clear();
            plugin_host::DispatchPluginSubcommand("crit", {"status"}, "status");
            if (!checkChatContains("hits=0")) {
                restoreWorldTestMem();
                MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
                std::printf("FAIL [crit-autoreset-on]: expected hits=0 after quest restart\n");
                return false;
            }

            // Disable `autoreset` -> quest restart must preserve `hits=1`
            plugin_host::DispatchPluginSubcommand("crit", {"autoreset", "off"}, "autoreset off");
            std::memset(mutAtk, 0, sizeof(mutAtk));
            *reinterpret_cast<float*>(mutAtk + 0x20U) = 200.0f;
            fnApplyCrit(&critCtx, mutAtk, 0U, 0U, 0U, 0U);
            *reinterpret_cast<std::uint32_t*>(synQuest.data() + mhw1523::kOffQuestId) = 9003U;
            MhwRng_PumpMainThreadTicks();
            g_capturedChatLines.clear();
            plugin_host::DispatchPluginSubcommand("crit", {"status"}, "status");
            if (!checkChatContains("hits=1")) {
                restoreWorldTestMem();
                MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
                std::printf("FAIL [crit-autoreset-off]: expected hits=1 preserved when autoreset off\n");
                return false;
            }
            plugin_host::DispatchPluginSubcommand("crit", {"autoreset", "on"}, "autoreset on");
            plugin_host::DispatchPluginSubcommand("crit", {"off"}, "off");
        }

        // Step D: Return to Title -> verify `title_return` & `quest_leave`
        worldTopics.clear();
        *reinterpret_cast<void**>(base + mhw1523::kRvaSPlayerRoot) = nullptr;
        *reinterpret_cast<void**>(base + mhw1523::kRvaSPlayer)     = nullptr;
        *reinterpret_cast<std::uint32_t*>(synQuest.data() + mhw1523::kOffQuestState)    = 0U;
        *reinterpret_cast<std::uint32_t*>(synQuest.data() + mhw1523::kOffQuestCategory) = 0U;
        MhwRng_PumpMainThreadTicks();
        if (!hasTopic(kMhwWorldEvTitleReturn) || !hasTopic(kMhwWorldEvQuestLeave)) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [world-title-return]: expected title_return + quest_leave events\n");
            return false;
        }

        // Step E: Graceful degradation when `mhw_world.dll` is unloaded + `steam` minimal showcase intact
        std::string worldUnloadMsg;
        if (!plugin_host::UnloadSingle("world", worldUnloadMsg)) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [world-unload]: %s\n", worldUnloadMsg.c_str());
            return false;
        }
        MhwWorldState wsMissing{};
        if (MhwWorld_TryGetState(host, &wsMissing)) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [world-degrade]: MhwWorld_TryGetState succeeded after unload\n");
            return false;
        }
        for (const char* sub : { "size", "endemic", "thk", "crit", "steam" }) {
            if (!plugin_host::DispatchPluginSubcommand(sub, {"status"}, "status")) {
                restoreWorldTestMem();
                MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
                std::printf("FAIL [world-degrade-%s]: status dispatch failed\n", sub);
                return false;
            }
        }
        (void)plugin_host::DispatchPluginSubcommand("steam", {"help"}, "help");
        if (!plugin_host::LoadOrReloadSingle("world", worldUnloadMsg)) {
            restoreWorldTestMem();
            MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
            std::printf("FAIL [world-reload]: %s\n", worldUnloadMsg.c_str());
            return false;
        }

        MhwRng_HostUnsubscribeEvent(host, kWorldTestId, hWorldSub);
        restoreWorldTestMem();
        std::printf("[OK] Test 25: `mhw_world.dll` + 5 refactored plugins + `mhw.catalog` + `steam` showcase + graceful unload verified!\n");
    }

    return true;
}

