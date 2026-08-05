// Piano roll: the MIDI note editor shown in the CLIP tab.
//
// Layout, top to bottom: a ruler (bar.beat numbers, FOLD, loop length), the
// note grid with a keyboard column on its left, then the velocity lane. Every
// pixel <-> musical conversion goes through the two axis structs below, so the
// drawing pass and the hit testing can never disagree about where a note is —
// which is the entire reason the editing code can be this short.
//
// Everything above `PianoRoll::draw` is pure: no Ui, no Renderer, no member
// state. That is deliberate — the grid mapping, the fold row set and the edit
// clamps are the parts that are worth testing without a window.
#include "pianoroll.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lat {
namespace {

// ---------------------------------------------------------------------------
// constants (logical px unless noted; multiply by the DPI scale)
// ---------------------------------------------------------------------------

constexpr f64 kGridStep   = 0.25;   // 1/16 note, the only grid this wave
constexpr int kBeatsPerBar = 4;     // no time signature reaches us; assume 4/4
constexpr int kMinFoldRows = 8;     // a one-note clip still needs room to click
constexpr f32 kKeyW       = 46.f;
constexpr f32 kRulerH     = 16.f;
constexpr f32 kLaneH      = 54.f;
constexpr f32 kRowH       = 12.f;
// Fit-to-width, the zoom a clip is first shown at, is kept inside a sane band:
// a two-beat sketch must not draw beats a hand-span apart, and a 64-bar clip
// must not open at one pixel per bar.
constexpr f32 kPxPerBeatMin = 44.f;
constexpr f32 kPxPerBeatMax = 128.f;
// Ctrl+wheel reaches much further in both directions than the fit ever does:
// far enough out to see a long pattern whole, far enough in to place a note
// against the grid line rather than near it.
constexpr f32 kZoomMin      = 8.f;
constexpr f32 kZoomMax      = 512.f;
constexpr f32 kZoomPerNotch = 0.25f;  // octaves of zoom per wheel notch
constexpr int kCentrePitch  = 60;   // C4, the middle of the default C3..C5 view
constexpr f64 kMaxLoopBeats = 64.0; // ceiling for Ctrl+U, 16 bars in 4/4

// ---------------------------------------------------------------------------
// axes
// ---------------------------------------------------------------------------

// `view` is the scroll offset in content pixels; `x0`/`y0` the screen origin of
// the grid. Both directions are affine, so one struct each covers draw and hit.
struct TimeAxis {
    f32 x0 = 0, pxPerBeat = 64.f, view = 0;
};
inline f32 beatToX(const TimeAxis& a, f64 b) { return a.x0 - a.view + (f32)(b * (f64)a.pxPerBeat); }
inline f64 xToBeat(const TimeAxis& a, f32 x) { return (f64)(x - a.x0 + a.view) / (f64)a.pxPerBeat; }

// Scroll offset for a zoom that must leave the beat under `anchorX` still under
// `anchorX` — the only zoom that feels like the content is being magnified
// rather than shuffled. Clamped like every other scroll, so within a view of
// either end the anchor gives way to the content edge (there is no scroll that
// satisfies both, and showing empty space past the loop is the worse answer).
inline f32 zoomView(const TimeAxis& a, f32 newPxPerBeat, f32 anchorX, f64 lenBeats, f32 viewW) {
    const f64 b = xToBeat(a, anchorX);
    const f32 view = a.x0 + (f32)(b * (f64)newPxPerBeat) - anchorX;
    return clampv(view, 0.f, std::max(0.f, (f32)(lenBeats * (f64)newPxPerBeat) - viewW));
}

struct PitchAxis {
    f32 y0 = 0, rowH = 12.f, view = 0;
};
inline f32 rowToY(const PitchAxis& a, int row) { return a.y0 - a.view + (f32)row * a.rowH; }
inline int yToRow(const PitchAxis& a, f32 y) {
    return (int)std::floor((y - a.y0 + a.view) / a.rowH);
}

// ---------------------------------------------------------------------------
// row set (fold)
// ---------------------------------------------------------------------------

// Row 0 is the top row = the highest pitch. Unfolded this is just 127 - pitch;
// folded it collapses the gaps, which is why every pitch lookup has to go
// through here instead of doing the arithmetic inline.
struct RowMap {
    int pitchOf[128]{};
    int rowOfP[128]{};
    int count = 0;
    int pitchAt(int row) const { return (row >= 0 && row < count) ? pitchOf[row] : -1; }
    int rowOf(int pitch) const { return (pitch >= 0 && pitch < 128) ? rowOfP[pitch] : -1; }
};

// `keepPitch` is the pitch a move drag started on: it stays on screen for the
// whole gesture even after the note leaves it, otherwise the row under the
// cursor would renumber mid-drag and the note would jump.
RowMap buildRows(const std::vector<NoteModel>& notes, bool fold, int keepPitch) {
    RowMap m;
    for (int p = 0; p < 128; ++p) m.rowOfP[p] = -1;

    bool used[128]{};
    int n = 0;
    if (fold) {
        for (const NoteModel& nt : notes)
            if (nt.pitch < 128 && !used[nt.pitch]) { used[nt.pitch] = true; ++n; }
        if (keepPitch >= 0 && keepPitch < 128 && !used[keepPitch]) { used[keepPitch] = true; ++n; }
    }
    // An empty clip has nothing to fold to, so it falls back to the full range.
    if (!fold || n == 0) {
        m.count = 128;
        for (int i = 0; i < 128; ++i) { m.pitchOf[i] = 127 - i; m.rowOfP[127 - i] = i; }
        return m;
    }
    int lo = 0;   while (!used[lo]) ++lo;
    int hi = 127; while (!used[hi]) --hi;
    // Pad downwards first (a melody sits above its padding, like a keyboard).
    while (n < kMinFoldRows && (lo > 0 || hi < 127)) {
        if (lo > 0) { used[--lo] = true; }
        else        { used[++hi] = true; }
        ++n;
    }
    for (int p = 127; p >= 0; --p)
        if (used[p]) { m.pitchOf[m.count] = p; m.rowOfP[p] = m.count; ++m.count; }
    return m;
}

inline bool isBlackKey(int pitch) {
    const int pc = ((pitch % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// ---------------------------------------------------------------------------
// edit primitives
// ---------------------------------------------------------------------------

inline f64 quantFloor(f64 b) { return std::floor(b / kGridStep) * kGridStep; }
inline f64 quantNear(f64 b)  { return std::floor(b / kGridStep + 0.5) * kGridStep; }

// Snapped note start, kept inside the clip at both ends.
inline f64 clampBeat(f64 raw, f64 len, f64 lengthBeats) {
    const f64 hi = std::max(0.0, lengthBeats - len);
    return clampv(quantNear(raw), 0.0, hi);
}

// Length from a dragged right edge: whole grid steps, never under one step and
// never past the end of the clip.
inline f64 clampLen(f64 rawRight, f64 beat, f64 lengthBeats) {
    const f64 len  = std::max(kGridStep, quantNear(rawRight - beat));
    const f64 room = std::max(kGridStep, lengthBeats - beat);
    return std::min(len, room);
}

inline bool noteLess(const NoteModel& a, const NoteModel& b) {
    return a.beat != b.beat ? a.beat < b.beat : a.pitch < b.pitch;
}
inline bool sameNote(const NoteModel& a, const NoteModel& b) {
    return a.beat == b.beat && a.pitch == b.pitch && a.len == b.len && a.vel == b.vel;
}

// Restores the sorted-by-beat invariant and reports where `key` ended up. An
// index is the only handle we have on a note, so any edit that can reorder the
// vector has to re-find the note it just touched. Two notes that compare equal
// on all four fields are interchangeable, so picking the first match is safe.
int sortTracking(std::vector<NoteModel>& v, const NoteModel& key) {
    std::sort(v.begin(), v.end(), noteLess);
    for (size_t i = 0; i < v.size(); ++i)
        if (sameNote(v[i], key)) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// the selection, as a set
// ---------------------------------------------------------------------------

// A selection expressed as *notes* rather than indices — the only form of it
// that survives a re-sort. `primary` is a slot in `notes`, or -1.
struct SelKeys {
    std::vector<NoteModel> notes;
    int primary = -1;
};

// sortTracking for a whole set. Two notes that compare equal on all four
// fields are interchangeable, but they are still two notes: each key claims a
// slot of its own, so a selection holding both never collapses onto one index
// (and then moves that one note twice as far on the next drag). The search
// starts at the sorted position of the key, so this stays near-linear even
// when the whole clip is selected and a group drag re-runs it every frame.
//
// `outSel` comes back sorted ascending; a key whose note no longer exists is
// simply dropped, which is what makes this safe to run after a trim.
void sortTrackingSet(std::vector<NoteModel>& v, const SelKeys& keys,
                     std::vector<int>& outSel, int& outPrimary) {
    std::sort(v.begin(), v.end(), noteLess);
    outSel.clear();
    outPrimary = -1;
    const size_t n = v.size(), k = keys.notes.size();
    if (k == 0 || n == 0) return;
    std::vector<bool> taken(n, false);
    for (size_t j = 0; j < k; ++j) {
        const NoteModel& key = keys.notes[j];
        size_t i = (size_t)(std::lower_bound(v.begin(), v.end(), key, noteLess) - v.begin());
        for (; i < n; ++i) {
            if (noteLess(key, v[i])) break;          // past every equal-ordering note
            if (taken[i] || !sameNote(v[i], key)) continue;
            taken[i] = true;
            outSel.push_back((int)i);
            if ((int)j == keys.primary) outPrimary = (int)i;
            break;
        }
    }
    std::sort(outSel.begin(), outSel.end());
}

// How far a selection can travel before one of its members hits a wall: the
// start of the clip, its end, pitch 0 or pitch 127. Clamping the *group* this
// way rather than each note on its own is the whole difference between a group
// move that keeps its shape and one that piles up against the edge.
struct GroupRoom {
    f64  left = 0.0, right = 0.0;   // beats: most negative / most positive move
    int  down = 0, up = 0;          // semitones
    bool empty = true;
};
GroupRoom groupRoom(const std::vector<NoteModel>& notes, const std::vector<int>& sel,
                    f64 lengthBeats) {
    GroupRoom g;
    f64 minBeat = 0.0, maxEnd = 0.0;
    int minP = 0, maxP = 0;
    for (int i : sel) {
        if (i < 0 || i >= (int)notes.size()) continue;
        const NoteModel& nt = notes[(size_t)i];
        if (g.empty) {
            minBeat = nt.beat; maxEnd = nt.beat + nt.len;
            minP = maxP = (int)nt.pitch;
            g.empty = false;
        } else {
            minBeat = std::min(minBeat, nt.beat);
            maxEnd  = std::max(maxEnd, nt.beat + nt.len);
            minP    = std::min(minP, (int)nt.pitch);
            maxP    = std::max(maxP, (int)nt.pitch);
        }
    }
    if (g.empty) return g;
    g.left  = -minBeat;
    // A group that already hangs past the end of the clip (an over-long note,
    // or a loop dragged shorter under it) has *negative* room to the right, and
    // taking that as the clamp pulls it back inside — which is exactly what the
    // single-note clamp does. It can never pull further left than beat 0.
    g.right = std::max(g.left, lengthBeats - maxEnd);
    g.down  = -minP;
    g.up    = 127 - maxP;
    return g;
}

// A group move, already clamped. Both fields are deltas, not destinations.
struct GroupDelta {
    f64 beats = 0.0;
    int semis = 0;
};
GroupDelta clampGroupDelta(const std::vector<NoteModel>& notes, const std::vector<int>& sel,
                           f64 dBeats, int dSemis, f64 lengthBeats) {
    GroupDelta d;
    const GroupRoom g = groupRoom(notes, sel, lengthBeats);
    if (g.empty) return d;
    d.beats = clampv(dBeats, g.left, g.right);
    d.semis = clampv(dSemis, g.down, g.up);
    return d;
}

// Applies a clamped delta to every selected note and restores the sorted-by-
// beat invariant, re-deriving `sel` and `primary` through it. Returns false
// when the delta was zero (a group already against both walls), in which case
// nothing was touched and the caller must not report a change.
//
// Note that only the group's *extremes* were clamped: the members are moved by
// the same delta, so the shape of a chord or a riff is preserved exactly.
bool applyGroupDelta(std::vector<NoteModel>& notes, std::vector<int>& sel, int& primary,
                     const GroupDelta& d) {
    if (d.beats == 0.0 && d.semis == 0) return false;
    SelKeys keys;
    keys.notes.reserve(sel.size());
    for (int i : sel) {
        if (i < 0 || i >= (int)notes.size()) continue;
        NoteModel& nt = notes[(size_t)i];
        nt.beat  = nt.beat + d.beats;
        nt.pitch = (u8)clampv((int)nt.pitch + d.semis, 0, 127);
        if (i == primary) keys.primary = (int)keys.notes.size();
        keys.notes.push_back(nt);
    }
    if (keys.notes.empty()) return false;
    sortTrackingSet(notes, keys, sel, primary);
    return true;
}

// Keyboard nudge: `steps` grid steps along time, `semis` semitones of pitch,
// applied to every selected note. Both clamped — into the clip at both ends,
// into 0..127 — once for the group.
//
// The time nudge goes through the same snap as a mouse move, measured on the
// PRIMARY note: nudging an off-grid note (one that arrived by MIDI recording)
// pulls it onto the grid rather than carrying the offset along forever, and a
// selection is pulled onto the grid by its anchor while keeping its internal
// spacing. For a one-note selection this is exactly clampBeat, note for note.
//
// `changed` is false when the nudge changed nothing at all (a group already
// against the clamp); otherwise `sel` and `primary` come back re-derived.
struct NudgeResult {
    bool changed = false;
    bool pitchChanged = false;
};
NudgeResult nudgeGroup(std::vector<NoteModel>& notes, std::vector<int>& sel, int& primary,
                       int steps, int semis, f64 lengthBeats) {
    NudgeResult res;
    if (sel.empty()) return res;
    const int anchor = (primary >= 0 && primary < (int)notes.size()) ? primary : sel.front();
    if (anchor < 0 || anchor >= (int)notes.size()) return res;
    const f64 aBeat = notes[(size_t)anchor].beat;
    const f64 want = steps != 0 ? quantNear(aBeat + (f64)steps * kGridStep) - aBeat : 0.0;
    const GroupDelta d = clampGroupDelta(notes, sel, want, semis, lengthBeats);
    if (!applyGroupDelta(notes, sel, primary, d)) return res;
    res.changed = true;
    res.pitchChanged = d.semis != 0;
    return res;
}

// Live's duplicate-loop: the loop doubles and everything in it is copied one
// old-length later, so a bar of material becomes two bars of it. The selection
// follows into the *copy* — the whole set does, note for note — because the
// copy is what the user is about to edit.
//
// The cap is a length, not a factor: doubling a 40-beat loop gives 64 and the
// copies that would start past the new end are simply not made (a note that
// straddles the end is trimmed). A selected note whose copy was not made keeps
// the selection on the original, so the set never silently shrinks. Nothing
// happens at all once the loop is already at the cap — a no-op that reports
// false, so the caller does not re-push an unchanged clip.
struct DupResult {
    bool changed = false;
    std::vector<int> sel;
    int  primary = -1;
};
DupResult duplicateLoopNotes(std::vector<NoteModel>& notes, f64& lengthBeats,
                             const std::vector<int>& selected, int primary) {
    DupResult res;
    const f64 oldLen = std::max(kGridStep, lengthBeats);
    if (oldLen >= kMaxLoopBeats) return res;
    const f64 newLen = std::min(kMaxLoopBeats, oldLen * 2.0);

    // The notes the selection should end on, tracked as notes rather than
    // indices: a bare sort would leave the caller holding indices into the old
    // order. Each starts as the original and is overwritten by its copy if one
    // gets made.
    const size_t n = notes.size();
    SelKeys keys;
    std::vector<int> slotOf(n, -1);          // note index -> slot in keys
    for (int i : selected) {
        if (i < 0 || i >= (int)n) continue;
        slotOf[(size_t)i] = (int)keys.notes.size();
        if (i == primary) keys.primary = (int)keys.notes.size();
        keys.notes.push_back(notes[(size_t)i]);
    }
    for (size_t i = 0; i < n; ++i) {
        NoteModel c = notes[i];
        c.beat += oldLen;
        if (c.beat >= newLen - 1e-9) continue;
        c.len = std::min(c.len, newLen - c.beat);
        if (slotOf[i] >= 0) keys.notes[(size_t)slotOf[i]] = c;
        notes.push_back(c);
    }
    lengthBeats = newLen;
    res.changed = true;
    // The vector is two sorted runs (originals, then copies, each in order and
    // the second entirely later), which the re-sort inside here restores.
    sortTrackingSet(notes, keys, res.sel, res.primary);
    return res;
}

// Screen span of a note along the time axis, including the minimum width the
// grid draws a very short note at. The hit tests and the drawing have to agree
// about where a note is, so both come through here.
inline void noteSpanX(const NoteModel& nt, const TimeAxis& ta, f32 minW, f32& x0, f32& x1) {
    x0 = beatToX(ta, nt.beat);
    x1 = std::max(beatToX(ta, nt.beat + nt.len), x0 + minW);
}

// Every note whose block touches `band`, in index order. Touching counts: a
// band that grazes an edge takes the note, and a band with no height (dragged
// straight along one row, which is the commonest way to sweep a line of notes)
// still takes the row it is on. Notes on a pitch the row map does not contain
// — folded away — cannot be banded, since they are not on screen to be swept.
void notesInBand(const std::vector<NoteModel>& notes, const RowMap& rows,
                 const TimeAxis& ta, const PitchAxis& pa, const Rect& band, f32 minW,
                 std::vector<int>& out) {
    out.clear();
    for (size_t i = 0; i < notes.size(); ++i) {
        const int row = rows.rowOf(notes[i].pitch);
        if (row < 0) continue;
        f32 x0 = 0.f, x1 = 0.f;
        noteSpanX(notes[i], ta, minW, x0, x1);
        if (x1 < band.x || x0 > band.right()) continue;
        const f32 y0 = rowToY(pa, row), y1 = y0 + pa.rowH;
        if (y1 < band.y || y0 > band.bottom()) continue;
        out.push_back((int)i);
    }
}

// Topmost note under a point, or -1. Later notes win so the hit order matches
// the draw order.
int noteAt(const std::vector<NoteModel>& notes, const RowMap& rows,
           const TimeAxis& ta, const PitchAxis& pa, f32 mx, f32 my, f32 minW) {
    const int pitch = rows.pitchAt(yToRow(pa, my));
    if (pitch < 0) return -1;
    int found = -1;
    for (size_t i = 0; i < notes.size(); ++i) {
        if ((int)notes[i].pitch != pitch) continue;
        f32 x0 = 0.f, x1 = 0.f;
        noteSpanX(notes[i], ta, minW, x0, x1);
        if (mx >= x0 && mx < x1) found = (int)i;
    }
    return found;
}

// ---------------------------------------------------------------------------
// DPI
// ---------------------------------------------------------------------------

// The renderer keeps its DPI scale private and draw() is handed only a Rect, so
// recover the scale from the font: App loads fSmall at round(9 * dpiScale) px
// and fBody at round(11 * dpiScale). Rounding costs at most ~5% at 1x, which is
// invisible in layout maths and cheaper than widening the frozen interface.
f32 dpiOf(const Ui& ui) {
    if (ui.fSmall && ui.fSmall->size() > 0) return std::max(0.5f, (f32)ui.fSmall->size() / 9.f);
    if (ui.fBody  && ui.fBody->size()  > 0) return std::max(0.5f, (f32)ui.fBody->size()  / 11.f);
    return 1.f;
}

} // namespace

// ---------------------------------------------------------------------------
// PianoRoll: the selection set
//
// Small, sorted and unique, with one member singled out as the primary — the
// note the last gesture was about, which is what the view follows and what the
// audition plays. Every path that can invalidate an index goes through one of
// these, so there is exactly one place where the set can get out of step with
// clip.notes.
// ---------------------------------------------------------------------------

bool PianoRoll::selHas(int i) const {
    return i >= 0 && std::binary_search(sel_.begin(), sel_.end(), i);
}

void PianoRoll::selClear() {
    sel_.clear();
    primary_ = -1;
}

void PianoRoll::selOne(int i) {
    sel_.clear();
    if (i >= 0) sel_.push_back(i);
    primary_ = i >= 0 ? i : -1;
}

void PianoRoll::selAdd(int i) {
    if (i < 0) return;
    const auto it = std::lower_bound(sel_.begin(), sel_.end(), i);
    if (it != sel_.end() && *it == i) return;
    sel_.insert(it, i);
    if (primary_ < 0) primary_ = i;
}

void PianoRoll::selToggle(int i) {
    if (i < 0) return;
    const auto it = std::lower_bound(sel_.begin(), sel_.end(), i);
    if (it != sel_.end() && *it == i) {
        sel_.erase(it);
        // The primary has to stay inside the set; which member inherits it does
        // not matter, only that one does while there is one to have it.
        if (primary_ == i) primary_ = sel_.empty() ? -1 : sel_.front();
        return;
    }
    sel_.insert(it, i);
    primary_ = i;                       // the note just added is under the hand
}

void PianoRoll::selErased(int at) {
    for (size_t k = 0; k < sel_.size();) {
        if (sel_[k] == at)      sel_.erase(sel_.begin() + (long)k);
        else                  { if (sel_[k] > at) --sel_[k]; ++k; }
    }
    if (primary_ == at)     primary_ = sel_.empty() ? -1 : sel_.front();
    else if (primary_ > at) --primary_;
}

void PianoRoll::selPrune(int noteCount) {
    while (!sel_.empty() && sel_.back() >= noteCount) sel_.pop_back();
    if (sel_.empty())         primary_ = -1;
    else if (!selHas(primary_)) primary_ = sel_.front();
}

// ---------------------------------------------------------------------------
// PianoRoll
// ---------------------------------------------------------------------------

bool PianoRoll::draw(Ui& ui, const Rect& r, ClipModel& clip, f64 playheadBeats, bool playing) {
    if (!ui.r || !ui.in) return false;
    Renderer& rr = *ui.r;
    Input& in = *ui.in;
    const f32 s = dpiOf(ui);
    bool changed = false;

    rr.rect(r, pal::appBg);
    if (r.w < 140.f * s || r.h < 70.f * s) return false;     // too small to be useful

    // --- layout ------------------------------------------------------------
    const f32 keyW = kKeyW * s, rowH = kRowH * s;
    const f32 laneH = std::min(kLaneH * s, r.h * 0.32f);
    const Rect ruler{r.x, r.y, r.w, kRulerH * s};
    const Rect body{r.x, ruler.bottom(), r.w, r.h - ruler.h - laneH - 1.f * s};
    const Rect keys{body.x, body.y, keyW, body.h};
    const Rect grid{body.x + keyW, body.y, body.w - keyW, body.h};
    const Rect lane{grid.x, body.bottom() + 1.f * s, grid.w, laneH};
    const Rect laneKey{r.x, lane.y, keyW, laneH};
    if (grid.w < 24.f * s || grid.h < rowH) return false;

    // The caller swaps the clip under us between frames (selecting another
    // slot), and everything the roll remembers — a selection index, where the
    // view sits, how far it is zoomed in — is about one particular clip. So the
    // whole lot resets when the identity changes, and the new clip is shown the
    // way a clip is first shown: fit to the width, nothing selected.
    if (clip.uid != clipUid_) {
        clipUid_ = clip.uid;
        selClear();
        bandBase_.clear();
        dragNote_ = -1;
        drag_ = Drag::None;
        scrollX_ = scrollY_ = 0.f;
        zoom_ = 0.f;                 // -> fit to width below
        addedLastPress_ = false;
        followSel_ = false;
        preview_.clear();            // an audition for a clip nobody is looking at
    }

    // A note count can still change under a live selection (undo, a MIDI take
    // finishing), so indices are re-checked on every frame regardless.
    const int noteCount = (int)clip.notes.size();
    selPrune(noteCount);
    if (dragNote_ >= noteCount) { dragNote_ = -1; drag_ = Drag::None; }

    // --- axes --------------------------------------------------------------
    const int keepPitch = (drag_ == Drag::Move && dragNote_ >= 0) ? dragPitch_ : -1;
    const RowMap rows = buildRows(clip.notes, fold_, keepPitch);

    const f64 lenBeats = std::max(1.0, clip.lengthBeats);
    // First sight of a clip: fit the loop to the width, so a pattern opens as
    // itself rather than as a window onto part of itself. From the first
    // Ctrl+wheel on, the zoom is the user's and nothing takes it back — not
    // even dragging the loop longer, which would otherwise re-fit under the
    // hand that was editing.
    if (zoom_ <= 0.f)
        zoom_ = clampv((f32)(grid.w / lenBeats) / s, kPxPerBeatMin, kPxPerBeatMax);
    f32 pxPerBeat = zoom_ * s;

    const u64 gridId = uiId(20, 0), laneId = uiId(20, 1);
    const bool overBody = (grid.contains(in.mx, in.my) || lane.contains(in.mx, in.my) ||
                           keys.contains(in.mx, in.my)) &&
                          rr.currentClip().contains(in.mx, in.my);
    if (overBody && in.wheel != 0.f) {
        if (in.ctrl()) {
            // Zoom about the cursor. Exponential in the wheel so a notch is the
            // same *proportion* everywhere, which is the only way one gesture
            // covers 8..512 px/beat without feeling geared wrong at one end.
            const f32 nz = clampv(zoom_ * std::pow(2.f, in.wheel * kZoomPerNotch),
                                  kZoomMin, kZoomMax);
            if (nz != zoom_) {
                const TimeAxis prev{grid.x, pxPerBeat, scrollX_};
                zoom_ = nz;
                pxPerBeat = zoom_ * s;
                // Off in the keyboard column there is no beat under the cursor,
                // so the left edge of the grid anchors instead.
                scrollX_ = zoomView(prev, pxPerBeat, clampv(in.mx, grid.x, grid.right()),
                                    lenBeats, grid.w);
            }
        } else if (in.shift()) {
            scrollX_ -= in.wheel * pxPerBeat * 0.5f;
        } else {
            scrollY_ -= in.wheel * rowH * 3.f;
        }
    }

    const f32 contentW = (f32)(lenBeats * (f64)pxPerBeat);
    const f32 contentH = (f32)rows.count * rowH;

    scrollX_ = clampv(scrollX_, 0.f, std::max(0.f, contentW - grid.w));
    // scrollY_ is an offset from the *default* view rather than from the top of
    // the content: 0 then means "centred on C3..C5" (or on the folded rows) with
    // no first-frame flag, which the frozen header has no room for. The clamp is
    // written back so scrolling never has a dead zone at either end.
    const int centreRow = fold_ ? rows.count / 2 : rows.rowOf(kCentrePitch);
    const f32 anchorY = (f32)std::max(0, centreRow) * rowH + rowH * 0.5f - grid.h * 0.5f;
    f32 viewY = clampv(anchorY + scrollY_, 0.f, std::max(0.f, contentH - grid.h));

    // A keyboard edit can push the selected note out of the view — an octave
    // nudge usually does — and a note the user cannot see is a note they have
    // lost. The view follows it, by the smallest move that puts it back on
    // screen (clamping to a window it is already inside is a no-op). It follows
    // the *primary* note: a group can be taller and longer than the view, and
    // chasing all of it would mean choosing which part to lose anyway.
    if (followSel_) {
        followSel_ = false;
        if (primary_ >= 0 && primary_ < noteCount) {
            const NoteModel& sel = clip.notes[(size_t)primary_];
            const int row = rows.rowOf(sel.pitch);
            if (row >= 0) {
                const f32 top = (f32)row * rowH;
                viewY = clampv(viewY, top + rowH - grid.h, top);
                viewY = clampv(viewY, 0.f, std::max(0.f, contentH - grid.h));
            }
            const f32 nx0 = (f32)(sel.beat * (f64)pxPerBeat);
            const f32 nx1 = (f32)((sel.beat + sel.len) * (f64)pxPerBeat);
            // For a note wider than the view the two bounds cross; either way
            // out of clampv lands on part of the note, which is all that is
            // promised here.
            scrollX_ = clampv(scrollX_, nx1 - grid.w, nx0);
            scrollX_ = clampv(scrollX_, 0.f, std::max(0.f, contentW - grid.w));
        }
    }
    scrollY_ = viewY - anchorY;

    const TimeAxis ta{grid.x, pxPerBeat, scrollX_};
    const PitchAxis pa{grid.y, rowH, viewY};
    const f32 minNoteW = 3.f * s;

    // --- interaction -------------------------------------------------------
    ui.setHot(gridId, grid);
    ui.setHot(laneId, lane);
    const bool hotGrid = ui.isHot(gridId), hotLane = ui.isHot(laneId);

    auto eraseNote = [&](int i) {
        if (i < 0 || i >= (int)clip.notes.size()) return;
        clip.notes.erase(clip.notes.begin() + i);
        selErased(i);
        if (dragNote_ == i) { dragNote_ = -1; drag_ = Drag::None; }
        else if (dragNote_ > i) --dragNote_;
        changed = true;
    };
    // Velocity lane drags are absolute: the stem follows the cursor height, so a
    // plain click on the lane also sets the value, like clicking a fader track.
    auto velAt = [&](f32 y) {
        const f32 t = clampv((lane.bottom() - 2.f * s - y) / std::max(1.f, lane.h - 6.f * s), 0.f, 1.f);
        return (u8)clampv((int)std::lround(t * 127.f), 1, 127);
    };

    // The rubber band, when one is in flight: computed with the interaction and
    // drawn later, inside the grid's clip.
    Rect bandRect{};
    bool showBand = false;

    if (drag_ != Drag::None) {
        // The band is the one drag with no note under it.
        const bool needsNote = drag_ != Drag::Band;
        if (!in.down[0] ||
            (needsNote && (dragNote_ < 0 || dragNote_ >= (int)clip.notes.size()))) {
            drag_ = Drag::None;
            dragNote_ = -1;
            bandBase_.clear();
            if (ui.active == gridId || ui.active == laneId) ui.active = 0;
        } else if (drag_ == Drag::Band) {
            // Live, not on release: the selection is whatever the band touches
            // *now*, so dragging back over a note un-takes it and there is no
            // moment where what is highlighted and what is selected disagree.
            // The anchor is in content space, so a wheel mid-band leaves the
            // corner on the material it was put on rather than on a pixel.
            const f32 ax = beatToX(ta, bandBeat_);
            const f32 ay = grid.y - viewY + bandY_;
            bandRect = Rect{std::min(ax, in.mx), std::min(ay, in.my),
                            std::fabs(in.mx - ax), std::fabs(in.my - ay)};
            showBand = true;
            std::vector<int> hits;
            notesInBand(clip.notes, rows, ta, pa, bandRect, minNoteW, hits);
            // Shift means "and also" here as everywhere, so the band adds to
            // what was selected when it started — which also means a band that
            // touches nothing takes nothing away.
            sel_ = bandBase_;
            for (int i : hits) selAdd(i);
            if (!selHas(primary_)) primary_ = sel_.empty() ? -1 : sel_.front();
        } else if (drag_ == Drag::Move) {
            // The note under the hand is always part of what moves. It is put
            // there on the press, but Escape can empty the set from the
            // keyboard between frames while the button is still down, and a
            // drag that silently stopped moving anything would look like a
            // freeze rather than a cancel.
            if (!selHas(dragNote_)) selOne(dragNote_);
            const NoteModel nt = clip.notes[(size_t)dragNote_];   // copy: we sort below
            // Pitch follows whole rows travelled since the press. Both ends go
            // through the current axis, so wheeling mid-drag can shift the
            // result by at most the one row the sub-row phase moved by, rather
            // than sending the note off with the scroll.
            const int baseRow = rows.rowOf(dragPitch_);
            int np = (int)nt.pitch;
            if (baseRow >= 0) {
                const int row = clampv(baseRow + yToRow(pa, in.my) - yToRow(pa, dragY_),
                                       0, rows.count - 1);
                const int p = rows.pitchAt(row);
                if (p >= 0) np = p;
            }
            // The gesture is measured on the note under the hand and applied to
            // the whole selection as one delta, so a chord keeps its shape and
            // the group stops when its extreme member reaches a wall. With one
            // note selected this is the old clampBeat/pitch clamp exactly.
            const GroupDelta d = clampGroupDelta(
                clip.notes, sel_, quantNear(xToBeat(ta, in.mx) - dragBeat_) - nt.beat,
                np - (int)nt.pitch, clip.lengthBeats);
            if (d.beats != 0.0 || d.semis != 0) {
                // Dragging across rows plays what is under the note, the way a
                // note dragged in Live does: the pitch is the thing being
                // chosen, and choosing it by eye alone is guesswork. Only the
                // note under the hand is auditioned — thirty at once is noise,
                // not a chord.
                if (d.semis != 0) preview_.push((int)nt.pitch + d.semis);
                if (applyGroupDelta(clip.notes, sel_, primary_, d)) changed = true;
                dragNote_ = primary_;
                if (dragNote_ < 0) drag_ = Drag::None;
            }
        } else if (drag_ == Drag::Resize) {
            // Length stays a one-note edit: a group resize has to choose
            // between absolute and proportional lengths, and neither is what
            // the hand on one note's right edge asked for.
            NoteModel& nt = clip.notes[(size_t)dragNote_];
            const f64 nl = clampLen(xToBeat(ta, in.mx), nt.beat, clip.lengthBeats);
            if (nl != nt.len) { nt.len = nl; changed = true; }
        } else if (drag_ == Drag::Velocity) {
            // Absolute, and for the whole selection: every selected stem takes
            // the value under the cursor. Relative (each stem keeping its
            // offset from the one being dragged) is the other defensible
            // answer, but absolute is what a group drag does in Live and it is
            // the one a user can aim. Velocity does not reorder, so no re-sort.
            if (!selHas(dragNote_)) selOne(dragNote_);
            const u8 nv = velAt(in.my);
            bool any = false;
            for (int i : sel_) {
                if (i < 0 || i >= (int)clip.notes.size()) continue;
                if (clip.notes[(size_t)i].vel == nv) continue;
                clip.notes[(size_t)i].vel = nv;
                any = true;
            }
            if (any) { lastVel_ = nv; changed = true; }
        }
    } else if (hotGrid && (in.pressed[0] || in.pressed[2])) {
        const int hit = noteAt(clip.notes, rows, ta, pa, in.mx, in.my, minNoteW);
        // Clicking empty space adds, so without this the second click of a
        // double-click on empty space would delete what the first click made.
        const bool prevAdded = addedLastPress_;
        addedLastPress_ = false;

        if (in.pressed[2]) {
            if (hit >= 0) eraseNote(hit);                 // right-click deletes
        } else if (hit >= 0 && in.dblClick && !prevAdded) {
            eraseNote(hit);                               // double-click deletes
        } else if (hit >= 0 && in.shift()) {
            // Shift+click is about membership and starts no drag: a group that
            // moved because a note was being added to it would be unusable.
            selToggle(hit);
            if (selHas(hit)) preview_.push((int)clip.notes[(size_t)hit].pitch);
        } else if (hit >= 0) {
            // A plain click on a note that is already part of a multi-selection
            // keeps the set, so the same press can start a group drag; on
            // anything else it reduces the selection to that one note. Standard
            // DAW behaviour, and the reason clicking inside a chord to move it
            // does not scatter the chord first.
            if (!selHas(hit)) selOne(hit);
            else              primary_ = hit;
            ui.active = gridId;
            const NoteModel& nt = clip.notes[(size_t)hit];
            preview_.push((int)nt.pitch);          // clicking a note plays it
            f32 x0 = 0.f, x1 = 0.f;
            noteSpanX(nt, ta, minNoteW, x0, x1);
            const f32 edge = std::min(4.f * s, (x1 - x0) * 0.35f);
            dragNote_ = hit;
            dragY_ = in.my;
            dragPitch_ = (int)nt.pitch;
            if (in.mx >= x1 - edge) { drag_ = Drag::Resize; dragBeat_ = 0.0; }
            else { drag_ = Drag::Move; dragBeat_ = xToBeat(ta, in.mx) - nt.beat; }
        } else if (in.shift()) {
            // Rubber band. Plain empty-drag still adds a note — press-drag-add
            // is the gesture the roll is built around — so the band is what
            // Shift buys on empty space. The anchor is kept in content space so
            // scrolling or zooming mid-band does not drag the corner with it.
            drag_ = Drag::Band;
            dragNote_ = -1;
            bandBeat_ = xToBeat(ta, in.mx);
            bandY_ = in.my - grid.y + viewY;
            bandBase_ = sel_;
            ui.active = gridId;
        } else {
            const int pitch = rows.pitchAt(yToRow(pa, in.my));
            const f64 b = quantFloor(xToBeat(ta, in.mx));
            if (pitch >= 0 && b >= 0.0 && b < clip.lengthBeats) {
                NoteModel nn;
                nn.beat = b;
                nn.len = kGridStep;
                nn.pitch = (u8)pitch;
                nn.vel = lastVel_;
                clip.notes.push_back(nn);
                const int idx = sortTracking(clip.notes, nn);
                selOne(idx);                     // a fresh note is the selection
                dragNote_ = idx;
                // Press-drag-add: the new note is grabbed by the same gesture,
                // so drawing a note and placing it is one movement.
                drag_ = Drag::Move;
                dragBeat_ = xToBeat(ta, in.mx) - nn.beat;
                dragY_ = in.my;
                dragPitch_ = pitch;
                addedLastPress_ = true;
                preview_.push(pitch);            // hear what was just written
                ui.active = gridId;
                changed = true;
            }
        }
    } else if (hotLane && in.pressed[0]) {
        // Nearest stem within a few pixels; ties go to the later note, matching
        // the draw order.
        int best = -1;
        f32 bestD = 6.f * s;
        for (size_t i = 0; i < clip.notes.size(); ++i) {
            const f32 d = std::fabs(in.mx - beatToX(ta, clip.notes[i].beat));
            if (d <= bestD) { bestD = d; best = (int)i; }
        }
        if (best >= 0) {
            // A stem that belongs to the current selection drags the whole set
            // (the same rule the grid uses); any other stem takes the selection
            // over first, so the lane can never edit a note the user cannot see
            // they picked.
            if (!selHas(best)) selOne(best);
            else               primary_ = best;
            dragNote_ = best;
            drag_ = Drag::Velocity;
            // The lane deliberately does not audition: a velocity drag would
            // retrigger the note on every pixel, and the value being edited is
            // not the pitch anyway.
            addedLastPress_ = false;
            ui.active = laneId;
            const u8 nv = velAt(in.my);
            bool any = false;
            for (int i : sel_) {
                if (i < 0 || i >= (int)clip.notes.size()) continue;
                if (clip.notes[(size_t)i].vel == nv) continue;
                clip.notes[(size_t)i].vel = nv;
                any = true;
            }
            if (any) { lastVel_ = nv; changed = true; }
        }
    }

    const Col base = pal::clipColors[((clip.colorIdx % pal::clipColorCount) + pal::clipColorCount) %
                                     pal::clipColorCount];
    const int firstRow = std::max(0, yToRow(pa, grid.y));
    const int lastRow  = std::min(rows.count - 1, yToRow(pa, grid.bottom()));

    // --- ruler -------------------------------------------------------------
    rr.rect(ruler, pal::panel);
    rr.rect({ruler.x, ruler.bottom() - 1.f * s, ruler.w, 1.f * s}, pal::divider);

    const Rect lenBox{ruler.right() - 70.f * s, ruler.y + 2.f * s, 66.f * s, ruler.h - 4.f * s};
    if (ui.fSmall) {
        rr.pushClip({grid.x, ruler.y, std::max(0.f, std::min(grid.right(), lenBox.x) - grid.x), ruler.h});
        const int b0 = std::max(0, (int)std::floor(xToBeat(ta, grid.x)));
        const int b1 = (int)std::ceil(xToBeat(ta, grid.right()));
        const f32 ty = ruler.y + (ruler.h - ui.fSmall->height()) * 0.5f;
        for (int b = b0; b <= b1; ++b) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%d.%d", b / kBeatsPerBar + 1, b % kBeatsPerBar + 1);
            rr.text(*ui.fSmall, std::round(beatToX(ta, b)) + 3.f * s, ty, buf,
                    (b % kBeatsPerBar) == 0 ? pal::textDim : pal::textFaint);
        }
        rr.popClip();
    }
    if (ui.button(uiId(21, 0), {ruler.x + 3.f * s, ruler.y + 2.f * s, keyW - 6.f * s, ruler.h - 4.f * s},
                  "FOLD", fold_, pal::accent))
        fold_ = !fold_;
    // Whole beats only: a loop length between beats is a tempo problem.
    if (ui.dragNumber(uiId(22, 0), lenBox, &clip.lengthBeats, 1.0, 512.0, 0.06, "%.0f beats",
                      Align::Right, nullptr, 1.0))
        changed = true;

    // --- keyboard column ---------------------------------------------------
    rr.pushClip(keys);
    rr.rect(keys, pal::panel);
    for (int i = firstRow; i <= lastRow; ++i) {
        const int p = rows.pitchAt(i);
        if (p < 0) continue;
        const Rect kr{keys.x, rowToY(pa, i), keys.w, rowH};
        rr.rect(kr, isBlackKey(p) ? pal::panel : pal::panelAlt);
        rr.rect({kr.x, kr.bottom() - 1.f * s, kr.w, 1.f * s}, pal::divider.alpha(0.55f));
        if (p % 12 == 0 && ui.fSmall) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "C%d", p / 12 - 1);
            rr.textIn(*ui.fSmall, kr, buf, pal::textDim, Align::Left, 5.f * s);
        }
    }
    if (hotGrid) {
        const int hr = yToRow(pa, in.my);
        if (hr >= 0 && hr < rows.count)
            rr.rect({keys.x, rowToY(pa, hr), keys.w, rowH}, pal::accent.alpha(0.16f));
    }
    rr.rect({keys.right() - 1.f * s, keys.y, 1.f * s, keys.h}, pal::divider);
    rr.popClip();

    // --- grid --------------------------------------------------------------
    rr.pushClip(grid);
    for (int i = firstRow; i <= lastRow; ++i) {
        const int p = rows.pitchAt(i);
        if (p < 0) continue;
        const f32 y = rowToY(pa, i);
        rr.rect({grid.x, y, grid.w, rowH}, isBlackKey(p) ? pal::appBg : pal::slotEmpty);
        // One separator per octave keeps the eye anchored without banding.
        if (p % 12 == 0) rr.rect({grid.x, y + rowH - 1.f * s, grid.w, 1.f * s}, pal::divider);
    }
    {
        const f64 startB = std::max(0.0, quantFloor(xToBeat(ta, grid.x)));
        const f32 stepPx = pxPerBeat * (f32)kGridStep;
        const int steps = stepPx > 0.5f ? (int)(grid.w / stepPx) + 2 : 0;
        for (int k = 0; k <= steps; ++k) {
            const f64 b = startB + (f64)k * kGridStep;
            const f32 x = beatToX(ta, b);
            if (x > grid.right()) break;
            const bool onBeat = std::fabs(b - std::round(b)) < 1e-6;
            const bool onBar  = onBeat && ((i64)std::llround(b) % kBeatsPerBar) == 0;
            rr.rect({std::round(x), grid.y, 1.f * s, grid.h},
                    onBar ? pal::ridge.scale(1.45f) : (onBeat ? pal::ridge : pal::divider));
        }
    }
    {   // Past the loop length is not editable, so dim it like Live does.
        const f32 endX = beatToX(ta, clip.lengthBeats);
        if (endX < grid.right())
            rr.rect({std::max(grid.x, endX), grid.y, grid.right() - std::max(grid.x, endX), grid.h},
                    pal::divider.alpha(0.55f));
    }
    for (size_t i = 0; i < clip.notes.size(); ++i) {
        const NoteModel& nt = clip.notes[i];
        const int row = rows.rowOf(nt.pitch);
        if (row < firstRow || row > lastRow) continue;
        f32 x0 = 0.f, x1 = 0.f;
        noteSpanX(nt, ta, minNoteW, x0, x1);
        if (x1 < grid.x || x0 > grid.right()) continue;
        const Rect nr{x0, rowToY(pa, row) + 1.f * s, std::max(minNoteW, x1 - x0 - 1.f * s), rowH - 2.f * s};
        // Velocity reads as brightness, so a part's dynamics are visible in the
        // note block itself and not only down in the lane.
        rr.roundRect(nr, 2.f * s, base.scale(0.55f + 0.45f * (f32)nt.vel / 127.f));
        if (selHas((int)i)) rr.roundRectOutline(nr, 2.f * s, 1.f * s, pal::accent);
    }
    // The band goes over the notes it is taking, translucent enough to leave
    // them readable underneath.
    if (showBand) {
        rr.rect(bandRect, pal::accent.alpha(0.12f));
        rr.roundRectOutline(bandRect, 0.f, 1.f * s, pal::accent);
    }
    if (playing) {
        const f32 px = beatToX(ta, playheadBeats);
        if (px >= grid.x && px <= grid.right())
            rr.rect({px, grid.y, 1.5f * s, grid.h}, pal::playGreen);
    }
    rr.popClip();

    // --- velocity lane -----------------------------------------------------
    rr.rect({r.x, body.bottom(), r.w, 1.f * s}, pal::divider);
    rr.rect(laneKey, pal::panel);
    if (ui.fSmall) rr.textIn(*ui.fSmall, laneKey, "VEL", pal::textFaint, Align::Right, 6.f * s);
    rr.rect(lane, pal::appBg);
    rr.pushClip(lane);
    {
        const f32 travel = std::max(1.f, lane.h - 6.f * s);
        const f32 foot = lane.bottom() - 2.f * s;
        for (size_t i = 0; i < clip.notes.size(); ++i) {
            const f32 x = std::round(beatToX(ta, clip.notes[i].beat));
            if (x < lane.x - 2.f * s || x > lane.right()) continue;
            const f32 top = foot - (f32)clip.notes[i].vel / 127.f * travel;
            // Every selected stem is accented, since a lane drag moves all of
            // them; the primary keeps the full accent so the note the gesture
            // is anchored on is still findable inside a large selection.
            const Col c = !selHas((int)i)     ? base.scale(0.8f)
                          : (int)i == primary_ ? pal::accent
                                               : pal::accent.alpha(0.75f);
            rr.rect({x, top, std::max(1.f, 1.f * s), foot - top}, c);
            rr.circle(x + 0.5f * s, top, 2.5f * s, c);
        }
        if (playing) {
            const f32 px = beatToX(ta, playheadBeats);
            if (px >= lane.x && px <= lane.right())
                rr.rect({px, lane.y, 1.5f * s, lane.h}, pal::playGreen);
        }
    }
    rr.popClip();
    rr.rect({lane.x, lane.y, lane.w, 1.f * s}, pal::divider);

    // --- cursor ------------------------------------------------------------
    if (drag_ == Drag::Resize)        ui.cursor = Cursor::ResizeH;
    else if (drag_ == Drag::Move)     ui.cursor = Cursor::Grab;
    else if (drag_ == Drag::Velocity) ui.cursor = Cursor::ResizeV;
    else if (hotLane)                 ui.cursor = Cursor::ResizeV;
    else if (hotGrid) {
        const int hover = noteAt(clip.notes, rows, ta, pa, in.mx, in.my, minNoteW);
        if (hover >= 0) {
            const NoteModel& nt = clip.notes[(size_t)hover];
            const f32 x0 = beatToX(ta, nt.beat);
            const f32 x1 = std::max(beatToX(ta, nt.beat + nt.len), x0 + minNoteW);
            ui.cursor = (in.mx >= x1 - std::min(4.f * s, (x1 - x0) * 0.35f)) ? Cursor::ResizeH
                                                                            : Cursor::Grab;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// keyboard API
//
// These run from App::handleShortcuts, i.e. *before* this frame's draw(), and
// they act on the state the last draw left behind. Hence the identity check on
// every one of them: the clip in front of the roll can have been swapped since,
// and an index into the wrong clip's notes is an edit to the wrong note.
// ---------------------------------------------------------------------------

// A set of any size answers yes, and one index out of range condemns the lot:
// the set is only ever rebuilt as a whole, so a stale member means the clip
// changed under it and nothing in it can be trusted. (sel_ is sorted, so the
// last element is the only one that has to be checked.)
bool PianoRoll::hasSelection(const ClipModel& clip) const {
    return owns(clip) && !sel_.empty() && sel_.back() < (int)clip.notes.size();
}

bool PianoRoll::clearSelection() {
    if (sel_.empty()) return false;
    selClear();
    return true;
}

bool PianoRoll::nudgeSelected(ClipModel& clip, int gridSteps, int semitones) {
    if (!hasSelection(clip)) return false;
    const NudgeResult res = nudgeGroup(clip.notes, sel_, primary_, gridSteps, semitones,
                                       clip.lengthBeats);
    if (!res.changed) return false;                // already against a clamp
    followSel_ = true;
    // One audition for the group: the primary note. A held arrow key on a
    // thirty-note chord would otherwise be a wall of retriggers.
    if (res.pitchChanged && primary_ >= 0 && primary_ < (int)clip.notes.size())
        preview_.push((int)clip.notes[(size_t)primary_].pitch);
    return true;
}

bool PianoRoll::deleteSelected(ClipModel& clip) {
    if (!hasSelection(clip)) return false;
    // Back to front: an index into a vector survives only until something
    // earlier than it is removed. sel_ is sorted, so walking it in reverse is
    // all the bookkeeping a multi-delete needs.
    for (size_t k = sel_.size(); k-- > 0;) {
        const int i = sel_[k];
        if (i >= 0 && i < (int)clip.notes.size())
            clip.notes.erase(clip.notes.begin() + i);
    }
    selClear();
    // A drag cannot be in flight (this arrives from the keyboard), but the
    // index would be stale if one ever were.
    dragNote_ = -1;
    drag_ = Drag::None;
    return true;
}

bool PianoRoll::duplicateLoop(ClipModel& clip) {
    if (!owns(clip)) return false;
    const DupResult res = duplicateLoopNotes(clip.notes, clip.lengthBeats, sel_, primary_);
    if (!res.changed) return false;                // already at the cap
    sel_ = res.sel;
    primary_ = res.primary;
    if (!sel_.empty() && !selHas(primary_)) primary_ = sel_.front();
    followSel_ = true;
    return true;
}

} // namespace lat
