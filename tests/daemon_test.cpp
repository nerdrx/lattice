// Engine-daemon tests.
//
// Spawns a real ./build/latticed in --driver null mode, attaches to its control
// region with ipc::EngineClient, and exercises the whole phase-1 boundary from
// the outside: version handshake, scalar commands, the polled state block, the
// refusal of every pointer-carrying command, engine death by SIGKILL, and clean
// shutdown by SIGTERM.
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

static int countLatticeShm() {
    DIR* d = ::opendir("/dev/shm");
    if (!d) return -1;
    int n = 0;
    while (dirent* e = ::readdir(d))
        if (std::strstr(e->d_name, "lattice")) { ++n; note("leftover /dev/shm/%s", e->d_name); }
    ::closedir(d);
    return n;
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
    note("phase 1 is scalar-only. SetClip/ClearClip/SetChain/RecordSlot/");
    note("RecordMidiSlot carry GUI-heap pointers, so the daemon refuses them at");
    note("the boundary with a reason rather than half-translating them.");

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

    // -- the five that cannot cross -----------------------------------------
    const Cmd pointerCmds[] = {Cmd::SetClip, Cmd::ClearClip, Cmd::SetChain,
                               Cmd::RecordSlot, Cmd::RecordMidiSlot};
    const u64 applied1 = h.commandsApplied.load();
    for (Cmd t : pointerCmds) {
        ipc::WireCommand w{};
        w.type = (u32)t;
        w.a = 0; w.b = 0; w.x = 4.0;
        w.ref = 0xdeadbeefull;                     // the future pool handle
        CHECK(c.pushCommand(w), "push pointer-carrying command %u", (u32)t);
    }
    const bool allRejected = waitUntil([&] {
        return h.commandsRejected.load() >= rejected0 + 5;
    }, 1000);
    CHECK(allRejected, "all five were refused (%llu rejected)",
          (unsigned long long)(h.commandsRejected.load() - rejected0));
    CHECK(h.commandsApplied.load() == applied1,
          "and not one of them reached the engine (%llu applied since)",
          (unsigned long long)(h.commandsApplied.load() - applied1));

    std::vector<ipc::WireEvent> evs;
    waitUntil([&] { drainEvents(c, &evs); return countEvents(evs, ipc::EvCommandRejected) >= 5; },
              1000);
    CHECK(countEvents(evs, ipc::EvCommandRejected) == 5,
          "one EvCommandRejected per refusal (%d)", countEvents(evs, ipc::EvCommandRejected));
    for (Cmd t : pointerCmds) {
        const ipc::WireEvent* e = findReject(evs, t);
        CHECK(e && (u32)e->b == ipc::RejectPointerPayload,
              "command %u refused with reason %u (%s)", (u32)t, e ? (u32)e->b : 0u,
              ipc::rejectReasonName(e ? (u32)e->b : 0u));
        CHECK(e && e->ref == 0xdeadbeefull, "the refusal echoes the caller's ref back");
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
// 6. engine crash: SIGKILL, detect, reap, respawn
// ---------------------------------------------------------------------------

static void testCrashAndRespawn(ipc::EngineClient& c, pid_t& daemon) {
    banner("6. SIGKILL the daemon: alive() drops, the orphan is reaped, respawn works");

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

    // The corpse is still in /dev/shm — nobody unlinked it — so a fresh attach
    // must both refuse it and clear it out of the way.
    c.detach();

    ipc::EngineClient corpse;
    const bool attachedCorpse = corpse.attach(gSession, 200);
    CHECK(!attachedCorpse, "attaching to the orphan is refused: %s", corpse.error());
    CHECK(!ipc::ShmRegion::reapIfStale(gRegion),
          "and the orphan is already gone: the refused attach reaped it");

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
}

// ---------------------------------------------------------------------------
// 7. clean shutdown
// ---------------------------------------------------------------------------

static void testCleanShutdown(ipc::EngineClient& c, pid_t& daemon) {
    banner("7. SIGTERM: the daemon stops, publishes the flag and unlinks");

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

    c.detach();

    ipc::EngineClient after;
    CHECK(!after.attach(gSession, 200), "the session name no longer attaches: %s", after.error());
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc > 1) gDaemonPath = argv[1];
    if (const char* p = ::getenv("LATTICED")) gDaemonPath = p;

    std::snprintf(gSession, sizeof gSession, "dtest-%d", (int)::getpid());
    ipc::controlRegionName(gSession, gRegion, sizeof gRegion);
    armCleanup();

    std::printf("lattice daemon tests  (shm v%u, protocol v%u, region %zu B)\n",
                ipc::kShmVersion, ipc::kProtocolVersion, ipc::control::kBytes);
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
        testCrashAndRespawn(client, daemon);
        testCleanShutdown(client, daemon);
    }

    banner("8. /dev/shm is clean");
    cleanup();
    const int leftover = countLatticeShm();
    CHECK(leftover == 0, "no lattice region left in /dev/shm (found %d)", leftover);

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
