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

} // namespace dsp
} // namespace lat
