// The realtime engine.
//
// Threading contract:
//   * GUI thread  -> pushCommand()  -> lock-free ring -> audio thread
//   * audio thread -> pushEvent()   -> lock-free ring -> popEvent() on GUI
//   * scalar state the GUI polls (meters, playhead, clip states) lives in
//     std::atomic members.
// The audio thread never allocates, locks, or touches std::string.
#pragma once
#include "../core/common.h"
#include "../core/ring.h"
#include <atomic>

namespace lat {

enum class Warp : int { Off = 0, Repitch = 1, Beats = 2 };

// What a clip does when it has played `followBeats` (0 = its own length).
enum class Follow : int { None = 0, Stop, Again, Next, Previous, First, Random };
inline constexpr const char* kFollowNames[] = {"None", "Stop", "Again", "Next", "Prev", "First", "Random"};
inline constexpr int kFollowCount = 7;

// Clip slot state as seen by the UI.
enum class SlotState : int { Empty = 0, Stopped, Queued, Playing, StopQueued };

// One note of a MIDI clip, in clip-relative beats. Arrays are sorted by
// `beat`, heap-allocated by the GUI, shipped whole inside RtClip, and never
// mutated after publication; a replaced array travels back to the GUI via
// Ev::NotesRetired before it may be freed — the same lifetime protocol as
// RtChain, for the same reason: editing notes while the clip plays.
struct RtNote {
    f64 beat = 0.0;
    f64 len  = 0.25;
    u8  pitch = 60, vel = 100;
};

// ---------------------------------------------------------------------------
// Automation. Full design and rationale: docs/AUTOMATION.md.
//
// The rule the whole feature hangs on: the engine NEVER writes an automated
// value into the field it is automating. Track::vol stays the user's value and
// an envelope produces an effective gain beside it, which is why stopping,
// overriding, undoing and saving all need no cleanup. Device parameters are
// the one documented exception — a plugin has a single storage slot and no
// notion of "effective" — and that is exactly why they carry a restore
// obligation when playback stops.
// ---------------------------------------------------------------------------

// What an envelope automates. The engine switches on this; nothing else does.
enum class AutoTarget : i32 {
    None = 0,
    TrackVol,       // Track::vol,        transform Fader
    TrackPan,       // Track::pan,        transform Direct
    TrackSend,      // Track::send[index], Direct
    DeviceParam,    // chain[devSlot] parameter `index`, Direct
    // Reserved, in this order, so the numbering never moves:
    // ClipGain, MasterVol, ReturnVol, TrackMute.
};

// How a stored value becomes the value the engine uses. The model stores what
// the UI edits, and for the volume fader that is a 0..1 fader position rather
// than a gain — so the mapping lives here, as data, and neither side needs a
// second copy of faderToGain's inverse.
enum class AutoXform : i32 { Direct = 0, Fader = 1 };

struct RtAutoPoint {                  // 16 B
    f64 beat = 0.0;
    f32 value = 0.f;                  // in the target's own units
    u8  curve = 0;                    // reserved; 0 = linear to the next point
    u8  pad[3] = {};
};

struct RtAutoLane {
    i32 target  = (i32)AutoTarget::None;
    i32 index   = 0;                  // return index / device param index
    i32 devSlot = -1;                 // chain position, -1 for engine scalars
    i32 xform   = (i32)AutoXform::Direct;
    i32 first = 0, count = 0;         // window into RtAutoSet::points
    f32 lo = 0.f, hi = 1.f;           // clamp, resolved GUI-side from ParamInfo
    u32 flags = 0;
};

inline constexpr u32 kAutoOverridden = 1u << 0;   // user grabbed the control
inline constexpr u32 kAutoInert      = 1u << 1;   // no realtime path for it
inline constexpr int kMaxRtAutoLanes = 16;

// One allocation, always: `points` addresses memory inside this same block,
// immediately past the struct. Two allocations would need two retirement
// events or a rule about which implies the other, and the RtNote protocol is
// only simple because there is exactly one pointer per slot. A replaced set
// travels back via Ev::AutosRetired before it may be freed.
struct RtAutoSet {
    const RtAutoPoint* points = nullptr;
    RtAutoLane lanes[kMaxRtAutoLanes] = {};
    int laneCount = 0;
    int pointCount = 0;
};

// The one evaluator, shared by the engine and the UI so a drawn envelope and
// an applied one cannot disagree. Pure: bisects the lane's sorted window,
// interpolates linearly (a non-zero `curve` is reserved and renders linear),
// clamps to the lane's [lo,hi], holds before the first point and after the
// last, and returns `fallback` unchanged for an empty lane.
f32 autoValueAt(const RtAutoSet& set, const RtAutoLane& lane, f64 beat, f32 fallback);

// Realtime view of a clip. The GUI fills one of these and ships it across;
// the audio thread only reads. `data` points into a SampleBuffer the GUI
// keeps alive for the lifetime of the session; `notes` follows the RtNote
// retirement protocol above.
struct RtClip {
    const f32* data   = nullptr;   // interleaved, already at engine rate
    i64  frames       = 0;
    int  channels     = 1;
    i64  loopStart    = 0;
    i64  loopEnd      = 0;
    f64  clipBpm      = 120.0;     // tempo of the recorded material
    f64  lengthBeats  = 4.0;       // musical length of [loopStart, loopEnd)
    f32  gain         = 1.0f;
    int  warp         = (int)Warp::Beats;
    bool loop         = true;
    int  quantumIdx   = -1;        // -1 => follow global quantum
    // Generative scheduling. `prob` gates each launch (a skipped launch keeps
    // whatever was playing); the follow action fires after `followBeats` of
    // playback and schedules like any user launch, quantum included.
    f64  prob         = 1.0;       // 0..1 launch probability
    int  followAction = (int)Follow::None;
    f64  followBeats  = 0.0;       // 0 => the clip's own lengthBeats

    // MIDI clip payload. When `isMidi` is set, `data`/`frames` are unused,
    // `lengthBeats` is the loop length, and playback means delivering these
    // notes to the track's note-capable devices with sample-accurate frame
    // offsets — including the note-offs at loop wraps, clip switches and
    // stops (a stopped MIDI clip must never leave a voice hanging).
    const RtNote* notes = nullptr;
    int  noteCount    = 0;
    bool isMidi       = false;

    // Clip envelopes. Same lifetime protocol as `notes`: GUI-owned, engine
    // borrows, a displaced set returns via Ev::AutosRetired before it is freed.
    const RtAutoSet* autos = nullptr;

    // Warp markers: a piecewise-linear beat -> source map, replacing the single
    // clipBpm/tempo ratio when present. Sorted, both sequences strictly
    // increasing, 0 or >= 2 entries (one marker pins nothing). Same one-pointer
    // retirement rule as `notes` and `autos` — a displaced array comes home in
    // Ev::WarpRetired before the GUI may free it. WarpMarker lives in
    // audio/sample.h until this header and that one merge their warp section.
    const struct WarpMarker* markers = nullptr;
    int markerCount = 0;

    // Onset positions in the SOURCE, borrowed from the SampleBuffer. No
    // retirement event, deliberately: transients belong to the sample and are
    // built once at load and never rebuilt, so the pointer is stable for the
    // life of the session exactly as `data` is. Markers belong to the clip;
    // two clips over one sample share one transient array. Beats-mode grain
    // scheduling aligns to these.
    const i64* transients = nullptr;
    int transientCount = 0;

    bool valid        = false;
};

// Raw MIDI into the engine, pushed from a reader thread via pushMidi(). The
// engine forwards each block's worth to note-capable devices on armed tracks
// through PluginInstance::midi().
struct MidiMsg {
    u8  status = 0, d1 = 0, d2 = 0, pad = 0;
    i32 frame  = 0;                // offset hint within the current block
};

// ---------------------------------------------------------------------------
// Device chains.
//
// A track's chain as the audio thread sees it. The GUI builds an RtChain on
// its own heap, fills it, and ships the pointer across via Cmd::SetChain. The
// audio thread only ever swaps the pointer; it never mutates, frees, or
// follows a chain after replacing it. The *previous* pointer travels back in
// an Ev::ChainRetired event, and only on receiving that may the GUI free the
// chain struct and any PluginInstances it removed. Until then both chains and
// every instance they reference must stay alive.
// ---------------------------------------------------------------------------
class PluginInstance;                  // src/plugin/host.h; RT-safe process()

inline constexpr int kMaxChainFx = 8;
inline constexpr int kMaxReturns = 4;   // A/B/C/D return tracks, like Live

struct RtChain {
    PluginInstance* fx[kMaxChainFx] = {};   // in processing order; nulls skipped
    int count = 0;
};

enum class Cmd : u32 {
    SetPlaying, SetTempo, SetQuantum, SetMetronome,
    LaunchClip, StopTrack, LaunchScene, StopAll,
    // SetClip/ClearClip on a slot whose previous RtClip carried a `notes`
    // array push Ev::NotesRetired for the old pointer (when it differs from
    // the incoming one), and any sounding notes from it get their offs first.
    SetClip, ClearClip,
    TrackVol, TrackPan, TrackMute, TrackSolo, TrackArm,
    MasterVol,
    ClipGain, ClipWarp, ClipLoop,
    SetChain,                          // a = track, p = RtChain* (null clears)
    SetReturnChain,                    // a = return index, p = RtChain*
    SetMasterChain,                    // p = RtChain*
    SendLevel,                         // a = track, b = return, x = linear gain 0..1+
    ReturnVol,                         // a = return, x = linear gain

    // Recording. RecordSlot toggles: first send queues a quantized record
    // start into slot (a=track, b=slot, p=GUI-owned f32* interleaved stereo
    // capture buffer, x=capacity in FRAMES); a second send to the same slot
    // queues a quantized stop. The engine appends input into the buffer and
    // never frees it; when recording ends it comes back via Ev::RecordFinished
    // and the GUI turns it into a clip. Buffers must stay alive until then.
    // Overdub (wave 3) will re-enter the same buffer mixing instead of
    // appending — nothing in this contract precludes that.
    RecordSlot,

    // MIDI take into a slot: same toggle/quantize semantics as RecordSlot,
    // but p = GUI-owned RtNote* buffer and x = capacity in NOTES. The engine
    // timestamps incoming MidiMsg against the beat clock, pairs ons with offs
    // (unpaired notes are closed at the stop boundary), and returns the
    // buffer via Ev::MidiRecordFinished with the note count in x.
    //
    // OVERDUB: when the target slot already holds a valid MIDI clip, the take
    // is a looper pass — the clip is (re)launched at the record start
    // boundary and keeps playing while incoming notes are captured with
    // their beats wrapped modulo the clip's lengthBeats, so a note played in
    // any pass lands at its in-loop position. The finish event still returns
    // only the NEW notes; the GUI merges them into the clip and re-pushes.
    RecordMidiSlot,
};

struct Command {
    Cmd    type = Cmd::SetPlaying;
    i32    a = 0, b = 0;
    f64    x = 0.0;
    void*  p = nullptr;                // SetChain payload
    RtClip clip{};
};

enum class Ev : u32 { ClipStarted, ClipStopped, TrackStopped, Xrun, TransportStopped,
                      ChainRetired,   // a = track, p = the RtChain* now safe to free
                      RecordStarted,  // a = track, b = slot, x = beat it began
                      RecordFinished, // a = track, b = slot, x = frames written, p = the buffer
                      NotesRetired,   // p = the RtNote* array now safe to free
                      MidiRecordFinished, // a = track, b = slot, x = note count, p = the buffer
                      AutosRetired,   // p = the RtAutoSet* now safe to free
                      AutoLaneInert,  // a = track, b = slot, x = lane index
                      WarpRetired     // p = the WarpMarker* now safe to free
                    };
struct Event { Ev type = Ev::Xrun; i32 a = 0, b = 0; f64 x = 0.0; void* p = nullptr; };

// Global launch quantum choices, in beats. Index 0 is "None".
inline constexpr f64 kQuantumBeats[] = {0.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125};
inline constexpr const char* kQuantumNames[] = {
    "None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32"
};
inline constexpr int kQuantumCount = 10;

class Engine {
public:
    void prepare(f64 sampleRate, int maxBlock);
    // Audio thread only. `inL`/`inR` are the capture buffers and may be null
    // when the backend has no input; recording and input monitoring read them.
    void process(const f32* inL, const f32* inR, f32* outL, f32* outR, int nframes);

    bool pushCommand(const Command& c) { return cmds_.push(c); }   // GUI thread
    bool popEvent(Event& e)            { return evts_.pop(e); }    // GUI thread
    // Two MIDI producers, two SPSC rings. Each Ring tolerates exactly one
    // producer; the ALSA reader thread and the GUI (computer keyboard, note
    // preview) are two, and sharing one ring races their head pointers and
    // drops messages — a lost note-off is a stuck note. pushMidi() is the
    // hardware reader's; pushMidiFromGui() is the GUI's; process() drains both.
    bool pushMidi(const MidiMsg& m)        { return midi_.push(m); }    // MIDI reader thread
    bool pushMidiFromGui(const MidiMsg& m) { return midiGui_.push(m); } // GUI thread

    // --- polled by the GUI ---------------------------------------------
    std::atomic<f64>  beat{0.0};            // absolute beats since transport start
    std::atomic<bool> playing{false};
    std::atomic<f64>  tempo{120.0};
    std::atomic<f32>  cpu{0.f};
    std::atomic<int>  slotState[kMaxTracks]{};    // SlotState of the *active* slot
    std::atomic<int>  activeSlot[kMaxTracks]{};   // playing slot, -1 if none
    std::atomic<int>  pendingSlot[kMaxTracks]{};  // queued slot, -1 stop, -2 none
    std::atomic<f64>  clipPhase[kMaxTracks]{};    // 0..1 through the running clip
    std::atomic<f32>  meterL[kMaxTracks]{}, meterR[kMaxTracks]{};
    std::atomic<f32>  masterMeterL{0.f}, masterMeterR{0.f};
    // Health telemetry the GUI/daemon polls. blocksRendered advances once per
    // process() call (a liveness heartbeat that does not depend on transport);
    // xruns counts dropouts the backend reports. Both relaxed — monotonic
    // counters read for display, never for control.
    std::atomic<u64>  blocksRendered{0};
    std::atomic<u32>  xruns{0};
    // Backend-reported dropout: the audio callback ran late or the device
    // signalled an under/overrun. The backend calls this from its own thread.
    void reportXrun() { xruns.fetch_add(1, std::memory_order_relaxed); }
    // Recording state per track: 0 idle, 1 queued, 2 recording; slot index.
    std::atomic<int>  recState[kMaxTracks]{};
    std::atomic<int>  recSlotIdx[kMaxTracks]{};
    // Return-bus meters and the engine's total delay-compensation latency.
    std::atomic<f32>  returnMeterL[kMaxReturns]{}, returnMeterR[kMaxReturns]{};
    std::atomic<int>  latencyFrames{0};
    // Bumped at the END of every drainCommands(). A command is provably
    // consumed by the audio thread once this counter has advanced past the
    // value observed after pushCommand() succeeded — the exact-retirement
    // primitive the process split's sample pool needs (PROCESS-SPLIT.md §10).
    std::atomic<u64>  drains{0};

    f64 sampleRate() const { return sr_; }

private:
    struct Voice {
        const RtClip* clip = nullptr;
        bool  active = false;
        f64   srcPos = 0.0;          // ideal read position, source frames
        f64   readA = 0.0, readB = 0.0;
        int   phase = 0, hop = 1024;
        f32   env = 0.f;             // declick ramp, 0..1
        bool  releasing = false;

        // MIDI clip playback: position in clip beats, the next note index to
        // fire, and the note-offs owed. 32 sounding notes per clip is beyond
        // anything a slot sequencer produces; overflow steals the oldest.
        f64   beatPos = 0.0;
        int   nextNote = 0;
        struct PendingOff { f64 beat = 0.0; u8 pitch = 0; bool used = false; };
        PendingOff offs[32];
    };
    struct Track {
        f32  vol = faderToGain(0.85f);
        f32  pan = 0.f;
        bool mute = false, solo = false, arm = false;
        int  playing = -1;           // slot index currently sounding
        int  queued  = -2;           // -2 none, -1 queued stop, >=0 queued slot
        f64  fireBeat = 0.0;
        Voice voice;                 // the clip currently launched
        Voice prev;                  // outgoing clip, fading out across a switch
        f32  mL = 0.f, mR = 0.f;

        // Device chain (see RtChain protocol above) and the pre-mix scratch
        // this track's clips render into. Signal flow per block:
        //   voices (clip gain + declick) -> fx chain -> [PDC delay] ->
        //   vol/pan/mute/solo -> meters -> master sum
        //   + post-fader sends into the return buses (Live's default tap).
        // Chains with count > 0 must run every block, playing or not, so
        // reverb tails and monitoring survive the transport stopping.
        //
        // Delay compensation invariant: every parallel path into the master
        // sum — dry tracks, sends through returns, the master chain — is
        // sample-aligned; Engine::latencyFrames publishes the total. The
        // implementation owns the delay-line details.
        const RtChain* chain = nullptr;
        f32 send[kMaxReturns] = {};    // post-fader send levels, linear
        f32 fxL[kMaxBlock]{};
        f32 fxR[kMaxBlock]{};

        // Recording into a GUI-owned interleaved stereo buffer (see the
        // Cmd::RecordSlot contract). The engine appends and never frees.
        f32* recBuf = nullptr;
        i64  recCap = 0;             // capacity in frames
        i64  recLen = 0;             // frames written so far
        int  recSlot = -1;           // target slot, -1 when idle
        int  recPhase = 0;           // 0 idle, 1 queued start, 2 recording, 3 queued stop
        f64  recFireBeat = 0.0;
        bool recMidi = false;        // this take captures notes, not audio
        // MIDI take state: the note buffer aliases recBuf, capacity/len are in
        // notes, and open notes await their off (closed at the stop boundary).
        f64  recStartBeat = 0.0;
        struct OpenNote { f64 beat = 0.0; u8 pitch = 0; u8 vel = 0; bool used = false; };
        OpenNote recOpen[32];
        // The hand-over slot that previously lived in a file-scope array
        // (engine.cpp gPendingRec) — a mid-take retarget keeps two buffers
        // alive, the old until the boundary and this one from it.
        f32* pendBuf = nullptr;
        i64  pendCap = 0;
        int  pendSlot = -1;
        bool pendMidi = false;
    };

    void  drainCommands();
    void  renderRange(f32* outL, f32* outR, int from, int to);
    void  fireDue(f64 atBeat);
    f64   nextQuantum(f64 fromBeat, int qIdx) const;
    void  startVoice(Track& t, const RtClip& c);
    void  publish();

    f64 sr_ = 48000.0;
    f64 tempo_ = 120.0;
    int sigNum_ = 4;
    int quantum_ = 4;               // index into kQuantumBeats -> 1 Bar
    bool playing_ = false;
    bool metronome_ = false;
    f64 beat_ = 0.0;
    f64 metPhase_ = 0.0;
    int metCountdown_ = 0;
    f32 metFreq_ = 0.f;

    f32 masterVol_ = faderToGain(0.85f);
    f32 masL_ = 0.f, masR_ = 0.f;

    RtClip clips_[kMaxTracks][kMaxScenes];
    Track  tracks_[kMaxTracks];

    // Return buses: a chain, a level, and their own scratch. They follow the
    // same RtChain retirement protocol as tracks (SetReturnChain /
    // SetMasterChain retire displaced chains via Ev::ChainRetired with
    // a = kMaxTracks + returnIdx, or a = -1 for the master chain).
    struct Return {
        const RtChain* chain = nullptr;
        f32 vol = 1.f;
        f32 mL = 0.f, mR = 0.f;
        f32 fxL[kMaxBlock]{};
        f32 fxR[kMaxBlock]{};
    };
    Return returns_[kMaxReturns];
    const RtChain* masterChain_ = nullptr;

    Ring<Command, 1024> cmds_;
    Ring<Event, 1024>   evts_;
    Ring<MidiMsg, 1024> midi_;      // hardware reader thread -> audio
    Ring<MidiMsg, 1024> midiGui_;   // GUI thread -> audio
};

} // namespace lat
