#pragma once
// Shared harness context & modular suite declarations for MIRA offline machine-code tests.
#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#include "command_router.h"
#include "console_core.h"
#include "hook.h"
#include "log_config.h"
#include "mhw_addresses.h"
#include "mhw_rng_api.h"
#include "mhw_rng_math.h"
#include "mhw_1523_world.h"
#include "mhw_world_api.h"
#include "mhw_catalog_api.h"
#include "plugin_host.h"
#include "rng_engine.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

constexpr std::size_t kImageSize = 0x06000000ULL;

using TestNrandFn          = std::uint32_t(__fastcall*)(std::uint32_t*);
using TestCtorSeedFn       = std::uint32_t*(__fastcall*)(std::uint32_t*, std::uint32_t);
using TestSphereFn         = float*(__fastcall*)(std::uint32_t*, float*);
using TestMtRangeFn        = float(__fastcall*)(void*, float, float);
using CallWithMonsterRbxFn = std::uint32_t(__fastcall*)(void* holder, std::uint32_t* tblIdxPtr, void* monster, void* targetFn);
using EmThinkUpdateThunkFn = void(__fastcall*)(void* thinkEm, void* monster, void* targetAddr);
using EndemicCaptureRollFn = std::uint32_t(__fastcall*)(std::int32_t animalId, std::int32_t animalType, float* outScale);
using SharedFloatRangeFn   = float(__fastcall*)(float minVal, float maxVal);

struct TestEnv {
    std::uint8_t*        img                 = nullptr;
    std::uintptr_t       base                = 0;
    std::uintptr_t       end                 = 0;
    std::uint8_t*        thunkMem            = nullptr;
    std::string          buildDir;
    std::string          plugDir;
    app::Config          cfg{};
    TestNrandFn          fnNrand             = nullptr;
    TestSphereFn         fnSphere            = nullptr;
    std::uint32_t*       sInst               = nullptr;
    CallWithMonsterRbxFn callSizeWithMonster = nullptr;
    EmThinkUpdateThunkFn callEmThinkUpdate   = nullptr;
};

extern volatile std::uint32_t g_antiTailCallSink;
extern std::vector<std::string> g_capturedChatLines;
extern std::vector<std::string> g_capturedAsyncChatLines;

void CaptureChatSink(const char* utf8Text) noexcept;
void CaptureAsyncChatSink(const char* utf8Text) noexcept;
void ResetSuiteState(TestEnv& env);

__declspec(noinline) std::uint32_t CallNrandSiteA(TestNrandFn fn, std::uint32_t* sInst);
__declspec(noinline) std::uint32_t CallNrandSiteB(TestNrandFn fn, std::uint32_t* sInst);
__declspec(noinline) std::uint32_t CallNrandV3Chain(TestNrandFn fn, std::uint32_t* sInst);
__declspec(noinline) std::uint32_t CallNrandV3Filter(TestNrandFn fn, std::uint32_t* sInst);
__declspec(noinline) std::uint32_t CallNrandV3Observe(TestNrandFn fn, std::uint32_t* sInst);
__declspec(noinline) std::uint32_t CallNrandV3Priv(TestNrandFn fn, std::uint32_t* privInst);
__declspec(noinline) std::uint32_t* CallCtorSeedV3Priv(TestCtorSeedFn fn, std::uint32_t* privInst, std::uint32_t seed);
__declspec(noinline) float CallMtRangeV3(TestMtRangeFn fn, void* ctx, float lo, float hi);
__declspec(noinline) std::uint32_t CallNoiseWorker(TestNrandFn fn, std::uint32_t* sInst);
__declspec(noinline) void CallSphereSite(TestSphereFn fn, std::uint32_t* sInst, float* out3);

bool RunSuite_KernelThreads(TestEnv& env);
bool RunSuite_PluginSize(TestEnv& env);
bool RunSuite_PluginEndemicSize(TestEnv& env);
bool RunSuite_PluginThk(TestEnv& env);
bool RunSuite_KernelHotload(TestEnv& env);
bool RunSuite_KernelChatWx(TestEnv& env);
bool RunSuite_SdkV3Core(TestEnv& env);
bool RunSuite_PluginSteam(TestEnv& env);
bool RunSuite_KernelSelective(TestEnv& env);
bool RunSuite_CmdRouter(TestEnv& env);
bool RunSuite_KernelWrappersMt(TestEnv& env);
bool RunSuite_SdkV3Filters(TestEnv& env);
bool RunSuite_SdkV3Trace(TestEnv& env);
bool RunSuite_I18nEnSweep(TestEnv& env);
bool RunSuite_ConsoleTab(TestEnv& env);
bool RunSuite_PluginCrit(TestEnv& env);
bool RunSuite_PluginEndemicRare(TestEnv& env);
bool RunSuite_SdkV35(TestEnv& env);
bool RunSuite_WorldE2E(TestEnv& env);
bool RunSuite_PluginNames(TestEnv& env);
