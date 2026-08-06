// Shared-memory IPC tests.
//
// Exercises src/ipc/shm.h across a real process boundary: the parent creates a
// region, fork()s, and the child attaches to it by name through shm_open. Two
// rings then run concurrently in opposite directions with sequence-numbered
// payloads, so a lost, duplicated or reordered message is a hard failure rather
// than a statistical one.
//
// Nothing here links the engine, the GUI or any audio library — the IPC layer
// depends on libc alone and the test keeps it that way.
//
//   g++ -std=c++20 -O2 -Wall -Wextra tests/ipc_test.cpp -o ipc_test -lrt -lpthread
#include "../src/ipc/shm.h"
#include "../src/ipc/pool.h"
#include "../src/ipc/client.h"

#include <chrono>
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
static const char* gTag = "";          // "" in the parent, "child " in the child

static void checkImpl(bool ok, int line, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (ok) { ++gPass; std::printf("  %sPASS  %s\n", gTag, msg); }
    else    { ++gFail; std::printf("  %sFAIL  %s   (ipc_test.cpp:%d)\n", gTag, msg, line); }
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
    std::printf("  %snote  %s\n", gTag, msg);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// wire types
// ---------------------------------------------------------------------------
//
// Deliberately *not* lat::Command / lat::Event: both of those carry a void* and
// RtClip carries a const f32*, which are meaningless in the peer's address
// space. These are the shapes the real protocol has to move to — fixed width,
// no pointers, a pool handle where a pointer used to be. See
// docs/PROCESS-SPLIT.md.

struct WireCmd {
    u32 type;
    i32 a, b;
    f64 x;
    u64 seq;        // strictly increasing, checked by the consumer
    u64 check;      // derived from seq: catches torn or stale slots
    u64 poolRef;    // stands in for the future sample-pool handle
};

struct WireEvt {
    u32 type;
    i32 a, b;
    f64 x;
    u64 seq;
    u64 check;
};

static_assert(std::is_trivially_copyable_v<WireCmd>);
static_assert(std::is_trivially_copyable_v<WireEvt>);

// Cheap avalanche so a one-bit error in seq cannot survive into check.
static inline u64 mix64(u64 v) {
    v ^= v >> 33; v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ull;
    v ^= v >> 33;
    return v;
}

// Test-only status block: the child's verdict, readable by the parent without
// squeezing it through an exit code.
struct TestStatus {
    std::atomic<u64> cmdsSeen;
    std::atomic<u64> evtsSent;
    std::atomic<u64> badOrder;
    std::atomic<u64> badPayload;
    std::atomic<u32> childDone;
};

// ---------------------------------------------------------------------------
// region layout
// ---------------------------------------------------------------------------
//
// Both sides compute these offsets from the same constants and fold them into
// the layout hash, so a build that disagrees fails at attach() instead of
// reading a ring through the wrong offset.

using CmdRing = ipc::ShmSpscRing<WireCmd, 1024>;
using EvtRing = ipc::ShmSpscRing<WireEvt, 1024>;

namespace layout {
inline constexpr size_t kState  = 0;
inline constexpr size_t kStatus = ipc::alignUp(kState  + sizeof(ipc::SharedState), ipc::kCacheLine);
inline constexpr size_t kCmds   = ipc::alignUp(kStatus + sizeof(TestStatus),       ipc::kCacheLine);
inline constexpr size_t kEvts   = ipc::alignUp(kCmds   + CmdRing::bytes(),         ipc::kCacheLine);
inline constexpr size_t kBytes  = kEvts + EvtRing::bytes();

inline constexpr u32 kHash =
    ipc::hashMix(ipc::hashMix(ipc::hashMix(ipc::fnv1a("nxtakt.ipc_test.v1"),
                 (u64)kBytes), (u64)CmdRing::capacity()), (u64)sizeof(WireCmd));
}

// ---------------------------------------------------------------------------
// cleanup discipline
// ---------------------------------------------------------------------------
//
// The creator's destructor unlinks, but a test that dies on a signal or an
// early return must not leave anything in /dev/shm either — an orphan region
// would make the *next* run's create() take the stale-reap path and mask the
// bug. Only the creating process arms this; the child clears it immediately
// after fork so it can never unlink a name it does not own.

static char gShmName[64]     = {};
static char gShmNameAlt[64]  = {};
static bool gOwnsShm         = false;

static void cleanupShm() {
    if (!gOwnsShm) return;
    if (gShmName[0])    ipc::ShmRegion::forceUnlink(gShmName);
    if (gShmNameAlt[0]) ipc::ShmRegion::forceUnlink(gShmNameAlt);
}
static void fatalSignal(int sig) {
    cleanupShm();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
static void armCleanup() {
    gOwnsShm = true;
    std::atexit(cleanupShm);
    for (int s : {SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGPIPE}) ::signal(s, fatalSignal);
}

static int countNxTaktShm() {
    DIR* d = ::opendir("/dev/shm");
    if (!d) return -1;
    int n = 0;
    while (dirent* e = ::readdir(d))
        if (std::strstr(e->d_name, "nxtakt")) { ++n; note("leftover /dev/shm/%s", e->d_name); }
    ::closedir(d);
    return n;
}

// Does a POSIX shm object by this name exist? Accepts both the "/name" and
// "name" spellings, matching how the region layer normalises them.
static bool shmExistsPool(const char* name) {
    const char* body = (name && *name == '/') ? name + 1 : name;
    char path[128];
    std::snprintf(path, sizeof path, "/dev/shm/%s", body ? body : "");
    return ::access(path, F_OK) == 0;
}

// ---------------------------------------------------------------------------
// 1. region create / attach / validate
// ---------------------------------------------------------------------------

static void testRegionBasics() {
    banner("1. region create, attach and header validation");

    // Note the shape used throughout: the call runs on its own line and only
    // then is its result checked. Folding it into CHECK()'s condition would
    // leave the order of the condition and the error()-reading argument
    // unspecified, and the message would report a stale error string.
    ipc::ShmRegion creator;
    const bool made = creator.create(gShmNameAlt, layout::kBytes, layout::kHash);
    CHECK(made, "create(%s, %zu bytes) -> %s", gShmNameAlt, layout::kBytes,
          made ? "ok" : creator.error());
    if (!made) return;

    CHECK(creator.isCreator(), "the creating region owns the unlink");
    CHECK(creator.payloadBytes() >= layout::kBytes,
          "payload is at least the requested %zu bytes (got %zu)",
          layout::kBytes, creator.payloadBytes());
    CHECK(creator.totalBytes() % 4096 == 0,
          "region is page-rounded (%zu bytes)", creator.totalBytes());

    // Not ready yet: an attacher must not see it.
    {
        ipc::ShmRegion early;
        const bool got = early.attach(gShmNameAlt, layout::kHash, ipc::kShmVersion, 0);
        CHECK(!got, "attach before publishReady() is refused (%s)", early.error());
    }

    auto* st = creator.at<ipc::SharedState>(layout::kState);
    CHECK(st != nullptr, "SharedState fits at offset %zu", layout::kState);
    if (st) st->init(48000.0, 256);
    creator.publishReady();

    // Same process, second independent mapping — still a real shm_open path.
    ipc::ShmRegion peer;
    const bool joined = peer.attach(gShmNameAlt, layout::kHash, ipc::kShmVersion, 1000);
    CHECK(joined, "attach after publishReady() succeeds%s%s",
          joined ? "" : ": ", joined ? "" : peer.error());
    CHECK(!peer.isCreator(), "an attacher never owns the unlink");
    CHECK(peer.totalBytes() == creator.totalBytes(),
          "attacher maps the same size (%zu vs %zu)", peer.totalBytes(), creator.totalBytes());

    if (peer.valid()) {
        auto* pst = peer.at<ipc::SharedState>(layout::kState);
        CHECK(pst && pst->sampleRate.load() == 48000.0,
              "attacher reads the state the creator published (%.0f Hz)",
              pst ? pst->sampleRate.load() : -1.0);
        CHECK(peer.header()->creatorPid == (i32)::getpid(),
              "header records the creator pid (%d)", peer.header()->creatorPid);
        CHECK(peer.header()->attached.load() == 1, "attach count is %u",
              peer.header()->attached.load());
        // Writes land on the other mapping: this is one region, not a copy.
        pst->tempo.store(137.5);
        CHECK(st && st->tempo.load() == 137.5,
              "a write through the attacher is visible to the creator (%.1f)",
              st ? st->tempo.load() : -1.0);

        // shm v4 (GUI-ON-DAEMON.md §1.2): the two fields the GUI polls every
        // frame that the block could not carry before. init() must zero them,
        // and they must cross like everything else in here.
        bool zeroed = pst->latencyFrames.load() == 0;
        for (int i = 0; i < ipc::kShmReturns; ++i)
            zeroed = zeroed && pst->returnMeterL[i].load() == 0.f &&
                     pst->returnMeterR[i].load() == 0.f;
        CHECK(zeroed, "init() zeroes latencyFrames and the %d return meters",
              ipc::kShmReturns);
        pst->latencyFrames.store(1536);
        pst->returnMeterL[ipc::kShmReturns - 1].store(0.75f);
        pst->returnMeterR[0].store(0.25f);
        CHECK(st && st->latencyFrames.load() == 1536 &&
                  st->returnMeterL[ipc::kShmReturns - 1].load() == 0.75f &&
                  st->returnMeterR[0].load() == 0.25f,
              "PDC latency and the return meters cross the mapping (%d frames)",
              st ? st->latencyFrames.load() : -1);
    }

    banner("2. mismatch handling");
    {
        ipc::ShmRegion bad;
        CHECK(!bad.attach(gShmNameAlt, layout::kHash, ipc::kShmVersion + 1, 200),
              "attaching with the wrong protocol version fails");
        CHECK(std::strstr(bad.error(), "version mismatch") != nullptr,
              "...and says why: %s", bad.error());
        CHECK(!bad.valid(), "the failed attacher holds no mapping");
    }
    {
        ipc::ShmRegion bad;
        CHECK(!bad.attach(gShmNameAlt, layout::kHash ^ 0xdeadbeefu, ipc::kShmVersion, 200),
              "attaching with the wrong layout hash fails");
        CHECK(std::strstr(bad.error(), "layout mismatch") != nullptr,
              "...and says why: %s", bad.error());
    }
    {
        ipc::ShmRegion bad;
        CHECK(!bad.attach("nxtakt-does-not-exist-xyz", layout::kHash, ipc::kShmVersion, 20),
              "attaching to a missing region times out cleanly (%s)", bad.error());
    }
    {
        ipc::ShmRegion bad;
        CHECK(!bad.create("has/slash", 4096, layout::kHash), "a malformed name is rejected");
        CHECK(!bad.create("", 4096, layout::kHash), "an empty name is rejected");
    }
    {
        // A live creator owns the name; a second create() must not steal it.
        ipc::ShmRegion squatter;
        CHECK(!squatter.create(gShmNameAlt, layout::kBytes, layout::kHash),
              "create() refuses a name a live process already owns (%s)", squatter.error());
        CHECK(peer.valid() && peer.at<ipc::SharedState>(layout::kState)->tempo.load() == 137.5,
              "...and the existing region is untouched");
    }
    CHECK(!ipc::ShmRegion::reapIfStale(gShmNameAlt),
          "reapIfStale() leaves a region whose creator is alive");

    // creator's destructor unlinks; peer's mapping stays valid until it goes.
}

// ---------------------------------------------------------------------------
// 3. backpressure
// ---------------------------------------------------------------------------

static void testBackpressure() {
    banner("3. full-ring backpressure");

    ipc::ShmRegion r;
    if (!r.create(gShmNameAlt, layout::kBytes, layout::kHash)) {
        CHECK(false, "create for backpressure test: %s", r.error());
        return;
    }
    CmdRing* ring = CmdRing::createAt(r, layout::kCmds);
    r.publishReady();
    if (!ring) { CHECK(false, "ring did not fit at offset %zu", layout::kCmds); return; }

    u32 accepted = 0, refused = 0;
    for (u32 i = 0; i < CmdRing::capacity() * 3; ++i) {
        WireCmd c{};
        c.type = 7; c.a = (i32)i; c.seq = i; c.check = mix64(i);
        if (ring->push(c)) ++accepted; else ++refused;
    }
    CHECK(accepted == CmdRing::capacity(),
          "exactly capacity (%u) pushes are accepted, then push() refuses (%u accepted, %u refused)",
          CmdRing::capacity(), accepted, refused);
    CHECK(refused > 0, "a full ring refuses instead of overwriting");
    CHECK(ring->size() == CmdRing::capacity(), "size() reports full (%u)", ring->size());

    // Everything that was accepted must come back intact and in order: the
    // refused pushes must not have clobbered a slot.
    bool order = true, payload = true;
    u32 got = 0;
    WireCmd out{};
    while (ring->pop(out)) {
        if (out.seq != got) order = false;
        if (out.check != mix64(out.seq) || out.a != (i32)out.seq) payload = false;
        ++got;
    }
    CHECK(got == accepted, "every accepted message pops back (%u of %u)", got, accepted);
    CHECK(order, "FIFO order survives saturation");
    CHECK(payload, "no slot was corrupted by the refused pushes");
    CHECK(ring->empty(), "the drained ring reports empty");

    // And it still works afterwards.
    WireCmd c{}; c.seq = 99; c.check = mix64(99); c.a = 99;
    CHECK(ring->push(c), "push() works again once the consumer has drained");
    CHECK(ring->pop(out) && out.seq == 99 && out.check == mix64(99),
          "...and the message round-trips");
}

// ---------------------------------------------------------------------------
// 4. cross-process exchange
// ---------------------------------------------------------------------------

static constexpr u64 kMessages = 100000;
static constexpr u64 kTimeoutNs = 30ull * 1000000000ull;   // generous: this is a liveness bound

static WireCmd mkCmd(u64 seq) {
    WireCmd c{};
    c.type    = (u32)(seq % 17);
    c.a       = (i32)(seq & 0x7fffffffu);
    c.b       = (i32)(seq % 32);
    c.x       = (f64)seq * 0.25;
    c.seq     = seq;
    c.check   = mix64(seq);
    c.poolRef = seq * 64;
    return c;
}
static bool cmdOk(const WireCmd& c, u64 seq) {
    return c.seq == seq && c.check == mix64(seq) && c.type == (u32)(seq % 17) &&
           c.a == (i32)(seq & 0x7fffffffu) && c.b == (i32)(seq % 32) &&
           c.x == (f64)seq * 0.25 && c.poolRef == seq * 64;
}
static WireEvt mkEvt(u64 seq) {
    WireEvt e{};
    e.type  = (u32)(seq % 6);
    e.a     = (i32)(seq % 32);
    e.b     = (i32)(seq % 8);
    e.x     = (f64)seq * 0.5;
    e.seq   = seq;
    e.check = mix64(seq ^ 0xa5a5a5a5ull);
    return e;
}
static bool evtOk(const WireEvt& e, u64 seq) {
    return e.seq == seq && e.check == mix64(seq ^ 0xa5a5a5a5ull) &&
           e.type == (u32)(seq % 6) && e.a == (i32)(seq % 32) &&
           e.b == (i32)(seq % 8) && e.x == (f64)seq * 0.5;
}

// The child: engine role. Consumes commands, produces events, publishes state.
// Returns an exit code; never runs the parent's atexit handlers.
[[noreturn]] static void childMain() {
    gTag     = "child ";
    gOwnsShm = false;                    // the child must never unlink

    ipc::ShmRegion r;
    if (!r.attach(gShmName, layout::kHash, ipc::kShmVersion, 5000)) {
        std::printf("  child FAIL  attach: %s\n", r.error());
        std::fflush(stdout);
        ::_exit(2);
    }
    auto*    state  = r.at<ipc::SharedState>(layout::kState);
    auto*    status = r.at<TestStatus>(layout::kStatus);
    CmdRing* cmds   = CmdRing::attachAt(r, layout::kCmds);
    EvtRing* evts   = EvtRing::attachAt(r, layout::kEvts);
    if (!state || !status || !cmds || !evts) {
        std::printf("  child FAIL  layout did not map\n");
        std::fflush(stdout);
        ::_exit(3);
    }
    state->engineState.store(ipc::SharedState::StateRunning, std::memory_order_relaxed);

    u64  sent = 0, seen = 0, badOrder = 0, badPayload = 0, spins = 0;
    u64  lastBlock = 0;
    const u64 deadline = ipc::monotonicNs() + kTimeoutNs;
    WireCmd c{};

    while (sent < kMessages || seen < kMessages) {
        if (sent < kMessages && evts->push(mkEvt(sent))) ++sent;

        while (seen < kMessages && cmds->pop(c)) {
            if (c.seq != seen)      ++badOrder;
            else if (!cmdOk(c, seen)) ++badPayload;
            ++seen;
        }

        // Stand in for Engine::publish(): one stamp per "block" of 1024
        // commands. Keyed on CROSSING a block boundary, not on `seen` landing
        // exactly on a multiple of 1024: the inner pop-loop above drains in
        // gulps under load (the parent runs far ahead), so `seen` can leap past
        // a boundary without ever equalling it — and a coincidence-based
        // `(seen & 0x3ff) == 0` then never fires, leaving playing/beat at their
        // init values. Crossing-based stamping publishes once per block whatever
        // the gulp size, so the parent always observes a live playhead.
        if (seen / 1024 != lastBlock) {
            lastBlock = seen / 1024;
            state->beat.store((f64)seen / 24000.0, std::memory_order_relaxed);
            state->playing.store(1, std::memory_order_relaxed);
            state->stampHeartbeat();
        }
        // The deadline is checked off a spin counter, not off progress: a
        // wedged peer makes no progress, which is exactly when the check has
        // to fire.
        if ((++spins & 0xffffull) == 0 && ipc::monotonicNs() > deadline) {
            std::printf("  child FAIL  timed out (sent %llu, seen %llu)\n",
                        (unsigned long long)sent, (unsigned long long)seen);
            std::fflush(stdout);
            ::_exit(4);
        }
    }

    status->cmdsSeen.store(seen, std::memory_order_relaxed);
    status->evtsSent.store(sent, std::memory_order_relaxed);
    status->badOrder.store(badOrder, std::memory_order_relaxed);
    status->badPayload.store(badPayload, std::memory_order_relaxed);
    state->engineState.store(ipc::SharedState::StateStopping, std::memory_order_relaxed);
    state->stampHeartbeat();
    status->childDone.store(1, std::memory_order_release);

    std::printf("  child note  consumed %llu commands, produced %llu events\n",
                (unsigned long long)seen, (unsigned long long)sent);
    std::fflush(stdout);
    ::_exit((badOrder || badPayload) ? 5 : 0);
}

static void testCrossProcess() {
    banner("4. cross-process exchange: 2 x 100k messages through two rings");
    note("parent = GUI role (pushes commands, drains events)");
    note("child  = engine role (drains commands, pushes events, publishes state)");

    // fork first, then create: the child must reach the region through
    // shm_open by name, not by inheriting a mapping. That is the code path the
    // real GUI/daemon pair takes.
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) childMain();
    if (pid < 0) { CHECK(false, "fork failed: %s", std::strerror(errno)); return; }

    ipc::ShmRegion r;
    if (!r.create(gShmName, layout::kBytes, layout::kHash)) {
        CHECK(false, "create: %s", r.error());
        ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0);
        return;
    }
    auto*    state  = r.at<ipc::SharedState>(layout::kState);
    auto*    status = r.at<TestStatus>(layout::kStatus);
    CmdRing* cmds   = CmdRing::createAt(r, layout::kCmds);
    EvtRing* evts   = EvtRing::createAt(r, layout::kEvts);
    if (!state || !status || !cmds || !evts) {
        CHECK(false, "layout did not fit in %zu payload bytes", r.payloadBytes());
        ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0);
        return;
    }
    state->init(48000.0, 256);
    r.publishReady();                    // the child's attach() unblocks here

    u64  sent = 0, got = 0, badOrder = 0, badPayload = 0;
    u64  pushRefused = 0, popEmpty = 0, spins = 0;
    bool timedOut = false;
    const u64 deadline = ipc::monotonicNs() + kTimeoutNs;
    const auto t0 = std::chrono::steady_clock::now();
    WireEvt e{};

    while (sent < kMessages || got < kMessages) {
        if (sent < kMessages) {
            if (cmds->push(mkCmd(sent))) ++sent; else ++pushRefused;
        }
        if (got < kMessages) {
            if (evts->pop(e)) {
                if (e.seq != got)      ++badOrder;
                else if (!evtOk(e, got)) ++badPayload;
                ++got;
            } else {
                ++popEmpty;
            }
        }
        // Spin counter, not progress: a dead child stops making progress and
        // that is precisely when the deadline has to be reachable.
        if ((++spins & 0xffffull) == 0 && ipc::monotonicNs() > deadline) { timedOut = true; break; }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const f64 secs = std::chrono::duration<f64>(t1 - t0).count();

    CHECK(!timedOut, "the exchange completed inside %llu s",
          (unsigned long long)(kTimeoutNs / 1000000000ull));
    CHECK(sent == kMessages, "parent pushed %llu commands", (unsigned long long)sent);
    CHECK(got == kMessages, "parent received %llu events", (unsigned long long)got);
    CHECK(badOrder == 0, "events arrived in strict FIFO order (%llu breaks)",
          (unsigned long long)badOrder);
    CHECK(badPayload == 0, "every event payload was intact (%llu corrupt)",
          (unsigned long long)badPayload);

    int wstat = 0;
    ::waitpid(pid, &wstat, 0);
    CHECK(WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0,
          "child exited cleanly (exited=%d status=%d)", WIFEXITED(wstat) != 0,
          WIFEXITED(wstat) ? WEXITSTATUS(wstat) : -1);
    CHECK(status->childDone.load(std::memory_order_acquire) == 1,
          "child published its verdict through shared memory");
    CHECK(status->cmdsSeen.load() == kMessages,
          "child received all %llu commands (%llu)", (unsigned long long)kMessages,
          (unsigned long long)status->cmdsSeen.load());
    CHECK(status->badOrder.load() == 0, "commands arrived in strict FIFO order (%llu breaks)",
          (unsigned long long)status->badOrder.load());
    CHECK(status->badPayload.load() == 0, "every command payload was intact (%llu corrupt)",
          (unsigned long long)status->badPayload.load());

    // The polled block: written by the child, read here, exactly as the GUI
    // will read the engine's publish().
    CHECK(state->generation.load() > 0,
          "SharedState generation advanced across the process boundary (%llu)",
          (unsigned long long)state->generation.load());
    CHECK(state->playing.load() == 1, "the child's transport flag is visible");
    CHECK(state->beat.load() > 0.0, "the child's playhead is visible (%.3f)", state->beat.load());
    CHECK(state->engineState.load() == ipc::SharedState::StateStopping,
          "the child's final engine state is visible (%u)", state->engineState.load());
    CHECK(state->enginePid.load() == (i32)::getpid(),
          "enginePid is whoever called init() (%d)", state->enginePid.load());

    if (secs > 0.0) {
        const f64 total = (f64)(sent + got);
        note("throughput: %.2f M msgs/sec aggregate (%llu msgs in %.3f s, %.0f ns/msg)",
             total / secs / 1e6, (unsigned long long)(sent + got), secs, secs / total * 1e9);
        note("backpressure encountered: %llu refused pushes, %llu empty pops",
             (unsigned long long)pushRefused, (unsigned long long)popEmpty);
    }
}

// ---------------------------------------------------------------------------
// 4b. SharedState is a seqlock (shm v5)
// ---------------------------------------------------------------------------
//
// The property under test is the one docs/GUI-ON-DAEMON.md §2.1 asks for and
// step 1 could not have locally: a reader can take a snapshot of the WHOLE block
// that provably came from one publish. Everything else in this file exercises
// message delivery; this exercises coherence, which is a different failure and
// an invisible one — a torn snapshot is not a lost message, it is a frame in
// which a clip slot is drawn Playing with activeSlot == -1.
//
// The writer stamps every field it touches with the same publish number `k`, so
// "coherent" has an exact meaning: every field the reader copied carries one
// value of k. It then PARKS mid-publish, sequence held odd, which is what turns
// the negative control from a statistical hope into a certainty — an unguarded
// copy taken during the park has the new k in the transport fields and the old k
// in the per-track arrays, every single time.
//
// The negative control is the load-bearing half. "readCoherent() never returned
// a torn copy" proves nothing on its own if nothing was ever torn; the run has
// to demonstrate that this writer tears an unguarded reader, and then that the
// guarded reader on the same data does not.

namespace seq {
inline constexpr size_t kState = 0;
inline constexpr size_t kDone  = ipc::alignUp(kState + sizeof(ipc::SharedState), ipc::kCacheLine);
inline constexpr size_t kBytes = kDone + ipc::kCacheLine;
inline constexpr u32    kHash  =
    ipc::hashMix(ipc::fnv1a("nxtakt.ipc_test.seqlock.v1"), (u64)kBytes);

// Every field the reader checks, stamped with the publish number. Chosen to
// span the block: two before the writer's park and two after it, so a straddle
// cannot hide.
inline void stamp(ipc::SharedState& s, u64 k) {
    s.beat.store((f64)k, std::memory_order_relaxed);
    s.tempo.store((f64)k, std::memory_order_relaxed);
}
inline void stampLate(ipc::SharedState& s, u64 k) {
    for (int t = 0; t < kMaxTracks; ++t) {
        s.slotState[t].store((i32)k, std::memory_order_relaxed);
        s.activeSlot[t].store((i32)k, std::memory_order_relaxed);
    }
    s.masterMeterL.store((f32)k, std::memory_order_relaxed);
}

// Copies the four sentinels out. Returns true if they agree, i.e. if the copy
// came from one publish.
struct Sample { f64 beat, tempo; i32 slot0, act31; f32 master; };
inline void grab(const ipc::SharedState& s, Sample& o) {
    o.beat   = s.beat.load(std::memory_order_relaxed);
    o.tempo  = s.tempo.load(std::memory_order_relaxed);
    o.slot0  = s.slotState[0].load(std::memory_order_relaxed);
    o.act31  = s.activeSlot[kMaxTracks - 1].load(std::memory_order_relaxed);
    o.master = s.masterMeterL.load(std::memory_order_relaxed);
}
inline bool agrees(const Sample& x) {
    const f64 k = x.beat;
    return x.tempo == k && (f64)x.slot0 == k && (f64)x.act31 == k && (f64)x.master == k;
}
} // namespace seq

static void seqWriterMain() {
    gOwnsShm = false;                     // the child never unlinks
    gTag = "child ";
    ipc::ShmRegion r;
    if (!r.attach(gShmNameAlt, seq::kHash, ipc::kShmVersion, 2000)) ::_exit(11);
    auto* s    = r.at<ipc::SharedState>(seq::kState);
    auto* done = r.at<std::atomic<u32>>(seq::kDone);
    if (!s || !done) ::_exit(12);

    for (u64 k = 1; k <= 400; ++k) {
        s->publishBegin();
        seq::stamp(*s, k);
        // The park. 200 us out of a 1 ms cadence: a fifth of the time this
        // block is half-written, which no reader can miss and no reader may
        // believe.
        timespec park{0, 200000};
        ::nanosleep(&park, nullptr);
        seq::stampLate(*s, k);
        s->publishEnd();
        timespec gap{0, 800000};
        ::nanosleep(&gap, nullptr);
    }
    done->store(1, std::memory_order_release);
    ::_exit(0);
}

static void testStateSeqlock() {
    banner("4b. SharedState is a seqlock: a snapshot cannot straddle a publish");

    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-ipc-seq-%d", (int)::getpid());

    ipc::ShmRegion r;
    if (!r.create(gShmNameAlt, seq::kBytes, seq::kHash)) {
        CHECK(false, "create: %s", r.error());
        gShmNameAlt[0] = '\0';
        return;
    }
    auto* s    = r.at<ipc::SharedState>(seq::kState);
    auto* done = r.at<std::atomic<u32>>(seq::kDone);
    if (!s || !done) {
        CHECK(false, "layout did not fit in %zu payload bytes", r.payloadBytes());
        gShmNameAlt[0] = '\0';
        return;
    }
    s->init(48000.0, 256);
    seq::stamp(*s, 0);
    seq::stampLate(*s, 0);
    done->store(0, std::memory_order_relaxed);
    r.publishReady();

    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) seqWriterMain();
    if (pid < 0) { CHECK(false, "fork failed: %s", std::strerror(errno)); gShmNameAlt[0] = '\0'; return; }

    u64 guarded = 0, guardedTorn = 0, guardedGaveUp = 0;
    u64 plain = 0, plainTorn = 0;
    u64 sawOdd = 0;
    const u64 deadline = ipc::monotonicNs() + 10ull * 1000000000ull;

    while (done->load(std::memory_order_acquire) == 0 && ipc::monotonicNs() < deadline) {
        // The negative control, first and on the same data: §2.1's own recipe
        // without the parity gate is exactly a plain copy, and a plain copy is
        // what step 1's local poll() has to live with.
        seq::Sample p{};
        seq::grab(*s, p);
        ++plain;
        if (!seq::agrees(p)) ++plainTorn;

        if (s->generation.load(std::memory_order_relaxed) & 1ull) ++sawOdd;

        seq::Sample g{};
        const bool ok = s->readCoherent([&] { seq::grab(*s, g); });
        ++guarded;
        if (!ok) ++guardedGaveUp;
        else if (!seq::agrees(g)) ++guardedTorn;
    }

    int wstat = 0;
    ::waitpid(pid, &wstat, 0);
    CHECK(WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0,
          "the writer exited cleanly (exited=%d status=%d)", WIFEXITED(wstat) != 0,
          WIFEXITED(wstat) ? WEXITSTATUS(wstat) : -1);

    CHECK(sawOdd > 0,
          "the sequence was observed ODD, so publishBegin() really brackets the "
          "publish (%llu of %llu samples)",
          (unsigned long long)sawOdd, (unsigned long long)plain);
    CHECK((s->generation.load() & 1ull) == 0,
          "and EVEN once the writer is done, so publishEnd() closes it (%llu)",
          (unsigned long long)s->generation.load());

    // The negative control. Remove publishBegin() from the writer and this stays
    // 0 while the check below starts failing — which is the same fact from the
    // other side and the reason both are asserted.
    CHECK(plainTorn > 0,
          "an UNGUARDED copy of the same block tears: %llu of %llu samples "
          "disagreed with themselves",
          (unsigned long long)plainTorn, (unsigned long long)plain);
    CHECK(guardedTorn == 0,
          "readCoherent() never handed back a torn snapshot (%llu of %llu proved "
          "coherent, %llu gave up on the retry budget)",
          (unsigned long long)(guarded - guardedTorn - guardedGaveUp),
          (unsigned long long)guarded, (unsigned long long)guardedGaveUp);
    note("the writer parks 200 us mid-publish, so the unguarded tear rate is a "
         "property of the test rather than of the machine it runs on.");

    r.close();
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 5. stale-region reaping
// ---------------------------------------------------------------------------

static void testStaleReap() {
    banner("5. crash-orphan detection and reaping");

    // A child creates a region and is killed with SIGKILL — no destructor, no
    // unlink. This is exactly the engine-crash case.
    char orphan[64];
    std::snprintf(orphan, sizeof orphan, "nxtakt-ipc-orphan-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "%s", orphan);

    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ipc::ShmRegion r;
        if (!r.create(orphan, 4096, layout::kHash)) ::_exit(1);
        r.publishReady();
        for (;;) ::pause();              // killed below, region left behind
    }
    if (pid < 0) { CHECK(false, "fork failed"); return; }

    // Wait for the region to appear.
    ipc::ShmRegion probe;
    const bool up = probe.attach(orphan, layout::kHash, ipc::kShmVersion, 3000);
    CHECK(up, "child created the region%s%s", up ? "" : ": ", up ? "" : probe.error());
    CHECK(!ipc::ShmRegion::reapIfStale(orphan), "a live creator's region is not reaped");
    probe.close();

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);

    CHECK(ipc::ShmRegion::reapIfStale(orphan),
          "the orphan of a SIGKILLed creator is detected and unlinked");
    CHECK(!ipc::ShmRegion::reapIfStale(orphan), "reaping is idempotent");

    // And a fresh creator can now take the name.
    ipc::ShmRegion fresh;
    const bool retaken = fresh.create(orphan, 4096, layout::kHash);
    CHECK(retaken, "a new creator claims the reclaimed name%s%s",
          retaken ? "" : ": ", retaken ? "" : fresh.error());
    fresh.close();
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 6. F6 — a forged EvBlockRetired must not corrupt the client's free list
// ---------------------------------------------------------------------------
//
// The daemon echoes EvBlockRetired back to say "the engine can no longer reach
// this block". A hostile (or buggy) peer can push the same event with a ref
// that points into live sample data: as f32 bit patterns, near-silent audio is
// full of offsets whose reinterpreted PoolBlock header reads state=Retiring,
// live=0, refs=0 — exactly the shape confirmRetired() used to act on. The fix
// is the self-mixed block magic in validRef(): an interior offset is not a
// block, so blockAt() returns null and the free list is never touched.

static void testForgedRetirement() {
    banner("6. a forged EvBlockRetired cannot corrupt the client free list (F6)");
    char session[32];
    std::snprintf(session, sizeof session, "ipc-forge-%d", (int)::getpid());

    // Register the pool name for the crash/atexit cleanup path so a mid-test
    // failure cannot leave it in /dev/shm.
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-pool-%s", session);

    ipc::EngineClient c;
    if (!c.createPool(session, 4u << 20)) { CHECK(false, "createPool: %s", c.error()); return; }
    ipc::SamplePool& p = c.pool();

    // Fill with a denormal ~ near-silence: the audit's "qualifying offset"
    // pattern, so the forged header genuinely looks retirement-shaped.
    std::vector<f32> dc((size_t)4096 * 2, 4.2e-45f);
    const u64 a = c.poolWrite(dc.data(), 4096, 2, 48000.0, 1);
    CHECK(a != 0, "a real sample block at %llu", (unsigned long long)a);
    if (!a) { c.closePool(); return; }

    const u32 freeLen0 = p.freeListLength();
    const u64 live0    = p.liveBlocks();

    // An interior, 64-aligned offset of the block's own data, under the bump —
    // the dangerous case bounds-only validation used to accept.
    const u64 forged = a + 1024;
    CHECK(forged % ipc::kPoolAlign == 0 && forged < p.bump(),
          "the forged ref is 64-aligned and under the bump (the dangerous case)");

    ipc::WireEvent e{};
    e.type = ipc::EvBlockRetired;
    e.ref  = forged;
    CHECK(c.observe(e), "observe() consumes the forged echo as a protocol event");

    // Nothing moved: free list, live count, and the real block are all intact.
    CHECK(p.freeListLength() == freeLen0,
          "the free list is unchanged (%u == %u)", p.freeListLength(), freeLen0);
    CHECK(p.liveBlocks() == live0,
          "the live-block count is unchanged (%llu == %llu)",
          (unsigned long long)p.liveBlocks(), (unsigned long long)live0);
    CHECK(p.stateOf(a) == ipc::BlockQuiescent,
          "the real block is untouched (%s)", ipc::poolStateName(p.stateOf(a)));

    // The allocator still works and hands back a usable block.
    const u64 b = c.poolWrite(dc.data(), 1024, 2, 48000.0, 2);
    CHECK(b != 0 && b != a, "a subsequent alloc still works (%llu)", (unsigned long long)b);
    CHECK(b && p.data<f32>(b) != nullptr, "and the block is usable");

    // Positive control: a genuinely-Retiring block IS retired by its own echo,
    // so the magic check did not break the real path.
    const u64 g = c.poolWrite(dc.data(), 512, 2, 48000.0, 3);
    CHECK(g != 0, "a third block to retire for real (%llu)", (unsigned long long)g);
    p.markLive(g);         // a clip cell points here
    p.markDisplaced(g);    // it stopped pointing here -> Retiring
    CHECK(p.stateOf(g) == ipc::BlockRetiring, "the real block is Retiring (%s)",
          ipc::poolStateName(p.stateOf(g)));
    ipc::WireEvent ge{};
    ge.type = ipc::EvBlockRetired;
    ge.ref  = g;
    c.observe(ge);
    CHECK(p.stateOf(g) == ipc::BlockQuiescent,
          "its own echo retires it correctly (%s)", ipc::poolStateName(p.stateOf(g)));

    c.poolRelease(a);
    c.poolRelease(b);
    c.poolRelease(g);
    c.closePool();
    CHECK(!shmExistsPool(session), "the forge-test pool is unlinked");
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 7. F8 — an adopted pool is not reaped when its ORIGINAL creator dies
// ---------------------------------------------------------------------------
//
// The pool is designed to outlive its creator: a crashed GUI leaves it behind
// and a replacement adopts it. reapIfStale() keys liveness on
// ShmHeader::creatorPid, which used to keep naming the dead original — so a
// live, adopted pool read as an orphan and could be unlinked out from under its
// new owner. adoptCreator() (called from SamplePool::attach) moves the liveness
// key to the adopting process, so the pool reads as alive.

static void testAdoptedPoolLiveness() {
    banner("7. an adopted pool survives a reap-check after its creator dies (F8)");
    char pool[48];
    std::snprintf(pool, sizeof pool, "nxtakt-ipc-adopt-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "%s", pool);   // crash-safe cleanup

    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ipc::SamplePool sp;
        if (!sp.create(pool, 2u << 20)) ::_exit(1);
        std::vector<f32> dc((size_t)1024 * 2, 0.5f);
        sp.writeSamples(dc.data(), 1024, 2, 48000.0, 1);
        sp.abandon();                // detach WITHOUT unlink: leave it behind
        for (;;) ::pause();          // SIGKILLed below; region persists
    }
    if (pid < 0) { CHECK(false, "fork failed"); return; }

    // Parent adopts the pool the child created (this re-stamps the liveness key).
    ipc::SamplePool mine;
    const bool up = mine.attach(pool, 3000);
    CHECK(up, "adopted the child's pool%s%s", up ? "" : ": ", up ? "" : mine.error());
    const u64 childBump = mine.bump();

    // The original creator is now provably gone.
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);

    // Pre-fix, this reaped a live adopted pool. Now the key is us, and we live.
    CHECK(!ipc::ShmRegion::reapIfStale(pool),
          "reapIfStale() leaves the adopted pool alone (the key is now this process)");
    CHECK(shmExistsPool(pool), "the pool region still exists in /dev/shm");
    CHECK(mine.valid(), "and our mapping is still valid");
    CHECK(mine.bump() == childBump && childBump > ipc::kPoolArenaOffset,
          "the block the child wrote survived (bump %llu)", (unsigned long long)childBump);

    // We adopted (unlink_ == false), so unlink explicitly to keep /dev/shm clean.
    mine.close();
    ipc::ShmRegion::forceUnlink(pool);
    CHECK(!shmExistsPool(pool), "and it unlinks cleanly on the way out");
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 8. the wire classifiers and shapes wave 8g adds
// ---------------------------------------------------------------------------
//
// 8a appended four commands and two events to engine.h and left them
// compiled-and-unused; commandIsKnown() still bounded at Cmd::RecordMidiSlot,
// so all four classified as UNKNOWN. That failed closed — which is why the tree
// was safe — and this is the assertion that it is now classified properly and
// that nothing else moved class while we were in there.

static void testArrangementClassifiers() {
    banner("8. the arrangement's commands and events classify (the 8a hand-off)");

    // The four appended commands. Two are pure scalars; two carry a pool blob.
    // None of them may be "unknown" any more, and none may be mistaken for a
    // pointer-carrying command — which is what a wrong answer here would mean
    // in the daemon: a permanent refusal that no client could work around.
    struct Row { const char* name; Cmd c; bool known, scalar, pooled, arr, ptr; };
    const Row rows[] = {
        {"SetArrangement",    Cmd::SetArrangement,    true,  false, false, true,  false},
        {"SetTrackAutos",     Cmd::SetTrackAutos,     true,  false, false, true,  false},
        {"Locate",            Cmd::Locate,            true,  true,  false, false, false},
        {"BackToArrangement", Cmd::BackToArrangement, true,  true,  false, false, false},
        // Unchanged neighbours, so a reclassification cannot slip in unnoticed.
        {"SetClip",           Cmd::SetClip,           true,  false, true,  false, false},
        {"SetChain",          Cmd::SetChain,          true,  false, false, false, true},
        {"RecordMidiSlot",    Cmd::RecordMidiSlot,    true,  false, false, false, true},
        {"SetTempo",          Cmd::SetTempo,          true,  true,  false, false, false},
    };
    for (const Row& r : rows) {
        const u32 t = (u32)r.c;
        const bool ok = ipc::commandIsKnown(t)       == r.known  &&
                        ipc::commandIsScalar(t)      == r.scalar &&
                        ipc::commandIsPooled(t)      == r.pooled &&
                        ipc::commandIsArrangement(t) == r.arr    &&
                        ipc::commandCarriesPointer(t)== r.ptr;
        CHECK(ok, "%s (%u): known=%d scalar=%d pooled=%d arrangement=%d pointer=%d",
              r.name, t, (int)ipc::commandIsKnown(t), (int)ipc::commandIsScalar(t),
              (int)ipc::commandIsPooled(t), (int)ipc::commandIsArrangement(t),
              (int)ipc::commandCarriesPointer(t));
    }
    CHECK(!ipc::commandIsKnown((u32)Cmd::BackToArrangement + 1),
          "and one past the last enumerator is still unknown — the bound is the "
          "enum's end, not a hand-copied number");

    // Both new events carry a pointer to a block the DAEMON built, so both are
    // consumed rather than forwarded. A `true` here would send a daemon-heap
    // address to another process.
    CHECK(!ipc::eventIsScalar((u32)Ev::ArrangementRetired), "Ev::ArrangementRetired is not scalar");
    CHECK(!ipc::eventIsScalar((u32)Ev::TrackAutosRetired),  "Ev::TrackAutosRetired is not scalar");
    CHECK(ipc::eventIsScalar((u32)Ev::ClipStarted), "and the scalar events still are");

    // The wire shapes, and the mirrors that make a memcpy honest.
    CHECK(sizeof(ipc::WireArrHeader) == 48 && sizeof(ipc::WireArrItem) == 40,
          "WireArrHeader is %zu B and WireArrItem is %zu B",
          sizeof(ipc::WireArrHeader), sizeof(ipc::WireArrItem));
    CHECK(sizeof(ipc::WireJournal) == sizeof(ArrJournal),
          "WireJournal mirrors ArrJournal (%zu B)", sizeof(ipc::WireJournal));
    CHECK(ipc::arrangementBytes(2, 1) == 48 + 2 * 40 + 120,
          "a two-item, one-clip blob is %llu B",
          (unsigned long long)ipc::arrangementBytes(2, 1));
    CHECK(ipc::kMaxArrLanes == kMaxRtArrLanes,
          "the wire lane bound and the engine's agree (%d)", (int)ipc::kMaxArrLanes);
    CHECK(ipc::kProtocolVersion == 5 && ipc::kPoolVersion == 4 && ipc::kShmVersion == 6,
          "protocol v%u, pool v%u, shm v%u", ipc::kProtocolVersion, ipc::kPoolVersion,
          ipc::kShmVersion);
    CHECK(ipc::control::kJournal > ipc::control::kParams &&
          ipc::control::kBytes == ipc::control::kJournal + ipc::JournalRing::bytes(),
          "the journal is the ninth section, appended at %zu of %zu B",
          ipc::control::kJournal, ipc::control::kBytes);
}

// ---------------------------------------------------------------------------
// 9. the journal ring across a real process boundary, and its drop counter
// ---------------------------------------------------------------------------
//
// §9.6: the journal gets a ring of its own because the two channels have
// different failure budgets. An event must not be lost — EvClipAck wedges a
// clip cell for the rest of the session if it is — while a journal entry must
// be KNOWN to be lost, because §5.4 refuses a take whose journal has a gap
// rather than committing it short.
//
// So both halves are asserted here: that a full run of entries crosses with its
// `seq` contiguous, and that an OVERRUN is visible as a gap in `seq` and as a
// drop count. Overflowing the ring on purpose is the point — the failure has to
// be observable, or "refuse, do not commit short" is not decidable.

static void testJournalRingCrossProcess() {
    banner("9. the journal ring carries entries, and a gap is observable (§5.4, §9.6)");

    char nm[64];
    std::snprintf(nm, sizeof nm, "nxtakt-ipc-journal-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "%s", nm);

    // A region holding exactly one journal ring plus a phase word. The phase is
    // what makes the overflow DETERMINISTIC rather than a race we hope to win:
    // the consumer is held off while the producer overruns the ring, and only
    // then allowed to drain, so the gap lands in a known place with a known
    // size.
    struct JStatus {
        std::atomic<u64> seen, gaps, gapSize, bad;
        std::atomic<u32> phase;      // see the handshake below
    };
    const size_t kStatusOff = 0;
    const size_t kRingOff   = ipc::alignUp(sizeof(JStatus), ipc::kCacheLine);
    const size_t kBytes     = kRingOff + ipc::JournalRing::bytes();
    const u32    kHash      = ipc::fnv1a("nxtakt.journal_test.v1");

    ipc::ShmRegion region;
    if (!region.create(nm, kBytes, kHash)) {
        CHECK(false, "create the journal region: %s", region.error());
        return;
    }
    JStatus* st = region.at<JStatus>(kStatusOff);
    ipc::JournalRing* ring = ipc::JournalRing::createAt(region, kRingOff);
    CHECK(st && ring, "a %zu B region holding one JournalRing", kBytes);
    if (!st || !ring) return;
    st->seen.store(0); st->gaps.store(0); st->gapSize.store(0); st->bad.store(0);
    st->phase.store(0, std::memory_order_release);
    region.publishReady();

    const u32 kBurstA = 12000;                       // overruns the ring on purpose
    const u32 kBurstB = 200;                         // sent after the drain: the gap
    CHECK(kBurstA > ipc::JournalRing::capacity(),
          "burst A is %u entries through a %u-slot ring, so it must overflow",
          kBurstA, ipc::JournalRing::capacity());

    // The payload every entry carries, derived from seq so a torn slot is a
    // hard failure rather than a plausible one.
    const auto fill = [](u32 i) {
        ipc::WireJournal j{};
        j.kind  = (u32)JournalKind::NoteOn;
        j.seq   = i;
        j.track = (i32)(i % 32);
        j.a     = 60 + (i32)(i % 12);
        j.beat  = (f64)i * 0.25;
        return j;
    };

    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        gOwnsShm = false;                            // the child never unlinks
        ipc::ShmRegion r;
        if (!r.attach(nm, kHash, ipc::kShmVersion, 3000)) ::_exit(1);
        JStatus* cs = r.at<JStatus>(kStatusOff);
        ipc::JournalRing* cr = ipc::JournalRing::attachAt(r, kRingOff);
        if (!cs || !cr) ::_exit(1);

        u64 seen = 0, gaps = 0, gapSize = 0, bad = 0;
        u32  expect = 0;
        bool first  = true;
        const auto drain = [&](u32 untilPhase) {
            const u64 deadline = ipc::monotonicNs() + 10ull * 1000000000ull;
            for (;;) {
                ipc::WireJournal j;
                while (cr->pop(j)) {
                    ++seen;
                    if (j.kind != (u32)JournalKind::NoteOn || j.track != (i32)(j.seq % 32) ||
                        j.beat != (f64)j.seq * 0.25)
                        ++bad;
                    // A consumer that sees seq jump knows exactly HOW MANY
                    // entries it lost and WHERE. That is the whole reason seq
                    // increments on every attempted push and not on every
                    // successful one.
                    if (!first && j.seq != expect) { ++gaps; gapSize += j.seq - expect; }
                    first  = false;
                    expect = j.seq + 1;
                }
                if (cs->phase.load(std::memory_order_acquire) >= untilPhase && cr->empty())
                    return;
                if (ipc::monotonicNs() >= deadline) return;
            }
        };

        // Held off until the producer has finished overrunning the ring.
        while (cs->phase.load(std::memory_order_acquire) < 1) {}
        drain(1);
        cs->phase.store(2, std::memory_order_release);   // "I am empty, send more"
        while (cs->phase.load(std::memory_order_acquire) < 3) {}
        drain(3);

        cs->seen.store(seen, std::memory_order_relaxed);
        cs->gaps.store(gaps, std::memory_order_relaxed);
        cs->gapSize.store(gapSize, std::memory_order_relaxed);
        cs->bad.store(bad, std::memory_order_relaxed);
        cs->phase.store(4, std::memory_order_release);
        ::_exit(0);
    }
    if (pid < 0) { CHECK(false, "fork failed"); region.close(); return; }

    // Burst A, with nobody draining: an entry that will not fit is GONE,
    // because there is nowhere to put it back. This is the daemon's own
    // behaviour and the engine's before it.
    u64 dropped = 0;
    for (u32 i = 0; i < kBurstA; ++i) if (!ring->push(fill(i))) ++dropped;
    st->phase.store(1, std::memory_order_release);

    const auto waitPhase = [&](u32 want) {
        const u64 deadline = ipc::monotonicNs() + 10ull * 1000000000ull;
        while (ipc::monotonicNs() < deadline)
            if (st->phase.load(std::memory_order_acquire) >= want) return true;
        return false;
    };
    const bool drained = waitPhase(2);
    CHECK(drained, "the consumer drained burst A and asked for more");

    // Burst B, into a ring that is now empty: every one of these arrives, so
    // the loss shows up as an INTERIOR gap rather than a truncated tail.
    for (u32 i = 0; i < kBurstB; ++i)
        if (!ring->push(fill(kBurstA + i))) ++dropped;
    st->phase.store(3, std::memory_order_release);

    const bool finished = waitPhase(4);
    ::waitpid(pid, nullptr, 0);
    CHECK(finished, "and finished");

    const u64 seen = st->seen.load(), gaps = st->gaps.load(),
              gapSize = st->gapSize.load(), bad = st->bad.load();
    CHECK(bad == 0, "every entry that crossed arrived intact (%llu torn)",
          (unsigned long long)bad);
    CHECK(seen + dropped == kBurstA + kBurstB,
          "%llu entries crossed and %llu were refused: %llu of %u sent",
          (unsigned long long)seen, (unsigned long long)dropped,
          (unsigned long long)(seen + dropped), kBurstA + kBurstB);
    CHECK(dropped > 0, "the ring did overflow, so the failure being tested happened "
          "(%llu refused)", (unsigned long long)dropped);
    CHECK(gaps == 1, "the loss is VISIBLE to the consumer as a gap in seq (%llu gap)",
          (unsigned long long)gaps);
    CHECK(gapSize == dropped,
          "and the gap measures EXACTLY what was lost: %llu missing, %llu refused — "
          "the same fact reported from both sides, which is what makes 'refuse the "
          "take, do not commit it short' decidable",
          (unsigned long long)gapSize, (unsigned long long)dropped);
    note("a consumer seeing this discards the take and names the count, per §5.4.");

    region.close();
    CHECK(!shmExistsPool(nm), "the journal region unlinks cleanly");
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // survives fork() without duplicating output
    std::printf("nxtakt ipc tests  (protocol v%u, %zu-byte region, %u-slot rings)\n",
                ipc::kShmVersion, layout::kBytes, CmdRing::capacity());

    std::snprintf(gShmName,    sizeof gShmName,    "nxtakt-ipc-test-%d",     (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-ipc-test-%d-alt", (int)::getpid());
    armCleanup();

    testRegionBasics();
    testBackpressure();
    testCrossProcess();
    testStateSeqlock();
    testStaleReap();
    testForgedRetirement();
    testAdoptedPoolLiveness();
    testArrangementClassifiers();
    testJournalRingCrossProcess();

    banner("10. /dev/shm is clean");
    cleanupShm();
    const int leftover = countNxTaktShm();
    CHECK(leftover == 0, "no nxtakt region left in /dev/shm (found %d)", leftover);

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
