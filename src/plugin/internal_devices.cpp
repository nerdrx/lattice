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
#include "internal_dsp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lat {
namespace detail {
namespace {

using dsp::flushDenormal;

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
constexpr const char* kSaturatorUri  = "nxtakt:saturator";
constexpr const char* kPulseUri      = "nxtakt:pulse";
// Added after the rename, so these have no legacy spelling and never will: no
// project file has ever named them `lattice:*`. The scheme-swap alias in
// host.cpp would happily map `lattice:eq3` onto them, which is harmless -- that
// URI was never written by anything.
constexpr const char* kEq3Uri        = "nxtakt:eq3";
constexpr const char* kCompressorUri = "nxtakt:compressor";
constexpr const char* kDelayUri      = "nxtakt:delay";
constexpr const char* kReverbUri     = "nxtakt:reverb";
constexpr const char* kRackUri       = "nxtakt:rack";
constexpr const char* kAutoFilterUri = "nxtakt:autofilter";
constexpr const char* kChorusUri     = "nxtakt:chorus";
constexpr const char* kLimiterUri    = "nxtakt:limiter";
constexpr const char* kUtilityUri    = "nxtakt:utility";

// Pre-rename spellings. Append-only; an entry may never be removed.
constexpr const char* kSaturatorUriLegacy = "lattice:saturator";
constexpr const char* kPulseUriLegacy     = "lattice:pulse";

// Musical divisions, in beats. 1/32 up to one bar. Triplets and dotted values
// are included because a delay without a dotted eighth is a delay nobody uses.
// "1 bar" is four beats: the time signature is not on the plugin contract
// either, and 4/4 is the honest default rather than a guess we could get wrong.
//
// Shared by every device with a Sync switch -- the Delay and the Auto Filter
// today. One table, so a division added later cannot mean 1/16 on one device
// and a dotted eighth on another; and because the INDEX is what a project file
// stores, the table is APPEND-ONLY. Reordering it would silently retune every
// saved delay in the world.
constexpr int kDivCount = 9;
constexpr f32 kDivBeats[kDivCount] = {
    0.125f,      // 1/32
    0.25f,       // 1/16
    1.f / 3.f,   // 1/8 triplet
    0.5f,        // 1/8
    0.75f,       // 1/8 dotted
    1.f,         // 1/4
    1.5f,        // 1/4 dotted
    2.f,         // 1/2
    4.f,         // 1 bar
};

// Denormals cost hundreds of cycles per operation on x86 when they leak into a
// feedback path (the one-pole filter state, a decaying envelope). We do not
// control the FPU mode of whatever thread the host handed us, so every state
// variable that can decay towards zero is flushed explicitly. The helper lives
// in internal_dsp.h now, next to the filters that need it most.

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
    // It is true by construction for all of them but one. The Saturator is a
    // memoryless waveshaper (out[i] depends only on in[i]); Pulse is a
    // generator whose first sample of a voice lands on the frame the note-on
    // asked for; EQ Three, the Compressor and the Auto Filter are recursive but
    // causal, so their first output sample responds to their first input
    // sample; the Delay, the Chorus and the Reverb are parallel wet paths whose
    // dry component is untouched, and their delay is the effect rather than a
    // processing cost; the Utility is a per-sample gain. None of those has a
    // lookahead buffer, an FFT window or an oversampling filter, which are the
    // three things that produce latency.
    //
    // THE EXCEPTION IS THE LIMITER, which overrides this with the real figure
    // because it genuinely delays its output — see the comment on the device.
    // Any other internal device that acquires a lookahead must do the same: the
    // engine's delay compensation trusts what it is told and caches it when the
    // chain is published, so reporting 0 while actually delaying would smear
    // transients across every parallel path in the set.
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

    // A switch. Stored as a float like everything else -- the contract has one
    // parameter type -- with isBool set so the UI draws a toggle instead of a
    // knob with two positions.
    int addBoolParam(const char* name, bool def) {
        const int i = addParam(name, "", 0.f, 1.f, def ? 1.f : 0.f);
        info_[(size_t)i].isBool = true;
        return i;
    }

    // A stepped choice (the delay's musical division). isInt asks the UI to
    // quantise the knob; the DSP rounds anyway, because an automation lane can
    // still hand us 3.5.
    int addIntParam(const char* name, int mn, int mx, int def) {
        const int i = addParam(name, "", (f32)mn, (f32)mx, (f32)def);
        info_[(size_t)i].isInt = true;
        return i;
    }

    f32 p(int i) const { return pv_[(size_t)i]; }

    // REALTIME. An OUTPUT value the device publishes for the UI to display --
    // today only the compressor's gain reduction.
    //
    // THE WART, stated plainly: ParamInfo has no read-only flag (host.h), so a
    // meter has to be an ordinary parameter that the device writes over. Three
    // consequences, all of them tolerable and none of them invisible:
    //   * the UI draws it as a knob the user can turn. Turning it does nothing
    //     for longer than one block, because process() overwrites it.
    //   * it is automatable. Drawing an envelope on it is equally pointless,
    //     and setParamRT's return value cannot say so.
    //   * it is persisted with the rest of the parameters, so a saved set
    //     carries whatever the meter happened to read at save time. Harmless:
    //     the next block overwrites it.
    // The fix is one bool on ParamInfo (`isOutput`) plus the UI honouring it,
    // which is a host.h change and therefore not this file's to make.
    void setReadout(int i, f32 v) {
        if (i < 0 || i >= n_) return;
        pv_[(size_t)i] = clampv(v, info_[(size_t)i].min, info_[(size_t)i].max);
    }

    // These devices model a stereo pair. A chain that hands us more channels
    // than that gets the extras copied rather than dropped -- silence would be
    // a worse answer than "unprocessed", and the engine only ever calls with 2.
    static void copyExtra(const f32* const* in, f32* const* out, int first,
                          int channels, int nframes) {
        for (int c = first; c < channels; ++c) {
            if (!out[c]) continue;
            const f32* src = in ? in[c] : nullptr;
            if (src == out[c]) continue;
            if (src) std::memcpy(out[c], src, (size_t)nframes * sizeof(f32));
            else     std::memset(out[c], 0, (size_t)nframes * sizeof(f32));
        }
    }

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

// --- EQ Three --------------------------------------------------------------
// Low shelf, peaking mid, high shelf. RBJ cookbook sections, transposed direct
// form II, per-channel state.
//
// Three decisions worth stating:
//
//   * Coefficients are recomputed when a parameter MOVES, not per sample and
//     not per block: a cos, a sin and a sqrt per section is far too much to pay
//     48000 times a second for a knob nobody is touching. The cached values are
//     compared verbatim, so a static EQ costs three multiply chains and nothing
//     else.
//   * The resulting coefficient step is glided per sample over ~10 ms. Without
//     it, sweeping the mid frequency steps the whole section once per block and
//     you hear the block rate. Interpolating direct-form coefficients is only
//     safe for sections that stay well inside the stability triangle, which
//     these do: |a2| < 1 for every RBJ shelf/peak below Nyquist, and the
//     frequency is clamped to 0.495*sr before the trigonometry so a project
//     opened at 32 kHz cannot ask for a section at 18 kHz.
//   * TDF-II rather than DF-I because its state stays on the order of the
//     signal rather than of the filter's internal gain, which is what keeps a
//     +15 dB shelf from parking large numbers in memory between blocks.
//
// Defaults are flat: 0 dB on all three bands is bit-for-bit unity through the
// biquads (b0 = 1, everything else 0), so adding the device to a chain and
// playing changes nothing at all until a knob moves. That is the correct
// behaviour for the one device that ends up on every channel.
class Eq3 final : public InternalInstance {
public:
    explicit Eq3(const PluginDesc& d) : InternalInstance(d) {
        pLoF_  = addParam("Low Freq",  "Hz", 30.f,   500.f,   100.f,  true);
        pLoG_  = addParam("Low Gain",  "dB", -15.f,  15.f,    0.f);
        pMidF_ = addParam("Mid Freq",  "Hz", 200.f,  8000.f,  1000.f, true);
        pMidG_ = addParam("Mid Gain",  "dB", -15.f,  15.f,    0.f);
        pMidQ_ = addParam("Mid Q",     "",   0.3f,   6.f,     0.9f,   true);
        pHiF_  = addParam("High Freq", "Hz", 2000.f, 18000.f, 8000.f, true);
        pHiG_  = addParam("High Gain", "dB", -15.f,  15.f,    0.f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;
        glide_ = dsp::poleCoef(sr_, 0.01f);
        for (int c = 0; c < kCh; ++c) { loS_[c].reset(); midS_[c].reset(); hiS_[c].reset(); }
        recompute(true);                                // snap: no glide at load
        first_ = true;
        return true;
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (bypassed_ || !in) { passthrough(in, out, channels, nframes); return; }

        recompute(false);
        // The parameters a project restores are written after instantiate(), so
        // the first block is the one that establishes them rather than one that
        // glides towards them.
        if (first_) { lo_ = loT_; mid_ = midT_; hi_ = hiT_; first_ = false; }

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        for (int i = 0; i < nframes; ++i) {
            dsp::biquadGlide(lo_,  loT_,  glide_);
            dsp::biquadGlide(mid_, midT_, glide_);
            dsp::biquadGlide(hi_,  hiT_,  glide_);

            for (int c = 0; c < nc; ++c) {
                if (!dst[c]) continue;
                f32 x = src[c] ? src[c][i] : 0.f;
                x = dsp::biquadTick(lo_,  loS_[c],  x);
                x = dsp::biquadTick(mid_, midS_[c], x);
                x = dsp::biquadTick(hi_,  hiS_[c],  x);
                dst[c][i] = x;
            }
        }

        // Once per block: stop the glide from grinding on denormal residues,
        // and reset any section whose state has gone non-finite. The second is
        // belt and braces -- an RBJ section below Nyquist cannot blow up -- but
        // a NaN in a recursive filter is permanent, and the cost of the check
        // is three comparisons per block.
        dsp::biquadSettle(lo_,  loT_);
        dsp::biquadSettle(mid_, midT_);
        dsp::biquadSettle(hi_,  hiT_);
        for (int c = 0; c < kCh; ++c) { loS_[c].check(); midS_[c].check(); hiS_[c].check(); }

        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh = 2;

    void recompute(bool snap) {
        const f32 lf = p(pLoF_),  lg = p(pLoG_);
        const f32 mf = p(pMidF_), mg = p(pMidG_), mq = p(pMidQ_);
        const f32 hf = p(pHiF_),  hg = p(pHiG_);
        if (!snap && lf == cLf_ && lg == cLg_ && mf == cMf_ && mg == cMg_ &&
            mq == cMq_ && hf == cHf_ && hg == cHg_)
            return;
        cLf_ = lf; cLg_ = lg; cMf_ = mf; cMg_ = mg; cMq_ = mq; cHf_ = hf; cHg_ = hg;

        loT_  = dsp::rbj::lowShelf (sr_, lf, lg);
        midT_ = dsp::rbj::peaking  (sr_, mf, mg, mq);
        hiT_  = dsp::rbj::highShelf(sr_, hf, hg);
        if (snap) { lo_ = loT_; mid_ = midT_; hi_ = hiT_; }
    }

    int pLoF_ = 0, pLoG_ = 0, pMidF_ = 0, pMidG_ = 0, pMidQ_ = 0, pHiF_ = 0, pHiG_ = 0;

    dsp::BiquadCoeffs lo_, mid_, hi_;                  // gliding
    dsp::BiquadCoeffs loT_, midT_, hiT_;               // target
    dsp::BiquadState  loS_[kCh], midS_[kCh], hiS_[kCh];
    f32  glide_ = 1.f;
    bool first_ = true;

    // Last parameter values the coefficients were built from. NaN-initialised
    // through the sentinel below would be neater; -1 is enough, because no
    // parameter here can legally be negative except the gains, and those start
    // at 0 with a matching flat coefficient set from prepare().
    f32 cLf_ = -1.f, cLg_ = -1.f, cMf_ = -1.f, cMg_ = -1.f, cMq_ = -1.f,
        cHf_ = -1.f, cHg_ = -1.f;
};

// --- Compressor ------------------------------------------------------------
// Feed-forward, peak-detecting, stereo-linked, with the whole detector living
// in dB.
//
// Detection is PEAK, not RMS: this is the compressor you reach for on a drum
// bus or a vocal, where the thing you are trying to catch is a transient, and
// an RMS window with a musically useful time constant simply does not see it.
// An RMS mode would be a second detector and a mode switch, and the honest
// version of that device is a separate one (a bus/glue compressor) rather than
// a switch on this one.
//
// Stereo-linked: the detector sees max(|L|, |R|) and ONE gain is applied to
// both channels. Independent per-channel gains on a stereo bus pull the image
// towards whichever side is quieter every time a snare lands -- that is not a
// subtlety, it is the difference between a mix compressor and a broken one.
//
// Log-domain smoothing: the attack/release one-pole runs on the gain reduction
// in dB, not on the linear envelope. Two things follow. The attack time means
// the same thing whatever the ratio (a 10 ms attack reaches 63% of ITS OWN
// target GR in 10 ms, whether that target is 3 dB or 30), and the release is a
// constant number of dB per second rather than an exponential in linear gain,
// which is what "release" means to everyone who has ever used one.
//
// Defaults: -18 dB threshold, 3:1, 10 ms / 120 ms, 6 dB knee, no makeup. A
// signal below -18 dBFS peak passes through at exactly unity, so dropping the
// device on a channel and playing is inaudible until the material asks for it.
class Compressor final : public InternalInstance {
public:
    explicit Compressor(const PluginDesc& d) : InternalInstance(d) {
        pThresh_ = addParam("Threshold", "dB", -60.f, 0.f,    -18.f);
        pRatio_  = addParam("Ratio",     "",   1.f,   20.f,   3.f,   true);
        pAttack_ = addParam("Attack",    "ms", 0.1f,  100.f,  10.f,  true);
        pRelease_= addParam("Release",   "ms", 10.f,  1000.f, 120.f, true);
        pKnee_   = addParam("Knee",      "dB", 0.f,   24.f,   6.f);
        pMakeup_ = addParam("Makeup",    "dB", 0.f,   24.f,   0.f);
        // Output-only. See InternalInstance::setReadout for the wart.
        pGr_     = addParam("Gain Reduction", "dB", 0.f, 60.f, 0.f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;
        env_ = 0.f;
        makeup_.setTime(sr_, 0.02f);
        makeup_.snap(dbToGain(clampv(p(pMakeup_), 0.f, 24.f)));
        setReadout(pGr_, 0.f);
        first_ = true;
        return true;
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (bypassed_ || !in) { passthrough(in, out, channels, nframes); return; }

        const f32 thr   = clampv(p(pThresh_), -60.f, 0.f);
        const f32 ratio = clampv(p(pRatio_), 1.f, 20.f);
        const f32 knee  = clampv(p(pKnee_), 0.f, 24.f);
        const f32 slope = 1.f - 1.f / ratio;             // dB of GR per dB over
        const f32 att   = dsp::poleCoef(sr_, clampv(p(pAttack_), 0.1f, 100.f) * 1e-3f);
        const f32 rel   = dsp::poleCoef(sr_, clampv(p(pRelease_), 10.f, 1000.f) * 1e-3f);
        makeup_.set(dbToGain(clampv(p(pMakeup_), 0.f, 24.f)));
        if (first_) { makeup_.settle(); first_ = false; }

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        f32 peakGr = 0.f;
        for (int i = 0; i < nframes; ++i) {
            const f32 l = src[0] ? src[0][i] : 0.f;
            const f32 r = (nc > 1 && src[1]) ? src[1][i] : l;

            // Detector. The floor is -140 dB, which is below any signal that
            // could matter and keeps the log finite for digital silence.
            const f32 det  = std::fmax(std::fabs(l), std::fabs(r));
            const f32 lvl  = 20.f * std::log10(std::fmax(det, 1e-7f));
            const f32 over = lvl - thr;

            // Static curve with a quadratic soft knee centred on the threshold:
            // continuous in value AND in slope at both knee edges, which is
            // what stops a knee from sounding like a second, softer threshold.
            f32 target;
            if (knee > 0.f && over > -0.5f * knee && over < 0.5f * knee) {
                const f32 t = over + 0.5f * knee;
                target = slope * t * t / (2.f * knee);
            } else {
                target = over > 0.f ? slope * over : 0.f;
            }

            env_ += (target - env_) * (target > env_ ? att : rel);
            env_ = flushDenormal(env_);
            if (env_ > peakGr) peakGr = env_;

            const f32 g = dbToGain(-env_) * makeup_.next();
            for (int c = 0; c < nc; ++c) {
                if (!dst[c]) continue;
                dst[c][i] = (c == 0 ? l : r) * g;
            }
        }

        if (!dsp::sane(env_)) env_ = 0.f;
        // One value per block: the meter the UI draws is the worst reduction in
        // the block, which is the number an engineer actually wants to see.
        setReadout(pGr_, peakGr);

        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh = 2;

    int pThresh_ = 0, pRatio_ = 0, pAttack_ = 0, pRelease_ = 0, pKnee_ = 0,
        pMakeup_ = 0, pGr_ = 0;

    f32           env_ = 0.f;                            // current GR, dB, >= 0
    dsp::Smoother makeup_;
    bool          first_ = true;
};

// --- Delay -----------------------------------------------------------------
// Stereo delay with a filtered feedback path and a ping-pong mode.
//
// THE TEMPO SITUATION, in full, because it is the one thing here that is not
// self-contained:
//
// PluginInstance (host.h) carries audio, MIDI, parameters and latency. It does
// NOT carry transport: there is no tempo, no beat position and no time
// signature on the contract, and the engine's process loop calls
// fx->process(bufs, bufs, 2, n) with nothing else. The engine has the tempo
// (Engine::tempo_, and the atomic it publishes for the GUI), but src/plugin
// deliberately does not depend on src/audio, and the plugin layer lives in the
// daemon process where reaching for a UI-side session struct would be a lie
// anyway.
//
// So sync is implemented in full -- the division table, the beats-to-samples
// conversion, the switch -- against a TEMPO PARAMETER on the device, defaulting
// to 120 BPM. Everything except the source of the number is done. The wart is
// exactly one field wide: the user (or an automation lane) has to tell the
// device what the session tempo is, and nothing tells them when it drifts out
// of agreement with the transport.
//
// What host.h would need, stated so it can be done in one pass when a contract
// change is on the table:
//
//     // REALTIME. Called before this block's process() with the transport
//     // state the block will be rendered at. Default no-op.
//     virtual void setTransport(f64 bpm, f64 beat, bool playing) {}
//
// plus the three fx->process() call sites in engine.cpp calling it first, and
// LV2/CLAP forwarding it (LV2: a time:Position atom on the event input; CLAP:
// clap_event_transport_t in the process struct -- both formats already have a
// place for it, which is the strongest argument that the shape above is the
// right one). This device would then drop its Tempo parameter and read the
// transport instead. The division maths, which is the part that can be wrong,
// is already written and already tested.
//
// Feedback path: the delayed signal is lowpassed BEFORE it re-enters the line
// and not on the way out, so each repeat is one filter pass darker than the
// last -- the tape/BBD behaviour every musician expects -- while the first
// repeat is exactly what went in.
//
// Ping-pong sums the input to mono and injects it into the left line only; left
// feeds right, right feeds back into left. That gives strict alternation.
// Cross-feeding both lines from a stereo input, which is the other common
// implementation, gives two delays that lean rather than one that bounces.
class Delay final : public InternalInstance {
public:
    explicit Delay(const PluginDesc& d) : InternalInstance(d) {
        pSync_  = addBoolParam("Sync", true);
        pDiv_   = addIntParam ("Division", 0, kDivCount - 1, 3);   // 1/8
        pTime_  = addParam("Time",     "ms",  1.f,   2000.f,  350.f, true);
        pTempo_ = addParam("Tempo",    "BPM", 20.f,  999.f,   120.f);
        pFb_    = addParam("Feedback", "",    0.f,   0.95f,   0.35f);
        pTone_  = addParam("Tone",     "Hz",  200.f, 18000.f, 6000.f, true);
        pPing_  = addBoolParam("Ping Pong", false);
        pMix_   = addParam("Dry/Wet",  "",    0.f,   1.f,     0.3f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        // GUI thread: the only allocation in the device, and the size is fixed
        // for the lifetime of the instance. Four seconds is 1 bar at 60 BPM,
        // which is the longest musically meaningful division; slower sets clamp
        // (documented in the parameter range rather than silently wrapping).
        const int n = (int)(kMaxDelaySec * sr_) + 8;
        lineL_.resize(n);
        lineR_.resize(n);
        lineL_.reset();
        lineR_.reset();
        lpL_.reset();
        lpR_.reset();

        time_.setTime(sr_, 0.05f);      // 50 ms: a time change bends, not clicks
        fb_.setTime(sr_, 0.02f);
        mix_.setTime(sr_, 0.02f);
        fb_.snap(clampv(p(pFb_), 0.f, 0.95f));
        mix_.snap(clampv(p(pMix_), 0.f, 1.f));
        first_ = true;                  // snap the delay time on the first block
        return true;
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (bypassed_ || !in || lineL_.buf.empty()) { passthrough(in, out, channels, nframes); return; }

        const bool sync = p(pSync_) >= 0.5f;
        const bool ping = p(pPing_) >= 0.5f;

        f32 sec;
        if (sync) {
            const int div = (int)clampv(p(pDiv_) + 0.5f, 0.f, (f32)(kDivCount - 1));
            const f32 bpm = clampv(p(pTempo_), 20.f, 999.f);
            sec = kDivBeats[div] * 60.f / bpm;
        } else {
            sec = clampv(p(pTime_), 1.f, 2000.f) * 1e-3f;
        }
        const f32 maxD = (f32)lineL_.capacity() - 2.f;
        const f32 want = clampv(sec * (f32)sr_, 1.f, maxD);
        time_.set(want);
        fb_.set(clampv(p(pFb_), 0.f, 0.95f));
        mix_.set(clampv(p(pMix_), 0.f, 1.f));
        // First block after prepare: whatever the parameters say now IS the
        // starting state, so a restored 100%-dry delay is silent from sample
        // zero and a restored delay time is exact rather than bent into place.
        if (first_) { time_.settle(); fb_.settle(); mix_.settle(); first_ = false; }
        lpL_.setCutoff(sr_, clampv(p(pTone_), 200.f, 18000.f));
        lpR_.a = lpL_.a;

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        for (int i = 0; i < nframes; ++i) {
            const f32 xl = src[0] ? src[0][i] : 0.f;
            const f32 xr = (nc > 1 && src[1]) ? src[1][i] : xl;

            const f32 d   = time_.next();
            const f32 dl  = lineL_.tapLerp(d);
            const f32 dr  = lineR_.tapLerp(d);
            const f32 fb  = fb_.next();
            const f32 mix = mix_.next();

            const f32 fl = lpL_.process(dl);
            const f32 fr = lpR_.process(dr);

            f32 wl, wr;
            if (ping) {
                const f32 mono = 0.5f * (xl + xr);
                wl = mono + fr * fb;
                wr = fl * fb;
            } else {
                wl = xl + fl * fb;
                wr = xr + fr * fb;
            }
            // The clamp is a guard, not a sound: feedback is capped at 0.95 and
            // the tone filter has unity DC gain, so the line's steady state is
            // bounded by 20x the input and cannot reach this. It exists so that
            // a denormal-flushed NaN arriving from upstream cannot become
            // permanent state in a feedback loop.
            lineL_.push(flushDenormal(clampv(wl, -32.f, 32.f)));
            lineR_.push(flushDenormal(clampv(wr, -32.f, 32.f)));

            if (dst[0]) dst[0][i] = xl * (1.f - mix) + dl * mix;
            if (nc > 1 && dst[1]) dst[1][i] = xr * (1.f - mix) + dr * mix;
        }

        lpL_.check();
        lpR_.check();
        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh = 2;
    static constexpr f64 kMaxDelaySec = 4.0;

    int pSync_ = 0, pDiv_ = 0, pTime_ = 0, pTempo_ = 0, pFb_ = 0, pTone_ = 0,
        pPing_ = 0, pMix_ = 0;

    dsp::DelayLine lineL_, lineR_;
    dsp::OnePole   lpL_, lpR_;
    dsp::Smoother  time_, fb_, mix_;
    bool           first_ = true;
};

// --- Reverb ----------------------------------------------------------------
// A Dattorro/Griesinger plate: input diffusion into a figure-of-eight tank,
// output taken as a fistful of decorrelated taps from inside the tank.
//
// Structure (Dattorro, "Effect Design Part 1: Reverberator and Other
// Filters", JAES 1997), with the delay lengths scaled from the paper's
// 29761 Hz reference to whatever rate we were prepared at:
//
//   in -> mono -> pre-delay -> bandwidth LP -> 4 diffusion allpasses -> x
//   left  half: (x + right output) -> modulated AP -> delay -> damping ->
//               decay -> AP -> delay -> right half
//   right half: (x + left output)  -> modulated AP -> delay -> damping ->
//               decay -> AP -> delay -> left half
//
// Decisions:
//
//   * Decay is a TIME, in seconds, not a dimensionless feedback number. The two
//     per-loop multipliers are solved for the requested RT60 from the actual
//     loop length at the actual sample rate: g = 10^(-1.5*loop/(sr*rt60)), so
//     g^2 per loop is exactly -60 dB after rt60 seconds. A "decay" knob from 0
//     to 1 means nothing to a user and cannot be tested; a knob in seconds is
//     both a promise and a measurement.
//   * The tank's first allpass in each half is modulated by a slow quadrature
//     LFO (~0.7 and ~1.1 Hz, a few samples of excursion). This is what stops
//     the tank's modes from ringing on a fixed set of frequencies -- an
//     unmodulated plate on a sustained note sounds metallic within a second.
//   * The input is summed to mono before the tank, which is what a plate is:
//     one driver, one sheet. Stereo comes from taking the output taps in two
//     different sets, and the Width control collapses those towards mono
//     without collapsing the reverb itself.
//   * Damping is a one-pole lowpass inside each half of the tank, so it
//     compounds once per pass: high frequencies decay faster than low ones,
//     which is the single largest difference between a reverb that sounds like
//     a room and one that sounds like a delay network.
//
// Defaults: 20 ms pre-delay, 2 s decay, 6 kHz damping, 25% wet. Audible,
// flattering, and nowhere near loud enough to be destructive on a first play.
class Reverb final : public InternalInstance {
public:
    explicit Reverb(const PluginDesc& d) : InternalInstance(d) {
        pPre_   = addParam("Pre-Delay", "ms", 0.f,   250.f,   20.f);
        pDecay_ = addParam("Decay",     "s",  0.2f,  12.f,    2.f,    true);
        pDamp_  = addParam("Damping",   "Hz", 500.f, 18000.f, 6000.f, true);
        pDiff_  = addParam("Diffusion", "",   0.f,   1.f,     0.7f);
        pWidth_ = addParam("Width",     "",   0.f,   1.f,     1.f);
        pMix_   = addParam("Dry/Wet",   "",   0.f,   1.f,     0.25f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        const f64 k = sr_ / 29761.0;                  // the paper's rate
        auto S = [k](int n) { const int v = (int)(n * k + 0.5); return v < 1 ? 1 : v; };

        for (int i = 0; i < 4; ++i) {
            lenDif_[i] = S(kRefDif[i]);
            dif_[i].resize(lenDif_[i] + 4);
        }
        lenApA_ = S(672);  lenDlA_ = S(4453); lenApB_ = S(1800); lenDlB_ = S(3720);
        lenApC_ = S(908);  lenDlC_ = S(4217); lenApD_ = S(2656); lenDlD_ = S(3163);

        // Modulation excursion, in samples, never more than a quarter of the
        // allpass it modulates (the fractional read must stay above 1).
        mod_ = (f32)S(8);
        mod_ = std::fmin(mod_, (f32)(lenApA_ < lenApC_ ? lenApA_ : lenApC_) * 0.25f);

        apA_.resize(lenApA_ + (int)mod_ + 8);
        apC_.resize(lenApC_ + (int)mod_ + 8);
        apB_.resize(lenApB_ + 4);
        apD_.resize(lenApD_ + 4);
        dlA_.resize(lenDlA_ + 4);
        dlB_.resize(lenDlB_ + 4);
        dlC_.resize(lenDlC_ + 4);
        dlD_.resize(lenDlD_ + 4);
        pre_.resize((int)(0.25 * sr_) + 8);

        // Output taps. The positions are the paper's, scaled, and clamped into
        // the line they are read from -- a tap is only ever "how long ago", so
        // any position the buffer can hold is a legitimate read.
        for (int i = 0; i < kTaps; ++i) {
            tapL_[i] = clampTap(S(kRefTapL[i]), tapLineL(i));
            tapR_[i] = clampTap(S(kRefTapR[i]), tapLineR(i));
        }

        loop_ = (f32)(lenApA_ + lenDlA_ + lenApB_ + lenDlB_ +
                      lenApC_ + lenDlC_ + lenApD_ + lenDlD_);

        // Quadrature LFOs: a rotating unit vector each, one multiply-add per
        // sample and no std::sin in the loop.
        lfoKA_ = (f32)(dsp::kTwoPi * 0.70 / sr_);
        lfoKC_ = (f32)(dsp::kTwoPi * 1.13 / sr_);
        sA_ = 0.f; cA_ = 1.f; sC_ = 0.f; cC_ = 1.f;

        inLp_.setCutoff(sr_, (f32)std::fmin(16000.0, sr_ * 0.45));
        preT_.setTime(sr_, 0.08f);
        preT_.snap(clampv(p(pPre_), 0.f, 250.f) * 1e-3f * (f32)sr_);
        width_.setTime(sr_, 0.02f);
        width_.snap(clampv(p(pWidth_), 0.f, 1.f));
        mix_.setTime(sr_, 0.02f);
        mix_.snap(clampv(p(pMix_), 0.f, 1.f));
        decay_.setTime(sr_, 0.1f);
        decay_.snap(decayGain());
        first_ = true;

        clear();
        return true;
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        // Unlike the other effects, "no input" is not "nothing to do": the tank
        // still has to ring out. A null input block is processed as silence.
        if (bypassed_ || pre_.buf.empty()) { passthrough(in, out, channels, nframes); return; }

        dampA_.setCutoff(sr_, clampv(p(pDamp_), 500.f, 18000.f));
        dampC_.a = dampA_.a;
        preT_.set(clampv(p(pPre_), 0.f, 250.f) * 1e-3f * (f32)sr_);
        width_.set(clampv(p(pWidth_), 0.f, 1.f));
        mix_.set(clampv(p(pMix_), 0.f, 1.f));
        decay_.set(decayGain());
        if (first_) {
            preT_.settle(); width_.settle(); mix_.settle(); decay_.settle();
            first_ = false;
        }

        const f32 dif = clampv(p(pDiff_), 0.f, 1.f);
        const f32 g1 = 0.75f * (0.35f + 0.65f * dif);
        const f32 g2 = 0.625f * (0.35f + 0.65f * dif);

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in ? in[c] : nullptr; dst[c] = out[c]; }

        for (int i = 0; i < nframes; ++i) {
            const f32 xl = src[0] ? src[0][i] : 0.f;
            const f32 xr = (nc > 1 && src[1]) ? src[1][i] : xl;

            // --- input chain
            const f32 mono = 0.5f * (xl + xr);
            const f32 xp = pre_.tapLerp(preT_.next());
            pre_.push(flushDenormal(mono));

            f32 v = inLp_.process(xp);
            v = dsp::allpassTick(dif_[0], (f32)lenDif_[0], g1, v);
            v = dsp::allpassTick(dif_[1], (f32)lenDif_[1], g1, v);
            v = dsp::allpassTick(dif_[2], (f32)lenDif_[2], g2, v);
            v = dsp::allpassTick(dif_[3], (f32)lenDif_[3], g2, v);

            // --- LFOs
            sA_ += lfoKA_ * cA_; cA_ -= lfoKA_ * sA_;
            sC_ += lfoKC_ * cC_; cC_ -= lfoKC_ * sC_;

            const f32 dg = decay_.next();

            // --- left half: fed by the right half's output from last sample.
            f32 a = dsp::allpassTick(apA_, (f32)lenApA_ + mod_ * sA_, kTankAp1, v + fromD_);
            f32 t = dlA_.tap(lenDlA_);
            dlA_.push(flushDenormal(a));
            t = dampA_.process(t) * dg;
            t = dsp::allpassTick(apB_, (f32)lenApB_, kTankAp2, t);
            const f32 outB = dlB_.tap(lenDlB_);
            dlB_.push(flushDenormal(t));

            // --- right half: fed by the left half's output from this sample.
            // The loop still has thousands of samples of delay in it, so there
            // is no delay-free path; taking the fresher value simply makes the
            // figure-of-eight one sample tighter.
            f32 b = dsp::allpassTick(apC_, (f32)lenApC_ + mod_ * sC_, kTankAp1, v + outB);
            f32 u = dlC_.tap(lenDlC_);
            dlC_.push(flushDenormal(b));
            u = dampC_.process(u) * dg;
            u = dsp::allpassTick(apD_, (f32)lenApD_, kTankAp2, u);
            const f32 outD = dlD_.tap(lenDlD_);
            dlD_.push(flushDenormal(u));
            fromD_ = flushDenormal(outD);

            // --- output taps: six per side, alternating signs, read from the
            // opposite half's lines wherever possible so the two outputs share
            // as little as possible.
            const f32 yl = kTapGain * ( dlC_.tap(tapL_[0]) + dlC_.tap(tapL_[1])
                                      - dlD_.tap(tapL_[2]) + dlA_.tap(tapL_[3])
                                      - dlB_.tap(tapL_[4]) - dlB_.tap(tapL_[5]) );
            const f32 yr = kTapGain * ( dlA_.tap(tapR_[0]) + dlA_.tap(tapR_[1])
                                      - dlB_.tap(tapR_[2]) + dlC_.tap(tapR_[3])
                                      - dlD_.tap(tapR_[4]) - dlD_.tap(tapR_[5]) );

            const f32 w   = width_.next();
            const f32 mid = 0.5f * (yl + yr);
            const f32 sid = 0.5f * (yl - yr) * w;
            const f32 mix = mix_.next();

            if (dst[0]) dst[0][i] = xl * (1.f - mix) + (mid + sid) * mix;
            if (nc > 1 && dst[1]) dst[1][i] = xr * (1.f - mix) + (mid - sid) * mix;
        }

        // Renormalise the LFO vectors (they drift by O(k^2) per sample) and
        // check the recursive state. A tank that has gone non-finite never
        // recovers on its own, so the check is the difference between one bad
        // block and a dead device.
        renorm(sA_, cA_);
        renorm(sC_, cC_);
        inLp_.check(); dampA_.check(); dampC_.check();
        if (!dsp::sane(fromD_)) { fromD_ = 0.f; clear(); }

        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh   = 2;
    static constexpr int kTaps = 6;
    static constexpr f32 kTankAp1 = 0.7f;    // "decay diffusion 1"
    static constexpr f32 kTankAp2 = 0.5f;    // "decay diffusion 2"
    // Six uncorrelated taps sum like a random walk, so ~1/sqrt(6) keeps a wet
    // signal in the same neighbourhood as the dry one. Measured, not guessed:
    // see the level check in tests/internal_device_test.cpp.
    static constexpr f32 kTapGain = 0.6f;

    static constexpr int kRefDif[4]  = { 142, 107, 379, 277 };
    static constexpr int kRefTapL[6] = { 266, 2974, 1913, 1990, 187, 1066 };
    static constexpr int kRefTapR[6] = { 353, 3627, 1228, 2673, 335, 2111 };

    const dsp::DelayLine& tapLineL(int i) const {
        return (i < 2) ? dlC_ : (i == 2 ? dlD_ : (i == 3 ? dlA_ : dlB_));
    }
    const dsp::DelayLine& tapLineR(int i) const {
        return (i < 2) ? dlA_ : (i == 2 ? dlB_ : (i == 3 ? dlC_ : dlD_));
    }
    static int clampTap(int t, const dsp::DelayLine& l) {
        const int m = l.capacity();
        return t < 1 ? 1 : (t > m ? m : t);
    }

    // Solve the two per-loop multipliers for the requested RT60.
    f32 decayGain() const {
        const f32 rt = clampv(p(pDecay_), 0.2f, 12.f);
        const f32 loops = loop_ / (f32)(sr_ * rt);      // loop traversals per RT60
        return clampv(std::pow(10.f, -1.5f * loops), 0.f, 0.98f);
    }

    static void renorm(f32& s, f32& c) {
        const f32 n = s * s + c * c;
        if (n > 1e-6f && n < 4.f) { const f32 k = 1.5f - 0.5f * n; s *= k; c *= k; }
        else { s = 0.f; c = 1.f; }
    }

    void clear() {
        pre_.reset();
        for (auto& d : dif_) d.reset();
        apA_.reset(); apB_.reset(); apC_.reset(); apD_.reset();
        dlA_.reset(); dlB_.reset(); dlC_.reset(); dlD_.reset();
        inLp_.reset(); dampA_.reset(); dampC_.reset();
        fromD_ = 0.f;
    }

    int pPre_ = 0, pDecay_ = 0, pDamp_ = 0, pDiff_ = 0, pWidth_ = 0, pMix_ = 0;

    dsp::DelayLine pre_, dif_[4];
    dsp::DelayLine apA_, dlA_, apB_, dlB_;      // left half
    dsp::DelayLine apC_, dlC_, apD_, dlD_;      // right half
    dsp::OnePole   inLp_, dampA_, dampC_;
    dsp::Smoother  preT_, width_, mix_, decay_;

    int lenDif_[4] = { 1, 1, 1, 1 };
    int lenApA_ = 1, lenDlA_ = 1, lenApB_ = 1, lenDlB_ = 1;
    int lenApC_ = 1, lenDlC_ = 1, lenApD_ = 1, lenDlD_ = 1;
    int tapL_[kTaps] = { 1, 1, 1, 1, 1, 1 };
    int tapR_[kTaps] = { 1, 1, 1, 1, 1, 1 };
    f32 mod_ = 0.f, loop_ = 1.f;
    f32 lfoKA_ = 0.f, lfoKC_ = 0.f, sA_ = 0.f, cA_ = 1.f, sC_ = 0.f, cC_ = 1.f;
    f32 fromD_ = 0.f;
    bool first_ = true;
};

// --- Auto Filter -----------------------------------------------------------
// Resonant multimode filter (12 dB/oct lowpass, bandpass or highpass) with an
// LFO and an envelope follower on its cutoff.
//
// THE TEMPO SITUATION AGAIN, and the same answer as the Delay's, on purpose:
// PluginInstance carries no transport, so a tempo-synced LFO cannot know the
// session's BPM. The division table, the beats-to-hertz conversion and the Sync
// switch are all implemented; the number they multiply comes from a `Tempo`
// PARAMETER defaulting to 120, exactly as the Delay's does, and the user (or an
// automation lane) has to keep it in agreement with the transport. Everything
// except the source of that one number is done.
//
// The contract addition that would retire the wart on both devices at once is
// written out in full in the Delay's header, and adding it is a change to
// host.h and to engine.cpp's process loop rather than to this file.
//
// Three decisions worth stating:
//
//   * TPT state-variable filter, not a biquad. The cutoff moves continuously —
//     that is the device — and a direct-form section swept fast can leave the
//     stability triangle between two samples because its state stops meaning
//     anything once the coefficients have moved. The TPT form's state IS the
//     two integrator outputs, which survive a coefficient change. See
//     internal_dsp.h.
//   * The modulators run at CONTROL RATE (kCtrl = 16 samples, 0.33 ms at
//     48 kHz) and the coefficients are ramped linearly to the new target over
//     the following 16 samples. An LFO at its fastest (20 Hz over 4 octaves)
//     moves the cutoff by 0.03 octaves in one tick, which no ear resolves, and
//     the ramp is what stops that step from being a step. The alternative — a
//     tan() and an exp2() per sample per channel — buys nothing audible for
//     roughly ten times the cost.
//   * The envelope follower is stereo-LINKED and reads the peak, for the same
//     reason the Compressor's detector does: two independent followers pull the
//     stereo image around every time one side is louder.
//
// Defaults: lowpass, cutoff at the top of its range, no LFO and no envelope. A
// filter that reshapes the sound the moment it is dropped on a channel is a
// filter people delete; this one is a wire until a knob moves, like EQ Three.
class AutoFilter final : public InternalInstance {
public:
    explicit AutoFilter(const PluginDesc& d) : InternalInstance(d) {
        pType_   = addIntParam ("Type", 0, 2, 0);            // 0 LP, 1 BP, 2 HP
        pCut_    = addParam("Cutoff",       "Hz",  20.f,  18000.f, 18000.f, true);
        pRes_    = addParam("Resonance",    "",    0.f,   1.f,     0.1f);
        pLfoAmt_ = addParam("LFO Amount",   "oct", 0.f,   4.f,     0.f);
        pRate_   = addParam("LFO Rate",     "Hz",  0.01f, 20.f,    1.f,     true);
        pSync_   = addBoolParam("LFO Sync", true);
        pDiv_    = addIntParam ("LFO Division", 0, kDivCount - 1, 5);   // 1/4
        pTempo_  = addParam("Tempo",        "BPM", 20.f,  999.f,   120.f);
        pShape_  = addIntParam ("LFO Shape", 0, 3, 0);       // sine/tri/saw/square
        pPhase_  = addParam("LFO Phase",    "deg", 0.f,   180.f,   0.f);
        pEnvAmt_ = addParam("Env Amount",   "oct", -4.f,  4.f,     0.f);
        pEnvA_   = addParam("Env Attack",   "ms",  0.1f,  100.f,   5.f,     true);
        pEnvR_   = addParam("Env Release",  "ms",  1.f,   1000.f,  100.f,   true);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;
        for (int c = 0; c < kCh; ++c) sv_[c].reset();
        lfo_.reset();
        env_ = 0.f;
        envNorm_ = 0.f;
        ctrl_ = 0;
        first_ = true;                  // snap the coefficients on the first block
        return true;                    // recursive but causal: zero latency
    }

    // REALTIME. No per-block buffer of any kind, so an nframes larger than the
    // one we were prepared for is processed rather than degraded -- the control
    // loop below is written in terms of nframes alone.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (bypassed_ || !in) { passthrough(in, out, channels, nframes); return; }

        const int type  = (int)clampv(p(pType_) + 0.5f, 0.f, 2.f);
        const int shape = (int)clampv(p(pShape_) + 0.5f, 0.f, 3.f);
        const f32 base  = clampv(p(pCut_), 20.f, 18000.f);
        // Resonance 0..1 -> Q 0.5..20, geometrically: a linear map spends most
        // of its travel below the point where resonance becomes audible.
        const f32 q     = 0.5f * std::pow(40.f, clampv(p(pRes_), 0.f, 1.f));
        const f32 lfoA  = clampv(p(pLfoAmt_), 0.f, 4.f);
        const f32 envA  = clampv(p(pEnvAmt_), -4.f, 4.f);
        const f32 phOff = clampv(p(pPhase_), 0.f, 180.f) * (1.f / 360.f);

        if (p(pSync_) >= 0.5f) {
            const int div = (int)clampv(p(pDiv_) + 0.5f, 0.f, (f32)(kDivCount - 1));
            const f32 bpm = clampv(p(pTempo_), 20.f, 999.f);
            // One cycle per division: a 1/4 at 120 BPM is 0.5 s, i.e. 2 Hz.
            lfo_.setRate(sr_, bpm / (60.f * kDivBeats[div]));
        } else {
            lfo_.setRate(sr_, clampv(p(pRate_), 0.01f, 20.f));
        }

        const f32 att   = dsp::poleCoef(sr_, clampv(p(pEnvA_), 0.1f, 100.f) * 1e-3f);
        const f32 rel   = dsp::poleCoef(sr_, clampv(p(pEnvR_), 1.f, 1000.f) * 1e-3f);
        const f32 fcMax = (f32)(sr_ * 0.45);

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        int i = 0;
        while (i < nframes) {
            // --- control tick: where the modulation becomes a coefficient set.
            //
            // `ctrl_` counts down across process() calls rather than restarting
            // at every block boundary, so a tick lands every 16 samples of
            // ABSOLUTE time whatever block sizes the host hands us. That is what
            // makes the device block-size invariant -- at a fixed parameter
            // setting, the same audio in blocks of 1, of 256 and of 300 comes
            // out bit-identical -- and it is not a nicety: a render at one block
            // size and the same render at another must be the same file.
            //
            // (Parameters themselves are still read once per block, like every
            // other device here, so a knob MOVING lands on a block boundary.
            // That is the one block-size dependence the whole file shares, and
            // it is the same one every plugin format has.)
            if (ctrl_ <= 0) {
                // The log lives here rather than in the sample loop: the cutoff
                // is only rebuilt at the tick, so a per-sample dB conversion
                // would be sixteen logarithms for one number that gets used.
                envNorm_ = clampv((20.f * std::log10(std::fmax(env_, 1e-7f)) + 60.f) * (1.f / 60.f),
                                  0.f, 1.f);
                const f32 eo = envA * envNorm_;

                // The two channels differ only by the LFO's stereo phase offset,
                // so at phase 0 they are bit-identical and the device is exactly
                // mono-compatible -- which is the property a stereo LFO has to
                // be able to give back.
                dsp::SvfCoeffs tgt[kCh];
                for (int c = 0; c < kCh; ++c) {
                    const f32 m = lfoA * dsp::Lfo::shape(shape, lfo_.phase + (c ? phOff : 0.f));
                    tgt[c] = dsp::svfCoeffs(sr_, clampv(base * std::exp2(m + eo), 20.f, fcMax), q);
                }
                // The first block after prepare() establishes the coefficients
                // rather than gliding towards them, for the reason
                // Smoother::settle gives: a project load writes every parameter
                // AFTER instantiate().
                if (first_) { cur_[0] = tgt[0]; cur_[1] = tgt[1]; first_ = false; }

                const f32 invk = 1.f / (f32)kCtrl;
                for (int c = 0; c < kCh; ++c) inc_[c] = dsp::svfSlope(cur_[c], tgt[c], invk);
                ctrl_ = kCtrl;
            }

            const int k = (nframes - i) < ctrl_ ? (nframes - i) : ctrl_;
            for (int j = 0; j < k; ++j) {
                const int n = i + j;
                const f32 l = src[0] ? src[0][n] : 0.f;
                const f32 r = (nc > 1 && src[1]) ? src[1][n] : l;

                // Envelope follower: stereo-linked peak through a one-pole with
                // separate attack and release. Per sample, because the whole
                // point of a follower is to catch a transient.
                const f32 det = std::fmax(std::fabs(l), std::fabs(r));
                env_ = flushDenormal(env_ + (det - env_) * (det > env_ ? att : rel));

                for (int c = 0; c < nc; ++c) {
                    if (!dst[c]) continue;
                    const dsp::SvfOut o = dsp::svfTick(cur_[c], sv_[c], c == 0 ? l : r);
                    // The bandpass is normalised by k = 1/Q so its peak stays at
                    // unity: an SVF's raw bandpass tap has a peak gain of Q, and
                    // a band filter that gets 26 dB louder as the resonance knob
                    // turns is a hazard rather than a feature. The lowpass and
                    // highpass keep their resonant peak, which is the whole
                    // point of the knob on those.
                    dst[c][n] = type == 0 ? o.lp : (type == 1 ? o.bp * cur_[c].k : o.hp);
                }
                for (int c = 0; c < kCh; ++c) dsp::svfStep(cur_[c], inc_[c]);
                lfo_.tick();
            }

            ctrl_ -= k;
            i += k;
        }

        for (int c = 0; c < kCh; ++c) sv_[c].check();
        if (!dsp::sane(env_)) { env_ = 0.f; envNorm_ = 0.f; }
        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh   = 2;
    // 16 samples: 0.33 ms at 48 kHz, 0.36 ms at 44.1. Small enough that the
    // fastest modulation this device can produce moves the cutoff by a
    // hundredth of an octave between ticks.
    static constexpr int kCtrl = 16;

    int pType_ = 0, pCut_ = 0, pRes_ = 0, pLfoAmt_ = 0, pRate_ = 0, pSync_ = 0,
        pDiv_ = 0, pTempo_ = 0, pShape_ = 0, pPhase_ = 0, pEnvAmt_ = 0,
        pEnvA_ = 0, pEnvR_ = 0;

    dsp::SvfCoeffs cur_[kCh], inc_[kCh];
    dsp::SvfState  sv_[kCh];
    dsp::Lfo       lfo_;
    f32  env_ = 0.f;                   // linear peak follower
    f32  envNorm_ = 0.f;               // that peak as 0..1 over the top 60 dB
    int  ctrl_ = 0;                    // samples until the next control tick
    bool first_ = true;
};

// --- Chorus ----------------------------------------------------------------
// Modulated multi-tap delay: up to four voices per channel, each reading the
// same line at a different point on one LFO's cycle.
//
// Decisions:
//
//   * ONE oscillator, read at N angles. Every voice is the same rotating unit
//     vector (dsp::Quad) read at a fixed offset, because sin(t + a) is
//     s*cos a + c*sin a and the offsets are constants for the block. N
//     independent LFOs would cost N sines per sample and would drift apart at
//     the same time -- the offsets are the whole point of an ensemble.
//   * The two channels are in QUADRATURE, always: right reads the same
//     oscillator a quarter cycle ahead of left. That fixed offset, not a
//     parameter, is what makes the device stereo out of a mono source.
//   * Width is therefore a mid/side control on the WET signal, per sample and
//     smoothed, rather than a phase offset. A phase offset can only change at
//     block boundaries -- it is baked into the per-voice constants -- and a
//     delay time that steps once per block is a click.
//   * Feedback is taken from the summed wet signal and clamped before it
//     re-enters the line, so a NaN arriving from upstream cannot become
//     permanent state in a loop. Negative feedback is allowed and is what turns
//     a slow chorus into a flanger.
//
// Defaults: 3 voices, 12 ms, 3 ms of depth at 0.6 Hz, no feedback, 50% wet.
// Audible on the first play, which is the point of the device -- unlike a
// filter or an EQ, a chorus that does nothing by default is just latency.
class Chorus final : public InternalInstance {
public:
    explicit Chorus(const PluginDesc& d) : InternalInstance(d) {
        pRate_   = addParam("Rate",     "Hz",  0.01f, 10.f, 0.6f, true);
        pDepth_  = addParam("Depth",    "ms",  0.f,   10.f, 3.f);
        pDelay_  = addParam("Delay",    "ms",  1.f,   30.f, 12.f);
        pVoices_ = addIntParam("Voices", 1, kMaxVoices, 3);
        pFb_     = addParam("Feedback", "",   -0.9f,  0.9f, 0.f);
        pWidth_  = addParam("Width",    "",    0.f,   1.f,  1.f);
        pMix_    = addParam("Dry/Wet",  "",    0.f,   1.f,  0.5f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        // GUI thread: the only allocation in the device. 50 ms covers the
        // longest tap the parameters can ask for (30 ms delay + 10 ms depth)
        // with room for the interpolator to read past it.
        const int n = (int)(kMaxSec * sr_) + 8;
        lineL_.resize(n);
        lineR_.resize(n);
        lineL_.reset();
        lineR_.reset();

        delay_.setTime(sr_, 0.05f);     // a time change bends, it does not click
        depth_.setTime(sr_, 0.05f);
        fb_.setTime(sr_, 0.02f);
        width_.setTime(sr_, 0.02f);
        mix_.setTime(sr_, 0.02f);
        lfo_.reset();
        voices_ = 0;                    // forces the angle table to be rebuilt
        renorm_ = 0;
        first_ = true;
        return true;
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (bypassed_ || !in || lineL_.buf.empty()) { passthrough(in, out, channels, nframes); return; }

        const int v = (int)clampv(p(pVoices_) + 0.5f, 1.f, (f32)kMaxVoices);
        if (v != voices_) setVoices(v);

        lfo_.setRate(sr_, clampv(p(pRate_), 0.01f, 10.f));
        delay_.set(clampv(p(pDelay_), 1.f, 30.f) * 1e-3f * (f32)sr_);
        depth_.set(clampv(p(pDepth_), 0.f, 10.f) * 1e-3f * (f32)sr_);
        fb_.set(clampv(p(pFb_), -0.9f, 0.9f));
        width_.set(clampv(p(pWidth_), 0.f, 1.f));
        mix_.set(clampv(p(pMix_), 0.f, 1.f));
        if (first_) {
            delay_.settle(); depth_.settle(); fb_.settle(); width_.settle(); mix_.settle();
            first_ = false;
        }

        const f32 maxD = (f32)lineL_.capacity() - 2.f;
        const f32 inv  = 1.f / (f32)voices_;

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        for (int i = 0; i < nframes; ++i) {
            const f32 xl = src[0] ? src[0][i] : 0.f;
            const f32 xr = (nc > 1 && src[1]) ? src[1][i] : xl;

            // Renormalising the oscillator on a fixed sample period rather than
            // once per block is what keeps the device block-size invariant: the
            // correction changes the vector slightly, so applying it at buffer
            // boundaries would make the output depend on the buffer size.
            if (renorm_ <= 0) { lfo_.renorm(); renorm_ = kRenorm; }
            --renorm_;

            lfo_.tick();
            const f32 s = lfo_.s, c = lfo_.c;
            const f32 d0 = delay_.next();
            const f32 dp = depth_.next() * 0.5f;

            // Read then push, which is what makes the tap an exact delay.
            f32 wl = 0.f, wr = 0.f;
            for (int k = 0; k < voices_; ++k) {
                const f32 ml = s * cosA_[k] + c * sinA_[k];   // sin(t + a)
                const f32 mr = c * cosA_[k] - s * sinA_[k];   // sin(t + a + 90 deg)
                wl += lineL_.tapLerp(clampv(d0 + dp * (1.f + ml), 1.f, maxD));
                wr += lineR_.tapLerp(clampv(d0 + dp * (1.f + mr), 1.f, maxD));
            }
            wl *= inv;
            wr *= inv;

            const f32 fb = fb_.next();
            lineL_.push(flushDenormal(clampv(xl + wl * fb, -32.f, 32.f)));
            lineR_.push(flushDenormal(clampv(xr + wr * fb, -32.f, 32.f)));

            const f32 w   = width_.next();
            const f32 mid = 0.5f * (wl + wr);
            const f32 sid = 0.5f * (wl - wr) * w;
            const f32 mix = mix_.next();

            if (dst[0]) dst[0][i] = xl * (1.f - mix) + (mid + sid) * mix;
            if (nc > 1 && dst[1]) dst[1][i] = xr * (1.f - mix) + (mid - sid) * mix;
        }

        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh        = 2;
    static constexpr int kMaxVoices = 4;
    static constexpr f64 kMaxSec    = 0.05;
    // The quadrature vector drifts by O(k^2) per sample; at the fastest rate
    // this device allows that is a part in 10^7 per sample, so 1024 samples
    // between corrections is three orders of magnitude of headroom.
    static constexpr int kRenorm    = 1024;

    // Voice angles, evenly spaced around the cycle. Rebuilt only when the voice
    // count changes: they are constants of the topology, not of the signal.
    void setVoices(int v) {
        voices_ = clampv(v, 1, kMaxVoices);
        for (int k = 0; k < voices_; ++k) {
            const f32 a = dsp::kTwoPi * (f32)k / (f32)voices_;
            cosA_[k] = std::cos(a);
            sinA_[k] = std::sin(a);
        }
    }

    int pRate_ = 0, pDepth_ = 0, pDelay_ = 0, pVoices_ = 0, pFb_ = 0, pWidth_ = 0,
        pMix_ = 0;

    dsp::DelayLine lineL_, lineR_;
    dsp::Quad      lfo_;
    dsp::Smoother  delay_, depth_, fb_, width_, mix_;
    f32  cosA_[kMaxVoices] = { 1.f, 1.f, 1.f, 1.f };
    f32  sinA_[kMaxVoices] = { 0.f, 0.f, 0.f, 0.f };
    int  voices_ = 0;
    int  renorm_ = 0;
    bool first_ = true;
};

// --- Limiter ---------------------------------------------------------------
// Lookahead brickwall limiter. The one internal device with latency, and the
// reason the base class's latencyFrames() comment names an exception.
//
// THE LATENCY, first, because it is the part the rest of the program depends
// on. The lookahead is five milliseconds, FIXED AT PREPARE TIME -- 240 frames
// at 48 kHz, 221 at 44.1 -- and latencyFrames() reports exactly that. It is not
// a parameter and must never become one:
//
//   * host.h says latency is constant after prepare();
//   * engine.cpp reads it ONCE, when a chain is published, and caches it beside
//     the pointer (see docs/RACKS.md §1);
//   * so a knob that changed the figure would leave every parallel path in the
//     project compensated by a number that is no longer true, silently, with no
//     way for the engine to find out.
//
// A device that under-reports smears the whole set; a device that over-reports
// does the same in the other direction. The number here is not an estimate --
// the output IS the input delayed by exactly `look_` samples (the test measures
// it by impulse), so the engine's compensation is exact rather than close.
//
// THE GAIN COMPUTER, and why this shape is brickwall rather than nearly:
//
//   1. level[n]   = 20*log10(peak of the stereo pair, after input gain)
//   2. rel[n]     = max(level[n], rel[n-1] released towards it)   -- release
//   3. m[n]       = max over rel[n-L .. n]                        -- lookahead
//   4. a[n]       = mean of m[n-L .. n]                           -- smoothing
//   5. gain[n]    = 10^(-max(0, a[n] - ceiling)/20), applied to input[n-L]
//
// Each stage is monotone upward in the level it reports, and the proof that the
// output cannot exceed the ceiling is three lines:
//   * rel[n] >= level[n], because step 2 only ever holds the level UP;
//   * every term of the mean in step 4 is m[n-j] for j <= L, and each of those
//     is a maximum over a window that CONTAINS index n-L (that is exactly what
//     makes L the right window length in both steps 3 and 4), so every term is
//     >= rel[n-L] >= level[n-L];
//   * therefore a[n] >= level[n-L], the attenuation applied to sample n-L is at
//     least what that sample needed, and |output| <= ceiling. No overshoot, at
//     any release time, for any signal.
//
// Release BEFORE the maximum, not after, is what makes that proof work and is
// the one ordering in the chain that is not free to move: smoothing after the
// maximum would let the gain arrive late.
//
// The attack is not a parameter either, because it is not a free variable: it
// is the mean in step 4, i.e. a linear ramp exactly `look_` samples long that
// finishes precisely when the peak arrives. That is what lookahead buys.
//
// Inter-sample peaks are NOT limited: this is a sample-peak limiter, and a
// signal that reads -0.1 dBFS here can still reconstruct above 0 dBFS in a
// converter. Saying so is cheaper than an oversampled detector nobody asked
// for, and the default ceiling of -0.3 dB is the usual allowance.
class Limiter final : public InternalInstance {
public:
    explicit Limiter(const PluginDesc& d) : InternalInstance(d) {
        pIn_   = addParam("Input",   "dB", -12.f, 24.f,   0.f);
        pCeil_ = addParam("Ceiling", "dB", -24.f, 0.f,    -0.3f);
        pRel_  = addParam("Release", "ms", 1.f,   1000.f, 100.f, true);
        // Output-only. See InternalInstance::setReadout for the wart.
        pGr_   = addParam("Gain Reduction", "dB", 0.f, 60.f, 0.f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        look_ = (int)(kLookaheadSec * sr_ + 0.5);
        if (look_ < 1) look_ = 1;
        const int w = look_ + 1;        // see the proof: both windows are L+1

        // GUI thread: every allocation in the device, sized from the sample
        // rate alone. Nothing here depends on the block size, which is why
        // process() has no nframes limit.
        lineL_.resize(look_ + 8);
        lineR_.resize(look_ + 8);
        lineL_.reset();
        lineR_.reset();
        max_.resize(w);
        box_.resize(w);
        in_.setTime(sr_, 0.02f);
        in_.snap(dbToGain(clampv(p(pIn_), -12.f, 24.f)));
        rel_ = kFloorDb;
        resum_ = 0;
        setReadout(pGr_, 0.f);
        first_ = true;
        return true;
    }

    // Constant after prepare(), audio-thread-safe to read, and true: the
    // measured delay through this device is this number.
    int latencyFrames() const override { return look_; }

    // REALTIME. No per-block buffer, so any nframes is processed rather than
    // degraded -- which matters more here than elsewhere, since a limiter that
    // fell back to passthrough on a long block would pass a peak it exists to
    // stop.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (bypassed_ || !in || lineL_.buf.empty()) { passthrough(in, out, channels, nframes); return; }

        in_.set(dbToGain(clampv(p(pIn_), -12.f, 24.f)));
        if (first_) { in_.settle(); first_ = false; }
        const f32 ceil = clampv(p(pCeil_), -24.f, 0.f);
        const f32 rel  = dsp::poleCoef(sr_, clampv(p(pRel_), 1.f, 1000.f) * 1e-3f);

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        f32 peakGr = 0.f;
        for (int i = 0; i < nframes; ++i) {
            // Rebuild the moving-average sum exactly, every kResum samples of
            // ABSOLUTE time rather than once per block. Both halves matter: the
            // rebuild keeps the gain a function of the last L samples instead of
            // of every sample since prepare(), and doing it on a fixed sample
            // period rather than at block boundaries is what keeps the device
            // block-size invariant -- the same audio in blocks of 1 and of 256
            // comes out bit-identical, so a render cannot depend on the buffer
            // size it happened to be made at.
            if (resum_ <= 0) { box_.resum(); resum_ = kResum; }
            --resum_;

            // The input gain is applied ONCE, on the way into the line, so the
            // detector and the delayed audio can never disagree about it --
            // which they would if the gain were smoothed and applied at the
            // output, where it would be `look_` samples out of step.
            const f32 g  = in_.next();
            const f32 xl = (src[0] ? src[0][i] : 0.f) * g;
            const f32 xr = (nc > 1 && src[1]) ? src[1][i] * g : xl;

            const f32 dl = lineL_.tap(look_);     // read then push: exact delay
            const f32 dr = lineR_.tap(look_);
            lineL_.push(xl);
            lineR_.push(xr);

            // Detector: stereo-LINKED peak, so one gain moves both channels and
            // the image does not swing towards the quieter side on every hit.
            const f32 pk  = std::fmax(std::fabs(xl), std::fabs(xr));
            const f32 lvl = 20.f * std::log10(std::fmax(pk, kFloorLin));
            rel_ = lvl > rel_ ? lvl : rel_ + (lvl - rel_) * rel;

            const f32 a   = box_.push(max_.push(rel_));
            const f32 att = a > ceil ? a - ceil : 0.f;
            if (att > peakGr) peakGr = att;

            // The branch is not an optimisation: it makes a limiter that is not
            // limiting a BIT-EXACT wire (plus its delay), rather than a wire
            // multiplied by whatever pow(10, -0.0) returns.
            const f32 gg = att > 0.f ? dbToGain(-att) : 1.f;
            if (dst[0]) dst[0][i] = dl * gg;
            if (nc > 1 && dst[1]) dst[1][i] = dr * gg;
        }

        if (!dsp::sane(rel_)) rel_ = kFloorDb;
        // One value per block: the worst reduction in the block, which is the
        // number an engineer wants to see.
        setReadout(pGr_, peakGr);

        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh = 2;
    // Five milliseconds. Long enough that the attack ramp is inaudible on
    // programme material, short enough to keep the reported latency small; and
    // it happens to be the same 240 frames at 48 kHz that LSP's limiter
    // reports, which is the figure docs/RACKS.md §1 already measures against.
    static constexpr f64 kLookaheadSec = 0.005;
    static constexpr f32 kFloorLin = 1e-7f;      // -140 dB
    static constexpr f32 kFloorDb  = -140.f;
    static constexpr int kResum    = 1024;

    int pIn_ = 0, pCeil_ = 0, pRel_ = 0, pGr_ = 0;

    dsp::DelayLine  lineL_, lineR_;
    dsp::SlidingMax max_;
    dsp::Boxcar     box_;
    dsp::Smoother   in_;
    f32  rel_   = kFloorDb;
    int  look_  = 1;
    int  resum_ = 0;
    bool first_ = true;
};

// --- Utility ---------------------------------------------------------------
// Gain, stereo width, mono fold, polarity and a DC blocker. The device with no
// sound of its own, and the one that ends up on the most channels.
//
// THE PROPERTY THAT MATTERS: at its defaults it is a BIT-EXACT WIRE. Not
// "transparent", not "within a fraction of a dB" -- the samples that come out
// are the samples that went in. That is what makes it safe to leave on every
// channel, and it is a claim that has to be engineered rather than hoped for,
// because the obvious implementation destroys it:
//
//     mid = (L+R)/2;  side = (L-R)/2;  out = mid + side
//
// is NOT L in floating point. So Width == 1 is a BRANCH, not a computation, and
// the same reasoning puts a branch on the limiter's unity gain. Everything else
// on the identity path is a multiply by exactly 1.0f, which is exact.
//
// Order of operations, and why: DC block, then polarity, then width, then gain.
//   * The DC blocker is first so it sees the signal as it arrived, before a
//     polarity flip has changed the sign of the offset it is tracking.
//   * Polarity is before width, so inverting one side and folding to mono
//     cancels -- which is exactly what someone flipping a polarity switch is
//     listening for.
//   * Gain is last, so the knob is a true output level whatever the rest does.
//
// Mono is Width 0 with a switch of its own, because folding to mono is the one
// width setting people reach for by name. It moves the same smoother, so the
// switch glides rather than clicks.
//
// Polarity is smoothed too, over 20 ms. Flipping it therefore dips through zero
// rather than stepping, which is the lesser of two artefacts: a true crossfade
// would need the un-inverted copy kept alive in a second buffer for the sake of
// a switch nobody automates at audio rate.
class Utility final : public InternalInstance {
public:
    explicit Utility(const PluginDesc& d) : InternalInstance(d) {
        // The bottom of the range is -70 dB because that is where dbToGain()
        // returns exactly zero: a Gain control that cannot actually mute is one
        // people work around.
        pGain_  = addParam("Gain",  "dB", -70.f, 24.f, 0.f);
        pWidth_ = addParam("Width", "",   0.f,   2.f,  1.f);
        pMono_  = addBoolParam("Mono",     false);
        pInvL_  = addBoolParam("Invert L", false);
        pInvR_  = addBoolParam("Invert R", false);
        pDc_    = addBoolParam("DC Block", false);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        gain_.setTime(sr_, 0.02f);
        width_.setTime(sr_, 0.02f);
        sgnL_.setTime(sr_, 0.02f);
        sgnR_.setTime(sr_, 0.02f);
        gain_.snap(dbToGain(clampv(p(pGain_), -70.f, 24.f)));
        width_.snap(p(pMono_) >= 0.5f ? 0.f : clampv(p(pWidth_), 0.f, 2.f));
        sgnL_.snap(p(pInvL_) >= 0.5f ? -1.f : 1.f);
        sgnR_.snap(p(pInvR_) >= 0.5f ? -1.f : 1.f);
        // 5 Hz: below anything a listener would call bass, and still settles a
        // hard offset in a fraction of a second.
        dcL_.setCutoff(sr_, 5.f);
        dcR_.setCutoff(sr_, 5.f);
        dcL_.reset();
        dcR_.reset();
        first_ = true;
        return true;
    }

    // REALTIME. Per-sample and per-channel only; any nframes is legal.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (bypassed_ || !in) { passthrough(in, out, channels, nframes); return; }

        const bool dc = p(pDc_) >= 0.5f;
        gain_.set(dbToGain(clampv(p(pGain_), -70.f, 24.f)));
        width_.set(p(pMono_) >= 0.5f ? 0.f : clampv(p(pWidth_), 0.f, 2.f));
        sgnL_.set(p(pInvL_) >= 0.5f ? -1.f : 1.f);
        sgnR_.set(p(pInvR_) >= 0.5f ? -1.f : 1.f);
        if (first_) { gain_.settle(); width_.settle(); sgnL_.settle(); sgnR_.settle(); first_ = false; }
        // Switched off, the blocker holds no state: re-enabling it starts from
        // the signal rather than from whatever it last saw.
        if (!dc) { dcL_.reset(); dcR_.reset(); }

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        for (int i = 0; i < nframes; ++i) {
            f32 l = src[0] ? src[0][i] : 0.f;
            f32 r = (nc > 1 && src[1]) ? src[1][i] : l;

            if (dc) { l = dcL_.process(l); r = dcR_.process(r); }

            // Every smoother is advanced on every sample whatever the branches
            // below do, so the glide is a function of time and not of settings.
            l *= sgnL_.next();
            r *= sgnR_.next();

            const f32 w = width_.next();
            f32 ol, orr;
            if (w == 1.f) { ol = l; orr = r; }          // the bit-exact path
            else {
                const f32 mid = 0.5f * (l + r);
                const f32 sid = 0.5f * (l - r) * w;
                ol  = mid + sid;
                orr = mid - sid;
            }

            const f32 g = gain_.next();
            if (dst[0]) dst[0][i] = ol * g;
            if (nc > 1 && dst[1]) dst[1][i] = orr * g;
        }

        dcL_.check();
        dcR_.check();
        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh = 2;

    int pGain_ = 0, pWidth_ = 0, pMono_ = 0, pInvL_ = 0, pInvR_ = 0, pDc_ = 0;

    dsp::Smoother gain_, width_, sgnL_, sgnR_;
    dsp::DcBlock  dcL_, dcR_;
    bool first_ = true;
};

// --- Rack ------------------------------------------------------------------
// Up to eight devices in series behind eight macro knobs.
//
// THE WHOLE IDEA: a rack is a PluginInstance that owns PluginInstances. It
// therefore has to honour every clause of the contract in host.h *and forward
// it*, which is where all the interesting decisions are:
//
//   * latencyFrames() is the SUM of the chain, and is summed ON DEMAND rather
//     than cached, so that a rack nested inside this one can gain a latent
//     device without this rack's figure going stale. Reporting 0 would be a lie
//     the engine acts on -- it compensates with the number we give it, so a
//     rack containing the stock Limiter would smear every parallel path in the
//     project by exactly the amount we failed to declare.
//   * midi() is forwarded to sub-devices that declare a note input, so a rack
//     can contain an instrument.
//   * setParamRT() drives macros from the automation path, which means the
//     macro -> target scaling runs on the audio thread and must be a pure
//     lerp over values resolved at edit time.
//   * bypass short-circuits the entire chain, not each device individually.
//
// THREADING, in full, because a container has a problem a leaf device does not.
// The audio thread walks a chain that the GUI thread can edit underneath it.
// The chain and the mappings are therefore never mutated in place: an edit
// builds a complete new Layout in a slot of a small ring and publishes it with
// one release store, and process()/midi()/setParamRT() take one acquire load
// and then read a structure nobody will touch again. The ring is four deep so
// that four edits would have to land inside a single audio block before the
// layout being read could be rewritten -- that is a user's hand against a
// 5.3 ms block at 256 frames.
//
// What that buys is safe UNLINKING. What it cannot buy is safe DELETION: no
// code inside a PluginInstance can know when the audio thread last dereferenced
// a pointer. So a removed sub-device is retired, not deleted -- it stays owned
// by the rack until the rack dies or RackControl::reclaim() is called at a
// moment the caller knows is quiet. Retiring is bounded (kOwnedCap) so a script
// hammering removeDevice() fails loudly instead of eating memory.
class Rack final : public InternalInstance, public RackControl {
public:
    Rack(const PluginDesc& d, PluginRegistry* reg) : InternalInstance(d), reg_(reg) {
        char nm[16];
        for (int i = 0; i < kRackMacros; ++i) {
            std::snprintf(nm, sizeof nm, "Macro %d", i + 1);
            // 0..1 rather than Live's 0..127: the contract has one parameter
            // type and one range convention, an automation lane draws a
            // normalised curve, and every mapping is a lerp of this value.
            addParam(nm, "", 0.f, 1.f, 0.f);
        }
        // ring_[0] is the empty layout an unprepared, unfilled rack presents.
        // It is published immediately so process() never sees a null.
        live_.store(&ring_[0], std::memory_order_release);
        next_ = 1;
    }

    RackControl* rack() override { return this; }

    // GUI thread. Every sub-device is prepared at the rack's rate and block
    // size, and the chain sum is recomputed from what they report afterwards.
    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        // The only allocation in the device, on the GUI thread, like the
        // Delay's line. Two stereo buffers is all a series chain needs: the
        // last device writes straight into the caller's output, so N devices
        // ping-pong between A and B for the N-1 intermediate results.
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < kCh; ++c)
                scratch_[b][c].assign((size_t)maxBlock_, 0.f);
        scratchFrames_ = maxBlock_;

        bool ok = true;
        for (const auto& up : owned_)
            if (up && !up->prepare(sr_, maxBlock_)) ok = false;

        republish();
        return ok;
    }

    // REALTIME. The chain, in series, through the scratch pair.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        const Layout* L = live_.load(std::memory_order_acquire);

        // An empty rack is a wire. A block bigger than the one we were prepared
        // for degrades to a wire too, rather than to a heap call.
        if (bypassed_ || !L || L->n == 0 || nframes > scratchFrames_) {
            passthrough(in, out, channels, nframes);
            return;
        }

        const int nc = channels < kCh ? channels : kCh;

        const f32* cur[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) cur[c] = in ? in[c] : nullptr;

        int which = 0;
        for (int i = 0; i < L->n; ++i) {
            if (i == L->n - 1) {
                // The last device writes into the caller's buffer. With one
                // device in the rack this is `dev->process(in, out, ...)`
                // verbatim -- the same call the device would get standing on a
                // track by itself, aliasing and all -- which is what makes a
                // rack containing one device bit-exact with that device.
                L->dev[i]->process(cur, out, nc, nframes);
                break;
            }
            f32* dst[kCh] = { scratch_[which][0].data(), scratch_[which][1].data() };
            L->dev[i]->process(cur, dst, nc, nframes);
            for (int c = 0; c < nc; ++c) cur[c] = dst[c];
            which ^= 1;
        }

        copyExtra(in, out, nc, channels, nframes);
    }

    // REALTIME. Forwarded to the sub-devices that declare a note input, so a
    // rack can hold an instrument. `hasMidiIn` is cached in the layout rather
    // than read through desc() per event: desc() is a virtual call returning a
    // struct full of std::string, and nothing on the audio thread should be
    // anywhere near one.
    //
    // Bypass drops events, because bypass short-circuits the chain and
    // delivering notes to devices that are not being rendered would leave a
    // rack that had been bypassed through a phrase holding voices nobody asked
    // for. A note held across the bypass edge resumes when it is lifted.
    void midi(const u8* data, int len, int frameOffset) override {
        if (bypassed_) return;
        const Layout* L = live_.load(std::memory_order_acquire);
        if (!L) return;
        for (int i = 0; i < L->n; ++i)
            if (L->midi[i]) L->dev[i]->midi(data, len, frameOffset);
    }

    // GUI thread. Store the macro, then drive its targets down the GUI-side
    // parameter path.
    void setParam(int i, f32 v) override {
        InternalInstance::setParam(i, v);
        applyMacro(i, false);
    }

    // REALTIME (host.h): the automation path.
    //
    // Returns false when any target of this macro has no realtime parameter
    // path of its own. A rack macro can only be automated as well as the worst
    // device it drives, and saying so is what lets the engine grey the lane
    // instead of drawing an envelope that does nothing. Every internal device
    // accepts, so an all-internal rack is always automatable.
    bool setParamRT(int i, f32 v) override {
        InternalInstance::setParamRT(i, v);
        return applyMacro(i, true);
    }

    // The chain sum, computed from the published topology when it is asked for
    // rather than cached alongside it.
    //
    // It used to be cached in the Layout, computed in republish(). That was
    // wrong in exactly one case, and the case is not exotic: a rack INSIDE a
    // rack. Adding a latent device to the inner rack republishes the inner
    // rack's layout, but the outer rack has no way to know that happened, so
    // its cached sum stayed at whatever the inner rack reported when it was
    // added -- zero, since a rack is empty when you drop it in. The outer
    // figure is the only one the engine ever reads, so the whole project's
    // delay compensation was silently short by the inner rack's latency.
    //
    // Summing on demand fixes it with no new plumbing: each entry answers for
    // itself, and a nested rack answers by doing the same thing one level down.
    // It is realtime-safe -- one acquire load, at most kRackMaxDevices virtual
    // calls, bounded by kRackMaxDepth levels, no allocation and no lock -- and
    // it costs nothing per block, because the engine reads latency once when a
    // chain is published (docs/RACKS.md §1) and not per callback.
    //
    // The contract friction that remains is the one already documented: host.h
    // says latency is constant after prepare(), and a rack's is not. THE CALLER
    // MUST STILL REPUBLISH THE TRACK'S CHAIN AFTER A RACK EDIT for the engine's
    // compensation to follow. What is fixed here is that when it does, the
    // number it reads is now right at any nesting depth.
    int latencyFrames() const override {
        const Layout* L = live_.load(std::memory_order_acquire);
        if (!L) return 0;
        int lat = 0;
        for (int i = 0; i < L->n; ++i) {
            if (!L->dev[i]) continue;
            const int l = L->dev[i]->latencyFrames();
            if (l > 0) lat += l;                    // a negative figure is a bug, not a credit
        }
        return lat;
    }

    // --- RackControl (GUI thread) ------------------------------------------

    int deviceCount() const override { return (int)chain_.size(); }

    PluginInstance* device(int i) const override {
        return (i >= 0 && i < (int)chain_.size()) ? chain_[(size_t)i] : nullptr;
    }

    bool addDevice(const PluginDesc& d) override { return insertDevice((int)chain_.size(), d); }

    bool insertDevice(int at, const PluginDesc& d) override {
        if ((int)chain_.size() >= kRackMaxDevices) {
            LOGE("rack: full (%d devices), cannot add %s", kRackMaxDevices, d.uri.c_str());
            return false;
        }
        if (!reg_) {
            LOGE("rack: no registry behind this instance, cannot add %s", d.uri.c_str());
            return false;
        }
        if ((int)owned_.size() >= kOwnedCap) {
            LOGE("rack: %d retired devices; call reclaim() while the rack is idle", kOwnedCap);
            return false;
        }
        // The one call that has to be here and nowhere near process(): slow,
        // allocating, GUI-thread-only, and already prepared on the way out.
        std::unique_ptr<PluginInstance> inst = reg_->instantiate(d, sr_, maxBlock_);
        if (!inst) return false;

        PluginInstance* raw = inst.get();
        owned_.push_back(std::move(inst));

        at = clampv(at, 0, (int)chain_.size());
        chain_.insert(chain_.begin() + at, raw);
        for (RackMapping& m : maps_) if (m.device >= at) ++m.device;

        republish();
        applyAllMacros();
        return true;
    }

    bool removeDevice(int i) override {
        if (i < 0 || i >= (int)chain_.size()) return false;
        chain_.erase(chain_.begin() + i);
        // Mappings that pointed at it are gone; the ones above it slide down.
        maps_.erase(std::remove_if(maps_.begin(), maps_.end(),
                                   [i](const RackMapping& m) { return m.device == i; }),
                    maps_.end());
        for (RackMapping& m : maps_) if (m.device > i) --m.device;
        republish();                      // the instance stays in owned_: see reclaim()
        return true;
    }

    bool moveDevice(int from, int to) override {
        const int n = (int)chain_.size();
        if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
        PluginInstance* d = chain_[(size_t)from];
        chain_.erase(chain_.begin() + from);
        chain_.insert(chain_.begin() + to, d);
        // Renumber so every mapping still points at the device it was made for.
        for (RackMapping& m : maps_) {
            if (m.device == from)                             m.device = to;
            else if (from < to && m.device > from && m.device <= to) --m.device;
            else if (to < from && m.device >= to && m.device < from) ++m.device;
        }
        republish();
        return true;
    }

    int mappingCount() const override { return (int)maps_.size(); }

    const RackMapping& mapping(int i) const override {
        static const RackMapping kNone{};
        return (i >= 0 && i < (int)maps_.size()) ? maps_[(size_t)i] : kNone;
    }

    int addMapping(const RackMapping& in) override {
        if ((int)maps_.size() >= kRackMaxMappings) return -1;
        if (in.macro < 0 || in.macro >= kRackMacros) return -1;
        if (in.device < 0 || in.device >= (int)chain_.size()) return -1;

        PluginInstance* d = chain_[(size_t)in.device];
        const int pi = paramIndexOf(*d, in.param);
        if (pi < 0) return -1;

        // Clamp the endpoints into the target's own range, which PRESERVES
        // inversion (min and max are clamped independently, so min > max stays
        // min > max) and makes mapping() report what the macro will really do
        // rather than what was asked for. A non-finite endpoint is a caller bug
        // that would poison the target on the audio thread, so it is refused.
        const ParamInfo& info = d->paramInfo(pi);
        const f32 lo = info.min < info.max ? info.min : info.max;
        const f32 hi = info.min < info.max ? info.max : info.min;
        RackMapping m = in;
        if (!std::isfinite(m.min) || !std::isfinite(m.max)) return -1;
        m.min = clampv(m.min, lo, hi);
        m.max = clampv(m.max, lo, hi);

        maps_.push_back(m);
        republish();
        applyMacro(m.macro, false);       // the target snaps to where the macro already is
        return (int)maps_.size() - 1;
    }

    bool removeMapping(int i) override {
        if (i < 0 || i >= (int)maps_.size()) return false;
        maps_.erase(maps_.begin() + i);
        republish();
        return true;
    }

    void clearMacro(int macro) override {
        maps_.erase(std::remove_if(maps_.begin(), maps_.end(),
                                   [macro](const RackMapping& m) { return m.macro == macro; }),
                    maps_.end());
        republish();
    }

    RackState state() const override {
        RackState s;
        for (PluginInstance* d : chain_) {
            RackState::Device sd;
            sd.uri    = d->desc().uri;
            sd.bypass = d->bypassed();
            for (int i = 0; i < d->paramCount(); ++i)
                sd.params.emplace_back(d->paramInfo(i).id, d->getParam(i));
            if (RackControl* nested = d->rack())      // a rack inside a rack
                sd.state = rackStateToString(nested->state());
            s.devices.push_back(std::move(sd));
        }
        for (int i = 0; i < kRackMacros; ++i) s.macros[i] = getParam(i);
        s.mappings = maps_;
        return s;
    }

    bool setState(const RackState& s) override { return setStateDepth(s, 0); }

    void reclaim() override {
        for (size_t i = 0; i < owned_.size(); ) {
            PluginInstance* raw = owned_[i].get();
            if (std::find(chain_.begin(), chain_.end(), raw) == chain_.end())
                owned_.erase(owned_.begin() + (std::ptrdiff_t)i);
            else
                ++i;
        }
    }

private:
    static constexpr int kCh       = 2;
    static constexpr int kRing     = 4;
    static constexpr int kOwnedCap = 64;

    // A mapping with the id already resolved to an index, because setParamRT
    // takes an index and resolving one means walking paramInfo() -- a loop over
    // std::string-carrying structs that has no business on the audio thread.
    struct LiveMap {
        int macro = 0, device = 0, pidx = 0;
        f32 min = 0.f, max = 1.f;
    };

    // Everything the audio thread reads, in one immutable-once-published block.
    //
    // The latency sum is deliberately NOT in here: see latencyFrames(). A
    // cached figure cannot see an edit made to a rack nested inside this one.
    struct Layout {
        int             n   = 0;
        PluginInstance* dev[kRackMaxDevices]  = {};
        bool            midi[kRackMaxDevices] = {};
        int             nMaps = 0;
        LiveMap         map[kRackMaxMappings];
    };

    static int paramIndexOf(const PluginInstance& d, u32 id) {
        const int n = d.paramCount();
        for (int i = 0; i < n; ++i)
            if (d.paramInfo(i).id == id) return i;
        return -1;
    }

    // GUI thread. Builds the next layout from the editable master copies and
    // swaps it in with one release store.
    void republish() {
        Layout& L = ring_[next_];
        L.n = 0;
        for (PluginInstance* d : chain_) {
            if (L.n >= kRackMaxDevices) break;
            L.dev[L.n]  = d;
            L.midi[L.n] = d->desc().hasMidiIn;
            ++L.n;
        }

        L.nMaps = 0;
        for (const RackMapping& m : maps_) {
            if (L.nMaps >= kRackMaxMappings) break;
            if (m.macro < 0 || m.macro >= kRackMacros) continue;
            if (m.device < 0 || m.device >= L.n) continue;
            const int pi = paramIndexOf(*L.dev[m.device], m.param);
            if (pi < 0) continue;                   // the device no longer has that parameter
            L.map[L.nMaps] = { m.macro, m.device, pi, m.min, m.max };
            ++L.nMaps;
        }

        live_.store(&L, std::memory_order_release);
        next_ = (next_ + 1) % kRing;
    }

    // THE SCALING RULE, in one line: target = min + (max - min) * macro, with
    // the macro in 0..1 and min/max in the TARGET's own units.
    //
    // That is all of it, and everything the feature promises falls out of it:
    //   * a partial range (min/max inside the parameter's range) sweeps only
    //     that slice;
    //   * an inverted range (min > max) walks the target down as the macro goes
    //     up, because (max - min) is negative;
    //   * two mappings on one macro move both targets, each in its own units;
    //   * two mappings on one target from different macros are applied in
    //     mapping order and the last write wins -- there is no summing, because
    //     "which knob owns this parameter" is a question with one answer.
    //
    // `rt` picks the sub-device's realtime path over its GUI path. That is not
    // cosmetic: host.h documents setParam() as the single producer on backends
    // whose parameter path is a queue (CLAP), so a rack calling setParam() from
    // the audio thread would be exactly the data race that entry point exists
    // to prevent.
    bool applyMacro(int macro, bool rt) {
        if (macro < 0 || macro >= kRackMacros) return true;
        const Layout* L = live_.load(std::memory_order_acquire);
        if (!L || L->nMaps == 0) return true;

        const f32 t = clampv(p(macro), 0.f, 1.f);
        bool ok = true;
        for (int k = 0; k < L->nMaps; ++k) {
            const LiveMap& m = L->map[k];
            if (m.macro != macro) continue;
            const f32 v = m.min + (m.max - m.min) * t;
            PluginInstance* d = L->dev[m.device];
            if (rt) { if (!d->setParamRT(m.pidx, v)) ok = false; }
            else    d->setParam(m.pidx, v);
        }
        return ok;
    }

    void applyAllMacros() {
        for (int i = 0; i < kRackMacros; ++i) applyMacro(i, false);
    }

    bool setStateDepth(const RackState& s, int depth) {
        chain_.clear();
        maps_.clear();
        republish();                                 // unlink before anything else moves

        bool ok = true;
        if (!reg_ && !s.devices.empty()) {
            LOGE("rack: no registry behind this instance, cannot restore %zu devices",
                 s.devices.size());
            ok = false;
        }

        for (const RackState::Device& sd : s.devices) {
            if ((int)chain_.size() >= kRackMaxDevices) { ok = false; break; }
            if (!reg_) break;
            const PluginDesc* pd = reg_->find(sd.uri);   // resolves lattice: -> nxtakt: too
            if (!pd) { LOGE("rack: device not installed: %s", sd.uri.c_str()); ok = false; continue; }
            if (!addDevice(*pd)) { ok = false; continue; }

            PluginInstance* d = chain_.back();
            for (const auto& pv : sd.params) {
                const int pi = paramIndexOf(*d, pv.first);
                if (pi >= 0) d->setParam(pi, pv.second);
            }
            d->setBypassed(sd.bypass);
            if (RackControl* nested = d->rack()) {
                if (depth + 1 >= kRackMaxDepth) {
                    LOGE("rack: nesting deeper than %d, inner rack left empty", kRackMaxDepth);
                    ok = false;
                } else if (!sd.state.empty()) {
                    RackState inner;
                    if (rackStateFromString(sd.state, inner)) {
                        // Depth is carried down the concrete type, so the cap
                        // is enforced by the loader rather than by a hostile
                        // file's own idea of how deep it goes.
                        if (Rack* r = dynamic_cast<Rack*>(nested)) ok = r->setStateDepth(inner, depth + 1) && ok;
                        else ok = nested->setState(inner) && ok;
                    } else {
                        LOGE("rack: nested state did not parse");
                        ok = false;
                    }
                }
            }
        }

        for (const RackMapping& m : s.mappings)
            if (addMapping(m) < 0) ok = false;

        // Macros are written WITHOUT re-driving their targets: the parameter
        // values restored above already are what these macro positions produced
        // when the state was saved, and re-applying would only round them.
        for (int i = 0; i < kRackMacros; ++i)
            InternalInstance::setParam(i, s.macros[i]);

        republish();
        return ok;
    }

    PluginRegistry* reg_ = nullptr;

    // GUI-side master copies. The audio thread never reads these.
    std::vector<std::unique_ptr<PluginInstance>> owned_;   // append-only until reclaim()
    std::vector<PluginInstance*>                 chain_;   // processing order
    std::vector<RackMapping>                     maps_;

    Layout                      ring_[kRing];
    std::atomic<const Layout*>  live_{nullptr};
    int                         next_ = 0;

    std::vector<f32> scratch_[2][kCh];
    int              scratchFrames_ = 0;
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

// Effects share every field but the identity, so the descriptor is built once
// and stamped. A new stock effect is now three lines plus its DSP.
PluginDesc effectDesc(const char* uri, const char* name, const char* category,
                      int paramCount) {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = uri;
    d.name       = name;
    d.vendor     = "NxTakt";
    d.category   = category;
    d.kind       = PluginKind::Effect;
    d.audioIn    = 2;
    d.audioOut   = 2;
    d.hasMidiIn  = false;
    d.paramCount = paramCount;
    return d;
}

// The rack is not built by effectDesc() because of one field: hasMidiIn.
//
// It is true unconditionally, and the descriptor never changes to reflect what
// is actually inside. A rack that contains Pulse has to be fed notes, and the
// engine decides what to feed from the descriptor, which it reads once when the
// device is added -- so a descriptor that only became note-capable after the
// user dropped an instrument in would be read too late to matter. Declaring the
// input and forwarding nothing when the rack holds no instrument costs one
// branch per event.
//
// `kind` stays Effect for the same reason in reverse: it is what the browser
// sorts and filters on, and a rack in the browser is empty, so "instrument" is
// a claim about a rack that does not exist yet. A rack containing an instrument
// therefore reports kind Effect -- see the report.
PluginDesc rackDesc() {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = kRackUri;
    d.name       = "Rack";
    d.vendor     = "NxTakt";
    d.category   = "Rack";
    d.kind       = PluginKind::Effect;
    d.audioIn    = 2;
    d.audioOut   = 2;
    d.hasMidiIn  = true;
    d.paramCount = kRackMacros;
    return d;
}

PluginDesc eq3Desc()        { return effectDesc(kEq3Uri,        "EQ Three",   "EQ",       7); }
PluginDesc compressorDesc() { return effectDesc(kCompressorUri, "Compressor", "Dynamics", 7); }
PluginDesc delayDesc()      { return effectDesc(kDelayUri,      "Delay",      "Delay",    8); }
PluginDesc reverbDesc()     { return effectDesc(kReverbUri,     "Reverb",     "Reverb",   6); }
PluginDesc autoFilterDesc() { return effectDesc(kAutoFilterUri, "Auto Filter", "Filter",     13); }
PluginDesc chorusDesc()     { return effectDesc(kChorusUri,     "Chorus",      "Modulation", 7); }
PluginDesc limiterDesc()    { return effectDesc(kLimiterUri,    "Limiter",     "Dynamics",   4); }
PluginDesc utilityDesc()    { return effectDesc(kUtilityUri,    "Utility",     "Utility",    6); }

} // namespace

// --- entry points ----------------------------------------------------------
void scanInternal(std::vector<PluginDesc>& out) {
    const size_t before = out.size();
    out.push_back(saturatorDesc());
    out.push_back(pulseDesc());
    out.push_back(eq3Desc());
    out.push_back(compressorDesc());
    out.push_back(delayDesc());
    out.push_back(reverbDesc());
    out.push_back(autoFilterDesc());
    out.push_back(chorusDesc());
    out.push_back(limiterDesc());
    out.push_back(utilityDesc());
    out.push_back(rackDesc());
    // Counted rather than spelled out: a device added without touching this
    // line would otherwise make the log quietly wrong.
    LOGI("internal: %zu devices", out.size() - before);
}

std::unique_ptr<PluginInstance> instantiateInternal(const PluginDesc& d,
                                                    f64 sampleRate, int maxBlock,
                                                    PluginRegistry* reg) {
    // Both spellings, always: see the note at kSaturatorUri. A descriptor that
    // came from a pre-rename project file rather than from the registry still
    // arrives here carrying `lattice:`.
    std::unique_ptr<PluginInstance> inst;
    if (d.uri == kSaturatorUri || d.uri == kSaturatorUriLegacy)
        inst = std::make_unique<Saturator>(saturatorDesc());
    else if (d.uri == kPulseUri || d.uri == kPulseUriLegacy)
        inst = std::make_unique<Pulse>(pulseDesc());
    else if (d.uri == kEq3Uri)
        inst = std::make_unique<Eq3>(eq3Desc());
    else if (d.uri == kCompressorUri)
        inst = std::make_unique<Compressor>(compressorDesc());
    else if (d.uri == kDelayUri)
        inst = std::make_unique<Delay>(delayDesc());
    else if (d.uri == kReverbUri)
        inst = std::make_unique<Reverb>(reverbDesc());
    else if (d.uri == kAutoFilterUri)
        inst = std::make_unique<AutoFilter>(autoFilterDesc());
    else if (d.uri == kChorusUri)
        inst = std::make_unique<Chorus>(chorusDesc());
    else if (d.uri == kLimiterUri)
        inst = std::make_unique<Limiter>(limiterDesc());
    else if (d.uri == kUtilityUri)
        inst = std::make_unique<Utility>(utilityDesc());
    else if (d.uri == kRackUri)
        // The one device that is handed the registry: it has to be able to
        // instantiate the devices it contains, and PluginRegistry::instantiate
        // is GUI-thread-only and allocating, which is exactly why that happens
        // here and at edit time rather than anywhere near process().
        inst = std::make_unique<Rack>(rackDesc(), reg);
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

// ---------------------------------------------------------------------------
// The rack's passive form, as text
//
// One line of printable ASCII with no whitespace, no quotes and no newline, so
// the persistence layer can carry it as an opaque scalar and never learn what a
// rack is. src/core owns the project format; this owns what a rack means; the
// string is the seam between them.
//
//   nxrack1;m=<8 floats>;d=<uri>,<bypass>,<nested|->,<id>:<v>...;x=<macro>,<dev>,<id>,<min>,<max>
//
// Records are ';'-separated and tagged by their first two characters, so a
// later version may add records and an older reader will skip them. `d` records
// are positional -- their order IS the chain order -- and `x` records name
// their device by that position.
//
// URIs (and nested rack states) are percent-escaped over the five characters
// that could be confused with structure plus anything non-printable, which
// makes an http:// LV2 URI with a query string safe and lets a nested rack
// nest to any depth: each level escapes the level below exactly once.
//
// Floats go through snprintf/strtod in the C locale. main.cpp pins LC_NUMERIC
// to "C" for the whole process precisely so a de_DE user's decimal comma cannot
// get into a project file; the same reasoning applies here and the same trick
// as project.cpp's fmtF32 finds the shortest round-tripping form.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kRackTag = "nxrack1";

bool rackNeedsEsc(unsigned char c) {
    return c <= ' ' || c >= 0x7F || c == '%' || c == ';' || c == ',' || c == ':' || c == '=';
}

void rackEsc(std::string& o, const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    for (char ch : s) {
        const unsigned char c = (unsigned char)ch;
        if (rackNeedsEsc(c)) { o += '%'; o += kHex[c >> 4]; o += kHex[c & 15]; }
        else                   o += ch;
    }
}

int rackHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string rackUnesc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = rackHex(s[i + 1]), lo = rackHex(s[i + 2]);
            if (hi >= 0 && lo >= 0) { o += (char)((hi << 4) | lo); i += 2; continue; }
        }
        o += s[i];
    }
    return o;
}

// project.cpp's fmtF32, reproduced rather than shared because src/core is not
// this layer's to reach into for a formatting helper.
std::string rackFmt(f32 v) {
    if (!std::isfinite(v)) v = 0.f;
    char buf[64];
    for (int p = 4; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, (f64)v);
        if ((f32)std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

f32 rackF32(const std::string& s) {
    const f32 v = (f32)std::strtod(s.c_str(), nullptr);
    return std::isfinite(v) ? v : 0.f;
}

std::vector<std::string> rackSplit(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) { out.push_back(s.substr(start, i - start)); start = i + 1; }
    }
    return out;
}

} // namespace

std::string rackStateToString(const RackState& s) {
    std::string o = kRackTag;

    o += ";m=";
    for (int i = 0; i < kRackMacros; ++i) {
        if (i) o += ',';
        o += rackFmt(s.macros[i]);
    }

    for (const RackState::Device& d : s.devices) {
        o += ";d=";
        rackEsc(o, d.uri);
        o += ',';
        o += d.bypass ? '1' : '0';
        o += ',';
        // A single '-' rather than an empty field, so a device with no nested
        // state and a device whose nested state failed to serialise cannot be
        // told apart by accident.
        if (d.state.empty()) o += '-';
        else                 rackEsc(o, d.state);
        for (const auto& pv : d.params) {
            o += ',';
            o += std::to_string(pv.first);
            o += ':';
            o += rackFmt(pv.second);
        }
    }

    for (const RackMapping& m : s.mappings) {
        o += ";x=";
        o += std::to_string(m.macro);  o += ',';
        o += std::to_string(m.device); o += ',';
        o += std::to_string(m.param);  o += ',';
        o += rackFmt(m.min);           o += ',';
        o += rackFmt(m.max);
    }
    return o;
}

bool rackStateFromString(const std::string& text, RackState& out) {
    out = RackState{};
    if (text.empty()) return false;

    const std::vector<std::string> recs = rackSplit(text, ';');
    if (recs.empty() || recs[0] != kRackTag) return false;

    for (size_t r = 1; r < recs.size(); ++r) {
        const std::string& rec = recs[r];
        if (rec.size() < 2 || rec[1] != '=') continue;      // unknown shape: skip, do not fail
        const std::string body = rec.substr(2);
        const std::vector<std::string> f = rackSplit(body, ',');

        switch (rec[0]) {
            case 'm':
                for (size_t i = 0; i < f.size() && i < (size_t)kRackMacros; ++i)
                    out.macros[i] = clampv(rackF32(f[i]), 0.f, 1.f);
                break;

            case 'd': {
                if (f.size() < 3) break;
                if ((int)out.devices.size() >= kRackMaxDevices) break;
                RackState::Device d;
                d.uri    = rackUnesc(f[0]);
                d.bypass = f[1] == "1";
                if (f[2] != "-") d.state = rackUnesc(f[2]);
                for (size_t i = 3; i < f.size(); ++i) {
                    const size_t colon = f[i].find(':');
                    if (colon == std::string::npos) continue;
                    d.params.emplace_back((u32)std::strtoul(f[i].substr(0, colon).c_str(), nullptr, 10),
                                          rackF32(f[i].substr(colon + 1)));
                }
                out.devices.push_back(std::move(d));
                break;
            }

            case 'x': {
                if (f.size() < 5) break;
                if ((int)out.mappings.size() >= kRackMaxMappings) break;
                RackMapping m;
                m.macro  = (int)std::strtol(f[0].c_str(), nullptr, 10);
                m.device = (int)std::strtol(f[1].c_str(), nullptr, 10);
                m.param  = (u32)std::strtoul(f[2].c_str(), nullptr, 10);
                m.min    = rackF32(f[3]);
                m.max    = rackF32(f[4]);
                // Structural validation only. Whether the parameter exists is a
                // question for the rack, which has the device; RackControl::
                // addMapping is where a mapping that no longer resolves is
                // dropped, and it is the only place that could know.
                if (m.macro < 0 || m.macro >= kRackMacros) break;
                if (m.device < 0 || m.device >= kRackMaxDevices) break;
                out.mappings.push_back(m);
                break;
            }

            default:
                break;                                       // a record from a newer writer
        }
    }
    return true;
}

} // namespace lat
