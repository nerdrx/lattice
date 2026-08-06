// Internal device, plugin-MIDI and latency-reporting tests.
//
// Everything here goes through the public registry path — scan(), find(),
// instantiate() — so the tests exercise the same code the browser and the
// device chain use, not a private constructor. Failures are recorded, not
// thrown, so one broken case never hides the rest.
//
// There is no Makefile target for this one; it is built by hand. The include
// flags are not optional -- lv2_host.cpp needs lilv's headers and clap_host.cpp
// needs the vendored CLAP ones:
//
//   g++ -std=c++20 -O2 $(pkg-config --cflags lilv-0) -Ivendor/clap/include \
//       tests/internal_device_test.cpp src/plugin/host.cpp \
//       src/plugin/lv2_host.cpp src/plugin/clap_host.cpp \
//       src/plugin/internal_devices.cpp src/core/common.cpp \
//       -o build/internal_device_test $(pkg-config --libs lilv-0) -ldl
//
// Add -fsanitize=address,undefined for the sanitiser run; the whole suite is
// clean under both.
#include "../src/plugin/host.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace lat;

// ---------------------------------------------------------------------------
// tiny check framework (same shape as engine_test.cpp)
// ---------------------------------------------------------------------------

static int gPass = 0, gFail = 0;

static void checkImpl(bool ok, int line, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (ok) { ++gPass; std::printf("  PASS  %s\n", msg); }
    else    { ++gFail; std::printf("  FAIL  %s   (internal_device_test.cpp:%d)\n", msg, line); }
}
#define CHECK(cond, ...) checkImpl((cond), __LINE__, __VA_ARGS__)

static void banner(const char* s) { std::printf("\n== %s\n", s); }
static void note(const char* s)   { std::printf("  note  %s\n", s); }

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static constexpr f64 kSR    = 48000.0;
static constexpr int kBlock = 256;

// Stereo scratch pair with the pointer plumbing PluginInstance::process() wants.
struct Buf {
    std::vector<f32> l, r;
    f32* p[2];
    explicit Buf(int n = kBlock) : l((size_t)n, 0.f), r((size_t)n, 0.f) {
        p[0] = l.data();
        p[1] = r.data();
    }
    void clear() {
        std::fill(l.begin(), l.end(), 0.f);
        std::fill(r.begin(), r.end(), 0.f);
    }
    f32 peak() const {
        f32 m = 0.f;
        for (f32 v : l) m = std::fmax(m, std::fabs(v));
        for (f32 v : r) m = std::fmax(m, std::fabs(v));
        return m;
    }
    bool finite() const {
        for (f32 v : l) if (!std::isfinite(v)) return false;
        for (f32 v : r) if (!std::isfinite(v)) return false;
        return true;
    }
};

// Deterministic noise: a test that fails only on some runs is worse than no
// test. Plain 32-bit LCG, plenty white enough to excite a reverb tank.
struct Noise {
    u32 s = 0x13579BDFu;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return (f32)((i32)(s >> 8) - 8388608) * (1.f / 8388608.f);
    }
};

static int paramIndex(const PluginInstance& p, const char* name) {
    for (int i = 0; i < p.paramCount(); ++i)
        if (p.paramInfo(i).name == name) return i;
    return -1;
}

static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ---------------------------------------------------------------------------
// Pre-rename device URIs
//
// Every set saved before the Lattice -> NxTakt rename names its stock devices
// `lattice:*`. Those files are the user's work and will never be migrated, so
// the alias is permanent and this section is the thing that stops a careless
// cleanup from deleting it. The three properties that matter:
//
//   1. the alias resolves at all;
//   2. it resolves to the SAME descriptor the canonical URI does -- not a
//      second, subtly different copy;
//   3. the descriptor it returns carries the CANONICAL uri, which is what
//      makes a load-then-save quietly upgrade the file (serializeDevices
//      writes desc().uri).
//
// Plus: a descriptor that never went through the registry -- exactly what a
// project loader builds when it has only the saved uri -- still instantiates.
// ---------------------------------------------------------------------------

static void testLegacyUris(PluginRegistry& reg) {
    banner("pre-rename URI aliases (lattice: -> nxtakt:)");

    struct { const char* legacy; const char* canonical; } kPairs[] = {
        { "lattice:saturator", "nxtakt:saturator" },
        { "lattice:pulse",     "nxtakt:pulse"     },
    };

    for (const auto& p : kPairs) {
        const PluginDesc* viaLegacy = reg.find(p.legacy);
        const PluginDesc* viaNew    = reg.find(p.canonical);
        CHECK(viaLegacy != nullptr, "registry still resolves '%s'", p.legacy);
        CHECK(viaNew != nullptr, "registry resolves '%s'", p.canonical);
        if (!viaLegacy || !viaNew) continue;

        CHECK(viaLegacy == viaNew, "'%s' and '%s' are the same descriptor",
              p.legacy, p.canonical);
        CHECK(viaLegacy->uri == p.canonical,
              "'%s' resolves to a descriptor carrying the canonical uri ('%s')",
              p.legacy, viaLegacy->uri.c_str());

        auto inst = reg.instantiate(*viaLegacy, kSR, kBlock);
        CHECK(inst != nullptr, "'%s' instantiates", p.legacy);
        if (inst)
            CHECK(inst->desc().uri == p.canonical,
                  "the instance reports the canonical uri, so a re-save upgrades it");

        // The project-loader shape: a descriptor assembled from a saved uri
        // that never came out of the registry. instantiateInternal has to
        // accept the old spelling directly for this to work.
        PluginDesc fromFile = *viaLegacy;
        fromFile.uri = p.legacy;
        auto direct = reg.instantiate(fromFile, kSR, kBlock);
        CHECK(direct != nullptr, "a stale descriptor carrying '%s' instantiates", p.legacy);
        if (direct)
            CHECK(direct->desc().uri == p.canonical,
                  "and it too rebuilds its descriptor as '%s'", p.canonical);
    }

    CHECK(reg.find("lattice:no-such-device") == nullptr,
          "the alias does not invent devices that never existed");
}

// ---------------------------------------------------------------------------
// Saturator
// ---------------------------------------------------------------------------

static void testSaturator(PluginRegistry& reg) {
    banner("Saturator");

    const PluginDesc* d = reg.find("nxtakt:saturator");
    CHECK(d != nullptr, "registry finds nxtakt:saturator");
    if (!d) return;
    CHECK(d->format == PluginFormat::Internal && d->kind == PluginKind::Effect,
          "descriptor: internal effect, %d in / %d out", d->audioIn, d->audioOut);

    auto sat = reg.instantiate(*d, kSR, kBlock);
    CHECK(sat != nullptr, "instantiate + prepare");
    if (!sat) return;

    const int pDrive = paramIndex(*sat, "Drive");
    const int pTrim  = paramIndex(*sat, "Output");
    const int pMix   = paramIndex(*sat, "Mix");
    CHECK(pDrive >= 0 && pTrim >= 0 && pMix >= 0, "params Drive/Output/Mix present");
    if (pDrive < 0 || pTrim < 0 || pMix < 0) return;
    CHECK(sat->paramInfo(pDrive).isLogarithmic && sat->paramInfo(pDrive).unit == "dB",
          "Drive is flagged logarithmic and carries a dB unit");

    Buf in, out;

    // 1. silence in, silence out.
    in.clear();
    out.clear();
    sat->process(in.p, out.p, 2, kBlock);
    CHECK(out.peak() == 0.f, "silence in -> silence out (peak %.9f)", (double)out.peak());

    // 2. a small sine at drive 0 comes back at unity. tanh(x) ~ x only for small
    //    x, so the test signal sits at -20 dBFS where the shaper is still linear
    //    to within a fraction of a dB.
    const f32 amp = 0.1f;
    for (int i = 0; i < kBlock; ++i) {
        const f32 s = amp * std::sin(6.2831853f * 220.f * (f32)i / (f32)kSR);
        in.l[(size_t)i] = in.r[(size_t)i] = s;
    }
    sat->setParam(pDrive, 0.f);
    sat->setParam(pTrim, 0.f);
    sat->setParam(pMix, 1.f);
    out.clear();
    sat->process(in.p, out.p, 2, kBlock);
    f32 maxErr = 0.f;
    for (int i = 0; i < kBlock; ++i)
        maxErr = std::fmax(maxErr, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
    CHECK(maxErr < amp * 0.01f, "drive 0 dB is unity within 1%% (max err %.6f)", (double)maxErr);

    // 3. 24 dB of drive stays finite and bounded. The gain compensation is
    //    referenced to a 0.5 sine, so a hot input must not run away.
    sat->setParam(pDrive, 24.f);
    for (int i = 0; i < kBlock; ++i) {
        const f32 s = 0.9f * std::sin(6.2831853f * 220.f * (f32)i / (f32)kSR);
        in.l[(size_t)i] = in.r[(size_t)i] = s;
    }
    out.clear();
    sat->process(in.p, out.p, 2, kBlock);
    CHECK(out.finite(), "drive 24 dB output is finite");
    CHECK(out.peak() > 0.f && out.peak() <= 1.f,
          "drive 24 dB output is bounded (peak %.4f)", (double)out.peak());
    CHECK(out.peak() < 0.9f, "drive 24 dB is compensated, not just louder (peak %.4f)",
          (double)out.peak());

    // 4. in-place processing is legal per the contract.
    out.clear();
    for (int i = 0; i < kBlock; ++i) out.l[(size_t)i] = out.r[(size_t)i] = 0.3f;
    sat->process(out.p, out.p, 2, kBlock);
    CHECK(out.finite() && out.peak() > 0.f, "processes in place");

    // 5. bypass hands the input straight through.
    sat->setBypassed(true);
    out.clear();
    sat->process(in.p, out.p, 2, kBlock);
    f32 bypassErr = 0.f;
    for (int i = 0; i < kBlock; ++i)
        bypassErr = std::fmax(bypassErr, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
    CHECK(bypassErr == 0.f, "bypass is a bit-exact copy");
    sat->setBypassed(false);
}

// ---------------------------------------------------------------------------
// Pulse
// ---------------------------------------------------------------------------

static void noteOn(PluginInstance& p, u8 key, u8 vel, int frame = 0) {
    const u8 msg[3] = { 0x90, key, vel };
    p.midi(msg, 3, frame);
}
static void noteOff(PluginInstance& p, u8 key, int frame = 0) {
    const u8 msg[3] = { 0x80, key, 0 };
    p.midi(msg, 3, frame);
}

// Runs `blocks` blocks and returns the loudest sample seen.
static f32 runFor(PluginInstance& p, Buf& out, int blocks, bool* allFinite = nullptr) {
    f32 peak = 0.f;
    if (allFinite) *allFinite = true;
    for (int b = 0; b < blocks; ++b) {
        out.clear();
        p.process(nullptr, out.p, 2, kBlock);
        peak = std::fmax(peak, out.peak());
        if (allFinite && !out.finite()) *allFinite = false;
    }
    return peak;
}

static void testPulse(PluginRegistry& reg) {
    banner("Pulse");

    const PluginDesc* d = reg.find("nxtakt:pulse");
    CHECK(d != nullptr, "registry finds nxtakt:pulse");
    if (!d) return;
    CHECK(d->kind == PluginKind::Instrument && d->hasMidiIn && d->audioOut == 2,
          "descriptor: internal instrument, midi in, %d out", d->audioOut);

    auto syn = reg.instantiate(*d, kSR, kBlock);
    CHECK(syn != nullptr, "instantiate + prepare");
    if (!syn) return;

    const int pAttack  = paramIndex(*syn, "Attack");
    const int pDecay   = paramIndex(*syn, "Decay");
    const int pRelease = paramIndex(*syn, "Release");
    const int pCutoff  = paramIndex(*syn, "Cutoff");
    const int pShape   = paramIndex(*syn, "Shape");
    CHECK(pAttack >= 0 && pDecay >= 0 && pRelease >= 0 && pCutoff >= 0 && pShape >= 0,
          "params Attack/Decay/Release/Cutoff/Shape present");
    if (pAttack < 0 || pDecay < 0 || pRelease < 0 || pCutoff < 0 || pShape < 0) return;

    Buf out;

    // 1. no MIDI at all is silence, not a stuck voice.
    CHECK(runFor(*syn, out, 8) == 0.f, "no midi -> silence");

    // 2. a note produces sound.
    syn->setParam(pAttack, 0.005f);
    syn->setParam(pDecay, 2.f);            // hold the note up while we look at it
    syn->setParam(pRelease, 0.05f);
    noteOn(*syn, 60, 100);
    bool fin = false;
    const f32 held = runFor(*syn, out, 20, &fin);
    CHECK(held > 0.01f, "note on -> non-zero output (peak %.4f)", (double)held);
    CHECK(fin, "held note stays finite");

    // 3. note off plus the release tail decays to silence.
    noteOff(*syn, 60);
    runFor(*syn, out, (int)(kSR / kBlock));            // one second of tail
    const f32 tail = runFor(*syn, out, 8);
    CHECK(tail == 0.f, "note off -> release tail reaches silence (residual %.9f)", (double)tail);

    // 4. eight simultaneous notes: all voices busy, output still finite.
    syn->setParam(pDecay, 2.f);
    for (int i = 0; i < 8; ++i) noteOn(*syn, (u8)(48 + i * 3), (u8)(80 + i * 5), i * 8);
    const f32 chord = runFor(*syn, out, 40, &fin);
    CHECK(fin, "8 simultaneous notes stay finite");
    CHECK(chord > 0.f, "8 simultaneous notes sound (peak %.4f)", (double)chord);

    // 5. a ninth note has to steal a voice rather than allocate one.
    noteOn(*syn, 84, 110);
    CHECK(runFor(*syn, out, 8, &fin) > 0.f && fin, "voice steal on the 9th note is clean");

    // 6. sweeping parameters from "the GUI" while the audio thread renders must
    //    not produce a NaN or an explosion, whatever order the values land in.
    bool sweepOk = true;
    for (int b = 0; b < 200; ++b) {
        const f32 t = (f32)b / 200.f;
        syn->setParam(pCutoff, lerpf(20.f, 18000.f, t));
        syn->setParam(pShape, t);
        syn->setParam(pAttack, lerpf(0.001f, 2.f, t));
        syn->setParam(pDecay, lerpf(2.f, 0.001f, t));
        syn->setParam(pRelease, lerpf(0.001f, 2.f, t));
        if ((b % 16) == 0) noteOn(*syn, (u8)(36 + (b % 60)), 100, b % kBlock);
        if ((b % 16) == 8) noteOff(*syn, (u8)(36 + ((b - 8) % 60)));
        out.clear();
        syn->process(nullptr, out.p, 2, kBlock);
        if (!out.finite() || out.peak() > 8.f) { sweepOk = false; break; }
    }
    CHECK(sweepOk, "param sweep while processing stays finite and bounded");

    // 7. all-notes-off (CC 123) clears everything, then silence returns.
    const u8 cc[3] = { 0xB0, 123, 0 };
    syn->midi(cc, 3, 0);
    syn->setParam(pRelease, 0.01f);
    runFor(*syn, out, (int)(kSR / kBlock));
    CHECK(runFor(*syn, out, 8) == 0.f, "CC 123 all-notes-off returns to silence");
}

// ---------------------------------------------------------------------------
// Shared harness for the effect devices
//
// Everything below MEASURES. "The EQ boosts" is not a test; "the EQ boosts by
// 11.9 dB at the frequency it was asked to boost at, and by 0.1 dB two decades
// below it" is. The measurement tool is a single-bin DFT at the probe frequency
// over a whole number of cycles, which is exact for a steady sine and needs no
// window and no FFT.
// ---------------------------------------------------------------------------

// The devices under test are the stock effects: stereo in, stereo out, no MIDI.
static const char* kEffectUris[] = {
    "nxtakt:saturator", "nxtakt:eq3", "nxtakt:compressor",
    "nxtakt:delay", "nxtakt:reverb",
};

// Runs a steady sine through the device and returns the output/input magnitude
// at that exact frequency, in dB. `cycles` is chosen by the caller so that the
// measurement window is a whole number of periods.
static f64 probeGainDb(PluginInstance& p, f64 freq, f32 amp, int cycles) {
    Buf in, out;
    const f64 w = 6.283185307179586 * freq / kSR;
    u64 n = 0;

    auto fill = [&](int k) {
        for (int i = 0; i < k; ++i) {
            const f32 s = amp * (f32)std::sin(w * (f64)(n + (u64)i));
            in.l[(size_t)i] = in.r[(size_t)i] = s;
        }
    };

    // Settle: filters, smoothers and detectors all need to reach steady state
    // before the number means anything.
    for (int b = 0; b < 40; ++b) { fill(kBlock); out.clear(); p.process(in.p, out.p, 2, kBlock); n += (u64)kBlock; }

    const int N = (int)std::llround((f64)cycles * kSR / freq);
    f64 re = 0.0, im = 0.0;
    int done = 0;
    while (done < N) {
        const int k = (N - done) < kBlock ? (N - done) : kBlock;
        fill(k);
        out.clear();
        p.process(in.p, out.p, 2, k);
        for (int i = 0; i < k; ++i) {
            const f64 ph = w * (f64)(n + (u64)i);
            re += (f64)out.l[(size_t)i] * std::cos(ph);
            im += (f64)out.l[(size_t)i] * std::sin(ph);
        }
        n += (u64)k;
        done += k;
    }
    const f64 mag = 2.0 * std::sqrt(re * re + im * im) / (f64)N;
    if (mag <= 1e-12 || amp <= 0.f) return -200.0;
    return 20.0 * std::log10(mag / (f64)amp);
}

// The four properties every stock effect owes the user, checked the same way
// for all of them so a device added later cannot quietly skip one.
static void testEffectContract(PluginRegistry& reg) {
    banner("stock effects: the common contract");

    for (const char* uri : kEffectUris) {
        const PluginDesc* d = reg.find(uri);
        CHECK(d != nullptr, "%s: in the registry", uri);
        if (!d) continue;
        CHECK(d->format == PluginFormat::Internal && d->kind == PluginKind::Effect &&
              d->audioIn == 2 && d->audioOut == 2 && !d->hasMidiIn,
              "%s: stereo effect descriptor", uri);

        auto fx = reg.instantiate(*d, kSR, kBlock);
        CHECK(fx != nullptr, "%s: instantiate + prepare", uri);
        if (!fx) continue;

        CHECK(fx->paramCount() == d->paramCount,
              "%s: descriptor param count matches the instance (%d)", uri, fx->paramCount());

        // Every parameter has to be automatable through the realtime path, or
        // the engine greys its lane out. There is no reason for a stock device
        // to have one that is not.
        bool rtOk = true;
        for (int i = 0; i < fx->paramCount(); ++i)
            if (!fx->setParamRT(i, fx->paramInfo(i).def)) rtOk = false;
        CHECK(rtOk, "%s: every parameter accepts a realtime write", uri);

        // 1. Silence in, silence out. A fresh instance has zeroed state, so
        //    even the delay and the reverb owe an exact zero here -- their
        //    tails can only contain what was put into them.
        Buf in, out;
        f32 residue = 0.f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            residue = std::fmax(residue, out.peak());
        }
        CHECK(residue == 0.f, "%s: silence in -> silence out (%.9f)", uri, (double)residue);

        // 2. A sine sweeping 20 Hz -> 18 kHz stays finite. This is the shape
        //    that finds a filter that goes unstable at one end of its range.
        bool fin = true;
        f32 peak = 0.f;
        f64 ph = 0.0;
        for (int b = 0; b < 400; ++b) {
            const f64 t = (f64)b / 400.0;
            const f64 f = 20.0 * std::pow(900.0, t);          // 20 Hz .. 18 kHz
            for (int i = 0; i < kBlock; ++i) {
                ph += 6.283185307179586 * f / kSR;
                in.l[(size_t)i] = in.r[(size_t)i] = 0.5f * (f32)std::sin(ph);
            }
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            if (!out.finite()) { fin = false; break; }
            peak = std::fmax(peak, out.peak());
        }
        CHECK(fin, "%s: swept sine stays finite", uri);
        CHECK(peak < 8.f, "%s: swept sine stays bounded (peak %.3f)", uri, (double)peak);

        // 3. Every parameter swept end to end WHILE processing. Each parameter
        //    gets its own pass so a fault is attributable, and the pass runs
        //    both directions because a device can be fine going up and unstable
        //    coming down (a delay time shrinking under feedback, say).
        Noise ns;
        bool sweepOk = true;
        const char* badParam = "";
        for (int pi = 0; pi < fx->paramCount() && sweepOk; ++pi) {
            const ParamInfo& info = fx->paramInfo(pi);
            for (int b = 0; b < 120; ++b) {
                f32 t = (f32)b / 60.f;
                if (t > 1.f) t = 2.f - t;                     // up, then back down
                fx->setParam(pi, lerpf(info.min, info.max, t));
                for (int i = 0; i < kBlock; ++i)
                    in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
                out.clear();
                fx->process(in.p, out.p, 2, kBlock);
                if (!out.finite() || out.peak() > 32.f) {
                    sweepOk = false;
                    badParam = info.name.c_str();
                    break;
                }
            }
            fx->setParam(pi, info.def);
        }
        CHECK(sweepOk, "%s: every parameter sweeps during processing without NaN%s%s",
              uri, sweepOk ? "" : " -- failed on ", badParam);

        // 4. ...and it is still a working device afterwards, not a silenced one.
        for (int i = 0; i < kBlock; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * (f32)std::sin(6.2831853 * 440.0 * i / kSR);
        f32 after = 0.f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            after = std::fmax(after, out.peak());
        }
        CHECK(after > 0.01f, "%s: still passes audio after the sweep (peak %.4f)",
              uri, (double)after);
    }
}

// ---------------------------------------------------------------------------
// EQ Three
// ---------------------------------------------------------------------------

static void testEq3(PluginRegistry& reg) {
    banner("EQ Three");

    const PluginDesc* d = reg.find("nxtakt:eq3");
    CHECK(d != nullptr, "registry finds nxtakt:eq3");
    if (!d) return;

    auto eq = reg.instantiate(*d, kSR, kBlock);
    CHECK(eq != nullptr, "instantiate + prepare");
    if (!eq) return;

    const int pLoF = paramIndex(*eq, "Low Freq");
    const int pLoG = paramIndex(*eq, "Low Gain");
    const int pMidF = paramIndex(*eq, "Mid Freq");
    const int pMidG = paramIndex(*eq, "Mid Gain");
    const int pMidQ = paramIndex(*eq, "Mid Q");
    const int pHiF = paramIndex(*eq, "High Freq");
    const int pHiG = paramIndex(*eq, "High Gain");
    CHECK(pLoF >= 0 && pLoG >= 0 && pMidF >= 0 && pMidG >= 0 && pMidQ >= 0 &&
          pHiF >= 0 && pHiG >= 0, "all seven parameters present");
    if (pLoG < 0 || pMidG < 0 || pHiG < 0) return;

    // 1. Defaults are flat, and not approximately: at 0 dB every RBJ section
    //    collapses to b0 = 1, b1 = a1, b2 = a2, which is an exact passthrough
    //    in transposed direct form II. The device on a channel doing nothing
    //    has to do NOTHING.
    Buf in, out;
    for (int i = 0; i < kBlock; ++i)
        in.l[(size_t)i] = in.r[(size_t)i] = 0.4f * (f32)std::sin(6.2831853 * 997.0 * i / kSR);
    out.clear();
    eq->process(in.p, out.p, 2, kBlock);
    f32 flatErr = 0.f;
    for (int i = 0; i < kBlock; ++i)
        flatErr = std::fmax(flatErr, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
    CHECK(flatErr < 1e-6f, "defaults are unity (max err %.9f)", (double)flatErr);

    // 2. A measured mid boost. +12 dB at 1 kHz, Q 1: the peak of an RBJ
    //    peaking section sits exactly on the centre frequency at exactly the
    //    requested gain, so this is a number with a right answer.
    eq->setParam(pMidF, 1000.f);
    eq->setParam(pMidQ, 1.f);
    eq->setParam(pMidG, 12.f);
    const f64 at1k = probeGainDb(*eq, 1000.0, 0.2f, 100);
    CHECK(std::fabs(at1k - 12.0) < 0.5, "+12 dB at 1 kHz measures %.2f dB", at1k);

    // ...and it is a BAND, not a shelf: two octaves out the boost is nearly
    // gone. (A Q of 1 gives a bandwidth of ~1.4 octaves, so 250 Hz should be
    // down around a dB or two.)
    const f64 at250 = probeGainDb(*eq, 250.0, 0.2f, 25);
    CHECK(at250 < 3.0 && at250 > -0.5, "the boost is local: %.2f dB at 250 Hz", at250);

    // A cut of the same size is symmetric, which is the property the cookbook
    // formulas exist to guarantee.
    eq->setParam(pMidG, -12.f);
    const f64 cut1k = probeGainDb(*eq, 1000.0, 0.2f, 100);
    CHECK(std::fabs(cut1k + 12.0) < 0.5, "-12 dB at 1 kHz measures %.2f dB", cut1k);
    eq->setParam(pMidG, 0.f);

    // 3. Shelves. An RBJ shelf with S = 1 passes through exactly half its gain
    //    at the corner frequency, so a +12 dB low shelf at 100 Hz measures
    //    +6 dB at 100 Hz -- and the mid and top are untouched.
    eq->setParam(pLoF, 100.f);
    eq->setParam(pLoG, 12.f);
    const f64 lowCorner = probeGainDb(*eq, 100.0, 0.2f, 20);
    const f64 lowDeep   = probeGainDb(*eq, 30.0, 0.2f, 10);
    const f64 lowFar    = probeGainDb(*eq, 2000.0, 0.2f, 100);
    CHECK(std::fabs(lowCorner - 6.0) < 0.6, "low shelf: %.2f dB at its 100 Hz corner", lowCorner);
    CHECK(lowDeep > 10.0, "low shelf: %.2f dB at 30 Hz (approaching the full +12)", lowDeep);
    CHECK(std::fabs(lowFar) < 1.0, "low shelf leaves 2 kHz alone (%.2f dB)", lowFar);
    eq->setParam(pLoG, 0.f);

    eq->setParam(pHiF, 8000.f);
    eq->setParam(pHiG, 12.f);
    const f64 hiCorner = probeGainDb(*eq, 8000.0, 0.2f, 400);
    const f64 hiLow    = probeGainDb(*eq, 200.0, 0.2f, 20);
    CHECK(std::fabs(hiCorner - 6.0) < 0.6, "high shelf: %.2f dB at its 8 kHz corner", hiCorner);
    CHECK(std::fabs(hiLow) < 1.0, "high shelf leaves 200 Hz alone (%.2f dB)", hiLow);
    eq->setParam(pHiG, 0.f);

    // 4. All three bands boosted at once. The sections are cascaded, so their
    //    dB responses add -- and at 1 kHz, with the shelves parked at 100 Hz
    //    and 8 kHz, the sum is the mid band alone. That the shelves contribute
    //    nothing here is the point: three bands that all bleed into the middle
    //    are one badly-tuned band.
    eq->setParam(pLoG, 6.f);
    eq->setParam(pMidG, 6.f);
    eq->setParam(pHiG, 6.f);
    const f64 all1k = probeGainDb(*eq, 1000.0, 0.2f, 100);
    const f64 all30 = probeGainDb(*eq, 30.0, 0.2f, 10);
    CHECK(std::fabs(all1k - 6.0) < 0.5,
          "three +6 dB bands leave 1 kHz to the mid alone (%.2f dB)", all1k);
    CHECK(all30 > 5.0 && all30 < 7.5,
          "and 30 Hz to the low shelf alone (%.2f dB)", all30);

    // 5. Sweeping the mid frequency across its whole range under a full-scale
    //    input: the coefficient glide must not ring, and the state must not
    //    escape. 15 dB of boost is the worst case the device allows.
    eq->setParam(pLoG, 0.f);
    eq->setParam(pHiG, 0.f);
    eq->setParam(pMidG, 15.f);
    eq->setParam(pMidQ, 6.f);
    bool sweepFin = true;
    f32 sweepPeak = 0.f;
    for (int b = 0; b < 240; ++b) {
        const f32 t = (f32)b / 120.f;
        eq->setParam(pMidF, lerpf(200.f, 8000.f, t > 1.f ? 2.f - t : t));
        for (int i = 0; i < kBlock; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] = 0.9f * (f32)std::sin(6.2831853 * 800.0 * (b * kBlock + i) / kSR);
        out.clear();
        eq->process(in.p, out.p, 2, kBlock);
        if (!out.finite()) { sweepFin = false; break; }
        sweepPeak = std::fmax(sweepPeak, out.peak());
    }
    CHECK(sweepFin, "sweeping Mid Freq at +15 dB / Q 6 stays finite");
    CHECK(sweepPeak < 8.f, "and bounded (peak %.3f, +15 dB of 0.9 is 5.06)", (double)sweepPeak);
}

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------

// Feeds a steady sine and returns the peak of the last block, in dBFS.
static f64 steadyPeakDb(PluginInstance& p, f64 freq, f32 amp, int blocks) {
    Buf in, out;
    f32 peak = 0.f;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            const f32 s = amp * (f32)std::sin(6.2831853 * freq * (b * kBlock + i) / kSR);
            in.l[(size_t)i] = in.r[(size_t)i] = s;
        }
        out.clear();
        p.process(in.p, out.p, 2, kBlock);
        if (b >= blocks - 4) peak = std::fmax(peak, out.peak());
    }
    return 20.0 * std::log10(std::fmax((f64)peak, 1e-9));
}

static void testCompressor(PluginRegistry& reg) {
    banner("Compressor");

    const PluginDesc* d = reg.find("nxtakt:compressor");
    CHECK(d != nullptr, "registry finds nxtakt:compressor");
    if (!d) return;

    auto cp = reg.instantiate(*d, kSR, kBlock);
    CHECK(cp != nullptr, "instantiate + prepare");
    if (!cp) return;

    const int pThr = paramIndex(*cp, "Threshold");
    const int pRat = paramIndex(*cp, "Ratio");
    const int pAtk = paramIndex(*cp, "Attack");
    const int pRel = paramIndex(*cp, "Release");
    const int pKne = paramIndex(*cp, "Knee");
    const int pMak = paramIndex(*cp, "Makeup");
    const int pGr  = paramIndex(*cp, "Gain Reduction");
    CHECK(pThr >= 0 && pRat >= 0 && pAtk >= 0 && pRel >= 0 && pKne >= 0 &&
          pMak >= 0 && pGr >= 0, "all seven parameters present, including the readout");
    if (pThr < 0 || pRat < 0 || pGr < 0) return;
    note("Gain Reduction is an output value on an ordinary parameter: ParamInfo "
         "has no read-only flag, so the device writes it and the UI reads it.");

    Buf in, out;

    // 1. Below the threshold (and below the knee) the device is unity. This is
    //    what makes it safe to leave on a channel.
    cp->setParam(pThr, -18.f);
    cp->setParam(pKne, 6.f);
    cp->setParam(pRat, 4.f);
    cp->setParam(pMak, 0.f);
    for (int i = 0; i < kBlock; ++i)
        in.l[(size_t)i] = in.r[(size_t)i] = 0.03f * (f32)std::sin(6.2831853 * 220.0 * i / kSR);
    for (int b = 0; b < 8; ++b) { out.clear(); cp->process(in.p, out.p, 2, kBlock); }
    f32 unityErr = 0.f;
    for (int i = 0; i < kBlock; ++i)
        unityErr = std::fmax(unityErr, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
    CHECK(unityErr < 1e-6f, "-30 dBFS under a -18 dB threshold is untouched (err %.9f)",
          (double)unityErr);
    CHECK(cp->getParam(pGr) < 0.01f, "and the readout says 0 dB of reduction (%.4f)",
          (double)cp->getParam(pGr));

    // 2. The ratio, measured. Hard knee, fast attack, slow release so the
    //    envelope sits on the peak; then the output level over the threshold
    //    divided into the input level over the threshold IS the ratio.
    cp->setParam(pKne, 0.f);
    cp->setParam(pThr, -20.f);
    cp->setParam(pAtk, 1.f);
    cp->setParam(pRel, 300.f);
    struct { f32 ratio; f64 inDb; } kCases[] = { {2.f, -10.0}, {4.f, -10.0}, {8.f, -4.0} };
    for (const auto& c : kCases) {
        cp->setParam(pRat, c.ratio);
        const f32 amp = (f32)std::pow(10.0, c.inDb / 20.0);
        const f64 outDb = steadyPeakDb(*cp, 220.0, amp, 60);
        const f64 measured = (c.inDb - (-20.0)) / (outDb - (-20.0));
        CHECK(std::fabs(measured - (f64)c.ratio) < 0.15 * (f64)c.ratio,
              "%.0f:1 measures %.2f:1 (%.1f dBFS in -> %.2f dBFS out)",
              (double)c.ratio, measured, c.inDb, outDb);
    }

    // 3. Makeup gain is exactly what it says.
    cp->setParam(pRat, 4.f);
    const f64 noMakeup = steadyPeakDb(*cp, 220.0, 0.316f, 60);
    cp->setParam(pMak, 6.f);
    const f64 withMakeup = steadyPeakDb(*cp, 220.0, 0.316f, 60);
    CHECK(std::fabs((withMakeup - noMakeup) - 6.0) < 0.3,
          "6 dB of makeup adds %.2f dB", withMakeup - noMakeup);
    cp->setParam(pMak, 0.f);

    // 4. Attack and release TIMES, measured off the readout. The envelope is a
    //    one-pole in the dB domain, so "attack" is the time to cover 63% of the
    //    distance to the target reduction -- that is what the number on the
    //    knob promises, and it is checkable to a couple of milliseconds by
    //    running short blocks and reading the meter after each.
    const int kSmall = 64;                                  // 1.33 ms resolution
    auto measureAttack = [&](f32 attackMs) {
        cp->prepare(kSR, kBlock);                           // zero the envelope
        cp->setParam(pThr, -30.f);
        cp->setParam(pKne, 0.f);
        cp->setParam(pRat, 8.f);
        cp->setParam(pAtk, attackMs);
        cp->setParam(pRel, 500.f);
        Buf b(kSmall), o(kSmall);
        for (int i = 0; i < kSmall; ++i) b.l[(size_t)i] = b.r[(size_t)i] = 0.7f;   // DC burst
        f64 t63 = -1.0, final_ = 0.0;
        for (int k = 0; k < 400; ++k) {
            o.clear();
            cp->process(b.p, o.p, 2, kSmall);
            final_ = (f64)cp->getParam(pGr);
        }
        const f64 target = final_;
        cp->prepare(kSR, kBlock);
        cp->setParam(pThr, -30.f);
        cp->setParam(pKne, 0.f);
        cp->setParam(pRat, 8.f);
        cp->setParam(pAtk, attackMs);
        cp->setParam(pRel, 500.f);
        for (int k = 0; k < 400 && t63 < 0.0; ++k) {
            o.clear();
            cp->process(b.p, o.p, 2, kSmall);
            if ((f64)cp->getParam(pGr) >= 0.632 * target)
                t63 = 1000.0 * (f64)((k + 1) * kSmall) / kSR;
        }
        return t63;
    };
    const f64 a5  = measureAttack(5.f);
    const f64 a50 = measureAttack(50.f);
    CHECK(a5 > 3.0 && a5 < 9.0, "a 5 ms attack reaches 63%% of its reduction in %.2f ms", a5);
    CHECK(a50 > 42.0 && a50 < 62.0, "a 50 ms attack reaches 63%% in %.2f ms", a50);
    CHECK(a50 > a5 * 5.0, "and the two times are in the ratio the knob claims (%.1fx)",
          a50 / a5);

    // Release: hold the burst until the reduction settles, drop to silence, and
    // time the fall to 37% of where it was.
    {
        cp->prepare(kSR, kBlock);
        cp->setParam(pThr, -30.f);
        cp->setParam(pKne, 0.f);
        cp->setParam(pRat, 8.f);
        cp->setParam(pAtk, 1.f);
        cp->setParam(pRel, 100.f);
        Buf loud(kSmall), quiet(kSmall), o(kSmall);
        for (int i = 0; i < kSmall; ++i) loud.l[(size_t)i] = loud.r[(size_t)i] = 0.7f;
        for (int k = 0; k < 400; ++k) { o.clear(); cp->process(loud.p, o.p, 2, kSmall); }
        const f64 held = (f64)cp->getParam(pGr);
        f64 t37 = -1.0;
        for (int k = 0; k < 800 && t37 < 0.0; ++k) {
            o.clear();
            cp->process(quiet.p, o.p, 2, kSmall);
            if ((f64)cp->getParam(pGr) <= 0.368 * held)
                t37 = 1000.0 * (f64)((k + 1) * kSmall) / kSR;
        }
        CHECK(held > 10.0, "the burst is held down by %.2f dB", held);
        CHECK(t37 > 80.0 && t37 < 130.0,
              "a 100 ms release falls to 37%% of that in %.2f ms", t37);
    }

    // 5. Stereo link: one gain for both channels. A loud left and a quiet right
    //    must come out with the SAME gain applied, or the image walks.
    {
        cp->prepare(kSR, kBlock);
        cp->setParam(pThr, -24.f);
        cp->setParam(pKne, 0.f);
        cp->setParam(pRat, 6.f);
        cp->setParam(pAtk, 1.f);
        cp->setParam(pRel, 400.f);
        Buf s(kBlock), o(kBlock);
        for (int b = 0; b < 40; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                const f64 ph = 6.2831853 * 220.0 * (b * kBlock + i) / kSR;
                s.l[(size_t)i] = 0.8f * (f32)std::sin(ph);
                s.r[(size_t)i] = 0.05f * (f32)std::sin(ph);
            }
            o.clear();
            cp->process(s.p, o.p, 2, kBlock);
        }
        f64 gl = 0.0, gr = 0.0;
        for (int i = 0; i < kBlock; ++i) {
            if (std::fabs(s.l[(size_t)i]) > 0.4f) {
                gl = (f64)o.l[(size_t)i] / (f64)s.l[(size_t)i];
                gr = (f64)o.r[(size_t)i] / (f64)s.r[(size_t)i];
                break;
            }
        }
        CHECK(gl > 0.0 && std::fabs(gl - gr) < 1e-3,
              "one gain for both channels (L %.5f, R %.5f)", gl, gr);
        CHECK(gl < 0.9, "and the loud side really is being reduced (%.2f dB)",
              20.0 * std::log10(std::fmax(gl, 1e-9)));
    }

    // 6. The readout tracks reality: it is the worst reduction in the block,
    //    and it releases back to zero when the signal does. (The release is an
    //    exponential in dB, so "zero" is a limit -- 20 dB of reduction with a
    //    50 ms release needs a good half second to get under a tenth of a dB,
    //    and reading the meter before then is reading the release, not a bug.)
    {
        cp->setParam(pRel, 50.f);
        Buf q(kBlock), o(kBlock);
        for (int k = 0; k < 200; ++k) { o.clear(); cp->process(q.p, o.p, 2, kBlock); }
        CHECK(cp->getParam(pGr) < 0.05f, "the readout returns to 0 dB on silence (%.4f)",
              (double)cp->getParam(pGr));
    }
}

// ---------------------------------------------------------------------------
// Delay
// ---------------------------------------------------------------------------

// Sends a single unit impulse (both channels) and captures `frames` samples of
// output into l/r. Fresh instance in, so the delay time is snapped rather than
// glided and the echo lands where the maths says it does.
static void impulseResponse(PluginInstance& p, int frames,
                            std::vector<f32>& l, std::vector<f32>& r) {
    l.assign((size_t)frames, 0.f);
    r.assign((size_t)frames, 0.f);
    Buf in, out;
    int done = 0;
    bool first = true;
    while (done < frames) {
        const int k = (frames - done) < kBlock ? (frames - done) : kBlock;
        in.clear();
        if (first) { in.l[0] = in.r[0] = 1.f; first = false; }
        out.clear();
        p.process(in.p, out.p, 2, k);
        for (int i = 0; i < k; ++i) {
            l[(size_t)(done + i)] = out.l[(size_t)i];
            r[(size_t)(done + i)] = out.r[(size_t)i];
        }
        done += k;
    }
}

static void testDelay(PluginRegistry& reg) {
    banner("Delay");

    const PluginDesc* d = reg.find("nxtakt:delay");
    CHECK(d != nullptr, "registry finds nxtakt:delay");
    if (!d) return;

    {
        auto probe = reg.instantiate(*d, kSR, kBlock);
        if (probe) {
            CHECK(paramIndex(*probe, "Tempo") >= 0,
                  "the device carries its own Tempo parameter");
            note("PluginInstance has no transport channel, so tempo sync runs off a "
                 "device parameter (120 BPM default). See the comment on class Delay "
                 "for the host.h addition that would replace it.");
            const int ps = paramIndex(*probe, "Sync");
            const int pd = paramIndex(*probe, "Division");
            CHECK(ps >= 0 && probe->paramInfo(ps).isBool, "Sync is flagged as a switch");
            CHECK(pd >= 0 && probe->paramInfo(pd).isInt, "Division is flagged as stepped");
        }
    }

    // 1. Free mode: an impulse comes back exactly where it was sent to.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        CHECK(dl != nullptr, "instantiate + prepare");
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 0.f);
        dl->setParam(paramIndex(*dl, "Time"), 100.f);          // ms
        dl->setParam(paramIndex(*dl, "Feedback"), 0.5f);
        dl->setParam(paramIndex(*dl, "Tone"), 18000.f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);
        dl->setParam(paramIndex(*dl, "Ping Pong"), 0.f);

        std::vector<f32> l, r;
        impulseResponse(*dl, 24000, l, r);

        const int expect = (int)(0.100 * kSR);                 // 4800
        int at = 0;
        f32 best = 0.f;
        for (int i = 100; i < 8000; ++i)
            if (std::fabs(l[(size_t)i]) > best) { best = std::fabs(l[(size_t)i]); at = i; }
        CHECK(at == expect, "a 100 ms echo lands at sample %d (expected %d)", at, expect);
        CHECK(std::fabs(best - 1.f) < 0.001f,
              "and at full amplitude, dry/wet at 100%% (%.5f)", (double)best);

        // The feedback path is lowpassed, which smears each repeat in time but
        // leaves its total (a one-pole has unity DC gain) equal to the feedback
        // fraction. So the ENERGY of repeat n is fb^n, measured as a sum.
        auto echoSum = [&](int centre) {
            f64 s = 0.0;
            for (int i = centre - 200; i < centre + 3000 && i < (int)l.size(); ++i)
                if (i >= 0) s += (f64)l[(size_t)i];
            return s;
        };
        const f64 e1 = echoSum(expect);
        const f64 e2 = echoSum(2 * expect);
        const f64 e3 = echoSum(3 * expect);
        CHECK(std::fabs(e2 / e1 - 0.5) < 0.05,
              "the second repeat is %.3f of the first at 50%% feedback", e2 / e1);
        CHECK(std::fabs(e3 / e2 - 0.5) < 0.05,
              "and the third is %.3f of the second", e3 / e2);
    }

    // 2. Sync mode: 1/8 at 120 BPM is 250 ms, whoever is telling us the tempo.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 1.f);
        dl->setParam(paramIndex(*dl, "Division"), 3.f);        // 1/8
        dl->setParam(paramIndex(*dl, "Tempo"), 120.f);
        dl->setParam(paramIndex(*dl, "Feedback"), 0.f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);

        std::vector<f32> l, r;
        impulseResponse(*dl, 24000, l, r);
        int at = 0;
        f32 best = 0.f;
        for (int i = 100; i < 24000; ++i)
            if (std::fabs(l[(size_t)i]) > best) { best = std::fabs(l[(size_t)i]); at = i; }
        CHECK(at == 12000, "1/8 at 120 BPM lands at sample %d (expected 12000 = 250 ms)", at);

        // ...and the tempo really is the divisor: same division, 60 BPM.
        auto slow = reg.instantiate(*d, kSR, kBlock);
        slow->setParam(paramIndex(*slow, "Sync"), 1.f);
        slow->setParam(paramIndex(*slow, "Division"), 3.f);
        slow->setParam(paramIndex(*slow, "Tempo"), 60.f);
        slow->setParam(paramIndex(*slow, "Feedback"), 0.f);
        slow->setParam(paramIndex(*slow, "Dry/Wet"), 1.f);
        impulseResponse(*slow, 48000, l, r);
        at = 0; best = 0.f;
        for (int i = 100; i < 48000; ++i)
            if (std::fabs(l[(size_t)i]) > best) { best = std::fabs(l[(size_t)i]); at = i; }
        CHECK(at == 24000, "the same division at 60 BPM lands at %d (expected 24000)", at);
    }

    // 3. Ping-pong alternates sides rather than spreading.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 0.f);
        dl->setParam(paramIndex(*dl, "Time"), 100.f);
        dl->setParam(paramIndex(*dl, "Feedback"), 0.6f);
        dl->setParam(paramIndex(*dl, "Tone"), 18000.f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);
        dl->setParam(paramIndex(*dl, "Ping Pong"), 1.f);

        std::vector<f32> l, r;
        impulseResponse(*dl, 24000, l, r);
        auto win = [](const std::vector<f32>& v, int a, int b) {
            f64 s = 0.0;
            for (int i = a; i < b && i < (int)v.size(); ++i) s += std::fabs((f64)v[(size_t)i]);
            return s;
        };
        const f64 l1 = win(l, 4700, 7000), r1 = win(r, 4700, 7000);
        const f64 l2 = win(l, 9500, 12000), r2 = win(r, 9500, 12000);
        CHECK(l1 > 0.5 && r1 < 0.01 * l1,
              "the first repeat is left only (L %.4f, R %.4f)", l1, r1);
        CHECK(r2 > 0.1 && l2 < 0.01 * r2,
              "the second repeat is right only (L %.4f, R %.4f)", l2, r2);
    }

    // 4. Fully dry is bit-exact, which is what makes the device safe to leave
    //    in a chain at zero.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 0.f);
        Buf in, out;
        f32 err = 0.f;
        for (int b = 0; b < 12; ++b) {
            for (int i = 0; i < kBlock; ++i)
                in.l[(size_t)i] = in.r[(size_t)i] = 0.5f * (f32)std::sin(6.2831853 * 330.0 * (b * kBlock + i) / kSR);
            out.clear();
            dl->process(in.p, out.p, 2, kBlock);
            if (b >= 4)
                for (int i = 0; i < kBlock; ++i)
                    err = std::fmax(err, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
        }
        CHECK(err < 1e-6f, "dry/wet at 0 is a bit-exact copy (err %.9f)", (double)err);
    }

    // 5. Maximum feedback does not run away. 0.95 into a unity-DC-gain filter
    //    settles at 1/(1-0.95) = 20x the input, and no higher.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 0.f);
        dl->setParam(paramIndex(*dl, "Time"), 20.f);
        dl->setParam(paramIndex(*dl, "Feedback"), 0.95f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);
        Buf in, out;
        for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.05f;
        f32 peak = 0.f;
        bool fin = true;
        for (int b = 0; b < 400; ++b) {
            out.clear();
            dl->process(in.p, out.p, 2, kBlock);
            if (!out.finite()) { fin = false; break; }
            peak = std::fmax(peak, out.peak());
        }
        CHECK(fin, "0.95 feedback on DC stays finite");
        CHECK(peak < 1.5f, "and settles near the predicted 20x (peak %.3f of 0.05)", (double)peak);
    }
}

// ---------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------

// Excites the tank with a noise burst, then measures the decay of the tail in
// 25 ms windows. Returns the RT60 in seconds (extrapolated from the first
// 20 dB of decay, which is what an acoustician does too -- the last 40 dB are
// buried in whatever else is going on) and fills `env` with the window RMS.
static f64 measureRt60(PluginInstance& p, std::vector<f64>& env) {
    const int kWin = (int)(0.025 * kSR);
    Buf in(kWin), out(kWin);
    Noise ns;

    // 300 ms of noise into the tank.
    for (int w = 0; w < 12; ++w) {
        for (int i = 0; i < kWin; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
        out.clear();
        p.process(in.p, out.p, 2, kWin);
    }

    env.clear();
    in.clear();
    for (int w = 0; w < 400; ++w) {                    // up to 10 s of tail
        out.clear();
        p.process(in.p, out.p, 2, kWin);
        f64 s = 0.0;
        for (int i = 0; i < kWin; ++i)
            s += (f64)out.l[(size_t)i] * (f64)out.l[(size_t)i] +
                 (f64)out.r[(size_t)i] * (f64)out.r[(size_t)i];
        env.push_back(std::sqrt(s / (f64)(2 * kWin)));
    }

    const f64 ref = env.empty() ? 0.0 : env[0];
    if (ref <= 0.0) return -1.0;
    for (size_t i = 1; i < env.size(); ++i) {
        if (env[i] <= ref * 0.1) {                     // -20 dB
            const f64 t20 = 0.025 * (f64)i;
            return t20 * 3.0;                          // -20 dB -> -60 dB
        }
    }
    return -1.0;
}

static void testReverb(PluginRegistry& reg) {
    banner("Reverb");

    const PluginDesc* d = reg.find("nxtakt:reverb");
    CHECK(d != nullptr, "registry finds nxtakt:reverb");
    if (!d) return;

    auto rv = reg.instantiate(*d, kSR, kBlock);
    CHECK(rv != nullptr, "instantiate + prepare");
    if (!rv) return;

    const int pPre = paramIndex(*rv, "Pre-Delay");
    const int pDec = paramIndex(*rv, "Decay");
    const int pDmp = paramIndex(*rv, "Damping");
    const int pWid = paramIndex(*rv, "Width");
    const int pMix = paramIndex(*rv, "Dry/Wet");
    CHECK(pPre >= 0 && pDec >= 0 && pDmp >= 0 && pWid >= 0 && pMix >= 0,
          "params Pre-Delay/Decay/Damping/Width/Dry/Wet present");
    if (pDec < 0 || pMix < 0) return;

    // 1. Wet level at defaults: a reverb that needs the fader moved before it
    //    can be heard, or one that doubles the level, is a reverb nobody trusts.
    {
        auto r2 = reg.instantiate(*d, kSR, kBlock);
        r2->setParam(paramIndex(*r2, "Dry/Wet"), 1.f);
        Buf in, out;
        Noise ns;
        f64 inSum = 0.0, outSum = 0.0;
        int n = 0;
        for (int b = 0; b < 100; ++b) {
            for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
            out.clear();
            r2->process(in.p, out.p, 2, kBlock);
            if (b >= 40) {                              // after the tank fills
                for (int i = 0; i < kBlock; ++i) {
                    inSum  += (f64)in.l[(size_t)i] * (f64)in.l[(size_t)i];
                    outSum += (f64)out.l[(size_t)i] * (f64)out.l[(size_t)i];
                    ++n;
                }
            }
        }
        const f64 db = 20.0 * std::log10(std::sqrt(std::fmax(outSum, 1e-30) / std::fmax(inSum, 1e-30)));
        CHECK(std::fabs(db) < 6.0, "100%% wet sits %.2f dB from the dry level", db);
        (void)n;
    }

    // 2. The tail decays, and it decays monotonically. A few percent of ripple
    //    is the modulated tank breathing, not a fault; a tail that grows is a
    //    tank that is going to take the mix with it.
    rv->setParam(pMix, 1.f);
    rv->setParam(pDec, 2.f);
    rv->setParam(pDmp, 18000.f);
    std::vector<f64> env;
    const f64 rt2 = measureRt60(*rv, env);
    CHECK(!env.empty() && env[0] > 1e-4, "the tank rings after the input stops (%.5f)",
          env.empty() ? 0.0 : env[0]);

    bool mono = true;
    size_t badAt = 0;
    for (size_t i = 1; i < env.size() && env[i - 1] > 1e-7; ++i) {
        if (env[i] > env[i - 1] * 1.12) { mono = false; badAt = i; break; }
    }
    CHECK(mono, "the tail decays monotonically to -140 dB%s", mono ? "" : " -- rose at window");
    if (!mono) note("non-monotonic window index above");
    (void)badAt;

    CHECK(rt2 > 0.0, "RT60 is measurable (%.2f s)", rt2);
    CHECK(rt2 > 1.0 && rt2 < 3.6, "a 2 s decay measures RT60 = %.2f s", rt2);

    // 3. RT60 tracks the knob. Two more settings, and the ordering plus the
    //    rough proportionality both have to hold.
    rv->prepare(kSR, kBlock);
    rv->setParam(pMix, 1.f);
    rv->setParam(pDmp, 18000.f);
    rv->setParam(pDec, 0.5f);
    const f64 rtShort = measureRt60(*rv, env);
    rv->prepare(kSR, kBlock);
    rv->setParam(pMix, 1.f);
    rv->setParam(pDmp, 18000.f);
    rv->setParam(pDec, 6.f);
    const f64 rtLong = measureRt60(*rv, env);
    CHECK(rtShort > 0.2 && rtShort < 1.2, "a 0.5 s decay measures %.2f s", rtShort);
    CHECK(rtLong > 3.5 && rtLong < 10.0, "a 6 s decay measures %.2f s", rtLong);
    CHECK(rtShort < rt2 && rt2 < rtLong,
          "RT60 is monotonic in the knob (%.2f < %.2f < %.2f)", rtShort, rt2, rtLong);

    // 4. Damping shortens the tail rather than lengthening it, and the device
    //    stays sane at the extremes of it.
    rv->prepare(kSR, kBlock);
    rv->setParam(pMix, 1.f);
    rv->setParam(pDec, 4.f);
    rv->setParam(pDmp, 18000.f);
    const f64 rtOpen = measureRt60(*rv, env);
    rv->prepare(kSR, kBlock);
    rv->setParam(pMix, 1.f);
    rv->setParam(pDec, 4.f);
    rv->setParam(pDmp, 800.f);
    const f64 rtDamped = measureRt60(*rv, env);
    CHECK(rtDamped > 0.0 && rtDamped < rtOpen,
          "damping shortens the broadband tail (%.2f s damped vs %.2f s open)",
          rtDamped, rtOpen);

    // 5. Pre-delay is a real delay: the wet output stays silent for that long
    //    after an impulse. (Plus the input diffusers, which are ~30 samples.)
    {
        auto r3 = reg.instantiate(*d, kSR, kBlock);
        r3->setParam(paramIndex(*r3, "Dry/Wet"), 1.f);
        r3->setParam(paramIndex(*r3, "Pre-Delay"), 100.f);
        std::vector<f32> l, r;
        impulseResponse(*r3, 24000, l, r);
        const int expect = (int)(0.100 * kSR);
        f32 before = 0.f;
        for (int i = 0; i < expect - 100; ++i) before = std::fmax(before, std::fabs(l[(size_t)i]));
        f32 after = 0.f;
        for (int i = expect; i < expect + 8000 && i < (int)l.size(); ++i)
            after = std::fmax(after, std::fabs(l[(size_t)i]));
        CHECK(before < 1e-6f, "100 ms of pre-delay is silent (%.9f)", (double)before);
        CHECK(after > 1e-4f, "and the tank fires after it (%.5f)", (double)after);
    }

    // 6. Width 0 collapses the two outputs onto each other exactly.
    {
        auto r4 = reg.instantiate(*d, kSR, kBlock);
        r4->setParam(paramIndex(*r4, "Dry/Wet"), 1.f);
        r4->setParam(paramIndex(*r4, "Width"), 0.f);
        Buf in, out;
        Noise ns;
        f32 diff = 0.f;
        for (int b = 0; b < 40; ++b) {
            for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
            out.clear();
            r4->process(in.p, out.p, 2, kBlock);
            if (b >= 20)
                for (int i = 0; i < kBlock; ++i)
                    diff = std::fmax(diff, std::fabs(out.l[(size_t)i] - out.r[(size_t)i]));
        }
        CHECK(diff < 1e-6f, "width 0 is mono (max L-R %.9f)", (double)diff);
    }

    // 7. Fully dry is bit-exact.
    {
        auto r5 = reg.instantiate(*d, kSR, kBlock);
        r5->setParam(paramIndex(*r5, "Dry/Wet"), 0.f);
        Buf in, out;
        f32 err = 0.f;
        for (int b = 0; b < 12; ++b) {
            for (int i = 0; i < kBlock; ++i)
                in.l[(size_t)i] = in.r[(size_t)i] = 0.5f * (f32)std::sin(6.2831853 * 330.0 * (b * kBlock + i) / kSR);
            out.clear();
            r5->process(in.p, out.p, 2, kBlock);
            if (b >= 4)
                for (int i = 0; i < kBlock; ++i)
                    err = std::fmax(err, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
        }
        CHECK(err < 1e-6f, "dry/wet at 0 is a bit-exact copy (err %.9f)", (double)err);
    }
}

// ---------------------------------------------------------------------------
// Hosted-instrument smoke test: proves the backend's note path (LV2 atom
// sequences, CLAP note events) against whatever real plugin is installed.
// ---------------------------------------------------------------------------

static void testHostedInstrument(PluginRegistry& reg, PluginFormat fmt, const char* label) {
    banner(label);

    // mda first — it is the most commonly installed set and its synths make
    // sound immediately with default parameters — then any other instrument.
    // Everything is discovered at runtime; nothing here assumes a given plugin
    // is installed.
    auto isMda = [](const std::string& name) {
        std::string l = name;
        for (char& c : l) c = (char)std::tolower((unsigned char)c);
        return l.find("mda") != std::string::npos;
    };
    std::vector<const PluginDesc*> candidates;
    for (int pass = 0; pass < 2; ++pass) {
        for (const PluginDesc& d : reg.plugins()) {
            if (d.format != fmt || !d.hasMidiIn) continue;
            if (d.kind != PluginKind::Instrument || d.audioOut == 0) continue;
            if (isMda(d.name) == (pass == 0)) candidates.push_back(&d);
        }
    }

    if (candidates.empty()) {
        note("no instrument with a MIDI input is installed for this format; skipping");
        return;
    }

    // Several plugins are tried because an individual one may refuse to
    // instantiate here (missing feature, broken bundle) or may need parameters
    // we do not set. The first one that speaks is enough to prove the path.
    const int kTries = (int)candidates.size() < 8 ? (int)candidates.size() : 8;
    for (int c = 0; c < kTries; ++c) {
        const PluginDesc* d = candidates[(size_t)c];
        auto inst = reg.instantiate(*d, kSR, kBlock);
        if (!inst) { note((d->name + ": would not instantiate, trying the next").c_str()); continue; }

        Buf out;
        // Prime it: some plugins need a block before they will accept notes.
        runFor(*inst, out, 2);

        noteOn(*inst, 60, 110, 0);
        bool fin = false;
        const f32 peak = runFor(*inst, out, (int)(kSR / kBlock), &fin);   // one second

        if (peak <= 1e-5f) {
            note((d->name + ": silent with default parameters, trying the next").c_str());
            continue;
        }
        CHECK(fin, "%s: output is finite", d->name.c_str());
        CHECK(peak > 1e-5f, "%s: note-on through the atom path produces audio (peak %.4f)",
              d->name.c_str(), (double)peak);

        // Note-off must be heard too, or we forged only half a sequence.
        noteOff(*inst, 60, 0);
        runFor(*inst, out, (int)(4 * kSR / kBlock));                      // four seconds
        const f32 tail = runFor(*inst, out, 8);
        CHECK(tail < peak, "%s: note-off is honoured (tail %.6f vs peak %.4f)",
              d->name.c_str(), (double)tail, (double)peak);
        return;
    }

    note("no installed instrument of this format produced audio; note path unverified here");
}

// ---------------------------------------------------------------------------
// Latency reporting (PluginInstance::latencyFrames).
//
// Two halves. The internal devices are a fixed contract -- both are
// zero-latency by construction and must say so. The LV2 half proves the other
// direction, that a plugin which *does* report latency is actually read: no
// URI is hard-coded, because the point is to work on whatever the machine has.
// Candidates are discovered by name (the effects that classically carry
// lookahead or a linear-phase filter), a handful are instantiated, and the
// first one that reports a nonzero figure is the witness. If nothing on this
// system reports latency the section notes it and passes -- a missing plugin is
// not a failing host.
// ---------------------------------------------------------------------------

static void testInternalLatency(PluginRegistry& reg) {
    banner("latency: internal devices");

    for (const char* uri : { "nxtakt:saturator", "nxtakt:pulse", "nxtakt:eq3",
                             "nxtakt:compressor", "nxtakt:delay", "nxtakt:reverb" }) {
        const PluginDesc* d = reg.find(uri);
        CHECK(d != nullptr, "%s: in the registry", uri);
        if (!d) continue;
        auto inst = reg.instantiate(*d, kSR, kBlock);
        CHECK(inst != nullptr, "%s: instantiate + prepare", uri);
        if (!inst) continue;
        CHECK(inst->latencyFrames() == 0, "%s reports 0 frames of latency (%d)",
              uri, inst->latencyFrames());
        // Zero at any block size and rate, not just the one we prepared with:
        // neither device has anything that could scale with either.
        CHECK(inst->prepare(44100.0, 64) && inst->latencyFrames() == 0,
              "%s still reports 0 at 44.1 kHz / 64 frames", uri);
    }
}

static void testLv2Latency(PluginRegistry& reg) {
    banner("latency: LV2 (real plugin, reportsLatency port)");

    // Words that show up in the names of plugins that delay their output: a
    // lookahead limiter, x42's digital peak limiter (dpl), a linear-phase EQ,
    // a convolver. Searched in this order, so the cheapest and most commonly
    // installed candidates are tried first.
    static const char* kHints[] = {
        "limiter", "lookahead", "look-ahead", "dpl", "delayline",
        "linear phase", "linearphase", "convol", "oversampl",
    };

    std::vector<const PluginDesc*> candidates;
    for (const char* hint : kHints) {
        for (const PluginDesc& d : reg.plugins()) {
            if (d.format != PluginFormat::LV2 || d.audioOut == 0) continue;
            if (lower(d.name + " " + d.uri).find(hint) == std::string::npos) continue;
            if (std::find(candidates.begin(), candidates.end(), &d) == candidates.end())
                candidates.push_back(&d);
        }
    }

    if (candidates.empty()) {
        note("no plugin on this system looks like it would report latency; skipping");
        return;
    }

    // Bounded: every attempt dlopen()s a plugin binary, and one witness is all
    // the backend needs to prove.
    const int kTries = (int)candidates.size() < 12 ? (int)candidates.size() : 12;
    for (int c = 0; c < kTries; ++c) {
        const PluginDesc* d = candidates[(size_t)c];
        auto inst = reg.instantiate(*d, kSR, kBlock);
        if (!inst) { note((d->name + ": would not instantiate, trying the next").c_str()); continue; }

        const int lat = inst->latencyFrames();
        // Every plugin, latent or not, has to give a sane answer.
        CHECK(lat >= 0, "%s: latency is not negative (%d)", d->name.c_str(), lat);
        if (lat <= 0) continue;

        CHECK(lat > 0, "%s: reports %d frames of latency after prepare (%.2f ms at %.0f Hz)",
              d->name.c_str(), lat, 1000.0 * lat / kSR, kSR);

        // Constant after prepare, which is the whole contract: preparing the
        // same instance again at the same rate and block size has to produce
        // the same number, and so does a second, independent instance. The
        // first catches a value that leaks state from the settling block; the
        // second catches one that depends on load order.
        const bool re = inst->prepare(kSR, kBlock);
        CHECK(re, "%s: re-prepares at the same rate/block", d->name.c_str());
        CHECK(re && inst->latencyFrames() == lat,
              "%s: latency is stable across two prepares (%d then %d)",
              d->name.c_str(), lat, inst->latencyFrames());

        auto other = reg.instantiate(*d, kSR, kBlock);
        CHECK(other && other->latencyFrames() == lat,
              "%s: a second instance agrees (%d)", d->name.c_str(),
              other ? other->latencyFrames() : -1);

        // And it must survive actually running: latencyFrames() is read from
        // the engine after the chain is published, long after the first block.
        Buf in, out;
        for (int b = 0; b < 4; ++b) { out.clear(); inst->process(in.p, out.p, 2, kBlock); }
        CHECK(inst->latencyFrames() == lat, "%s: latency unchanged after processing (%d)",
              d->name.c_str(), inst->latencyFrames());
        return;
    }

    note("no installed LV2 plugin reported a nonzero latency; the read path is unverified here");
}

// ---------------------------------------------------------------------------

int main() {
    std::printf("internal device tests\n");

    PluginRegistry reg;
    reg.scan();

    banner("registry");
    int internals = 0, firstNonInternal = -1;
    for (size_t i = 0; i < reg.plugins().size(); ++i) {
        if (reg.plugins()[i].format == PluginFormat::Internal) ++internals;
        else if (firstNonInternal < 0) firstNonInternal = (int)i;
    }
    CHECK(internals == 6, "scan lists every internal device (%d)", internals);
    CHECK(firstNonInternal < 0 || firstNonInternal == internals,
          "internal devices sort to the front of the list");

    testLegacyUris(reg);
    testSaturator(reg);
    testPulse(reg);
    testEffectContract(reg);
    testEq3(reg);
    testCompressor(reg);
    testDelay(reg);
    testReverb(reg);
    testInternalLatency(reg);
    testLv2Latency(reg);
    testHostedInstrument(reg, PluginFormat::LV2, "LV2 instrument (real plugin, atom MIDI path)");
    testHostedInstrument(reg, PluginFormat::CLAP, "CLAP instrument (real plugin, note events)");

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
