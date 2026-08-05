// latticed — the Lattice engine daemon.
//
// Phase 1 of the process split (docs/PROCESS-SPLIT.md §6, brought forward from
// phase 4): a headless process that owns an Engine and an audio backend and
// exposes them through one shared-memory control region. Nothing in src/ui
// talks to it yet — the GUI adopts EngineClient in phase 2 — so this binary is
// currently exercised by tests/daemon_test.cpp and by hand.
//
//   latticed [--session NAME] [--driver null|auto|jack|alsa]
//            [--rate HZ] [--block FRAMES] [--verbose]
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
#include "../audio/backend.h"
#include "../audio/engine.h"
#include "../core/common.h"
#include "../ipc/control.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
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
        "latticed — the Lattice engine daemon\n"
        "\n"
        "  --session NAME     session id; the control region is /lattice-engine-NAME\n"
        "                     (default: $LATTICE_SESSION, else \"default\")\n"
        "  --driver KIND      null | auto | jack | alsa   (default: $LATTICE_AUDIO, else auto)\n"
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
            LOGE("another latticed may already own session '%s'", opt_.session);
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
        LOGI("latticed ready: session '%s', region %s, %.0f Hz / %d frames, driver %s",
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
    // -- startup ------------------------------------------------------------

    bool startDriver() {
        const char* want = opt_.driver;
        if (!want) want = ::getenv("LATTICE_AUDIO");   // same knob the GUI honours

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
            pumpCommands();
            pumpMidi();
            pumpEvents();
            map_.hdr->heartbeat.fetch_add(1, std::memory_order_relaxed);
            timespec ts{0, 1000000};        // 1 ms
            ::nanosleep(&ts, nullptr);
        }
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
            if (!engine_->pushCommand(pending_)) return;
            havePending_ = false;
            map_.hdr->commandsApplied.fetch_add(1, std::memory_order_relaxed);
        }
        ipc::WireCommand w;
        while (map_.cmds->pop(w)) {
            Command c{};
            u32 reason = ipc::RejectNone;
            if (!translate(w, c, reason)) {
                reject(w, reason);
                continue;
            }
            if (!engine_->pushCommand(c)) {
                pending_     = c;
                havePending_ = true;
                map_.hdr->commandsDeferred.fetch_add(1, std::memory_order_relaxed);
                return;                      // resume from here next tick
            }
            map_.hdr->commandsApplied.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // PHASE 1 SCOPE: scalars only. Everything that carries a pointer is
    // refused here, at the boundary, with a reason — see the table in
    // src/ipc/control.h. Half-translating one (a SetClip with a null `data`)
    // would produce a clip that plays silence, which is a bug you find on
    // stage rather than in CI.
    static bool translate(const ipc::WireCommand& w, Command& out, u32& reason) {
        if (!ipc::commandIsKnown(w.type)) { reason = ipc::RejectUnknownCommand; return false; }
        if (!ipc::commandIsScalar(w.type)) { reason = ipc::RejectPointerPayload; return false; }
        if (!std::isfinite(w.x)) { reason = ipc::RejectNotFinite; return false; }

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

        out.type = t;
        out.a    = w.a;
        out.b    = w.b;
        out.x    = w.x;
        out.p    = nullptr;              // no pointer ever crosses in phase 1
        return true;
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

    // Engine events out. The four pointer-carrying events cannot cross and are
    // dropped with a counter; in phase 1 they are also unreachable, because the
    // commands that would allocate their payloads are refused above. If the
    // counter ever moves, something reached the engine that should not have.
    void pumpEvents() {
        Event ev;
        while (engine_->popEvent(ev)) {
            if (!ipc::eventIsScalar((u32)ev.type)) {
                map_.hdr->eventsDropped.fetch_add(1, std::memory_order_relaxed);
                LOGW("engine event %u carries a pointer and cannot cross (phase 2)",
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
        LOGI("latticed stopping (session '%s')", opt_.session);

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
        LOGI("latticed stopped, region unlinked");
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
    }

    static constexpr int kRejectLogLimit = 8;

    Options                        opt_;
    std::unique_ptr<Engine>        engine_;     // ~2 MB of scratch: never on the stack
    std::unique_ptr<AudioBackend>  backend_;
    std::unique_ptr<NullDriver>    nullDriver_;
    ipc::ShmRegion                 region_;
    ipc::ControlMap                map_;
    std::thread                    mirror_;
    std::atomic<bool>              mirrorRun_{false};
    Command                        pending_{};
    bool                           havePending_ = false;
    int                            rejectLogged_ = 0;
    f64                            sr_    = 48000.0;
    int                            block_ = 256;
    char                           driverName_[32] = "none";
};

} // namespace
} // namespace lat

int main(int argc, char** argv) {
    lat::Options o;
    if (const char* s = ::getenv("LATTICE_SESSION")) o.session = s;
    if (!lat::parseArgs(argc, argv, o)) return 2;

    lat::Daemon d(o);
    return d.run();
}
