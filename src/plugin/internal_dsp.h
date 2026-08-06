// Shared DSP primitives for NxTakt's stock devices.
//
// Header-only and dependency-free on purpose: internal_devices.cpp is the only
// translation unit that includes it, so there is nothing to add to the build,
// and every function here is small enough that the compiler inlines it into the
// per-sample loops it was written for.
//
// Realtime rules are the ones in internal_devices.cpp: anything called from
// process() allocates nothing, locks nothing and throws nothing. The types that
// own memory (DelayLine) allocate in a resize() that is only ever called from
// prepare(), which is GUI-thread by the PluginInstance contract.
//
// Denormals are treated as a correctness problem rather than a performance one:
// we do not control the FPU mode of the thread the host handed us, so every
// state variable that decays towards zero is flushed explicitly. A feedback
// delay or a reverb tank left running into denormals costs hundreds of cycles
// per sample and shows up as an xrun, not as a wrong number.
#pragma once
#include "../core/common.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lat {
namespace dsp {

inline constexpr f32 kTwoPi = 6.28318530718f;

// Anything below this is inaudible at any sane gain and is either a denormal or
// on its way to being one.
inline f32 flushDenormal(f32 v) {
    return (std::fabs(v) < 1e-25f) ? 0.f : v;
}

// A state variable that has gone non-finite (or absurd) can only have come from
// a coefficient set we should not have produced, and it is contagious: one NaN
// in a feedback loop is silence-until-reload. Every recursive structure below
// checks its state once per block and resets rather than propagating.
inline bool sane(f32 v) { return std::isfinite(v) && std::fabs(v) < 1e6f; }

// One-pole coefficient for a time constant in seconds: the fraction of the
// remaining distance to cover per sample.
inline f32 poleCoef(f64 sr, f32 seconds) {
    if (!(seconds > 0.f) || sr <= 0.0) return 1.f;
    const f32 n = (f32)(seconds * sr);
    if (n < 1.f) return 1.f;
    return 1.f - std::exp(-1.f / n);
}

// --- parameter smoothing ---------------------------------------------------
// A knob is read once per block but must not step once per block: 24 dB of
// makeup gain arriving between two samples is a click. Every scalar that
// multiplies the signal goes through one of these.
struct Smoother {
    f32 cur = 0.f, target = 0.f, a = 1.f;

    void setTime(f64 sr, f32 seconds) { a = poleCoef(sr, seconds); }
    void snap(f32 v)                  { cur = target = v; }
    void set(f32 v)                   { target = v; }
    // Arrive at wherever the target currently is, without gliding. Used on the
    // first block after prepare(): a project load sets every parameter AFTER
    // instantiate(), so a device that glided from its defaults would spend the
    // first 20 ms of a session playing something nobody asked for -- audibly so
    // for a dry/wet at zero, which would sputter wet signal on the first block.
    void settle()                     { cur = target; }

    // REALTIME.
    inline f32 next() {
        cur += (target - cur) * a;
        // Snap once the residue stops mattering: without this the difference
        // decays into the denormal range and stays there forever.
        if (std::fabs(target - cur) < 1e-9f) cur = target;
        return cur;
    }
};

// --- one-pole lowpass ------------------------------------------------------
// The workhorse: feedback-path tone control, compressor detector, reverb
// damping. y[n] = y[n-1] + a*(x[n] - y[n-1]), a from the bilinear-ish
// 1 - exp(-2*pi*fc/sr), which is exact for the impulse-invariant one-pole and
// well behaved right up to Nyquist (unlike the small-angle form).
struct OnePole {
    f32 a = 1.f, z = 0.f;

    void setCutoff(f64 sr, f32 hz) {
        if (sr <= 0.0) { a = 1.f; return; }
        const f32 fc = clampv(hz, 1.f, (f32)(sr * 0.49));
        a = 1.f - std::exp(-kTwoPi * fc / (f32)sr);
        a = clampv(a, 1e-5f, 1.f);
    }
    void reset() { z = 0.f; }
    void check() { if (!sane(z)) z = 0.f; }

    // REALTIME.
    inline f32 process(f32 x) {
        z = flushDenormal(z + a * (x - z));
        return z;
    }
};

// --- biquad ----------------------------------------------------------------
// Coefficients and state are separate types because they have different
// lifetimes: one coefficient set drives every channel, and each channel keeps
// its own two-sample memory. Folding them together is the classic way to end up
// gliding a coefficient set once per channel instead of once per sample.
//
// Transposed Direct Form II: fewest state variables, and the one form whose
// state stays bounded by roughly the signal level rather than by the internal
// gain of the filter, which is what makes it the sane choice for a shelf with
// +15 dB in it.
struct BiquadCoeffs {
    f32 b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;   // a0 normalised out
};

struct BiquadState {
    f32 z1 = 0.f, z2 = 0.f;
    void reset() { z1 = z2 = 0.f; }
    void check() { if (!sane(z1) || !sane(z2)) reset(); }
};

// REALTIME.
inline f32 biquadTick(const BiquadCoeffs& c, BiquadState& s, f32 x) {
    const f32 y = c.b0 * x + s.z1;
    s.z1 = flushDenormal(c.b1 * x - c.a1 * y + s.z2);
    s.z2 = flushDenormal(c.b2 * x - c.a2 * y);
    return y;
}

// Per-sample glide towards a new coefficient set. Recomputing an RBJ section
// costs a cos, a sin and a sqrt, so it happens when a parameter changes and not
// per sample; the glide is what stops the resulting step from clicking.
// REALTIME.
inline void biquadGlide(BiquadCoeffs& c, const BiquadCoeffs& t, f32 g) {
    c.b0 += (t.b0 - c.b0) * g;
    c.b1 += (t.b1 - c.b1) * g;
    c.b2 += (t.b2 - c.b2) * g;
    c.a1 += (t.a1 - c.a1) * g;
    c.a2 += (t.a2 - c.a2) * g;
}

// Once per block: kill the residue so the glide multiplies do not spend the
// rest of the session on denormals.
inline void biquadSettle(BiquadCoeffs& c, const BiquadCoeffs& t) {
    if (std::fabs(t.b0 - c.b0) < 1e-9f && std::fabs(t.b1 - c.b1) < 1e-9f &&
        std::fabs(t.b2 - c.b2) < 1e-9f && std::fabs(t.a1 - c.a1) < 1e-9f &&
        std::fabs(t.a2 - c.a2) < 1e-9f)
        c = t;
}

// RBJ Audio EQ Cookbook, normalised by a0. The frequency is clamped below
// Nyquist before the trigonometry: a shelf asked for 18 kHz at 32 kHz sample
// rate is not an error the user made, it is a rate the project was opened at.
namespace rbj {

struct Raw { f64 b0, b1, b2, a0, a1, a2; };

inline BiquadCoeffs normalise(const Raw& r) {
    BiquadCoeffs c;
    const f64 a0 = (std::fabs(r.a0) < 1e-20) ? 1e-20 : r.a0;
    c.b0 = (f32)(r.b0 / a0);
    c.b1 = (f32)(r.b1 / a0);
    c.b2 = (f32)(r.b2 / a0);
    c.a1 = (f32)(r.a1 / a0);
    c.a2 = (f32)(r.a2 / a0);
    return c;
}

inline f64 omega(f64 sr, f32 hz) {
    const f64 f = clampv((f64)hz, 1.0, sr * 0.495);
    return 6.283185307179586 * f / sr;
}

inline BiquadCoeffs peaking(f64 sr, f32 hz, f32 gainDb, f32 q) {
    const f64 A  = std::pow(10.0, (f64)gainDb / 40.0);
    const f64 w  = omega(sr, hz);
    const f64 cs = std::cos(w), sn = std::sin(w);
    const f64 al = sn / (2.0 * (f64)clampv(q, 0.05f, 40.f));
    return normalise({ 1.0 + al * A, -2.0 * cs, 1.0 - al * A,
                       1.0 + al / A, -2.0 * cs, 1.0 - al / A });
}

inline BiquadCoeffs lowShelf(f64 sr, f32 hz, f32 gainDb) {
    const f64 A  = std::pow(10.0, (f64)gainDb / 40.0);
    const f64 w  = omega(sr, hz);
    const f64 cs = std::cos(w), sn = std::sin(w);
    const f64 al = sn * 0.7071067811865476;             // slope S = 1
    const f64 tsa = 2.0 * std::sqrt(A) * al;
    return normalise({       A * ((A + 1.0) - (A - 1.0) * cs + tsa),
                       2.0 * A * ((A - 1.0) - (A + 1.0) * cs),
                             A * ((A + 1.0) - (A - 1.0) * cs - tsa),
                                 (A + 1.0) + (A - 1.0) * cs + tsa,
                      -2.0 *     ((A - 1.0) + (A + 1.0) * cs),
                                 (A + 1.0) + (A - 1.0) * cs - tsa });
}

inline BiquadCoeffs highShelf(f64 sr, f32 hz, f32 gainDb) {
    const f64 A  = std::pow(10.0, (f64)gainDb / 40.0);
    const f64 w  = omega(sr, hz);
    const f64 cs = std::cos(w), sn = std::sin(w);
    const f64 al = sn * 0.7071067811865476;
    const f64 tsa = 2.0 * std::sqrt(A) * al;
    return normalise({        A * ((A + 1.0) + (A - 1.0) * cs + tsa),
                      -2.0 *  A * ((A - 1.0) + (A + 1.0) * cs),
                              A * ((A + 1.0) + (A - 1.0) * cs - tsa),
                                  (A + 1.0) - (A - 1.0) * cs + tsa,
                       2.0 *     ((A - 1.0) - (A + 1.0) * cs),
                                  (A + 1.0) - (A - 1.0) * cs - tsa });
}

} // namespace rbj

// --- delay line ------------------------------------------------------------
// Power-of-two length so the wrap is a mask rather than a branch or a modulo,
// which matters when a reverb tank does a dozen reads per sample.
//
// Read/write order is fixed by the tap convention: tap(d) is the sample written
// d pushes ago, so `read then push` gives an exact d-sample delay and is the
// order every feedback structure here uses.
struct DelayLine {
    std::vector<f32> buf;
    u32 mask = 0;
    u32 w    = 0;

    // GUI thread (prepare) only: this is the one thing in the file that
    // allocates.
    void resize(int minSamples) {
        u32 n = 8;
        while (n < (u32)(minSamples < 8 ? 8 : minSamples) + 4u) n <<= 1;
        buf.assign((size_t)n, 0.f);
        mask = n - 1;
        w = 0;
    }
    int capacity() const { return (int)buf.size() - 2; }
    void reset() { std::fill(buf.begin(), buf.end(), 0.f); w = 0; }

    // REALTIME.
    inline void push(f32 x) {
        buf[w] = x;
        w = (w + 1) & mask;
    }
    inline f32 tap(int d) const {
        return buf[(w - (u32)d) & mask];
    }
    // Fractional tap, linear interpolation. Used where the delay time moves
    // (the delay's time smoother, the reverb's modulated allpasses).
    inline f32 tapLerp(f32 d) const {
        const int i = (int)d;
        const f32 fr = d - (f32)i;
        const f32 a = buf[(w - (u32)i) & mask];
        const f32 b = buf[(w - (u32)(i + 1)) & mask];
        return a + (b - a) * fr;
    }
};

// Schroeder allpass around a DelayLine, the reverb's building block.
// Y/X = (z^-N - g) / (1 - g*z^-N): flat magnitude, dispersive phase, which is
// how a plate turns one impulse into a cloud without colouring it.
// REALTIME.
inline f32 allpassTick(DelayLine& l, f32 delay, f32 g, f32 x) {
    const f32 d = l.tapLerp(delay);
    const f32 u = x + g * d;
    l.push(flushDenormal(u));
    return d - g * u;
}

// --- state-variable filter, topology-preserving transform -------------------
// Zavalishin's TPT SVF: one structure that yields lowpass, bandpass and
// highpass from the same two state variables, at the same cutoff and the same
// resonance, for the price of one.
//
// Why this and not another biquad. The auto filter MODULATES its cutoff — that
// is the whole device — and a direct-form biquad whose coefficients are swept
// quickly does not merely sound different, it can leave the stability triangle
// between one sample and the next because its state has no physical meaning
// once the coefficients have moved. The TPT form's state variables are the two
// integrator outputs; they stay meaningful across a coefficient change, which
// is what makes it the standard choice for a filter with an LFO on it.
//
// Coefficients and state are separate types for the same reason the biquad
// splits them: one coefficient set drives every channel, each channel keeps its
// own memory. The cutoff itself is not stored — a1/a2/a3 and k are all the tick
// needs, so a per-sample glide is four adds and nothing else.
struct SvfCoeffs {
    f32 a1 = 1.f, a2 = 0.f, a3 = 0.f;
    f32 k  = 1.f;                       // 1/Q, also the highpass feedback term
};

// `q` is the resonance in the usual sense: 0.7071 is Butterworth, higher rings.
// The frequency is clamped below Nyquist before the tangent, like the RBJ
// sections above and for the same reason.
inline SvfCoeffs svfCoeffs(f64 sr, f32 hz, f32 q) {
    SvfCoeffs c;
    if (sr <= 0.0) return c;
    const f32 fc = clampv(hz, 1.f, (f32)(sr * 0.49));
    // std::tan, not an approximation of it: this is called once per control
    // tick (see kCtrl in the auto filter), not once per sample, so the exact
    // function costs a few thousand calls a second and buys exactness at the
    // top of the range where every cheap tan approximation falls apart.
    const f32 g = (f32)std::tan(3.14159265358979 * (f64)fc / sr);
    c.k  = 1.f / clampv(q, 0.5f, 40.f);
    c.a1 = 1.f / (1.f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

struct SvfState {
    f32 ic1 = 0.f, ic2 = 0.f;
    void reset() { ic1 = ic2 = 0.f; }
    void check() { if (!sane(ic1) || !sane(ic2)) reset(); }
};

struct SvfOut { f32 lp, bp, hp; };

// REALTIME. All three outputs at once; the caller picks. Returning the unused
// two costs nothing — they are already computed, and the compiler drops the
// dead ones at the call site.
inline SvfOut svfTick(const SvfCoeffs& c, SvfState& s, f32 x) {
    const f32 v3 = x - s.ic2;
    const f32 v1 = c.a1 * s.ic1 + c.a2 * v3;
    const f32 v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = flushDenormal(2.f * v1 - s.ic1);
    s.ic2 = flushDenormal(2.f * v2 - s.ic2);
    return { v2, v1, x - c.k * v1 - v2 };
}

// REALTIME. c += d, once per sample, to walk a coefficient set to a target that
// was computed at control rate. Linear in the coefficients rather than in the
// cutoff, which is what makes it four adds.
inline void svfStep(SvfCoeffs& c, const SvfCoeffs& d) {
    c.a1 += d.a1; c.a2 += d.a2; c.a3 += d.a3; c.k += d.k;
}

// (to - from) / n, the per-sample increment of that walk.
inline SvfCoeffs svfSlope(const SvfCoeffs& from, const SvfCoeffs& to, f32 inv) {
    return { (to.a1 - from.a1) * inv, (to.a2 - from.a2) * inv,
             (to.a3 - from.a3) * inv, (to.k  - from.k)  * inv };
}

// --- LFO -------------------------------------------------------------------
// A phasor plus a shape function. The phase is kept separately from the shape
// so one oscillator can be read at two offsets (the auto filter's stereo phase)
// without running two of them and having them drift apart.
struct Lfo {
    f32 phase = 0.f, inc = 0.f;         // phase in cycles, inc in cycles/sample

    void setRate(f64 sr, f32 hz) {
        if (sr <= 0.0 || !(hz > 0.f)) { inc = 0.f; return; }
        inc = clampv((f32)((f64)hz / sr), 0.f, 0.5f);
    }
    void reset() { phase = 0.f; }

    // REALTIME. ONE SAMPLE AT A TIME, deliberately, even though the shape is
    // only sampled at control ticks and `phase += inc*n` would look cheaper.
    // Floating-point addition is not associative: advancing by 16 and then by 4
    // does not land on the same bits as advancing by 20, so an LFO stepped in
    // block-sized jumps produces a different signal at every buffer size. One
    // add per sample is the price of a render that does not depend on the audio
    // interface it was made on. `inc` is capped at 0.5, so one subtraction is
    // always enough to wrap.
    inline void tick() {
        phase += inc;
        if (phase >= 1.f) phase -= 1.f;
        if (!(phase >= 0.f && phase < 1.f)) phase = 0.f;  // a bad rate cannot stick
    }

    // Bipolar -1..1. `ph` may be any non-negative phase; it is wrapped here so
    // callers can add a stereo offset without wrapping it themselves.
    static f32 shape(int s, f32 ph) {
        ph -= (f32)(int)ph;
        if (ph < 0.f) ph += 1.f;
        switch (s) {
            case 1:  return ph < 0.5f ? (4.f * ph - 1.f) : (3.f - 4.f * ph);  // triangle
            case 2:  return 1.f - 2.f * ph;                                   // saw down
            case 3:  return ph < 0.5f ? 1.f : -1.f;                           // square
            default: return std::sin(kTwoPi * ph);                            // sine
        }
    }
};

// --- quadrature oscillator -------------------------------------------------
// A rotating unit vector: one multiply-add per sample for a sine AND a cosine,
// which is what a modulated multi-tap delay needs (every tap is the same
// oscillator read at a different angle, and sin(t+a) falls out of s*cos a +
// c*sin a). Drifts by O(k^2) per sample, so renorm() once per block.
struct Quad {
    f32 s = 0.f, c = 1.f, k = 0.f;

    void setRate(f64 sr, f32 hz) {
        if (sr <= 0.0 || !(hz > 0.f)) { k = 0.f; return; }
        k = clampv((f32)(kTwoPi * (f64)hz / sr), 0.f, 1.f);
    }
    void reset() { s = 0.f; c = 1.f; }
    // REALTIME.
    inline void tick() { s += k * c; c -= k * s; }
    void renorm() {
        const f32 n = s * s + c * c;
        if (n > 1e-6f && n < 4.f) { const f32 g = 1.5f - 0.5f * n; s *= g; c *= g; }
        else { s = 0.f; c = 1.f; }
    }
};

// --- sliding maximum over a fixed window -----------------------------------
// The lookahead limiter's gain computer, and the only structure here that is
// not a filter.
//
// A monotonic wedge: the ring holds the indices of the values that could still
// become the maximum, in decreasing order of value. Every sample is pushed once
// and popped at most once, so the cost is O(1) amortised and — the part that
// matters for an audio thread — BOUNDED per sample by the wedge length, which
// is bounded by the window. The naive alternative (rescan the window) is O(N)
// per sample with N = 240 at 48 kHz, which is 11.5 million comparisons a second
// for an answer that changes hardly at all.
struct SlidingMax {
    std::vector<f32> val;
    std::vector<u64> at;
    u32 mask = 0;
    u64 head = 0, tail = 0;             // wedge occupies [head, tail), masked
    u64 t    = 0;                       // samples pushed so far
    int win  = 1;

    // GUI thread (prepare) only.
    void resize(int window) {
        win = window < 1 ? 1 : window;
        u32 n = 8;
        while (n < (u32)win + 2u) n <<= 1;
        val.assign((size_t)n, 0.f);
        at.assign((size_t)n, 0);
        mask = n - 1;
        reset();
    }
    void reset() { head = tail = 0; t = 0; std::fill(val.begin(), val.end(), 0.f); }

    // REALTIME. Returns max over the last `win` pushes, this one included.
    // `t` is 64-bit on purpose: a 32-bit sample counter wraps after 24 hours at
    // 48 kHz, and the eviction test is an arithmetic comparison on it.
    inline f32 push(f32 x) {
        while (tail > head && val[(size_t)((tail - 1) & mask)] <= x) --tail;
        val[(size_t)(tail & mask)] = x;
        at[(size_t)(tail & mask)]  = t;
        ++tail;
        while (at[(size_t)(head & mask)] + (u64)win <= t) ++head;
        ++t;
        return val[(size_t)(head & mask)];
    }
};

// --- boxcar (moving average) over a fixed window ----------------------------
// The limiter's gain smoother. A running sum, so the per-sample cost is one add
// and one subtract whatever the window length.
//
// The sum is f64 and is rebuilt exactly once per block (resum(), O(window)),
// because a f32 running sum over a session-length signal accumulates rounding
// that never gets a chance to cancel. The rebuild costs about one extra add per
// sample at a 256-frame block and makes the output a function of the last
// `window` samples alone rather than of every sample since prepare().
struct Boxcar {
    std::vector<f32> buf;
    u32 mask = 0, w = 0;
    int n = 1;
    f64 sum = 0.0;

    // GUI thread (prepare) only.
    void resize(int len) {
        n = len < 1 ? 1 : len;
        u32 c = 8;
        while (c < (u32)n + 2u) c <<= 1;
        buf.assign((size_t)c, 0.f);
        mask = c - 1;
        reset();
    }
    void reset() { std::fill(buf.begin(), buf.end(), 0.f); w = 0; sum = 0.0; }
    void resum() {
        f64 s = 0.0;
        for (int i = 1; i <= n; ++i) s += (f64)buf[(size_t)((w - (u32)i) & mask)];
        sum = s;
    }
    // REALTIME.
    inline f32 push(f32 x) {
        sum += (f64)x - (f64)buf[(size_t)((w - (u32)n) & mask)];
        buf[w] = x;
        w = (w + 1) & mask;
        return (f32)(sum / (f64)n);
    }
};

// --- DC blocker ------------------------------------------------------------
// y[n] = x[n] - x[n-1] + R*y[n-1]: the one-zero/one-pole highpass with its zero
// exactly at DC, so what it removes is a constant offset and not the bottom of
// the bass. R is set from a corner frequency; 5 Hz is inaudible and still
// settles a hard offset in well under a second.
struct DcBlock {
    f32 r = 0.9995f, x1 = 0.f, y1 = 0.f;

    void setCutoff(f64 sr, f32 hz) {
        if (sr <= 0.0) { r = 0.9995f; return; }
        r = clampv(1.f - (f32)(kTwoPi * (f64)hz / sr), 0.f, 0.99999f);
    }
    void reset() { x1 = y1 = 0.f; }
    void check() { if (!sane(x1) || !sane(y1)) reset(); }
    // REALTIME.
    inline f32 process(f32 x) {
        const f32 y = x - x1 + r * y1;
        x1 = x;
        y1 = flushDenormal(y);
        return y1;
    }
};

} // namespace dsp
} // namespace lat
