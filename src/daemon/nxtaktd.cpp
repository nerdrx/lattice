// nxtaktd — the NxTakt engine daemon.
//
// Phase 1 of the process split (docs/PROCESS-SPLIT.md §6, brought forward from
// phase 4): a headless process that owns an Engine and an audio backend and
// exposes them through one shared-memory control region. Phase 2 added the
// sample pool, so it can now actually play clips. Nothing in src/ui talks to it
// yet, so this binary is currently exercised by tests/daemon_test.cpp and by
// hand.
//
//   nxtaktd [--session NAME] [--driver null|auto|jack|alsa]
//            [--rate HZ] [--block FRAMES] [--verbose]
//
// THE ONE PLACE A NUMBER BECOMES A POINTER
// ----------------------------------------
// The daemon's phase-2 job is small and sharp: a clip arrives as a WireClip in
// the control region whose sample data is a `u64` byte offset into the pool the
// GUI created, and the daemon turns that offset into `const f32*` for
// Engine::pushCommand. Engine never learns anything changed — no header in
// src/audio moved, no field means something new — which is exactly what makes
// the translation reviewable: there is one function (translateClip) with one
// job, and everything that could go wrong with an untrusted offset has to go
// wrong there. Bounds, alignment, block magic, block kind, block state and
// declared size are all checked before the addition happens, and a failure is a
// refusal with a reason, never a clamped pointer.
//
// Three threads:
//
//   audio     Engine::process(). A real backend's callback thread, or the null
//             driver's cadence thread. Never touches the region.
//   pump      main(). Drains the command ring into the engine, the engine's
//             events into the event ring, and the MIDI ring into the engine.
//             This is the thread that plays the GUI's role in the in-process
//             build, which is why the engine's single-producer/single-consumer
//             ring contract is preserved unchanged.
//   mirror    Copies Engine's published atomics into SharedState every ~4 ms
//             and stamps the heartbeat.
//
// WHY THE MIRROR IS NOT ON THE AUDIO THREAD
// -----------------------------------------
// Engine::publish() runs at the end of every block and writes ~1.4 KiB of
// relaxed atomics into the engine's own address space. Making it write into
// shared memory instead is phase 1 of the doc's plan and it is the right end
// state, but it means editing src/audio — which this wave does not own. So the
// daemon mirrors instead: same values, one copy later, at a cadence (4 ms)
// finer than any display refresh. The cost is one memcpy-ish pass per 4 ms on
// a non-RT thread; the benefit is that src/audio is untouched and the GUI-side
// protocol is already the final one.
// PHASE 3: THE PLUGINS MOVE IN
// ----------------------------
// The daemon now links src/plugin and owns every PluginInstance in the system
// (§3.6). Three consequences shape everything below:
//
//   * `RtChain::fx` stops crossing, because it has nowhere to cross to. The
//     client says AddDevice{track, uri}; the daemon instantiates, builds an
//     RtChain out of *its own* instances, and pushes Cmd::SetChain. The
//     pointer-swap-and-retire dance §2.5 describes is kept verbatim — it was
//     always the right pattern, it just stopped being a cross-process concern.
//   * PluginRegistry::scan() runs here, on a short-lived thread, lazily, on the
//     first device command. It is slow (lilv walks every bundle on the system)
//     and device commands that arrive while it runs are queued rather than
//     refused.
//   * setParam() is called from the pump thread. See "the param table" below
//     for why that is the same guarantee host.h documents and not a weaker one.
#include "../audio/backend.h"
#include "../audio/engine.h"
#include "../core/common.h"
#include "../ipc/control.h"
#include "../ipc/pool.h"
#include "../plugin/host.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <time.h>
#include <unistd.h>

namespace lat {
namespace {

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------
//
// SIGTERM/SIGINT set a flag; the pump loop notices within a tick and runs the
// ordinary shutdown path (stop the backend, publish the flag, unlink). A fatal
// signal is different: there is no ordinary path left, so the handler unlinks
// the region name and re-raises. shm_unlink() is async-signal-safe, and the
// alternative — leaving an orphan — makes the *next* daemon's create() take
// the stale-reap path and mask real bugs.
volatile sig_atomic_t gQuit = 0;
char gRegionName[128] = {};
volatile sig_atomic_t gOwnsRegion = 0;

void onQuit(int) { gQuit = 1; }

void onFatal(int sig) {
    if (gOwnsRegion && gRegionName[0]) ::shm_unlink(gRegionName);
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

void installSignals() {
    ::signal(SIGTERM, onQuit);
    ::signal(SIGINT,  onQuit);
    ::signal(SIGHUP,  onQuit);
    ::signal(SIGPIPE, SIG_IGN);
    for (int s : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}) ::signal(s, onFatal);
}

// ---------------------------------------------------------------------------
// The null driver
// ---------------------------------------------------------------------------
//
// No audio device: a thread that calls Engine::process() at the block cadence
// against CLOCK_MONOTONIC and throws the samples away. It exists for CI, for
// tests, and for "does the transport still run" debugging on a machine whose
// sound server is busy.
//
// Timing jitter is fine and expected — this is not an audio device and nothing
// downstream cares when a block happens. What *is* required is that the engine
// advances at the right average rate, because the beat clock is derived from
// frames rendered, so a wall-clock-referenced beat is the correctness property
// the tests measure. That is why the loop is deadline-based rather than
// sleep-for-a-block: after a scheduling hiccup it renders the blocks it owes
// (up to kMaxCatchUp at once) instead of quietly losing musical time.
//
// A backlog larger than kMaxCatchUp means something pathological — a suspend,
// a stopped process, an overloaded machine. Rendering it would be a burst of
// hundreds of blocks that starves the pump thread, so the loop resynchronises
// its origin instead and logs. Losing time loudly beats a death spiral.
class NullDriver {
public:
    static constexpr u64 kMaxCatchUp = 32;

    bool start(Engine& e, f64 sampleRate, int block) {
        engine_ = &e;
        sr_     = sampleRate;
        block_  = block;
        l_.assign((size_t)block_, 0.f);
        r_.assign((size_t)block_, 0.f);
        engine_->prepare(sr_, block_);
        run_.store(true, std::memory_order_release);
        thread_ = std::thread(&NullDriver::loop, this);
        LOGI("null driver up: %.0f Hz, %d frames (no audio device)", sr_, block_);
        return true;
    }

    void stop() {
        if (!run_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    u64 blocks() const { return blocks_.load(std::memory_order_relaxed); }

private:
    void loop() {
        const u64 blockNs = (u64)((f64)block_ / sr_ * 1e9);
        const u64 origin0 = ipc::monotonicNs();
        u64 origin   = origin0;
        u64 rendered = 0;

        while (run_.load(std::memory_order_relaxed)) {
            const u64 now = ipc::monotonicNs();
            const u64 due = now >= origin ? (now - origin) / blockNs + 1 : 1;

            if (due > rendered) {
                u64 want = due - rendered;
                if (want > kMaxCatchUp) {
                    LOGW("null driver fell %llu blocks behind, resynchronising",
                         (unsigned long long)want);
                    rendered = due - 1;
                    want     = 1;
                }
                for (u64 i = 0; i < want; ++i) {
                    engine_->process(nullptr, nullptr, l_.data(), r_.data(), block_);
                    ++rendered;
                    blocks_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Absolute deadline, so the sleep's own overshoot does not
            // accumulate into the beat clock.
            const u64 next = origin + rendered * blockNs;
            timespec ts{(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
            ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        }
    }

    Engine*          engine_ = nullptr;
    f64              sr_     = 48000.0;
    int              block_  = 256;
    std::vector<f32> l_, r_;
    std::thread      thread_;
    std::atomic<bool> run_{false};
    std::atomic<u64>  blocks_{0};
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    const char* session = "default";
    const char* driver  = nullptr;   // null / auto / jack / alsa
    f64  rate    = 48000.0;          // null driver only; a device dictates its own
    int  block   = 256;              // ditto
    bool verbose = false;
};

void usage() {
    std::printf(
        "nxtaktd — the NxTakt engine daemon\n"
        "\n"
        "  --session NAME     session id; the control region is /nxtakt-engine-NAME\n"
        "                     (default: $NXTAKT_SESSION, else \"default\")\n"
        "  --driver KIND      null | auto | jack | alsa   (default: $NXTAKT_AUDIO, else auto)\n"
        "                     ($LATTICE_SESSION / $LATTICE_AUDIO are still read too)\n"
        "                     null renders at block cadence with no audio device\n"
        "  --rate HZ          null driver sample rate (default 48000)\n"
        "  --block FRAMES     null driver block size   (default 256)\n"
        "  --verbose          log every rejected command\n"
        "  --help\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto value = [&](const char** out) {
            if (i + 1 >= argc) { LOGE("%s needs a value", a); return false; }
            *out = argv[++i];
            return true;
        };
        if (!std::strcmp(a, "--session")) { if (!value(&o.session)) return false; }
        else if (!std::strcmp(a, "--driver")) { if (!value(&o.driver)) return false; }
        else if (!std::strcmp(a, "--rate")) {
            const char* v = nullptr;
            if (!value(&v)) return false;
            o.rate = std::strtod(v, nullptr);
        } else if (!std::strcmp(a, "--block")) {
            const char* v = nullptr;
            if (!value(&v)) return false;
            o.block = (int)std::strtol(v, nullptr, 10);
        } else if (!std::strcmp(a, "--verbose")) { o.verbose = true; }
        else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) { usage(); return false; }
        else { LOGE("unknown option '%s'", a); usage(); return false; }
    }
    if (o.rate < 8000.0 || o.rate > 384000.0) { LOGE("--rate %.0f out of range", o.rate); return false; }
    if (o.block < 16 || o.block > kMaxBlock)  { LOGE("--block %d out of range", o.block); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// The daemon
// ---------------------------------------------------------------------------

class Daemon {
public:
    explicit Daemon(const Options& o) : opt_(o), engine_(new Engine) {}

    int run() {
        installSignals();

        // 1. Claim the name. create() is O_CREAT|O_EXCL and reclaims only a
        //    region whose creator is provably gone, so two daemons racing for
        //    one session resolve here: the loser exits and its GUI's retry
        //    loop finds the winner (§4.1).
        ipc::controlRegionName(opt_.session, gRegionName, sizeof gRegionName);
        ipc::ShmRegion::reapIfStale(gRegionName);
        if (!region_.create(gRegionName, ipc::control::kBytes, ipc::control::kHash)) {
            LOGE("cannot create the control region: %s", region_.error());
            LOGE("another nxtaktd may already own session '%s'", opt_.session);
            return 1;
        }
        gOwnsRegion = 1;
        if (!map_.create(region_)) {
            LOGE("control region layout does not fit its own size — build mismatch");
            return 1;
        }

        // 2. Bring up audio. Between here and publishReady() the region exists
        //    but is not ready, so no attacher can see a half-built one; if the
        //    driver fails we unlink on the way out and the GUI sees a clean
        //    "no engine" rather than a wedged one.
        if (!startDriver()) return 1;

        // 3. Initialise the payload and open for business.
        map_.state->init(sr_, (u32)block_);
        map_.state->engineState.store(ipc::SharedState::StateRunning, std::memory_order_relaxed);
        map_.hdr->init((i32)::getpid(), nullDriver_ != nullptr, driverName_);
        region_.publishReady();
        LOGI("nxtaktd ready: session '%s', region %s, %.0f Hz / %d frames, driver %s",
             opt_.session, gRegionName, sr_, block_, driverName_);

        // 4. Serve.
        mirrorRun_.store(true, std::memory_order_release);
        mirror_ = std::thread(&Daemon::mirrorLoop, this);
        pumpLoop();

        // 5. Shut down in the order the doc requires (§4.5): tell the client
        //    first, then stop rendering, then drop the region.
        return shutdown();
    }

private:
    // A translated command plus what the boundary has to remember about it
    // until the engine has actually taken it. Declared up here because member
    // function *signatures* below need the type complete.
    struct Staged {
        Command       cmd{};
        ipc::WireClip cell{};
        bool          pooled  = false;
        bool          isClear = false;
    };

    // A pool block that no clip cell references any more, waiting for the
    // proof that the audio thread has drained past the command that displaced
    // it. See "the free-after-confirm rule, daemon side" below.
    struct Retire {
        u64  ref        = 0;
        u32  kind       = 0;
        i32  track      = 0, slot = 0;
        u64  dueDrains  = 0;      // exact: Engine::drains must reach this
        u64  dueNs      = 0;      // legacy fallback, see drainsExact_
        u64  dueBlocks  = 0;
        bool confirmed  = false;  // the engine proved it (Ev::NotesRetired)
    };

    // -- devices ------------------------------------------------------------

    // One loaded plugin. The instance lives here and nowhere else: this is
    // §2.5's `DeviceModel::inst` after it moved across the process boundary,
    // and the client's half of it is a number.
    struct Device {
        std::unique_ptr<PluginInstance> inst;
        u32 id         = 0;
        u32 generation = 0;                 // +1 per allocation of this slot
        u32 target     = ipc::DevTargetTrack;
        int targetIdx  = 0;
        int paramCount = 0;                 // clamped to kMaxDevParams
        u32 seenParamGen = 0;               // last WireDeviceParams::generation applied
        f32 cached[ipc::kMaxDevParams] = {};
    };

    // One chain: the order the client asked for, and the RtChain the engine is
    // currently holding for it.
    struct Chain {
        std::vector<u32>         order;      // device ids, in processing order
        std::unique_ptr<RtChain> published;  // what the engine holds; owned here
    };

    // A chain publication waiting for room in Engine's command ring. Chains
    // queue rather than drop for the same reason commands do (§5): a lost
    // SetChain is a track that silently loses its plugins.
    struct ChainPush {
        Command                  cmd{};
        std::unique_ptr<RtChain> old;    // displaced; freed once the proof lands
        std::vector<std::unique_ptr<PluginInstance>> dying;
    };

    // The retirement pattern §2.5 describes, kept verbatim and moved inside one
    // process. The audio thread never frees a chain or an instance; the pump
    // does, and only once it can prove the engine has drained past the swap.
    struct ChainRetire {
        std::unique_ptr<RtChain> chain;
        std::vector<std::unique_ptr<PluginInstance>> dying;
        const RtChain* watch = nullptr;   // matched against Ev::ChainRetired
        u64  dueDrains = 0;
        u64  dueNs     = 0;
        bool confirmed = false;
    };

    // -- startup ------------------------------------------------------------

    bool startDriver() {
        const char* want = opt_.driver;
        if (!want) want = env("AUDIO");   // same knob the GUI honours

        if (want && !std::strcmp(want, "null")) {
            nullDriver_ = std::make_unique<NullDriver>();
            if (!nullDriver_->start(*engine_, opt_.rate, opt_.block)) {
                LOGE("null driver failed to start");
                return false;
            }
            sr_    = opt_.rate;
            block_ = opt_.block;
            std::snprintf(driverName_, sizeof driverName_, "null");
            return true;
        }

        // "auto" is createBackend's own null-means-auto convention.
        const char* prefer = (want && std::strcmp(want, "auto")) ? want : nullptr;
        backend_ = createBackend(*engine_, prefer);
        if (!backend_) {
            LOGE("no audio backend available (tried %s)", prefer ? prefer : "JACK then ALSA");
            LOGE("use --driver null for a headless engine with no audio device");
            return false;
        }
        sr_    = backend_->sampleRate();
        block_ = backend_->bufferSize();
        std::snprintf(driverName_, sizeof driverName_, "%s", backend_->name());
        return true;
    }

    // -- the pump -----------------------------------------------------------
    //
    // One tick: commands in, MIDI in, events out, heartbeat. 1 ms is well
    // inside a block at any sane buffer size, so a command never waits more
    // than one audio block longer than it would have in-process.

    void pumpLoop() {
        while (!gQuit) {
            observeDrains();
            pumpPool();
            pumpCommands();
            pumpDeviceQueue();
            pumpChainPushes();
            pumpParams();
            pumpMidi();
            pumpEvents();
            pumpRetirements();
            pumpChainRetirements();
            map_.hdr->heartbeat.fetch_add(1, std::memory_order_relaxed);
            timespec ts{0, 1000000};        // 1 ms
            ::nanosleep(&ts, nullptr);
        }
    }

    // -- the drain counter --------------------------------------------------
    //
    // Engine::drains is bumped at the END of every drainCommands(), which is
    // exactly the observation phase 2 wanted and could not have
    // (docs/PROCESS-SPLIT.md §10.3: "the exact version needs Engine to publish
    // a drain counter"). Republished into ControlHeader so that a client — or a
    // test — can tell an engine that counts from one that does not, because the
    // daemon's retirement behaviour differs between the two and a silent
    // difference is worse than a slow one.
    void observeDrains() {
        const u64 d = engine_->drains.load(std::memory_order_acquire);
        map_.hdr->engineDrains.store(d, std::memory_order_relaxed);
        if (!drainsExact_ && d > 0) {
            drainsExact_ = true;
            map_.hdr->drainsExact.store(1, std::memory_order_release);
            LOGI("Engine counts its command drains: retirement is exact, "
                 "not deadline-based");
        }
    }

    // THE PROOF, AND ITS OFF-BY-ONE
    //
    // Call this immediately after a successful Engine::pushCommand and keep the
    // number; the command is provably consumed once `drains` has reached it.
    //
    // Why +2 and not +1. Let P be the moment the push's release store lands and
    // R > P the moment we read `drains` here, giving d0. By definition drain
    // #d0 completed before R. Drain #(d0+1) completes after R — but it may have
    // *started* before P, in which case it never saw our command. Drain #(d0+2)
    // cannot start until #(d0+1) has finished, which is after R and therefore
    // after P, so it must observe the push. Two is the smallest safe number,
    // and it is a proof rather than a guess: no clock is involved, and a wedged
    // backend simply never satisfies it — which is the correct behaviour, since
    // a wedged backend has not drained anything.
    //
    // Cost: at most two block periods (~10.7 ms at 256 frames / 48 kHz), versus
    // phase 2's max(100 ms, 8 blocks).
    u64 drainProof() const {
        return engine_->drains.load(std::memory_order_acquire) + 2;
    }
    bool drainProven(u64 due) const {
        return engine_->drains.load(std::memory_order_acquire) >= due;
    }

    // -- the sample pool ----------------------------------------------------
    //
    // The GUI creates the pool and announces it here (ControlHeader::poolEpoch,
    // §3.5). Attaching is an mmap, so it happens on this thread and never on
    // the audio thread — the doc's rule about growth applies just as much to
    // the first mapping as to a later one.
    //
    // A pool is mapped ONCE per daemon lifetime. That is a real restriction and
    // it is deliberate: every RtClip the engine holds is a raw pointer into
    // this mapping, so unmapping it to attach a different one would turn every
    // live clip into a dangling read on the audio thread — and there is no
    // handshake in phase 2 that could prove the engine has dropped them all
    // first. A GUI that genuinely needs a different pool restarts the engine,
    // which costs a respawn and is honest. Re-announcing the *same* epoch is a
    // no-op, which is what makes publishPool() idempotent for the client.
    void pumpPool() {
        const u64 epoch = map_.hdr->poolEpoch.load(std::memory_order_acquire);
        if (epoch == 0 || epoch == poolEpoch_) return;
        if (pool_.valid()) {
            if (!poolRebindLogged_) {
                poolRebindLogged_ = true;
                LOGW("ignoring pool epoch %llu: '%s' is already mapped and live clips "
                     "point into it (restart the engine to change pools)",
                     (unsigned long long)epoch, pool_.name());
            }
            map_.hdr->poolAttachFailures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (ipc::monotonicNs() < poolRetryNs_) return;

        // Snapshot the name: it is the peer's memory and the peer may rewrite
        // it. Sized to hold ControlHeader::poolName in full, because a name
        // truncated here would open a *different* region, not fail.
        char nm[sizeof map_.hdr->poolName + 1];
        std::snprintf(nm, sizeof nm, "%s", map_.hdr->poolName);
        if (!pool_.attach(nm)) {
            // Back off rather than hammering shm_open every millisecond: the
            // usual cause is that the GUI has not called publishReady() yet.
            poolRetryNs_ = ipc::monotonicNs() + 100ull * 1000000ull;
            map_.hdr->poolAttachFailures.fetch_add(1, std::memory_order_relaxed);
            if (!poolFailLogged_) {
                poolFailLogged_ = true;
                LOGW("cannot attach the sample pool: %s [further attempts silent]",
                     pool_.error());
            }
            return;
        }
        poolEpoch_ = epoch;
        map_.hdr->poolAttachedEpoch.store(epoch, std::memory_order_release);
        LOGI("sample pool '%s' mapped read-only: %zu B, epoch %llu",
             pool_.name(), pool_.bytes(), (unsigned long long)epoch);
        ipc::WireEvent e{};
        e.type = ipc::EvPoolAttached;
        e.ref  = epoch;
        e.x    = (f64)pool_.bytes();
        map_.evts->push(e);
    }

    // Translates and validates each wire command, then hands it to the engine.
    //
    // Backpressure is handled by *not* consuming: if Engine's own command ring
    // is full the translated command is parked in pending_ and retried next
    // tick, so a burst is delayed rather than dropped. Dropping would lose user
    // intent silently, which docs/PROCESS-SPLIT.md §5 calls out as the thing
    // phase 1 owes.
    void pumpCommands() {
        if (havePending_) {
            if (!engine_->pushCommand(pending_.cmd)) return;
            commit(pending_);
            havePending_ = false;
        }
        ipc::WireCommand w;
        while (map_.cmds->pop(w)) {
            // Device commands are not translated and never reach the engine —
            // the daemon *executes* them, and what the engine hears about is
            // the Cmd::SetChain that comes out the other end. They are queued
            // rather than applied here because the first one triggers a plugin
            // scan that takes about a second, and the pump must keep beating
            // through it (§11.6).
            if (ipc::commandIsDevice(w.type)) { enqueueDevice(w); continue; }

            Staged st{};
            u32 reason = ipc::RejectNone;
            if (!translate(w, st, reason)) {
                reject(w, reason);
                if (ipc::commandIsPooled(w.type)) ackClip(w, reason);
                continue;
            }
            if (!engine_->pushCommand(st.cmd)) {
                pending_     = st;
                havePending_ = true;
                map_.hdr->commandsDeferred.fetch_add(1, std::memory_order_relaxed);
                return;                      // resume from here next tick
            }
            commit(st);
        }
    }

    // Everything that happens once a command is definitely in the engine's
    // ring. Split out of pumpCommands because a deferred command reaches this
    // point a tick later and must have exactly the same effects — the clip
    // shadow and the retirement clock in particular, which key off "the engine
    // has it", not "the client sent it".
    void commit(const Staged& st) {
        map_.hdr->commandsApplied.fetch_add(1, std::memory_order_relaxed);
        if (!st.pooled) return;
        installClip(st.cmd.a, st.cmd.b, st.cell);
        map_.hdr->clipsApplied.fetch_add(1, std::memory_order_relaxed);
        ipc::WireEvent e{};
        e.type  = ipc::EvClipAck;
        e.a     = st.cmd.a;
        e.b     = st.cmd.b;
        e.ref   = st.cell.generation;
        e.flags = st.isClear ? ipc::ClipAckWasClear : 0u;
        map_.evts->push(e);
    }

    // The acknowledgement a *refused* clip command still owes its sender. One
    // of these answers every SetClip/ClearClip, whatever happened to it,
    // because the client blocks further writes to a cell until it hears back
    // (src/ipc/control.h, WireClip::generation) and a silent refusal would wedge
    // that cell for the rest of the session.
    void ackClip(const ipc::WireCommand& w, u32 reason) {
        ipc::WireEvent e{};
        e.type  = ipc::EvClipAck;
        e.a     = w.a;
        e.b     = w.b;
        e.ref   = w.ref;                  // the generation the client sent
        e.flags = ipc::ClipAckRefused | ((Cmd)w.type == Cmd::ClearClip ? ipc::ClipAckWasClear : 0u);
        e.x     = (f64)reason;
        map_.evts->push(e);
    }

    // Scalars pass straight through; SetClip/ClearClip go via the clip table
    // and the pool; SetChain and the two Record commands still carry GUI-heap
    // pointers and are still refused at the boundary rather than
    // half-translated, because a SetChain whose `p` was silently zeroed is a
    // track that loses its plugins, and you find that on stage.
    bool translate(const ipc::WireCommand& w, Staged& out, u32& reason) {
        if (!ipc::commandIsKnown(w.type)) { reason = ipc::RejectUnknownCommand; return false; }
        if (ipc::commandIsPooled(w.type)) return translateClip(w, out, reason);
        if (!ipc::commandIsScalar(w.type)) { reason = ipc::RejectPointerPayload; return false; }
        if (!std::isfinite(w.x)) { reason = ipc::RejectNotFinite; return false; }

        Command& c  = out.cmd;
        const Cmd t = (Cmd)w.type;
        // Bounds-check indices at drain time (§5). The engine checks too, but a
        // peer's wild index should never get as far as the audio thread, and a
        // refused command tells the sender something a silently-ignored one
        // does not.
        const bool needsTrack =
            t == Cmd::LaunchClip || t == Cmd::StopTrack || t == Cmd::TrackVol ||
            t == Cmd::TrackPan   || t == Cmd::TrackMute || t == Cmd::TrackSolo ||
            t == Cmd::TrackArm   || t == Cmd::ClipGain  || t == Cmd::ClipWarp ||
            t == Cmd::ClipLoop;
        const bool needsSlot =
            t == Cmd::LaunchClip || t == Cmd::ClipGain || t == Cmd::ClipWarp ||
            t == Cmd::ClipLoop;
        if (needsTrack && (w.a < 0 || w.a >= kMaxTracks)) { reason = ipc::RejectBadIndex; return false; }
        if (needsSlot  && (w.b < 0 || w.b >= kMaxScenes)) { reason = ipc::RejectBadIndex; return false; }
        if (t == Cmd::LaunchScene && (w.a < 0 || w.a >= kMaxScenes)) {
            reason = ipc::RejectBadIndex;
            return false;
        }

        c.type = t;
        c.a    = w.a;
        c.b    = w.b;
        c.x    = w.x;
        c.p    = nullptr;                // no raw pointer ever crosses the wire
        return true;
    }

    // -- clips --------------------------------------------------------------
    //
    // THE VALIDATION CONTRACT
    //
    // Everything below runs on a WireClip the peer wrote into shared memory. It
    // is not input from a trusted library; it is whatever another process last
    // stored at that address, which under a crashed or compromised GUI is any
    // 120 bytes at all. The rule that follows from that is absolute and it is
    // the reason this function is long:
    //
    //     no path through here may produce an RtClip whose `data` or `notes`
    //     is anything other than a pointer into the mapped pool, backed by a
    //     block that is allocated, of the right kind, and at least as large as
    //     the clip says it will read.
    //
    // So the offsets go through poolValidate() (bounds, alignment, self-mixed
    // block magic, block state and size — src/ipc/pool.h) and the *scalars* go
    // through range checks here, because they are what the engine multiplies
    // the pointer by. `frames * channels` is the read extent; a wild
    // `loopEnd` walks `fetch()` off the end just as effectively as a wild
    // offset would. Engine clamps some of these and the doc's §5 row says a bad
    // offset "never reaches a voice" — this is where that is made true.
    bool translateClip(const ipc::WireCommand& w, Staged& out, u32& reason) {
        if (w.a < 0 || w.a >= kMaxTracks || w.b < 0 || w.b >= kMaxScenes) {
            reason = ipc::RejectBadIndex;
            return false;
        }
        const ipc::WireClip* cell = map_.clip(w.a, w.b);
        if (!cell) { reason = ipc::RejectBadIndex; return false; }

        out.pooled  = true;
        out.cmd.a   = w.a;
        out.cmd.b   = w.b;
        out.cmd.p   = nullptr;

        if ((Cmd)w.type == Cmd::ClearClip) {
            // Nothing to validate: clearing names no memory. The cell is not
            // even read — an empty shadow entry is what "cleared" means.
            out.cmd.type = Cmd::ClearClip;
            out.isClear  = true;
            out.cell     = ipc::WireClip{};
            out.cell.generation = (u32)w.ref;
            return true;
        }

        // Snapshot first. The client may write the cell again the moment it
        // sees the acknowledgement, so every check below and the RtClip that
        // comes out of them must be reading one consistent copy, not the live
        // shared memory.
        const ipc::WireClip c = *cell;
        out.cmd.type = Cmd::SetClip;
        out.cell     = c;

        const f64 scalars[] = {c.clipBpm, c.lengthBeats, c.prob, c.followBeats, (f64)c.gain};
        for (f64 v : scalars) if (!std::isfinite(v)) { reason = ipc::RejectNotFinite; return false; }

        if (c.channels < 1 || c.channels > 2)                    { reason = ipc::RejectBadClip; return false; }
        if (c.frames < 0 || c.noteCount < 0)                     { reason = ipc::RejectBadClip; return false; }
        if (c.warp < 0 || c.warp > (i32)Warp::Beats)             { reason = ipc::RejectBadClip; return false; }
        if (c.followAction < 0 || c.followAction >= kFollowCount){ reason = ipc::RejectBadClip; return false; }
        if (c.quantumIdx < -1 || c.quantumIdx >= kQuantumCount)  { reason = ipc::RejectBadClip; return false; }
        if (c.clipBpm <= 0.0 || c.lengthBeats < 0.0)             { reason = ipc::RejectBadClip; return false; }
        if (c.prob < 0.0 || c.prob > 1.0)                        { reason = ipc::RejectBadClip; return false; }
        if (c.noteCount > (i64)INT32_MAX)                        { reason = ipc::RejectBadClip; return false; }

        // An invalid cell is a legal thing to publish — it is how a GUI parks
        // an empty slot — and it references nothing, so it skips the pool
        // entirely. Anything that *does* reference the pool needs one mapped.
        if ((c.sampleRef || c.notesRef) && !pool_.valid()) {
            reason = ipc::RejectNoPool;
            return false;
        }

        RtClip rc{};
        if (c.valid) {
            if (c.isMidi) {
                if (!c.notesRef || c.noteCount <= 0) { reason = ipc::RejectBadClip; return false; }
                if (c.lengthBeats <= 0.0)            { reason = ipc::RejectBadClip; return false; }
            } else {
                if (!c.sampleRef || c.frames <= 0)   { reason = ipc::RejectBadClip; return false; }
                // The loop window is a read range, so it is bounded like one.
                if (c.loopStart < 0 || c.loopEnd < c.loopStart || c.loopEnd > c.frames) {
                    reason = ipc::RejectBadClip;
                    return false;
                }
            }
        }

        if (c.sampleRef) {
            const u64 need = (u64)c.frames * (u64)c.channels * sizeof(f32);
            const char* why = "";
            if (!pool_.validate(c.sampleRef, ipc::PoolKindSamples, need, &why)) {
                logBadRef("sample", c.sampleRef, w.a, w.b, why);
                reason = ipc::RejectBadPoolRef;
                return false;
            }
            rc.data = (const f32*)pool_.at(c.sampleRef);
        }
        if (c.notesRef) {
            const u64 need = (u64)c.noteCount * sizeof(ipc::WireNote);
            const char* why = "";
            if (!pool_.validate(c.notesRef, ipc::PoolKindNotes, need, &why)) {
                logBadRef("notes", c.notesRef, w.a, w.b, why);
                reason = ipc::RejectBadPoolRef;
                return false;
            }
            // WireNote is asserted to mirror RtNote field for field (pool.h),
            // so this is a reinterpretation and not a conversion — which is
            // what keeps a 10 000-note clip free at the boundary.
            rc.notes = (const RtNote*)pool_.at(c.notesRef);
        }

        rc.frames       = c.frames;
        rc.channels     = (int)c.channels;
        rc.loopStart    = c.loopStart;
        rc.loopEnd      = c.loopEnd;
        rc.clipBpm      = c.clipBpm;
        rc.lengthBeats  = c.lengthBeats;
        rc.gain         = c.gain;
        rc.warp         = (int)c.warp;
        rc.loop         = c.loop != 0;
        rc.quantumIdx   = (int)c.quantumIdx;
        rc.prob         = c.prob;
        rc.followAction = (int)c.followAction;
        rc.followBeats  = c.followBeats;
        rc.noteCount    = (int)c.noteCount;
        rc.isMidi       = c.isMidi != 0;
        rc.valid        = c.valid != 0;
        out.cmd.clip    = rc;
        return true;
    }

    // Rate-limited like reject(): a GUI with a stale project can produce one of
    // these per clip, and a log line per clip would bury everything else.
    void logBadRef(const char* what, u64 ref, i32 t, i32 s, const char* why) {
        if (opt_.verbose || badRefLogged_ < kRejectLogLimit) {
            ++badRefLogged_;
            LOGW("clip [%d][%d]: %s offset %llu rejected: %s%s", (int)t, (int)s, what,
                 (unsigned long long)ref, why,
                 (!opt_.verbose && badRefLogged_ == kRejectLogLimit) ? " [further bad offsets silent]" : "");
        }
    }

    void reject(const ipc::WireCommand& w, u32 reason) {
        map_.hdr->commandsRejected.fetch_add(1, std::memory_order_relaxed);
        // Rate-limited by default: a GUI that has not been ported yet will send
        // SetClip on every project load, and a log line per clip would bury
        // everything else. --verbose logs them all.
        if (opt_.verbose || rejectLogged_ < kRejectLogLimit) {
            ++rejectLogged_;
            LOGW("command %u refused: %s (a=%d b=%d x=%g)%s",
                 w.type, ipc::rejectReasonName(reason), w.a, w.b, w.x,
                 (!opt_.verbose && rejectLogged_ == kRejectLogLimit) ? " [further rejects silent]" : "");
        }
        ipc::WireEvent e{};
        e.type = ipc::EvCommandRejected;
        e.a    = (i32)w.type;
        e.b    = (i32)reason;
        e.ref  = w.ref;
        map_.evts->push(e);              // a full event ring means the client is
                                         // not draining; nothing useful to do
    }

    void pumpMidi() {
        ipc::WireMidi w;
        while (map_.midi->pop(w)) {
            MidiMsg m{};
            m.status = w.status; m.d1 = w.d1; m.d2 = w.d2; m.frame = w.frame;
            if (!engine_->pushMidi(m)) return;   // ring full: the rest waits a tick
            map_.hdr->midiApplied.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // =======================================================================
    // Devices (docs/PROCESS-SPLIT.md §3.6, §3.7, §11)
    // =======================================================================

    // -- the scan -----------------------------------------------------------
    //
    // PluginRegistry::scan() walks every LV2 bundle on the system with lilv. It
    // takes the better part of a second on a normal install, it allocates
    // freely, and it is documented GUI-thread-only. So it runs once, lazily, on
    // the first device command, on a thread of its own — and the pump keeps
    // ticking, which is the property that matters: a pump that blocked for a
    // second would stop the heartbeat, and a client watching the heartbeat
    // would conclude the engine had wedged and start thinking about respawning
    // it. Losing the audio because we went looking for plugins would be a
    // remarkable own goal.
    //
    // The registry is still touched by exactly one thread at a time: the
    // scanning thread owns `scanned_` alone until it sets `scanDone_`, and the
    // pump joins before it reads. host.h's contract is honoured, not bent —
    // "GUI thread" in that file means "the one non-realtime thread that owns
    // this object", and here that is the pump, briefly delegated.
    //
    // §3.6 asks for a short-lived `nxtakt-scan` *child process*, so that a
    // plugin which segfaults during discovery takes down neither the GUI nor
    // the engine. That is strictly better and it is still deferred: it needs a
    // way to ship the catalog back, which is the socket.
    void startScan() {
        if (scanState_ != ipc::ScanIdle) return;
        scanState_ = ipc::ScanRunning;
        map_.hdr->scanState.store(ipc::ScanRunning, std::memory_order_release);
        scanStartNs_ = ipc::monotonicNs();
        scanDone_.store(false, std::memory_order_relaxed);
        scanned_ = std::make_unique<PluginRegistry>();
        LOGI("plugin scan started (first device command)");
        scanThread_ = std::thread([this] {
            scanned_->scan();
            scanDone_.store(true, std::memory_order_release);
        });
    }

    // True once the catalog is usable.
    bool registryReady() {
        if (scanState_ == ipc::ScanDone) return true;
        if (scanState_ != ipc::ScanRunning) return false;
        if (!scanDone_.load(std::memory_order_acquire)) return false;
        if (scanThread_.joinable()) scanThread_.join();
        registry_ = std::move(*scanned_);
        scanned_.reset();
        scanState_ = ipc::ScanDone;
        const u32 n = (u32)registry_.plugins().size();
        const f64 secs = (f64)(ipc::monotonicNs() - scanStartNs_) / 1e9;
        map_.hdr->scanPlugins.store(n, std::memory_order_relaxed);
        map_.hdr->scanState.store(ipc::ScanDone, std::memory_order_release);
        LOGI("plugin scan complete: %u plugins in %.2f s", n, secs);
        ipc::WireEvent e{};
        e.type = ipc::EvScanComplete;
        e.a    = (i32)n;
        e.x    = secs;
        map_.evts->push(e);
        return true;
    }

    // -- the device command queue -------------------------------------------
    //
    // Bounded, because it is fed by a peer. Overflow is answered rather than
    // dropped, exactly like every other refusal here.
    static constexpr size_t kDeviceQueueMax = 256;

    void enqueueDevice(const ipc::WireCommand& w) {
        if (w.type == ipc::CmdScanPlugins) { startScan(); return; }
        if (deviceQueue_.size() >= kDeviceQueueMax) {
            failDevice(w, ipc::RejectScanBusy);
            return;
        }
        deviceQueue_.push_back(w);
        startScan();                       // idempotent; the first one starts it
    }

    void pumpDeviceQueue() {
        if (deviceQueue_.empty()) { registryReady(); return; }
        if (!registryReady()) return;      // still scanning: everything waits, in order
        while (!deviceQueue_.empty()) {
            const ipc::WireCommand w = deviceQueue_.front();
            deviceQueue_.pop_front();
            switch (w.type) {
                case ipc::CmdAddDevice:    doAddDevice(w);    break;
                case ipc::CmdRemoveDevice: doRemoveDevice(w); break;
                case ipc::CmdMoveDevice:   doMoveDevice(w);   break;
                case ipc::CmdSetBypass:    doSetBypass(w);    break;
                default:                   break;             // CmdScanPlugins: done
            }
        }
    }

    // -- strings through the pool -------------------------------------------
    //
    // §3.2 says a ring cannot carry a plugin URI and that stretching it to do
    // so would be a mistake, and it is right — so the string travels in the one
    // shared region that already exists for variable-length payloads, and the
    // 32-byte command carries its offset. The blob goes through exactly the
    // same validation every sample offset does (pool.h, poolValidate), with one
    // extra rule that is specific to text: **the terminator must be inside the
    // block**. A "string" whose NUL is past its own allocation is not a string
    // that got truncated, it is a read off the end of somebody else's data.
    bool readPoolString(u64 ref, char* out, size_t cap) {
        if (cap) out[0] = '\0';
        if (!ref) return false;
        if (!pool_.valid()) return false;
        const char* why = "";
        if (!pool_.validate(ref, ipc::PoolKindString, 1, &why)) {
            logBadRef("string", ref, 0, 0, why);
            return false;
        }
        u64 n = pool_.block(ref)->bytes;
        if (n > ipc::kMaxPoolString) n = ipc::kMaxPoolString;
        const char* s = (const char*)pool_.at(ref);
        u64 len = 0;
        while (len < n && s[len]) ++len;
        if (len >= n) return false;                 // no terminator inside the block
        if (len + 1 > cap) return false;
        std::memcpy(out, s, (size_t)len + 1);
        return true;
    }

    // A string blob is retired the instant it has been copied. It never reaches
    // the engine and no voice can ever be inside it, so there is nothing to be
    // quiescent *of* — the whole drain-proof apparatus below simply does not
    // apply. Queuing it as already-confirmed keeps one code path for
    // publishing retirements (and its ring-full retry) instead of two.
    void retireString(u64 ref) {
        Retire r{};
        r.ref       = ref;
        r.kind      = ipc::PoolKindString;
        r.track     = -1;
        r.slot      = -1;
        r.confirmed = true;
        retiring_.push_back(r);
    }

    // -- the device table ---------------------------------------------------

    static constexpr u32 kNoDevice = ipc::kMaxDevices;

    Device* deviceById(u32 id) {
        return (id < ipc::kMaxDevices) ? devices_[id].get() : nullptr;
    }

    u32 allocDeviceSlot() {
        for (u32 i = 0; i < ipc::kMaxDevices; ++i)
            if (!devices_[i]) return i;
        return kNoDevice;
    }

    // Chains are indexed in one flat array: tracks, then returns, then master.
    // One array rather than three members because every operation below is
    // "whatever chain this device is on", and a three-way branch per operation
    // is three chances to get it wrong.
    static constexpr int kChainCount = kMaxTracks + kMaxReturns + 1;
    static constexpr int kMasterChainIdx = kMaxTracks + kMaxReturns;

    static int chainIndex(u32 target, int idx) {
        switch (target) {
            case ipc::DevTargetTrack:  return (idx >= 0 && idx < kMaxTracks) ? idx : -1;
            case ipc::DevTargetReturn: return (idx >= 0 && idx < kMaxReturns) ? kMaxTracks + idx : -1;
            case ipc::DevTargetMaster: return kMasterChainIdx;
            default:                   return -1;
        }
    }

    // -- AddDevice ----------------------------------------------------------

    void doAddDevice(const ipc::WireCommand& w) {
        // The URI blob first, and it is retired whatever happens next: a client
        // whose AddDevice was refused must not be left holding a pool block it
        // can never free. Exactly one retirement echo per blob, accepted or
        // refused — the same discipline EvClipAck established for cells.
        char uri[ipc::kMaxPoolString];
        const bool haveUri = readPoolString(w.ref, uri, sizeof uri);
        if (w.ref) retireString(w.ref);
        if (!haveUri) { failDevice(w, w.ref ? ipc::RejectBadString : ipc::RejectNoPool); return; }

        const u32 target = w.flags;
        const int idx    = (target == ipc::DevTargetMaster) ? 0 : w.a;
        const int ci     = chainIndex(target, idx);
        if (ci < 0) { failDevice(w, ipc::RejectBadIndex); return; }

        const PluginDesc* desc = registry_.find(std::string(uri));
        if (!desc) {
            logDevice("AddDevice: no plugin with URI '%s'", uri);
            failDevice(w, ipc::RejectUnknownUri);
            return;
        }

        Chain& ch = chains_[ci];
        if ((int)ch.order.size() >= kMaxChainFx) { failDevice(w, ipc::RejectChainFull); return; }

        const u32 id = allocDeviceSlot();
        if (id == kNoDevice) { failDevice(w, ipc::RejectDeviceTableFull); return; }

        // Slow, allocating, and able to fail or crash: precisely why it is here
        // on the pump thread and not anywhere near the audio callback.
        std::unique_ptr<PluginInstance> inst = registry_.instantiate(*desc, sr_, block_);
        if (!inst) {
            logDevice("AddDevice: '%s' failed to instantiate", uri);
            failDevice(w, ipc::RejectInstantiate);
            return;
        }
        if (!inst->prepare(sr_, block_)) {
            logDevice("AddDevice: '%s' refused %.0f Hz / %d frames", uri, sr_, block_);
            failDevice(w, ipc::RejectInstantiate);
            return;
        }

        auto dev = std::make_unique<Device>();
        dev->id         = id;
        dev->generation = ++deviceGen_[id];       // never reused: the param guard
        dev->target     = target;
        dev->targetIdx  = idx;
        dev->paramCount = inst->paramCount();
        if (dev->paramCount > (int)ipc::kMaxDevParams) dev->paramCount = (int)ipc::kMaxDevParams;
        for (int i = 0; i < dev->paramCount; ++i) dev->cached[i] = inst->getParam(i);
        dev->inst = std::move(inst);

        int pos = (w.b < 0 || w.b > (int)ch.order.size()) ? (int)ch.order.size() : w.b;
        ch.order.insert(ch.order.begin() + pos, id);

        publishDeviceInfo(*dev, *desc, pos);
        resetParamRow(*dev);
        devices_[id] = std::move(dev);
        liveIds_.push_back(id);

        publishChain(target, idx);
        renumberChain(target, idx, /*silentFor*/(int)id);

        map_.hdr->devicesAdded.fetch_add(1, std::memory_order_relaxed);
        map_.hdr->devicesLive.store((u64)liveIds_.size(), std::memory_order_relaxed);

        ipc::WireEvent e{};
        e.type  = ipc::EvDeviceAdded;
        e.flags = target;
        e.a     = idx;
        e.b     = pos;
        e.x     = (f64)devices_[id]->paramCount;
        e.ref   = id;
        map_.evts->push(e);
        LOGI("device %u: '%s' on %s %d at position %d (%d params, %d frames latency)",
             id, uri, ipc::devTargetName(target), idx, pos,
             devices_[id]->paramCount, devices_[id]->inst->latencyFrames());
    }

    void failDevice(const ipc::WireCommand& w, u32 reason) {
        map_.hdr->devicesFailed.fetch_add(1, std::memory_order_relaxed);
        map_.hdr->commandsRejected.fetch_add(1, std::memory_order_relaxed);
        ipc::WireEvent e{};
        e.type = ipc::EvDeviceFailed;
        e.a    = (i32)w.flags;
        e.b    = (i32)reason;
        e.x    = (f64)w.type;
        e.ref  = w.ref;
        map_.evts->push(e);
    }

    // -- RemoveDevice / MoveDevice / SetBypass ------------------------------

    void doRemoveDevice(const ipc::WireCommand& w) {
        const u32 id = (u32)w.ref;
        Device* d = deviceById(id);
        if (!d) { failDevice(w, ipc::RejectBadDevice); return; }

        const u32 target = d->target;
        const int idx    = d->targetIdx;
        const int ci     = chainIndex(target, idx);
        Chain& ch = chains_[ci];
        for (size_t i = 0; i < ch.order.size(); ++i)
            if (ch.order[i] == id) { ch.order.erase(ch.order.begin() + (long)i); break; }

        // Out of the live set *first*. The pump calls setParam on live devices,
        // and an instance on its way out must stop receiving them before the
        // unique_ptr moves — otherwise the last thing this function does is
        // race with the thing it is running on.
        for (size_t i = 0; i < liveIds_.size(); ++i)
            if (liveIds_[i] == id) { liveIds_.erase(liveIds_.begin() + (long)i); break; }
        std::unique_ptr<PluginInstance> dying = std::move(d->inst);
        map_.device(id)->state.store(ipc::DeviceSlotFree, std::memory_order_release);
        devices_[id].reset();

        // The instance is not destroyed here. It rides the same proof the
        // displaced chain does, because until the engine has drained past the
        // new chain the audio thread may still be inside the old one, and the
        // old one names this instance. This is App::retiring_, moved.
        publishChain(target, idx);
        chainPush_.back().dying.push_back(std::move(dying));
        renumberChain(target, idx, -1);

        map_.hdr->devicesRemoved.fetch_add(1, std::memory_order_relaxed);
        map_.hdr->devicesLive.store((u64)liveIds_.size(), std::memory_order_relaxed);

        ipc::WireEvent e{};
        e.type = ipc::EvDeviceRemoved;
        e.a    = (i32)target;
        e.b    = idx;
        e.ref  = id;
        map_.evts->push(e);
        LOGI("device %u removed from %s %d", id, ipc::devTargetName(target), idx);
    }

    void doMoveDevice(const ipc::WireCommand& w) {
        const u32 id = (u32)w.ref;
        Device* d = deviceById(id);
        if (!d) { failDevice(w, ipc::RejectBadDevice); return; }
        Chain& ch = chains_[chainIndex(d->target, d->targetIdx)];
        size_t from = ch.order.size();
        for (size_t i = 0; i < ch.order.size(); ++i) if (ch.order[i] == id) { from = i; break; }
        if (from == ch.order.size()) { failDevice(w, ipc::RejectBadDevice); return; }
        int to = w.b;
        if (to < 0) to = 0;
        if (to >= (int)ch.order.size()) to = (int)ch.order.size() - 1;
        if ((size_t)to == from) { emitChanged(id, ipc::DeviceChangedMoved, to, 0); return; }
        ch.order.erase(ch.order.begin() + (long)from);
        ch.order.insert(ch.order.begin() + to, id);
        publishChain(d->target, d->targetIdx);
        renumberChain(d->target, d->targetIdx, -1);
    }

    // Bypass is a plain bool store into the instance, read by process() without
    // synchronisation — the same contract as setParam and safe for the same
    // reasons (see "the param table" below). It is a command rather than a
    // table write because §3.7 says ordering matters for structural changes,
    // and it does: bypass-then-remove and remove-then-bypass are different.
    void doSetBypass(const ipc::WireCommand& w) {
        const u32 id = (u32)w.ref;
        Device* d = deviceById(id);
        if (!d || !d->inst) { failDevice(w, ipc::RejectBadDevice); return; }
        const bool on = w.a != 0;
        d->inst->setBypassed(on);
        if (ipc::WireDeviceInfo* row = map_.device(id))
            row->bypass.store(on ? 1u : 0u, std::memory_order_relaxed);
        emitChanged(id, ipc::DeviceChangedBypass, -1, on ? 1 : 0);
    }

    void emitChanged(u32 id, u32 flags, int pos, int a) {
        ipc::WireEvent e{};
        e.type  = ipc::EvDeviceChanged;
        e.flags = flags;
        e.a     = a;
        e.b     = pos;
        e.ref   = id;
        map_.evts->push(e);
    }

    // A chain edit moves everything after it. The table rows are updated in
    // place and each moved device gets one EvDeviceChanged, because a client
    // mirror that silently disagreed about processing order would draw the
    // chain in the wrong order and nothing would ever correct it.
    void renumberChain(u32 target, int idx, int silentFor) {
        Chain& ch = chains_[chainIndex(target, idx)];
        for (size_t i = 0; i < ch.order.size(); ++i) {
            const u32 id = ch.order[i];
            ipc::WireDeviceInfo* row = map_.device(id);
            if (!row) continue;
            if (row->chainPos == (i32)i) continue;
            row->chainPos = (i32)i;
            if ((int)id != silentFor) emitChanged(id, ipc::DeviceChangedMoved, (int)i, 0);
        }
    }

    // -- chain publication --------------------------------------------------
    //
    // Builds the RtChain the engine will hold and queues the swap. The daemon
    // owns both the chain struct and every instance in it, so this is §2.5's
    // protocol with the process boundary taken out of the middle — and that is
    // the entire content of "the chain protocol becomes id-based, and gets
    // simpler" (§3.6): there are no ids on the wire at all, because Add/Remove/
    // Move carry position and the daemon is the only thing that needs to know
    // what a chain looks like.
    void publishChain(u32 target, int idx) {
        Chain& ch = chains_[chainIndex(target, idx)];

        std::unique_ptr<RtChain> next;
        if (!ch.order.empty()) {
            next = std::make_unique<RtChain>();
            for (u32 id : ch.order) {
                Device* d = deviceById(id);
                if (d && d->inst && next->count < kMaxChainFx)
                    next->fx[next->count++] = d->inst.get();
            }
        }

        ChainPush p;
        p.cmd.type = target == ipc::DevTargetTrack  ? Cmd::SetChain
                   : target == ipc::DevTargetReturn ? Cmd::SetReturnChain
                                                    : Cmd::SetMasterChain;
        p.cmd.a = (target == ipc::DevTargetMaster) ? 0 : idx;
        p.cmd.p = (void*)next.get();
        p.old   = std::move(ch.published);
        ch.published = std::move(next);          // the daemon keeps ownership
        chainPush_.push_back(std::move(p));
    }

    void pumpChainPushes() {
        while (!chainPush_.empty()) {
            ChainPush& p = chainPush_.front();
            if (!engine_->pushCommand(p.cmd)) return;    // ring full: next tick
            map_.hdr->chainsPublished.fetch_add(1, std::memory_order_relaxed);

            ChainRetire r;
            r.watch     = p.old.get();
            r.dueDrains = drainProof();                 // read AFTER the push
            r.dueNs     = ipc::monotonicNs() + kRetireGraceNs;
            r.chain     = std::move(p.old);
            r.dying     = std::move(p.dying);
            chainPush_.pop_front();
            if (r.chain || !r.dying.empty()) chainRetire_.push_back(std::move(r));
        }
    }

    // Frees a displaced chain and the instances it was the last reference to.
    // Two proofs are accepted and either is enough:
    //
    //   * Ev::ChainRetired naming this exact pointer. It is pushed from inside
    //     drainCommands(), so its arrival *is* the drain — the same exactness
    //     Ev::NotesRetired gives the sample path.
    //   * Engine::drains reaching dueDrains (see drainProof). This one covers
    //     the chains the engine has no event for, and it is what makes the
    //     return and master chains safe on the day they land.
    void pumpChainRetirements() {
        if (chainRetire_.empty()) return;
        const u64 now = ipc::monotonicNs();
        size_t keep = 0;
        for (size_t i = 0; i < chainRetire_.size(); ++i) {
            ChainRetire& r = chainRetire_[i];
            const bool due = r.confirmed ||
                             (drainsExact_ ? drainProven(r.dueDrains) : now >= r.dueNs);
            if (!due) {
                if (keep != i) chainRetire_[keep] = std::move(r);
                ++keep;
                continue;
            }
            map_.hdr->chainsRetired.fetch_add(1, std::memory_order_relaxed);
            // r goes out of scope at the end of the loop body -> the RtChain and
            // every instance in `dying` are destructed here, on the pump thread,
            // provably outside anything the audio thread can reach.
            r.chain.reset();
            r.dying.clear();
        }
        chainRetire_.resize(keep);
    }

    void confirmChainRetire(const void* p) {
        for (ChainRetire& r : chainRetire_)
            if ((const void*)r.watch == p) { r.confirmed = true; return; }
    }

    // -- the param table ----------------------------------------------------
    //
    // WHY setParam ON THE PUMP THREAD IS THE CONTRACT, NOT A RELAXATION OF IT
    //
    // src/plugin/host.h documents setParam as "GUI thread, concurrent with
    // process()", and every backend implements it as a plain float store that
    // the plugin reads once per run(). That wording names the caller, but the
    // *guarantee* it describes has three parts and none of them is about which
    // thread it is:
    //
    //   1. it is not the audio thread — so it may not be inside process(), and
    //      it does not have to be realtime-safe itself;
    //   2. it writes a 4-byte-aligned scalar the reader loads without
    //      synchronisation, so a torn read is unrepresentable on every target
    //      we build for and a stale read costs at most one block;
    //   3. exactly one thread does it, so two writers cannot interleave on one
    //      parameter.
    //
    // The pump satisfies all three, identically. It is not the audio thread; it
    // is the same non-realtime thread that instantiated the plugin and that
    // will destroy it, so (3) is stronger here than it was in-process, where
    // setParam and instantiate were merely both "the GUI thread" by convention.
    // In-process, the GUI thread was simply the thread that happened to own the
    // plugin; here the pump is. Nothing about the guarantee changed — the name
    // of the thread did.
    //
    // What *is* new is that the values arrive from another process. That is
    // handled before setParam is reached: a non-finite float is dropped, and a
    // write stamped with a device generation the slot no longer has is dropped,
    // so a knob drag that was in flight when a device was removed cannot land
    // on its replacement.
    //
    // Cost when nothing is moving: one acquire load per live device per
    // millisecond. Cost when a knob is dragging: one compare and one store per
    // changed parameter.
    void pumpParams() {
        if (liveIds_.empty()) return;
        u64 writes = 0;
        for (u32 id : liveIds_) {
            Device* d = devices_[id].get();
            ipc::WireDeviceParams* p = map_.param(id);
            if (!d || !d->inst || !p) continue;
            const u32 g = p->generation.load(std::memory_order_acquire);
            if (g == d->seenParamGen) continue;
            d->seenParamGen = g;
            if (p->deviceGeneration.load(std::memory_order_relaxed) != d->generation) continue;
            for (int i = 0; i < d->paramCount; ++i) {
                const f32 v = p->value[i].load(std::memory_order_relaxed);
                if (!std::isfinite(v) || v == d->cached[i]) continue;
                d->cached[i] = v;
                d->inst->setParam(i, v);
                ++writes;
            }
        }
        if (writes) map_.hdr->paramWrites.fetch_add(writes, std::memory_order_relaxed);
    }

    // The device's metadata row: everything static about the instance, written
    // once. `state` goes last with release, so a client that sees the slot live
    // has seen every byte that describes it — the same publication rule
    // PoolBlock::magic uses, for the same reason.
    void publishDeviceInfo(const Device& dev, const PluginDesc& desc, int pos) {
        ipc::WireDeviceInfo* w = map_.device(dev.id);
        if (!w) return;
        w->state.store(ipc::DeviceSlotFree, std::memory_order_relaxed);
        w->generation.store(dev.generation, std::memory_order_relaxed);
        w->bypass.store(dev.inst->bypassed() ? 1u : 0u, std::memory_order_relaxed);
        w->paramCount      = (u32)dev.paramCount;
        const int total    = dev.inst->paramCount();
        w->truncatedParams = total > dev.paramCount ? (u32)(total - dev.paramCount) : 0u;
        w->target          = (i32)dev.target;
        w->targetIdx       = dev.targetIdx;
        w->chainPos        = pos;
        w->latencyFrames   = dev.inst->latencyFrames();
        w->format          = (u32)desc.format;
        w->kind            = (u32)desc.kind;
        w->audioIn         = (u32)desc.audioIn;
        w->audioOut        = (u32)desc.audioOut;
        w->hasMidiIn       = desc.hasMidiIn ? 1u : 0u;
        w->reserved[0] = w->reserved[1] = 0;
        copyFixed(w->uri,    sizeof w->uri,    desc.uri.c_str());
        copyFixed(w->name,   sizeof w->name,   desc.name.c_str());
        copyFixed(w->vendor, sizeof w->vendor, desc.vendor.c_str());
        for (int i = 0; i < dev.paramCount; ++i) {
            const ParamInfo& s = dev.inst->paramInfo(i);
            ipc::WireParamInfo& t = w->params[i];
            t.min   = s.min;
            t.max   = s.max;
            t.def   = s.def;
            t.id    = s.id;
            t.flags = (s.isBool ? ipc::ParamIsBool : 0u) |
                      (s.isInt  ? ipc::ParamIsInt  : 0u) |
                      (s.isLogarithmic ? ipc::ParamIsLog : 0u);
            t.reserved = 0;
            copyFixed(t.name, sizeof t.name, s.name.c_str());
            copyFixed(t.unit, sizeof t.unit, s.unit.c_str());
        }
        for (u32 i = (u32)dev.paramCount; i < ipc::kMaxDevParams; ++i)
            w->params[i] = ipc::WireParamInfo{};
        if (w->truncatedParams)
            LOGW("device %u ('%s') has %d controls; the table carries %u",
                 dev.id, desc.name.c_str(), total, (u32)ipc::kMaxDevParams);
        w->state.store(ipc::DeviceSlotLive, std::memory_order_release);
    }

    // The param row starts holding the plugin's own values, so a GUI that
    // attaches to a device it did not create draws the right knob positions
    // without asking. `generation` is reset because the row may have belonged
    // to a previous occupant; `deviceGeneration` is stamped so that a client
    // write which crosses this reset is judged against the right device.
    void resetParamRow(const Device& dev) {
        ipc::WireDeviceParams* p = map_.param(dev.id);
        if (!p) return;
        for (u32 i = 0; i < ipc::kMaxDevParams; ++i)
            p->value[i].store(i < (u32)dev.paramCount ? dev.cached[i] : 0.f,
                              std::memory_order_relaxed);
        p->deviceGeneration.store(dev.generation, std::memory_order_relaxed);
        p->generation.store(0, std::memory_order_relaxed);
        p->engineGeneration.fetch_add(1, std::memory_order_release);
    }

    static void copyFixed(char* dst, size_t cap, const char* src) {
        std::memset(dst, 0, cap);
        if (!src) return;
        std::snprintf(dst, cap, "%s", src);
    }

    void logDevice(const char* fmt, ...) {
        if (!opt_.verbose && deviceLogged_ >= kRejectLogLimit) return;
        ++deviceLogged_;
        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        LOGW("%s%s", msg,
             (!opt_.verbose && deviceLogged_ == kRejectLogLimit) ? " [further device failures silent]" : "");
    }

    // Everything the daemon owns that the engine might still be inside. Called
    // once the driver has stopped, when every proof is trivially satisfied.
    void destroyAllDevices() {
        for (Chain& ch : chains_) { ch.order.clear(); ch.published.reset(); }
        chainPush_.clear();
        chainRetire_.clear();
        liveIds_.clear();
        for (auto& d : devices_) d.reset();
        for (u32 i = 0; i < ipc::kMaxDevices; ++i)
            if (ipc::WireDeviceInfo* row = map_.device(i))
                row->state.store(ipc::DeviceSlotFree, std::memory_order_relaxed);
        map_.hdr->devicesLive.store(0, std::memory_order_relaxed);
    }

    // -- retirement ---------------------------------------------------------
    //
    // THE FREE-AFTER-CONFIRM RULE, DAEMON SIDE
    //
    // The client may return a pool block to its free list only after this
    // daemon has echoed the block's offset back in an EvBlockRetired event
    // (src/ipc/pool.h states the client's half). What the echo asserts is:
    //
    //   1. the command that displaced the block — a SetClip installing
    //      something else in that cell, or a ClearClip — has been handed to
    //      Engine::pushCommand. Not "sent by the client": handed over, which is
    //      why the entry is queued from commit() and not from translate();
    //   2. no other cell of the daemon's shadow clip table still names the
    //      offset (a block may legitimately back several slots);
    //   3. the audio thread has since run drainCommands() at least once.
    //
    // (3) is the interesting one, and it is what makes (1) sufficient. A Voice
    // does not hold a copy of its RtClip; it holds `&clips_[t][s]`, and
    // drainCommands() overwrites that cell in place. So the instant the engine
    // drains the displacing command, every voice reading that slot is reading
    // the *new* clip — there is no release tail over the old buffer to wait
    // out, unlike the chain-retirement case this pattern comes from. One
    // completed drain is the whole proof.
    //
    // How we know a drain happened. Phase 2 had two answers and said plainly
    // that one of them was weak; phase 3 replaces the weak one.
    //
    //   * EXACTLY, from the counter. Engine::drains is bumped at the end of
    //     every drainCommands(), so `drains >= (value read after the push) + 2`
    //     is a proof that the audio thread has consumed the displacing command.
    //     See drainProof() above for why the constant is 2 and not 1 — that
    //     off-by-one is the whole of the correctness argument, and getting it
    //     wrong would produce a retirement that is right almost always.
    //   * Exactly, when the engine volunteers it. Replacing or clearing a MIDI
    //     clip makes Engine push Ev::NotesRetired from inside drainCommands,
    //     so the event's arrival *is* the drain and beats the counter by up to
    //     one block. Kept, because it is free and it is tighter.
    //
    // WHAT WAS REMOVED. Phase 2 waited max(100 ms, 8 block periods) plus four
    // rendered blocks for anything the engine did not volunteer an event for,
    // and §10.3 called that "the weak half": a wedged backend does not drain,
    // and the deadline fired anyway — announcing a block free while a voice
    // could still be reading it. The counter cannot do that. A wedged backend
    // simply never satisfies the proof, so nothing is ever announced, which is
    // the correct answer to "has the audio thread passed this point" when the
    // audio thread has not moved.
    //
    // THE DEADLINE IS STILL COMPILED IN, for exactly one case: an Engine built
    // without the counter, where `drains` stays 0 forever. The daemon latches
    // `drainsExact_` the first time it sees the counter move and publishes the
    // answer in ControlHeader::drainsExact, so which rule is in force is
    // observable from outside rather than inferred. When the counter is live —
    // which it is in this tree — the timer is never consulted.
    static constexpr u64 kRetireGraceNs = 100ull * 1000000ull;   // 100 ms (legacy)
    static constexpr u64 kRetireBlocks  = 4;                     //        (legacy)

    // Records what the engine now holds for a cell and queues whatever that
    // displaced. Runs only from commit().
    void installClip(int track, int slot, const ipc::WireClip& nc) {
        ipc::WireClip& cur = shadow_[track][slot];
        const u64 oldSample = cur.sampleRef;
        const u64 oldNotes  = cur.notesRef;
        cur = nc;
        // A block can come back: clear a slot and re-publish the same sample
        // into another one, and the retirement queued a moment ago is now
        // wrong. Cancelling is not strictly required — the client ignores an
        // echo for a block it has since marked live again — but an event that
        // says "you may free this" about a block that is in use is the kind of
        // thing that is true today and load-bearing tomorrow.
        if (nc.sampleRef) cancelRetire(nc.sampleRef);
        if (nc.notesRef)  cancelRetire(nc.notesRef);
        if (oldSample) considerRetire(oldSample, ipc::PoolKindSamples, track, slot);
        if (oldNotes)  considerRetire(oldNotes,  ipc::PoolKindNotes,   track, slot);
    }

    void cancelRetire(u64 ref) {
        for (size_t i = 0; i < retiring_.size(); ++i)
            if (retiring_[i].ref == ref) {
                retiring_.erase(retiring_.begin() + (long)i);
                return;
            }
    }

    void considerRetire(u64 ref, u32 kind, int track, int slot) {
        // Still backing another slot? Then it is not retiring, it is shared.
        for (int t = 0; t < kMaxTracks; ++t)
            for (int s = 0; s < kMaxScenes; ++s)
                if (shadow_[t][s].sampleRef == ref || shadow_[t][s].notesRef == ref) return;
        for (const Retire& r : retiring_) if (r.ref == ref) return;

        Retire r{};
        r.ref       = ref;
        r.kind      = kind;
        r.track     = track;
        r.slot      = slot;
        // Read after the push that displaced the block — installClip() runs
        // from commit(), which runs only once Engine::pushCommand has returned
        // true. That ordering is what makes the +2 argument hold, and it is
        // why the retirement was already queued from commit() and not from
        // translate().
        r.dueDrains = drainProof();
        const u64 blockNs = (u64)((f64)block_ / sr_ * 1e9);
        r.dueNs     = ipc::monotonicNs() +
                      (kRetireGraceNs > 8 * blockNs ? kRetireGraceNs : 8 * blockNs);
        r.dueBlocks = (nullDriver_ ? nullDriver_->blocks() : 0) + kRetireBlocks;
        retiring_.push_back(r);
    }

    // An address the engine handed back names a block the client knows by
    // offset. A pointer that is not inside the pool is not a block at all and
    // must never be echoed as one — that is what offsetOf() returning 0 means.
    void confirmRetire(const void* p) {
        const u64 off = pool_.offsetOf(p);
        if (!off) {
            map_.hdr->eventsDropped.fetch_add(1, std::memory_order_relaxed);
            LOGW("Ev::NotesRetired carried %p, which is not inside the sample pool", p);
            return;
        }
        for (Retire& r : retiring_) if (r.ref == off) { r.confirmed = true; return; }
    }

    void pumpRetirements() {
        if (retiring_.empty()) return;
        const u64 now    = ipc::monotonicNs();
        const u64 blocks = nullDriver_ ? nullDriver_->blocks() : 0;
        size_t keep = 0;
        for (size_t i = 0; i < retiring_.size(); ++i) {
            const Retire& r = retiring_[i];
            const bool due = r.confirmed ||
                             (drainsExact_
                                  ? drainProven(r.dueDrains)
                                  : (now >= r.dueNs && (!nullDriver_ || blocks >= r.dueBlocks)));
            if (due && publishRetired(r)) continue;          // done with it
            retiring_[keep++] = r;                           // not yet, or ring full
        }
        retiring_.resize(keep);
    }

    bool publishRetired(const Retire& r) {
        ipc::WireEvent e{};
        e.type  = ipc::EvBlockRetired;
        e.flags = r.kind;
        e.a     = r.track;
        e.b     = r.slot;
        e.ref   = r.ref;
        if (!map_.evts->push(e)) return false;               // client asleep; retry
        map_.hdr->blocksRetired.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Engine events out. Two of the pointer-carrying four are consumed here
    // rather than dropped, and both for the same reason: the pointer is an
    // address in *this* process, and the daemon knows what it names.
    //
    //   Ev::NotesRetired  an offset in the pool, echoed back as EvBlockRetired.
    //   Ev::ChainRetired  a chain the daemon built. §3.6 predicted this event
    //                     would "disappear rather than being ported"; what
    //                     disappeared is its trip across the boundary. The
    //                     swap-and-retire dance is still exactly right and is
    //                     still run — between the pump and the audio thread of
    //                     one process, which is where it always belonged.
    //
    // The remaining two stay unreachable, because the commands that would
    // allocate their payloads are still refused. If their counter ever moves,
    // something reached the engine that should not have.
    void pumpEvents() {
        Event ev;
        while (engine_->popEvent(ev)) {
            if (ev.type == Ev::NotesRetired) {
                confirmRetire(ev.p);
                continue;
            }
            if (ev.type == Ev::ChainRetired) {
                confirmChainRetire(ev.p);
                continue;
            }
            if (!ipc::eventIsScalar((u32)ev.type)) {
                map_.hdr->eventsDropped.fetch_add(1, std::memory_order_relaxed);
                LOGW("engine event %u carries a pointer and cannot cross",
                     (u32)ev.type);
                ipc::WireEvent d{};
                d.type = ipc::EvEventDropped;
                d.a    = (i32)ev.type;
                map_.evts->push(d);
                continue;
            }
            ipc::WireEvent e{};
            e.type = (u32)ev.type;
            e.a    = ev.a;
            e.b    = ev.b;
            e.x    = ev.x;
            if (!map_.evts->push(e)) return;     // client asleep; retry next tick
            map_.hdr->eventsForwarded.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // -- the mirror ---------------------------------------------------------
    //
    // Engine's published atomics -> SharedState, every ~4 ms. Relaxed on both
    // sides exactly as the in-process GUI reads them today: no two fields have
    // an invariant between them, so a meter one tick stale next to a playhead
    // one tick fresh is indistinguishable from the latency the display already
    // has (shm.h, SharedStateT).
    //
    // generation is bumped last, with release, so a reader that observes a new
    // generation is guaranteed to see the values that went with it — the one
    // ordering guarantee worth paying for, because it is what lets a client
    // sample the beat clock without a seqlock.
    void mirrorLoop() {
        Engine& e = *engine_;
        ipc::SharedState& s = *map_.state;
        while (mirrorRun_.load(std::memory_order_relaxed)) {
            s.beat.store(e.beat.load(std::memory_order_relaxed), std::memory_order_relaxed);
            s.tempo.store(e.tempo.load(std::memory_order_relaxed), std::memory_order_relaxed);
            s.playing.store(e.playing.load(std::memory_order_relaxed) ? 1u : 0u,
                            std::memory_order_relaxed);
            s.cpu.store(e.cpu.load(std::memory_order_relaxed), std::memory_order_relaxed);

            for (int t = 0; t < kMaxTracks; ++t) {
                s.slotState[t].store(e.slotState[t].load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
                s.activeSlot[t].store(e.activeSlot[t].load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
                s.pendingSlot[t].store(e.pendingSlot[t].load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
                s.clipPhase[t].store(e.clipPhase[t].load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
                s.meterL[t].store(e.meterL[t].load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
                s.meterR[t].store(e.meterR[t].load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
                s.recState[t].store(e.recState[t].load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
                s.recSlotIdx[t].store(e.recSlotIdx[t].load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            }
            s.masterMeterL.store(e.masterMeterL.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
            s.masterMeterR.store(e.masterMeterR.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);

            if (nullDriver_)
                s.blocksRendered.store(nullDriver_->blocks(), std::memory_order_relaxed);

            s.heartbeatNs.store(ipc::monotonicNs(), std::memory_order_relaxed);
            s.generation.fetch_add(1, std::memory_order_release);

            timespec ts{0, 4000000};        // 4 ms
            ::nanosleep(&ts, nullptr);
        }
    }

    // -- shutdown -----------------------------------------------------------

    int shutdown() {
        LOGI("nxtaktd stopping (session '%s')", opt_.session);

        // Tell whoever is attached before anything stops moving, so a client
        // polling the state block sees "stopping", not "wedged".
        map_.state->engineState.store(ipc::SharedState::StateStopping, std::memory_order_relaxed);
        ipc::WireEvent e{};
        e.type = ipc::EvEngineStopping;
        map_.evts->push(e);

        mirrorRun_.store(false, std::memory_order_release);
        if (mirror_.joinable()) mirror_.join();

        if (backend_)    backend_->stop();
        if (nullDriver_) nullDriver_->stop();

        // With no audio thread left, every proof is trivially satisfied: the
        // engine cannot be inside a chain nobody is calling. Chains and
        // instances go first, because a plugin's destructor is the one place a
        // third-party library is most likely to misbehave and we would rather
        // it happen with the region still intact and the log still open.
        // A scan in flight is joined rather than abandoned: it holds a lilv
        // world and half the plugin libraries on the system open, and detaching
        // from that at exit is how a "clean shutdown" turns into a crash in
        // somebody else's atexit handler.
        if (scanThread_.joinable()) scanThread_.join();
        destroyAllDevices();

        // Rendering has stopped, so every outstanding retirement is trivially
        // true: there is no audio thread left to be inside a clip. Publishing
        // them now lets a client that is still attached free its pool cleanly
        // instead of leaving blocks stuck in Retiring for the rest of its life
        // — which matters because the pool outlives us and may well be handed
        // to the next engine.
        for (const Retire& r : retiring_) publishRetired(r);
        retiring_.clear();

        // The shutdown flag is the last thing written and it is published with
        // release: an attacher's mapping survives shm_unlink, so this is how a
        // still-attached client learns the difference between "the engine went
        // away cleanly" and "the engine died" without a socket.
        map_.state->playing.store(0, std::memory_order_relaxed);
        map_.state->heartbeatNs.store(ipc::monotonicNs(), std::memory_order_relaxed);
        map_.hdr->shutdown.store(1, std::memory_order_release);

        logCounters();
        gOwnsRegion = 0;
        map_.clear();
        region_.close();                 // creator: unlinks the name
        LOGI("nxtaktd stopped, region unlinked");
        return 0;
    }

    void logCounters() const {
        LOGI("commands: %llu applied, %llu rejected, %llu deferred; midi %llu; "
             "events: %llu forwarded, %llu dropped",
             (unsigned long long)map_.hdr->commandsApplied.load(),
             (unsigned long long)map_.hdr->commandsRejected.load(),
             (unsigned long long)map_.hdr->commandsDeferred.load(),
             (unsigned long long)map_.hdr->midiApplied.load(),
             (unsigned long long)map_.hdr->eventsForwarded.load(),
             (unsigned long long)map_.hdr->eventsDropped.load());
        LOGI("clips: %llu applied, %llu pool blocks retired; pool %s",
             (unsigned long long)map_.hdr->clipsApplied.load(),
             (unsigned long long)map_.hdr->blocksRetired.load(),
             pool_.valid() ? pool_.name() : "(none)");
        LOGI("devices: %llu added, %llu removed, %llu failed; %llu param writes; "
             "chains %llu published / %llu retired",
             (unsigned long long)map_.hdr->devicesAdded.load(),
             (unsigned long long)map_.hdr->devicesRemoved.load(),
             (unsigned long long)map_.hdr->devicesFailed.load(),
             (unsigned long long)map_.hdr->paramWrites.load(),
             (unsigned long long)map_.hdr->chainsPublished.load(),
             (unsigned long long)map_.hdr->chainsRetired.load());
        LOGI("engine drains: %llu (%s)",
             (unsigned long long)map_.hdr->engineDrains.load(),
             drainsExact_ ? "exact retirement" : "no counter — legacy deadline");
    }

    static constexpr int kRejectLogLimit = 8;

    Options                        opt_;
    std::unique_ptr<Engine>        engine_;     // ~2 MB of scratch: never on the stack
    std::unique_ptr<AudioBackend>  backend_;
    std::unique_ptr<NullDriver>    nullDriver_;
    ipc::ShmRegion                 region_;
    ipc::ControlMap                map_;
    ipc::PoolReader                pool_;       // read-only: the GUI owns it
    std::thread                    mirror_;
    std::atomic<bool>              mirrorRun_{false};
    Staged                         pending_{};
    bool                           havePending_ = false;
    int                            rejectLogged_ = 0;
    int                            badRefLogged_ = 0;
    u64                            poolEpoch_    = 0;
    u64                            poolRetryNs_  = 0;
    bool                           poolFailLogged_   = false;
    bool                           poolRebindLogged_ = false;
    f64                            sr_    = 48000.0;
    int                            block_ = 256;
    char                           driverName_[32] = "none";

    // What the engine holds, as far as the boundary knows. It is a shadow and
    // not a view of the shared table because the table is the *client's*
    // memory: the client may rewrite a cell the moment it is acknowledged, and
    // the diff that decides what got displaced has to be against what was
    // actually forwarded.
    ipc::WireClip                  shadow_[kMaxTracks][kMaxScenes]{};
    std::vector<Retire>            retiring_;

    // -- phase 3 ------------------------------------------------------------
    PluginRegistry                  registry_;          // the catalog, once scanned
    std::unique_ptr<PluginRegistry> scanned_;           // the scanning thread's own
    std::thread                     scanThread_;
    std::atomic<bool>               scanDone_{false};
    u32                             scanState_   = ipc::ScanIdle;
    u64                             scanStartNs_ = 0;

    std::deque<ipc::WireCommand>    deviceQueue_;       // waiting for the catalog
    std::unique_ptr<Device>         devices_[ipc::kMaxDevices];
    std::vector<u32>                liveIds_;           // what pumpParams walks
    u32                             deviceGen_[ipc::kMaxDevices] = {};
    Chain                           chains_[kChainCount];
    std::deque<ChainPush>           chainPush_;
    std::vector<ChainRetire>        chainRetire_;
    int                             deviceLogged_ = 0;

    // Latched the first time Engine::drains moves. False means "this engine
    // does not count its drains", which is a real build (phase 2's) and not a
    // hypothetical, so the legacy deadline stays reachable rather than being
    // deleted and rediscovered.
    bool                            drainsExact_ = false;
};

} // namespace
} // namespace lat

int main(int argc, char** argv) {
    lat::Options o;
    if (const char* s = lat::env("SESSION")) o.session = s;
    if (!lat::parseArgs(argc, argv, o)) return 2;

    lat::Daemon d(o);
    return d.run();
}
