// NxTakt's own stock devices.
//
// These are ordinary PluginInstance implementations, so they ride the browser,
// the device strip, bypass, parameter persistence and the chain scheduler with
// no special cases anywhere else. That is the whole point of hosting them
// through the plugin contract instead of hard-coding them into the engine.
//
// Realtime rules are the same as every other backend: everything the audio
// thread touches is a fixed-size member allocated (or rather, sized) at
// construction. prepare() only recomputes coefficients; process() and midi()
// allocate nothing, lock nothing and throw nothing.
//
// Parameters are plain float stores. The GUI writes them while the audio thread
// reads them, exactly as documented on PluginInstance::setParam: a 4-byte
// aligned float cannot tear on any target we build for, and a stale read costs
// at most one block of latency.
#include "host.h"

#include <cmath>
#include <cstring>

namespace lat {
namespace detail {
namespace {

// Device URIs. These are not decoration: a saved set stores the URI verbatim
// and asks the registry for it again on load, so a URI is a permanent public
// identifier the moment one set has been written with it.
//
// The scheme was `lattice:` before the rename and is `nxtakt:` after it. The
// canonical spelling -- the one the descriptor carries, and therefore the one
// serializeDevices writes into every new save -- is the new one. The old one
// stays a resolvable ALIAS forever, because every set saved before the rename
// names its devices by it. Resolution happens in two places, and both are
// required:
//
//   * PluginRegistry::find (host.cpp) maps an alias to the canonical
//     descriptor. This is what a project load and a daemon AddDevice go
//     through, so it is what makes an old set materialise its devices.
//   * instantiateInternal, below, accepts either spelling directly, so a
//     PluginDesc that came from an old project file (rather than from the
//     registry) still loads.
//
// Loading an old set and re-saving it therefore rewrites `lattice:pulse` to
// `nxtakt:pulse` -- the descriptor won, as it must, since the registry only
// ever hands back canonical descriptors. That is a one-way upgrade of the
// user's file, and it is safe precisely because the alias never expires: the
// upgraded file is readable by nothing older, but the un-upgraded one stays
// readable by everything newer.
constexpr const char* kSaturatorUri = "nxtakt:saturator";
constexpr const char* kPulseUri     = "nxtakt:pulse";

// Pre-rename spellings. Append-only; an entry may never be removed.
constexpr const char* kSaturatorUriLegacy = "lattice:saturator";
constexpr const char* kPulseUriLegacy     = "lattice:pulse";

// Denormals cost hundreds of cycles per operation on x86 when they leak into a
// feedback path (the one-pole filter state, a decaying envelope). We do not
// control the FPU mode of whatever thread the host handed us, so every state
// variable that can decay towards zero is flushed explicitly.
inline f32 flushDenormal(f32 v) {
    return (std::fabs(v) < 1e-25f) ? 0.f : v;
}

// --- shared base -----------------------------------------------------------
// Parameter storage, descriptor and bypass are identical for every internal
// device, so they live here and each device only writes DSP.
class InternalInstance : public PluginInstance {
public:
    explicit InternalInstance(const PluginDesc& d) : desc_(d) {}

    int              paramCount() const override     { return n_; }
    const ParamInfo& paramInfo(int i) const override { return info_[(size_t)i]; }

    f32 getParam(int i) const override {
        return (i >= 0 && i < n_) ? pv_[(size_t)i] : 0.f;
    }

    // GUI thread, concurrent with process(). See the file header.
    void setParam(int i, f32 v) override {
        if (i < 0 || i >= n_) return;
        pv_[(size_t)i] = clampv(v, info_[(size_t)i].min, info_[(size_t)i].max);
    }

    // REALTIME (host.h): the automation path. Literally setParam's body, and
    // that is the honest answer for this backend rather than a shortcut —
    // there is no queue to have a second producer on, only a clamp and a
    // 4-byte aligned plain store into pv_[], which is precisely what the file
    // header already argues is safe for the GUI-side writer. Two writers
    // instead of one changes nothing about tearing: the value a run() reads is
    // always one of the two that were written, never a mixture.
    bool setParamRT(int i, f32 v) override {
        if (i < 0 || i >= n_) return true;    // out of range, not "no RT path"
        pv_[(size_t)i] = clampv(v, info_[(size_t)i].min, info_[(size_t)i].max);
        return true;
    }

    const PluginDesc& desc() const override { return desc_; }

    // Stated explicitly rather than inherited from the default in host.h, so
    // that "these devices are sample-aligned with their own input" is a
    // property of the devices and not an accident of what the base class
    // happens to return today.
    //
    // It is true by construction for both: the Saturator is a memoryless
    // waveshaper (out[i] depends only on in[i]), and Pulse is a generator whose
    // first sample of a voice lands on the frame the note-on asked for. Neither
    // has a lookahead buffer, an FFT window or an oversampling filter, which
    // are the three things that produce latency. Any future internal device
    // that does acquire one must override this again with its real figure --
    // the engine's delay compensation trusts it, so reporting 0 while actually
    // delaying would smear transients across every parallel path.
    int latencyFrames() const override      { return 0; }

    void setBypassed(bool b) override       { bypassed_ = b; }
    bool bypassed() const override          { return bypassed_; }

protected:
    static constexpr int kMaxParams = 16;

    // Construction time only. Returns the parameter index so devices can keep
    // named constants honest.
    int addParam(const char* name, const char* unit, f32 mn, f32 mx, f32 def,
                 bool logarithmic = false) {
        if (n_ >= kMaxParams) return n_ - 1;
        ParamInfo& pi = info_[(size_t)n_];
        pi.name = name;
        pi.unit = unit;
        pi.min  = mn;
        pi.max  = mx;
        pi.def  = clampv(def, mn, mx);
        pi.isLogarithmic = logarithmic;
        pi.id   = (u32)n_;
        pv_[(size_t)n_] = pi.def;
        return n_++;
    }

    f32 p(int i) const { return pv_[(size_t)i]; }

    // REALTIME. Bypass and "we have nothing to say" both land here.
    static void passthrough(const f32* const* in, f32* const* out, int channels, int nframes) {
        const size_t bytes = (size_t)nframes * sizeof(f32);
        for (int c = 0; c < channels; ++c) {
            if (!out[c]) continue;
            const f32* src = in ? in[c] : nullptr;
            if (src == out[c]) continue;              // in-place: already correct
            if (src) std::memcpy(out[c], src, bytes);
            else     std::memset(out[c], 0, bytes);
        }
    }

    PluginDesc desc_;
    ParamInfo  info_[kMaxParams];
    f32        pv_[kMaxParams]{};
    int        n_ = 0;
    bool       bypassed_ = false;
    f64        sr_ = 48000.0;
    int        maxBlock_ = kMaxBlock;
};

// --- Saturator -------------------------------------------------------------
// tanh waveshaper with gain compensation.
//
// Compensation: the shaper is y = tanh(g*x) * tanh(a0) / tanh(g*a0), with the
// reference amplitude a0 = 0.5 (-6 dBFS, roughly where a mixed signal sits).
// The trailing factor is exactly the gain that keeps a sine of amplitude a0 at
// the same peak level it had at 0 dB drive, so turning the knob changes the
// *shape* and not the loudness. Two properties fall out of writing it this way:
// at drive = 0 the factor is 1 and the device is exactly tanh(x) (unity for
// small signals), and at large drive it tends to tanh(a0) = 0.462, i.e. the
// hard-clipped square is pulled back to the reference level instead of running
// 20 dB hot. It is a static compensation, not a loudness match — a bass-heavy
// source will still read louder when driven, which is the point of the device.
class Saturator final : public InternalInstance {
public:
    explicit Saturator(const PluginDesc& d) : InternalInstance(d) {
        // dB ranges are already perceptual, but a linear knob over 36 dB spends
        // most of its travel in the region that sounds destroyed, so the flag
        // asks the UI to skew the control's *normalised position*. It is not a
        // request to take a logarithm of the value, which would be undefined at
        // the 0 dB end of the range.
        pDrive_ = addParam("Drive",  "dB", 0.f,   36.f, 0.f, true);
        pTrim_  = addParam("Output", "dB", -24.f, 24.f, 0.f);
        pMix_   = addParam("Mix",    "",   0.f,   1.f,  1.f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;
        return true;                                   // stateless, zero latency
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (bypassed_ || !in) { passthrough(in, out, channels, nframes); return; }

        // Coefficients are read once per block: a knob turn lands on the next
        // block boundary, which is the same latency every other backend has.
        const f32 g    = dbToGain(clampv(p(pDrive_), 0.f, 36.f));
        const f32 trim = dbToGain(clampv(p(pTrim_), -24.f, 24.f));
        const f32 mix  = clampv(p(pMix_), 0.f, 1.f);

        constexpr f32 kRef = 0.5f;                     // -6 dBFS reference
        const f32 comp = std::tanh(kRef) / std::tanh(g * kRef);

        for (int c = 0; c < channels; ++c) {
            const f32* src = in[c];
            f32* dst = out[c];
            if (!dst) continue;
            if (!src) { std::memset(dst, 0, (size_t)nframes * sizeof(f32)); continue; }

            for (int i = 0; i < nframes; ++i) {
                const f32 x   = src[i];
                const f32 wet = std::tanh(g * x) * comp;
                // Trim sits after the mix so it stays a true output level even
                // when the device is running mostly dry.
                dst[i] = flushDenormal((x + (wet - x) * mix) * trim);
            }
        }
    }

private:
    int pDrive_ = 0, pTrim_ = 0, pMix_ = 0;
};

// --- Pulse -----------------------------------------------------------------
// Eight-voice subtractive synth. Deliberately small: one morphing oscillator,
// one one-pole lowpass with envelope modulation, one ADR envelope.
//
// Threading: midi() and process() both run on the audio thread and midi() is
// documented to be called before process() for the same block, so voice state
// is plain members with no synchronisation at all. Note-on/off carry a frame
// offset, which is honoured by starting (or releasing) the voice partway
// through the block rather than by splitting the render into segments.
class Pulse final : public InternalInstance {
public:
    explicit Pulse(const PluginDesc& d) : InternalInstance(d) {
        pShape_   = addParam("Shape",    "",   0.f,  1.f,     0.5f);
        pCutoff_  = addParam("Cutoff",   "Hz", 20.f, 18000.f, 6000.f, true);
        pEnvAmt_  = addParam("Env Amt",  "",   0.f,  1.f,     0.4f);
        pAttack_  = addParam("Attack",   "s",  0.001f, 2.f,   0.005f, true);
        pDecay_   = addParam("Decay",    "s",  0.001f, 2.f,   0.6f,   true);
        pRelease_ = addParam("Release",  "s",  0.001f, 2.f,   0.15f,  true);
        pVolume_  = addParam("Volume",   "dB", -60.f, 6.f,    -6.f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;
        for (Voice& v : voices_) v = Voice{};
        age_ = 0;
        return true;
    }

    // REALTIME. Only touches voice state; the render happens in process().
    void midi(const u8* data, int len, int frameOffset) override {
        if (!data || len < 1) return;
        const u8 status = (u8)(data[0] & 0xF0u);
        const int off = frameOffset < 0 ? 0 : frameOffset;

        switch (status) {
            case 0x90:                                  // note on (vel 0 = off)
                if (len >= 3 && data[2] > 0) { noteOn(data[1], data[2], off); return; }
                if (len >= 2) noteOff(data[1], off);
                return;
            case 0x80:
                if (len >= 2) noteOff(data[1], off);
                return;
            case 0xB0:
                // 120 = all sound off, 123 = all notes off. Anything else is a
                // controller we do not map; ignoring it is the honest answer.
                if (len >= 2 && data[1] == 120) allSoundOff();
                else if (len >= 2 && data[1] == 123) allNotesOff(off);
                return;
            default:
                return;
        }
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        // The accumulator is sized for kMaxBlock and process() may not grow it,
        // so an oversized block degrades to silence rather than a heap call.
        if (bypassed_ || nframes > kMaxBlock) {
            // An instrument's "input" is silence, so bypass means silence out,
            // not passthrough of whatever the chain handed us.
            passthrough(nullptr, out, channels, nframes);
            clearSchedule();
            return;
        }

        // Per-block coefficients. Envelope times are converted to a linear ramp
        // (attack) and to one-pole decay coefficients (decay/release) so the
        // per-sample cost is one multiply.
        const f32 shape  = clampv(p(pShape_), 0.f, 1.f);
        const f32 cutoff = clampv(p(pCutoff_), 20.f, 18000.f);
        const f32 envAmt = clampv(p(pEnvAmt_), 0.f, 1.f);
        const f32 vol    = dbToGain(clampv(p(pVolume_), -60.f, 6.f));

        const f32 atkInc = 1.f / std::fmax(1.f, (f32)(clampv(p(pAttack_), 0.001f, 2.f) * sr_));
        const f32 decCf  = decayCoef(clampv(p(pDecay_), 0.001f, 2.f));
        const f32 relCf  = decayCoef(clampv(p(pRelease_), 0.001f, 2.f));

        // The one-pole coefficient is the small-angle approximation of
        // 1 - exp(-2*pi*fc/sr): accurate below about sr/8 and, more to the
        // point, cheap enough to evaluate once per sample per voice while the
        // envelope sweeps the cutoff.
        const f32 wScale = (f32)(6.2831853f / sr_);
        const f32 fcMax  = (f32)(sr_ * 0.45);

        // Render into the accumulator, then fan out. Voices are mono, so both
        // output channels get the same signal; a stereo spread would need a
        // per-voice pan and is not worth the parameter yet.
        for (int i = 0; i < nframes; ++i) acc_[(size_t)i] = 0.f;

        for (Voice& v : voices_) {
            if (!v.active && v.startFrame <= 0) continue;

            for (int i = 0; i < nframes; ++i) {
                if (i < v.startFrame) continue;         // note-on later in the block
                if (v.offFrame >= 0 && i == v.offFrame && v.stage != kRelease) {
                    v.stage = kRelease;
                    v.offFrame = -1;
                }
                if (!v.active) break;

                // Envelope.
                switch (v.stage) {
                    case kAttack:
                        v.env += atkInc;
                        if (v.env >= 1.f) { v.env = 1.f; v.stage = kDecay; }
                        break;
                    case kDecay:   v.env *= decCf; break;
                    default:       v.env *= relCf; break;
                }
                if (v.env < 1e-4f && v.stage != kAttack) {
                    v.active = false;
                    v.env = 0.f;
                    break;
                }

                // Oscillator.
                const f32 osc = oscillator(v.phase, v.inc, shape);
                v.phase += v.inc;
                if (v.phase >= 1.f) v.phase -= 1.f;

                // Filter: cutoff tracks the envelope up to five octaves.
                const f32 fc = clampv(cutoff * std::exp2(envAmt * v.env * 5.f), 20.f, fcMax);
                const f32 a  = clampv(fc * wScale, 0.f, 0.99f);
                v.lp = flushDenormal(v.lp + a * (osc - v.lp));

                acc_[(size_t)i] += v.lp * v.env * v.vel;
            }

            // Consume the schedule: offsets are relative to the block that has
            // just been rendered.
            v.startFrame = v.startFrame > nframes ? v.startFrame - nframes : 0;
            if (v.offFrame >= 0) {
                // Note-off landed past the end of this block (the engine should
                // not do that, but clamp instead of losing the note).
                v.offFrame = v.offFrame >= nframes ? v.offFrame - nframes : -1;
                if (v.offFrame < 0 && v.active && v.stage != kRelease) v.stage = kRelease;
            }
        }

        for (int c = 0; c < channels; ++c) {
            f32* dst = out[c];
            if (!dst) continue;
            for (int i = 0; i < nframes; ++i) dst[i] = acc_[(size_t)i] * vol;
        }
    }

private:
    enum : u8 { kAttack = 0, kDecay, kRelease };
    static constexpr int kVoices = 8;

    struct Voice {
        bool active = false;
        u8   note   = 0;
        u8   stage  = kAttack;
        f32  vel    = 0.f;
        f32  phase  = 0.f;
        f32  inc    = 0.f;     // cycles per sample
        f32  env    = 0.f;
        f32  lp     = 0.f;
        u32  age    = 0;       // note-on order, for oldest-first stealing
        int  startFrame = 0;   // sample offset of the pending note-on
        int  offFrame   = -1;  // sample offset of the pending note-off, or -1
    };

    // One-pole coefficient that decays to -60 dB in `seconds`.
    f32 decayCoef(f32 seconds) const {
        const f32 n = std::fmax(1.f, (f32)(seconds * sr_));
        return std::exp(-6.9077553f / n);              // ln(1000) = 6.908
    }

    // Morphing oscillator: sine -> saw -> square across shape 0..1. Saw and
    // square are PolyBLEP-corrected; without it a note near the top of the
    // keyboard folds a wall of aliases back down into the mix.
    static f32 polyBlep(f32 t, f32 dt) {
        if (dt <= 0.f) return 0.f;
        if (t < dt)          { const f32 x = t / dt;       return x + x - x * x - 1.f; }
        if (t > 1.f - dt)    { const f32 x = (t - 1.f) / dt; return x * x + x + x + 1.f; }
        return 0.f;
    }

    static f32 oscillator(f32 phase, f32 inc, f32 shape) {
        if (shape <= 0.f) return std::sin(6.2831853f * phase);

        f32 saw = 2.f * phase - 1.f - polyBlep(phase, inc);
        if (shape < 0.5f) {
            const f32 sine = std::sin(6.2831853f * phase);
            return lerpf(sine, saw, shape * 2.f);
        }
        f32 half = phase + 0.5f;
        if (half >= 1.f) half -= 1.f;
        const f32 sq = (phase < 0.5f ? 1.f : -1.f) + polyBlep(phase, inc) - polyBlep(half, inc);
        return lerpf(saw, sq, (shape - 0.5f) * 2.f);
    }

    void noteOn(u8 note, u8 vel, int frameOffset) {
        Voice* v = allocVoice();
        v->active = true;
        v->note   = note;
        v->stage  = kAttack;
        // Velocity is squared-ish: linear velocity feels dead at the bottom of
        // the range on a synth this simple.
        const f32 nv = (f32)vel / 127.f;
        v->vel   = nv * nv;
        v->phase = 0.f;
        v->inc   = (f32)(440.0 * std::pow(2.0, ((f64)note - 69.0) / 12.0) / sr_);
        if (v->inc > 0.45f) v->inc = 0.45f;            // above Nyquist, park it
        v->env   = 0.f;
        v->lp    = 0.f;
        v->age   = ++age_;
        v->startFrame = frameOffset;
        v->offFrame   = -1;
    }

    void noteOff(u8 note, int frameOffset) {
        // Newest matching voice first: a repeated note that stole its own older
        // voice should release the one actually sounding.
        Voice* best = nullptr;
        for (Voice& v : voices_) {
            if (!v.active || v.note != note || v.offFrame >= 0) continue;
            if (v.stage == kRelease) continue;
            if (!best || v.age > best->age) best = &v;
        }
        if (best) best->offFrame = frameOffset;
    }

    void allNotesOff(int frameOffset) {
        for (Voice& v : voices_)
            if (v.active && v.stage != kRelease && v.offFrame < 0) v.offFrame = frameOffset;
    }

    void allSoundOff() {
        for (Voice& v : voices_) v = Voice{};
    }

    void clearSchedule() {
        for (Voice& v : voices_) { v.startFrame = 0; v.offFrame = -1; }
    }

    // Free voice if there is one, otherwise the oldest — releasing voices are
    // preferred over held ones so a sustained chord survives a stray note.
    Voice* allocVoice() {
        Voice* oldest = &voices_[0];
        Voice* oldestReleasing = nullptr;
        for (Voice& v : voices_) {
            if (!v.active) return &v;
            if (v.age < oldest->age) oldest = &v;
            if (v.stage == kRelease && (!oldestReleasing || v.age < oldestReleasing->age))
                oldestReleasing = &v;
        }
        return oldestReleasing ? oldestReleasing : oldest;
    }

    int pShape_ = 0, pCutoff_ = 0, pEnvAmt_ = 0;
    int pAttack_ = 0, pDecay_ = 0, pRelease_ = 0, pVolume_ = 0;

    Voice voices_[kVoices];
    u32   age_ = 0;
    // Fixed at kMaxBlock rather than sized in prepare() so process() can never
    // meet a buffer that has not been allocated yet.
    f32   acc_[kMaxBlock]{};
};

PluginDesc saturatorDesc() {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = kSaturatorUri;
    d.name       = "Saturator";
    d.vendor     = "NxTakt";
    d.category   = "Distortion";
    d.kind       = PluginKind::Effect;
    d.audioIn    = 2;
    d.audioOut   = 2;
    d.hasMidiIn  = false;
    d.paramCount = 3;
    return d;
}

PluginDesc pulseDesc() {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = kPulseUri;
    d.name       = "Pulse";
    d.vendor     = "NxTakt";
    d.category   = "Instrument";
    d.kind       = PluginKind::Instrument;
    d.audioIn    = 0;
    d.audioOut   = 2;
    d.hasMidiIn  = true;
    d.paramCount = 7;
    return d;
}

} // namespace

// --- entry points ----------------------------------------------------------
void scanInternal(std::vector<PluginDesc>& out) {
    out.push_back(saturatorDesc());
    out.push_back(pulseDesc());
    LOGI("internal: 2 devices");
}

std::unique_ptr<PluginInstance> instantiateInternal(const PluginDesc& d,
                                                    f64 sampleRate, int maxBlock) {
    // Both spellings, always: see the note at kSaturatorUri. A descriptor that
    // came from a pre-rename project file rather than from the registry still
    // arrives here carrying `lattice:`.
    std::unique_ptr<PluginInstance> inst;
    if (d.uri == kSaturatorUri || d.uri == kSaturatorUriLegacy)
        inst = std::make_unique<Saturator>(saturatorDesc());
    else if (d.uri == kPulseUri || d.uri == kPulseUriLegacy)
        inst = std::make_unique<Pulse>(pulseDesc());
    else {
        LOGE("internal: unknown device %s", d.uri.c_str());
        return nullptr;
    }
    // The descriptor is rebuilt from source rather than trusting the caller's
    // copy, which may have come from a project file written by an older build.
    if (!inst->prepare(sampleRate, maxBlock)) return nullptr;
    return inst;
}

} // namespace detail
} // namespace lat
