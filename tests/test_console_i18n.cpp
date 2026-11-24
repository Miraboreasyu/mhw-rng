#include "test_env.h"

bool RunSuite_CmdRouter(TestEnv& env) {
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

    // 14. Verify `cmd_router`: `lang zh|en` across core + all 5 plugins, `rawseq`, `reload` state preservation & INI delta detection, and `exec` + recursion guard
    {
        const MhwRngHostApi* host = MhwRng_GetHostApi();
        std::string tmpMsg;
        if (!plugin_host::LoadOrReloadSingle("inspector", tmpMsg)) {
            std::printf("FAIL [CmdRouter-1]: failed to reload inspector: %s\n", tmpMsg.c_str());
            return false;
        }
        host->SetEngineMode(kMhwRngModeTakeover);
        host->ClearAllRules();
        g_capturedChatLines.clear();

        cmd_router::Init(&cfg, &CaptureChatSink);

        // (a) Language toggle `en` <-> `zh` across core + all 5 plugins (+ direct C export check)
        if (!MhwRng_ExecuteCommand("lang en") ||
            std::strcmp(MhwRng_GetLanguage(), "en") != 0 || !app::IsEnglish()) {
            std::printf("FAIL [CmdRouter-2a]: `MhwRng_ExecuteCommand(\"lang en\")` failed!\n");
            return false;
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
            return false;
        }

        if (!MhwRng_ExecuteCommand("lang zh") ||
            std::strcmp(MhwRng_GetLanguage(), "zh") != 0 || app::IsEnglish()) {
            std::printf("FAIL [CmdRouter-2c]: `MhwRng_ExecuteCommand(\"lang zh\")` failed!\n");
            return false;
        }

        // (b) `$rng rawseq` + `rawclear`
        if (!cmd_router::HandleCommandLine("$rng rawseq 0x05000600 10,20,30 hold", true)) {
            std::printf("FAIL [CmdRouter-3a]: `$rng rawseq` failed!\n");
            return false;
        }
        MhwRngSlotSnapshot rsSnap{};
        if (!host->GetCondSlotInfo(0x05000600U, 0U, &rsSnap) ||
            rsSnap.mode != kMhwRngSlotSequence || rsSnap.seqLength != 3U ||
            rsSnap.seqEndPolicy != kMhwRngSeqHoldLast) {
            std::printf("FAIL [CmdRouter-3b]: rawseq slot mismatch (mode=%u len=%u pol=%u)\n",
                        rsSnap.mode, rsSnap.seqLength, rsSnap.seqEndPolicy);
            return false;
        }
        if (!cmd_router::HandleCommandLine("$rng rawclear 0x05000600", true) ||
            !host->GetCondSlotInfo(0x05000600U, 0U, &rsSnap) ||
            rsSnap.mode != kMhwRngSlotMasterDerived) {
            std::printf("FAIL [CmdRouter-3c]: `$rng rawclear` failed!\n");
            return false;
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
            return false;
        }
        if (!writeTextFile(testIniPath,
                           "[General]\r\n"
                           "Enabled=1\r\n"
                           "BootMode=Selective\r\n"
                           "MasterSeed=12345\r\n"
                           "Language=zh\r\n"
                           "AutoExec=_test_autoexec.txt\r\n")) {
            std::printf("FAIL [CmdRouter-4]: could not write temp ini file\n");
            return false;
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
            return false;
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
            return false;
        }
        // Verify `$rng reload ini` forces re-applying INI even over changed runtime mode/seed
        host->SetEngineMode(kMhwRngModeObserveOnly);
        MhwRng_SetMasterSeed(111222333ULL, false);
        cmd_router::HandleCommandLine("$rng reload ini", true);
        if (host->GetEngineMode() != kMhwRngModeTakeover ||
            host->GetMasterSeed() != 555666777ULL) {
            std::printf("FAIL [CmdRouter-4d]: `$rng reload ini` did not force INI values (mode=%u seed=%llu)\n",
                        host->GetEngineMode(), static_cast<unsigned long long>(host->GetMasterSeed()));
            return false;
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
            return false;
        }
        MhwRngSlotSnapshot longSnap{};
        if (!host->GetCondSlotInfo(0x05000620U, 0U, &longSnap) ||
            longSnap.mode != kMhwRngSlotSequence || longSnap.seqLength != 60U) {
            std::printf("FAIL [CmdRouter-5b]: >200B script line truncated or failed (len=%u)!\n", longSnap.seqLength);
            return false;
        }

        ::DeleteFileA(autoExecFile.c_str());
        ::DeleteFileA(longExecFile.c_str());
        ::DeleteFileA(subExecFile.c_str());
        ::DeleteFileA(loopExecFile.c_str());
        if (hadOrigIni) writeTextFile(testIniPath, origIniContent);
        else ::DeleteFileA(testIniPath.c_str());
        cmd_router::ResetSnapshots();
        host->ClearAllRules();
        MhwRng_SetMasterSeed(20261122ULL, true);
        std::printf("[OK] cmd_router (`lang zh|en`, `rawseq`, `reload` preservation + delta, `exec` >200B & recursion guard) verified!\n");
    }

    return true;
}

bool RunSuite_I18nEnSweep(TestEnv& env) {
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
            return false;
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
            return false;
        }

        MhwRng_SetLanguage("fr");
        if (std::strcmp(MhwRng_GetLanguage(), "fr") != 0 ||
            MhwRng_GetLanguageId() != kMhwRngLangOther ||
            std::strcmp(MhwRng_Tr(host, u8"中", "EN"), "EN") != 0) {
            std::printf("FAIL [i18n-fr]: lang=%s id=%u\n", MhwRng_GetLanguage(), MhwRng_GetLanguageId());
            host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
            return false;
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
            "$rng world status",
            "$rng world help",
            "$rng world events on",
            "$rng world events off",
            "$rng crit status",
            "$rng crit help",
            "$rng crit rate 100",
            "$rng crit random",
            "$rng crit autoreset off",
            "$rng crit autoreset on",
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
                return false;
            }
        }
        cmd_router::HandleCommandLine("$rng trace off", true);
        cmd_router::HandleCommandLine("$rng trace clear", true);

        for (const std::string& line : g_capturedChatLines) {
            if (containsCjkUtf8(line)) {
                std::printf("FAIL [i18n-NoCJK-Chat]: unexpected CJK text in en mode: %s\n", line.c_str());
                ::DeleteFileA(dumpEnPath.c_str());
                host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
                return false;
            }
        }
        {
            FILE* dfp = nullptr;
            if (fopen_s(&dfp, dumpEnPath.c_str(), "rb") != 0 || !dfp) {
                std::printf("FAIL [i18n-DumpEn]: failed to open %s\n", dumpEnPath.c_str());
                host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
                return false;
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
                return false;
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
                return false;
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
                return false;
            }
        }

        cmd_router::HandleCommandLine("$rng fatalis off", true);
        cmd_router::HandleCommandLine("$rng size clear", true);
        cmd_router::HandleCommandLine("$rng endemic clear", true);
        host->ClearAllRules();
        host->UnregisterLifecycleHook(kTestPluginId, hLangLife);
        std::printf("[OK] Standardized i18n API (SDK v3.3), fallback chain, zero-CJK en sweep & hot lang switch verified!\n");
    }

    return true;
}

bool RunSuite_ConsoleTab(TestEnv& env) {
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
            return false;
        }
        console::InsertUtf8(u8"size \u91D1\u72EE\u5B50 max"); // "size 金狮子 max"
        for (int i = 0; i < 4; ++i) console::OnKeyDown(VK_LEFT, false, false, false);
        for (int i = 0; i < 3; ++i) console::OnKeyDown(VK_BACK, false, false, false);
        console::InsertUtf8("rajang");
        console::ConsoleSnapshot snap{};
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "size rajang max") {
            std::printf("FAIL [console-utf8]: got '%s'\n", snap.inputLine.c_str());
            return false;
        }

        // Test readline Ctrl+U / Ctrl+Y / Ctrl+A / Ctrl+K
        console::OnKeyDown(VK_END, false, false, false);
        console::OnKeyDown('U', true, false, false); // kill whole line
        console::CaptureSnapshot(0U, snap);
        if (!snap.inputLine.empty()) {
            std::printf("FAIL [console-ctrlu]: expected empty, got '%s'\n", snap.inputLine.c_str());
            return false;
        }
        console::OnKeyDown('Y', true, false, false); // yank back
        console::OnKeyDown('A', true, false, false); // Home
        console::OnKeyDown(VK_RIGHT, true, false, false); // word right -> before "rajang"
        console::OnKeyDown('K', true, false, false); // kill to end -> "size "
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "size ") {
            std::printf("FAIL [console-ctrlk]: got '%s'\n", snap.inputLine.c_str());
            return false;
        }

        // Dynamic Tab completion: plugin usage keywords + plugin names + monsters + multi-match cycle
        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("steam l");
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "steam lock ") {
            std::printf("FAIL [console-tab-steam]: expected 'steam lock ', got '%s'\n", snap.inputLine.c_str());
            return false;
        }

        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("reload ins");
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "reload inspector ") {
            std::printf("FAIL [console-tab-reload]: expected 'reload inspector ', got '%s'\n", snap.inputLine.c_str());
            return false;
        }

        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("size furious");
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "size furious_rajang ") {
            std::printf("FAIL [console-tab-size]: expected 'size furious_rajang ', got '%s'\n", snap.inputLine.c_str());
            return false;
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
            !checkTabComplete("trace log d", "trace log d", "trace-log-noop") ||
            !checkTabComplete("world s", "world status ", "world-status") ||
            !checkTabComplete("world ev", "world events ", "world-events") ||
            !checkTabComplete("world events of", "world events off ", "world-events-off") ||
            !checkTabComplete("crit aut", "crit autoreset ", "crit-autoreset") ||
            !checkTabComplete("crit autoreset of", "crit autoreset off ", "crit-autoreset-off") ||
            !checkTabComplete("reload w", "reload world ", "reload-world")) {
            return false;
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
                return false;
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
                return false;
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
                return false;
            }
        }
        // Ambiguous sub-sub-command: `thk preset tag bipedal_lock ` -> LCP `'o'`, then cycles `off` -> `on`
        if (!checkTabComplete("thk preset tag bipedal_lock ", "thk preset tag bipedal_lock o", "thk-tag-lcp")) {
            return false;
        }
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "thk preset tag bipedal_lock off") {
            std::printf("FAIL [console-tab-thk-tag-off]: expected '... off', got '%s'\n", snap.inputLine.c_str());
            return false;
        }
        console::OnKeyDown(VK_TAB, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "thk preset tag bipedal_lock on") {
            std::printf("FAIL [console-tab-thk-tag-cycle]: expected '... on', got '%s'\n", snap.inputLine.c_str());
            return false;
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
            return false;
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
            return false;
        }

        // History Up/Down preserving draft & Ctrl+R reverse search
        console::OnKeyDown('U', true, false, false);
        console::InsertUtf8("draft_text");
        console::OnKeyDown(VK_UP, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "status") {
            std::printf("FAIL [console-hist-up]: expected 'status', got '%s'\n", snap.inputLine.c_str());
            return false;
        }
        console::OnKeyDown(VK_DOWN, false, false, false);
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "draft_text") {
            std::printf("FAIL [console-hist-draft]: expected 'draft_text', got '%s'\n", snap.inputLine.c_str());
            return false;
        }
        console::OnKeyDown('R', true, false, false); // Ctrl+R
        console::InsertUtf8("chat o");
        console::CaptureSnapshot(0U, snap);
        if (snap.inputLine != "chat off" || snap.searchPrompt.empty()) {
            std::printf("FAIL [console-ctrlr]: got '%s' prompt='%s'\n",
                        snap.inputLine.c_str(), snap.searchPrompt.c_str());
            return false;
        }
        if (snap.lines.size() < 4U) {
            std::printf("FAIL [console-scrollback]: expected >=4 output lines, got %zu\n", snap.lines.size());
            return false;
        }
        console::OnKeyDown(VK_ESCAPE, false, false, false); // exit Ctrl+R
        if (!runConsoleLine("?") || !runConsoleLine("help plugins")) {
            std::printf("FAIL [console-help]: help execution via console failed\n");
            return false;
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
                return false;
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
                return false;
            }
            console::ClampScrollOffset(topSnap.totalLines - 6U);
            console::ScrollBy(-2);
            console::ConsoleSnapshot downSnap{};
            console::CaptureSnapshot(6U, downSnap);
            if (downSnap.scrollOffset != topSnap.totalLines - 8U || downSnap.lines.size() != 6U) {
                std::printf("FAIL [console-scroll-clamp]: offset=%zu lines=%zu\n",
                            downSnap.scrollOffset, downSnap.lines.size());
                return false;
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
                return false;
            }

            console::SetImeCompositionUtf16(L"\u6708\u7FBD\u5929\u6BCD", 4, 2);
            console::CaptureSnapshot(0U, snap);
            if (snap.imeCompText != u8"\u6708\u7FBD\u5929\u6BCD" || snap.imeCompCursorByte != 6U) {
                std::printf("FAIL [console-ime-comp-cjk]: text='%s' cur=%zu\n",
                            snap.imeCompText.c_str(), snap.imeCompCursorByte);
                return false;
            }

            console::SetImeCandidatesUtf16({L"\u6708\u7FBD\u5929\u6BCD", L"\u6708\u5149\u58C1\u864E"}, 1U, 0U, 14U);
            console::CaptureSnapshot(0U, snap);
            if (!console::HasImeComposition() || snap.imeCandidates.size() != 2U ||
                snap.imeCandSelected != 1U || snap.imeCandTotal != 14U) {
                std::printf("FAIL [console-ime-cands]: has=%d n=%zu sel=%zu tot=%zu\n",
                            console::HasImeComposition() ? 1 : 0, snap.imeCandidates.size(),
                            snap.imeCandSelected, snap.imeCandTotal);
                return false;
            }

            console::ClearImeCandidates();
            console::CaptureSnapshot(0U, snap);
            if (!console::HasImeComposition() || !snap.imeCandidates.empty() || snap.imeCompText.empty()) {
                std::printf("FAIL [console-ime-clear-cands]: has=%d cands=%zu comp='%s'\n",
                            console::HasImeComposition() ? 1 : 0, snap.imeCandidates.size(), snap.imeCompText.c_str());
                return false;
            }

            console::ClearImeComposition();
            console::CaptureSnapshot(0U, snap);
            if (console::HasImeComposition() || !snap.imeCompText.empty() || !snap.imeCandidates.empty()) {
                std::printf("FAIL [console-ime-clear-all]: has=%d\n", console::HasImeComposition() ? 1 : 0);
                return false;
            }

            // Closing console clears active IME state automatically
            console::SetImeCompositionUtf16(L"test", 4, 2);
            console::SetImeCandidatesUtf16({L"\u6D4B\u8BD5"}, 0U, 0U, 1U);
            console::SetOpen(false);
            console::CaptureSnapshot(0U, snap);
            if (console::HasImeComposition() || !snap.imeCompText.empty() || !snap.imeCandidates.empty()) {
                std::printf("FAIL [console-ime-close-clear]: IME state survived SetOpen(false)\n");
                return false;
            }
            console::SetOpen(true);

            // Surrogate pair UTF-16 insertion (`U+1F409` dragon emoji = `\xF0\x9F\x90\x89`)
            console::OnKeyDown('U', true, false, false);
            const wchar_t kDragonSurrogate[2] = { static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDC09) };
            console::InsertUtf16(kDragonSurrogate, 2);
            console::CaptureSnapshot(0U, snap);
            if (snap.inputLine != "\xF0\x9F\x90\x89") {
                std::printf("FAIL [console-ime-surrogate]: unexpected UTF-8 output (len=%zu)\n", snap.inputLine.size());
                return false;
            }

            // Reverse-i-search with CJK UTF-8 history & CJK search query
            if (!runConsoleLine(u8"endemic rare \u6708\u7FBD\u5929\u6BCD status")) {
                std::printf("FAIL [console-ime-cjk-history-exec]\n");
                return false;
            }
            console::OnKeyDown('U', true, false, false);
            console::OnKeyDown('R', true, false, false);
            console::InsertUtf8(u8"\u6708\u7FBD");
            console::CaptureSnapshot(0U, snap);
            if (snap.inputLine != u8"endemic rare \u6708\u7FBD\u5929\u6BCD status" || snap.searchPrompt.empty()) {
                std::printf("FAIL [console-ime-cjk-ctrlr]: got '%s'\n", snap.inputLine.c_str());
                return false;
            }
            console::OnKeyDown(VK_ESCAPE, false, false, false); // exit Ctrl+R
        }

        console::OnKeyDown(VK_ESCAPE, false, false, false); // close console
        if (console::IsOpen()) {
            std::printf("FAIL [console-esc]: Esc did not close console\n");
            return false;
        }
        cmd_router::Init(&cfg, &CaptureChatSink);
        std::printf("[OK] Terminal core (UTF-8/IME readline, history, Ctrl+R, dynamic plugin Tab completion, chat echo toggle) verified!\n");
    }

    return true;
}

