// Engine-daemon tests.
//
// Spawns a real ./build/latticed in --driver null mode, attaches to its control
// region with ipc::EngineClient, and exercises the whole boundary from the
// outside: version handshake, scalar commands, the polled state block, the
// refusal of every pointer-carrying command, engine death by SIGKILL, and clean
// shutdown by SIGTERM.
//
// Phase 2 added the sample pool, and with it the assertions that matter most
// here: a clip synthesised in this process, written into shared memory,
// published as an *offset*, launched by the engine in another process, and
// heard coming back out through the published meters. Plus the ownership
// inversion that makes the pool worth having — SIGKILL the engine and the
// samples are still there, because the pool belongs to this process.
//
// Nothing here links the engine, the GUI or any audio library — the client side
// of the protocol depends on libc alone and the test keeps it that way. The
// daemon is a separate process, which is the entire point.
//
//   g++ -std=c++20 -O2 -Wall -Wextra tests/daemon_test.cpp -o daemon_test -lrt -lpthread
//   (and ./build/latticed must exist — the Makefile makes it a dependency)
#include "../src/ipc/client.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lat;

// ---------------------------------------------------------------------------
// tiny check framework  (same shape as tests/engine_test.cpp)
// ---------------------------------------------------------------------------

static int gPass = 0, gFail = 0;

static void checkImpl(bool ok, int line, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (ok) { ++gPass; std::printf("  PASS  %s\n", msg); }
    else    { ++gFail; std::printf("  FAIL  %s   (daemon_test.cpp:%d)\n", msg, line); }
    std::fflush(stdout);
}
#define CHECK(cond, ...) checkImpl((cond), __LINE__, __VA_ARGS__)

static void banner(const char* s) { std::printf("\n== %s\n", s); std::fflush(stdout); }
static void note(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::printf("  note  %s\n", msg);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// process and region cleanup
// ---------------------------------------------------------------------------
//
// A test that dies early must not leave a daemon rendering forever or a region
// in /dev/shm; an orphan of either kind would make the *next* run take a
// recovery path and mask the bug that caused it.

static const char* gDaemonPath = "./build/latticed";
static char        gSession[64] = {};
static char        gRegion[128] = {};
static char        gPool[128]   = {};
static pid_t       gDaemons[8]  = {};
static int         gDaemonCount = 0;

static void trackDaemon(pid_t p) {
    if (p > 0 && gDaemonCount < (int)(sizeof gDaemons / sizeof gDaemons[0]))
        gDaemons[gDaemonCount++] = p;
}

static void cleanup() {
    for (int i = 0; i < gDaemonCount; ++i) {
        if (gDaemons[i] <= 0) continue;
        ::kill(gDaemons[i], SIGKILL);
        ::waitpid(gDaemons[i], nullptr, 0);
        gDaemons[i] = 0;
    }
    if (gRegion[0]) ipc::ShmRegion::forceUnlink(gRegion);
    // The pool is GUI-owned, so a test that dies mid-run is exactly the "GUI
    // crashed" case: nothing else will ever unlink it.
    if (gPool[0]) ipc::ShmRegion::forceUnlink(gPool);
}
static void fatalSignal(int sig) {
    cleanup();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
static void armCleanup() {
    std::atexit(cleanup);
    for (int s : {SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGPIPE}) ::signal(s, fatalSignal);
}

// Lattice regions currently in /dev/shm, excluding `allow` (a leading '/' is
// tolerated, since that is how region names are spelled everywhere else).
// Anything counted is also printed: a leaked region is a bug report, not a
// number.
static int countLatticeShm(const char* allow = nullptr) {
    const char* skip = (allow && *allow == '/') ? allow + 1 : allow;
    DIR* d = ::opendir("/dev/shm");
    if (!d) return -1;
    int n = 0;
    while (dirent* e = ::readdir(d)) {
        if (!std::strstr(e->d_name, "lattice")) continue;
        if (skip && !std::strcmp(e->d_name, skip)) continue;
        ++n;
        note("leftover /dev/shm/%s", e->d_name);
    }
    ::closedir(d);
    return n;
}

static bool shmExists(const char* name) {
    char path[256];
    std::snprintf(path, sizeof path, "/dev/shm/%s", (*name == '/') ? name + 1 : name);
    return ::access(path, F_OK) == 0;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void sleepMs(int ms) {
    timespec ts{ms / 1000, (long)(ms % 1000) * 1000000L};
    ::nanosleep(&ts, nullptr);
}

// Polls a predicate to a deadline. Every wait in this file goes through here so
// that a slow machine costs time rather than a failure: the assertions are
// about what the daemon converges to, never about how fast it gets there.
template <typename F>
static bool waitUntil(F pred, int timeoutMs, int pollMs = 1) {
    const u64 deadline = ipc::monotonicNs() + (u64)timeoutMs * 1000000ull;
    for (;;) {
        if (pred()) return true;
        if (ipc::monotonicNs() >= deadline) return false;
        sleepMs(pollMs);
    }
}

static pid_t spawnDaemon(const char* session) {
    const char* args[] = {"--driver", "null", "--session", session, nullptr};
    const pid_t p = ipc::EngineClient::spawnDaemon(gDaemonPath, args);
    trackDaemon(p);
    return p;
}

static void drainEvents(ipc::EngineClient& c, std::vector<ipc::WireEvent>* into = nullptr) {
    ipc::WireEvent e;
    while (c.popEvent(e)) if (into) into->push_back(e);
}

static int countEvents(const std::vector<ipc::WireEvent>& v, u32 type) {
    int n = 0;
    for (const ipc::WireEvent& e : v) if (e.type == type) ++n;
    return n;
}

static const ipc::WireEvent* findReject(const std::vector<ipc::WireEvent>& v, Cmd forCmd) {
    for (const ipc::WireEvent& e : v)
        if (e.type == ipc::EvCommandRejected && e.a == (i32)forCmd) return &e;
    return nullptr;
}

// The peak master meter over `ms`, sampled fast enough to catch a metronome
// click: the meter decays 0.72 per block, so a click is only a handful of
// blocks wide and a lazy poll would miss it.
static f32 peakMaster(ipc::EngineClient& c, int ms) {
    f32 peak = 0.f;
    const u64 deadline = ipc::monotonicNs() + (u64)ms * 1000000ull;
    do {
        const f32 l = c.state().masterMeterL.load(std::memory_order_relaxed);
        const f32 r = c.state().masterMeterR.load(std::memory_order_relaxed);
        peak = std::max(peak, std::max(l, r));
        sleepMs(1);
    } while (ipc::monotonicNs() < deadline);
    return peak;
}

static f32 peakTrack(ipc::EngineClient& c, int track, int ms) {
    f32 peak = 0.f;
    const u64 deadline = ipc::monotonicNs() + (u64)ms * 1000000ull;
    do {
        const f32 l = c.state().meterL[track].load(std::memory_order_relaxed);
        const f32 r = c.state().meterR[track].load(std::memory_order_relaxed);
        peak = std::max(peak, std::max(l, r));
        sleepMs(1);
    } while (ipc::monotonicNs() < deadline);
    return peak;
}

// The same, after letting the meter come down. peakTrack() is a peak *hold*
// over its window, and Engine's meter decays 0.72 per block, so measuring a
// downward change immediately reports the value from before it: the first
// sample of the window is still the old peak. Anything asserting "the level
// dropped" has to wait out the decay first — about three blocks — or it is
// asserting on history.
static f32 settledPeak(ipc::EngineClient& c, int track, int ms) {
    sleepMs(60);
    return peakTrack(c, track, ms);
}


// ---------------------------------------------------------------------------
// clip helpers
// ---------------------------------------------------------------------------

// The events the pool protocol answers with. Every clip publication gets
// exactly one EvClipAck; a retirement gets one EvBlockRetired. Draining is not
// optional in this test — EngineClient::popEvent() is where the client-side
// bookkeeping happens (a cell unblocks, a block becomes freeable), so a section
// that stopped draining would wedge itself.
static bool waitClipIdle(ipc::EngineClient& c, int track, int slot, int timeoutMs = 2000) {
    return waitUntil([&] { drainEvents(c); return !c.clipBusy(track, slot); }, timeoutMs);
}

static bool waitRetired(ipc::EngineClient& c, u64 ref, int timeoutMs = 3000) {
    return waitUntil([&] {
        drainEvents(c);
        return c.pool().stateOf(ref) != ipc::BlockRetiring;
    }, timeoutMs);
}

// A DC clip: every sample the same value. Deliberately the least musical
// signal there is, because it makes the meter a *measurement* — a DC clip at
// 0.5 through unity gain has to publish a peak of 0.5, so the assertion is an
// equality with a tolerance rather than "something happened".
static std::vector<f32> makeDc(i64 frames, int channels, f32 level) {
    std::vector<f32> v((size_t)frames * (size_t)channels, level);
    return v;
}

// An ascending run of notes. Built one at a time and pushed rather than sized
// and indexed, because the latter lets gcc merge the two u8 stores into one
// 16-bit store it then cannot prove is in bounds (-Wstringop-overflow); this
// spelling is also the one a real note editor would use.
static std::vector<ipc::WireNote> makeNotes(int count, int firstPitch, f64 step, f64 len) {
    std::vector<ipc::WireNote> v;
    v.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        ipc::WireNote n{};
        n.beat  = step * i;
        n.len   = len;
        n.pitch = (u8)(firstPitch + i);
        n.vel   = 100;
        v.push_back(n);
    }
    return v;
}

// Fills in the fields every clip in this file shares. Warp::Off on purpose:
// the warp modes resample, and while DC survives resampling exactly, "the
// meter reads the level" should not depend on that being true.
static ipc::WireClip audioClip(u64 ref, i64 frames, int channels) {
    ipc::WireClip c = ipc::defaultWireClip();
    c.sampleRef   = ref;
    c.frames      = frames;
    c.channels    = channels;
    c.loopStart   = 0;
    c.loopEnd     = frames;
    c.warp        = (i32)Warp::Off;
    c.loop        = 1;
    c.quantumIdx  = 0;            // launch now, do not wait for the bar line
    c.lengthBeats = 4.0;
    c.gain        = 1.0f;
    c.valid       = 1;
    return c;
}

// Puts track 0 back to unity and audible. Section 4 leaves a mute on track 0
// and a solo on track 2, both of which would silence everything here — and a
// meter test that silently measured a muted track would pass for the wrong
// reason on the day the clip stopped playing.
static void resetMixer(ipc::EngineClient& c) {
    c.pushCommand(Cmd::TrackMute, 0, 0);
    c.pushCommand(Cmd::TrackSolo, 2, 0);
    c.pushCommand(Cmd::TrackArm,  3, 0);
    c.pushCommand(Cmd::TrackVol,  0, 0, 1.0);
    c.pushCommand(Cmd::TrackVol,  1, 0, 1.0);
    c.pushCommand(Cmd::TrackPan,  1, 0, 0.0);
    c.pushCommand(Cmd::MasterVol, 0, 0, 1.0);
}

// ---------------------------------------------------------------------------
// 1. spawn, attach, handshake
// ---------------------------------------------------------------------------

static bool testHandshake(ipc::EngineClient& c, pid_t& daemon) {
    banner("1. spawn latticed --driver null and shake hands");

    daemon = spawnDaemon(gSession);
    CHECK(daemon > 0, "fork/exec %s (pid %d)", gDaemonPath, (int)daemon);
    if (daemon <= 0) return false;

    const bool up = c.attach(gSession, 5000);
    CHECK(up, "attach to session '%s'%s%s", gSession, up ? "" : ": ", up ? "" : c.error());
    if (!up) return false;

    // Layer 1 (magic/version/layout) is inside ShmRegion::attach and has
    // already run; layers 2 and 3 are these.
    CHECK(c.header().protocolVersion == ipc::kProtocolVersion,
          "protocol handshake: region v%u == build v%u",
          c.header().protocolVersion, ipc::kProtocolVersion);
    CHECK(c.enginePid() == daemon,
          "the region's creator is the daemon we spawned (%d vs %d)",
          c.enginePid(), (int)daemon);
    CHECK(c.header().daemonPid == daemon, "ControlHeader agrees about the pid (%d)",
          c.header().daemonPid);
    CHECK(c.header().driverIsNull == 1 && !std::strcmp(c.header().driverName, "null"),
          "the daemon reports the null driver ('%s')", c.header().driverName);
    CHECK(std::fabs(c.sampleRate() - 48000.0) < 1e-9 && c.blockSize() == 256,
          "audio format published before any command: %.0f Hz / %u frames",
          c.sampleRate(), c.blockSize());
    CHECK(c.state().engineState.load() == ipc::SharedState::StateRunning,
          "engineState is Running (%u)", c.state().engineState.load());
    CHECK(c.alive(), "alive() right after the handshake");

    const u64 h0 = c.heartbeat();
    const u64 g0 = c.state().generation.load();
    const bool beating = waitUntil([&] {
        return c.heartbeat() > h0 && c.state().generation.load() > g0;
    }, 500);
    CHECK(beating, "heartbeat and state generation advance (%llu -> %llu, %llu -> %llu)",
          (unsigned long long)h0, (unsigned long long)c.heartbeat(),
          (unsigned long long)g0, (unsigned long long)c.state().generation.load());

    // A wrong-version peer must be refused, not misread. Faking a mismatch
    // without a second build means poking the region's own header; the client
    // reads it back through the same path a stale binary would.
    note("layout hash 0x%08x, region %zu B, %u-slot command ring",
         ipc::control::kHash, ipc::control::kBytes, ipc::CommandRing::capacity());
    return true;
}

// ---------------------------------------------------------------------------
// 2. transport: the beat clock advances at the tempo, in wall-clock time
// ---------------------------------------------------------------------------

struct BeatSample { f64 beat; u64 ns; };

// Samples the beat immediately after a mirror publish, so the value is at most
// one audio block old. The clock is read either side of the beat load and the
// midpoint taken, so a deschedule between the two reads costs half its length
// instead of all of it.
static BeatSample sampleBeat(const ipc::SharedState& s) {
    const u64 g = s.generation.load(std::memory_order_acquire);
    for (int i = 0; i < 2000 && s.generation.load(std::memory_order_acquire) == g; ++i)
        sleepMs(1);
    const u64 t0 = ipc::monotonicNs();
    const f64 b  = s.beat.load(std::memory_order_relaxed);
    const u64 t1 = ipc::monotonicNs();
    return {b, t0 + (t1 - t0) / 2};
}

static void testTransport(ipc::EngineClient& c) {
    banner("2. transport: SetTempo + SetPlaying advance the beat at the right rate");
    note("the null driver renders 256-frame blocks against CLOCK_MONOTONIC, so");
    note("beats/second must equal tempo/60 in wall-clock time, jitter aside.");

    CHECK(c.pushCommand(Cmd::SetTempo, 0, 0, 133.0), "push SetTempo 133");
    CHECK(c.pushCommand(Cmd::SetPlaying, 1), "push SetPlaying 1");

    const bool got = waitUntil([&] {
        return std::fabs(c.state().tempo.load() - 133.0) < 1e-9 &&
               c.state().playing.load() == 1;
    }, 1000);
    CHECK(got, "SharedState reflects tempo %.3f, playing %u",
          c.state().tempo.load(), c.state().playing.load());

    sleepMs(100);                                   // let the driver settle
    const BeatSample a = sampleBeat(c.state());
    sleepMs(600);
    const BeatSample b = sampleBeat(c.state());

    const f64 secs  = (f64)(b.ns - a.ns) / 1e9;
    const f64 rate  = (b.beat - a.beat) / secs;     // beats per second
    const f64 bpm   = rate * 60.0;
    const f64 err   = (bpm - 133.0) / 133.0 * 100.0;
    note("measured %.3f BPM over %.3f s (%.4f beats/s), error %+.2f %%", bpm, secs, rate, err);

    CHECK(b.beat > a.beat, "the beat advanced at all (%.4f -> %.4f)", a.beat, b.beat);
    CHECK(std::fabs(err) < 10.0,
          "beat clock tracks wall clock within 10%%: %.3f BPM (expected 133)", bpm);

    // Stopping is the other half of the round trip, and it is also the first
    // engine-to-GUI event of the run: Ev::TransportStopped is scalar, so unlike
    // the retirement events it crosses unchanged.
    drainEvents(c);
    const u64 forwarded0 = c.header().eventsForwarded.load();
    CHECK(c.pushCommand(Cmd::SetPlaying, 0), "push SetPlaying 0");
    const bool stopped = waitUntil([&] { return c.state().playing.load() == 0; }, 1000);
    CHECK(stopped, "SharedState reports the transport stopped");

    std::vector<ipc::WireEvent> evs;
    const bool sawStop = waitUntil([&] {
        drainEvents(c, &evs);
        return countEvents(evs, (u32)Ev::TransportStopped) > 0;
    }, 1000);
    CHECK(sawStop, "Ev::TransportStopped came back over the event ring (%d events)",
          (int)evs.size());
    CHECK(c.header().eventsForwarded.load() > forwarded0,
          "the daemon counted it as forwarded (%llu)",
          (unsigned long long)c.header().eventsForwarded.load());

    // The engine rewinds on stop, so "frozen" here means frozen at zero.
    const f64 frozen = c.state().beat.load();
    CHECK(std::fabs(frozen) < 1e-9, "stopping rewinds to beat 0 (%.4f)", frozen);
    sleepMs(120);
    CHECK(std::fabs(c.state().beat.load() - frozen) < 1e-9,
          "and the beat stays put while stopped (%.4f -> %.4f)",
          frozen, c.state().beat.load());
}

// ---------------------------------------------------------------------------
// 3. metronome and master volume, round-tripped through the audio path
// ---------------------------------------------------------------------------
//
// The metronome is the only sound a phase-1 daemon can make: clips cannot cross
// the boundary yet, so with the metronome off the master meter is the digital
// zero the engine started at. That makes SharedState::masterMeter a real
// end-to-end probe — command in, audio rendered, meter published, meter read
// from another process — rather than an echo of what we just sent.

static void testMetronomeAndMaster(ipc::EngineClient& c) {
    banner("3. metronome and master volume round-trip through the rendered audio");

    c.pushCommand(Cmd::SetTempo, 0, 0, 133.0);
    c.pushCommand(Cmd::SetMetronome, 1);
    c.pushCommand(Cmd::SetPlaying, 1);
    waitUntil([&] { return c.state().playing.load() == 1; }, 1000);

    // One beat at 133 BPM is 451 ms, so a 1.1 s window contains at least two.
    const f32 onPeak = peakMaster(c, 1100);
    CHECK(onPeak > 0.01f, "metronome on -> the master meter sees clicks (peak %.4f)",
          (double)onPeak);

    c.pushCommand(Cmd::SetMetronome, 0);
    sleepMs(200);                                   // the last click decays out
    const f32 offPeak = peakMaster(c, 700);
    CHECK(offPeak < 1e-3f, "metronome off -> silence again (peak %.3g)", (double)offPeak);

    // MasterVol is a scalar command whose effect is audible: with the metronome
    // back on and the master at zero, the meter must stay down.
    c.pushCommand(Cmd::SetMetronome, 1);
    c.pushCommand(Cmd::MasterVol, 0, 0, 0.0);
    sleepMs(200);
    const f32 mutedPeak = peakMaster(c, 700);
    CHECK(mutedPeak < 1e-3f, "MasterVol 0 silences the metronome (peak %.3g)",
          (double)mutedPeak);

    c.pushCommand(Cmd::MasterVol, 0, 0, 1.0);
    const f32 backPeak = peakMaster(c, 1100);
    CHECK(backPeak > 0.01f, "MasterVol 1 brings it back (peak %.4f)", (double)backPeak);

    c.pushCommand(Cmd::SetMetronome, 0);
    c.pushCommand(Cmd::SetPlaying, 0);
    waitUntil([&] { return c.state().playing.load() == 0; }, 1000);
}

// ---------------------------------------------------------------------------
// 4. the command boundary: scalars through, pointers refused
// ---------------------------------------------------------------------------

static void testCommandBoundary(ipc::EngineClient& c) {
    banner("4. scalar commands cross; pointer-carrying commands are refused");
    note("SetChain/RecordSlot/RecordMidiSlot still carry GUI-heap pointers, so");
    note("the daemon refuses them at the boundary with a reason rather than");
    note("half-translating them. SetClip and ClearClip left this list in phase 2");
    note("and are exercised against a real pool in sections 6-10.");

    drainEvents(c);
    const ipc::ControlHeader& h = c.header();
    const u64 applied0  = h.commandsApplied.load();
    const u64 rejected0 = h.commandsRejected.load();

    // -- the mixer scalars ---------------------------------------------------
    //
    // Engine::publish() does not publish vol/pan/mute/solo/arm, and phase 1
    // does not touch src/audio, so there is nothing in SharedState to read them
    // back from (see the "explicitly deferred" list in docs/PROCESS-SPLIT.md).
    // What *is* observable is the boundary's own accounting: these five were
    // accepted and handed to Engine::pushCommand, none was refused.
    const struct { Cmd t; i32 a, b; f64 x; } mixer[] = {
        {Cmd::TrackVol,  0, 0, 0.5},
        {Cmd::TrackMute, 0, 1, 0.0},
        {Cmd::TrackPan,  1, 0, -0.5},
        {Cmd::TrackSolo, 2, 1, 0.0},
        {Cmd::TrackArm,  3, 1, 0.0},
    };
    for (const auto& m : mixer) CHECK(c.pushCommand(m.t, m.a, m.b, m.x),
                                      "push command %u", (u32)m.t);
    const bool allApplied = waitUntil([&] {
        return h.commandsApplied.load() >= applied0 + 5;
    }, 1000);
    CHECK(allApplied, "all five mixer scalars reached the engine (%llu applied)",
          (unsigned long long)(h.commandsApplied.load() - applied0));
    CHECK(h.commandsRejected.load() == rejected0, "and none of them was refused");

    // -- the three that still cannot cross ----------------------------------
    const Cmd pointerCmds[] = {Cmd::SetChain, Cmd::RecordSlot, Cmd::RecordMidiSlot};
    const int kPointerCmds  = (int)(sizeof pointerCmds / sizeof pointerCmds[0]);
    const u64 applied1 = h.commandsApplied.load();
    for (Cmd t : pointerCmds) {
        ipc::WireCommand w{};
        w.type = (u32)t;
        w.a = 0; w.b = 0; w.x = 4.0;
        w.ref = 0xdeadbeefull;
        CHECK(c.pushCommand(w), "push pointer-carrying command %u", (u32)t);
    }
    const bool allRejected = waitUntil([&] {
        return h.commandsRejected.load() >= rejected0 + (u64)kPointerCmds;
    }, 1000);
    CHECK(allRejected, "all three were refused (%llu rejected)",
          (unsigned long long)(h.commandsRejected.load() - rejected0));
    CHECK(h.commandsApplied.load() == applied1,
          "and not one of them reached the engine (%llu applied since)",
          (unsigned long long)(h.commandsApplied.load() - applied1));

    std::vector<ipc::WireEvent> evs;
    waitUntil([&] {
        drainEvents(c, &evs);
        return countEvents(evs, ipc::EvCommandRejected) >= kPointerCmds;
    }, 1000);
    CHECK(countEvents(evs, ipc::EvCommandRejected) == kPointerCmds,
          "one EvCommandRejected per refusal (%d)", countEvents(evs, ipc::EvCommandRejected));
    for (Cmd t : pointerCmds) {
        const ipc::WireEvent* e = findReject(evs, t);
        CHECK(e && (u32)e->b == ipc::RejectPointerPayload,
              "command %u refused with reason %u (%s)", (u32)t, e ? (u32)e->b : 0u,
              ipc::rejectReasonName(e ? (u32)e->b : 0u));
        CHECK(e && e->ref == 0xdeadbeefull, "the refusal echoes the caller's ref back");
    }

    // -- a clip with no pool behind it --------------------------------------
    //
    // SetClip is legal now, but only against a pool the daemon has mapped, and
    // no pool exists yet. This is the first half of the "a bad offset never
    // becomes a pointer" property: the offset here is plausible — aligned,
    // small, positive — and it is still refused, because there is nothing to
    // resolve it against.
    evs.clear();
    drainEvents(c);
    {
        ipc::WireClip wc = audioClip(/*ref*/64 * 1024, /*frames*/1024, /*channels*/2);
        CHECK(c.setClip(0, 0, wc), "publish a clip cell that references a pool");
        CHECK(c.clipBusy(0, 0), "the cell is blocked until the daemon answers");
        const bool answered = waitUntil([&] {
            drainEvents(c, &evs);
            return countEvents(evs, ipc::EvClipAck) > 0;
        }, 1000);
        CHECK(answered, "an EvClipAck came back for it");
        const ipc::WireEvent* ack = nullptr;
        for (const ipc::WireEvent& e : evs) if (e.type == ipc::EvClipAck) ack = &e;
        CHECK(ack && (ack->flags & ipc::ClipAckRefused),
              "marked refused (flags 0x%x)", ack ? ack->flags : 0u);
        CHECK(ack && (u32)ack->x == ipc::RejectNoPool,
              "with reason %u (%s)", ack ? (u32)ack->x : 0u,
              ipc::rejectReasonName(ack ? (u32)ack->x : 0u));
        CHECK(!c.clipBusy(0, 0), "and the acknowledgement unblocks the cell for a retry");
        CHECK(c.clipShadow(0, 0).sampleRef == 0,
              "the client's shadow still says the slot is empty (%llu)",
              (unsigned long long)c.clipShadow(0, 0).sampleRef);
    }

    // -- garbage is refused too, and refusing is not fatal -------------------
    const u64 rejected2 = h.commandsRejected.load();
    ipc::WireCommand bad{};
    bad.type = 9999;
    c.pushCommand(bad);
    c.pushCommand(Cmd::TrackVol, kMaxTracks + 5, 0, 0.5);       // out-of-range track
    c.pushCommand(Cmd::SetTempo, 0, 0, std::nan(""));           // non-finite scalar
    const bool moreRejected = waitUntil([&] {
        return h.commandsRejected.load() >= rejected2 + 3;
    }, 1000);
    CHECK(moreRejected, "an unknown type, a wild track index and a NaN are all refused");

    evs.clear();
    drainEvents(c, &evs);
    bool sawUnknown = false, sawIndex = false, sawNaN = false;
    for (const ipc::WireEvent& e : evs) {
        if (e.type != ipc::EvCommandRejected) continue;
        if ((u32)e.b == ipc::RejectUnknownCommand) sawUnknown = true;
        if ((u32)e.b == ipc::RejectBadIndex)       sawIndex   = true;
        if ((u32)e.b == ipc::RejectNotFinite)      sawNaN     = true;
    }
    CHECK(sawUnknown && sawIndex && sawNaN,
          "each refusal names its own reason (unknown %d, index %d, NaN %d)",
          sawUnknown, sawIndex, sawNaN);

    // The whole point of refusing rather than crashing: the engine is still
    // there afterwards and still takes scalars.
    CHECK(c.pushCommand(Cmd::SetTempo, 0, 0, 96.0), "push SetTempo 96 after the refusals");
    const bool tempoTook = waitUntil([&] {
        return std::fabs(c.state().tempo.load() - 96.0) < 1e-9;
    }, 1000);
    CHECK(tempoTook, "the daemon survived and applied it (tempo %.3f)", c.state().tempo.load());
    CHECK(c.alive(), "and it is still alive");
    CHECK(c.header().eventsDropped.load() == 0,
          "no engine event had to be dropped at the boundary (%llu)",
          (unsigned long long)c.header().eventsDropped.load());

    // MIDI has its own ring and is scalar by construction, so it crosses.
    const u64 midi0 = h.midiApplied.load();
    CHECK(c.pushMidi(0x90, 60, 100, 0), "push a note-on through the MIDI ring");
    // The wait runs on its own line, never inside CHECK's condition: the order
    // of a condition and the arguments that report it is unspecified, and the
    // message would print the value from *before* the wait.
    const bool midiTook = waitUntil([&] { return h.midiApplied.load() > midi0; }, 1000);
    CHECK(midiTook, "the daemon forwarded it to Engine::pushMidi (%llu)",
          (unsigned long long)h.midiApplied.load());
}

// ---------------------------------------------------------------------------
// 5. burst: the boundary defers, it does not drop
// ---------------------------------------------------------------------------
//
// A process boundary makes bursts worse, not better — the client can be
// descheduled for a whole frame and then empty a scene launch into the ring at
// once. The wire ring holds 4095, but Engine's own ring holds 1023 and only
// drains once per audio block, so a big burst *must* back up somewhere. The
// contract is that it backs up rather than evaporates: the daemon parks the
// command it could not hand over and retries next tick.

static void testBurst(ipc::EngineClient& c) {
    banner("5. a command burst is deferred, never dropped");

    const ipc::ControlHeader& h = c.header();
    const u64 applied0  = h.commandsApplied.load();
    const u64 rejected0 = h.commandsRejected.load();
    const u64 deferred0 = h.commandsDeferred.load();

    // Three times Engine's ring capacity, ending on a value we can read back.
    const int kBurst = 3000;
    int pushed = 0;
    for (int i = 0; i < kBurst; ++i) {
        const f64 tempo = (i == kBurst - 1) ? 128.0 : 60.0 + (f64)(i % 100);
        if (!c.pushCommand(Cmd::SetTempo, 0, 0, tempo)) break;
        ++pushed;
    }
    CHECK(pushed == kBurst, "pushed %d commands into a %u-slot ring without a refusal",
          pushed, ipc::CommandRing::capacity());

    const bool allThrough = waitUntil([&] {
        return h.commandsApplied.load() >= applied0 + (u64)pushed;
    }, 5000);
    CHECK(allThrough, "every one of them reached the engine: %llu applied",
          (unsigned long long)(h.commandsApplied.load() - applied0));
    CHECK(h.commandsRejected.load() == rejected0, "none was refused");
    note("%llu had to be deferred past a full engine ring (0 just means the "
         "audio thread kept up)",
         (unsigned long long)(h.commandsDeferred.load() - deferred0));

    const bool landed = waitUntil([&] {
        return std::fabs(c.state().tempo.load() - 128.0) < 1e-9;
    }, 2000);
    CHECK(landed, "the last command in the burst is the one that stuck (tempo %.3f)",
          c.state().tempo.load());
}

// ---------------------------------------------------------------------------
// 6. the sample pool: create, publish, attach
// ---------------------------------------------------------------------------
//
// The pool is the one region the *client* owns. Everything downstream of here
// depends on that inversion holding, so it is asserted directly rather than
// inferred from clips working.

static constexpr size_t kTestPoolBytes = 16u << 20;   // 16 MiB, sparse

static void testPoolHandshake(ipc::EngineClient& c) {
    banner("6. the sample pool: the client creates it, the daemon maps it read-only");

    CHECK(c.createPool(gSession, kTestPoolBytes), "create %s: %s", gPool, c.error());
    CHECK(c.pool().valid(), "the pool is mapped in this process");
    CHECK(!std::strcmp(c.pool().name(), gPool), "under the session's name ('%s')",
          c.pool().name());
    CHECK(shmExists(gPool), "and it exists in /dev/shm");
    CHECK(c.pool().bytes() >= kTestPoolBytes, "%zu B of payload (asked for %zu)",
          c.pool().bytes(), kTestPoolBytes);
    note("F_SEAL_SHRINK on a shm_open object: %s (memfd + SCM_RIGHTS is the "
         "upgrade path, §3.2)", c.pool().sealed() ? "accepted" : "refused by the kernel");
    note("bump %llu, largest free %llu B", (unsigned long long)c.pool().bump(),
         (unsigned long long)c.pool().largestFree());

    const bool mapped = waitUntil([&] { drainEvents(c); return c.poolReady(); }, 3000);
    CHECK(mapped, "the daemon attached to it (epoch %llu, daemon says %llu)",
          (unsigned long long)c.poolEpoch(),
          (unsigned long long)c.header().poolAttachedEpoch.load());
    CHECK(c.header().poolAttachFailures.load() == 0,
          "with no failed attempts (%llu)",
          (unsigned long long)c.header().poolAttachFailures.load());

    // A second handle onto the same region: this is the §4.3 reattach path in
    // miniature, and it is what proves the allocator's metadata really is in
    // the region rather than in this object.
    {
        ipc::SamplePool second;
        const bool ok = second.attach(gPool);
        CHECK(ok, "a second handle attaches to the same pool%s%s", ok ? "" : ": ",
              ok ? "" : second.error());
        CHECK(ok && second.epoch() == c.poolEpoch(),
              "and reads the same epoch (%llu)", (unsigned long long)second.epoch());
        CHECK(ok && second.bump() == c.pool().bump(),
              "and the same allocator state (bump %llu)",
              (unsigned long long)second.bump());
    }

    // Layer 1 of the handshake applies to the pool exactly as it does to the
    // control region: a build that disagrees about the layout must be refused
    // rather than allowed to read blocks through the wrong offsets.
    {
        ipc::ShmRegion wrong;
        const bool got = wrong.attach(gPool, ipc::pool::kHash ^ 1u, ipc::kShmVersion, 0);
        CHECK(!got, "a mismatched layout hash is refused: %s", wrong.error());
        ipc::ShmRegion oldVer;
        const bool got2 = oldVer.attach(gPool, ipc::pool::kHash, ipc::kShmVersion + 1, 0);
        CHECK(!got2, "so is a mismatched shm version: %s", oldVer.error());
    }
}

// ---------------------------------------------------------------------------
// 7. an audio clip, end to end
// ---------------------------------------------------------------------------
//
// The headline of phase 2. A DC clip is synthesised here, memcpy'd into the
// pool, published as an offset, launched by a command, and rendered by an
// engine in another process — and because DC at 0.5 through unity gain is
// exactly 0.5 at the meter, the check at the end is a measurement and not a
// liveness test.

static u64 gAudioRef = 0;

static void testAudioClip(ipc::EngineClient& c) {
    banner("7. upload a DC clip, SetClip, LaunchClip, and hear it in the meters");

    resetMixer(c);

    const i64 kFrames = 24000;              // half a second at 48 kHz
    const f32 kLevel  = 0.5f;
    const std::vector<f32> dc = makeDc(kFrames, 2, kLevel);

    const u64 bump0 = c.pool().bump();
    const u64 ref = c.poolWrite(dc.data(), kFrames, 2, 48000.0, /*key*/0xC0FFEEull);
    gAudioRef = ref;
    CHECK(ref != 0, "poolWrite %lld frames x 2ch -> offset %llu: %s",
          (long long)kFrames, (unsigned long long)ref, ref ? "" : c.error());
    if (!ref) return;
    CHECK(ref % ipc::kPoolAlign == 0, "the offset is 64-byte aligned (%llu)",
          (unsigned long long)ref);
    CHECK(c.pool().stateOf(ref) == ipc::BlockQuiescent && c.pool().refsOf(ref) == 1,
          "a fresh block is quiescent with one GUI reference (%s, refs %u)",
          ipc::poolStateName(c.pool().stateOf(ref)), c.pool().refsOf(ref));
    CHECK(c.pool().bump() > bump0, "the bump pointer moved (%llu -> %llu)",
          (unsigned long long)bump0, (unsigned long long)c.pool().bump());
    // The data really is in shared memory, not in the vector we built.
    CHECK(c.pool().data<f32>(ref) && c.pool().data<f32>(ref)[0] == kLevel,
          "and the samples are readable through the pool mapping");

    ipc::WireClip wc = audioClip(ref, kFrames, 2);
    CHECK(c.setClip(0, 0, wc), "publish it into clip cell [0][0]");
    CHECK(waitClipIdle(c, 0, 0), "the daemon acknowledged the cell");
    CHECK(c.clipShadow(0, 0).sampleRef == ref,
          "the client's shadow holds the offset (%llu)",
          (unsigned long long)c.clipShadow(0, 0).sampleRef);
    CHECK(c.pool().stateOf(ref) == ipc::BlockLive && c.pool().liveOf(ref) == 1,
          "and the block is live in one cell (%s, live %u)",
          ipc::poolStateName(c.pool().stateOf(ref)), c.pool().liveOf(ref));
    CHECK(c.header().clipsApplied.load() > 0, "the daemon counted a clip applied (%llu)",
          (unsigned long long)c.header().clipsApplied.load());

    // LaunchClip starts the transport itself, and the clip's quantum is None,
    // so this fires on the next drained block rather than on a bar line.
    CHECK(c.pushCommand(Cmd::LaunchClip, 0, 0), "push LaunchClip [0][0]");
    const bool playing = waitUntil([&] {
        drainEvents(c);
        return c.state().slotState[0].load() == (int)SlotState::Playing &&
               c.state().activeSlot[0].load() == 0;
    }, 2000);
    CHECK(playing, "slotState[0] is Playing on slot %d (state %d)",
          c.state().activeSlot[0].load(), c.state().slotState[0].load());

    const f64 phase0 = c.state().clipPhase[0].load();
    const bool advanced = waitUntil([&] {
        return c.state().clipPhase[0].load() != phase0;
    }, 1000);
    CHECK(advanced, "clipPhase advances (%.4f -> %.4f)", phase0,
          c.state().clipPhase[0].load());

    // The payoff: a number this process wrote into shared memory came back as
    // audio rendered by another process.
    const f32 peak = peakTrack(c, 0, 400);
    CHECK(std::fabs(peak - kLevel) < 0.05f,
          "the track meter reads the DC level: %.4f (expected %.2f)",
          (double)peak, (double)kLevel);
    const f32 master = peakMaster(c, 200);
    CHECK(master > 0.4f, "and it reaches the master bus too (%.4f)", (double)master);

    // A clip started event crossed as well; it is scalar, so it needed nothing
    // from the pool.
    std::vector<ipc::WireEvent> evs;
    drainEvents(c, &evs);
    note("%d events drained after the launch, %d of them ClipStarted",
         (int)evs.size(), countEvents(evs, (u32)Ev::ClipStarted));
}

// ---------------------------------------------------------------------------
// 8. ClearClip, the retirement echo, and reuse
// ---------------------------------------------------------------------------
//
// The free-after-confirm rule, exercised in the order it is written down: the
// block does not become freeable when the GUI stops wanting it, and it does not
// become freeable when the GUI stops referencing it from a cell. It becomes
// freeable when the daemon says the engine cannot reach it.

static void testClearAndRetire(ipc::EngineClient& c) {
    banner("8. ClearClip retires the block, and only then may the GUI free it");

    const u64 ref = gAudioRef;
    if (!ref) { CHECK(false, "section 7 left no block to retire"); return; }

    const u64 bumpWithBlock = c.pool().bump();
    const u64 blockBytes    = c.pool().blockAt(ref)->bytes;

    // Freeing now must be refused: the block is Live.
    CHECK(!c.pool().free(ref), "free() refuses a live block: %s", c.pool().error());
    CHECK(c.pool().stateOf(ref) == ipc::BlockLive, "and it is still live");

    CHECK(c.clearClip(0, 0), "push ClearClip [0][0]");
    CHECK(waitClipIdle(c, 0, 0), "the daemon acknowledged the clear");
    CHECK(c.clipShadow(0, 0).sampleRef == 0, "the shadow cell is empty");
    // Displaced, but NOT freeable: this is the state the whole rule exists for.
    CHECK(c.pool().stateOf(ref) == ipc::BlockRetiring ||
          c.pool().stateOf(ref) == ipc::BlockQuiescent,
          "the block left Live (%s)", ipc::poolStateName(c.pool().stateOf(ref)));

    const u64 retired0 = c.header().blocksRetired.load();
    std::vector<ipc::WireEvent> evs;
    const bool echoed = waitUntil([&] {
        drainEvents(c, &evs);
        for (const ipc::WireEvent& e : evs)
            if (e.type == ipc::EvBlockRetired && e.ref == ref) return true;
        return false;
    }, 3000);
    CHECK(echoed, "an EvBlockRetired echoed offset %llu back (%llu retired in total)",
          (unsigned long long)ref, (unsigned long long)c.header().blocksRetired.load());
    for (const ipc::WireEvent& e : evs)
        if (e.type == ipc::EvBlockRetired && e.ref == ref)
            CHECK(e.flags == ipc::PoolKindSamples && e.a == 0 && e.b == 0,
                  "naming the kind and the cell it left (kind %u, [%d][%d])",
                  e.flags, e.a, e.b);
    CHECK(c.header().blocksRetired.load() > retired0,
          "the daemon counted the retirement (%llu -> %llu)",
          (unsigned long long)retired0,
          (unsigned long long)c.header().blocksRetired.load());
    CHECK(c.pool().stateOf(ref) == ipc::BlockQuiescent,
          "and the block is quiescent again (%s)", ipc::poolStateName(c.pool().stateOf(ref)));
    CHECK(c.pool().refsOf(ref) == 1, "still holding the GUI's own reference");

    // Now — and only now — dropping the last reference frees it.
    CHECK(c.poolRelease(ref), "poolRelease() frees it");
    CHECK(c.pool().stateOf(ref) == ipc::BlockFree, "the block is free (%s)",
          ipc::poolStateName(c.pool().stateOf(ref)));
    CHECK(c.pool().liveBlocks() == 0, "the pool holds no live blocks (%llu)",
          (unsigned long long)c.pool().liveBlocks());

    // Allocator behaviour, asserted rather than assumed: freeing the only block
    // hands the arena's tail back to the bump pointer, so the next allocation
    // of the same size lands on exactly the same offset. That is what keeps the
    // edit-a-clip-and-repush loop from walking the pool.
    CHECK(c.pool().bump() == bumpWithBlock - blockBytes - sizeof(ipc::PoolBlock),
          "the bump pointer retracted over it (%llu, was %llu)",
          (unsigned long long)c.pool().bump(), (unsigned long long)bumpWithBlock);
    CHECK(c.pool().freeListLength() == 0,
          "and nothing was left on the free list (%u entries)", c.pool().freeListLength());

    const std::vector<f32> dc = makeDc(24000, 2, 0.25f);
    const u64 again = c.poolWrite(dc.data(), 24000, 2, 48000.0, 0);
    CHECK(again == ref, "reallocating the same size returns the same offset (%llu vs %llu)",
          (unsigned long long)again, (unsigned long long)ref);
    CHECK(c.poolRelease(again), "and it frees again immediately: it was never published");
    gAudioRef = 0;

    // Retirement is per *block*, not per cell. A block backing two slots must
    // survive losing one of them — this is the case where a naive "the cell
    // changed, so retire what it held" would hand the GUI permission to free
    // memory the engine is still playing out of the other slot.
    const std::vector<f32> mono = makeDc(2048, 1, 0.2f);
    const u64 shared = c.poolWrite(mono.data(), 2048, 1, 48000.0, 0);
    CHECK(shared != 0, "a block to share between two cells, at %llu",
          (unsigned long long)shared);
    if (!shared) return;
    const ipc::WireClip sc = audioClip(shared, 2048, 1);
    CHECK(c.setClip(3, 0, sc) && waitClipIdle(c, 3, 0), "publish it into [3][0]");
    CHECK(c.setClip(3, 1, sc) && waitClipIdle(c, 3, 1), "and into [3][1] as well");
    CHECK(c.pool().liveOf(shared) == 2, "the block is live in two cells (%u)",
          c.pool().liveOf(shared));

    CHECK(c.clearClip(3, 0) && waitClipIdle(c, 3, 0), "clear [3][0]");
    sleepMs(300);                       // three times the retirement grace period
    drainEvents(c);
    CHECK(c.pool().stateOf(shared) == ipc::BlockLive,
          "it stays live, because [3][1] still names it (%s)",
          ipc::poolStateName(c.pool().stateOf(shared)));
    CHECK(c.pool().liveOf(shared) == 1, "with one cell left (%u)", c.pool().liveOf(shared));
    CHECK(!c.pool().free(shared), "and free() still refuses it");

    CHECK(c.clearClip(3, 1) && waitClipIdle(c, 3, 1), "clear [3][1] too");
    const bool sharedRetired = waitRetired(c, shared);
    CHECK(sharedRetired, "now the last cell is gone, it retires");
    CHECK(c.poolRelease(shared), "and the GUI may free it");
    CHECK(c.pool().liveBlocks() == 0, "the pool is empty again (%llu live blocks)",
          (unsigned long long)c.pool().liveBlocks());
}

// ---------------------------------------------------------------------------
// 9. a MIDI clip through the notes pool
// ---------------------------------------------------------------------------
//
// A MIDI clip carries no audio, so there is nothing to hear; what matters is
// that the second kind of pool block survives the same round trip, that the
// engine schedules from it, and that its retirement uses the *exact* path
// rather than the deadline — replacing a notes array is the one displacement
// the engine reports itself, through Ev::NotesRetired.

static void testMidiClip(ipc::EngineClient& c) {
    banner("9. a MIDI clip: notes cross as a pool offset too");

    c.pushCommand(Cmd::StopAll);
    c.pushCommand(Cmd::SetTempo, 0, 0, 120.0);

    std::vector<ipc::WireNote> notes = makeNotes(8, 60, 0.5, 0.25);
    const u64 nref = c.poolWriteNotes(notes.data(), (i64)notes.size(), 0);
    CHECK(nref != 0, "poolWriteNotes 8 notes -> offset %llu", (unsigned long long)nref);
    if (!nref) return;
    CHECK(c.pool().blockAt(nref) && c.pool().blockAt(nref)->kind == ipc::PoolKindNotes,
          "the block is tagged as notes, not samples");

    ipc::WireClip wc = ipc::defaultWireClip();
    wc.notesRef    = nref;
    wc.noteCount   = (i64)notes.size();
    wc.isMidi      = 1;
    wc.lengthBeats = 4.0;
    wc.loop        = 1;
    wc.quantumIdx  = 0;
    wc.valid       = 1;
    CHECK(c.setClip(1, 0, wc), "publish it into clip cell [1][0]");
    CHECK(waitClipIdle(c, 1, 0), "the daemon acknowledged the cell");
    CHECK(c.pool().stateOf(nref) == ipc::BlockLive, "the notes block is live (%s)",
          ipc::poolStateName(c.pool().stateOf(nref)));

    CHECK(c.pushCommand(Cmd::LaunchClip, 1, 0), "push LaunchClip [1][0]");
    const bool playing = waitUntil([&] {
        drainEvents(c);
        return c.state().slotState[1].load() == (int)SlotState::Playing;
    }, 2000);
    CHECK(playing, "slotState[1] is Playing (state %d)", c.state().slotState[1].load());

    const f64 phase0 = c.state().clipPhase[1].load();
    const bool advanced = waitUntil([&] {
        return c.state().clipPhase[1].load() != phase0;
    }, 1000);
    CHECK(advanced, "clipPhase advances through the MIDI clip (%.4f -> %.4f)",
          phase0, c.state().clipPhase[1].load());

    // Replace the notes with a different array. Engine pushes Ev::NotesRetired
    // for the old one from inside drainCommands, which the daemon turns back
    // into an offset — so this retirement is proved rather than timed out.
    const u64 dropped0 = c.header().eventsDropped.load();
    std::vector<ipc::WireNote> more = makeNotes(16, 48, 0.25, 0.125);
    const u64 nref2 = c.poolWriteNotes(more.data(), (i64)more.size(), 0);
    CHECK(nref2 != 0 && nref2 != nref, "a second notes block at %llu",
          (unsigned long long)nref2);

    ipc::WireClip wc2 = wc;
    wc2.notesRef  = nref2;
    wc2.noteCount = (i64)more.size();
    CHECK(c.setClip(1, 0, wc2), "repush the cell with the new notes");
    CHECK(waitClipIdle(c, 1, 0), "acknowledged");
    // The wait runs on its own line, never inside CHECK's condition: the order
    // of a condition and the arguments that report it is unspecified, and the
    // message would print the state from *before* the wait.
    const bool oldRetired = waitRetired(c, nref);
    CHECK(oldRetired, "the old notes block was retired");
    CHECK(c.pool().stateOf(nref) == ipc::BlockQuiescent, "and is quiescent (%s)",
          ipc::poolStateName(c.pool().stateOf(nref)));
    CHECK(c.header().eventsDropped.load() == dropped0,
          "Ev::NotesRetired was translated, not dropped (%llu dropped)",
          (unsigned long long)(c.header().eventsDropped.load() - dropped0));
    CHECK(c.poolRelease(nref), "so the GUI can free it");

    // Leave the track quiet; the crash section wants a clean picture.
    CHECK(c.clearClip(1, 0), "clear [1][0]");
    CHECK(waitClipIdle(c, 1, 0), "acknowledged");
    const bool secondRetired = waitRetired(c, nref2);
    CHECK(secondRetired, "the second notes block retired too");
    CHECK(c.poolRelease(nref2), "and freed");
    c.pushCommand(Cmd::StopAll);
}

// ---------------------------------------------------------------------------
// 10. bad offsets
// ---------------------------------------------------------------------------
//
// Every one of these is a `u64` that must never become a pointer the engine
// dereferences. The daemon has to refuse each of them and stay up: a boundary
// that crashes on bad input has moved the failure, not prevented it.

static void testBadOffsets(ipc::EngineClient& c) {
    banner("10. a bad pool offset is refused, and the daemon survives every one");

    const std::vector<f32> dc = makeDc(4096, 2, 0.3f);
    const u64 good = c.poolWrite(dc.data(), 4096, 2, 48000.0, 0);
    CHECK(good != 0, "a good block to compare against, at %llu", (unsigned long long)good);
    if (!good) return;
    const u64 blockBytes = c.pool().blockAt(good)->bytes;

    struct BadCase { const char* what; u64 ref; i64 frames; i32 channels; };
    const BadCase bad[] = {
        {"a wild offset far past the arena",  1ull << 40,      4096, 2},
        {"an offset inside the pool header",  1024,            4096, 2},
        {"a misaligned offset",               good + 8,        4096, 2},
        {"an offset one block past the good one", good + blockBytes + 64, 4096, 2},
        {"the maximum u64",                   ~0ull,           4096, 2},
        {"a valid block read past its end",   good,            1 << 20, 2},
        {"a valid block with a wild channel count", good,      4096, 99},
    };

    const ipc::ControlHeader& h = c.header();
    int refused = 0;
    for (const BadCase& b : bad) {
        drainEvents(c);
        const u64 applied0 = h.clipsApplied.load();
        ipc::WireClip wc = audioClip(b.ref, b.frames, b.channels);
        wc.loopEnd = b.frames;
        if (!c.setClip(2, 0, wc)) { CHECK(false, "could not push %s", b.what); continue; }

        std::vector<ipc::WireEvent> evs;
        const bool answered = waitUntil([&] {
            drainEvents(c, &evs);
            return !c.clipBusy(2, 0);
        }, 2000);
        const ipc::WireEvent* ack = nullptr;
        for (const ipc::WireEvent& e : evs) if (e.type == ipc::EvClipAck) ack = &e;
        const bool ok = answered && ack && (ack->flags & ipc::ClipAckRefused) &&
                        h.clipsApplied.load() == applied0;
        if (ok) ++refused;
        CHECK(ok, "%s is refused (reason %s)", b.what,
              ipc::rejectReasonName(ack ? (u32)ack->x : 0u));
    }
    CHECK(refused == (int)(sizeof bad / sizeof bad[0]),
          "all %d bad offsets refused", (int)(sizeof bad / sizeof bad[0]));

    // The point of refusing rather than crashing.
    CHECK(c.alive(), "the daemon is still alive");
    CHECK(c.state().slotState[2].load() == (int)SlotState::Empty ||
          c.state().activeSlot[2].load() < 0,
          "and track 2 never got a clip (activeSlot %d)", c.state().activeSlot[2].load());

    // A good offset still works right afterwards, which is what proves the
    // refusals did not leave the boundary in a bad state.
    ipc::WireClip wc = audioClip(good, 4096, 2);
    CHECK(c.setClip(2, 0, wc), "a valid clip after all of them");
    CHECK(waitClipIdle(c, 2, 0), "is acknowledged");
    CHECK(c.pool().stateOf(good) == ipc::BlockLive, "and installed (%s)",
          ipc::poolStateName(c.pool().stateOf(good)));

    CHECK(c.clearClip(2, 0), "clear it again");
    CHECK(waitClipIdle(c, 2, 0), "acknowledged");
    const bool goodRetired = waitRetired(c, good);
    CHECK(goodRetired, "and retired");
    CHECK(c.poolRelease(good), "and freed");
}

// ---------------------------------------------------------------------------
// 11. devices: the plugin layer lives in the daemon now
// ---------------------------------------------------------------------------
//
// Phase 3. The client names a plugin by URI, the daemon loads it in its own
// address space, and what comes back is a device id plus a table row. Nothing
// in this file links src/plugin — that is the point — so everything asserted
// here is asserted through the wire: the metadata table, the param table, and
// the *rendered audio*.
//
// The signal is a DC clip at 0.2. Saturator's shaper is
// y = tanh(g*x) * tanh(0.5)/tanh(g*0.5), so at drive 0 dB it passes 0.2 through
// as tanh(0.2) = 0.197 (indistinguishable from unity, deliberately) and at
// drive 36 dB it lifts it to tanh(0.5) = 0.462. That is a factor of 2.3 in the
// meter from one parameter, which is what makes "the plugin is actually
// processing" a measurement rather than a liveness check. 0.5 would have been
// the obvious level to pick and is exactly the wrong one: it is the shaper's
// own reference amplitude, so it reads 0.462 at *both* ends of the knob.

static constexpr u32 kSatDrive = 0;   // the ordinals the table is indexed by
static constexpr u32 kSatTrim  = 1;
static constexpr u32 kSatMix   = 2;

// A scan walks every LV2 bundle on the system: about four seconds here, and
// several times that under ASan with a cold page cache. The wait is generous
// on purpose — this test is about what the daemon converges to, never about
// how fast it gets there.
static constexpr int kScanTimeoutMs = 180000;

// Pops until an event of `type` shows up. Everything popped on the way is
// still observed (popEvent does the client-side bookkeeping), so draining past
// an EvBlockRetired here does not lose the free it authorises.
static bool waitEvent(ipc::EngineClient& c, u32 type, ipc::WireEvent& out, int timeoutMs) {
    return waitUntil([&] {
        ipc::WireEvent e;
        while (c.popEvent(e)) if (e.type == type) { out = e; return true; }
        return false;
    }, timeoutMs);
}

// A device that never appears would otherwise hang the whole section on the
// next assertion instead of failing on this one.
static bool addDeviceAndWait(ipc::EngineClient& c, u32 target, i32 idx, i32 pos,
                             const char* uri, u32& idOut, int timeoutMs) {
    idOut = 0;
    if (!c.addDevice(target, idx, pos, uri)) return false;
    ipc::WireEvent e{};
    if (!waitEvent(c, ipc::EvDeviceAdded, e, timeoutMs)) return false;
    idOut = (u32)e.ref;
    return true;
}

static void testDevices(ipc::EngineClient& c) {
    banner("11. devices: AddDevice over the wire, metadata back, audio through it");

    resetMixer(c);
    drainEvents(c);
    const ipc::ControlHeader& h = c.header();

    // -- a DC clip to hear the device with ----------------------------------
    const i64 kFrames = 12000;
    const std::vector<f32> dc = makeDc(kFrames, 2, 0.2f);
    const u64 ref = c.poolWrite(dc.data(), kFrames, 2, 48000.0, /*key*/0xD1CEull);
    CHECK(ref != 0, "a 0.2 DC clip in the pool at offset %llu", (unsigned long long)ref);
    ipc::WireClip wc = audioClip(ref, kFrames, 2);
    CHECK(c.setClip(0, 1, wc) && waitClipIdle(c, 0, 1), "published into [0][1]");
    c.pushCommand(Cmd::LaunchClip, 0, 1);
    const bool playing = waitUntil([&] {
        drainEvents(c);
        return c.state().slotState[0].load() == (int)SlotState::Playing;
    }, 2000);
    CHECK(playing, "and playing");
    const u64 blocksBefore = c.pool().liveBlocks();   // whatever earlier sections left
    const f32 dry = peakTrack(c, 0, 300);
    CHECK(std::fabs(dry - 0.2f) < 0.02f, "the dry track meter reads %.4f", (double)dry);

    // -- the scan ------------------------------------------------------------
    //
    // Lazy: nothing has been scanned yet, because nothing has asked for a
    // plugin. The first AddDevice starts it, on a thread of its own, and the
    // heartbeat has to keep going while it runs — a pump that blocked for four
    // seconds would look exactly like a wedged engine to a client watching
    // SharedState::stale().
    CHECK(c.scanState() == ipc::ScanIdle,
          "no plugin scan has run yet (state %u) — it is lazy", c.scanState());
    const u64 hb0 = c.heartbeat();
    CHECK(c.scanPlugins(), "ask for the catalog");

    ipc::WireEvent scanEv{};
    const bool scanned = waitEvent(c, ipc::EvScanComplete, scanEv, kScanTimeoutMs);
    CHECK(scanned, "EvScanComplete arrived: %d plugins in %.2f s", scanEv.a, scanEv.x);
    CHECK(c.scanState() == ipc::ScanDone, "scanState is Done (%u)", c.scanState());
    CHECK(scanEv.a >= 2, "the catalog has at least the two stock devices (%d)", scanEv.a);
    CHECK(c.heartbeat() > hb0 + 100,
          "the pump kept beating right through the scan (%llu -> %llu ticks)",
          (unsigned long long)hb0, (unsigned long long)c.heartbeat());
    CHECK(c.alive(), "so the engine never looked wedged");

    // -- AddDevice -----------------------------------------------------------
    const u64 added0 = h.devicesAdded.load();
    u32 sat = 0;
    const bool got = addDeviceAndWait(c, ipc::DevTargetTrack, 0, -1, "lattice:saturator",
                                      sat, 5000);
    CHECK(got, "AddDevice 'lattice:saturator' on track 0 -> device %u", sat);
    if (!got) return;
    CHECK(h.devicesAdded.load() == added0 + 1, "one device added (%llu)",
          (unsigned long long)h.devicesAdded.load());
    CHECK(h.devicesLive.load() == 1, "one device live (%llu)",
          (unsigned long long)h.devicesLive.load());
    CHECK(h.chainsPublished.load() >= 1, "and a chain was published to the engine (%llu)",
          (unsigned long long)h.chainsPublished.load());

    // -- the metadata table --------------------------------------------------
    ipc::DeviceMirror d;
    const bool read = c.readDevice(sat, d);
    CHECK(read, "the device table row parses into the client's own mirror");
    CHECK(read && d.uri == "lattice:saturator", "uri '%s'", read ? d.uri.c_str() : "");
    CHECK(read && d.name == "Saturator", "name '%s'", read ? d.name.c_str() : "");
    CHECK(read && d.target == ipc::DevTargetTrack && d.targetIdx == 0 && d.chainPos == 0,
          "on %s %d at position %d", read ? ipc::devTargetName(d.target) : "?",
          read ? d.targetIdx : -1, read ? d.chainPos : -1);
    CHECK(read && d.params.size() == 3, "three parameters (%zu)",
          read ? d.params.size() : 0);
    CHECK(read && d.truncatedParams == 0, "none of them truncated away");
    CHECK(read && d.latencyFrames == 0, "zero reported latency (%d)",
          read ? d.latencyFrames : -1);
    if (read && d.params.size() == 3) {
        CHECK(d.params[kSatDrive].name == "Drive" && d.params[kSatDrive].unit == "dB" &&
              std::fabs(d.params[kSatDrive].min - 0.f) < 1e-6f &&
              std::fabs(d.params[kSatDrive].max - 36.f) < 1e-6f &&
              std::fabs(d.params[kSatDrive].def - 0.f) < 1e-6f,
              "param 0 '%s' %s [%.1f..%.1f] def %.1f",
              d.params[0].name.c_str(), d.params[0].unit.c_str(),
              (double)d.params[0].min, (double)d.params[0].max, (double)d.params[0].def);
        CHECK(d.params[kSatDrive].isLog(),
              "and it is flagged logarithmic, as the device asks (flags 0x%x)",
              d.params[kSatDrive].flags);
        CHECK(d.params[kSatTrim].name == "Output" &&
              std::fabs(d.params[kSatTrim].min + 24.f) < 1e-6f &&
              std::fabs(d.params[kSatTrim].max - 24.f) < 1e-6f,
              "param 1 '%s' [%.1f..%.1f]", d.params[1].name.c_str(),
              (double)d.params[1].min, (double)d.params[1].max);
        CHECK(d.params[kSatMix].name == "Mix" &&
              std::fabs(d.params[kSatMix].min) < 1e-6f &&
              std::fabs(d.params[kSatMix].max - 1.f) < 1e-6f &&
              std::fabs(d.params[kSatMix].def - 1.f) < 1e-6f,
              "param 2 '%s' [%.1f..%.1f] def %.1f", d.params[2].name.c_str(),
              (double)d.params[2].min, (double)d.params[2].max, (double)d.params[2].def);
    }
    CHECK(std::fabs(c.deviceParam(sat, kSatMix) - 1.0f) < 1e-6f,
          "the param table starts at the plugin's own values (Mix %.3f)",
          (double)c.deviceParam(sat, kSatMix));

    // -- the URI blob came back ---------------------------------------------
    //
    // The string crossed in the pool. A string is never handed to the engine,
    // so it is retired the instant the daemon has copied it — and because the
    // client dropped its own reference at push time, the echo is the whole of
    // the free. If this leaked, the pool would grow by one block per device for
    // the life of the session.
    const bool blobGone = waitUntil([&] {
        drainEvents(c);
        return c.pool().liveBlocks() == blocksBefore;
    }, 2000);
    CHECK(blobGone, "the URI blob was freed by its retirement echo (%llu live blocks)",
          (unsigned long long)c.pool().liveBlocks());

    // -- audio through it ----------------------------------------------------
    const f32 unity = settledPeak(c, 0, 300);
    CHECK(unity > 0.15f && unity < dry + 0.02f,
          "with drive at 0 dB the meter is unchanged: %.4f (was %.4f)",
          (double)unity, (double)dry);

    // -- the param table drives it -------------------------------------------
    //
    // §3.7: a plain store plus a generation bump, no ring and therefore no
    // drops. The daemon's pump notices within a millisecond and calls
    // PluginInstance::setParam from its own thread.
    const u64 writes0 = h.paramWrites.load();
    CHECK(c.setDeviceParam(sat, kSatDrive, 36.f), "write Drive = 36 dB into the param table");
    const bool applied = waitUntil([&] { return h.paramWrites.load() > writes0; }, 500);
    CHECK(applied, "the pump applied it (%llu setParam calls)",
          (unsigned long long)(h.paramWrites.load() - writes0));
    const f32 driven = peakTrack(c, 0, 300);
    CHECK(driven > unity * 1.5f,
          "and the rendered audio changed direction with it: %.4f -> %.4f",
          (double)unity, (double)driven);

    // A write for a device that does not exist must not be silently applied to
    // one that does.
    CHECK(!c.setDeviceParam(sat + 1, 0, 1.f),
          "a param write for an unknown device is refused by the client");

    // -- bypass round-trips --------------------------------------------------
    CHECK(c.setBypass(sat, true), "SetBypass on");
    ipc::WireEvent chg{};
    const bool bypassEv = waitEvent(c, ipc::EvDeviceChanged, chg, 2000);
    CHECK(bypassEv && (chg.flags & ipc::DeviceChangedBypass) && chg.ref == sat,
          "EvDeviceChanged says bypass (flags 0x%x, device %llu)",
          chg.flags, (unsigned long long)chg.ref);
    CHECK(c.readDevice(sat, d) && d.bypassed, "and the table row reads bypassed");
    const f32 bypassed = settledPeak(c, 0, 300);
    CHECK(std::fabs(bypassed - dry) < 0.02f,
          "the audio is passing through untouched again: %.4f (dry was %.4f)",
          (double)bypassed, (double)dry);

    CHECK(c.setBypass(sat, false), "SetBypass off");
    const bool backOn = waitUntil([&] {
        drainEvents(c);
        return c.readDevice(sat, d) && !d.bypassed;
    }, 2000);
    CHECK(backOn, "the table row reads active again");
    const f32 driven2 = settledPeak(c, 0, 300);
    CHECK(driven2 > bypassed * 1.5f, "and the saturation is back: %.4f", (double)driven2);

    // -- a second device, and MoveDevice ------------------------------------
    u32 sat2 = 0;
    const bool got2 = addDeviceAndWait(c, ipc::DevTargetTrack, 0, 0, "lattice:saturator",
                                       sat2, 5000);
    CHECK(got2 && sat2 != sat, "a second saturator inserted at position 0 -> device %u", sat2);
    if (got2) {
        CHECK(c.readDevice(sat2, d) && d.chainPos == 0, "it is first in the chain (%d)",
              d.chainPos);
        const bool shifted = waitUntil([&] {
            drainEvents(c);
            return c.readDevice(sat, d) && d.chainPos == 1;
        }, 2000);
        CHECK(shifted, "and the first one moved to position 1 (%d)", d.chainPos);

        CHECK(c.moveDevice(sat2, 1), "MoveDevice it to position 1");
        const bool moved = waitUntil([&] {
            drainEvents(c);
            return c.readDevice(sat2, d) && d.chainPos == 1;
        }, 2000);
        CHECK(moved, "which the table reflects (%d)", d.chainPos);

        CHECK(c.removeDevice(sat2), "and remove it again");
        ipc::WireEvent rm{};
        CHECK(waitEvent(c, ipc::EvDeviceRemoved, rm, 2000) && rm.ref == sat2,
              "EvDeviceRemoved for %u", sat2);
        CHECK(!c.readDevice(sat2, d), "its table row is free");
    }

    // -- a garbage URI is answered, not fatal --------------------------------
    const u64 failed0 = h.devicesFailed.load();
    const u64 live0   = c.pool().liveBlocks();
    CHECK(c.addDevice(ipc::DevTargetTrack, 0, -1, "urn:no-such-plugin:nope"),
          "AddDevice with a URI nothing answers to");
    ipc::WireEvent fail{};
    const bool answered = waitEvent(c, ipc::EvDeviceFailed, fail, 3000);
    CHECK(answered, "EvDeviceFailed came back");
    CHECK(answered && (u32)fail.b == ipc::RejectUnknownUri,
          "with reason %u (%s)", answered ? (u32)fail.b : 0u,
          ipc::rejectReasonName(answered ? (u32)fail.b : 0u));
    CHECK(h.devicesFailed.load() == failed0 + 1, "counted as a failure (%llu)",
          (unsigned long long)h.devicesFailed.load());
    const bool blobFreed = waitUntil([&] {
        drainEvents(c);
        return c.pool().liveBlocks() == live0;
    }, 2000);
    CHECK(blobFreed, "and its URI blob was retired anyway — a refusal must not leak");
    CHECK(c.alive(), "the daemon is still alive after a failed instantiation");

    // A bad device id is refused the same way.
    CHECK(c.removeDevice(ipc::kMaxDevices - 1), "RemoveDevice on an empty slot");
    ipc::WireEvent fail2{};
    const bool answered2 = waitEvent(c, ipc::EvDeviceFailed, fail2, 2000);
    CHECK(answered2 && (u32)fail2.b == ipc::RejectBadDevice,
          "answered with %s", ipc::rejectReasonName(answered2 ? (u32)fail2.b : 0u));

    // -- RemoveDevice restores passthrough -----------------------------------
    const u64 removed0 = h.devicesRemoved.load();
    const u64 retired0 = h.chainsRetired.load();
    CHECK(c.removeDevice(sat), "RemoveDevice %u", sat);
    ipc::WireEvent rm{};
    CHECK(waitEvent(c, ipc::EvDeviceRemoved, rm, 2000) && rm.ref == sat,
          "EvDeviceRemoved for it");
    CHECK(h.devicesRemoved.load() >= removed0 + 1, "counted (%llu removed)",
          (unsigned long long)h.devicesRemoved.load());
    CHECK(h.devicesLive.load() == 0, "no devices live (%llu)",
          (unsigned long long)h.devicesLive.load());
    // The instance is not destroyed at RemoveDevice — it rides the same proof
    // the displaced chain does, because until the engine has drained past the
    // new chain the audio thread may still be inside the old one. Under ASan a
    // premature destruction here is a use-after-free on the audio thread, which
    // is exactly the bug class this phase exists to remove.
    const bool chainFreed = waitUntil([&] {
        drainEvents(c);
        return h.chainsRetired.load() > retired0;
    }, 2000);
    CHECK(chainFreed, "the displaced chain and its instance were freed after the proof (%llu)",
          (unsigned long long)h.chainsRetired.load());
    const f32 passthrough = settledPeak(c, 0, 300);
    CHECK(std::fabs(passthrough - dry) < 0.02f,
          "and the track is passing the clip through again: %.4f (dry %.4f)",
          (double)passthrough, (double)dry);

    // -- returns and the master ----------------------------------------------
    //
    // The engine grew return buses and a master chain in the same wave as this
    // phase. If SetReturnChain is not functional in the Engine this daemon was
    // linked against, there is nothing here to test and saying so is better
    // than failing: the probe is whether the chain is accepted and retired at
    // all, which is a property of the daemon either way.
    u32 ret = 0;
    const bool retAdded = addDeviceAndWait(c, ipc::DevTargetReturn, 0, -1,
                                           "lattice:saturator", ret, 5000);
    CHECK(retAdded, "AddDevice saturator on return 0 -> device %u", ret);
    if (retAdded) {
        CHECK(c.readDevice(ret, d) && d.target == ipc::DevTargetReturn && d.targetIdx == 0,
              "the row says return %d", d.targetIdx);
        CHECK(c.pushCommand(Cmd::ReturnVol, 0, 0, 1.0) &&
              c.pushCommand(Cmd::SendLevel, 0, 0, 0.5),
              "ReturnVol and SendLevel cross as ordinary scalars now");
        const u64 retired1 = h.chainsRetired.load();
        CHECK(c.removeDevice(ret), "remove it again");
        const bool freed = waitUntil([&] {
            drainEvents(c);
            return h.chainsRetired.load() > retired1;
        }, 3000);
        CHECK(freed, "the return chain retired through the same proof");
    }

    u32 mas = 0;
    const bool masAdded = addDeviceAndWait(c, ipc::DevTargetMaster, 0, -1,
                                           "lattice:saturator", mas, 5000);
    CHECK(masAdded, "AddDevice saturator on the master -> device %u", mas);
    if (masAdded) {
        CHECK(c.readDevice(mas, d) && d.target == ipc::DevTargetMaster,
              "the row says master");
        const f32 masterPeak = peakMaster(c, 300);
        CHECK(masterPeak > 0.f, "the master is still rendering with a chain on it (%.4f)",
              (double)masterPeak);
        CHECK(c.removeDevice(mas), "remove it");
        CHECK(waitEvent(c, ipc::EvDeviceRemoved, rm, 3000), "EvDeviceRemoved");
    }

    CHECK(c.alive(), "the daemon survived the whole section");
    CHECK(h.eventsDropped.load() == 0,
          "and no engine event had to be dropped — Ev::ChainRetired is consumed, "
          "not dropped (%llu)", (unsigned long long)h.eventsDropped.load());

    // Leave track 0 empty for the sections after this one.
    CHECK(c.clearClip(0, 1) && waitClipIdle(c, 0, 1), "clear [0][1]");
    waitRetired(c, ref);
    c.poolRelease(ref);
}

// ---------------------------------------------------------------------------
// 12. exact retirement: the drains counter replaces the deadline
// ---------------------------------------------------------------------------
//
// §10.3 shipped a sample-block retirement that waited max(100 ms, 8 block
// periods) and said plainly that this was the weak half: a wedged backend does
// not drain, and the deadline fires anyway. Engine::drains makes the same
// statement provable — a command is consumed once the counter has advanced two
// past the value read after the push (two, not one, because the drain in
// flight at push time may have missed it).
//
// The test is therefore not "does a block retire" — section 8 covers that — but
// *how* it retires: with no sleep, in a couple of block periods, and with the
// counter having moved by the amount the proof requires.

static void testDrainsExactness(ipc::EngineClient& c) {
    banner("12. sample retirement is a proof, not a deadline");

    const ipc::ControlHeader& h = c.header();
    CHECK(h.drainsExact.load() == 1,
          "this Engine counts its command drains (%llu so far)",
          (unsigned long long)h.engineDrains.load());
    if (h.drainsExact.load() != 1) {
        note("Engine::drains never moved, so the daemon is on the legacy deadline");
        note("and the timing assertions below would be measuring the timer.");
        return;
    }

    resetMixer(c);
    drainEvents(c);

    const i64 kFrames = 4800;
    const std::vector<f32> a = makeDc(kFrames, 1, 0.3f);
    const u64 refA = c.poolWrite(a.data(), kFrames, 1, 48000.0);
    const u64 refB = c.poolWrite(a.data(), kFrames, 1, 48000.0);
    CHECK(refA && refB && refA != refB, "two blocks in the pool (%llu, %llu)",
          (unsigned long long)refA, (unsigned long long)refB);

    CHECK(c.setClip(1, 0, audioClip(refA, kFrames, 1)) && waitClipIdle(c, 1, 0),
          "publish block A into [1][0]");
    CHECK(c.pool().stateOf(refA) == ipc::BlockLive, "A is live");

    // Displace A with B and time the echo. No sleep anywhere in the loop: the
    // whole claim is that the answer arrives on the engine's terms and not on a
    // timer's, so a poll that slept would be measuring itself.
    const u64 blocks0  = c.state().blocksRendered.load();
    const u64 drains0  = h.engineDrains.load();
    const u64 t0       = ipc::monotonicNs();
    CHECK(c.setClip(1, 0, audioClip(refB, kFrames, 1)), "displace it with block B");

    const bool retired = waitUntil([&] {
        drainEvents(c);
        return c.pool().stateOf(refA) != ipc::BlockRetiring &&
               c.pool().stateOf(refA) != ipc::BlockLive;
    }, 2000, /*pollMs*/0);
    const u64 elapsedNs = ipc::monotonicNs() - t0;
    const u64 blocks1   = c.state().blocksRendered.load();
    const u64 drains1   = h.engineDrains.load();

    CHECK(retired, "A retired (state %s)", ipc::poolStateName(c.pool().stateOf(refA)));
    CHECK(elapsedNs < 60ull * 1000000ull,
          "in %.1f ms — under phase 2's 100 ms floor, so no deadline was involved",
          (double)elapsedNs / 1e6);
    CHECK(drains1 >= drains0 + 2,
          "and the drain counter moved by at least the two the proof needs (%llu -> %llu)",
          (unsigned long long)drains0, (unsigned long long)drains1);
    CHECK(blocks1 - blocks0 <= 8,
          "within %llu rendered blocks (phase 2 waited four *plus* 100 ms)",
          (unsigned long long)(blocks1 - blocks0));
    CHECK(c.poolRelease(refA), "and the client could free it immediately");

    // Clean up: B is still live in the cell.
    CHECK(c.clearClip(1, 0) && waitClipIdle(c, 1, 0), "clear [1][0]");
    CHECK(waitRetired(c, refB, 2000), "B retires on the same proof");
    CHECK(c.poolRelease(refB), "and frees");
    note("%llu blocks still live in the pool (earlier sections')",
         (unsigned long long)c.pool().liveBlocks());
}

// ---------------------------------------------------------------------------
// 13. engine crash: SIGKILL, detect, reap, respawn — with the pool attached
// ---------------------------------------------------------------------------

static void testCrashAndRespawn(ipc::EngineClient& c, pid_t& daemon) {
    banner("13. SIGKILL the daemon: alive() drops, the orphan is reaped, respawn works");
    note("and the pool survives, because the pool is ours and not the engine's.");

    // Put a clip up first, so the kill happens with the pool mapped on both
    // sides and a live block inside it. That is the case §4.4 cares about:
    // samples survive an engine restart, so republish is not a reload.
    resetMixer(c);
    const i64 kFrames = 12000;
    const std::vector<f32> dc = makeDc(kFrames, 2, 0.5f);
    const u64 ref = c.poolWrite(dc.data(), kFrames, 2, 48000.0, /*key*/0xBEEFull);
    CHECK(ref != 0, "a clip in the pool at offset %llu", (unsigned long long)ref);
    ipc::WireClip wc = audioClip(ref, kFrames, 2);
    CHECK(c.setClip(0, 0, wc) && waitClipIdle(c, 0, 0), "published into [0][0]");
    c.pushCommand(Cmd::LaunchClip, 0, 0);
    const bool wasPlaying = waitUntil([&] {
        drainEvents(c);
        return c.state().slotState[0].load() == (int)SlotState::Playing;
    }, 2000);
    CHECK(wasPlaying, "and playing before the kill");

    // And a device loaded, because that is the interesting half now: the
    // instance lives in the daemon's address space and dies with it. That is
    // the design and not a regression — a plugin cannot outlive the process
    // hosting it — so what has to survive is the *pool*, and what has to work
    // afterwards is re-adding the device to a fresh engine.
    u32 preKillDevice = 0;
    const bool hadDevice = addDeviceAndWait(c, ipc::DevTargetTrack, 0, -1,
                                            "lattice:saturator", preKillDevice, 10000);
    CHECK(hadDevice, "a saturator on track 0 before the kill (device %u)", preKillDevice);
    CHECK(c.header().devicesLive.load() == 1, "one device live (%llu)",
          (unsigned long long)c.header().devicesLive.load());

    CHECK(c.alive(), "alive() before the kill");
    ::kill(daemon, SIGKILL);
    int status = 0;
    const bool reaped = ipc::EngineClient::waitFor(daemon, 2000, &status);
    CHECK(reaped && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "the daemon died of SIGKILL");
    for (int i = 0; i < gDaemonCount; ++i) if (gDaemons[i] == daemon) gDaemons[i] = 0;

    // Two detectors, two failures (§4.4): the pid is gone *and* the heartbeat
    // has stopped. A 200 ms tolerance is far too tight for production — the doc
    // asks for several hundred ms at least — but this is a test measuring the
    // mechanism, not a policy.
    const u64 tol = 200ull * 1000000ull;
    const bool wentDead = waitUntil([&] { return !c.alive(tol); }, 2000);
    CHECK(wentDead, "alive(200 ms) goes false after the kill");
    // The pid check trips first and instantly; the heartbeat needs its
    // tolerance to elapse. Both must work, because only the second one catches
    // an engine that is alive but no longer rendering.
    const bool wentStale = waitUntil([&] { return c.state().stale(tol); }, 2000);
    CHECK(wentStale, "the frozen heartbeat goes stale on its own terms too");
    CHECK(c.state().generation.load() > 0, "the mapping still reads (the region outlives its creator)");

    // THE POINT: the control region died with its creator, but the pool did
    // not, because we created it. The block, its contents and the client's
    // allocator state are all exactly where they were.
    CHECK(shmExists(gPool), "the pool is still in /dev/shm — the engine never owned it");
    CHECK(c.pool().valid(), "and still mapped here");
    CHECK(c.pool().stateOf(ref) == ipc::BlockLive,
          "the block is still live (%s)", ipc::poolStateName(c.pool().stateOf(ref)));
    CHECK(c.pool().data<f32>(ref) && c.pool().data<f32>(ref)[0] == 0.5f,
          "and its samples are intact");
    CHECK(c.pool().findByKey(0xBEEFull) == ref,
          "the content key still finds it (§4.3 step 4): %llu",
          (unsigned long long)c.pool().findByKey(0xBEEFull));

    // The corpse is still in /dev/shm — nobody unlinked it — so a fresh attach
    // must both refuse it and clear it out of the way.
    c.detach();

    ipc::EngineClient corpse;
    const bool attachedCorpse = corpse.attach(gSession, 200);
    CHECK(!attachedCorpse, "attaching to the orphan is refused: %s", corpse.error());
    CHECK(!ipc::ShmRegion::reapIfStale(gRegion),
          "and the orphan is already gone: the refused attach reaped it");
    CHECK(shmExists(gPool), "reaping the engine's corpse left the pool alone");

    // Respawn on the same session, from scratch.
    daemon = spawnDaemon(gSession);
    CHECK(daemon > 0, "respawn latticed (pid %d)", (int)daemon);
    const bool back = c.attach(gSession, 5000);
    CHECK(back, "attach to the replacement%s%s", back ? "" : ": ", back ? "" : c.error());
    if (!back) return;
    CHECK(c.header().protocolVersion == ipc::kProtocolVersion, "fresh version handshake");
    CHECK(c.enginePid() == daemon, "the region belongs to the new daemon (%d)", c.enginePid());
    CHECK(std::fabs(c.state().tempo.load() - 120.0) < 1e-9,
          "the replacement starts from defaults, not the dead engine's state (tempo %.1f)",
          c.state().tempo.load());
    CHECK(c.alive(), "and it is alive");

    // It is a working engine, not just a mapping.
    CHECK(c.pushCommand(Cmd::SetTempo, 0, 0, 101.0), "push SetTempo 101 to the new daemon");
    const bool took = waitUntil([&] {
        return std::fabs(c.state().tempo.load() - 101.0) < 1e-9;
    }, 1000);
    CHECK(took, "which applies it (tempo %.3f)", c.state().tempo.load());

    // §4.4 step 3: republish. attach() already re-announced the pool — the
    // client does that for you precisely so a respawn cannot forget — and the
    // clip table goes back as a memcpy plus one SetClip per occupied cell. No
    // sample is decoded, no offset changes.
    const bool poolBack = waitUntil([&] { drainEvents(c); return c.poolReady(); }, 3000);
    CHECK(poolBack, "the new daemon mapped the *same* pool (epoch %llu)",
          (unsigned long long)c.header().poolAttachedEpoch.load());
    const int sent = c.republishClips();
    CHECK(sent == 1, "republishClips() re-sent %d occupied cell(s)", sent);
    const bool reapplied =
        waitUntil([&] { drainEvents(c); return c.header().clipsApplied.load() > 0; }, 2000);
    CHECK(reapplied, "which the new engine applied (%llu)",
          (unsigned long long)c.header().clipsApplied.load());
    CHECK(c.clipShadow(0, 0).sampleRef == ref,
          "against the same offset as before the crash (%llu)",
          (unsigned long long)c.clipShadow(0, 0).sampleRef);

    resetMixer(c);
    CHECK(c.pushCommand(Cmd::LaunchClip, 0, 0), "launch it on the new engine");
    const bool playingAgain = waitUntil([&] {
        drainEvents(c);
        return c.state().slotState[0].load() == (int)SlotState::Playing;
    }, 2000);
    CHECK(playingAgain, "slotState[0] is Playing again (state %d)",
          c.state().slotState[0].load());
    const f32 peak = peakTrack(c, 0, 400);
    CHECK(std::fabs(peak - 0.5f) < 0.05f,
          "and the same samples are sounding: meter %.4f", (double)peak);

    // Devices, on the other hand, did not survive — they were instances in a
    // process that no longer exists — and the replacement daemon says so
    // honestly rather than inheriting a table full of ghosts.
    CHECK(c.header().devicesLive.load() == 0,
          "the new engine has no devices: they died with the process (%llu)",
          (unsigned long long)c.header().devicesLive.load());
    ipc::DeviceMirror gone;
    CHECK(!c.readDevice(preKillDevice, gone),
          "and device %u's table row is free in the fresh region", preKillDevice);
    CHECK(c.deviceGeneration(preKillDevice) == 0,
          "the client dropped its own record of it on detach, so a stale param "
          "write cannot land on whatever takes that id next");

    // Re-adding is the whole recovery story: the URI is a string, the string
    // rides the pool that survived, and the new daemon scans and instantiates
    // from scratch.
    u32 fresh = 0;
    const bool readded = addDeviceAndWait(c, ipc::DevTargetTrack, 0, -1,
                                          "lattice:saturator", fresh, kScanTimeoutMs);
    CHECK(readded, "re-AddDevice on the replacement engine -> device %u", fresh);
    if (readded) {
        ipc::DeviceMirror d;
        CHECK(c.readDevice(fresh, d) && d.params.size() == 3,
              "with its metadata back (%zu params)", d.params.size());
        // This clip is DC at 0.5, which is Saturator's own reference
        // amplitude: fully driven, tanh pins it to tanh(0.5) = 0.462, so the
        // level goes *down*. Asserting the direction rather than the number is
        // the point — what is being tested is that a param write in one process
        // reached a plugin in another and changed the samples.
        const u64 writes0 = c.header().paramWrites.load();
        CHECK(c.setDeviceParam(fresh, kSatDrive, 36.f), "drive it to 36 dB");
        const bool took = waitUntil([&] {
            return c.header().paramWrites.load() > writes0;
        }, 2000);
        CHECK(took, "the pump applied it");
        const f32 shaped = settledPeak(c, 0, 400);
        CHECK(shaped < 0.49f && shaped > 0.40f,
              "and the rendered audio changed: 0.5 DC shaped to %.4f (tanh(0.5) = 0.4621)",
              (double)shaped);
        CHECK(c.removeDevice(fresh), "remove it again so the shutdown section is clean");
        ipc::WireEvent rm{};
        CHECK(waitEvent(c, ipc::EvDeviceRemoved, rm, 3000), "EvDeviceRemoved");
    }
}

// ---------------------------------------------------------------------------
// 14. clean shutdown, in two stages
// ---------------------------------------------------------------------------

static void testCleanShutdown(ipc::EngineClient& c, pid_t& daemon) {
    banner("14. SIGTERM: the daemon stops, publishes the flag and unlinks");

    drainEvents(c);
    ::kill(daemon, SIGTERM);

    int status = 0;
    const bool exited = ipc::EngineClient::waitFor(daemon, 3000, &status);
    CHECK(exited, "the daemon exited within 3 s of SIGTERM");
    CHECK(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "with status 0 (exited %d, code %d, signal %d)",
          exited && WIFEXITED(status), exited && WIFEXITED(status) ? WEXITSTATUS(status) : -1,
          exited && WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    for (int i = 0; i < gDaemonCount; ++i) if (gDaemons[i] == daemon) gDaemons[i] = 0;

    // shm_unlink removes the name, not the mapping: an attached client keeps
    // reading, which is exactly how it learns this was a clean exit and not a
    // crash. Without that distinction a GUI would respawn an engine that meant
    // to go away.
    CHECK(c.header().shutdown.load() == 1, "the shutdown flag is set in the control header");
    CHECK(c.state().engineState.load() == ipc::SharedState::StateStopping,
          "engineState reads Stopping (%u)", c.state().engineState.load());
    CHECK(!c.alive(), "alive() is false for a cleanly stopped engine, immediately");

    std::vector<ipc::WireEvent> evs;
    drainEvents(c, &evs);
    CHECK(countEvents(evs, ipc::EvEngineStopping) == 1,
          "an EvEngineStopping event was published before the region went (%d)",
          countEvents(evs, ipc::EvEngineStopping));

    // Stage one: the engine is gone and its region with it, but the *session*
    // is not over until the client says so, and the pool is the session's. A
    // shutdown that took the pool with it would make "attach a new engine to a
    // running session" impossible, which is the feature §4.3 is built on.
    CHECK(!shmExists(gRegion), "the control region is unlinked");
    CHECK(shmExists(gPool), "and the pool is still there — it is the session's, not the engine's");
    const int strays = countLatticeShm(gPool);
    CHECK(strays == 0, "nothing else is left in /dev/shm (%d)", strays);

    c.detach();

    ipc::EngineClient after;
    CHECK(!after.attach(gSession, 200), "the session name no longer attaches: %s", after.error());

    // Stage two: the client ends the session.
    c.closePool();
    CHECK(!shmExists(gPool), "closePool() unlinks the pool");
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc > 1) gDaemonPath = argv[1];
    if (const char* p = ::getenv("LATTICED")) gDaemonPath = p;

    std::snprintf(gSession, sizeof gSession, "dtest-%d", (int)::getpid());
    ipc::controlRegionName(gSession, gRegion, sizeof gRegion);
    ipc::poolRegionName(gSession, gPool, sizeof gPool);
    armCleanup();

    std::printf("lattice daemon tests  (shm v%u, protocol v%u, pool v%u, region %zu B)\n",
                ipc::kShmVersion, ipc::kProtocolVersion, ipc::kPoolVersion,
                ipc::control::kBytes);
    std::printf("daemon: %s   session: %s\n", gDaemonPath, gSession);

    if (::access(gDaemonPath, X_OK) != 0) {
        std::printf("  FAIL  %s is not executable — build it first (make build/latticed)\n",
                    gDaemonPath);
        return 1;
    }

    ipc::EngineClient client;
    pid_t daemon = -1;
    if (testHandshake(client, daemon)) {
        testTransport(client);
        testMetronomeAndMaster(client);
        testCommandBoundary(client);
        testBurst(client);
        testPoolHandshake(client);
        testAudioClip(client);
        testClearAndRetire(client);
        testMidiClip(client);
        testBadOffsets(client);
        testDevices(client);
        testDrainsExactness(client);
        testCrashAndRespawn(client, daemon);
        testCleanShutdown(client, daemon);
    }

    banner("15. /dev/shm is clean");
    cleanup();
    const int leftover = countLatticeShm();
    CHECK(leftover == 0, "no lattice region left in /dev/shm (found %d)", leftover);

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
