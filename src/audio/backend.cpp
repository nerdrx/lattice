#include "backend.h"
#include "engine.h"

#include <jack/jack.h>
#include <alsa/asoundlib.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstring>
#include <pthread.h>

namespace lat {

// ---------------------------------------------------------------------------
// JACK  (also the path used by PipeWire via its libjack shim)
// ---------------------------------------------------------------------------
class JackBackend final : public AudioBackend {
public:
    ~JackBackend() override { stop(); }

    bool start(Engine& e) override {
        engine_ = &e;
        jack_status_t st;
        client_ = jack_client_open("NxTakt", JackNoStartServer, &st);
        if (!client_) return false;

        sr_ = (f64)jack_get_sample_rate(client_);
        bs_ = (int)jack_get_buffer_size(client_);
        engine_->prepare(sr_, bs_);

        outL_ = jack_port_register(client_, "out_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        outR_ = jack_port_register(client_, "out_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!outL_ || !outR_) { jack_client_close(client_); client_ = nullptr; return false; }

        // Capture is optional: losing it costs recording and monitoring, not
        // playback, so a failure here is a warning and the engine gets nulls.
        inL_ = jack_port_register(client_, "in_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        inR_ = jack_port_register(client_, "in_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!inL_ || !inR_) LOGW("JACK: no input ports, recording disabled");

        jack_set_process_callback(client_, &JackBackend::processCb, this);
        jack_set_sample_rate_callback(client_, &JackBackend::srCb, this);
        jack_set_buffer_size_callback(client_, &JackBackend::bufSizeCb, this);
        jack_set_xrun_callback(client_, &JackBackend::xrunCb, this);
        if (jack_activate(client_)) { jack_client_close(client_); client_ = nullptr; return false; }

        // Auto-connect to the system playback ports so there is sound without
        // the user having to open a patchbay.
        if (const char** ports = jack_get_ports(client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                                JackPortIsPhysical | JackPortIsInput)) {
            if (ports[0]) jack_connect(client_, jack_port_name(outL_), ports[0]);
            if (ports[1]) jack_connect(client_, jack_port_name(outR_), ports[1]);
            jack_free(ports);
        }
        // Same for capture. A physical *source* is an output port from the
        // graph's point of view, which is why the mask is inverted here.
        if (inL_ && inR_) {
            if (const char** ports = jack_get_ports(client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                                    JackPortIsPhysical | JackPortIsOutput)) {
                if (ports[0]) jack_connect(client_, ports[0], jack_port_name(inL_));
                // A mono interface exposes one capture port; feed it to both
                // sides so a mono source still records as centred stereo.
                if (ports[1]) jack_connect(client_, ports[1], jack_port_name(inR_));
                else if (ports[0]) jack_connect(client_, ports[0], jack_port_name(inR_));
                jack_free(ports);
            }
        }
        LOGI("JACK backend up: %.0f Hz, %d frames", sr_, bs_);
        return true;
    }

    void stop() override {
        if (!client_) return;
        jack_deactivate(client_);
        jack_client_close(client_);
        client_ = nullptr;
    }

    f64  sampleRate() const override { return sr_; }
    int  bufferSize() const override { return bs_; }
    const char* name() const override { return "JACK"; }

private:
    static int processCb(jack_nframes_t n, void* arg) {
        auto* self = (JackBackend*)arg;
        auto* l = (f32*)jack_port_get_buffer(self->outL_, n);
        auto* r = (f32*)jack_port_get_buffer(self->outR_, n);
        const f32* il = self->inL_ ? (const f32*)jack_port_get_buffer(self->inL_, n) : nullptr;
        const f32* ir = self->inR_ ? (const f32*)jack_port_get_buffer(self->inR_, n) : nullptr;
        self->engine_->process(il, ir, l, r, (int)n);
        return 0;
    }
    // JACK's sample rate is fixed for the lifetime of an active client on jack2
    // (and PipeWire's libjack shim): it is read once at start() and cannot change
    // under a running process callback. The old body called Engine::prepare()
    // straight from here — concurrently with process(), which JACK does NOT
    // serialise the sample-rate callback against — tearing RtClip cells a live
    // voice was mid-read, racing ~2 MB of stores, calling calloc()/fprintf() from
    // a callback, and nulling every chain with no retirement (RT-AUDIT §1.1,
    // CRITICAL). The race is simply removed: this is now a no-op that logs if
    // JACK ever contradicts the prepared rate. Honouring a live rate change would
    // require stopping the client and rebuilding every plugin off the audio
    // thread, which the daemon owns; there is nothing safe to do from here.
    static int srCb(jack_nframes_t n, void* arg) {
        auto* self = (JackBackend*)arg;
        if ((f64)n != self->sr_)
            LOGW("JACK sample rate changed %.0f -> %u Hz while active; not honoured, "
                 "restart required (engine stays prepared at %.0f Hz)",
                 self->sr_, (unsigned)n, self->sr_);
        return 0;
    }
    // Buffer size, unlike sample rate, CAN change at runtime (jack_set_buffer_size,
    // and PipeWire renegotiates on its own). JACK suspends the process cycle for
    // the duration of this callback and explicitly permits allocation here, so —
    // unlike srCb — it is safe to re-prepare the engine between blocks
    // (RT-AUDIT §4.1). prepare() resets engine state and drops device chains
    // without retiring them, on the same contract as a rate change: the owner
    // (daemon/GUI) republishes every chain afterwards. Blocks over kMaxBlock are
    // still clamped by process(); plugin instances prepared at the old size must
    // be rebuilt by the daemon to process the new size, or they degrade to
    // passthrough — that rebuild is out of this backend's reach.
    static int bufSizeCb(jack_nframes_t n, void* arg) {
        auto* self = (JackBackend*)arg;
        if ((int)n == self->bs_) return 0;
        LOGW("JACK buffer size %d -> %u frames; re-preparing engine (chains must be "
             "republished by the owner)", self->bs_, (unsigned)n);
        self->bs_ = (int)n;
        self->engine_->prepare(self->sr_, self->bs_);
        return 0;
    }
    // A dropout the JACK server observed. Called from JACK's own thread, not the
    // RT cycle; reportXrun() is a relaxed atomic increment, safe from anywhere.
    static int xrunCb(void* arg) {
        ((JackBackend*)arg)->engine_->reportXrun();
        return 0;
    }

    jack_client_t* client_ = nullptr;
    jack_port_t *outL_ = nullptr, *outR_ = nullptr;
    jack_port_t *inL_  = nullptr, *inR_  = nullptr;
    Engine* engine_ = nullptr;
    f64 sr_ = 48000.0;
    int bs_ = 1024;
};

// ---------------------------------------------------------------------------
// ALSA fallback
// ---------------------------------------------------------------------------

// libasound writes its own diagnostics straight to stderr, and on a machine
// with a partial /usr/share/alsa config -- which is most of them -- opening
// "default" walks the whole PCM definition tree and prints one line per
// definition it cannot resolve. A real session produced fifty of these before
// the app had drawn a frame:
//
//   ALSA lib confmisc.c:1377:(snd_func_refer) [error.core] Unable to find
//     definition 'cards.0.pcm.hdmi.0:CARD=0,AES0=4,AES1=130,AES2=0,AES3=2'
//   ALSA lib pcm.c:2722:(snd_pcm_open_noupdate) [error.pcm] Unknown PCM hdmi
//
// None of it is about this program. All of it says "error" in red, arrives
// before anything of ours, and buries the two lines that matter -- which
// backend came up and at what rate. Users reasonably read it as our crash.
//
// So they are routed through our logger instead, and the noisy stretch -- the
// PCM tree walk inside snd_pcm_open -- is COLLECTED rather than printed.
//
// Discarding it outright would be the easy version and the wrong one: if the
// device genuinely fails to open, those very lines are the explanation, and a
// silent "ALSA unavailable" is exactly the bug report nobody can act on. So:
//
//   * open succeeds -> one line saying how many notes were swallowed, and how
//     to see them. The walk complained about hdmi and phoneline definitions
//     while opening the card that works; that is not news.
//   * open fails    -> every collected line is printed. It is the diagnosis.
//   * after startup -> printed as they arrive, because a message from a live
//     stream (an xrun, a device disappearing) is always worth having.
//
// NXTAKT_ALSA_VERBOSE=1 leaves libasound's own handler in place, unfiltered.
static std::atomic<bool> gAlsaCollect{false};
static std::vector<std::string> gAlsaNotes;

static void alsaLogRedirect(const char* file, int line, const char* fn, int err,
                            const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    // file/line/fn are libasound's own source coordinates; they help nobody
    // here, so only the message and, when there is one, the errno survive.
    char full[600];
    if (err) snprintf(full, sizeof full, "%s (%s)", msg, snd_strerror(err));
    else     snprintf(full, sizeof full, "%s", msg);

    // Collection only ever runs on the startup thread, between the two calls
    // that bracket snd_pcm_open, so the vector needs no lock: any handler call
    // from the audio thread necessarily happens when the flag is false.
    if (gAlsaCollect.load(std::memory_order_relaxed)) {
        if (gAlsaNotes.size() < 200) gAlsaNotes.emplace_back(full);
        return;
    }

    // Outside a collected window -- overwhelmingly, a plugin opening a device
    // of its own -- print each DISTINCT message once and never again.
    //
    // This is what the real session needed and the device-open brackets did
    // not cover. The flood there arrived when the user loaded an instrument
    // that opens ALSA itself: fifty lines, but only about twelve distinct ones,
    // the same "Unknown PCM cards.pcm.rear" arriving four and twelve times over
    // as the plugin retried. Repetition carries no information here -- the
    // second occurrence of a definition libasound cannot resolve says exactly
    // what the first did -- so once is the honest amount to print.
    //
    // A plain std::mutex, not a lock-free structure: a plugin can instantiate
    // on the GUI thread while the audio thread is running, this handler is not
    // on any realtime path (libasound calls it while failing, not while
    // streaming), and correctness here is worth more than never blocking.
    {
        static std::mutex m;
        static std::set<std::string> seen;
        std::lock_guard<std::mutex> lk(m);
        if (!seen.insert(full).second) return;
        if (seen.size() == 1)
            LOGW("alsa: messages below come from libasound, not from NxTakt; "
                 "each distinct one is printed once");
    }
    LOGW("alsa: %s", full);
}

// Installed once, before the first snd_* call anywhere in the process.
//
// Called from main() rather than only from AlsaBackend::start, because the
// flood this exists for does not come from our backend at all. In the session
// that prompted this, the messages arrived MID-SESSION, long after audio was
// up, when the user loaded an instrument plugin that opens ALSA on its own.
// Installing it from the backend would have missed that entirely on a JACK
// machine -- which is the common case, and the case where those lines are
// least explicable.
void alsaInstallLogHandler() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* v = env("ALSA_VERBOSE");
    if (v && v[0] == '1') return;          // leave libasound's own handler alone
    snd_lib_error_set_handler(alsaLogRedirect);
}

// Bracket the device-open walk. `ok` decides whether what was collected was
// noise or evidence.
static void alsaCollectBegin() {
    gAlsaNotes.clear();
    gAlsaCollect.store(true, std::memory_order_relaxed);
}
static void alsaCollectEnd(bool ok) {
    gAlsaCollect.store(false, std::memory_order_relaxed);
    if (gAlsaNotes.empty()) return;
    if (ok) {
        LOGI("alsa: %zu configuration note(s) while opening the device, "
             "suppressed because it opened (NXTAKT_ALSA_VERBOSE=1 to see them)",
             gAlsaNotes.size());
    } else {
        LOGE("alsa: the device did not open; libasound said:");
        for (const std::string& n : gAlsaNotes) LOGE("  %s", n.c_str());
    }
    gAlsaNotes.clear();
}

class AlsaBackend final : public AudioBackend {
public:
    ~AlsaBackend() override { stop(); }

    bool start(Engine& e) override {
        engine_ = &e;
        alsaInstallLogHandler();
        alsaCollectBegin();
        const bool opened = snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0) >= 0;
        alsaCollectEnd(opened);
        if (!opened) return false;

        snd_pcm_hw_params_t* hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm_, hw);
        snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_FLOAT_LE);
        snd_pcm_hw_params_set_channels(pcm_, hw, 2);

        unsigned rate = 48000;
        snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr);
        snd_pcm_uframes_t period = 256;
        snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr);
        unsigned periods = 3;
        snd_pcm_hw_params_set_periods_near(pcm_, hw, &periods, nullptr);
        if (snd_pcm_hw_params(pcm_, hw) < 0) { snd_pcm_close(pcm_); pcm_ = nullptr; return false; }

        sr_ = (f64)rate;
        bs_ = (int)period;
        engine_->prepare(sr_, bs_);

        l_.resize((size_t)bs_);
        r_.resize((size_t)bs_);
        inter_.resize((size_t)bs_ * 2);
        capL_.assign((size_t)bs_, 0.f);
        capR_.assign((size_t)bs_, 0.f);
        capInter_.assign((size_t)bs_ * 2, 0.f);

        openCapture();

        run_.store(true);
        thread_ = std::thread(&AlsaBackend::loop, this);
        LOGI("ALSA backend up: %.0f Hz, %d frames", sr_, bs_);
        return true;
    }

    void stop() override {
        if (!pcm_) return;
        run_.store(false);
        if (thread_.joinable()) thread_.join();
        if (cap_) { snd_pcm_drop(cap_); snd_pcm_close(cap_); cap_ = nullptr; }
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }

    f64  sampleRate() const override { return sr_; }
    int  bufferSize() const override { return bs_; }
    const char* name() const override { return "ALSA"; }

private:
    // Capture rides alongside playback on the same device, rate and period so
    // the two streams stay in lockstep without a resampler or a ring buffer in
    // between. It is strictly optional: a machine with no input (or an input
    // that will not do float/stereo) still plays back, it just cannot record.
    void openCapture() {
        // Bracketed like the playback open, and for a sharper reason: capture
        // is optional, so its failure is a warning rather than a stop. Without
        // collection, the *expected* case -- a machine with no capture device --
        // prints the entire PCM walk before the one line that explains it.
        alsaCollectBegin();
        const bool opened = snd_pcm_open(&cap_, "default", SND_PCM_STREAM_CAPTURE, 0) >= 0;
        alsaCollectEnd(/*ok=*/true);   // never evidence: the next line is the verdict
        if (!opened) {
            cap_ = nullptr;
            LOGW("ALSA: no capture device, recording and input monitoring disabled");
            return;
        }
        snd_pcm_hw_params_t* hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(cap_, hw);
        snd_pcm_hw_params_set_access(cap_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(cap_, hw, SND_PCM_FORMAT_FLOAT_LE);
        snd_pcm_hw_params_set_channels(cap_, hw, 2);

        unsigned rate = (unsigned)sr_;
        snd_pcm_hw_params_set_rate_near(cap_, hw, &rate, nullptr);
        snd_pcm_uframes_t period = (snd_pcm_uframes_t)bs_;
        snd_pcm_hw_params_set_period_size_near(cap_, hw, &period, nullptr);
        unsigned periods = 3;
        snd_pcm_hw_params_set_periods_near(cap_, hw, &periods, nullptr);

        // A capture stream that disagrees about rate or period would drift
        // against playback; better no input than input that slides.
        if (snd_pcm_hw_params(cap_, hw) < 0 || (f64)rate != sr_ || (int)period != bs_) {
            LOGW("ALSA: capture cannot match %.0f Hz / %d frames, recording disabled", sr_, bs_);
            snd_pcm_close(cap_);
            cap_ = nullptr;
            return;
        }
        snd_pcm_prepare(cap_);
        snd_pcm_start(cap_);
        LOGI("ALSA capture up: %.0f Hz, %d frames", sr_, bs_);
    }

    // Fills capL_/capR_ for this cycle. Returns false once capture is gone, in
    // which case the engine is handed nulls from here on.
    bool readCapture() {
        if (!cap_) return false;
        const snd_pcm_sframes_t got = snd_pcm_readi(cap_, capInter_.data(), (snd_pcm_uframes_t)bs_);
        if (got < 0) {
            engine_->reportXrun();               // a capture over/underrun is a dropout
            if (snd_pcm_recover(cap_, (int)got, 1) < 0) {
                LOGW("ALSA: capture stream lost, continuing without input");
                snd_pcm_close(cap_);
                cap_ = nullptr;
                return false;
            }
            // A recovered xrun has no samples to show for this cycle.
            std::fill(capL_.begin(), capL_.end(), 0.f);
            std::fill(capR_.begin(), capR_.end(), 0.f);
            return true;
        }
        for (int i = 0; i < bs_; ++i) {
            const bool have = i < (int)got;
            capL_[(size_t)i] = have ? capInter_[(size_t)i * 2]     : 0.f;
            capR_[(size_t)i] = have ? capInter_[(size_t)i * 2 + 1] : 0.f;
        }
        return true;
    }

    void loop() {
        // Best effort: ask for realtime priority, carry on without it.
        sched_param sp{};
        sp.sched_priority = 70;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

        while (run_.load(std::memory_order_relaxed)) {
            const bool haveIn = readCapture();
            engine_->process(haveIn ? capL_.data() : nullptr,
                             haveIn ? capR_.data() : nullptr,
                             l_.data(), r_.data(), bs_);
            for (int i = 0; i < bs_; ++i) { inter_[i * 2] = l_[i]; inter_[i * 2 + 1] = r_[i]; }
            const snd_pcm_sframes_t w = snd_pcm_writei(pcm_, inter_.data(), (snd_pcm_uframes_t)bs_);
            if (w < 0) {
                engine_->reportXrun();           // playback underrun: the audible dropout
                if (snd_pcm_recover(pcm_, (int)w, 1) < 0) break;
            }
        }
    }

    snd_pcm_t* pcm_ = nullptr;
    snd_pcm_t* cap_ = nullptr;
    Engine* engine_ = nullptr;
    std::thread thread_;
    std::atomic<bool> run_{false};
    std::vector<f32> l_, r_, inter_;
    std::vector<f32> capL_, capR_, capInter_;
    f64 sr_ = 48000.0;
    int bs_ = 256;
};

// ---------------------------------------------------------------------------

std::unique_ptr<AudioBackend> createBackend(Engine& e, const char* prefer) {
    const bool wantJack = !prefer || std::strcmp(prefer, "jack") == 0;
    const bool wantAlsa = !prefer || std::strcmp(prefer, "alsa") == 0;

    if (wantJack) {
        auto b = std::make_unique<JackBackend>();
        if (b->start(e)) return b;
        LOGW("JACK unavailable, falling back");
    }
    if (wantAlsa) {
        auto b = std::make_unique<AlsaBackend>();
        if (b->start(e)) return b;
        LOGE("ALSA unavailable too");
    }
    return nullptr;
}

} // namespace lat
