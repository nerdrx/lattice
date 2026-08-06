// EngineHandle: both paths. See engine_handle.h for the shape and
// docs/GUI-ON-DAEMON.md §2 for why it is a concrete class rather than an
// interface.
//
// The file is in three parts: the local path (unchanged from step 1), the
// RemoteEngine that step 2 and step 3 add, and the dispatch between them.
#include "engine_handle.h"
#include "../ipc/client.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <string>
#include <unordered_map>

#include <signal.h>
#include <unistd.h>

namespace lat {

// ===========================================================================
// RemoteEngine — the daemon path
// ===========================================================================
//
// Everything ipc-shaped lives in here so that engine_handle.h can stay free of
// src/ipc and the view translation units with it.
//
// THE THREE JOBS
// --------------
//   1. Own the link: reap, attach or spawn-and-attach, create the sample pool,
//      and stop a daemon we started when we go (§6, parent-of-record).
//   2. Translate. A Command carrying an RtClip full of GUI-heap pointers has to
//      become a WireClip full of pool offsets. That is what samples_/notes_
//      below are for and it is the whole of step 3.
//   3. Stand in for the engine's retirement events. `pushClip()` hands the
//      engine a fresh RtNote[] and waits for Ev::NotesRetired before freeing the
//      one it displaced; there is no engine here to send it, and the array never
//      crossed the boundary in the first place (it was COPIED into the pool). So
//      this object keeps the same per-cell "last published" table the engine
//      keeps and synthesises the event. Without it App::retiringNotes_ grows for
//      the life of the session and nothing ever comes home.

namespace {

// The command names the refusal log prints. A switch rather than a table so
// that adding a Cmd without adding a name fails the -Wswitch build.
const char* cmdName(Cmd t) {
    switch (t) {
        case Cmd::SetPlaying:        return "SetPlaying";
        case Cmd::SetTempo:          return "SetTempo";
        case Cmd::SetQuantum:        return "SetQuantum";
        case Cmd::SetMetronome:      return "SetMetronome";
        case Cmd::LaunchClip:        return "LaunchClip";
        case Cmd::StopTrack:         return "StopTrack";
        case Cmd::LaunchScene:       return "LaunchScene";
        case Cmd::StopAll:           return "StopAll";
        case Cmd::SetClip:           return "SetClip";
        case Cmd::ClearClip:         return "ClearClip";
        case Cmd::TrackVol:          return "TrackVol";
        case Cmd::TrackPan:          return "TrackPan";
        case Cmd::TrackMute:         return "TrackMute";
        case Cmd::TrackSolo:         return "TrackSolo";
        case Cmd::TrackArm:          return "TrackArm";
        case Cmd::MasterVol:         return "MasterVol";
        case Cmd::ClipGain:          return "ClipGain";
        case Cmd::ClipWarp:          return "ClipWarp";
        case Cmd::ClipLoop:          return "ClipLoop";
        case Cmd::SetChain:          return "SetChain";
        case Cmd::SetReturnChain:    return "SetReturnChain";
        case Cmd::SetMasterChain:    return "SetMasterChain";
        case Cmd::SendLevel:         return "SendLevel";
        case Cmd::ReturnVol:         return "ReturnVol";
        case Cmd::RecordSlot:        return "RecordSlot";
        case Cmd::RecordMidiSlot:    return "RecordMidiSlot";
        case Cmd::SetArrangement:    return "SetArrangement";
        case Cmd::SetTrackAutos:     return "SetTrackAutos";
        case Cmd::Locate:            return "Locate";
        case Cmd::BackToArrangement: return "BackToArrangement";
        case Cmd::SetSignatures:     return "SetSignatures";
    }
    return "?";
}

// FNV-1a over the scalars plus up to kProbes strided 8-byte words of the
// payload. See RemoteEngine::poolRefFor for why a fingerprint exists at all.
u64 fingerprint(const void* p, size_t bytes, i64 a, i64 b, f64 c) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](u64 v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xffull; h *= 1099511628211ull; }
    };
    mix((u64)a);
    mix((u64)b);
    u64 cbits = 0;
    std::memcpy(&cbits, &c, sizeof cbits);
    mix(cbits);
    mix((u64)bytes);
    if (!p || bytes < sizeof(u64)) return h;
    constexpr size_t kProbes = 256;
    const size_t words = bytes / sizeof(u64);
    const size_t step  = words > kProbes ? words / kProbes : 1;
    const u8* base = (const u8*)p;
    for (size_t i = 0; i < words; i += step) {
        u64 w = 0;
        std::memcpy(&w, base + i * sizeof(u64), sizeof w);
        mix(w);
    }
    return h;
}

// argv[0]'s directory plus "nxtaktd". The daemon ships beside the GUI, so
// /proc/self/exe is the answer that keeps working from a build tree, an install
// prefix and a test harness alike. $NXTAKT_DAEMON overrides it outright.
std::string daemonPath() {
    if (const char* s = env("DAEMON")) return s;
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "nxtaktd";
    buf[n] = '\0';
    char* slash = std::strrchr(buf, '/');
    if (!slash) return "nxtaktd";
    *slash = '\0';
    return std::string(buf) + "/nxtaktd";
}

} // namespace

struct RemoteEngine {
    ipc::EngineClient cli;
    std::string session;
    // > 0 only when WE started it. §6's parent-of-record rule: quitting the GUI
    // stops the daemon it spawned and leaves alone one it merely attached to.
    pid_t spawned = -1;

    // Cached because the region they live in goes away with the daemon, and
    // sampleRate() in particular must keep answering a usable number on the way
    // out — a zero would make loadSample() resample a whole set to nothing.
    char driverName[40] = {};
    f64  rate = 48000.0;
    u32  block = 0;

    // -- step 3: the double copy (§5 step 3, decision (i-a)) ----------------
    //
    // A ClipModel's audio is a SampleBuffer on the GUI heap and the engine is in
    // another process, so the bytes have to exist twice: once where
    // drawWaveform() reads them and once in the pool where the daemon can. The
    // doc weighs this against decoding straight into the pool and picks the copy
    // for this step, because the alternative touches src/audio/sample.h,
    // project.cpp, the undo snapshot and tools/render.cpp — none of which this
    // wave owns — and would make a high-risk step unreviewable.
    //
    // Keyed by the SOURCE ADDRESS, with a content fingerprint beside it. The
    // address alone is not enough: a SampleBuffer can be freed and a different
    // one allocated at the same address (undo does exactly this), and serving
    // the cached offset then would publish the wrong audio. The fingerprint is
    // 256 strided words plus the shape, which no accident produces a collision
    // in and which costs a microsecond on a buffer of any size.
    struct Cached { u64 ref = 0; u64 finger = 0; };
    std::unordered_map<const void*, Cached> samples;
    std::unordered_map<const void*, Cached> notes;

    // -- the retirement stand-in --------------------------------------------
    //
    // The engine announces a *replaced* pointer, and only when it differs from
    // the incoming one. Same rule here, same tables, so App's publishNotes /
    // publishAutos / publishWarp bookkeeping sees exactly what it would have.
    const void* pubNotes[kMaxTracks][kMaxScenes] = {};
    const void* pubAutos[kMaxTracks][kMaxScenes] = {};
    const void* pubWarp [kMaxTracks][kMaxScenes] = {};
    const void* pubArr  [kMaxTracks + 1] = {};      // index kMaxTracks = transport
    const void* pubTrackAutos[kMaxTracks] = {};
    std::deque<Event> synth;

    // -- accounting ---------------------------------------------------------
    u64  refusals = 0;                  // commands consumed because we cannot carry them
    u64  tears    = 0;                  // snapshots that exhausted the seqlock retries
    u64  poolFull = 0;
    bool loggedCmd[64] = {};            // one log line per Cmd type, not per push
    bool loggedPoolFull = false;
    bool loggedLost = false;

    // ---------------------------------------------------------------------
    // Link
    // ---------------------------------------------------------------------

    // §4.1's ladder, in the order EngineClient already implements: attach first
    // (a live daemon is the common case and the cheap one), and only reap and
    // spawn when that fails. Attaching first is what stops a pre-emptive reap
    // from unlinking a region whose daemon is merely between shm_open() and
    // ftruncate() — see the note on EngineClient::attach().
    bool open(const char* sess, const char* driver) {
        session = (sess && *sess) ? sess : "default";

        if (!cli.attach(session.c_str(), 0)) {
            const std::string path = daemonPath();
            const char* args[6];
            int n = 0;
            args[n++] = "--session";
            args[n++] = session.c_str();
            if (driver && *driver) { args[n++] = "--driver"; args[n++] = driver; }
            args[n] = nullptr;
            LOGI("no engine on session '%s'; starting %s", session.c_str(), path.c_str());
            spawned = ipc::EngineClient::spawnDaemon(path.c_str(), args);
            if (spawned < 0) {
                LOGE("could not fork a daemon (%s)", path.c_str());
                return false;
            }
            if (!cli.attach(session.c_str(), 2000)) {
                LOGE("the engine did not come up: %s", cli.error());
                // Reap the corpse rather than leaving a zombie behind a GUI that
                // is about to run in degraded mode for the rest of the session.
                ::kill(spawned, SIGTERM);
                ipc::EngineClient::waitFor(spawned, 1000);
                spawned = -1;
                return false;
            }
        }

        std::snprintf(driverName, sizeof driverName, "daemon:%s", cli.header().driverName);
        const f64 r = cli.sampleRate();
        if (r >= 8000.0 && r <= 384000.0) rate = r;
        block = cli.blockSize();

        // The pool, before anything decodes. §1.4's ordering constraint: the GUI
        // must know the engine rate before it resamples, AND it must have
        // somewhere to put what it decodes. Both are true from here on, and
        // App::init() does not touch a sample until after openLocal() returns.
        if (!cli.createPool(session.c_str())) {
            // Not fatal, and specifically not a reason to fall back to a local
            // engine: transport, tempo, the mixer and the meters all work with
            // no pool at all. What is lost is clips, and the daemon says so —
            // every clip that names an offset comes back RejectNoPool.
            LOGE("no sample pool: %s (clips will not sound)", cli.error());
        }
        LOGI("engine: nxtaktd session '%s', %s, %.0f Hz / %u frames, pid %d%s",
             session.c_str(), cli.header().driverName, rate, block, (int)cli.enginePid(),
             spawned > 0 ? " (started by us)" : " (already running)");
        return true;
    }

    void close() {
        if (refusals)
            LOGW("daemon mode refused %llu command(s) it cannot carry yet "
                 "(devices, recording, the arrangement, time signatures)",
                 (unsigned long long)refusals);
        if (tears)
            LOGW("%llu frame(s) drawn from a state snapshot that could not be "
                 "proved coherent — the engine was stopped mid-publish",
                 (unsigned long long)tears);

        // The pool goes FIRST, and the order is not the obvious one. "Stop the
        // engine, then drop its memory" reads better and leaks: stopping the
        // daemon is a signal plus a wait of up to three seconds, and anything
        // that kills the GUI inside that window (a session ending, a compositor
        // tearing down, an impatient user) leaves 256 MiB named in /dev/shm with
        // nobody left to unlink it. Unlinking first is safe precisely because of
        // the property the pool was designed around: shm_unlink removes the NAME
        // and not the mapping, so the daemon keeps reading the samples it is
        // playing right up until it exits.
        cli.detach();
        cli.closePool();

        // §6: we stop what we started and leave alone what we found. SIGTERM
        // runs nxtaktd's ordinary shutdown (publish the flag, unlink the
        // region); SIGKILL is the escalation a supervisor owes a process that
        // will not go.
        if (spawned > 0) {
            ::kill(spawned, SIGTERM);
            if (!ipc::EngineClient::waitFor(spawned, 2000)) {
                LOGW("the engine did not stop on SIGTERM; killing it");
                ::kill(spawned, SIGKILL);
                ipc::EngineClient::waitFor(spawned, 1000);
            }
            spawned = -1;
        }
    }

    // ---------------------------------------------------------------------
    // The pool
    // ---------------------------------------------------------------------

    // Returns the pool offset that holds a copy of [p, p+bytes), writing one if
    // this is the first time or if the content has changed underneath us.
    // 0 means the pool refused, which is reported once and then counted.
    u64 poolRefFor(std::unordered_map<const void*, Cached>& tbl, const void* p,
                   size_t bytes, u64 finger, bool asNotes, i64 frames, int channels) {
        if (!p || !bytes) return 0;
        auto it = tbl.find(p);
        if (it != tbl.end()) {
            if (it->second.finger == finger) return it->second.ref;
            // The address was reused, or the buffer was edited in place. Drop
            // our own reference to the old block — it frees now if no clip cell
            // ever saw it, and waits for the daemon's echo if one did.
            cli.poolRelease(it->second.ref);
            tbl.erase(it);
        }
        const u64 ref = asNotes
            ? cli.poolWriteNotes((const ipc::WireNote*)p, frames)
            : cli.poolWrite((const f32*)p, frames, channels, rate, 0);
        if (!ref) {
            ++poolFull;
            if (!loggedPoolFull) {
                loggedPoolFull = true;
                LOGE("the sample pool would not take %zu B: %s. Clips beyond this "
                     "point will not sound. [further attempts silent]", bytes, cli.error());
            }
            return 0;
        }
        tbl.emplace(p, Cached{ref, finger});
        return ref;
    }

    u64 sampleRefFor(const RtClip& rc) {
        if (!rc.data || rc.frames <= 0 || rc.channels < 1) return 0;
        const size_t bytes = (size_t)rc.frames * (size_t)rc.channels * sizeof(f32);
        return poolRefFor(samples, rc.data, bytes,
                          fingerprint(rc.data, bytes, rc.frames, rc.channels, rate),
                          false, rc.frames, rc.channels);
    }

    u64 notesRefFor(const RtClip& rc) {
        if (!rc.notes || rc.noteCount <= 0) return 0;
        const size_t bytes = (size_t)rc.noteCount * sizeof(RtNote);
        // WireNote is asserted to mirror RtNote field for field (pool.h), which
        // is what makes this a cast and not a conversion loop.
        return poolRefFor(notes, rc.notes, bytes,
                          fingerprint(rc.notes, bytes, rc.noteCount, 0, 0.0),
                          true, rc.noteCount, 0);
    }

    // ---------------------------------------------------------------------
    // Commands
    // ---------------------------------------------------------------------

    // Consume a command the remote path cannot carry, loudly. Answering `false`
    // instead would be worse than wrong: App::flushPending() re-queues a refused
    // publication and retries it every frame, so a permanent `false` wedges the
    // FIFO and with it every scalar behind it. "Refused with a reason" here
    // means consumed, counted, and logged once per command type — never
    // silently dropped, and never pretended to have worked.
    bool refuse(Cmd t, const char* why) {
        ++refusals;
        const u32 i = (u32)t;
        if (i < 64 && !loggedCmd[i]) {
            loggedCmd[i] = true;
            LOGW("daemon mode cannot carry %s: %s [further ones counted, not logged]",
                 cmdName(t), why);
        }
        return true;
    }

    // The engine's retirement rule, verbatim: announce the displaced pointer,
    // and only when it differs from the incoming one.
    void retire(const void*& slot, const void* fresh, Ev ev, i32 a, i32 b) {
        const void* old = slot;
        slot = fresh;
        if (old && old != fresh) {
            Event e;
            e.type = ev; e.a = a; e.b = b; e.x = 0.0;
            e.p = const_cast<void*>(old);
            synth.push_back(e);
        }
    }

    bool push(const Command& c) {
        const u32 type = (u32)c.type;

        if (ipc::commandIsScalar(type))
            return cli.pushCommand(c.type, c.a, c.b, c.x);

        switch (c.type) {
            case Cmd::SetClip:
            case Cmd::ClearClip:
                return pushClipCell(c);

            // The chain family is refused permanently by the protocol itself
            // (§11.7.6): a client has no business naming an RtChain because it
            // has no RtChains. `false` is safe here and is the honest answer —
            // App::publishChain() is deliberately NOT on the deferred FIFO, so
            // it frees the chain it built and logs, and nothing spins.
            case Cmd::SetChain:
            case Cmd::SetReturnChain:
            case Cmd::SetMasterChain:
                ++refusals;
                return false;

            // Same: startRecording() handles a refusal by freeing the capture
            // buffer and saying "Engine busy", which is the correct behaviour
            // until §7's take protocol exists.
            case Cmd::RecordSlot:
            case Cmd::RecordMidiSlot:
                ++refusals;
                return false;

            // These three DO go through the FIFO, so they must be consumed. The
            // daemon can take an arrangement — it has translateArrangement() and
            // daemon_test proves it — but only as a pool blob, and building one
            // out of an already-built RtArrangement is the next step's work.
            case Cmd::SetArrangement: {
                const int cell = (c.a == -1) ? kMaxTracks : c.a;
                if (cell >= 0 && cell <= kMaxTracks)
                    retire(pubArr[cell], c.p, Ev::ArrangementRetired, c.a, 0);
                return refuse(c.type, "the arrangement needs a pool blob this step "
                                      "does not build; the timeline will not sound");
            }
            case Cmd::SetTrackAutos: {
                if (c.a >= 0 && c.a < kMaxTracks)
                    retire(pubTrackAutos[c.a], c.p, Ev::TrackAutosRetired, c.a, 0);
                return refuse(c.type, "arrangement automation rides the arrangement blob");
            }
            case Cmd::SetSignatures:
                // Not reachable today — session.h's publishSignatures() takes an
                // Engine& and so cannot be called at all in daemon mode — but
                // spelled out rather than left to the default, because the day it
                // is routed through the handle this is what must happen. Note
                // there is no retirement table: the map is one array and the
                // caller keeps the pointer it published.
                return refuse(c.type, "Cmd::SetSignatures is outside "
                                      "ipc::commandIsKnown's bound, so the daemon answers "
                                      "RejectUnknownCommand and plays the set in 4/4");

            // Every scalar returned above, through commandIsScalar()'s own
            // exhaustive switch — which is where a newly appended Cmd gets its
            // -Wswitch reminder, and where it belongs, because the classifier is
            // the protocol. Anything arriving here is a command this build does
            // not know at all, and it is refused rather than guessed at.
            default: break;
        }
        return refuse(c.type, "this build does not know that command");
    }

    // Step 3. The RtClip App built is full of GUI-heap pointers; what goes over
    // the wire is a WireClip full of pool offsets, written into the clip table
    // by EngineClient::setClip.
    //
    // ORDER MATTERS. The pool writes happen first and are idempotent (the cache
    // keeps the block across a refusal), then the cell write, and only if THAT
    // succeeds does the retirement bookkeeping run — because App's own
    // publishNotes() runs on exactly the same condition, and the two tables have
    // to agree or a note array comes home twice or never.
    bool pushClipCell(const Command& c) {
        const int t = c.a, s = c.b;
        if (t < 0 || t >= kMaxTracks || s < 0 || s >= kMaxScenes) return true;  // nothing to do

        if (c.type == Cmd::ClearClip) {
            if (!cli.clearClip(t, s)) return false;
            retire(pubNotes[t][s], nullptr, Ev::NotesRetired, t, s);
            retire(pubAutos[t][s], nullptr, Ev::AutosRetired, t, s);
            retire(pubWarp[t][s],  nullptr, Ev::WarpRetired,  t, s);
            return true;
        }

        const RtClip& rc = c.clip;
        ipc::WireClip w = ipc::defaultWireClip();
        w.sampleRef    = sampleRefFor(rc);
        w.notesRef     = notesRefFor(rc);
        w.frames       = rc.frames;
        w.loopStart    = rc.loopStart;
        w.loopEnd      = rc.loopEnd;
        w.noteCount    = rc.noteCount;
        w.clipBpm      = rc.clipBpm;
        w.lengthBeats  = rc.lengthBeats;
        w.prob         = rc.prob;
        w.followBeats  = rc.followBeats;
        w.gain         = rc.gain;
        w.channels     = rc.channels;
        w.warp         = rc.warp;
        w.quantumIdx   = rc.quantumIdx;
        w.followAction = rc.followAction;
        w.loop         = rc.loop ? 1u : 0u;
        w.isMidi       = rc.isMidi ? 1u : 0u;
        w.valid        = rc.valid ? 1u : 0u;

        // A clip whose bytes never reached the pool must not be published as a
        // valid one: the daemon would answer RejectBadPoolRef and the cell would
        // sit refused. Publish it as an EMPTY cell instead, which is a state the
        // protocol has a name for, and let the pool-full log line say why.
        if (rc.valid && ((rc.data && !w.sampleRef) || (rc.notes && !w.notesRef))) {
            w = ipc::WireClip{};
            w.channels = 1; w.clipBpm = 120.0; w.lengthBeats = 4.0; w.gain = 1.0f;
            w.warp = (i32)Warp::Beats; w.quantumIdx = -1; w.prob = 1.0;
            w.followAction = (i32)Follow::None;
        }

        // Three cross-process gaps this step does not close, and RtClip is where
        // they show: `autos`, `markers` and `transients` have no WireClip field
        // to travel in, so a clip with an envelope, a warp map or a transient
        // grid plays without them. The daemon cannot be at fault for this — it
        // is not expressible — so the GUI has to be the one that says so.
        if (rc.valid && (rc.autos || rc.markers || rc.transients))
            refuse(Cmd::SetClip, "clip envelopes, warp markers and transients have no "
                                 "WireClip field; the clip crosses without them");

        if (!cli.setClip(t, s, w)) return false;     // busy cell or full ring: retry

        retire(pubNotes[t][s], rc.notes,   Ev::NotesRetired, t, s);
        retire(pubAutos[t][s], rc.autos,   Ev::AutosRetired, t, s);
        retire(pubWarp[t][s],  rc.markers, Ev::WarpRetired,  t, s);
        return true;
    }

    // ---------------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------------

    bool pop(Event& e) {
        // Synthesised retirements first: they were queued during this frame's
        // flushPending() and the caller is draining right after it, so handing
        // them back before the wire's own events keeps the order App would have
        // seen in-process.
        if (!synth.empty()) { e = synth.front(); synth.pop_front(); return true; }

        ipc::WireEvent w;
        while (cli.popEvent(w)) {
            if (w.type >= ipc::kDaemonEventBase) { observeDaemon(w); continue; }
            // A pointer-carrying engine event cannot have crossed — the daemon
            // does not forward them — so anything here is a scalar one.
            if (!ipc::eventIsScalar(w.type)) continue;
            e.type = (Ev)w.type;
            e.a = w.a; e.b = w.b; e.x = w.x;
            e.p = nullptr;
            return true;
        }
        return false;
    }

    void observeDaemon(const ipc::WireEvent& w) {
        switch (w.type) {
            case ipc::EvCommandRejected:
                LOGW("the engine refused %s: %s",
                     cmdName((Cmd)w.a), ipc::rejectReasonName((u32)w.b));
                break;
            case ipc::EvEngineStopping:
                LOGW("the engine is shutting down");
                break;
            case ipc::EvEventDropped:
                LOGW("the engine dropped an event that could not cross (%d)", (int)w.a);
                break;
            case ipc::EvPoolAttached:
                LOGI("the engine mapped the sample pool (%.0f B, epoch %llu)",
                     w.x, (unsigned long long)w.ref);
                break;
            default:
                // EvClipAck and EvBlockRetired are already applied by
                // EngineClient::popEvent()'s observe(); the device and scan
                // events belong to step 4 and there is nothing to do with them
                // yet. Deliberately quiet rather than deliberately ignored: the
                // client's bookkeeping ran, which is the part that matters.
                break;
        }
    }
};

// ===========================================================================
// EngineHandle
// ===========================================================================

EngineHandle::EngineHandle()  = default;
EngineHandle::~EngineHandle() = default;

bool EngineHandle::openLocal(const char* driver) {
    const char* which = env("ENGINE");
    if (which && (!std::strcmp(which, "daemon") || !std::strcmp(which, "remote"))) {
        const char* sess = env("SESSION");
        if (openDaemon(sess, driver)) return true;
        // §8's exception, and the reason this does not silently fall back to a
        // local engine: two engines is worse than none. A GUI that cannot reach
        // a daemon opens anyway, loads the project, edits and saves — with every
        // send() a no-op and a log line saying so — because the case a fallback
        // is actually for is a broken audio setup on somebody else's machine,
        // and quietly starting a second engine under a wedged one is §4.4's
        // worst available outcome.
        LOGE("NXTAKT_ENGINE=daemon but no engine could be reached: "
             "running with no engine at all (the set can still be edited and saved)");
        return true;
    }
    return openLocalEngine(driver);
}

bool EngineHandle::openLocalEngine(const char* driver) {
    engine_ = std::unique_ptr<Engine>(new (std::nothrow) Engine());
    if (!engine_) { LOGE("could not allocate the engine"); return false; }

    audio_ = createBackend(*engine_, driver);
    if (!audio_) {
        // Not an error. A set can be edited, saved and looked at with no audio
        // device at all, and refusing to start would make a broken ALSA
        // configuration on somebody else's machine fatal.
        LOGW("no audio backend available - running silent");
        engine_->prepare(48000.0, 1024);
    }

    // MIDI comes up after the audio backend: the reader thread pushes straight
    // into the engine's ring, so the engine must already be prepared. Missing
    // hardware or a missing sequencer device is not an error - a set can be
    // played entirely from the mouse.
    if (midi_.start(*engine_)) LOGI("midi in: alsa seq client %d:0", midi_.clientId());
    else                       LOGW("no MIDI input - continuing without it");
    return true;
}

bool EngineHandle::openDaemon(const char* session, const char* driver) {
    auto r = std::unique_ptr<RemoteEngine>(new (std::nothrow) RemoteEngine());
    if (!r) { LOGE("could not allocate the engine client"); return false; }
    if (!r->open(session, driver)) return false;
    remote_ = std::move(r);
    // Hardware MIDI does not follow. MidiInput::start() takes an Engine& and
    // pushes straight into its ring; there is no Engine here, and src/audio is
    // frozen this wave. §1.3's answer is to move the ALSA reader into nxtaktd,
    // which is where it belongs anyway — a daemon that keeps playing after a GUI
    // crash must keep answering the keyboard too. Until then daemon mode is
    // mouse-and-computer-keyboard only, and says so rather than looking broken.
    LOGW("daemon mode: hardware MIDI input is not connected (see GUI-ON-DAEMON.md §1.3)");
    return true;
}

void EngineHandle::close() {
    // Order matters, and it is the order App::shutdown() used to spell out
    // inline. The MIDI reader goes first: it pushes into the engine's ring from
    // its own thread, so it has to be joined before anything else starts
    // tearing the engine down, or a push could land in a ring nobody owns any
    // more. Stopping the backend then joins the audio thread.
    midi_.stop();
    if (audio_) { audio_->stop(); audio_.reset(); }
    // engine_ is deliberately NOT released here: App frees the chains, note
    // arrays and capture buffers the engine was borrowing *after* this returns,
    // and one of the debug hooks still reaches it through local(). It dies with
    // the handle, which dies with App.
    if (remote_) { remote_->close(); remote_.reset(); }
}

// ---------------------------------------------------------------------------
// The snapshot
// ---------------------------------------------------------------------------
//
// One tight copy per frame. What this fixes and what it does not, precisely:
//
//   It removes the INTRA-FRAME incoherence, which is the one that showed. The
//   four reads drawClipSlot used to make were separated by whatever the draw
//   code did in between — easily a millisecond, i.e. several audio blocks at
//   256 frames — so a slot could be drawn Playing with activeSlot == -1. After
//   this they are one copy taken microseconds apart.
//
//   LOCALLY it does NOT make the copy itself atomic against Engine::publish().
//   There is nothing to gate on: publish() bumps no generation counter, and
//   blocksRendered is incremented at the TOP of process() while publish() runs
//   at the bottom, so a reader that saw the same blocksRendered either side of
//   its copy could still have straddled the publish for that block. engine.h is
//   frozen, so adding one is not on the table — and the remaining window is the
//   duration of this function.
//
//   REMOTELY it closes completely, and that is what step 2 bought: SharedState
//   is a seqlock as of shm v5 (odd while the daemon's mirror is writing), so the
//   copy below runs inside readCoherent() and is retried until it provably did
//   not straddle a publish. A copy that could not be proved coherent after eight
//   tries is counted in snapshotTears() and handed over anyway, because a UI
//   that hangs on a stopped daemon is worse than one that draws a stale frame.
void EngineHandle::poll(EngineState& out) {
    if (remote_) {
        const ipc::SharedState& s = remote_->cli.state();
        const bool ok = s.readCoherent([&] {
            out.beat    = s.beat.load(std::memory_order_relaxed);
            out.tempo   = s.tempo.load(std::memory_order_relaxed);
            out.playing = s.playing.load(std::memory_order_relaxed) != 0;
            out.cpu     = s.cpu.load(std::memory_order_relaxed);

            out.posBar       = s.posBar.load(std::memory_order_relaxed);
            out.posBeat      = s.posBeat.load(std::memory_order_relaxed);
            out.posSixteenth = s.posSixteenth.load(std::memory_order_relaxed);
            out.posSigNum    = s.posSigNum.load(std::memory_order_relaxed);
            out.posSigDen    = s.posSigDen.load(std::memory_order_relaxed);

            out.sampleRate    = s.sampleRate.load(std::memory_order_relaxed);
            out.blockSize     = s.blockSize.load(std::memory_order_relaxed);
            out.latencyFrames = s.latencyFrames.load(std::memory_order_relaxed);

            for (int t = 0; t < kMaxTracks; ++t) {
                out.slotState[t]   = s.slotState[t].load(std::memory_order_relaxed);
                out.activeSlot[t]  = s.activeSlot[t].load(std::memory_order_relaxed);
                out.pendingSlot[t] = s.pendingSlot[t].load(std::memory_order_relaxed);
                out.clipPhase[t]   = s.clipPhase[t].load(std::memory_order_relaxed);
                out.meterL[t]      = s.meterL[t].load(std::memory_order_relaxed);
                out.meterR[t]      = s.meterR[t].load(std::memory_order_relaxed);
                out.recState[t]    = s.recState[t].load(std::memory_order_relaxed);
                out.recSlotIdx[t]  = s.recSlotIdx[t].load(std::memory_order_relaxed);
            }
            for (int i = 0; i < kMaxReturns; ++i) {
                out.returnMeterL[i] = s.returnMeterL[i].load(std::memory_order_relaxed);
                out.returnMeterR[i] = s.returnMeterR[i].load(std::memory_order_relaxed);
            }
            out.masterMeterL = s.masterMeterL.load(std::memory_order_relaxed);
            out.masterMeterR = s.masterMeterR.load(std::memory_order_relaxed);

            out.arrOverride    = s.arrOverride.load(std::memory_order_relaxed);
            out.journalDropped = s.journalDropped.load(std::memory_order_relaxed);
        });
        if (!ok) ++remote_->tears;
        // A detached client hands back a zeroed block, in which sampleRate is 0.
        // Every other field reading as a stopped transport is correct; that one
        // is not, because the status bar prints it and because a 0 here next to
        // a live sampleRate() accessor would look like a contradiction.
        if (out.sampleRate <= 0.0) out.sampleRate = remote_->rate;

        // The link, once. §4.4's rule is that a stale heartbeat is NOT grounds
        // for respawning — a laptop resuming from suspend and a JACK restart
        // both look exactly like a wedged engine for a few hundred milliseconds,
        // and a second daemon under a live one is the worst available outcome.
        // So this notices and says so, and does nothing else.
        if (!remote_->cli.alive() && !remote_->loggedLost) {
            remote_->loggedLost = true;
            LOGW("the audio engine stopped answering. Your set is intact; "
                 "restart the GUI to reconnect.");
        }
        return;
    }

    const Engine* e = engine_.get();
    if (!e) { out = EngineState{}; return; }        // detached: a stopped transport

    out.beat    = e->beat.load(std::memory_order_relaxed);
    out.tempo   = e->tempo.load(std::memory_order_relaxed);
    out.playing = e->playing.load(std::memory_order_relaxed);
    out.cpu     = e->cpu.load(std::memory_order_relaxed);

    // The engine's own bars.beats.sixteenths and the signature at the playhead.
    // Read here rather than recomputed by the transport bar from the session's
    // map, because the two differ exactly when a map was REFUSED — see the note
    // in engine_state.h.
    out.posBar       = e->posBar.load(std::memory_order_relaxed);
    out.posBeat      = e->posBeat.load(std::memory_order_relaxed);
    out.posSixteenth = e->posSixteenth.load(std::memory_order_relaxed);
    out.posSigNum    = e->posSigNum.load(std::memory_order_relaxed);
    out.posSigDen    = e->posSigDen.load(std::memory_order_relaxed);

    out.sampleRate    = e->sampleRate();
    out.blockSize     = audio_ ? (u32)audio_->bufferSize() : 0u;
    out.latencyFrames = e->latencyFrames.load(std::memory_order_relaxed);

    for (int t = 0; t < kMaxTracks; ++t) {
        out.slotState[t]   = e->slotState[t].load(std::memory_order_relaxed);
        out.activeSlot[t]  = e->activeSlot[t].load(std::memory_order_relaxed);
        out.pendingSlot[t] = e->pendingSlot[t].load(std::memory_order_relaxed);
        out.clipPhase[t]   = e->clipPhase[t].load(std::memory_order_relaxed);
        out.meterL[t]      = e->meterL[t].load(std::memory_order_relaxed);
        out.meterR[t]      = e->meterR[t].load(std::memory_order_relaxed);
        out.recState[t]    = e->recState[t].load(std::memory_order_relaxed);
        out.recSlotIdx[t]  = e->recSlotIdx[t].load(std::memory_order_relaxed);
    }
    for (int i = 0; i < kMaxReturns; ++i) {
        out.returnMeterL[i] = e->returnMeterL[i].load(std::memory_order_relaxed);
        out.returnMeterR[i] = e->returnMeterR[i].load(std::memory_order_relaxed);
    }
    out.masterMeterL = e->masterMeterL.load(std::memory_order_relaxed);
    out.masterMeterR = e->masterMeterR.load(std::memory_order_relaxed);

    out.arrOverride    = e->arrOverride.load(std::memory_order_relaxed);
    out.journalDropped = e->journalDropped.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool EngineHandle::send(Cmd t, i32 a, i32 b, f64 x) {
    Command c;
    c.type = t; c.a = a; c.b = b; c.x = x;
    return pushCommand(c);
}

bool EngineHandle::pushCommand(const Command& c) {
    if (remote_) return remote_->push(c);
    return engine_ ? engine_->pushCommand(c) : false;
}

bool EngineHandle::pushMidi(const MidiMsg& m) {
    if (remote_) return remote_->cli.pushMidi(m.status, m.d1, m.d2, m.frame);
    return engine_ ? engine_->pushMidiFromGui(m) : false;
}

bool EngineHandle::popEvent(Event& e) {
    if (remote_) return remote_->pop(e);
    return engine_ ? engine_->popEvent(e) : false;
}

f64 EngineHandle::sampleRate() const {
    if (remote_) {
        // The live wire value, and the cached one only if the region has gone.
        // Never 0: every caller of this resamples with it.
        const f64 r = remote_->cli.sampleRate();
        return (r >= 8000.0 && r <= 384000.0) ? r : remote_->rate;
    }
    return engine_ ? engine_->sampleRate() : 48000.0;
}

u32 EngineHandle::journalDropped() const {
    if (remote_) {
        // TWO hops can lose a journal entry across the boundary — the engine's
        // ring into the daemon's pump, and the pump's ring into ours — and §5.4
        // refuses a take on either. The sum is what a caller asking "was
        // anything lost?" means, and it is monotonic like the parts.
        const u64 sum = (u64)remote_->cli.engineJournalDropped() + remote_->cli.journalDropped();
        return sum > 0xffffffffull ? 0xffffffffu : (u32)sum;
    }
    return engine_ ? engine_->journalDropped.load(std::memory_order_relaxed) : 0u;
}

// ---------------------------------------------------------------------------
// Backend description
// ---------------------------------------------------------------------------

const char* EngineHandle::driverName() const {
    if (remote_) return remote_->driverName;
    return audio_ ? audio_->name() : nullptr;
}
f64 EngineHandle::driverSampleRate() const {
    if (remote_) return sampleRate();
    return audio_ ? audio_->sampleRate() : 0.0;
}
int EngineHandle::driverBufferSize() const {
    if (remote_) return (int)remote_->cli.blockSize();
    return audio_ ? audio_->bufferSize() : 0;
}
// All three answer "no" in daemon mode rather than lying about the GUI's own
// (unstarted) MidiInput. The status bar draws exactly that, which is the
// intended reading: there is no hardware MIDI on this path yet.
bool EngineHandle::midiRunning() const { return remote_ ? false : midi_.running(); }
int  EngineHandle::midiClientId() const { return remote_ ? -1 : midi_.clientId(); }
u64  EngineHandle::midiReceived() const { return remote_ ? 0u : midi_.received(); }

u64 EngineHandle::remoteRefusals() const { return remote_ ? remote_->refusals : 0u; }
u64 EngineHandle::snapshotTears() const  { return remote_ ? remote_->tears : 0u; }

} // namespace lat
