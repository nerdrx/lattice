// The arrangement's two publishers and their reapers (docs/ARRANGEMENT.md §3.2,
// §3.3, §3.6, §3.7, §6.2, §6.6).
//
// Everything here runs on the GUI thread and produces memory the audio thread
// borrows. There is no new lifetime protocol in this file: it is the RtNote
// protocol for the fifth and sixth time, and where a comment says "verbatim" it
// means the lines above it were written by reading publishNotes / publishAutos
// and changing the type.
//
// The two things worth reading before anything else:
//
//   1. ONE ALLOCATION per lane. `[RtArrangement][RtArrItem[]][RtClip[]][RtNote[]]`
//      is one `new char[]`, one `delete[]`, and therefore exactly one pointer for
//      the retirement protocol to talk about. buildArrangement and freeArrangement
//      are the two halves of that bargain and sit next to each other, below.
//
//   2. THE DEDUPE IS NOT AN OPTIMISATION. Splitting an item copies the whole
//      ClipModel (§2 rule 1), so two halves of one split hold two identical
//      copies. §3.5's continuation rule tests RtClip POINTER equality, so a split
//      can only ever continue instead of retriggering if this publisher notices
//      that the two copies are the same content and emits one RtClip for both.
//      Without the dedupe the R3 gate is unreachable, and it is unreachable
//      silently — the render is merely wrong, not broken.
//
#include "app.h"
#include "app_internal.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>
#include <type_traits>

namespace lat {

// ---------------------------------------------------------------------------
// the block arithmetic
// ---------------------------------------------------------------------------

namespace {

// Rounds an offset inside a block up to what the next array needs. Written out
// rather than left to a static_assert that today's sizes happen to satisfy: the
// RtAutoSetN block genuinely needs it (sizeof(RtAutoLane) is 36, so an odd lane
// count leaves the RtAutoPoint array on a 4-byte boundary and RtAutoPoint holds
// an f64), and a rule that is only sometimes applied is a bus error waiting for
// the first member somebody adds.
constexpr size_t alignUp(size_t off, size_t a) { return (off + a - 1) / a * a; }

// `new char[]` is guaranteed to come back suitably aligned for any object with
// a fundamental alignment, which is every type in these two blocks. Said out
// loud so that a member with a stricter alignment fails here rather than on the
// first machine that cares.
static_assert(alignof(RtArrangement) <= alignof(std::max_align_t), "lane block over-aligned");
static_assert(alignof(RtArrItem)     <= alignof(std::max_align_t), "lane block over-aligned");
static_assert(alignof(RtClip)        <= alignof(std::max_align_t), "lane block over-aligned");
static_assert(alignof(RtNote)        <= alignof(std::max_align_t), "lane block over-aligned");
static_assert(alignof(RtAutoSetN)    <= alignof(std::max_align_t), "auto block over-aligned");
static_assert(alignof(RtAutoLane)    <= alignof(std::max_align_t), "auto block over-aligned");
static_assert(alignof(RtAutoPoint)   <= alignof(std::max_align_t), "auto block over-aligned");

// Nothing in either block has a destructor to run, which is what makes the free
// a single `delete[]` on the char[] rather than a walk. If one of these ever
// grows a non-trivial member the free below becomes wrong, silently, so it is
// asserted here instead.
static_assert(std::is_trivially_destructible<RtArrangement>::value, "lane block frees flat");
static_assert(std::is_trivially_destructible<RtArrItem>::value,     "lane block frees flat");
static_assert(std::is_trivially_destructible<RtClip>::value,        "lane block frees flat");
static_assert(std::is_trivially_destructible<RtNote>::value,        "lane block frees flat");
static_assert(std::is_trivially_destructible<RtAutoSetN>::value,    "auto block frees flat");
static_assert(std::is_trivially_destructible<RtAutoLane>::value,    "auto block frees flat");
static_assert(std::is_trivially_destructible<RtAutoPoint>::value,   "auto block frees flat");

// THE FREE, for both kinds of block, and the reason the cast is what it is: the
// allocation is a char[] holding a placement-new'd header followed by its
// arrays, so it is a char[] that has to be deleted, not the header. Freeing the
// header instead would be a delete of a pointer the allocator never handed out.
// The publisher and the reaper must agree on this, which is why they are in one
// file with this comment between them.
void freeBlock(const void* header) {
    delete[] reinterpret_cast<const char*>(header);
}

// ---------------------------------------------------------------------------
// the dedupe (§3.3)
//
// Identity is on EVERYTHING THAT REACHES THE WIRE. The list in §3.3 is the
// sample pointer, frames, channels, loopStart, loopEnd, clipBpm, lengthBeats,
// gain, warp, loop, isMidi, the published `autos` pointer, the marker array, the
// transient array, and the contents of the note vector.
//
// One reconciliation, and it is the only place this file departs from a literal
// reading of the document. §3.3 lists "the published `autos` pointer" among the
// fields compared, and §3.2 keys the item-envelope table by ITEM uid. Taken
// literally those two are circular: an envelope set built per item gives two
// halves of a split two different `autos` pointers, the pointers differ, the
// clips do not dedupe, and R3 fails for every clip that carries an envelope —
// which is precisely the case the dedupe exists to serve.
//
// So the comparison is made on the MODEL's envelopes, before anything is built,
// and one RtAutoSet is built per DISTINCT PAYLOAD rather than per item. The
// published `autos` pointers are then equal exactly when the envelopes are, so
// §3.3's rule holds by construction and is strictly stronger than comparing
// pointers built after the fact. The table still records a uid (§3.2's reason —
// indices renumber, uids do not); it records the uid of the first item whose
// payload produced the set.
// ---------------------------------------------------------------------------

// Bit-exact throughout, deliberately. Two copies of one ClipModel are bitwise
// equal, so exactness costs the dedupe nothing and buys it the guarantee that it
// NEVER merges two payloads that would render differently. The failure mode of
// being too strict is a duplicated RtClip; the failure mode of being too loose
// is one item playing another item's material.
bool sameNotes(const std::vector<NoteModel>& a, const std::vector<NoteModel>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].beat != b[i].beat || a[i].len != b[i].len ||
            a[i].pitch != b[i].pitch || a[i].vel != b[i].vel) return false;
    }
    return true;
}

bool samePoints(const std::vector<AutoPoint>& a, const std::vector<AutoPoint>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].beat != b[i].beat || a[i].value != b[i].value ||
            a[i].curve != b[i].curve) return false;
    }
    return true;
}

// The envelopes as the MODEL holds them, not as they resolve: resolution is the
// same for two lanes with the same text on the same track, so comparing the text
// is comparing the outcome, and it does not need a resolver to run first.
bool sameEnvelopes(const std::vector<AutoLane>& a, const std::vector<AutoLane>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].enabled != b[i].enabled || a[i].address != b[i].address) return false;
        if (!samePoints(a[i].points, b[i].points)) return false;
    }
    return true;
}

bool sameMarkers(const std::vector<WarpMarker>& a, const std::vector<WarpMarker>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].srcFrame != b[i].srcFrame || a[i].beat != b[i].beat) return false;
    return true;
}

// The cheap pre-filter §3.3 asks for, so the O(n) scan over clips[] rejects on
// three scalars before it ever looks at a note vector.
struct PayloadKey {
    const SampleBuffer* sample = nullptr;
    size_t noteCount = 0;
    f64 lengthBeats = 0.0;
    bool operator==(const PayloadKey& o) const {
        return sample == o.sample && noteCount == o.noteCount && lengthBeats == o.lengthBeats;
    }
};

PayloadKey payloadKey(const ClipModel& m) {
    return PayloadKey{m.sample.get(), m.notes.size(), m.lengthBeats};
}

// The full comparison, run only when the keys match.
bool samePayload(const ClipModel& a, const ClipModel& b) {
    if (a.kind != b.kind) return false;
    if (a.sample.get() != b.sample.get()) return false;
    if (a.lengthBeats != b.lengthBeats) return false;
    if (a.gain != b.gain) return false;
    if (a.loop != b.loop) return false;
    if (a.quantumIdx != b.quantumIdx) return false;
    if (a.prob != b.prob) return false;
    if (a.followAction != b.followAction) return false;
    if (a.followBeats != b.followBeats) return false;
    // Audio-only fields. Compared unconditionally anyway: a MIDI clip carries
    // whatever these were before it became one, and two payloads that differ in
    // a field the engine does not read are still two payloads the user can make
    // differ later.
    if (a.loopStart != b.loopStart || a.loopEnd != b.loopEnd) return false;
    if (a.clipBpm != b.clipBpm) return false;
    if (a.warp != b.warp) return false;
    if (!sameMarkers(a.markers, b.markers)) return false;
    if (!sameNotes(a.notes, b.notes)) return false;
    if (!sameEnvelopes(a.envelopes, b.envelopes)) return false;
    return true;
}

// A finite double, or the fallback. Non-finite values are the one thing this
// publisher replaces rather than passes through, for buildAutos's reason: a NaN
// beat is not ugly, it is a linear scan that never terminates.
f64 finiteOr(f64 v, f64 fallback) { return std::isfinite(v) ? v : fallback; }

} // namespace

// ---------------------------------------------------------------------------
// one item's clip envelopes (§3.2)
//
// buildAutos's body against an arbitrary ClipModel instead of against
// ses_.tracks[t].slots[s]. It is not the same function, and it deliberately is
// not made into one: buildAutos lives in app_engine.cpp, which this milestone
// does not own, and hoisting a parameter through it is an edit to a file with
// another owner for the duration. When these two files are next opened by one
// milestone, buildAutos(track, slot) becomes a two-line forwarder onto this.
//
// The block is registered in the SHARED autoBlocks_ pool, which is what makes
// dropAutos() and the existing Ev::AutosRetired reaper work on these sets with
// no change at all: one pool, one free, one handshake.
// ---------------------------------------------------------------------------

const RtAutoSet* App::buildAutosFor(int track, const ClipModel& m) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return nullptr;
    if (!m.valid() || m.envelopes.empty()) return nullptr;

    // Resolve first, allocate second: the number of lanes that survive
    // resolution is what decides whether there is anything to allocate at all.
    RtAutoLane lanes[kMaxRtAutoLanes];
    i32 modelLane[kMaxRtAutoLanes];
    int laneCount = 0, pointCount = 0;
    for (size_t i = 0; i < m.envelopes.size() && laneCount < kMaxRtAutoLanes; ++i) {
        const AutoLane& L = m.envelopes[i];
        if (!L.enabled || L.points.empty()) continue;
        RtAutoLane rl;
        if (!resolveAutoLane(track, L.address, rl)) continue;
        int n = (int)L.points.size();
        if (pointCount + n > kMaxClipAutoPoints) {
            n = kMaxClipAutoPoints - pointCount;
            LOGW("arrangement item '%s' exceeds %d automation points - the tail of "
                 "'%s' will not be applied", m.name.c_str(), kMaxClipAutoPoints,
                 L.address.c_str());
        }
        if (n <= 0) break;
        rl.first = pointCount;
        rl.count = n;
        rl.flags = 0;
        lanes[laneCount] = rl;
        modelLane[laneCount] = (i32)i;
        ++laneCount;
        pointCount += n;
    }
    if (laneCount == 0) return nullptr;

    const size_t ptsOff = alignUp(sizeof(RtAutoSet), alignof(RtAutoPoint));
    const size_t bytes  = ptsOff + (size_t)pointCount * sizeof(RtAutoPoint);
    char* mem = new (std::nothrow) char[bytes];
    if (!mem) {
        status_ = "Out of memory - arrangement automation not updated";
        return nullptr;
    }
    RtAutoSet* set = new (mem) RtAutoSet();
    RtAutoPoint* pts = reinterpret_cast<RtAutoPoint*>(mem + ptsOff);

    int w = 0;
    for (int l = 0; l < laneCount; ++l) {
        const AutoLane& L = m.envelopes[(size_t)modelLane[l]];
        for (int k = 0; k < lanes[l].count; ++k) {
            const AutoPoint& src = L.points[(size_t)k];
            new (&pts[w]) RtAutoPoint();
            pts[w].beat  = std::isfinite(src.beat) ? std::max(0.0, src.beat) : 0.0;
            pts[w].value = std::isfinite(src.value) ? src.value : 0.f;
            pts[w].curve = src.curve;
            ++w;
        }
        set->lanes[l] = lanes[l];
    }
    set->points     = pts;
    set->laneCount  = laneCount;
    set->pointCount = pointCount;

    AutoBlock b;
    b.mem = mem;
    b.set = set;
    b.clipUid = m.uid;
    b.modelLane.assign(modelLane, modelLane + laneCount);
    autoBlocks_.v.push_back(std::move(b));
    return set;
}

// ---------------------------------------------------------------------------
// the lane block (§3.2, §3.3)
// ---------------------------------------------------------------------------

const RtArrangement* App::buildArrangement(int track) {
    // Anything a previous build left unadopted is a build whose publish never
    // happened, which cannot occur — publishArrangement always adopts or drops.
    // Cleared first anyway, because "cannot occur" is not a memory-safety
    // argument and this is two lines.
    for (const ArrAutoPub& p : pendingArrPubs_) dropAutos(p.set);
    pendingArrPubs_.clear();

    if (track < 0 || track >= (int)ses_.tracks.size()) return nullptr;
    const TrackModel& tr = ses_.tracks[(size_t)track];
    if (tr.arrange.empty()) return nullptr;

    // PASS ONE: decide the shape. Which items survive, which distinct payloads
    // they collapse onto, and how many notes the tail has to hold. Nothing is
    // allocated until all three numbers are known, which is what keeps the block
    // exactly one allocation and the failure path a plain `return nullptr`.
    //
    // The bounds are enforced HERE and not in the parser (session.h): a file may
    // say anything, and the publisher is the last place that can refuse to hand
    // the audio thread more than it agreed to read.
    int itemCount = (int)tr.arrange.size();
    if (itemCount > kMaxArrItems) {
        LOGW("track %d has %d arrangement items - only the first %d are published",
             track, itemCount, kMaxArrItems);
        itemCount = kMaxArrItems;
    }

    // Which distinct payload each item landed on, and the item that first
    // produced each payload. Both indexed into tr.arrange.
    std::vector<int> itemClip((size_t)itemCount, -1);
    std::vector<int> firstOf;              // clip index -> item index
    std::vector<PayloadKey> keys;
    std::vector<int> clipNotes;            // clip index -> notes actually published
    int noteCount = 0;

    for (int i = 0; i < itemCount; ++i) {
        const ClipModel& m = tr.arrange[(size_t)i].src;
        if (!m.valid()) continue;          // an item whose sample is offline

        const PayloadKey k = payloadKey(m);
        int found = -1;
        for (size_t c = 0; c < firstOf.size(); ++c) {
            if (!(keys[c] == k)) continue; // the cheap pre-filter, first
            if (samePayload(tr.arrange[(size_t)firstOf[c]].src, m)) { found = (int)c; break; }
        }
        if (found >= 0) { itemClip[(size_t)i] = found; continue; }

        // A new distinct payload. Its notes are copied into the block's tail
        // ONCE, which is what makes kMaxArrNotes reachable: 64 splits of a
        // 10 000-note clip cost 10 000 notes on the wire, not 640 000.
        int n = (m.kind == ClipKind::Midi) ? (int)m.notes.size() : 0;
        if (noteCount + n > kMaxArrNotes) {
            n = kMaxArrNotes - noteCount;
            LOGW("track %d exceeds %d arrangement notes - clip '%s' is published "
                 "with %d of its notes", track, kMaxArrNotes, m.name.c_str(), n < 0 ? 0 : n);
            if (n < 0) n = 0;
        }
        itemClip[(size_t)i] = (int)firstOf.size();
        firstOf.push_back(i);
        keys.push_back(k);
        clipNotes.push_back(n);
        noteCount += n;
    }

    const int clipCount = (int)firstOf.size();
    if (clipCount == 0) return nullptr;    // every item was invalid: nothing to play

    // PASS TWO: the envelopes, one set per DISTINCT PAYLOAD (see the dedupe note
    // above). Built before the block, because RtClip::autos is one of the things
    // the block holds and these are separate allocations with their own
    // retirement — folding them into the lane would mean dragging one breakpoint
    // republishes up to 1.6 MB of notes.
    std::vector<const RtAutoSet*> clipAutos((size_t)clipCount, nullptr);
    for (int c = 0; c < clipCount; ++c) {
        const ArrangeClip& item = tr.arrange[(size_t)firstOf[(size_t)c]];
        const RtAutoSet* set = buildAutosFor(track, item.src);
        if (!set) continue;
        clipAutos[(size_t)c] = set;
        pendingArrPubs_.push_back(ArrAutoPub{item.uid, set});
    }

    // PASS THREE: the one allocation.
    const size_t itemsOff = alignUp(sizeof(RtArrangement), alignof(RtArrItem));
    const size_t clipsOff = alignUp(itemsOff + (size_t)itemCount * sizeof(RtArrItem),
                                    alignof(RtClip));
    const size_t notesOff = alignUp(clipsOff + (size_t)clipCount * sizeof(RtClip),
                                    alignof(RtNote));
    const size_t bytes    = notesOff + (size_t)noteCount * sizeof(RtNote);

    char* mem = new (std::nothrow) char[bytes];
    if (!mem) {
        status_ = "Out of memory - arrangement not updated";
        // The envelope sets were built and nothing will ever borrow them.
        for (const ArrAutoPub& p : pendingArrPubs_) dropAutos(p.set);
        pendingArrPubs_.clear();
        return nullptr;
    }

    RtArrangement* arr = new (mem) RtArrangement();
    RtArrItem* items   = reinterpret_cast<RtArrItem*>(mem + itemsOff);
    RtClip*    clips   = reinterpret_cast<RtClip*>(mem + clipsOff);
    RtNote*    notes   = reinterpret_cast<RtNote*>(mem + notesOff);

    // The clips, one per distinct payload, with their notes in the block's tail.
    int nw = 0;
    for (int c = 0; c < clipCount; ++c) {
        const ClipModel& m = tr.arrange[(size_t)firstOf[(size_t)c]].src;
        RtClip& rc = *new (&clips[c]) RtClip();
        const bool midi = (m.kind == ClipKind::Midi);
        if (midi) {
            rc.isMidi    = true;
            rc.noteCount = clipNotes[(size_t)c];
            if (rc.noteCount > 0) {
                rc.notes = notes + nw;
                for (int k = 0; k < rc.noteCount; ++k) {
                    const NoteModel& n = m.notes[(size_t)k];
                    RtNote& out = *new (&notes[nw]) RtNote();
                    out.beat  = n.beat;
                    out.len   = n.len;
                    out.pitch = n.pitch;
                    out.vel   = n.vel;
                    ++nw;
                }
            }
        } else {
            rc.data      = m.sample->data.data();
            rc.frames    = m.sample->frames;
            rc.channels  = m.sample->channels;
            rc.loopStart = m.loopStart;
            rc.loopEnd   = m.loopEnd > m.loopStart ? m.loopEnd : m.sample->frames;
            rc.clipBpm   = m.clipBpm;
            rc.warp      = (int)m.warp;
            // Transients are BORROWED from the SampleBuffer and never retired —
            // built once at load, and the item holds a SampleRef, so the pointer
            // outlives every RtClip that names it exactly as `data` does.
            if (!m.sample->transients.empty()) {
                rc.transients     = m.sample->transients.data();
                rc.transientCount = (int)m.sample->transients.size();
            }
            // WARP MARKERS ARE NOT PUBLISHED FOR ARRANGEMENT ITEMS in this
            // milestone, and that is a gap rather than a decision: a per-item
            // marker array is a fifth allocation with a retirement of its own,
            // and §3.2 names a table for the item ENVELOPES and none for the item
            // markers. Left null (an item then warps at its single clipBpm ratio,
            // which is a working clip and not a broken one) and reported, rather
            // than invented here where the engine half could not know to announce
            // it. The dedupe already compares the marker vectors, so the day the
            // table exists nothing about identity changes.
        }
        rc.lengthBeats  = m.lengthBeats;
        rc.gain         = m.gain;
        rc.loop         = m.loop;
        rc.quantumIdx   = m.quantumIdx;
        rc.prob         = m.prob;
        rc.followAction = (int)m.followAction;
        rc.followBeats  = m.followBeats;
        rc.autos        = clipAutos[(size_t)c];
        rc.valid        = true;
    }

    // The items, in LANE ORDER — which is model order, because the invariant
    // §2.3 states is stated on the vector in order and arrangeRepair sorts as its
    // first step. The publisher does not sort: an ordering the engine depends on
    // is established by the editor and by the loader, and a publisher that
    // quietly re-sorted would hide the day one of them stopped.
    int w = 0;
    for (int i = 0; i < itemCount; ++i) {
        if (itemClip[(size_t)i] < 0) continue;      // an invalid item places nothing
        const ArrangeClip& src = tr.arrange[(size_t)i];
        RtArrItem& it = *new (&items[w]) RtArrItem();
        it.start     = finiteOr(src.start, 0.0);
        it.length    = finiteOr(src.length, 0.0);
        it.offset    = finiteOr(src.offset, 0.0);
        it.fadeIn    = (f32)finiteOr(src.fadeIn, 0.0);
        it.fadeOut   = (f32)finiteOr(src.fadeOut, 0.0);
        it.fadeShape = (i32)src.fadeShape;
        it.clip      = itemClip[(size_t)i];
        ++w;
    }
    // Items that placed nothing shrink the array in place. The tail of the
    // reservation is left constructed-but-unreferenced rather than the block
    // resized: itemCount is what the engine reads, one allocation is the
    // property being defended, and a second pass to count first would read the
    // whole model twice to save at most 512 * 40 bytes.
    for (int i = w; i < itemCount; ++i) new (&items[i]) RtArrItem();

    arr->items     = items;
    arr->clips     = clips;
    arr->itemCount = w;
    arr->clipCount = clipCount;
    arr->noteCount = nw;
    // Zero on every track's lane; the transport cell is the only carrier of a
    // loop brace, and it is built by publishTransportCell below.
    arr->loopStart = 0.0;
    arr->loopEnd   = 0.0;
    arr->loopOn    = 0;

    // The envelope table stays PENDING: it is adopted by publishArrangement once
    // the engine has accepted the lane that names these sets, and discarded by
    // dropArrangement if it has not. A lane the ring refused must leave the
    // track's envelope table exactly as it found it.
    return arr;
}

void App::dropArrangement(const RtArrangement* arr) {
    // The envelope sets this lane named were built for it and nothing else ever
    // borrowed them, so they go with it. Done first, and unconditionally, so a
    // null lane (a track whose items all resolved to nothing) still cleans up.
    for (const ArrAutoPub& p : pendingArrPubs_) dropAutos(p.set);
    pendingArrPubs_.clear();
    if (!arr) return;
    auto it = arr_.retiring.begin();
    for (; it != arr_.retiring.end(); ++it) if (*it == arr) break;
    if (it != arr_.retiring.end()) {
        // Refusing rather than freeing: a lane in `retiring` has been borrowed,
        // and dropArrangement's whole contract is "nothing ever borrowed it".
        LOGW("dropArrangement for a lane %p the engine has already borrowed - "
             "leaving it to its retirement", (const void*)arr);
        return;
    }
    for (const RtArrangement* p : arr_.published) {
        if (p != arr) continue;
        LOGW("dropArrangement for a lane %p that is still published - leaving it "
             "alone rather than freeing memory the engine is reading", (const void*)arr);
        return;
    }
    freeBlock(arr);
}

// §3.7, verbatim.
void App::publishArrangement(int track) {
    const int cell = arrCell(track);
    if (cell < 0 || cell == kMaxTracks) return;   // the transport cell has its own path

    const RtArrangement* fresh = buildArrangement(track);
    Command c;
    c.type = Cmd::SetArrangement;
    c.a = track;
    c.p = const_cast<RtArrangement*>(fresh);
    if (!engine_.pushCommand(c)) {
        // Nobody borrowed it. The track keeps whatever lane the engine already
        // holds, and the next edit tries again.
        LOGW("command ring full - track %d's arrangement not updated", track);
        dropArrangement(fresh);
        return;
    }
    const RtArrangement* old = arr_.published[(size_t)cell];
    arr_.published[(size_t)cell] = fresh;
    // The engine only announces a *replaced* lane, and only when it differs from
    // the incoming one; an entry that would never be announced must not be queued
    // for a retirement that will never arrive.
    if (old && old != fresh) arr_.retiring.push_back(old);

    // And now the envelope table the new lane names. The sets the OLD lane named
    // are displaced and wait for their own Ev::AutosRetired — which §3.7 requires
    // to arrive AFTER the lane that named them has come home, since the lane's
    // RtClip::autos points at them. That ordering is the engine half's to honour
    // and is called out in the report.
    std::vector<ArrAutoPub> displaced;
    displaced.swap(publishedArrAutos_[(size_t)track]);
    publishedArrAutos_[(size_t)track].swap(pendingArrPubs_);
    pendingArrPubs_.clear();
    for (const ArrAutoPub& p : displaced) retiringAutos_.push_back(p.set);
}

// ---------------------------------------------------------------------------
// the transport cell (§3.6)
//
// Cmd::SetArrangement with a = -1. Its RtArrangement carries NO items, NO clips
// and NO notes — only loopStart / loopEnd / loopOn — so the "one allocation" is
// the struct and nothing else, and the block arithmetic above is not involved.
//
// It rides this table rather than a Cmd::SetLoop because `Command` has one f64
// and a brace is two numbers plus a flag: widening Command would grow every
// entry of a 1024-deep ring and break WireCommand, which is 32 B, pointer-free
// and load-bearing in the region layout hash. The cell is idempotent, which is
// what makes republish-after-engine-restart a memcpy.
//
// `loopStart >= loopEnd` is published UNCHANGED rather than repaired: §3.6 makes
// that state disable the loop, and a publisher that clamped it would invent a
// length the user did not ask for. Only non-finite values are replaced.
// ---------------------------------------------------------------------------

void App::publishTransportCell() {
    char* mem = new (std::nothrow) char[sizeof(RtArrangement)];
    if (!mem) { status_ = "Out of memory - loop brace not updated"; return; }
    RtArrangement* cellv = new (mem) RtArrangement();
    cellv->items     = nullptr;
    cellv->clips     = nullptr;
    cellv->itemCount = 0;
    cellv->clipCount = 0;
    cellv->noteCount = 0;
    cellv->loopStart = finiteOr(ses_.loopStart, 0.0);
    cellv->loopEnd   = finiteOr(ses_.loopEnd, 0.0);
    cellv->loopOn    = ses_.loopOn ? 1u : 0u;

    Command c;
    c.type = Cmd::SetArrangement;
    c.a = -1;
    c.p = cellv;
    if (!engine_.pushCommand(c)) {
        LOGW("command ring full - loop brace not updated");
        freeBlock(cellv);
        return;
    }
    const size_t cell = (size_t)kMaxTracks;
    const RtArrangement* old = arr_.published[cell];
    arr_.published[cell] = cellv;
    if (old && old != cellv) arr_.retiring.push_back(old);
}

// ---------------------------------------------------------------------------
// arrangement automation (§6.2, §6.6)
//
// buildAutos with three differences, and no fourth: it reads
// TrackModel::arrangeAutos, it resolves against the track itself (so the scope
// check of AUTOMATION.md §4.2 step 2 is trivially satisfied — a track lane on
// track T names track T), and the lane array is variable-width and lives inside
// the block.
//
// ADDRESS RESOLUTION IS resolveAutoLane, unchanged and unduplicated. There is
// deliberately no second resolver: two resolvers are two things that have to
// keep agreeing about which chain slot a device sits in, and a devSlot that
// disagrees with the chain automates the wrong plugin — a silent, audible,
// untraceable wrong answer.
//
// The beats are ABSOLUTE (§6.1). Nothing here transforms them; the difference
// between a clip envelope's clip-relative beat and a track lane's timeline beat
// is entirely in what the evaluator is handed at evaluation time.
// ---------------------------------------------------------------------------

// The lane ceiling is the smaller of what the model admits and what the wire
// admits. They are equal today and this says so out loud, so that changing one
// alone is a compile error rather than a truncation nobody sees.
static_assert(kMaxArrLanes == kMaxRtArrLanes, "the model and the wire disagree about lanes");

const RtAutoSetN* App::buildArrangeAutos(int track) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return nullptr;
    const TrackModel& tr = ses_.tracks[(size_t)track];
    if (tr.arrangeAutos.empty()) return nullptr;

    // Resolve first, allocate second: how many lanes survive resolution is what
    // decides whether there is anything to allocate at all. An address that
    // resolves to nothing publishes NO lane — PARAM-ADDRESS.md's fail-soft rule,
    // the same one a clip envelope naming a deleted device gets.
    RtAutoLane lanes[kMaxRtArrLanes];
    int modelLane[kMaxRtArrLanes];
    int laneCount = 0, pointCount = 0;
    for (size_t i = 0; i < tr.arrangeAutos.size() && laneCount < kMaxRtArrLanes; ++i) {
        const AutoLane& L = tr.arrangeAutos[i];
        // A deactivated lane and an empty one are both UI state rather than
        // content: neither has anything to apply, and an empty lane evaluates to
        // the fallback anyway, so publishing one buys nothing.
        if (!L.enabled || L.points.empty()) continue;
        RtAutoLane rl;
        if (!resolveAutoLane(track, L.address, rl)) continue;
        int n = (int)L.points.size();
        if (pointCount + n > kMaxArrPoints) {
            n = kMaxArrPoints - pointCount;
            LOGW("track %d exceeds %d arrangement automation points - the tail of "
                 "'%s' will not be applied", track, kMaxArrPoints, L.address.c_str());
        }
        if (n <= 0) break;
        rl.first = pointCount;
        rl.count = n;
        rl.flags = 0;
        lanes[laneCount] = rl;
        modelLane[laneCount] = (int)i;
        ++laneCount;
        pointCount += n;
    }
    if (laneCount == 0) return nullptr;

    // ONE allocation: [RtAutoSetN][RtAutoLane[laneCount]][RtAutoPoint[pointCount]].
    // The alignUp on the point offset is load-bearing here and not defensive:
    // sizeof(RtAutoLane) is 36, so an odd lane count leaves the points on a
    // 4-byte boundary, and RtAutoPoint begins with an f64.
    const size_t lanesOff = alignUp(sizeof(RtAutoSetN), alignof(RtAutoLane));
    const size_t ptsOff   = alignUp(lanesOff + (size_t)laneCount * sizeof(RtAutoLane),
                                    alignof(RtAutoPoint));
    const size_t bytes    = ptsOff + (size_t)pointCount * sizeof(RtAutoPoint);

    char* mem = new (std::nothrow) char[bytes];
    if (!mem) {
        status_ = "Out of memory - arrangement automation not updated";
        return nullptr;
    }
    RtAutoSetN* set  = new (mem) RtAutoSetN();
    RtAutoLane* lv   = reinterpret_cast<RtAutoLane*>(mem + lanesOff);
    RtAutoPoint* pts = reinterpret_cast<RtAutoPoint*>(mem + ptsOff);

    int w = 0;
    for (int l = 0; l < laneCount; ++l) {
        const AutoLane& L = tr.arrangeAutos[(size_t)modelLane[l]];
        for (int k = 0; k < lanes[l].count; ++k) {
            const AutoPoint& src = L.points[(size_t)k];
            // Copied in MODEL ORDER, not sorted: the editor holds the sorted
            // invariant and the publisher preserves what it is given, which is
            // the same rule the file format follows. Non-finite values are the
            // one thing replaced, because a NaN beat is a hang in a linear scan.
            new (&pts[w]) RtAutoPoint();
            pts[w].beat  = std::isfinite(src.beat) ? std::max(0.0, src.beat) : 0.0;
            pts[w].value = std::isfinite(src.value) ? src.value : 0.f;
            pts[w].curve = src.curve;
            ++w;
        }
        new (&lv[l]) RtAutoLane(lanes[l]);
    }
    set->points     = pts;
    set->lanes      = lv;
    set->laneCount  = laneCount;
    set->pointCount = pointCount;
    return set;
}

void App::dropArrangeAutos(const RtAutoSetN* set) {
    if (!set) return;
    auto it = arrAutos_.retiring.begin();
    for (; it != arrAutos_.retiring.end(); ++it) if (*it == set) break;
    if (it != arrAutos_.retiring.end()) {
        LOGW("dropArrangeAutos for a set %p the engine has already borrowed - "
             "leaving it to its retirement", (const void*)set);
        return;
    }
    for (const RtAutoSetN* p : arrAutos_.published) {
        if (p != set) continue;
        LOGW("dropArrangeAutos for a set %p that is still published - leaving it "
             "alone rather than freeing memory the engine is reading", (const void*)set);
        return;
    }
    freeBlock(set);
}

void App::publishArrangeAutos(int track) {
    if (track < 0 || track >= kMaxTracks) return;
    const RtAutoSetN* fresh = buildArrangeAutos(track);
    Command c;
    c.type = Cmd::SetTrackAutos;
    c.a = track;
    c.p = const_cast<RtAutoSetN*>(fresh);
    if (!engine_.pushCommand(c)) {
        LOGW("command ring full - track %d's arrangement automation not updated", track);
        dropArrangeAutos(fresh);
        return;
    }
    const RtAutoSetN* old = arrAutos_.published[(size_t)track];
    arrAutos_.published[(size_t)track] = fresh;
    if (old && old != fresh) arrAutos_.retiring.push_back(old);
}

void App::publishArrangementFor(int track) {
    publishArrangement(track);
    publishArrangeAutos(track);
}

void App::publishArrangementAll() {
    for (int t = 0; t < (int)ses_.tracks.size() && t < kMaxTracks; ++t)
        publishArrangementFor(t);
    // Tracks the current session no longer has still hold whatever was last
    // published into them — an RtArrangement pointing at a SampleBuffer this
    // session has stopped owning. releaseStaleSlots makes the same argument for
    // clip slots; this is that argument for lanes.
    for (int t = (int)ses_.tracks.size(); t < kMaxTracks; ++t) {
        if (!arr_.published[(size_t)t] && !arrAutos_.published[(size_t)t]) continue;
        publishArrangementFor(t);        // an empty model publishes null, which clears
    }
    publishTransportCell();
}

// ---------------------------------------------------------------------------
// the reaper (§3.7)
//
// One line inside pumpEngineEvents() — `if (reapArrangementEvent(e)) continue;`
// — reaches this, because that function lives in a file this milestone does not
// own. Both events are the same handshake for the fifth and sixth time, and the
// same refusal to free a pointer we have no record of owning: a bad free here
// would be a use-after-free in whoever DOES own it, which is strictly worse than
// the leak this takes instead.
// ---------------------------------------------------------------------------

bool App::reapArrangementEvent(const Event& e) {
    if (e.type == Ev::ArrangementRetired) {
        const RtArrangement* old = (const RtArrangement*)e.p;
        if (!old) return true;
        auto it = arr_.retiring.begin();
        for (; it != arr_.retiring.end(); ++it) if (*it == old) break;
        if (it == arr_.retiring.end()) {
            LOGW("ArrangementRetired for an unknown lane %p (cell %d) - leaking it "
                 "rather than freeing a pointer we do not own", (const void*)old, e.a);
            return true;
        }
        arr_.retiring.erase(it);
        // The free is delete[] on the char[] the lane was placement-new'd into:
        // one allocation, one pointer, one delete[], with the items, the clips
        // and every clip's notes inside the same block. See freeBlock.
        freeBlock(old);
        return true;
    }
    if (e.type == Ev::TrackAutosRetired) {
        const RtAutoSetN* old = (const RtAutoSetN*)e.p;
        if (!old) return true;
        auto it = arrAutos_.retiring.begin();
        for (; it != arrAutos_.retiring.end(); ++it) if (*it == old) break;
        if (it == arrAutos_.retiring.end()) {
            LOGW("TrackAutosRetired for an unknown set %p (track %d) - leaking it "
                 "rather than freeing a pointer we do not own", (const void*)old, e.a);
            return true;
        }
        arrAutos_.retiring.erase(it);
        freeBlock(old);
        return true;
    }
    return false;
}

// The two destructors, out of line so this file is the only one that has to know
// what the allocations are. App is destroyed after shutdown() has joined the
// audio thread — the same moment shutdown() frees the note arrays, and for the
// same reason — so what these free is what the engine still held when the
// process ended plus anything whose retirement event was never drained.
App::ArrPubs::~ArrPubs() {
    for (const RtArrangement*& a : published) { freeBlock(a); a = nullptr; }
    for (const RtArrangement* a : retiring) freeBlock(a);
    retiring.clear();
}

App::ArrAutoPubs::~ArrAutoPubs() {
    for (const RtAutoSetN*& s : published) { freeBlock(s); s = nullptr; }
    for (const RtAutoSetN* s : retiring) freeBlock(s);
    retiring.clear();
}

} // namespace lat
