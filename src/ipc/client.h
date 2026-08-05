// Lattice IPC — the client side of the control region.
//
// EngineClient is what the GUI will hold instead of an Engine& once phase 4
// lands (docs/PROCESS-SPLIT.md §6): attach to a running latticed, push
// commands, drain events, read the polled state block, and notice when the
// engine stops answering. It is written now, ahead of the GUI adopting it, so
// that the daemon has a real second peer in the tests rather than a bespoke
// test harness that agrees with the daemon by accident.
//
// Deliberately dependency-light: core/, audio/engine.h for the enums, and
// libc. No GUI headers, no audio libraries, nothing that needs a link order.
// A control surface, a headless test, or `lattice-ctl` can all include this
// and cost nothing.
//
// Threading contract, unchanged from the in-process one it replaces:
//   * pushCommand()/pushMidi() are single-producer — one thread, or your own
//     lock. (pushMidi may be a *different* single thread from pushCommand:
//     they are separate rings.)
//   * popEvent() is single-consumer.
//   * state() is racy by design and safe to read from anywhere: relaxed
//     atomics with no cross-field invariants (shm.h, SharedStateT).
#pragma once
#include "control.h"

#include <string>

#include <sys/wait.h>

namespace lat::ipc {

class EngineClient {
public:
    EngineClient() = default;
    ~EngineClient() { detach(); }
    EngineClient(const EngineClient&)            = delete;
    EngineClient& operator=(const EngineClient&) = delete;

    // -----------------------------------------------------------------------
    // Attach / detach
    // -----------------------------------------------------------------------

    // Three layers of handshake, in the order a mismatch should be reported
    // (§4.2): the region validates magic + kShmVersion + layout hash, then we
    // check the message-level protocol version, then the caller reads
    // state().sampleRate before it decodes anything.
    //
    // Stale regions are reaped on the way *out*, never on the way in. Reaping
    // first looks tidier and is wrong: reapIfStale() treats a region that
    // exists but is not yet sized as an orphan, which is exactly what a daemon
    // between shm_open() and ftruncate() looks like — so a pre-emptive reap can
    // unlink a live engine's region microseconds after it claimed the name.
    // Attaching first cannot make that mistake, and a corpse is just as
    // reapable after the attach as before it.
    //
    // On failure the region name is free (reaped, or never existed) and the
    // caller's next move is to spawn a daemon and call attach() again.
    bool attach(const char* session, int timeoutMs = 2000) {
        detach();
        controlRegionName(session, name_, sizeof name_);
        session_ = session ? session : "default";

        if (!region_.attach(name_, control::kHash, kShmVersion, timeoutMs)) {
            setErr("%s", region_.error());
            ShmRegion::reapIfStale(name_);   // a half-built corpse blocks create()
            return false;
        }
        // The region is valid, but is anybody home? pid *and* start time, never
        // the pid alone: reuse would make us reap a live session's region.
        {
            const ShmHeader* h = region_.header();
            if (!processAlive(h->creatorPid, h->creatorStartTicks)) {
                setErr("%s: the engine that created this region (pid %d) is gone; "
                       "region reaped, respawn the engine", name_, h->creatorPid);
                region_.close();
                map_.clear();
                ShmRegion::reapIfStale(name_);
                return false;
            }
        }
        if (!map_.attach(region_)) {
            setErr("%s: control region is the right size but the sections do not fit", name_);
            region_.close();
            map_.clear();
            return false;
        }
        if (map_.hdr->protocolVersion != kProtocolVersion) {
            // Specific, not silent: the region was structurally fine, so the
            // user needs to hear "restart the engine", not "attach failed".
            setErr("%s: engine speaks protocol v%u, this build speaks v%u — restart the engine",
                   name_, map_.hdr->protocolVersion, kProtocolVersion);
            region_.close();
            map_.clear();
            return false;
        }
        if (map_.hdr->shutdown.load(std::memory_order_acquire) != 0) {
            setErr("%s: the engine that owns this region is shutting down", name_);
            region_.close();
            map_.clear();
            return false;
        }
        err_[0] = '\0';
        return true;
    }

    void detach() {
        map_.clear();
        region_.close();
        name_[0] = '\0';
    }

    bool attached() const { return region_.valid() && map_.valid(); }

    // -----------------------------------------------------------------------
    // Messages
    // -----------------------------------------------------------------------
    //
    // Every push returns false when the ring is full and the caller must
    // handle it — retry on the next frame, do not drop. Silently ignoring a
    // refused push is how user intent goes missing under a burst
    // (docs/PROCESS-SPLIT.md §5, the one hardening item phase 1 owes).

    bool pushCommand(const WireCommand& c) { return attached() && map_.cmds->push(c); }

    bool pushCommand(Cmd type, i32 a = 0, i32 b = 0, f64 x = 0.0, u64 ref = 0) {
        WireCommand c{};
        c.type = (u32)type;
        c.a = a; c.b = b; c.x = x; c.ref = ref;
        return pushCommand(c);
    }

    bool popEvent(WireEvent& e) { return attached() && map_.evts->pop(e); }

    bool pushMidi(const WireMidi& m) { return attached() && map_.midi->push(m); }
    bool pushMidi(u8 status, u8 d1, u8 d2, i32 frame = 0) {
        WireMidi m{};
        m.status = status; m.d1 = d1; m.d2 = d2; m.frame = frame;
        return pushMidi(m);
    }

    // -----------------------------------------------------------------------
    // Polled state
    // -----------------------------------------------------------------------

    // Valid only while attached(). A detached client gets a zeroed block
    // rather than a null dereference, so a UI that polls once more on its way
    // out draws a stopped transport instead of crashing.
    const SharedState& state() const {
        static const SharedState kEmpty{};
        return map_.state ? *map_.state : kEmpty;
    }
    const ControlHeader& header() const {
        static const ControlHeader kEmpty{};
        return map_.hdr ? *map_.hdr : kEmpty;
    }

    f64 sampleRate() const { return state().sampleRate.load(std::memory_order_relaxed); }
    u32 blockSize()  const { return state().blockSize.load(std::memory_order_relaxed); }
    i32 enginePid()  const { return attached() ? region_.header()->creatorPid : -1; }
    u64 heartbeat()  const { return header().heartbeat.load(std::memory_order_relaxed); }
    const char* regionName() const { return name_; }
    const char* error() const { return err_; }

    // Two failures, two mechanisms, per §4.4:
    //   dead   — the creator pid (with its start time, never the pid alone) is
    //            gone, or it published the shutdown flag on its way out;
    //   wedged — alive but no longer publishing, so the heartbeat is stale.
    // The tolerance must be generous. A laptop resuming from suspend or a JACK
    // server being restarted is not a dead engine, and respawning a second
    // daemon under a live one is the worst possible outcome.
    bool alive(u64 toleranceNs = 500ull * 1000000ull) const {
        if (!attached()) return false;
        if (map_.hdr->shutdown.load(std::memory_order_acquire) != 0) return false;
        const ShmHeader* h = region_.header();
        if (!processAlive(h->creatorPid, h->creatorStartTicks)) return false;
        return !map_.state->stale(toleranceNs);
    }

    // -----------------------------------------------------------------------
    // Daemon lifecycle helpers
    // -----------------------------------------------------------------------

    // The crash-orphan hook, exposed so a GUI can run it before spawning
    // (§4.1). True if a stale region existed and was removed.
    static bool reapStale(const char* session) {
        char nm[128];
        controlRegionName(session, nm, sizeof nm);
        return ShmRegion::reapIfStale(nm);
    }

    // fork + execv. No shell, ever: the session id and the region name would
    // otherwise be a command injection through a project filename, and a shell
    // in between also breaks the "the daemon's parent is us" assumption that
    // waitFor() below relies on.
    //
    // `args` is the argument list *after* argv[0], null-terminated; argv[0] is
    // filled in from `path`. Returns the child pid, or -1.
    static pid_t spawnDaemon(const char* path, const char* const* args) {
        const char* argv[32];
        int n = 0;
        argv[n++] = path;
        if (args)
            for (int i = 0; args[i] && n < (int)(sizeof argv / sizeof argv[0]) - 1; ++i)
                argv[n++] = args[i];
        argv[n] = nullptr;

        std::fflush(nullptr);          // never duplicate buffered output into the child
        const pid_t pid = ::fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            // The child must not inherit a handler that would unlink a region
            // it does not own yet; exec resets them anyway, but a failed exec
            // must die quietly rather than run the parent's atexit handlers.
            ::execv(path, (char* const*)argv);
            ::_exit(127);
        }
        return pid;
    }

    // Waits for a spawned daemon, up to timeoutMs. Returns true if it exited
    // (status filled in), false on timeout — the caller then escalates from
    // SIGTERM to SIGKILL, which is exactly what a supervisor should do.
    static bool waitFor(pid_t pid, int timeoutMs, int* status = nullptr) {
        const u64 deadline = monotonicNs() + (u64)timeoutMs * 1000000ull;
        for (;;) {
            int st = 0;
            const pid_t r = ::waitpid(pid, &st, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) {
                if (status) *status = st;
                return true;
            }
            if (monotonicNs() >= deadline) return false;
            timespec ts{0, 500000};     // 0.5 ms
            nanosleep(&ts, nullptr);
        }
    }

private:
    void setErr(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(err_, sizeof err_, fmt, ap);
        va_end(ap);
    }

    ShmRegion   region_;
    ControlMap  map_;
    std::string session_;
    char        name_[128] = {};
    char        err_[256]  = {};
};

} // namespace lat::ipc
