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
//   * the sample pool and the clip table belong to the pushCommand() thread.
//     They are ordinary memory with a single writer, not rings.
//
// popEvent() is not a pure read: it feeds every event through observe() before
// handing it back, because the clip/pool protocol has bookkeeping on the
// client side (a cell acknowledgement unblocks the next write to that cell, a
// retirement echo unblocks a free). Making that automatic rather than the
// caller's duty is deliberate — a GUI that forgets to call observe() would
// deadlock its own clip edits and leak its own pool, and neither failure would
// look like a missing call.
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
        // A pool that already exists belongs to the *session*, not to the
        // engine we just attached to (§4.4): if this is a respawn, the samples
        // are still in memory and the new daemon needs to be told where. The
        // clip table is not republished here — that is the caller's call,
        // because it wants to happen after the daemon confirms the pool.
        if (pool_.valid()) publishPool();
        err_[0] = '\0';
        return true;
    }

    // Drops the control region. The pool is untouched: it outlives engines by
    // construction, and closePool() is a separate, deliberate act.
    void detach() {
        map_.clear();
        region_.close();
        name_[0] = '\0';
        // Any cell write still waiting for an acknowledgement will never get
        // one. Treat it as refused rather than leaving the cell blocked
        // forever: whatever engine comes next is rebuilt from the shadow table
        // by republishClips(), so the un-acknowledged value was never part of
        // anything's state.
        rollbackPendingCells();
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

    // Drains one event *and* applies its client-side bookkeeping. See the note
    // at the top of this file for why that is not the caller's job.
    bool popEvent(WireEvent& e) {
        if (!attached() || !map_.evts->pop(e)) return false;
        observe(e);
        return true;
    }

    bool pushMidi(const WireMidi& m) { return attached() && map_.midi->push(m); }
    bool pushMidi(u8 status, u8 d1, u8 d2, i32 frame = 0) {
        WireMidi m{};
        m.status = status; m.d1 = d1; m.d2 = d2; m.frame = frame;
        return pushMidi(m);
    }

    // -----------------------------------------------------------------------
    // The sample pool
    // -----------------------------------------------------------------------
    //
    // The client owns the pool outright: it creates it, allocates in it, frees
    // in it, and unlinks it. The daemon only ever attaches read-only. That is
    // the ownership asymmetry §3.1 calls for and it is what makes "samples
    // survive an engine restart" true rather than aspirational — see pool.h.

    // Creates /lattice-pool-<session>. Independent of attach(): a GUI decodes
    // its project while it is still waiting for a daemon to come up, and the
    // pool is where it decodes *into*.
    bool createPool(const char* session, size_t payloadBytes = kDefaultPoolBytes) {
        char nm[128];
        poolRegionName(session, nm, sizeof nm);
        if (!pool_.create(nm, payloadBytes, ++poolEpoch_)) {
            setErr("%s", pool_.error());
            return false;
        }
        if (attached()) publishPool();
        return true;
    }

    // Adopt a pool that already exists — a replacement GUI after a crash
    // (§4.3), or a second handle in a test. Read/write: the attacher of a pool
    // is always a GUI.
    bool attachPool(const char* session, int timeoutMs = 0) {
        char nm[128];
        poolRegionName(session, nm, sizeof nm);
        if (!pool_.attach(nm, timeoutMs)) {
            setErr("%s", pool_.error());
            return false;
        }
        poolEpoch_ = pool_.epoch();
        if (attached()) publishPool();
        return true;
    }

    // Unlink and unmap. The last thing a GUI does on the way out, *after* the
    // engine has stopped — the region outliving the engine is the point, but
    // outliving the session is a leak (§4.5).
    void closePool() { pool_.close(); }

    // Detach without unlinking: hand the live session on to whoever attaches
    // next. This is what a GUI crash does implicitly and what an orderly
    // hand-off does on purpose.
    void abandonPool() { pool_.abandon(); }

    // Tell the attached daemon where the pool is. Idempotent, and re-run
    // automatically by attach() so a respawned engine is pointed at the same
    // samples without the caller having to remember.
    bool publishPool() {
        if (!attached() || !pool_.valid()) return false;
        std::snprintf(map_.hdr->poolName, sizeof map_.hdr->poolName, "%s", pool_.name());
        map_.hdr->poolBytes.store(pool_.bytes(), std::memory_order_relaxed);
        // Release: the daemon acquires the epoch and only then reads the name,
        // so it can never map a half-written string.
        map_.hdr->poolEpoch.store(poolEpoch_, std::memory_order_release);
        return true;
    }

    // True once the daemon has the same pool mapped. Until then every clip
    // that references an offset is refused, which is exactly right: an
    // unmapped offset has no meaning to translate.
    bool poolReady() const {
        return attached() && pool_.valid() &&
               map_.hdr->poolAttachedEpoch.load(std::memory_order_acquire) == poolEpoch_;
    }

    SamplePool&       pool()       { return pool_; }
    const SamplePool& pool() const { return pool_; }
    u64               poolEpoch() const { return poolEpoch_; }

    // Copy `frames * channels` interleaved floats into the pool and return the
    // offset the engine will know them by. 0 means the pool is full or the
    // arguments are nonsense; the caller must check, because the alternative is
    // publishing offset 0 as a clip and finding out on stage.
    u64 poolWrite(const f32* interleaved, i64 frames, int channels,
                  f64 rate = 0.0, u64 key = 0) {
        const u64 r = pool_.writeSamples(interleaved, frames, channels, rate, key);
        if (!r) setErr("%s", pool_.error());
        return r;
    }
    u64 poolWriteNotes(const WireNote* notes, i64 count, u64 key = 0) {
        const u64 r = pool_.writeNotes(notes, count, key);
        if (!r) setErr("%s", pool_.error());
        return r;
    }

    // The free-after-confirm bookkeeping helper. Drop the GUI's reference and
    // let the state machine decide: a block no clip cell has ever seen is freed
    // here and now; one the engine might still hold is freed later, by the
    // EvBlockRetired echo arriving in popEvent(). Returns true if it was freed
    // on this call — useful to assert on, not something a caller should need.
    bool poolRelease(u64 ref) { return pool_.release(ref); }

    // -----------------------------------------------------------------------
    // The clip table
    // -----------------------------------------------------------------------

    // Writes cell (track, slot) and tells the engine which cell moved.
    //
    // Returns false — and changes nothing — in three cases, all of which mean
    // "try again next frame" rather than "this failed":
    //   * a previous write to this cell has not been acknowledged yet (see
    //     WireClip::generation for why that matters);
    //   * the command ring is full;
    //   * there is no engine attached.
    // The caller must handle it. Silently dropping a clip publication is how
    // an edit goes missing (§5).
    bool setClip(int track, int slot, const WireClip& c) {
        return writeCell(track, slot, c, Cmd::SetClip);
    }

    bool clearClip(int track, int slot) {
        WireClip empty{};
        return writeCell(track, slot, empty, Cmd::ClearClip);
    }

    // The last value this client believes the engine holds for a cell. This is
    // the republish source, and it is the client's own memory rather than the
    // control region precisely because the control region dies with the engine.
    const WireClip& clipShadow(int track, int slot) const {
        static const WireClip kEmpty{};
        const int i = cellIndex(track, slot);
        return i < 0 ? kEmpty : shadow_[i];
    }
    // True while a cell write is outstanding, i.e. setClip() on it would
    // refuse.
    bool clipBusy(int track, int slot) const {
        const int i = cellIndex(track, slot);
        return i >= 0 && pending_[i].generation != shadow_[i].generation;
    }

    // §4.4 step 3: after a respawn, put the session back. The table is a
    // memcpy — that is the whole reason §3.4 prefers a table to a clip ring —
    // followed by one SetClip per occupied cell so the engine installs them.
    // The pool is untouched, so this is a republish and not a reload: nothing
    // is decoded, no offset changes.
    int republishClips() {
        if (!attached()) return 0;
        std::memcpy(map_.clips, shadow_, sizeof shadow_);
        int sent = 0;
        for (int t = 0; t < kMaxTracks; ++t)
            for (int s = 0; s < kMaxScenes; ++s) {
                const WireClip& c = shadow_[cellIndex(t, s)];
                if (!c.valid) continue;
                if (!pushCommand(Cmd::SetClip, t, s, 0.0, c.generation)) return sent;
                pending_[cellIndex(t, s)] = c;      // already in sync: no bookkeeping
                ++sent;
            }
        return sent;
    }

    // Applies an event's client-side bookkeeping. popEvent() does this for
    // you; it is public only so a client that drains the ring some other way
    // (a test, a bridge) can stay honest. Returns true if the event was one of
    // the protocol's own.
    bool observe(const WireEvent& e) {
        switch (e.type) {
            case EvClipAck:      return onClipAck(e);
            case EvBlockRetired: pool_.confirmRetired(e.ref); return true;
            case EvPoolAttached: return true;
            default:             return false;
        }
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
    static constexpr int kCells = kMaxTracks * kMaxScenes;

    static int cellIndex(int track, int slot) {
        if (track < 0 || track >= kMaxTracks || slot < 0 || slot >= kMaxScenes) return -1;
        return track * kMaxScenes + slot;
    }

    // The one place a cell is written. Order matters and is the protocol:
    //
    //   1. refuse if the cell is still un-acknowledged, or the ring is full.
    //      Checking the ring *first* is what lets everything after it be
    //      unconditional — this client is the ring's only producer, so a
    //      measured space is a space that is still there a line later.
    //   2. mark the incoming blocks Live before publishing them, so that a
    //      concurrent poolRelease() cannot free a block the engine is about to
    //      be handed.
    //   3. write the cell, then push the command. The push is a release store
    //      and the pop an acquire, so the daemon cannot see the command
    //      without seeing the cell.
    //   4. the *displacement* bookkeeping waits for the acknowledgement,
    //      because until the daemon has read the cell we do not know whether
    //      the old contents were displaced or the write was refused.
    bool writeCell(int track, int slot, const WireClip& in, Cmd cmd) {
        const int i = cellIndex(track, slot);
        if (i < 0 || !attached()) return false;
        if (pending_[i].generation != shadow_[i].generation) return false;
        if (map_.cmds->size() >= CommandRing::capacity()) return false;

        WireClip c = in;
        c.generation = shadow_[i].generation + 1;
        if (cmd == Cmd::ClearClip) { c.sampleRef = 0; c.notesRef = 0; c.valid = 0; }

        if (c.sampleRef) pool_.markLive(c.sampleRef);
        if (c.notesRef)  pool_.markLive(c.notesRef);

        *map_.clip(track, slot) = c;
        pending_[i] = c;
        pushCommand(cmd, track, slot, 0.0, c.generation);   // space measured above
        return true;
    }

    bool onClipAck(const WireEvent& e) {
        const int i = cellIndex(e.a, e.b);
        if (i < 0) return true;
        if (pending_[i].generation != (u32)e.ref) return true;   // stale echo
        if (pending_[i].generation == shadow_[i].generation) return true;  // republish

        if (e.flags & ClipAckRefused) {
            // The engine never saw it, so nothing was displaced and the blocks
            // we optimistically marked Live are not published after all.
            if (pending_[i].sampleRef) pool_.unmarkLive(pending_[i].sampleRef);
            if (pending_[i].notesRef)  pool_.unmarkLive(pending_[i].notesRef);
            shadow_[i].generation = pending_[i].generation;   // keep them monotonic
            pending_[i] = shadow_[i];
            return true;
        }

        // Accepted: whatever the cell used to hold is now displaced. Note this
        // runs unconditionally on the old refs, including when the new clip
        // names the same block — markLive/markDisplaced are a counted pair, and
        // a repush that changes only the gain must not leave the count high.
        if (shadow_[i].sampleRef) pool_.markDisplaced(shadow_[i].sampleRef);
        if (shadow_[i].notesRef)  pool_.markDisplaced(shadow_[i].notesRef);
        shadow_[i] = pending_[i];
        return true;
    }

    void rollbackPendingCells() {
        for (int i = 0; i < kCells; ++i) {
            if (pending_[i].generation == shadow_[i].generation) continue;
            if (pending_[i].sampleRef) pool_.unmarkLive(pending_[i].sampleRef);
            if (pending_[i].notesRef)  pool_.unmarkLive(pending_[i].notesRef);
            shadow_[i].generation = pending_[i].generation;
            pending_[i] = shadow_[i];
        }
    }

    void setErr(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(err_, sizeof err_, fmt, ap);
        va_end(ap);
    }

    ShmRegion   region_;
    ControlMap  map_;
    SamplePool  pool_;
    u64         poolEpoch_ = 0;
    std::string session_;
    char        name_[128] = {};
    char        err_[256]  = {};

    // The clip table, twice: what the engine is believed to hold (shadow_) and
    // what has been written but not acknowledged (pending_). Client memory, not
    // shared memory, because the control region dies with the engine and the
    // whole point of the shadow is to outlive one.
    WireClip    shadow_[kCells]  = {};
    WireClip    pending_[kCells] = {};
};

} // namespace lat::ipc
