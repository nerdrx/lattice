#include "engine.h"
#include "../plugin/host.h"
#include <chrono>
#include <cstring>

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

// Appends to the take buffer keeping it sorted by start beat. Notes close in
// note-off order, which for anything a human plays is almost start order, so
// the backwards scan below normally stops on its first compare. Sorting as we
// write rather than in one pass at the stop boundary keeps the work per event
// small and spread out, instead of handing the audio thread an O(n^2) burst on
// the single block a take happens to end on.
static bool insertNote(RtNote* buf, i64& len, i64 cap, const RtNote& n) {
    if (!buf || len >= cap) return false;
    i64 i = len;
    while (i > 0 && buf[i - 1].beat > n.beat) { buf[i] = buf[i - 1]; --i; }
    buf[i] = n;
    ++len;
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
static void finishRec(int ti, TrackT& t, EvRing& evts, f64 endBeat, f64 loopLen = 0.0) {
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
            insertNote(notes, t.recLen, t.recCap, n);
            o.used = false;
        }
        evts.push({Ev::MidiRecordFinished, ti, t.recSlot, (f64)t.recLen, (void*)t.recBuf});
    } else {
        evts.push({Ev::RecordFinished, ti, t.recSlot, (f64)t.recLen, (void*)t.recBuf});
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
                        if (oc) finishRec(ti, t, evts_, t.voice.beatPos, oc->lengthBeats);
                        else    finishRec(ti, t, evts_, stopBeat - t.recStartBeat);
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
                if (changed) {
                    if (t.voice.clip == &dst && t.voice.active) flushOffs(t, t.voice, 0);
                    if (t.prev.clip  == &dst && t.prev.active)  flushOffs(t, t.prev,  0);
                }
                dst = c.clip;
                if (t.voice.clip == &dst && t.voice.active) reseekNotes(t.voice, dst);
                if (t.prev.clip  == &dst && t.prev.active)  reseekNotes(t.prev,  dst);
                if (changed) evts_.push({Ev::NotesRetired, c.a, c.b, 0.0, (void*)old});
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
            t.chain = (const RtChain*)c.p;
            if (old) evts_.push({Ev::ChainRetired, c.a, 0, 0.0, (void*)old});
            break;
        }

        case Cmd::ClearClip:
            if (c.a >= 0 && c.a < kMaxTracks && c.b >= 0 && c.b < kMaxScenes) {
                Track& t = tracks_[c.a];
                RtClip& dst = clips_[c.a][c.b];
                const RtNote* old = dst.notes;
                if (t.playing == c.b) { t.voice.releasing = true; t.playing = -1;
                                        t.fireBeat = kNoFollow; }
                if (t.queued  == c.b) { t.queued = -2; t.fireBeat = kNoFollow; }
                // An audio voice keeps its release ramp over the now-empty clip
                // (fetch() reads silence out of it); a MIDI voice has nothing to
                // fade and everything to hand back, so it ends here.
                dropVoice(t, t.prev,  c.a, false, &dst);
                dropVoice(t, t.voice, c.a, true,  &dst);
                dst = RtClip{};
                if (old) evts_.push({Ev::NotesRetired, c.a, c.b, 0.0, (void*)old});
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
            if (oc) finishRec(ti, t, evts_, t.voice.beatPos, oc->lengthBeats);
            else    finishRec(ti, t, evts_, boundary - t.recStartBeat);
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

        if (primary) clipPhase[ti].store(v.beatPos / L, std::memory_order_relaxed);
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
            clipPhase[ti].store((v.srcPos - (f64)c.loopStart) / loopLen, std::memory_order_relaxed);
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
            if (t.recLen >= t.recCap) finishRec(ti, t, evts_, 0.0);
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
                            insertNote(notes, t.recLen, t.recCap, nn);
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
                        insertNote(notes, t.recLen, t.recCap, nn);
                        break;
                    }
                }

                // Same rule as audio: the engine cannot grow a GUI-owned buffer
                // and must not write past it, so a full one ends the take. The
                // notes still held close against this event's position, which
                // for an overdub is already the in-loop one.
                if (t.recLen >= t.recCap) { finishRec(ti, t, evts_, at, loopLen); break; }
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

    // Per-track post stage. The launch-boundary loop above splits *voice*
    // rendering only; everything from here runs exactly once over the whole
    // block, because a plugin must see one contiguous run per callback and
    // because a fader change mid-block would be a click either way.
    bool anySolo = false;
    for (const auto& t : tracks_) if (t.solo) { anySolo = true; break; }

    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (!live[ti]) continue;

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

        const bool audible = !t.mute && (!anySolo || t.solo);
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
    masterMeterL.store(masL_, std::memory_order_relaxed);
    masterMeterR.store(masR_, std::memory_order_relaxed);
    masL_ *= kDecay; masR_ *= kDecay;
}

} // namespace lat
