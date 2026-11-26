#include "audio_mark.h"

#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace audio_mark {
namespace {

constexpr std::uint32_t kRefLagsP[EchoStreamProcessor::kKernelLen]  = {60U, 84U, 89U, 108U, 118U, 142U, 151U, 166U, 175U, 190U, 209U, 223U};
constexpr float         kSignsP[EchoStreamProcessor::kKernelLen]    = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f, 1.f, 1.f, -1.f};

constexpr std::uint32_t kRefLagsD1[EchoStreamProcessor::kKernelLen] = {70U, 74U, 98U, 103U, 122U, 137U, 156U, 161U, 180U, 199U, 214U, 228U};
constexpr float         kSignsD1[EchoStreamProcessor::kKernelLen]   = {-1.f, 1.f, -1.f, -1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f, 1.f, 1.f};

constexpr std::uint32_t kRefLagsD2[EchoStreamProcessor::kKernelLen] = {65U, 79U, 94U, 113U, 127U, 132U, 146U, 170U, 185U, 194U, 204U, 218U};
constexpr float         kSignsD2[EchoStreamProcessor::kKernelLen]   = {1.f, 1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, -1.f, 1.f, -1.f, -1.f};

constexpr double kPi = 3.14159265358979323846;

inline std::uint32_t ScaleLag48k(std::uint32_t lag48, std::uint32_t sr) noexcept {
    const long long scaled = std::llround(
        static_cast<double>(lag48) * static_cast<double>(sr) / 48000.0);
    if (scaled < 1LL) return 1U;
    if (scaled > static_cast<long long>(EchoStreamProcessor::kRingMask)) {
        return static_cast<std::uint32_t>(EchoStreamProcessor::kRingMask);
    }
    return static_cast<std::uint32_t>(scaled);
}

inline float SampleToFloat(float v) noexcept { return v; }
inline float SampleToFloat(std::int16_t v) noexcept { return static_cast<float>(v) * (1.0f / 32768.0f); }

inline void StoreFromFloat(float* dst, float v) noexcept {
    *dst = std::clamp(v, -0.999f, 0.999f);
}

inline void StoreFromFloat(std::int16_t* dst, float v) noexcept {
    const long iv = std::lround(std::clamp(v, -0.999f, 0.999f) * 32767.0f);
    *dst = static_cast<std::int16_t>(std::clamp(iv, -32768L, 32767L));
}

template <typename SampleT>
void ProcessInterleavedImpl(EchoStreamProcessor& st,
                            SampleT* samples,
                            std::uint32_t numFrames,
                            const peer_beacon::AudioMarkPacket& pkt) noexcept {
    if (!samples || numFrames == 0U ||
        st.channels == 0U || st.channels > EchoStreamProcessor::kMaxChannels ||
        st.sampleRate == 0U) {
        return;
    }

    const std::uint32_t nCh = st.channels;
    const bool hasActivePkt = pkt.active && pkt.slotCount > 0U;
    if (hasActivePkt) {
        st.lastValidPkt = pkt;
    }
    const float targetGain = hasActivePkt ? EchoStreamProcessor::kTargetAlpha : 0.0f;

    // Fast path when completely idle (clean audio stays bit-exact, delay line stays warm)
    if (st.currentGain <= 0.0f && targetGain <= 0.0f) {
        for (std::uint32_t f = 0U; f < numFrames; ++f) {
            const SampleT* row = samples + static_cast<std::size_t>(f) * st.channels;
            for (std::uint32_t ch = 0U; ch < nCh; ++ch) {
                st.ring[ch][st.ringPos] = SampleToFloat(row[ch]);
            }
            st.ringPos = (st.ringPos + 1U) & EchoStreamProcessor::kRingMask;
            ++st.sampleCounter;
        }
        return;
    }

    const double invSr = 1.0 / static_cast<double>(st.sampleRate);
    const std::uint32_t slotCount = st.lastValidPkt.slotCount;

    for (std::uint32_t f = 0U; f < numFrames; ++f) {
        if (st.currentGain < targetGain) {
            st.currentGain = std::min(targetGain, st.currentGain + st.rampStep);
        } else if (st.currentGain > targetGain) {
            st.currentGain = std::max(targetGain, st.currentGain - st.rampStep);
        }

        SampleT* row = samples + static_cast<std::size_t>(f) * st.channels;
        float dry[EchoStreamProcessor::kMaxChannels]{};
        for (std::uint32_t ch = 0U; ch < nCh; ++ch) {
            const float x = SampleToFloat(row[ch]);
            dry[ch] = x;
            st.ring[ch][st.ringPos] = x;
        }

        if (st.currentGain > 1e-6f && slotCount > 0U) {
            const double t = static_cast<double>(st.sampleCounter) * invSr;
            const std::uint64_t slotIdx =
                static_cast<std::uint64_t>(std::floor(t * EchoStreamProcessor::kSlotsPerSec)) % slotCount;
            const double carrier = std::sin(2.0 * kPi * EchoStreamProcessor::kCarrierHz * t);
            const float mp = static_cast<float>(static_cast<double>(st.currentGain) * carrier);
            const float m1 = static_cast<float>(static_cast<double>(st.currentGain) *
                                                static_cast<double>(st.lastValidPkt.b1[slotIdx]) * carrier);
            const float m2 = static_cast<float>(static_cast<double>(st.currentGain) *
                                                static_cast<double>(st.lastValidPkt.b2[slotIdx]) * carrier);

            for (std::uint32_t ch = 0U; ch < nCh; ++ch) {
                const float* line = st.ring[ch];
                float accP = 0.0f;
                float acc1 = 0.0f;
                float acc2 = 0.0f;
                for (std::size_t k = 0U; k < EchoStreamProcessor::kKernelLen; ++k) {
                    accP += kSignsP[k]  * line[(st.ringPos - st.lagsP[k])  & EchoStreamProcessor::kRingMask];
                    acc1 += kSignsD1[k] * line[(st.ringPos - st.lagsD1[k]) & EchoStreamProcessor::kRingMask];
                    acc2 += kSignsD2[k] * line[(st.ringPos - st.lagsD2[k]) & EchoStreamProcessor::kRingMask];
                }
                StoreFromFloat(&row[ch], dry[ch] + mp * accP + m1 * acc1 + m2 * acc2);
            }
        }

        st.ringPos = (st.ringPos + 1U) & EchoStreamProcessor::kRingMask;
        ++st.sampleCounter;
    }
}

// ============================================================================
//  WASAPI COM VTable Hook (IAudioClient + IAudioRenderClient)
// ============================================================================

struct StreamFmt {
    bool          valid     = false;
    bool          isFloat32 = false;
    bool          isInt16   = false;
    std::uint32_t sr        = 48000U;
    std::uint16_t ch        = 2U;
};

struct ClientFmtSlot {
    IAudioClient* client     = nullptr;
    StreamFmt     fmt{};
    std::uint64_t lastUseSeq = 0ULL;
};

struct RenderClientSlot {
    SRWLOCK             procLock      = SRWLOCK_INIT;
    IAudioRenderClient* render        = nullptr;
    BYTE*               pendingBuf    = nullptr;
    UINT32              pendingFrames = 0U;
    StreamFmt           fmt{};
    EchoStreamProcessor proc{};
    std::uint64_t       lastUseSeq    = 0ULL;
};

using FnAudioClientInit      = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID);
using FnAudioClientGetSvc    = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, REFIID, void**);
using FnRenderGetBuffer      = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, BYTE**);
using FnRenderReleaseBuffer  = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);

constexpr std::size_t kMaxAudioSlots = 16U;

SRWLOCK           g_slotsLock        = SRWLOCK_INIT;
ClientFmtSlot     g_clientSlots[kMaxAudioSlots]{};
RenderClientSlot  g_renderSlots[kMaxAudioSlots]{};
std::uint64_t     g_clientUseSeq     = 0ULL;
std::uint64_t     g_slotUseSeq       = 0ULL;
StreamFmt         g_defaultFmt{};

void**            g_clientVft        = nullptr;
void**            g_renderVft        = nullptr;
FnAudioClientInit     g_origClientInit    = nullptr;
FnAudioClientGetSvc   g_origClientGetSvc  = nullptr;
FnRenderGetBuffer     g_origRenderGetBuf  = nullptr;
FnRenderReleaseBuffer g_origRenderRelBuf  = nullptr;

std::atomic<bool>          g_hooked{false};
std::atomic<bool>          g_shuttingDown{false};
std::atomic<std::uint32_t> g_inFlight{0U};

struct InFlightGuard {
    InFlightGuard() noexcept { g_inFlight.fetch_add(1U, std::memory_order_seq_cst); }
    ~InFlightGuard() noexcept { g_inFlight.fetch_sub(1U, std::memory_order_seq_cst); }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
};

bool ParseWaveFormat(const WAVEFORMATEX* wfx, StreamFmt& out) noexcept {
    out = StreamFmt{};
    if (!wfx || wfx->nSamplesPerSec == 0U || wfx->nChannels == 0U ||
        wfx->nChannels > EchoStreamProcessor::kMaxChannels) {
        return false;
    }
    out.sr = wfx->nSamplesPerSec;
    out.ch = wfx->nChannels;

    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32U) {
        out.isFloat32 = true;
        out.valid     = true;
        return true;
    }
    if (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16U) {
        out.isInt16 = true;
        out.valid   = true;
        return true;
    }
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22U) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (std::memcmp(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, sizeof(GUID)) == 0 &&
            wfx->wBitsPerSample == 32U) {
            out.isFloat32 = true;
            out.valid     = true;
            return true;
        }
        if (std::memcmp(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM, sizeof(GUID)) == 0 &&
            wfx->wBitsPerSample == 16U) {
            out.isInt16 = true;
            out.valid   = true;
            return true;
        }
    }
    return false;
}

void RegisterClientFmt(IAudioClient* client, const StreamFmt& fmt) noexcept {
    if (!client || !fmt.valid) return;
    ::AcquireSRWLockExclusive(&g_slotsLock);
    int emptyIdx  = -1;
    int oldestIdx = 0;
    std::uint64_t oldestSeq = ~0ULL;
    for (std::size_t i = 0U; i < kMaxAudioSlots; ++i) {
        if (g_clientSlots[i].client == client) {
            g_clientSlots[i].fmt        = fmt;
            g_clientSlots[i].lastUseSeq = ++g_clientUseSeq;
            ::ReleaseSRWLockExclusive(&g_slotsLock);
            return;
        }
        if (g_clientSlots[i].client == nullptr) {
            if (emptyIdx < 0) emptyIdx = static_cast<int>(i);
        } else if (g_clientSlots[i].lastUseSeq < oldestSeq) {
            oldestSeq = g_clientSlots[i].lastUseSeq;
            oldestIdx = static_cast<int>(i);
        }
    }
    const std::size_t dst = static_cast<std::size_t>((emptyIdx >= 0) ? emptyIdx : oldestIdx);
    g_clientSlots[dst].client     = client;
    g_clientSlots[dst].fmt        = fmt;
    g_clientSlots[dst].lastUseSeq = ++g_clientUseSeq;
    ::ReleaseSRWLockExclusive(&g_slotsLock);
}

bool LookupClientFmt(IAudioClient* client, StreamFmt& outFmt) noexcept {
    ::AcquireSRWLockShared(&g_slotsLock);
    for (std::size_t i = 0U; i < kMaxAudioSlots; ++i) {
        if (g_clientSlots[i].client == client && g_clientSlots[i].fmt.valid) {
            outFmt = g_clientSlots[i].fmt;
            ::ReleaseSRWLockShared(&g_slotsLock);
            return true;
        }
    }
    outFmt = g_defaultFmt;
    ::ReleaseSRWLockShared(&g_slotsLock);
    return outFmt.valid;
}

RenderClientSlot* FindOrAllocRenderSlotLocked(IAudioRenderClient* render, const StreamFmt* explicitFmt) noexcept {
    int emptyIdx  = -1;
    int oldestIdx = 0;
    std::uint64_t oldestSeq = ~0ULL;

    for (std::size_t i = 0U; i < kMaxAudioSlots; ++i) {
        if (g_renderSlots[i].render == render) {
            g_renderSlots[i].lastUseSeq = ++g_slotUseSeq;
            if (explicitFmt && explicitFmt->valid &&
                (g_renderSlots[i].fmt.sr != explicitFmt->sr ||
                 g_renderSlots[i].fmt.ch != explicitFmt->ch ||
                 g_renderSlots[i].fmt.isFloat32 != explicitFmt->isFloat32 ||
                 g_renderSlots[i].fmt.isInt16 != explicitFmt->isInt16)) {
                ::AcquireSRWLockExclusive(&g_renderSlots[i].procLock);
                g_renderSlots[i].fmt = *explicitFmt;
                g_renderSlots[i].proc.Configure(explicitFmt->sr, explicitFmt->ch);
                ::ReleaseSRWLockExclusive(&g_renderSlots[i].procLock);
            }
            return &g_renderSlots[i];
        }
        if (g_renderSlots[i].render == nullptr && emptyIdx < 0) {
            emptyIdx = static_cast<int>(i);
        } else if (g_renderSlots[i].lastUseSeq < oldestSeq) {
            oldestSeq = g_renderSlots[i].lastUseSeq;
            oldestIdx = static_cast<int>(i);
        }
    }

    RenderClientSlot* slot = &g_renderSlots[(emptyIdx >= 0) ? emptyIdx : oldestIdx];
    ::AcquireSRWLockExclusive(&slot->procLock);
    slot->render        = render;
    slot->pendingBuf    = nullptr;
    slot->pendingFrames = 0U;
    slot->fmt           = (explicitFmt && explicitFmt->valid) ? *explicitFmt : g_defaultFmt;
    slot->lastUseSeq    = ++g_slotUseSeq;
    if (slot->fmt.valid) {
        slot->proc.Configure(slot->fmt.sr, slot->fmt.ch);
    } else {
        slot->proc.Reset();
    }
    ::ReleaseSRWLockExclusive(&slot->procLock);
    return slot;
}

bool PatchVftSlot(void** vft, std::size_t idx, void* newFn, void** outOrig) noexcept {
    if (!vft || !newFn) return false;
    DWORD oldProt = 0U;
    if (!::VirtualProtect(&vft[idx], sizeof(void*), PAGE_READWRITE, &oldProt)) {
        return false;
    }
    if (outOrig) {
        *outOrig = vft[idx];
    }
    ::InterlockedExchangePointer(&vft[idx], newFn);
    DWORD unused = 0U;
    ::VirtualProtect(&vft[idx], sizeof(void*), oldProt, &unused);
    return true;
}

void RestoreVftSlot(void** vft, std::size_t idx, void* expectedOurFn, void* origFn) noexcept {
    if (!vft || !origFn) return;
    DWORD oldProt = 0U;
    if (!::VirtualProtect(&vft[idx], sizeof(void*), PAGE_READWRITE, &oldProt)) {
        return;
    }
    ::InterlockedCompareExchangePointer(&vft[idx], origFn, expectedOurFn);
    DWORD unused = 0U;
    ::VirtualProtect(&vft[idx], sizeof(void*), oldProt, &unused);
}

void ProcessPendingBufferSEH(RenderClientSlot& slot,
                             BYTE* data,
                             UINT32 numFrames,
                             const peer_beacon::AudioMarkPacket& pkt) noexcept {
    if (!data || numFrames == 0U || !slot.fmt.valid) return;
    __try {
        if (slot.fmt.isFloat32) {
            slot.proc.ProcessFloatInterleaved(reinterpret_cast<float*>(data), numFrames, pkt);
        } else if (slot.fmt.isInt16) {
            slot.proc.ProcessInt16Interleaved(reinterpret_cast<std::int16_t*>(data), numFrames, pkt);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

HRESULT STDMETHODCALLTYPE Hook_IAudioClient_Initialize(IAudioClient* This,
                                                       AUDCLNT_SHAREMODE ShareMode,
                                                       DWORD StreamFlags,
                                                       REFERENCE_TIME hnsBufferDuration,
                                                       REFERENCE_TIME hnsPeriodicity,
                                                       const WAVEFORMATEX* pFormat,
                                                       LPCGUID AudioSessionGuid) {
    InFlightGuard guard;
    const HRESULT hr = g_origClientInit
                           ? g_origClientInit(This, ShareMode, StreamFlags, hnsBufferDuration,
                                              hnsPeriodicity, pFormat, AudioSessionGuid)
                           : E_FAIL;
    if (!g_shuttingDown.load(std::memory_order_seq_cst) && SUCCEEDED(hr) && pFormat) {
        StreamFmt fmt{};
        if (ParseWaveFormat(pFormat, fmt)) {
            RegisterClientFmt(This, fmt);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_IAudioClient_GetService(IAudioClient* This,
                                                       REFIID riid,
                                                       void** ppv) {
    InFlightGuard guard;
    const HRESULT hr = g_origClientGetSvc ? g_origClientGetSvc(This, riid, ppv) : E_FAIL;
    if (!g_shuttingDown.load(std::memory_order_seq_cst) &&
        SUCCEEDED(hr) && ppv && *ppv && ::IsEqualGUID(riid, __uuidof(IAudioRenderClient))) {
        StreamFmt fmt{};
        if (LookupClientFmt(This, fmt)) {
            auto* rc = reinterpret_cast<IAudioRenderClient*>(*ppv);
            ::AcquireSRWLockExclusive(&g_slotsLock);
            FindOrAllocRenderSlotLocked(rc, &fmt);
            ::ReleaseSRWLockExclusive(&g_slotsLock);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_IAudioRenderClient_GetBuffer(IAudioRenderClient* This,
                                                            UINT32 NumFramesRequested,
                                                            BYTE** ppData) {
    InFlightGuard guard;
    const HRESULT hr = g_origRenderGetBuf ? g_origRenderGetBuf(This, NumFramesRequested, ppData) : E_FAIL;
    if (!g_shuttingDown.load(std::memory_order_seq_cst) && SUCCEEDED(hr) && ppData && *ppData) {
        ::AcquireSRWLockExclusive(&g_slotsLock);
        RenderClientSlot* slot = FindOrAllocRenderSlotLocked(This, nullptr);
        if (slot) {
            ::AcquireSRWLockExclusive(&slot->procLock);
            slot->pendingBuf    = *ppData;
            slot->pendingFrames = NumFramesRequested;
            ::ReleaseSRWLockExclusive(&slot->procLock);
        }
        ::ReleaseSRWLockExclusive(&g_slotsLock);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_IAudioRenderClient_ReleaseBuffer(IAudioRenderClient* This,
                                                                UINT32 NumFramesWritten,
                                                                DWORD dwFlags) {
    InFlightGuard guard;
    if (!g_shuttingDown.load(std::memory_order_seq_cst)) {
        peer_beacon::AudioMarkPacket pkt{};
        bool snapBusy = false;
        (void)peer_beacon::GetCurrentAudioMarkPacket(pkt, &snapBusy);

        ::AcquireSRWLockExclusive(&g_slotsLock);
        RenderClientSlot* slot = FindOrAllocRenderSlotLocked(This, nullptr);
        if (slot) {
            ::AcquireSRWLockExclusive(&slot->procLock);
            ::ReleaseSRWLockExclusive(&g_slotsLock);

            BYTE* const buf = slot->pendingBuf;
            const UINT32 pFrames = slot->pendingFrames;
            slot->pendingBuf    = nullptr;
            slot->pendingFrames = 0U;

            if (NumFramesWritten > 0U) {
                const UINT32 framesToProc = (pFrames > 0U)
                                                ? std::min(NumFramesWritten, pFrames)
                                                : NumFramesWritten;
                const peer_beacon::AudioMarkPacket& effPkt =
                    snapBusy ? slot->proc.lastValidPkt : pkt;
                if ((dwFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U) {
                    slot->proc.AdvanceSilent(framesToProc, effPkt);
                } else if (buf != nullptr) {
                    ProcessPendingBufferSEH(*slot, buf, framesToProc, effPkt);
                }
            }
            ::ReleaseSRWLockExclusive(&slot->procLock);
        } else {
            ::ReleaseSRWLockExclusive(&g_slotsLock);
        }
    }

    return g_origRenderRelBuf ? g_origRenderRelBuf(This, NumFramesWritten, dwFlags) : E_FAIL;
}

} // namespace

void EchoStreamProcessor::Configure(std::uint32_t sr, std::uint32_t ch) noexcept {
    sampleRate    = (sr > 0U) ? sr : 48000U;
    channels      = (ch > 0U && ch <= kMaxChannels) ? ch : 0U;
    sampleCounter = 0ULL;
    ringPos       = 0U;
    currentGain   = 0.0f;
    rampStep      = kTargetAlpha / std::max(1.0f, 0.010f * static_cast<float>(sampleRate));
    lastValidPkt  = peer_beacon::AudioMarkPacket{};
    for (std::size_t k = 0U; k < kKernelLen; ++k) {
        lagsP[k]  = ScaleLag48k(kRefLagsP[k], sampleRate);
        lagsD1[k] = ScaleLag48k(kRefLagsD1[k], sampleRate);
        lagsD2[k] = ScaleLag48k(kRefLagsD2[k], sampleRate);
    }
    std::memset(ring, 0, sizeof(ring));
}

void EchoStreamProcessor::Reset() noexcept {
    sampleCounter = 0ULL;
    ringPos       = 0U;
    currentGain   = 0.0f;
    lastValidPkt  = peer_beacon::AudioMarkPacket{};
    std::memset(ring, 0, sizeof(ring));
}

void EchoStreamProcessor::AdvanceSilent(std::uint32_t numFrames,
                                        const peer_beacon::AudioMarkPacket& pkt) noexcept {
    if (numFrames == 0U || channels == 0U || channels > kMaxChannels || sampleRate == 0U) {
        return;
    }
    const bool hasActivePkt = pkt.active && pkt.slotCount > 0U;
    if (hasActivePkt) {
        lastValidPkt = pkt;
    }
    const float targetGain = hasActivePkt ? kTargetAlpha : 0.0f;
    const float deltaGain  = rampStep * static_cast<float>(numFrames);
    if (currentGain < targetGain) {
        currentGain = std::min(targetGain, currentGain + deltaGain);
    } else if (currentGain > targetGain) {
        currentGain = std::max(targetGain, currentGain - deltaGain);
    }

    if (numFrames >= kRingCap) {
        std::memset(ring, 0, sizeof(ring));
        ringPos = (ringPos + numFrames) & kRingMask;
    } else {
        for (std::uint32_t f = 0U; f < numFrames; ++f) {
            for (std::uint32_t ch = 0U; ch < channels; ++ch) {
                ring[ch][ringPos] = 0.0f;
            }
            ringPos = (ringPos + 1U) & kRingMask;
        }
    }
    sampleCounter += numFrames;
}

void EchoStreamProcessor::ProcessFloatInterleaved(float* samples,
                                                  std::uint32_t numFrames,
                                                  const peer_beacon::AudioMarkPacket& pkt) noexcept {
    ProcessInterleavedImpl(*this, samples, numFrames, pkt);
}

void EchoStreamProcessor::ProcessInt16Interleaved(std::int16_t* samples,
                                                  std::uint32_t numFrames,
                                                  const peer_beacon::AudioMarkPacket& pkt) noexcept {
    ProcessInterleavedImpl(*this, samples, numFrames, pkt);
}

void Init() noexcept {
    if (g_hooked.load(std::memory_order_acquire)) return;
    g_shuttingDown.store(false, std::memory_order_seq_cst);

    const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didComInit = (hrCom == S_OK || hrCom == S_FALSE);

    IMMDeviceEnumerator* pEnum   = nullptr;
    IMMDevice*           pDev    = nullptr;
    IAudioClient*        pClient = nullptr;
    IAudioRenderClient*  pRender = nullptr;
    WAVEFORMATEX*        pMixFmt = nullptr;

    auto releaseDummy = [&]() noexcept {
        if (pRender) { pRender->Release(); pRender = nullptr; }
        if (pClient) { pClient->Release(); pClient = nullptr; }
        if (pDev)    { pDev->Release();    pDev = nullptr; }
        if (pEnum)   { pEnum->Release();   pEnum = nullptr; }
        if (pMixFmt) { ::CoTaskMemFree(pMixFmt); pMixFmt = nullptr; }
        if (didComInit) ::CoUninitialize();
    };

    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
    if (FAILED(hr) || !pEnum) {
        releaseDummy();
        return;
    }

    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
    if (FAILED(hr) || !pDev) {
        releaseDummy();
        return;
    }

    hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pClient));
    if (FAILED(hr) || !pClient) {
        releaseDummy();
        return;
    }

    hr = pClient->GetMixFormat(&pMixFmt);
    if (FAILED(hr) || !pMixFmt) {
        releaseDummy();
        return;
    }

    StreamFmt defFmt{};
    if (ParseWaveFormat(pMixFmt, defFmt)) {
        g_defaultFmt = defFmt;
    }

    hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 500000LL, 0, pMixFmt, nullptr);
    if (FAILED(hr)) {
        releaseDummy();
        return;
    }

    hr = pClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&pRender));
    if (FAILED(hr) || !pRender) {
        releaseDummy();
        return;
    }

    // Snapshot COM vtable pointers while keeping dummy objects alive across PatchVftSlot
    void** clientVft = *reinterpret_cast<void***>(pClient);
    void** renderVft = *reinterpret_cast<void***>(pRender);

    if (!clientVft || !renderVft) {
        releaseDummy();
        return;
    }

    void* origInit   = clientVft[3];
    void* origGetSvc = clientVft[14];
    void* origGetBuf = renderVft[3];
    void* origRelBuf = renderVft[4];

    if (origGetBuf == reinterpret_cast<void*>(&Hook_IAudioRenderClient_GetBuffer)) {
        releaseDummy();
        return;
    }

    g_clientVft       = clientVft;
    g_renderVft       = renderVft;
    g_origClientInit  = reinterpret_cast<FnAudioClientInit>(origInit);
    g_origClientGetSvc = reinterpret_cast<FnAudioClientGetSvc>(origGetSvc);
    g_origRenderGetBuf = reinterpret_cast<FnRenderGetBuffer>(origGetBuf);
    g_origRenderRelBuf = reinterpret_cast<FnRenderReleaseBuffer>(origRelBuf);

    const bool okPatch =
        PatchVftSlot(clientVft, 3U,  reinterpret_cast<void*>(&Hook_IAudioClient_Initialize), nullptr) &&
        PatchVftSlot(clientVft, 14U, reinterpret_cast<void*>(&Hook_IAudioClient_GetService), nullptr) &&
        PatchVftSlot(renderVft, 3U,  reinterpret_cast<void*>(&Hook_IAudioRenderClient_GetBuffer), nullptr) &&
        PatchVftSlot(renderVft, 4U,  reinterpret_cast<void*>(&Hook_IAudioRenderClient_ReleaseBuffer), nullptr);

    if (!okPatch) {
        RestoreVftSlot(clientVft, 3U,  reinterpret_cast<void*>(&Hook_IAudioClient_Initialize), origInit);
        RestoreVftSlot(clientVft, 14U, reinterpret_cast<void*>(&Hook_IAudioClient_GetService), origGetSvc);
        RestoreVftSlot(renderVft, 3U,  reinterpret_cast<void*>(&Hook_IAudioRenderClient_GetBuffer), origGetBuf);
        RestoreVftSlot(renderVft, 4U,  reinterpret_cast<void*>(&Hook_IAudioRenderClient_ReleaseBuffer), origRelBuf);
        releaseDummy();
        return;
    }

    releaseDummy();
    g_hooked.store(true, std::memory_order_release);
}

void Shutdown() noexcept {
    g_shuttingDown.store(true, std::memory_order_seq_cst);
    if (!g_hooked.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (g_clientVft) {
        RestoreVftSlot(g_clientVft, 3U,  reinterpret_cast<void*>(&Hook_IAudioClient_Initialize),
                       reinterpret_cast<void*>(g_origClientInit));
        RestoreVftSlot(g_clientVft, 14U, reinterpret_cast<void*>(&Hook_IAudioClient_GetService),
                       reinterpret_cast<void*>(g_origClientGetSvc));
    }
    if (g_renderVft) {
        RestoreVftSlot(g_renderVft, 3U, reinterpret_cast<void*>(&Hook_IAudioRenderClient_GetBuffer),
                       reinterpret_cast<void*>(g_origRenderGetBuf));
        RestoreVftSlot(g_renderVft, 4U, reinterpret_cast<void*>(&Hook_IAudioRenderClient_ReleaseBuffer),
                       reinterpret_cast<void*>(g_origRenderRelBuf));
    }

    for (int i = 0; i < 50 && g_inFlight.load(std::memory_order_seq_cst) != 0U; ++i) {
        ::Sleep(1);
    }

    if (g_inFlight.load(std::memory_order_seq_cst) == 0U) {
        ::AcquireSRWLockExclusive(&g_slotsLock);
        std::memset(g_clientSlots, 0, sizeof(g_clientSlots));
        std::memset(g_renderSlots, 0, sizeof(g_renderSlots));
        for (std::size_t i = 0U; i < kMaxAudioSlots; ++i) {
            ::InitializeSRWLock(&g_renderSlots[i].procLock);
        }
        g_clientUseSeq = 0ULL;
        g_slotUseSeq   = 0ULL;
        ::ReleaseSRWLockExclusive(&g_slotsLock);
    }
}

bool IsHooked() noexcept {
    return g_hooked.load(std::memory_order_acquire);
}

} // namespace audio_mark
