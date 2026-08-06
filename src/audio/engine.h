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
//
// `chance` and `velTo` are Live's per-note Chance and Velocity Range, and both
// are evaluated ON THE AUDIO THREAD at the moment the note-on would go out --
// the same place every other note decision is made, and the only place that
// knows which lap of the loop this is.
//
//   chance  0..100 percent. 100 (the default) skips the draw entirely, so a
//           pattern that does not use the feature costs exactly what it cost
//           before it existed. 0 never sounds; a note that loses its roll is
//           silent for that lap and does NOT release whatever is holding its
//           pitch, because a note that did not happen cannot end one that did.
//   velTo   0 (the default) means "fixed velocity, use `vel`". Anything else is
//           the far end of a velocity SPAN: each sounding is drawn uniformly
//           from the closed integer range between `vel` and `velTo`, in either
//           order, so a range may be written downwards without meaning
//           anything different.
//
// DETERMINISM is a hard requirement, not a nicety: this project's demo renders
// are cmp-identical gates. Neither draw uses a generator with state, for the
// reason spelled out at randUnit() in engine.cpp -- a stateful RNG's output
// depends on how many times it has been called, which depends on the buffer
// size. Both are a pure hash of the musical event: the track, the note's index
// in its clip, its pitch, its clip-relative beat, and the LOOP LAP the voice is
// on. Nothing in that list is a function of how the audio was chopped up.
//
// BOTH FIELDS LIVE IN PADDING RtNote ALREADY HAD. beat and len force 8-byte
// alignment, so `pitch` and `vel` were followed by six dead bytes and the
// struct was 24 B; it still is. That is not an accident of layout, it is the
// reason this could be a one-wave change: src/ipc/pool.h asserts WireNote
// mirrors this struct field for field and the note pool is a raw byte copy, so
// two more bytes inside the existing footprint cross the process boundary with
// no change to the wire, the pool, or the region's layout hash. The assert
// below is what keeps that true.
struct RtNote {
    f64 beat = 0.0;
    f64 len  = 0.25;
    u8  pitch = 60, vel = 100;
    u8  chance = 100;                 // 0..100 percent, evaluated per sounding
    u8  velTo  = 0;                   // 0 = fixed velocity; else the far end
};
static_assert(sizeof(RtNote) == 24,
              "RtNote must stay 24 B: src/ipc/pool.h's WireNote mirrors it and the "
              "note pool is copied as raw bytes");

// ---------------------------------------------------------------------------
// Time signatures.
//
// `beat_` -- the engine's timeline -- is and stays ABSOLUTE MUSICAL BEATS, a
// beat being a quarter note. The map below converts beats to BARS and never the
// other way round. That direction is the whole design: an arrangement item at
// beat 8 is still at beat 8 after a signature change and only the bar it is
// *displayed* at moves, the loop brace is two beats and stays two beats, and
// nothing that was recorded against the clock has to be rewritten because
// somebody re-barred the piece.
//
// A bar of num/den is `num * 4/den` beats -- 4 for 4/4, 3 for 3/4, 3.5 for 7/8.
// Numerators are 1..kSigNumMax and denominators are POWERS OF TWO in
// 1..kSigDenMax: the GUI clamps, the parser clamps, and the engine validates the
// whole map once at publication and refuses one it cannot walk.
// ---------------------------------------------------------------------------

inline constexpr int kSigNumMax = 32;
inline constexpr int kSigDenMax = 32;
inline constexpr int kMaxSigs   = 256;   // signature changes per set

// One signature change, and -- as a sorted array of them -- the realtime form of
// the whole map. Entry 0 is ALWAYS at bar 0 and is the session signature; entry
// i covers bars [sigs[i].bar, sigs[i+1].bar) and the last entry runs forever.
//
// `beat` is DERIVED: the absolute beat that `bar` begins on. It is computed once
// by the publisher (sigMapRebase) instead of being re-summed by the engine on
// every query, which is what makes beat -> bar a bisect rather than a walk from
// bar zero. It is not merely trusted, either -- sigMapValid re-derives it and
// refuses a map whose beats do not follow from its own bar lengths, so a
// publisher cannot lie the engine into putting bar lines where there are none.
struct RtSig {
    i32 bar = 0;
    i32 num = 4, den = 4;
    i32 pad = 0;
    f64 beat = 0.0;
};

// Beats (quarter notes) in one bar of num/den. The one place the 4/den ratio is
// written; everything else asks this.
inline constexpr f64 sigBarBeats(int num, int den) {
    return den > 0 ? (f64)num * 4.0 / (f64)den : 4.0;
}

// A map the engine may walk: 1..kMaxSigs entries, entry 0 at bar 0 beat 0, bars
// and beats both strictly increasing, every num/den in range and a power of two,
// and every `beat` consistent with the bar lengths ahead of it. Run once per
// publication, never per query.
bool sigMapValid(const RtSig* sigs, int count);

// Fill in every entry's derived `beat` from bar 0 forwards and report whether
// the result is walkable. The publisher's half of the contract above; the input
// must already be sorted, deduplicated and clamped (Session::normalizeSigs).
bool sigMapRebase(RtSig* sigs, int count);

// The entry covering `beat` / covering `bar`. Both return 0 for an empty map,
// which every caller reads as plain 4/4 -- the behaviour of every build before
// signatures existed.
int sigIndexAtBeat(const RtSig* sigs, int count, f64 beat);
int sigIndexAtBar (const RtSig* sigs, int count, i64 bar);

// Where bar `bar` begins, in beats, and its inverse. Both take and return
// FRACTIONAL bars so a ruler can ask where 4.5 bars is; both extrapolate below
// bar 0 and past the last change with the signature in force there.
f64 sigBeatOfBar(const RtSig* sigs, int count, f64 bar);
f64 sigBarOfBeat(const RtSig* sigs, int count, f64 beat);

// The playhead as a musician reads it. `bar`/`beat`/`sixteenth` are 0-BASED here
// and the readout adds one; `beat` counts signature units (1/den notes), so 7/8
// runs 0..6 and each of those units is half a quarter-note beat long.
struct BarPos {
    i32 bar = 0;
    i32 beat = 0;
    i32 sixteenth = 0;
    i32 num = 4, den = 4;
    f64 barStart = 0.0;    // absolute beat the bar begins on
    f64 unit = 1.0;        // beats per signature unit, == 4/den
};

// THE conversion, shared by the metronome, the position readout, the launch
// quantum and the UI's ruler, for the same reason autoValueAt is one function:
// a grid that is drawn and a grid that is played must not be able to disagree.
BarPos sigPosAt(const RtSig* sigs, int count, f64 beat);

// The next bar line at or after `beat`, aligned to a multiple of `bars` bars
// counted FROM BAR 0 -- so "4 Bars" stays a phrase boundary rather than becoming
// "four bars from wherever the last signature change was". Walks the map; the
// one thing it may not do is multiply, because bars are no longer all the same
// length.
f64 sigNextBarLine(const RtSig* sigs, int count, f64 beat, int bars);

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

// The arrangement's automation container (docs/ARRANGEMENT.md §6.2). Same
// one-allocation layout as RtAutoSet, same retirement protocol, ONE difference:
// the lane array is variable-width and lives in the block rather than being a
// fixed member.
//
//   [RtAutoSetN][RtAutoLane[laneCount]][RtAutoPoint[pointCount]]
//
// RtAutoSet is NOT widened to 32 lanes instead, for two reasons. Its `lanes` is
// a fixed array by value, so its width is baked into sizeof(RtAutoSet) on both
// sides of the process boundary and into the region's layout hash — widening it
// would change the size of every published clip envelope set, a shipped and
// versioned shape, for a feature clips do not use. And the right ceilings
// genuinely differ: sixteen is right for a clip, which is *about* one or two
// parameters, while a track's timeline accumulates every automated parameter in
// its chain over the length of a song. One constant for both would be wrong for
// one of them.
//
// A replaced set travels back via Ev::TrackAutosRetired before it may be freed.
struct RtAutoSetN {
    const RtAutoPoint* points = nullptr;
    const RtAutoLane*  lanes  = nullptr;
    int laneCount  = 0;
    int pointCount = 0;
};

inline constexpr int kMaxRtArrLanes = 32;    // == kMaxArrLanes (session.h)

// THE evaluator, shared by the engine and the UI so a drawn envelope and an
// applied one cannot disagree. Pure: bisects the lane's sorted window,
// interpolates linearly (a non-zero `curve` is reserved and renders linear),
// clamps to the lane's [lo,hi], holds before the first point and after the
// last, and returns `fallback` unchanged for an empty lane.
//
// It takes the points as a pointer and a count rather than a container, because
// there are now two containers (RtAutoSet, RtAutoSetN) and AUTOMATION.md §2.4's
// claim — "they cannot disagree if there is one function reading one set of
// points against one beat" — is only true while this stays ONE function. The two
// convenience overloads below are inline forwarders and must never grow a body
// of their own.
f32 autoValueAt(const RtAutoPoint* points, int pointCount, const RtAutoLane& lane,
                f64 beat, f32 fallback);

inline f32 autoValueAt(const RtAutoSet& s, const RtAutoLane& l, f64 b, f32 f) {
    return autoValueAt(s.points, s.pointCount, l, b, f);
}
inline f32 autoValueAt(const RtAutoSetN& s, const RtAutoLane& l, f64 b, f32 f) {
    return autoValueAt(s.points, s.pointCount, l, b, f);
}

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

// ---------------------------------------------------------------------------
// The arrangement, as the audio thread sees it. Full design: docs/ARRANGEMENT.md
// §3. Landed here, compiled and not yet used, because engine.h is the daemon's
// contract and exactly one wave may open it.
// ---------------------------------------------------------------------------

// One placed item.
struct RtArrItem {
    f64 start  = 0.0;      // absolute timeline beats
    f64 length = 0.0;
    f64 offset = 0.0;      // clip-relative beat the item begins at
    f32 fadeIn = 0.f, fadeOut = 0.f;   // beats
    i32 fadeShape = 0;
    i32 clip = -1;         // index into RtArrangement::clips
};

// One track's lane, or -- for the cell addressed as track -1 -- the transport's
// loop brace (§3.6). ONE ALLOCATION, always:
//
//   [RtArrangement][RtArrItem[itemCount]][RtClip[clipCount]][RtNote[noteCount]]
//
// `items`, `clips` and every RtClip::notes inside it address memory in this same
// block, so the whole lane is one new[] and one delete[] and the retirement
// protocol has exactly ONE pointer to talk about. That is the RtAutoSet argument
// above extended by one more array: two allocations would need two retirement
// events or a rule about which one implies the other, and the RtNote protocol is
// only simple because there is one pointer per slot.
//
// Clip envelopes are the deliberate exception and stay in their own RtAutoSet
// allocations, pointed at by RtClip::autos. Folding them in would mean dragging
// one breakpoint republishes the lane, and the lane is up to 1.6 MB of notes
// (kMaxArrNotes * sizeof(RtNote)); a 60 Hz drag would move 96 MB/s to change
// sixteen bytes.
//
// A replaced lane travels back to the owner in Ev::ArrangementRetired before it
// may be freed.
struct RtArrangement {
    const RtArrItem* items = nullptr;
    const RtClip*    clips = nullptr;
    int itemCount = 0;
    int clipCount = 0;
    int noteCount = 0;                 // for the daemon's bounds arithmetic
    // Transport cell only (a = -1); zero on every track's lane.
    f64 loopStart = 0.0, loopEnd = 0.0;
    u32 loopOn = 0;
};

// ---------------------------------------------------------------------------
// The arrangement record journal (§5.3).
//
// Audio thread -> GUI thread, its own SPSC ring, and deliberately NOT the event
// ring: `evts_` is designed to drop under load (push returns false when full and
// the engine discards), and a channel that is designed to drop cannot be the
// thing a recording is made of. Nor can the GUI's own clock: an event stamped
// when the reader pops it is a recording of the reader.
// ---------------------------------------------------------------------------
enum class JournalKind : u32 {
    None = 0, TakeStart, TakeEnd, ClipOn, ClipOff, NoteOn, NoteOff, Locate, LoopWrap
};

// Pointer-free and trivially copyable, because it rides an SPSC ring here and a
// shared-memory ring under the process split. §5.3 calls it 32 B; the field list
// it gives is four 4-byte integers and one f64, which is 24 with natural
// alignment, and 24 is what this is. Nothing depends on the number -- the ring is
// a template over the type -- so the fields are kept and the arithmetic is
// corrected rather than padded up to match the prose.
struct ArrJournal {            // 24 B, pointer-free, trivially copyable
    u32 kind  = 0;             // JournalKind
    u32 seq   = 0;             // monotonic per engine run; a gap means a drop
    i32 track = 0;
    i32 a     = 0;             // slot / pitch / velocity, per kind
    f64 beat  = 0.0;           // the ENGINE's beat, exact
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

    // --- the arrangement (docs/ARRANGEMENT.md §3, §4, §6) ------------------
    //
    // APPENDED, never inserted: the numeric value of every command above is
    // protocol (ipc/control.h classifies by it and WireCommand carries it), so
    // the enum only ever grows at the end.

    // a = track, p = const RtArrangement* (null clears). The displaced pointer
    // comes back in Ev::ArrangementRetired, pushed from inside drainCommands()
    // and only when it differs from the incoming one -- the RtNote protocol
    // verbatim.
    //
    // a = -1 names the TRANSPORT CELL rather than a track: its RtArrangement
    // carries no items, only loopStart/loopEnd/loopOn. That is deliberately
    // Ev::ChainRetired's own addressing (a = kMaxTracks + returnIdx, a = -1 for
    // the master chain), so a reader who knows one knows the other. The loop
    // brace rides this table instead of a Cmd::SetLoop because `Command` has one
    // f64 and a brace is two numbers and a flag: widening Command would grow
    // every entry of a 1024-deep ring and break WireCommand, which is 32 B,
    // pointer-free and load-bearing in the region layout hash.
    SetArrangement,

    // a = track, p = const RtAutoSetN* (null clears). Retired through
    // Ev::TrackAutosRetired. The arrangement's automation lanes: absolute-beat,
    // one lane per address per track, evaluated in a pass that runs BEFORE the
    // clip envelope pass so that the clip's value wins by overwriting it (§6.4).
    SetTrackAutos,

    // a = 0 (reserved), x = the beat to go to. Flushes every sounding note-off
    // at frame 0, re-seeks every arrangement cursor, and ASSIGNS beat_ rather
    // than adding to it, so sixty-four laps of a loop accumulate no drift.
    // Deliberately leaves session voices alone: a locate is a statement about
    // the timeline, not about the performance a track is currently playing.
    Locate,

    // a = track, or -1 for every track. Clears the per-track override that a
    // session clip launch set, unquantized -- it is a corrective gesture, and a
    // correction that waits a bar is the wrong feel. The track's cursor
    // re-seeks to beat_ and resumes whatever item covers it, mid-item.
    BackToArrangement,

    // a = count, p = const RtSig* (null, or count 0, clears back to plain 4/4).
    // ONE allocation and ONE pointer -- the RtNote retirement rule verbatim, and
    // the reason the map is a flat array rather than a struct pointing at one:
    // there is exactly one array here, so there is exactly one pointer to talk
    // about and no rule needed about which retirement implies which. The
    // displaced array comes back in Ev::SigsRetired and may not be freed before.
    //
    // A map that does not pass sigMapValid is REFUSED and handed straight back
    // in the same sweep: the engine keeps no map at all and every bar line goes
    // back to 4/4, which is a place the user can hear, rather than walking a
    // map whose beats do not follow from its own bars.
    //
    // NOT YET ON THE WIRE. ipc/control.h classifies commands by value and its
    // bound is the last enumerator it knew, so this one lands in "unknown" and
    // the daemon answers RejectUnknownCommand -- fail-closed, exactly as
    // Cmd::SetArrangement did for a wave before it was wired up. The daemon
    // therefore plays every set in 4/4 until that file grows the two cases.
    SetSignatures,
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
                      WarpRetired,    // p = the WarpMarker* now safe to free
                      // Appended, never inserted: ipc/control.h classifies
                      // events by this value. Both are the RtNote retirement
                      // protocol, a = the cell the pointer was published to
                      // (a = -1 being SetArrangement's transport cell).
                      ArrangementRetired, // a = track, p = the RtArrangement*
                      TrackAutosRetired,  // a = track, p = the RtAutoSetN*
                      SigsRetired         // p = the RtSig[] now safe to free
                    };
struct Event { Ev type = Ev::Xrun; i32 a = 0, b = 0; f64 x = 0.0; void* p = nullptr; };

// Global launch quantum choices, in beats. Index 0 is "None".
//
// The first four entries are BAR counts wearing 4/4 clothes: 32 beats is eight
// bars only while a bar is four beats. They keep their 4/4 values because that
// is still what they mean in 4/4 and because that is what the no-map path
// multiplies by -- kQuantumBars beside them is what nextQuantum reads once a map
// exists, and it walks the map instead of multiplying. The fractional entries
// from index kQuantumBarMax + 1 on are genuine beat fractions and stay so; they
// are anchored to the containing BAR, which is a no-op in 4/4 (every bar line is
// a multiple of every one of them) and is what makes a 1/4 grid restart at the
// bar line in 7/8, where 3.5 is not a multiple of 1.
inline constexpr f64 kQuantumBeats[] = {0.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125};
inline constexpr const char* kQuantumNames[] = {
    "None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32"
};
inline constexpr int kQuantumCount = 10;
// How many bars each of the bar-shaped quanta means, indexed by the same index.
inline constexpr int kQuantumBars[]  = {0, 8, 4, 2, 1};
inline constexpr int kQuantumBarMax  = 4;   // last index that means whole bars

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
    // The playhead as a musician reads it: bars.beats.sixteenths, ONE-BASED,
    // recomputed from the signature map every block. Published here rather than
    // derived by each reader because the map lives here -- a readout that
    // divided `beat` by a single sigNum would be wrong from the first signature
    // change on, and would be wrong differently in the app and in the daemon.
    // posSigNum/posSigDen are the signature in force AT THE PLAYHEAD, which is
    // what a transport bar should show and is not in general the set's.
    std::atomic<i32>  posBar{1}, posBeat{1}, posSixteenth{1};
    std::atomic<i32>  posSigNum{4}, posSigDen{4};
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

    // --- the arrangement (docs/ARRANGEMENT.md §4, §5) ----------------------

    // Bit i set == track i is overridden: a session clip was launched on it, so
    // its arrangement lane and its arrangement automation are both suspended
    // until Cmd::BackToArrangement.
    //
    // ENGINE-OWNED, and set at the quantized launch the engine computes rather
    // than when the command arrives or when the user clicks. The GUI asks for
    // "launch clip 3"; the engine decides which bar line that lands on, from the
    // quantum, the clip's own quantumIdx and beat_. A flag set at click time
    // would silence the arrangement on that track up to a whole bar before the
    // session clip started -- an audible hole in the gesture a performer makes
    // most. Published here for the UI only (the Back to Arrangement button, and
    // drawing an overridden lane desaturated).
    std::atomic<u32>  arrOverride{0};

    // The record journal (§5.3). Drained by the GUI; every entry is stamped with
    // the engine's own beat_ at the sub-block boundary it happened on -- the same
    // number the scheduler used, not one anyone inferred.
    bool popJournal(ArrJournal& j) { return journal_.pop(j); }
    // Refused pushes. The same information as a gap in ArrJournal::seq, and
    // published separately because a consumer that has not yet drained the ring
    // can still read it. A take whose span shows either is REFUSED rather than
    // committed short: a recording silently missing four bars is
    // indistinguishable from a performance that had four bars of rest in it.
    std::atomic<u32>  journalDropped{0};

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
        // Which time round the loop this is, from 0 at the launch. The one
        // input to the per-note dice (RtNote::chance, RtNote::velTo) that makes
        // a probabilistic pattern re-roll each lap instead of being frozen at
        // whatever the first pass decided.
        //
        // A COUNT and not a time, deliberately: it is incremented by the wrap
        // itself, so it is a function of how much musical time has passed and
        // of nothing else. An absolute beat would be accumulated per block and
        // would therefore differ by ulps between one buffer size and another,
        // which is the exact property the dice may not have. See noteKey().
        u32   lap = 0;
        struct PendingOff { f64 beat = 0.0; u8 pitch = 0; bool used = false; };
        PendingOff offs[32];

        // Arrangement item fades (docs/ARRANGEMENT.md §3.4). 1.0 in the ordinary
        // case -- a session clip, an item with no fades, an item past its fade
        // regions -- so a session render takes the same arithmetic it takes
        // today. Multiplying by exactly 1.0f is bit-exact in IEEE-754 for every
        // finite value, which is what lets the headline gate be BIT-identity
        // rather than a tolerance.
        //
        // These are the one part of the arrangement's engine state that cannot
        // live in the side table the scheduler uses, and the reason is specific:
        // startVoice copies a Voice wholesale (`t.prev = t.voice`), so a fade
        // parked in a table keyed by track index would lose its association with
        // the voice at the exact moment that voice becomes the outgoing half of
        // a crossfade -- which is the only moment a fade-out is interesting.
        // They travel with the voice, so they are fields on the voice.
        f32   fade   = 1.f;          // multiplier at the start of the sub-block
        f32   fadeTo = 1.f;          // multiplier at its end
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
    // The signature map, borrowed from the GUI under the Cmd::SetSignatures
    // contract. Null is not a degenerate map and is the ORDINARY case for a
    // renderer, a daemon and every set nobody has re-barred: it means 4/4
    // everywhere, and every caller takes the arithmetic it took before this
    // existed. Deliberately NOT cleared by prepare(): the pointer is the GUI's,
    // dropping it would leak an array with no retirement event behind it, and a
    // sample-rate change does not re-bar the piece.
    const RtSig* sigs_ = nullptr;
    int sigCount_ = 0;
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

    // Capacity 4096: a dense performance -- sixteen notes per beat across eight
    // armed tracks at 140 BPM -- is about 300 entries per second, so this holds
    // roughly thirteen seconds of the worst case anyone plays against a GUI
    // draining it sixty times a second. Sized so that an overflow means "the GUI
    // stopped", not "the player played fast".
    Ring<ArrJournal, 4096> journal_;
    u32 journalSeq_ = 0;            // audio thread only; +1 per ATTEMPTED push

    Ring<Command, 1024> cmds_;
    Ring<Event, 1024>   evts_;
    Ring<MidiMsg, 1024> midi_;      // hardware reader thread -> audio
    Ring<MidiMsg, 1024> midiGui_;   // GUI thread -> audio
};

} // namespace lat
