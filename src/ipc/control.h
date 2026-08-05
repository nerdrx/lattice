// Lattice IPC — the control region: what actually travels between latticed and
// the GUI, and where it lives inside a ShmRegion.
//
// shm.h is the transport (a validated mapping and an SPSC ring); this header is
// the protocol: the wire message shapes, the region map, and the layout hash
// both sides fold into ShmRegion::attach() so a mismatched build fails at
// startup instead of reading a ring through the wrong offset.
//
// Region map (docs/PROCESS-SPLIT.md §3.3), all offsets past ShmHeader:
//
//   kHeader   ControlHeader                 protocol version, heartbeat counter,
//                                           shutdown flag, boundary counters,
//                                           the sample-pool handshake
//   kState    ipc::SharedState              the polled atomics block
//   kCmds     ShmSpscRing<WireCommand,4096> GUI -> engine
//   kEvts     ShmSpscRing<WireEvent,4096>   engine -> GUI
//   kMidi     ShmSpscRing<WireMidi,1024>    GUI/MIDI reader -> engine
//   kClips    WireClip[32][32]              the clip table, GUI -> engine
//
// The doc's map has three payload sections; this adds two. ControlHeader exists
// because ShmHeader is the *transport's* header (magic, layout, creator
// liveness) and must not grow protocol fields — kShmVersion says "these two
// builds agree about how a region is shaped", kProtocolVersion says "these two
// builds agree about what the messages mean", and they change for different
// reasons. The MIDI ring is here because Engine::pushMidi() already exists and
// a phase-1 daemon that could not carry MIDI would be a regression against the
// in-process build.
//
// Header-only, like shm.h, and it deliberately pulls in nothing but core/ and
// audio/engine.h — the enums, not the Engine — because Cmd and Ev *are* the
// protocol's vocabulary and a second, hand-copied definition of them in the
// IPC layer would drift the first time somebody adds a command.
#pragma once
#include "../audio/engine.h"
#include "pool.h"
#include "shm.h"

namespace lat::ipc {

// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------

// Bump on any change to the meaning of a wire message, a reject reason, or a
// ControlHeader field. Layout changes bump kShmVersion (shm.h) instead — and
// the layout hash below catches the times we forget either.
//
//   v2 — the sample pool (phase 2): the clip table, the pool handshake in
//        ControlHeader, EvClipAccepted/EvBlockRetired, and SetClip/ClearClip
//        moving from "refused" to "accepted".
inline constexpr u32 kProtocolVersion = 2;

// Daemon-generated wire events start here, well clear of lat::Ev. The event
// ring carries a superset of Ev: the boundary itself has things to report
// (a refused command, "I am going away") that no engine ever needs to say.
inline constexpr u32 kDaemonEventBase = 0x1000;

enum : u32 {
    EvCommandRejected = kDaemonEventBase + 0,  // a = Cmd, b = RejectReason
    EvEngineStopping  = kDaemonEventBase + 1,  // clean shutdown has begun
    EvEventDropped    = kDaemonEventBase + 2,  // a = Ev that could not cross

    // The daemon has finished with clip cell (a, b): ref = the generation it
    // acted on, x = a RejectReason, flags per EvClipAckFlag below. Exactly one
    // of these answers every SetClip/ClearClip, accepted or refused. It is a
    // *flow-control* acknowledgement, not permission to free anything: it is
    // what lets the client know a cell may be rewritten, and what tells it
    // whether the engine took the write (see WireClip::generation).
    EvClipAck         = kDaemonEventBase + 3,

    // ref = a pool offset the engine can no longer reach; a, b = the clip cell
    // it was displaced from; flags = PoolKind*. **The only thing that
    // authorises a free.** See the free-after-confirm rule in pool.h and the
    // proof it rests on in src/daemon/latticed.cpp.
    EvBlockRetired    = kDaemonEventBase + 4,

    // The daemon has mapped the pool named in ControlHeader; ref = the epoch,
    // x = the mapped byte count. Purely informational — the client may publish
    // clips before it arrives, they are simply refused until the pool is in.
    EvPoolAttached    = kDaemonEventBase + 5,
};

// EvClipAck::flags.
enum : u32 {
    ClipAckWasClear = 1u << 0,   // the command was Cmd::ClearClip
    ClipAckRefused  = 1u << 1,   // the engine did not get it; x says why
};

// Why the daemon refused a command. The pointer-payload family is what remains
// of phase 1's scalars-only rule: SetChain and the two Record commands still
// carry GUI-heap addresses and still cannot cross (phase 3). The pool family
// below it is new, and every one of those reasons is a bad offset caught before
// it could become a pointer.
enum : u32 {
    RejectNone           = 0,
    RejectPointerPayload = 1,  // SetChain / RecordSlot / RecordMidiSlot
    RejectUnknownCommand = 2,
    RejectBadIndex       = 3,  // track/slot out of range
    RejectNotFinite      = 4,  // NaN/inf in x
    RejectNoPool         = 5,  // a clip references the pool and none is mapped
    RejectBadPoolRef     = 6,  // offset failed poolValidate() — see the log line
    RejectBadClip        = 7,  // the clip's own scalars are inconsistent
};

inline const char* rejectReasonName(u32 r) {
    switch (r) {
        case RejectPointerPayload: return "carries a pointer (phase 3)";
        case RejectUnknownCommand: return "unknown command type";
        case RejectBadIndex:       return "track/slot index out of range";
        case RejectNotFinite:      return "non-finite scalar";
        case RejectNoPool:         return "no sample pool is attached";
        case RejectBadPoolRef:     return "sample pool offset failed validation";
        case RejectBadClip:        return "clip fields are inconsistent";
        default:                   return "none";
    }
}

// ---------------------------------------------------------------------------
// Wire types
// ---------------------------------------------------------------------------
//
// Fixed width, no pointers, no bool, trivially copyable — the ring's static
// asserts enforce the last one and the reviews enforce the rest. `ref` is the
// field every pointer becomes: a pool offset in phase 2, a device id in phase
// 3. It is carried now, unused, so adding those phases does not change the
// region layout.

struct WireCommand {
    u32 type;        // lat::Cmd
    u32 flags;
    i32 a, b;
    f64 x;
    u64 ref;
};

struct WireEvent {
    u32 type;        // lat::Ev, or one of the Ev* daemon codes above
    u32 flags;
    i32 a, b;
    f64 x;
    u64 ref;
};

// The wire twin of lat::MidiMsg. Identical layout by construction (asserted
// below), so the daemon's translation is a field copy the compiler folds away,
// and a future change to MidiMsg cannot silently change the wire.
struct WireMidi {
    u8  status, d1, d2, pad;
    i32 frame;
};

static_assert(std::is_trivially_copyable_v<WireCommand>);
static_assert(std::is_trivially_copyable_v<WireEvent>);
static_assert(std::is_trivially_copyable_v<WireMidi>);
static_assert(sizeof(WireCommand) == 32 && sizeof(WireEvent) == 32);
static_assert(sizeof(WireMidi) == sizeof(MidiMsg), "WireMidi must mirror MidiMsg");

// ---------------------------------------------------------------------------
// WireClip — lat::RtClip with the two pointers replaced by pool offsets
// ---------------------------------------------------------------------------
//
// Every scalar of RtClip, plus `sampleRef`/`notesRef` where `data` and `notes`
// used to be. This is the whole of §2.4 and half of §2.5's problem, solved: the
// GUI names sample data by an offset into a region the daemon also maps, and
// the daemon does the one addition that turns it back into a pointer.
//
// It travels in a *table*, not in the command, for the reason §3.4 gives: 112
// bytes do not fit a 32-byte message, and of the two ways to carry them the
// table is idempotent (a cell can be rewritten and re-sent with no ordering
// question) and makes republish-after-engine-restart a memcpy rather than a
// protocol. Cmd::SetClip{a=track, b=slot, ref=generation} says which cell moved.
//
// `generation` is the cell's own counter, bumped by the client on every write
// and echoed by the daemon in EvClipAccepted. It exists because a table is
// mutable state shared with a peer that reads it later: if the client wrote a
// cell twice before the daemon popped the first command, the daemon would read
// the *second* value for both commands and never learn what the first one
// displaced — a pool block that is retired on the client's books and never
// retired on the daemon's, i.e. a leak. So the client refuses to overwrite a
// cell that has not been acknowledged yet and retries on the next frame,
// which is the same "handle a refused push" discipline §5 already requires of
// every ring push. One 1 ms pump tick is the entire cost.
struct WireClip {
    u64 sampleRef;      // pool offset of interleaved f32, 0 = none  <-- was const f32*
    u64 notesRef;       // pool offset of WireNote[], 0 = none       <-- was const RtNote*
    i64 frames;
    i64 loopStart, loopEnd;
    i64 noteCount;
    f64 clipBpm, lengthBeats, prob, followBeats;
    f32 gain;
    i32 channels;
    i32 warp;           // lat::Warp
    i32 quantumIdx;     // -1 = follow the global quantum
    i32 followAction;   // lat::Follow
    u32 loop;           // u32, never bool
    u32 isMidi;
    u32 valid;
    u32 generation;     // +1 per client write to this cell
    u32 reserved;       // spelled out rather than left to the padding
};

static_assert(std::is_trivially_copyable_v<WireClip>);
static_assert(sizeof(WireClip) == 120, "WireClip is part of the region layout");
static_assert(alignof(WireClip) == 8);

// Defaults that match RtClip's, so a client can send a half-filled cell and get
// the engine's documented behaviour rather than a clip at 0 BPM.
inline WireClip defaultWireClip() {
    WireClip c{};
    c.channels     = 1;
    c.clipBpm      = 120.0;
    c.lengthBeats  = 4.0;
    c.gain         = 1.0f;
    c.warp         = (i32)Warp::Beats;
    c.loop         = 1;
    c.quantumIdx   = -1;
    c.prob         = 1.0;
    c.followAction = (i32)Follow::None;
    return c;
}

// The name the task-level API uses for the same thing; RtClip's wire twin is
// only ever sent by Cmd::SetClip, so both spellings mean this struct.
using WireSetClip = WireClip;

// ---------------------------------------------------------------------------
// Command policy
// ---------------------------------------------------------------------------
//
// Three classes now, where phase 1 had two.
//
//   scalar   Seventeen commands whose whole payload is `a`, `b` and `x`
//            (docs/PROCESS-SPLIT.md §2.1). They cross unchanged.
//   pooled   SetClip and ClearClip. Their payload is a WireClip in the clip
//            table, whose pointers have become pool offsets — so they cross,
//            but only after the daemon has validated every offset against its
//            own mapping of the pool. This is phase 2's entire delta.
//   refused  SetChain and the two Record commands. Still GUI-heap addresses,
//            still refused at the boundary with a reason rather than
//            half-translated: a SetChain whose `p` was silently zeroed is a
//            track that loses its plugins, and you find that on stage.
//
//   SetChain        Command::p is an RtChain* full of PluginInstance*
//                                                              -> phase 3 ids
//   RecordSlot      Command::p is a GUI-owned capture buffer    -> phase 3
//   RecordMidiSlot  Command::p is a GUI-owned RtNote buffer     -> phase 3
inline constexpr bool commandIsScalar(u32 type) {
    switch ((Cmd)type) {
        case Cmd::SetPlaying: case Cmd::SetTempo: case Cmd::SetQuantum:
        case Cmd::SetMetronome: case Cmd::LaunchClip: case Cmd::StopTrack:
        case Cmd::LaunchScene: case Cmd::StopAll: case Cmd::TrackVol:
        case Cmd::TrackPan: case Cmd::TrackMute: case Cmd::TrackSolo:
        case Cmd::TrackArm: case Cmd::MasterVol: case Cmd::ClipGain:
        case Cmd::ClipWarp: case Cmd::ClipLoop:
            return true;
        case Cmd::SetClip: case Cmd::ClearClip: case Cmd::SetChain:
        case Cmd::RecordSlot: case Cmd::RecordMidiSlot:
            return false;
    }
    return false;
}

// Carries a clip cell rather than a scalar: the daemon reads the table, not
// the command.
inline constexpr bool commandIsPooled(u32 type) {
    return (Cmd)type == Cmd::SetClip || (Cmd)type == Cmd::ClearClip;
}

// Cannot cross at all, in this phase or any until §3.6 lands.
inline constexpr bool commandCarriesPointer(u32 type) {
    return !commandIsScalar(type) && !commandIsPooled(type);
}

// True for a type this build knows at all. An unknown type is a peer from the
// future; the version check should already have caught it, so this is the
// belt to that pair of braces.
inline constexpr bool commandIsKnown(u32 type) {
    return type <= (u32)Cmd::RecordMidiSlot;
}

// Events that cannot cross as they stand: each hands a pointer back to whoever
// allocated it. Ev::NotesRetired is the one that changed in phase 2 — it is no
// longer dropped but *translated*, because its pointer now lands inside the
// sample pool and the daemon can turn it back into the offset the GUI knows it
// by (EvBlockRetired). It stays false here because it still may not be
// forwarded verbatim; see Daemon::pumpEvents.
//
// The other three remain unreachable, because the commands that would allocate
// their payloads are still refused. If their counter ever moves, something
// reached the engine that should not have.
inline constexpr bool eventIsScalar(u32 type) {
    switch ((Ev)type) {
        case Ev::ClipStarted: case Ev::ClipStopped: case Ev::TrackStopped:
        case Ev::Xrun: case Ev::TransportStopped: case Ev::RecordStarted:
            return true;
        case Ev::ChainRetired: case Ev::RecordFinished: case Ev::NotesRetired:
        case Ev::MidiRecordFinished:
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ControlHeader
// ---------------------------------------------------------------------------
//
// Written by the daemon, read by everyone. The counters are not decoration:
// they are the only way a client (or a test) can tell "the daemon accepted my
// command" from "the daemon threw it away", which matters precisely because
// phase 1 throws some away on purpose.
struct ControlHeader {
    u32 protocolVersion;             // kProtocolVersion of the daemon
    u32 flags;
    i32 daemonPid;
    u32 driverIsNull;                // 1 = --driver null (no audio device)

    std::atomic<u64> heartbeat;      // +1 per daemon pump tick
    std::atomic<u64> startedNs;      // CLOCK_MONOTONIC at publishReady()
    std::atomic<u32> shutdown;       // 1 = the daemon has stopped cleanly
    std::atomic<u32> reserved0;

    // Boundary accounting.
    std::atomic<u64> commandsApplied;   // forwarded to Engine::pushCommand
    std::atomic<u64> commandsRejected;  // refused, with an EvCommandRejected
    std::atomic<u64> commandsDeferred;  // engine ring full, retried next tick
    std::atomic<u64> midiApplied;
    std::atomic<u64> eventsForwarded;
    std::atomic<u64> eventsDropped;     // pointer-carrying, cannot cross
    std::atomic<u64> clipsApplied;      // clip cells forwarded to the engine
    std::atomic<u64> blocksRetired;     // EvBlockRetired events published

    char driverName[32];             // "null", "JACK", "ALSA"

    // --- the sample-pool handshake (docs/PROCESS-SPLIT.md §3.5) ------------
    //
    // The only fields in this region the *client* writes. That inversion is
    // the pool's ownership inversion showing through: the GUI creates the pool
    // and therefore the GUI is the one with something to announce. Without a
    // socket to pass an fd over (§3.2) the announcement is a name plus an
    // epoch, published here.
    //
    // Ordering: the client fills poolName and poolBytes, then stores poolEpoch
    // with release. The daemon loads poolEpoch with acquire and only then
    // reads the name, so it can never map a half-written string. The daemon
    // answers in poolAttachedEpoch.
    // 96 bytes, matching ShmRegion's own name limit: a truncated region name
    // would be a name the daemon could not open, or worse, could.
    char poolName[96];                  // client -> daemon, e.g. /lattice-pool-foo
    std::atomic<u64> poolBytes;         // client: payload bytes of that region
    std::atomic<u64> poolEpoch;         // client: +1 per published pool, 0 = none
    std::atomic<u64> poolAttachedEpoch; // daemon: the epoch it has mapped, 0 = none
    std::atomic<u64> poolAttachFailures;// daemon: attaches that did not work
    u32  reserved[8];

    // Creator only, before publishReady().
    void init(i32 pid, bool nullDriver, const char* driver) {
        protocolVersion = kProtocolVersion;
        flags           = 0;
        daemonPid       = pid;
        driverIsNull    = nullDriver ? 1u : 0u;
        heartbeat.store(0, std::memory_order_relaxed);
        startedNs.store(monotonicNs(), std::memory_order_relaxed);
        shutdown.store(0, std::memory_order_relaxed);
        reserved0.store(0, std::memory_order_relaxed);
        commandsApplied.store(0, std::memory_order_relaxed);
        commandsRejected.store(0, std::memory_order_relaxed);
        commandsDeferred.store(0, std::memory_order_relaxed);
        midiApplied.store(0, std::memory_order_relaxed);
        eventsForwarded.store(0, std::memory_order_relaxed);
        eventsDropped.store(0, std::memory_order_relaxed);
        clipsApplied.store(0, std::memory_order_relaxed);
        blocksRetired.store(0, std::memory_order_relaxed);
        std::memset(driverName, 0, sizeof driverName);
        std::snprintf(driverName, sizeof driverName, "%s", driver ? driver : "?");
        std::memset(poolName, 0, sizeof poolName);
        poolBytes.store(0, std::memory_order_relaxed);
        poolEpoch.store(0, std::memory_order_relaxed);
        poolAttachedEpoch.store(0, std::memory_order_relaxed);
        poolAttachFailures.store(0, std::memory_order_relaxed);
        for (u32& r : reserved) r = 0;
    }
};

// ---------------------------------------------------------------------------
// Region layout
// ---------------------------------------------------------------------------
//
// Capacities: 4096 slots each way, up from lat::Ring's 1024, because a process
// boundary makes bursts worse rather than better — the GUI can be descheduled
// for a whole frame while a scene launch queues 32 commands, and the daemon
// still only drains at its pump tick. 4096 * 32 B is 128 KiB a side; the whole
// control region is well under a megabyte.

using CommandRing = ShmSpscRing<WireCommand, 4096>;
using EventRing   = ShmSpscRing<WireEvent, 4096>;
using MidiRing    = ShmSpscRing<WireMidi, 1024>;

// The clip table. 32 x 32 x 120 B is 120 KiB — the same order as one ring, and
// preallocated for the same reason everything else here is: the engine cannot
// wait for an allocation and a republish must not need one either.
inline constexpr size_t kClipTableBytes = sizeof(WireClip) * kMaxTracks * kMaxScenes;

namespace control {

inline constexpr size_t kHeader = 0;
inline constexpr size_t kState  = alignUp(kHeader + sizeof(ControlHeader),  kCacheLine);
inline constexpr size_t kCmds   = alignUp(kState  + sizeof(SharedState),    kCacheLine);
inline constexpr size_t kEvts   = alignUp(kCmds   + CommandRing::bytes(),   kCacheLine);
inline constexpr size_t kMidi   = alignUp(kEvts   + EventRing::bytes(),     kCacheLine);
inline constexpr size_t kClips  = alignUp(kMidi   + MidiRing::bytes(),      kCacheLine);
inline constexpr size_t kBytes  = kClips + kClipTableBytes;

// Everything that could move an offset out from under a peer goes into the
// hash: the total size, every section offset, both message sizes, the ring
// capacities and the protocol version.
inline constexpr u32 kHash =
    hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(
        fnv1a("lattice.control.v2"),
        (u64)kBytes), (u64)kState), (u64)kCmds), (u64)kEvts), (u64)kMidi), (u64)kClips),
        (u64)(sizeof(WireCommand) * 65536 + sizeof(WireEvent) * 256 + sizeof(WireMidi))),
        (u64)(CommandRing::capacity() * 65536ull + EventRing::capacity()) ^
        (u64)(kProtocolVersion * 65536u + (u32)sizeof(WireClip)));

} // namespace control

// The default region name for a session. POSIX shm names are one path
// component, so the session id is pasted in rather than nested.
inline void controlRegionName(const char* session, char* out, size_t cap) {
    std::snprintf(out, cap, "/lattice-engine-%s", (session && *session) ? session : "default");
}
// The pool's name lives in pool.h (poolRegionName) because the pool is not part
// of the control protocol: the GUI could hand the daemon any name it likes
// through ControlHeader::poolName, and the default is only a default.

// ---------------------------------------------------------------------------
// ControlMap — the five pointers, resolved once
// ---------------------------------------------------------------------------
//
// at<T>() is bounds- and alignment-checked, so a layout mistake surfaces here,
// at startup, on both sides. Nothing below ever recomputes an offset.
struct ControlMap {
    ControlHeader* hdr   = nullptr;
    SharedState*   state = nullptr;
    CommandRing*   cmds  = nullptr;
    EventRing*     evts  = nullptr;
    MidiRing*      midi  = nullptr;
    WireClip*      clips = nullptr;      // [kMaxTracks * kMaxScenes], row-major

    bool valid() const { return hdr && state && cmds && evts && midi && clips; }

    // The clip table is one flat array with an accessor rather than a 2-D
    // pointer, so the region layout has one offset in it and the index
    // arithmetic lives in exactly one place on both sides.
    WireClip* clip(int track, int slot) {
        if (!clips || track < 0 || track >= kMaxTracks || slot < 0 || slot >= kMaxScenes)
            return nullptr;
        return clips + (size_t)track * kMaxScenes + (size_t)slot;
    }
    const WireClip* clip(int track, int slot) const {
        return const_cast<ControlMap*>(this)->clip(track, slot);
    }

    // Creator: adopt the memory and reset every ring. Must run before
    // ShmRegion::publishReady().
    bool create(ShmRegion& r) {
        hdr   = r.at<ControlHeader>(control::kHeader);
        state = r.at<SharedState>(control::kState);
        cmds  = CommandRing::createAt(r, control::kCmds);
        evts  = EventRing::createAt(r, control::kEvts);
        midi  = MidiRing::createAt(r, control::kMidi);
        clips = r.at<WireClip>(control::kClips);
        // The table needs the last WireClip to fit too, which at<T>() cannot
        // know: it checks one T, and this is an array.
        if (clips && !r.at<WireClip>(control::kClips + kClipTableBytes - sizeof(WireClip)))
            clips = nullptr;
        return valid();
    }
    // Attacher: adopt the memory, touching nothing.
    bool attach(ShmRegion& r) {
        hdr   = r.at<ControlHeader>(control::kHeader);
        state = r.at<SharedState>(control::kState);
        cmds  = CommandRing::attachAt(r, control::kCmds);
        evts  = EventRing::attachAt(r, control::kEvts);
        midi  = MidiRing::attachAt(r, control::kMidi);
        clips = r.at<WireClip>(control::kClips);
        if (clips && !r.at<WireClip>(control::kClips + kClipTableBytes - sizeof(WireClip)))
            clips = nullptr;
        return valid();
    }
    void clear() { *this = ControlMap{}; }
};

} // namespace lat::ipc
