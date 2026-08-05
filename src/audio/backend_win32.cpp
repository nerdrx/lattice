// WASAPI shared-mode output. Guarded whole-file so it can live next to the
// JACK/ALSA backend in the same tree.
//
// TODO(asio): shared mode caps out around 10 ms round trip, which is fine for
// playback but not for tracking. ASIO is the next step and is what every
// Windows DAW actually ships. The ASIO SDK is licensed by Steinberg and cannot
// be vendored into this repo, so it has to be an opt-in build: user drops the
// SDK in third_party/asiosdk, the makefile defines LAT_HAVE_ASIO, and an
// AsioBackend gets tried ahead of WASAPI in createBackend(). WASAPI stays as
// the always-available fallback. Exclusive-mode WASAPI is a cheaper middle
// step (no SDK, ~3 ms) but takes the device away from the rest of the system.
#if defined(_WIN32)

#include "backend.h"
#include "engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <atomic>
#include <thread>
#include <vector>
#include <cstring>

namespace lat {
namespace {

// Every GUID this file needs, spelled out locally. The alternatives are
// <ksmedia.h> + -lksuser for the subtypes and -luuid (or __uuidof, which only
// works because mingw-w64 happens to emit __CRT_UUID_DECL) for the interfaces;
// both differ between the Windows SDK and mingw-w64. These values are frozen
// by the ABI, so hardcoding them is the portable choice.
const GUID kSubtypePcm   = {0x00000001, 0x0000, 0x0010, {0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

const CLSID kClsidDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
const IID   kIidDeviceEnumerator   = {0xA95664D2, 0x9614, 0x4F35, {0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
const IID   kIidAudioClient        = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
const IID   kIidAudioRenderClient  = {0xF294ACFC, 0x3146, 0x4483, {0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};

enum class Sfmt { Unsupported, F32, S16 };

Sfmt classify(const WAVEFORMATEX* wf) {
    if (!wf) return Sfmt::Unsupported;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = (const WAVEFORMATEXTENSIBLE*)wf;
        if (IsEqualGUID(ext->SubFormat, kSubtypeFloat) && wf->wBitsPerSample == 32) return Sfmt::F32;
        if (IsEqualGUID(ext->SubFormat, kSubtypePcm)   && wf->wBitsPerSample == 16) return Sfmt::S16;
        return Sfmt::Unsupported;
    }
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) return Sfmt::F32;
    if (wf->wFormatTag == WAVE_FORMAT_PCM        && wf->wBitsPerSample == 16) return Sfmt::S16;
    return Sfmt::Unsupported;
}

inline i16 toS16(f32 v) {
    // Clamp before scaling: the engine's master bus can overshoot 0 dBFS and
    // wrapping a wrapped integer is the worst-sounding failure mode there is.
    const f32 c = clampv(v, -1.f, 1.f);
    return (i16)(c * 32767.f);
}

class WasapiBackend final : public AudioBackend {
public:
    ~WasapiBackend() override { stop(); }

    bool start(Engine& e) override;
    void stop() override;

    f64  sampleRate() const override { return sr_; }
    int  bufferSize() const override { return bs_; }
    const char* name() const override { return "WASAPI"; }

private:
    void run();
    void writeOut(BYTE* dst, UINT32 frames);
    bool fail(const char* what, HRESULT hr);

    IMMDeviceEnumerator* denum_  = nullptr;
    IMMDevice*           dev_    = nullptr;
    IAudioClient*        client_ = nullptr;
    IAudioRenderClient*  render_ = nullptr;
    WAVEFORMATEX*        mix_    = nullptr;
    HANDLE               evt_    = nullptr;

    Engine* engine_ = nullptr;
    std::thread thread_;
    std::atomic<bool> run_{false};
    std::vector<f32> l_, r_;

    Sfmt fmt_ = Sfmt::Unsupported;
    int  channels_ = 2;
    UINT32 bufFrames_ = 0;
    UINT32 frameBytes_ = 0;          // mix format nBlockAlign
    bool comInit_ = false;
    bool started_ = false;           // IAudioClient::Start() succeeded

    f64 sr_ = 48000.0;
    int bs_ = 0;
};

bool WasapiBackend::fail(const char* what, HRESULT hr) {
    LOGE("WASAPI: %s failed (hr=0x%08lX)", what, (unsigned long)hr);
    stop();
    return false;
}

bool WasapiBackend::start(Engine& e) {
    engine_ = &e;

    // MTA because the audio thread touches the same interfaces. RPC_E_CHANGED_MODE
    // means the host process already picked an apartment; the WASAPI objects are
    // free-threaded so we can live with whatever it chose, we just must not
    // balance a CoUninitialize against an init we did not perform.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (co == RPC_E_CHANGED_MODE) comInit_ = false;
    else if (FAILED(co)) return fail("CoInitializeEx", co);
    else comInit_ = true;

    HRESULT hr = CoCreateInstance(kClsidDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  kIidDeviceEnumerator, (void**)&denum_);
    if (FAILED(hr)) return fail("CoCreateInstance(MMDeviceEnumerator)", hr);

    hr = denum_->GetDefaultAudioEndpoint(eRender, eConsole, &dev_);
    if (FAILED(hr)) return fail("GetDefaultAudioEndpoint", hr);

    hr = dev_->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, (void**)&client_);
    if (FAILED(hr)) return fail("IMMDevice::Activate", hr);

    hr = client_->GetMixFormat(&mix_);
    if (FAILED(hr)) return fail("GetMixFormat", hr);

    // Shared mode only ever accepts the engine's own mix format, so we convert
    // to it rather than negotiating. In practice it is float32; 16-bit PCM
    // shows up on some Bluetooth and virtual endpoints.
    fmt_ = classify(mix_);
    if (fmt_ == Sfmt::Unsupported) {
        LOGE("WASAPI: unhandled mix format (tag %u, %u bits, %u ch)",
             (unsigned)mix_->wFormatTag, (unsigned)mix_->wBitsPerSample, (unsigned)mix_->nChannels);
        stop();
        return false;
    }
    channels_ = mix_->nChannels;
    sr_ = (f64)mix_->nSamplesPerSec;

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    hr = client_->GetDevicePeriod(&defPeriod, &minPeriod);
    if (FAILED(hr)) return fail("GetDevicePeriod", hr);

    // Periodicity must be 0 in shared mode; the engine picks its own period and
    // hnsBufferDuration is only a hint about how much slack we want.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             defPeriod, 0, mix_, nullptr);
    if (FAILED(hr)) return fail("IAudioClient::Initialize", hr);

    evt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt_) return fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));

    hr = client_->SetEventHandle(evt_);
    if (FAILED(hr)) return fail("SetEventHandle", hr);

    hr = client_->GetBufferSize(&bufFrames_);
    if (FAILED(hr)) return fail("GetBufferSize", hr);

    hr = client_->GetService(kIidAudioRenderClient, (void**)&render_);
    if (FAILED(hr)) return fail("GetService(IAudioRenderClient)", hr);

    frameBytes_ = mix_->nBlockAlign;

    // A late wakeup can leave the entire endpoint buffer free, so the worst
    // case is bufFrames_ — but plugins size their scratch buffers off
    // kMaxBlock, so anything larger has to be rendered in chunks.
    bs_ = (int)(bufFrames_ < (UINT32)kMaxBlock ? bufFrames_ : (UINT32)kMaxBlock);
    engine_->prepare(sr_, bs_);
    l_.assign((size_t)bs_, 0.f);
    r_.assign((size_t)bs_, 0.f);

    // Pre-roll silence so the first wakeup is not racing an empty buffer.
    BYTE* pre = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufFrames_, &pre)))
        render_->ReleaseBuffer(bufFrames_, AUDCLNT_BUFFERFLAGS_SILENT);

    run_.store(true);
    thread_ = std::thread(&WasapiBackend::run, this);

    hr = client_->Start();
    if (FAILED(hr)) return fail("IAudioClient::Start", hr);
    started_ = true;

    LOGI("WASAPI backend up: %.0f Hz, %d frames, %d ch, %s", sr_, bs_, channels_,
         fmt_ == Sfmt::F32 ? "float32" : "int16");
    return true;
}

void WasapiBackend::stop() {
    run_.store(false);
    // Kick the render thread out of its wait instead of making it time out.
    if (evt_) SetEvent(evt_);
    if (thread_.joinable()) thread_.join();

    if (client_ && started_) { client_->Stop(); client_->Reset(); started_ = false; }

    if (render_) { render_->Release(); render_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
    if (dev_)    { dev_->Release();    dev_    = nullptr; }
    if (denum_)  { denum_->Release();  denum_  = nullptr; }
    if (mix_)    { CoTaskMemFree(mix_); mix_   = nullptr; }
    if (evt_)    { CloseHandle(evt_);  evt_    = nullptr; }

    // Only valid because start() and stop() are both called from the GUI thread;
    // COM initialisation is per-thread, not per-process.
    if (comInit_) { CoUninitialize(); comInit_ = false; }
}

void WasapiBackend::writeOut(BYTE* dst, UINT32 frames) {
    const int ch = channels_;
    const f32* l = l_.data();
    const f32* r = r_.data();

    if (fmt_ == Sfmt::F32) {
        auto* o = (f32*)dst;
        if (ch == 1) {
            for (UINT32 i = 0; i < frames; ++i) o[i] = (l[i] + r[i]) * 0.5f;
        } else {
            for (UINT32 i = 0; i < frames; ++i) {
                f32* f = o + (size_t)i * ch;
                f[0] = l[i];
                f[1] = r[i];
                // Surround endpoints: we only produce stereo, so centre/LFE/
                // rears stay silent rather than getting a fold-down.
                for (int c = 2; c < ch; ++c) f[c] = 0.f;
            }
        }
    } else {
        auto* o = (i16*)dst;
        if (ch == 1) {
            for (UINT32 i = 0; i < frames; ++i) o[i] = toS16((l[i] + r[i]) * 0.5f);
        } else {
            for (UINT32 i = 0; i < frames; ++i) {
                i16* s = o + (size_t)i * ch;
                s[0] = toS16(l[i]);
                s[1] = toS16(r[i]);
                for (int c = 2; c < ch; ++c) s[c] = 0;
            }
        }
    }
}

void WasapiBackend::run() {
    // The render thread needs its own apartment. The interfaces created in
    // start() are free-threaded, so sharing them across apartments is fine.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // "Pro Audio" is what MMCSS uses to keep us off the normal scheduler; the
    // handle has to be reverted on the same thread that took it.
    DWORD taskIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    if (!mm) LOGW("WASAPI: MMCSS unavailable, running at normal priority");

    while (run_.load(std::memory_order_relaxed)) {
        // 2 s is far past any legitimate period; hitting it means the endpoint
        // is gone and there is nothing useful left to do on this thread.
        if (WaitForSingleObject(evt_, 2000) != WAIT_OBJECT_0) break;
        if (!run_.load(std::memory_order_relaxed)) break;

        UINT32 pad = 0;
        HRESULT hr = client_->GetCurrentPadding(&pad);
        if (FAILED(hr)) break;

        const UINT32 frames = (pad < bufFrames_) ? bufFrames_ - pad : 0;
        if (frames == 0) continue;

        BYTE* dst = nullptr;
        hr = render_->GetBuffer(frames, &dst);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { LOGE("WASAPI: device invalidated"); break; }
        if (FAILED(hr)) continue;

        for (UINT32 done = 0; done < frames; ) {
            const UINT32 n = (frames - done < (UINT32)bs_) ? frames - done : (UINT32)bs_;
            engine_->process(nullptr, nullptr, l_.data(), r_.data(), (int)n);
            writeOut(dst + (size_t)done * frameBytes_, n);
            done += n;
        }

        // Every successful GetBuffer must be matched, including on the way out.
        render_->ReleaseBuffer(frames, 0);
    }

    if (mm) AvRevertMmThreadCharacteristics(mm);
    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace

std::unique_ptr<AudioBackend> createWasapiBackend(Engine& e) {
    auto b = std::make_unique<WasapiBackend>();
    if (b->start(e)) return b;
    LOGE("no usable WASAPI render endpoint");
    return nullptr;
}

// backend.cpp (JACK/ALSA) is excluded from the Windows build, so this TU also
// has to satisfy the entry point declared in backend.h. `prefer` is honoured
// only for "wasapi"; ASIO will slot in ahead of it here.
std::unique_ptr<AudioBackend> createBackend(Engine& e, const char* prefer) {
    if (prefer && std::strcmp(prefer, "wasapi") != 0) {
        LOGW("audio backend '%s' is not available on Windows, using WASAPI", prefer);
    }
    return createWasapiBackend(e);
}

} // namespace lat

#endif // _WIN32
