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

// Keyboard nudge: `steps` grid steps along time, `semis` semitones of pitch.
// Both clamped — into the clip at both ends, into 0..127 — and the time nudge
// goes through the same snap as a mouse move, so nudging an off-grid note (one
// that arrived by MIDI recording) pulls it onto the grid rather than carrying
// the offset along forever.
//
// `index` comes back as the note's index *after* the re-sort, or -1 when the
// nudge changed nothing at all (a note already against a clamp).
struct NudgeResult {
    int  index = -1;
    bool pitchChanged = false;
};
NudgeResult nudgeNote(std::vector<NoteModel>& notes, int idx, int steps, int semis,
                      f64 lengthBeats) {
    NudgeResult res;
    if (idx < 0 || idx >= (int)notes.size()) return res;
    NoteModel nt = notes[(size_t)idx];
    const NoteModel was = nt;
    if (steps != 0) nt.beat = clampBeat(nt.beat + (f64)steps * kGridStep, nt.len, lengthBeats);
    if (semis != 0) nt.pitch = (u8)clampv((int)nt.pitch + semis, 0, 127);
    if (sameNote(nt, was)) return res;
    notes[(size_t)idx] = nt;
    res.index = sortTracking(notes, nt);
    res.pitchChanged = nt.pitch != was.pitch;
    return res;
}

// Live's duplicate-loop: the loop doubles and everything in it is copied one
// old-length later, so a bar of material becomes two bars of it. `selected`
// (an index, or -1) follows into the *copy*, which is the note the user is
// about to edit; the returned index is where that copy landed.
//
// The cap is a length, not a factor: doubling a 40-beat loop gives 64 and the
// copies that would start past the new end are simply not made (a note that
// straddles the end is trimmed). Nothing happens at all once the loop is
// already at the cap — a no-op that reports false, so the caller does not
// re-push an unchanged clip.
struct DupResult {
    bool changed = false;
    int  index = -1;
};
DupResult duplicateLoopNotes(std::vector<NoteModel>& notes, f64& lengthBeats, int selected) {
    DupResult res;
    const f64 oldLen = std::max(kGridStep, lengthBeats);
    if (oldLen >= kMaxLoopBeats) return res;
    const f64 newLen = std::min(kMaxLoopBeats, oldLen * 2.0);

    // The note the selection should end on: the copy of the selected note, or
    // the selected note itself when the cap left no room for its copy. Either
    // way it is tracked through the sort, because a bare sort would leave the
    // caller holding an index into the old order.
    const size_t n = notes.size();
    NoteModel key{};
    bool haveKey = selected >= 0 && selected < (int)n;
    if (haveKey) key = notes[(size_t)selected];
    for (size_t i = 0; i < n; ++i) {
        NoteModel c = notes[i];
        c.beat += oldLen;
        if (c.beat >= newLen - 1e-9) continue;
        c.len = std::min(c.len, newLen - c.beat);
        if ((int)i == selected) { key = c; haveKey = true; }
        notes.push_back(c);
    }
    lengthBeats = newLen;
    res.changed = true;
    // The vector is two sorted runs (originals, then copies, each in order and
    // the second entirely later), which sortTracking restores in one pass.
    if (haveKey) res.index = sortTracking(notes, key);
    else         std::sort(notes.begin(), notes.end(), noteLess);
    return res;
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
        const f32 x0 = beatToX(ta, notes[i].beat);
        const f32 x1 = std::max(beatToX(ta, notes[i].beat + notes[i].len), x0 + minW);
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
        selected_ = -1;
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
    if (selected_ >= noteCount) selected_ = -1;
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
    // screen (clamping to a window it is already inside is a no-op).
    if (followSel_) {
        followSel_ = false;
        if (selected_ >= 0 && selected_ < noteCount) {
            const NoteModel& sel = clip.notes[(size_t)selected_];
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
        if (selected_ == i) selected_ = -1; else if (selected_ > i) --selected_;
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

    if (drag_ != Drag::None) {
        if (!in.down[0] || dragNote_ < 0 || dragNote_ >= (int)clip.notes.size()) {
            drag_ = Drag::None;
            dragNote_ = -1;
            if (ui.active == gridId || ui.active == laneId) ui.active = 0;
        } else if (drag_ == Drag::Move) {
            NoteModel& nt = clip.notes[(size_t)dragNote_];
            const f64 nb = clampBeat(xToBeat(ta, in.mx) - dragBeat_, nt.len, clip.lengthBeats);
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
            if (nb != nt.beat || np != (int)nt.pitch) {
                // Dragging across rows plays what is under the note, the way a
                // note dragged in Live does: the pitch is the thing being
                // chosen, and choosing it by eye alone is guesswork.
                if (np != (int)nt.pitch) preview_.push(np);
                nt.beat = nb;
                nt.pitch = (u8)np;
                const NoteModel key = nt;           // nt dangles once we sort
                dragNote_ = sortTracking(clip.notes, key);
                selected_ = dragNote_;
                if (dragNote_ < 0) drag_ = Drag::None;
                changed = true;
            }
        } else if (drag_ == Drag::Resize) {
            NoteModel& nt = clip.notes[(size_t)dragNote_];
            const f64 nl = clampLen(xToBeat(ta, in.mx), nt.beat, clip.lengthBeats);
            if (nl != nt.len) { nt.len = nl; changed = true; }
        } else if (drag_ == Drag::Velocity) {
            NoteModel& nt = clip.notes[(size_t)dragNote_];
            const u8 nv = velAt(in.my);
            if (nv != nt.vel) { nt.vel = nv; lastVel_ = nv; changed = true; }
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
        } else if (hit >= 0) {
            selected_ = hit;
            ui.active = gridId;
            const NoteModel& nt = clip.notes[(size_t)hit];
            preview_.push((int)nt.pitch);          // clicking a note plays it
            const f32 x0 = beatToX(ta, nt.beat);
            const f32 x1 = std::max(beatToX(ta, nt.beat + nt.len), x0 + minNoteW);
            const f32 edge = std::min(4.f * s, (x1 - x0) * 0.35f);
            dragNote_ = hit;
            dragY_ = in.my;
            dragPitch_ = (int)nt.pitch;
            if (in.mx >= x1 - edge) { drag_ = Drag::Resize; dragBeat_ = 0.0; }
            else { drag_ = Drag::Move; dragBeat_ = xToBeat(ta, in.mx) - nt.beat; }
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
                selected_ = idx;
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
            selected_ = best;
            dragNote_ = best;
            drag_ = Drag::Velocity;
            // The lane deliberately does not audition: a velocity drag would
            // retrigger the note on every pixel, and the value being edited is
            // not the pitch anyway.
            addedLastPress_ = false;
            ui.active = laneId;
            const u8 nv = velAt(in.my);
            if (nv != clip.notes[(size_t)best].vel) {
                clip.notes[(size_t)best].vel = nv;
                lastVel_ = nv;
                changed = true;
            }
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
        const f32 x0 = beatToX(ta, nt.beat);
        const f32 x1 = std::max(beatToX(ta, nt.beat + nt.len), x0 + minNoteW);
        if (x1 < grid.x || x0 > grid.right()) continue;
        const Rect nr{x0, rowToY(pa, row) + 1.f * s, std::max(minNoteW, x1 - x0 - 1.f * s), rowH - 2.f * s};
        // Velocity reads as brightness, so a part's dynamics are visible in the
        // note block itself and not only down in the lane.
        rr.roundRect(nr, 2.f * s, base.scale(0.55f + 0.45f * (f32)nt.vel / 127.f));
        if ((int)i == selected_) rr.roundRectOutline(nr, 2.f * s, 1.f * s, pal::accent);
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
            const bool sel = ((int)i == selected_);
            const Col c = sel ? pal::accent : base.scale(0.8f);
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

bool PianoRoll::hasSelection(const ClipModel& clip) const {
    return owns(clip) && selected_ >= 0 && selected_ < (int)clip.notes.size();
}

bool PianoRoll::clearSelection() {
    if (selected_ < 0) return false;
    selected_ = -1;
    return true;
}

bool PianoRoll::nudgeSelected(ClipModel& clip, int gridSteps, int semitones) {
    if (!hasSelection(clip)) return false;
    const NudgeResult res = nudgeNote(clip.notes, selected_, gridSteps, semitones,
                                      clip.lengthBeats);
    if (res.index < 0) return false;               // already against a clamp
    selected_ = res.index;
    followSel_ = true;
    if (res.pitchChanged) preview_.push((int)clip.notes[(size_t)res.index].pitch);
    return true;
}

bool PianoRoll::deleteSelected(ClipModel& clip) {
    if (!hasSelection(clip)) return false;
    clip.notes.erase(clip.notes.begin() + selected_);
    selected_ = -1;
    // A drag cannot be in flight (this arrives from the keyboard), but the
    // index would be stale if one ever were.
    dragNote_ = -1;
    drag_ = Drag::None;
    return true;
}

bool PianoRoll::duplicateLoop(ClipModel& clip) {
    if (!owns(clip)) return false;
    const DupResult res = duplicateLoopNotes(clip.notes, clip.lengthBeats, selected_);
    if (!res.changed) return false;                // already at the cap
    selected_ = res.index;
    followSel_ = true;
    return true;
}

} // namespace lat
