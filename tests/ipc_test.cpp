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
    testStaleReap();
    testForgedRetirement();
    testAdoptedPoolLiveness();

    banner("8. /dev/shm is clean");
    cleanupShm();
    const int leftover = countNxTaktShm();
    CHECK(leftover == 0, "no nxtakt region left in /dev/shm (found %d)", leftover);

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
