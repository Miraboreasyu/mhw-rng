#ifndef MHW_RNG_EXPORTS
#define MHW_RNG_EXPORTS
#endif
#include "console_overlay.h"
#include "command_router.h"
#include "console_core.h"
#include "log_config.h"
#include "mhw_addresses.h"
#include "mhw_rng_api.h"
#include "peer_beacon.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace console_overlay {
namespace {

constexpr const char* kConsoleHlsl = R"(
cbuffer ConsoleCB : register(b0) {
    float2 gPanelOrigin;
    float2 gPanelSize;
    float2 gInvBbSize;
    float  gTargetNits;
    uint   gHdrMode;
};

Texture2D<float4> tConsole : register(t0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    static const float2 kQuad[6] = {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(1.0f, 0.0f),
        float2(1.0f, 1.0f),
        float2(0.0f, 1.0f)
    };
    float2 q  = kQuad[vid];
    float2 px = gPanelOrigin + q * gPanelSize;
    VSOut o;
    o.pos = float4(px.x * gInvBbSize.x * 2.0f - 1.0f, 1.0f - px.y * gInvBbSize.y * 2.0f, 0.0f, 1.0f);
    o.uv  = q;
    return o;
}

float3 SrgbToLinear3(float3 c) {
    c = saturate(c);
    float3 lo = c / 12.92f;
    float3 hi = pow(max(c + 0.055f, 1e-5f) / 1.055f, 2.4f);
    return (c <= 0.04045f) ? lo : hi;
}

float3 LinearToSt2084Pq3(float3 lNorm) {
    lNorm = saturate(lNorm);
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float3 ym = pow(max(lNorm, 0.0f), m1);
    return pow((c1 + c2 * ym) / (1.0f + c3 * ym), m2);
}

float4 PSMain(VSOut i) : SV_TARGET {
    int2 maxTex = max(int2(gPanelSize) - int2(1, 1), int2(0, 0));
    int2 tp     = clamp(int2(i.uv * gPanelSize), int2(0, 0), maxTex);
    float4 col  = tConsole.Load(int3(tp, 0));
    if (gHdrMode == 0u) {
        return col;
    }
    float3 lin = SrgbToLinear3(col.rgb);
    if (gHdrMode == 2u) {
        return float4(lin * (gTargetNits / 80.0f), col.a);
    }
    float3 r2020 = float3(
        0.6274040f * lin.r + 0.3292820f * lin.g + 0.0433136f * lin.b,
        0.0690970f * lin.r + 0.9195400f * lin.g + 0.0113612f * lin.b,
        0.0163916f * lin.r + 0.0880132f * lin.g + 0.8955950f * lin.b);
    return float4(LinearToSt2084Pq3(r2020 * (gTargetNits / 10000.0f)), col.a);
}
)";

struct alignas(16) ConsoleConstants {
    float         panelOriginX;
    float         panelOriginY;
    float         panelW;
    float         panelH;
    float         invBbW;
    float         invBbH;
    float         targetNits;
    std::uint32_t hdrMode; // 0 = SDR, 1 = HDR10 PQ, 2 = scRGB
};
static_assert(sizeof(ConsoleConstants) == 32, "ConsoleConstants must be 32 bytes");

ID3DBlob* g_vsBlob          = nullptr;
ID3DBlob* g_psBlob          = nullptr;
bool      g_shaderTried     = false;

constexpr const char* kAuxHlsl = R"(
cbuffer AuxCB : register(b0) {
    float4 gCellW[48];
    float4 gParams;
};

struct AuxVSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

AuxVSOut VSAux(uint vid : SV_VertexID) {
    float2 p = float2((vid == 2u) ? 3.0f : -1.0f, (vid == 1u) ? -3.0f : 1.0f);
    AuxVSOut o;
    o.pos = float4(p, 0.0f, 1.0f);
    o.uv  = float2(p.x * 0.5f + 0.5f, 0.5f - p.y * 0.5f);
    return o;
}

float4 PSAux(AuxVSOut i) : SV_TARGET {
    if (i.uv.x < 0.05f || i.uv.x >= 0.95f || i.uv.y < 0.05f || i.uv.y >= 0.95f) discard;
    float gx = (i.uv.x - 0.05f) * (16.0f / 0.90f);
    float gy = (i.uv.y - 0.05f) * (12.0f / 0.90f);
    uint c = min(15u, (uint)gx);
    uint r = min(11u, (uint)gy);
    float u = gx - (float)c;
    float v = gy - (float)r;

    float su = sin(3.14159265f * u);
    float sv = sin(3.14159265f * v);
    float hu = su * su;
    float hv = sv * sv;
    float shape = (((r + c) & 1u) == 0u)
        ? (2.59807621f * hu * cos(3.14159265f * u) * hv)
        : (2.59807621f * hv * cos(3.14159265f * v) * hu);

    uint cid = r * 16u + c;
    float4 vec = gCellW[cid >> 2u];
    uint lane = cid & 3u;
    float cw = (lane == 0u) ? vec.x : ((lane == 1u) ? vec.y : ((lane == 2u) ? vec.z : vec.w));

    float e  = gParams.x * cw * shape;
    float ae = abs(e);
    if (ae < 1e-5f) discard;

    if (gParams.z > 0.0f && ae < gParams.z) {
        float ign = frac(52.9829189f * frac(dot(i.pos.xy, float2(0.06711056f, 0.00583715f))));
        ae = (ign * gParams.z < ae) ? gParams.z : 0.0f;
        if (ae <= 0.0f) discard;
    }

    float eB = ae;
    float eR = ae * gParams.y;
    bool isAdd = (gParams.w < 0.5f);
    if (e >= 0.0f) {
        return isAdd ? float4(0.0f, 0.0f, eB, 0.0f) : float4(eR, 0.0f, 0.0f, 0.0f);
    }
    return isAdd ? float4(eR, 0.0f, 0.0f, 0.0f) : float4(0.0f, 0.0f, eB, 0.0f);
}
)";

struct alignas(16) MarkCbData {
    float        cellW[192];
    float        params[4];
    std::uint8_t pad[240];
};
static_assert(sizeof(MarkCbData) == 1024U, "MarkCbData must be 256-byte aligned");

ID3DBlob* g_markVsBlob      = nullptr;
ID3DBlob* g_markPsBlob      = nullptr;
bool      g_markShaderTried = false;

bool CompileSourceEntry(pD3DCompile fnCompile,
                        const char* src,
                        const char* srcName,
                        const char* entry,
                        const char* target,
                        ID3DBlob** outBlob) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = fnCompile(src, std::strlen(src), srcName,
                                 nullptr, nullptr, entry, target,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, outBlob, &err);
    if (FAILED(hr)) {
        app::Log("console: shader %s (%s) compile failed hr=0x%08lX (%s)",
                 entry, target, hr, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

pD3DCompile ResolveD3DCompileFn() {
    HMODULE d3dc = ::GetModuleHandleW(L"d3dcompiler_47.dll");
    if (!d3dc) d3dc = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3dc) {
        app::Log("console: failed to load d3dcompiler_47.dll (err=%lu)", ::GetLastError());
        return nullptr;
    }
    auto fnCompile = reinterpret_cast<pD3DCompile>(::GetProcAddress(d3dc, "D3DCompile"));
    if (!fnCompile) {
        app::Log("console: D3DCompile export not found");
        return nullptr;
    }
    return fnCompile;
}

bool EnsureShaderBlobs() {
    if (g_vsBlob && g_psBlob) return true;
    if (g_shaderTried) return false;
    g_shaderTried = true;

    auto fnCompile = ResolveD3DCompileFn();
    if (!fnCompile) return false;
    return CompileSourceEntry(fnCompile, kConsoleHlsl, "console_overlay.hlsl", "VSMain", "vs_4_0", &g_vsBlob) &&
           CompileSourceEntry(fnCompile, kConsoleHlsl, "console_overlay.hlsl", "PSMain", "ps_4_0", &g_psBlob);
}

bool EnsureMarkShaderBlobs() {
    if (g_markVsBlob && g_markPsBlob) return true;
    if (g_markShaderTried) return false;
    g_markShaderTried = true;

    auto fnCompile = ResolveD3DCompileFn();
    if (!fnCompile) return false;
    return CompileSourceEntry(fnCompile, kAuxHlsl, "console_aux.hlsl", "VSAux", "vs_4_0", &g_markVsBlob) &&
           CompileSourceEntry(fnCompile, kAuxHlsl, "console_aux.hlsl", "PSAux", "ps_4_0", &g_markPsBlob);
}

void ComputeMarkParams(std::uint32_t hdrMode,
                       bool debugVis,
                       float& outEpsB,
                       float& outRedRatio,
                       float& outIgnPulse) noexcept {
    if (hdrMode == 2U) {
        outEpsB     = 0.030f;
        outRedRatio = 0.340f;
        outIgnPulse = 0.0f;
    } else if (hdrMode == 1U) {
        outEpsB     = 0.010f;
        outRedRatio = 0.226f;
        outIgnPulse = 0.0f;
    } else {
        outEpsB     = 0.014f;
        outRedRatio = 0.340f;
        outIgnPulse = 0.022f;
    }
    if (debugVis) {
        const float boosted = outEpsB * 16.0f;
        outEpsB     = (boosted < 0.45f) ? boosted : 0.45f;
        outIgnPulse = 0.0f;
    }
}

void PackMarkBuffers(const peer_beacon::VideoMarkFrame& f,
                     float epsB,
                     float redRatio,
                     float ignPulse,
                     MarkCbData& outAdd,
                     MarkCbData& outSub) noexcept {
    std::memcpy(outAdd.cellW, f.cellWeights, sizeof(outAdd.cellW));
    std::memcpy(outSub.cellW, f.cellWeights, sizeof(outSub.cellW));
    outAdd.params[0] = epsB;
    outAdd.params[1] = redRatio;
    outAdd.params[2] = ignPulse;
    outAdd.params[3] = 0.0f;
    outSub.params[0] = epsB;
    outSub.params[1] = redRatio;
    outSub.params[2] = ignPulse;
    outSub.params[3] = 1.0f;
}

inline bool IsValidPtr(std::uintptr_t p) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & 0x7ULL) == 0ULL;
}

DXGI_FORMAT ResolveTypedRtvFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:                                return fmt;
    }
}

void ResolveHdrModeSEH(std::uintptr_t exeBase,
                       DXGI_FORMAT rtvFmt,
                       std::uint32_t& outMode,
                       float& outTargetNits) noexcept {
    outMode       = 0U;
    outTargetNits = 200.0f;
    bool hdrActive = false;
    float whiteNits = 200.0f;
    __try {
        if (exeBase) {
            const std::uintptr_t mhRender = *reinterpret_cast<const std::uintptr_t*>(
                exeBase + mhw::kRvaSMhRenderInstance);
            if (IsValidPtr(mhRender)) {
                hdrActive = (*reinterpret_cast<const std::uint8_t*>(mhRender + mhw::kOffRenderHdrOutput) != 0);
                const float wn = *reinterpret_cast<const float*>(mhRender + mhw::kOffRenderHdrWhiteNits);
                if (wn >= 50.0f && wn <= 2000.0f) whiteNits = wn;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hdrActive = false;
    }

    float target = whiteNits * 0.85f;
    if (target < 120.0f) target = 120.0f;
    if (target > 320.0f) target = 320.0f;
    outTargetNits = target;

    if (rtvFmt == DXGI_FORMAT_R16G16B16A16_FLOAT || rtvFmt == DXGI_FORMAT_R16G16B16A16_TYPELESS) {
        outMode = 2U;
    } else if ((rtvFmt == DXGI_FORMAT_R10G10B10A2_UNORM || rtvFmt == DXGI_FORMAT_R10G10B10A2_TYPELESS) &&
               hdrActive) {
        outMode = 1U;
    }
}

// ============================================================================
//  Dirty-Driven GDI 32-bit BGRA Rasterizer (UTF-8 + CJK + 4K DPI scaling)
// ============================================================================
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], n);
    return w;
}

COLORREF LineColorRgb(std::uint8_t kind) noexcept {
    switch (kind) {
        case console::kColorCommand: return RGB(120, 232, 255);
        case console::kColorKernel:  return RGB(142, 240, 184);
        case console::kColorPlugin:  return RGB(130, 196, 255);
        case console::kColorError:   return RGB(255, 134, 118);
        default:                     return RGB(228, 234, 242);
    }
}

struct WrappedRow {
    std::wstring text;
    COLORREF     color       = 0;
    bool         isCont      = false;
    bool         spaceElided = false;
};

struct SelPos {
    std::size_t row = 0U;
    std::size_t col = 0U;

    bool operator<(const SelPos& o) const noexcept {
        return (row != o.row) ? (row < o.row) : (col < o.col);
    }
    bool operator==(const SelPos& o) const noexcept {
        return row == o.row && col == o.col;
    }
};

std::mutex                 g_uiMx;
std::atomic<bool>          g_draggingAtomic{false};
std::atomic<bool>          g_hasSelectionAtomic{false};
std::atomic<std::uint64_t> g_selGen{1ULL};

bool   g_hasSel             = false;
bool   g_isDragging         = false;
SelPos g_selAnchor{};
SelPos g_selActive{};
UINT64 g_copiedUntilTick    = 0ULL;
UINT64 g_lastAutoScrollTick = 0ULL;
UINT64 g_lastClickTick      = 0ULL;
int    g_lastClickLx        = -10000;
int    g_lastClickLy        = -10000;
int    g_clickStreak        = 0;

inline bool IsWordChar(wchar_t ch) noexcept {
    if (ch >= L'0' && ch <= L'9') return true;
    if (ch >= L'a' && ch <= L'z') return true;
    if (ch >= L'A' && ch <= L'Z') return true;
    return ch == L'_' || ch == L'.' || ch == L':' || ch == L'-' || ch == L'#' || ch == L'$' || ch >= 0x80;
}

struct GdiSurface {
    HDC                     memDc           = nullptr;
    HBITMAP                 dib             = nullptr;
    HBITMAP                 oldBmp          = nullptr;
    void*                   bits            = nullptr;
    HFONT                   fontMono        = nullptr;
    HFONT                   fontHeader      = nullptr;
    UINT                    width           = 0U;
    UINT                    height          = 0U;
    UINT                    bbWidth         = 0U;
    UINT                    bbHeight        = 0U;
    UINT                    panelOrigX      = 0U;
    UINT                    panelOrigY      = 0U;
    float                   scale           = 1.0f;
    int                     fontPx          = 16;
    std::uint64_t           lastGen         = 0ULL;
    std::uint64_t           lastSelGen      = 0ULL;
    bool                    lastBlink       = false;
    bool                    lastCopiedBadge = false;
    std::uint32_t           lastMode        = 99U;
    std::uint64_t           lastSeed        = 0ULL;
    std::uint64_t           lastEpoch       = 0ULL;
    std::uint32_t           lastLangId      = 0U;
    bool                    forceRedraw     = true;
    std::size_t             lastTotalLines  = 0U;

    // Cached layout & wrapped rows for hit-testing and selection highlight/copy
    int                     cachePad        = 10;
    int                     cacheContIndent = 16;
    int                     cacheLogTop     = 32;
    int                     cacheLogBot     = 400;
    int                     cacheLineStep   = 21;
    int                     cacheCaretBbX   = 0;
    int                     cacheCaretBbY   = 0;
    int                     cacheCaretH     = 20;
    std::size_t             visStartRow     = 0U;
    std::size_t             visEndRow       = 0U;
    std::vector<WrappedRow> cachedRows;

    void releaseSurface() {
        if (memDc && oldBmp) {
            ::SelectObject(memDc, oldBmp);
            oldBmp = nullptr;
        }
        if (dib)        { ::DeleteObject(dib);        dib = nullptr; }
        if (memDc)      { ::DeleteDC(memDc);          memDc = nullptr; }
        if (fontMono)   { ::DeleteObject(fontMono);   fontMono = nullptr; }
        if (fontHeader) { ::DeleteObject(fontHeader); fontHeader = nullptr; }
        bits        = nullptr;
        width       = 0U;
        height      = 0U;
        forceRedraw = true;
        visStartRow = 0U;
        visEndRow   = 0U;
        cachedRows.clear();
    }

    bool ensure(UINT w, UINT h, float sc) {
        if (w == 0U || h == 0U) return false;
        const int wantFontPx = std::max(13, static_cast<int>(std::lround(16.0f * sc)));
        if (memDc && dib && bits && width == w && height == h && fontPx == wantFontPx) {
            return true;
        }
        releaseSurface();

        memDc = ::CreateCompatibleDC(nullptr);
        if (!memDc) return false;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = static_cast<LONG>(w);
        bmi.bmiHeader.biHeight      = -static_cast<LONG>(h); // Top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        dib = ::CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            releaseSurface();
            return false;
        }
        oldBmp = static_cast<HBITMAP>(::SelectObject(memDc, dib));

        fontMono = ::CreateFontW(-wantFontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        fontHeader = ::CreateFontW(-std::max(12, static_cast<int>(std::lround(14.0f * sc))),
                                   0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (!fontMono || !fontHeader) {
            releaseSurface();
            return false;
        }
        width       = w;
        height      = h;
        scale       = sc;
        fontPx      = wantFontPx;
        forceRedraw = true;
        return true;
    }

    bool hitTestLocked(int lx, int ly, SelPos& outPos) const {
        if (!memDc || cachedRows.empty() || visEndRow <= visStartRow) return false;
        const std::size_t visLast = visEndRow - 1U;
        std::size_t row = visStartRow;
        if (ly >= cacheLogTop) {
            const std::size_t relRow = static_cast<std::size_t>((ly - cacheLogTop) / std::max(1, cacheLineStep));
            row = std::min(visLast, visStartRow + relRow);
        }
        const WrappedRow& wr = cachedRows[row];
        const int textX = cachePad + (wr.isCont ? cacheContIndent : 0);
        const int relX  = lx - textX;
        std::size_t col = 0U;
        if (relX > 0 && !wr.text.empty()) {
            const auto oldF = ::SelectObject(memDc, fontMono);
            INT fit = 0;
            SIZE sz{};
            if (::GetTextExtentExPointW(memDc, wr.text.data(), static_cast<int>(wr.text.size()),
                                        relX, &fit, nullptr, &sz)) {
                col = static_cast<std::size_t>(std::max(0, fit));
                if (col < wr.text.size()) {
                    SIZE leftSz{}, rightSz{};
                    if (col > 0U) {
                        ::GetTextExtentPoint32W(memDc, wr.text.data(), static_cast<int>(col), &leftSz);
                    }
                    ::GetTextExtentPoint32W(memDc, wr.text.data(), static_cast<int>(col + 1U), &rightSz);
                    if ((relX - leftSz.cx) > (rightSz.cx - relX)) {
                        ++col;
                    }
                }
            }
            ::SelectObject(memDc, oldF);
        }
        outPos = SelPos{row, col};
        return true;
    }

    // Returns true if pixels changed and GPU upload is needed. Sets outClampScroll if console scrollOffset exceeded rows.
    bool updateIfDirty(UINT bbW, UINT bbH, UINT ox, UINT oy, UINT w, UINT h, float sc,
                       bool& outNeedClampScroll, std::size_t& outClampMaxScroll) {
        outNeedClampScroll = false;
        outClampMaxScroll  = 0U;
        if (!ensure(w, h, sc)) return false;
        bbWidth    = bbW;
        bbHeight   = bbH;
        panelOrigX = ox;
        panelOrigY = oy;

        const UINT64        nowTick     = ::GetTickCount64();
        const std::uint64_t gen         = console::Generation();
        const std::uint64_t selGen      = g_selGen.load(std::memory_order_acquire);
        const bool          blink       = (((nowTick / 530ULL) & 1ULL) == 0ULL);
        const bool          copiedBadge = (nowTick < g_copiedUntilTick);
        const std::uint32_t mode        = MhwRng_GetEngineMode();
        const std::uint64_t seed        = MhwRng_GetMasterSeed();
        const std::uint64_t epoch       = MhwRng_GetEpoch();
        const std::uint32_t lang        = MhwRng_GetLanguageId();

        if (!forceRedraw && gen == lastGen && selGen == lastSelGen && blink == lastBlink &&
            copiedBadge == lastCopiedBadge && mode == lastMode && seed == lastSeed &&
            epoch == lastEpoch && lang == lastLangId) {
            return false;
        }

        forceRedraw     = false;
        lastGen         = gen;
        lastSelGen      = selGen;
        lastBlink       = blink;
        lastCopiedBadge = copiedBadge;
        lastMode        = mode;
        lastSeed        = seed;
        lastEpoch       = epoch;
        lastLangId      = lang;

        const int pw       = static_cast<int>(w);
        const int ph       = static_cast<int>(h);
        const int pad      = std::max(8, static_cast<int>(std::lround(10.0f * sc)));
        const int headerH  = std::max(24, static_cast<int>(std::lround(28.0f * sc)));
        const int inputH   = std::max(28, static_cast<int>(std::lround(32.0f * sc)));
        const int lineStep = std::max(fontPx + 4, static_cast<int>(std::lround(21.0f * sc)));
        const int contInd  = std::max(12, static_cast<int>(std::lround(16.0f * sc)));

        // Fill base dark-glass background
        const RECT fullRc{0, 0, pw, ph};
        HBRUSH bgBrush = ::CreateSolidBrush(RGB(14, 18, 24));
        ::FillRect(memDc, &fullRc, bgBrush);
        ::DeleteObject(bgBrush);

        const RECT hdrRc{1, 1, pw - 1, headerH};
        HBRUSH hdrBrush = ::CreateSolidBrush(RGB(22, 30, 42));
        ::FillRect(memDc, &hdrRc, hdrBrush);
        ::DeleteObject(hdrBrush);

        const RECT inpRc{1, ph - inputH, pw - 1, ph - 1};
        HBRUSH inpBrush = ::CreateSolidBrush(RGB(10, 14, 20));
        ::FillRect(memDc, &inpRc, inpBrush);
        ::DeleteObject(inpBrush);

        // Draw crisp separators & outer border
        HBRUSH borderBrush = ::CreateSolidBrush(RGB(68, 136, 188));
        ::FrameRect(memDc, &fullRc, borderBrush);
        ::DeleteObject(borderBrush);

        HBRUSH sepBrush = ::CreateSolidBrush(RGB(44, 72, 102));
        const RECT hdrSep{1, headerH, pw - 1, headerH + 1};
        const RECT inpSep{1, ph - inputH - 1, pw - 1, ph - inputH};
        ::FillRect(memDc, &hdrSep, sepBrush);
        ::FillRect(memDc, &inpSep, sepBrush);
        ::DeleteObject(sepBrush);

        ::SetBkMode(memDc, TRANSPARENT);

        // Header text
        const auto oldFont = ::SelectObject(memDc, fontHeader);
        char hdrLeft[160]{};
        std::snprintf(hdrLeft, sizeof(hdrLeft),
                      "MIRA Console v3.3  |  %s  |  Seed:%llu  |  Epoch:%llu  |  %s",
                      cmd_router::EngineModeLabelAscii(mode),
                      static_cast<unsigned long long>(seed),
                      static_cast<unsigned long long>(epoch),
                      MhwRng_GetLanguage());
        const std::wstring wHdrLeft = Utf8ToWide(hdrLeft);
        RECT hdrTextRc{pad, 2, pw - pad, headerH};
        ::SetTextColor(memDc, RGB(150, 224, 255));
        ::DrawTextW(memDc, wHdrLeft.c_str(), static_cast<int>(wHdrLeft.size()), &hdrTextRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        const wchar_t* wHdrRight = copiedBadge
                                       ? L"[Copied!]  [`] Close  [Tab] Complete  [Drag/RClick] Copy"
                                       : L"[`] Close  [Tab] Complete  [Drag/RClick] Copy";
        ::SetTextColor(memDc, copiedBadge ? RGB(130, 245, 168) : RGB(148, 166, 186));
        ::DrawTextW(memDc, wHdrRight, -1, &hdrTextRc,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // Capture full buffered lines & wrap into display rows so scroll clamping matches rendered rows
        ::SelectObject(memDc, fontMono);
        console::ConsoleSnapshot snap{};
        console::CaptureSnapshot(0U, snap);

        if (snap.totalLines < lastTotalLines) {
            g_hasSel = false;
            g_hasSelectionAtomic.store(false, std::memory_order_release);
        }
        lastTotalLines = snap.totalLines;

        const bool hasImeCands = !snap.imeCandidates.empty();
        const bool hasAuxBar   = hasImeCands || !snap.searchPrompt.empty() || !snap.completionHint.empty();
        const int auxH = hasAuxBar ? std::max(22, static_cast<int>(std::lround(24.0f * sc))) : 0;
        const int logTop = headerH + 5;
        const int logBot = ph - inputH - 3 - auxH;

        cachePad        = pad;
        cacheContIndent = contInd;
        cacheLogTop     = logTop;
        cacheLogBot     = logBot;
        cacheLineStep   = lineStep;

        if (hasAuxBar) {
            const RECT auxRc{1, ph - inputH - 1 - auxH, pw - 1, ph - inputH - 1};
            HBRUSH auxBrush = ::CreateSolidBrush(hasImeCands ? RGB(20, 34, 50) : RGB(24, 38, 52));
            ::FillRect(memDc, &auxRc, auxBrush);
            ::DeleteObject(auxBrush);

            if (hasImeCands) {
                HBRUSH topSep = ::CreateSolidBrush(RGB(64, 128, 180));
                const RECT auxTopSep{1, auxRc.top, pw - 1, auxRc.top + 1};
                ::FillRect(memDc, &auxTopSep, topSep);
                ::DeleteObject(topSep);

                const std::size_t pageSz = snap.imeCandidates.size();
                char hdrBuf[48]{};
                if (snap.imeCandTotal > pageSz && pageSz > 0U) {
                    const std::size_t curPage    = (snap.imeCandPageStart / pageSz) + 1U;
                    const std::size_t totalPages = (snap.imeCandTotal + pageSz - 1U) / pageSz;
                    std::snprintf(hdrBuf, sizeof(hdrBuf), "[IME %zu/%zu]", curPage, totalPages);
                } else {
                    std::snprintf(hdrBuf, sizeof(hdrBuf), "[IME]");
                }
                const std::wstring wPref = Utf8ToWide(hdrBuf);
                const int textTop = auxRc.top + std::max(1, (auxH - fontPx) / 2);
                const int itemGap = std::max(10, static_cast<int>(std::lround(12.0f * sc)));
                int x = pad;

                SIZE prefSz{};
                ::GetTextExtentPoint32W(memDc, wPref.c_str(), static_cast<int>(wPref.size()), &prefSz);
                ::SetTextColor(memDc, RGB(124, 212, 255));
                ::ExtTextOutW(memDc, x, textTop, ETO_CLIPPED, &auxRc,
                              wPref.c_str(), static_cast<UINT>(wPref.size()), nullptr);
                x += prefSz.cx + itemGap;

                struct CandItem {
                    std::wstring wNum;
                    std::wstring wWord;
                    int          numW  = 0;
                    int          wordW = 0;
                    int          totW  = 0;
                };
                std::vector<CandItem> items(pageSz);
                for (std::size_t i = 0U; i < pageSz; ++i) {
                    const unsigned numDigit = (i == 9U) ? 0U : static_cast<unsigned>(i + 1U);
                    wchar_t nbuf[8]{};
                    std::swprintf(nbuf, 8, L"%u.", numDigit);
                    items[i].wNum  = nbuf;
                    items[i].wWord = Utf8ToWide(snap.imeCandidates[i]);
                    SIZE nSz{}, wSz{};
                    ::GetTextExtentPoint32W(memDc, items[i].wNum.c_str(), static_cast<int>(items[i].wNum.size()), &nSz);
                    if (!items[i].wWord.empty()) {
                        ::GetTextExtentPoint32W(memDc, items[i].wWord.c_str(), static_cast<int>(items[i].wWord.size()), &wSz);
                    }
                    items[i].numW  = nSz.cx;
                    items[i].wordW = wSz.cx;
                    items[i].totW  = nSz.cx + wSz.cx;
                }

                const int rightEdge = pw - pad;
                const std::size_t selIdx = std::min(snap.imeCandSelected, pageSz - 1U);
                std::size_t firstVis = 0U;
                while (firstVis < selIdx) {
                    int span = x;
                    for (std::size_t k = firstVis; k <= selIdx; ++k) {
                        if (k > firstVis) span += itemGap;
                        span += items[k].totW;
                    }
                    if (span <= rightEdge) break;
                    ++firstVis;
                }

                HBRUSH selCandBrush = ::CreateSolidBrush(RGB(42, 98, 164));
                for (std::size_t i = firstVis; i < pageSz; ++i) {
                    if (i > firstVis && x + items[i].totW > rightEdge) break;
                    const bool isSel = (i == selIdx);
                    if (isSel) {
                        const RECT hiRc{std::max(1, x - 3), auxRc.top + 2, std::min(rightEdge, x + items[i].totW + 3), auxRc.bottom - 2};
                        ::FillRect(memDc, &hiRc, selCandBrush);
                        ::SetTextColor(memDc, RGB(255, 244, 176));
                        ::ExtTextOutW(memDc, x, textTop, ETO_CLIPPED, &auxRc,
                                      items[i].wNum.c_str(), static_cast<UINT>(items[i].wNum.size()), nullptr);
                        ::ExtTextOutW(memDc, x + items[i].numW, textTop, ETO_CLIPPED, &auxRc,
                                      items[i].wWord.c_str(), static_cast<UINT>(items[i].wWord.size()), nullptr);
                    } else {
                        ::SetTextColor(memDc, RGB(126, 174, 212));
                        ::ExtTextOutW(memDc, x, textTop, ETO_CLIPPED, &auxRc,
                                      items[i].wNum.c_str(), static_cast<UINT>(items[i].wNum.size()), nullptr);
                        ::SetTextColor(memDc, RGB(234, 240, 248));
                        ::ExtTextOutW(memDc, x + items[i].numW, textTop, ETO_CLIPPED, &auxRc,
                                      items[i].wWord.c_str(), static_cast<UINT>(items[i].wWord.size()), nullptr);
                    }
                    x += items[i].totW + itemGap;
                    if (x >= rightEdge) break;
                }
                ::DeleteObject(selCandBrush);
            } else {
                const std::string& auxStr = !snap.searchPrompt.empty() ? snap.searchPrompt : snap.completionHint;
                const std::wstring wAux = Utf8ToWide(auxStr);
                RECT auxTextRc{pad, auxRc.top, pw - pad, auxRc.bottom};
                ::SetTextColor(memDc, !snap.searchPrompt.empty() ? RGB(255, 214, 102) : RGB(164, 216, 255));
                ::DrawTextW(memDc, wAux.c_str(), static_cast<int>(wAux.size()), &auxTextRc,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }

        const int availLogW = std::max(100, pw - pad * 2 - 14);
        const int maxRows   = std::max(1, (logBot - logTop) / lineStep);
        cachedRows.clear();
        cachedRows.reserve(snap.lines.size() + 16U);

        for (const auto& ol : snap.lines) {
            const std::wstring wline = Utf8ToWide(ol.text);
            const COLORREF col = LineColorRgb(ol.colorKind);
            if (wline.empty()) {
                cachedRows.push_back(WrappedRow{L"", col, false, false});
                continue;
            }
            std::size_t off = 0U;
            bool nextSpaceElided = false;
            while (off < wline.size()) {
                const bool isCont = (off > 0U);
                const bool spaceElided = nextSpaceElided;
                nextSpaceElided = false;
                const int rowMaxW = isCont ? std::max(80, availLogW - contInd) : availLogW;
                const int rem = static_cast<int>(wline.size() - off);
                INT fit = 0;
                SIZE sz{};
                if (!::GetTextExtentExPointW(memDc, wline.data() + off, rem, rowMaxW, &fit, nullptr, &sz) ||
                    fit <= 0) {
                    fit = 1;
                }
                std::size_t take = static_cast<std::size_t>(fit);
                std::size_t nextOff = off + take;
                if (nextOff < wline.size() && take > 4U) {
                    const std::size_t minBreak = take / 2U;
                    for (std::size_t k = take; k > minBreak; --k) {
                        if (wline[off + k] == L' ') {
                            take = k;
                            nextOff = off + k;
                            while (nextOff < wline.size() && wline[nextOff] == L' ') {
                                ++nextOff;
                            }
                            nextSpaceElided = true;
                            break;
                        }
                    }
                }
                cachedRows.push_back(WrappedRow{wline.substr(off, take), col, isCont, spaceElided});
                off = nextOff;
            }
        }

        if (cachedRows.empty()) {
            g_hasSel = false;
            g_hasSelectionAtomic.store(false, std::memory_order_release);
            visStartRow = 0U;
            visEndRow   = 0U;
        } else {
            if (g_hasSel && (g_selAnchor.row >= cachedRows.size() || g_selActive.row >= cachedRows.size())) {
                g_hasSel = false;
                g_hasSelectionAtomic.store(false, std::memory_order_release);
            }
            const std::size_t totalRows = cachedRows.size();
            const std::size_t visCap    = static_cast<std::size_t>(maxRows);
            const std::size_t maxScroll = (totalRows > visCap) ? (totalRows - visCap) : 0U;
            const std::size_t scrollRows = std::min(snap.scrollOffset, maxScroll);
            if (snap.scrollOffset > maxScroll) {
                outNeedClampScroll = true;
                outClampMaxScroll  = maxScroll;
            }
            visEndRow   = totalRows - scrollRows;
            visStartRow = (visEndRow > visCap) ? (visEndRow - visCap) : 0U;

            SelPos sMin{}, sMax{};
            const bool drawSel = g_hasSel && !(g_selAnchor == g_selActive);
            if (drawSel) {
                if (g_selAnchor < g_selActive) {
                    sMin = g_selAnchor;
                    sMax = g_selActive;
                } else {
                    sMin = g_selActive;
                    sMax = g_selAnchor;
                }
            }

            HBRUSH selBrush = drawSel ? ::CreateSolidBrush(RGB(42, 98, 164)) : nullptr;
            int y = logTop;
            for (std::size_t r = visStartRow; r < visEndRow; ++r) {
                const WrappedRow& wr = cachedRows[r];
                const int drawX = pad + (wr.isCont ? contInd : 0);

                if (drawSel && selBrush && r >= sMin.row && r <= sMax.row && !wr.text.empty()) {
                    const std::size_t c0 = (r == sMin.row) ? std::min(sMin.col, wr.text.size()) : 0U;
                    const std::size_t c1 = (r == sMax.row) ? std::min(sMax.col, wr.text.size()) : wr.text.size();
                    if (c1 > c0 || (r < sMax.row && c1 >= c0)) {
                        SIZE sz0{}, sz1{};
                        if (c0 > 0U) {
                            ::GetTextExtentPoint32W(memDc, wr.text.data(), static_cast<int>(c0), &sz0);
                        }
                        if (c1 > 0U) {
                            ::GetTextExtentPoint32W(memDc, wr.text.data(), static_cast<int>(c1), &sz1);
                        }
                        const int x0 = drawX + sz0.cx;
                        int x1 = drawX + sz1.cx;
                        if (r < sMax.row && c1 == wr.text.size()) {
                            x1 = std::max(x1, x0 + std::max(6, fontPx / 2));
                        }
                        if (x1 > x0) {
                            const RECT hlRc{x0, y, std::min(pw - pad, x1), y + lineStep};
                            ::FillRect(memDc, &hlRc, selBrush);
                        }
                    }
                }

                ::SetTextColor(memDc, wr.color);
                ::ExtTextOutW(memDc, drawX, y, ETO_CLIPPED, &fullRc,
                              wr.text.c_str(), static_cast<UINT>(wr.text.size()), nullptr);
                y += lineStep;
            }
            if (selBrush) ::DeleteObject(selBrush);

            // Scrollbar track & proportional thumb
            if (totalRows > visCap) {
                const int sbLeft  = pw - std::max(5, static_cast<int>(std::lround(6.0f * sc)));
                const int sbRight = pw - 2;
                const int sbTop   = logTop;
                const int sbBot   = std::max(logTop + 12, logBot);
                const int sbH     = sbBot - sbTop;
                const RECT trackRc{sbLeft, sbTop, sbRight, sbBot};
                HBRUSH trBrush = ::CreateSolidBrush(RGB(26, 36, 48));
                ::FillRect(memDc, &trackRc, trBrush);
                ::DeleteObject(trBrush);

                const int thumbH = std::max(14, static_cast<int>((static_cast<double>(visCap) / totalRows) * sbH));
                const int maxThumbTravel = sbH - thumbH;
                const int thumbTop = sbTop + (maxScroll > 0U
                                                  ? static_cast<int>((static_cast<double>(maxScroll - scrollRows) / maxScroll) * maxThumbTravel)
                                                  : 0);
                const RECT thumbRc{sbLeft, thumbTop, sbRight, thumbTop + thumbH};
                HBRUSH thBrush = ::CreateSolidBrush(RGB(86, 152, 206));
                ::FillRect(memDc, &thumbRc, thBrush);
                ::DeleteObject(thBrush);
            }

            if (scrollRows > 0U) {
                char scrBuf[48]{};
                std::snprintf(scrBuf, sizeof(scrBuf), "[+%zu rows]", scrollRows);
                const std::wstring wScr = Utf8ToWide(scrBuf);
                RECT scrRc{pad, logTop, pw - pad - 6, logTop + lineStep};
                ::SetTextColor(memDc, RGB(255, 206, 94));
                ::DrawTextW(memDc, wScr.c_str(), static_cast<int>(wScr.size()), &scrRc,
                            DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            }
        }

        // Input bar (`> ` + horizontally-panned UTF-16 input + inline IME pre-edit + cursor)
        const std::wstring wPrompt = snap.searchPrompt.empty() ? L"> " : L"(search)> ";
        SIZE promptSz{};
        ::GetTextExtentPoint32W(memDc, wPrompt.c_str(), static_cast<int>(wPrompt.size()), &promptSz);
        const int inpY = ph - inputH + std::max(2, (inputH - fontPx) / 2 - 1);
        ::SetTextColor(memDc, RGB(94, 224, 255));
        ::TextOutW(memDc, pad, inpY, wPrompt.c_str(), static_cast<int>(wPrompt.size()));

        const int textX = pad + promptSz.cx;
        const int availInpW = std::max(60, pw - pad - textX - 4);
        const std::size_t curByte = std::min(snap.cursorByte, snap.inputLine.size());
        const std::wstring wBefore = Utf8ToWide(snap.inputLine.substr(0U, curByte));
        const std::wstring wAfter  = Utf8ToWide(snap.inputLine.substr(curByte));

        const std::size_t compCurByte = std::min(snap.imeCompCursorByte, snap.imeCompText.size());
        const std::wstring wCompBefore = Utf8ToWide(snap.imeCompText.substr(0U, compCurByte));
        const std::wstring wCompAfter  = Utf8ToWide(snap.imeCompText.substr(compCurByte));
        const std::wstring wComp       = wCompBefore + wCompAfter;
        const std::wstring wFull       = wBefore + wComp + wAfter;
        const std::size_t  caretIdx    = wBefore.size() + wCompBefore.size();

        std::size_t visStart = 0U;
        SIZE beforeSz{};
        if (caretIdx > 0U) {
            ::GetTextExtentPoint32W(memDc, wFull.c_str(), static_cast<int>(caretIdx), &beforeSz);
            while (visStart < caretIdx && beforeSz.cx > availInpW - 12) {
                ++visStart;
                ::GetTextExtentPoint32W(memDc, wFull.c_str() + visStart,
                                        static_cast<int>(caretIdx - visStart), &beforeSz);
            }
        }

        const RECT clipInp{textX, ph - inputH + 1, pw - pad, ph - 1};
        const std::size_t compOff   = wBefore.size();
        const std::size_t compEnd   = wBefore.size() + wComp.size();
        const std::size_t visBefore = std::max(visStart, compOff);
        int drawX = textX;

        if (visStart < compOff) {
            const std::size_t nBefore = compOff - visStart;
            SIZE sz{};
            ::GetTextExtentPoint32W(memDc, wFull.c_str() + visStart, static_cast<int>(nBefore), &sz);
            ::SetTextColor(memDc, RGB(244, 248, 255));
            ::ExtTextOutW(memDc, drawX, inpY, ETO_CLIPPED, &clipInp,
                          wFull.c_str() + visStart, static_cast<UINT>(nBefore), nullptr);
            drawX += sz.cx;
        }

        if (!wComp.empty() && visBefore < compEnd) {
            const std::size_t nComp = compEnd - visBefore;
            SIZE sz{};
            ::GetTextExtentPoint32W(memDc, wFull.c_str() + visBefore, static_cast<int>(nComp), &sz);
            const int compRight = std::min<int>(pw - pad, drawX + sz.cx);
            if (compRight > drawX) {
                HBRUSH compBg = ::CreateSolidBrush(RGB(30, 48, 68));
                const RECT compRc{drawX, ph - inputH + 3, compRight, ph - 4};
                ::FillRect(memDc, &compRc, compBg);
                ::DeleteObject(compBg);

                HBRUSH ulBrush = ::CreateSolidBrush(RGB(255, 206, 94));
                const RECT ulRc{drawX, ph - 4, compRight, ph - 2};
                ::FillRect(memDc, &ulRc, ulBrush);
                ::DeleteObject(ulBrush);
            }
            ::SetTextColor(memDc, RGB(255, 226, 134));
            ::ExtTextOutW(memDc, drawX, inpY, ETO_CLIPPED, &clipInp,
                          wFull.c_str() + visBefore, static_cast<UINT>(nComp), nullptr);
            drawX += sz.cx;
        }

        const std::size_t visAfterStart = std::max(visStart, compEnd);
        if (visAfterStart < wFull.size()) {
            const std::size_t nAfter = wFull.size() - visAfterStart;
            ::SetTextColor(memDc, RGB(244, 248, 255));
            ::ExtTextOutW(memDc, drawX, inpY, ETO_CLIPPED, &clipInp,
                          wFull.c_str() + visAfterStart, static_cast<UINT>(nAfter), nullptr);
        }

        const int cursorX = textX + beforeSz.cx;
        cacheCaretBbX = static_cast<int>(ox) + std::max(0, std::min(cursorX, pw));
        cacheCaretBbY = static_cast<int>(oy) + ph - inputH;
        cacheCaretH   = inputH;

        if (blink || !wComp.empty()) {
            const int cursorW = std::max(2, static_cast<int>(std::lround(2.0f * sc)));
            const RECT curRc{cursorX, ph - inputH + 5, std::min(pw - pad, cursorX + cursorW), ph - 5};
            HBRUSH curBrush = ::CreateSolidBrush(RGB(120, 232, 255));
            ::FillRect(memDc, &curRc, curBrush);
            ::DeleteObject(curBrush);
        }

        ::SelectObject(memDc, oldFont);

        // Alpha reconstruction pass: GDI clears high byte of touched pixels to 0.
        // Set translucent glass interior (~89% opaque) and boost touched text/lines to 100% opaque.
        auto* px = static_cast<std::uint32_t*>(bits);
        const std::size_t totalPx = static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph);
        for (std::size_t i = 0U; i < totalPx; ++i) {
            const std::uint32_t c = px[i];
            const int b = static_cast<int>(c & 0xFFU);
            const int g = static_cast<int>((c >> 8U) & 0xFFU);
            const int r = static_cast<int>((c >> 16U) & 0xFFU);
            // Reference interior floor ~(14, 18, 24); anything noticeably brighter is text/border
            const int diff = std::max({r - 24, g - 32, b - 44, 0});
            const int a    = std::min(255, 226 + diff * 3);
            px[i] = (static_cast<std::uint32_t>(a) << 24U) | (c & 0x00FFFFFFU);
        }
        return true;
    }
} g_gdi;

// ============================================================================
//  DirectX 11 Overlay & Aux Backend
// ============================================================================
struct D3D11State {
    ID3D11Device*             device       = nullptr;
    ID3D11VertexShader*       vs           = nullptr;
    ID3D11PixelShader*        ps           = nullptr;
    ID3D11BlendState*         blend        = nullptr;
    ID3D11RasterizerState*    rast         = nullptr;
    ID3D11DepthStencilState*  depth        = nullptr;
    ID3D11Buffer*             cb           = nullptr;
    ID3D11Texture2D*          tex          = nullptr;
    ID3D11ShaderResourceView* srv          = nullptr;
    UINT                      texW         = 0U;
    UINT                      texH         = 0U;
    ID3D11VertexShader*       vsMark       = nullptr;
    ID3D11PixelShader*        psMark       = nullptr;
    ID3D11BlendState*         blendMarkAdd = nullptr;
    ID3D11BlendState*         blendMarkSub = nullptr;
    ID3D11Buffer*             cbMark       = nullptr;

    void release() {
        if (cbMark)       { cbMark->Release();       cbMark = nullptr; }
        if (blendMarkSub) { blendMarkSub->Release(); blendMarkSub = nullptr; }
        if (blendMarkAdd) { blendMarkAdd->Release(); blendMarkAdd = nullptr; }
        if (psMark)       { psMark->Release();       psMark = nullptr; }
        if (vsMark)       { vsMark->Release();       vsMark = nullptr; }
        if (srv)          { srv->Release();          srv = nullptr; }
        if (tex)          { tex->Release();          tex = nullptr; }
        if (cb)           { cb->Release();           cb = nullptr; }
        if (depth)        { depth->Release();        depth = nullptr; }
        if (rast)         { rast->Release();         rast = nullptr; }
        if (blend)        { blend->Release();        blend = nullptr; }
        if (ps)           { ps->Release();           ps = nullptr; }
        if (vs)           { vs->Release();           vs = nullptr; }
        device = nullptr;
        texW   = 0U;
        texH   = 0U;
    }

    bool ensureCommon(ID3D11Device* dev) {
        if (device != dev) {
            release();
            device = dev;
        }
        if (!rast) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode        = D3D11_FILL_SOLID;
            rd.CullMode        = D3D11_CULL_NONE;
            rd.DepthClipEnable = FALSE;
            rd.ScissorEnable   = FALSE;
            if (FAILED(dev->CreateRasterizerState(&rd, &rast))) return false;
        }
        if (!depth) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable   = FALSE;
            dd.StencilEnable = FALSE;
            if (FAILED(dev->CreateDepthStencilState(&dd, &depth))) return false;
        }
        return true;
    }

    bool ensureMark(ID3D11Device* dev) {
        if (!ensureCommon(dev)) return false;
        if (!EnsureMarkShaderBlobs()) return false;
        if (!vsMark && FAILED(dev->CreateVertexShader(g_markVsBlob->GetBufferPointer(), g_markVsBlob->GetBufferSize(), nullptr, &vsMark))) {
            return false;
        }
        if (!psMark && FAILED(dev->CreatePixelShader(g_markPsBlob->GetBufferPointer(), g_markPsBlob->GetBufferSize(), nullptr, &psMark))) {
            return false;
        }
        if (!blendMarkAdd) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable           = TRUE;
            bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_DEST_COLOR;
            bd.RenderTarget[0].DestBlend             = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_BLUE;
            if (FAILED(dev->CreateBlendState(&bd, &blendMarkAdd))) return false;
        }
        if (!blendMarkSub) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable           = TRUE;
            bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_DEST_COLOR;
            bd.RenderTarget[0].DestBlend             = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_REV_SUBTRACT;
            bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_BLUE;
            if (FAILED(dev->CreateBlendState(&bd, &blendMarkSub))) return false;
        }
        if (!cbMark) {
            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth      = sizeof(MarkCbData);
            cbd.Usage          = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&cbd, nullptr, &cbMark))) return false;
        }
        return true;
    }

    bool ensureConsole(ID3D11Device* dev, UINT pw, UINT ph, bool& outNewTex) {
        outNewTex = false;
        if (!ensureCommon(dev)) return false;
        if (!EnsureShaderBlobs()) return false;
        if (!vs && FAILED(dev->CreateVertexShader(g_vsBlob->GetBufferPointer(), g_vsBlob->GetBufferSize(), nullptr, &vs))) {
            return false;
        }
        if (!ps && FAILED(dev->CreatePixelShader(g_psBlob->GetBufferPointer(), g_psBlob->GetBufferSize(), nullptr, &ps))) {
            return false;
        }
        if (!blend) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable           = TRUE;
            bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
            bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(dev->CreateBlendState(&bd, &blend))) return false;
        }
        if (!cb) {
            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth      = sizeof(ConsoleConstants);
            cbd.Usage          = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&cbd, nullptr, &cb))) return false;
        }
        if (!tex || !srv || texW != pw || texH != ph) {
            if (srv) { srv->Release(); srv = nullptr; }
            if (tex) { tex->Release(); tex = nullptr; }

            D3D11_TEXTURE2D_DESC td{};
            td.Width            = pw;
            td.Height           = ph;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels       = 1;
            sd.Texture2D.MostDetailedMip = 0;
            if (FAILED(dev->CreateShaderResourceView(tex, &sd, &srv))) {
                tex->Release();
                tex = nullptr;
                return false;
            }
            texW      = pw;
            texH      = ph;
            outNewTex = true;
        }
        return true;
    }
} g_d3d11;

// ============================================================================
//  DirectX 12 Overlay & Aux Backend
// ============================================================================
struct D3D12State {
    ID3D12Device*              device        = nullptr;
    ID3D12RootSignature*       rootSig       = nullptr;
    ID3D12PipelineState*       pso           = nullptr;
    DXGI_FORMAT                psoFormat     = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap*      srvHeap       = nullptr;
    ID3D12Resource*            tex           = nullptr;
    ID3D12Resource*            uploadBuf     = nullptr;
    void*                      uploadMapped  = nullptr;
    UINT                       texW          = 0U;
    UINT                       texH          = 0U;
    UINT                       uploadPitch   = 0U;
    ID3D12RootSignature*       markRootSig   = nullptr;
    ID3D12PipelineState*       psoMarkAdd    = nullptr;
    ID3D12PipelineState*       psoMarkSub    = nullptr;
    DXGI_FORMAT                markPsoFormat = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource*            markCbBuf     = nullptr;
    std::uint8_t*              markCbMapped  = nullptr;
    ID3D12CommandAllocator*    allocs[4]{};
    UINT64                     slotFence[4]{};
    ID3D12GraphicsCommandList* cmdList       = nullptr;
    ID3D12Fence*               fence         = nullptr;
    HANDLE                     fenceEvent    = nullptr;
    UINT64                     nextFenceVal  = 0ULL;
    std::uint32_t              slotIndex     = 0U;

    void waitForIdle() {
        if (!fence || !fenceEvent) return;
        if (nextFenceVal > 0ULL && fence->GetCompletedValue() < nextFenceVal) {
            fence->SetEventOnCompletion(nextFenceVal, fenceEvent);
            ::WaitForSingleObject(fenceEvent, 2000);
        }
    }

    void releaseTex() {
        waitForIdle();
        if (uploadBuf) {
            if (uploadMapped) { uploadBuf->Unmap(0, nullptr); uploadMapped = nullptr; }
            uploadBuf->Release();
            uploadBuf = nullptr;
        }
        if (tex) { tex->Release(); tex = nullptr; }
        texW        = 0U;
        texH        = 0U;
        uploadPitch = 0U;
    }

    void release() {
        waitForIdle();
        releaseTex();
        if (markCbBuf) {
            if (markCbMapped) { markCbBuf->Unmap(0, nullptr); markCbMapped = nullptr; }
            markCbBuf->Release();
            markCbBuf = nullptr;
        }
        if (psoMarkSub)  { psoMarkSub->Release();  psoMarkSub = nullptr; }
        if (psoMarkAdd)  { psoMarkAdd->Release();  psoMarkAdd = nullptr; }
        if (markRootSig) { markRootSig->Release(); markRootSig = nullptr; }
        markPsoFormat = DXGI_FORMAT_UNKNOWN;
        for (int i = 0; i < 4; ++i) {
            if (allocs[i]) { allocs[i]->Release(); allocs[i] = nullptr; }
            slotFence[i] = 0ULL;
        }
        if (cmdList)    { cmdList->Release();        cmdList = nullptr; }
        if (srvHeap)    { srvHeap->Release();        srvHeap = nullptr; }
        if (pso)        { pso->Release();            pso = nullptr; }
        if (rootSig)    { rootSig->Release();        rootSig = nullptr; }
        if (fence)      { fence->Release();          fence = nullptr; }
        if (fenceEvent) { ::CloseHandle(fenceEvent); fenceEvent = nullptr; }
        device       = nullptr;
        psoFormat    = DXGI_FORMAT_UNKNOWN;
        nextFenceVal = 0ULL;
        slotIndex    = 0U;
    }

    bool ensureCore(ID3D12Device* dev) {
        if (device != dev) {
            release();
            device = dev;
        }
        if (!fence) {
            if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
            fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent) return false;
        }
        for (int i = 0; i < 4; ++i) {
            if (!allocs[i] &&
                FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs[i])))) {
                return false;
            }
        }
        if (!cmdList) {
            if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0], nullptr,
                                              IID_PPV_ARGS(&cmdList)))) {
                return false;
            }
            cmdList->Close();
        }
        return true;
    }

    bool ensureMark(ID3D12Device* dev, DXGI_FORMAT rtvFmt) {
        if (!ensureCore(dev)) return false;
        if (!EnsureMarkShaderBlobs()) return false;

        if (!markRootSig) {
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            param.Descriptor.ShaderRegister = 0;
            param.Descriptor.RegisterSpace  = 0;
            param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 1;
            rsd.pParameters   = &param;
            rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            HMODULE d3d12Mod = ::GetModuleHandleW(L"d3d12.dll");
            auto fnSer = d3d12Mod
                             ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
                                   ::GetProcAddress(d3d12Mod, "D3D12SerializeRootSignature"))
                             : nullptr;
            if (!fnSer) return false;
            ID3DBlob* sig = nullptr;
            ID3DBlob* err = nullptr;
            if (FAILED(fnSer(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
                if (err) err->Release();
                return false;
            }
            if (err) err->Release();
            const HRESULT hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                        IID_PPV_ARGS(&markRootSig));
            sig->Release();
            if (FAILED(hr)) return false;
        }

        if (!psoMarkAdd || !psoMarkSub || markPsoFormat != rtvFmt) {
            waitForIdle();
            if (psoMarkAdd) { psoMarkAdd->Release(); psoMarkAdd = nullptr; }
            if (psoMarkSub) { psoMarkSub->Release(); psoMarkSub = nullptr; }

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature                                   = markRootSig;
            pd.VS                                               = {g_markVsBlob->GetBufferPointer(), g_markVsBlob->GetBufferSize()};
            pd.PS                                               = {g_markPsBlob->GetBufferPointer(), g_markPsBlob->GetBufferSize()};
            pd.SampleMask                                       = UINT_MAX;
            pd.BlendState.RenderTarget[0].BlendEnable           = TRUE;
            pd.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_DEST_COLOR;
            pd.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ZERO;
            pd.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_BLUE;
            pd.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable                  = FALSE;
            pd.DepthStencilState.DepthEnable                    = FALSE;
            pd.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets                                 = 1;
            pd.RTVFormats[0]                                    = rtvFmt;
            pd.SampleDesc.Count                                 = 1;
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoMarkAdd)))) return false;

            pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_REV_SUBTRACT;
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoMarkSub)))) {
                psoMarkAdd->Release();
                psoMarkAdd = nullptr;
                return false;
            }
            markPsoFormat = rtvFmt;
        }

        if (!markCbBuf || !markCbMapped) {
            if (markCbBuf) {
                if (markCbMapped) { markCbBuf->Unmap(0, nullptr); markCbMapped = nullptr; }
                markCbBuf->Release();
                markCbBuf = nullptr;
            }
            D3D12_HEAP_PROPERTIES upHp{};
            upHp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width            = 4ULL * 2ULL * sizeof(MarkCbData);
            bd.Height           = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels        = 1;
            bd.Format           = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&upHp, D3D12_HEAP_FLAG_NONE, &bd,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&markCbBuf)))) {
                return false;
            }
            const D3D12_RANGE readNone{0, 0};
            void* mapped = nullptr;
            if (FAILED(markCbBuf->Map(0, &readNone, &mapped)) || !mapped) {
                markCbBuf->Release();
                markCbBuf = nullptr;
                return false;
            }
            markCbMapped = static_cast<std::uint8_t*>(mapped);
        }
        return true;
    }

    bool ensureConsole(ID3D12Device* dev, DXGI_FORMAT rtvFmt, UINT pw, UINT ph, bool& outNewTex) {
        outNewTex = false;
        if (!ensureCore(dev)) return false;
        if (!EnsureShaderBlobs()) return false;

        if (!rootSig) {
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors                    = 1;
            range.BaseShaderRegister                = 0;
            range.RegisterSpace                     = 0;
            range.OffsetInDescriptorsFromTableStart = 0;

            D3D12_ROOT_PARAMETER params[2]{};
            params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.ShaderRegister = 0;
            params[0].Constants.RegisterSpace  = 0;
            params[0].Constants.Num32BitValues = sizeof(ConsoleConstants) / 4U;
            params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

            params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable.NumDescriptorRanges = 1;
            params[1].DescriptorTable.pDescriptorRanges   = &range;
            params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 2;
            rsd.pParameters   = params;
            rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            HMODULE d3d12Mod = ::GetModuleHandleW(L"d3d12.dll");
            auto fnSer = d3d12Mod
                             ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
                                   ::GetProcAddress(d3d12Mod, "D3D12SerializeRootSignature"))
                             : nullptr;
            if (!fnSer) return false;
            ID3DBlob* sig = nullptr;
            ID3DBlob* err = nullptr;
            if (FAILED(fnSer(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
                if (err) err->Release();
                return false;
            }
            if (err) err->Release();
            const HRESULT hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                        IID_PPV_ARGS(&rootSig));
            sig->Release();
            if (FAILED(hr)) return false;
        }

        if (!pso || psoFormat != rtvFmt) {
            waitForIdle();
            if (pso) { pso->Release(); pso = nullptr; }

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature                                   = rootSig;
            pd.VS                                               = {g_vsBlob->GetBufferPointer(), g_vsBlob->GetBufferSize()};
            pd.PS                                               = {g_psBlob->GetBufferPointer(), g_psBlob->GetBufferSize()};
            pd.SampleMask                                       = UINT_MAX;
            pd.BlendState.RenderTarget[0].BlendEnable           = TRUE;
            pd.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable                  = FALSE;
            pd.DepthStencilState.DepthEnable                    = FALSE;
            pd.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets                                 = 1;
            pd.RTVFormats[0]                                    = rtvFmt;
            pd.SampleDesc.Count                                 = 1;
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) return false;
            psoFormat = rtvFmt;
        }

        if (!srvHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC shd{};
            shd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            shd.NumDescriptors = 1;
            shd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srvHeap)))) return false;
        }

        if (!tex || !uploadBuf || texW != pw || texH != ph) {
            releaseTex();

            D3D12_HEAP_PROPERTIES defHp{};
            defHp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width            = pw;
            td.Height           = ph;
            td.DepthOrArraySize = 1;
            td.MipLevels        = 1;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(dev->CreateCommittedResource(&defHp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    nullptr, IID_PPV_ARGS(&tex)))) {
                return false;
            }

            uploadPitch = (pw * 4U + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1U) &
                          ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1U);
            D3D12_HEAP_PROPERTIES upHp{};
            upHp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width            = static_cast<UINT64>(uploadPitch) * static_cast<UINT64>(ph);
            bd.Height           = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels        = 1;
            bd.Format           = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&upHp, D3D12_HEAP_FLAG_NONE, &bd,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&uploadBuf)))) {
                releaseTex();
                return false;
            }
            const D3D12_RANGE readNone{0, 0};
            if (FAILED(uploadBuf->Map(0, &readNone, &uploadMapped)) || !uploadMapped) {
                releaseTex();
                return false;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels       = 1;
            sd.Texture2D.MostDetailedMip = 0;
            dev->CreateShaderResourceView(tex, &sd, srvHeap->GetCPUDescriptorHandleForHeapStart());

            texW      = pw;
            texH      = ph;
            outNewTex = true;
        }
        return true;
    }
} g_d3d12;

void ComputePanelLayout(UINT bbW, UINT bbH, float& outScale,
                        UINT& outOrigX, UINT& outOrigY, UINT& outW, UINT& outH) noexcept {
    float sc = static_cast<float>(bbH) / 1080.0f;
    if (sc < 0.85f) sc = 0.85f;
    if (sc > 2.50f) sc = 2.50f;
    outScale = sc;

    outOrigX = static_cast<UINT>(std::lround(28.0f * sc));
    outOrigY = static_cast<UINT>(std::lround(24.0f * sc));
    if (outOrigX * 2U >= bbW) outOrigX = bbW / 16U;
    if (outOrigY * 2U >= bbH) outOrigY = bbH / 16U;

    const UINT maxW = (bbW > outOrigX * 2U) ? (bbW - outOrigX * 2U) : bbW;
    const UINT maxH = (bbH > outOrigY * 2U) ? (bbH - outOrigY * 2U) : bbH;

    UINT pw = static_cast<UINT>(std::lround(980.0f * sc));
    UINT ph = static_cast<UINT>(std::lround(460.0f * sc));
    if (pw > maxW) pw = maxW;
    if (ph > maxH) ph = maxH;
    if (pw < 320U && maxW >= 320U) pw = 320U;
    if (ph < 180U && maxH >= 180U) ph = 180U;
    outW = pw;
    outH = ph;
}

void RenderD3D11(std::uintptr_t exeBase,
                 std::uintptr_t d3dRender,
                 bool drawConsole,
                 const peer_beacon::VideoMarkFrame& markFrame) {
    if (*reinterpret_cast<const std::uint8_t*>(d3dRender + mhw::kOffD3DOccludedByte) != 0) return;
    if (*reinterpret_cast<const std::uint32_t*>(d3dRender + mhw::kOffD3DSwapChainCount) == 0U) return;

    auto* dev  = *reinterpret_cast<ID3D11Device* const*>(d3dRender + mhw::kOffD3DDevice11);
    auto* ctx  = *reinterpret_cast<ID3D11DeviceContext* const*>(d3dRender + mhw::kOffD3DContext11);
    auto* swap = *reinterpret_cast<IDXGISwapChain* const*>(d3dRender + mhw::kOffD3DSwapChain11);
    if (!dev || !ctx || !swap) return;

    auto* cs = reinterpret_cast<LPCRITICAL_SECTION>(d3dRender + mhw::kOffD3DPresentLock);
    ::EnterCriticalSection(cs);

    ID3D11Texture2D* backBuf = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&backBuf))) && backBuf) {
        D3D11_TEXTURE2D_DESC bbDesc{};
        backBuf->GetDesc(&bbDesc);
        const DXGI_FORMAT rtvFmt = ResolveTypedRtvFormat(bbDesc.Format);

        std::uint32_t hdrMode = 0U;
        float targetNits = 200.0f;
        ResolveHdrModeSEH(exeBase, rtvFmt, hdrMode, targetNits);

        bool willDrawMark = markFrame.active && g_d3d11.ensureMark(dev);
        MarkCbData cbAdd{}, cbSub{};
        if (willDrawMark) {
            float epsB = 0.0f, redRatio = 0.0f, ignPulse = 0.0f;
            ComputeMarkParams(hdrMode, markFrame.debugVisible, epsB, redRatio, ignPulse);
            PackMarkBuffers(markFrame, epsB, redRatio, ignPulse, cbAdd, cbSub);
        }

        float sc = 1.0f;
        UINT ox = 0U, oy = 0U, pw = 0U, ph = 0U;
        bool willDrawConsole = false;
        if (drawConsole) {
            ComputePanelLayout(bbDesc.Width, bbDesc.Height, sc, ox, oy, pw, ph);
            bool newTex = false;
            if (pw > 0U && ph > 0U && g_d3d11.ensureConsole(dev, pw, ph, newTex)) {
                bool needClamp = false;
                std::size_t clampMax = 0U;
                {
                    std::lock_guard<std::mutex> uiLk(g_uiMx);
                    const bool dirty = g_gdi.updateIfDirty(bbDesc.Width, bbDesc.Height, ox, oy, pw, ph, sc,
                                                           needClamp, clampMax);
                    if ((dirty || newTex) && g_gdi.bits) {
                        ctx->UpdateSubresource(g_d3d11.tex, 0, nullptr, g_gdi.bits, pw * 4U, 0U);
                    }
                }
                if (needClamp) {
                    console::ClampScrollOffset(clampMax);
                }
                willDrawConsole = true;
            }
        }

        if (willDrawMark || willDrawConsole) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format        = rtvFmt;
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(dev->CreateRenderTargetView(backBuf, &rtvDesc, &rtv)) && rtv) {
                ID3D11RenderTargetView*   oldRtv = nullptr;
                ID3D11DepthStencilView*   oldDsv = nullptr;
                ID3D11BlendState*         oldBlend = nullptr;
                FLOAT                     oldBf[4]{};
                UINT                      oldMask = 0U;
                ID3D11DepthStencilState*  oldDepth = nullptr;
                UINT                      oldStencil = 0U;
                ID3D11RasterizerState*    oldRast = nullptr;
                D3D11_VIEWPORT            oldVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
                UINT                      numVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                ID3D11InputLayout*        oldLayout = nullptr;
                D3D11_PRIMITIVE_TOPOLOGY  oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
                ID3D11VertexShader*       oldVs = nullptr;
                ID3D11PixelShader*        oldPs = nullptr;
                ID3D11GeometryShader*     oldGs = nullptr;
                ID3D11Buffer*             oldVsCb = nullptr;
                ID3D11Buffer*             oldPsCb = nullptr;
                ID3D11ShaderResourceView* oldPsSrv = nullptr;

                ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
                ctx->OMGetBlendState(&oldBlend, oldBf, &oldMask);
                ctx->OMGetDepthStencilState(&oldDepth, &oldStencil);
                ctx->RSGetState(&oldRast);
                ctx->RSGetViewports(&numVps, oldVps);
                ctx->IAGetInputLayout(&oldLayout);
                ctx->IAGetPrimitiveTopology(&oldTopo);
                ctx->VSGetShader(&oldVs, nullptr, nullptr);
                ctx->PSGetShader(&oldPs, nullptr, nullptr);
                ctx->GSGetShader(&oldGs, nullptr, nullptr);
                ctx->VSGetConstantBuffers(0, 1, &oldVsCb);
                ctx->PSGetConstantBuffers(0, 1, &oldPsCb);
                ctx->PSGetShaderResources(0, 1, &oldPsSrv);

                const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(bbDesc.Width),
                                        static_cast<FLOAT>(bbDesc.Height), 0.0f, 1.0f};
                const FLOAT bf[4]{0, 0, 0, 0};
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                ctx->OMSetDepthStencilState(g_d3d11.depth, 0);
                ctx->RSSetState(g_d3d11.rast);
                ctx->RSSetViewports(1, &vp);
                ctx->IASetInputLayout(nullptr);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx->GSSetShader(nullptr, nullptr, 0);

                if (willDrawMark) {
                    ctx->VSSetShader(g_d3d11.vsMark, nullptr, 0);
                    ctx->PSSetShader(g_d3d11.psMark, nullptr, 0);
                    ctx->PSSetConstantBuffers(0, 1, &g_d3d11.cbMark);
                    {
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(ctx->Map(g_d3d11.cbMark, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                            std::memcpy(mapped.pData, &cbAdd, sizeof(cbAdd));
                            ctx->Unmap(g_d3d11.cbMark, 0);
                            ctx->OMSetBlendState(g_d3d11.blendMarkAdd, bf, 0xFFFFFFFFU);
                            ctx->Draw(3U, 0U);
                        }
                    }
                    {
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(ctx->Map(g_d3d11.cbMark, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                            std::memcpy(mapped.pData, &cbSub, sizeof(cbSub));
                            ctx->Unmap(g_d3d11.cbMark, 0);
                            ctx->OMSetBlendState(g_d3d11.blendMarkSub, bf, 0xFFFFFFFFU);
                            ctx->Draw(3U, 0U);
                        }
                    }
                }

                if (willDrawConsole) {
                    ConsoleConstants cbData{};
                    cbData.panelOriginX = static_cast<float>(ox);
                    cbData.panelOriginY = static_cast<float>(oy);
                    cbData.panelW       = static_cast<float>(pw);
                    cbData.panelH       = static_cast<float>(ph);
                    cbData.invBbW       = 1.0f / static_cast<float>(bbDesc.Width);
                    cbData.invBbH       = 1.0f / static_cast<float>(bbDesc.Height);
                    cbData.targetNits   = targetNits;
                    cbData.hdrMode      = hdrMode;

                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(ctx->Map(g_d3d11.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        std::memcpy(mapped.pData, &cbData, sizeof(cbData));
                        ctx->Unmap(g_d3d11.cb, 0);
                        ctx->OMSetBlendState(g_d3d11.blend, bf, 0xFFFFFFFFU);
                        ctx->VSSetShader(g_d3d11.vs, nullptr, 0);
                        ctx->PSSetShader(g_d3d11.ps, nullptr, 0);
                        ctx->VSSetConstantBuffers(0, 1, &g_d3d11.cb);
                        ctx->PSSetConstantBuffers(0, 1, &g_d3d11.cb);
                        ctx->PSSetShaderResources(0, 1, &g_d3d11.srv);
                        ctx->Draw(6U, 0U);
                    }
                }

                ctx->PSSetShaderResources(0, 1, &oldPsSrv);
                ctx->PSSetConstantBuffers(0, 1, &oldPsCb);
                ctx->VSSetConstantBuffers(0, 1, &oldVsCb);
                ctx->GSSetShader(oldGs, nullptr, 0);
                ctx->PSSetShader(oldPs, nullptr, 0);
                ctx->VSSetShader(oldVs, nullptr, 0);
                ctx->IASetPrimitiveTopology(oldTopo);
                ctx->IASetInputLayout(oldLayout);
                if (numVps > 0U) ctx->RSSetViewports(numVps, oldVps);
                ctx->RSSetState(oldRast);
                ctx->OMSetDepthStencilState(oldDepth, oldStencil);
                ctx->OMSetBlendState(oldBlend, oldBf, oldMask);
                ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);

                if (oldPsSrv)  oldPsSrv->Release();
                if (oldPsCb)   oldPsCb->Release();
                if (oldVsCb)   oldVsCb->Release();
                if (oldGs)     oldGs->Release();
                if (oldPs)     oldPs->Release();
                if (oldVs)     oldVs->Release();
                if (oldLayout) oldLayout->Release();
                if (oldRast)   oldRast->Release();
                if (oldDepth)  oldDepth->Release();
                if (oldBlend)  oldBlend->Release();
                if (oldDsv)    oldDsv->Release();
                if (oldRtv)    oldRtv->Release();
                rtv->Release();
            }
        }
        backBuf->Release();
    }

    ::LeaveCriticalSection(cs);
}

void RenderD3D12(std::uintptr_t exeBase,
                 std::uintptr_t d3dRender,
                 bool drawConsole,
                 const peer_beacon::VideoMarkFrame& markFrame) {
    if (*reinterpret_cast<const std::uint8_t*>(d3dRender + mhw::kOffD3DOccludedByte) != 0) return;
    if (*reinterpret_cast<const std::uint32_t*>(d3dRender + mhw::kOffD3DSwapChainCount) == 0U) return;

    auto* dev     = *reinterpret_cast<ID3D12Device* const*>(d3dRender + mhw::kOffD3DDevice12);
    auto* queue   = *reinterpret_cast<ID3D12CommandQueue* const*>(d3dRender + mhw::kOffD3DCmdQueue12);
    auto* swap    = *reinterpret_cast<IDXGISwapChain3* const*>(d3dRender + mhw::kOffD3DSwapChain12);
    auto* rtvHeap = *reinterpret_cast<ID3D12DescriptorHeap* const*>(d3dRender + mhw::kOffD3DRtvHeap12);
    if (!dev || !queue || !swap || !rtvHeap) return;

    auto* cs = reinterpret_cast<LPCRITICAL_SECTION>(d3dRender + mhw::kOffD3DPresentLock);
    ::EnterCriticalSection(cs);

    BOOL fsState = FALSE;
    swap->GetFullscreenState(&fsState, nullptr);
    const bool curFs  = (fsState != FALSE);
    const bool prevFs = (*reinterpret_cast<const std::uint8_t*>(d3dRender + mhw::kOffD3DFullscreenLatch) != 0);
    if (curFs != prevFs) {
        g_d3d12.waitForIdle();
        ::LeaveCriticalSection(cs);
        return;
    }

    const UINT bbIdx = swap->GetCurrentBackBufferIndex();
    if (bbIdx >= 3U) {
        ::LeaveCriticalSection(cs);
        return;
    }
    auto* backBuf = *reinterpret_cast<ID3D12Resource* const*>(d3dRender + mhw::kOffD3DBackBuffers12 + bbIdx * 8U);
    if (!backBuf) {
        ::LeaveCriticalSection(cs);
        return;
    }

    const D3D12_RESOURCE_DESC bbDesc = backBuf->GetDesc();
    const DXGI_FORMAT rtvFmt = ResolveTypedRtvFormat(bbDesc.Format);
    const UINT bbW = static_cast<UINT>(bbDesc.Width);
    const UINT bbH = bbDesc.Height;

    std::uint32_t hdrMode = 0U;
    float targetNits = 200.0f;
    ResolveHdrModeSEH(exeBase, rtvFmt, hdrMode, targetNits);

    bool willDrawMark = markFrame.active && g_d3d12.ensureMark(dev, rtvFmt);
    MarkCbData cbAdd{}, cbSub{};
    if (willDrawMark) {
        float epsB = 0.0f, redRatio = 0.0f, ignPulse = 0.0f;
        ComputeMarkParams(hdrMode, markFrame.debugVisible, epsB, redRatio, ignPulse);
        PackMarkBuffers(markFrame, epsB, redRatio, ignPulse, cbAdd, cbSub);
    }

    float sc = 1.0f;
    UINT ox = 0U, oy = 0U, pw = 0U, ph = 0U;
    bool willDrawConsole = false;
    bool needUpload = false;
    if (drawConsole) {
        ComputePanelLayout(bbW, bbH, sc, ox, oy, pw, ph);
        bool newTex = false;
        if (pw > 0U && ph > 0U && g_d3d12.ensureConsole(dev, rtvFmt, pw, ph, newTex)) {
            bool needClamp = false;
            std::size_t clampMax = 0U;
            {
                std::lock_guard<std::mutex> uiLk(g_uiMx);
                const bool dirty = g_gdi.updateIfDirty(bbW, bbH, ox, oy, pw, ph, sc,
                                                       needClamp, clampMax);
                needUpload = (dirty || newTex) && (g_gdi.bits != nullptr);
                if (needUpload) {
                    g_d3d12.waitForIdle();
                    const auto* srcRows = static_cast<const std::uint8_t*>(g_gdi.bits);
                    auto* dstRows       = static_cast<std::uint8_t*>(g_d3d12.uploadMapped);
                    const std::size_t rowBytes = static_cast<std::size_t>(pw) * 4U;
                    for (UINT y = 0U; y < ph; ++y) {
                        std::memcpy(dstRows + static_cast<std::size_t>(y) * g_d3d12.uploadPitch,
                                    srcRows + static_cast<std::size_t>(y) * rowBytes, rowBytes);
                    }
                }
            }
            if (needClamp) {
                console::ClampScrollOffset(clampMax);
            }
            willDrawConsole = true;
        }
    }

    if (willDrawMark || willDrawConsole) {
        const std::uint32_t curSlot = g_d3d12.slotIndex;
        g_d3d12.slotIndex = (curSlot + 1U) & 3U;
        if (g_d3d12.slotFence[curSlot] > 0ULL &&
            g_d3d12.fence->GetCompletedValue() < g_d3d12.slotFence[curSlot]) {
            g_d3d12.fence->SetEventOnCompletion(g_d3d12.slotFence[curSlot], g_d3d12.fenceEvent);
            ::WaitForSingleObject(g_d3d12.fenceEvent, 2000);
        }

        ID3D12CommandAllocator* alloc = g_d3d12.allocs[curSlot];
        if (SUCCEEDED(alloc->Reset()) && SUCCEEDED(g_d3d12.cmdList->Reset(alloc, nullptr))) {
            auto makeBarrier = [](ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) noexcept -> D3D12_RESOURCE_BARRIER {
                D3D12_RESOURCE_BARRIER b{};
                b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = res;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = before;
                b.Transition.StateAfter  = after;
                return b;
            };

            if (willDrawConsole && needUpload) {
                const D3D12_RESOURCE_BARRIER bTexIn = makeBarrier(
                    g_d3d12.tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                g_d3d12.cmdList->ResourceBarrier(1, &bTexIn);

                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource        = g_d3d12.tex;
                dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource                          = g_d3d12.uploadBuf;
                src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset             = 0;
                src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_B8G8R8A8_UNORM;
                src.PlacedFootprint.Footprint.Width    = pw;
                src.PlacedFootprint.Footprint.Height   = ph;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = g_d3d12.uploadPitch;

                g_d3d12.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }

            D3D12_RESOURCE_BARRIER bIn[2]{};
            UINT nIn = 0U;
            bIn[nIn++] = makeBarrier(backBuf, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (willDrawConsole && needUpload) {
                bIn[nIn++] = makeBarrier(g_d3d12.tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            g_d3d12.cmdList->ResourceBarrier(nIn, bIn);

            const std::uint32_t rtvDescSize = *reinterpret_cast<const std::uint32_t*>(
                d3dRender + mhw::kOffD3DRtvDescSize12);
            const SIZE_T rtvStart = *reinterpret_cast<const SIZE_T*>(d3dRender + mhw::kOffD3DRtvStart12);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{rtvStart + static_cast<SIZE_T>(bbIdx) * rtvDescSize};

            const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(bbW), static_cast<FLOAT>(bbH), 0.0f, 1.0f};
            const D3D12_RECT scRect{0, 0, static_cast<LONG>(bbW), static_cast<LONG>(bbH)};

            g_d3d12.cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
            g_d3d12.cmdList->RSSetViewports(1, &vp);
            g_d3d12.cmdList->RSSetScissorRects(1, &scRect);
            g_d3d12.cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            if (willDrawMark) {
                const std::size_t slotByteOff = static_cast<std::size_t>(curSlot) * (2U * sizeof(MarkCbData));
                const D3D12_GPU_VIRTUAL_ADDRESS baseGpuVa = g_d3d12.markCbBuf->GetGPUVirtualAddress() + slotByteOff;
                g_d3d12.cmdList->SetGraphicsRootSignature(g_d3d12.markRootSig);
                std::memcpy(g_d3d12.markCbMapped + slotByteOff, &cbAdd, sizeof(cbAdd));
                g_d3d12.cmdList->SetPipelineState(g_d3d12.psoMarkAdd);
                g_d3d12.cmdList->SetGraphicsRootConstantBufferView(0, baseGpuVa);
                g_d3d12.cmdList->DrawInstanced(3U, 1U, 0U, 0U);

                std::memcpy(g_d3d12.markCbMapped + slotByteOff + sizeof(MarkCbData), &cbSub, sizeof(cbSub));
                g_d3d12.cmdList->SetPipelineState(g_d3d12.psoMarkSub);
                g_d3d12.cmdList->SetGraphicsRootConstantBufferView(0, baseGpuVa + sizeof(MarkCbData));
                g_d3d12.cmdList->DrawInstanced(3U, 1U, 0U, 0U);
            }

            if (willDrawConsole) {
                ConsoleConstants cbData{};
                cbData.panelOriginX = static_cast<float>(ox);
                cbData.panelOriginY = static_cast<float>(oy);
                cbData.panelW       = static_cast<float>(pw);
                cbData.panelH       = static_cast<float>(ph);
                cbData.invBbW       = 1.0f / static_cast<float>(bbW);
                cbData.invBbH       = 1.0f / static_cast<float>(bbH);
                cbData.targetNits   = targetNits;
                cbData.hdrMode      = hdrMode;

                ID3D12DescriptorHeap* heaps[1] = {g_d3d12.srvHeap};
                g_d3d12.cmdList->SetPipelineState(g_d3d12.pso);
                g_d3d12.cmdList->SetDescriptorHeaps(1, heaps);
                g_d3d12.cmdList->SetGraphicsRootSignature(g_d3d12.rootSig);
                g_d3d12.cmdList->SetGraphicsRoot32BitConstants(0, sizeof(ConsoleConstants) / 4U, &cbData, 0U);
                g_d3d12.cmdList->SetGraphicsRootDescriptorTable(1, g_d3d12.srvHeap->GetGPUDescriptorHandleForHeapStart());
                g_d3d12.cmdList->DrawInstanced(6U, 1U, 0U, 0U);
            }

            const D3D12_RESOURCE_BARRIER bOut = makeBarrier(
                backBuf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            g_d3d12.cmdList->ResourceBarrier(1, &bOut);

            if (SUCCEEDED(g_d3d12.cmdList->Close())) {
                ID3D12CommandList* lists[1] = {g_d3d12.cmdList};
                queue->ExecuteCommandLists(1, lists);
                const UINT64 fv = ++g_d3d12.nextFenceVal;
                queue->Signal(g_d3d12.fence, fv);
                g_d3d12.slotFence[curSlot] = fv;
            }
        }
    }

    ::LeaveCriticalSection(cs);
}

bool IsDx12ActiveSEH(std::uintptr_t exeBase) noexcept {
    __try {
        return (*reinterpret_cast<const std::uint8_t*>(exeBase + mhw::kRvaIsD3D12Byte) != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ClientToLocalPanel(void* hwndVoid, int clientX, int clientY,
                        int& outLx, int& outLy, bool& outInLogArea) noexcept {
    if (g_gdi.bbWidth == 0U || g_gdi.bbHeight == 0U || g_gdi.width == 0U || g_gdi.height == 0U) {
        return false;
    }
    double sx = 1.0, sy = 1.0;
    if (hwndVoid) {
        RECT cr{};
        if (::GetClientRect(static_cast<HWND>(hwndVoid), &cr)) {
            const int cw = cr.right - cr.left;
            const int ch = cr.bottom - cr.top;
            if (cw > 0 && ch > 0) {
                sx = static_cast<double>(g_gdi.bbWidth) / static_cast<double>(cw);
                sy = static_cast<double>(g_gdi.bbHeight) / static_cast<double>(ch);
            }
        }
    }
    const int bbX = static_cast<int>(std::lround(static_cast<double>(clientX) * sx));
    const int bbY = static_cast<int>(std::lround(static_cast<double>(clientY) * sy));
    outLx = bbX - static_cast<int>(g_gdi.panelOrigX);
    outLy = bbY - static_cast<int>(g_gdi.panelOrigY);
    outInLogArea = (outLx >= 0 && outLx < static_cast<int>(g_gdi.width) &&
                    outLy >= g_gdi.cacheLogTop && outLy < g_gdi.cacheLogBot);
    return (outLx >= 0 && outLx < static_cast<int>(g_gdi.width) &&
            outLy >= 0 && outLy < static_cast<int>(g_gdi.height));
}

} // namespace

void OnPresent(std::uintptr_t exeBase, std::uintptr_t d3dRender) noexcept {
    if (!exeBase || !d3dRender) return;
    const bool drawConsole = console::IsOpen();
    peer_beacon::VideoMarkFrame markFrame{};
    const bool drawMark = peer_beacon::GetCurrentVideoMarkFrame(markFrame);
    if (!drawConsole && !drawMark) return;
    if (IsDx12ActiveSEH(exeBase)) {
        RenderD3D12(exeBase, d3dRender, drawConsole, markFrame);
    } else {
        RenderD3D11(exeBase, d3dRender, drawConsole, markFrame);
    }
}

void OnMouseDown(void* hwnd, int clientX, int clientY, bool isDoubleClick) noexcept {
    if (!console::IsOpen()) return;
    std::lock_guard<std::mutex> lk(g_uiMx);
    int lx = 0, ly = 0;
    bool inLog = false;
    (void)ClientToLocalPanel(hwnd, clientX, clientY, lx, ly, inLog);
    if (!inLog) {
        if (g_hasSel) {
            g_hasSel = false;
            g_hasSelectionAtomic.store(false, std::memory_order_release);
            g_selGen.fetch_add(1ULL, std::memory_order_release);
        }
        g_isDragging = false;
        g_draggingAtomic.store(false, std::memory_order_release);
        g_clickStreak   = 0;
        g_lastClickTick = 0ULL;
        return;
    }

    SelPos hit{};
    if (!g_gdi.hitTestLocked(lx, ly, hit)) return;

    const UINT64 now   = ::GetTickCount64();
    const UINT   maxDt = std::max(200U, ::GetDoubleClickTime());
    const int    slop  = std::max(8, static_cast<int>(std::lround(8.0f * g_gdi.scale)));
    const bool withinStreak = isDoubleClick ||
                              (g_clickStreak > 0 &&
                               (now - g_lastClickTick) <= static_cast<UINT64>(maxDt) &&
                               std::abs(lx - g_lastClickLx) <= slop &&
                               std::abs(ly - g_lastClickLy) <= slop);
    if (withinStreak) {
        g_clickStreak = (g_clickStreak >= 3) ? 1 : (g_clickStreak + 1);
        if (isDoubleClick && g_clickStreak < 2) g_clickStreak = 2;
    } else {
        g_clickStreak = 1;
    }
    g_lastClickTick = now;
    g_lastClickLx   = lx;
    g_lastClickLy   = ly;

    if (g_clickStreak == 2 && hit.row < g_gdi.cachedRows.size()) {
        const std::wstring& txt = g_gdi.cachedRows[hit.row].text;
        if (!txt.empty()) {
            std::size_t p = (hit.col < txt.size()) ? hit.col : (txt.size() - 1U);
            if (p > 0U && !IsWordChar(txt[p]) && IsWordChar(txt[p - 1U])) --p;
            if (IsWordChar(txt[p])) {
                std::size_t a = p, b = p + 1U;
                while (a > 0U && IsWordChar(txt[a - 1U])) --a;
                while (b < txt.size() && IsWordChar(txt[b])) ++b;
                g_selAnchor = SelPos{hit.row, a};
                g_selActive = SelPos{hit.row, b};
            } else {
                g_selAnchor = SelPos{hit.row, p};
                g_selActive = SelPos{hit.row, p + 1U};
            }
            g_hasSel     = !(g_selAnchor == g_selActive);
            g_isDragging = false;
            g_hasSelectionAtomic.store(g_hasSel, std::memory_order_release);
            g_draggingAtomic.store(false, std::memory_order_release);
            g_selGen.fetch_add(1ULL, std::memory_order_release);
            return;
        }
    }

    if (g_clickStreak == 3 && hit.row < g_gdi.cachedRows.size()) {
        std::size_t rStart = hit.row;
        while (rStart > 0U && g_gdi.cachedRows[rStart].isCont) {
            --rStart;
        }
        std::size_t rEnd = hit.row;
        while ((rEnd + 1U) < g_gdi.cachedRows.size() && g_gdi.cachedRows[rEnd + 1U].isCont) {
            ++rEnd;
        }
        g_selAnchor  = SelPos{rStart, 0U};
        g_selActive  = SelPos{rEnd, g_gdi.cachedRows[rEnd].text.size()};
        g_hasSel     = !(g_selAnchor == g_selActive);
        g_isDragging = false;
        g_hasSelectionAtomic.store(g_hasSel, std::memory_order_release);
        g_draggingAtomic.store(false, std::memory_order_release);
        g_selGen.fetch_add(1ULL, std::memory_order_release);
        return;
    }

    g_selAnchor  = hit;
    g_selActive  = hit;
    g_hasSel     = true;
    g_isDragging = true;
    g_hasSelectionAtomic.store(false, std::memory_order_release); // Not a non-empty span until dragged
    g_draggingAtomic.store(true, std::memory_order_release);
    g_selGen.fetch_add(1ULL, std::memory_order_release);
}

void OnMouseMove(void* hwnd, int clientX, int clientY, bool leftButtonDown) noexcept {
    if (!g_draggingAtomic.load(std::memory_order_acquire)) return;
    int autoScrollDelta = 0;
    {
        std::lock_guard<std::mutex> lk(g_uiMx);
        if (!g_isDragging) return;
        if (!leftButtonDown || !console::IsOpen()) {
            g_isDragging = false;
            g_draggingAtomic.store(false, std::memory_order_release);
            g_hasSelectionAtomic.store(g_hasSel && !(g_selAnchor == g_selActive), std::memory_order_release);
            return;
        }

        int lx = 0, ly = 0;
        bool inLog = false;
        (void)ClientToLocalPanel(hwnd, clientX, clientY, lx, ly, inLog);
        const UINT64 now = ::GetTickCount64();
        if (ly < g_gdi.cacheLogTop && (now - g_lastAutoScrollTick) >= 45ULL) {
            autoScrollDelta      = 1;
            g_lastAutoScrollTick = now;
        } else if (ly >= g_gdi.cacheLogBot && (now - g_lastAutoScrollTick) >= 45ULL) {
            autoScrollDelta      = -1;
            g_lastAutoScrollTick = now;
        }

        SelPos hit{};
        if (g_gdi.hitTestLocked(lx, ly, hit) && !(hit == g_selActive)) {
            g_selActive = hit;
            g_hasSelectionAtomic.store(g_hasSel && !(g_selAnchor == g_selActive), std::memory_order_release);
            g_selGen.fetch_add(1ULL, std::memory_order_release);
        }
    }
    if (autoScrollDelta != 0) {
        console::ScrollBy(autoScrollDelta);
    }
}

void OnMouseUp(void* hwnd, int clientX, int clientY) noexcept {
    if (!g_draggingAtomic.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_uiMx);
    if (!g_isDragging) return;
    int lx = 0, ly = 0;
    bool inLog = false;
    (void)ClientToLocalPanel(hwnd, clientX, clientY, lx, ly, inLog);
    SelPos hit{};
    if (g_gdi.hitTestLocked(lx, ly, hit)) {
        g_selActive = hit;
    }
    g_isDragging = false;
    if (g_selAnchor == g_selActive) {
        g_hasSel = false;
    }
    g_draggingAtomic.store(false, std::memory_order_release);
    g_hasSelectionAtomic.store(g_hasSel, std::memory_order_release);
    g_selGen.fetch_add(1ULL, std::memory_order_release);
}

bool HasSelection() noexcept {
    return g_hasSelectionAtomic.load(std::memory_order_acquire);
}

void ClearSelection() noexcept {
    std::lock_guard<std::mutex> lk(g_uiMx);
    const bool had = g_hasSel || g_isDragging;
    g_hasSel        = false;
    g_isDragging    = false;
    g_clickStreak   = 0;
    g_lastClickTick = 0ULL;
    g_draggingAtomic.store(false, std::memory_order_release);
    g_hasSelectionAtomic.store(false, std::memory_order_release);
    if (had) g_selGen.fetch_add(1ULL, std::memory_order_release);
}

bool CopySelectionToClipboard(bool clearAfterCopy) noexcept {
    std::wstring text;
    {
        std::lock_guard<std::mutex> lk(g_uiMx);
        if (!g_hasSel || g_selAnchor == g_selActive || g_gdi.cachedRows.empty()) {
            return false;
        }
        SelPos sMin = g_selAnchor;
        SelPos sMax = g_selActive;
        if (sMax < sMin) std::swap(sMin, sMax);
        if (sMin.row >= g_gdi.cachedRows.size()) return false;
        sMax.row = std::min(sMax.row, g_gdi.cachedRows.size() - 1U);

        for (std::size_t r = sMin.row; r <= sMax.row; ++r) {
            const WrappedRow& wr = g_gdi.cachedRows[r];
            const std::size_t c0 = (r == sMin.row) ? std::min(sMin.col, wr.text.size()) : 0U;
            const std::size_t c1 = (r == sMax.row) ? std::min(sMax.col, wr.text.size()) : wr.text.size();
            if (c1 > c0) {
                text.append(wr.text, c0, c1 - c0);
            }
            if (r < sMax.row) {
                const WrappedRow& nextWr = g_gdi.cachedRows[r + 1U];
                if (nextWr.isCont) {
                    if (nextWr.spaceElided) text.push_back(L' ');
                } else {
                    text.append(L"\r\n");
                }
            }
        }
    }
    if (text.empty()) return false;

    if (::OpenClipboard(nullptr)) {
        ::EmptyClipboard();
        const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
        HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hg) {
            void* dst = ::GlobalLock(hg);
            if (dst) {
                std::memcpy(dst, text.c_str(), bytes);
                ::GlobalUnlock(hg);
            }
            ::SetClipboardData(CF_UNICODETEXT, hg);
        }
        ::CloseClipboard();
    }

    {
        std::lock_guard<std::mutex> lk(g_uiMx);
        g_copiedUntilTick = ::GetTickCount64() + 1200ULL;
        if (clearAfterCopy) {
            g_hasSel     = false;
            g_isDragging = false;
            g_draggingAtomic.store(false, std::memory_order_release);
            g_hasSelectionAtomic.store(false, std::memory_order_release);
        }
        g_selGen.fetch_add(1ULL, std::memory_order_release);
    }
    return true;
}

bool GetInputCaretClientPos(void* hwnd, int& outClientX, int& outClientY, int& outLineHeight) noexcept {
    std::lock_guard<std::mutex> lk(g_uiMx);
    if (!hwnd || g_gdi.bbWidth == 0U || g_gdi.bbHeight == 0U || g_gdi.width == 0U) {
        return false;
    }
    RECT cr{};
    if (!::GetClientRect(static_cast<HWND>(hwnd), &cr)) return false;
    const int cw = cr.right - cr.left;
    const int ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0) return false;

    const double sx = static_cast<double>(cw) / static_cast<double>(g_gdi.bbWidth);
    const double sy = static_cast<double>(ch) / static_cast<double>(g_gdi.bbHeight);
    outClientX    = static_cast<int>(std::lround(static_cast<double>(g_gdi.cacheCaretBbX) * sx));
    outClientY    = static_cast<int>(std::lround(static_cast<double>(g_gdi.cacheCaretBbY) * sy));
    outLineHeight = std::max(12, static_cast<int>(std::lround(static_cast<double>(g_gdi.cacheCaretH) * sy)));
    return true;
}

void Shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lk(g_uiMx);
        g_hasSel          = false;
        g_isDragging      = false;
        g_copiedUntilTick = 0ULL;
        g_clickStreak     = 0;
        g_lastClickTick   = 0ULL;
        g_draggingAtomic.store(false, std::memory_order_release);
        g_hasSelectionAtomic.store(false, std::memory_order_release);
        g_gdi.releaseSurface();
    }
    g_d3d12.release();
    g_d3d11.release();
    if (g_markPsBlob) { g_markPsBlob->Release(); g_markPsBlob = nullptr; }
    if (g_markVsBlob) { g_markVsBlob->Release(); g_markVsBlob = nullptr; }
    if (g_psBlob)     { g_psBlob->Release();     g_psBlob = nullptr; }
    if (g_vsBlob)     { g_vsBlob->Release();     g_vsBlob = nullptr; }
    g_markShaderTried = false;
    g_shaderTried     = false;
}

} // namespace console_overlay
