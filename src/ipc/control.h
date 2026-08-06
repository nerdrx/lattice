// NxTakt IPC — the control region: what actually travels between nxtaktd and
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
//   v3 — devices (phase 3): AddDevice/RemoveDevice/MoveDevice/SetBypass, string
//        blobs through the pool, the device metadata table (daemon -> client)
//        and the param table (client -> daemon). Cmd::SendLevel/ReturnVol join
//        the scalars; Cmd::SetChain and its return/master siblings become
//        daemon-internal and are refused on the wire *permanently* rather than
//        pending a phase.
inline constexpr u32 kProtocolVersion = 3;

// Daemon-generated wire events start here, well clear of lat::Ev. The event
// ring carries a superset of Ev: the boundary itself has things to report
// (a refused command, "I am going away") that no engine ever needs to say.
inline constexpr u32 kDaemonEventBase = 0x1000;

// Daemon-*consumed* commands start here, well clear of lat::Cmd, and for the
// mirror-image reason: the boundary has things done to it that no Engine ever
// hears about. `AddDevice` is the archetype — it names a plugin by URI,
// instantiates it in the daemon's address space, and only then turns into a
// `Cmd::SetChain` the engine understands. lat::Cmd stays the engine's
// vocabulary; this is the boundary's.
inline constexpr u32 kDaemonCommandBase = 0x1000;

enum : u32 {
    // flags = DevTarget*, a = track/return index (ignored for master),
    // b = chain position (-1 appends), ref = pool offset of a PoolKindString
    // holding the plugin URI. Answered by exactly one EvDeviceAdded or
    // EvDeviceFailed, and the URI blob is retired (EvBlockRetired) either way.
    CmdAddDevice    = kDaemonCommandBase + 0,

    // ref = device id. Answered by EvDeviceRemoved (or EvDeviceFailed).
    CmdRemoveDevice = kDaemonCommandBase + 1,

    // ref = device id, b = new position within its own chain. Answered by
    // EvDeviceChanged.
    CmdMoveDevice   = kDaemonCommandBase + 2,

    // ref = device id, a = 0/1. A *command* and not a param-table write
    // because §3.7 says so and it is right: bypass has to land in a defined
    // order relative to the chain edits around it.
    CmdSetBypass    = kDaemonCommandBase + 3,

    // Force the plugin scan to start now rather than on the first AddDevice.
    // Purely an optimisation for a GUI that knows it is about to need the
    // catalog; the lazy path is identical.
    CmdScanPlugins  = kDaemonCommandBase + 4,
};

// What a chain belongs to. The engine has three chain commands with three
// different shapes (Cmd::SetChain / SetReturnChain / SetMasterChain); one enum
// on the wire keeps the client from having to know which is which.
enum : u32 {
    DevTargetTrack  = 0,   // a = track index
    DevTargetReturn = 1,   // a = return index
    DevTargetMaster = 2,   // a ignored
};

// ControlHeader::scanState. The plugin scan is lazy and asynchronous, which is
// §3.6 being honest: it always took a second, the in-process GUI just blocked
// on it.
enum : u32 {
    ScanIdle    = 0,   // not started; the first device command starts it
    ScanRunning = 1,
    ScanDone    = 2,
};

inline const char* devTargetName(u32 t) {
    switch (t) {
        case DevTargetTrack:  return "track";
        case DevTargetReturn: return "return";
        case DevTargetMaster: return "master";
        default:              return "?";
    }
}

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
    // proof it rests on in src/daemon/nxtaktd.cpp.
    EvBlockRetired    = kDaemonEventBase + 4,

    // The daemon has mapped the pool named in ControlHeader; ref = the epoch,
    // x = the mapped byte count. Purely informational — the client may publish
    // clips before it arrives, they are simply refused until the pool is in.
    EvPoolAttached    = kDaemonEventBase + 5,

    // --- devices (phase 3) -------------------------------------------------
    //
    // A device command is answered exactly once, the same discipline EvClipAck
    // established: a silent refusal would leave a GUI showing an
    // "instantiating…" strip forever.

    // The plugin is loaded, prepared and in a published chain.
    // ref = device id, flags = DevTarget*, a = target index, b = chain
    // position, x = param count. **The metadata is not in this event** — it is
    // in the device table at `ControlMap::device(id)`, which the client reads
    // once and mirrors. See §11.3 for why a table and not a blob.
    EvDeviceAdded     = kDaemonEventBase + 6,

    // ref = the URI blob offset the client sent (0 if the command never named
    // one), b = a Reject* reason, a = DevTarget*. The daemon is still alive;
    // that is the whole point of answering rather than dying.
    EvDeviceFailed    = kDaemonEventBase + 7,

    // ref = device id. The instance is destroyed, the chain is republished and
    // the id is free for reuse — with a bumped WireDeviceInfo::generation, so
    // a param write aimed at the old occupant cannot land on the new one.
    EvDeviceRemoved   = kDaemonEventBase + 8,

    // ref = device id. Something in the device's table row changed that the
    // client did not write: a move (b = the new position), a bypass the daemon
    // applied (flags & DeviceChangedBypass), a re-published chain.
    EvDeviceChanged   = kDaemonEventBase + 9,

    // The plugin scan finished. a = plugin count, x = seconds it took.
    EvScanComplete    = kDaemonEventBase + 10,
};

// EvDeviceChanged::flags.
enum : u32 {
    DeviceChangedMoved  = 1u << 0,
    DeviceChangedBypass = 1u << 1,
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

    // --- devices (phase 3) --------------------------------------------------
    RejectUnknownUri     = 8,  // the scan does not know that URI
    RejectInstantiate    = 9,  // the plugin refused to load or to activate
    RejectDeviceTableFull= 10, // kMaxDevices devices already exist
    RejectChainFull      = 11, // kMaxChainFx devices already on that chain
    RejectBadDevice      = 12, // no such device id, or its generation is stale
    RejectBadString      = 13, // the URI blob is not a terminated string
    RejectScanBusy       = 14, // the scan is running and the queue is full
};

inline const char* rejectReasonName(u32 r) {
    switch (r) {
        case RejectPointerPayload:  return "carries a pointer the daemon owns";
        case RejectUnknownCommand:  return "unknown command type";
        case RejectBadIndex:        return "track/slot index out of range";
        case RejectNotFinite:       return "non-finite scalar";
        case RejectNoPool:          return "no sample pool is attached";
        case RejectBadPoolRef:      return "sample pool offset failed validation";
        case RejectBadClip:         return "clip fields are inconsistent";
        case RejectUnknownUri:      return "no plugin with that URI was found";
        case RejectInstantiate:     return "the plugin failed to load or activate";
        case RejectDeviceTableFull: return "the device table is full";
        case RejectChainFull:       return "the chain is full";
        case RejectBadDevice:       return "no such device";
        case RejectBadString:       return "the string blob is not terminated";
        case RejectScanBusy:        return "the plugin scan is busy and the queue is full";
        default:                    return "none";
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
// Devices: two tables, opposite directions
// ---------------------------------------------------------------------------
//
// Phase 3's problem is §2.5's: `RtChain::fx` is an array of PluginInstance*,
// and both the pointer and the object it names have to stop crossing. They do,
// by the plugin moving into the daemon (§3.6). What is left crossing is
// *description* and *value*, and those pull in opposite directions, so they get
// one table each:
//
//   device table   WireDeviceInfo[kMaxDevices]    DAEMON writes, client reads.
//                  What a loaded plugin is: uri, name, latency, and one
//                  WireParamInfo per control. Static per instance, so it is
//                  written once at AddDevice and read once at EvDeviceAdded.
//
//   param table    WireDeviceParams[kMaxDevices]  CLIENT writes, daemon reads.
//                  What its controls are currently set to. Written at knob
//                  rate, scanned by the daemon's pump every millisecond.
//
// Both are preallocated in the control region for the reason everything here
// is: no side may wait for an allocation, and a republish after an engine
// restart must not need one either.
//
// §3.7 puts the param table in the *session* region instead, so that it
// survives an engine restart the way the sample pool does. It is here because
// the device *ids* it is indexed by are the daemon's, and they do not survive:
// a respawned daemon re-instantiates from scratch and hands out fresh ids, so
// a surviving table would be indexed by numbers that no longer mean anything.
// The client keeps its own mirror and re-pushes after a respawn, exactly as it
// re-pushes the clip table — see §11.4.

// A device id is an index into both tables. There are enough for every chain
// position the engine can address (32 tracks + 4 returns + master, 8 devices
// each = 296) with room to spare, so "the table is full" means a leak, not a
// large session.
inline constexpr u32 kMaxDevices   = 320;

// Controls per device that cross the boundary. §3.7 sketches 256; 64 covers
// every plugin in practice and keeps the table at 1.4 MiB instead of 5.5.
// A device with more reports the first 64 and says so in `truncatedParams`,
// which is a visible, testable degradation rather than a silent one.
inline constexpr u32 kMaxDevParams = 64;

static_assert(kMaxDevices >= (u32)(kMaxTracks * kMaxChainFx + kMaxReturns * kMaxChainFx + kMaxChainFx),
              "the device table must be able to hold every addressable chain position");

// WireParamInfo::flags — lat::ParamInfo's three booleans, as bits.
enum : u32 {
    ParamIsBool = 1u << 0,
    ParamIsInt  = 1u << 1,
    ParamIsLog  = 1u << 2,
};

// lat::ParamInfo with the two std::strings truncated to fixed widths. A name
// longer than 31 bytes is cut, never wrapped and never heap-allocated: this
// struct lives in shared memory and the whole point of the exercise is that
// nothing in it can point anywhere.
struct WireParamInfo {
    f32  min, max, def;
    u32  id;              // backend-defined; LV2 uses the port index
    u32  flags;           // ParamIs*
    u32  reserved;
    char name[32];        // NUL-terminated, truncated
    char unit[8];         // "dB", "Hz", ""
};
static_assert(std::is_trivially_copyable_v<WireParamInfo>);
static_assert(sizeof(WireParamInfo) == 64, "WireParamInfo is part of the region layout");

// WireDeviceInfo::state.
enum : u32 {
    DeviceSlotFree = 0,
    DeviceSlotLive = 1,
};

// Written by the daemon, read by the client. `state` and `generation` are the
// only atomics, and `state` is written *last* with release for the same reason
// PoolBlock::magic is: a reader that sees the slot live has seen every field
// that describes it.
struct WireDeviceInfo {
    std::atomic<u32> state;        // DeviceSlot*
    std::atomic<u32> generation;   // +1 per allocation of this slot; never reused
    std::atomic<u32> bypass;       // what the daemon actually applied
    u32 paramCount;                // <= kMaxDevParams
    u32 truncatedParams;           // controls beyond kMaxDevParams, 0 normally
    i32 target;                    // DevTarget*
    i32 targetIdx;                 // track or return index
    i32 chainPos;                  // position within that chain
    i32 latencyFrames;             // PluginInstance::latencyFrames()
    u32 format;                    // lat::PluginFormat
    u32 kind;                      // lat::PluginKind
    u32 audioIn, audioOut;
    u32 hasMidiIn;
    u32 reserved[2];
    char uri[128];
    char name[64];
    char vendor[64];
    WireParamInfo params[kMaxDevParams];
};
static_assert(std::is_trivially_copyable_v<WireDeviceInfo>);
static_assert(sizeof(WireDeviceInfo) == 320 + sizeof(WireParamInfo) * kMaxDevParams,
              "WireDeviceInfo is part of the region layout");

// Written by the client, read by the daemon's pump thread.
//
// This is §3.7 relocated and otherwise unchanged: the GUI stores a plain float
// and bumps a generation; the reader notices the generation and re-reads. No
// ring, therefore no drops — a dropped param write would leave the knob and the
// plugin permanently disagreeing, which is the worst bug class in this corner
// of the system.
//
// `deviceGeneration` is the one addition a process boundary forces. Device ids
// are reused, so a write that was in flight when a device was removed could
// otherwise land on its replacement. The client stamps the slot generation it
// believes it is talking to and the daemon ignores anything stale.
struct WireDeviceParams {
    std::atomic<u32> generation;        // client: +1 per write batch (release)
    std::atomic<u32> engineGeneration;  // daemon: +1 when the plugin moved a param
    std::atomic<u32> deviceGeneration;  // client: the WireDeviceInfo generation it wrote for
    std::atomic<u32> reserved;
    std::atomic<f32> value[kMaxDevParams];
};
static_assert(sizeof(WireDeviceParams) == 16 + 4 * kMaxDevParams,
              "WireDeviceParams is part of the region layout");
static_assert(std::atomic<f32>::is_always_lock_free,
              "a param table of non-lock-free atomics would put a futex in the pump");

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
//   device   The five CmdAddDevice-family codes above. They are not lat::Cmd
//            at all: the daemon consumes them, loads or unloads a plugin, and
//            *generates* the Cmd::SetChain the engine sees. Phase 3's delta.
//   refused  The three chain commands and the two Record commands. Their
//            `Command::p` is an address in whoever built them, and for the
//            chain family that is now permanently the daemon: a client has no
//            business naming an RtChain, because it has no RtChains. This is
//            no longer "not yet" — it is the design.
//
//   SetChain/SetReturnChain/SetMasterChain
//                   Command::p is an RtChain* full of PluginInstance*, built
//                   by the daemon from its own device table. Use AddDevice.
//   RecordSlot      Command::p is a GUI-owned capture buffer    -> phase 4
//   RecordMidiSlot  Command::p is a GUI-owned RtNote buffer     -> phase 4
inline constexpr bool commandIsScalar(u32 type) {
    if (type >= kDaemonCommandBase) return false;
    switch ((Cmd)type) {
        case Cmd::SetPlaying: case Cmd::SetTempo: case Cmd::SetQuantum:
        case Cmd::SetMetronome: case Cmd::LaunchClip: case Cmd::StopTrack:
        case Cmd::LaunchScene: case Cmd::StopAll: case Cmd::TrackVol:
        case Cmd::TrackPan: case Cmd::TrackMute: case Cmd::TrackSolo:
        case Cmd::TrackArm: case Cmd::MasterVol: case Cmd::ClipGain:
        case Cmd::ClipWarp: case Cmd::ClipLoop:
        // Phase 3: the bus topology's own scalars. SendLevel and ReturnVol are
        // pure numbers into the engine's mixer and always were; they only look
        // new because the engine grew return buses in the same wave.
        case Cmd::SendLevel: case Cmd::ReturnVol:
            return true;
        case Cmd::SetClip: case Cmd::ClearClip:
        case Cmd::SetChain: case Cmd::SetReturnChain: case Cmd::SetMasterChain:
        case Cmd::RecordSlot: case Cmd::RecordMidiSlot:
            return false;
    }
    return false;
}

// Carries a clip cell rather than a scalar: the daemon reads the table, not
// the command.
inline constexpr bool commandIsPooled(u32 type) {
    return type < kDaemonCommandBase &&
           ((Cmd)type == Cmd::SetClip || (Cmd)type == Cmd::ClearClip);
}

// Consumed by the daemon, never forwarded verbatim.
inline constexpr bool commandIsDevice(u32 type) {
    return type >= kDaemonCommandBase && type <= CmdScanPlugins;
}

// Names memory the sender does not own on the receiving side. Permanently
// refused, not deferred.
inline constexpr bool commandCarriesPointer(u32 type) {
    return !commandIsScalar(type) && !commandIsPooled(type) && !commandIsDevice(type);
}

// True for a type this build knows at all. An unknown type is a peer from the
// future; the version check should already have caught it, so this is the
// belt to that pair of braces.
inline constexpr bool commandIsKnown(u32 type) {
    return type <= (u32)Cmd::RecordMidiSlot || commandIsDevice(type);
}

// Events that cannot cross as they stand: each hands a pointer back to whoever
// allocated it. Two of the four are now *consumed* rather than dropped, and
// both stay false here because "may not be forwarded verbatim" is what this
// predicate means:
//
//   Ev::NotesRetired  (phase 2) its pointer lands inside the sample pool, so
//                     the daemon turns it back into the offset the client
//                     knows it by and republishes it as EvBlockRetired.
//   Ev::ChainRetired  (phase 3) its pointer is a chain the *daemon* built, so
//                     the daemon keeps it: the retirement dance §2.5 describes
//                     still happens, it just happens between two threads of
//                     one process now. §3.6 said this event would disappear
//                     from the protocol; it disappeared from the wire.
//
// The other three remain unreachable, because the commands that would allocate
// their payloads are still refused. If their counter ever moves, something
// reached the engine that should not have.
inline constexpr bool eventIsScalar(u32 type) {
    switch ((Ev)type) {
        case Ev::ClipStarted: case Ev::ClipStopped: case Ev::TrackStopped:
        case Ev::Xrun: case Ev::TransportStopped: case Ev::RecordStarted:
        // AutoLaneInert names a lane by index — no payload, so it crosses.
        case Ev::AutoLaneInert:
            return true;
        case Ev::ChainRetired: case Ev::RecordFinished: case Ev::NotesRetired:
        case Ev::MidiRecordFinished:
        // AutosRetired carries a pointer into GUI memory, like NotesRetired.
        case Ev::AutosRetired: case Ev::WarpRetired:
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

    // --- devices (phase 3) --------------------------------------------------
    std::atomic<u64> devicesAdded;      // instances created
    std::atomic<u64> devicesRemoved;    // instances destroyed
    std::atomic<u64> devicesFailed;     // AddDevice answered with EvDeviceFailed
    std::atomic<u64> devicesLive;       // instances alive right now
    std::atomic<u64> paramWrites;       // setParam() calls made from the pump
    std::atomic<u64> chainsPublished;   // Cmd::Set*Chain handed to the engine
    std::atomic<u64> chainsRetired;     // RtChains freed after their proof landed
    std::atomic<u32> scanState;         // ScanState below
    std::atomic<u32> scanPlugins;       // catalog size once the scan is done

    // --- the retirement proof (§11.5) ---------------------------------------
    //
    // Engine::drains counts completed drainCommands() passes and is the exact
    // primitive phase 2's retirement deadline was standing in for. It is
    // republished here for one reason beyond diagnostics: a client (or a test)
    // cannot see the daemon's Engine, so without this it could not tell an
    // engine that counts its drains from one that does not — and the daemon's
    // behaviour differs between the two.
    std::atomic<u64> engineDrains;
    std::atomic<u32> drainsExact;       // 1 = the engine counts; retirement is a proof
    std::atomic<u32> reserved1;

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
    char poolName[96];                  // client -> daemon, e.g. /nxtakt-pool-foo
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
        devicesAdded.store(0, std::memory_order_relaxed);
        devicesRemoved.store(0, std::memory_order_relaxed);
        devicesFailed.store(0, std::memory_order_relaxed);
        devicesLive.store(0, std::memory_order_relaxed);
        paramWrites.store(0, std::memory_order_relaxed);
        chainsPublished.store(0, std::memory_order_relaxed);
        chainsRetired.store(0, std::memory_order_relaxed);
        scanState.store(ScanIdle, std::memory_order_relaxed);
        scanPlugins.store(0, std::memory_order_relaxed);
        engineDrains.store(0, std::memory_order_relaxed);
        drainsExact.store(0, std::memory_order_relaxed);
        reserved1.store(0, std::memory_order_relaxed);
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

// 320 x 4.3 KiB of metadata and 320 x 272 B of values: 1.4 MiB, which on tmpfs
// costs one page per table until something is written into it. Preallocated for
// the same reason the clip table is — a device may not wait for an allocation,
// and neither may a republish.
inline constexpr size_t kDeviceTableBytes = sizeof(WireDeviceInfo)   * kMaxDevices;
inline constexpr size_t kParamTableBytes  = sizeof(WireDeviceParams) * kMaxDevices;

namespace control {

inline constexpr size_t kHeader  = 0;
inline constexpr size_t kState   = alignUp(kHeader  + sizeof(ControlHeader),  kCacheLine);
inline constexpr size_t kCmds    = alignUp(kState   + sizeof(SharedState),    kCacheLine);
inline constexpr size_t kEvts    = alignUp(kCmds    + CommandRing::bytes(),   kCacheLine);
inline constexpr size_t kMidi    = alignUp(kEvts    + EventRing::bytes(),     kCacheLine);
inline constexpr size_t kClips   = alignUp(kMidi    + MidiRing::bytes(),      kCacheLine);
inline constexpr size_t kDevices = alignUp(kClips   + kClipTableBytes,        kCacheLine);
inline constexpr size_t kParams  = alignUp(kDevices + kDeviceTableBytes,      kCacheLine);
inline constexpr size_t kBytes   = kParams + kParamTableBytes;

// Everything that could move an offset out from under a peer goes into the
// hash: the total size, every section offset, both message sizes, the ring
// capacities and the protocol version.
inline constexpr u32 kHash =
    hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(
        fnv1a("nxtakt.control.v3"),   // renamed seed: see the note at pool::kHash
        (u64)kBytes), (u64)kState), (u64)kCmds), (u64)kEvts), (u64)kMidi), (u64)kClips),
        (u64)kDevices), (u64)kParams),
        (u64)(sizeof(WireCommand) * 65536 + sizeof(WireEvent) * 256 + sizeof(WireMidi))),
        (u64)(CommandRing::capacity() * 65536ull + EventRing::capacity()) ^
        (u64)(kProtocolVersion * 65536u + (u32)sizeof(WireClip)) ^
        (u64)(sizeof(WireDeviceInfo) * 65536ull + sizeof(WireDeviceParams)));

} // namespace control

// The default region name for a session. POSIX shm names are one path
// component, so the session id is pasted in rather than nested.
inline void controlRegionName(const char* session, char* out, size_t cap) {
    std::snprintf(out, cap, "/nxtakt-engine-%s", (session && *session) ? session : "default");
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
    ControlHeader*    hdr     = nullptr;
    SharedState*      state   = nullptr;
    CommandRing*      cmds    = nullptr;
    EventRing*        evts    = nullptr;
    MidiRing*         midi    = nullptr;
    WireClip*         clips   = nullptr;   // [kMaxTracks * kMaxScenes], row-major
    WireDeviceInfo*   devices = nullptr;   // [kMaxDevices], daemon -> client
    WireDeviceParams* params  = nullptr;   // [kMaxDevices], client -> daemon

    bool valid() const {
        return hdr && state && cmds && evts && midi && clips && devices && params;
    }

    WireDeviceInfo* device(u32 id) {
        return (devices && id < kMaxDevices) ? devices + id : nullptr;
    }
    const WireDeviceInfo* device(u32 id) const {
        return const_cast<ControlMap*>(this)->device(id);
    }
    WireDeviceParams* param(u32 id) {
        return (params && id < kMaxDevices) ? params + id : nullptr;
    }
    const WireDeviceParams* param(u32 id) const {
        return const_cast<ControlMap*>(this)->param(id);
    }

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
        mapTables(r);
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
        mapTables(r);
        return valid();
    }
    void clear() { *this = ControlMap{}; }

private:
    // Same last-element check as the clip table, for the same reason: at<T>()
    // proves one T fits, and these are arrays of hundreds.
    void mapTables(ShmRegion& r) {
        devices = r.at<WireDeviceInfo>(control::kDevices);
        if (devices && !r.at<WireDeviceInfo>(control::kDevices + kDeviceTableBytes -
                                             sizeof(WireDeviceInfo)))
            devices = nullptr;
        params = r.at<WireDeviceParams>(control::kParams);
        if (params && !r.at<WireDeviceParams>(control::kParams + kParamTableBytes -
                                              sizeof(WireDeviceParams)))
            params = nullptr;
    }
};

} // namespace lat::ipc
