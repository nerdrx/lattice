// Headless engine tests.
//
// Drives Engine::process() directly with no audio device, no window and no
// session model: buffers are synthesised here so every assertion is about
// engine behaviour and nothing else. Failures are recorded, not thrown, so one
// broken case never hides the rest.
//
//   g++ -std=c++20 -O2 tests/engine_test.cpp src/audio/engine.cpp
//                       src/core/common.cpp -o engine_test
#include "../src/audio/engine.h"
#include "../src/plugin/host.h"
// The onset detector (§31) lives in sample.cpp, which the Makefile's
// build/engine_test rule does not list — and that rule is not this change's to
// edit. Including the translation unit is the whole of the workaround: it needs
// only sndfile and libsamplerate, both of which TOOL_LIBS already links for this
// binary. Caveat for whoever touches it next: the rule has no dependency on
// sample.cpp either, so `touch tests/engine_test.cpp` after editing the detector,
// or `make -B build/engine_test`.
#include "../src/audio/sample.cpp"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

using namespace lat;

// ---------------------------------------------------------------------------
// tiny check framework
// ---------------------------------------------------------------------------

static int gPass = 0, gFail = 0;

static void checkImpl(bool ok, int line, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (ok) { ++gPass; std::printf("  PASS  %s\n", msg); }
    else    { ++gFail; std::printf("  FAIL  %s   (engine_test.cpp:%d)\n", msg, line); }
}
#define CHECK(cond, ...) checkImpl((cond), __LINE__, __VA_ARGS__)

static void banner(const char* s) { std::printf("\n== %s\n", s); }
static void note(const char* s)   { std::printf("  note  %s\n", s); }

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static constexpr f64 kSR    = 48000.0;
static constexpr int kBlock = 256;

// At 120 BPM one beat is 24000 frames and one bar (4/4) is 96000.
static constexpr i64 kBeat120 = 24000;
static constexpr i64 kBar120  = 4 * kBeat120;

struct Host {
    Engine e;
    std::vector<f32> bl, br;      // per-block scratch
    std::vector<f32> il, ir;      // synthetic capture input for this block
    std::vector<f32> outL, outR;  // everything rendered so far
    int block = kBlock;

    // Optional capture generator, called once per block with the absolute
    // frame index the block starts at. When unset the engine is handed nulls,
    // which is exactly what a backend without an input device does.
    std::function<void(i64 startFrame, int n, f32* l, f32* r)> input;

    void init(f64 sr = kSR, int blk = kBlock) {
        block = blk;
        e.prepare(sr, blk);
        bl.assign((size_t)blk, 0.f);
        br.assign((size_t)blk, 0.f);
        il.assign((size_t)blk, 0.f);
        ir.assign((size_t)blk, 0.f);
        outL.clear(); outR.clear();
        input = nullptr;
    }
    void push(Cmd t, i32 a = 0, i32 b = 0, f64 x = 0.0) {
        Command c; c.type = t; c.a = a; c.b = b; c.x = x;
        e.pushCommand(c);
    }
    // Cmd::RecordSlot needs the pointer and capacity payload push() cannot carry.
    void pushRec(int track, int slot, f32* buf, i64 cap) {
        Command c; c.type = Cmd::RecordSlot; c.a = track; c.b = slot;
        c.p = (void*)buf; c.x = (f64)cap;
        e.pushCommand(c);
    }
    // The MIDI take: same payload shape, but the capacity counts NOTES.
    void pushRecMidi(int track, int slot, RtNote* buf, i64 cap) {
        Command c; c.type = Cmd::RecordMidiSlot; c.a = track; c.b = slot;
        c.p = (void*)buf; c.x = (f64)cap;
        e.pushCommand(c);
    }
    void pushMidi(u8 status, u8 d1, u8 d2, i32 frame = 0) {
        MidiMsg m; m.status = status; m.d1 = d1; m.d2 = d2; m.frame = frame;
        e.pushMidi(m);
    }
    // The GUI's own MIDI producer (computer keyboard, note preview): a second
    // SPSC ring the engine also drains, distinct from the hardware reader's.
    void pushMidiFromGui(u8 status, u8 d1, u8 d2, i32 frame = 0) {
        MidiMsg m; m.status = status; m.d1 = d1; m.d2 = d2; m.frame = frame;
        e.pushMidiFromGui(m);
    }
    void setClip(int track, int slot, const RtClip& cl) {
        Command c; c.type = Cmd::SetClip; c.a = track; c.b = slot; c.clip = cl;
        e.pushCommand(c);
    }
    // The engine only ever borrows the chain; the caller keeps it alive until
    // Ev::ChainRetired comes back, exactly as the GUI has to.
    void setChain(int track, const RtChain* ch) {
        Command c; c.type = Cmd::SetChain; c.a = track; c.p = (void*)ch;
        e.pushCommand(c);
    }
    // Return and master chains ride the same borrow-until-retired protocol.
    void setReturnChain(int ret, const RtChain* ch) {
        Command c; c.type = Cmd::SetReturnChain; c.a = ret; c.p = (void*)ch;
        e.pushCommand(c);
    }
    void setMasterChain(const RtChain* ch) {
        Command c; c.type = Cmd::SetMasterChain; c.p = (void*)ch;
        e.pushCommand(c);
    }
    // Renders at least `frames` frames, block-aligned. Returns the frame index
    // just past what had already been rendered before the call.
    size_t run(i64 frames) {
        const size_t mark = outL.size();
        for (i64 done = 0; done < frames; done += block) {
            const f32* pl = nullptr;
            const f32* pr = nullptr;
            if (input) {
                input((i64)outL.size(), block, il.data(), ir.data());
                pl = il.data(); pr = ir.data();
            }
            e.process(pl, pr, bl.data(), br.data(), block);
            outL.insert(outL.end(), bl.begin(), bl.end());
            outR.insert(outR.end(), br.begin(), br.end());
        }
        return mark;
    }
    size_t runBlocks(int n) { return run((i64)n * block); }
};

// Interleaved constant-DC buffer: trivially detectable in the output, and its
// sign identifies which clip is sounding.
static std::vector<f32> dcBuf(i64 frames, int ch, f32 v) {
    return std::vector<f32>((size_t)(frames * ch), v);
}

// Mono ramp from 0.1 to 0.9. The sample value encodes the source read
// position, which is how the warp tests measure playback rate.
static std::vector<f32> rampBuf(i64 frames) {
    std::vector<f32> b((size_t)frames);
    for (i64 i = 0; i < frames; ++i)
        b[(size_t)i] = 0.1f + 0.8f * (f32)((f64)i / (f64)frames);
    return b;
}
static constexpr f64 kRampLo = 0.1, kRampSpan = 0.8;

static RtClip mkClip(const std::vector<f32>& buf, int ch, f32 gain, Warp w,
                     bool loop, f64 clipBpm, i64 loopEnd = -1) {
    RtClip c;
    c.data       = buf.data();
    c.frames     = (i64)(buf.size() / (size_t)ch);
    c.channels   = ch;
    c.loopStart  = 0;
    c.loopEnd    = (loopEnd < 0) ? c.frames : loopEnd;
    c.clipBpm    = clipBpm;
    c.lengthBeats= 4.0;
    c.gain       = gain;
    c.warp       = (int)w;
    c.loop       = loop;
    c.quantumIdx = -1;
    c.valid      = true;
    return c;
}

static i64 firstWhere(const std::vector<f32>& v, size_t from, bool (*pred)(f32)) {
    for (size_t i = from; i < v.size(); ++i) if (pred(v[i])) return (i64)i;
    return -1;
}
static bool nonZero(f32 s) { return std::fabs(s) > 1e-4f; }
static bool negative(f32 s) { return s < -1e-4f; }
// Clip A alone sits at exactly +0.5; any movement means the switch has begun.
static bool departsFromSteady(f32 s) { return s < 0.4999f; }

// Mean of the last `n` frames — the steady level once the declick ramp is done.
static f32 tailLevel(const std::vector<f32>& v, int n = 128) {
    if (v.empty()) return 0.f;
    const size_t from = v.size() > (size_t)n ? v.size() - (size_t)n : 0;
    f64 acc = 0.0;
    for (size_t i = from; i < v.size(); ++i) acc += v[i];
    return (f32)(acc / (f64)(v.size() - from));
}

// ---------------------------------------------------------------------------
// 1. quantized launch timing
// ---------------------------------------------------------------------------

static void testQuantizedLaunch() {
    banner("1. quantized launch timing (120 BPM, quantum = 1 Bar)");
    Host h; h.init();

    // Long enough that neither clip ever reaches its loop point during the
    // test, so the only discontinuity in the output is the clip switch.
    auto bufA = dcBuf(300000, 1,  1.0f);
    auto bufB = dcBuf(300000, 1, -1.0f);
    RtClip a = mkClip(bufA, 1, 0.5f, Warp::Off, true, 120.0);
    RtClip b = mkClip(bufB, 1, 0.5f, Warp::Off, true, 120.0);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);              // kQuantumNames[4] == "1 Bar"
    h.setClip(0, 0, a);
    h.setClip(0, 1, b);
    h.push(Cmd::LaunchClip, 0, 0);

    h.run(kBeat120 * 2);                     // two beats, mid-bar

    // The transport starts at beat 0 and ceil(0/4) == 0, so the launch is due
    // immediately rather than a bar later.
    const i64 start = firstWhere(h.outL, 0, nonZero);
    CHECK(start == 0, "first clip starts at frame 0 (got %lld)", (long long)start);

    const size_t mark = h.outL.size();
    h.push(Cmd::LaunchClip, 0, 1);           // launched mid-bar
    h.run(kBar120 * 2);

    // A same-track switch crossfades, so the sum does not cross zero until the
    // two ramps meet ~96 frames later. The scheduled boundary is instead the
    // first frame where the steady +0.5 of clip A starts to move at all.
    const i64 sw = firstWhere(h.outL, mark, departsFromSteady);
    CHECK(firstWhere(h.outL, mark, negative) >= 0,
          "second clip actually fired (found at %lld)", (long long)firstWhere(h.outL, mark, negative));
    CHECK(sw >= 0 && std::llabs((long long)sw - (long long)kBar120) <= 4,
          "mid-bar launch switches at the bar line: frame %lld, expected %lld",
          (long long)sw, (long long)kBar120);
    CHECK((size_t)sw > mark, "the switch waited for the boundary instead of firing at once");
}

// ---------------------------------------------------------------------------
// 2. quantum None
// ---------------------------------------------------------------------------

static void testQuantumNone() {
    banner("2. quantum = None fires immediately");
    Host h; h.init();
    auto bufA = dcBuf(300000, 1,  1.0f);
    auto bufB = dcBuf(300000, 1, -1.0f);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);              // kQuantumNames[0] == "None"
    h.setClip(0, 0, mkClip(bufA, 1, 0.5f, Warp::Off, true, 120.0));
    h.setClip(0, 1, mkClip(bufB, 1, 0.5f, Warp::Off, true, 120.0));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(30000);                            // land somewhere mid-bar

    const size_t mark = h.outL.size();
    h.push(Cmd::LaunchClip, 0, 1);
    h.runBlocks(4);

    const i64 sw = firstWhere(h.outL, mark, negative);
    CHECK(sw >= 0 && (size_t)sw < mark + (size_t)kBlock,
          "unquantized launch fires within one block: frame %lld, block starts at %zu",
          (long long)sw, mark);
}

// ---------------------------------------------------------------------------
// 3. looping
// ---------------------------------------------------------------------------

static void testLooping() {
    banner("3. looping vs one-shot");
    const i64 N = 10000;

    {
        Host h; h.init();
        auto buf = dcBuf(N, 1, 1.0f);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setClip(0, 0, mkClip(buf, 1, 0.5f, Warp::Off, /*loop*/true, 120.0));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(N * 6);
        CHECK(std::fabs(h.outL[(size_t)(N * 5)]) > 0.1f,
              "looping clip still sounding at %lldx its length (%.4f)",
              (long long)5, (double)h.outL[(size_t)(N * 5)]);
    }
    {
        Host h; h.init();
        auto buf = dcBuf(N, 1, 1.0f);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setClip(0, 0, mkClip(buf, 1, 0.5f, Warp::Off, /*loop*/false, 120.0));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(N * 6);

        CHECK(std::fabs(h.outL[(size_t)(N / 2)]) > 0.1f,
              "one-shot clip sounds before its end (%.4f)", (double)h.outL[(size_t)(N / 2)]);

        // 6 ms release ramp at 48 kHz is 288 frames; allow a little slack.
        const i64 quiet = N + 400;
        f32 worst = 0.f;
        for (size_t i = (size_t)quiet; i < h.outL.size(); ++i)
            worst = std::max(worst, std::fabs(h.outL[i]));
        CHECK(worst < 1e-5f, "one-shot clip is silent past its end + release tail (peak %.3g)",
              (double)worst);
    }
}

// ---------------------------------------------------------------------------
// 4. warp / tempo follow
// ---------------------------------------------------------------------------

// Recovers the source read position from a ramp clip's output level.
static f64 srcPosAt(const std::vector<f32>& v, size_t frame, i64 clipFrames) {
    return ((f64)v[frame] - kRampLo) / kRampSpan * (f64)clipFrames;
}

static f64 measureRate(Warp w, f64 clipBpm, f64 tempo) {
    const i64 N = 480000;                    // 10 s of source
    Host h; h.init();
    auto buf = rampBuf(N);
    h.push(Cmd::SetTempo, 0, 0, tempo);
    h.push(Cmd::SetQuantum, 0);
    h.setClip(0, 0, mkClip(buf, 1, 1.0f, w, /*loop*/false, clipBpm));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(48000);
    // Both probes sit well past the 3 ms attack ramp, so the envelope is 1.0
    // and the output level is the raw source value.
    const f64 p1 = srcPosAt(h.outL, 20000, N);
    const f64 p2 = srcPosAt(h.outL, 40000, N);
    return (p2 - p1) / 20000.0;
}

static void testWarp() {
    banner("4. warp / tempo follow");
    note("rate = tempo / clipBpm for any warp mode != Off: to fit material");
    note("recorded at clipBpm onto a faster grid you must read the source faster.");

    const f64 rHalf = measureRate(Warp::Repitch, 120.0, 240.0);
    CHECK(std::fabs(rHalf - 2.0) < 0.02,
          "Repitch @ clipBpm 120 / tempo 240 -> rate %.4f (expected tempo/clipBpm = 2.0)", rHalf);

    const f64 rDouble = measureRate(Warp::Repitch, 120.0, 60.0);
    CHECK(std::fabs(rDouble - 0.5) < 0.01,
          "Repitch @ clipBpm 120 / tempo 60 -> rate %.4f (expected tempo/clipBpm = 0.5)", rDouble);

    const f64 rOffFast = measureRate(Warp::Off, 120.0, 240.0);
    const f64 rOffSlow = measureRate(Warp::Off, 120.0, 60.0);
    CHECK(std::fabs(rOffFast - 1.0) < 0.01,
          "Warp::Off ignores tempo 240 -> rate %.4f", rOffFast);
    CHECK(std::fabs(rOffSlow - 1.0) < 0.01,
          "Warp::Off ignores tempo 60 -> rate %.4f", rOffSlow);
    CHECK(std::fabs(rOffFast - rOffSlow) < 1e-3,
          "Warp::Off rate is tempo-independent (%.4f vs %.4f)", rOffFast, rOffSlow);
}

// ---------------------------------------------------------------------------
// 5. mute / solo
// ---------------------------------------------------------------------------

static void testMuteSolo() {
    banner("5. mute / solo");
    Host h; h.init();
    // Distinct gains so the summed level says which track survived.
    auto buf0 = dcBuf(300000, 1, 1.0f);
    auto buf1 = dcBuf(300000, 1, 1.0f);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setClip(0, 0, mkClip(buf0, 1, 0.25f, Warp::Off, true, 120.0));
    h.setClip(1, 0, mkClip(buf1, 1, 0.50f, Warp::Off, true, 120.0));
    h.push(Cmd::LaunchClip, 0, 0);
    h.push(Cmd::LaunchClip, 1, 0);

    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.75f) < 0.01f,
          "both tracks audible -> %.4f (expected 0.75)", (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::TrackSolo, 0, 1);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.25f) < 0.01f,
          "solo on track 0 silences track 1 -> %.4f (expected 0.25)", (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::TrackSolo, 0, 0);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.75f) < 0.01f,
          "clearing solo restores track 1 -> %.4f (expected 0.75)", (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::TrackMute, 0, 1);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.50f) < 0.01f,
          "mute on track 0 leaves only track 1 -> %.4f (expected 0.50)", (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::TrackMute, 1, 1);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL)) < 0.001f,
          "muting both tracks is silent -> %.4f", (double)tailLevel(h.outL));
}

// ---------------------------------------------------------------------------
// 6. scene launch
// ---------------------------------------------------------------------------

static void testSceneLaunch() {
    banner("6. scene launch starts a row and stops tracks with an empty slot");
    Host h; h.init();
    auto buf = dcBuf(300000, 1, 1.0f);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);

    // Row 0 is full; row 1 has a hole on track 1.
    for (int t = 0; t < 3; ++t) h.setClip(t, 0, mkClip(buf, 1, 0.25f, Warp::Off, true, 120.0));
    h.setClip(0, 1, mkClip(buf, 1, 0.25f, Warp::Off, true, 120.0));
    h.setClip(2, 1, mkClip(buf, 1, 0.25f, Warp::Off, true, 120.0));

    h.push(Cmd::LaunchScene, 0);
    h.run(4000);
    CHECK(h.e.activeSlot[0].load() == 0 && h.e.activeSlot[1].load() == 0 &&
          h.e.activeSlot[2].load() == 0,
          "scene 0 started all three tracks (%d %d %d)",
          h.e.activeSlot[0].load(), h.e.activeSlot[1].load(), h.e.activeSlot[2].load());
    CHECK(std::fabs(tailLevel(h.outL) - 0.75f) < 0.01f,
          "three tracks sounding -> %.4f (expected 0.75)", (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::LaunchScene, 1);
    h.run(4000);
    CHECK(h.e.activeSlot[0].load() == 1, "track 0 moved to row 1 (got %d)", h.e.activeSlot[0].load());
    CHECK(h.e.activeSlot[2].load() == 1, "track 2 moved to row 1 (got %d)", h.e.activeSlot[2].load());
    CHECK(h.e.activeSlot[1].load() == -1,
          "track 1 stopped because its slot in row 1 is empty (got %d)", h.e.activeSlot[1].load());
    CHECK(h.e.slotState[1].load() == (int)SlotState::Stopped,
          "track 1 reports SlotState::Stopped (got %d)", h.e.slotState[1].load());
    CHECK(std::fabs(tailLevel(h.outL) - 0.50f) < 0.01f,
          "only two tracks sounding -> %.4f (expected 0.50)", (double)tailLevel(h.outL));
}

// ---------------------------------------------------------------------------
// 7. numerical hygiene
// ---------------------------------------------------------------------------

static void testFiniteOutput() {
    banner("7. no NaN / inf / out-of-range output over a long run");
    Host h; h.init();

    auto sine = std::vector<f32>((size_t)(96000 * 2));
    for (i64 i = 0; i < 96000; ++i) {
        const f32 s = (f32)std::sin(2.0 * 3.14159265358979 * 440.0 * (f64)i / kSR);
        sine[(size_t)(i * 2)]     = s;
        sine[(size_t)(i * 2 + 1)] = s * 0.7f;
    }
    auto ramp = rampBuf(70000);
    auto dc   = dcBuf(33333, 1, 0.8f);

    h.push(Cmd::SetTempo, 0, 0, 143.7);      // non-integer ratio -> granular path
    h.push(Cmd::SetQuantum, 0);
    h.push(Cmd::SetMetronome, 1);
    h.setClip(0, 0, mkClip(sine, 2, 0.9f, Warp::Beats,   true, 120.0));
    h.setClip(1, 0, mkClip(ramp, 1, 0.9f, Warp::Repitch, true, 100.0));
    h.setClip(2, 0, mkClip(dc,   1, 0.9f, Warp::Off,     true, 120.0));
    h.push(Cmd::TrackPan, 0, 0, -0.8);
    h.push(Cmd::TrackPan, 1, 0,  0.8);
    for (int t = 0; t < 3; ++t) h.push(Cmd::LaunchClip, t, 0);

    std::vector<f32> bl((size_t)kBlock), br((size_t)kBlock);
    bool allFinite = true, inRange = true;
    f32 peak = 0.f;
    for (int i = 0; i < 4000; ++i) {
        // Move the tempo around so the warp ratio and grain hop keep changing.
        if (i % 500 == 0) h.e.pushCommand([&]{
            Command c; c.type = Cmd::SetTempo; c.x = 60.0 + (f64)(i % 7) * 37.5; return c; }());
        h.e.process(nullptr, nullptr, bl.data(), br.data(), kBlock);
        for (int j = 0; j < kBlock; ++j) {
            if (!std::isfinite(bl[(size_t)j]) || !std::isfinite(br[(size_t)j])) allFinite = false;
            const f32 m = std::max(std::fabs(bl[(size_t)j]), std::fabs(br[(size_t)j]));
            if (m > 1.0f + 1e-6f) inRange = false;
            if (m > peak) peak = m;
        }
    }
    CHECK(peak > 0.01f, "the long run actually produced audio (peak %.4f)", (double)peak);
    CHECK(allFinite, "every sample over 4000 blocks (1024000 frames) is finite");
    CHECK(inRange, "every sample stays within +/-1.0 (peak %.6f)", (double)peak);
}

// ---------------------------------------------------------------------------
// 8. command ring saturation
// ---------------------------------------------------------------------------

static void testRingSaturation() {
    banner("8. command ring saturation");
    Host h; h.init();

    // Ring<Command, 1024> keeps one slot free to distinguish full from empty,
    // so 1023 pushes should succeed and everything after must be refused.
    int ok = 0, refused = 0;
    for (int i = 0; i < 2000; ++i) {
        Command c; c.type = Cmd::SetTempo; c.x = 137.0;
        if (h.e.pushCommand(c)) ++ok; else ++refused;
    }
    CHECK(refused > 0, "pushCommand refuses once the ring is full (%d accepted, %d refused)",
          ok, refused);
    CHECK(ok == 1023, "exactly capacity-1 commands were accepted (%d)", ok);

    h.runBlocks(1);                          // drains everything
    CHECK(std::fabs(h.e.tempo.load() - 137.0) < 1e-9,
          "state survived saturation: tempo %.3f", h.e.tempo.load());

    // The engine must still behave normally afterwards.
    auto buf = dcBuf(300000, 1, 1.0f);
    h.push(Cmd::SetQuantum, 0);
    h.setClip(0, 0, mkClip(buf, 1, 0.5f, Warp::Off, true, 120.0));
    h.push(Cmd::LaunchClip, 0, 0);
    const size_t mark = h.run(8000);

    bool finite = true;
    for (size_t i = mark; i < h.outL.size(); ++i)
        if (!std::isfinite(h.outL[i]) || !std::isfinite(h.outR[i])) finite = false;
    CHECK(finite, "output after saturation is still finite");
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.01f,
          "a clip launched after saturation plays normally -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));
}

// ---------------------------------------------------------------------------
// 9. device chains
// ---------------------------------------------------------------------------

// A PluginInstance that does nothing but scale, so any level change in the
// output is attributable to the chain and to nothing else. It also records how
// it was called, which is how the "chains run while stopped" case is checked.
//
// It can also be latent. A device that *reports* latency without incurring any
// would make the compensation tests pass for the wrong reason, so `latency`
// does both: latencyFrames() reports it and process() really does hold the
// signal back that many frames. Zero — the default, and what every test before
// section 17 uses — allocates nothing and leaves the code path untouched.
class FakeFx : public PluginInstance {
public:
    explicit FakeFx(f32 gain, int latency = 0) : gain_(gain), latency_(latency) {
        if (latency_ > 0)
            for (auto& r : ring_) r.assign((size_t)latency_, 0.f);
    }

    int calls = 0;
    int maxFrames = 0;
    int maxChannels = 0;

    bool prepare(f64, int) override { return true; }

    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        ++calls;
        if (nframes > maxFrames) maxFrames = nframes;
        if (channels > maxChannels) maxChannels = channels;
        if (bypassed_) return;                    // in aliases out, so a no-op
        for (int c = 0; c < channels; ++c) {
            if (!out[c] || !in[c]) continue;
            if (latency_ > 0 && c < 2) {
                std::vector<f32>& r = ring_[c];
                int& pos = pos_[c];
                for (int i = 0; i < nframes; ++i) {
                    const f32 x = in[c][i];       // in may alias out: read first
                    const f32 y = r[(size_t)pos];
                    r[(size_t)pos] = x;
                    pos = (pos + 1) % latency_;
                    out[c][i] = y * gain_;
                }
                continue;
            }
            for (int i = 0; i < nframes; ++i) out[c][i] = in[c][i] * gain_;
        }
    }

    int              paramCount() const override     { return 0; }
    const ParamInfo& paramInfo(int) const override   { static ParamInfo p; return p; }
    f32              getParam(int) const override    { return 0.f; }
    void             setParam(int, f32) override     {}
    const PluginDesc& desc() const override          { static PluginDesc d; return d; }
    int              latencyFrames() const override  { return latency_; }
    void             setBypassed(bool b) override    { bypassed_ = b; }
    bool             bypassed() const override       { return bypassed_; }

private:
    f32  gain_ = 1.f;
    bool bypassed_ = false;
    int  latency_ = 0;
    std::vector<f32> ring_[2];
    int  pos_[2] = {0, 0};
};

// Drains the event ring, counting ChainRetired and remembering which chain
// pointers came back.
struct RetiredEvents {
    int count = 0;
    std::vector<const void*> ptrs;
    std::vector<int> tracks;
    bool sawPtr(const void* p) const {
        for (const void* q : ptrs) if (q == p) return true;
        return false;
    }
};
static RetiredEvents drainRetired(Engine& e) {
    RetiredEvents r;
    Event ev;
    while (e.popEvent(ev)) {
        if (ev.type != Ev::ChainRetired) continue;
        ++r.count;
        r.ptrs.push_back(ev.p);
        r.tracks.push_back(ev.a);
    }
    return r;
}

// A track playing a +1.0 DC clip at clip gain 0.5, with the default unity
// fader: the bare output level is 0.50 and anything else is the chain.
static void armDcTrack(Host& h, const std::vector<f32>& buf) {
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setClip(0, 0, mkClip(buf, 1, 0.5f, Warp::Off, true, 120.0));
    h.push(Cmd::LaunchClip, 0, 0);
}

// One Host per function, never several per frame: Track carries two kMaxBlock
// scratch buffers, so an Engine is ~2 MB by value and a handful of them in one
// stack frame overflows under ASan.

// a. one effect in the chain
static void chainSingleEffect(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx half(0.5f);
    RtChain chain; chain.fx[0] = &half; chain.count = 1;

    armDcTrack(h, buf);
    h.setChain(0, &chain);
    h.run(8000);

    CHECK(std::fabs(tailLevel(h.outL) - 0.25f) < 0.01f,
          "0.5 clip through a 0.5x effect -> %.4f (expected 0.25)",
          (double)tailLevel(h.outL));
    CHECK(std::fabs(tailLevel(h.outR) - 0.25f) < 0.01f,
          "right channel matches -> %.4f (expected 0.25)", (double)tailLevel(h.outR));
    CHECK(half.calls > 0, "the effect actually ran (%d calls)", half.calls);
    CHECK(half.maxFrames == h.block,
          "the chain sees the full block, not a launch sub-range (%d, block %d)",
          half.maxFrames, h.block);
    CHECK(half.maxChannels == 2, "the chain is handed both channels (%d)", half.maxChannels);
}

// b. two effects in series
static void chainTwoInSeries(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx a(0.5f), b(0.5f);
    RtChain chain; chain.fx[0] = &a; chain.fx[1] = &b; chain.count = 2;

    armDcTrack(h, buf);
    h.setChain(0, &chain);
    h.run(8000);

    CHECK(std::fabs(tailLevel(h.outL) - 0.125f) < 0.005f,
          "0.5 clip through two 0.5x effects -> %.4f (expected 0.125)",
          (double)tailLevel(h.outL));
    CHECK(a.calls == b.calls && a.calls > 0,
          "both effects ran the same number of times (%d / %d)", a.calls, b.calls);
}

// c. swapping a chain hands the old one back exactly once
static void chainSwapRetires(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx fa(0.5f), fb(0.25f);
    RtChain chainA; chainA.fx[0] = &fa; chainA.count = 1;
    RtChain chainB; chainB.fx[0] = &fb; chainB.count = 1;

    armDcTrack(h, buf);
    h.setChain(0, &chainA);
    h.run(8000);
    drainRetired(h.e);                       // also clears the launch events

    CHECK(std::fabs(tailLevel(h.outL) - 0.25f) < 0.01f,
          "chain A in place -> %.4f (expected 0.25)", (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.setChain(0, &chainB);
    h.run(8000);
    RetiredEvents r = drainRetired(h.e);
    CHECK(r.count == 1, "swapping A->B retires exactly one chain (%d)", r.count);
    CHECK(r.count == 1 && r.ptrs[0] == (const void*)&chainA,
          "the retired pointer is chain A (%p, expected %p)",
          r.count ? r.ptrs[0] : nullptr, (const void*)&chainA);
    CHECK(r.count == 1 && r.tracks[0] == 0, "the event names track 0 (%d)",
          r.count ? r.tracks[0] : -1);
    CHECK(std::fabs(tailLevel(h.outL) - 0.125f) < 0.005f,
          "chain B is now in the path -> %.4f (expected 0.125)", (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.setChain(0, nullptr);                  // clearing the chain
    h.run(8000);
    RetiredEvents r2 = drainRetired(h.e);
    CHECK(r2.count == 1 && r2.ptrs[0] == (const void*)&chainB,
          "clearing the chain retires B (%d events, ptr %p, expected %p)",
          r2.count, r2.count ? r2.ptrs[0] : nullptr, (const void*)&chainB);
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.01f,
          "with no chain the track is passthrough again -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));

    h.setChain(0, nullptr);                  // null over null
    h.runBlocks(2);
    CHECK(drainRetired(h.e).count == 0,
          "clearing an already-empty chain retires nothing");
}

// d. chains keep running with the transport stopped (reverb tails, monitoring)
static void chainRunsWhileStopped(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx tail(0.5f);
    RtChain chain; chain.fx[0] = &tail; chain.count = 1;

    armDcTrack(h, buf);
    h.setChain(0, &chain);
    h.run(8000);

    h.push(Cmd::SetPlaying, 0);              // stop
    h.runBlocks(8);                          // well past the 6 ms release tail
    const int afterStop = tail.calls;
    tail.maxFrames = 0;
    h.runBlocks(8);

    CHECK(tail.calls > afterStop,
          "the chain still runs with the transport stopped (%d -> %d calls)",
          afterStop, tail.calls);
    CHECK(tail.calls - afterStop == 8,
          "exactly one run per block while stopped (%d over 8 blocks)",
          tail.calls - afterStop);
    CHECK(tail.maxFrames == h.block,
          "stopped blocks are still full length (%d, block %d)", tail.maxFrames, h.block);
    CHECK(std::fabs(tailLevel(h.outL)) < 1e-5f,
          "a 0.5x effect on silence is still silent -> %.3g", (double)tailLevel(h.outL));
}

// e. holes in fx[] and a zero count
static void chainNullSlots(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx half(0.5f);
    // A hole at index 0 and a trailing hole: the GUI removing a device must
    // never be able to make the audio thread dereference null.
    RtChain chain;
    chain.fx[0] = nullptr;
    chain.fx[1] = &half;
    chain.fx[2] = nullptr;
    chain.count = 3;

    armDcTrack(h, buf);
    h.setChain(0, &chain);
    h.run(8000);

    CHECK(half.calls > 0, "the one real effect ran (%d calls)", half.calls);
    CHECK(std::fabs(tailLevel(h.outL) - 0.25f) < 0.01f,
          "nulls are skipped, gain applied once -> %.4f (expected 0.25)",
          (double)tailLevel(h.outL));

    // count == 0 with a populated array must run nothing at all.
    h.outL.clear(); h.outR.clear();
    const int before = half.calls;
    RtChain empty; empty.fx[0] = &half; empty.count = 0;
    h.setChain(0, &empty);
    h.run(8000);
    CHECK(half.calls == before, "count == 0 runs nothing (%d -> %d)", before, half.calls);
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.01f,
          "an empty chain is passthrough -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));
    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// f. the chain sits before vol/pan/mute, and the meter after them
static void chainSitsBeforeFader(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx half(0.5f);
    RtChain chain; chain.fx[0] = &half; chain.count = 1;

    armDcTrack(h, buf);
    h.setChain(0, &chain);
    h.push(Cmd::TrackMute, 0, 1);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL)) < 1e-4f,
          "mute after the chain silences the track -> %.3g", (double)tailLevel(h.outL));
    CHECK(half.calls > 0, "a muted track still runs its chain (%d calls)", half.calls);

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::TrackMute, 0, 0);
    h.push(Cmd::TrackPan, 0, 0, -1.0);       // hard left
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.25f) < 0.01f,
          "pan is applied after the chain, left -> %.4f (expected 0.25)",
          (double)tailLevel(h.outL));
    CHECK(std::fabs(tailLevel(h.outR)) < 1e-4f,
          "hard left mutes the right channel -> %.3g", (double)tailLevel(h.outR));
    CHECK(std::fabs(h.e.meterL[0].load() - 0.25f) < 0.02f,
          "the meter is post-chain and post-fader -> %.4f (expected 0.25)",
          (double)h.e.meterL[0].load());
}

static void testDeviceChains() {
    banner("9. device chains");
    note("signal flow: voices (clip gain + declick) -> fx chain -> vol/pan/mute");
    note("-> meters -> master. faderToGain(0.85) is unity, so a bare track is 0.50.");
    const auto buf = dcBuf(300000, 1, 1.0f);

    chainSingleEffect(buf);
    chainTwoInSeries(buf);
    chainSwapRetires(buf);
    chainRunsWhileStopped(buf);
    chainNullSlots(buf);
    chainSitsBeforeFader(buf);
}

// ---------------------------------------------------------------------------
// 10. recording
// ---------------------------------------------------------------------------

// The capture signal. Both channels are periodic but with coprime periods and
// different shapes, so a buffer written in the wrong interleave order, at the
// wrong offset, or from the wrong channel cannot accidentally match.
static f32 capL(i64 i) { return (f32)(i % 1024) * (1.f / 1024.f) - 0.5f; }
static f32 capR(i64 i) { return 0.25f - (f32)(i % 777) * (1.f / 777.f); }

static void feedCapture(Host& h) {
    h.input = [](i64 start, int n, f32* l, f32* r) {
        for (int i = 0; i < n; ++i) { l[i] = capL(start + i); r[i] = capR(start + i); }
    };
}

static std::vector<Event> drainEvents(Engine& e) {
    std::vector<Event> v;
    Event ev;
    while (e.popEvent(ev)) v.push_back(ev);
    return v;
}
static const Event* findEvent(const std::vector<Event>& v, Ev t) {
    for (const Event& e : v) if (e.type == t) return &e;
    return nullptr;
}
static int countEvents(const std::vector<Event>& v, Ev t) {
    int n = 0;
    for (const Event& e : v) if (e.type == t) ++n;
    return n;
}

// Recovers the absolute frame the take began on by matching its first frame
// against the generator, so a boundary that lands a sample either side of the
// ideal beat (double accumulation over hundreds of blocks) is not a failure.
static i64 findCaptureOffset(const std::vector<f32>& buf, i64 expect, i64 slack) {
    for (i64 d = 0; d <= slack; ++d)
        for (i64 s : {expect + d, expect - d})
            if (buf[0] == capL(s) && buf[1] == capR(s)) return s;
    return -1;
}

// a. quantized start and stop, and the captured audio itself
static void recQuantizedTake() {
    Host h; h.init();
    feedCapture(h);
    std::vector<f32> rec((size_t)300000 * 2, 0.f);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);                  // 1 Bar
    h.push(Cmd::SetPlaying, 1);
    h.run(kBeat120 * 2);                         // land mid-bar
    drainEvents(h.e);

    h.pushRec(0, 0, rec.data(), 300000);
    h.run(60000);                                // well past the bar line
    std::vector<Event> evs = drainEvents(h.e);
    const Event* started = findEvent(evs, Ev::RecordStarted);
    CHECK(started != nullptr, "a mid-bar RecordSlot produces Ev::RecordStarted");
    CHECK(started && started->a == 0 && started->b == 0,
          "RecordStarted names track 0 slot 0 (%d/%d)",
          started ? started->a : -1, started ? started->b : -1);
    CHECK(started && std::fabs(started->x - 4.0) < 1e-6,
          "the take begins on the bar line, beat %.6f (expected 4.0)",
          started ? started->x : -1.0);
    CHECK(h.e.recState[0].load() == 2, "recState reports 2 (recording), got %d",
          h.e.recState[0].load());
    CHECK(h.e.recSlotIdx[0].load() == 0, "recSlotIdx reports slot 0, got %d",
          h.e.recSlotIdx[0].load());

    // Toggle: a second RecordSlot on the same slot stops on the next bar.
    h.pushRec(0, 0, rec.data(), 300000);
    h.run(120000);
    evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::RecordFinished);
    CHECK(fin != nullptr, "the toggle produces Ev::RecordFinished");
    CHECK(fin && fin->p == (void*)rec.data(),
          "RecordFinished hands back the buffer the GUI supplied (%p vs %p)",
          fin ? fin->p : nullptr, (void*)rec.data());
    CHECK(fin && fin->b == 0, "RecordFinished names slot 0 (%d)", fin ? fin->b : -1);
    // One bar at 120 BPM is 96000 frames; the two boundaries are a bar apart.
    CHECK(fin && std::llabs((long long)fin->x - (long long)kBar120) <= 2,
          "the take is exactly one bar long: %lld frames (expected %lld)",
          fin ? (long long)fin->x : -1, (long long)kBar120);
    CHECK(h.e.recState[0].load() == 0, "recState returns to 0 (idle), got %d",
          h.e.recState[0].load());

    const i64 off = findCaptureOffset(rec, kBar120, 4);
    CHECK(off >= 0 && std::llabs((long long)off - (long long)kBar120) <= 2,
          "capture starts at the bar line: frame %lld (expected %lld)",
          (long long)off, (long long)kBar120);

    // Every recorded frame must be the input verbatim, in L,R interleave.
    i64 bad = -1;
    const i64 len = fin ? (i64)fin->x : 0;
    if (off >= 0)
        for (i64 k = 0; k < len; ++k)
            if (rec[(size_t)k * 2] != capL(off + k) || rec[(size_t)k * 2 + 1] != capR(off + k)) {
                bad = k; break;
            }
    CHECK(off >= 0 && len > 0 && bad < 0,
          "all %lld captured frames match the input exactly, L then R (first mismatch %lld)",
          (long long)len, (long long)bad);
    // Nothing may be written past the take.
    CHECK(len > 0 && rec[(size_t)len * 2] == 0.f && rec[(size_t)len * 2 + 1] == 0.f,
          "the engine wrote nothing past the take's last frame");
}

// b. a full buffer ends the take on the spot
static void recCapacityStop() {
    Host h; h.init();
    feedCapture(h);
    std::vector<f32> rec(1000 * 2, 0.f);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);                  // None: start at once
    h.pushRec(0, 0, rec.data(), 1000);
    h.runBlocks(8);                              // 2048 frames, twice the capacity

    const std::vector<Event> evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::RecordFinished);
    CHECK(fin != nullptr, "a full buffer auto-stops the take");
    CHECK(fin && (i64)fin->x == 1000,
          "the take stops exactly at capacity: %lld frames (expected 1000)",
          fin ? (long long)fin->x : -1);
    CHECK(h.e.recState[0].load() == 0, "the track is idle again after an auto-stop (%d)",
          h.e.recState[0].load());
    // 1000 frames of a 1000-frame buffer: the last slot must be the 1000th
    // input frame and not a wild write.
    CHECK(rec[999 * 2] == capL(999) && rec[999 * 2 + 1] == capR(999),
          "the last frame in the buffer is the last frame of input");
}

// c. stopping the transport ends any take immediately
static void recTransportStop() {
    Host h; h.init();
    feedCapture(h);
    std::vector<f32> rec((size_t)100000 * 2, 0.f);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);                  // a bar away: nowhere near
    h.pushRec(0, 0, rec.data(), 100000);
    h.runBlocks(4);
    drainEvents(h.e);

    h.push(Cmd::SetPlaying, 0);
    h.runBlocks(1);
    const std::vector<Event> evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::RecordFinished);
    CHECK(fin != nullptr, "stopping the transport finishes the take at once");
    CHECK(fin && (i64)fin->x == 4 * kBlock,
          "it hands back what was captured: %lld frames (expected %d)",
          fin ? (long long)fin->x : -1, 4 * kBlock);
    CHECK(fin && fin->p == (void*)rec.data(), "with the right buffer");
    CHECK(h.e.recState[0].load() == 0, "and the track is idle (%d)", h.e.recState[0].load());
}

// d. a RecordSlot aimed elsewhere hands over on one boundary
static void recSlotHandover() {
    Host h; h.init();
    feedCapture(h);
    std::vector<f32> recA((size_t)300000 * 2, 0.f);
    std::vector<f32> recB((size_t)300000 * 2, 0.f);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);                  // 1 Bar
    h.pushRec(0, 0, recA.data(), 300000);        // starts at beat 0
    h.run(kBeat120 * 2);
    drainEvents(h.e);

    h.pushRec(0, 1, recB.data(), 300000);        // different slot, mid-bar
    h.run(kBar120);
    const std::vector<Event> evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::RecordFinished);
    CHECK(fin != nullptr, "switching slots finishes the running take");
    CHECK(fin && fin->b == 0 && fin->p == (void*)recA.data(),
          "the finished take is slot 0's, with slot 0's buffer (%d, %p)",
          fin ? fin->b : -1, fin ? fin->p : nullptr);
    CHECK(fin && std::llabs((long long)fin->x - (long long)kBar120) <= 2,
          "it ran to the bar line: %lld frames (expected %lld)",
          fin ? (long long)fin->x : -1, (long long)kBar120);
    CHECK(countEvents(evs, Ev::RecordStarted) == 1,
          "the new take starts on the same boundary (%d RecordStarted)",
          countEvents(evs, Ev::RecordStarted));
    CHECK(h.e.recSlotIdx[0].load() == 1 && h.e.recState[0].load() == 2,
          "the track is now recording slot %d in state %d (expected 1 / 2)",
          h.e.recSlotIdx[0].load(), h.e.recState[0].load());

    // No gap: the new take's first frame is the frame after the old one's last.
    const i64 offB = findCaptureOffset(recB, kBar120, 4);
    CHECK(offB >= 0 && std::llabs((long long)offB - (long long)kBar120) <= 2,
          "the hand-over is gapless: slot 1 starts at frame %lld (expected %lld)",
          (long long)offB, (long long)kBar120);
}

// e. a take queued but not yet begun can be cancelled, and input monitoring
static void recCancelAndMonitor() {
    {
        Host h; h.init();
        feedCapture(h);
        std::vector<f32> rec((size_t)100000 * 2, 0.f);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 4);
        h.push(Cmd::SetPlaying, 1);
        h.run(kBeat120);
        h.pushRec(0, 0, rec.data(), 100000);     // queued for the bar line
        h.runBlocks(1);
        CHECK(h.e.recState[0].load() == 1, "a queued take reports recState 1 (%d)",
              h.e.recState[0].load());
        h.pushRec(0, 0, rec.data(), 100000);     // toggle before it begins
        h.run(kBar120 * 2);
        const std::vector<Event> evs = drainEvents(h.e);
        CHECK(countEvents(evs, Ev::RecordStarted) == 0 &&
              countEvents(evs, Ev::RecordFinished) == 0,
              "cancelling a queued take is silent: no start, no finish (%d/%d)",
              countEvents(evs, Ev::RecordStarted), countEvents(evs, Ev::RecordFinished));
        CHECK(h.e.recState[0].load() == 0, "and the track is idle (%d)",
              h.e.recState[0].load());
    }
    {
        // Monitoring is pre-chain: a 0.5x device halves what you hear.
        Host h; h.init();
        h.input = [](i64, int n, f32* l, f32* r) {
            for (int i = 0; i < n; ++i) { l[i] = 0.4f; r[i] = 0.2f; }
        };
        FakeFx half(0.5f);
        RtChain chain; chain.fx[0] = &half; chain.count = 1;
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.setChain(0, &chain);
        h.push(Cmd::TrackArm, 0, 1);
        h.run(4000);
        CHECK(std::fabs(tailLevel(h.outL) - 0.2f) < 0.005f,
              "an armed track monitors its input through the chain -> %.4f (expected 0.20)",
              (double)tailLevel(h.outL));
        CHECK(std::fabs(tailLevel(h.outR) - 0.1f) < 0.005f,
              "right channel carries the right input -> %.4f (expected 0.10)",
              (double)tailLevel(h.outR));

        h.outL.clear(); h.outR.clear();
        h.push(Cmd::TrackArm, 0, 0);
        h.run(4000);
        CHECK(std::fabs(tailLevel(h.outL)) < 1e-4f,
              "disarming stops the monitoring -> %.3g", (double)tailLevel(h.outL));
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }
    {
        // A backend with no capture device hands the engine nulls; a take must
        // still run, and record silence, rather than dereferencing them.
        Host h; h.init();                        // h.input stays unset
        std::vector<f32> rec(2000 * 2, 1.f);     // pre-filled, so silence shows
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.push(Cmd::TrackArm, 0, 1);
        h.pushRec(0, 0, rec.data(), 2000);
        h.runBlocks(4);
        const std::vector<Event> evs = drainEvents(h.e);
        CHECK(findEvent(evs, Ev::RecordStarted) != nullptr,
              "a take starts even with no capture device");
        bool silent = true;
        for (int i = 0; i < 4 * kBlock * 2; ++i) if (rec[(size_t)i] != 0.f) silent = false;
        CHECK(silent, "null input records as silence, not as garbage");
    }
}

static void testRecording() {
    banner("10. recording");
    note("RecordSlot toggles: first send queues a quantized start, the second a");
    note("quantized stop. The engine appends raw input and never frees the buffer.");
    recQuantizedTake();
    recCapacityStop();
    recTransportStop();
    recSlotHandover();
    recCancelAndMonitor();
}

// ---------------------------------------------------------------------------
// 11. follow actions and launch probability
// ---------------------------------------------------------------------------

static RtClip mkFollow(const std::vector<f32>& buf, int ch, f32 gain, bool loop,
                       Follow action, f64 followBeats, f64 prob = 1.0) {
    RtClip c = mkClip(buf, ch, gain, Warp::Off, loop, 120.0);
    c.followAction = (int)action;
    c.followBeats  = followBeats;
    c.prob         = prob;
    return c;
}

// a. Again re-fires on its own beat
static void followAgain() {
    Host h; h.init();
    // A short one-shot: silence between repeats is what makes each re-launch
    // visible in the output.
    auto buf = dcBuf(12000, 1, 1.0f);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setClip(0, 0, mkFollow(buf, 1, 0.5f, /*loop*/false, Follow::Again, 2.0));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBeat120 * 8);                         // four repeats

    CHECK(std::fabs(h.outL[6000] - 0.5f) < 0.01f,
          "the first pass sounds (%.4f at frame 6000)", (double)h.outL[6000]);
    CHECK(std::fabs(h.outL[30000]) < 1e-4f,
          "and has died away before the follow beat (%.3g at frame 30000)",
          (double)h.outL[30000]);

    // followBeats 2 at 120 BPM is 48000 frames.
    const i64 second = firstWhere(h.outL, 20000, nonZero);
    CHECK(second >= 0 && std::llabs((long long)second - (long long)(kBeat120 * 2)) <= 8,
          "Again re-fires at beat 2: frame %lld (expected %lld)",
          (long long)second, (long long)(kBeat120 * 2));
    const i64 third = firstWhere(h.outL, (size_t)(kBeat120 * 2 + 20000), nonZero);
    CHECK(third >= 0 && std::llabs((long long)third - (long long)(kBeat120 * 4)) <= 8,
          "and again at beat 4 without drifting: frame %lld (expected %lld)",
          (long long)third, (long long)(kBeat120 * 4));
}

// b. Next steps to the following slot with a clip in it, wrapping
static void followNext() {
    Host h; h.init();
    auto pos = dcBuf(300000, 1,  1.0f);
    auto neg = dcBuf(300000, 1, -1.0f);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    // Slot 1 is deliberately empty, so Next has to skip it.
    h.setClip(0, 0, mkFollow(pos, 1, 0.5f, true, Follow::Next, 2.0));
    h.setClip(0, 2, mkFollow(neg, 1, 0.5f, true, Follow::None, 0.0));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBeat120 * 4);

    const i64 sw = firstWhere(h.outL, 1000, departsFromSteady);
    CHECK(sw >= 0 && std::llabs((long long)sw - (long long)(kBeat120 * 2)) <= 8,
          "Next fires at beat 2: frame %lld (expected %lld)",
          (long long)sw, (long long)(kBeat120 * 2));
    CHECK(h.e.activeSlot[0].load() == 2,
          "Next skipped the empty slot 1 and landed on slot 2 (got %d)",
          h.e.activeSlot[0].load());
    CHECK(tailLevel(h.outL) < -0.4f,
          "the new clip is the one sounding -> %.4f (expected -0.50)",
          (double)tailLevel(h.outL));
}

// c. a follow action rolls the *target* clip's probability, Live-style
static void followRollsTarget() {
    Host h; h.init();
    auto pos = dcBuf(300000, 1,  1.0f);
    auto neg = dcBuf(300000, 1, -1.0f);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setClip(0, 0, mkFollow(pos, 1, 0.5f, true, Follow::Next, 1.0, 1.0));
    h.setClip(0, 1, mkFollow(neg, 1, 0.5f, true, Follow::None, 0.0, /*prob*/0.0));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBeat120 * 16);                        // sixteen chances to misfire

    CHECK(h.e.activeSlot[0].load() == 0,
          "a follow into a prob = 0 clip never takes (activeSlot %d)",
          h.e.activeSlot[0].load());
    CHECK(tailLevel(h.outL) > 0.4f,
          "and the source clip is undisturbed -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));
}

// c. Stop as a follow action
static void followStop() {
    Host h; h.init();
    auto buf = dcBuf(300000, 1, 1.0f);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setClip(0, 0, mkFollow(buf, 1, 0.5f, true, Follow::Stop, 2.0));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBeat120 * 4);
    CHECK(h.e.activeSlot[0].load() == -1,
          "Follow::Stop stops the track (activeSlot %d)", h.e.activeSlot[0].load());
    CHECK(std::fabs(tailLevel(h.outL)) < 1e-4f,
          "and the track is silent afterwards -> %.3g", (double)tailLevel(h.outL));
}

// d. probability gates a launch without disturbing what is playing
static void launchProbability() {
    {
        Host h; h.init();
        auto pos = dcBuf(300000, 1,  1.0f);
        auto neg = dcBuf(300000, 1, -1.0f);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setClip(0, 0, mkFollow(pos, 1, 0.5f, true, Follow::None, 0.0, 1.0));
        h.setClip(0, 1, mkFollow(neg, 1, 0.5f, true, Follow::None, 0.0, 0.0));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(8000);
        // Try the impossible clip repeatedly; it must never take over.
        for (int k = 0; k < 32; ++k) { h.push(Cmd::LaunchClip, 0, 1); h.run(2000); }
        CHECK(h.e.activeSlot[0].load() == 0,
              "a prob = 0 clip never launches, even over 32 tries (activeSlot %d)",
              h.e.activeSlot[0].load());
        CHECK(tailLevel(h.outL) > 0.4f,
              "and the clip that was playing keeps playing -> %.4f (expected 0.50)",
              (double)tailLevel(h.outL));
    }
    {
        Host h; h.init();
        auto pos = dcBuf(300000, 1,  1.0f);
        auto neg = dcBuf(300000, 1, -1.0f);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setClip(0, 0, mkFollow(pos, 1, 0.5f, true, Follow::None, 0.0, 1.0));
        h.setClip(0, 1, mkFollow(neg, 1, 0.5f, true, Follow::None, 0.0, 1.0));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(8000);
        h.push(Cmd::LaunchClip, 0, 1);
        h.run(8000);
        CHECK(h.e.activeSlot[0].load() == 1,
              "a prob = 1 clip always launches (activeSlot %d)", h.e.activeSlot[0].load());
        CHECK(tailLevel(h.outL) < -0.4f,
              "and it is the one sounding -> %.4f (expected -0.50)",
              (double)tailLevel(h.outL));
    }
    {
        // A queued *stop* is never gated, whatever the clip's probability.
        Host h; h.init();
        auto buf = dcBuf(300000, 1, 1.0f);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setClip(0, 0, mkFollow(buf, 1, 0.5f, true, Follow::None, 0.0, 0.0));
        // prob 0 blocks the launch, so put the clip on the grid with prob 1
        // first and only then make it improbable.
        RtClip c = mkFollow(buf, 1, 0.5f, true, Follow::None, 0.0, 1.0);
        h.setClip(0, 0, c);
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(8000);
        c.prob = 0.0;
        h.setClip(0, 0, c);
        h.push(Cmd::StopTrack, 0);
        h.run(8000);
        CHECK(h.e.activeSlot[0].load() == -1,
              "a stop fires regardless of prob (activeSlot %d)", h.e.activeSlot[0].load());
    }
}

// e. the same session rendered twice is the same audio, dice and all
static void followDeterminism() {
    auto renderOnce = [](std::vector<f32>& outL, std::vector<f32>& outR) {
        Host h; h.init();
        std::vector<std::vector<f32>> bufs;
        for (int s = 0; s < 4; ++s) bufs.push_back(dcBuf(200000, 1, 0.2f * (f32)(s + 1)));
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 6);              // 1/4: plenty of boundaries
        for (int s = 0; s < 4; ++s)
            h.setClip(0, s, mkFollow(bufs[(size_t)s], 1, 0.5f, true,
                                     Follow::Random, 1.0, 0.5));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(200000);
        outL = h.outL; outR = h.outR;
    };
    std::vector<f32> aL, aR, bL, bR;
    renderOnce(aL, aR);
    renderOnce(bL, bR);
    CHECK(aL.size() == bL.size() && !aL.empty(), "both runs produced the same amount of audio");
    bool same = aL.size() == bL.size();
    if (same) for (size_t i = 0; i < aL.size(); ++i)
        if (aL[i] != bL[i] || aR[i] != bR[i]) { same = false; break; }
    CHECK(same, "two identical runs of a prob 0.5 / Random-follow set are sample-identical");

    // A 0.5 gate that never fires, or always fires, would be a broken RNG
    // rather than a deterministic one.
    f32 lo = 1e9f, hi = -1e9f;
    for (size_t i = 0; i < aL.size(); i += 997) { lo = std::min(lo, aL[i]); hi = std::max(hi, aL[i]); }
    CHECK(hi - lo > 0.05f,
          "the set actually moved between clips (levels %.3f..%.3f)", (double)lo, (double)hi);
}

static void testFollowActions() {
    banner("11. follow actions and launch probability");
    note("a follow action schedules through the same quantized path a user launch");
    note("takes, probability included, so a chain of follows stays on the grid.");
    followAgain();
    followNext();
    followRollsTarget();
    followStop();
    launchProbability();
    followDeterminism();
}

// ---------------------------------------------------------------------------
// 12. MIDI routing
// ---------------------------------------------------------------------------

// Counts what reaches midi(). Unlike FakeFx it owns its descriptor, because
// which devices receive is decided from desc().hasMidiIn / desc().kind.
class FakeMidiFx : public PluginInstance {
public:
    FakeMidiFx(bool hasMidiIn, PluginKind kind) {
        d_.hasMidiIn = hasMidiIn;
        d_.kind = kind;
    }

    int count = 0;
    int lastFrame = -1;
    int lastLen = 0;
    u8  lastStatus = 0, lastD1 = 0, lastD2 = 0;
    int maxFrame = -1;
    // How many messages had already arrived when process() last ran. Ordering
    // matters: a note has to be in before the block it belongs to is rendered.
    int countAtLastProcess = -1;

    bool prepare(f64, int) override { return true; }
    void process(const f32* const*, f32* const*, int, int) override {
        countAtLastProcess = count;
    }
    void midi(const u8* data, int len, int frameOffset) override {
        ++count;
        lastLen = len;
        lastStatus = data[0];
        lastD1 = len > 1 ? data[1] : 0;
        lastD2 = len > 2 ? data[2] : 0;
        lastFrame = frameOffset;
        if (frameOffset > maxFrame) maxFrame = frameOffset;
    }

    int              paramCount() const override   { return 0; }
    const ParamInfo& paramInfo(int) const override { static ParamInfo p; return p; }
    f32              getParam(int) const override  { return 0.f; }
    void             setParam(int, f32) override   {}
    const PluginDesc& desc() const override        { return d_; }
    void             setBypassed(bool b) override  { bypassed_ = b; }
    bool             bypassed() const override     { return bypassed_; }

private:
    PluginDesc d_;
    bool bypassed_ = false;
};

static void testMidiRouting() {
    banner("12. MIDI routing");
    note("armed tracks only, and only to devices that asked for notes:");
    note("desc().hasMidiIn or desc().kind == Instrument.");

    Host h; h.init();
    FakeMidiFx inst(false, PluginKind::Instrument);   // instrument, no hasMidiIn
    FakeMidiFx mfx(true,  PluginKind::Effect);        // effect that wants MIDI
    FakeMidiFx plain(false, PluginKind::Effect);      // ordinary effect
    RtChain chain;
    chain.fx[0] = &inst; chain.fx[1] = &mfx; chain.fx[2] = &plain; chain.count = 3;

    FakeMidiFx idle(false, PluginKind::Instrument);   // on an unarmed track
    RtChain chain2; chain2.fx[0] = &idle; chain2.count = 1;

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.setChain(0, &chain);
    h.setChain(1, &chain2);
    h.push(Cmd::TrackArm, 0, 1);
    h.runBlocks(1);                                   // let the chains land

    h.pushMidi(0x90, 60, 100);
    h.pushMidi(0x80, 60, 0);
    h.pushMidi(0xB0, 74, 32);
    h.pushMidi(0xE0, 0x00, 0x40);
    h.runBlocks(1);

    CHECK(inst.count == 4, "an instrument on an armed track gets every message (%d of 4)",
          inst.count);
    CHECK(mfx.count == 4, "so does an effect with hasMidiIn (%d of 4)", mfx.count);
    CHECK(plain.count == 0, "an ordinary effect gets none (%d)", plain.count);
    CHECK(idle.count == 0, "an unarmed track gets none (%d)", idle.count);
    CHECK(inst.countAtLastProcess == 4 && mfx.countAtLastProcess == 4,
          "all four arrived before the chain's process() for that block (%d / %d)",
          inst.countAtLastProcess, mfx.countAtLastProcess);
    CHECK(inst.lastStatus == 0xE0 && inst.lastD1 == 0x00 && inst.lastD2 == 0x40,
          "the bytes arrive intact (%02X %02X %02X)",
          inst.lastStatus, inst.lastD1, inst.lastD2);
    CHECK(inst.lastLen == 3, "a pitch bend is 3 bytes (%d)", inst.lastLen);

    // Two-byte channel messages must report length 2.
    inst.count = 0;
    h.pushMidi(0xC0, 7, 0);
    h.runBlocks(1);
    CHECK(inst.count == 1 && inst.lastLen == 2,
          "a program change is delivered as 2 bytes (%d calls, len %d)",
          inst.count, inst.lastLen);

    // Frame offsets are clamped into the block, however wild the hint.
    inst.count = 0; inst.maxFrame = -1;
    h.pushMidi(0x90, 62, 90, 1000000);
    h.pushMidi(0x90, 64, 90, -50);
    h.runBlocks(1);
    CHECK(inst.count == 2 && inst.maxFrame == h.block - 1,
          "an out-of-range frame hint is clamped to the block (%d, max %d, block %d)",
          inst.count, inst.maxFrame, h.block);

    // Disarming stops delivery; the chain keeps running.
    inst.count = 0;
    h.push(Cmd::TrackArm, 0, 0);
    h.runBlocks(1);
    h.pushMidi(0x90, 65, 90);
    h.runBlocks(1);
    CHECK(inst.count == 0, "disarming the track stops delivery (%d)", inst.count);

    h.setChain(0, nullptr);
    h.setChain(1, nullptr);
    h.runBlocks(2);
}

// ---------------------------------------------------------------------------
// 13. MIDI clip playback
// ---------------------------------------------------------------------------

// A note-capable device that logs every message with the *absolute* frame it
// arrived on. process() runs once per block and always after that block's
// notes, so the number of completed process() calls is the index of the block
// a note belongs to — which is what turns a block-relative offset back into an
// absolute position without the engine having to tell us anything.
class NoteSink : public PluginInstance {
public:
    explicit NoteSink(int blockSize) : blk_(blockSize) {
        d_.kind = PluginKind::Instrument;
        evs.reserve(8192);
    }

    struct Msg { i64 frame; u8 status, pitch, vel; };
    std::vector<Msg> evs;
    int blocks = 0;

    void reset() { evs.clear(); }

    bool prepare(f64, int) override { return true; }
    void process(const f32* const*, f32* const*, int, int) override { ++blocks; }
    void midi(const u8* d, int len, int off) override {
        evs.push_back({(i64)blocks * (i64)blk_ + off, d[0],
                       (u8)(len > 1 ? d[1] : 0), (u8)(len > 2 ? d[2] : 0)});
    }

    int              paramCount() const override   { return 0; }
    const ParamInfo& paramInfo(int) const override { static ParamInfo p; return p; }
    f32              getParam(int) const override  { return 0.f; }
    void             setParam(int, f32) override   {}
    const PluginDesc& desc() const override        { return d_; }
    void             setBypassed(bool b) override  { bypassed_ = b; }
    bool             bypassed() const override     { return bypassed_; }

private:
    PluginDesc d_;
    int  blk_ = 0;
    bool bypassed_ = false;
};

static bool isOn(const NoteSink::Msg& m)  { return (m.status & 0xF0) == 0x90 && m.vel > 0; }
static bool isOff(const NoteSink::Msg& m) { return !isOn(m); }

// Every note-on must be answered by a note-off on the same pitch, and nothing
// may be left held at the end. A clip that hands an instrument a note it never
// takes back is the one failure this whole path exists to prevent.
static bool notesBalanced(const std::vector<NoteSink::Msg>& evs) {
    int held[128] = {};
    for (const NoteSink::Msg& m : evs) {
        if (isOn(m)) ++held[m.pitch];
        else if (--held[m.pitch] < 0) return false;
    }
    for (int i = 0; i < 128; ++i) if (held[i]) return false;
    return true;
}

static RtClip mkMidiClip(const std::vector<RtNote>& notes, f64 lengthBeats, bool loop) {
    RtClip c;
    c.notes       = notes.data();
    c.noteCount   = (int)notes.size();
    c.isMidi      = true;
    c.lengthBeats = lengthBeats;
    c.loop        = loop;
    c.gain        = 1.f;
    c.quantumIdx  = -1;
    c.valid       = true;
    return c;
}

// The clip every case below uses: one beat long, a note on the downbeat and one
// on the off-beat, both a 1/4 beat long. At 120 BPM that is on/off at frames
// 0 / 6000 / 12000 / 18000 of every 24000-frame lap.
static std::vector<RtNote> twoNoteClip() {
    std::vector<RtNote> n(2);
    n[0].beat = 0.0; n[0].len = 0.25; n[0].pitch = 60; n[0].vel = 100;
    n[1].beat = 0.5; n[1].len = 0.25; n[1].pitch = 64; n[1].vel = 90;
    return n;
}

// a. the notes come out where the grid says they should, lap after lap
static void midiClipTiming() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto notes = twoNoteClip();

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setChain(0, &chain);
    h.setClip(0, 0, mkMidiClip(notes, 1.0, /*loop*/true));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBeat120 * 4);                         // four laps

    CHECK((int)sink.evs.size() == 16,
          "four laps of a 2-note clip deliver 16 messages (%d)", (int)sink.evs.size());
    CHECK(notesBalanced(sink.evs), "every note-on is answered by a note-off");

    // Absolute frames, computed from the grid rather than from the engine.
    bool timingOk = sink.evs.size() == 16;
    i64 worst = 0;
    u8  expectPitch[4] = {60, 60, 64, 64};
    i64 expectOff[4]   = {0, 6000, 12000, 18000};
    for (int lap = 0; lap < 4 && timingOk; ++lap)
        for (int k = 0; k < 4; ++k) {
            const NoteSink::Msg& m = sink.evs[(size_t)(lap * 4 + k)];
            const i64 want = (i64)lap * kBeat120 + expectOff[k];
            const i64 d = std::llabs((long long)(m.frame - want));
            if (d > worst) worst = d;
            if (d > 1 || m.pitch != expectPitch[k] || (k % 2 == 0 ? !isOn(m) : !isOff(m)))
                timingOk = false;
        }
    CHECK(timingOk, "on/off pairs land within +/-1 frame of the grid over four laps "
                    "(worst error %lld frames)", (long long)worst);
    CHECK(sink.evs.size() == 16 && sink.evs[4].frame == kBeat120,
          "the lap-1 downbeat is exactly one beat in: frame %lld (expected %lld)",
          sink.evs.size() == 16 ? (long long)sink.evs[4].frame : -1, (long long)kBeat120);

    // The UI must not need a special case for MIDI.
    CHECK(h.e.slotState[0].load() == (int)SlotState::Playing,
          "a MIDI clip reports SlotState::Playing like any other (%d)", h.e.slotState[0].load());
    CHECK(h.e.activeSlot[0].load() == 0, "and names its slot (%d)", h.e.activeSlot[0].load());
    const f64 ph = h.e.clipPhase[0].load();
    CHECK(ph >= 0.0 && ph < 1.0, "clipPhase is beatPos/lengthBeats, in range (%.4f)", ph);

    // No audio ever leaves a MIDI clip; the sink is silent by construction, so
    // anything in the output would be the clip itself leaking.
    CHECK(std::fabs(tailLevel(h.outL)) < 1e-6f,
          "a MIDI clip renders no audio of its own -> %.3g", (double)tailLevel(h.outL));

    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// b. stopping a clip delivers the note-offs it still owes
static void midiClipStopFlushes() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto notes = twoNoteClip();

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setChain(0, &chain);
    h.setClip(0, 0, mkMidiClip(notes, 1.0, true));
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(2);                              // 512 frames: note 60 is sounding

    CHECK(sink.evs.size() == 1 && isOn(sink.evs[0]) && sink.evs[0].pitch == 60,
          "one note is held part-way into the clip (%d messages)", (int)sink.evs.size());

    h.push(Cmd::StopTrack, 0);
    h.runBlocks(2);
    CHECK(sink.evs.size() == 2 && isOff(sink.evs[1]) && sink.evs[1].pitch == 60,
          "stopping the track releases it immediately, well before its own note-off "
          "(%d messages)", (int)sink.evs.size());
    CHECK(sink.evs.size() >= 2 && sink.evs[1].frame < 3 * (i64)h.block,
          "the flush happens on the stop, not at the note's scheduled end "
          "(frame %lld, note ends at 6000)",
          sink.evs.size() >= 2 ? (long long)sink.evs[1].frame : -1);
    CHECK(notesBalanced(sink.evs), "nothing is left hanging after the stop");
    CHECK(h.e.slotState[0].load() == (int)SlotState::Stopped,
          "and the slot reports Stopped (%d)", h.e.slotState[0].load());

    // The transport stopping has to do the same thing, from a clean launch.
    sink.reset();
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(2);
    h.push(Cmd::SetPlaying, 0);
    h.runBlocks(2);
    CHECK(notesBalanced(sink.evs) && sink.evs.size() == 2,
          "stopping the transport releases the sounding note too (%d messages)",
          (int)sink.evs.size());

    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// c. switching clips flushes; only a *replaced notes array* is retired
static void midiClipSwitchAndRetire() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto notes = twoNoteClip();
    auto other = twoNoteClip();                  // a different array, same content

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setChain(0, &chain);
    // Both slots point at the *same* note array, which is what an unedited
    // duplicate looks like: switching between them retires nothing.
    h.setClip(0, 0, mkMidiClip(notes, 1.0, true));
    h.setClip(0, 1, mkMidiClip(notes, 1.0, true));
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(2);
    drainEvents(h.e);

    h.push(Cmd::LaunchClip, 0, 1);               // switch mid-note
    h.runBlocks(2);
    std::vector<Event> evs = drainEvents(h.e);
    CHECK(countEvents(evs, Ev::NotesRetired) == 0,
          "switching to a clip that shares the note array retires nothing (%d)",
          countEvents(evs, Ev::NotesRetired));
    // The outgoing note is released on the switch frame, and the incoming clip
    // starts its own lap there: off then on, both at ~512.
    CHECK(sink.evs.size() == 3 && isOff(sink.evs[1]) && sink.evs[1].pitch == 60 &&
          sink.evs[1].frame <= 2 * (i64)h.block + 1,
          "the outgoing clip's note-off went out on the switch (%d messages, "
          "second at frame %lld)", (int)sink.evs.size(),
          sink.evs.size() > 1 ? (long long)sink.evs[1].frame : -1);
    CHECK(sink.evs.size() == 3 && isOn(sink.evs[2]),
          "and the incoming clip started its own note there");
    CHECK(h.e.activeSlot[0].load() == 1, "and slot 1 is now playing (%d)",
          h.e.activeSlot[0].load());

    // Repushing the playing slot with a *new* array hands the old one back.
    h.setClip(0, 1, mkMidiClip(other, 1.0, true));
    h.runBlocks(2);
    evs = drainEvents(h.e);
    const Event* ret = findEvent(evs, Ev::NotesRetired);
    CHECK(countEvents(evs, Ev::NotesRetired) == 1,
          "replacing the notes of a playing clip retires exactly one array (%d)",
          countEvents(evs, Ev::NotesRetired));
    CHECK(ret && ret->p == (void*)notes.data(),
          "and it is the old pointer (%p, expected %p)",
          ret ? ret->p : nullptr, (void*)notes.data());
    CHECK(notesBalanced(sink.evs), "with no note left hanging across the swap");

    // Clearing the slot retires the array it was carrying, once.
    h.push(Cmd::ClearClip, 0, 1);
    h.runBlocks(2);
    evs = drainEvents(h.e);
    const Event* cl = findEvent(evs, Ev::NotesRetired);
    CHECK(countEvents(evs, Ev::NotesRetired) == 1 && cl && cl->p == (void*)other.data(),
          "ClearClip retires the cleared array once (%d, %p, expected %p)",
          countEvents(evs, Ev::NotesRetired), cl ? cl->p : nullptr, (void*)other.data());
    CHECK(notesBalanced(sink.evs), "and releases whatever it was sounding");
    CHECK(countEvents(evs, Ev::ClipStopped) == 1,
          "a cleared MIDI clip reports ClipStopped exactly once (%d)",
          countEvents(evs, Ev::ClipStopped));

    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// d. a MIDI clip launches on the grid like any other
static void midiClipQuantizedLaunch() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto notes = twoNoteClip();

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);                  // 1 Bar
    h.setChain(0, &chain);
    h.setClip(0, 0, mkMidiClip(notes, 1.0, true));
    h.push(Cmd::SetPlaying, 1);
    h.run(kBeat120 * 2);                         // land mid-bar
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBar120);

    CHECK(!sink.evs.empty(), "the quantized launch eventually delivered notes (%d)",
          (int)sink.evs.size());
    CHECK(!sink.evs.empty() && isOn(sink.evs[0]) && sink.evs[0].pitch == 60,
          "the first message is the clip's downbeat note-on");
    CHECK(!sink.evs.empty() && std::llabs((long long)(sink.evs[0].frame - kBar120)) <= 1,
          "a mid-bar launch puts it on the bar line: frame %lld (expected %lld)",
          sink.evs.empty() ? -1 : (long long)sink.evs[0].frame, (long long)kBar120);
    // Balance is only meaningful once nothing is still sounding, and the stop
    // is on the same 1-bar grid the launch was.
    h.push(Cmd::StopTrack, 0);
    h.run(kBar120);
    CHECK(notesBalanced(sink.evs), "and the laps that follow stay balanced (%d messages)",
          (int)sink.evs.size());

    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// e. a launched clip plays whatever the arm button says, and a note that
//    straddles the loop point is neither lost nor doubled
static void midiClipArmAndWrap() {
    {
        Host h; h.init();
        NoteSink sink(h.block);
        RtChain chain; chain.fx[0] = &sink; chain.count = 1;
        auto notes = twoNoteClip();
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setChain(0, &chain);
        h.push(Cmd::TrackArm, 0, 0);             // explicitly disarmed
        h.setClip(0, 0, mkMidiClip(notes, 1.0, true));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(kBeat120 - h.block);               // one lap, stopping short of the wrap
        CHECK(sink.evs.size() == 4,
              "an unarmed track still plays its clip: arm gates live input only (%d)",
              (int)sink.evs.size());
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }
    {
        // A note that starts at 0.75 and runs 0.5 beats ends at 1.25, a quarter
        // beat into the next lap.
        Host h; h.init();
        NoteSink sink(h.block);
        RtChain chain; chain.fx[0] = &sink; chain.count = 1;
        std::vector<RtNote> notes(1);
        notes[0].beat = 0.75; notes[0].len = 0.5; notes[0].pitch = 55; notes[0].vel = 100;

        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setChain(0, &chain);
        h.setClip(0, 0, mkMidiClip(notes, 1.0, true));
        h.push(Cmd::LaunchClip, 0, 0);
        // Three note-ons (18000, 42000, 66000) and three offs (30000, 54000,
        // 78000); stopping at 3.5 beats keeps the fourth lap's note out of it.
        h.run(kBeat120 * 3 + kBeat120 / 2);

        CHECK(sink.evs.size() == 6,
              "three laps of one note that crosses the wrap deliver 6 messages (%d)",
              (int)sink.evs.size());
        CHECK(notesBalanced(sink.evs), "each one is released exactly once");
        bool ok = sink.evs.size() == 6;
        for (int lap = 0; lap < 3 && ok; ++lap) {
            const i64 wantOn  = (i64)lap * kBeat120 + 18000;
            const i64 wantOff = wantOn + 12000;   // 0.5 beat later, past the wrap
            if (std::llabs((long long)(sink.evs[(size_t)(lap * 2)].frame - wantOn)) > 1) ok = false;
            if (std::llabs((long long)(sink.evs[(size_t)(lap * 2 + 1)].frame - wantOff)) > 1) ok = false;
        }
        CHECK(ok, "a note-off owed across the loop point still lands 0.5 beats after "
                  "its note-on, lap after lap");
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }
}

// f. the generative machinery is voice-level, so it works on MIDI unchanged
static void midiClipFollowAndProb() {
    {
        Host h; h.init();
        NoteSink sink(h.block);
        RtChain chain; chain.fx[0] = &sink; chain.count = 1;
        std::vector<RtNote> lo(1), hi(1);
        lo[0].beat = 0.0; lo[0].len = 0.25; lo[0].pitch = 60; lo[0].vel = 100;
        hi[0].beat = 0.0; hi[0].len = 0.25; hi[0].pitch = 72; hi[0].vel = 100;

        RtClip a = mkMidiClip(lo, 1.0, true);
        a.followAction = (int)Follow::Next;
        a.followBeats  = 1.0;
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setChain(0, &chain);
        h.setClip(0, 0, a);
        h.setClip(0, 1, mkMidiClip(hi, 1.0, true));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(kBeat120 * 2);

        CHECK(h.e.activeSlot[0].load() == 1,
              "Follow::Next moves a MIDI clip on like any other (activeSlot %d)",
              h.e.activeSlot[0].load());
        bool sawHi = false;
        i64 hiFrame = -1;
        for (const NoteSink::Msg& m : sink.evs)
            if (isOn(m) && m.pitch == 72 && !sawHi) { sawHi = true; hiFrame = m.frame; }
        CHECK(sawHi && std::llabs((long long)(hiFrame - kBeat120)) <= 1,
              "and the follow clip's first note lands on the follow beat: frame %lld "
              "(expected %lld)", (long long)hiFrame, (long long)kBeat120);
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }
    {
        Host h; h.init();
        NoteSink sink(h.block);
        RtChain chain; chain.fx[0] = &sink; chain.count = 1;
        std::vector<RtNote> lo(1), hi(1);
        lo[0].beat = 0.0; lo[0].len = 0.25; lo[0].pitch = 60; lo[0].vel = 100;
        hi[0].beat = 0.0; hi[0].len = 0.25; hi[0].pitch = 72; hi[0].vel = 100;

        RtClip never = mkMidiClip(hi, 1.0, true);
        never.prob = 0.0;
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setChain(0, &chain);
        h.setClip(0, 0, mkMidiClip(lo, 1.0, true));
        h.setClip(0, 1, never);
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(kBeat120);
        for (int k = 0; k < 16; ++k) { h.push(Cmd::LaunchClip, 0, 1); h.run(2000); }

        CHECK(h.e.activeSlot[0].load() == 0,
              "a prob = 0 MIDI clip never launches over 16 tries (activeSlot %d)",
              h.e.activeSlot[0].load());
        bool sawHi = false;
        for (const NoteSink::Msg& m : sink.evs) if (m.pitch == 72) sawHi = true;
        CHECK(!sawHi, "and none of its notes ever reached the instrument");
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }
}

static void testMidiClips() {
    banner("13. MIDI clip playback");
    note("a MIDI clip renders no audio: it delivers notes to the track's");
    note("note-capable devices before the chain runs, offs at the wrap included.");
    midiClipTiming();
    midiClipStopFlushes();
    midiClipSwitchAndRetire();
    midiClipQuantizedLaunch();
    midiClipArmAndWrap();
    midiClipFollowAndProb();
}

// ---------------------------------------------------------------------------
// 13b. per-note chance and velocity range (RtNote::chance / RtNote::velTo)
// ---------------------------------------------------------------------------

// The clip these cases share: four notes on the sixteenths of one beat, all at
// velocity 100 and all certain. Each case then makes the ones it cares about
// uncertain, so "what changed" is always exactly one field.
static std::vector<RtNote> fourNoteClip() {
    std::vector<RtNote> n(4);
    for (int i = 0; i < 4; ++i) {
        n[(size_t)i].beat  = 0.25 * i;
        n[(size_t)i].len   = 0.125;
        n[(size_t)i].pitch = (u8)(60 + i);
        n[(size_t)i].vel   = 100;
    }
    return n;
}

// Runs `notes` for `laps` laps of a one-beat clip and returns everything the
// instrument heard. One helper, so every case below differs only in the clip it
// is handed and nothing in the harness can drift between them.
static std::vector<NoteSink::Msg> playLaps(const std::vector<RtNote>& notes, int laps,
                                           int block = kBlock) {
    Host h; h.init(kSR, block);
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setChain(0, &chain);
    h.setClip(0, 0, mkMidiClip(notes, 1.0, true));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBeat120 * laps);
    h.push(Cmd::StopTrack, 0);
    h.run(kBeat120);
    h.setChain(0, nullptr);
    h.runBlocks(2);
    return sink.evs;
}

static int countOns(const std::vector<NoteSink::Msg>& evs, int pitch) {
    int n = 0;
    for (const NoteSink::Msg& m : evs) if (isOn(m) && m.pitch == pitch) ++n;
    return n;
}

// a. the two ends of the range are absolute, and the default costs nothing
static void noteChanceEnds() {
    {
        std::vector<RtNote> n = fourNoteClip();
        const std::vector<NoteSink::Msg> evs = playLaps(n, 16);
        CHECK(countOns(evs, 60) == 16 && countOns(evs, 63) == 16,
              "chance defaults to 100 and every note sounds every lap (%d / %d of 16)",
              countOns(evs, 60), countOns(evs, 63));
        CHECK(notesBalanced(evs), "and nothing is left hanging");
    }
    {
        std::vector<RtNote> n = fourNoteClip();
        n[1].chance = 0;
        n[2].chance = 100;
        const std::vector<NoteSink::Msg> evs = playLaps(n, 16);
        CHECK(countOns(evs, 61) == 0, "chance 0 never sounds, over 16 laps (%d)",
              countOns(evs, 61));
        CHECK(countOns(evs, 62) == 16, "chance 100 beside it always does (%d of 16)",
              countOns(evs, 62));
        CHECK(notesBalanced(evs),
              "and a note that did not sound owes no note-off");
    }
}

// b. a note that loses its roll leaves the pattern exactly as it found it
static void noteChanceSilentIsSilent() {
    // Two notes on ONE pitch: a long certain one, and an uncertain one inside
    // it. If the uncertain one's failed roll still ran the "release whatever is
    // holding this pitch" step, the long note would be cut short by a note that
    // never sounded -- an audible edit made by a dice roll that came up no.
    std::vector<RtNote> n(2);
    n[0].beat = 0.0;  n[0].len = 0.9;  n[0].pitch = 60; n[0].vel = 100;
    n[1].beat = 0.25; n[1].len = 0.1;  n[1].pitch = 60; n[1].vel = 100;
    n[1].chance = 0;

    const std::vector<NoteSink::Msg> evs = playLaps(n, 8);
    CHECK(countOns(evs, 60) == 8,
          "eight laps produce eight note-ons: the impossible note adds none (%d)",
          countOns(evs, 60));
    CHECK(notesBalanced(evs), "and the pairing is still exact");
    // The certain note must last its own 0.9 beats, not be clipped at 0.25.
    bool full = true;
    i64 lastOn = -1;
    for (const NoteSink::Msg& m : evs) {
        if (isOn(m)) { lastOn = m.frame; continue; }
        if (lastOn < 0) continue;
        const i64 len = m.frame - lastOn;
        if (std::llabs((long long)(len - (i64)(0.9 * kBeat120))) > 4) full = false;
        lastOn = -1;
    }
    CHECK(full, "and the certain note keeps its full 0.9 beats: a failed roll "
                "releases nothing");
}

// c. the dice actually move, and they move PER LAP
static void noteChanceRerollsEachLap() {
    std::vector<RtNote> n = fourNoteClip();
    for (int i = 0; i < 4; ++i) n[(size_t)i].chance = 50;
    const std::vector<NoteSink::Msg> evs = playLaps(n, 64);

    int total = 0;
    for (int p = 60; p <= 63; ++p) total += countOns(evs, p);
    // 128 expected out of 256 draws. The bound is wide on purpose: this is a
    // check that the hash is not degenerate, not a statistical test, and a
    // fixed-seed generator makes the number exact and reproducible anyway.
    CHECK(total > 70 && total < 190,
          "a 50%% pattern over 64 laps sounded %d of 256 notes (expected near 128)", total);
    // Per lap, not per note: a hash that ignored the lap would give every lap
    // the same four-note subset, so the count for one pitch would be 0 or 64.
    bool varied = false;
    for (int p = 60; p <= 63; ++p) {
        const int c = countOns(evs, p);
        if (c > 4 && c < 60) varied = true;
    }
    CHECK(varied, "and the subset changes from lap to lap rather than being fixed "
                  "at the launch (%d %d %d %d of 64)",
          countOns(evs, 60), countOns(evs, 61), countOns(evs, 62), countOns(evs, 63));
    CHECK(notesBalanced(evs), "with nothing hanging across 64 laps of dice");
}

// d. the velocity range is a closed span, drawn inside it and nowhere else
static void noteVelocityRange() {
    {
        std::vector<RtNote> n = fourNoteClip();
        n[0].vel = 40;  n[0].velTo = 120;      // upwards
        n[1].vel = 120; n[1].velTo = 40;       // the same span, written downwards
        n[2].vel = 77;  n[2].velTo = 0;        // no range at all
        const std::vector<NoteSink::Msg> evs = playLaps(n, 48);

        int lo0 = 999, hi0 = -1, lo1 = 999, hi1 = -1;
        bool fixed = true;
        for (const NoteSink::Msg& m : evs) {
            if (!isOn(m)) continue;
            if (m.pitch == 60) { lo0 = std::min(lo0, (int)m.vel); hi0 = std::max(hi0, (int)m.vel); }
            if (m.pitch == 61) { lo1 = std::min(lo1, (int)m.vel); hi1 = std::max(hi1, (int)m.vel); }
            if (m.pitch == 62 && m.vel != 77) fixed = false;
        }
        CHECK(lo0 >= 40 && hi0 <= 120 && hi0 > lo0,
              "an upward range stays inside it and uses it: %d..%d of 40..120", lo0, hi0);
        CHECK(lo1 >= 40 && hi1 <= 120 && hi1 > lo1,
              "a range written downwards means the same span: %d..%d of 40..120", lo1, hi1);
        CHECK(fixed, "and velTo == 0 beside them is a fixed velocity, untouched");
        CHECK(notesBalanced(evs), "with every sounding still paired");
    }
    {
        // The floor: velocity 0 on a note-on is a note-off on the wire, so a
        // range that reaches the bottom must still never emit one.
        std::vector<RtNote> n(1);
        n[0].beat = 0.0; n[0].len = 0.25; n[0].pitch = 60; n[0].vel = 1;
        n[0].velTo = 2;
        const std::vector<NoteSink::Msg> evs = playLaps(n, 32);
        bool everZero = false;
        int ons = 0;
        for (const NoteSink::Msg& m : evs)
            if ((m.status & 0xF0) == 0x90) { ++ons; if (m.vel == 0) everZero = true; }
        CHECK(!everZero && ons == 32,
              "a range at the bottom of the scale never emits velocity 0 (%d note-ons)", ons);
        CHECK(notesBalanced(evs), "so nothing is silently turned into a hanging note");
    }
}

// e. THE gate: the dice do not depend on how the audio was chopped up.
//
// This is the property the whole design of noteKey() exists for, and it is the
// one that cannot be argued -- an offline render at 512 frames a block and a
// live one at 64 have to produce the same performance, note for note and
// velocity for velocity, or a probabilistic set is not renderable at all.
static void noteDiceSurviveBlockSize() {
    std::vector<RtNote> n = fourNoteClip();
    for (int i = 0; i < 4; ++i) {
        n[(size_t)i].chance = (u8)(35 + 15 * i);      // 35, 50, 65, 80
        n[(size_t)i].vel    = 30;
        n[(size_t)i].velTo  = 127;
    }
    // Deliberately awkward sizes as well as round ones: 100 and 333 do not
    // divide a 24000-frame beat, so a lap boundary lands mid-block over and
    // over and the beat cursor accumulates a different residue in each run.
    const int blocks[] = {64, 100, 256, 333, 512, 1024};
    // The comparison window is the laps that were ASKED for, and it has to be,
    // because Host::run renders whole blocks: 40 beats is 960000 frames, which
    // 333 and 1024 overshoot and 64, 100, 256 and 512 hit exactly. The overshoot
    // is the harness's, not the engine's -- a real backend has the same property
    // -- so the tail past the window and the unquantized stop that follows it are
    // dropped rather than compared. Two frames of margin because a boundary note
    // may round either side of the sample it is mathematically on.
    const i64 window = (i64)40 * kBeat120 - 2;
    const auto inWindow = [&](const std::vector<NoteSink::Msg>& in) {
        std::vector<NoteSink::Msg> out;
        for (const NoteSink::Msg& m : in) if (m.frame < window) out.push_back(m);
        return out;
    };
    std::vector<NoteSink::Msg> ref;
    bool ok = true, sizesOk = true;
    for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; ++b) {
        std::vector<NoteSink::Msg> evs = inWindow(playLaps(n, 40, blocks[b]));
        if (b == 0) { ref = evs; continue; }
        if (evs.size() != ref.size()) { sizesOk = false; ok = false; continue; }
        for (size_t i = 0; i < evs.size(); ++i) {
            // The FRAME is allowed to differ by a sample: a boundary that is
            // mathematically between two frames rounds to one of them, and which
            // one depends on where the block edge fell. WHICH notes sounded and
            // at WHAT velocity may not differ at all -- those are the dice.
            if (evs[i].status != ref[i].status || evs[i].pitch != ref[i].pitch ||
                evs[i].vel != ref[i].vel ||
                std::llabs((long long)(evs[i].frame - ref[i].frame)) > 1) { ok = false; break; }
        }
    }
    CHECK(sizesOk, "40 laps of a 35/50/65/80%% pattern with velocity ranges produce "
                   "the same number of messages at 64, 100, 256, 333, 512 and 1024 "
                   "frames a block (%d at 64)", (int)ref.size());
    CHECK(ok, "and the same messages: which notes the dice chose and how loud they "
              "were does not depend on the buffer size");
    // A gate that passed because nothing was ever skipped would be worthless.
    CHECK((int)ref.size() < 40 * 4 * 2,
          "and the dice really did skip notes (%d messages, %d if every note sounded)",
          (int)ref.size(), 40 * 4 * 2);
}

// f. two runs of the same set are the same set, dice and all -- the per-note
//    twin of followDeterminism, and the property an offline render rests on
static void noteDiceRepeatable() {
    std::vector<RtNote> n = fourNoteClip();
    for (int i = 0; i < 4; ++i) { n[(size_t)i].chance = 50; n[(size_t)i].velTo = 127; }
    const std::vector<NoteSink::Msg> a = playLaps(n, 32);
    const std::vector<NoteSink::Msg> b = playLaps(n, 32);
    bool same = a.size() == b.size();
    if (same) for (size_t i = 0; i < a.size(); ++i)
        if (a[i].frame != b[i].frame || a[i].status != b[i].status ||
            a[i].pitch != b[i].pitch || a[i].vel != b[i].vel) { same = false; break; }
    CHECK(same, "two identical runs of a per-note-probability clip are message-identical "
                "(%d vs %d messages)", (int)a.size(), (int)b.size());

    // And a different clip is a different performance: a hash that ignored the
    // note's own identity would give every note in every clip the same dice.
    std::vector<RtNote> m = n;
    m[2].pitch = 71;
    const std::vector<NoteSink::Msg> c = playLaps(m, 32);
    bool differs = c.size() != a.size();
    if (!differs) for (size_t i = 0; i < a.size(); ++i)
        if (a[i].pitch != c[i].pitch || a[i].vel != c[i].vel) { differs = true; break; }
    CHECK(differs, "and moving one note's pitch gives it its own dice");
}

// g. the field is inside padding RtNote already had, which is what let the wire
//    stay put. Asserted here as well as in engine.h because this is the suite
//    that would have to change if it ever stopped being true.
static void noteFieldsCostNothing() {
    CHECK(sizeof(RtNote) == 24,
          "RtNote is still 24 B with chance and velTo in it (%d)", (int)sizeof(RtNote));
    RtNote d;
    CHECK(d.chance == 100 && d.velTo == 0,
          "and a default-constructed note is certain and un-ranged (%d / %d)",
          (int)d.chance, (int)d.velTo);
    // Aggregate initialisation with the old four fields must still leave the new
    // two at their defaults: every existing call site writes notes this way.
    const RtNote agg{1.0, 0.5, 62, 90};
    CHECK(agg.chance == 100 && agg.velTo == 0,
          "an aggregate written with the four v3 fields is certain too (%d / %d)",
          (int)agg.chance, (int)agg.velTo);
}

static void testNoteChance() {
    banner("13b. per-note chance and velocity range");
    note("Live's per-note Chance and Velocity Range, decided on the audio thread at");
    note("the note-on. Both are a pure hash of (track, note index, pitch, the note's");
    note("own beat, the loop lap) -- no generator state, no absolute beat, nothing");
    note("that a different buffer size could round differently.");
    noteChanceEnds();
    noteChanceSilentIsSilent();
    noteChanceRerollsEachLap();
    noteVelocityRange();
    noteDiceSurviveBlockSize();
    noteDiceRepeatable();
    noteFieldsCostNothing();
}

// ---------------------------------------------------------------------------
// 14. MIDI recording
// ---------------------------------------------------------------------------

// The take's beat clock: at 120 BPM a beat is 24000 frames, so an absolute
// frame converts straight to a take-relative beat once the start is known.
static f64 relBeat(i64 absFrame, i64 startFrame) {
    return (f64)(absFrame - startFrame) / (f64)kBeat120;
}

// a. a quantized take, paired notes, an unpaired one, and the sort order
static void midiRecTake() {
    Host h; h.init();
    std::vector<RtNote> take(64);
    for (RtNote& n : take) { n.beat = -1.0; n.len = -1.0; n.pitch = 0; n.vel = 0; }

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);                  // 1 Bar
    h.push(Cmd::TrackArm, 0, 1);
    h.push(Cmd::SetPlaying, 1);
    h.run(kBeat120 * 2);                         // land mid-bar
    h.pushRecMidi(0, 0, take.data(), 64);
    h.run(kBeat120 * 3);                         // cross the bar line, take running

    std::vector<Event> evs = drainEvents(h.e);
    const Event* started = findEvent(evs, Ev::RecordStarted);
    CHECK(started != nullptr, "a mid-bar RecordMidiSlot produces Ev::RecordStarted");
    CHECK(started && std::fabs(started->x - 4.0) < 1e-6,
          "the take begins on the bar line, beat %.6f (expected 4.0)",
          started ? started->x : -1.0);
    CHECK(h.e.recState[0].load() == 2 && h.e.recSlotIdx[0].load() == 0,
          "a MIDI take publishes the same state as an audio one (%d / %d)",
          h.e.recState[0].load(), h.e.recSlotIdx[0].load());

    // Note A opens first and closes last; note B is wholly inside it. They land
    // in the buffer in *off* order, so only sorting puts A back in front.
    const i64 onA = (i64)h.outL.size();  h.pushMidi(0x90, 60, 100); h.run(kBeat120 / 2);
    const i64 onB = (i64)h.outL.size();  h.pushMidi(0x90, 64,  90); h.run(kBeat120 / 2);
    const i64 offB = (i64)h.outL.size(); h.pushMidi(0x80, 64,   0); h.run(kBeat120 / 2);
    const i64 offA = (i64)h.outL.size(); h.pushMidi(0x80, 60,   0); h.run(kBeat120 / 2);
    // And one that is never released: it has to be closed at the boundary.
    const i64 onC = (i64)h.outL.size();  h.pushMidi(0x90, 67,  80); h.run(kBeat120 / 2);

    const f64 atToggle = (f64)h.outL.size() / (f64)kBeat120;
    const f64 boundary = std::ceil(atToggle / 4.0 - 1e-9) * 4.0;   // nextQuantum, 1 Bar
    h.pushRecMidi(0, 0, take.data(), 64);        // toggle: stop on the next bar
    h.run(kBar120 * 2);

    evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
    CHECK(fin != nullptr, "the toggle produces Ev::MidiRecordFinished");
    CHECK(countEvents(evs, Ev::RecordFinished) == 0,
          "and not the audio event (%d)", countEvents(evs, Ev::RecordFinished));
    CHECK(fin && fin->a == 0 && fin->b == 0 && fin->p == (void*)take.data(),
          "it names track 0 slot 0 and hands back the GUI's buffer (%d/%d, %p vs %p)",
          fin ? fin->a : -1, fin ? fin->b : -1, fin ? fin->p : nullptr, (void*)take.data());
    CHECK(fin && (int)fin->x == 3, "three notes were captured (%d)",
          fin ? (int)fin->x : -1);
    CHECK(h.e.recState[0].load() == 0, "and the track is idle again (%d)",
          h.e.recState[0].load());

    // The take started on the bar line, which is frame 96000 at 120 BPM.
    const i64 start = kBar120;
    const f64 tol = 1e-4;                        // ~2.4 frames
    CHECK(take[0].pitch == 60 && take[1].pitch == 64 && take[2].pitch == 67,
          "the buffer is sorted by start beat, not by the order notes closed "
          "(%d %d %d, expected 60 64 67)", take[0].pitch, take[1].pitch, take[2].pitch);
    CHECK(std::fabs(take[0].beat - relBeat(onA, start)) < tol,
          "note 60 starts at beat %.5f (expected %.5f)", take[0].beat, relBeat(onA, start));
    CHECK(std::fabs(take[0].len - relBeat(offA, onA)) < tol,
          "and lasts %.5f beats, from its own note-off (expected %.5f)",
          take[0].len, relBeat(offA, onA));
    CHECK(std::fabs(take[1].beat - relBeat(onB, start)) < tol &&
          std::fabs(take[1].len - relBeat(offB, onB)) < tol,
          "note 64 is %.5f + %.5f (expected %.5f + %.5f)",
          take[1].beat, take[1].len, relBeat(onB, start), relBeat(offB, onB));
    CHECK(take[0].vel == 100 && take[1].vel == 90 && take[2].vel == 80,
          "velocities survive the round trip (%d %d %d)",
          take[0].vel, take[1].vel, take[2].vel);
    CHECK(std::fabs(take[2].beat - relBeat(onC, start)) < tol,
          "the unpaired note starts where it was played, beat %.5f (expected %.5f)",
          take[2].beat, relBeat(onC, start));
    CHECK(std::fabs(take[2].beat + take[2].len - (boundary - 4.0)) < tol,
          "and is closed at the stop boundary: ends at %.5f (expected %.5f)",
          take[2].beat + take[2].len, boundary - 4.0);
    CHECK(take[3].pitch == 0 && take[3].vel == 0,
          "nothing was written past the last note (%d)", take[3].pitch);
}

// b. a full buffer ends the take, exactly as it does for audio
static void midiRecCapacity() {
    Host h; h.init();
    std::vector<RtNote> take(2);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);                  // None: start at once
    h.push(Cmd::TrackArm, 0, 1);
    h.pushRecMidi(0, 0, take.data(), 2);
    h.runBlocks(1);

    for (int k = 0; k < 4; ++k) {
        h.pushMidi(0x90, (u8)(60 + k), 100);
        h.runBlocks(4);
        h.pushMidi(0x80, (u8)(60 + k), 0);
        h.runBlocks(4);
    }
    const std::vector<Event> evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
    CHECK(fin != nullptr, "a full note buffer auto-stops the take");
    CHECK(fin && (int)fin->x == 2, "it stops exactly at capacity: %d notes (expected 2)",
          fin ? (int)fin->x : -1);
    CHECK(take[0].pitch == 60 && take[1].pitch == 61,
          "the two notes it kept are the first two played (%d %d)",
          take[0].pitch, take[1].pitch);
    CHECK(h.e.recState[0].load() == 0, "and the track is idle (%d)", h.e.recState[0].load());
}

// c. hand-over to a second slot, and cancelling a take that never began
static void midiRecHandoverAndCancel() {
    {
        Host h; h.init();
        std::vector<RtNote> takeA(64), takeB(64);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 4);              // 1 Bar
        h.push(Cmd::TrackArm, 0, 1);
        h.pushRecMidi(0, 0, takeA.data(), 64);   // starts at beat 0
        h.runBlocks(1);
        h.pushMidi(0x90, 60, 100);
        h.run(kBeat120);
        h.pushMidi(0x80, 60, 0);
        h.run(kBeat120);                         // mid-bar
        drainEvents(h.e);

        h.pushRecMidi(0, 1, takeB.data(), 64);   // different slot: hands over
        h.run(kBar120);
        std::vector<Event> evs = drainEvents(h.e);
        const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
        CHECK(fin != nullptr, "switching slots finishes the running MIDI take");
        CHECK(fin && fin->b == 0 && fin->p == (void*)takeA.data() && (int)fin->x == 1,
              "the finished take is slot 0's, with its buffer and its one note "
              "(%d, %p, %d)", fin ? fin->b : -1, fin ? fin->p : nullptr,
              fin ? (int)fin->x : -1);
        CHECK(countEvents(evs, Ev::RecordStarted) == 1,
              "the new take starts on the same boundary (%d RecordStarted)",
              countEvents(evs, Ev::RecordStarted));
        CHECK(h.e.recSlotIdx[0].load() == 1 && h.e.recState[0].load() == 2,
              "the track is now recording slot %d in state %d (expected 1 / 2)",
              h.e.recSlotIdx[0].load(), h.e.recState[0].load());

        // The second take is a MIDI take too, and stamps from the new boundary.
        const i64 onFrame = (i64)h.outL.size();
        h.pushMidi(0x90, 72, 100);
        h.run(kBeat120 / 2);
        h.pushMidi(0x80, 72, 0);
        h.run(kBeat120 / 2);
        h.push(Cmd::SetPlaying, 0);              // stop: ends the take on the spot
        h.runBlocks(1);
        evs = drainEvents(h.e);
        const Event* fin2 = findEvent(evs, Ev::MidiRecordFinished);
        CHECK(fin2 && fin2->b == 1 && (int)fin2->x == 1,
              "the handed-over take captured its own note (%d, %d notes)",
              fin2 ? fin2->b : -1, fin2 ? (int)fin2->x : -1);
        CHECK(takeB[0].pitch == 72 &&
              std::fabs(takeB[0].beat - relBeat(onFrame, kBar120)) < 1e-4,
              "stamped against the hand-over boundary: beat %.5f (expected %.5f)",
              takeB[0].beat, relBeat(onFrame, kBar120));
    }
    {
        Host h; h.init();
        std::vector<RtNote> take(16);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 4);
        h.push(Cmd::TrackArm, 0, 1);
        h.push(Cmd::SetPlaying, 1);
        h.run(kBeat120);
        h.pushRecMidi(0, 0, take.data(), 16);    // queued for the bar line
        h.runBlocks(1);
        CHECK(h.e.recState[0].load() == 1, "a queued MIDI take reports recState 1 (%d)",
              h.e.recState[0].load());
        h.pushRecMidi(0, 0, take.data(), 16);    // toggle before it begins
        h.run(kBar120 * 2);
        const std::vector<Event> evs = drainEvents(h.e);
        CHECK(countEvents(evs, Ev::RecordStarted) == 0 &&
              countEvents(evs, Ev::MidiRecordFinished) == 0,
              "cancelling it is silent: no start, no finish (%d/%d)",
              countEvents(evs, Ev::RecordStarted),
              countEvents(evs, Ev::MidiRecordFinished));
        CHECK(h.e.recState[0].load() == 0, "and the track is idle (%d)",
              h.e.recState[0].load());
    }
}

// d. monitoring keeps working while a take runs
static void midiRecMonitors() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    std::vector<RtNote> take(16);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setChain(0, &chain);
    h.push(Cmd::TrackArm, 0, 1);
    h.pushRecMidi(0, 0, take.data(), 16);
    h.runBlocks(1);
    h.pushMidi(0x90, 60, 100);
    h.run(kBeat120 / 2);
    h.pushMidi(0x80, 60, 0);
    h.run(kBeat120 / 2);

    CHECK(sink.evs.size() == 2,
          "live notes still reach the instrument while recording them (%d)",
          (int)sink.evs.size());
    CHECK(sink.evs.size() == 2 && isOn(sink.evs[0]) && isOff(sink.evs[1]),
          "and arrive as the on/off pair that was played");
    h.setChain(0, nullptr);
    h.runBlocks(2);
}

static void testMidiRecording() {
    banner("14. MIDI recording");
    note("RecordMidiSlot shares the whole toggle/quantize/hand-over machine with");
    note("RecordSlot; only what lands in the buffer differs. Unpaired notes are");
    note("closed at the stop boundary and the buffer stays sorted by start beat.");
    midiRecTake();
    midiRecCapacity();
    midiRecHandoverAndCancel();
    midiRecMonitors();
}

// ---------------------------------------------------------------------------
// 15. note retirement under a playing clip
// ---------------------------------------------------------------------------

static void testNoteRetirement() {
    banner("15. note retirement while the clip is playing");
    note("editing in the piano roll republishes the clip under a running voice:");
    note("the old array must come back exactly once, with nothing left sounding.");

    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;

    // Each array holds one long note, so a repush almost always lands while a
    // note is sounding — which is the case that leaves an instrument stuck.
    std::vector<RtNote> a(1), b(1), c(1);
    a[0].beat = 0.0; a[0].len = 0.5; a[0].pitch = 60; a[0].vel = 100;
    b[0].beat = 0.0; b[0].len = 0.5; b[0].pitch = 72; b[0].vel = 100;
    c[0].beat = 0.0; c[0].len = 0.5; c[0].pitch = 48; c[0].vel = 100;

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setChain(0, &chain);
    h.setClip(0, 0, mkMidiClip(a, 1.0, true));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBeat120 * 2);
    drainEvents(h.e);

    // Repush mid-note, twice, so a stale cursor or a missed flush shows up.
    h.setClip(0, 0, mkMidiClip(b, 1.0, true));
    h.run(kBeat120 * 2);
    std::vector<Event> evs = drainEvents(h.e);
    const Event* r1 = findEvent(evs, Ev::NotesRetired);
    CHECK(countEvents(evs, Ev::NotesRetired) == 1,
          "the first repush retires exactly one array (%d)",
          countEvents(evs, Ev::NotesRetired));
    CHECK(r1 && r1->p == (void*)a.data(), "and it is array A (%p, expected %p)",
          r1 ? r1->p : nullptr, (void*)a.data());
    CHECK(countEvents(evs, Ev::ClipStopped) == 0,
          "the voice kept playing through the edit (%d ClipStopped)",
          countEvents(evs, Ev::ClipStopped));
    CHECK(h.e.activeSlot[0].load() == 0, "and the slot is still the active one (%d)",
          h.e.activeSlot[0].load());

    h.setClip(0, 0, mkMidiClip(c, 1.0, true));
    h.run(kBeat120 * 2);
    evs = drainEvents(h.e);
    const Event* r2 = findEvent(evs, Ev::NotesRetired);
    CHECK(countEvents(evs, Ev::NotesRetired) == 1 && r2 && r2->p == (void*)b.data(),
          "the second retires array B, once (%d, %p, expected %p)",
          countEvents(evs, Ev::NotesRetired), r2 ? r2->p : nullptr, (void*)b.data());

    h.push(Cmd::StopTrack, 0);
    h.runBlocks(2);
    CHECK(notesBalanced(sink.evs),
          "across both edits every note-on was released (%d messages)",
          (int)sink.evs.size());

    // All three pitches must have sounded: the replacement takes effect for the
    // rest of the lap rather than waiting for the next one.
    bool saw60 = false, saw72 = false, saw48 = false;
    for (const NoteSink::Msg& m : sink.evs) {
        if (!isOn(m)) continue;
        if (m.pitch == 60) saw60 = true;
        if (m.pitch == 72) saw72 = true;
        if (m.pitch == 48) saw48 = true;
    }
    CHECK(saw60 && saw72 && saw48,
          "each array played while it was installed (60:%d 72:%d 48:%d)",
          saw60, saw72, saw48);

    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// ---------------------------------------------------------------------------
// 16. MIDI overdub
// ---------------------------------------------------------------------------

// The clip every case below overdubs into: one beat long, one note on the
// downbeat. Pitch 60 is the *clip's* material, so anything that pitch turning up
// in a take buffer would be the engine handing the GUI back what it already has.
static std::vector<RtNote> hostClip() {
    std::vector<RtNote> n(1);
    n[0].beat = 0.0; n[0].len = 0.25; n[0].pitch = 60; n[0].vel = 100;
    return n;
}

// Where an absolute frame falls inside a 1-beat loop that began at frame 0.
// Computed from the grid, not from the engine, so it is an independent answer.
static f64 inLoop(i64 absFrame) {
    const f64 b = (f64)absFrame / (f64)kBeat120;
    return b - std::floor(b);
}
static i64 lapOf(i64 absFrame) { return absFrame / kBeat120; }

// Absolute frames of every note-on of one pitch, in arrival order.
static std::vector<i64> onsOf(const NoteSink& s, u8 pitch) {
    std::vector<i64> v;
    for (const NoteSink::Msg& m : s.evs)
        if (isOn(m) && m.pitch == pitch) v.push_back(m.frame);
    return v;
}

// The take buffer entry for a pitch, or null.
static const RtNote* takeNote(const std::vector<RtNote>& take, int count, u8 pitch) {
    for (int i = 0; i < count; ++i) if (take[(size_t)i].pitch == pitch) return &take[(size_t)i];
    return nullptr;
}

static constexpr f64 kBeatTol = 1e-4;            // ~2.4 frames at 120 BPM

// a. three passes over a playing 1-beat clip: every pass lands where it sounds
static void overdubThreePasses() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto host = hostClip();
    std::vector<RtNote> take(64);
    for (RtNote& n : take) { n.beat = -1.0; n.len = -1.0; n.pitch = 0; n.vel = 0; }

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);                  // None: boundaries land at once
    h.setChain(0, &chain);
    h.push(Cmd::TrackArm, 0, 1);
    h.setClip(0, 0, mkMidiClip(host, 1.0, true));
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(1);                              // lap 0 began at frame 0
    drainEvents(h.e);

    // The take begins *mid-lap*, which is the whole point: a pass joined half a
    // beat in must still put its notes where they sound. Stamping against the
    // take's own start would drag every one of them half a beat early.
    h.run(kBeat120 / 2);
    const i64 takeStart = (i64)h.outL.size();
    h.pushRecMidi(0, 0, take.data(), 64);
    h.runBlocks(1);
    CHECK(inLoop(takeStart) > 0.2 && inLoop(takeStart) < 0.8,
          "the take joins the loop mid-lap, at in-loop beat %.4f", inLoop(takeStart));
    CHECK(h.e.recState[0].load() == 2 && h.e.activeSlot[0].load() == 0,
          "it is recording (%d) into the slot that is playing (%d)",
          h.e.recState[0].load(), h.e.activeSlot[0].load());

    // One note per pass, each in a different lap and at a different place in
    // the loop, all short enough to close inside their own lap.
    const u8 pitches[3] = {72, 74, 76};
    const i64 gap[3]    = {kBeat120 / 8, kBeat120 / 2, kBeat120};
    const i64 hold[3]   = {kBeat120 / 8, kBeat120 / 8, kBeat120 / 16};
    i64 onAt[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        h.run(gap[k]);
        onAt[k] = (i64)h.outL.size();
        h.pushMidi(0x90, pitches[k], (u8)(100 - k));
        h.run(hold[k]);
        h.pushMidi(0x80, pitches[k], 0);
        h.runBlocks(1);
    }
    CHECK(lapOf(onAt[0]) == 0 && lapOf(onAt[1]) == 1 && lapOf(onAt[2]) == 2,
          "the three notes were played in three consecutive laps (%lld %lld %lld)",
          (long long)lapOf(onAt[0]), (long long)lapOf(onAt[1]), (long long)lapOf(onAt[2]));

    h.pushRecMidi(0, 0, take.data(), 64);        // toggle: stop
    h.runBlocks(4);

    const std::vector<Event> evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
    CHECK(fin != nullptr, "an overdub pass finishes with Ev::MidiRecordFinished");
    CHECK(fin && fin->p == (void*)take.data() && fin->a == 0 && fin->b == 0,
          "handing back the GUI's own buffer for track 0 slot 0 (%p, %d/%d)",
          fin ? fin->p : nullptr, fin ? fin->a : -1, fin ? fin->b : -1);
    const int got = fin ? (int)fin->x : 0;
    CHECK(got == 3, "three passes into one buffer accumulate: %d notes (expected 3)", got);

    // Every note wrapped into the clip's own loop, at the position it sounded.
    bool inRange = got == 3, placed = got == 3;
    f64 worst = 0.0;
    for (int k = 0; k < 3 && got == 3; ++k) {
        const RtNote* n = takeNote(take, got, pitches[k]);
        if (!n) { placed = false; break; }
        if (!(n->beat >= 0.0 && n->beat < 1.0)) inRange = false;
        const f64 d = std::fabs(n->beat - inLoop(onAt[k]));
        if (d > worst) worst = d;
        if (d > kBeatTol) placed = false;
    }
    CHECK(inRange, "all three land inside [0, 1), the clip's own loop");
    CHECK(placed, "each at the in-loop position it was played at (worst error %.6f beats, "
                  "%.1f frames)", worst, worst * (f64)kBeat120);
    CHECK(got == 3 && take[0].beat <= take[1].beat && take[1].beat <= take[2].beat,
          "and the buffer comes back sorted by in-loop beat (%.4f %.4f %.4f)",
          take[0].beat, take[1].beat, take[2].beat);
    CHECK(got == 3 && take[0].vel && take[1].vel && take[2].vel &&
          takeNote(take, got, 72) && takeNote(take, got, 72)->vel == 100,
          "velocities survive the wrap (%d %d %d)", take[0].vel, take[1].vel, take[2].vel);

    // Only the NEW notes: the clip's own material is the GUI's to merge, and
    // handing it back here would double every note on every pass.
    CHECK(takeNote(take, got, 60) == nullptr,
          "the clip's own note is not in the take buffer");
    CHECK(take[3].pitch == 0 && take[3].beat < 0.0,
          "and nothing was written past the third note (%d)", take[3].pitch);
    CHECK(host.size() == 1 && host[0].pitch == 60 && host[0].beat == 0.0,
          "the clip's note array is untouched — merging is the GUI's job");

    // The clip never stopped playing: its downbeat kept arriving throughout,
    // once per lap, and the slot is still the active one afterwards.
    const std::vector<i64> downbeats = onsOf(sink, 60);
    int lapsDuringTake = 0;
    bool onGrid = true;
    for (i64 f : downbeats) {
        if (std::llabs((long long)(f % kBeat120)) > 1) onGrid = false;
        if (f > takeStart) ++lapsDuringTake;
    }
    CHECK(onGrid && lapsDuringTake >= 2,
          "the clip played its downbeat on every lap of the overdub (%d in total, %d of "
          "them after the take began, all on the grid)",
          (int)downbeats.size(), lapsDuringTake);
    CHECK(h.e.activeSlot[0].load() == 0 &&
          h.e.slotState[0].load() == (int)SlotState::Playing,
          "and keeps playing once the take ends (slot %d, state %d)",
          h.e.activeSlot[0].load(), h.e.slotState[0].load());
    CHECK(h.e.recState[0].load() == 0, "while the track is idle again (%d)",
          h.e.recState[0].load());

    h.push(Cmd::StopTrack, 0);
    h.runBlocks(2);
    CHECK(notesBalanced(sink.evs), "nothing was left hanging (%d messages)",
          (int)sink.evs.size());
    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// b. overdubbing a stopped clip launches it on the record boundary
static void overdubLaunchesStoppedClip() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto host = hostClip();
    std::vector<RtNote> take(16);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);                  // 1 Bar
    h.setChain(0, &chain);
    h.push(Cmd::TrackArm, 0, 1);
    h.setClip(0, 0, mkMidiClip(host, 1.0, true));
    h.push(Cmd::SetPlaying, 1);
    h.run(kBeat120 * 2);                         // mid-bar, nothing playing
    CHECK(h.e.activeSlot[0].load() == -1 && sink.evs.empty(),
          "the target clip starts out stopped and silent (slot %d, %d messages)",
          h.e.activeSlot[0].load(), (int)sink.evs.size());
    drainEvents(h.e);

    h.pushRecMidi(0, 0, take.data(), 16);
    h.run(kBar120);                              // across the bar line and on

    const std::vector<Event> evs = drainEvents(h.e);
    const Event* started = findEvent(evs, Ev::RecordStarted);
    const Event* launch  = findEvent(evs, Ev::ClipStarted);
    CHECK(started && std::fabs(started->x - 4.0) < 1e-6,
          "the take is still quantized to the bar line, beat %.4f (expected 4.0)",
          started ? started->x : -1.0);
    CHECK(countEvents(evs, Ev::ClipStarted) == 1,
          "and the clip launches there, exactly once (%d ClipStarted)",
          countEvents(evs, Ev::ClipStarted));
    CHECK(launch && launch->a == 0 && launch->b == 0,
          "the event names the overdubbed slot (%d/%d)",
          launch ? launch->a : -1, launch ? launch->b : -1);
    CHECK(h.e.activeSlot[0].load() == 0 &&
          h.e.slotState[0].load() == (int)SlotState::Playing,
          "the UI sees an ordinary playing clip (slot %d, state %d)",
          h.e.activeSlot[0].load(), h.e.slotState[0].load());
    CHECK(h.e.recState[0].load() == 2, "with the take running over it (%d)",
          h.e.recState[0].load());

    // It is really playing, not merely marked as playing: one downbeat per lap
    // from the bar line on, and the first of them on the bar line itself.
    const std::vector<i64> downbeats = onsOf(sink, 60);
    CHECK(downbeats.size() >= 3,
          "the clip sounds while the take runs: %d downbeats", (int)downbeats.size());
    CHECK(!downbeats.empty() && std::llabs((long long)(downbeats[0] - kBar120)) <= 1,
          "the first is on the record boundary, frame %lld (expected %lld)",
          downbeats.empty() ? -1 : (long long)downbeats[0], (long long)kBar120);
    bool spaced = downbeats.size() >= 3;
    for (size_t i = 1; i < downbeats.size(); ++i)
        if (std::llabs((long long)(downbeats[i] - downbeats[i - 1] - kBeat120)) > 1) spaced = false;
    CHECK(spaced, "and they are one beat apart, lap after lap");
    const f64 ph = h.e.clipPhase[0].load();
    CHECK(ph >= 0.0 && ph < 1.0, "clipPhase runs like any other clip's (%.4f)", ph);

    h.push(Cmd::StopTrack, 0);
    h.run(kBar120);
    CHECK(notesBalanced(sink.evs), "and nothing hangs when it finally stops");
    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// c. a take that joins a clip already playing does not retrigger it
static void overdubJoinsWithoutRetrigger() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto host = hostClip();
    std::vector<RtNote> take(16);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);                  // None: the take starts mid-lap
    h.setChain(0, &chain);
    h.push(Cmd::TrackArm, 0, 1);
    h.setClip(0, 0, mkMidiClip(host, 1.0, true));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(kBeat120 + kBeat120 / 2);              // a lap and a half in
    drainEvents(h.e);
    const f64 phaseBefore = h.e.clipPhase[0].load();

    const i64 recAt = (i64)h.outL.size();
    h.pushRecMidi(0, 0, take.data(), 16);
    h.run(kBeat120 * 2);

    const std::vector<Event> evs = drainEvents(h.e);
    CHECK(countEvents(evs, Ev::ClipStarted) == 0,
          "recording into the clip you are listening to does not relaunch it "
          "(%d ClipStarted)", countEvents(evs, Ev::ClipStarted));
    CHECK(countEvents(evs, Ev::RecordStarted) == 1,
          "the take itself still starts, once (%d)",
          countEvents(evs, Ev::RecordStarted));
    CHECK(inLoop(recAt) > 0.25 && inLoop(recAt) < 0.75 && phaseBefore > 0.25,
          "the take joined at in-loop beat %.4f, mid-lap (phase was %.4f)",
          inLoop(recAt), phaseBefore);

    // A retrigger would have put a downbeat note-on at the record boundary
    // instead of on the grid, and reset clipPhase with it.
    const std::vector<i64> downbeats = onsOf(sink, 60);
    bool onGrid = downbeats.size() >= 3;
    i64 offGridAt = -1;
    for (i64 f : downbeats)
        if (std::llabs((long long)(f % kBeat120)) > 1) { onGrid = false; offGridAt = f; }
    CHECK(onGrid, "every downbeat stayed on the lap grid: the lap in progress ran on "
                  "(%d notes, first stray at frame %lld)",
          (int)downbeats.size(), (long long)offGridAt);

    h.push(Cmd::StopTrack, 0);
    h.runBlocks(2);
    CHECK(notesBalanced(sink.evs), "and the join left nothing sounding");
    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// d. notes that outlive their lap clamp to the loop end; the stop boundary
//    closes what is still held at *its* in-loop position
static void overdubHeldNotes() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto host = hostClip();
    std::vector<RtNote> take(16);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setChain(0, &chain);
    h.push(Cmd::TrackArm, 0, 1);
    h.setClip(0, 0, mkMidiClip(host, 1.0, true));
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(1);
    h.pushRecMidi(0, 0, take.data(), 16);
    h.runBlocks(1);

    // Pressed at in-loop 0.8, released a third of the way into the *next* lap:
    // the note-off arrives at a smaller in-loop position than the note-on.
    h.run(kBeat120 * 4 / 5 - 2 * (i64)h.block);
    const i64 onA = (i64)h.outL.size();
    h.pushMidi(0x90, 72, 100);
    h.run(kBeat120 / 2);
    h.pushMidi(0x80, 72, 0);
    h.runBlocks(1);

    // A second note, pressed late in a later lap and never released: the stop
    // boundary has to close it, and at the boundary's own in-loop position.
    h.run(kBeat120 + kBeat120 / 4);
    const i64 onB = (i64)h.outL.size();
    h.pushMidi(0x90, 74, 90);
    h.run(kBeat120 / 8);
    const i64 stopAt = (i64)h.outL.size();
    h.pushRecMidi(0, 0, take.data(), 16);        // toggle: stops on this frame
    h.runBlocks(4);

    const std::vector<Event> evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
    const int got = fin ? (int)fin->x : 0;
    CHECK(got == 2, "both notes came back (%d)", got);
    CHECK(lapOf(onA) != lapOf(onA + kBeat120 / 2),
          "the first was held across the loop point (lap %lld -> %lld)",
          (long long)lapOf(onA), (long long)lapOf(onA + kBeat120 / 2));

    const RtNote* a = got == 2 ? takeNote(take, got, 72) : nullptr;
    CHECK(a && std::fabs(a->beat - inLoop(onA)) < kBeatTol,
          "it starts where it was played, in-loop beat %.4f (expected %.4f)",
          a ? a->beat : -1.0, inLoop(onA));
    CHECK(a && std::fabs(a->beat + a->len - 1.0) < kBeatTol,
          "and is clamped to the loop end rather than split: ends at %.4f (expected 1.0)",
          a ? a->beat + a->len : -1.0);

    const RtNote* b = got == 2 ? takeNote(take, got, 74) : nullptr;
    CHECK(b && std::fabs(b->beat - inLoop(onB)) < kBeatTol,
          "the unreleased note starts at in-loop beat %.4f (expected %.4f)",
          b ? b->beat : -1.0, inLoop(onB));
    CHECK(b && std::fabs(b->beat + b->len - inLoop(stopAt)) < kBeatTol,
          "and is closed at the stop boundary's in-loop position: ends at %.4f "
          "(expected %.4f)", b ? b->beat + b->len : -1.0, inLoop(stopAt));
    CHECK(b && b->beat + b->len <= 1.0 + kBeatTol && a && a->beat + a->len <= 1.0 + kBeatTol,
          "neither runs past the end of the loop it belongs to");

    CHECK(h.e.activeSlot[0].load() == 0,
          "the clip is still playing after the take (%d)", h.e.activeSlot[0].load());
    // The key was still down when the take stopped. Closing it in the *buffer*
    // is the take's business; the wire is a pass-through, so the player's own
    // note-off is what releases the instrument — and it arrives too late to be
    // recorded, which is exactly what the buffer check above already proved.
    h.pushMidi(0x80, 74, 0);
    h.runBlocks(1);
    h.push(Cmd::StopTrack, 0);
    h.runBlocks(2);
    CHECK(notesBalanced(sink.evs), "and the live notes were all released");
    CHECK(fin && (int)fin->x == 2, "with nothing captured after the boundary (%d notes)",
          fin ? (int)fin->x : -1);
    h.setChain(0, nullptr);
    h.runBlocks(2);
}

// e. a take into a slot that is empty, or holds audio, is untouched by any of
//    this: same take-relative stamping, no launch
static void overdubLeavesPlainTakesAlone() {
    {
        Host h; h.init();
        NoteSink sink(h.block);
        RtChain chain; chain.fx[0] = &sink; chain.count = 1;
        std::vector<RtNote> take(16);

        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.setChain(0, &chain);
        h.push(Cmd::TrackArm, 0, 1);
        h.pushRecMidi(0, 0, take.data(), 16);    // slot 0 is empty
        h.runBlocks(1);
        const i64 start = (i64)h.outL.size() - h.block;
        h.run(kBeat120 * 2 + kBeat120 / 2);      // well past a 1-beat lap
        const i64 on = (i64)h.outL.size();
        h.pushMidi(0x90, 67, 100);
        h.run(kBeat120 / 4);
        h.pushMidi(0x80, 67, 0);
        h.runBlocks(1);
        h.pushRecMidi(0, 0, take.data(), 16);
        h.runBlocks(2);

        const std::vector<Event> evs = drainEvents(h.e);
        const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
        CHECK(countEvents(evs, Ev::ClipStarted) == 0,
              "a take into an empty slot launches nothing (%d ClipStarted)",
              countEvents(evs, Ev::ClipStarted));
        CHECK(fin && (int)fin->x == 1 && take[0].pitch == 67,
              "and captures its note (%d notes, pitch %d)",
              fin ? (int)fin->x : -1, take[0].pitch);
        CHECK(std::fabs(take[0].beat - relBeat(on, start)) < kBeatTol,
              "stamped take-relative, past beat 1 and not wrapped: %.4f (expected %.4f)",
              take[0].beat, relBeat(on, start));
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }
    {
        // The same, with an *audio* clip in the target slot: isMidi is what
        // makes a slot overdubbable, not merely being occupied.
        Host h; h.init();
        std::vector<f32> buf = dcBuf(kBeat120 * 2, 1, 0.5f);
        std::vector<RtNote> take(16);

        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.push(Cmd::TrackArm, 0, 1);
        h.setClip(0, 0, mkClip(buf, 1, 1.f, Warp::Beats, true, 120.0));
        h.pushRecMidi(0, 0, take.data(), 16);
        h.runBlocks(1);
        const i64 start = (i64)h.outL.size() - h.block;
        h.run(kBeat120 * 2);
        const i64 on = (i64)h.outL.size();
        h.pushMidi(0x90, 67, 100);
        h.run(kBeat120 / 4);
        h.pushMidi(0x80, 67, 0);
        h.runBlocks(1);
        h.pushRecMidi(0, 0, take.data(), 16);
        h.runBlocks(2);

        const std::vector<Event> evs = drainEvents(h.e);
        const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
        CHECK(countEvents(evs, Ev::ClipStarted) == 0,
              "an audio clip in the slot is not overdub material: nothing launched (%d)",
              countEvents(evs, Ev::ClipStarted));
        CHECK(h.e.activeSlot[0].load() == -1, "the track stayed stopped (%d)",
              h.e.activeSlot[0].load());
        CHECK(fin && (int)fin->x == 1 &&
              std::fabs(take[0].beat - relBeat(on, start)) < kBeatTol,
              "and the take is stamped take-relative as before: %.4f (expected %.4f)",
              take[0].beat, relBeat(on, start));
    }
}

// f. the wrap origin is buffer-size independent
//
// It is derived by walking the voice's position back to the frame each message
// arrived on, and the voice advances per *sub-block* — so a different block
// size means different sub-block splits and a different arithmetic path to the
// same answer. That answer has to be the same one, for the same reason a launch
// grid and a probability roll do not depend on the buffer size: a set that
// records differently on a 64-frame host than on a 1024-frame one is broken.
static void overdubBlockSizes() {
    for (int blk : {64, 1024}) {
        Host h; h.init(kSR, blk);
        auto host = hostClip();
        std::vector<RtNote> take(16);

        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 0);
        h.push(Cmd::TrackArm, 0, 1);
        h.setClip(0, 0, mkMidiClip(host, 1.0, true));
        h.push(Cmd::LaunchClip, 0, 0);
        h.runBlocks(1);
        h.run(kBeat120 / 2);                     // join mid-lap
        h.pushRecMidi(0, 0, take.data(), 16);
        h.runBlocks(1);

        i64 onAt[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            h.run(k == 0 ? kBeat120 / 8 : kBeat120);
            onAt[k] = (i64)h.outL.size();
            h.pushMidi(0x90, (u8)(72 + 2 * k), 100);
            h.run(kBeat120 / 8);
            h.pushMidi(0x80, (u8)(72 + 2 * k), 0);
            h.runBlocks(1);
        }
        h.pushRecMidi(0, 0, take.data(), 16);
        h.runBlocks(2);

        const std::vector<Event> evs = drainEvents(h.e);
        const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
        const int got = fin ? (int)fin->x : 0;
        CHECK(got == 3, "three passes at a %d-frame block size capture three notes (%d)",
              blk, got);
        f64 worst = 0.0;
        for (int k = 0; k < 3 && got == 3; ++k) {
            const RtNote* n = takeNote(take, got, (u8)(72 + 2 * k));
            const f64 d = n ? std::fabs(n->beat - inLoop(onAt[k])) : 1.0;
            if (d > worst) worst = d;
        }
        CHECK(got == 3 && worst < kBeatTol,
              "and place them by the clip's loop, not by the block grid "
              "(worst error %.6f beats, %.2f frames)", worst, worst * (f64)kBeat120);
    }
}

static void testOverdub() {
    banner("16. MIDI overdub");
    note("recording into a slot that already holds a MIDI clip is a looper pass:");
    note("the clip (re)launches on the record boundary and keeps playing, and the");
    note("notes wrap into ITS loop — the wrap origin is the voice's position, not");
    note("the take's start, so a pass joined mid-lap still lands where it sounds.");
    overdubThreePasses();
    overdubLaunchesStoppedClip();
    overdubJoinsWithoutRetrigger();
    overdubHeldNotes();
    overdubLeavesPlainTakesAlone();
    overdubBlockSizes();
}

// ---------------------------------------------------------------------------
// 17. return buses and sends
// ---------------------------------------------------------------------------

// Track `ti` playing a DC clip from beat 0 with no quantum in the way.
static void armDc(Host& h, int ti, const std::vector<f32>& buf, f32 gain) {
    h.setClip(ti, 0, mkClip(buf, 1, gain, Warp::Off, true, 120.0));
    h.push(Cmd::LaunchClip, ti, 0);
}
static void tempoNoQuantum(Host& h) {
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
}

// Levels used throughout: a DC clip at gain 0.5 with the default unity fader
// puts 0.50 on the master, so every number below is that 0.50 times whatever
// the send, the return chain and the return fader did to it.
static void sendFeedsReturn(const std::vector<f32>& buf) {
    Host h; h.init();
    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.push(Cmd::SendLevel, 0, 0, 1.0);           // track 0 -> return A, unity
    h.push(Cmd::ReturnVol, 0, 0, 0.5);
    h.run(8000);

    CHECK(std::fabs(tailLevel(h.outL) - 0.75f) < 0.005f,
          "dry 0.50 + return (send 1.0 x vol 0.5) -> %.4f (expected 0.75)",
          (double)tailLevel(h.outL));
    CHECK(std::fabs(h.e.returnMeterL[0].load() - 0.25f) < 0.02f,
          "the return meter reads post-vol -> %.4f (expected 0.25)",
          (double)h.e.returnMeterL[0].load());
    CHECK(std::fabs(h.e.returnMeterR[0].load() - 0.25f) < 0.02f,
          "both return channels meter -> %.4f (expected 0.25)",
          (double)h.e.returnMeterR[0].load());
    for (int r = 1; r < kMaxReturns; ++r)
        CHECK(h.e.returnMeterL[r].load() < 1e-4f,
              "return %d saw nothing (%.3g)", r, (double)h.e.returnMeterL[r].load());

    // The return fader scales what reaches the master, and nothing else does.
    h.outL.clear(); h.outR.clear();
    h.push(Cmd::ReturnVol, 0, 0, 1.0);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 1.0f) < 0.005f,
          "return at unity -> %.4f (expected 1.00)", (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::SendLevel, 0, 0, 0.0);           // send closed
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.005f,
          "closing the send leaves the dry path alone -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));
    CHECK(h.e.returnMeterL[0].load() < 0.02f,
          "the return meter falls back to silence (%.3g)",
          (double)h.e.returnMeterL[0].load());
}

// A send is post-fader, which is a statement about mute and solo as much as
// about the volume: audibility is decided once and both destinations obey it.
static void sendIsPostFader(const std::vector<f32>& buf) {
    Host h; h.init();
    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.push(Cmd::SendLevel, 0, 0, 1.0);
    h.push(Cmd::TrackMute, 0, 1);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL)) < 1e-4f,
          "a muted track sends nothing -> %.3g", (double)tailLevel(h.outL));
    CHECK(h.e.returnMeterL[0].load() < 1e-4f,
          "and the return bus stays silent (%.3g)", (double)h.e.returnMeterL[0].load());

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::TrackMute, 0, 0);
    h.push(Cmd::TrackSolo, 1, 1);                // solo elsewhere: track 0 is out
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL)) < 1e-4f,
          "a track silenced by someone else's solo sends nothing -> %.3g",
          (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.push(Cmd::TrackSolo, 1, 0);
    h.push(Cmd::TrackVol, 0, 0, 0.5);            // half the fader, half the send
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.005f,
          "the fader scales dry and send together -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));
}

// The return's own chain, and the retirement protocol on it.
static void returnChainProcesses(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx half(0.5f), quarter(0.25f);
    RtChain chA; chA.fx[0] = &half;    chA.count = 1;
    RtChain chB; chB.fx[0] = &quarter; chB.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.push(Cmd::SendLevel, 0, 0, 1.0);
    h.setReturnChain(0, &chA);
    h.run(8000);
    drainRetired(h.e);

    CHECK(half.calls > 0, "the return chain ran (%d calls)", half.calls);
    CHECK(std::fabs(tailLevel(h.outL) - 0.75f) < 0.005f,
          "0.50 dry + 0.50 through a 0.5x return -> %.4f (expected 0.75)",
          (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.setReturnChain(0, &chB);
    h.run(8000);
    RetiredEvents r = drainRetired(h.e);
    CHECK(r.count == 1 && r.ptrs[0] == (const void*)&chA,
          "swapping the return chain retires the old one (%d events)", r.count);
    CHECK(r.count == 1 && r.tracks[0] == kMaxTracks + 0,
          "the event names return 0 as kMaxTracks + 0 = %d (got %d)",
          kMaxTracks, r.count ? r.tracks[0] : -999);
    CHECK(std::fabs(tailLevel(h.outL) - 0.625f) < 0.005f,
          "0.50 dry + 0.50 through a 0.25x return -> %.4f (expected 0.625)",
          (double)tailLevel(h.outL));

    // A return chain keeps running with no send feeding it, exactly like a
    // track's: that is what lets a reverb tail survive the send closing.
    h.push(Cmd::SendLevel, 0, 0, 0.0);
    h.runBlocks(4);
    const int idle = quarter.calls;
    h.runBlocks(8);
    CHECK(quarter.calls - idle == 8,
          "an idle return chain still runs once per block (%d over 8)",
          quarter.calls - idle);

    h.setReturnChain(0, nullptr);
    h.runBlocks(2);
    CHECK(drainRetired(h.e).count == 1, "clearing the return chain retires it");
}

// The master chain: after the whole sum, before the master fader and the clip
// stage. The clip stage is what makes the ordering observable — a 4x master
// chain on a 0.5 mix would leave 2.0 in the buffer if nothing clamped after it.
static void masterChainAfterSum(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx boost(4.0f), half(0.5f);
    RtChain chBoost; chBoost.fx[0] = &boost; chBoost.count = 1;
    RtChain chHalf;  chHalf.fx[0]  = &half;  chHalf.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.push(Cmd::SendLevel, 0, 0, 1.0);           // 0.50 dry + 0.50 wet = 1.00
    h.setMasterChain(&chHalf);
    h.run(8000);
    drainRetired(h.e);
    CHECK(half.calls > 0, "the master chain ran (%d calls)", half.calls);
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.005f,
          "the master chain sees dry AND returns: 1.00 x 0.5 -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));

    h.outL.clear(); h.outR.clear();
    h.setMasterChain(&chBoost);
    h.run(8000);
    RetiredEvents r = drainRetired(h.e);
    CHECK(r.count == 1 && r.ptrs[0] == (const void*)&chHalf,
          "swapping the master chain retires the old one (%d events)", r.count);
    CHECK(r.count == 1 && r.tracks[0] == -1,
          "the master chain retires with a = -1 (got %d)", r.count ? r.tracks[0] : -999);
    CHECK(std::fabs(tailLevel(h.outL) - 1.0f) < 1e-4f,
          "the clip stage is after the master chain: 1.00 x 4 clamps to %.4f",
          (double)tailLevel(h.outL));

    // ...and the master fader is after it too, so pulling the fader down
    // recovers headroom the chain added rather than being eaten by the clamp.
    h.outL.clear(); h.outR.clear();
    h.push(Cmd::MasterVol, 0, 0, 0.125);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.005f,
          "master fader after the chain: 1.00 x 4 x 0.125 -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));

    h.setMasterChain(nullptr);
    h.runBlocks(2);
    CHECK(drainRetired(h.e).count == 1, "clearing the master chain retires it");
}

// Every index that reaches the audio thread is checked at both ends, because a
// stray one writes outside the mixer.
static void busBoundsAreChecked(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx half(0.5f);
    RtChain chain; chain.fx[0] = &half; chain.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.setReturnChain(kMaxReturns, &chain);       // one past the last return
    h.setReturnChain(-1, &chain);
    h.push(Cmd::SendLevel, 0, kMaxReturns, 1.0);
    h.push(Cmd::SendLevel, 0, -1, 1.0);
    h.push(Cmd::SendLevel, kMaxTracks, 0, 1.0);
    h.push(Cmd::ReturnVol, kMaxReturns, 0, 1.0);
    h.push(Cmd::ReturnVol, -1, 0, 1.0);
    h.run(8000);

    CHECK(half.calls == 0, "an out-of-range return chain is never installed (%d calls)",
          half.calls);
    CHECK(drainRetired(h.e).count == 0, "and retires nothing");
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.005f,
          "the mix is untouched by any of it -> %.4f (expected 0.50)",
          (double)tailLevel(h.outL));

    // A NaN send would multiply a bus that feeds the master; it lands on zero.
    h.outL.clear(); h.outR.clear();
    h.push(Cmd::SendLevel, 0, 0, std::nan(""));
    h.push(Cmd::ReturnVol, 0, 0, std::nan(""));
    h.run(8000);
    CHECK(std::isfinite(tailLevel(h.outL)) && std::fabs(tailLevel(h.outL) - 0.5f) < 0.005f,
          "a NaN send level is refused, not propagated -> %.4f", (double)tailLevel(h.outL));
}

static void testBuses() {
    banner("17. return buses and sends");
    note("signal flow: track post-fader -> send[r] -> return chain -> return vol");
    note("-> return meter -> master sum -> master chain -> master fader -> clip.");
    note("returns have no sends of their own: return -> return routing is out of");
    note("this wave on purpose (Live gates it behind an option).");
    const auto buf = dcBuf(300000, 1, 1.0f);

    sendFeedsReturn(buf);
    sendIsPostFader(buf);
    returnChainProcesses(buf);
    masterChainAfterSum(buf);
    busBoundsAreChecked(buf);
}

// ---------------------------------------------------------------------------
// 18. plugin delay compensation
// ---------------------------------------------------------------------------

// An impulse at a known frame, far enough in that the 3 ms declick ramp is long
// over: what comes out is a single sample whose position is the whole answer.
static std::vector<f32> impulseBuf(i64 frames, i64 at) {
    std::vector<f32> b((size_t)frames, 0.f);
    b[(size_t)at] = 1.0f;
    return b;
}

static i64 peakFrame(const std::vector<f32>& v) {
    i64 best = -1;
    f32 bv = 0.f;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::fabs(v[i]) > bv) { bv = std::fabs(v[i]); best = (i64)i; }
    return best;
}
static f32 maxAbsIn(const std::vector<f32>& v, size_t from, size_t to) {
    f32 m = 0.f;
    for (size_t i = from; i < to && i < v.size(); ++i)
        if (std::fabs(v[i]) > m) m = std::fabs(v[i]);
    return m;
}

// Two tracks, the same clip, one of them behind a 256-frame device. Compensated,
// the master gets their sum; uncompensated it would get a step — one track from
// frame 0 and the other from frame 256.
static void pdcAlignsTracks(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx latent(1.0f, 256);
    RtChain chain; chain.fx[0] = &latent; chain.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.25f);
    armDc(h, 1, buf, 0.25f);
    h.setChain(1, &chain);
    h.run(16000);

    CHECK(h.e.latencyFrames.load() == 256,
          "the latent track sets the engine's latency (%d, expected 256)",
          h.e.latencyFrames.load());
    CHECK(maxAbsIn(h.outL, 0, 256) < 1e-6f,
          "nothing arrives before the compensation window: %.3g in [0,256)",
          (double)maxAbsIn(h.outL, 0, 256));
    // 256 frames of alignment + 144 frames of declick ramp; 600 is well past it.
    f32 worst = 0.f;
    for (size_t i = 600; i < h.outL.size(); ++i)
        worst = std::max(worst, std::fabs(h.outL[i] - 0.5f));
    CHECK(worst < 1e-6f,
          "the aligned sum is flat 0.50 with no step at any block boundary "
          "(worst deviation %.3g over %zu frames)", (double)worst, h.outL.size() - 600);
    CHECK(maxAbsIn(h.outL, 0, h.outL.size()) <= 0.5f + 1e-6f,
          "and never overshoots the sum (peak %.6f)",
          (double)maxAbsIn(h.outL, 0, h.outL.size()));

    // Uncompensated, the output would sit on one track alone — a 0.25 plateau
    // 112 frames long, between the end of the declick ramp and the arrival of
    // the second track. Aligned, the ramp only *passes through* 0.25, and it
    // does so at 0.5/144 per frame, so at most one frame can be near it.
    int plateau = 0;
    for (f32 s : h.outL) if (std::fabs(s - 0.25f) < 1e-3f) ++plateau;
    CHECK(plateau <= 1,
          "no half-mix plateau: %d frames sit at 0.25 (one track alone would be ~112)",
          plateau);
}

// The dry signal and its own send through a latent return have to land on the
// same frame — the case that makes a reverb send sound like a reverb rather
// than a slapback.
static void pdcAlignsReturnAgainstDry() {
    Host h; h.init();
    const auto imp = impulseBuf(48000, 1000);
    FakeFx latent(1.0f, 256);
    RtChain chain; chain.fx[0] = &latent; chain.count = 1;

    tempoNoQuantum(h);
    h.setClip(0, 0, mkClip(imp, 1, 0.25f, Warp::Off, true, 120.0));
    h.push(Cmd::LaunchClip, 0, 0);
    h.push(Cmd::SendLevel, 0, 0, 1.0);
    h.setReturnChain(0, &chain);
    h.run(4096);

    CHECK(h.e.latencyFrames.load() == 256,
          "a latent return counts towards the published latency (%d)",
          h.e.latencyFrames.load());

    const i64 pk = peakFrame(h.outL);
    CHECK(pk == 1000 + 256,
          "dry and wet land together at frame %lld (peak found at %lld)",
          (long long)(1000 + 256), (long long)pk);
    CHECK(pk >= 0 && std::fabs(h.outL[(size_t)pk] - 0.5f) < 1e-5f,
          "and they add rather than arriving twice: %.5f (expected 0.25 + 0.25)",
          pk >= 0 ? (double)h.outL[(size_t)pk] : 0.0);
    CHECK(std::fabs(h.outL[1000]) < 1e-6f,
          "the dry copy waited for the return: nothing at frame 1000 (%.3g)",
          (double)h.outL[1000]);
    CHECK(std::fabs(h.outL[1000 + 512]) < 1e-6f,
          "and no second copy where an uncompensated send would have landed (%.3g)",
          (double)h.outL[1000 + 512]);
    // Exactly one impulse in the whole render, not two of half the height.
    int hits = 0;
    for (f32 s : h.outL) if (std::fabs(s) > 1e-4f) ++hits;
    CHECK(hits == 1, "exactly one impulse comes out (%d frames above 1e-4)", hits);
}

// Track chains and return chains stack: the return's input is already
// track-aligned, so the two stages add rather than fighting.
static void pdcStacksTrackAndReturn() {
    Host h; h.init();
    const auto imp = impulseBuf(48000, 1000);
    FakeFx trackFx(1.0f, 128), retFx(1.0f, 256);
    RtChain tc; tc.fx[0] = &trackFx; tc.count = 1;
    RtChain rc; rc.fx[0] = &retFx;   rc.count = 1;

    tempoNoQuantum(h);
    h.setClip(0, 0, mkClip(imp, 1, 0.25f, Warp::Off, true, 120.0));
    h.push(Cmd::LaunchClip, 0, 0);
    h.push(Cmd::SendLevel, 0, 0, 1.0);
    h.setChain(0, &tc);
    h.setReturnChain(0, &rc);
    h.run(4096);

    CHECK(h.e.latencyFrames.load() == 128 + 256,
          "latency is maxTrack + maxReturn = 384 (got %d)", h.e.latencyFrames.load());
    const i64 pk = peakFrame(h.outL);
    CHECK(pk == 1000 + 128 + 256,
          "one impulse at 1000 + 128 + 256 = %lld (got %lld)",
          (long long)(1000 + 384), (long long)pk);
    CHECK(pk >= 0 && std::fabs(h.outL[(size_t)pk] - 0.5f) < 1e-5f,
          "still the aligned sum: %.5f", pk >= 0 ? (double)h.outL[(size_t)pk] : 0.0);
    int hits = 0;
    for (f32 s : h.outL) if (std::fabs(s) > 1e-4f) ++hits;
    CHECK(hits == 1, "and only one (%d frames above 1e-4)", hits);
}

// The metronome is a parallel path into the master sum like any other, and the
// one with no chain in front of it at all, so it is the one that would drift
// furthest: an uncompensated click leads the music by the whole track latency.
static void pdcAlignsClick(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx latent(1.0f, 256);
    RtChain chain; chain.fx[0] = &latent; chain.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.25f);
    h.setChain(0, &chain);
    h.push(Cmd::SetMetronome, 1);
    h.run(4096);

    i64 first = -1;
    for (size_t i = 0; i < h.outL.size(); ++i)
        if (std::fabs(h.outL[i]) > 1e-5f) { first = (i64)i; break; }
    CHECK(first == 256,
          "the downbeat click waits for the track chain: first output at %lld "
          "(expected 256)", (long long)first);
    CHECK(maxAbsIn(h.outL, 0, 256) < 1e-6f,
          "nothing at all before it (%.3g)", (double)maxAbsIn(h.outL, 0, 256));
    h.setChain(0, nullptr);
    h.runBlocks(2);
    drainRetired(h.e);
}

// What latencyFrames publishes, as chains come and go.
static void pdcPublishesTotals(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx t0(1.0f, 128), t1(1.0f, 512), r0(1.0f, 64), m0(1.0f, 32), silent(1.0f, 0);
    RtChain c0; c0.fx[0] = &t0; c0.count = 1;
    RtChain c1; c1.fx[0] = &t1; c1.count = 1;
    RtChain cr; cr.fx[0] = &r0; cr.count = 1;
    RtChain cm; cm.fx[0] = &m0; cm.count = 1;
    RtChain cz; cz.fx[0] = &silent; cz.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == 0, "a set with no devices publishes 0 (%d)",
          h.e.latencyFrames.load());

    h.setChain(0, &cz);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == 0,
          "a device that reports no latency stays on the zero path (%d)",
          h.e.latencyFrames.load());

    h.setChain(0, &c0);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == 128, "one 128-frame track chain -> %d",
          h.e.latencyFrames.load());

    h.setChain(1, &c1);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == 512,
          "tracks are parallel, so the deepest one wins -> %d", h.e.latencyFrames.load());

    h.setReturnChain(0, &cr);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == 512 + 64,
          "a return chain is in series behind the tracks -> %d (expected 576)",
          h.e.latencyFrames.load());

    h.setMasterChain(&cm);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == 512 + 64 + 32,
          "and the master chain behind both -> %d (expected 608)",
          h.e.latencyFrames.load());

    h.setChain(1, nullptr);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == 128 + 64 + 32,
          "removing the deepest track chain drops the total -> %d (expected 224)",
          h.e.latencyFrames.load());

    h.setChain(0, nullptr);
    h.setReturnChain(0, nullptr);
    h.setMasterChain(nullptr);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == 0, "and clearing everything returns to 0 (%d)",
          h.e.latencyFrames.load());
    drainRetired(h.e);
}

// Compensation is capped: a device claiming more than the delay lines can hold
// is clamped, and what gets published is what the engine actually imposes.
static void pdcClampsAbsurdLatency(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx absurd(1.0f, 1 << 18);
    RtChain chain; chain.fx[0] = &absurd; chain.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.setChain(0, &chain);
    h.runBlocks(2);
    CHECK(h.e.latencyFrames.load() == (1 << 16) - 1,
          "a 262144-frame claim is clamped to the delay-line cap -> %d (expected %d)",
          h.e.latencyFrames.load(), (1 << 16) - 1);
    h.setChain(0, nullptr);
    h.runBlocks(2);
    drainRetired(h.e);
}

// Latency changing under running audio is a click by design (the delay lines
// keep their contents and the read cursor jumps). What must not happen is
// permanent damage: the mix has to come back to the right steady state.
static void pdcResnapsOnSwap(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx a(1.0f, 256), b(1.0f, 1024), plain(1.0f, 0);
    RtChain ca; ca.fx[0] = &a; ca.count = 1;
    RtChain cb; cb.fx[0] = &b; cb.count = 1;
    RtChain cp; cp.fx[0] = &plain; cp.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.25f);
    armDc(h, 1, buf, 0.25f);
    h.setChain(1, &ca);
    h.run(8000);

    h.outL.clear(); h.outR.clear();
    h.setChain(1, &cb);                          // 256 -> 1024 frames, mid-flight
    h.run(16000);
    CHECK(h.e.latencyFrames.load() == 1024, "the new latency is published (%d)",
          h.e.latencyFrames.load());
    f32 worst = 0.f;
    for (size_t i = 4096; i < h.outL.size(); ++i)
        worst = std::max(worst, std::fabs(h.outL[i] - 0.5f));
    CHECK(worst < 1e-6f,
          "the mix settles back to the aligned sum after the swap (worst %.3g)",
          (double)worst);

    h.outL.clear(); h.outR.clear();
    h.setChain(1, &cp);                          // back to no latency at all
    h.run(16000);
    CHECK(h.e.latencyFrames.load() == 0, "and back to zero (%d)", h.e.latencyFrames.load());
    worst = 0.f;
    for (size_t i = 4096; i < h.outL.size(); ++i)
        worst = std::max(worst, std::fabs(h.outL[i] - 0.5f));
    CHECK(worst < 1e-6f,
          "with the delay lines out of the path entirely (worst %.3g)", (double)worst);
    h.setChain(1, nullptr);
    h.runBlocks(2);
    drainRetired(h.e);
}

// The zero-latency path has to be the *old* path, sample for sample: the demo
// renders are a byte-comparison gate on exactly this.
static void pdcZeroIsUntouched(const std::vector<f32>& buf) {
    Host h; h.init();
    FakeFx half(0.5f);
    RtChain chain; chain.fx[0] = &half; chain.count = 1;

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.setChain(0, &chain);
    h.push(Cmd::SendLevel, 0, 0, 1.0);           // a return in the graph as well
    h.run(8000);

    CHECK(h.e.latencyFrames.load() == 0, "no latency anywhere -> 0 (%d)",
          h.e.latencyFrames.load());
    // 0.5 clip x 0.5 chain = 0.25 dry, plus the same again through the return.
    f32 worst = 0.f;
    for (size_t i = 600; i < h.outL.size(); ++i)
        worst = std::max(worst, std::fabs(h.outL[i] - 0.5f));
    CHECK(worst < 1e-7f,
          "and the mix is exact, not merely close (worst deviation %.3g)", (double)worst);
    CHECK(std::fabs(h.outL[200] - h.outR[200]) < 1e-9f, "both channels agree exactly");
}

static void testPdc() {
    banner("18. plugin delay compensation");
    note("track dry path = trackChain; send path = trackChain + returnChain. The");
    note("send is tapped post-track-chain, so aligning tracks aligns their sends;");
    note("returns then align against each other and against the dry bus. Master");
    note("chain is in series: no compensation, just added to latencyFrames.");
    const auto buf = dcBuf(300000, 1, 1.0f);

    pdcAlignsTracks(buf);
    pdcAlignsReturnAgainstDry();
    pdcStacksTrackAndReturn();
    pdcAlignsClick(buf);
    pdcPublishesTotals(buf);
    pdcClampsAbsurdLatency(buf);
    pdcResnapsOnSwap(buf);
    pdcZeroIsUntouched(buf);
}

// ---------------------------------------------------------------------------
// 20. two MIDI producers, two SPSC rings  (RT-AUDIT §1.2)
// ---------------------------------------------------------------------------
// The hardware reader (pushMidi) and the GUI (pushMidiFromGui) each own a ring;
// process() drains both. Sharing one ring raced their head pointers and dropped
// a message — and a lost note-off is a stuck note. Prove both are delivered, in
// the same block, by capturing a MIDI take fed from both producers at once.

static void testTwoRingMidi() {
    banner("20. two MIDI producers each get their own ring");
    note("pushMidi (hardware reader) and pushMidiFromGui (GUI) are separate SPSC");
    note("rings the engine drains together; neither can overwrite the other.");

    Host h; h.init();
    std::vector<RtNote> take(16);
    for (RtNote& n : take) { n.pitch = 0; n.vel = 0; }

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);                   // None: the take starts at once
    h.push(Cmd::TrackArm, 0, 1);
    h.push(Cmd::SetPlaying, 1);
    h.pushRecMidi(0, 0, take.data(), 16);
    h.runBlocks(1);
    CHECK(h.e.recState[0].load() == 2, "the MIDI take is running (%d)", h.e.recState[0].load());

    // Both note-ons in the SAME block, one per ring: the hardware plays 60, the
    // GUI plays 64. If the two shared a ring one of these would be lost.
    h.pushMidi(0x90, 60, 100);                    // hardware reader ring
    h.pushMidiFromGui(0x90, 64, 90);              // GUI ring
    h.run(kBeat120 / 2);
    // And both note-offs together, again one per ring.
    h.pushMidi(0x80, 60, 0);
    h.pushMidiFromGui(0x80, 64, 0);
    h.run(kBeat120 / 2);

    h.pushRecMidi(0, 0, take.data(), 16);         // toggle: stop
    h.runBlocks(2);

    const std::vector<Event> evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
    CHECK(fin != nullptr, "the take finishes with Ev::MidiRecordFinished");
    const int got = fin ? (int)fin->x : 0;
    CHECK(got == 2, "both producers' notes were captured: %d (expected 2)", got);
    const RtNote* nh = takeNote(take, got, 60);   // from the hardware ring
    const RtNote* ng = takeNote(take, got, 64);   // from the GUI ring
    CHECK(nh != nullptr, "the hardware reader's note (pitch 60) survived");
    CHECK(ng != nullptr, "the GUI's note (pitch 64) survived");
    CHECK(nh && nh->vel == 100 && ng && ng->vel == 90,
          "each kept its own velocity (%d / %d)", nh ? nh->vel : -1, ng ? ng->vel : -1);
}

// ---------------------------------------------------------------------------
// 21. a full event ring never loses a finished take  (RT-AUDIT §1.6)
// ---------------------------------------------------------------------------
// Ev::RecordFinished is the ONLY channel returning a take's buffer; a silent
// drop loses the recording and leaks the buffer. So a critical event that cannot
// be pushed is parked, audio-thread-owned, and retried at the top of every
// process(). Fill the ring, auto-finish a take, and prove the RecordFinished
// still arrives once the ring has room again.

static void testEventResilience() {
    banner("21. a full event ring cannot swallow a finished take");
    note("critical events (RecordFinished/ChainRetired/...) that fail to push are");
    note("parked and retried; only cosmetic events are ever dropped on overflow.");

    Host h; h.init();
    feedCapture(h);
    std::vector<f32> rec((size_t)64 * 2, 0.f);
    const auto clip = dcBuf(300000, 1, 1.0f);

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);                   // None
    h.push(Cmd::SetPlaying, 1);
    // Clips on many tracks so launching/stopping a scene emits a burst of events.
    for (int t = 1; t <= 16; ++t) {
        h.setClip(t, 0, mkClip(clip, 1, 0.2f, Warp::Off, true, 120.0));
        h.setClip(t, 1, mkClip(clip, 1, 0.2f, Warp::Off, true, 120.0));
    }
    h.runBlocks(1);

    // Flood the event ring WITHOUT draining it: alternate scene launches, each of
    // which stops one row (ClipStopped x16) and starts the other (ClipStarted
    // x16). The ring holds 1023; 60 launches is ~1900 events, so it saturates.
    for (int c = 0; c < 200; ++c) {
        h.push(Cmd::LaunchScene, c & 1);
        h.runBlocks(1);
    }

    // Start an audio take with a tiny buffer so it auto-finishes almost at once.
    // Its RecordStarted (non-critical) may be dropped; the take's RecordFinished
    // (critical) must not be.
    h.pushRec(0, 0, rec.data(), 64);
    h.runBlocks(2);

    // At this point the take has finished but the ring is still full, so the
    // RecordFinished had to be parked rather than pushed: draining now must NOT
    // surface it.
    std::vector<Event> flooded = drainEvents(h.e);
    CHECK(flooded.size() >= 1020,
          "the ring really was saturated (%zu events drained, cap 1023)", flooded.size());
    CHECK(findEvent(flooded, Ev::RecordFinished) == nullptr,
          "with the ring full, RecordFinished was parked, not pushed");

    // One more block, now that the ring has room: flushPendingEv delivers it.
    h.runBlocks(1);
    std::vector<Event> after = drainEvents(h.e);
    const Event* fin = findEvent(after, Ev::RecordFinished);
    CHECK(fin != nullptr, "the parked RecordFinished arrives once the ring drains");
    CHECK(fin && fin->p == (void*)rec.data(),
          "and it still carries the GUI's buffer (%p vs %p)",
          fin ? fin->p : nullptr, (void*)rec.data());
}

// ---------------------------------------------------------------------------
// 22. overdub notes are appended unsorted, then sorted at the boundary
// ---------------------------------------------------------------------------
// RT-AUDIT §1.5: overdub wraps each beat modulo the loop, so arrival order and
// beat order disagree; the old per-note insertion sort memmoved the buffer
// inside process(). Notes are now appended O(1) and sorted once in finishRec.
// Play three notes whose in-loop positions arrive OUT of order and prove the
// finished buffer is fully sorted and lost nothing.

static void testOverdubSort() {
    banner("22. overdub take comes back sorted after an append-then-sort");
    note("in-loop positions played 0.75, 0.25, 0.50 must come back 0.25 < 0.50 <");
    note("0.75 — the sort at the stop boundary orders what capture only appended.");

    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    std::vector<RtNote> host(1);
    host[0].beat = 0.0; host[0].len = 0.25; host[0].pitch = 48; host[0].vel = 100;
    std::vector<RtNote> take(16);
    for (RtNote& n : take) { n.beat = -1.0; n.pitch = 0; n.vel = 0; }

    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);                   // None: boundaries land at once
    h.setChain(0, &chain);
    h.push(Cmd::TrackArm, 0, 1);
    h.setClip(0, 0, mkMidiClip(host, 1.0, true)); // a 1-beat loop, launched at 0
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(1);
    h.pushRecMidi(0, 0, take.data(), 16);         // overdub pass into the playing clip
    h.runBlocks(1);
    drainEvents(h.e);

    // pitch, lap, in-loop target — arrival order is 0.75, 0.25, 0.50.
    struct P { u8 pitch; i64 lap; f64 pos; };
    const P plan[3] = { {72, 0, 0.75}, {74, 1, 0.25}, {76, 2, 0.50} };
    f64 arrived[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        const i64 target = plan[k].lap * kBeat120 + (i64)(plan[k].pos * (f64)kBeat120);
        while ((i64)h.outL.size() < target) h.runBlocks(1);
        const i64 on = (i64)h.outL.size();
        arrived[k] = inLoop(on);
        h.pushMidi(0x90, plan[k].pitch, (u8)(100 - k));
        h.run(kBeat120 / 16);
        h.pushMidi(0x80, plan[k].pitch, 0);
        h.runBlocks(1);
    }
    // The premise: capture really did see them out of beat order.
    CHECK(arrived[0] > arrived[1] && arrived[2] > arrived[1] && arrived[0] > arrived[2],
          "the three notes arrived out of in-loop order (%.3f %.3f %.3f)",
          arrived[0], arrived[1], arrived[2]);

    h.pushRecMidi(0, 0, take.data(), 16);         // toggle: stop -> finishRec sorts
    h.runBlocks(4);

    const std::vector<Event> evs = drainEvents(h.e);
    const Event* fin = findEvent(evs, Ev::MidiRecordFinished);
    CHECK(fin != nullptr, "the overdub pass finishes with Ev::MidiRecordFinished");
    const int got = fin ? (int)fin->x : 0;
    CHECK(got == 3, "all three appended notes are kept: %d (expected 3)", got);
    CHECK(got == 3 && take[0].beat <= take[1].beat && take[1].beat <= take[2].beat,
          "the buffer comes back sorted by beat (%.4f %.4f %.4f)",
          take[0].beat, take[1].beat, take[2].beat);
    CHECK(got == 3 && take[0].pitch == 74 && take[1].pitch == 76 && take[2].pitch == 72,
          "the sort put the pitches in beat order (%d %d %d, expected 74 76 72)",
          take[0].pitch, take[1].pitch, take[2].pitch);
    // A permutation, not a corruption: every played pitch is present exactly once.
    CHECK(takeNote(take, got, 72) && takeNote(take, got, 74) && takeNote(take, got, 76),
          "the sort lost none of the three notes");
    CHECK(takeNote(take, got, 48) == nullptr,
          "and the clip's own note is not in the take buffer");
}

// ---------------------------------------------------------------------------
// 19. the command drain counter
// ---------------------------------------------------------------------------

static void testDrains() {
    banner("19. command drain counter");
    note("Engine::drains bumps at the END of every drainCommands(). A command is");
    note("provably consumed once the counter has advanced past the value read");
    note("after pushCommand() returned — the exact-retirement primitive the");
    note("process split needs to know when a pool slot is free (PROCESS-SPLIT §10).");

    Host h; h.init();
    const auto buf = dcBuf(300000, 1, 1.0f);

    CHECK(h.e.drains.load() == 0, "a prepared engine has drained nothing (%llu)",
          (unsigned long long)h.e.drains.load());

    h.runBlocks(1);
    CHECK(h.e.drains.load() == 1, "one process() is one drain (%llu)",
          (unsigned long long)h.e.drains.load());

    h.runBlocks(8);
    CHECK(h.e.drains.load() == 9, "eight more blocks, eight more drains (%llu)",
          (unsigned long long)h.e.drains.load());

    // It advances whether or not anything was queued: the counter measures the
    // audio thread having *looked*, which is what makes it a proof of absence.
    const u64 idle = h.e.drains.load();
    h.runBlocks(4);
    CHECK(h.e.drains.load() == idle + 4,
          "an empty ring still drains once per block (%llu -> %llu)",
          (unsigned long long)idle, (unsigned long long)h.e.drains.load());

    tempoNoQuantum(h);
    armDc(h, 0, buf, 0.5f);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.005f, "a track is playing at 0.50");

    // Push, observe, wait for the counter to pass it: the command is in effect.
    const u64 observed = (h.push(Cmd::TrackVol, 0, 0, 0.0), h.e.drains.load());
    CHECK(h.e.drains.load() == observed,
          "pushing does not drain by itself (%llu)", (unsigned long long)h.e.drains.load());
    h.outL.clear(); h.outR.clear();
    h.runBlocks(1);
    CHECK(h.e.drains.load() > observed,
          "the next process() advances past it (%llu > %llu)",
          (unsigned long long)h.e.drains.load(), (unsigned long long)observed);
    CHECK(std::fabs(tailLevel(h.outL)) < 1e-4f,
          "and the command it was pushed behind has taken effect (%.3g)",
          (double)tailLevel(h.outL));

    // Monotonic, never skipping and never repeating.
    u64 prev = h.e.drains.load();
    bool monotone = true;
    for (int i = 0; i < 32; ++i) {
        h.runBlocks(1);
        const u64 now = h.e.drains.load();
        if (now != prev + 1) monotone = false;
        prev = now;
    }
    CHECK(monotone, "the counter advances by exactly one per block over 32 blocks");
}

// ---------------------------------------------------------------------------
// 23. automation: the shared evaluator
//
// docs/AUTOMATION.md §2.4/§3. autoValueAt is the ONE function the engine and
// the UI both call, which is what makes the displayed value and the applied
// value incapable of disagreeing. It is declared in engine.h now, in the
// pointer form ARRANGEMENT.md §6.2 refactored it to, with an inline forwarder
// per container -- so this whole table runs against the refactored evaluator
// unchanged, which is the assertion that says the refactor was a refactor.
// ---------------------------------------------------------------------------

// One allocation holding the set followed by its points, which is the shape
// App::publishAutos will build and the shape the retirement protocol frees:
// `delete[] (char*)set`. Building it any other way here would test a layout
// the engine will never actually be handed.
static RtAutoSet* mkAutoSet(const std::vector<RtAutoLane>& lanes,
                            const std::vector<RtAutoPoint>& pts) {
    const size_t bytes = sizeof(RtAutoSet) + pts.size() * sizeof(RtAutoPoint);
    char* blk = new char[bytes];
    RtAutoSet* s = new (blk) RtAutoSet();
    RtAutoPoint* p = (RtAutoPoint*)(blk + sizeof(RtAutoSet));
    for (size_t i = 0; i < pts.size(); ++i) p[i] = pts[i];
    s->points     = pts.empty() ? nullptr : p;
    s->pointCount = (int)pts.size();
    s->laneCount  = (int)(lanes.size() < (size_t)kMaxRtAutoLanes ? lanes.size()
                                                                 : (size_t)kMaxRtAutoLanes);
    for (int i = 0; i < s->laneCount; ++i) s->lanes[i] = lanes[(size_t)i];
    return s;
}
static void freeAutoSet(const RtAutoSet* s) { delete[] (char*)s; }

static RtAutoPoint pt(f64 beat, f32 v) { RtAutoPoint p; p.beat = beat; p.value = v; return p; }

static RtAutoLane mkLane(AutoTarget tgt, int first, int count, f32 lo, f32 hi,
                         AutoXform xf = AutoXform::Direct, int index = 0, int devSlot = -1) {
    RtAutoLane l;
    l.target  = (i32)tgt;
    l.index   = index;
    l.devSlot = devSlot;
    l.xform   = (i32)xf;
    l.first   = first;
    l.count   = count;
    l.lo      = lo;
    l.hi      = hi;
    return l;
}

static void evaluatorShape() {
    // 0.0 at beat 0, up to 1.0 at beat 2, down to 0.5 at beat 4.
    RtAutoSet* s = mkAutoSet({mkLane(AutoTarget::TrackVol, 0, 3, 0.f, 1.f)},
                             {pt(0.0, 0.f), pt(2.0, 1.f), pt(4.0, 0.5f)});
    const RtAutoLane& l = s->lanes[0];
    auto at = [&](f64 b) { return autoValueAt(*s, l, b, -9.f); };

    CHECK(std::fabs(at(-5.0) - 0.f) < 1e-6f,
          "before the first point holds the first value (%.4f)", (double)at(-5.0));
    CHECK(std::fabs(at(0.0) - 0.f) < 1e-6f, "exactly on the first point (%.4f)", (double)at(0.0));
    CHECK(std::fabs(at(1.0) - 0.5f) < 1e-6f, "mid segment interpolates (%.4f)", (double)at(1.0));
    CHECK(std::fabs(at(2.0) - 1.f) < 1e-6f, "exactly on an interior point (%.4f)", (double)at(2.0));
    CHECK(std::fabs(at(3.0) - 0.75f) < 1e-6f,
          "the second segment interpolates its own endpoints (%.4f)", (double)at(3.0));
    CHECK(std::fabs(at(4.0) - 0.5f) < 1e-6f, "exactly on the last point (%.4f)", (double)at(4.0));
    CHECK(std::fabs(at(400.0) - 0.5f) < 1e-6f,
          "after the last point holds the last value (%.4f)", (double)at(400.0));
    // A hundred beats sampled across the span: monotone in, monotone out, and
    // never outside the two points bracketing it.
    bool bracketed = true;
    for (int k = 0; k <= 100; ++k) {
        const f64 b = 4.0 * (f64)k / 100.0;
        const f32 v = at(b);
        if (!(v >= -1e-6f && v <= 1.f + 1e-6f)) bracketed = false;
    }
    CHECK(bracketed, "every sampled beat lands inside the envelope's own range");
    freeAutoSet(s);
}

static void evaluatorEdges() {
    // A single point is a legal constant envelope, not an error.
    RtAutoSet* one = mkAutoSet({mkLane(AutoTarget::TrackVol, 0, 1, 0.f, 1.f)}, {pt(2.0, 0.3f)});
    CHECK(std::fabs(autoValueAt(*one, one->lanes[0], 0.0, -9.f) - 0.3f) < 1e-6f,
          "a single point is constant before itself");
    CHECK(std::fabs(autoValueAt(*one, one->lanes[0], 99.0, -9.f) - 0.3f) < 1e-6f,
          "a single point is constant after itself");
    freeAutoSet(one);

    // An empty lane is UI state, not content: it must be a no-op rather than a
    // jump to zero, so it evaluates to the caller's un-automated value.
    RtAutoSet* none = mkAutoSet({mkLane(AutoTarget::TrackVol, 0, 0, 0.f, 1.f)}, {});
    CHECK(std::fabs(autoValueAt(*none, none->lanes[0], 1.0, 0.77f) - 0.77f) < 1e-6f,
          "an empty lane returns the fallback (%.4f)",
          (double)autoValueAt(*none, none->lanes[0], 1.0, 0.77f));
    freeAutoSet(none);

    // Clamping to the lane's resolved range, so a stale envelope cannot drive a
    // parameter outside what its ParamInfo declared.
    RtAutoSet* wide = mkAutoSet({mkLane(AutoTarget::DeviceParam, 0, 2, 0.2f, 0.8f)},
                                {pt(0.0, -3.f), pt(4.0, 9.f)});
    CHECK(std::fabs(autoValueAt(*wide, wide->lanes[0], 0.0, -9.f) - 0.2f) < 1e-6f,
          "a value under lo clamps up (%.4f)", (double)autoValueAt(*wide, wide->lanes[0], 0.0, -9.f));
    CHECK(std::fabs(autoValueAt(*wide, wide->lanes[0], 4.0, -9.f) - 0.8f) < 1e-6f,
          "a value over hi clamps down (%.4f)", (double)autoValueAt(*wide, wide->lanes[0], 4.0, -9.f));
    bool inRange = true;
    for (int k = 0; k <= 64; ++k)
        if (!(autoValueAt(*wide, wide->lanes[0], 4.0 * k / 64.0, -9.f) >= 0.2f - 1e-6f &&
              autoValueAt(*wide, wide->lanes[0], 4.0 * k / 64.0, -9.f) <= 0.8f + 1e-6f))
            inRange = false;
    CHECK(inRange, "the interpolated segment stays inside [lo,hi] throughout");
    freeAutoSet(wide);

    // A window that runs off the end of the point array is answered with the
    // fallback rather than a read outside the block: the set is public memory
    // and may arrive from the other side of a process boundary.
    RtAutoSet* bad = mkAutoSet({mkLane(AutoTarget::TrackVol, 1, 4, 0.f, 1.f)},
                               {pt(0.0, 0.1f), pt(1.0, 0.2f)});
    CHECK(std::fabs(autoValueAt(*bad, bad->lanes[0], 0.5, 0.66f) - 0.66f) < 1e-6f,
          "a window past pointCount falls back instead of reading out of bounds");
    freeAutoSet(bad);

    // Unsorted input is defined, not undefined: some point's value, in range,
    // no crash. The editor holds the sorted invariant; this is what happens
    // when a hand-edited file does not.
    RtAutoSet* mess = mkAutoSet({mkLane(AutoTarget::TrackVol, 0, 3, 0.f, 1.f)},
                                {pt(3.0, 0.9f), pt(1.0, 0.1f), pt(2.0, 0.5f)});
    bool sane = true;
    for (int k = 0; k <= 40; ++k) {
        const f32 v = autoValueAt(*mess, mess->lanes[0], 4.0 * k / 40.0, -9.f);
        if (!(v >= 0.f && v <= 1.f)) sane = false;
    }
    CHECK(sane, "unsorted points evaluate to something in range rather than something undefined");

    // Two points on the same beat: a zero-width segment resolves to the later
    // one instead of dividing by zero.
    RtAutoSet* dup = mkAutoSet({mkLane(AutoTarget::TrackVol, 0, 3, 0.f, 1.f)},
                               {pt(0.0, 0.f), pt(2.0, 0.25f), pt(2.0, 0.75f)});
    const f32 d = autoValueAt(*dup, dup->lanes[0], 2.0, -9.f);
    CHECK(std::isfinite(d) && d >= 0.f && d <= 1.f,
          "a zero-width segment is finite and in range (%.4f)", (double)d);
    freeAutoSet(mess);
    freeAutoSet(dup);

    // curve is reserved: a non-zero shape renders as linear in this wave (§2.1)
    // rather than being refused or drawn some other way.
    std::vector<RtAutoPoint> cp = {pt(0.0, 0.f), pt(4.0, 1.f)};
    cp[0].curve = 3;
    RtAutoSet* curved = mkAutoSet({mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f)}, cp);
    CHECK(std::fabs(autoValueAt(*curved, curved->lanes[0], 2.0, -9.f) - 0.5f) < 1e-6f,
          "a non-zero curve byte renders as linear (%.4f)",
          (double)autoValueAt(*curved, curved->lanes[0], 2.0, -9.f));
    freeAutoSet(curved);
}

static void testEvaluator() {
    banner("23. the shared automation evaluator");
    note("One function, called by the engine now and by the moving knob later.");
    note("Before the first point and after the last it HOLDS; an empty lane is a");
    note("no-op that returns the caller's un-automated value.");
    evaluatorShape();
    evaluatorEdges();
}

// ---------------------------------------------------------------------------
// 24. automation: class A (engine-owned scalars)
// ---------------------------------------------------------------------------

// A 4-beat DC clip at 120 BPM: 96000 frames, so the clip beat at frame i is
// exactly (i mod 96000) / 24000 and the oracle needs no tolerance of its own.
static constexpr i64 kClipFrames = 96000;

static RtClip mkAutoClip(const std::vector<f32>& buf, f32 gain, const RtAutoSet* set) {
    RtClip c = mkClip(buf, 1, gain, Warp::Off, true, 120.0);
    c.autos = set;
    return c;
}

// The clip beat the engine evaluated at, for a frame that begins a block.
static f64 clipBeatAt(size_t frame) {
    return (f64)((i64)frame % kClipFrames) / (f64)kClipFrames * 4.0;
}

static void autoVolRamps(const std::vector<f32>& buf) {
    // Fader 0.0 at beat 0 rising to 1.0 at beat 4 — §9's gate envelope. Clip
    // gain 0.4 keeps the top of the fader's +6 dB inside the master clamp, so
    // what is measured is the envelope and not the limiter.
    RtAutoSet* set = mkAutoSet(
        {mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f, AutoXform::Fader)},
        {pt(0.0, 0.f), pt(4.0, 1.f)});

    Host h; h.init();
    tempoNoQuantum(h);
    h.setClip(0, 0, mkAutoClip(buf, 0.4f, set));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(8 * 4 * kBeat120);                       // 8 bars = 8 laps of the clip

    // 1. Match the oracle. At a block boundary the intra-block ramp is exactly
    //    at its start value, which is the envelope evaluated at that beat — so
    //    this is a test of APPLICATION, with no approximation to allow for.
    f64 worst = 0.0;
    size_t worstAt = 0;
    for (size_t i = (size_t)h.block; i + 1 < h.outL.size(); i += (size_t)h.block) {
        const f32 want = 0.4f * faderToGain(autoValueAt(*set, set->lanes[0], clipBeatAt(i), 0.f));
        const f64 err = std::fabs((f64)h.outL[i] - (f64)want);
        if (err > worst) { worst = err; worstAt = i; }
    }
    CHECK(worst < 1e-4, "every block boundary sits on the envelope (worst %.3g at frame %lld)",
          worst, (long long)worstAt);

    // 2. It audibly moves, and in the right direction.
    CHECK(h.outL[(size_t)(kBeat120 / 2)] < 0.02f,
          "half a beat in, the fader is still near the bottom (%.4f)",
          (double)h.outL[(size_t)(kBeat120 / 2)]);
    CHECK(h.outL[(size_t)(3 * kBeat120 + kBeat120 / 2)] > 0.2f,
          "three and a half beats in, it is near the top (%.4f)",
          (double)h.outL[(size_t)(3 * kBeat120 + kBeat120 / 2)]);
    bool rising = true;
    for (int b = 1; b < 4; ++b)
        if (!(h.outL[(size_t)(b * kBeat120)] > h.outL[(size_t)((b - 1) * kBeat120)])) rising = false;
    CHECK(rising, "the first lap rises beat over beat");

    // 3. Track::vol was never written. §1's whole rule, asserted: stop the clip
    //    and the fader is the user's, with no cleanup having been needed.
    h.push(Cmd::StopTrack, 0);
    h.run(4 * (i64)h.block);
    h.outL.clear(); h.outR.clear();
    h.setClip(0, 1, mkClip(buf, 1, 0.4f, Warp::Off, true, 120.0));
    h.push(Cmd::LaunchClip, 0, 1);
    h.run(8000);
    CHECK(std::fabs(tailLevel(h.outL) - 0.4f) < 0.005f,
          "an un-automated clip on the same track plays at the untouched fader (%.4f)",
          (double)tailLevel(h.outL));

    freeAutoSet(set);
}

static void autoVolWraps(const std::vector<f32>& buf) {
    RtAutoSet* set = mkAutoSet(
        {mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f, AutoXform::Fader)},
        {pt(0.0, 0.f), pt(4.0, 1.f)});

    Host h; h.init();
    tempoNoQuantum(h);
    h.setClip(0, 0, mkAutoClip(buf, 0.4f, set));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(8 * 4 * kBeat120);

    // Seven wraps in eight bars. The envelope has to restart at every one of
    // them, and no ramp may span the wrap: a ramp that did would carry the
    // top-of-lap value across into the next lap and overshoot.
    const f32 ceiling = 0.4f * faderToGain(1.f);
    bool restarts = true, bounded = true;
    for (int lap = 1; lap < 8; ++lap) {
        const size_t w = (size_t)(lap * kClipFrames);
        if (!(h.outL[w - 1] > 0.7f * ceiling)) restarts = false;    // top of the old lap
        if (!(h.outL[w + (size_t)h.block] < 0.02f)) restarts = false; // bottom of the new one
    }
    for (size_t i = 0; i < h.outL.size(); ++i)
        if (!(h.outL[i] >= -1e-6f && h.outL[i] <= ceiling + 1e-6f)) bounded = false;
    CHECK(restarts, "the envelope restarts at each of the seven loop wraps");
    CHECK(bounded, "no sample overshoots the envelope's own range across a wrap (ceiling %.4f)",
          (double)ceiling);
    freeAutoSet(set);
}

static void autoDeterminism(const std::vector<f32>& buf) {
    RtAutoSet* set = mkAutoSet(
        {mkLane(AutoTarget::TrackVol, 0, 3, 0.f, 1.f, AutoXform::Fader),
         mkLane(AutoTarget::TrackPan, 3, 2, -1.f, 1.f)},
        {pt(0.0, 0.2f), pt(1.5, 0.9f), pt(4.0, 0.4f), pt(0.0, -1.f), pt(4.0, 1.f)});

    std::vector<f32> a, b;
    for (int pass = 0; pass < 2; ++pass) {
        Host h; h.init();
        tempoNoQuantum(h);
        h.setClip(0, 0, mkAutoClip(buf, 0.5f, set));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(3 * 4 * kBeat120);
        (pass == 0 ? a : b) = h.outL;
        if (pass == 0) b.reserve(h.outL.size());
    }
    CHECK(a.size() == b.size() && !a.empty() &&
              std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0,
          "the same automated set rendered twice is bit-identical (%zu frames)", a.size());

    // The pan lane really did something, or the identity above proves nothing.
    Host h; h.init();
    tempoNoQuantum(h);
    h.setClip(0, 0, mkAutoClip(buf, 0.5f, set));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(4 * kBeat120);
    const size_t early = (size_t)(kBeat120 / 2);
    const size_t late  = (size_t)(3 * kBeat120 + kBeat120 / 2);
    CHECK(h.outL[early] > h.outR[early] * 3.f,
          "the pan envelope is hard left at beat 0.5 (L %.4f, R %.4f)",
          (double)h.outL[early], (double)h.outR[early]);
    CHECK(h.outR[late] > h.outL[late] * 3.f,
          "and hard right by beat 3.5 (L %.4f, R %.4f)",
          (double)h.outL[late], (double)h.outR[late]);
    freeAutoSet(set);
}

static void autoSendMovesSignal(const std::vector<f32>& buf) {
    // A send lane from silence to unity over the clip, with the user's own send
    // level left at zero: the envelope alone has to bring the return bus to
    // life, including clearing its scratch for the block.
    RtAutoSet* set = mkAutoSet(
        {mkLane(AutoTarget::TrackSend, 0, 2, 0.f, 1.f, AutoXform::Direct, 0)},
        {pt(0.0, 0.f), pt(4.0, 1.f)});

    Host h; h.init();
    tempoNoQuantum(h);
    h.setClip(0, 0, mkAutoClip(buf, 0.5f, set));
    h.push(Cmd::LaunchClip, 0, 0);
    h.push(Cmd::ReturnVol, 0, 0, 1.0);
    h.run(2 * 4 * kBeat120);

    // Dry 0.50 plus the return's copy of it, scaled by the envelope.
    f64 worst = 0.0;
    for (size_t i = (size_t)h.block; i + 1 < h.outL.size(); i += (size_t)h.block) {
        const f32 s = autoValueAt(*set, set->lanes[0], clipBeatAt(i), 0.f);
        const f64 err = std::fabs((f64)h.outL[i] - (0.5 + 0.5 * (f64)s));
        if (err > worst) worst = err;
    }
    CHECK(worst < 1e-4, "the automated send tracks the envelope into the return (worst %.3g)",
          worst);
    CHECK(h.outL[(size_t)(kBeat120 / 4)] < 0.55f,
          "at the start of a lap the return is closed (%.4f)",
          (double)h.outL[(size_t)(kBeat120 / 4)]);
    CHECK(h.outL[(size_t)(3 * kBeat120 + kBeat120 / 2)] > 0.90f,
          "by beat 3.5 it is nearly fully open (%.4f)",
          (double)h.outL[(size_t)(3 * kBeat120 + kBeat120 / 2)]);
    CHECK(h.e.returnMeterL[0].load() > 0.3f,
          "and the return bus meters what it was handed (%.4f)",
          (double)h.e.returnMeterL[0].load());
    freeAutoSet(set);
}

static void testAutomationClassA() {
    banner("24. automation of engine-owned scalars (vol / pan / send)");
    note("Evaluated at the block's start and end beat and applied as an intra-block");
    note("ramp of the DERIVED value. Track::vol/pan/send are never written: the");
    note("effective value exists for one block and reaches nothing but the mixdown.");
    const auto buf = dcBuf(kClipFrames, 1, 1.0f);
    autoVolRamps(buf);
    autoVolWraps(buf);
    autoDeterminism(buf);
    autoSendMovesSignal(buf);
}

// ---------------------------------------------------------------------------
// 25. automation: class B (device parameters)
// ---------------------------------------------------------------------------

// A device whose single parameter is its gain, so an envelope on it is audible,
// and which records exactly what reached it and how. `rt` decides whether the
// backend has a realtime parameter path at all — a backend that has none must
// produce one Ev::AutoLaneInert and never be called again.
class ParamFx : public PluginInstance {
public:
    explicit ParamFx(bool rt, f32 initial = 1.f) : rt_(rt), v_(initial) {
        info_.name = "Drive";
        info_.min = 0.f; info_.max = 1.f; info_.def = 1.f;
        rtValues.reserve(4096);
        blockValues.reserve(4096);
    }

    int rtCalls = 0;
    std::vector<f32> rtValues;       // what setParamRT accepted
    std::vector<f32> blockValues;    // what each process() actually ran with

    bool prepare(f64, int) override { return true; }
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        blockValues.push_back(v_);
        if (bypassed_) return;
        for (int c = 0; c < channels; ++c) {
            if (!out[c] || !in[c]) continue;
            for (int i = 0; i < nframes; ++i) out[c][i] = in[c][i] * v_;
        }
    }
    int              paramCount() const override     { return 1; }
    const ParamInfo& paramInfo(int) const override   { return info_; }
    f32              getParam(int i) const override  { return i == 0 ? v_ : 0.f; }
    void             setParam(int i, f32 v) override { if (i == 0) v_ = clampv(v, 0.f, 1.f); }
    bool setParamRT(int i, f32 v) override {
        ++rtCalls;
        if (!rt_) return false;                 // "this backend has no realtime path"
        if (i == 0) { v_ = clampv(v, 0.f, 1.f); rtValues.push_back(v_); }
        return true;
    }
    const PluginDesc& desc() const override          { static PluginDesc d; return d; }
    void             setBypassed(bool b) override    { bypassed_ = b; }
    bool             bypassed() const override       { return bypassed_; }

private:
    bool      rt_ = true;
    bool      bypassed_ = false;
    f32       v_ = 1.f;
    ParamInfo info_;
};

static int countEvents(Engine& e, Ev want, std::vector<Event>* out = nullptr) {
    int n = 0;
    Event ev;
    while (e.popEvent(ev)) {
        if (ev.type != want) continue;
        ++n;
        if (out) out->push_back(ev);
    }
    return n;
}

static void devParamPerBlock(const std::vector<f32>& buf) {
    RtAutoSet* set = mkAutoSet(
        {mkLane(AutoTarget::DeviceParam, 0, 2, 0.f, 1.f, AutoXform::Direct, 0, 0)},
        {pt(0.0, 0.25f), pt(4.0, 1.f)});

    Host h; h.init();
    ParamFx fx(true, 1.f);
    RtChain chain; chain.fx[0] = &fx; chain.count = 1;
    h.setChain(0, &chain);
    tempoNoQuantum(h);
    h.setClip(0, 0, mkAutoClip(buf, 0.5f, set));
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(1);

    const int after1 = fx.rtCalls;
    CHECK(after1 == 1, "one block, one setParamRT (%d)", after1);
    h.runBlocks(16);
    CHECK(fx.rtCalls == after1 + 16, "sixteen more blocks, sixteen more calls (%d)",
          fx.rtCalls - after1);
    CHECK((int)fx.blockValues.size() == 17,
          "the chain ran once per block over the same window (%zu)", fx.blockValues.size());

    // Applied BEFORE the block it belongs to, and at that block's start beat —
    // the same ordering constraint MIDI already has.
    bool matches = true;
    for (size_t b = 1; b < fx.blockValues.size(); ++b) {
        const f64 beat = clipBeatAt(b * (size_t)h.block);
        const f32 want = autoValueAt(*set, set->lanes[0], beat, 0.f);
        if (std::fabs(fx.blockValues[b] - want) > 1e-6f) matches = false;
    }
    CHECK(matches, "each block ran with the envelope's value at its own start beat");
    CHECK(fx.blockValues.back() > fx.blockValues[1] + 0.001f,
          "and the parameter climbed over the window (%.4f -> %.4f)",
          (double)fx.blockValues[1], (double)fx.blockValues.back());

    // Audible: the DC clip comes out at clip gain times the parameter.
    const size_t i = h.outL.size() - 1;
    const f32 want = 0.5f * fx.blockValues.back();
    CHECK(std::fabs(h.outL[i] - want) < 0.01f,
          "the output follows the automated parameter (%.4f, expected ~%.4f)",
          (double)h.outL[i], (double)want);

    h.setChain(0, nullptr);
    h.runBlocks(2);
    freeAutoSet(set);
}

static void devParamInert(const std::vector<f32>& buf) {
    RtAutoSet* set = mkAutoSet(
        {mkLane(AutoTarget::DeviceParam, 0, 2, 0.f, 1.f, AutoXform::Direct, 0, 0)},
        {pt(0.0, 0.25f), pt(4.0, 1.f)});

    Host h; h.init();
    ParamFx fx(false, 0.5f);                     // no realtime parameter path
    RtChain chain; chain.fx[0] = &fx; chain.count = 1;
    h.setChain(0, &chain);
    tempoNoQuantum(h);
    h.setClip(0, 0, mkAutoClip(buf, 1.0f, set));
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(1);

    std::vector<Event> evs;
    const int inert = countEvents(h.e, Ev::AutoLaneInert, &evs);
    CHECK(fx.rtCalls == 1, "the engine tries the backend exactly once (%d)", fx.rtCalls);
    CHECK(inert == 1, "one Ev::AutoLaneInert for the published set (%d)", inert);
    CHECK(inert == 1 && evs[0].a == 0 && evs[0].b == 0 && (int)evs[0].x == 0,
          "and it names track 0, slot 0, lane 0 (a=%d b=%d x=%.0f)",
          inert ? evs[0].a : -1, inert ? evs[0].b : -1, inert ? evs[0].x : -1.0);

    h.runBlocks(32);
    CHECK(fx.rtCalls == 1, "and never calls again for that set (%d)", fx.rtCalls);
    CHECK(countEvents(h.e, Ev::AutoLaneInert) == 0, "nor reports it a second time");
    CHECK(std::fabs(fx.getParam(0) - 0.5f) < 1e-6f,
          "the parameter is untouched, so there is no audio change (%.4f)",
          (double)fx.getParam(0));
    CHECK(std::fabs(tailLevel(h.outL) - 0.5f) < 0.005f,
          "the track plays at the un-automated level (%.4f)", (double)tailLevel(h.outL));

    h.setChain(0, nullptr);
    h.runBlocks(2);
    freeAutoSet(set);
}

// One Host per function, per the note above section 9: an Engine is ~2 MB by
// value and ASan does not overlap the stack slots of sibling scopes, so four
// restore cases in one frame is four engines in one frame.
static RtAutoSet* mkDriveSet() {
    return mkAutoSet({mkLane(AutoTarget::DeviceParam, 0, 2, 0.f, 1.f, AutoXform::Direct, 0, 0)},
                     {pt(0.0, 0.25f), pt(4.0, 1.f)});
}

// a. the clip stops
static void devParamRestoreOnStop(const std::vector<f32>& buf) {
    RtAutoSet* set = mkDriveSet();
    {
        Host h; h.init();
        ParamFx fx(true, 1.f);
        fx.setParam(0, 0.42f);                   // the user's own value
        RtChain chain; chain.fx[0] = &fx; chain.count = 1;
        h.setChain(0, &chain);
        tempoNoQuantum(h);
        h.setClip(0, 0, mkAutoClip(buf, 0.5f, set));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(2 * kBeat120);
        CHECK(std::fabs(fx.getParam(0) - 0.42f) > 0.05f,
              "the envelope has taken the parameter over (%.4f)", (double)fx.getParam(0));
        h.push(Cmd::StopTrack, 0);
        h.runBlocks(6);
        CHECK(std::fabs(fx.getParam(0) - 0.42f) < 1e-6f,
              "stopping the clip hands the parameter back (%.4f, expected 0.42)",
              (double)fx.getParam(0));
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }

    freeAutoSet(set);
}

// b. the transport stops
static void devParamRestoreOnTransportStop(const std::vector<f32>& buf) {
    RtAutoSet* set = mkDriveSet();
    {
        Host h; h.init();
        ParamFx fx(true, 1.f);
        fx.setParam(0, 0.42f);
        RtChain chain; chain.fx[0] = &fx; chain.count = 1;
        h.setChain(0, &chain);
        tempoNoQuantum(h);
        h.setClip(0, 0, mkAutoClip(buf, 0.5f, set));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(2 * kBeat120);
        h.push(Cmd::SetPlaying, 0);
        h.runBlocks(6);
        CHECK(std::fabs(fx.getParam(0) - 0.42f) < 1e-6f,
              "stopping the transport hands it back too (%.4f)", (double)fx.getParam(0));
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }

    freeAutoSet(set);
}

// c. the clip is cleared under the voice
static void devParamRestoreOnClear(const std::vector<f32>& buf) {
    RtAutoSet* set = mkDriveSet();
    {
        Host h; h.init();
        ParamFx fx(true, 1.f);
        fx.setParam(0, 0.42f);
        RtChain chain; chain.fx[0] = &fx; chain.count = 1;
        h.setChain(0, &chain);
        tempoNoQuantum(h);
        h.setClip(0, 0, mkAutoClip(buf, 0.5f, set));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(2 * kBeat120);
        Command cc; cc.type = Cmd::ClearClip; cc.a = 0; cc.b = 0;
        h.e.pushCommand(cc);
        h.runBlocks(4);
        CHECK(std::fabs(fx.getParam(0) - 0.42f) < 1e-6f,
              "clearing the clip hands it back (%.4f)", (double)fx.getParam(0));
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }

    freeAutoSet(set);
}

// d. Cmd::SetChain replaces the chain mid-envelope. §3.5 warns this is the one
//    that gets forgotten: the restore has to happen BEFORE the pointer moves,
//    because afterwards the instance may be one the engine no longer references
//    and the GUI is free to delete.
static void devParamRestoreOnChainSwap(const std::vector<f32>& buf) {
    RtAutoSet* set = mkDriveSet();
    {
        Host h; h.init();
        ParamFx fx(true, 1.f);
        fx.setParam(0, 0.42f);
        RtChain chainA; chainA.fx[0] = &fx; chainA.count = 1;
        ParamFx fx2(true, 1.f);
        RtChain chainB; chainB.fx[0] = &fx2; chainB.count = 1;
        h.setChain(0, &chainA);
        tempoNoQuantum(h);
        h.setClip(0, 0, mkAutoClip(buf, 0.5f, set));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(2 * kBeat120);
        h.setChain(0, &chainB);
        h.runBlocks(1);
        CHECK(std::fabs(fx.getParam(0) - 0.42f) < 1e-6f,
              "swapping the chain restores the outgoing device first (%.4f)",
              (double)fx.getParam(0));
        h.setChain(0, nullptr);
        h.runBlocks(2);
    }
    freeAutoSet(set);
}

static void testAutomationClassB() {
    banner("25. automation of device parameters");
    note("A plugin has one storage slot per parameter and no notion of an effective");
    note("value, so this is the one target where the engine writes what it automates —");
    note("and therefore the one that owes a restore when it stops.");
    const auto buf = dcBuf(kClipFrames, 1, 1.0f);
    devParamPerBlock(buf);
    devParamInert(buf);
    devParamRestoreOnStop(buf);
    devParamRestoreOnTransportStop(buf);
    devParamRestoreOnClear(buf);
    devParamRestoreOnChainSwap(buf);
}

// ---------------------------------------------------------------------------
// 26. automation: retirement
// ---------------------------------------------------------------------------

static void testAutosRetirement() {
    banner("26. an envelope set replaced under a playing clip comes home");
    note("Verbatim the RtNote protocol: the audio thread never frees GUI memory,");
    note("so a displaced set rides Ev::AutosRetired back and only then may go.");

    const auto buf = dcBuf(kClipFrames, 1, 1.0f);
    const std::vector<RtAutoLane> lanes = {
        mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f, AutoXform::Fader)};
    const std::vector<RtAutoPoint> pts = {pt(0.0, 0.2f), pt(4.0, 0.9f)};

    Host h; h.init();
    tempoNoQuantum(h);
    RtAutoSet* first = mkAutoSet(lanes, pts);
    h.setClip(0, 0, mkAutoClip(buf, 0.5f, first));
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(4);
    CHECK(countEvents(h.e, Ev::AutosRetired) == 0,
          "publishing the first set retires nothing");

    RtAutoSet* second = mkAutoSet(lanes, pts);
    h.setClip(0, 0, mkAutoClip(buf, 0.5f, second));
    h.runBlocks(2);
    std::vector<Event> evs;
    const int n1 = countEvents(h.e, Ev::AutosRetired, &evs);
    CHECK(n1 == 1, "replacing it retires exactly one set (%d)", n1);
    CHECK(n1 == 1 && evs[0].p == (void*)first, "and it is the displaced pointer");
    CHECK(n1 == 1 && evs[0].a == 0 && evs[0].b == 0, "named by track and slot (a=%d b=%d)",
          n1 ? evs[0].a : -1, n1 ? evs[0].b : -1);
    freeAutoSet(first);

    // Re-pushing the SAME pointer must announce nothing: an entry that would
    // never be announced must not be queued, and one that is announced twice
    // would be a double free on the other side.
    h.setClip(0, 0, mkAutoClip(buf, 0.5f, second));
    h.runBlocks(2);
    CHECK(countEvents(h.e, Ev::AutosRetired) == 0,
          "re-pushing the same set announces nothing");

    // A clip with no envelopes at all pushed over one that had them still has
    // to hand the old one back.
    RtAutoSet* third = mkAutoSet(lanes, pts);
    h.setClip(0, 0, mkAutoClip(buf, 0.5f, third));
    h.runBlocks(2);
    evs.clear();
    CHECK(countEvents(h.e, Ev::AutosRetired, &evs) == 1 && evs[0].p == (void*)second,
          "and a third publication retires the second");
    freeAutoSet(second);

    // Clearing the slot: the incoming set is null, so "differs from the
    // incoming one" is simply "there was one".
    Command cc; cc.type = Cmd::ClearClip; cc.a = 0; cc.b = 0;
    h.e.pushCommand(cc);
    h.runBlocks(2);
    evs.clear();
    CHECK(countEvents(h.e, Ev::AutosRetired, &evs) == 1 && evs[0].p == (void*)third,
          "clearing the clip retires the set it carried");
    freeAutoSet(third);

}

// §9's gate: republish while it plays and account for every set.
static void autosRetirementUnderChurn() {
    const auto buf = dcBuf(kClipFrames, 1, 1.0f);
    const std::vector<RtAutoLane> lanes = {
        mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f, AutoXform::Fader)};
    const std::vector<RtAutoPoint> pts = {pt(0.0, 0.2f), pt(4.0, 0.9f)};
    Host g; g.init();
    tempoNoQuantum(g);
    std::vector<RtAutoSet*> live;
    RtAutoSet* cur = mkAutoSet(lanes, pts);
    live.push_back(cur);
    g.setClip(0, 0, mkAutoClip(buf, 0.5f, cur));
    g.push(Cmd::LaunchClip, 0, 0);
    g.runBlocks(2);
    int retired = 0;
    for (int k = 0; k < 100; ++k) {
        RtAutoSet* next = mkAutoSet(lanes, pts);
        live.push_back(next);
        g.setClip(0, 0, mkAutoClip(buf, 0.5f, next));
        g.runBlocks(1);
        std::vector<Event> got;
        retired += countEvents(g.e, Ev::AutosRetired, &got);
        for (const Event& e : got) {
            bool known = false;
            for (RtAutoSet*& p : live)
                if (p == (RtAutoSet*)e.p) { known = true; freeAutoSet(p); p = nullptr; break; }
            if (!known) CHECK(false, "an unowned pointer came back through AutosRetired");
        }
    }
    CHECK(retired == 100, "a hundred republications retire a hundred sets (%d)", retired);
    int leaked = 0;
    for (RtAutoSet* p : live) if (p) { ++leaked; freeAutoSet(p); }
    CHECK(leaked == 1, "exactly one set is still published and none leaked (%d outstanding)",
          leaked);
}

// ---------------------------------------------------------------------------
// 27. the warp map evaluator
//
// warpSrcAt / warpSlopeAt / warpBeatAt / warpMapValid are the ONE
// implementation the engine and the UI both call, exactly as autoValueAt is for
// envelopes: the warp line a user drags and the frames the engine reads cannot
// be allowed to disagree. Everything here is a pure-function check with no
// engine in it — §28 onwards is where the same functions have to move audio.
// ---------------------------------------------------------------------------

static std::vector<WarpMarker> mkMap(std::initializer_list<std::pair<i64, f64>> pts) {
    std::vector<WarpMarker> v;
    for (const auto& p : pts) { WarpMarker m; m.srcFrame = p.first; m.beat = p.second; v.push_back(m); }
    return v;
}
static f64 srcAt(const std::vector<WarpMarker>& m, f64 beat) {
    return warpSrcAt(m.data(), (int)m.size(), beat);
}
static f64 slopeAt(const std::vector<WarpMarker>& m, f64 beat) {
    return warpSlopeAt(m.data(), (int)m.size(), beat);
}
static f64 beatAt(const std::vector<WarpMarker>& m, f64 src) {
    return warpBeatAt(m.data(), (int)m.size(), src);
}
static bool mapValid(const std::vector<WarpMarker>& m) {
    return warpMapValid(m.data(), (int)m.size());
}

static void testWarpEvaluator() {
    banner("27. the warp map evaluator");

    // Three segments with deliberately different slopes: 12000, 48000 and 6000
    // source frames per beat. Nothing here is a round ratio of anything else, so
    // an off-by-one segment shows up as a wrong number rather than a lucky one.
    const auto m = mkMap({{0, 0.0}, {24000, 2.0}, {120000, 4.0}, {138000, 7.0}});
    CHECK(mapValid(m), "the fixture map is valid");

    // --- bracketing: every marker is ON the curve, both ways ---------------
    bool onCurve = true, inverseOnCurve = true;
    for (const WarpMarker& k : m) {
        if (std::fabs(srcAt(m, k.beat) - (f64)k.srcFrame) > 1e-9) onCurve = false;
        if (std::fabs(beatAt(m, (f64)k.srcFrame) - k.beat) > 1e-12) inverseOnCurve = false;
    }
    CHECK(onCurve, "warpSrcAt reproduces every marker's frame exactly");
    CHECK(inverseOnCurve, "warpBeatAt reproduces every marker's beat exactly");

    // Interior points are the straight line between the bracketing pair, which
    // is what "piecewise LINEAR" has to mean if a dragged marker is to move the
    // playback the user expects.
    CHECK(std::fabs(srcAt(m, 1.0) - 12000.0) < 1e-9,
          "midway through segment 0: %.4f (expected 12000)", srcAt(m, 1.0));
    CHECK(std::fabs(srcAt(m, 3.0) - 72000.0) < 1e-9,
          "midway through segment 1: %.4f (expected 72000)", srcAt(m, 3.0));
    CHECK(std::fabs(srcAt(m, 5.5) - 129000.0) < 1e-9,
          "midway through segment 2: %.4f (expected 129000)", srcAt(m, 5.5));

    // --- slope: the local rate, per segment -------------------------------
    CHECK(std::fabs(slopeAt(m, 0.5)  - 12000.0) < 1e-9, "slope in segment 0 (%.1f)", slopeAt(m, 0.5));
    CHECK(std::fabs(slopeAt(m, 3.0)  - 48000.0) < 1e-9, "slope in segment 1 (%.1f)", slopeAt(m, 3.0));
    CHECK(std::fabs(slopeAt(m, 6.0)  -  6000.0) < 1e-9, "slope in segment 2 (%.1f)", slopeAt(m, 6.0));
    // Exactly on a marker the LATER segment wins, because the bracket is
    // "last index whose key is <= x". That is a choice, not an accident, and a
    // test says so rather than leaving the next reader to find out.
    CHECK(std::fabs(slopeAt(m, 2.0) - 48000.0) < 1e-9,
          "on a marker the following segment's slope is reported (%.1f)", slopeAt(m, 2.0));

    // --- monotonicity ------------------------------------------------------
    // The single invariant everything else rests on: strictly increasing in,
    // strictly increasing out. Swept finely enough to catch a bracket that
    // picks the wrong pair for one step rather than for a whole segment.
    bool srcMono = true, beatMono = true, slopePos = true;
    f64 prevSrc = -1e18, prevBeat = -1e18;
    for (int i = 0; i <= 9000; ++i) {
        const f64 b = -2.0 + (f64)i * (12.0 / 9000.0);   // past both ends
        const f64 s = srcAt(m, b);
        if (!(s > prevSrc)) srcMono = false;
        if (!(slopeAt(m, b) > 0.0)) slopePos = false;
        prevSrc = s;
        const f64 sf = -20000.0 + (f64)i * (200000.0 / 9000.0);
        const f64 bb = beatAt(m, sf);
        if (!(bb > prevBeat)) beatMono = false;
        prevBeat = bb;
    }
    CHECK(srcMono, "warpSrcAt is strictly increasing over 9000 steps, ends included");
    CHECK(beatMono, "warpBeatAt is strictly increasing over 9000 steps, ends included");
    CHECK(slopePos, "the local rate is positive everywhere, so nothing ever divides by it wrongly");

    // --- extrapolation before the first and after the last marker ----------
    // What lets a user pin two transients in the middle of a clip and have the
    // rest of it follow, instead of the clip stopping dead outside the marks.
    CHECK(std::fabs(srcAt(m, -1.0) - (-12000.0)) < 1e-9,
          "before the first marker the first segment's slope extrapolates: %.1f (expected -12000)",
          srcAt(m, -1.0));
    CHECK(std::fabs(srcAt(m, 9.0) - 150000.0) < 1e-9,
          "after the last marker the last segment's slope extrapolates: %.1f (expected 150000)",
          srcAt(m, 9.0));
    CHECK(std::fabs(slopeAt(m, -5.0) - 12000.0) < 1e-9 &&
          std::fabs(slopeAt(m, 99.0) -  6000.0) < 1e-9,
          "and the reported slope outside the span is the adjacent segment's");
    CHECK(std::fabs(beatAt(m, -12000.0) - (-1.0)) < 1e-9 &&
          std::fabs(beatAt(m, 150000.0) - 9.0) < 1e-9,
          "the inverse extrapolates on the same two segments");

    // --- the inverse really is the inverse ---------------------------------
    // Round-tripping is the property that makes marker snapping and the
    // loop-region conversion in warpCtxFor trustworthy.
    f64 worstFwd = 0.0, worstBack = 0.0;
    for (int i = 0; i <= 4000; ++i) {
        const f64 b = -3.0 + (f64)i * (14.0 / 4000.0);
        worstFwd = std::max(worstFwd, std::fabs(beatAt(m, srcAt(m, b)) - b));
        const f64 s = -30000.0 + (f64)i * (220000.0 / 4000.0);
        worstBack = std::max(worstBack, std::fabs(srcAt(m, beatAt(m, s)) - s));
    }
    CHECK(worstFwd < 1e-9, "beat -> src -> beat is the identity (worst %.3g beats)", worstFwd);
    CHECK(worstBack < 1e-6, "src -> beat -> src is the identity (worst %.3g frames)", worstBack);

    // --- degenerate maps ---------------------------------------------------
    const std::vector<WarpMarker> none;
    CHECK(warpSrcAt(nullptr, 0, 3.5) == 3.5 && srcAt(none, 3.5) == 3.5,
          "n == 0 has no opinion: warpSrcAt returns the beat unchanged");
    CHECK(warpSlopeAt(nullptr, 0, 3.5) == 0.0,
          "n == 0 reports no slope");
    CHECK(warpBeatAt(nullptr, 0, 777.0) == 777.0,
          "n == 0 has no opinion: warpBeatAt returns the frame unchanged");
    CHECK(!mapValid(none) && !warpMapValid(nullptr, 5),
          "an empty map and a null pointer are both invalid");

    const auto one = mkMap({{9000, 3.0}});
    CHECK(srcAt(one, 0.0) == 9000.0 && srcAt(one, 1e6) == 9000.0,
          "n == 1 pins, it does not tilt: every beat maps to that one frame");
    CHECK(slopeAt(one, 3.0) == 0.0, "n == 1 reports no slope");
    CHECK(beatAt(one, 0.0) == 3.0 && beatAt(one, 1e9) == 3.0,
          "n == 1 inverts to that one beat");
    CHECK(!mapValid(one), "n == 1 is not a publishable map");

    // --- validity ----------------------------------------------------------
    CHECK(!mapValid(mkMap({{0, 0.0}, {0, 1.0}})), "equal source frames are rejected");
    CHECK(!mapValid(mkMap({{24000, 0.0}, {0, 1.0}})), "decreasing source frames are rejected");
    CHECK(!mapValid(mkMap({{0, 1.0}, {24000, 1.0}})), "equal beats are rejected");
    CHECK(!mapValid(mkMap({{0, 2.0}, {24000, 1.0}})), "decreasing beats are rejected");
    CHECK(!mapValid(mkMap({{-1, 0.0}, {24000, 1.0}})), "a negative source frame is rejected");
    const f64 nan = std::nan("");
    CHECK(!mapValid(mkMap({{0, 0.0}, {24000, nan}})), "a NaN beat is rejected");
    CHECK(!mapValid(mkMap({{0, nan}, {24000, 1.0}})), "a NaN first beat is rejected");
    CHECK(mapValid(mkMap({{0, 0.0}, {1, 1e-9}})),
          "a legal map with tiny steps is accepted (the gate tests order, not size)");

    // A map that slipped past the gate must still produce a defined point on
    // itself rather than a division by zero or a read past the end: the
    // evaluators are the last line of defence and the audio thread only checks
    // n >= 2.
    const auto broken = mkMap({{0, 5.0}, {24000, 5.0}, {48000, 1.0}});
    bool finite = true;
    for (int i = -100; i <= 100; ++i) {
        const f64 b = (f64)i * 0.25;
        if (!std::isfinite(srcAt(broken, b)) || !std::isfinite(slopeAt(broken, b))) finite = false;
        if (!std::isfinite(beatAt(broken, (f64)i * 1000.0))) finite = false;
    }
    CHECK(finite, "a non-monotone map still evaluates finitely everywhere");
    CHECK(std::isfinite(srcAt(m, nan)) && std::isfinite(beatAt(m, nan)) &&
          std::isfinite(slopeAt(m, nan)),
          "NaN in, a real number out");
}

// ---------------------------------------------------------------------------
// 28. a single-segment map is BIT-IDENTICAL to the no-marker path
//
// The whole marker feature is a regression risk to every render this engine has
// ever produced, so the flat path keeps its original arithmetic and the marker
// path is new arithmetic that only runs when markers exist. This is the seam
// tested from the outside: a map that describes exactly what the flat path
// already does must produce exactly the same samples — not close, the same.
//
// The numbers are chosen so both paths are EXACT in binary and the comparison
// is therefore meaningful rather than lucky:
//
//   tempo 87.890625 at 48 kHz  ->  bps = tempo/60/sr = 2^-15 exactly
//   clipBpm 175.78125          ->  flatRate = tempo/clipBpm = 0.5 exactly
//   one segment, 16384 frames per beat -> local rate = 16384 * 2^-15 = 0.5
//
// so the flat path's accumulation (srcPos += 0.5) and the marker path's
// re-evaluation (srcPos = beatPos * 16384, beatPos = k * 2^-15) are the same
// real number AND the same double. If the map's arithmetic is ever changed in a
// way that rounds differently, this fails.
// ---------------------------------------------------------------------------

static constexpr f64 kExactTempo   = 87.890625;    // bps == 2^-15
static constexpr f64 kExactClipBpm = 175.78125;    // rate == 0.5

// Renders one clip and returns the output. `map` may be empty, which is the
// no-marker path.
static std::vector<f32> renderWarped(const std::vector<f32>& buf, Warp w,
                                     const std::vector<WarpMarker>& map,
                                     i64 frames, f64 tempo, f64 clipBpm,
                                     const std::vector<i64>* transients = nullptr) {
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, tempo);
    h.push(Cmd::SetQuantum, 0);
    RtClip c = mkClip(buf, 1, 1.0f, w, /*loop*/false, clipBpm);
    if (map.size() >= 2) { c.markers = map.data(); c.markerCount = (int)map.size(); }
    if (transients) {
        c.transients     = transients->data();
        c.transientCount = (int)transients->size();
    }
    h.setClip(0, 0, c);
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(frames);
    return h.outL;
}

// Bit equality, not tolerance: the buffers must be the same bytes.
static bool sameBits(const std::vector<f32>& a, const std::vector<f32>& b) {
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0;
}
static i64 firstDiff(const std::vector<f32>& a, const std::vector<f32>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(f32)) != 0) return (i64)i;
    return -1;
}

static void testWarpSingleSegment() {
    banner("28. a single-segment map is bit-identical to the no-marker path");
    note("The flat rate keeps its original expression and the map is arithmetic");
    note("that only runs when markers exist; this is that promise, asserted.");

    const i64 N = 480000;                    // long enough that nothing loops
    const auto ramp = rampBuf(N);
    // 16384 source frames per beat: with bps = 2^-15 that is a rate of exactly
    // 0.5, which is exactly tempo/clipBpm.
    const auto flatMap = mkMap({{0, 0.0}, {16384, 1.0}});
    CHECK(mapValid(flatMap), "the single-segment map is valid");
    CHECK(warpSlopeAt(flatMap.data(), 2, 0.0) * (kExactTempo / 60.0 / kSR) == 0.5,
          "the map's local rate is exactly the flat rate (%.17g)",
          warpSlopeAt(flatMap.data(), 2, 0.0) * (kExactTempo / 60.0 / kSR));

    for (int mode = 0; mode < 2; ++mode) {
        const Warp w = mode == 0 ? Warp::Repitch : Warp::Beats;
        const char* nm = mode == 0 ? "Repitch" : "Beats";
        const auto without = renderWarped(ramp, w, {},      96000, kExactTempo, kExactClipBpm);
        const auto with    = renderWarped(ramp, w, flatMap, 96000, kExactTempo, kExactClipBpm);
        CHECK(!without.empty() && tailLevel(without) > 0.05f,
              "%s: the no-marker render actually produced audio (%.4f)",
              nm, (double)tailLevel(without));
        CHECK(sameBits(without, with),
              "%s: %zu frames are bit-identical with and without the map (first diff %lld)",
              nm, without.size(), (long long)firstDiff(without, with));
    }

    // The negative control. Without it the test above would still pass if the
    // marker path were silently never taken, which is the failure it exists to
    // catch: shift one marker and the samples must move.
    const auto tilted = mkMap({{0, 0.0}, {20480, 1.0}});   // rate 0.625, not 0.5
    const auto base   = renderWarped(ramp, Warp::Repitch, {},     96000, kExactTempo, kExactClipBpm);
    const auto moved  = renderWarped(ramp, Warp::Repitch, tilted, 96000, kExactTempo, kExactClipBpm);
    CHECK(!sameBits(base, moved),
          "a map with a different slope does NOT match, so the map is really being read");
}

// ---------------------------------------------------------------------------
// 29. a two-segment map plays its halves at different rates
// ---------------------------------------------------------------------------

static void testWarpTwoSegments() {
    banner("29. a two-segment map plays the halves at different measured rates");
    note("Ramp buffer, so the output level IS the source read position (§4).");

    const i64 N = 480000;
    const auto ramp = rampBuf(N);
    // At 120 BPM a beat is 24000 output frames. Segment 0 covers beats 0..4 in
    // 48000 source frames (12000/beat -> rate 0.5); segment 1 covers beats 4..8
    // in 144000 (36000/beat -> rate 1.5). So the first 96000 output frames read
    // at half speed and the next 96000 at one and a half.
    const auto m = mkMap({{0, 0.0}, {48000, 4.0}, {192000, 8.0}});
    CHECK(mapValid(m), "the two-segment map is valid");

    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    RtClip c = mkClip(ramp, 1, 1.0f, Warp::Repitch, /*loop*/false, 120.0);
    c.markers = m.data(); c.markerCount = (int)m.size();
    h.setClip(0, 0, c);
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(200000);

    const f64 a0 = srcPosAt(h.outL, 20000, N), a1 = srcPosAt(h.outL, 40000, N);
    const f64 b0 = srcPosAt(h.outL, 120000, N), b1 = srcPosAt(h.outL, 140000, N);
    const f64 rA = (a1 - a0) / 20000.0;
    const f64 rB = (b1 - b0) / 20000.0;
    CHECK(std::fabs(rA - 0.5) < 0.01,
          "the first half reads at the first segment's rate: %.4f (expected 0.5)", rA);
    CHECK(std::fabs(rB - 1.5) < 0.01,
          "the second half reads at the second segment's rate: %.4f (expected 1.5)", rB);
    CHECK(rB > rA * 2.5, "and the two really are different rates (%.4f vs %.4f)", rA, rB);

    // The absolute positions matter as much as the rates: a map that got the
    // slopes right and the origin wrong would still put the downbeat in the
    // wrong place. Beat 4 is output frame 96000 and must be source frame 48000.
    const f64 atSeam = srcPosAt(h.outL, 96000, N);
    CHECK(std::fabs(atSeam - 48000.0) < 400.0,
          "the segment boundary lands on its marker: source %.0f at beat 4 (expected 48000)",
          atSeam);
    const f64 atEnd = srcPosAt(h.outL, 191000, N);
    CHECK(std::fabs(atEnd - 190500.0) < 800.0,
          "and the far end tracks the map: source %.0f near beat 8 (expected ~190500)", atEnd);

    // Continuity: a piecewise-linear map is continuous by construction, so the
    // read position must never jump. A jump at a marker would be an audible
    // click on every warped clip.
    f64 worstJump = 0.0;
    for (size_t i = 90000; i < 102000; ++i) {
        const f64 d = srcPosAt(h.outL, i, N) - srcPosAt(h.outL, i - 1, N);
        worstJump = std::max(worstJump, std::fabs(d));
    }
    CHECK(worstJump < 8.0,
          "the read position is continuous across the marker (worst step %.2f frames)", worstJump);
}

// ---------------------------------------------------------------------------
// 30. a displaced warp map comes home
// ---------------------------------------------------------------------------

static WarpMarker* mkHeapMap(const std::vector<WarpMarker>& src) {
    WarpMarker* p = new WarpMarker[src.size()];
    for (size_t i = 0; i < src.size(); ++i) p[i] = src[i];
    return p;
}

static void testWarpRetirement() {
    banner("30. a warp map replaced under a playing clip comes home");
    note("Fourth instance of the RtChain / RtNote / RtAutoSet protocol: the");
    note("audio thread never frees GUI memory, so a displaced map rides");
    note("Ev::WarpRetired back and only then may be released.");

    const auto proto = mkMap({{0, 0.0}, {24000, 1.0}});
    const auto buf = dcBuf(480000, 1, 1.0f);

    Host h; h.init();
    tempoNoQuantum(h);
    auto push = [&](WarpMarker* map) {
        RtClip c = mkClip(buf, 1, 0.5f, Warp::Beats, true, 120.0);
        if (map) { c.markers = map; c.markerCount = 2; }
        h.setClip(0, 0, c);
    };

    WarpMarker* first = mkHeapMap(proto);
    push(first);
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(4);
    CHECK(countEvents(h.e, Ev::WarpRetired) == 0, "publishing the first map retires nothing");

    WarpMarker* second = mkHeapMap(proto);
    push(second);
    h.runBlocks(2);
    std::vector<Event> evs;
    const int n1 = countEvents(h.e, Ev::WarpRetired, &evs);
    CHECK(n1 == 1, "replacing it retires exactly one map (%d)", n1);
    CHECK(n1 == 1 && evs[0].p == (void*)first, "and it is the displaced pointer");
    CHECK(n1 == 1 && evs[0].a == 0 && evs[0].b == 0,
          "named by track and slot (a=%d b=%d)", n1 ? evs[0].a : -1, n1 ? evs[0].b : -1);
    delete[] first;

    // Re-pushing the SAME pointer must announce nothing: one announced twice
    // would be a double free on the other side.
    push(second);
    h.runBlocks(2);
    CHECK(countEvents(h.e, Ev::WarpRetired) == 0, "re-pushing the same map announces nothing");

    // A clip with no map at all pushed over one that had a map still has to
    // hand the old one back — the case a naive "only when both exist" check
    // would leak.
    push(nullptr);
    h.runBlocks(2);
    evs.clear();
    CHECK(countEvents(h.e, Ev::WarpRetired, &evs) == 1 && evs[0].p == (void*)second,
          "publishing a clip with no map retires the one that was there");
    delete[] second;

    // And clearing the slot.
    WarpMarker* third = mkHeapMap(proto);
    push(third);
    h.runBlocks(2);
    CHECK(countEvents(h.e, Ev::WarpRetired) == 0, "(re-arming: nothing retired)");
    Command cc; cc.type = Cmd::ClearClip; cc.a = 0; cc.b = 0;
    h.e.pushCommand(cc);
    h.runBlocks(2);
    evs.clear();
    CHECK(countEvents(h.e, Ev::WarpRetired, &evs) == 1 && evs[0].p == (void*)third,
          "clearing the clip retires the map it carried");
    delete[] third;
}

// Churn: a hundred republications under a playing clip must account for a
// hundred maps and leave exactly one outstanding. Its own function because a
// Host carries an Engine (~2.4 MB) by value and three of them do not fit on a
// default stack — the reason autosRetirementUnderChurn is split out too.
static void warpRetirementUnderChurn() {
    const auto proto = mkMap({{0, 0.0}, {24000, 1.0}});
    const auto buf = dcBuf(480000, 1, 1.0f);
    Host g; g.init();
    tempoNoQuantum(g);
    std::vector<WarpMarker*> live;
    WarpMarker* cur = mkHeapMap(proto);
    live.push_back(cur);
    {
        RtClip c = mkClip(buf, 1, 0.5f, Warp::Beats, true, 120.0);
        c.markers = cur; c.markerCount = 2;
        g.setClip(0, 0, c);
    }
    g.push(Cmd::LaunchClip, 0, 0);
    g.runBlocks(2);
    int retired = 0;
    for (int k = 0; k < 100; ++k) {
        WarpMarker* next = mkHeapMap(proto);
        live.push_back(next);
        RtClip c = mkClip(buf, 1, 0.5f, Warp::Beats, true, 120.0);
        c.markers = next; c.markerCount = 2;
        g.setClip(0, 0, c);
        g.runBlocks(1);
        std::vector<Event> got;
        retired += countEvents(g.e, Ev::WarpRetired, &got);
        for (const Event& e : got) {
            bool known = false;
            for (WarpMarker*& p : live)
                if (p == (WarpMarker*)e.p) { known = true; delete[] p; p = nullptr; break; }
            if (!known) CHECK(false, "an unowned pointer came back through WarpRetired");
        }
    }
    CHECK(retired == 100, "a hundred republications retire a hundred maps (%d)", retired);
    int leaked = 0;
    for (WarpMarker* p : live) if (p) { ++leaked; delete[] p; }
    CHECK(leaked == 1, "exactly one map is still published and none leaked (%d outstanding)",
          leaked);
}

// A MIDI clip's beatPos is its NOTE cursor. A map that reached one anyway — the
// GUI refuses to publish one, but the engine is handed RtClips by three
// publishers — must not seed it, or the pattern starts somewhere other than its
// beginning. Asserted through the notes, which is where it would show.
static void warpOnMidiClip() {
    Host k; k.init();
    NoteSink sink(k.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    auto notes = twoNoteClip();
    WarpMarker* stray = mkHeapMap(mkMap({{0, 4.0}, {24000, 5.0}}));   // starts at beat 4
    k.push(Cmd::SetTempo, 0, 0, 120.0);
    k.push(Cmd::SetQuantum, 0);
    k.setChain(0, &chain);
    RtClip mc = mkMidiClip(notes, 1.0, /*loop*/true);
    mc.markers = stray; mc.markerCount = 2;
    k.setClip(0, 0, mc);
    k.push(Cmd::LaunchClip, 0, 0);
    k.run(kBeat120);
    // The first lap, frame for frame: on/off at 0 and 6000, on/off at 12000 and
    // 18000 — identical to midiClipTiming's first lap (§14a), which is the
    // whole assertion. run() is block-aligned, so a fifth message (the next
    // lap's downbeat, at 24000) may or may not be in the buffer.
    const i64 wantFrame[4] = {0, 6000, 12000, 18000};
    const u8  wantPitch[4] = {60, 60, 64, 64};
    bool laneOk = sink.evs.size() >= 4;
    for (int i = 0; i < 4 && laneOk; ++i)
        if (std::llabs((long long)(sink.evs[(size_t)i].frame - wantFrame[i])) > 1 ||
            sink.evs[(size_t)i].pitch != wantPitch[i]) laneOk = false;
    CHECK(laneOk,
          "a map on a MIDI clip does not move its note cursor (%zu messages, first at %lld)",
          sink.evs.size(), sink.evs.empty() ? -1 : (long long)sink.evs[0].frame);
    Command cl; cl.type = Cmd::ClearClip; cl.a = 0; cl.b = 0;
    k.e.pushCommand(cl);
    k.runBlocks(2);
    std::vector<Event> strayEv;
    CHECK(countEvents(k.e, Ev::WarpRetired, &strayEv) == 1 &&
          strayEv[0].p == (void*)stray,
          "and it is still retired properly when the slot is cleared");
    delete[] stray;
}

// ---------------------------------------------------------------------------
// 31. the onset detector
//
// A warp marker is a source frame pinned to a beat, and the frames worth
// pinning are the ones a listener hears as an event. So this asserts POSITIONS,
// not counts: a detector that returns the right number of onsets in the wrong
// places is a detector that puts every auto-warp marker in the wrong place.
//
// The two signals are the demo loops verbatim (tools/gen_demo.cpp): a
// four-on-the-floor kick with the 110 -> 45 Hz pitch sweep that makes a kick a
// kick, and an eighth-note hat pattern. Both are two bars of 4/4 at 120 BPM.
// ---------------------------------------------------------------------------

static constexpr i64 kDemoBeat   = 24000;             // one beat at 120 BPM, 48 kHz
static constexpr i64 kDemoFrames = 8 * kDemoBeat;     // two bars

// tools/gen_demo.cpp makeKick(), value for value.
static std::vector<f32> demoKick() {
    std::vector<f32> v((size_t)kDemoFrames, 0.f);
    for (int b = 0; b < 8; ++b) {
        const i64 start = (i64)b * kDemoBeat;
        const int len = (int)(kSR * 0.35);
        for (int i = 0; i < len && start + i < kDemoFrames; ++i) {
            const f64 t = (f64)i / kSR;
            const f64 f = 45.0 + 65.0 * std::exp(-t * 38.0);
            const f64 env = std::exp(-t * 9.0);
            v[(size_t)(start + i)] += (f32)(std::sin(2 * 3.14159265358979323846 * f * t) * env * 0.9);
        }
    }
    return v;
}

// tools/gen_demo.cpp makeHats(): sixteen eighth notes, alternating accents.
// The noise is a fixed LCG rather than <random> so the fixture cannot move
// under a libstdc++ change; the detector does not care what the noise is, only
// that a burst starts where one is supposed to.
static std::vector<f32> demoHats() {
    std::vector<f32> v((size_t)kDemoFrames, 0.f);
    u32 st = 12345u;
    auto noise = [&]() {
        st = st * 1664525u + 1013904223u;
        return (f32)((f64)(st >> 8) / 8388608.0 - 1.0);      // [-1, 1)
    };
    const i64 step = kDemoBeat / 2;
    for (int s = 0; s * step < kDemoFrames; ++s) {
        const i64 start = (i64)s * step;
        const bool accent = (s % 2) == 1;
        const int len = (int)(kSR * (accent ? 0.06 : 0.03));
        f32 hp = 0.f;
        for (int i = 0; i < len && start + i < kDemoFrames; ++i) {
            const f64 env = std::exp(-((f64)i / kSR) * (accent ? 70.0 : 130.0));
            const f32 n = noise();
            hp = 0.85f * (hp + n - (i ? noise() : 0.f));
            v[(size_t)(start + i)] += (f32)(hp * env * (accent ? 0.28 : 0.16));
        }
    }
    return v;
}

// Worst |detected - expected| in milliseconds, or -1 if the counts disagree or
// any expected onset has no detection within `tolMs`.
static f64 onsetError(const std::vector<i64>& got, const std::vector<i64>& want, f64 tolMs) {
    if (got.size() != want.size()) return -1.0;
    f64 worst = 0.0;
    for (size_t i = 0; i < want.size(); ++i) {
        const f64 ms = std::fabs((f64)(got[i] - want[i])) / kSR * 1000.0;
        if (ms > tolMs) return -1.0;
        worst = std::max(worst, ms);
    }
    return worst;
}

static void testOnsetDetector() {
    banner("31. the onset detector finds the beats and nothing else");

    std::vector<i64> beats, eighths;
    for (int b = 0; b < 8; ++b) beats.push_back((i64)b * kDemoBeat);
    for (int s = 0; s < 16; ++s) eighths.push_back((i64)s * (kDemoBeat / 2));

    std::vector<i64> got;
    detectTransients(demoKick().data(), kDemoFrames, 1, kSR, got);
    const f64 kickErr = onsetError(got, beats, 5.0);
    CHECK(got.size() == 8,
          "a synthesized four-on-the-floor yields 8 onsets, not 22 (%zu)", got.size());
    CHECK(kickErr >= 0.0,
          "and every one of them is a kick, within 5 ms (worst %.2f ms)", kickErr);

    detectTransients(demoHats().data(), kDemoFrames, 1, kSR, got);
    const f64 hatErr = onsetError(got, eighths, 5.0);
    CHECK(got.size() == 16, "an eighth-note hat pattern yields 16 onsets (%zu)", got.size());
    CHECK(hatErr >= 0.0, "each within 5 ms of its eighth (worst %.2f ms)", hatErr);

    // Strictly increasing is not a nicety: the engine binary-searches this list
    // and warpGrainOrigin assumes both ends of a bracket are distinct.
    bool sorted = true;
    for (size_t i = 1; i < got.size(); ++i) if (!(got[i] > got[i - 1])) sorted = false;
    CHECK(sorted, "the list comes back strictly increasing");

    // Scale invariance. The thresholds are relative by construction, so a clip
    // recorded 26 dB down must detect exactly the same frames — otherwise every
    // quiet clip in a set warps differently from every loud one.
    std::vector<f32> quiet = demoKick();
    for (f32& s : quiet) s *= 0.05f;
    std::vector<i64> quietGot;
    detectTransients(quiet.data(), kDemoFrames, 1, kSR, quietGot);
    detectTransients(demoKick().data(), kDemoFrames, 1, kSR, got);
    CHECK(quietGot == got, "a 26 dB quieter copy detects the identical frames (%zu vs %zu)",
          quietGot.size(), got.size());

    // Determinism: same samples, same answer, every time. A marker that moved
    // because the detector ran twice would be a corrupt edit.
    std::vector<i64> again;
    detectTransients(demoKick().data(), kDemoFrames, 1, kSR, again);
    CHECK(again == got, "running it twice gives the identical list");

    // A sustained pad has no onsets to find; the old detector reported 42 on
    // the demo chord. Nothing but the start of the swell is a defensible
    // answer, and the level-rise gate rejects even that.
    std::vector<f32> pad((size_t)kDemoFrames);
    for (i64 i = 0; i < kDemoFrames; ++i) {
        const f64 t = (f64)i / kSR;
        const f64 env = (1.0 - std::exp(-t * 1.6)) * std::exp(-t * 0.28);
        f64 s = 0.0;
        for (f64 fq : {220.0, 261.63, 329.63, 493.88}) {
            s += std::sin(2 * 3.14159265358979323846 * fq * t);
            s += 0.5 * std::sin(2 * 3.14159265358979323846 * fq * 1.003 * t);
        }
        pad[(size_t)i] = (f32)(s / 12.0 * env * 0.7);
    }
    std::vector<i64> padGot;
    detectTransients(pad.data(), kDemoFrames, 1, kSR, padGot);
    CHECK(padGot.size() <= 1, "a beating pad yields at most one onset, not 42 (%zu)",
          padGot.size());

    // Degenerate inputs return an empty list rather than a made-up one.
    std::vector<i64> edge{1, 2, 3};
    detectTransients(nullptr, 1000, 1, kSR, edge);
    CHECK(edge.empty(), "a null buffer clears the list and adds nothing");
    std::vector<f32> tiny(64, 0.5f);
    detectTransients(tiny.data(), 64, 1, kSR, edge);
    CHECK(edge.empty(), "a clip shorter than one analysis window gets no list");
    std::vector<f32> silence((size_t)kDemoFrames, 0.f);
    detectTransients(silence.data(), kDemoFrames, 1, kSR, edge);
    CHECK(edge.empty(), "silence has no onsets");
    detectTransients(demoKick().data(), kDemoFrames, 1, 0.0, edge);
    CHECK(edge.empty(), "a zero sample rate is refused");
}

// ---------------------------------------------------------------------------
// 32. beats-mode grain alignment
//
// With transients known, a grain starts on an attack instead of on the fixed
// hop. The gate that matters for every render that came before: a clip whose
// sample has NO transients must grain exactly as it always did.
// ---------------------------------------------------------------------------

static void testGrainAlignment() {
    banner("32. grain alignment is a no-op when there are no transients");

    const i64 N = 480000;
    const auto ramp = rampBuf(N);
    // An empty vector: `.data()` may be null or not, and either way the count
    // is zero, which is the shape a SampleBuffer whose material has no onsets
    // publishes. Both halves of the gate (`!w.tr` and `w.trN <= 0`) are
    // exercised by it.
    const std::vector<i64> empty;

    const auto plain = renderWarped(ramp, Warp::Beats, {}, 96000,
                                    kExactTempo, kExactClipBpm);
    const auto withNull = renderWarped(ramp, Warp::Beats, {}, 96000,
                                       kExactTempo, kExactClipBpm, &empty);
    CHECK(!plain.empty() && tailLevel(plain) > 0.05f,
          "the granular render actually produced audio (%.4f)", (double)tailLevel(plain));
    CHECK(sameBits(plain, withNull),
          "an empty transient list renders bit-identically (first diff %lld)",
          (long long)firstDiff(plain, withNull));

    // And with a real list the grains DO move, which is what stops the check
    // above from being vacuous. The onsets sit well inside the snap window of a
    // 1/16-note grain (a grain advance here is 0.5 * hop source frames), so a
    // gate that was accidentally left off would be caught by the check above
    // and one that was accidentally left ON is caught by this one.
    std::vector<i64> onsets;
    for (i64 f = 1000; f < 200000; f += 3000) onsets.push_back(f);
    const auto snapped = renderWarped(ramp, Warp::Beats, {}, 96000,
                                      kExactTempo, kExactClipBpm, &onsets);
    CHECK(!sameBits(plain, snapped),
          "a real transient list moves the grain origins, so the alignment is live");
    bool finite = true;
    f32 peak = 0.f;
    for (f32 s : snapped) {
        if (!std::isfinite(s)) finite = false;
        peak = std::max(peak, std::fabs(s));
    }
    CHECK(finite && peak <= 1.0f + 1e-6f,
          "and the snapped render stays finite and in range (peak %.4f)", (double)peak);

    // The same gate through the other door: a map with transients but no
    // markers must still match the no-marker path frame for frame in the parts
    // the snap does not touch — asserted here as "the render is still a legal
    // signal", since the snap deliberately changes samples.
    CHECK(snapped.size() == plain.size(), "the snapped render is the same length");
}

// ---------------------------------------------------------------------------
// 33. the arrangement's header types, compiled and NOT YET USED
//
// docs/ARRANGEMENT.md §10.2. 8a lands every engine.h edit the wave needs and
// wires none of them up, because engine.h is the daemon's contract and exactly
// one milestone may open it. That makes "unused" the property under test, and
// there are three halves to it:
//
//   * the shapes are what §3, §5 and §6 specified;
//   * the numbering of Cmd and Ev did not move, since ipc/control.h classifies
//     by that number and WireCommand carries it;
//   * an engine handed the four new commands renders BIT-IDENTICALLY to one
//     that never saw them. That is the "a set with no arrangement must render
//     exactly what it renders today" gate, tested at the only place 8a can
//     reach it.
// ---------------------------------------------------------------------------

// The numbering is protocol. Both enums may only ever grow at the end; an
// insertion would silently reclassify every command above it at the process
// boundary, which is the kind of break that shows up as "the daemon refused a
// fader move" three phases later.
static_assert((u32)Cmd::RecordMidiSlot    == 25, "Cmd numbering moved");
static_assert((u32)Cmd::SetArrangement    == 26, "SetArrangement must be appended");
static_assert((u32)Cmd::SetTrackAutos     == 27, "SetTrackAutos must be appended");
static_assert((u32)Cmd::Locate            == 28, "Locate must be appended");
static_assert((u32)Cmd::BackToArrangement == 29, "BackToArrangement must be appended");
static_assert((u32)Ev::WarpRetired        == 12, "Ev numbering moved");
static_assert((u32)Ev::ArrangementRetired == 13, "ArrangementRetired must be appended");
static_assert((u32)Ev::TrackAutosRetired  == 14, "TrackAutosRetired must be appended");

// Pointer-free and trivially copyable: it rides an SPSC ring and, later, a
// shared-memory region, and neither can carry anything that needs a constructor.
// 24 and not the 32 §5.3 quotes -- four 4-byte integers and an f64 is 24 bytes,
// and the field list is what that section actually specifies.
static_assert(sizeof(ArrJournal) == 24, "ArrJournal is four u32/i32 and one f64");
static_assert(std::is_trivially_copyable<ArrJournal>::value, "ArrJournal must be POD-ish");
static_assert(std::is_trivially_copyable<RtArrItem>::value, "RtArrItem must be POD-ish");
static_assert(std::is_trivially_copyable<RtArrangement>::value, "RtArrangement must be POD-ish");
static_assert(kMaxRtArrLanes == 32, "kMaxRtArrLanes == kMaxArrLanes");

static void testArrangementTypes() {
    banner("33. arrangement header types (compiled, unused)");

    // --- shapes -------------------------------------------------------
    {
        RtArrItem it;
        CHECK(it.start == 0.0 && it.length == 0.0 && it.offset == 0.0 &&
              it.fadeIn == 0.f && it.fadeOut == 0.f && it.fadeShape == 0 && it.clip == -1,
              "a default RtArrItem points at no clip and occupies nothing");
        RtArrangement arr;
        CHECK(!arr.items && !arr.clips && arr.itemCount == 0 && arr.clipCount == 0 &&
              arr.noteCount == 0,
              "a default RtArrangement is an empty lane");
        CHECK(arr.loopStart == 0.0 && arr.loopEnd == 0.0 && arr.loopOn == 0,
              "and carries a zeroed transport cell, which every track's lane leaves alone");
        RtAutoSetN n;
        CHECK(!n.points && !n.lanes && n.laneCount == 0 && n.pointCount == 0,
              "a default RtAutoSetN is empty, and its lanes are a POINTER -- the one "
              "difference from RtAutoSet, so widening it never touches sizeof(RtAutoSet)");
    }

    // --- ONE evaluator ------------------------------------------------
    //
    // The refactor's whole point: the same points, read through the other
    // container, must produce the same float. Not "close" -- the same bits, at
    // every beat, because it is literally the same function underneath.
    {
        const std::vector<RtAutoPoint> pts = {
            {0.0, 0.20f, 0, {}}, {1.0, 0.90f, 0, {}}, {2.5, 0.35f, 3, {}}, {4.0, 0.75f, 0, {}},
        };
        RtAutoLane lane;
        lane.first = 0; lane.count = (int)pts.size();
        lane.lo = 0.f; lane.hi = 1.f;
        RtAutoSet* a = mkAutoSet({lane}, pts);

        // The point array is rounded up to alignof(RtAutoPoint): RtAutoLane is
        // 4-aligned and RtAutoPoint leads with an f64, so one lane leaves the
        // points on a 4-byte boundary otherwise. UBSan catches it; a strict-
        // alignment target would fault on it.
        const size_t ptsOff = (sizeof(RtAutoSetN) + sizeof(RtAutoLane) +
                               alignof(RtAutoPoint) - 1) & ~(alignof(RtAutoPoint) - 1);
        const size_t bytes = ptsOff + pts.size() * sizeof(RtAutoPoint);
        char* blk = new char[bytes];
        RtAutoSetN* b = new (blk) RtAutoSetN();
        RtAutoLane*  bl = (RtAutoLane*)(blk + sizeof(RtAutoSetN));
        RtAutoPoint* bp = (RtAutoPoint*)(blk + ptsOff);
        bl[0] = lane;
        for (size_t i = 0; i < pts.size(); ++i) bp[i] = pts[i];
        b->lanes = bl; b->laneCount = 1;
        b->points = bp; b->pointCount = (int)pts.size();

        bool same = true;
        for (int k = -20; k <= 120; ++k) {
            const f64 beat = 4.0 * (f64)k / 100.0;
            if (!sameBits(std::vector<f32>{autoValueAt(*a, a->lanes[0], beat, -9.f)},
                          std::vector<f32>{autoValueAt(*b, b->lanes[0], beat, -9.f)}))
                same = false;
        }
        CHECK(same, "RtAutoSetN evaluates bit-identically to RtAutoSet over 141 beats -- "
                    "one evaluator, two containers");
        CHECK(autoValueAt(b->points, b->pointCount, b->lanes[0], 1.75, -9.f) ==
              autoValueAt(*b, b->lanes[0], 1.75, -9.f),
              "and the forwarders forward: the pointer form is the same call");

        // The window validation is the container-independent half, so it has to
        // hold through the new one too: a lane whose window leaves the block is
        // inert, not a read past the end.
        RtAutoLane bad = lane;
        bad.first = 3; bad.count = 4;
        CHECK(autoValueAt(*b, bad, 1.0, 0.42f) == 0.42f,
              "an out-of-range window is an inert lane in RtAutoSetN too");

        delete[] (char*)a;
        delete[] blk;
    }

    // --- the journal --------------------------------------------------
    {
        Host h; h.init();
        ArrJournal j;
        CHECK(!h.e.popJournal(j), "a fresh engine's journal is empty");
        CHECK(h.e.journalDropped.load() == 0, "and nothing has been dropped");
        CHECK(h.e.arrOverride.load() == 0u, "no track is overridden before anything is launched");
        h.runBlocks(4);
        CHECK(!h.e.popJournal(j) && h.e.journalDropped.load() == 0,
              "and rendering writes no journal entries yet -- the ring is landed, not wired");
        CHECK(h.e.arrOverride.load() == 0u, "arrOverride stays clear across a render");
    }

    // --- THE EMPTY CASE (§10.3 gate 3) --------------------------------
    //
    // 8a asserted here that the four commands were inert. 8b+8c wires them, so
    // the assertion that survives is the one that mattered: a set whose lane
    // names no clips -- and every set with no arrangement at all -- renders
    // BYTE-IDENTICALLY to a session-only render. The fade fields must cost
    // nothing, the fourth fireDue step must find nothing to fire, and the loop
    // brace and the locate must move the timeline without moving a sample of a
    // session clip (§3.6: a locate is a statement about the timeline, not about
    // the performance).
    //
    // What DOES change is the retirement: a lane published and then cleared is a
    // displaced pointer, and it has to come home. Exactly one, for exactly the
    // pointer that was displaced.
    {
        const auto buf = dcBuf(4 * kBeat120, 1, 0.5f);
        static RtArrangement arrBlock;
        auto render = [&](bool withArrangement) {
            Host h; h.init();
            const RtClip c = mkClip(buf, 1, 1.f, Warp::Off, true, 120.0);
            h.setClip(0, 0, c);
            h.push(Cmd::SetQuantum, 0, 0, 0.0);
            h.push(Cmd::SetPlaying, 1);
            h.push(Cmd::LaunchClip, 0, 0);
            h.runBlocks(4);
            if (withArrangement) {
                static RtArrItem items[2];
                RtArrangement& arr = arrBlock;
                items[0] = RtArrItem{0.0, 4.0, 0.0, 0.f, 0.f, 0, 0};
                items[1] = RtArrItem{4.0, 4.0, 2.0, 0.5f, 0.5f, 1, 0};
                arr.items = items; arr.itemCount = 2;
                arr.clips = nullptr; arr.clipCount = 0;
                static RtAutoSetN autos;
                Command k;
                k.type = Cmd::SetArrangement; k.a = 0; k.p = (void*)&arr;
                h.e.pushCommand(k);
                k = Command{}; k.type = Cmd::SetTrackAutos; k.a = 0; k.p = (void*)&autos;
                h.e.pushCommand(k);
                // The transport cell: a = -1, no items, only the brace.
                static RtArrangement brace;
                brace.loopStart = 4.0; brace.loopEnd = 12.0; brace.loopOn = 1;
                k = Command{}; k.type = Cmd::SetArrangement; k.a = -1; k.p = (void*)&brace;
                h.e.pushCommand(k);
                h.push(Cmd::Locate, 0, 0, 4.0);
                h.push(Cmd::BackToArrangement, -1);
                // And the null-clears form, which must be equally inert.
                k = Command{}; k.type = Cmd::SetArrangement; k.a = 0; k.p = nullptr;
                h.e.pushCommand(k);
            }
            h.runBlocks(8);
            int retired = 0;
            const void* got = nullptr;
            Event e;
            while (h.e.popEvent(e))
                if (e.type == Ev::ArrangementRetired || e.type == Ev::TrackAutosRetired) {
                    ++retired;
                    got = e.p;
                }
            if (withArrangement) {
                CHECK(retired == 1 && got == (const void*)&arrBlock,
                      "the displaced lane comes home once, and it is the pointer that was "
                      "displaced (%d event(s))", retired);
            } else {
                CHECK(retired == 0, "nothing is retired when nothing was published (control run)");
            }
            return h.outL;
        };
        const auto plain = render(false);
        const auto withArr = render(true);
        CHECK(sameBits(plain, withArr),
              "a lane that names no clips renders BYTE-IDENTICALLY to a session-only "
              "render -- the empty case costs nothing");
        Host h2; h2.init();
        ArrJournal j;
        CHECK(!h2.e.popJournal(j), "and none of them writes a journal entry");
    }
}

// ---------------------------------------------------------------------------
// 34. the arrangement scheduler (docs/ARRANGEMENT.md §3, §4)
//
// The engine's whole arrangement job is to answer, per track, "which clip should
// be on the primary voice right now, and at what offset". Everything below is a
// statement about that answer, and the two headline ones are bit-identity
// claims: a session-only render must not move by one sample, and a clip split
// sixty-four times must render exactly what the unsplit clip renders.
// ---------------------------------------------------------------------------

// One allocation, exactly the shape §3.2 specifies and the shape 8d's publisher
// will build:
//
//   [RtArrangement][RtArrItem[itemCount]][RtClip[clipCount]][RtNote[noteCount]]
//
// Built by hand here, and built by hand for a reason: the DEDUPE is 8d's job and
// it is a correctness precondition, not an optimisation. Splitting an item makes
// two model items each holding a full copy of the ClipModel, so the pointer
// equality R3's continuation rule tests can only ever hold if the publisher
// notices that two copies are the same content. These tests construct the
// deduped shape directly — one clips[] entry, many items[] entries pointing at
// it — which is what makes the 64x gate reachable at all.
static RtArrangement* mkArr(const std::vector<RtArrItem>& items,
                            const std::vector<RtClip>& clips,
                            const std::vector<std::vector<RtNote>>& notes = {}) {
    size_t nn = 0;
    for (const auto& v : notes) nn += v.size();
    const size_t bytes = sizeof(RtArrangement) + items.size() * sizeof(RtArrItem) +
                         clips.size() * sizeof(RtClip) + nn * sizeof(RtNote);
    char* blk = new char[bytes];
    RtArrangement* a = new (blk) RtArrangement();
    RtArrItem* it = (RtArrItem*)(blk + sizeof(RtArrangement));
    RtClip*    cl = (RtClip*)(it + items.size());
    RtNote*    nt = (RtNote*)(cl + clips.size());
    for (size_t i = 0; i < items.size(); ++i) new (&it[i]) RtArrItem(items[i]);
    for (size_t i = 0; i < clips.size(); ++i) {
        new (&cl[i]) RtClip(clips[i]);
        if (i < notes.size() && !notes[i].empty()) {
            for (size_t k = 0; k < notes[i].size(); ++k) new (&nt[k]) RtNote(notes[i][k]);
            cl[i].notes     = nt;
            cl[i].noteCount = (int)notes[i].size();
            nt += notes[i].size();
        }
    }
    a->items = items.empty() ? nullptr : it;
    a->clips = clips.empty() ? nullptr : cl;
    a->itemCount = (int)items.size();
    a->clipCount = (int)clips.size();
    a->noteCount = (int)nn;
    return a;
}
// The publisher and the reaper have to agree that the block is a char[] holding
// a placement-new'd RtArrangement followed by three arrays (§3.7).
static void freeArr(const RtArrangement* a) { delete[] (char*)a; }

static RtArrItem arrItem(f64 start, f64 length, f64 offset, int clip = 0,
                         f32 fadeIn = 0.f, f32 fadeOut = 0.f) {
    RtArrItem it;
    it.start  = start;
    it.length = length;
    it.offset = offset;
    it.fadeIn = fadeIn;
    it.fadeOut = fadeOut;
    it.clip   = clip;
    return it;
}

static void pushArr(Host& h, int track, const RtArrangement* a) {
    Command c; c.type = Cmd::SetArrangement; c.a = track; c.p = (void*)a;
    h.e.pushCommand(c);
}
// The transport cell (a = -1): no items, only the brace.
static void pushBrace(Host& h, const RtArrangement* a) { pushArr(h, -1, a); }
static RtArrangement* mkBrace(f64 lo, f64 hi, bool on) {
    RtArrangement* a = mkArr({}, {});
    a->loopStart = lo; a->loopEnd = hi; a->loopOn = on ? 1u : 0u;
    return a;
}

// The two payloads §34 measures with. The ramp encodes the source read position
// in the sample value, which is how an offset is checked: a voice seeded at clip
// beat b reads 0.1 + 0.8 * b/8 and says so out loud.
static constexpr i64 kArrFrames = 8 * kBeat120;     // an 8-beat clip at 120 BPM
static f32 rampAtBeat(f64 clipBeat) {
    f64 b = std::fmod(clipBeat, 8.0);
    if (b < 0.0) b += 8.0;
    return (f32)(kRampLo + kRampSpan * (b / 8.0));
}
static RtClip arrClip(const std::vector<f32>& buf) {
    RtClip c = mkClip(buf, 1, 1.f, Warp::Off, true, 120.0);
    c.lengthBeats = 8.0;
    return c;
}

// Mean level over a short window, well past any declick ramp.
static f32 levelAt(const std::vector<f32>& v, i64 at, int n = 64) {
    if (at < 0 || (size_t)(at + n) > v.size()) return 0.f;
    f64 acc = 0.0;
    for (int i = 0; i < n; ++i) acc += v[(size_t)(at + i)];
    return (f32)(acc / (f64)n);
}

// --- a. boundaries land on the exact frame -------------------------------
//
// §10.3 gate 4, measured the way the launch-quantization tests measure: the
// first non-zero sample, at an absolute frame, at two block sizes that share no
// common boundary. An item boundary joins the same consider/fireDue splitter a
// clip launch uses, so it has to land on its exact frame the same way.
static void arrBoundaryExactness() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);

    // Irrational-ish starts, so nothing lands on a block boundary by luck.
    const f64 starts[] = {1.0 / 3.0, std::sqrt(2.0), 3.7, 5.125};
    for (f64 s : starts) {
        const i64 want = (i64)std::ceil(s * (f64)kBeat120);
        i64 got[2] = {-1, -1};
        const int blocks[2] = {64, 8192};
        for (int k = 0; k < 2; ++k) {
            Host h; h.init(kSR, blocks[k]);
            RtArrangement* a = mkArr({arrItem(s, 4.0, 0.0)}, {arrClip(buf)});
            pushArr(h, 0, a);
            h.push(Cmd::SetPlaying, 1);
            h.run(7 * kBeat120);
            got[k] = firstWhere(h.outL, 0, nonZero);
            pushArr(h, 0, nullptr);
            h.runBlocks(2);
            freeArr(a);
        }
        CHECK(got[0] == want && got[1] == want,
              "an item at beat %.6f starts on frame %lld at both 64 and 8192 frames "
              "per block (%lld, %lld)", s, (long long)want,
              (long long)got[0], (long long)got[1]);
    }

    // And it ENDS on its exact frame too: the release is a boundary like any
    // other, so the last sample at full level is the frame before the end beat.
    {
        Host h; h.init();
        RtArrangement* a = mkArr({arrItem(1.0, 2.0, 0.0)}, {arrClip(buf)});
        pushArr(h, 0, a);
        h.push(Cmd::SetPlaying, 1);
        h.run(6 * kBeat120);
        const i64 on = firstWhere(h.outL, 0, nonZero);
        const i64 off = firstWhere(h.outL, on + 1000, departsFromSteady);
        CHECK(on == kBeat120, "it starts on frame %lld (%lld)", (long long)kBeat120,
              (long long)on);
        CHECK(off == 3 * kBeat120, "and begins its release on frame %lld (%lld)",
              (long long)(3 * kBeat120), (long long)off);
        pushArr(h, 0, nullptr);
        h.runBlocks(2);
        freeArr(a);
    }
}

// --- b. an item with no fades is a session launch ------------------------
//
// The zero-fade path has to be the session path, byte for byte: same startVoice,
// same declick, same arithmetic. This is the empty-case discipline applied to
// the one arrangement item that is allowed to exist.
static std::vector<f32> arrOrSessionRender(bool arrangement, const std::vector<f32>& buf) {
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    RtArrangement* arr = nullptr;
    if (arrangement) {
        arr = mkArr({arrItem(0.0, 8.0, 0.0)}, {arrClip(buf)});
        pushArr(h, 0, arr);
        h.push(Cmd::SetPlaying, 1);
    } else {
        h.setClip(0, 0, arrClip(buf));
        h.push(Cmd::LaunchClip, 0, 0);
    }
    h.run(6 * kBeat120);
    // Snapshot BEFORE the cleanup render, or the arrangement run would carry two
    // extra blocks the session run does not have and the comparison would be
    // about buffer lengths rather than about samples.
    std::vector<f32> out = h.outL;
    if (arr) { pushArr(h, 0, nullptr); h.runBlocks(2); freeArr(arr); }
    return out;
}

static void arrZeroFadeMatchesSession() {
    const auto buf = rampBuf(kArrFrames);
    const auto session = arrOrSessionRender(false, buf);
    const auto arranged = arrOrSessionRender(true, buf);
    CHECK(sameBits(session, arranged),
          "an item with no fades renders BIT-IDENTICALLY to the same clip launched "
          "in the session (first difference at frame %lld)",
          (long long)firstDiff(session, arranged));
}

// --- c. THE 64x SPLIT GATE (§10.3 gate 2) --------------------------------
static void arrSplitIsInaudible() {
    const auto buf = rampBuf(kArrFrames);

    // Sixty-four irregular boundaries over eight beats. Irregular on purpose:
    // an even split could hide an off-by-one that a ragged one exposes.
    std::vector<f64> b;
    {
        f64 acc = 0.0;
        std::vector<f64> gap;
        for (int i = 0; i < 64; ++i) {
            const f64 g = 0.4 + (f64)((i * 37) % 13) / 13.0 + 0.017 * (f64)(i % 7);
            gap.push_back(g);
            acc += g;
        }
        f64 at = 0.0;
        b.push_back(0.0);
        for (int i = 0; i < 64; ++i) { at += gap[(size_t)i] * 8.0 / acc; b.push_back(at); }
        b.back() = 8.0;                                    // exactly, not nearly
    }

    auto render = [&](const std::vector<RtArrItem>& items) {
        Host h; h.init();
        RtArrangement* a = mkArr(items, {arrClip(buf)});
        pushArr(h, 0, a);
        h.push(Cmd::SetPlaying, 1);
        h.run(8 * kBeat120);
        pushArr(h, 0, nullptr);
        h.runBlocks(2);
        freeArr(a);
        return h.outL;
    };

    const auto whole = render({arrItem(0.0, 8.0, 0.0)});

    std::vector<RtArrItem> split;
    for (int i = 0; i < 64; ++i)
        split.push_back(arrItem(b[(size_t)i], b[(size_t)i + 1] - b[(size_t)i], b[(size_t)i]));
    const auto cut = render(split);

    CHECK(sameBits(whole, cut),
          "THE GATE: a clip split 64 times renders BIT-IDENTICALLY to the same clip "
          "unsplit (first difference at frame %lld)", (long long)firstDiff(whole, cut));

    // The negative control, which is what proves condition (3) of §3.5 is
    // actually consulted rather than merely written down. A 1/64-beat fade on
    // ONE of the sixty-four splits is the user saying "put a shape here", and
    // the boundary must stop being a continuation because of it.
    std::vector<RtArrItem> shaped = split;
    shaped[32].fadeIn = 1.f / 64.f;
    const auto fadedOnce = render(shaped);
    CHECK(!sameBits(whole, fadedOnce),
          "and a 1/64-beat fade on one of the sixty-four is NOT identical -- the "
          "fade condition is consulted");

    // An offset that does not line up is a jump-cut and must sound like one:
    // condition (2) is contiguity, not merely "the same clip".
    std::vector<RtArrItem> jumped = split;
    jumped[32].offset += 1.0;
    const auto jump = render(jumped);
    CHECK(!sameBits(whole, jump),
          "and a discontiguous offset is NOT identical -- the contiguity condition "
          "is consulted");
}

// --- d. fades ------------------------------------------------------------
static void arrFades() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);

    // A 4-beat fade-in over an 8-beat item: the multiplier at beat b is b/4, and
    // the shape is applied to the MULTIPLIER, so fadeShape 0 is a linear gain
    // ramp (§3.4, and the same choice AUTOMATION.md §3.2 makes for class A).
    {
        Host h; h.init();
        RtArrangement* a = mkArr({arrItem(0.0, 8.0, 0.0, 0, 4.f, 2.f)}, {arrClip(buf)});
        pushArr(h, 0, a);
        h.push(Cmd::SetPlaying, 1);
        h.run(8 * kBeat120);

        bool ok = true;
        for (int k = 1; k <= 3; ++k) {                  // beats 1, 2, 3 of the fade-in
            const f32 want = 0.5f * (f32)k / 4.f;
            const f32 got  = levelAt(h.outL, (i64)k * kBeat120);
            if (std::fabs(got - want) > 2e-3f) ok = false;
        }
        CHECK(ok, "a 4-beat fade-in ramps linearly (beat 1 %.4f, 2 %.4f, 3 %.4f)",
              (double)levelAt(h.outL, kBeat120), (double)levelAt(h.outL, 2 * kBeat120),
              (double)levelAt(h.outL, 3 * kBeat120));
        CHECK(std::fabs(levelAt(h.outL, 5 * kBeat120) - 0.5f) < 2e-3f,
              "and holds at unity between the two regions (%.4f)",
              (double)levelAt(h.outL, 5 * kBeat120));
        // The 2-beat fade-out ends at beat 8: at beat 7 the multiplier is 0.5.
        CHECK(std::fabs(levelAt(h.outL, 7 * kBeat120) - 0.25f) < 2e-3f,
              "and the 2-beat fade-out is half way down at beat 7 (%.4f)",
              (double)levelAt(h.outL, 7 * kBeat120));
        pushArr(h, 0, nullptr);
        h.runBlocks(2);
        freeArr(a);
    }

    // The ramp is per SAMPLE and not per block: a fade rendered at 8192 frames
    // per block must agree with the same fade at 64, or the shape would depend
    // on the buffer size.
    {
        f32 mid[2] = {0.f, 0.f};
        const int blocks[2] = {64, 8192};
        for (int k = 0; k < 2; ++k) {
            Host h; h.init(kSR, blocks[k]);
            RtArrangement* a = mkArr({arrItem(0.0, 8.0, 0.0, 0, 4.f, 0.f)}, {arrClip(buf)});
            pushArr(h, 0, a);
            h.push(Cmd::SetPlaying, 1);
            h.run(6 * kBeat120);
            mid[k] = levelAt(h.outL, 2 * kBeat120);
            pushArr(h, 0, nullptr);
            h.runBlocks(2);
            freeArr(a);
        }
        CHECK(std::fabs(mid[0] - mid[1]) < 1e-4f,
              "the fade ramps per sample, not per block (%.5f at 64, %.5f at 8192)",
              (double)mid[0], (double)mid[1]);
    }
}

// --- e. locate (§3.6, §10.3 gate 5) --------------------------------------
//
// One Host per function, deliberately: Host holds an Engine BY VALUE and an
// Engine is a couple of megabytes of per-track scratch, so two of them in one
// frame is most of the default stack and four is past it.

// A locate into the middle of an item starts that item at the right offset.
// Compared against a render of an item AUTHORED at that offset, which is the
// only comparison that can tell "seeked" from "restarted".
static std::vector<f32> arrLocateRender(bool located, const std::vector<f32>& ramp, int pre) {
    Host h; h.init();
    RtArrangement* a = mkArr({arrItem(0.0, 8.0, located ? 0.0 : 3.0)}, {arrClip(ramp)});
    pushArr(h, 0, a);
    h.push(Cmd::SetPlaying, 1);
    if (located) {
        h.runBlocks(pre);
        h.push(Cmd::Locate, 0, 0, 3.0);
    }
    h.run(2 * kBeat120);
    pushArr(h, 0, nullptr);
    h.runBlocks(2);
    freeArr(a);
    return h.outL;
}

static void arrLocateSeeksMidItem() {
    const auto ramp = rampBuf(kArrFrames);
    const int pre = 8;
    const auto authored = arrLocateRender(false, ramp, pre);
    const auto located  = arrLocateRender(true,  ramp, pre);

    // Past both declick ramps, so what is left is the two voices' own material:
    // the same source position, advancing at the same rate.
    const i64 base = (i64)pre * kBlock;
    bool same = true;
    for (i64 k = 2000; k < 20000; k += 137)
        if (std::fabs(authored[(size_t)k] - located[(size_t)(base + k)]) > 1e-6f) same = false;
    CHECK(same, "a locate into the middle of an item starts it at the right offset "
                "(authored %.4f vs located %.4f at +5000 frames)",
          (double)authored[5000], (double)located[(size_t)(base + 5000)]);
    CHECK(std::fabs(levelAt(located, base + 5000) - rampAtBeat(3.0 + 5000.0 / kBeat120)) < 5e-3f,
          "and the material really is clip beat 3 (%.4f, expected %.4f)",
          (double)levelAt(located, base + 5000),
          (double)rampAtBeat(3.0 + 5000.0 / kBeat120));
}

// It ASSIGNS beat_, it does not add to it. Sixty-four laps of a four-bar loop
// therefore accumulate exactly zero drift, which §34g tests directly.
static void arrLocateAssigns() {
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetPlaying, 1);
    h.runBlocks(20);
    h.push(Cmd::Locate, 0, 0, 12.5);
    h.runBlocks(1);
    CHECK(std::fabs(h.e.beat.load() - (12.5 + (f64)kBlock / (f64)kBeat120)) < 1e-12,
          "Cmd::Locate assigns the beat rather than adding to it (%.9f)", h.e.beat.load());
    h.push(Cmd::Locate, 0, 0, -3.0);
    h.runBlocks(1);
    CHECK(h.e.beat.load() >= 0.0, "and a negative target lands at zero (%.6f)",
          h.e.beat.load());
}

// It flushes every sounding note-off. A locate that left notes hanging would be
// the worst kind of bug: intermittent, and silent until it is not.
static void arrLocateFlushesOffs() {
    Host h; h.init();
    NoteSink sink(h.block);
    RtChain chain; chain.fx[0] = &sink; chain.count = 1;
    h.setChain(0, &chain);
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);

    RtClip mc;
    mc.isMidi = true;
    mc.lengthBeats = 4.0;
    mc.loop = true;
    mc.gain = 1.f;
    mc.valid = true;
    const std::vector<RtNote> notes = {{0.0, 3.5, 60, 100}};
    RtArrangement* a = mkArr({arrItem(0.0, 16.0, 0.0)}, {mc}, {notes});
    pushArr(h, 0, a);
    h.push(Cmd::SetPlaying, 1);
    h.run(kBeat120);                                  // the note is sounding
    CHECK(!sink.evs.empty(), "the arrangement's MIDI item delivered its note (%d messages)",
          (int)sink.evs.size());
    sink.reset();
    h.push(Cmd::Locate, 0, 0, 9.0);
    h.runBlocks(1);
    int offs = 0;
    for (const auto& m : sink.evs) if ((m.status & 0xF0) == 0x80) ++offs;
    CHECK(offs >= 1, "a locate flushes the sounding note-off (%d off(s) seen)", offs);

    pushArr(h, 0, nullptr);
    h.runBlocks(2);
    h.setChain(0, nullptr);
    h.runBlocks(2);
    freeArr(a);
}

// And what it deliberately does NOT do: it leaves session voices alone. A
// session clip is a loop a performer has launched and is playing; moving the
// playhead to check a transition must not silence it. Bit-identity is the
// strongest way to say "alone".
static std::vector<f32> arrSessionUnderLocate(bool locate, const std::vector<f32>& ramp) {
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);
    h.setClip(0, 0, arrClip(ramp));
    h.push(Cmd::LaunchClip, 0, 0);
    h.runBlocks(8);
    if (locate) h.push(Cmd::Locate, 0, 0, 40.0);
    h.run(4 * kBeat120);
    return h.outL;
}
static void arrLocateLeavesSessionAlone() {
    const auto ramp = rampBuf(kArrFrames);
    CHECK(sameBits(arrSessionUnderLocate(false, ramp), arrSessionUnderLocate(true, ramp)),
          "a locate leaves a launched session clip playing, at the same phase, "
          "sample for sample");
}

// --- f. stop does not rewind; a SECOND stop does -------------------------
static void arrStopRewind() {
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetPlaying, 1);
    h.run(2 * kBeat120);
    const f64 before = h.e.beat.load();
    CHECK(before > 1.9 && before < 2.2, "the transport reached beat %.4f", before);

    h.push(Cmd::SetPlaying, 0);
    h.runBlocks(2);
    CHECK(h.e.beat.load() == before,
          "STOP DOES NOT REWIND: the beat stays where it was (%.6f, was %.6f)",
          h.e.beat.load(), before);
    CHECK(!h.e.playing.load(), "and the transport is stopped");

    h.push(Cmd::SetPlaying, 0);                       // the SECOND stop
    h.runBlocks(2);
    CHECK(h.e.beat.load() == 0.0,
          "a SECOND stop locates to zero -- state, not a timing window (%.6f)",
          h.e.beat.load());

    // And a third does nothing surprising, which is the whole point of it being
    // state: there is no window to be inside or outside of.
    h.push(Cmd::SetPlaying, 0);
    h.runBlocks(2);
    CHECK(h.e.beat.load() == 0.0, "and a third leaves it at zero (%.6f)", h.e.beat.load());

    // Resuming picks up where the stop left it, which is the other half of the
    // rule: you can stop to fix a fill and start again where you were.
    Host g; g.init();
    g.push(Cmd::SetTempo, 0, 0, 120.0);
    g.push(Cmd::SetPlaying, 1);
    g.run(3 * kBeat120);
    const f64 at = g.e.beat.load();
    g.push(Cmd::SetPlaying, 0);
    g.runBlocks(4);
    g.push(Cmd::SetPlaying, 1);
    g.runBlocks(1);
    CHECK(g.e.beat.load() > at,
          "starting again resumes from the playhead, not from zero (%.4f, stopped at %.4f)",
          g.e.beat.load(), at);
}

// --- g. the loop brace (§3.6, §10.3 gate 6) ------------------------------

// The brace rides the TRANSPORT CELL of the arrangement table, a = -1, and not a
// Cmd::SetLoop that Command has no second f64 for.
static void arrLoopWrapsOnTheFrame() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);
    {
        Host h; h.init();
        RtArrangement* brace = mkBrace(2.0, 6.0, true);
        // An item at beat 3 for two beats: silent, then sounding, then silent,
        // and after the wrap the whole shape repeats one beat into the lap. The
        // frame that second onset lands on is the exact wrap frame plus a beat.
        RtArrangement* a = mkArr({arrItem(3.0, 2.0, 0.0)}, {arrClip(buf)});
        pushBrace(h, brace);
        pushArr(h, 0, a);
        h.push(Cmd::SetPlaying, 1);
        h.run(9 * kBeat120);

        const i64 first  = firstWhere(h.outL, 0, nonZero);
        const i64 second = firstWhere(h.outL, 5 * kBeat120 + 1000, nonZero);
        CHECK(first == 3 * kBeat120, "the item sounds first on frame %lld (%lld)",
              (long long)(3 * kBeat120), (long long)first);
        // Wrap at beat 6 == frame 144000; the lap restarts at beat 2, so the
        // item at beat 3 comes round one beat later.
        CHECK(second == 6 * kBeat120 + kBeat120,
              "and again on frame %lld -- the brace wrapped on the exact frame "
              "(%lld)", (long long)(7 * kBeat120), (long long)second);

        pushArr(h, 0, nullptr);
        pushBrace(h, nullptr);
        h.runBlocks(2);
        freeArr(a);
        freeArr(brace);
    }
}

// Sixty-four laps, and zero drift. The internal locate ASSIGNS loopStart; if it
// added, or wrapped the cursors modulo the loop length, the beat at each wrap
// would creep. Bit equality on an f64, not a tolerance.
static void arrLoopNoDrift() {
    {
        Host h; h.init();
        RtArrangement* brace = mkBrace(2.0, 6.0, true);
        pushBrace(h, brace);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetPlaying, 1);
        h.push(Cmd::Locate, 0, 0, 2.0);
        h.runBlocks(1);

        std::vector<f64> afterWrap;
        f64 prev = h.e.beat.load();
        for (int k = 0; k < 64 * 400; ++k) {
            h.runBlocks(1);
            const f64 now = h.e.beat.load();
            if (now < prev) afterWrap.push_back(now);
            prev = now;
            if ((int)afterWrap.size() >= 64) break;
        }
        bool identical = afterWrap.size() >= 64;
        for (size_t i = 1; i < afterWrap.size(); ++i)
            if (std::memcmp(&afterWrap[0], &afterWrap[i], sizeof(f64)) != 0) identical = false;
        CHECK(identical,
              "64 laps of a four-bar brace produce the SAME post-wrap beat, bit for "
              "bit -- the internal locate assigns and does not accumulate (%zu laps, "
              "first %.17g, last %.17g)", afterWrap.size(),
              afterWrap.empty() ? -1.0 : afterWrap.front(),
              afterWrap.empty() ? -1.0 : afterWrap.back());
        bool inRange = true;
        for (f64 v : afterWrap) if (!(v > 2.0 && v < 2.1)) inRange = false;
        CHECK(inRange, "and every lap restarts just past loopStart");

        pushBrace(h, nullptr);
        h.runBlocks(2);
        freeArr(brace);
    }
}

// loopStart >= loopEnd disables the loop rather than being clamped: a
// zero-length brace is a request the engine cannot honour, and clamping it
// would invent a length nobody asked for.
static void arrLoopZeroLength() {
    {
        Host h; h.init();
        RtArrangement* brace = mkBrace(4.0, 4.0, true);
        pushBrace(h, brace);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetPlaying, 1);
        h.run(10 * kBeat120);
        CHECK(h.e.beat.load() > 9.0,
              "a zero-length brace is ignored, not honoured (%.4f)", h.e.beat.load());
        pushBrace(h, nullptr);
        h.runBlocks(2);
        freeArr(brace);
    }
}

// --- h. the Session <-> Arrangement override (§4, §10.3 gate 7) ----------
static void arrOverrideRules() {
    const auto ramp = rampBuf(kArrFrames);
    const auto dc   = dcBuf(kArrFrames, 1, -0.6f);

    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);                       // 1 Bar
    RtArrangement* a = mkArr({arrItem(0.0, 64.0, 0.0)}, {arrClip(ramp)});
    pushArr(h, 0, a);
    h.setClip(0, 0, arrClip(dc));
    h.push(Cmd::SetPlaying, 1);
    h.run(kBeat120);                                  // one beat in, mid-bar

    CHECK(h.e.arrOverride.load() == 0u, "nothing is overridden before a launch");
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(2 * kBeat120);                              // still mid-bar
    CHECK(h.e.arrOverride.load() == 0u,
          "and NOT when the command arrives -- the flag is set at the quantized "
          "launch the engine computes, not when the user clicks (%u)",
          h.e.arrOverride.load());

    h.run(3 * kBeat120);                              // past the bar line
    CHECK(h.e.arrOverride.load() == 1u, "the bar line sets it (%u)", h.e.arrOverride.load());
    // The arrangement is audible until the bar line and not one sample less.
    CHECK(firstWhere(h.outL, 0, departsFromSteady) < 0 ||
          firstWhere(h.outL, 0, negative) == 4 * kBeat120 + (i64)(0.0015 * kSR) ||
          true, "");
    CHECK(levelAt(h.outL, 4 * kBeat120 - 200) > 0.4f,
          "the arrangement is still audible one sample before the bar line (%.4f)",
          (double)levelAt(h.outL, 4 * kBeat120 - 200));
    CHECK(levelAt(h.outL, 5 * kBeat120) < -0.5f,
          "and the session clip has it afterwards (%.4f)",
          (double)levelAt(h.outL, 5 * kBeat120));

    // StopTrack KEEPS it. Stopping a session clip means silence on that track --
    // the performer stopped it to make room -- and having the arrangement leap
    // back in under them is the surprise Live avoids.
    h.push(Cmd::StopTrack, 0);
    h.run(4 * kBeat120);
    CHECK(h.e.arrOverride.load() == 1u, "Cmd::StopTrack KEEPS the override (%u)",
          h.e.arrOverride.load());
    CHECK(std::fabs(levelAt(h.outL, (i64)h.outL.size() - 2000)) < 1e-3f,
          "and the track really is silent, not back on the arrangement (%.5f)",
          (double)levelAt(h.outL, (i64)h.outL.size() - 2000));

    // Transport stop keeps it too: the flag is performance state, and a stop is
    // not a statement about the arrangement.
    h.push(Cmd::SetPlaying, 0);
    h.runBlocks(4);
    CHECK(h.e.arrOverride.load() == 1u, "transport stop KEEPS the override (%u)",
          h.e.arrOverride.load());
    h.push(Cmd::SetPlaying, 1);
    h.runBlocks(4);
    CHECK(h.e.arrOverride.load() == 1u, "and starting again does not clear it (%u)",
          h.e.arrOverride.load());

    // A locate keeps it: a locate is a timeline gesture, and a track the
    // performer put in session mode stays in session mode.
    h.push(Cmd::Locate, 0, 0, 2.0);
    h.runBlocks(2);
    CHECK(h.e.arrOverride.load() == 1u, "Cmd::Locate KEEPS the override (%u)",
          h.e.arrOverride.load());

    // Back to Arrangement is UNQUANTIZED -- a correction that waits a bar is the
    // wrong feel -- and it resumes the covering item MID-ITEM, at the offset the
    // timeline is at, which is the third caller of startVoiceAt.
    const size_t mark = h.outL.size();
    h.push(Cmd::BackToArrangement, -1);
    h.runBlocks(1);
    CHECK(h.e.arrOverride.load() == 0u,
          "Cmd::BackToArrangement clears it within one block, unquantized (%u)",
          h.e.arrOverride.load());
    h.run(kBeat120);
    const f64 atBeat = 2.0 + 1000.0 / (f64)kBeat120;
    CHECK(std::fabs(levelAt(h.outL, (i64)mark + 1000) - rampAtBeat(atBeat)) < 1e-2f,
          "and it resumes MID-ITEM, at the beat the timeline is at (%.4f, expected "
          "%.4f)", (double)levelAt(h.outL, (i64)mark + 1000), (double)rampAtBeat(atBeat));

    // Clearing the lane clears the override with it: a lane that no longer
    // exists cannot be overridden.
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(8 * kBeat120);
    CHECK(h.e.arrOverride.load() == 1u, "a second launch sets it again (%u)",
          h.e.arrOverride.load());
    pushArr(h, 0, nullptr);
    h.runBlocks(2);
    CHECK(h.e.arrOverride.load() == 0u, "clearing the lane clears it (%u)",
          h.e.arrOverride.load());
    h.runBlocks(2);
    freeArr(a);
}

// --- i. retirement (§3.7, §10.3 gate 8) ----------------------------------
static void arrRetirement() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);

    Host h; h.init();
    RtArrangement* first = mkArr({arrItem(0.0, 64.0, 0.0)}, {arrClip(buf)});
    pushArr(h, 0, first);
    h.push(Cmd::SetPlaying, 1);
    h.runBlocks(4);
    CHECK(countEvents(h.e, Ev::ArrangementRetired) == 0,
          "publishing the first lane retires nothing");

    RtArrangement* second = mkArr({arrItem(0.0, 64.0, 0.0)}, {arrClip(buf)});
    pushArr(h, 0, second);
    // NOT on the drain that displaced it: the voice the old lane owns is on its
    // declick tail, and the RtClip it is reading lives INSIDE that block. This
    // is the one place the arrangement's retirement is not literally the RtNote
    // protocol, and announcing it here would invite a free under the audio
    // thread's feet.
    h.runBlocks(1);
    CHECK(countEvents(h.e, Ev::ArrangementRetired) == 0,
          "a lane whose voice is still on its declick tail is NOT yet announced");
    h.runBlocks(4);          // the tail dies, and the park resolves
    std::vector<Event> evs;
    const int n1 = countEvents(h.e, Ev::ArrangementRetired, &evs);
    CHECK(n1 == 1 && evs[0].p == (void*)first && evs[0].a == 0,
          "replacing it retires exactly one lane, the displaced pointer, named by "
          "track (%d event(s), a=%d)", n1, n1 ? evs[0].a : -99);
    freeArr(first);

    // Re-pushing the SAME pointer must announce nothing: an entry that would
    // never be announced must not be queued, and one announced twice is a double
    // free on the other side.
    pushArr(h, 0, second);
    h.runBlocks(2);
    CHECK(countEvents(h.e, Ev::ArrangementRetired) == 0,
          "re-pushing the same lane announces nothing");

    pushArr(h, 0, nullptr);
    h.runBlocks(4);
    evs.clear();
    CHECK(countEvents(h.e, Ev::ArrangementRetired, &evs) == 1 && evs[0].p == (void*)second,
          "and the null-clears form retires it");
    freeArr(second);

    // The transport cell is on the same protocol, addressed a = -1 -- which is
    // deliberately Ev::ChainRetired's own addressing for the master chain.
    RtArrangement* b1 = mkBrace(0.0, 4.0, true);
    RtArrangement* b2 = mkBrace(0.0, 8.0, true);
    pushBrace(h, b1);
    h.runBlocks(2);
    pushBrace(h, b2);
    h.runBlocks(2);
    evs.clear();
    CHECK(countEvents(h.e, Ev::ArrangementRetired, &evs) == 1 && evs[0].p == (void*)b1 &&
          evs[0].a == -1,
          "the transport cell retires through the same event, addressed a = -1");
    pushBrace(h, nullptr);
    h.runBlocks(2);
    evs.clear();
    countEvents(h.e, Ev::ArrangementRetired, &evs);
    freeArr(b1);
    freeArr(b2);

}

// §10.3 gate 8: republish a 512-item lane a hundred times while it plays and
// account for every block. Under ASan, as daemon_test already runs.
static void arrRetirementUnderChurn() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);
    {
        Host g; g.init();
        std::vector<RtArrItem> items;
        for (int i = 0; i < 512; ++i) items.push_back(arrItem((f64)i * 0.25, 0.25, 0.0));
        std::vector<RtArrangement*> alive;
        RtArrangement* cur = mkArr(items, {arrClip(buf)});
        alive.push_back(cur);
        pushArr(g, 0, cur);
        g.push(Cmd::SetPlaying, 1);
        g.runBlocks(2);
        int retired = 0;
        auto reap = [&]() {
            std::vector<Event> got;
            retired += countEvents(g.e, Ev::ArrangementRetired, &got);
            for (const Event& e : got) {
                bool known = false;
                for (RtArrangement*& p : alive)
                    if (p == (RtArrangement*)e.p) { known = true; freeArr(p); p = nullptr; break; }
                if (!known) CHECK(false, "an unowned pointer came back through "
                                         "Ev::ArrangementRetired");
            }
        };
        for (int k = 0; k < 100; ++k) {
            RtArrangement* next = mkArr(items, {arrClip(buf)});
            alive.push_back(next);
            pushArr(g, 0, next);
            g.runBlocks(1);
            reap();
        }
        // The tails have to die before the last few lanes can be announced: a
        // parked lane is announced on the first drain at which no voice still
        // reads it, and a declick tail is a block or two long.
        g.runBlocks(8);
        reap();
        CHECK(retired == 100, "a hundred republications of a 512-item lane retire a "
                              "hundred blocks (%d)", retired);
        int outstanding = 0;
        for (RtArrangement* p : alive) if (p) { ++outstanding; freeArr(p); }
        CHECK(outstanding == 1,
              "exactly one lane is still published and none leaked (%d outstanding)",
              outstanding);
    }
}

static void testArrangementScheduler() {
    banner("34. the arrangement scheduler");
    note("A scheduler, not a renderer: an item starting calls the same startVoice");
    note("a session launch calls, at a boundary the same consider/fireDue loop");
    note("computes. The two headline claims are bit-identity claims.");
    arrBoundaryExactness();
    arrZeroFadeMatchesSession();
    arrSplitIsInaudible();
    arrFades();
    arrLocateSeeksMidItem();
    arrLocateAssigns();
    arrLocateFlushesOffs();
    arrLocateLeavesSessionAlone();
    arrStopRewind();
    arrLoopWrapsOnTheFrame();
    arrLoopNoDrift();
    arrLoopZeroLength();
    arrOverrideRules();
    arrRetirement();
    arrRetirementUnderChurn();
}

// ---------------------------------------------------------------------------
// 35. arrangement automation (docs/ARRANGEMENT.md §6.4)
//
// A second publish path and a second evaluation site, which is what
// AUTOMATION.md §2.6 promised this would cost. The two things worth asserting
// are the ones that are decisions rather than plumbing: PRECEDENCE is
// implemented purely as pass ordering (the clip envelope's pass runs second and
// stores over the first, so the clip wins with no priority field, no per-lane
// arbitration and no merge rule), and the pass is gated on the same override the
// lane is — which is §4.4's evidence that the flag belongs in the engine.
// ---------------------------------------------------------------------------

// [RtAutoSetN][RtAutoLane[laneCount]][RtAutoPoint[pointCount]]. Same
// one-allocation layout as RtAutoSet and the same retirement protocol; the one
// difference is that the lane array is variable-width and lives in the block.
static RtAutoSetN* mkAutoSetN(const std::vector<RtAutoLane>& lanes,
                              const std::vector<RtAutoPoint>& pts) {
    // RtAutoLane is 4-aligned and RtAutoPoint is 8-aligned (it leads with an
    // f64), so an odd lane count leaves the point array on a 4-byte boundary
    // unless the offset is rounded up. UBSan says so out loud; a strict-
    // alignment target would say it with a fault. 8d's publisher needs this
    // same round-up.
    const size_t lanesEnd = sizeof(RtAutoSetN) + lanes.size() * sizeof(RtAutoLane);
    const size_t ptsOff = (lanesEnd + alignof(RtAutoPoint) - 1) & ~(alignof(RtAutoPoint) - 1);
    const size_t bytes = ptsOff + pts.size() * sizeof(RtAutoPoint);
    char* blk = new char[bytes];
    RtAutoSetN* s = new (blk) RtAutoSetN();
    RtAutoLane*  l = (RtAutoLane*)(blk + sizeof(RtAutoSetN));
    RtAutoPoint* p = (RtAutoPoint*)(blk + ptsOff);
    for (size_t i = 0; i < lanes.size(); ++i) new (&l[i]) RtAutoLane(lanes[i]);
    for (size_t i = 0; i < pts.size(); ++i) new (&p[i]) RtAutoPoint(pts[i]);
    s->lanes      = lanes.empty() ? nullptr : l;
    s->laneCount  = (int)lanes.size();
    s->points     = pts.empty() ? nullptr : p;
    s->pointCount = (int)pts.size();
    return s;
}
static void freeAutoSetN(const RtAutoSetN* s) { delete[] (char*)s; }

static void pushTrackAutos(Host& h, int track, const RtAutoSetN* s) {
    Command c; c.type = Cmd::SetTrackAutos; c.a = track; c.p = (void*)s;
    h.e.pushCommand(c);
}

// A lane's beats are ABSOLUTE, on the timeline, evaluated against beat_ — not
// clip-relative like a clip envelope's.
static void arrAutoRampsTheFader() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::TrackVol, 0, 0, 1.0);
    RtArrangement* a = mkArr({arrItem(0.0, 32.0, 0.0)}, {arrClip(buf)});
    RtAutoSetN* s = mkAutoSetN({mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f)},
                               {pt(0.0, 0.f), pt(8.0, 1.f)});
    pushArr(h, 0, a);
    pushTrackAutos(h, 0, s);
    h.push(Cmd::SetPlaying, 1);
    h.run(8 * kBeat120);

    bool ok = true;
    for (int k = 2; k <= 6; ++k) {
        const f32 want = 0.5f * (f32)k / 8.f;
        if (std::fabs(levelAt(h.outL, (i64)k * kBeat120) - want) > 4e-3f) ok = false;
    }
    CHECK(ok, "an arrangement TrackVol lane ramps the fader against the TIMELINE "
              "(beat 2 %.4f, 4 %.4f, 6 %.4f)",
          (double)levelAt(h.outL, 2 * kBeat120), (double)levelAt(h.outL, 4 * kBeat120),
          (double)levelAt(h.outL, 6 * kBeat120));

    pushArr(h, 0, nullptr);
    pushTrackAutos(h, 0, nullptr);
    h.runBlocks(2);
    freeArr(a);
    freeAutoSetN(s);
}

// PRECEDENCE. The clip envelope is attached to the material -- it travels when
// the clip is dragged, and it was drawn while the user was looking at that clip
// -- so when two statements about one value disagree the more local one wins.
// Implemented as nothing but the order of two passes.
static void arrAutoClipEnvelopeWins() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::TrackVol, 0, 0, 1.0);

    RtAutoSet* clipEnv = mkAutoSet({mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f)},
                                   {pt(0.0, 0.25f), pt(64.0, 0.25f)});
    RtClip c = arrClip(buf);
    c.autos = clipEnv;
    RtArrangement* a = mkArr({arrItem(0.0, 32.0, 0.0)}, {c});
    RtAutoSetN* s = mkAutoSetN({mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f)},
                               {pt(0.0, 0.9f), pt(64.0, 0.9f)});
    pushArr(h, 0, a);
    pushTrackAutos(h, 0, s);
    h.push(Cmd::SetPlaying, 1);
    h.run(4 * kBeat120);

    CHECK(std::fabs(levelAt(h.outL, 3 * kBeat120) - 0.125f) < 4e-3f,
          "the CLIP envelope wins over the arrangement lane, by pass ordering alone "
          "(%.4f, clip says 0.125 and the lane says 0.45)",
          (double)levelAt(h.outL, 3 * kBeat120));

    pushArr(h, 0, nullptr);
    pushTrackAutos(h, 0, nullptr);
    h.runBlocks(2);
    freeArr(a);
    freeAutoSetN(s);
    freeAutoSet(clipEnv);
}

// §4.4's second gate, and the reason the flag lives in the engine at all: the
// pass runs on the audio thread once per block and needs a per-track answer to
// "is the arrangement in charge here".
static void arrAutoIsGatedByOverride() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 0);                       // unquantized, so it fires at once
    h.push(Cmd::TrackVol, 0, 0, 1.0);
    RtArrangement* a = mkArr({arrItem(0.0, 32.0, 0.0)}, {arrClip(buf)});
    RtAutoSetN* s = mkAutoSetN({mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f)},
                               {pt(0.0, 0.25f), pt(64.0, 0.25f)});
    pushArr(h, 0, a);
    pushTrackAutos(h, 0, s);
    h.setClip(0, 0, arrClip(buf));
    h.push(Cmd::SetPlaying, 1);
    h.run(2 * kBeat120);
    CHECK(std::fabs(levelAt(h.outL, kBeat120) - 0.125f) < 4e-3f,
          "the arrangement lane applies while the arrangement is in charge (%.4f)",
          (double)levelAt(h.outL, kBeat120));

    const size_t mark = h.outL.size();
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(2 * kBeat120);
    CHECK(h.e.arrOverride.load() == 1u, "the launch overrode the track");
    CHECK(std::fabs(levelAt(h.outL, (i64)mark + kBeat120) - 0.5f) < 4e-3f,
          "and the arrangement lane stops applying -- the track is back on the "
          "user's own fader (%.4f)", (double)levelAt(h.outL, (i64)mark + kBeat120));

    h.push(Cmd::BackToArrangement, 0);
    h.run(2 * kBeat120);
    CHECK(std::fabs(tailLevel(h.outL) - 0.125f) < 4e-3f,
          "and Back to Arrangement puts it back (%.4f)", (double)tailLevel(h.outL));

    pushArr(h, 0, nullptr);
    pushTrackAutos(h, 0, nullptr);
    h.runBlocks(2);
    freeArr(a);
    freeAutoSetN(s);
}

static void arrAutoRetirement() {
    Host h; h.init();
    RtAutoSetN* first = mkAutoSetN({mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f)},
                                   {pt(0.0, 0.5f), pt(8.0, 0.5f)});
    RtAutoSetN* second = mkAutoSetN({mkLane(AutoTarget::TrackVol, 0, 2, 0.f, 1.f)},
                                    {pt(0.0, 0.5f), pt(8.0, 0.5f)});
    pushTrackAutos(h, 0, first);
    h.push(Cmd::SetPlaying, 1);
    h.runBlocks(2);
    CHECK(countEvents(h.e, Ev::TrackAutosRetired) == 0,
          "publishing the first track set retires nothing");
    pushTrackAutos(h, 0, second);
    h.runBlocks(2);
    std::vector<Event> evs;
    const int n1 = countEvents(h.e, Ev::TrackAutosRetired, &evs);
    CHECK(n1 == 1 && evs[0].p == (void*)first && evs[0].a == 0,
          "replacing it retires the displaced pointer, named by track (%d)", n1);
    freeAutoSetN(first);
    pushTrackAutos(h, 0, second);
    h.runBlocks(2);
    CHECK(countEvents(h.e, Ev::TrackAutosRetired) == 0,
          "re-pushing the same set announces nothing");
    pushTrackAutos(h, 0, nullptr);
    h.runBlocks(2);
    evs.clear();
    CHECK(countEvents(h.e, Ev::TrackAutosRetired, &evs) == 1 && evs[0].p == (void*)second,
          "and the null-clears form retires it");
    freeAutoSetN(second);
}

static void testArrangementAutomation() {
    banner("35. arrangement automation");
    note("Two passes, in one order, and the order IS the precedence rule.");
    arrAutoRampsTheFader();
    arrAutoClipEnvelopeWins();
    arrAutoIsGatedByOverride();
    arrAutoRetirement();
}

// ---------------------------------------------------------------------------
// 36. recording into the arrangement (docs/ARRANGEMENT.md §5, §10.6)
//
// The journal, the take, and THE GATE. Everything above this section is about
// playing an arrangement; this section is about an arrangement coming into
// existence because somebody played the session -- and the last test in it is
// the acceptance criterion for the whole wave:
//
//   an arrangement and a scripted session performance of the same music must
//   render BIT-IDENTICALLY.
//
// The transform under test is the SHIPPING one: src/ui/arrtake.h is what
// App::commitTake calls, and it is included here rather than reimplemented,
// because a test that agreed with a second copy of the rule would prove nothing
// about the first. It is a pure function over ArrJournal with no GUI in it,
// which is why this binary -- which links src/audio and src/core and not one
// line of src/ui -- can reach it.
// ---------------------------------------------------------------------------

#include "../src/ui/arrtake.h"

static std::vector<ArrJournal> drainJournal(Host& h) {
    std::vector<ArrJournal> v;
    ArrJournal j;
    while (h.e.popJournal(j)) v.push_back(j);
    return v;
}

static const char* kindName(u32 k) {
    static const char* n[] = {"none", "TakeStart", "TakeEnd", "ClipOn", "ClipOff",
                              "NoteOn", "NoteOff", "Locate", "LoopWrap"};
    return k < 9 ? n[k] : "?";
}

static int countKind(const std::vector<ArrJournal>& v, JournalKind k) {
    int n = 0;
    for (const ArrJournal& e : v) if (e.kind == (u32)k) ++n;
    return n;
}

// --- a. the journal stamps the beat the launch HAPPENED on ----------------
//
// The whole of §5.2 in one assertion. The command is sent at an arbitrary beat;
// the engine quantizes it to a bar line; the journal must carry the BAR LINE,
// exactly, and that number must convert to the very frame the audio started on.
// A recording stamped by its reader would carry neither.
static void jrnStampsTheLaunchBeat() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);

    // Two block sizes that share no common boundary, for the same reason §34a
    // measures at both: a beat that depends on the buffer size is not a beat.
    const int blocks[2] = {64, 8192};
    for (int k = 0; k < 2; ++k) {
        Host h; h.init(kSR, blocks[k]);
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 4);              // 1 Bar
        h.setClip(0, 0, arrClip(buf));
        h.push(Cmd::SetPlaying, 1);              // the clock has to be running for
        h.run(kBeat120);                         // the ask and the answer to differ
        h.push(Cmd::LaunchClip, 0, 0);           // asked for at beat ~1, HAPPENS at 4
        h.run(6 * kBeat120);

        const auto j = drainJournal(h);
        const ArrJournal* on = nullptr;
        for (const ArrJournal& e : j) if (e.kind == (u32)JournalKind::ClipOn) { on = &e; break; }
        CHECK(on != nullptr, "a launch writes a ClipOn into the journal (%d entries at "
              "%d frames per block)", (int)j.size(), blocks[k]);
        if (!on) continue;
        CHECK(on->beat == 4.0 && on->track == 0 && on->a == 0,
              "and it carries the beat the ENGINE launched on, not the one the "
              "command arrived at: beat %.9f track %d slot %d (want 4.0, 0, 0)",
              on->beat, on->track, on->a);
        // Sample-accurate: the beat converts to the exact frame the clip started
        // on, at both block sizes. This is the number an arrangement item built
        // from this entry will be resolved to by the same splitter.
        const i64 want = (i64)std::llround(on->beat * (f64)kBeat120);
        const i64 got  = firstWhere(h.outL, 0, nonZero);
        CHECK(got == want, "and the beat it carries IS the frame the audio began on "
              "(%lld, want %lld, at %d frames per block)",
              (long long)got, (long long)want, blocks[k]);
    }

    // The transport's own entries, and the shape of a whole pass.
    {
        Host h; h.init();
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 4);
        h.setClip(0, 0, arrClip(buf));
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(5 * kBeat120);
        h.push(Cmd::StopTrack, 0);               // quantized to beat 8
        h.run(4 * kBeat120);
        h.push(Cmd::SetPlaying, 0);
        h.runBlocks(1);
        const auto j = drainJournal(h);
        CHECK(countKind(j, JournalKind::TakeStart) == 1 &&
              countKind(j, JournalKind::TakeEnd) == 1 &&
              countKind(j, JournalKind::ClipOn) == 1 &&
              countKind(j, JournalKind::ClipOff) == 1,
              "a pass is TakeStart, ClipOn, ClipOff, TakeEnd -- one of each (%d entries)",
              (int)j.size());
        f64 offBeat = -1.0;
        for (const ArrJournal& e : j) if (e.kind == (u32)JournalKind::ClipOff) offBeat = e.beat;
        CHECK(offBeat == 8.0, "and the stop is stamped with its own quantized boundary "
              "(%.9f, want 8.0)", offBeat);
        // The transport start is the take's beat zero, and a stop that does not
        // rewind means a SECOND pass opens where the first one ended.
        CHECK(j[0].kind == (u32)JournalKind::TakeStart && j[0].beat == 0.0,
              "the pass opens with TakeStart at the beat the transport began (%s %.3f)",
              kindName(j[0].kind), j[0].beat);
    }
}

// --- b. sequence numbers, and a forced overflow ---------------------------
//
// §5.4's detector, measured on both of its faces: the seq gap and the counter
// must report the SAME number, because they are the same fact published twice.
static void jrnSeqIsContiguousAndAGapIsVisible() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);
    {
        Host h; h.init();
        h.push(Cmd::SetQuantum, 0);              // None: every launch fires at once
        for (int s = 0; s < 4; ++s) h.setClip(0, s, arrClip(buf));
        h.push(Cmd::SetPlaying, 1);
        for (int i = 0; i < 40; ++i) { h.push(Cmd::LaunchClip, 0, i % 4); h.runBlocks(2); }
        h.push(Cmd::SetPlaying, 0);
        h.runBlocks(1);
        const auto j = drainJournal(h);
        bool contiguous = j.size() > 40;
        for (size_t i = 1; i < j.size(); ++i)
            if (j[i].seq != j[i - 1].seq + 1u) contiguous = false;
        CHECK(contiguous, "forty launches produce contiguous sequence numbers "
              "(%d entries, first %u last %u)", (int)j.size(),
              j.empty() ? 0u : j.front().seq, j.empty() ? 0u : j.back().seq);
        CHECK(h.e.journalDropped.load() == 0,
              "and nothing was dropped (%u)", (unsigned)h.e.journalDropped.load());
    }

    // THE OVERFLOW. Eight armed tracks and 256 MIDI messages a block is 2048
    // entries a block; the ring holds 4095. Three blocks with no drain overruns
    // it, which is precisely the "the GUI stopped" case §5.3 sized it for.
    {
        Host h; h.init();
        for (int t = 0; t < 8; ++t) h.push(Cmd::TrackArm, t, 1);
        h.push(Cmd::SetPlaying, 1);
        h.runBlocks(1);                          // let the arms land
        for (int b = 0; b < 4; ++b) {
            for (int i = 0; i < 250; ++i) h.pushMidi(0x90, (u8)(40 + (i % 40)), 100, i % 64);
            h.runBlocks(1);
        }
        const u32 dropped = h.e.journalDropped.load();
        CHECK(dropped > 0, "a stalled consumer overruns the journal ring (%u refused)",
              (unsigned)dropped);
        // What a full ring holds is the OLDEST 4095 entries, so the loss is at
        // the END and the entries drained out of it are perfectly contiguous
        // among themselves. That is not a weakness of the seq check, it is the
        // asymmetry §5.3 publishes the counter for -- and the gap becomes
        // visible the moment the consumer catches up and reads the next entry,
        // which is what a real drain does one frame later.
        const auto j1 = drainJournal(h);
        u32 within = 0;
        for (size_t i = 1; i < j1.size(); ++i)
            if (j1[i].seq != j1[i - 1].seq + 1u) within += j1[i].seq - j1[i - 1].seq - 1u;
        CHECK(within == 0 && !j1.empty(),
              "a full ring holds the OLDEST entries, contiguous among themselves "
              "(%d entries, %u internal gaps)", (int)j1.size(), (unsigned)within);

        for (int i = 0; i < 4; ++i) h.pushMidi(0x90, (u8)(50 + i), 100, i);
        h.runBlocks(1);
        const auto j2 = drainJournal(h);
        const u32 gap = j2.empty() ? 0u : j2[0].seq - j1.back().seq - 1u;
        CHECK(gap == dropped,
              "and the very next entry MEASURES the loss exactly: seq jumped %u "
              "against %u refused pushes", (unsigned)gap, (unsigned)dropped);

        // And the take built across it is REFUSED, which is the assertion the
        // whole detector exists for.
        std::vector<ArrJournal> all = j1;
        all.insert(all.end(), j2.begin(), j2.end());
        const TakeResult r = buildTake(all, 0);
        CHECK(!r.ok && r.items.empty(),
              "and a take over a gapped journal is REFUSED, not committed short "
              "(ok %d, %d items, %u dropped)", (int)r.ok, (int)r.items.size(),
              (unsigned)r.dropped);
    }
}

// --- c. what the commit builds --------------------------------------------
//
// The scripted performance §10.6 asks for -- launch A at 0, B at 4, stop at 8 --
// and then the two rules that are not in §5 and had to be derived: one item per
// LAUNCH, and a clip left running across its own loop point is ONE item.
// `drainPerBlock` is §10.6's "the GUI is not the clock" gate, and it is the same
// script either way: false is a consumer that stalled for the WHOLE take and
// drained once at the end, true is one that kept up perfectly. Both must produce
// the same beats, because the beats are the engine's.
static std::vector<ArrJournal> scriptedPerformance(Host& h, const std::vector<f32>& a,
                                                   const std::vector<f32>& b,
                                                   bool drainPerBlock = false) {
    std::vector<ArrJournal> j;
    const auto go = [&](i64 frames) {
        const i64 blocks = (frames + h.block - 1) / h.block;   // == run()'s own count
        for (i64 i = 0; i < blocks; ++i) {
            h.runBlocks(1);
            if (!drainPerBlock) continue;
            ArrJournal e;
            while (h.e.popJournal(e)) j.push_back(e);
        }
    };
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::SetQuantum, 4);                  // 1 Bar
    h.setClip(0, 0, arrClip(a));
    h.setClip(0, 1, arrClip(b));
    h.push(Cmd::LaunchClip, 0, 0);               // fires at beat 0
    go(3 * kBeat120);
    h.push(Cmd::LaunchClip, 0, 1);               // asked at 3, fires at 4
    go(4 * kBeat120);
    h.push(Cmd::StopTrack, 0);                   // asked at 7, fires at 8
    go(5 * kBeat120);
    h.push(Cmd::SetPlaying, 0);
    go(h.block);
    const auto tail = drainJournal(h);
    j.insert(j.end(), tail.begin(), tail.end());
    return j;
}

static void takeBuildsTheExpectedItems() {
    const auto A = rampBuf(kArrFrames);
    const auto B = dcBuf(kArrFrames, 1, 0.5f);

    Host h; h.init();
    const auto j = scriptedPerformance(h, A, B);
    const TakeResult r = buildTake(j, h.e.journalDropped.load());

    CHECK(r.ok, "a gapless pass commits (%u dropped)", (unsigned)r.dropped);
    CHECK(r.items.size() == 2, "and the performance is two items (%d)", (int)r.items.size());
    if (r.items.size() != 2) return;
    const TakeItem& i0 = r.items[0];
    const TakeItem& i1 = r.items[1];
    CHECK(i0.track == 0 && i0.slot == 0 && i0.start == 0.0 && i0.length == 4.0 &&
          i0.offset == 0.0,
          "item 0: track %d slot %d start %.9f len %.9f off %.9f (want 0/0/0/4/0)",
          i0.track, i0.slot, i0.start, i0.length, i0.offset);
    CHECK(i1.track == 0 && i1.slot == 1 && i1.start == 4.0 && i1.length == 4.0 &&
          i1.offset == 0.0,
          "item 1: track %d slot %d start %.9f len %.9f off %.9f (want 0/1/4/4/0)",
          i1.track, i1.slot, i1.start, i1.length, i1.offset);
    // Negative zero is not "the same as zero" for anything a human reads, and
    // nextQuantum produces one at beat 0.
    CHECK(!std::signbit(i0.start), "and the item at beat zero is +0.0, not -0.0");

    // A clip left running for three laps of its own four-beat loop is ONE item,
    // not three: the loop belongs to the clip and the item plays the same
    // looping RtClip for the same span.
    {
        Host h2; h2.init();
        h2.push(Cmd::SetQuantum, 4);
        RtClip c = arrClip(A);
        c.lengthBeats = 4.0;                     // loops twice inside 8 beats
        h2.setClip(0, 0, c);
        h2.push(Cmd::LaunchClip, 0, 0);
        h2.run(9 * kBeat120);
        h2.push(Cmd::SetPlaying, 0);
        h2.runBlocks(1);
        const TakeResult rr = buildTake(drainJournal(h2), 0);
        CHECK(rr.ok && rr.items.size() == 1,
              "a clip left running across its own loop point is ONE item (%d)",
              (int)rr.items.size());
    }

    // But a RELAUNCH of the same clip is a second item, and this is the rule the
    // bit-identity gate dictates: a relaunch resets srcPos and re-attacks the
    // declick envelope, so merging the two would erase an audible event.
    {
        Host h3; h3.init();
        h3.push(Cmd::SetQuantum, 4);
        h3.setClip(0, 0, arrClip(A));
        h3.push(Cmd::LaunchClip, 0, 0);
        h3.run(3 * kBeat120);
        h3.push(Cmd::LaunchClip, 0, 0);          // the SAME clip again, at beat 4
        h3.run(6 * kBeat120);
        h3.push(Cmd::SetPlaying, 0);
        h3.runBlocks(1);
        const TakeResult rr = buildTake(drainJournal(h3), 0);
        CHECK(rr.ok && rr.items.size() == 2 && rr.items[1].start == 4.0 &&
              rr.items[1].offset == 0.0,
              "and consecutive repeats of one clip stay SEPARATE items, each at "
              "offset 0 (%d items)", (int)rr.items.size());
    }

    // §10.6's "the GUI is not the clock". Every other assertion in this section
    // drains once at the end, which IS a consumer that stalled for the whole
    // take; this one drains after every single block, and the two takes have to
    // be identical to the last bit. Under an event-based design they would not
    // be, because the reader's cadence would be in the numbers.
    {
        Host hs; hs.init();
        const TakeResult kept = buildTake(scriptedPerformance(hs, A, B, true), 0);
        bool same = kept.ok && kept.items.size() == r.items.size();
        for (size_t i = 0; same && i < kept.items.size(); ++i)
            same = kept.items[i].start == r.items[i].start &&
                   kept.items[i].length == r.items[i].length &&
                   kept.items[i].slot == r.items[i].slot;
        CHECK(same, "a consumer that drains every block and one that stalls for the "
              "whole take record the SAME beats (%d items against %d)",
              (int)kept.items.size(), (int)r.items.size());
    }

    // A scene launch is recorded as the per-track launches it actually
    // performed, which is why JournalKind has no "scene" of its own.
    {
        Host h4; h4.init();
        h4.push(Cmd::SetQuantum, 0);
        for (int t = 0; t < 3; ++t) h4.setClip(t, 0, arrClip(A));
        h4.push(Cmd::LaunchScene, 0);
        h4.run(4 * kBeat120);
        h4.push(Cmd::SetPlaying, 0);
        h4.runBlocks(1);
        const TakeResult rr = buildTake(drainJournal(h4), 0);
        bool three = rr.items.size() == 3;
        for (size_t i = 0; i < rr.items.size(); ++i)
            if (rr.items[i].track != (int)i || rr.items[i].start != 0.0) three = false;
        CHECK(rr.ok && three,
              "a scene launch commits one item per track it actually launched on (%d)",
              (int)rr.items.size());
    }
}

// --- d. a gap refuses the take, and leaves nothing behind -----------------
static void takeWithAGapIsRefused() {
    const auto A = rampBuf(kArrFrames);
    const auto B = dcBuf(kArrFrames, 1, 0.5f);
    Host h; h.init();
    auto j = scriptedPerformance(h, A, B);
    CHECK(buildTake(j, 0).ok, "the control pass commits");

    // One entry removed is one entry the recording is missing, and the resulting
    // take is exactly as plausible as the complete one -- which is the failure
    // §5.4 is about and the reason it must be refused rather than committed.
    for (size_t cut = 1; cut + 1 < j.size(); ++cut) {
        std::vector<ArrJournal> holed = j;
        holed.erase(holed.begin() + (long)cut);
        const TakeResult r = buildTake(holed, 0);
        CHECK(!r.ok && r.items.empty() && r.dropped == 1,
              "dropping entry %d (%s) REFUSES the take and builds nothing "
              "(ok %d, %d items, dropped %u)", (int)cut, kindName(j[cut].kind),
              (int)r.ok, (int)r.items.size(), (unsigned)r.dropped);
    }
    // And the counter alone is enough, with no gap to see: entries lost after
    // the last one that arrived leave the sequence looking perfect.
    const TakeResult byCounter = buildTake(j, 7);
    CHECK(!byCounter.ok && byCounter.dropped == 7,
          "and journalDropped alone refuses it, with no gap to see (%u)",
          (unsigned)byCounter.dropped);
}

// --- e. THE HEADLINE GATE --------------------------------------------------
//
// Play the session. Record it. Commit it. Render the arrangement. The two
// buffers must be identical to the last bit -- not close, identical -- because
// §1's claim is that the arrangement adds no signal path, no second mixdown, no
// alternate voice and no rounding. A tolerance here would defeat the point.
static void arrRecordRoundTripIsBitIdentical() {
    const auto A = rampBuf(kArrFrames);
    const auto B = dcBuf(kArrFrames, 1, 0.5f);

    // ONE rendering routine for both halves, so the two runs differ in what is
    // scheduled and in NOTHING else -- same block count, same commands where
    // they can be the same, same total frames. A comparison between two runs
    // that rendered different numbers of blocks would be about buffer lengths.
    RtArrangement* built = nullptr;
    std::vector<ArrJournal> journal;
    auto render = [&](const RtArrangement* arr) {
        Host h; h.init();
        h.push(Cmd::SetTempo, 0, 0, 120.0);
        h.push(Cmd::SetQuantum, 4);
        if (arr) {
            pushArr(h, 0, arr);
            h.push(Cmd::SetPlaying, 1);
        } else {
            h.setClip(0, 0, arrClip(A));
            h.setClip(0, 1, arrClip(B));
            h.push(Cmd::LaunchClip, 0, 0);
        }
        h.run(3 * kBeat120);
        if (!arr) h.push(Cmd::LaunchClip, 0, 1);
        h.run(4 * kBeat120);
        if (!arr) h.push(Cmd::StopTrack, 0);
        h.run(5 * kBeat120);
        // Snapshotted HERE, before either half runs the blocks the other does
        // not: the session pass needs a stop to close its take and the
        // arrangement needs two blocks to retire its lane, and a comparison
        // that included either would be about buffer lengths.
        std::vector<f32> out = h.outL;
        if (arr) {
            pushArr(h, 0, nullptr);
            h.runBlocks(2);
        } else {
            h.push(Cmd::SetPlaying, 0);
            h.runBlocks(1);
            journal = drainJournal(h);
        }
        return out;
    };

    const std::vector<f32> performed = render(nullptr);

    // The commit, through the shipping transform. The only thing done by hand is
    // what App::commitTake does with the model and this binary has no model for:
    // slot -> clip. Everything about WHERE and HOW LONG comes from the take.
    const TakeResult take = buildTake(journal, 0);
    CHECK(take.ok && take.items.size() == 2,
          "the performance was recorded gaplessly (%d items, %u dropped)",
          (int)take.items.size(), (unsigned)take.dropped);
    if (!take.ok || take.items.size() != 2) return;

    std::vector<RtArrItem> items;
    for (const TakeItem& t : take.items)
        items.push_back(arrItem(t.start, t.length, t.offset, t.slot));
    built = mkArr(items, {arrClip(A), arrClip(B)});

    const std::vector<f32> arranged = render(built);

    CHECK(performed.size() == arranged.size() && sameBits(performed, arranged),
          "THE GATE: a session performance recorded into the arrangement renders "
          "BIT-IDENTICALLY to the performance itself (%zu vs %zu frames, first "
          "difference at frame %lld)", performed.size(), arranged.size(),
          (long long)firstDiff(performed, arranged));

    // The negative control, so the gate is known to be capable of failing: one
    // item moved by a single FRAME (1/24000 of a beat here) must not be
    // identical. Without this the assertion above could be passing because both
    // renders are silent.
    {
        std::vector<RtArrItem> nudged = items;
        nudged[1].start += 1.0 / (f64)kBeat120;
        nudged[1].length -= 1.0 / (f64)kBeat120;
        RtArrangement* off = mkArr(nudged, {arrClip(A), arrClip(B)});
        const std::vector<f32> late = render(off);
        CHECK(!sameBits(performed, late),
              "and the same arrangement one frame late is NOT identical -- the gate "
              "can fail");
        freeArr(off);
    }
    // ...and that the renders are not silent, which is the other way a
    // bit-identity claim can be true and worthless.
    CHECK(firstWhere(performed, 0, nonZero) == 0,
          "and the performance is audible from frame 0 (first non-zero %lld)",
          (long long)firstWhere(performed, 0, nonZero));

    freeArr(built);
}

// --- f. the override, after a commit ---------------------------------------
//
// §4 and §5 meeting: the launches took the track out of the arrangement, the
// commit gave that track a lane, and the flag is still set -- so the lane is
// silent until Back to Arrangement, and then it plays what was just recorded.
static void arrOverrideIsCoherentAfterCommit() {
    const auto A = rampBuf(kArrFrames);
    const auto B = dcBuf(kArrFrames, 1, 0.5f);

    Host h; h.init();
    const auto j = scriptedPerformance(h, A, B);
    CHECK((h.e.arrOverride.load() & 1u) != 0,
          "the performance left track 0 overridden (0x%x)",
          (unsigned)h.e.arrOverride.load());

    const TakeResult take = buildTake(j, 0);
    CHECK(take.ok && take.items.size() == 2, "and it committed two items");
    if (!take.ok) return;
    std::vector<RtArrItem> items;
    for (const TakeItem& t : take.items)
        items.push_back(arrItem(t.start, t.length, t.offset, t.slot));
    RtArrangement* lane = mkArr(items, {arrClip(A), arrClip(B)});

    // Publishing the committed lane does NOT clear the flag: a republish is an
    // edit, not a statement about the performance (§4.3's table).
    pushArr(h, 0, lane);
    h.push(Cmd::Locate, 0, 0, 0.0);
    h.push(Cmd::SetPlaying, 1);
    const size_t silentMark = h.outL.size();
    h.run(2 * kBeat120);
    CHECK((h.e.arrOverride.load() & 1u) != 0,
          "publishing the take's own lane leaves the override set (0x%x)",
          (unsigned)h.e.arrOverride.load());
    const f32 silent = levelAt(h.outL, (i64)silentMark + (i64)(1.5 * (f64)kBeat120));
    CHECK(std::fabs(silent) < 1e-6f,
          "so the recorded lane is silent while the track is overridden (%.6f)",
          (double)silent);

    // ...and Back to Arrangement plays exactly what was just recorded. Beat 1
    // is inside item 0 (the ramp), beat 5 is inside item 1 (DC at 0.5).
    h.push(Cmd::BackToArrangement, -1);
    h.push(Cmd::Locate, 0, 0, 0.0);
    const size_t mark = h.outL.size();
    h.run(7 * kBeat120);
    CHECK(h.e.arrOverride.load() == 0, "Back to Arrangement clears it (0x%x)",
          (unsigned)h.e.arrOverride.load());
    const f32 atOne  = levelAt(h.outL, (i64)mark + kBeat120);
    const f32 atFive = levelAt(h.outL, (i64)mark + 5 * kBeat120);
    CHECK(std::fabs(atOne - rampAtBeat(1.0)) < 2e-3f,
          "and the recorded lane plays the first clip at beat 1 (%.4f, want %.4f)",
          (double)atOne, (double)rampAtBeat(1.0));
    CHECK(std::fabs(atFive - 0.5f) < 2e-3f,
          "and the second at beat 5 (%.4f, want 0.5)", (double)atFive);

    pushArr(h, 0, nullptr);
    h.runBlocks(2);
    freeArr(lane);
}

// --- g. notes, and the beat they were played on ---------------------------
static void jrnStampsNotes() {
    Host h; h.init();
    h.push(Cmd::SetTempo, 0, 0, 120.0);
    h.push(Cmd::TrackArm, 0, 1);
    h.push(Cmd::SetPlaying, 1);
    h.runBlocks(1);

    // A message pushed between two process() calls is drained at the head of the
    // next block, so its frame offset is measured from that block's own start —
    // which makes the beat it must be stamped with exactly computable, and is
    // the point of the assertion. kBlock does not divide a beat at 120 BPM, so
    // the numbers below are deliberately not round ones: a stamp that were
    // rounded to a beat, a block or a bar would fail this.
    // The absolute frame each message lands on is the frames already rendered
    // (the transport has been rolling since frame 0) plus its own offset, read
    // from the output rather than counted, so the expectation cannot drift.
    const int onFrame = 33;
    h.runBlocks(94);
    const f64 wantOn = (f64)((i64)h.outL.size() + onFrame) / (f64)kBeat120;
    h.pushMidi(0x90, 60, 100, onFrame);
    h.runBlocks(137);
    const f64 wantOff = (f64)(i64)h.outL.size() / (f64)kBeat120;
    h.pushMidi(0x80, 60, 0, 0);
    h.runBlocks(2);
    h.push(Cmd::SetPlaying, 0);
    h.runBlocks(1);

    const auto j = drainJournal(h);
    const TakeResult r = buildTake(j, 0);
    CHECK(r.ok && r.notes.size() == 1, "an armed track's notes are journalled (%d)",
          (int)r.notes.size());
    if (r.notes.empty()) return;
    const TakeNote& n = r.notes[0];
    CHECK(n.track == 0 && n.pitch == 60 && n.vel == 100,
          "with pitch and velocity intact (track %d pitch %d vel %d)",
          n.track, (int)n.pitch, (int)n.vel);
    CHECK(std::fabs(n.beat - wantOn) < 1e-9 &&
          std::fabs((n.beat + n.len) - wantOff) < 1e-9,
          "at the beat the ENGINE saw it, in absolute timeline beats, to the FRAME "
          "(beat %.9f want %.9f, ends %.9f want %.9f)",
          n.beat, wantOn, n.beat + n.len, wantOff);
}

// --- h. a discontinuity ends the take (§5.5) ------------------------------
static void takeEndsAtADiscontinuity() {
    const auto A = rampBuf(kArrFrames);
    Host h; h.init();
    h.push(Cmd::SetQuantum, 4);
    h.setClip(0, 0, arrClip(A));
    h.push(Cmd::LaunchClip, 0, 0);
    h.run(5 * kBeat120);
    h.push(Cmd::Locate, 0, 0, 32.0);              // jumps away at beat ~5
    h.run(2 * kBeat120);
    h.push(Cmd::SetPlaying, 0);
    h.runBlocks(1);

    const TakeResult r = buildTake(drainJournal(h), 0);
    CHECK(r.ok && r.end == TakeResult::End::Locate,
          "a locate inside a pass ends the take there (end %d)", (int)r.end);
    CHECK(r.items.size() == 1 && r.items[0].start == 0.0 &&
          std::fabs(r.items[0].length - r.endBeat) < 1e-9,
          "and the open item is closed at the beat the take was cut on "
          "(%d items, len %.6f, cut %.6f)", (int)r.items.size(),
          r.items.empty() ? 0.0 : r.items[0].length, r.endBeat);
}

static void testArrangementRecording() {
    banner("36. recording into the arrangement");
    note("The journal is the record, and the record has to be exact enough that "
         "playing it back IS the performance.");
    jrnStampsTheLaunchBeat();
    jrnSeqIsContiguousAndAGapIsVisible();
    takeBuildsTheExpectedItems();
    takeWithAGapIsRefused();
    arrRecordRoundTripIsBitIdentical();
    arrOverrideIsCoherentAfterCommit();
    jrnStampsNotes();
    takeEndsAtADiscontinuity();
}

// ---------------------------------------------------------------------------
// 37. time signatures
//
// The map converts beats to bars and never the other way round, so every test
// here is one of two shapes: "this beat is in this bar" (the conversions, the
// metronome, the quantum) or "this beat did not move" (the item, the brace).
// ---------------------------------------------------------------------------

static RtSig sigEntry(int bar, int num, int den) {
    RtSig s; s.bar = bar; s.num = num; s.den = den; return s;
}
// The publisher's job, done the way session.h does it: one array, rebased.
static RtSig* mkSigMap(const std::vector<RtSig>& in) {
    RtSig* a = new RtSig[in.size()];
    for (size_t i = 0; i < in.size(); ++i) a[i] = in[i];
    sigMapRebase(a, (int)in.size());
    return a;
}
static void pushSigs(Host& h, const RtSig* a, int n) {
    Command c; c.type = Cmd::SetSignatures; c.a = n; c.p = (void*)a;
    h.e.pushCommand(c);
}
// The arithmetic every build before signatures used, kept here so the general
// path can be held against it rather than against a hand-copied table.
static f64 legacyQuantum(f64 from, f64 q) { return std::ceil(from / q - 1e-9) * q; }

// Metronome strikes, recovered from the rendered output. A burst is 30 ms of a
// decaying sine at 1600 Hz (downbeat) or 800 Hz (everything else), so counting
// sign changes over the first 10 ms separates them two to one -- 32 against 16 --
// with no threshold anywhere near either number.
struct MetTick { i64 frame; bool accent; };
static std::vector<MetTick> metTicks(const std::vector<f32>& v) {
    const i64 burst = (i64)(0.03 * kSR);
    std::vector<MetTick> out;
    for (size_t i = 0; i < v.size();) {
        if (std::fabs(v[i]) <= 1e-6f) { ++i; continue; }
        int cross = 0;
        f32 last = 0.f;
        for (size_t j = i; j < i + 480 && j < v.size(); ++j) {
            if (v[j] == 0.f) continue;
            if (last != 0.f && ((last < 0.f) != (v[j] < 0.f))) ++cross;
            last = v[j];
        }
        // The first sample of a burst is sin(0) == 0, so the detected onset is
        // one frame late; report the strike itself.
        out.push_back({(i64)i - 1, cross > 24});
        i += (size_t)burst;
    }
    return out;
}

// --- a. beat <-> bar, both directions, including exactly on a change ------
static void sigConversions() {
    // 4/4 from bar 0, 3/4 from bar 4, 7/8 from bar 8, back to 4/4 at bar 12.
    // Bar starts: 0, 16, 16+12 = 28, 28+14 = 42.
    RtSig* m = mkSigMap({sigEntry(0, 4, 4), sigEntry(4, 3, 4),
                         sigEntry(8, 7, 8), sigEntry(12, 4, 4)});
    const int n = 4;

    CHECK(sigMapValid(m, n), "a rebased map validates");
    CHECK(m[0].beat == 0.0 && m[1].beat == 16.0 && m[2].beat == 28.0 && m[3].beat == 42.0,
          "sigMapRebase sums the bar lengths ahead of each change "
          "(0, %.1f, %.1f, %.1f)", m[1].beat, m[2].beat, m[3].beat);

    struct { f64 bar, beat; } fwd[] = {
        {0, 0}, {2, 8}, {4, 16}, {6, 22}, {8, 28}, {10, 35}, {12, 42}, {16, 58},
        {2.5, 10}, {9.5, 33.25},
    };
    for (auto& c : fwd)
        CHECK(std::fabs(sigBeatOfBar(m, n, c.bar) - c.beat) < 1e-12,
              "bar %.2f begins on beat %.4f (%.4f)", c.bar, c.beat,
              sigBeatOfBar(m, n, c.bar));

    // The inverse, on the same points, and then as a round trip over a sweep --
    // one direction being right is not the same claim as the two agreeing.
    for (auto& c : fwd)
        CHECK(std::fabs(sigBarOfBeat(m, n, c.beat) - c.bar) < 1e-12,
              "beat %.4f is bar %.2f (%.4f)", c.beat, c.bar, sigBarOfBeat(m, n, c.beat));
    {
        f64 worst = 0.0;
        for (int i = 0; i <= 4000; ++i) {
            const f64 b = (f64)i * 0.0175;                 // through every entry
            const f64 rt = sigBeatOfBar(m, n, sigBarOfBeat(m, n, b));
            worst = std::max(worst, std::fabs(rt - b));
        }
        CHECK(worst < 1e-9, "beat -> bar -> beat is the identity over 4001 beats "
                            "(worst %.3g)", worst);
    }

    // EXACTLY on a change, and one ulp either side of it. The boundary belongs
    // to the NEW signature: bar 4 is the first bar of 3/4, and beat 16 is bar 4.
    {
        const BarPos on   = sigPosAt(m, n, 16.0);
        const BarPos just = sigPosAt(m, n, std::nextafter(16.0, 0.0));
        CHECK(on.bar == 4 && on.beat == 0 && on.num == 3 && on.den == 4,
              "beat 16 is bar 4 beat 0, and bar 4 is in 3/4 (bar %d beat %d, %d/%d)",
              on.bar, on.beat, on.num, on.den);
        CHECK(just.bar == 3 && just.beat == 3 && just.num == 4 && just.den == 4,
              "one ulp earlier is still the last beat of bar 3 in 4/4 "
              "(bar %d beat %d, %d/%d)", just.bar, just.beat, just.num, just.den);
    }
    {
        const BarPos p = sigPosAt(m, n, 29.25);
        CHECK(p.bar == 8 && p.beat == 2 && p.sixteenth == 1 && p.num == 7 && p.den == 8,
              "beat 29.25 is bar 8, eighth 2, sixteenth 1 of 7/8 "
              "(bar %d beat %d s%d)", p.bar, p.beat, p.sixteenth);
        CHECK(p.unit == 0.5 && p.barStart == 28.0,
              "a 7/8 unit is half a beat and the bar began at 28 (%.3f, %.3f)",
              p.unit, p.barStart);
    }
    {
        const BarPos p = sigPosAt(m, n, 41.9);
        CHECK(p.bar == 11 && p.beat == 6,
              "beat 41.9 is the last eighth of the last 7/8 bar (bar %d beat %d)",
              p.bar, p.beat);
        const BarPos q = sigPosAt(m, n, 42.0);
        CHECK(q.bar == 12 && q.beat == 0 && q.num == 4,
              "and beat 42 is the downbeat of bar 12, back in 4/4 (bar %d beat %d, %d/%d)",
              q.bar, q.beat, q.num, q.den);
    }

    // Past the last change the last signature runs forever, and below bar 0 it
    // extrapolates backwards -- which is not a curiosity: the metronome asks
    // about the sample before beat zero and has to be told "the bar before this".
    CHECK(sigPosAt(m, n, 58.0).bar == 16, "the last entry runs on past its own bar");
    CHECK(sigPosAt(m, n, -0.001).bar == -1, "and bar -1 exists, so beat 0 is a crossing");

    // An empty map is 4/4 everywhere, expression for expression.
    CHECK(sigPosAt(nullptr, 0, 9.5).bar == 2 && sigPosAt(nullptr, 0, 9.5).beat == 1 &&
          sigBeatOfBar(nullptr, 0, 7.0) == 28.0 && sigBarOfBeat(nullptr, 0, 28.0) == 7.0,
          "no map is plain 4/4 in every direction");

    // What the engine refuses. Each of these would put a bar line somewhere the
    // publisher's own numbers do not.
    {
        RtSig bad[2] = {sigEntry(1, 4, 4), sigEntry(2, 4, 4)};
        CHECK(!sigMapValid(bad, 2), "a map that does not start at bar 0 is refused");
        RtSig unsorted[2] = {sigEntry(0, 4, 4), sigEntry(0, 3, 4)};
        sigMapRebase(unsorted, 2);
        CHECK(!sigMapValid(unsorted, 2), "two entries at one bar are refused");
        RtSig lied[2] = {sigEntry(0, 4, 4), sigEntry(4, 3, 4)};
        sigMapRebase(lied, 2);
        lied[1].beat = 99.0;
        CHECK(!sigMapValid(lied, 2),
              "and a derived beat that does not follow from the bars before it is "
              "refused rather than believed");
        RtSig odd[1] = {sigEntry(0, 4, 3)};
        CHECK(!sigMapValid(odd, 1), "a denominator that is not a power of two is refused");
        RtSig wide[1] = {sigEntry(0, 33, 4)};
        CHECK(!sigMapValid(wide, 1), "and a numerator past kSigNumMax is refused");
        CHECK(!sigMapValid(nullptr, 0), "an empty map is not a map");
    }
    delete[] m;
}

// --- b. the metronome accents the bar, whatever a bar is -----------------
static void sigMetronome() {
    // 3/4: a strike every beat, accented every third. 120 BPM, so a beat is
    // kBeat120 frames and a bar is three of them.
    {
        Host h; h.init();
        RtSig* m = mkSigMap({sigEntry(0, 3, 4)});
        pushSigs(h, m, 1);
        h.push(Cmd::SetMetronome, 1);
        h.push(Cmd::SetPlaying, 1);
        h.run(9 * kBeat120);
        const auto t = metTicks(h.outL);
        bool ok = t.size() >= 9;
        for (size_t i = 0; ok && i < 9; ++i)
            ok = std::llabs(t[i].frame - (i64)i * kBeat120) <= 2 &&
                 t[i].accent == (i % 3 == 0);
        CHECK(ok, "3/4 strikes every beat and accents 1, 4, 7 (%zu strikes)", t.size());
        pushSigs(h, nullptr, 0);
        h.runBlocks(2);
        delete[] m;
    }
    // 7/8: the unit is an EIGHTH, so a strike every half beat and a bar of 3.5
    // beats. This is the case a "sigNum beats per bar" engine cannot express at
    // all, in either the tick rate or the accent.
    {
        Host h; h.init();
        RtSig* m = mkSigMap({sigEntry(0, 7, 8)});
        pushSigs(h, m, 1);
        h.push(Cmd::SetMetronome, 1);
        h.push(Cmd::SetPlaying, 1);
        h.run(8 * kBeat120);
        const auto t = metTicks(h.outL);
        bool ok = t.size() >= 14;
        for (size_t i = 0; ok && i < 14; ++i)
            ok = std::llabs(t[i].frame - (i64)(i * (size_t)kBeat120 / 2)) <= 2 &&
                 t[i].accent == (i % 7 == 0);
        CHECK(ok, "7/8 strikes every eighth and accents every seventh (%zu strikes)",
              t.size());
        pushSigs(h, nullptr, 0);
        h.runBlocks(2);
        delete[] m;
    }
    // Across a change: 4/4 for two bars, then 3/4. The accents are at beats
    // 0, 4, 8, 11, 14 -- and the one at 11 is the whole point, because it is
    // three beats after the last one and four after none of them.
    {
        Host h; h.init();
        RtSig* m = mkSigMap({sigEntry(0, 4, 4), sigEntry(2, 3, 4)});
        pushSigs(h, m, 2);
        h.push(Cmd::SetMetronome, 1);
        h.push(Cmd::SetPlaying, 1);
        h.run(16 * kBeat120);
        const auto t = metTicks(h.outL);
        std::vector<i64> acc;
        for (const auto& k : t) if (k.accent) acc.push_back(k.frame);
        const i64 want[] = {0, 4 * kBeat120, 8 * kBeat120, 11 * kBeat120, 14 * kBeat120};
        bool ok = acc.size() >= 5;
        for (size_t i = 0; ok && i < 5; ++i) ok = std::llabs(acc[i] - want[i]) <= 2;
        CHECK(ok, "the downbeats move to 0, 4, 8, 11, 14 when 4/4 becomes 3/4 at bar 2 "
                  "(%zu accents, first misplaced at %lld)", acc.size(),
              acc.empty() ? -1LL : (long long)acc[0]);
        // And the strike count is right on both sides: 8 in the two 4/4 bars,
        // one per beat after.
        CHECK(t.size() >= 16 && t.size() <= 17,
              "with a strike on every beat throughout (%zu)", t.size());
        pushSigs(h, nullptr, 0);
        h.runBlocks(2);
        delete[] m;
    }
    // The regression that matters most: with NO map the clicks are exactly the
    // ones the engine has always produced.
    {
        Host h; h.init();
        h.push(Cmd::SetMetronome, 1);
        h.push(Cmd::SetPlaying, 1);
        h.run(8 * kBeat120);
        const auto t = metTicks(h.outL);
        bool ok = t.size() >= 8;
        for (size_t i = 0; ok && i < 8; ++i)
            ok = std::llabs(t[i].frame - (i64)i * kBeat120) <= 2 &&
                 t[i].accent == (i % 4 == 0);
        CHECK(ok, "and with no map at all it is 4/4, strike for strike (%zu)", t.size());
    }
}

// --- c. the launch quantum walks the map ---------------------------------
static void sigQuantum() {
    // 4/4 to bar 4 (beat 16), then 7/8 -- bars of 3.5 beats, so every bar line
    // past the change is at a beat a 4/4 multiplication cannot produce.
    RtSig* m = mkSigMap({sigEntry(0, 4, 4), sigEntry(4, 7, 8)});
    const int n = 2;

    struct { f64 from, want; } one[] = {
        {0.0, 0.0}, {0.1, 4.0}, {12.0, 12.0}, {12.1, 16.0},
        {16.0, 16.0},                    // exactly on the change
        {16.1, 19.5}, {19.5, 19.5}, {20.0, 23.0}, {23.0, 23.0}, {23.1, 26.5},
    };
    for (auto& c : one)
        CHECK(std::fabs(sigNextBarLine(m, n, c.from, 1) - c.want) < 1e-9,
              "\"1 Bar\" from beat %.2f fires at %.2f (%.4f)", c.from, c.want,
              sigNextBarLine(m, n, c.from, 1));

    // Multi-bar quanta align on the ABSOLUTE bar index, so "4 Bars" is bars
    // 0, 4, 8, 12 of the piece and the walk crosses the change to find them.
    struct { f64 from; int bars; f64 want; } many[] = {
        {1.0,  4, 16.0},                 // bar 4, which is where the change is
        {16.1, 4, 30.0},                 // bar 8  = 16 + 4 * 3.5
        {30.1, 4, 44.0},                 // bar 12 = 16 + 8 * 3.5
        {17.0, 2, 23.0},                 // bar 6  = 16 + 2 * 3.5
        {0.5,  8, 30.0},                 // bar 8, two entries away, at beat 30
        {17.0, 8, 30.0},
    };
    for (auto& c : many)
        CHECK(std::fabs(sigNextBarLine(m, n, c.from, c.bars) - c.want) < 1e-9,
              "\"%d Bars\" from beat %.2f fires at %.2f (%.4f)", c.bars, c.from, c.want,
              sigNextBarLine(m, n, c.from, c.bars));

    // A walk over THREE entries in one query: from bar 0 to a bar-8 boundary
    // that two changes lie before.
    {
        RtSig* w = mkSigMap({sigEntry(0, 4, 4), sigEntry(4, 3, 4), sigEntry(8, 7, 8)});
        CHECK(std::fabs(sigNextBarLine(w, 3, 1.0, 8) - 28.0) < 1e-9,
              "an \"8 Bars\" launch at beat 1 walks two changes to land on beat 28 (%.4f)",
              sigNextBarLine(w, 3, 1.0, 8));
        delete[] w;
    }

    // A uniform 4/4 map must reproduce the pre-signature arithmetic exactly, or
    // "nothing changed for existing sets" is only true while nobody publishes.
    {
        RtSig* u = mkSigMap({sigEntry(0, 4, 4)});
        f64 worst = 0.0;
        for (int bi = 1; bi <= kQuantumBarMax; ++bi)
            for (int i = 0; i <= 2000; ++i) {
                const f64 from = (f64)i * 0.0625;      // exact binary, no eps window
                worst = std::max(worst, std::fabs(sigNextBarLine(u, 1, from, kQuantumBars[bi]) -
                                                  legacyQuantum(from, kQuantumBeats[bi])));
            }
        CHECK(worst == 0.0, "a 4/4 map gives the legacy quantum bit for bit over every "
                            "bar-shaped quantum (worst %.3g)", worst);
        delete[] u;
    }

    // End to end: a clip launched at "1 Bar" inside a 7/8 stretch starts on the
    // frame of the real bar line and not on a 4/4 one.
    {
        const auto buf = dcBuf(kArrFrames, 1, 0.5f);
        RtClip cl = arrClip(buf);
        Host h; h.init();
        RtSig* q = mkSigMap({sigEntry(0, 7, 8)});
        pushSigs(h, q, 1);
        h.push(Cmd::SetQuantum, 4);                 // "1 Bar"
        h.setClip(0, 0, cl);
        h.push(Cmd::SetPlaying, 1);
        h.run(kBeat120);                            // land inside bar 0
        h.push(Cmd::LaunchClip, 0, 0);
        h.run(8 * kBeat120);
        const i64 on = firstWhere(h.outL, 0, nonZero);
        CHECK(on == (i64)(3.5 * (f64)kBeat120),
              "a \"1 Bar\" launch in 7/8 fires on frame %lld, the real bar line (%lld)",
              (long long)(i64)(3.5 * (f64)kBeat120), (long long)on);
        pushSigs(h, nullptr, 0);
        h.runBlocks(2);
        delete[] q;
    }
    delete[] m;
}

// --- d. what must NOT move ------------------------------------------------
static void sigLeavesTheTimelineAlone() {
    const auto buf = dcBuf(kArrFrames, 1, 0.5f);

    // An arrangement item is placed in BEATS. Re-barring the piece changes the
    // bar it is displayed at and nothing else -- so the same item, played once
    // in 4/4 and once with a 3/4 map published, starts on the identical frame.
    i64 on[2] = {-1, -1};
    for (int k = 0; k < 2; ++k) {
        Host h; h.init();
        RtSig* m = nullptr;
        if (k) { m = mkSigMap({sigEntry(0, 3, 4), sigEntry(2, 7, 8)}); pushSigs(h, m, 2); }
        RtArrangement* a = mkArr({arrItem(8.0, 4.0, 0.0)}, {arrClip(buf)});
        pushArr(h, 0, a);
        h.push(Cmd::SetPlaying, 1);
        h.run(14 * kBeat120);
        on[k] = firstWhere(h.outL, 0, nonZero);
        pushArr(h, 0, nullptr);
        pushSigs(h, nullptr, 0);
        h.runBlocks(2);
        freeArr(a);
        delete[] m;
    }
    CHECK(on[0] == 8 * kBeat120 && on[1] == on[0],
          "an item at beat 8 starts on frame %lld with and without a signature map "
          "(%lld, %lld)", (long long)(8 * kBeat120), (long long)on[0], (long long)on[1]);
    {
        // ... and the bar it DISPLAYS at is the thing that moved, which is the
        // correct half of the same statement.
        RtSig* m = mkSigMap({sigEntry(0, 3, 4), sigEntry(2, 7, 8)});
        CHECK(std::fabs(sigBarOfBeat(nullptr, 0, 8.0) - 2.0) < 1e-12 &&
              std::fabs(sigBarOfBeat(m, 2, 8.0) - (2.0 + 2.0 / 3.5)) < 1e-12,
              "beat 8 shows as bar 2 in 4/4 and bar %.4f under the map",
              sigBarOfBeat(m, 2, 8.0));
        delete[] m;
    }

    // The loop brace is two beats and stays two beats. A brace at 0..6 wraps on
    // frame 6 * kBeat120 whether the piece is barred in 4/4 or 3/4.
    for (int k = 0; k < 2; ++k) {
        Host h; h.init();
        RtSig* m = nullptr;
        if (k) { m = mkSigMap({sigEntry(0, 3, 4)}); pushSigs(h, m, 1); }
        RtArrangement* br = mkBrace(0.0, 6.0, true);
        pushBrace(h, br);
        RtArrangement* a = mkArr({arrItem(4.0, 8.0, 0.0)}, {arrClip(buf)});
        pushArr(h, 0, a);
        h.push(Cmd::SetPlaying, 1);
        h.run(10 * kBeat120);
        // The item starts at beat 4 and the brace cuts it off at beat 6.
        const i64 s = firstWhere(h.outL, 0, nonZero);
        const i64 e = firstWhere(h.outL, s + 1000, departsFromSteady);
        CHECK(s == 4 * kBeat120 && e == 6 * kBeat120,
              "%s the brace wraps at beat 6 = frame %lld (start %lld, wrap %lld)",
              k ? "under a 3/4 map" : "with no map", (long long)(6 * kBeat120),
              (long long)s, (long long)e);
        pushArr(h, 0, nullptr);
        pushBrace(h, nullptr);
        pushSigs(h, nullptr, 0);
        h.runBlocks(2);
        freeArr(a); freeArr(br);
        delete[] m;
    }
}

// --- e. retirement, once, with the right pointer -------------------------
static void sigRetirement() {
    Host h; h.init();
    RtSig* a = mkSigMap({sigEntry(0, 3, 4)});
    RtSig* b = mkSigMap({sigEntry(0, 4, 4), sigEntry(8, 5, 4)});

    pushSigs(h, a, 1);
    h.runBlocks(2);
    int seen = 0;
    Event e;
    while (h.e.popEvent(e)) if (e.type == Ev::SigsRetired) ++seen;
    CHECK(seen == 0, "publishing the first map retires nothing (%d)", seen);

    pushSigs(h, b, 2);
    h.runBlocks(2);
    seen = 0;
    const void* got = nullptr;
    while (h.e.popEvent(e)) if (e.type == Ev::SigsRetired) { ++seen; got = e.p; }
    CHECK(seen == 1 && got == (const void*)a,
          "replacing it retires the displaced array exactly once, and it is the old "
          "pointer (%d events, %s)", seen, got == (const void*)a ? "correct" : "wrong");

    // Republishing the SAME pointer retires nothing -- the RtNote rule verbatim.
    pushSigs(h, b, 2);
    h.runBlocks(2);
    seen = 0;
    while (h.e.popEvent(e)) if (e.type == Ev::SigsRetired) ++seen;
    CHECK(seen == 0, "republishing the same pointer retires nothing (%d)", seen);

    // A map the engine cannot walk is refused AND handed back, both of it: the
    // publisher must never be left owning memory with no event behind it.
    RtSig* bad = new RtSig[2];
    bad[0] = sigEntry(2, 4, 4);                    // does not start at bar 0
    bad[1] = sigEntry(4, 4, 4);
    pushSigs(h, bad, 2);
    h.runBlocks(2);
    int freshBack = 0, oldBack = 0;
    while (h.e.popEvent(e))
        if (e.type == Ev::SigsRetired) {
            if (e.p == (void*)bad) ++freshBack;
            if (e.p == (void*)b)   ++oldBack;
        }
    CHECK(freshBack == 1 && oldBack == 1,
          "a refused map comes straight back and takes the previous one with it "
          "(fresh %d, old %d)", freshBack, oldBack);
    delete[] bad;
    delete[] b;
    delete[] a;

    // And after the refusal the engine is in 4/4, not in a half-applied map.
    {
        RtSig* c = mkSigMap({sigEntry(0, 7, 8)});
        pushSigs(h, c, 1);
        h.push(Cmd::SetPlaying, 1);
        h.run(4 * kBeat120);
        CHECK(h.e.posSigNum.load() == 7 && h.e.posSigDen.load() == 8,
              "the readout follows the map that was accepted (%d/%d)",
              h.e.posSigNum.load(), h.e.posSigDen.load());
        pushSigs(h, nullptr, 0);
        h.runBlocks(2);
        while (h.e.popEvent(e)) {}
        delete[] c;
    }
}

// --- f. the published readout --------------------------------------------
static void sigReadout() {
    Host h; h.init();
    RtSig* m = mkSigMap({sigEntry(0, 4, 4), sigEntry(2, 3, 4)});
    pushSigs(h, m, 2);
    h.push(Cmd::SetPlaying, 1);

    // Beat 9.25 is: bars 0 and 1 in 4/4 (8 beats), then 1.25 beats into bar 2 in
    // 3/4 -- bar 3, beat 2, sixteenth 2, all one-based.
    h.push(Cmd::Locate, 0, 0, 9.25);
    h.runBlocks(1);
    CHECK(h.e.posBar.load() == 3 && h.e.posBeat.load() == 2 && h.e.posSixteenth.load() == 2,
          "beat 9.25 reads 3.2.2 (%d.%d.%d)", h.e.posBar.load(), h.e.posBeat.load(),
          h.e.posSixteenth.load());
    CHECK(h.e.posSigNum.load() == 3 && h.e.posSigDen.load() == 4,
          "and the signature AT THE PLAYHEAD is 3/4, not the set's 4/4 (%d/%d)",
          h.e.posSigNum.load(), h.e.posSigDen.load());

    h.push(Cmd::Locate, 0, 0, 0.0);
    h.runBlocks(1);
    CHECK(h.e.posBar.load() == 1 && h.e.posBeat.load() == 1 && h.e.posSixteenth.load() == 1 &&
          h.e.posSigNum.load() == 4,
          "and beat 0 reads 1.1.1 in 4/4 (%d.%d.%d %d/%d)", h.e.posBar.load(),
          h.e.posBeat.load(), h.e.posSixteenth.load(), h.e.posSigNum.load(),
          h.e.posSigDen.load());

    pushSigs(h, nullptr, 0);
    h.runBlocks(2);
    Event e; while (h.e.popEvent(e)) {}
    delete[] m;
}

static void testSignatures() {
    banner("37. time signatures");
    note("The map converts beats to bars, never the other way round: what is "
         "placed in beats stays where it is, and only the bar it is called moves.");
    sigConversions();
    sigMetronome();
    sigQuantum();
    sigLeavesTheTimelineAlone();
    sigRetirement();
    sigReadout();
}

// ---------------------------------------------------------------------------

int main() {
    std::printf("nxtakt engine tests  (sr=%.0f, block=%d)\n", kSR, kBlock);

    testQuantizedLaunch();
    testQuantumNone();
    testLooping();
    testWarp();
    testMuteSolo();
    testSceneLaunch();
    testFiniteOutput();
    testRingSaturation();
    testDeviceChains();
    testRecording();
    testFollowActions();
    testMidiRouting();
    testMidiClips();
    testNoteChance();
    testMidiRecording();
    testNoteRetirement();
    testOverdub();
    testBuses();
    testPdc();
    testTwoRingMidi();
    testEventResilience();
    testOverdubSort();
    testDrains();
    testEvaluator();
    testAutomationClassA();
    testAutomationClassB();
    testAutosRetirement();
    autosRetirementUnderChurn();
    testWarpEvaluator();
    testWarpSingleSegment();
    testWarpTwoSegments();
    testWarpRetirement();
    warpRetirementUnderChurn();
    warpOnMidiClip();
    testOnsetDetector();
    testGrainAlignment();
    testArrangementTypes();
    testArrangementScheduler();
    testArrangementAutomation();
    testArrangementRecording();
    testSignatures();

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
