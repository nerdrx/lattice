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
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
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
    testMidiRecording();
    testNoteRetirement();
    testOverdub();
    testBuses();
    testPdc();
    testDrains();

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
