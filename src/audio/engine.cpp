#include "engine.h"
#include "../plugin/host.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
// FTZ/DAZ (denormal flushing) is x86-only here; other ISAs no-op. See process().
#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>   // _MM_SET_DENORMALS_ZERO_MODE (DAZ)
#include <xmmintrin.h>   // _MM_SET_FLUSH_ZERO_MODE (FTZ)
#endif

namespace lat {

static constexpr f64 kPi = 3.14159265358979323846;
static constexpr f64 kEps = 1e-9;

// Sentinel for "this clip has no follow action pending". See the note on
// Track::fireBeat's double duty below.
static constexpr f64 kNoFollow = 1e300;

// Messages pulled off the MIDI ring per block. The ring holds 1024; taking a
// quarter of it at a time bounds the stack cost (2 KB) and the work a single
// callback can be handed, and anything left over is simply one block late.
static constexpr int kMidiPerBlock = 256;

// Shortest loop a MIDI clip may claim. Anything under a 1/64 note is a bad edit
// rather than music, and it is what would turn the lap loop in renderMidiVoice
// into a spin and the overdub wrap below into a division by ~zero. Both paths
// use this one constant so "plays" and "can be overdubbed into" never disagree.
static constexpr f64 kMinLoopBeats = 1.0 / 64.0;

// ---------------------------------------------------------------------------
// resilient critical events (RT-AUDIT §1.6)
//
// RecordFinished / MidiRecordFinished / ChainRetired / NotesRetired each carry a
// heap pointer back to the GUI and are the ONLY channel that returns it: a
// dropped RecordFinished loses a recording *and* leaks its buffer, unrecoverably
// (the cosmetic events — ClipStarted, meters, ... — either self-heal from the
// mirrored atomics or are re-derivable by the drain proof). The event ring is
// SPSC and a full ring makes push() fail silently, so a failed push of one of
// these is instead PARKED here, audio-thread-owned, and retried at the top of
// every process() before the ring is touched again.
//
// engine.h is a frozen contract with no room for this, so — exactly like the PDC
// state below — it lives in a side-table keyed by the Engine's address. The
// parking buffer is fixed and audio-thread-only; overflowing it (the GUI wedged
// for >kCap critical events) bumps a counter rather than allocating.
namespace {

struct PendingEv {
    static constexpr int kCap = 128;
    Event ev[kCap];
    int   len = 0;
    std::atomic<u64> dropped{0};
};

struct PendTable {
    static constexpr int kSlots = 4;   // app, daemon, renderer, tests: one each
    std::atomic<const Engine*> owner[kSlots];
    PendingEv* slot[kSlots] = {};
    ~PendTable() { for (auto* p : slot) delete p; }
};
PendTable gPend;

// Audio thread: a handful of pointer compares. Null => never prepared.
PendingEv* pendFind(const Engine* e) {
    for (int i = 0; i < PendTable::kSlots; ++i)
        if (gPend.owner[i].load(std::memory_order_acquire) == e) return gPend.slot[i];
    return nullptr;
}

// GUI thread, from prepare(): claim a slot (allocating on first use) and clear it.
PendingEv* pendAcquire(const Engine* e) {
    int idx = -1;
    for (int i = 0; i < PendTable::kSlots; ++i)
        if (gPend.owner[i].load(std::memory_order_relaxed) == e) { idx = i; break; }
    if (idx < 0)
        for (int i = 0; i < PendTable::kSlots; ++i)
            if (!gPend.owner[i].load(std::memory_order_relaxed)) { idx = i; break; }
    if (idx < 0) idx = 0;               // four is more than any process needs
    if (!gPend.slot[idx]) {
        gPend.slot[idx] = new (std::nothrow) PendingEv();
        if (!gPend.slot[idx]) return nullptr;
    }
    gPend.slot[idx]->len = 0;
    gPend.owner[idx].store(e, std::memory_order_release);
    return gPend.slot[idx];
}

// Push a critical event, falling back to the parking buffer. If anything is
// already parked this event must queue behind it to keep order.
void emitCritical(const Engine* e, Ring<Event, 1024>& evts, const Event& ev) {
    PendingEv* pe = pendFind(e);
    if ((!pe || pe->len == 0) && evts.push(ev)) return;
    if (!pe) return;                    // no slot (allocation failed): nothing to do
    if (pe->len < PendingEv::kCap) pe->ev[pe->len++] = ev;
    else pe->dropped.fetch_add(1, std::memory_order_relaxed);
}

// Retry parked critical events, in order, before any fresh event this block.
void flushPendingEv(const Engine* e, Ring<Event, 1024>& evts) {
    PendingEv* pe = pendFind(e);
    if (!pe || pe->len == 0) return;
    int i = 0;
    while (i < pe->len && evts.push(pe->ev[i])) ++i;    // deliver a prefix
    if (i == 0) return;                                 // ring still full
    const int remain = pe->len - i;
    for (int j = 0; j < remain; ++j) pe->ev[j] = pe->ev[i + j];
    pe->len = remain;
}

} // namespace

// ---------------------------------------------------------------------------
// generative scheduling: deterministic "randomness"
//
// engine.h is a frozen contract with no room for an RNG state member, but a
// stateful generator would have been the wrong tool anyway: its output depends
// on how many times it has been called, which depends on the buffer size and
// on the order commands happened to arrive in. Hashing the *musical event*
// instead — track, slot, and the beat the launch was scheduled for — gives a
// value that is uniform, allocation free, branch-light, and identical for the
// same event no matter how the audio was chopped up. That is what makes an
// offline render of a probabilistic set reproducible, and what lets a test
// assert that two runs agree sample for sample.
// ---------------------------------------------------------------------------

static inline u64 mix64(u64 x) {                  // splitmix64 finaliser
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// `domain` separates independent draws that share an event (the launch roll and
// the Random follow pick must not correlate).
static inline f64 randUnit(int domain, int track, int slot, f64 beat) {
    // 960 ticks per beat is finer than the smallest quantum we schedule on
    // (1/32 note = 0.125 beats = 120 ticks), so distinct events never collide.
    const u64 ticks = (u64)(i64)std::llround(beat * 960.0);
    const u64 k = mix64(((u64)(u32)domain << 56) ^ ((u64)(u32)track << 44) ^
                        ((u64)(u32)(slot + 1) << 32) ^ ticks);
    return (f64)(k >> 11) * (1.0 / 9007199254740992.0);   // 53 bits -> [0,1)
}

// Live semantics: a failed roll cancels the launch and leaves whatever was
// playing alone. prob >= 1 skips the draw entirely so the common case is free.
static inline bool rollLaunch(const RtClip& c, int track, int slot, f64 beat) {
    if (c.prob >= 1.0) return true;
    if (c.prob <= 0.0) return false;
    return randUnit(1, track, slot, beat) < c.prob;
}

// Beat at which `c`'s follow action comes due, given it launched at `at`.
static inline f64 followDueBeat(const RtClip& c, f64 at) {
    if (c.followAction <= (int)Follow::None || c.followAction >= kFollowCount) return kNoFollow;
    const f64 len = c.followBeats > 0.0 ? c.followBeats : c.lengthBeats;
    if (len <= 0.0) return kNoFollow;
    return at + len;
}

// Which slot a follow action lands on, or -1 for "nothing to launch". `row` is
// one track's slots. Stop is handled by the caller; it queues a stop, not a
// launch.
static int followTarget(const RtClip* row, int cur, int action, int track, f64 beat) {
    switch ((Follow)action) {
    case Follow::Again: return row[cur].valid ? cur : -1;
    case Follow::First: return row[0].valid ? 0 : -1;
    case Follow::Next:
    case Follow::Previous: {
        // Walking backwards is the same walk with a step of -1 taken mod the
        // slot count, so both directions share one wrap-around loop.
        const int step = (action == (int)Follow::Next) ? 1 : kMaxScenes - 1;
        for (int k = 1; k < kMaxScenes; ++k) {
            const int s = (cur + step * k) % kMaxScenes;
            if (row[s].valid) return s;
        }
        return -1;
    }
    case Follow::Random: {
        // Uniform over the valid slots, repeats included: Live's Random can
        // pick the clip that is already playing and that is musically useful.
        int valid[kMaxScenes];
        int nv = 0;
        for (int s = 0; s < kMaxScenes; ++s) if (row[s].valid) valid[nv++] = s;
        if (nv == 0) return -1;
        int idx = (int)(randUnit(2, track, cur, beat) * (f64)nv);
        if (idx >= nv) idx = nv - 1;              // guards the 1.0 corner case
        return valid[idx];
    }
    default: return -1;
    }
}

// ---------------------------------------------------------------------------
// MIDI plumbing, shared by clip playback, note retirement and the take machine.
//
// Track and Voice are private nested types of Engine and engine.h is frozen, so
// these deduce them through templates rather than naming them. Their *members*
// are public, which is all a template body needs.
// ---------------------------------------------------------------------------

// One channel-voice message to every note-capable device on the track's chain,
// with the frame offset the caller worked out. Unlike the live-input path this
// is deliberately *not* gated on arm: arm decides whether the player's keyboard
// reaches the instrument, while a launched clip has to sound whatever the arm
// button says — same as Live.
template <class TrackT>
static void sendNote(const TrackT& t, u8 status, u8 pitch, u8 vel, int frame) {
    if (!t.chain || t.chain->count <= 0) return;
    const int cnt = t.chain->count < kMaxChainFx ? t.chain->count : kMaxChainFx;
    const u8 bytes[3] = {status, (u8)(pitch & 0x7F), (u8)(vel & 0x7F)};
    for (int i = 0; i < cnt; ++i) {
        PluginInstance* fx = t.chain->fx[i];
        if (!fx) continue;
        const PluginDesc& d = fx->desc();
        if (!d.hasMidiIn && d.kind != PluginKind::Instrument) continue;
        fx->midi(bytes, 3, frame);
    }
}

// Every note-off the voice still owes, delivered now. This is the one thing a
// MIDI voice must never skip: a clip that stops, switches, loses its notes or
// dies with the transport would otherwise leave the instrument holding whatever
// it happened to be playing, and nothing downstream can undo that.
template <class TrackT, class VoiceT>
static void flushOffs(const TrackT& t, VoiceT& v, int frame) {
    for (auto& o : v.offs)
        if (o.used) { sendNote(t, 0x80, o.pitch, 0, frame); o.used = false; }
}

// After the notes under a playing voice are replaced, `nextNote` indexes an
// array that no longer exists. Re-seeking to the first note at or after the
// current position keeps the lap running without replaying what already sounded
// or skipping the rest of the bar.
template <class VoiceT>
static void reseekNotes(VoiceT& v, const RtClip& c) {
    int i = 0;
    if (c.notes) while (i < c.noteCount && c.notes[i].beat < v.beatPos) ++i;
    v.nextNote = i;
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

// Shortest note a take will keep. A note-off in the same millisecond as its
// note-on is a mis-hit, not a grace note, and a zero-length note is invisible
// in the piano roll and impossible to grab.
static constexpr f64 kMinNoteLen = 1.0 / 32.0;

// ---------------------------------------------------------------------------
// overdub: a MIDI take that laps over the clip already in the slot
//
// Whether a take is an overdub is *derived*, every time it is needed, from what
// the target slot holds — there is no flag. engine.h is frozen and every Track
// field is spoken for, so latching one at the toggle would have meant stealing
// the sign bit of some unrelated member, and that would have been the worse
// answer even with room to spare: overdub is a property of what is in the slot
// *now*, and the GUI may clear, replace or repush that clip in the middle of a
// take. A latched flag would go on wrapping notes into a clip that no longer
// exists, or keep treating a slot as empty after one appeared; re-deriving
// cannot. The cost is a handful of loads on a path that already walks every
// track once per sub-block.
// ---------------------------------------------------------------------------

// The clip a take into `slot` would lap over, or null when this is an ordinary
// take (audio take, empty slot, audio clip, unusable loop length).
static const RtClip* overdubSlot(const RtClip* row, int slot, bool midi) {
    if (!midi || slot < 0 || slot >= kMaxScenes) return nullptr;
    const RtClip& c = row[slot];
    if (!c.valid || !c.isMidi || c.lengthBeats <= kMinLoopBeats) return nullptr;
    return &c;
}

// The same clip, but only once it is the voice actually running on the track:
// the wrap origin is the *voice's* position in the loop, so with no voice on it
// there is nothing to wrap against and the take falls back to take-relative
// stamping. A voice marked `releasing` still counts — it dies in the next
// renderRange, and until then its beatPos is the truthful clip position, which
// is exactly what the transport-stop path needs to close its open notes with.
template <class TrackT>
static const RtClip* overdubVoice(const RtClip* row, const TrackT& t) {
    const RtClip* c = overdubSlot(row, t.recSlot, t.recMidi);
    if (!c || !t.voice.active || t.voice.clip != c) return nullptr;
    return c;
}

// Reduces a beat position into [0, L). Used for the wrap origin and nothing
// else, so the fp guard is worth its two branches: a position a hair under zero
// would otherwise come back as L itself and put a note one lap out.
static inline f64 wrapBeat(f64 b, f64 L) {
    b -= std::floor(b / L) * L;
    if (!(b >= 0.0)) return 0.0;               // also catches NaN
    return b < L ? b : 0.0;
}

// How long a note captured in an overdub pass lasts, from two *in-loop*
// positions. Both the wrap ("held past the loop point": off is numerically
// before on) and the long hold ("held more than a lap": off comes round again
// past where it started) end at the loop end rather than splitting the note in
// two. Clamping is the choice that matches what the pattern can actually replay
// — the clip is one lap long, so a note that outlives the lap has nowhere to be
// except the lap's end — and it is also what the player hears: the next pass
// re-triggers the note at its in-loop start, so a clamped tail joins seamlessly
// onto the next lap's attack instead of stacking a second voice on top of it.
static inline f64 overdubNoteLen(f64 from, f64 to, f64 L) {
    const f64 cap = L - from;                  // clamp-to-loop-end
    f64 len = to - from;
    if (len <= 0.0 || len > cap) len = cap;
    return len > kMinNoteLen ? len : kMinNoteLen;
}

// Appends one note to the take buffer, O(1). (RT-AUDIT §1.5)
//
// This used to insertion-sort each note into place to keep the buffer ordered.
// The stated premise — "notes close in note-off order, which for a human is
// almost start order, so the backward scan stops on its first compare" — is
// FALSE for an overdub pass: there the beat is wrapBeat(...) reduced modulo the
// clip loop, so pass 3 can land a note at beat 0.5 after pass 2 landed one at
// beat 3.9. A single wrapped note-off then memmoves the whole buffer (~96 KB at
// 4000 notes) inside process(). So append here — O(1), bounded, no memmove —
// and sort ONCE at the stop boundary in finishRec(), which is a single bounded
// (<= recCap, one-time) sort instead of a per-note burst. The buffer only ever
// leaves via the finish event, and finishRec sorts before pushing it, so the
// GUI still receives it ordered; nothing in the engine reads note order in the
// meantime.
static bool appendNote(RtNote* buf, i64& len, i64 cap, const RtNote& n) {
    if (!buf || len >= cap) return false;
    buf[len++] = n;
    return true;
}

// Cancels a take that has not begun. There is no buffer to hand back, so no
// event goes out either — it is pure state, including the hand-over request.
template <class TrackT>
static void cancelRec(TrackT& t) {
    t.recBuf = nullptr;
    t.recCap = 0;
    t.recLen = 0;
    t.recSlot = -1;
    t.recPhase = 0;
    t.recFireBeat = 0.0;
    t.recMidi = false;
    t.recStartBeat = 0.0;
    for (auto& o : t.recOpen) o.used = false;
    t.pendBuf = nullptr;
    t.pendCap = 0;
    t.pendSlot = -1;
    t.pendMidi = false;
}

// Hands a finished take back to the GUI and returns the track to idle. Track is
// a private nested type and engine.h cannot be touched, so this deduces it
// through a template rather than naming it. `endBeat` is the take-relative beat
// the boundary fell on; a MIDI take closes whatever is still held there, which
// is what stops a key that was down when you hit stop from becoming a note of
// zero length or, worse, of no length at all.
//
// `loopLen` > 0 marks an overdub pass: `endBeat` is then the boundary's
// position *inside the clip's loop*, not a take-relative beat, and the notes
// still held close against it the same way they would have closed against a
// note-off — wrap and over-long hold clamped to the loop end.
template <class TrackT, class EvRing>
static void finishRec(const Engine* eng, int ti, TrackT& t, EvRing& evts, f64 endBeat,
                      f64 loopLen = 0.0) {
    if (t.recMidi) {
        // recBuf is the f32* the Cmd contract gives us; a MIDI take stores
        // RtNote through it and recCap/recLen count notes. See the note on
        // Cmd::RecordMidiSlot in engine.h — the GUI allocated it as RtNote*.
        RtNote* notes = (RtNote*)t.recBuf;
        for (auto& o : t.recOpen) {
            if (!o.used) continue;
            RtNote n;
            n.beat  = o.beat;
            n.len   = loopLen > 0.0
                          ? overdubNoteLen(o.beat, endBeat, loopLen)
                          : ((endBeat - o.beat) > kMinNoteLen ? (endBeat - o.beat) : kMinNoteLen);
            n.pitch = o.pitch;
            n.vel   = o.vel;
            appendNote(notes, t.recLen, t.recCap, n);
            o.used = false;
        }
        // Notes were appended unsorted during capture (see appendNote); sort by
        // start beat here, once, at the boundary. std::sort is in-place introsort
        // — no allocation — and bounded by recCap, versus the per-note memmove
        // the old insertion sort ran inside the sub-block loop.
        if (notes && t.recLen > 1)
            std::sort(notes, notes + t.recLen,
                      [](const RtNote& a, const RtNote& b) { return a.beat < b.beat; });
        emitCritical(eng, evts, {Ev::MidiRecordFinished, ti, t.recSlot, (f64)t.recLen,
                                 (void*)t.recBuf});
    } else {
        emitCritical(eng, evts, {Ev::RecordFinished, ti, t.recSlot, (f64)t.recLen,
                                 (void*)t.recBuf});
    }
    t.recBuf = nullptr;
    t.recCap = 0;
    t.recLen = 0;
    t.recSlot = -1;
    t.recPhase = 0;
    t.recFireBeat = 0.0;
    t.recMidi = false;
    t.recStartBeat = 0.0;
}

// ---------------------------------------------------------------------------
// sample fetch
// ---------------------------------------------------------------------------

// Reads one interpolated stereo frame at `pos` (source frames), wrapping into
// the clip's loop region. Realtime safe, branch-light.
static inline void fetch(const RtClip& c, f64 pos, f32& outL, f32& outR) {
    const f64 ls = (f64)c.loopStart, le = (f64)c.loopEnd;
    if (c.loop && le > ls) {
        const f64 len = le - ls;
        pos = pos - ls;
        pos -= std::floor(pos / len) * len;
        pos += ls;
    }
    if (pos < 0.0 || pos >= (f64)c.frames) { outL = outR = 0.f; return; }
    const i64 i0 = (i64)pos;
    // The partner sample for interpolation has to wrap back to the loop start,
    // otherwise the last frame before every wrap interpolates towards silence
    // and short loops tick once per cycle.
    i64 i1 = i0 + 1;
    if (c.loop && le > ls && i1 >= c.loopEnd) i1 = c.loopStart;
    if (i1 >= c.frames) i1 = i0;

    const f32 fr = (f32)(pos - (f64)i0);
    const f32* p0 = c.data + (size_t)i0 * c.channels;
    const f32* p1 = c.data + (size_t)i1 * c.channels;
    if (c.channels >= 2) {
        outL = p0[0] + (p1[0] - p0[0]) * fr;
        outR = p0[1] + (p1[1] - p0[1]) * fr;
    } else {
        outL = outR = p0[0] + (p1[0] - p0[0]) * fr;
    }
}

// ---------------------------------------------------------------------------
// automation (docs/AUTOMATION.md §3)
//
// The rule everything below serves: the engine never writes an automated value
// into the field it is automating. Track::vol/pan/send stay whatever the user's
// fader last said; an envelope produces an *effective* value that exists for
// one block and reaches nothing but the mixdown. Device parameters are the one
// documented exception (§3.4/§3.5) — a plugin has a single storage slot and no
// notion of "effective" — and that is exactly why they carry a restore
// obligation, discharged below.
// ---------------------------------------------------------------------------

// Value of one lane at a clip-relative beat. Pure, allocation-free and safe on
// the audio thread; deliberately the same function the UI will call to draw the
// moving knob, so the displayed value and the applied value cannot disagree
// (§2.4).
//
// LINKAGE: engine.h is frozen for this pass and does not declare it, so it is
// defined here with external linkage. A `f32 autoValueAt(const RtAutoSet&,
// const RtAutoLane&, f64, f32);` declaration belongs beside the Rt structs the
// next time the header opens; until then the UI (and the tests) declare it
// themselves and link against this definition. Nothing about the body changes
// when that happens.
//
// Semantics, all three load-bearing:
//   * before the first point: the first point's value (there is no "nowhere" to
//     ramp in from at clip start);
//   * after the last point: the last point's value, held to the loop end;
//   * a lane with no points evaluates to `fallback` — the caller's
//     un-automated value. An empty lane is UI state, not content, so it must be
//     a no-op rather than a jump to zero. `fallback` is returned unclamped: it
//     is a value the lane has no opinion about.
//
// The search is a bisection rather than the cursor-scan §2.4 sketched, because
// the signature the contract froze carries no cursor and a shared pure function
// cannot own one: the GUI calls it at arbitrary beats for whatever clip the
// mouse is over. At kMaxClipAutoPoints (4096) that is at most 12 compares once
// per block per lane, against a scan whose worst case is 4096 of them.
// `curve` is reserved: any non-zero shape renders as linear in this wave (§2.1).
f32 autoValueAt(const RtAutoSet& s, const RtAutoLane& l, f64 beat, f32 fallback) {
    const int n = l.count;
    // The window is validated here and not trusted: the set is public memory
    // built on the other side of a process boundary, and a bad first/count must
    // be an inert lane rather than a read outside the block.
    if (!s.points || n <= 0 || l.first < 0 || l.first > s.pointCount - n) return fallback;
    const RtAutoPoint* p = s.points + l.first;

    // A publisher that inverted lo/hi would otherwise turn clampv into a value
    // that is neither bound.
    const f32 lo = l.lo <= l.hi ? l.lo : l.hi;
    const f32 hi = l.lo <= l.hi ? l.hi : l.lo;

    if (!(beat > p[0].beat))     return clampv(p[0].value, lo, hi);      // NaN lands here
    if (beat >= p[n - 1].beat)   return clampv(p[n - 1].value, lo, hi);

    // Last index whose beat is <= `beat`. Unsorted input gives some point's
    // value rather than an out-of-range read — defined, if ugly, which is what
    // §4.2 asks for.
    int a = 0, b = n - 1;
    while (b - a > 1) {
        const int m = (a + b) >> 1;
        if (p[m].beat <= beat) a = m; else b = m;
    }
    const f64 span = p[b].beat - p[a].beat;
    if (!(span > 0.0)) return clampv(p[b].value, lo, hi);
    f64 t = (beat - p[a].beat) / span;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return clampv((f32)((f64)p[a].value + ((f64)p[b].value - (f64)p[a].value) * t), lo, hi);
}

// The clip-relative position of a voice. ONE definition, so the number the UI
// draws the playhead at and the number an envelope is evaluated against are
// provably the same quantity (§3.1) — clipPhase's stores go through it too.
template <class VoiceT>
static inline f64 voiceClipPhase(const VoiceT& v, const RtClip& c) {
    if (c.isMidi) return c.lengthBeats > 0.0 ? v.beatPos / c.lengthBeats : 0.0;
    const f64 loopLen = (f64)(c.loopEnd - c.loopStart);
    return loopLen > 0.0 ? (v.srcPos - (f64)c.loopStart) / loopLen : 0.0;
}
template <class VoiceT>
static inline f64 voiceClipBeat(const VoiceT& v, const RtClip& c) {
    if (c.isMidi) return v.beatPos;                 // already in clip beats
    return voiceClipPhase(v, c) * c.lengthBeats;
}

namespace {

// Engine-side automation state.
//
// Two things have to persist across blocks and neither can live where §3
// wanted it (Track::autoA, Track::autoHold): engine.h is frozen. So, exactly as
// the PDC and parked-event state above already do, it sits in a side table
// keyed by the Engine's address, claimed in prepare() and only ever read and
// written by the audio thread afterwards.
//
//   * `inert` — the lanes this engine has given up on. The published lane flags
//     carry kAutoInert, but the published RtAutoSet is *const*: the engine may
//     not write into GUI-owned memory, so it keeps its own bitmap. One u32 is
//     one bit per lane and kMaxRtAutoLanes is 16.
//   * `hold`  — the pre-automation value of every device parameter this track's
//     envelopes have taken over (§3.5), captured with getParam() the first time
//     a lane writes one and written back when the lane stops applying.
//
// Both are scoped to `set`, the published RtAutoSet they belong to. That is
// what makes "emit Ev::AutoLaneInert once per published set" exact and what
// makes the restore fire on every event that ends the application — the clip
// stopping (no voice, so no set), the clip being cleared (autos gone), the set
// being republished (a different pointer), the transport stopping (the voice
// releases and dies). Cmd::SetChain is the one trigger that cannot be derived
// from the set pointer, and it is handled where it happens, before the swap.
struct AutoTrack {
    const RtAutoSet* set = nullptr;
    u32 inert = 0;
    struct Hold { i32 devSlot = -1; i32 param = -1; f32 was = 0.f; bool used = false; };
    Hold hold[kMaxRtAutoLanes];
    bool anyHold = false;
};

struct AutoState {
    AutoTrack t[kMaxTracks];
};

struct AutoTable {
    static constexpr int kSlots = 4;   // app, daemon, renderer, tests: one each
    std::atomic<const Engine*> owner[kSlots];
    AutoState* slot[kSlots]  = {};
    u64        stamp[kSlots] = {};
    u64        clock = 0;
    // Freed at exit so a leak checker has nothing to say. By then the backend
    // has stopped and no audio thread is inside process().
    ~AutoTable() { for (auto*& p : slot) { delete p; p = nullptr; } }
};
AutoTable gAuto;

// Audio thread: a handful of pointer compares. Null => never prepared, which
// every caller reads as "this engine applies no automation".
AutoState* autoFind(const Engine* e) {
    for (int i = 0; i < AutoTable::kSlots; ++i)
        if (gAuto.owner[i].load(std::memory_order_acquire) == e) return gAuto.slot[i];
    return nullptr;
}

// GUI thread, from prepare(). Allocates on first use and reuses the slot on
// every re-prepare, like pdcAcquire.
AutoState* autoAcquire(const Engine* e) {
    int idx = -1;
    for (int i = 0; i < AutoTable::kSlots; ++i)
        if (gAuto.owner[i].load(std::memory_order_relaxed) == e) { idx = i; break; }
    if (idx < 0)
        for (int i = 0; i < AutoTable::kSlots; ++i)
            if (!gAuto.owner[i].load(std::memory_order_relaxed)) { idx = i; break; }
    if (idx < 0) {                      // table full: take the oldest slot over
        idx = 0;
        for (int i = 1; i < AutoTable::kSlots; ++i)
            if (gAuto.stamp[i] < gAuto.stamp[idx]) idx = i;
        // The engine that held it applies no automation from here on, which is
        // the safe degradation: its scalars are the user's own values and its
        // device parameters were restored when its clips stopped. A fifth
        // *concurrently prepared* Engine has never existed in this tree; the
        // real fix is a member in engine.h next time it thaws.
        LOGW("auto: no free automation slot, taking the oldest (engine %p)", (const void*)e);
    }
    if (!gAuto.slot[idx]) {
        gAuto.slot[idx] = new (std::nothrow) AutoState();
        if (!gAuto.slot[idx]) return nullptr;
    }
    *gAuto.slot[idx] = AutoState{};
    gAuto.stamp[idx] = ++gAuto.clock;
    gAuto.owner[idx].store(e, std::memory_order_release);
    return gAuto.slot[idx];
}

// Hands the parameters an outgoing set took over back to what they were.
//
// This is the engine cleaning up what the engine started, which is the same
// obligation flushOffs() discharges for note-offs, and it fails the same way if
// skipped: silently, and only sometimes. `fresh` is the set taking over, if
// any: a parameter the incoming set marks kAutoOverridden is *dropped* rather
// than written back, because the user's hand on the knob is the newer statement
// (§3.6) and the value they are dragging must not be stamped over.
void autoRestore(AutoTrack& at, const RtChain* chain, const RtAutoSet* fresh) {
    if (!at.anyHold) return;
    const int freshLanes =
        fresh ? (fresh->laneCount < kMaxRtAutoLanes ? fresh->laneCount : kMaxRtAutoLanes) : 0;
    for (auto& h : at.hold) {
        if (!h.used) continue;
        bool overridden = false;
        for (int i = 0; i < freshLanes; ++i) {
            const RtAutoLane& l = fresh->lanes[i];
            if (l.target == (i32)AutoTarget::DeviceParam && l.devSlot == h.devSlot &&
                l.index == h.param && (l.flags & kAutoOverridden)) { overridden = true; break; }
        }
        if (!overridden && chain && h.devSlot >= 0 && h.devSlot < kMaxChainFx &&
            h.devSlot < chain->count)
            if (PluginInstance* fx = chain->fx[h.devSlot]) fx->setParamRT(h.param, h.was);
        h.used = false;
        h.devSlot = -1;
        h.param = -1;
    }
    at.anyHold = false;
}

// What one block of class-A automation says about one track. Block-local: it is
// computed at the top of process() and consumed by the mixdown at the bottom,
// and nothing about it outlives the callback — which is the whole of §1's rule
// expressed as a lifetime. `any` false is the ordinary case and costs the
// mixdown exactly nothing (see the branch in process()).
struct AutoBlock {
    bool any = false;
    bool hasVol = false, hasPan = false;
    f32  vol0 = 0.f, vol1 = 0.f;      // DERIVED: gain, already through faderToGain
    f32  pan0 = 0.f, pan1 = 0.f;
    u32  sendMask = 0;
    f32  snd0[kMaxReturns] = {}, snd1[kMaxReturns] = {};
};

// A pan position that cannot poison the mix. NaN lands on centre, exactly as
// busGain lands a NaN send on silence.
inline f32 autoPan(f32 p) { return (p >= -1.f && p <= 1.f) ? p : 0.f; }

} // namespace

// ---------------------------------------------------------------------------
// mixer topology and plugin delay compensation
//
// The graph, and what every path from a voice to the master sum costs in
// frames (Lt = a track's chain latency, Lr = a return's, Lm = the master's):
//
//   dry    voice -> track chain (Lt) -> fader/pan -> master sum
//   send   voice -> track chain (Lt) -> fader/pan -> send tap
//                -> return chain (Lr) -> return vol -> master sum
//   click  metronome ------------------------------> master sum   (no chain)
//   all of the above ------------------> master chain (Lm) -> master fader
//
// The send tap sits *after* the track chain, so both paths out of one track
// carry the same Lt: whatever aligns a track's dry signal aligns its sends with
// it for free. That splits the problem into two independent stages.
//
//   1. Tracks against each other. Delay track i by (maxLt - Lt_i). Afterwards
//      every track's post-fader signal — dry and tapped alike — sits at maxLt.
//   2. Returns against each other and against the dry sum. A return's output
//      now sits at maxLt + Lr_r while the dry sum sits at maxLt, so delay
//      return r by (maxLr - Lr_r) and the dry sum by maxLr. Everything then
//      lands at maxLt + maxLr.
//
// The metronome is not on a track and enters the graph at 0, so it takes the
// same maxLt a zero-latency track would (it gets its own line, applied while
// outL/outR still holds nothing but the click) and then rides the dry bus's
// maxLr along with the tracks.
//
// The master chain is in series with the whole sum — nothing runs in parallel
// beside it — so it needs no compensation at all. Its latency is simply part of
// what the engine publishes:
//
//   Engine::latencyFrames = maxLt + maxLr + Lm
//
// Returns have no sends of their own, so there is no return -> return path to
// align. Live gates that behind an option and so could we; it is deliberately
// out of this wave, and nothing above assumes its absence beyond stage 2.
//
// Two properties this implementation holds onto:
//   * A chain's latency is read exactly once, when the chain is published, and
//     cached beside the pointer. latencyFrames() is const after prepare() per
//     the PluginInstance contract, so calling it per block would be a virtual
//     call per device per block for an answer that cannot have changed.
//   * When no chain anywhere reports latency, not one delay line is touched and
//     the arithmetic is the pre-PDC arithmetic, sample for sample. A set with no
//     latent devices must render bit-identically to before this existed.
// ---------------------------------------------------------------------------

namespace {

// Compensation is capped per stage. 1<<16 frames is 1.37 s at 48 kHz, which is
// past any sane linear-phase mastering chain, and it bounds both the memory
// below and the damage a plugin lying about its latency can do. A chain over
// the cap is clamped to it: the alignment is then wrong by the excess, which is
// strictly better than an unbounded allocation, and the audio thread cannot
// warn about it (no logging here) so latencyFrames simply reports the clamped
// figure — what the engine actually imposes.
constexpr int kPdcCap  = 1 << 16;
constexpr int kPdcMask = kPdcCap - 1;

// One delay line per parallel path: every track, every return, the dry bus and
// the click.
constexpr int kPdcDry   = kMaxTracks + kMaxReturns;
constexpr int kPdcClick = kPdcDry + 1;
constexpr int kPdcLines = kPdcClick + 1;

// Per-engine delay state. engine.h is a frozen contract with no room for any of
// this, and the delay storage is far too fat to sit in the Engine by value
// anyway (see the table below for where it lives and why).
struct Pdc {
    f32* mem = nullptr;                 // kPdcLines * 2 * kPdcCap frames

    // Cached chain latencies, written when a chain is published and never per
    // block. maxTrackLat / maxRetLat are derived from them at the end of the
    // drain that changed one.
    int  trackLat[kMaxTracks] = {};
    int  retLat[kMaxReturns]  = {};
    int  masterLat  = 0;
    int  maxTrackLat = 0, maxRetLat = 0;

    int  wpos   = 0;      // shared write cursor: every line is written the same
                          // n frames per block, so one cursor serves all of them
    int  filled = 0;      // frames written since the lines went into service
    bool active = false;  // any compensation at all? false => lines untouched
    bool dirty  = true;   // a cached latency changed; recompute the maxima

    f32* line(int i, int ch) { return mem + ((size_t)i * 2 + (size_t)ch) * (size_t)kPdcCap; }

    void reset() {
        for (auto& v : trackLat) v = 0;
        for (auto& v : retLat) v = 0;
        masterLat = maxTrackLat = maxRetLat = 0;
        wpos = 0;
        filled = 0;
        active = false;
        dirty = true;
        // The ring contents are deliberately *not* cleared: `filled` already
        // guarantees a line reads zeros until it has been written far enough
        // back to answer honestly, and a 20 MB memset is not free.
    }
};

// Where the state lives.
//
// engine.h is frozen, so the Engine cannot carry a pointer to this and the
// association has to be made on the side, keyed by the Engine's address.
// prepare() (GUI thread, before the audio thread exists) claims a slot and
// allocates; the audio thread only ever looks one up, which is a handful of
// pointer compares once per block.
//
// Four slots is three more than any process has ever needed — the app, the
// daemon, the renderer and the tests each run exactly one Engine — and the
// table is bounded on purpose so a process that churned through Engines cannot
// grow this without limit. A fifth *concurrently prepared* Engine evicts the
// least recently prepared slot and shares its storage, which would mean two
// engines writing one set of delay lines: audible nonsense, but not a crash and
// not out-of-bounds. The real fix is a member in engine.h next time it thaws.
struct PdcTable {
    static constexpr int kSlots = 4;
    std::atomic<const Engine*> owner[kSlots];
    Pdc* slot[kSlots]  = {};
    u64  stamp[kSlots] = {};
    u64  clock = 0;
    // Freed at exit so a leak checker has nothing to say about 20 MB of rings.
    // By then the backend has stopped and no audio thread is inside process().
    ~PdcTable() {
        for (int i = 0; i < kSlots; ++i)
            if (slot[i]) { std::free(slot[i]->mem); delete slot[i]; slot[i] = nullptr; }
    }
};
PdcTable gPdc;

// Audio thread. Null means "this Engine was never prepared, or its allocation
// failed" — every caller then behaves as if nothing on it reported latency.
Pdc* pdcFind(const Engine* e) {
    for (int i = 0; i < PdcTable::kSlots; ++i)
        if (gPdc.owner[i].load(std::memory_order_acquire) == e) return gPdc.slot[i];
    return nullptr;
}

// GUI thread, from prepare(). Allocates on first use for a given Engine and
// reuses the slot on every re-prepare (a sample-rate change, say).
Pdc* pdcAcquire(const Engine* e) {
    int idx = -1;
    for (int i = 0; i < PdcTable::kSlots; ++i)
        if (gPdc.owner[i].load(std::memory_order_relaxed) == e) { idx = i; break; }
    if (idx < 0)
        for (int i = 0; i < PdcTable::kSlots; ++i)
            if (!gPdc.owner[i].load(std::memory_order_relaxed)) { idx = i; break; }
    if (idx < 0) {                                  // table full: evict the oldest
        idx = 0;
        for (int i = 1; i < PdcTable::kSlots; ++i)
            if (gPdc.stamp[i] < gPdc.stamp[idx]) idx = i;
        LOGW("pdc: no free delay-compensation slot, sharing one (engine %p)", (const void*)e);
    }
    if (!gPdc.slot[idx]) {
        Pdc* p = new (std::nothrow) Pdc();
        if (!p) return nullptr;
        // calloc, not new[]: the pages stay untouched (and unresident) until a
        // line is actually written, which for a set with no latent device is
        // never. Zeroed anyway, so a line read before it is filled is silent.
        p->mem = (f32*)std::calloc((size_t)kPdcLines * 2 * (size_t)kPdcCap, sizeof(f32));
        if (!p->mem) { delete p; return nullptr; }
        gPdc.slot[idx] = p;
    }
    gPdc.stamp[idx] = ++gPdc.clock;
    gPdc.owner[idx].store(e, std::memory_order_release);
    return gPdc.slot[idx];
}

// Send and return-volume gains. Written this way rather than with clampv so a
// NaN lands on 0 instead of passing straight through: both multiply a bus that
// feeds the master, so one bad value out of a mis-scaled fader would poison the
// whole mix rather than a single track. 16 is +24 dB, past any useful send.
f32 busGain(f64 x) { return (x > 0.0 && x < 16.0) ? (f32)x : (x >= 16.0 ? 16.f : 0.f); }

// A chain's total latency: its devices are in series, so they add. Read once
// per publication, never per block.
int chainLatency(const RtChain* c) {
    if (!c) return 0;
    const int cnt = c->count < kMaxChainFx ? c->count : kMaxChainFx;
    int lat = 0;
    for (int i = 0; i < cnt; ++i)
        if (const PluginInstance* fx = c->fx[i]) {
            const int l = fx->latencyFrames();
            if (l > 0) lat += l;                    // a negative report is a lie
        }
    return lat < kPdcCap ? lat : kPdcCap - 1;
}

// Delays one channel in place by `d` frames. The ring is written first and read
// `d` behind, so d == 0 reads back the very sample just written and is an exact
// passthrough — that is what lets the zero-compensation case share this code
// path without changing a single output sample. Frames older than the line has
// been in service read as silence rather than as whatever the ring held from
// before, which is what keeps a line that has just come back into use from
// replaying ancient audio.
void pdcDelayChan(f32* ring, f32* buf, int n, int wpos, int d, int filled) {
    for (int i = 0; i < n; ++i) {
        const int w = (wpos + i) & kPdcMask;
        ring[w] = buf[i];
        buf[i]  = (filled + i >= d) ? ring[(w - d) & kPdcMask] : 0.f;
    }
}

void pdcDelay(Pdc& p, int lineIdx, f32* l, f32* r, int n, int d) {
    if (d < 0) d = 0;
    if (d > kPdcMask) d = kPdcMask;
    pdcDelayChan(p.line(lineIdx, 0), l, n, p.wpos, d, p.filled);
    pdcDelayChan(p.line(lineIdx, 1), r, n, p.wpos, d, p.filled);
}

// A line whose path produced nothing this block still has to be fed, or the
// silence would never travel down it and the gap would come out as a repeat of
// whatever the ring held. Costs one memset per channel per idle path.
void pdcFlush(Pdc& p, int lineIdx, int n) {
    for (int ch = 0; ch < 2; ++ch) {
        f32* ring = p.line(lineIdx, ch);
        const int head = p.wpos;
        const int first = (head + n <= kPdcCap) ? n : (kPdcCap - head);
        std::memset(ring + head, 0, (size_t)first * sizeof(f32));
        if (first < n) std::memset(ring, 0, (size_t)(n - first) * sizeof(f32));
    }
}

} // namespace

// Track::fireBeat does double duty, and this is the one place to look for why.
// While something is queued (Track::queued != -2) it is the beat that queued
// action fires on, exactly as before. While nothing is queued and a clip is
// playing it holds the beat that clip's *follow action* comes due on, or
// kNoFollow when it has none. The two never overlap: queuing anything (a user
// launch, a scene, a stop) supersedes a pending follow, which is the behaviour
// we want anyway. engine.h is frozen, so a dedicated member was not an option.
void Engine::prepare(f64 sampleRate, int /*maxBlock*/) {
    sr_ = sampleRate;
    for (int t = 0; t < kMaxTracks; ++t) {
        tracks_[t] = Track{};
        tracks_[t].fireBeat = kNoFollow;
        activeSlot[t].store(-1);
        pendingSlot[t].store(-2);
        slotState[t].store((int)SlotState::Stopped);
        clipPhase[t].store(0.0);
        meterL[t].store(0.f); meterR[t].store(0.f);
        recState[t].store(0);
        recSlotIdx[t].store(-1);
        for (int s = 0; s < kMaxScenes; ++s) clips_[t][s] = RtClip{};
    }
    // Return buses reset with the tracks. A re-prepare already drops every
    // track's chain on the floor without retiring it (the GUI is expected to
    // republish after a rate change, and there is no audio thread running at
    // this point to be inside one), so the returns and the master follow the
    // same rule rather than inventing a second one.
    for (int r = 0; r < kMaxReturns; ++r) {
        returns_[r] = Return{};
        returnMeterL[r].store(0.f);
        returnMeterR[r].store(0.f);
    }
    masterChain_ = nullptr;

    // Delay compensation storage. This is the one allocation the engine makes,
    // and it is made here for exactly that reason: prepare() is GUI-thread and
    // runs before the audio thread starts (a sample-rate change re-prepares
    // under the same rule), so process() never has to.
    if (Pdc* p = pdcAcquire(this)) p->reset();
    else LOGW("pdc: delay compensation unavailable, latent chains will not be aligned");
    latencyFrames.store(0);

    // Claim (and clear) the parking buffer for resilient critical events. Same
    // discipline as the PDC state: GUI thread, before the audio thread exists.
    if (!pendAcquire(this))
        LOGW("engine: no slot for resilient events; a full ring may drop a take");

    // Automation state, on the same discipline again. Without it the engine
    // simply applies no envelopes, which is the correct degradation: the sound
    // is the un-automated one rather than a crash or a stuck parameter.
    if (!autoAcquire(this))
        LOGW("engine: no slot for automation state; clip envelopes will not apply");

    LOGI("engine prepared @ %.0f Hz", sr_);
}

f64 Engine::nextQuantum(f64 fromBeat, int qIdx) const {
    const int idx = (qIdx < 0) ? quantum_ : qIdx;
    if (idx <= 0 || idx >= kQuantumCount) return fromBeat;
    const f64 q = kQuantumBeats[idx];
    if (q <= 0.0) return fromBeat;
    return std::ceil(fromBeat / q - kEps) * q;
}

void Engine::startVoice(Track& t, const RtClip& c) {
    // Hand the outgoing clip to the release slot so a same-track switch
    // crossfades instead of hard-cutting mid-waveform.
    if (t.voice.active) {
        // Anything already fading there is about to be overwritten, and a MIDI
        // voice's note-offs would go with it. Frame 0 because a launch boundary
        // is the caller's business and the alternative is a stuck note. In
        // practice this never fires: renderRange retires a releasing MIDI voice
        // in the very sub-block it is marked in, and one always runs between
        // two launches on the same track.
        if (t.prev.active && t.prev.clip && t.prev.clip->isMidi) flushOffs(t, t.prev, 0);
        t.prev = t.voice;
        t.prev.releasing = true;
    }
    Voice& v = t.voice;
    v.clip   = &c;
    v.active = true;
    v.srcPos = (f64)c.loopStart;
    v.readA  = v.srcPos;
    v.readB  = v.srcPos;
    v.phase  = 0;
    v.env    = 0.f;
    v.releasing = false;
    // MIDI position. Reset for every clip, audio included: a slot whose clip is
    // swapped from audio to MIDI must not inherit a stale cursor, and a MIDI
    // clip needs nothing else — there is no audio state to prime.
    v.beatPos  = 0.0;
    v.nextNote = 0;
    for (auto& o : v.offs) o.used = false;
    // Grain hop of one 1/16 note keeps transients intact, which is what makes
    // Beats-mode warping sound like a beat repeat rather than a smear.
    const f64 sixteenth = (60.0 / tempo_) * 0.25 * sr_;
    v.hop = (int)clampv(sixteenth, 512.0, 16384.0);
}

void Engine::drainCommands() {
    Command c;
    Pdc* pdc = pdcFind(this);
    AutoState* aut = autoFind(this);
    // Retires a voice that is losing the clip under it. Note-offs first: the
    // array it reads is about to go away and a release ramp it cannot hear will
    // not deliver them for us. Frame 0 because a GUI edit has no grid line of
    // its own, so the earliest possible frame is the least wrong one.
    auto dropVoice = [&](Track& t, Voice& v, int ti, bool primary, const RtClip* target) {
        if (!v.active || v.clip != target) return;
        const bool wasMidi = v.clip->isMidi;
        flushOffs(t, v, 0);
        if (!wasMidi) return;                  // audio still gets its release ramp
        v.active = false;
        v.clip = nullptr;
        v.releasing = false;
        if (primary) evts_.push({Ev::ClipStopped, ti, 0, 0.0});
    };

    while (cmds_.pop(c)) {
        switch (c.type) {
        case Cmd::SetPlaying:
            if (!c.a) {
                // Takes close against the beat we stopped on, so grab it before
                // the transport rewinds.
                const f64 stopBeat = beat_;
                playing_ = false;
                beat_ = 0.0;
                for (int ti = 0; ti < kMaxTracks; ++ti) {
                    Track& t = tracks_[ti];
                    if (t.voice.active) t.voice.releasing = true;
                    if (t.prev.active)  t.prev.releasing = true;
                    t.playing = -1; t.queued = -2;
                    t.fireBeat = kNoFollow;
                    // Recording needs the clock, so stopping the transport ends
                    // any take on the spot rather than at some boundary that is
                    // never going to arrive. The GUI still gets whatever was
                    // captured; a short take beats a lost one.
                    t.pendBuf = nullptr; t.pendCap = 0;
                    t.pendSlot = -1; t.pendMidi = false;
                    if (t.recPhase == 2 || t.recPhase == 3) {
                        // An overdub pass closes its held notes against where
                        // the clip *was*, not against the take's own elapsed
                        // beats: drainCommands runs at the top of the block, so
                        // the voice's beatPos is still the position the stop
                        // lands on.
                        const RtClip* oc = overdubVoice(clips_[ti], t);
                        if (oc) finishRec(this, ti, t, evts_, t.voice.beatPos, oc->lengthBeats);
                        else    finishRec(this, ti, t, evts_, stopBeat - t.recStartBeat);
                    } else if (t.recPhase == 1) cancelRec(t);
                }
                evts_.push({Ev::TransportStopped, 0, 0, 0.0});
            } else if (!playing_) {
                playing_ = true;
                beat_ = 0.0;
            }
            break;
        case Cmd::SetTempo:     tempo_ = clampv(c.x, 20.0, 999.0); break;
        case Cmd::SetQuantum:   quantum_ = clampv(c.a, 0, kQuantumCount - 1); break;
        case Cmd::SetMetronome: metronome_ = c.a != 0; break;

        case Cmd::LaunchClip: {
            if (c.a < 0 || c.a >= kMaxTracks || c.b < 0 || c.b >= kMaxScenes) break;
            const RtClip& cl = clips_[c.a][c.b];
            if (!cl.valid) break;
            if (!playing_) { playing_ = true; beat_ = 0.0; }
            Track& t = tracks_[c.a];
            t.queued = c.b;
            t.fireBeat = nextQuantum(beat_, cl.quantumIdx);
            break;
        }
        case Cmd::StopTrack: {
            if (c.a < 0 || c.a >= kMaxTracks) break;
            Track& t = tracks_[c.a];
            // Stopping a track is also how you end a take on it, so this runs
            // before the "nothing to stop" bail-out below.
            if (t.recPhase == 1) {
                cancelRec(t);
            } else if (t.recPhase == 2) {
                t.recPhase = 3;
                t.recFireBeat = nextQuantum(beat_, -1);
            }
            if (t.playing < 0 && t.queued == -2) break;
            t.queued = -1;
            t.fireBeat = nextQuantum(beat_, -1);
            break;
        }
        case Cmd::LaunchScene: {
            if (c.a < 0 || c.a >= kMaxScenes) break;
            if (!playing_) { playing_ = true; beat_ = 0.0; }
            const f64 fire = nextQuantum(beat_, -1);
            for (int ti = 0; ti < kMaxTracks; ++ti) {
                Track& t = tracks_[ti];
                const RtClip& cl = clips_[ti][c.a];
                // An empty slot in the scene stops that track, matching Live.
                if (cl.valid)                            { t.queued = c.a; t.fireBeat = fire; }
                else if (t.playing >= 0 || t.queued >= 0) { t.queued = -1;  t.fireBeat = fire; }
            }
            break;
        }
        case Cmd::StopAll: {
            const f64 fire = nextQuantum(beat_, -1);
            for (auto& t : tracks_)
                if (t.playing >= 0 || t.queued >= 0) { t.queued = -1; t.fireBeat = fire; }
            break;
        }

        // A repush from the piano roll lands on a clip that may be sounding
        // right now, and the array under it is GUI-owned memory the audio
        // thread must never free. So: offs out first (the notes the voice is
        // holding belong to the array being replaced), then the swap, then the
        // read cursor re-seeks into the new array, then the old pointer rides
        // an Ev::NotesRetired back to the GUI, which is the only side allowed
        // to release it — and only once this event proves we are out of it.
        case Cmd::SetClip:
            if (c.a >= 0 && c.a < kMaxTracks && c.b >= 0 && c.b < kMaxScenes) {
                Track& t = tracks_[c.a];
                RtClip& dst = clips_[c.a][c.b];
                const RtNote* old = dst.notes;
                const bool changed = old && old != c.clip.notes;
                // An envelope set rides the same protocol, for the same reason:
                // it can be edited, and recorded into, while the clip plays. The
                // "only when it differs" condition is publishNotes' — an entry
                // that would never be announced must not be queued — and the
                // event is critical because a lost one leaks GUI memory with no
                // second channel to notice it by.
                const RtAutoSet* oldAutos = dst.autos;
                const bool autosChanged = oldAutos && oldAutos != c.clip.autos;
                if (changed) {
                    if (t.voice.clip == &dst && t.voice.active) flushOffs(t, t.voice, 0);
                    if (t.prev.clip  == &dst && t.prev.active)  flushOffs(t, t.prev,  0);
                }
                dst = c.clip;
                if (t.voice.clip == &dst && t.voice.active) reseekNotes(t.voice, dst);
                if (t.prev.clip  == &dst && t.prev.active)  reseekNotes(t.prev,  dst);
                if (changed) emitCritical(this, evts_, {Ev::NotesRetired, c.a, c.b, 0.0, (void*)old});
                if (autosChanged)
                    emitCritical(this, evts_, {Ev::AutosRetired, c.a, c.b, 0.0, (void*)oldAutos});
            }
            break;
        // A pointer swap and nothing else. The audio thread must never free a
        // chain or a PluginInstance, so the displaced chain rides an event back
        // to the GUI, which owns the memory and is the only side allowed to
        // release it — and only once this event proves we are no longer in it.
        case Cmd::SetChain: {
            if (c.a < 0 || c.a >= kMaxTracks) break;
            Track& t = tracks_[c.a];
            const RtChain* old = t.chain;
            // Restore BEFORE the pointer moves (§3.5). After the swap the
            // instance a hold names may be one the engine no longer references,
            // and writing the captured value into it would be a write through a
            // pointer the GUI is about to free.
            if (aut) autoRestore(aut->t[c.a], old, nullptr);
            t.chain = (const RtChain*)c.p;
            // The one place a chain's latency is read. It is const after
            // prepare() per the PluginInstance contract, so the cached copy is
            // good until the chain is replaced — and replacing it comes through
            // here. Compensation may change, which the block below resolves.
            if (pdc) { pdc->trackLat[c.a] = chainLatency(t.chain); pdc->dirty = true; }
            if (old) emitCritical(this, evts_, {Ev::ChainRetired, c.a, 0, 0.0, (void*)old});
            break;
        }

        // The return buses and the master, on the same protocol: a pointer swap
        // and an event carrying the displaced chain home. The `a` field says
        // which bus it came off — kMaxTracks + index for a return, -1 for the
        // master — so one event type covers all three kinds of chain without the
        // GUI having to guess.
        case Cmd::SetReturnChain: {
            if (c.a < 0 || c.a >= kMaxReturns) break;
            Return& rt = returns_[c.a];
            const RtChain* old = rt.chain;
            rt.chain = (const RtChain*)c.p;
            if (pdc) { pdc->retLat[c.a] = chainLatency(rt.chain); pdc->dirty = true; }
            if (old) emitCritical(this, evts_, {Ev::ChainRetired, kMaxTracks + c.a, 0, 0.0, (void*)old});
            break;
        }
        case Cmd::SetMasterChain: {
            const RtChain* old = masterChain_;
            masterChain_ = (const RtChain*)c.p;
            if (pdc) { pdc->masterLat = chainLatency(masterChain_); pdc->dirty = true; }
            if (old) emitCritical(this, evts_, {Ev::ChainRetired, -1, 0, 0.0, (void*)old});
            break;
        }
        // Both ends checked, as everywhere else here: a stray index would have
        // the audio thread writing outside the mixer. See busGain for why these
        // two are the only levels in the engine that are sanitised.
        case Cmd::SendLevel:
            if (c.a >= 0 && c.a < kMaxTracks && c.b >= 0 && c.b < kMaxReturns)
                tracks_[c.a].send[c.b] = busGain(c.x);
            break;
        case Cmd::ReturnVol:
            if (c.a >= 0 && c.a < kMaxReturns) returns_[c.a].vol = busGain(c.x);
            break;

        case Cmd::ClearClip:
            if (c.a >= 0 && c.a < kMaxTracks && c.b >= 0 && c.b < kMaxScenes) {
                Track& t = tracks_[c.a];
                RtClip& dst = clips_[c.a][c.b];
                const RtNote* old = dst.notes;
                const RtAutoSet* oldAutos = dst.autos;
                if (t.playing == c.b) { t.voice.releasing = true; t.playing = -1;
                                        t.fireBeat = kNoFollow; }
                if (t.queued  == c.b) { t.queued = -2; t.fireBeat = kNoFollow; }
                // An audio voice keeps its release ramp over the now-empty clip
                // (fetch() reads silence out of it); a MIDI voice has nothing to
                // fade and everything to hand back, so it ends here.
                dropVoice(t, t.prev,  c.a, false, &dst);
                dropVoice(t, t.voice, c.a, true,  &dst);
                dst = RtClip{};
                if (old) emitCritical(this, evts_, {Ev::NotesRetired, c.a, c.b, 0.0, (void*)old});
                // The cleared slot's incoming `autos` is null, so "differs from
                // the incoming one" is simply "there was one".
                if (oldAutos)
                    emitCritical(this, evts_, {Ev::AutosRetired, c.a, c.b, 0.0, (void*)oldAutos});
            }
            break;

        // Toggle protocol, per the contract in engine.h. Everything here only
        // *schedules*; the phase changes themselves happen on the grid line in
        // fireDue(), so a take always starts and ends in time.
        //
        // Audio and MIDI takes share every bit of this state machine — the only
        // difference is the recMidi flag, which decides what gets written into
        // the buffer and which event carries it home. Toggling and hand-over
        // ignore the kind on purpose: a second send to the slot that is
        // recording means "stop", whichever button the user pressed.
        case Cmd::RecordSlot:
        case Cmd::RecordMidiSlot: {
            if (c.a < 0 || c.a >= kMaxTracks || c.b < 0 || c.b >= kMaxScenes) break;
            Track& t   = tracks_[c.a];
            f32*   buf = (f32*)c.p;
            const i64  cap  = (i64)c.x;        // frames, or NOTES for a MIDI take
            const bool midi = (c.type == Cmd::RecordMidiSlot);

            if (t.recPhase == 0) {
                if (!buf || cap <= 0) break;
                // A take needs a running clock. Arm the transport exactly the
                // way LaunchClip does so the first grid line is beat 0.
                if (!playing_) { playing_ = true; beat_ = 0.0; }
                t.recBuf = buf; t.recCap = cap; t.recLen = 0;
                t.recSlot = c.b; t.recPhase = 1; t.recMidi = midi;
                t.recFireBeat = nextQuantum(beat_, -1);
            } else if (t.recSlot == c.b) {
                // Toggling a take that has not begun cancels it. There is no
                // buffer to hand back, so no event goes out either.
                if (t.recPhase == 1) {
                    cancelRec(t);
                } else if (t.recPhase == 2) {
                    t.recPhase = 3;
                    t.recFireBeat = nextQuantum(beat_, -1);
                }
                // phase 3: a stop is already on the grid, nothing to add.
            } else {
                if (!buf || cap <= 0) break;
                if (t.recPhase == 1) {
                    // Nothing captured yet, so this is just a retarget.
                    t.recBuf = buf; t.recCap = cap; t.recSlot = c.b; t.recMidi = midi;
                    t.recFireBeat = nextQuantum(beat_, -1);
                } else {
                    // Hand-over: the running take ends on the same grid line
                    // the new one begins, so the two are gapless and both land
                    // on the beat. Track carries one set of recording fields, so
                    // the incoming request waits in pend* until the boundary.
                    t.recPhase = 3;
                    t.recFireBeat = nextQuantum(beat_, -1);
                    t.pendBuf = buf; t.pendCap = cap;
                    t.pendSlot = c.b; t.pendMidi = midi;
                }
            }
            break;
        }

        // Both ends are checked: a negative index here would write out of
        // bounds on the audio thread, which is not survivable.
        default: {
            const bool trackOk = c.a >= 0 && c.a < kMaxTracks;
            const bool slotOk  = trackOk && c.b >= 0 && c.b < kMaxScenes;
            switch (c.type) {
            case Cmd::TrackVol:  if (trackOk) tracks_[c.a].vol  = (f32)c.x; break;
            case Cmd::TrackPan:  if (trackOk) tracks_[c.a].pan  = (f32)clampv(c.x, -1.0, 1.0); break;
            case Cmd::TrackMute: if (trackOk) tracks_[c.a].mute = c.b != 0; break;
            case Cmd::TrackSolo: if (trackOk) tracks_[c.a].solo = c.b != 0; break;
            case Cmd::TrackArm:  if (trackOk) tracks_[c.a].arm  = c.b != 0; break;
            case Cmd::MasterVol: masterVol_ = (f32)c.x; break;
            case Cmd::ClipGain:  if (slotOk) clips_[c.a][c.b].gain = (f32)c.x; break;
            case Cmd::ClipWarp:  if (slotOk) clips_[c.a][c.b].warp = (int)c.x; break;
            case Cmd::ClipLoop:  if (slotOk) clips_[c.a][c.b].loop = c.x != 0.0; break;
            default: break;
            }
            break;
        }
        }
    }

    // Compensation depends on the *maxima* across the mixer, so a single chain
    // swap can move every other path. Resolving it once here rather than per
    // command keeps a scene's worth of chain pushes to one recompute, and keeps
    // the per-block path free of it entirely.
    //
    // The delay amounts change under running audio, which is a click: the lines
    // keep their contents and the read cursor simply jumps. That is the accepted
    // cost of inserting a latent device while playing (Live glitches here too);
    // what would be worse is a memset of 20 MB on the audio thread. Going from
    // no compensation at all to some is the one case that cannot jump, because
    // the lines have been out of service and hold whatever they last held — so
    // that transition restarts `filled` and the lines read silence until they
    // have been refilled honestly.
    if (pdc && pdc->dirty) {
        int mt = 0, mr = 0;
        for (int ti = 0; ti < kMaxTracks; ++ti) if (pdc->trackLat[ti] > mt) mt = pdc->trackLat[ti];
        for (int r = 0; r < kMaxReturns; ++r)   if (pdc->retLat[r]   > mr) mr = pdc->retLat[r];
        const bool was = pdc->active;
        pdc->maxTrackLat = mt;
        pdc->maxRetLat   = mr;
        pdc->active      = (mt > 0 || mr > 0);
        if (pdc->active && !was) pdc->filled = 0;
        pdc->dirty = false;
    }

    // Last, and after everything above has landed: this counter is what proves
    // to the other side that a command it pushed has been consumed, so it must
    // not be observable before the effects it vouches for. Release for the same
    // reason — the state writes above have to be visible to anyone who sees the
    // new value.
    drains.fetch_add(1, std::memory_order_release);
}

void Engine::fireDue(f64 atBeat) {
    // A take whose target slot already holds a playable MIDI clip is a looper
    // pass, and a pass needs something to lap over: the record boundary is
    // therefore also that clip's launch boundary. It goes through startVoice()
    // rather than poking the voice directly so every downstream detail stays
    // uniform with an ordinary launch — the outgoing clip's note-offs, the
    // beatPos reset, clipPhase, Ev::ClipStarted, the follow timer. That is what
    // keeps this three lines instead of thirty, and it is why the GUI needs no
    // special case for a clip that started because you hit record.
    //
    // A clip that is *already* the voice on this track is left strictly alone.
    // Restarting it would be the wrong musical answer: hitting record on a loop
    // you are listening to should drop you into the lap that is running, at the
    // position you are hearing it, the way a hardware looper does — the take
    // joins in progress. (It is also precisely why the wrap origin cannot be
    // the take's start beat: a pass joined mid-loop is offset from it, and after
    // the first wrap the two are a whole lap apart.)
    auto armOverdub = [&](int ti, Track& t, int slot, bool midi, f64 sched) {
        const RtClip* c = overdubSlot(clips_[ti], slot, midi);
        if (!c) return;
        if (t.voice.active && t.voice.clip == c) return;   // joins in progress
        // The user's own launch of this same clip is already due on this same
        // grid line (record and launch pressed together, say). Step 3 below
        // will fire it in this very pass; doing it here as well would start the
        // voice twice and report two ClipStarted for one launch.
        if (t.queued == slot && t.fireBeat <= atBeat + kEps) return;
        startVoice(t, *c);
        t.playing = slot;
        // fireBeat is the queued action's beat for as long as something is
        // queued (see the note above prepare()); only claim it for the follow
        // timer when nothing is, or this launch would eat that queued action.
        if (t.queued == -2) t.fireBeat = followDueBeat(*c, sched);
        evts_.push({Ev::ClipStarted, ti, slot, atBeat});
    };

    // 1. Recording boundaries. Independent of clip scheduling, but on the same
    //    grid, so they are resolved in the same sub-block pass.
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (t.recPhase != 1 && t.recPhase != 3) continue;
        if (t.recFireBeat > atBeat + kEps) continue;

        if (t.recPhase == 1) {
            t.recPhase = 2;
            t.recLen = 0;
            // The *scheduled* beat, not the sub-block one: they differ by a
            // fraction of a frame and the GUI wants the grid line. A MIDI take
            // stamps its notes against it, so it is also the take's beat zero —
            // for an overdub pass the clip's own loop takes that job instead,
            // but the event still reports the grid line either way.
            t.recStartBeat = t.recFireBeat;
            for (auto& o : t.recOpen) o.used = false;
            evts_.push({Ev::RecordStarted, ti, t.recSlot, t.recFireBeat});
            armOverdub(ti, t, t.recSlot, t.recMidi, t.recFireBeat);
        } else {
            const f64 boundary = t.recFireBeat;
            // fireDue runs at the head of a sub-block, before renderRange has
            // moved anything, so the voice's beatPos is exactly the boundary's
            // position inside the loop — what an overdub's held notes close
            // against. The clip keeps playing; only the take ends here.
            const RtClip* oc = overdubVoice(clips_[ti], t);
            if (oc) finishRec(this, ti, t, evts_, t.voice.beatPos, oc->lengthBeats);
            else    finishRec(this, ti, t, evts_, boundary - t.recStartBeat);
            // A take displaced by a Record*Slot into another slot hands over
            // here, on the very same grid line it stopped on.
            if (t.pendBuf) {
                t.recBuf = t.pendBuf; t.recCap = t.pendCap; t.recLen = 0;
                t.recSlot = t.pendSlot; t.recPhase = 2; t.recFireBeat = boundary;
                t.recMidi = t.pendMidi; t.recStartBeat = boundary;
                for (auto& o : t.recOpen) o.used = false;
                t.pendBuf = nullptr; t.pendCap = 0;
                t.pendSlot = -1; t.pendMidi = false;
                evts_.push({Ev::RecordStarted, ti, t.recSlot, boundary});
                armOverdub(ti, t, t.recSlot, t.recMidi, boundary);
            }
        }
    }

    // 2. Follow actions. A due follow *schedules* like any user launch — same
    //    quantum, same probability gate — rather than switching clips itself,
    //    which is what keeps a chain of follows locked to the grid.
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (t.queued != -2) continue;             // a queued action supersedes
        if (t.playing < 0 || t.playing >= kMaxScenes) continue;
        if (t.fireBeat >= kNoFollow || t.fireBeat > atBeat + kEps) continue;

        // Quantize from the exact due beat rather than from the sub-block beat
        // we happened to notice it on: with quantum None a chain of Again
        // follows would otherwise creep forward by a fraction of a frame per
        // repeat and slowly walk off the grid.
        const f64 due = t.fireBeat;
        const RtClip& cur = clips_[ti][t.playing];
        const int action = cur.followAction;
        if (action == (int)Follow::Stop) {
            t.queued = -1;
            t.fireBeat = nextQuantum(due, -1);
            continue;
        }
        const int target = followTarget(clips_[ti], t.playing, action, ti, due);
        if (target < 0) {
            t.fireBeat = kNoFollow;               // nowhere to go: stop asking
            continue;
        }
        t.queued = target;
        t.fireBeat = nextQuantum(due, clips_[ti][target].quantumIdx);
    }

    // 3. Queued launches and stops.
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (t.queued == -2) continue;
        if (t.fireBeat > atBeat + kEps) continue;

        // The beat the action was scheduled for, not the sub-block beat we
        // noticed it on. Probability rolls and follow timers both key off this
        // so they stay independent of the buffer size.
        const f64 sched = t.fireBeat;

        if (t.queued == -1) {
            if (t.voice.active) t.voice.releasing = true;
            t.playing = -1;
            t.fireBeat = kNoFollow;
            evts_.push({Ev::TrackStopped, ti, 0, atBeat});
        } else {
            const RtClip& cl = clips_[ti][t.queued];
            if (!cl.valid) {
                if (t.voice.active) t.voice.releasing = true;
                t.playing = -1;
                t.fireBeat = kNoFollow;
            } else if (!rollLaunch(cl, ti, t.queued, sched)) {
                // A failed roll is a no-op, not a stop: whatever is playing
                // keeps playing. Restart that clip's follow timer from this
                // grid line so the dice are rolled again next time round
                // instead of the track going quiet for good.
                t.fireBeat = (t.playing >= 0 && clips_[ti][t.playing].valid)
                                 ? followDueBeat(clips_[ti][t.playing], sched)
                                 : kNoFollow;
                t.queued = -2;
                continue;
            } else {
                startVoice(t, cl);
                t.playing = t.queued;
                t.fireBeat = followDueBeat(cl, sched);
                evts_.push({Ev::ClipStarted, ti, t.queued, atBeat});
            }
        }
        t.queued = -2;
    }
}

// Renders voices into each track's pre-fader scratch for the sub-range
// [from, to). Only clip gain and the declick envelope are applied here: volume,
// pan and mute/solo sit *after* the device chain, and the chain runs once over
// the whole block, so those stages cannot live in this per-sub-block path.
// The metronome is the one thing that goes straight to the master, since it is
// not on any track and must not be coloured by a track's plugins.
void Engine::renderRange(f32* outL, f32* outR, int from, int to) {
    if (to <= from) return;
    const f64 bps = tempo_ / 60.0 / sr_;

    // Ramp lengths for click-free starts and stops.
    const f32 attack  = 1.f / (f32)std::max(1.0, 0.003 * sr_);
    const f32 release = 1.f / (f32)std::max(1.0, 0.006 * sr_);

    // MIDI clips carry no audio at all. "Rendering" one means handing its notes
    // to the track's note-capable devices with sample-accurate frame offsets,
    // which is exactly why it lives here: renderRange runs for every sub-block
    // *before* the chain processes the block, so a note is always in before the
    // audio it is meant to produce, alongside the live-input forwarding. The
    // declick envelope does not apply — there is no waveform to fade, and a MIDI
    // voice's whole "release" is its note-offs. Everything else (ClipStarted /
    // ClipStopped, slotState, clipPhase) is deliberately identical to an audio
    // clip so the UI needs no special case anywhere.
    auto renderMidiVoice = [&](Track& t, Voice& v, int ti, bool primary) {
        const RtClip& c = *v.clip;
        // A clip shorter than a 1/64 note is not music, it is a bad edit; it is
        // also what would turn the lap loop below into a spin, so it ends here.
        const f64 L = c.lengthBeats > kMinLoopBeats ? c.lengthBeats : 0.0;

        // Stop, switch, transport stop: deliver what is owed and die on the
        // spot rather than after a ramp that would carry no sound anyway.
        if (v.releasing || L <= 0.0) {
            flushOffs(t, v, from);
            v.active = false;
            v.clip = nullptr;
            if (primary) evts_.push({Ev::ClipStopped, ti, 0, 0.0});
            return;
        }

        // Fires every note-on and every note-off owed in [beatPos, beatPos+span)
        // in beat order. `base` is the (fractional) frame beatPos sits on, kept
        // in f64 so a wrap part-way through a block does not round the rest of
        // the block off the grid.
        auto emit = [&](f64 base, f64 span) {
            const f64 b0 = v.beatPos, b1 = v.beatPos + span;
            // Nearest frame, not the one below it: the beat clock accumulates
            // per sub-block, so a note that is mathematically dead on the beat
            // arrives as 23999.9999998 and truncation would put every downbeat
            // one sample early. Rounding is also the smaller error either way.
            auto frameAt = [&](f64 b) {
                return clampv((int)std::lround(base + (b - b0) / bps), from, to - 1);
            };
            for (;;) {
                // Earliest note-off owed inside the span. The array is 32 long
                // and unsorted, so this is a bounded scan per event.
                int oi = -1;
                f64 ob = 1e300;
                for (int k = 0; k < 32; ++k)
                    if (v.offs[k].used && v.offs[k].beat < b1 && v.offs[k].beat < ob) {
                        ob = v.offs[k].beat; oi = k;
                    }
                // Notes are sorted by beat, so the next one is the next index.
                const bool haveNote = c.notes && v.nextNote < c.noteCount &&
                                      c.notes[v.nextNote].beat < b1;
                if (oi < 0 && !haveNote) break;

                // A tie goes to the note-off: re-triggering a pitch the
                // instrument is still holding has to release it first.
                if (oi >= 0 && (!haveNote || ob <= c.notes[v.nextNote].beat)) {
                    sendNote(t, 0x80, v.offs[oi].pitch, 0, frameAt(ob));
                    v.offs[oi].used = false;
                    continue;
                }

                const RtNote& nt = c.notes[v.nextNote++];
                const int fr = frameAt(nt.beat);
                // Same pitch still sounding from an overlapping note: off first,
                // for the same reason.
                for (auto& o : v.offs)
                    if (o.used && o.pitch == nt.pitch) {
                        sendNote(t, 0x80, o.pitch, 0, fr);
                        o.used = false;
                    }
                // Velocity 0 on a note-on *is* a note-off on the wire, so a
                // silent note in the clip would hang the previous one.
                sendNote(t, 0x90, nt.pitch, nt.vel ? nt.vel : (u8)1, fr);

                // Park the off. beat+len may run past the loop end; the wrap
                // below walks it down one lap at a time, which is what lets a
                // note longer than the clip still end where it should.
                int slot = -1;
                for (int k = 0; k < 32; ++k) if (!v.offs[k].used) { slot = k; break; }
                if (slot < 0) {
                    // 32 notes sounding at once out of one slot is past anything
                    // musical; steal the one due first so nothing is left hung.
                    f64 bb = 1e300;
                    slot = 0;
                    for (int k = 0; k < 32; ++k)
                        if (v.offs[k].beat < bb) { bb = v.offs[k].beat; slot = k; }
                    sendNote(t, 0x80, v.offs[slot].pitch, 0, fr);
                }
                v.offs[slot].used  = true;
                v.offs[slot].beat  = nt.beat + (nt.len > 1e-9 ? nt.len : 1e-3);
                v.offs[slot].pitch = nt.pitch;
            }
        };

        // MIDI clips ignore warp: they are already in beats, so the block's
        // beat span is the whole of it.
        f64 remain = (f64)(to - from) * bps;
        f64 base   = (f64)from;
        // Bounded by L >= 1/64 beat against a block of at most kMaxBlock frames;
        // the guard is there so a pathological rate cannot spin the audio thread.
        for (int lap = 0; remain > 1e-12 && lap < 4096; ++lap) {
            f64 seg = remain;
            bool wrapped = false;
            if (v.beatPos + seg >= L) { seg = L - v.beatPos; wrapped = true; }
            if (seg < 0.0) seg = 0.0;

            emit(base, seg);
            base      += seg / bps;
            v.beatPos += seg;
            remain    -= seg;
            if (!wrapped) break;

            if (!c.loop) {
                // A one-shot's tail is its note-offs and nothing else.
                flushOffs(t, v, clampv((int)std::lround(base), from, to - 1));
                v.active = false;
                v.clip = nullptr;
                v.releasing = true;
                if (primary) {
                    clipPhase[ti].store(1.0, std::memory_order_relaxed);
                    evts_.push({Ev::ClipStopped, ti, 0, 0.0});
                }
                return;
            }
            // A new lap. Note-offs still owed travel with it: their beat is
            // clip-relative, so it moves down by one loop length rather than
            // being dropped, which is what keeps a note straddling the wrap
            // from either hanging or firing twice.
            v.beatPos  = 0.0;
            v.nextNote = 0;
            for (auto& o : v.offs)
                if (o.used) { o.beat -= L; if (o.beat < 0.0) o.beat = 0.0; }
        }

        // Through voiceClipPhase so the playhead the UI draws and the beat an
        // envelope is evaluated against are the same quantity (§3.1).
        if (primary) clipPhase[ti].store(voiceClipPhase(v, c), std::memory_order_relaxed);
    };

    // Renders one voice into the track scratch. Called for the live voice and,
    // during a clip switch, for the outgoing one that is still fading out.
    auto renderVoice = [&](Track& t, Voice& v, int ti, bool primary) {
        if (!v.active || !v.clip) return;
        if (v.clip->isMidi) { renderMidiVoice(t, v, ti, primary); return; }

        const RtClip& c = *v.clip;

        // Fitting material recorded at clipBpm onto a grid running at tempo_
        // means consuming source frames at tempo_/clipBpm: a 120 BPM loop in a
        // 240 BPM set has to be read twice as fast to cover the same bar.
        const f64 rate = (c.warp == (int)Warp::Off) ? 1.0 : (tempo_ / c.clipBpm);
        const bool granular = (c.warp == (int)Warp::Beats) && std::fabs(rate - 1.0) > 1e-4;
        const f64 loopLen = (f64)(c.loopEnd - c.loopStart);

        for (int i = from; i < to; ++i) {
            f32 l, r;
            if (granular) {
                // Two-grain overlap-add with a complementary raised cosine.
                // Read heads run at natural speed; only the grain *origin*
                // moves at `rate`, so pitch is preserved.
                const f32 w  = 0.5f - 0.5f * std::cos((f32)(kPi * v.phase / (f64)v.hop));
                f32 aL, aR, bL, bR;
                fetch(c, v.readA, aL, aR);
                fetch(c, v.readB, bL, bR);
                l = aL * (1.f - w) + bL * w;
                r = aR * (1.f - w) + bR * w;
                v.readA += 1.0;
                v.readB += 1.0;
                if (++v.phase >= v.hop) { v.readA = v.readB; v.readB = v.srcPos; v.phase = 0; }
            } else {
                fetch(c, v.srcPos, l, r);
            }

            // Advance the musical position.
            v.srcPos += rate;
            if (v.srcPos >= (f64)c.loopEnd) {
                if (c.loop && loopLen > 0.0) {
                    v.srcPos = (f64)c.loopStart + std::fmod(v.srcPos - (f64)c.loopStart, loopLen);
                    if (!granular) { v.readA = v.srcPos; v.readB = v.srcPos; v.phase = 0; }
                } else {
                    v.releasing = true;
                }
            }

            // Declick envelope.
            if (v.releasing) { v.env -= release; if (v.env <= 0.f) { v.env = 0.f; } }
            else if (v.env < 1.f) { v.env += attack; if (v.env > 1.f) v.env = 1.f; }

            // Pre-fader: clip gain and declick only. Both voices of a track sum
            // into the same scratch, which is what makes the crossfade work.
            const f32 g = c.gain * v.env;
            t.fxL[i] += l * g;
            t.fxR[i] += r * g;
        }

        if (v.releasing && v.env <= 0.f) {
            v.active = false;
            v.clip = nullptr;
            if (primary) evts_.push({Ev::ClipStopped, ti, 0, 0.0});
        }
        // Only the live voice drives the UI progress bar; the fading one is
        // already off-screen as far as the grid is concerned.
        if (primary && loopLen > 0.0)
            clipPhase[ti].store(voiceClipPhase(v, c), std::memory_order_relaxed);
    };

    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        renderVoice(t, t.prev, ti, false);
        renderVoice(t, t.voice, ti, true);
    }

    // Metronome, rendered last so it sits on top of the mix.
    if (metronome_) {
        for (int i = from; i < to; ++i) {
            const f64 b = beat_ + (f64)i * bps;
            const i64 fl = (i64)std::floor(b);
            if (fl != (i64)std::floor(b - bps)) {
                metCountdown_ = (int)(0.03 * sr_);
                metPhase_ = 0.0;
                metFreq_ = (fl % sigNum_ == 0) ? 1600.f : 800.f;
            }
            if (metCountdown_ > 0) {
                const f32 decay = (f32)metCountdown_ / (f32)(0.03 * sr_);
                const f32 s = (f32)std::sin(metPhase_) * 0.25f * decay * decay;
                metPhase_ += 2.0 * kPi * metFreq_ / sr_;
                outL[i] += s; outR[i] += s;
                --metCountdown_;
            }
        }
    }
}

void Engine::process(const f32* inL, const f32* inR, f32* outL, f32* outR, int nframes) {
    const auto t0 = std::chrono::steady_clock::now();

    // Denormals in feedback/reverb tails and the multiplicative meter decays cost
    // orders of magnitude on the audio thread. MXCSR is per-thread state, so arm
    // FTZ/DAZ here on the first call on this thread rather than once at startup:
    // that covers the in-process app AND the daemon (which never armed it at all,
    // yet is where all plugin DSP now runs — RT-AUDIT §1.7). x86 only; on other
    // ISAs (e.g. AArch64 FPCR) this is a no-op and denormals remain enabled.
#if defined(__x86_64__) || defined(__i386__)
    static thread_local bool ftzArmed = false;
    if (!ftzArmed) {
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
        ftzArmed = true;
    }
#endif

    // A liveness heartbeat independent of transport: advances every callback so
    // the GUI/daemon can tell the audio thread is running even when stopped.
    blocksRendered.fetch_add(1, std::memory_order_relaxed);

    // Retry any critical events parked because the ring was full last block,
    // before anything else touches it, so they keep their order (RT-AUDIT §1.6).
    flushPendingEv(this, evts_);

    drainCommands();
    std::memset(outL, 0, (size_t)nframes * sizeof(f32));
    std::memset(outR, 0, (size_t)nframes * sizeof(f32));

    // The per-track scratch is sized kMaxBlock. Growing it here would mean
    // allocating on the audio thread, so an oversized block renders what fits
    // and leaves the remainder silent rather than running off the end.
    const int n = nframes < kMaxBlock ? nframes : kMaxBlock;
    if (n <= 0) { publish(); return; }

    // One MIDI drain for the whole block: every armed track sees the same list,
    // so pulling it per track would only cost the ring an extra pass. Anything
    // past the cap stays queued and arrives one block later.
    MidiMsg midi[kMidiPerBlock];
    int midiCount = 0;
    { MidiMsg m; while (midiCount < kMidiPerBlock && midi_.pop(m)) midi[midiCount++] = m; }
    // Second producer, second SPSC ring: the GUI (computer keyboard, note
    // preview) has its own so it never races the hardware reader's head pointer
    // — sharing one ring drops messages, and a lost note-off is a stuck note
    // (RT-AUDIT §1.2). Drain it into the same per-block list under the same cap.
    // No frame-order merge is needed: both rings are same-block, and every
    // consumer (captureMidiRange, the chain fan-out) already keys off m.frame.
    { MidiMsg m; while (midiCount < kMidiPerBlock && midiGui_.pop(m)) midi[midiCount++] = m; }

    // Decide up front which tracks take part in this block, and clear their
    // scratch before any voice writes into it. A track is live if it has audio
    // now, will have audio before the block ends (a launch is queued, or a
    // follow action is pending and may queue one part-way through this very
    // block), owns a chain, or is armed — a chain has to keep running on
    // silence so reverb tails and monitoring survive both the transport
    // stopping and the clip ending, and an armed track has to run so its input
    // is heard. Getting the follow case wrong here is silent: the scratch would
    // go uncleared and the whole track would be skipped for the block the
    // follow fires in.
    bool live[kMaxTracks];
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        // A queued MIDI take into a slot that holds a MIDI clip launches that
        // clip on its own boundary, which can fall part-way through this very
        // block — the same trap as the follow case above, and just as silent:
        // the track would be skipped for the block its clip started in, so the
        // instrument would never see that block and the notes handed to it
        // would be rendered a block late. Phase 3 counts too, because a
        // hand-over starts its successor on the same grid line it stops on.
        const bool recWillLaunch =
            (t.recPhase == 1 && overdubSlot(clips_[ti], t.recSlot, t.recMidi)) ||
            (t.recPhase == 3 && t.pendBuf &&
             overdubSlot(clips_[ti], t.pendSlot, t.pendMidi));
        live[ti] = t.voice.active || t.prev.active || t.queued != -2 || t.arm ||
                   (t.playing >= 0 && t.fireBeat < kNoFollow) || recWillLaunch ||
                   (t.chain && t.chain->count > 0);
        if (live[ti]) {
            std::memset(t.fxL, 0, (size_t)n * sizeof(f32));
            std::memset(t.fxR, 0, (size_t)n * sizeof(f32));
        }
    }

    // -----------------------------------------------------------------------
    // Automation pass (§3.3). One pass over the tracks, here and not later, for
    // two non-negotiable reasons:
    //
    //   1. Class B (device parameters) must reach the plugin BEFORE this
    //      block's process() runs — the same ordering constraint MIDI already
    //      has, and the reason this cannot live in the post stage.
    //   2. The block's end beat is computed FORWARD from its start beat, not
    //      read back after rendering. Reading the voice's position afterwards
    //      would fold the loop wrap into the ramp and produce a jump; computing
    //      it forward and clamping to the loop end makes the wrap a known case
    //      — the last block of a lap ramps to the envelope's end value and the
    //      next block starts from its start value, which is what a loop *is*.
    //
    // Only the primary voice drives envelopes. Track::prev — the clip fading
    // out across a switch — does not: two clips' envelopes fighting over one
    // gain across a 6 ms crossfade produce a value that is neither, and holding
    // the outgoing one's last applied value for the length of the fade is
    // correct. A track with no active primary voice applies nothing at all; its
    // scalars are whatever the user set, which is what makes §1's rule free.
    AutoBlock autoA[kMaxTracks];
    AutoState* aut = autoFind(this);

    // `launchOnly` is the second call, made after the sub-block loop for tracks
    // whose clip started part-way through this very block: they had no voice
    // when the first call ran, so without it a clip whose envelope begins at
    // silence would sound one block at the user's own fader — a click, and the
    // one thing the whole feature is supposed to make impossible. Those tracks
    // get the envelope's value at their FIRST beat, held constant for the
    // block: a few milliseconds early, inaudible inside the voice's 3 ms attack
    // ramp, and exactly what §3.3 documents. It still lands before the chain
    // runs, so class B keeps its ordering guarantee.
    auto autoPass = [&](bool launchOnly) {
        if (!aut) return;
        const f64 bps = playing_ ? (tempo_ / 60.0 / sr_) : 0.0;
        for (int ti = 0; ti < kMaxTracks; ++ti) {
            Track& t = tracks_[ti];
            AutoTrack& at = aut->t[ti];
            // Already applied above: at.set is non-null exactly when the first
            // call found a set for this track.
            if (launchOnly && at.set) continue;

            const RtClip* c = (t.voice.active && t.voice.clip) ? t.voice.clip : nullptr;
            const RtAutoSet* set =
                (c && c->autos && c->autos->laneCount > 0) ? c->autos : nullptr;

            // Every event that ends an application shows up here as a change of
            // set pointer: the voice stopped (null), the clip was cleared (its
            // autos went with it), the set was republished (a new pointer). One
            // condition, one restore, and the inert bitmap resets with it so
            // "once per published set" stays exact.
            if (at.set != set) {
                autoRestore(at, t.chain, set);
                at.set = set;
                at.inert = 0;
            }
            if (!set) continue;                  // the ordinary case, and free

            const f64 L  = c->lengthBeats;
            f64 b0 = 0.0, b1 = 0.0;
            if (!launchOnly) {
                b0 = voiceClipBeat(t.voice, *c);
                b1 = b0 + (f64)n * bps;
                if (L > 0.0 && b1 > L) b1 = L;   // clamp the ramp to the loop end
            }

            AutoBlock& ab = autoA[ti];
            const int lanes = set->laneCount < kMaxRtAutoLanes ? set->laneCount
                                                              : kMaxRtAutoLanes;
            for (int li = 0; li < lanes; ++li) {
                const RtAutoLane& l = set->lanes[li];
                // The user's hand and a lane the engine has given up on are
                // both "not applying". An empty lane is skipped rather than
                // evaluated against a fallback: the fallback for a class-A
                // target is the un-automated value, and not applying the lane
                // *is* that value, exactly and for free.
                if (l.flags & (kAutoOverridden | kAutoInert)) continue;
                if (at.inert & (1u << li)) continue;
                if (l.count <= 0) continue;

                switch ((AutoTarget)l.target) {
                case AutoTarget::TrackVol: {
                    // The ramp interpolates the DERIVED value. Interpolating
                    // the fader position and mapping per sample would be a pow
                    // and a log10 per sample, and would make the ramp's shape
                    // depend on the block size in a way this does not.
                    const f32 v0 = autoValueAt(*set, l, b0, 0.f);
                    const f32 v1 = autoValueAt(*set, l, b1, 0.f);
                    ab.vol0 = busGain((f64)(l.xform == (i32)AutoXform::Fader ? faderToGain(v0) : v0));
                    ab.vol1 = busGain((f64)(l.xform == (i32)AutoXform::Fader ? faderToGain(v1) : v1));
                    ab.hasVol = ab.any = true;
                    break;
                }
                case AutoTarget::TrackPan:
                    ab.pan0 = autoPan(autoValueAt(*set, l, b0, 0.f));
                    ab.pan1 = autoPan(autoValueAt(*set, l, b1, 0.f));
                    ab.hasPan = ab.any = true;
                    break;
                case AutoTarget::TrackSend: {
                    if (l.index < 0 || l.index >= kMaxReturns) break;
                    ab.snd0[l.index] = busGain((f64)autoValueAt(*set, l, b0, 0.f));
                    ab.snd1[l.index] = busGain((f64)autoValueAt(*set, l, b1, 0.f));
                    ab.sendMask |= 1u << l.index;
                    ab.any = true;
                    break;
                }
                case AutoTarget::DeviceParam: {
                    // Not ramped, because there is nothing to ramp: a plugin
                    // parameter is one value handed over once, and every
                    // backend's own smoothing is the plugin's business.
                    if (!t.chain || l.devSlot < 0 || l.devSlot >= kMaxChainFx ||
                        l.devSlot >= t.chain->count) break;
                    PluginInstance* fx = t.chain->fx[l.devSlot];
                    if (!fx || l.index < 0 || l.index >= fx->paramCount()) break;

                    // The one place §1's rule cannot hold, so the first write
                    // takes a copy of what it is destroying. getParam() is a
                    // plain load in every backend in the tree, which is what
                    // makes this safe here.
                    AutoTrack::Hold& h = at.hold[li];
                    if (!h.used) {
                        h.devSlot = l.devSlot;
                        h.param   = l.index;
                        h.was     = fx->getParam(l.index);
                        h.used    = true;
                        at.anyHold = true;
                    }

                    const f32 v = autoValueAt(*set, l, b0, 0.f);
                    if (!fx->setParamRT(l.index, v)) {
                        // This backend has no realtime parameter path. Say so
                        // once — a silently ignored lane is the worst outcome:
                        // the envelope is drawn, the sound does not move, and
                        // nothing says why — and never call again for this set.
                        // The hold is dropped without a write-back: nothing was
                        // ever applied, so there is nothing to undo.
                        at.inert |= 1u << li;
                        h.used = false; h.devSlot = -1; h.param = -1;
                        emitCritical(this, evts_,
                                     {Ev::AutoLaneInert, ti, t.playing, (f64)li});
                    }
                    break;
                }
                default: break;                  // None, and the reserved codes
                }
            }
        }
    };
    autoPass(false);

    // Appends [from, to) of the capture input to every take in progress. This
    // runs inside the sub-block loop so a take starts and ends on the exact
    // frame its grid line falls on, not merely on a block boundary. What lands
    // in the buffer is the raw input: monitoring is a listening path, the take
    // is the source, and putting the chain between them would bake the devices
    // into the recording.
    auto captureRange = [&](int from, int to) {
        for (int ti = 0; ti < kMaxTracks; ++ti) {
            Track& t = tracks_[ti];
            // Phase 3 is "stop queued", not "stopped": the take keeps taking
            // until the grid line actually arrives, which is the whole point of
            // quantizing the stop.
            if ((t.recPhase != 2 && t.recPhase != 3) || !t.recBuf) continue;
            if (t.recMidi) continue;             // notes are stamped, not sampled
            int i = from;
            for (; i < to && t.recLen < t.recCap; ++i) {
                const size_t o = (size_t)t.recLen * 2;
                t.recBuf[o]     = inL ? inL[i] : 0.f;
                t.recBuf[o + 1] = inR ? inR[i] : 0.f;
                ++t.recLen;
            }
            // The engine cannot grow a GUI-owned buffer and must not write past
            // it, so a full buffer ends the take here and now.
            if (t.recLen >= t.recCap) finishRec(this, ti, t, evts_, 0.0);
        }
    };

    // The MIDI half of the same job. Notes are stamped against the beat clock
    // rather than sampled, and pairing note-ons with note-offs is what turns a
    // stream of messages into clip material. It shares the sub-block loop with
    // the audio path for the same reason: a take must begin and end on the exact
    // frame its grid line falls on, not merely on a block boundary.
    auto captureMidiRange = [&](int from, int to) {
        if (midiCount == 0) return;
        const f64 bpf = tempo_ / 60.0 / sr_;
        for (int ti = 0; ti < kMaxTracks; ++ti) {
            Track& t = tracks_[ti];
            if ((t.recPhase != 2 && t.recPhase != 3) || !t.recMidi || !t.recBuf) continue;
            // Consistent with the live routing: an unarmed track is not
            // listening, so it has nothing to record either.
            if (!t.arm) continue;
            RtNote* notes = (RtNote*)t.recBuf;   // aliased per Cmd::RecordMidiSlot

            // The wrap origin of an overdub pass.
            //
            // What a note has to land on is its position in the *clip's* loop,
            // and the only thing that knows that is the voice. recStartBeat is
            // where the take began, which for a pass joined mid-loop is not
            // where the lap began, and after the first wrap the two are a whole
            // lap apart — stamping against it would smear every pass by its own
            // start offset. So the origin is beatPos, read fresh per event.
            //
            // renderRange has already walked the voice across [from, to) by the
            // time we get here (it runs first in the sub-block loop, so an
            // instrument sees a note before the audio it is meant to make), so
            // beatPos is the position at frame `to` and the position at frame
            // `fr` is that walked back the frames it ran ahead. Exact across
            // wraps: beatPos comes back already reduced into the loop and the
            // walk-back is reduced again below.
            const RtClip* oc = overdubVoice(clips_[ti], t);
            const f64 loopLen = oc ? oc->lengthBeats : 0.0;

            for (int mi = 0; mi < midiCount; ++mi) {
                const MidiMsg& m = midi[mi];
                const int fr = clampv((int)m.frame, 0, n - 1);
                if (fr < from || fr >= to) continue;
                const u8 hi = (u8)(m.status & 0xF0);
                if (hi != 0x90 && hi != 0x80) continue;
                f64 at;
                if (oc) {
                    at = wrapBeat(t.voice.beatPos - (f64)(to - fr) * bpf, loopLen);
                } else {
                    at = beat_ + (f64)fr * bpf - t.recStartBeat;
                    if (at < 0.0) at = 0.0;
                }
                const u8 pitch = (u8)(m.d1 & 0x7F);

                // Note-on with velocity 0 is a note-off; every source that
                // bothers with running status sends them that way.
                if (hi == 0x90 && m.d2 > 0) {
                    // A retrigger without an intervening off closes the old
                    // note rather than leaving two entries fighting over the
                    // same pitch.
                    for (auto& o : t.recOpen)
                        if (o.used && o.pitch == pitch) {
                            RtNote nn;
                            nn.beat  = o.beat;
                            nn.len   = loopLen > 0.0
                                           ? overdubNoteLen(o.beat, at, loopLen)
                                           : ((at - o.beat) > kMinNoteLen ? (at - o.beat)
                                                                          : kMinNoteLen);
                            nn.pitch = o.pitch;
                            nn.vel   = o.vel;
                            o.used = false;
                            appendNote(notes, t.recLen, t.recCap, nn);
                        }
                    int k = -1;
                    for (int j = 0; j < 32; ++j) if (!t.recOpen[j].used) { k = j; break; }
                    // 32 keys held at once is past ten fingers and two hands;
                    // the 33rd is dropped rather than stealing a sounding note.
                    if (k >= 0) {
                        t.recOpen[k].used  = true;
                        t.recOpen[k].beat  = at;
                        t.recOpen[k].pitch = pitch;
                        t.recOpen[k].vel   = (u8)(m.d2 & 0x7F);
                    }
                } else {
                    for (auto& o : t.recOpen) {
                        if (!o.used || o.pitch != pitch) continue;
                        RtNote nn;
                        nn.beat  = o.beat;
                        nn.len   = loopLen > 0.0
                                       ? overdubNoteLen(o.beat, at, loopLen)
                                       : ((at - o.beat) > kMinNoteLen ? (at - o.beat)
                                                                      : kMinNoteLen);
                        nn.pitch = o.pitch;
                        nn.vel   = o.vel;
                        o.used = false;
                        appendNote(notes, t.recLen, t.recCap, nn);
                        break;
                    }
                }

                // Same rule as audio: the engine cannot grow a GUI-owned buffer
                // and must not write past it, so a full one ends the take. The
                // notes still held close against this event's position, which
                // for an overdub is already the in-loop one.
                if (t.recLen >= t.recCap) { finishRec(this, ti, t, evts_, at, loopLen); break; }
            }
        }
    };

    if (playing_) {
        const f64 bps = tempo_ / 60.0 / sr_;
        const f64 blockEnd = beat_ + (f64)n * bps;
        int pos = 0;
        while (pos < n) {
            const f64 curBeat = beat_ + (f64)pos * bps;
            fireDue(curBeat);

            // Next scheduled boundary inside this block, if any: a queued
            // launch or stop, a due follow action, or a record start/stop.
            f64 nextB = blockEnd;
            auto consider = [&](f64 b) { if (b > curBeat && b < nextB) nextB = b; };
            for (const auto& t : tracks_) {
                if (t.queued != -2) consider(t.fireBeat);
                else if (t.playing >= 0 && t.fireBeat < kNoFollow) consider(t.fireBeat);
                if (t.recPhase == 1 || t.recPhase == 3) consider(t.recFireBeat);
            }

            int upto = (int)std::ceil((nextB - beat_) / bps);
            upto = clampv(upto, pos + 1, n);
            renderRange(outL, outR, pos, upto);
            captureRange(pos, upto);
            captureMidiRange(pos, upto);
            pos = upto;
        }
        beat_ += (f64)n * bps;
    } else {
        // Voices still get a release tail so stopping never clicks.
        bool anyTail = false;
        for (auto& t : tracks_) if (t.voice.active || t.prev.active) { anyTail = true; break; }
        if (anyTail) renderRange(outL, outR, 0, n);
    }

    // Clips that started inside the block just rendered. See the note on
    // autoPass: still before any chain runs, so class B keeps its ordering.
    autoPass(true);

    // Per-track post stage. The launch-boundary loop above splits *voice*
    // rendering only; everything from here runs exactly once over the whole
    // block, because a plugin must see one contiguous run per callback and
    // because a fader change mid-block would be a click either way.
    bool anySolo = false;
    for (const auto& t : tracks_) if (t.solo) { anySolo = true; break; }

    // Delay compensation for this block. `comp` false is the ordinary case — no
    // device anywhere reports latency — and it must stay free: not a line is
    // touched and every sum below is the arithmetic it always was.
    Pdc* pdc = pdcFind(this);
    const bool comp = pdc && pdc->active;

    // Which return buses take part in this block. A chain with devices on it has
    // to run every block for its tail, exactly like a track's; a live track with
    // a send up brings its return in for as long as the send is up. Their
    // scratch is cleared here, before any track taps into it, for the same
    // reason the tracks' is cleared before any voice writes.
    bool retLive[kMaxReturns];
    for (int r = 0; r < kMaxReturns; ++r)
        retLive[r] = returns_[r].chain && returns_[r].chain->count > 0;
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        if (!live[ti]) continue;
        // An automated send counts even when the user's own level is zero:
        // moving signal into a return is exactly what the envelope is for, and
        // a return whose scratch was never cleared would sum this block's
        // contribution on top of the last one's.
        for (int r = 0; r < kMaxReturns; ++r)
            if (tracks_[ti].send[r] != 0.f || (autoA[ti].sendMask & (1u << r))) retLive[r] = true;
    }
    for (int r = 0; r < kMaxReturns; ++r)
        if (retLive[r]) {
            std::memset(returns_[r].fxL, 0, (size_t)n * sizeof(f32));
            std::memset(returns_[r].fxR, 0, (size_t)n * sizeof(f32));
        }

    // The click. outL/outR holds nothing but the metronome at this point, so
    // this is the only moment it can be aligned on its own: it enters the graph
    // with no chain in front of it, which puts it maxTrackLat ahead of every
    // track. Stage 2 of the derivation then carries it along with the dry sum.
    if (comp) pdcDelay(*pdc, kPdcClick, outL, outR, n, pdc->maxTrackLat);

    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        // A silent path still has to feed its delay line, or the silence never
        // travels down it and the gap comes back out as stale audio.
        if (!live[ti]) { if (comp) pdcFlush(*pdc, ti, n); continue; }

        // Input monitoring, pre-chain: an armed track hears its input through
        // its own devices, so what you hear while recording is what the take
        // will sound like once it is played back through the same chain.
        if (t.arm && (inL || inR)) {
            for (int i = 0; i < n; ++i) {
                t.fxL[i] += inL ? inL[i] : 0.f;
                t.fxR[i] += inR ? inR[i] : 0.f;
            }
        }

        if (t.chain && t.chain->count > 0) {
            const int cnt = t.chain->count < kMaxChainFx ? t.chain->count : kMaxChainFx;

            // MIDI goes in before the chain runs, because a note event has to
            // reach an instrument in time for the block it belongs to. Only
            // armed tracks receive, and only devices that asked for notes:
            // handing a reverb a note-on would be noise on the wire.
            if (midiCount > 0 && t.arm) {
                for (int fi = 0; fi < cnt; ++fi) {
                    PluginInstance* fx = t.chain->fx[fi];
                    if (!fx) continue;
                    const PluginDesc& d = fx->desc();
                    if (!d.hasMidiIn && d.kind != PluginKind::Instrument) continue;
                    for (int mi = 0; mi < midiCount; ++mi) {
                        const MidiMsg& m = midi[mi];
                        const u8 bytes[3] = {m.status, m.d1, m.d2};
                        // Program change and channel pressure are the only two
                        // channel messages with a single data byte.
                        const u8 hi = (u8)(m.status & 0xF0);
                        const int len = (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
                        fx->midi(bytes, len, clampv((int)m.frame, 0, n - 1));
                    }
                }
            }

            // In-place is part of the PluginInstance contract, so the whole
            // chain runs through the one scratch pair with no copies.
            f32* bufs[2] = {t.fxL, t.fxR};
            for (int fi = 0; fi < cnt; ++fi)
                if (PluginInstance* fx = t.chain->fx[fi])
                    fx->process(bufs, bufs, 2, n);
        }

        // Stage 1 of the derivation: line this track's chain up with the
        // latest one. Post-chain and pre-fader, so the dry signal and every
        // send tapped off it move together.
        if (comp) pdcDelay(*pdc, ti, t.fxL, t.fxR, n, pdc->maxTrackLat - pdc->trackLat[ti]);

        const bool audible = !t.mute && (!anySolo || t.solo);
        const AutoBlock& ab = autoA[ti];

        // The two branches below are the same mixdown twice, and the split is
        // deliberate: a track with no class-A lane must keep the constant-gain
        // path it has always had, byte for byte, which is the same "the
        // ordinary case stays free" discipline the delay compensation states
        // for comp == false. The render gates prove it.
        if (!ab.any) {
            const f32 pgL = t.pan <= 0.f ? 1.f : 1.f - t.pan;
            const f32 pgR = t.pan >= 0.f ? 1.f : 1.f + t.pan;
            const f32 gL = audible ? t.vol * pgL : 0.f;
            const f32 gR = audible ? t.vol * pgR : 0.f;

            // Meters are post-fader: what the user sees is what the master gets.
            f32 pkL = 0.f, pkR = 0.f;
            for (int i = 0; i < n; ++i) {
                const f32 l = t.fxL[i] * gL;
                const f32 r = t.fxR[i] * gR;
                outL[i] += l;
                outR[i] += r;
                const f32 al = std::fabs(l), ar = std::fabs(r);
                if (al > pkL) pkL = al;
                if (ar > pkR) pkR = ar;
            }
            if (pkL > t.mL) t.mL = pkL;
            if (pkR > t.mR) t.mR = pkR;

            // Post-fader sends, Live's default tap: what the return hears is what
            // the master hears from this track, scaled. Pan, mute and solo are all
            // already in gL/gR, so a muted track sends nothing and a track silenced
            // by someone else's solo sends nothing either — audibility is one
            // decision, made once, for both destinations.
            for (int r = 0; r < kMaxReturns; ++r) {
                const f32 s = t.send[r];
                if (s == 0.f || !retLive[r]) continue;
                Return& rt = returns_[r];
                const f32 sL = gL * s, sR = gR * s;
                for (int i = 0; i < n; ++i) {
                    rt.fxL[i] += t.fxL[i] * sL;
                    rt.fxR[i] += t.fxR[i] * sR;
                }
            }
        } else {
            // The automated track: the same arithmetic with the gain moving.
            // A step in a gain once per callback is a zipper, and the ramp is
            // free — this loop already runs per sample and already multiplies
            // by gL/gR, so it costs two adds. The interpolated quantity is the
            // derived one (§3.2): the fader position has already been through
            // faderToGain, and t.vol/t.pan are untouched.
            const f32 v0 = ab.hasVol ? ab.vol0 : t.vol;
            const f32 v1 = ab.hasVol ? ab.vol1 : t.vol;
            const f32 p0 = ab.hasPan ? ab.pan0 : t.pan;
            const f32 p1 = ab.hasPan ? ab.pan1 : t.pan;
            const f32 gL0 = audible ? v0 * (p0 <= 0.f ? 1.f : 1.f - p0) : 0.f;
            const f32 gR0 = audible ? v0 * (p0 >= 0.f ? 1.f : 1.f + p0) : 0.f;
            const f32 gL1 = audible ? v1 * (p1 <= 0.f ? 1.f : 1.f - p1) : 0.f;
            const f32 gR1 = audible ? v1 * (p1 >= 0.f ? 1.f : 1.f + p1) : 0.f;
            const f32 dL = (gL1 - gL0) / (f32)n, dR = (gR1 - gR0) / (f32)n;

            f32 gL = gL0, gR = gR0, pkL = 0.f, pkR = 0.f;
            for (int i = 0; i < n; ++i) {
                const f32 l = t.fxL[i] * gL;
                const f32 r = t.fxR[i] * gR;
                outL[i] += l;
                outR[i] += r;
                const f32 al = std::fabs(l), ar = std::fabs(r);
                if (al > pkL) pkL = al;
                if (ar > pkR) pkR = ar;
                gL += dL; gR += dR;
            }
            if (pkL > t.mL) t.mL = pkL;
            if (pkR > t.mR) t.mR = pkR;

            // The send tap re-walks the same fader ramp — the same adds in the
            // same order, so the two agree sample for sample — and rides its
            // own send ramp on top of it.
            for (int r = 0; r < kMaxReturns; ++r) {
                const bool autoSend = (ab.sendMask & (1u << r)) != 0;
                const f32 s0 = autoSend ? ab.snd0[r] : t.send[r];
                const f32 s1 = autoSend ? ab.snd1[r] : t.send[r];
                if ((s0 == 0.f && s1 == 0.f) || !retLive[r]) continue;
                Return& rt = returns_[r];
                const f32 ds = (s1 - s0) / (f32)n;
                f32 sgL = gL0, sgR = gR0, s = s0;
                for (int i = 0; i < n; ++i) {
                    rt.fxL[i] += t.fxL[i] * (sgL * s);
                    rt.fxR[i] += t.fxR[i] * (sgR * s);
                    sgL += dL; sgR += dR; s += ds;
                }
            }
        }
    }

    // Stage 2: the dry sum (tracks + the already-aligned click) waits for the
    // longest return chain, and each return waits for the difference between it
    // and its own. Everything now lands at maxTrackLat + maxRetLat.
    if (comp) pdcDelay(*pdc, kPdcDry, outL, outR, n, pdc->maxRetLat);

    for (int r = 0; r < kMaxReturns; ++r) {
        Return& rt = returns_[r];
        if (!retLive[r]) { if (comp) pdcFlush(*pdc, kMaxTracks + r, n); continue; }

        if (rt.chain && rt.chain->count > 0) {
            const int cnt = rt.chain->count < kMaxChainFx ? rt.chain->count : kMaxChainFx;
            // No MIDI goes to a return: a return is an effect bus, and nothing
            // is armed onto it. In-place, like a track's chain.
            f32* bufs[2] = {rt.fxL, rt.fxR};
            for (int fi = 0; fi < cnt; ++fi)
                if (PluginInstance* fx = rt.chain->fx[fi])
                    fx->process(bufs, bufs, 2, n);
        }
        if (comp) pdcDelay(*pdc, kMaxTracks + r, rt.fxL, rt.fxR, n,
                           pdc->maxRetLat - pdc->retLat[r]);

        // chain -> vol -> meter -> master, so the return meter reads what the
        // master actually receives, exactly as a track's does.
        const f32 g = rt.vol;
        f32 rpkL = 0.f, rpkR = 0.f;
        for (int i = 0; i < n; ++i) {
            const f32 l = rt.fxL[i] * g;
            const f32 rr = rt.fxR[i] * g;
            outL[i] += l;
            outR[i] += rr;
            const f32 al = std::fabs(l), ar = std::fabs(rr);
            if (al > rpkL) rpkL = al;
            if (ar > rpkR) rpkR = ar;
        }
        if (rpkL > rt.mL) rt.mL = rpkL;
        if (rpkR > rt.mR) rt.mR = rpkR;
    }

    // Every line for this block has now been written, so the cursor moves once
    // and the fill mark follows it.
    if (comp) {
        pdc->wpos = (pdc->wpos + n) & kPdcMask;
        if (pdc->filled < kPdcCap) {
            pdc->filled += n;
            if (pdc->filled > kPdcCap) pdc->filled = kPdcCap;
        }
    }

    // The master chain sees the finished sum — tracks, returns and click — and
    // sees it before the master fader and the clip stage, so a bus compressor
    // reacts to the mix rather than to the fader. It is in series with
    // everything, with no parallel path beside it, so it needs no compensation:
    // its latency is simply added to what latencyFrames publishes.
    if (masterChain_ && masterChain_->count > 0) {
        const int cnt = masterChain_->count < kMaxChainFx ? masterChain_->count : kMaxChainFx;
        f32* bufs[2] = {outL, outR};
        for (int fi = 0; fi < cnt; ++fi)
            if (PluginInstance* fx = masterChain_->fx[fi])
                fx->process(bufs, bufs, 2, n);
    }

    // Master bus.
    f32 pkL = 0.f, pkR = 0.f;
    for (int i = 0; i < n; ++i) {
        f32 l = outL[i] * masterVol_;
        f32 r = outR[i] * masterVol_;
        l = clampv(l, -1.f, 1.f);
        r = clampv(r, -1.f, 1.f);
        outL[i] = l; outR[i] = r;
        const f32 al = std::fabs(l), ar = std::fabs(r);
        if (al > pkL) pkL = al;
        if (ar > pkR) pkR = ar;
    }
    if (pkL > masL_) masL_ = pkL;
    if (pkR > masR_) masR_ = pkR;

    publish();

    const auto t1 = std::chrono::steady_clock::now();
    const f64 elapsed = std::chrono::duration<f64>(t1 - t0).count();
    const f64 budget  = (f64)n / sr_;
    const f32 load = (f32)(elapsed / budget * 100.0);
    cpu.store(cpu.load(std::memory_order_relaxed) * 0.9f + load * 0.1f, std::memory_order_relaxed);
}

void Engine::publish() {
    beat.store(beat_, std::memory_order_relaxed);
    playing.store(playing_, std::memory_order_relaxed);
    tempo.store(tempo_, std::memory_order_relaxed);

    constexpr f32 kDecay = 0.72f;
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        meterL[ti].store(t.mL, std::memory_order_relaxed);
        meterR[ti].store(t.mR, std::memory_order_relaxed);
        t.mL *= kDecay; t.mR *= kDecay;

        activeSlot[ti].store(t.playing, std::memory_order_relaxed);
        pendingSlot[ti].store(t.queued, std::memory_order_relaxed);
        SlotState st = SlotState::Stopped;
        if (t.queued >= 0)      st = SlotState::Queued;
        else if (t.queued == -1) st = SlotState::StopQueued;
        else if (t.playing >= 0) st = SlotState::Playing;
        slotState[ti].store((int)st, std::memory_order_relaxed);

        // The published state has three values, not four: a take with a stop
        // already queued (phase 3) is still recording as far as the UI and the
        // user are concerned.
        recState[ti].store(t.recPhase == 3 ? 2 : t.recPhase, std::memory_order_relaxed);
        recSlotIdx[ti].store(t.recSlot, std::memory_order_relaxed);
    }
    for (int r = 0; r < kMaxReturns; ++r) {
        Return& rt = returns_[r];
        returnMeterL[r].store(rt.mL, std::memory_order_relaxed);
        returnMeterR[r].store(rt.mR, std::memory_order_relaxed);
        rt.mL *= kDecay; rt.mR *= kDecay;
    }
    masterMeterL.store(masL_, std::memory_order_relaxed);
    masterMeterR.store(masR_, std::memory_order_relaxed);
    masL_ *= kDecay; masR_ *= kDecay;

    // What the engine delays the world by: the deepest track chain, plus the
    // deepest return chain behind it, plus the master chain in series after
    // both. A host that reports latency to a backend reads this.
    if (const Pdc* p = pdcFind(this))
        latencyFrames.store(p->maxTrackLat + p->maxRetLat + p->masterLat,
                            std::memory_order_relaxed);
}

} // namespace lat
