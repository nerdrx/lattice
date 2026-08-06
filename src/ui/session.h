// Session model — the GUI-side truth for a set.
//
// The model here is the editable, allocating, std::string-carrying version.
// Whenever something changes that the audio thread needs, it is pushed across
// as a Command; the engine keeps its own realtime-safe mirror.
//
// This header is ONLY the model (clips, tracks, returns, scenes, the Session
// aggregate) plus the small UI value-types that ride with it. It is what
// src/core/project.* and src/ui/pianoroll.* need to see — deliberately nothing
// about class App. app.h includes this; nothing here includes app.h.
#pragma once
#include "../audio/engine.h"
#include "../audio/sample.h"
#include "../plugin/host.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace lat {

enum class ClipKind { Audio, Midi };

// One note in a MIDI clip, clip-relative beats. The GUI edits these freely;
// pushClip snapshots them into a heap RtNote array for the engine, and the
// old array comes back via Ev::NotesRetired before it is freed.
struct NoteModel {
    f64 beat = 0.0;
    f64 len  = 0.25;
    u8  pitch = 60, vel = 100;
};

// One breakpoint in an envelope, in clip-relative beats. `curve` is reserved
// and must be 0 for now: the segment to the next point is linear. It is a byte
// because the shapes worth having are an enumeration (linear, ease, S, hold)
// rather than a continuum, and a byte costs nothing where a second control
// point would double the wire form for a feature nobody has asked for.
struct AutoPoint {
    f64 beat  = 0.0;
    f32 value = 0.f;                  // in the target's own units
    u8  curve = 0;
    u8  pad[3] = {};
};

// One address's worth of automation inside one clip.
//
// The address is kept as TEXT, never as a resolved target — PARAM-ADDRESS.md's
// "resolution lives GUI-side" applied here. A lane naming a device that is not
// loaded today survives a save/load intact, exactly as ClipModel::path survives
// a missing sample. Resolution happens at publish time and is thrown away on
// every structural change.
struct AutoLane {
    std::string address;              // canonical, see docs/PARAM-ADDRESS.md
    std::vector<AutoPoint> points;    // sorted by beat, unique beats
    bool enabled = true;              // Live's "deactivate envelope"
};

inline constexpr int kMaxClipLanes      = 16;    // == kMaxRtAutoLanes
inline constexpr int kMaxClipAutoPoints = 4096;  // total across a clip's lanes

struct ClipModel {
    // Stable identity. UIDs never change once assigned and are serialized, so
    // undo, set-diff, automation targets and collaboration can reference an
    // entity across moves and renames. 0 = unassigned (Session::newUid()).
    u64 uid = 0;
    ClipKind kind = ClipKind::Audio;
    std::vector<NoteModel> notes;      // Midi clips only; kept sorted by beat
    std::vector<AutoLane>  envelopes;  // clip automation; audio and MIDI alike
    // The clip's warp map: a piecewise-linear beat -> source frame curve, one
    // marker per pinned point. Sorted, and BOTH sequences strictly increasing —
    // warpMapValid() in audio/sample.h is the gate, and pushClip refuses a map
    // it rejects rather than handing the engine one it cannot invert.
    //
    // EMPTY IS THE ORDINARY CASE and is not a degenerate map: a clip with no
    // markers warps at the single clipBpm/tempo ratio, exactly as every clip did
    // before markers existed. A one-marker map is meaningless (a point pins, it
    // does not tilt) and is likewise not published. pushClip snapshots this into
    // a heap WarpMarker array for the engine; the displaced array comes back via
    // Ev::WarpRetired before it is freed, the same lifetime `notes` has.
    std::vector<WarpMarker> markers;
    SampleRef sample;
    std::string name;
    // Source file. Authoritative for save/load: it survives a missing sample,
    // so a set whose media is offline does not silently lose the reference.
    std::string path;
    int  colorIdx = 0;
    f32  gain = 1.0f;
    Warp warp = Warp::Beats;
    bool loop = true;
    f64  clipBpm = 120.0;
    f64  lengthBeats = 4.0;
    i64  loopStart = 0, loopEnd = 0;
    int  quantumIdx = -1;              // -1 => follow the global quantum
    // Generative scheduling, mirrored into RtClip (see engine.h Follow).
    f64  prob = 1.0;
    Follow followAction = Follow::None;
    f64  followBeats = 0.0;            // 0 => the clip's own length
    // A MIDI clip is valid even while empty — an empty pattern is editable
    // and launchable (it plays silence at its loop length, exactly like Live).
    bool valid() const { return kind == ClipKind::Midi ? true : sample != nullptr; }
};

// One loaded plugin on a track. The instance is GUI-owned; the audio thread
// borrows it through the RtChain currently published to the engine, so it may
// only be destroyed after that chain has come back via Ev::ChainRetired.
struct DeviceModel {
    u64 uid = 0;
    PluginDesc desc;
    std::unique_ptr<PluginInstance> inst;
    bool bypass = false;
    // Set only when `inst` is null: the plugin named by desc.uri was not on
    // this machine when the set loaded. The device keeps its slot in the chain
    // (silent, skipped by publishChain) and carries its saved parameters here
    // so that saving the set again does not throw them away.
    std::vector<std::pair<u32, f32>> lostParams;
};

// A device as it sits in a saved set: no instance, just what's needed to
// rebuild one. The project layer reads/writes ONLY this passive form; the App
// materializes savedDevices -> devices (instantiate, apply params, publish)
// after load and serializes devices -> savedDevices before save. That keeps
// plugin instantiation out of src/core entirely.
struct SavedDevice {
    u64 uid = 0;
    std::string uri;                   // PluginDesc::uri
    std::string name;                  // display fallback if the plugin is gone
    bool bypass = false;
    std::vector<std::pair<u32, f32>> params;   // (ParamInfo::id, value)
};

// A live plugin instance lifted out of a session that is about to be replaced,
// so an undo can rebind it instead of loading the plugin again. Identity is the
// uid; the uri is carried too because a uid only means "the same device" if the
// plugin behind it is still the same one. See App::adoptSession.
struct LiveDevice {
    u64 uid = 0;
    std::string uri;
    PluginDesc desc;
    std::unique_ptr<PluginInstance> inst;
};

// One clip's audio at the moment an undo snapshot was taken, keyed by clip uid.
//
// Keyed by uid and not by source path because the path is not always the
// truth: a take that has been recorded but not exported has no file behind it
// at all, and a project document can only name files. Restoring such a set
// from its text alone would quietly turn a recording into an empty clip, so an
// undo entry carries the audio itself -- a shared_ptr each, so the cost is a
// pointer per clip and the history pins the buffers it may still have to give
// back. See App::UndoEntry.
struct ClipSample {
    u64 uid = 0;
    SampleRef sample;
};

// ---------------------------------------------------------------------------
// The arrangement (docs/ARRANGEMENT.md §2)
// ---------------------------------------------------------------------------

// Bounds. Enforced by the editor and by the publisher, NEVER by the parser --
// the same split kMaxClipLanes and kMaxClipAutoPoints already make. All are
// "a human cannot reach this by hand" numbers; hitting one is a bug report
// rather than a limitation. The first three are validated at the process
// boundary, so changing them later is a protocol change and not a constant one.
inline constexpr int kMaxArrItems  = 512;      // items per track
inline constexpr int kMaxArrNotes  = 65536;    // notes per track, over every item's src
inline constexpr int kMaxArrLanes  = 32;       // arrangement automation lanes per track
inline constexpr int kMaxArrPoints = 65536;    // breakpoints per track across its lanes
// The longest admissible crossfade, in BEATS and not milliseconds: a crossfade
// is a musical gesture, and a bound in time would mean the invariant a file
// satisfies at 120 BPM is violated at 60. One bar in 4/4, two seconds at 120.
inline constexpr f64 kMaxOverlapBeats = 4.0;
// The shortest item. Below this an item is not grabbable at any zoom and cannot
// carry a fade, so a trim that would go under it deletes instead. Finer than a
// note's 1/32 floor because an item may be a one-shot transient and a note may
// not.
inline constexpr f64 kMinArrBeats = 1.0 / 64.0;
// Overlaps below this are float noise from a trim, not a crossfade: a trim sets
// `length = next.start - start`, and `start + length` need not come back to
// `next.start` exactly. Both arrangeRepair and anything checking the invariant
// must use the same slack, or a lane repaired once would be repaired forever.
inline constexpr f64 kArrOverlapEps = 1e-9;
// A track's default lane height in the arrangement, in logical px. Named because
// the project writer suppresses `arrheight` at exactly this value.
inline constexpr f32 kArrHeightDefault = 68.f;

// One clip placed on the timeline.
//
// `src` is BY VALUE, and that is decision one of the arrangement: an item OWNS
// its clip. Everything a user wants to differ between two placements of the same
// material -- gain, loop, warp, the clip's own envelopes, its notes -- is
// already a field of ClipModel, so copying costs nothing in schema and saves
// declaring the whole of ClipModel a second time as "the per-instance
// overrides".
//
// The rejected alternative is reference-by-uid, and it is rejected for a product
// reason rather than a performance one: a session scratchpad must not
// retroactively rewrite the record. Nudging the gain on a loop at 2 a.m. must
// not silently move the gain of the eight places that loop was committed to the
// timeline three hours earlier.
//
// The copy is cheap where it matters: ClipModel::sample is a SampleRef, so
// copying an item bumps a refcount and a 40-item arrangement over one drum loop
// holds one decoded buffer. Notes are genuinely duplicated, bounded by
// kMaxArrNotes, and deduped again by the publisher on the way to the engine.
struct ArrangeClip {
    u64 uid = 0;            // stable identity; Session::newUid()
    f64 start  = 0.0;       // absolute timeline beats
    f64 length = 4.0;       // beats occupied on the timeline
    f64 offset = 0.0;       // clip-relative beat this item begins at
    f64 fadeIn  = 0.0;      // beats, from `start`
    f64 fadeOut = 0.0;      // beats, back from `start + length`
    u8  fadeShape = 0;      // reserved, exactly as AutoPoint::curve is
    u8  pad[7] = {};
    // PROVENANCE ONLY. The uid of the ClipModel this item was made from, kept so
    // that "select every instance of this loop" and a future "update from
    // source" have something to match on. It is never resolved during playback,
    // never during save, and never during load: it DANGLES SOFT, exactly as a
    // parameter address naming a deleted device does (PARAM-ADDRESS.md), and is
    // written back unchanged forever.
    u64 sourceUid = 0;
    ClipModel src;          // the copy -- see above
    f64 end() const { return start + length; }
};

// Restores the lane invariant on one track's arrangement and reports whether it
// changed anything. Idempotent -- repair(repair(x)) == repair(x) -- which is
// what makes it safe to call after every edit and again after a load.
//
// The invariant (§2.3), stated on the vector in ORDER, which is why the sort is
// step one:
//
//   1. sorted by `start`;
//   2. every `length >= kMinArrBeats`;
//   3. fades non-negative and `fadeIn + fadeOut <= length`;
//   4. neighbours overlap only as a crossfade: either `b.start >= a.end`, or all
//      of `a.end - b.start <= kMaxOverlapBeats`, `a.fadeOut >= the overlap` and
//      `b.fadeIn >= the overlap`;
//   5. at most two items sound at once: for every three consecutive items,
//      `c.start >= a.end`.
//
// Rule 5 is the load-bearing one: two simultaneous items per track is exactly
// Track::voice and Track::prev, the pair the engine already keeps so a same-track
// clip switch crossfades instead of hard-cutting. So the one place this model
// diverges from Live's strictly non-overlapping lane costs the engine nothing --
// no third voice, no voice pool, no change to the mixdown.
//
// WHERE THIS LIVES. §2.5 puts the definition in src/ui/app_arrange.cpp, which
// belongs to a later milestone and does not exist yet; 8a has to ship and test
// the function, so it is inline here instead. It has no dependency on App -- it
// is a pure transform of a vector -- so the only thing that changes if it moves
// is which file it is in.
//
// ONE SWEEP is not enough, and the fixed-point loop is not belt and braces: a
// trim that lands under kMinArrBeats has to become a deletion, and a head-trim
// moves an item's start, so the lane needs re-sorting. Running to a fixed point
// is also exactly what makes the whole function idempotent.
//
// Termination: across sweeps no item's `start` ever decreases, no `length` ever
// grows and the count never grows, and every new boundary is copied verbatim
// from a neighbour's edge rather than computed by a shrinking step -- so the
// reachable configurations are finite and monotone.
inline bool arrangeRepair(std::vector<ArrangeClip>& lane) {
    // One sweep. Returns true if it changed anything.
    const auto sweep = [](std::vector<ArrangeClip>& lane) {
        bool changed = false;

        // (a) Per-item clamps, and the minimum length. Deleting rather than clamping
        //     up: the alternative leaves a sliver the user did not ask for exactly
        //     where they were trying to remove one.
        for (size_t i = 0; i < lane.size();) {
            ArrangeClip& c = lane[i];
            if (!(c.start >= 0.0)) { c.start = 0.0; changed = true; }        // NaN lands here
            if (!(c.offset >= 0.0)) { c.offset = 0.0; changed = true; }
            if (!(c.length >= kMinArrBeats)) {
                lane.erase(lane.begin() + (long)i);
                changed = true;
                continue;
            }
            if (!(c.fadeIn >= 0.0))  { c.fadeIn = 0.0;  changed = true; }
            if (!(c.fadeOut >= 0.0)) { c.fadeOut = 0.0; changed = true; }
            if (c.fadeIn > c.length) { c.fadeIn = c.length; changed = true; }
            // The slack is not fussiness: `fadeOut = length - fadeIn` need not add
            // back up to `length` in binary floating point, so an exact comparison
            // would re-clamp a lane that is already correct, report a change, and
            // cost this function its idempotence forever. The rewrite is also gated
            // on the value actually moving, for the same reason.
            if (c.fadeIn + c.fadeOut > c.length + kArrOverlapEps) {
                const f64 fo = c.length - c.fadeIn;
                const f64 nv = fo > 0.0 ? fo : 0.0;
                if (nv != c.fadeOut) { c.fadeOut = nv; changed = true; }
            }
            ++i;
        }

        // (b) Sorted, and STABLY: two items that genuinely begin on the same beat
        //     keep the order the user made them in rather than a coin toss.
        for (size_t i = 1; i < lane.size(); ++i)
            if (lane[i].start < lane[i - 1].start) {
                std::stable_sort(lane.begin(), lane.end(),
                                 [](const ArrangeClip& a, const ArrangeClip& b) { return a.start < b.start; });
                changed = true;
                break;
            }

        // (c) Overlaps: the LATER statement keeps its span and its neighbour gives
        //     way, which is what makes a drop onto an occupied stretch of lane do
        //     what the user's hand just said rather than what the lane used to say.
        std::vector<char> drop(lane.size(), 0);
        for (size_t i = 0; i + 1 < lane.size(); ++i) {
            ArrangeClip& a = lane[i];
            const ArrangeClip& b = lane[i + 1];
            const f64 ov = a.end() - b.start;
            if (ov <= kArrOverlapEps) continue;
            // The one admitted overlap: a bounded crossfade both sides asked for.
            // An uncovered overlap is two clips summing at full level, which is a
            // mix decision the timeline cannot express and the user cannot see.
            if (ov <= kMaxOverlapBeats && a.fadeOut >= ov && b.fadeIn >= ov) continue;
            if (b.start <= a.start) {
                if (b.end() >= a.end()) { drop[i] = 1; }        // entirely covered
                else {
                    // Head overlapped: the item moves forward, and `offset` moves
                    // with `start` by the same number of beats. That is the whole of
                    // "trimming the front of a clip does not change which audio is
                    // under the rest of it", and getting it wrong is the classic
                    // arrangement-editor bug.
                    const f64 delta = b.end() - a.start;
                    a.start  += delta;
                    a.offset += delta;
                    a.length -= delta;
                }
            } else {
                a.length = b.start - a.start;                   // tail overlapped
            }
            changed = true;
        }
        for (size_t i = lane.size(); i-- > 0;)
            if (drop[i]) lane.erase(lane.begin() + (long)i);

        // (d) Rule 5. Three items sounding at once cannot come from a gesture -- (c)
        //     runs first -- so this only ever fires on a hand-edited file or a
        //     hostile client, and the middle item is the one that goes.
        for (size_t i = 0; i + 2 < lane.size(); ++i)
            if (lane[i + 2].start < lane[i].end() - kArrOverlapEps) {
                lane.erase(lane.begin() + (long)(i + 1));
                changed = true;
                break;
            }

        return changed;
    };

    bool changed = false;
    // The cap is a belt to the termination argument above, not the mechanism:
    // a lane that hit it would be a bug, and stopping is still safe because a
    // sweep only ever shortens, moves forward or deletes.
    const size_t cap = lane.size() * 4 + 8;
    for (size_t i = 0; i < cap; ++i) {
        if (!sweep(lane)) return changed;
        changed = true;
    }
    return changed;
}

struct TrackModel {
    u64 uid = 0;
    std::string name = "Track";
    int   colorIdx = 0;
    ClipModel slots[kMaxScenes];
    std::vector<DeviceModel> devices;   // makes TrackModel move-only
    std::vector<SavedDevice> savedDevices;
    f32   fader = 0.85f;               // 0..1, mapped through faderToGain
    f32   pan   = 0.f;                 // -1..1
    f32   sends[kMaxReturns] = {};     // post-fader send levels, 0..1 linear
    bool  mute = false, solo = false, arm = false;
    f32   width = 94.f;

    // --- the arrangement -----------------------------------------------
    // Sorted by `start` and non-overlapping except for the bounded crossfade;
    // arrangeRepair() is what upholds that, and every edit goes through it.
    std::vector<ArrangeClip> arrange;
    // ABSOLUTE-beat automation lanes, one per address per track. Same AutoLane
    // type as a clip's envelopes and the same "the address is text, resolution
    // is GUI-side" rule; only the beat space differs, which is why the format
    // spells them `autolane` rather than `env`.
    std::vector<AutoLane> arrangeAutos;
    f32 arrHeight = kArrHeightDefault;   // this track's lane height, logical px
};

// A return bus (Live's A/B/... return tracks): a device chain and a level,
// no clips. Same instance-ownership rules as TrackModel::devices.
struct ReturnModel {
    u64 uid = 0;
    std::string name = "Return";
    std::vector<DeviceModel> devices;
    std::vector<SavedDevice> savedDevices;
    f32 fader = 0.85f;
};

struct SceneModel {
    u64 uid = 0;
    std::string name;
    f64 tempo = 0.0;                   // 0 => no tempo change on launch
};

// ---------------------------------------------------------------------------
// Time signatures, GUI side.
//
// The model type IS the realtime type. One struct, so publishing the map is a
// copy and not a translation, and so the beat <-> bar conversions in engine.h --
// the ones the metronome and the launch quantum use -- can be pointed straight
// at the session's own vector. `RtSig::beat` is derived and is maintained by
// normalizeSigs(); nothing else may write it.
// ---------------------------------------------------------------------------
using SigChange = RtSig;

inline int clSigNum(int n) { return clampv(n, 1, kSigNumMax); }
// A denominator is a POWER OF TWO. Out-of-range clamps and anything between two
// powers rounds DOWN to the one below it, because that is the direction that
// cannot lengthen a bar past what the file asked for: a hand-typed `sig 4 3`
// becomes 4/2, never 4/4. Applied identically on save and on load, so a file
// that says 3 says 2 forever after and the round trip is byte-stable.
inline int clSigDen(int d) {
    if (d < 1) return 1;
    if (d >= kSigDenMax) return kSigDenMax;
    int p = 1;
    while (p * 2 <= d) p *= 2;
    return p;
}
// A ceiling on the bar a change may sit at. kMaxSigs changes at this bound is
// still under the 1e7-beat ceiling every other beat field is clamped to.
inline constexpr int kMaxSigBar = 1000000;
inline int clSigBar(i64 b) { return (int)clampv(b, (i64)0, (i64)kMaxSigBar); }

// Sort, deduplicate, clamp, guarantee an entry at bar 0, and fill in the derived
// beats. THE normalizer: the parser calls it, every edit calls it, and the
// publisher calls it, so there is one definition of what a well-formed map is
// and it is the one sigMapValid checks for.
//
// Duplicates resolve LAST-WINS, which is what "a change at a bar that already
// has one replaces it" means when the two arrive in one list -- the stable sort
// is what makes "last" mean the later of the two in the file.
inline std::vector<SigChange> normalizedSigMap(const std::vector<SigChange>& in,
                                               int fallbackNum, int fallbackDen) {
    std::vector<SigChange> m;
    m.reserve(in.size() + 1);
    for (const SigChange& s : in) {
        SigChange c{};
        c.bar = clSigBar(s.bar);
        c.num = clSigNum(s.num);
        c.den = clSigDen(s.den);
        m.push_back(c);
    }
    std::stable_sort(m.begin(), m.end(),
                     [](const SigChange& a, const SigChange& b) { return a.bar < b.bar; });
    // Keep the last of every run of equal bars.
    std::vector<SigChange> out;
    out.reserve(m.size() + 1);
    for (size_t i = 0; i < m.size(); ++i) {
        if (i + 1 < m.size() && m[i + 1].bar == m[i].bar) continue;
        out.push_back(m[i]);
    }
    if (out.empty() || out.front().bar != 0)
        out.insert(out.begin(), SigChange{0, clSigNum(fallbackNum), clSigDen(fallbackDen), 0, 0.0});
    if (out.size() > (size_t)kMaxSigs) out.resize((size_t)kMaxSigs);
    sigMapRebase(out.data(), (int)out.size());
    return out;
}

struct Session {
    std::vector<TrackModel> tracks;
    std::vector<SceneModel> scenes;
    ReturnModel returns[kMaxReturns];   // fixed buses; empty chains = inert
    std::vector<DeviceModel> masterDevices;
    std::vector<SavedDevice> masterSavedDevices;
    // Monotonic UID source for every entity in this set. Serialized, so IDs
    // stay unique across save/load. Assign at creation; never reuse.
    u64 nextUid = 1;
    u64 newUid() { return nextUid++; }
    f64  tempo = 120.0;
    // The session signature -- bar 0's, and a MIRROR of sigs.front() once the
    // map is normalized. Kept as plain fields because that is what every reader
    // of a set in one signature actually wants and because a method cannot
    // share a name with the field it replaced; write it through
    // setSignature(0, ...), never directly, or the next normalize will overwrite
    // it from the map.
    int  sigNum = 4, sigDen = 4;
    // The signature map: one entry per change, sorted by bar, the first at bar
    // 0. Empty means "never touched" and reads as plain sigNum/sigDen
    // everywhere; normalizeSigs() turns it into the canonical one-entry form.
    std::vector<SigChange> sigs;

    // Normalize in place and re-mirror sigNum/sigDen from bar 0. Idempotent.
    void normalizeSigs() {
        sigs = normalizedSigMap(sigs, sigNum, sigDen);
        sigNum = sigs.front().num;
        sigDen = sigs.front().den;
    }
    // Set (or replace) the signature in force from `bar` on. bar 0 is the
    // session signature. THE mutator -- it normalizes, so the derived beats and
    // the sigNum/sigDen mirror can never be stale after it.
    void setSignature(int bar, int num, int den) {
        SigChange c{};
        c.bar = clSigBar(bar); c.num = clSigNum(num); c.den = clSigDen(den);
        sigs.push_back(c);                 // last-wins dedupe does the replacing
        normalizeSigs();
    }
    // Remove the change at `bar`, if there is one. Bar 0 is REFUSED: a piece is
    // always in some signature from its first bar, and "no session signature" is
    // not a state the map can express or the engine could play.
    bool removeSignature(int bar) {
        if (bar <= 0) return false;
        const size_t before = sigs.size();
        for (size_t i = 0; i < sigs.size(); ++i)
            if (sigs[i].bar == bar) { sigs.erase(sigs.begin() + (long)i); break; }
        if (sigs.size() == before) return false;
        normalizeSigs();
        return true;
    }
    // The conversions, over this session's map. Thin forwarders to the ONE
    // implementation in engine.cpp -- the UI's ruler and the engine's metronome
    // are reading the same function, which is the only way a drawn bar line and
    // a played one cannot disagree. Safe on an un-normalized (even empty) map:
    // the helpers read an empty one as plain 4/4.
    //
    // An EMPTY map is not "4/4": it is "one entry, sigNum/sigDen, at bar 0",
    // which is what a set that has never been re-barred means and what the
    // parser produces for a v1..v6 file. Synthesizing it here rather than
    // demanding a normalize first is what keeps `Session s; s.sigNum = 7;` --
    // exactly what a caller writes in a test or a fresh set -- from silently
    // measuring in 4/4.
    SigChange sigAtBar(int bar) const {
        const SigChange one = lone();
        const SigChange* d = sigs.empty() ? &one : sigs.data();
        const int n = sigs.empty() ? 1 : (int)sigs.size();
        return d[sigIndexAtBar(d, n, bar)];
    }
    SigChange sigAtBeat(f64 beat) const {
        const SigChange one = lone();
        const SigChange* d = sigs.empty() ? &one : sigs.data();
        const int n = sigs.empty() ? 1 : (int)sigs.size();
        return d[sigIndexAtBeat(d, n, beat)];
    }
    f64 beatOfBar(f64 bar) const {
        const SigChange one = lone();
        return sigs.empty() ? sigBeatOfBar(&one, 1, bar)
                            : sigBeatOfBar(sigs.data(), (int)sigs.size(), bar);
    }
    f64 barOfBeat(f64 beat) const {
        const SigChange one = lone();
        return sigs.empty() ? sigBarOfBeat(&one, 1, beat)
                            : sigBarOfBeat(sigs.data(), (int)sigs.size(), beat);
    }
    BarPos barPosAt(f64 beat) const {
        const SigChange one = lone();
        return sigs.empty() ? sigPosAt(&one, 1, beat)
                            : sigPosAt(sigs.data(), (int)sigs.size(), beat);
    }

    int  quantumIdx = 4;               // index into kQuantumBeats -> "1 Bar"
    bool metronome = false;
    // The arrangement loop brace. Session-wide, like the tempo and the quantum,
    // because there is one timeline. A disabled brace still remembers where it
    // was, which is what makes toggling it useful, so `loopOn` is its own field
    // rather than a zero-length range. `loopStart >= loopEnd` disables the loop
    // rather than being clamped: a zero-length loop is a request the engine
    // cannot honour, and clamping it would invent a length nobody asked for.
    f64  loopStart = 0.0;
    f64  loopEnd   = 16.0;
    bool loopOn    = false;
    std::string name = "Untitled";
    std::string path;                  // last saved location, empty if never

private:
    // The one-entry map an empty `sigs` stands for.
    SigChange lone() const {
        SigChange c{};
        c.num = clSigNum(sigNum);
        c.den = clSigDen(sigDen);
        return c;
    }
};

// Snapshot the session's signature map into ONE heap array and hand it to the
// engine. The displaced array comes home in Ev::SigsRetired and may be
// delete[]'d then and NOT BEFORE -- the RtNote protocol verbatim, and the reason
// this returns the pointer it published rather than swallowing it: the caller
// keeps it alive until the event arrives.
//
// Returns null when the command ring is full or the allocation failed, having
// published nothing; the engine keeps whatever map it had. A caller that gets
// null must try again, exactly as it must for a clip it failed to push.
//
// Every set should publish once after load and once after each signature edit.
// A set that never publishes plays in 4/4, which is what every build before this
// one did, and is why nothing breaks while a caller has not been taught yet.
inline const RtSig* publishSignatures(Engine& eng, const Session& s) {
    const std::vector<SigChange> m = normalizedSigMap(s.sigs, s.sigNum, s.sigDen);
    if (m.empty()) return nullptr;
    RtSig* a = new (std::nothrow) RtSig[m.size()];
    if (!a) return nullptr;
    for (size_t i = 0; i < m.size(); ++i) a[i] = m[i];
    Command c{};
    c.type = Cmd::SetSignatures;
    c.a = (i32)m.size();
    c.p = a;
    if (!eng.pushCommand(c)) { delete[] a; return nullptr; }
    return a;
}

enum class MainView { Session, Arrangement };

// The bottom panel shows one of two things at a time, like Live's Clip / Device
// view toggle. Ctrl+D still hides the whole panel.
enum class DetailTab { Clip, Devices };

struct BrowserEntry {
    std::string name, path;
    bool isDir = false;
    bool isAudio = false;
};

// A drag in flight, either from the browser or between clip slots.
struct DragState {
    enum class Kind { None, BrowserFile, Clip } kind = Kind::None;
    std::string path;                  // BrowserFile
    int srcTrack = -1, srcSlot = -1;   // Clip
    f32 startX = 0, startY = 0;
    bool armed = false;                // past the movement threshold
};

// Live's "Computer MIDI Keyboard": the letter rows become a piano feeding the
// same MIDI ring a hardware controller uses.
//
// Deliberately knows nothing about App, the window or the engine: update() is
// handed a raw physical-key snapshot and emits through a callback. That keeps
// the part with the actual subtlety in it — edge detection and note ownership —
// testable without a GUI, an audio device or a keyboard to press.
//
// The subtlety, part one: Input::keyPressed[] includes auto-repeat, so a held
// key would machine-gun note-ons. Notes therefore come from *edges* of held
// state, and each key remembers the note it started so a note-off is always the
// note that sounded, even if the octave moved while the key was down.
//
// Part two, and the reason this maps Input::scanDown[] rather than keyDown[]:
// a piano layout is POSITIONAL. The keys under the fingers form two and a bit
// octaves whatever the locale prints on them; on the reporter's German QWERTZ
// board the keysym map put the bottom row's C on 'y' and its A on 'z', which
// is not a keyboard anyone can play. Scancodes are evdev's (KEY_Z = 44), so
// the bottom row is the bottom row everywhere. Shortcuts keep using keysyms —
// Ctrl+S should be the key labelled S — which is why the two are now
// unrelated, and why consumes() had to change with them.
struct KbdPiano {
    static constexpr int kScanCount   = 256;   // Input::scanDown[]
    static constexpr int kHighestSemi = 28;    // top row's last key, the top of the range
    static constexpr int kDefaultBase = 48;    // C3
    static constexpr int kDefaultVel  = 100;
    static constexpr int kOctave      = 12;

    struct Result { bool baseChanged = false; };

    KbdPiano() { for (int i = 0; i < kScanCount; ++i) active_[i] = -1; }

    // Semitones above the base note for a physical key, -1 if it is not one.
    // FL Studio's layout, which lays two and a bit octaves across the board
    // instead of Live's one. Positions are named by their US-QWERTY legends
    // purely because that is how the layout is documented everywhere; the codes
    // are what matters, and they are evdev's (linux/input-event-codes.h).
    static int semiFor(int scan) {
        switch (scan) {
        // lower octave — white (Z X C V B N M row, KEY_Z = 44)
        case 44: return 0;    case 45: return 2;    case 46: return 4;
        case 47: return 5;    case 48: return 7;    case 49: return 9;
        case 50: return 11;
        // lower octave — black (S D _ G H J on the home row, KEY_A = 30)
        case 31: return 1;    case 32: return 3;    case 34: return 6;
        case 35: return 8;    case 36: return 10;
        // upper octave — white (Q W E R T Y U row, KEY_Q = 16)
        case 16: return 12;   case 17: return 14;   case 18: return 16;
        case 19: return 17;   case 20: return 19;   case 21: return 21;
        case 22: return 23;
        // upper octave — black (2 3 _ 5 6 7 on the digit row, KEY_1 = 2)
        case 3:  return 13;   case 4:  return 15;   case 6:  return 18;
        case 7:  return 20;   case 8:  return 22;
        // and on into the third (I O P, with 9 0 as its blacks)
        case 23: return 24;   case 24: return 26;   case 25: return 28;
        case 10: return 25;   case 11: return 27;
        default: return -1;
        }
    }

    // Shortcut gating, and no longer a per-key question. Shortcuts are keysym-
    // based while the piano is scancode-based, so on a non-US layout there is
    // no correspondence left to consult: the key that plays a C types 'y' on
    // QWERTZ, 'w' on AZERTY. While the piano is live it therefore owns the
    // whole printable block — every unmodified letter/digit shortcut is
    // suppressed, which is also what Live does. Modified chords are unaffected
    // (notes only fire unmodified, so Ctrl+S still saves), and space is left
    // out on purpose: transport works while playing, on every DAW there is.
    static bool consumes(int key) { return key > 32 && key <= 126; }

    // `enabled` is the whole gate: feature on, no text field focused, no
    // command modifier down. When it is false the piano still runs, because
    // that is what releases notes held across losing the gate, and because it
    // has to re-adopt the physical key state: a key already down when the gate
    // returns (the 'k' of Ctrl+Shift+K, a letter typed into a field) must not
    // read as a fresh press.
    //
    // The octave keys come in as their own flags rather than through the
    // scancode array: PageUp/PageDown are *labelled* keys, not positions on a
    // keyboard-shaped instrument, so they follow the layout like every other
    // named shortcut. They moved off Z and X when those became notes; velocity
    // lost its keys entirely (C and V are notes) and lives on the control bar.
    template <class Emit>
    Result update(const bool* scanDown, bool octaveUp, bool octaveDown, bool enabled,
                  const Emit& emit) {
        Result res;
        if (!enabled) {
            allNotesOff(emit);
            for (int k = 0; k < kScanCount; ++k) prev_[k] = scanDown[k];
            prevOctUp_ = octaveUp;
            prevOctDown_ = octaveDown;
            return res;
        }
        // Down edges only: the octave moves once per physical press.
        if (octaveUp   && !prevOctUp_)   res.baseChanged |= shiftOctave(kOctave);
        if (octaveDown && !prevOctDown_) res.baseChanged |= shiftOctave(-kOctave);
        prevOctUp_ = octaveUp;
        prevOctDown_ = octaveDown;

        for (int k = 0; k < kScanCount; ++k) {
            const bool now = scanDown[k], was = prev_[k];
            prev_[k] = now;
            if (now == was) continue;              // held: no event, no repeat
            if (!now) {                            // up edge -> the note this key started
                if (active_[k] >= 0) {
                    emit(MidiMsg{0x80, (u8)active_[k], 0, 0, 0});
                    active_[k] = -1;
                }
                continue;
            }
            const int semi = semiFor(k);
            if (semi < 0) continue;
            const int note = base_ + semi;         // shiftOctave keeps this in 0..127
            active_[k] = (i8)note;
            emit(MidiMsg{0x90, (u8)note, (u8)vel_, 0, 0});
        }
        return res;
    }

    // Ends every sounding note. Called when the piano is switched off, and from
    // update() whenever the gate closes: a hung note outlives the UI state that
    // started it, and nothing downstream will clean it up.
    template <class Emit>
    void allNotesOff(const Emit& emit) {
        for (int k = 0; k < kScanCount; ++k) {
            if (active_[k] < 0) continue;
            emit(MidiMsg{0x80, (u8)active_[k], 0, 0, 0});
            active_[k] = -1;
        }
    }

    int base() const { return base_; }
    int velocity() const { return vel_; }
    // Velocity is a control-bar number now rather than a pair of keys, so the
    // owner sets it directly. Only notes started after this point take it: a
    // sounding note keeps the velocity it was struck with.
    void setVelocity(int v) { vel_ = clampv(v, 1, 127); }
    // The base is always a C (it only ever moves by whole octaves from C3).
    int octave() const { return base_ / kOctave - 1; }

private:
    // Clamped so the whole mapped span stays inside 0..127: the lowest key must
    // not go under 0 and 'p', twenty-eight semitones up, must not go over 127.
    bool shiftOctave(int by) {
        const int b = base_ + by;
        if (b < 0 || b + kHighestSemi > 127) return false;
        base_ = b;
        return true;
    }

    int  base_ = kDefaultBase;
    int  vel_  = kDefaultVel;
    bool prev_[kScanCount]{};      // scanDown[] as of last update, for edges
    i8   active_[kScanCount];      // note each key started, -1 = silent
    bool prevOctUp_ = false, prevOctDown_ = false;
};

} // namespace lat
