// The take builder — what a journalled performance becomes (docs/ARRANGEMENT.md
// §5.4, §5.5).
//
// A PURE TRANSFORM over a run of ArrJournal entries: no App, no Session, no
// allocation the caller cannot see, and no knowledge of what a ClipModel is. It
// answers two questions and nothing else:
//
//   1. is this pass admissible at all (§5.4 — a gap REFUSES the take), and
//   2. if it is, which stretches of timeline were occupied by which slot, and
//      which notes were played, in absolute beats.
//
// WHY ITS OWN HEADER, and not app_arrange.cpp where its one caller lives: the
// bit-identity gate lives in tests/engine_test.cpp, which links src/audio and
// src/core and deliberately not one line of the GUI. The gate has to build the
// arrangement THE COMMIT WOULD BUILD and render it against the performance it
// came from, so the rule that turns launches into items has to be reachable from
// a binary that has no App in it. Putting it here means the test measures the
// shipping transform rather than a second copy of it written to agree.
//
// Turning a TakeItem into an ArrangeClip — copying the session slot's ClipModel
// in, taking the uid, setting sourceUid — is the caller's, because that is the
// half that needs the model. See App::commitTake in src/ui/app_arrange.cpp.
#pragma once
#include "../audio/engine.h"
#include <cstddef>
#include <vector>

namespace lat {

// One stretch of timeline a session clip occupied, in absolute beats.
struct TakeItem {
    int track  = 0;
    int slot   = 0;         // the session slot the engine launched
    f64 start  = 0.0;       // the beat the engine launched it on
    f64 length = 0.0;       // until it was stopped, replaced, or the pass ended
    // Always 0.0, and the reason is worth stating rather than leaving to be
    // rediscovered: a launch starts the clip at its beginning, and everything a
    // performance does after that -- looping, warping, the follow action -- is
    // inside the clip and is reproduced by playing the same RtClip for the same
    // span. An item at (start, length, offset = 0) over a 4-beat looping clip
    // held for 12 beats loops twice exactly as the launch did, because it IS the
    // same voice on the same clip. Anything else would be the arrangement
    // re-deriving what the clip already knows.
    f64 offset = 0.0;
};

// One note an armed track was played during the pass, in ABSOLUTE beats.
struct TakeNote {
    int track = 0;
    f64 beat  = 0.0;
    f64 len   = 0.0;
    u8  pitch = 60;
    u8  vel   = 100;
};

struct TakeResult {
    // False means REFUSE (§5.4): discard, take no undo point, and say how many
    // entries were lost. Committing what arrived is the worse failure, because a
    // recording silently missing four bars is indistinguishable from a
    // performance that had four bars of rest in it.
    bool ok = false;
    // Entries the journal lost, from the seq gaps and from the caller's own
    // Engine::journalDropped delta. This is the N in the status line answer #6
    // fixes the wording of: "take discarded: N journal entries dropped".
    u32  dropped = 0;
    bool sawStart = false;      // a TakeStart was seen
    bool sawEnd   = false;      // ...and a terminator for it
    // Why the pass ended, for the caller's status line. `Wrap` and `Locate` are
    // §5.5's rule: a discontinuity ENDS the take there and commits it, which is
    // honest and is what a first version can defend. Overdub onto existing
    // arrangement material is §11.
    enum class End { None = 0, Stop, Wrap, Locate };
    End  end = End::None;
    f64  startBeat = 0.0;
    f64  endBeat   = 0.0;
    std::vector<TakeItem> items;
    std::vector<TakeNote> notes;
};

// `entries` is everything the consumer drained across the pass, in ring order.
// `extraDropped` is what the consumer knows and the entries cannot say: the
// delta in Engine::journalDropped across the pass, plus (under the process
// split) the daemon's own — §5.4 requires both hops to be covered, and a
// consumer that has not drained the ring can still read the counter.
inline TakeResult buildTake(const ArrJournal* entries, size_t count,
                            u32 extraDropped) {
    TakeResult r;
    r.dropped = extraDropped;

    // --- 1. the pass's span, and its contiguity ---------------------------
    //
    // Contiguity is checked over the entries BETWEEN the start and the
    // terminator, which is exactly the span §5.4 names. A gap before the take
    // began belongs to whatever came before it and must not condemn this pass;
    // a gap after it ended belongs to the next one.
    size_t first = count, last = count;
    for (size_t i = 0; i < count; ++i)
        if (entries[i].kind == (u32)JournalKind::TakeStart) { first = i; break; }
    if (first == count) {
        // No TakeStart at all. Either nothing was performed, or the entry that
        // opened the pass is one of the ones that was dropped -- and the second
        // is indistinguishable from the first without the counter, which is
        // precisely why the counter is published separately.
        r.ok = (r.dropped == 0);
        return r;
    }
    r.sawStart  = true;
    r.startBeat = entries[first].beat;
    r.endBeat   = entries[first].beat;

    for (size_t i = first + 1; i < count; ++i) {
        const u32 k = entries[i].kind;
        if (k == (u32)JournalKind::TakeEnd)   { last = i; r.end = TakeResult::End::Stop;   break; }
        if (k == (u32)JournalKind::LoopWrap)  { last = i; r.end = TakeResult::End::Wrap;   break; }
        if (k == (u32)JournalKind::Locate)    { last = i; r.end = TakeResult::End::Locate; break; }
        // A second TakeStart without a terminator cannot happen (the engine
        // pushes one only out of a stopped transport), but if the entry that
        // ended the previous pass was lost, this is where it shows up. Treat it
        // as the terminator of the pass being built rather than silently
        // splicing two performances into one.
        if (k == (u32)JournalKind::TakeStart) { last = i; r.end = TakeResult::End::Stop;   break; }
    }
    if (last == count) {
        // No terminator: the consumer asked to commit while the pass was still
        // open (ARR disarmed mid-take, say). The take ends at the last thing
        // that happened, which is the only defensible answer available.
        last = count - 1;
        r.end = TakeResult::End::None;
    } else {
        r.sawEnd = true;
    }
    r.endBeat = entries[last].beat;
    if (r.endBeat < r.startBeat) r.endBeat = r.startBeat;

    u32 gaps = 0;
    for (size_t i = first + 1; i <= last; ++i) {
        const u32 want = entries[i - 1].seq + 1u;      // wraps with the counter
        if (entries[i].seq != want) gaps += entries[i].seq - want;
    }
    r.dropped += gaps;
    if (r.dropped != 0) return r;                      // REFUSED. Nothing else runs.

    // --- 2. what was performed -------------------------------------------
    //
    // One item per LAUNCH, and this is the rule §5 leaves to be derived, so it
    // is derived here in the open:
    //
    //   * a clip that played from X to Y becomes an item at X of length Y - X;
    //   * consecutive repeats of one clip stay SEPARATE items, one per launch.
    //
    // The second half is not a stylistic choice, it is the bit-identity gate
    // speaking. A relaunch calls startVoice: srcPos goes back to zero, the
    // declick envelope re-attacks over 3 ms and the grain phase resets. Merging
    // two launches of one clip into a single long item would erase that
    // re-attack -- and playing the merged item back would sound the loop wrap
    // the clip does at its own length instead, which is a different sound. Kept
    // separate, the two items fail §3.5's contiguity condition (the second's
    // offset is 0, not `out.offset + (in.start - out.start)`), so the scheduler
    // relaunches at exactly the beat the performer did.
    //
    // Conversely a clip left running across its own loop point is ONE item: the
    // loop is the clip's, not the timeline's, and the item plays the same
    // looping RtClip for the same span.
    struct Open { bool on = false; int slot = -1; f64 start = 0.0; };
    std::vector<Open> open((size_t)kMaxTracks);
    struct Held { bool on = false; f64 beat = 0.0; u8 vel = 100; };
    std::vector<Held> held((size_t)kMaxTracks * 128);

    const auto closeItem = [&](int ti, f64 at) {
        Open& o = open[(size_t)ti];
        if (!o.on) return;
        o.on = false;
        const f64 len = at - o.start;
        // A launch and its stop on the same grid line occupied no timeline at
        // all. arrangeRepair would delete the sliver anyway (its minimum-length
        // rule deletes rather than clamping up, so that a trim to nothing is a
        // deletion); dropping it here keeps a refused item from ever reaching
        // the model and being undone by the repair a frame later.
        if (!(len > 0.0)) return;
        TakeItem it;
        it.track  = ti;
        it.slot   = o.slot;
        // NEGATIVE ZERO, and it is not pedantry. nextQuantum computes
        // `ceil(fromBeat / q - kEps) * q`, so a launch at beat 0 is scheduled for
        // -0.0 -- which compares equal to 0.0 everywhere and PRINTS as "-0.000"
        // in every log line and every inspector for the rest of the set's life.
        // The comparison below is true for -0.0, so this is the whole fix.
        it.start  = (o.start == 0.0) ? 0.0 : o.start;
        it.length = len;
        it.offset = 0.0;
        r.items.push_back(it);
    };

    for (size_t i = first; i <= last; ++i) {
        const ArrJournal& e = entries[i];
        const int ti = e.track;
        switch ((JournalKind)e.kind) {
        case JournalKind::ClipOn: {
            if (ti < 0 || ti >= kMaxTracks) break;
            closeItem(ti, e.beat);              // a launch ends whatever it replaced
            Open& o = open[(size_t)ti];
            o.on = true; o.slot = e.a; o.start = e.beat;
            break;
        }
        case JournalKind::ClipOff:
            if (ti < 0 || ti >= kMaxTracks) break;
            closeItem(ti, e.beat);
            break;
        case JournalKind::NoteOn: {
            if (ti < 0 || ti >= kMaxTracks) break;
            const int pitch = e.a & 0x7F;
            const int vel   = (e.a >> 8) & 0x7F;
            Held& h = held[(size_t)ti * 128 + (size_t)pitch];
            // A retrigger without an intervening off closes the old note rather
            // than leaving two entries fighting over one pitch -- the same rule
            // captureMidiRange applies to a session take.
            if (h.on) {
                TakeNote n;
                n.track = ti; n.beat = h.beat; n.len = e.beat - h.beat;
                n.pitch = (u8)pitch; n.vel = h.vel;
                if (n.len > 0.0) r.notes.push_back(n);
            }
            h.on = true; h.beat = e.beat; h.vel = (u8)vel;
            break;
        }
        case JournalKind::NoteOff: {
            if (ti < 0 || ti >= kMaxTracks) break;
            const int pitch = e.a & 0x7F;
            Held& h = held[(size_t)ti * 128 + (size_t)pitch];
            if (!h.on) break;
            h.on = false;
            TakeNote n;
            n.track = ti; n.beat = h.beat; n.len = e.beat - h.beat;
            n.pitch = (u8)pitch; n.vel = h.vel;
            if (n.len > 0.0) r.notes.push_back(n);
            break;
        }
        default: break;
        }
    }

    // §5.5: unmatched ons are closed at the take's end, and so is every clip
    // still sounding when the transport stopped. A performance that was still
    // going when it ended is a performance up to the moment it ended.
    for (int ti = 0; ti < kMaxTracks; ++ti) closeItem(ti, r.endBeat);
    for (int ti = 0; ti < kMaxTracks; ++ti)
        for (int p = 0; p < 128; ++p) {
            Held& h = held[(size_t)ti * 128 + (size_t)p];
            if (!h.on) continue;
            h.on = false;
            TakeNote n;
            n.track = ti; n.beat = h.beat; n.len = r.endBeat - h.beat;
            n.pitch = (u8)p; n.vel = h.vel;
            if (n.len > 0.0) r.notes.push_back(n);
        }

    r.ok = true;
    return r;
}

inline TakeResult buildTake(const std::vector<ArrJournal>& entries, u32 extraDropped) {
    return buildTake(entries.empty() ? nullptr : entries.data(), entries.size(), extraDropped);
}

} // namespace lat
