#include "test_env.h"

#include <cctype>

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

void ResetSuiteState(TestEnv& env) {
    if (const MhwRngHostApi* host = MhwRng_GetHostApi()) {
        host->ClearAllRules();
        host->SetEngineMode(kMhwRngModeTakeover);
        host->ResetEpoch();
    }
    MhwRng_SetLanguage("zh");
    cmd_router::Init(&env.cfg, &CaptureChatSink);
    g_capturedChatLines.clear();
    g_capturedAsyncChatLines.clear();
    *reinterpret_cast<void**>(env.base + 0x5012908U) = nullptr;
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

namespace {

struct SuiteEntry {
    const char* name;
    const char* desc;
    bool (*fn)(TestEnv&);
};

std::string LowerAsciiCopy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    static constexpr SuiteEntry kSuites[] = {
        { "kernel_threads",     "Test 1: 8-thread stream isolation & O(1) Epoch rewind",                  &RunSuite_KernelThreads     },
        { "plugin_size",        "Tests 3-6: rng_size (Rajang/FuriousRajang, 0-weight skip, exact)",       &RunSuite_PluginSize        },
        { "plugin_endemic_sz",  "Test 7: rng_endemic size tiers, capture/field & exact cm/scale solver",   &RunSuite_PluginEndemicSize },
        { "plugin_thk",         "Test 8: rng_thk Fatalis P1 preset & generic THK VM engine",              &RunSuite_PluginThk         },
        { "kernel_hotload",     "Test 9: Zero-lock shadow-copy reload/unload & inspector dump/top",       &RunSuite_KernelHotload     },
        { "kernel_chat_wx",     "Tests 10 & 12: sChat chaining/self-heal & 64KB W^X standalone pool",     &RunSuite_KernelChatWx      },
        { "sdk_v3_core",        "Test 11 (V3-1..V3-8): GPR snapshot, priority chains, snapshots, heap",  &RunSuite_SdkV3Core         },
        { "plugin_steam",       "Test 11 (V3-9): rng_steam ObserveOnly hints, lock, pity & reload",       &RunSuite_PluginSteam       },
        { "kernel_selective",   "Test 13: Selective mode & 31 ProtectSaveSeeds RVAs",                     &RunSuite_KernelSelective   },
        { "cmd_router",         "Test 14: cmd_router lang/rawseq/reload delta/exec recursion guard",      &RunSuite_CmdRouter         },
        { "kernel_wrappers_mt", "Tests 15-17: Transparent wrappers, mt19937 policy & concurrent drain",   &RunSuite_KernelWrappersMt  },
        { "sdk_v3_filters",     "Test 18: Chained FilterVanilla & FilterSlot across modes",               &RunSuite_SdkV3Filters      },
        { "sdk_v3_trace",       "Test 19: XMM6/7 capture, 4-hop .pdata CaptureCallerRvas & $rng trace",   &RunSuite_SdkV3Trace        },
        { "i18n_en_sweep",      "Test 20: SDK v3.3 i18n API & zero-CJK sweep in $rng lang en",            &RunSuite_I18nEnSweep       },
        { "console_tab",        "Test 21: Overlay console core, IME state, Ctrl+R & Tab completion",      &RunSuite_ConsoleTab        },
        { "plugin_crit",        "Test 22: rng_crit modes, coop passthrough, affinity & hot reload",       &RunSuite_PluginCrit        },
        { "plugin_endemic_rare","Test 23: rng_endemic rare/GL Moly/group/mutex tie-breaker & async log",  &RunSuite_PluginEndemicRare },
        { "sdk_v35",            "Test 24: SDK v3.5 tick, event bus & concurrent ServiceGuard reload",     &RunSuite_SdkV35            },
        { "world_e2e",          "Test 25: mhw_world.dll + 5 plugins + mhw.catalog + steam showcase",      &RunSuite_WorldE2E          },
        { "plugin_names",       "Test 26: Plugin shortName rules ([a-z_], 1..24) & collision auto-suffix", &RunSuite_PluginNames       },
    };

    bool listOnly = false;
    std::string textBinCli;
    std::vector<std::string> filters;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        const std::string a = argv[i];
        if (a == "--list" || a == "-l") {
            listOnly = true;
        } else if (a == "--filter" && i + 1 < argc && argv[i + 1]) {
            filters.push_back(LowerAsciiCopy(argv[++i]));
        } else if (a.rfind("--filter=", 0) == 0) {
            filters.push_back(LowerAsciiCopy(a.substr(9)));
        } else if (!a.empty() && a[0] != '-') {
            textBinCli = a;
        }
    }

    if (listOnly) {
        std::printf("Available test suites (%zu):\n", sizeof(kSuites) / sizeof(kSuites[0]));
        for (const auto& s : kSuites) {
            std::printf("  %-20s %s\n", s.name, s.desc);
        }
        return 0;
    }

    std::printf("=== MHW RNG Microkernel v3.5 + Official Plugin Audit Suite ===\n");

    auto* img = static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!img) return 1;

    char modPath[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, modPath, MAX_PATH);
    std::string buildDir(modPath);
    buildDir = buildDir.substr(0, buildDir.find_last_of("\\/"));

    FILE* tf = nullptr;
    std::vector<std::string> textBinCands;
    if (!textBinCli.empty()) textBinCands.push_back(textBinCli);
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
        ::VirtualFree(img, 0, MEM_RELEASE);
        return 1;
    }
    const std::size_t nRead = std::fread(img + 0x1000, 1, 0x03000000ULL, tf);
    std::fclose(tf);
    std::printf("[OK] Mapped text_1523.bin (%zu bytes) at %p\n", nRead, img);

    const auto base = reinterpret_cast<std::uintptr_t>(img);
    const auto end  = base + kImageSize;

    *reinterpret_cast<float*>(base + 0x2F173C4U) = 100.0f / 4294967296.0f;
    *reinterpret_cast<float*>(base + 0x2F12798U) = 1.0f / 4294967296.0f;
    *reinterpret_cast<float*>(base + 0x2E4FE48U) = 1.192092896e-7f;
    *reinterpret_cast<float*>(base + 0x2E4FE58U) = 1.0f;
    *reinterpret_cast<float*>(base + 0x2F127C4U) = 1000.0f;
    *reinterpret_cast<float*>(base + 0x2F17850U) = -1000.0f;
    *reinterpret_cast<float*>(base + 0x2F0F354U) = 0.01f;
    *reinterpret_cast<float*>(base + 0x2F16EC4U) = 100.0f;

    static constexpr std::uint8_t kRbxCallerThunk[] = {
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xD8,
        0x41, 0xFF, 0xD1, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3
    };
    static constexpr std::uint8_t kEmThinkUpdateThunk[] = {
        0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40,
        0x48, 0x89, 0x6C, 0x24, 0x68,
        0x4C, 0x89, 0x74, 0x24, 0x70,
        0x48, 0x8B, 0xF1, 0x48, 0x8B, 0xFA, 0x41, 0xFF, 0xE0
    };
    auto* thunkMem = static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!thunkMem) return 1;
    std::memcpy(thunkMem, kRbxCallerThunk, sizeof(kRbxCallerThunk));
    std::memcpy(thunkMem + 0x100U, kEmThinkUpdateThunk, sizeof(kEmThinkUpdateThunk));

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

    const std::string plugDir = buildDir + "\\mhw_rng";
    plugin_host::Init(base, end, plugDir, &CaptureChatSink, &CaptureAsyncChatSink);
    const std::size_t loadedCnt = plugin_host::LoadAllFromDirectory();
    if (loadedCnt != 7U) {
        std::printf("FAIL: Expected == 7 plugins in %s, loaded %zu\n", plugDir.c_str(), loadedCnt);
        return 1;
    }
    std::printf("[OK] Shadow-copy loaded %zu official modules (`world` + `inspector` + `size` + `endemic` + `thk` + `steam` + `crit`)!\n", loadedCnt);
    cmd_router::Init(&cfg, &CaptureChatSink);

    TestEnv env{};
    env.img                 = img;
    env.base                = base;
    env.end                 = end;
    env.thunkMem            = thunkMem;
    env.buildDir            = buildDir;
    env.plugDir             = plugDir;
    env.cfg                 = cfg;
    env.fnNrand             = reinterpret_cast<TestNrandFn>(base + mhw::kRvaMtRandomNrand);
    env.fnSphere            = reinterpret_cast<TestSphereFn>(base + mhw::kRvaMtRandomSphere);
    env.sInst               = reinterpret_cast<std::uint32_t*>(base + mhw::kRvaMtRandomInstance);
    env.callSizeWithMonster = reinterpret_cast<CallWithMonsterRbxFn>(thunkMem);
    env.callEmThinkUpdate   = reinterpret_cast<EmThinkUpdateThunkFn>(thunkMem + 0x100U);

    auto matchesFilters = [&](const char* suiteName) -> bool {
        if (filters.empty()) return true;
        const std::string low = LowerAsciiCopy(suiteName);
        for (const auto& f : filters) {
            if (low.find(f) != std::string::npos) return true;
        }
        return false;
    };

    std::size_t ranCount = 0U;
    bool allOk = true;
    for (const auto& s : kSuites) {
        if (!matchesFilters(s.name)) continue;
        ResetSuiteState(env);
        ++ranCount;
        if (!s.fn(env)) {
            std::printf("FAIL: Suite '%s' failed!\n", s.name);
            allOk = false;
            break;
        }
    }

    plugin_host::ShutdownAll();
    hook::RemoveAllHooks();
    rng::Shutdown();
    if (std::memcmp(img + mhw::kRvaMtRandomNrand, mhw::kPrologueMtRandomNrand, sizeof(mhw::kPrologueMtRandomNrand)) != 0 ||
        std::memcmp(img + mhw::kRvaSChatMove, mhw::kPrologueSChatMove, sizeof(mhw::kPrologueSChatMove)) != 0) {
        std::printf("FAIL: RemoveAllHooks failed\n");
        ::VirtualFree(thunkMem, 0, MEM_RELEASE);
        ::VirtualFree(img, 0, MEM_RELEASE);
        return 1;
    }
    ::VirtualFree(thunkMem, 0, MEM_RELEASE);
    ::VirtualFree(img, 0, MEM_RELEASE);

    if (!allOk) return 1;
    if (ranCount == 0U) {
        std::printf("FAIL: No test suites matched filter!\n");
        return 1;
    }
    if (filters.empty()) {
        std::printf("=== ALL MICROKERNEL v3.5 & OFFICIAL PLUGIN AUDIT TESTS PASSED ===\n");
    } else {
        std::printf("=== FILTERED AUDIT PASSED (%zu suite(s)) ===\n", ranCount);
    }
    return 0;
}
