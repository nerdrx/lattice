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
//                                           shutdown flag, boundary counters
//   kState    ipc::SharedState              the polled atomics block
//   kCmds     ShmSpscRing<WireCommand,4096> GUI -> engine
//   kEvts     ShmSpscRing<WireEvent,4096>   engine -> GUI
//   kMidi     ShmSpscRing<WireMidi,1024>    GUI/MIDI reader -> engine
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
#include "shm.h"

namespace lat::ipc {

// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------

// Bump on any change to the meaning of a wire message, a reject reason, or a
// ControlHeader field. Layout changes bump kShmVersion (shm.h) instead — and
// the layout hash below catches the times we forget either.
inline constexpr u32 kProtocolVersion = 1;

// Daemon-generated wire events start here, well clear of lat::Ev. The event
// ring carries a superset of Ev: the boundary itself has things to report
// (a refused command, "I am going away") that no engine ever needs to say.
inline constexpr u32 kDaemonEventBase = 0x1000;

enum : u32 {
    EvCommandRejected = kDaemonEventBase + 0,  // a = Cmd, b = RejectReason
    EvEngineStopping  = kDaemonEventBase + 1,  // clean shutdown has begun
    EvEventDropped    = kDaemonEventBase + 2,  // a = Ev that could not cross
};

// Why the daemon refused a command. Phase 1 is scalar-only, so most of these
// are "this message carries a pointer and pointers do not cross a process
// boundary" — see kPhase2 below.
enum : u32 {
    RejectNone          = 0,
    RejectPointerPayload = 1,  // SetClip/ClearClip/SetChain/Record*Slot
    RejectUnknownCommand = 2,
    RejectBadIndex       = 3,  // track/slot out of range
    RejectNotFinite      = 4,  // NaN/inf in x
};

inline const char* rejectReasonName(u32 r) {
    switch (r) {
        case RejectPointerPayload: return "carries a pointer (phase 2)";
        case RejectUnknownCommand: return "unknown command type";
        case RejectBadIndex:       return "track/slot index out of range";
        case RejectNotFinite:      return "non-finite scalar";
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
// Phase-1 command policy
// ---------------------------------------------------------------------------
//
// Eighteen of twenty commands are pure scalars (docs/PROCESS-SPLIT.md §2.1) and
// cross unchanged. The rest are refused at the daemon boundary rather than
// half-translated, because a SetClip whose `data` pointer was silently zeroed
// would be a clip that plays silence — a bug you find on stage. Refusing is
// loud: a log line, a counter, and an EvCommandRejected back to the sender.
//
//   SetClip         RtClip::data is a GUI-heap pointer         -> phase 2 pool
//   ClearClip       clears a clip that cannot exist yet, and its retirement
//                   event (Ev::NotesRetired) hands a pointer back             .
//   SetChain        Command::p is an RtChain* full of PluginInstance*
//                                                              -> phase 3 ids
//   RecordSlot      Command::p is a GUI-owned capture buffer    -> phase 2
//   RecordMidiSlot  Command::p is a GUI-owned RtNote buffer     -> phase 2
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

// True for a type this build knows at all. An unknown type is a peer from the
// future; the version check should already have caught it, so this is the
// belt to that pair of braces.
inline constexpr bool commandIsKnown(u32 type) {
    return type <= (u32)Cmd::RecordMidiSlot;
}

// Events that cannot cross: every one of them hands a pointer back to whoever
// allocated it. In phase 1 they are unreachable — the commands that create the
// allocations are refused above — so seeing one is a bug worth counting.
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

    char driverName[32];             // "null", "JACK", "ALSA"
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
        std::memset(driverName, 0, sizeof driverName);
        std::snprintf(driverName, sizeof driverName, "%s", driver ? driver : "?");
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

namespace control {

inline constexpr size_t kHeader = 0;
inline constexpr size_t kState  = alignUp(kHeader + sizeof(ControlHeader),  kCacheLine);
inline constexpr size_t kCmds   = alignUp(kState  + sizeof(SharedState),    kCacheLine);
inline constexpr size_t kEvts   = alignUp(kCmds   + CommandRing::bytes(),   kCacheLine);
inline constexpr size_t kMidi   = alignUp(kEvts   + EventRing::bytes(),     kCacheLine);
inline constexpr size_t kBytes  = kMidi + MidiRing::bytes();

// Everything that could move an offset out from under a peer goes into the
// hash: the total size, every section offset, both message sizes, the ring
// capacities and the protocol version.
inline constexpr u32 kHash =
    hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(
        fnv1a("lattice.control.v1"),
        (u64)kBytes), (u64)kState), (u64)kCmds), (u64)kEvts), (u64)kMidi),
        (u64)(sizeof(WireCommand) * 65536 + sizeof(WireEvent) * 256 + sizeof(WireMidi))),
        (u64)(CommandRing::capacity() * 65536ull + EventRing::capacity()) ^ (u64)kProtocolVersion);

} // namespace control

// The default region name for a session. POSIX shm names are one path
// component, so the session id is pasted in rather than nested.
inline void controlRegionName(const char* session, char* out, size_t cap) {
    std::snprintf(out, cap, "/lattice-engine-%s", (session && *session) ? session : "default");
}

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

    bool valid() const { return hdr && state && cmds && evts && midi; }

    // Creator: adopt the memory and reset every ring. Must run before
    // ShmRegion::publishReady().
    bool create(ShmRegion& r) {
        hdr   = r.at<ControlHeader>(control::kHeader);
        state = r.at<SharedState>(control::kState);
        cmds  = CommandRing::createAt(r, control::kCmds);
        evts  = EventRing::createAt(r, control::kEvts);
        midi  = MidiRing::createAt(r, control::kMidi);
        return valid();
    }
    // Attacher: adopt the memory, touching nothing.
    bool attach(ShmRegion& r) {
        hdr   = r.at<ControlHeader>(control::kHeader);
        state = r.at<SharedState>(control::kState);
        cmds  = CommandRing::attachAt(r, control::kCmds);
        evts  = EventRing::attachAt(r, control::kEvts);
        midi  = MidiRing::attachAt(r, control::kMidi);
        return valid();
    }
    void clear() { *this = ControlMap{}; }
};

} // namespace lat::ipc
