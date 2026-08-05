#include "backend.h"
#include "engine.h"

#include <jack/jack.h>
#include <alsa/asoundlib.h>

#include <atomic>
#include <thread>
#include <vector>
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
        client_ = jack_client_open("Lattice", JackNoStartServer, &st);
        if (!client_) return false;

        sr_ = (f64)jack_get_sample_rate(client_);
        bs_ = (int)jack_get_buffer_size(client_);
        engine_->prepare(sr_, bs_);

        outL_ = jack_port_register(client_, "out_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        outR_ = jack_port_register(client_, "out_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!outL_ || !outR_) { jack_client_close(client_); client_ = nullptr; return false; }

        jack_set_process_callback(client_, &JackBackend::processCb, this);
        jack_set_sample_rate_callback(client_, &JackBackend::srCb, this);
        if (jack_activate(client_)) { jack_client_close(client_); client_ = nullptr; return false; }

        // Auto-connect to the system playback ports so there is sound without
        // the user having to open a patchbay.
        if (const char** ports = jack_get_ports(client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                                JackPortIsPhysical | JackPortIsInput)) {
            if (ports[0]) jack_connect(client_, jack_port_name(outL_), ports[0]);
            if (ports[1]) jack_connect(client_, jack_port_name(outR_), ports[1]);
            jack_free(ports);
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
        self->engine_->process(l, r, (int)n);
        return 0;
    }
    static int srCb(jack_nframes_t n, void* arg) {
        auto* self = (JackBackend*)arg;
        self->sr_ = (f64)n;
        self->engine_->prepare(self->sr_, self->bs_);
        return 0;
    }

    jack_client_t* client_ = nullptr;
    jack_port_t *outL_ = nullptr, *outR_ = nullptr;
    Engine* engine_ = nullptr;
    f64 sr_ = 48000.0;
    int bs_ = 1024;
};

// ---------------------------------------------------------------------------
// ALSA fallback
// ---------------------------------------------------------------------------
class AlsaBackend final : public AudioBackend {
public:
    ~AlsaBackend() override { stop(); }

    bool start(Engine& e) override {
        engine_ = &e;
        if (snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) return false;

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

        run_.store(true);
        thread_ = std::thread(&AlsaBackend::loop, this);
        LOGI("ALSA backend up: %.0f Hz, %d frames", sr_, bs_);
        return true;
    }

    void stop() override {
        if (!pcm_) return;
        run_.store(false);
        if (thread_.joinable()) thread_.join();
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }

    f64  sampleRate() const override { return sr_; }
    int  bufferSize() const override { return bs_; }
    const char* name() const override { return "ALSA"; }

private:
    void loop() {
        // Best effort: ask for realtime priority, carry on without it.
        sched_param sp{};
        sp.sched_priority = 70;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

        while (run_.load(std::memory_order_relaxed)) {
            engine_->process(l_.data(), r_.data(), bs_);
            for (int i = 0; i < bs_; ++i) { inter_[i * 2] = l_[i]; inter_[i * 2 + 1] = r_[i]; }
            const snd_pcm_sframes_t w = snd_pcm_writei(pcm_, inter_.data(), (snd_pcm_uframes_t)bs_);
            if (w < 0) {
                if (snd_pcm_recover(pcm_, (int)w, 1) < 0) break;
            }
        }
    }

    snd_pcm_t* pcm_ = nullptr;
    Engine* engine_ = nullptr;
    std::thread thread_;
    std::atomic<bool> run_{false};
    std::vector<f32> l_, r_, inter_;
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
