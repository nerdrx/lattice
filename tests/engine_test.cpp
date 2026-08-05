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
    std::vector<f32> outL, outR;  // everything rendered so far
    int block = kBlock;

    void init(f64 sr = kSR, int blk = kBlock) {
        block = blk;
        e.prepare(sr, blk);
        bl.assign((size_t)blk, 0.f);
        br.assign((size_t)blk, 0.f);
        outL.clear(); outR.clear();
    }
    void push(Cmd t, i32 a = 0, i32 b = 0, f64 x = 0.0) {
        Command c; c.type = t; c.a = a; c.b = b; c.x = x;
        e.pushCommand(c);
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
    // Renders at least `frames` frames, block-aligned. Returns the frame index
    // just past what had already been rendered before the call.
    size_t run(i64 frames) {
        const size_t mark = outL.size();
        for (i64 done = 0; done < frames; done += block) {
            e.process(bl.data(), br.data(), block);
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
        h.e.process(bl.data(), br.data(), kBlock);
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
class FakeFx : public PluginInstance {
public:
    explicit FakeFx(f32 gain) : gain_(gain) {}

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
            for (int i = 0; i < nframes; ++i) out[c][i] = in[c][i] * gain_;
        }
    }

    int              paramCount() const override     { return 0; }
    const ParamInfo& paramInfo(int) const override   { static ParamInfo p; return p; }
    f32              getParam(int) const override    { return 0.f; }
    void             setParam(int, f32) override     {}
    const PluginDesc& desc() const override          { static PluginDesc d; return d; }
    void             setBypassed(bool b) override    { bypassed_ = b; }
    bool             bypassed() const override       { return bypassed_; }

private:
    f32  gain_ = 1.f;
    bool bypassed_ = false;
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

int main() {
    std::printf("lattice engine tests  (sr=%.0f, block=%d)\n", kSR, kBlock);

    testQuantizedLaunch();
    testQuantumNone();
    testLooping();
    testWarp();
    testMuteSolo();
    testSceneLaunch();
    testFiniteOutput();
    testRingSaturation();
    testDeviceChains();

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
