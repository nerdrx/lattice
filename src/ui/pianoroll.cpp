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

// kGridStep, kBeatsPerBar, kPxPerBeatMin/Max and kZoomMin/Max/PerNotch moved
// into timeaxis.h with TimeAxis itself (docs/ARRANGEMENT.md §7.2): they are
// about *time*, and the arrangement needs the identical numbers. What stays
// here is everything that is about a piano roll rather than about a timeline.
constexpr int kMinFoldRows = 8;     // a one-note clip still needs room to click
constexpr f32 kKeyW       = 46.f;
constexpr f32 kRulerH     = 16.f;
constexpr f32 kLaneH      = 54.f;
constexpr f32 kRowH       = 12.f;
constexpr int kCentrePitch  = 60;   // C4, the middle of the default C3..C5 view
constexpr f64 kMaxLoopBeats = 64.0; // ceiling for Ctrl+U, 16 bars in 4/4
// The undo labels the caller reads back off lastEdit().
constexpr const char* kEditNote = "note edit";
constexpr const char* kEditAuto = "automation edit";

// ---------------------------------------------------------------------------
// axes
//
// The TIME axis moved to timeaxis.h. The PITCH axis did not: nothing but a
// piano roll has one.
// ---------------------------------------------------------------------------

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

// quantFloor / quantNear moved to timeaxis.h with the grid step they are
// measured in: a snap is a statement about the time axis and nothing else.

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
void sortTrackingSet(std::vector<NoteModel>& v, const SelKeys& keys, IndexSel& out) {
    std::sort(v.begin(), v.end(), noteLess);
    out.clear();
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
            out.items.push_back((int)i);
            if ((int)j == keys.primary) out.primary = (int)i;
            break;
        }
    }
    std::sort(out.items.begin(), out.items.end());
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
bool applyGroupDelta(std::vector<NoteModel>& notes, IndexSel& sel, const GroupDelta& d) {
    if (d.beats == 0.0 && d.semis == 0) return false;
    SelKeys keys;
    keys.notes.reserve(sel.items.size());
    for (int i : sel.items) {
        if (i < 0 || i >= (int)notes.size()) continue;
        NoteModel& nt = notes[(size_t)i];
        nt.beat  = nt.beat + d.beats;
        nt.pitch = (u8)clampv((int)nt.pitch + d.semis, 0, 127);
        if (i == sel.primary) keys.primary = (int)keys.notes.size();
        keys.notes.push_back(nt);
    }
    if (keys.notes.empty()) return false;
    sortTrackingSet(notes, keys, sel);
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
NudgeResult nudgeGroup(std::vector<NoteModel>& notes, IndexSel& sel,
                       int steps, int semis, f64 lengthBeats) {
    NudgeResult res;
    if (sel.empty()) return res;
    const int anchor = (sel.primary >= 0 && sel.primary < (int)notes.size()) ? sel.primary
                                                                            : sel.items.front();
    if (anchor < 0 || anchor >= (int)notes.size()) return res;
    const f64 aBeat = notes[(size_t)anchor].beat;
    const f64 want = steps != 0 ? quantNear(aBeat + (f64)steps * kGridStep) - aBeat : 0.0;
    const GroupDelta d = clampGroupDelta(notes, sel.items, want, semis, lengthBeats);
    if (!applyGroupDelta(notes, sel, d)) return res;
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
    IndexSel sel;
};
DupResult duplicateLoopNotes(std::vector<NoteModel>& notes, f64& lengthBeats,
                             const IndexSel& selected) {
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
    for (int i : selected.items) {
        if (i < 0 || i >= (int)n) continue;
        slotOf[(size_t)i] = (int)keys.notes.size();
        if (i == selected.primary) keys.primary = (int)keys.notes.size();
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
    sortTrackingSet(notes, keys, res.sel);
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
// the automation lane
//
// It moved. ValAxis, ptLess/samePt, PtKeys, sortTrackingPts, the PtDelta clamp
// and its apply, ptScreen, pointAt, pointsInBand and insertPoint are now
// src/ui/autolane.cpp, together with the lane's own drag and selection state
// and the slice of draw() that used them (docs/ARRANGEMENT.md §7.3, which is
// docs/AUTOMATION.md §6.5 finally paid). The roll is the lane's FIRST caller
// and keeps its chooser, drawLaneKey, which is roll-specific: it is what *adds*
// a lane to a clip. The lane is handed the roll's own TimeAxis, so the property
// that made it live here in the first place is now a parameter.
// ---------------------------------------------------------------------------

// dpiOf moved to autolane.h with the lane: the arrangement needs the same
// answer, and two ways of recovering the DPI scale would be two answers.

} // namespace

// IndexSel: its implementation moved to autolane.cpp with its declaration.

// ---------------------------------------------------------------------------
// PianoRoll: the lane's key block
//
// Where the static "VEL" label used to be (decision #9 in
// docs/audits/WAVE7-DECISIONS.md): three stacked rows in the 46 px gutter under
// the keyboard column — what the lane shows, what "+" would add, and the shown
// lane's on/off toggle beside the "+" itself.
//
// `selector` rather than a new dropdown widget because the codebase has no
// dropdown and inventing one is a separate piece of work: click cycles forward,
// right-click cycles back, wheel scrubs, which is the documented idiom
// everywhere else in the program. The gutter is too narrow for a plugin's
// parameter name, so the full text goes to the tooltip and the row shows what
// fits.
// ---------------------------------------------------------------------------

void PianoRoll::drawLaneKey(Ui& ui, const Rect& b, ClipModel& clip,
                            const AutoTargets& targets, f32 s, bool& changed) {
    if (!ui.r) return;
    Renderer& rr = *ui.r;
    rr.pushClip(b);

    const f32 rowH = std::max(9.f * s, (b.h - 4.f * s) / 3.f);
    const Rect r0{b.x + 2.f * s, b.y + 1.f * s, b.w - 4.f * s, rowH};
    const Rect r1{r0.x, r0.bottom() + 1.f * s, r0.w, rowH};
    const Rect r2{r0.x, r1.bottom() + 1.f * s, r0.w, rowH};

    // What the lane shows: velocity, then one entry per envelope the clip has.
    // The names are the target's short label where the address resolves and the
    // address itself where it does not — a lane naming a missing device must
    // still be findable.
    std::vector<std::string> names;
    names.push_back(clip.kind == ClipKind::Midi ? "VEL" : "--");
    for (const AutoLane& l : clip.envelopes) {
        const AutoTargets::Entry* e = targets.find(l.address);
        names.push_back(e ? e->label : l.address);
    }
    std::vector<const char*> ptrs;
    ptrs.reserve(names.size());
    for (const std::string& n : names) ptrs.push_back(n.c_str());

    laneSel_ = clampv(laneSel_, 0, (int)ptrs.size() - 1);
    int shown = laneSel_;
    if (ui.selector(uiId(23, 0), r0, &shown, ptrs.data(), (int)ptrs.size())) {
        laneSel_ = shown;
        // The point indices belong to the lane that was on show, so switching
        // lanes drops them rather than carrying a stale set across.
        lane_.detach();
    }
    if (ui.hovered(r0) && laneSel_ > 0 && laneSel_ <= (int)clip.envelopes.size())
        ui.tip = clip.envelopes[(size_t)laneSel_ - 1].address;

    // What "+" would automate, with the button that does it beside the name so
    // the pair reads as one control. A dot marks a target this clip already has
    // a lane for, so the list says where the automation in a set actually is
    // rather than making the user cycle it to find out.
    const Rect addR{r2.x, r2.y, 18.f * s, r2.h};
    const Rect tgtR = r1;
    if (!targets.entries.empty()) {
        std::vector<const char*> tnames;
        tnames.reserve(targets.entries.size());
        for (const AutoTargets::Entry& e : targets.entries) tnames.push_back(e.label.c_str());
        targetSel_ = clampv(targetSel_, 0, (int)tnames.size() - 1);
        ui.selector(uiId(23, 1), tgtR, &targetSel_, tnames.data(), (int)tnames.size());
        const AutoTargets::Entry& sel = targets.entries[(size_t)targetSel_];
        if (sel.automated)
            rr.circle(tgtR.right() - 3.5f * s, tgtR.y + 3.5f * s, 1.8f * s, pal::accentHi);
        if (ui.hovered(tgtR)) ui.tip = sel.group + " " + sel.label + "  " + sel.address;
    } else if (ui.fSmall) {
        rr.textIn(*ui.fSmall, tgtR, "no targets", pal::textFaint, Align::Center, 0);
    }

    // The toggle is a lit dot rather than a word: at 46 px wide, "OFF" beside a
    // "+" ellipsises, and the dot is the same control the device strip already
    // uses for bypass — a lit plate means the thing is doing something.
    const Rect onR{addR.right() + 2.f * s, r2.y, r2.right() - addR.right() - 2.f * s, r2.h};
    if (!targets.entries.empty() && ui.button(uiId(23, 2), addR, "+")) {
        const std::string& addr = targets.entries[(size_t)targetSel_].address;
        int found = -1;
        for (size_t i = 0; i < clip.envelopes.size(); ++i)
            if (clip.envelopes[i].address == addr) { found = (int)i; break; }
        if (found >= 0) {
            laneSel_ = found + 1;            // already there: show it
            lane_.detach();
        } else if ((int)clip.envelopes.size() < kMaxClipLanes) {
            AutoLane l;
            l.address = addr;
            clip.envelopes.push_back(std::move(l));
            laneSel_ = (int)clip.envelopes.size();
            lane_.detach();
            changed = true;
            lastEdit_ = kEditAuto;
        }
    }
    // Live's "deactivate envelope": the lane keeps its points and stops driving
    // anything. Only meaningful with a lane on show.
    if (laneSel_ > 0 && laneSel_ <= (int)clip.envelopes.size()) {
        AutoLane& l = clip.envelopes[(size_t)laneSel_ - 1];
        bool on = l.enabled;
        if (ui.squareToggle(uiId(23, 3), onR, "", &on, pal::accent)) {
            l.enabled = on;
            changed = true;
            lastEdit_ = kEditAuto;
        }
        rr.circle(onR.cx(), onR.cy(), 3.f * s, l.enabled ? pal::textOnClip : pal::textFaint);
        if (ui.hovered(onR))
            ui.tip = l.enabled ? "envelope active - click to deactivate it"
                               : "envelope deactivated - click to put it back in charge";
    }
    rr.popClip();
}

// ---------------------------------------------------------------------------
// PianoRoll
// ---------------------------------------------------------------------------

bool PianoRoll::draw(Ui& ui, const Rect& r, ClipModel& clip, const AutoTargets& targets,
                     f64 playheadBeats, bool playing) {
    if (!ui.r || !ui.in) return false;
    Renderer& rr = *ui.r;
    Input& in = *ui.in;
    const f32 s = dpiOf(ui);
    bool changed = false;
    // An audio clip has no notes and no pitch axis, but it has envelopes and a
    // time axis — so it gets the same editor with its waveform where the note
    // grid would be. Everything below that is about notes is gated on this.
    const bool midiClip = clip.kind == ClipKind::Midi;

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
        sel_.clear();
        lane_.detach();
        bandBase_.clear();
        dragNote_ = -1;
        drag_ = Drag::None;
        scrollX_ = scrollY_ = 0.f;
        zoom_ = 0.f;                 // -> fit to width below
        addedLastPress_ = false;
        followSel_ = false;
        preview_.clear();            // an audition for a clip nobody is looking at
        targetSel_ = 0;
        // An audio clip has no velocity stems, so the lane opens on its first
        // envelope rather than on an entry that would draw nothing. A choice
        // made from outside for this clip (showLane) outranks both.
        laneSel_ = pendingLane_ >= 0 ? pendingLane_
                                     : ((!midiClip && !clip.envelopes.empty()) ? 1 : 0);
        pendingLane_ = -1;
    }

    // A note count can still change under a live selection (undo, a MIDI take
    // finishing), so indices are re-checked on every frame regardless.
    const int noteCount = midiClip ? (int)clip.notes.size() : 0;
    sel_.prune(noteCount);
    if (dragNote_ >= noteCount) {
        dragNote_ = -1;
        // A lane drag holds no note index and is AutoLaneView's anyway, so it is
        // not this check's business.
        drag_ = Drag::None;
    }

    // Same for the lane: a lane and its points can go away under a live
    // selection (undo, a project load), and laneSel_ is an index like any other.
    if (laneSel_ > (int)clip.envelopes.size()) { laneSel_ = 0; lane_.detach(); }

    // The lane's key block, drawn (and clicked) FIRST, before anything takes a
    // pointer into clip.envelopes: it can add a lane, and a push_back moves the
    // vector. It sits in its own rect at the bottom left and overlaps nothing,
    // so being early costs it neither hover nor paint order.
    rr.rect(laneKey, pal::panel);
    drawLaneKey(ui, laneKey, clip, targets, s, changed);

    AutoLane* const env = shownLane(clip);
    // A lane and its points can go away under a live selection (undo, a project
    // load), and an index is only ever as good as the frame it was made in.
    if (env) lane_.prune((int)env->points.size());
    else     lane_.detach();
    // The target this lane names, and therefore its value axis. A lane whose
    // address the set cannot resolve today (a deleted device, a set opened on
    // another machine) keeps its points and draws greyed against a plain 0..1 —
    // losing a filter sweep because a plugin is missing is the worst bug this
    // feature could have.
    const AutoTargets::Entry* const tgt = env ? targets.find(env->address) : nullptr;
    const bool laneInert = env && laneSel_ <= 32 &&
                           ((targets.inert & (1u << (u32)(laneSel_ - 1))) != 0u);
    // "Switched off" and "therefore drawn greyed" are AutoLaneView's own
    // conclusions now: it is handed `enabled`, `inert` and `resolved` and works
    // out the rest, which is the same three facts this used to fold by hand.

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
        } else if (in.shift() || !midiClip) {
            // An audio clip has no pitch axis to scroll, so a plain wheel there
            // means the thing it does have: time.
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
        if (sel_.primary >= 0 && sel_.primary < noteCount) {
            const NoteModel& sel = clip.notes[(size_t)sel_.primary];
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
    // The lane's own value axis is AutoLaneView's, built from the same rect the
    // velocity stems use — so switching the lane from VEL to an envelope changes
    // what is drawn and not where it is drawn.
    const f32 minNoteW = 3.f * s;

    // --- interaction -------------------------------------------------------
    ui.setHot(gridId, grid);
    ui.setHot(laneId, lane);
    const bool hotGrid = ui.isHot(gridId), hotLane = ui.isHot(laneId);

    auto eraseNote = [&](int i) {
        if (i < 0 || i >= (int)clip.notes.size()) return;
        clip.notes.erase(clip.notes.begin() + i);
        sel_.erased(i);
        if (dragNote_ == i) { dragNote_ = -1; drag_ = Drag::None; }
        else if (dragNote_ > i) --dragNote_;
        changed = true;
        lastEdit_ = kEditNote;
    };
    // Velocity lane drags are absolute: the stem follows the cursor height, so a
    // plain click on the lane also sets the value, like clicking a fader track.
    auto velAt = [&](f32 y) {
        const f32 t = clampv((lane.bottom() - 2.f * s - y) / std::max(1.f, lane.h - 6.f * s), 0.f, 1.f);
        return (u8)clampv((int)std::lround(t * 127.f), 1, 127);
    };

    // The rubber band, when one is in flight: computed with the interaction and
    // drawn later, inside the grid's clip. The LANE's band is the lane's own,
    // and is drawn by AutoLaneView.
    Rect bandRect{};
    bool showBand = false;

    if (drag_ != Drag::None) {
        // Band is the one drag with nothing under it.
        const bool needsNote = drag_ == Drag::Move || drag_ == Drag::Resize ||
                               drag_ == Drag::Velocity;
        if (!in.down[0] ||
            (needsNote && (dragNote_ < 0 || dragNote_ >= (int)clip.notes.size()))) {
            drag_ = Drag::None;
            dragNote_ = -1;
            bandBase_.clear();
            if (ui.active == gridId) ui.active = 0;
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
            sel_.adopt(bandBase_);
            for (int i : hits) sel_.add(i);
        } else if (drag_ == Drag::Move) {
            // The note under the hand is always part of what moves. It is put
            // there on the press, but Escape can empty the set from the
            // keyboard between frames while the button is still down, and a
            // drag that silently stopped moving anything would look like a
            // freeze rather than a cancel.
            if (!sel_.has(dragNote_)) sel_.one(dragNote_);
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
                clip.notes, sel_.items, quantNear(xToBeat(ta, in.mx) - dragBeat_) - nt.beat,
                np - (int)nt.pitch, clip.lengthBeats);
            if (d.beats != 0.0 || d.semis != 0) {
                // Dragging across rows plays what is under the note, the way a
                // note dragged in Live does: the pitch is the thing being
                // chosen, and choosing it by eye alone is guesswork. Only the
                // note under the hand is auditioned — thirty at once is noise,
                // not a chord.
                if (d.semis != 0) preview_.push((int)nt.pitch + d.semis);
                if (applyGroupDelta(clip.notes, sel_, d)) { changed = true; lastEdit_ = kEditNote; }
                dragNote_ = sel_.primary;
                if (dragNote_ < 0) drag_ = Drag::None;
            }
        } else if (drag_ == Drag::Resize) {
            // Length stays a one-note edit: a group resize has to choose
            // between absolute and proportional lengths, and neither is what
            // the hand on one note's right edge asked for.
            NoteModel& nt = clip.notes[(size_t)dragNote_];
            const f64 nl = clampLen(xToBeat(ta, in.mx), nt.beat, clip.lengthBeats);
            if (nl != nt.len) { nt.len = nl; changed = true; lastEdit_ = kEditNote; }
        } else if (drag_ == Drag::Velocity) {
            // Absolute, and for the whole selection: every selected stem takes
            // the value under the cursor. Relative (each stem keeping its
            // offset from the one being dragged) is the other defensible
            // answer, but absolute is what a group drag does in Live and it is
            // the one a user can aim. Velocity does not reorder, so no re-sort.
            if (!sel_.has(dragNote_)) sel_.one(dragNote_);
            const u8 nv = velAt(in.my);
            bool any = false;
            for (int i : sel_.items) {
                if (i < 0 || i >= (int)clip.notes.size()) continue;
                if (clip.notes[(size_t)i].vel == nv) continue;
                clip.notes[(size_t)i].vel = nv;
                any = true;
            }
            if (any) { lastVel_ = nv; changed = true; lastEdit_ = kEditNote; }
        }
    } else if (hotGrid && midiClip && (in.pressed[0] || in.pressed[2])) {
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
            sel_.toggle(hit);
            if (sel_.has(hit)) preview_.push((int)clip.notes[(size_t)hit].pitch);
        } else if (hit >= 0) {
            // A plain click on a note that is already part of a multi-selection
            // keeps the set, so the same press can start a group drag; on
            // anything else it reduces the selection to that one note. Standard
            // DAW behaviour, and the reason clicking inside a chord to move it
            // does not scatter the chord first.
            if (!sel_.has(hit)) sel_.one(hit);
            else              sel_.primary = hit;
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
            bandBase_ = sel_.items;
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
                sel_.one(idx);                   // a fresh note is the selection
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
                lastEdit_ = kEditNote;
            }
        }
    } else if (hotLane && !env && midiClip && in.pressed[0]) {
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
            if (!sel_.has(best)) sel_.one(best);
            else                 sel_.primary = best;
            dragNote_ = best;
            drag_ = Drag::Velocity;
            // The lane deliberately does not audition: a velocity drag would
            // retrigger the note on every pixel, and the value being edited is
            // not the pitch anyway.
            addedLastPress_ = false;
            ui.active = laneId;
            const u8 nv = velAt(in.my);
            bool any = false;
            for (int i : sel_.items) {
                if (i < 0 || i >= (int)clip.notes.size()) continue;
                if (clip.notes[(size_t)i].vel == nv) continue;
                clip.notes[(size_t)i].vel = nv;
                any = true;
            }
            if (any) { lastVel_ = nv; changed = true; lastEdit_ = kEditNote; }
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
        // The shared ruler (timeaxis.h). Same loop, same numbers, same colours —
        // now also the arrangement's, which is the point of the extraction.
        drawRulerLabels(rr, *ui.fSmall, ta, grid.x, grid.right(),
                        ruler.y + (ruler.h - ui.fSmall->height()) * 0.5f, s,
                        pal::textDim, pal::textFaint);
        rr.popClip();
    }
    const Rect foldBox{ruler.x + 3.f * s, ruler.y + 2.f * s, keyW - 6.f * s, ruler.h - 4.f * s};
    if (midiClip) {
        if (ui.button(uiId(21, 0), foldBox, "FOLD", fold_, pal::accent)) fold_ = !fold_;
        // Whole beats only: a loop length between beats is a tempo problem.
        if (ui.dragNumber(uiId(22, 0), lenBox, &clip.lengthBeats, 1.0, 512.0, 0.06, "%.0f beats",
                          Align::Right, nullptr, 1.0)) {
            changed = true;
            lastEdit_ = kEditNote;
        }
    } else if (ui.fSmall) {
        // An audio clip has no pitches to fold and its length belongs to the
        // sample and the warp settings on the panel beside this, not to a drag
        // in here — so both read out instead of editing.
        rr.textIn(*ui.fSmall, foldBox, "WAVE", pal::textFaint, Align::Center, 0);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f beats", clip.lengthBeats);
        rr.textIn(*ui.fSmall, lenBox, buf, pal::textFaint, Align::Right, 0);
    }

    // --- keyboard column ---------------------------------------------------
    rr.pushClip(keys);
    rr.rect(keys, pal::panel);
    for (int i = firstRow; i <= lastRow && midiClip; ++i) {
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
    if (hotGrid && midiClip) {
        const int hr = yToRow(pa, in.my);
        if (hr >= 0 && hr < rows.count)
            rr.rect({keys.x, rowToY(pa, hr), keys.w, rowH}, pal::accent.alpha(0.16f));
    }
    if (!midiClip && ui.fSmall) {
        // The column stays: it is what the lane's key block hangs under, and an
        // empty gutter beside a waveform reads as a missing piece.
        rr.textIn(*ui.fSmall, {keys.x, keys.y + 4.f * s, keys.w, 12.f * s}, "AUDIO",
                  pal::textFaint, Align::Center, 0);
        if (clip.sample) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%d ch", clip.sample->channels);
            rr.textIn(*ui.fSmall, {keys.x, keys.y + 16.f * s, keys.w, 12.f * s}, buf,
                      pal::textFaint, Align::Center, 0);
        }
    }
    rr.rect({keys.right() - 1.f * s, keys.y, 1.f * s, keys.h}, pal::divider);
    rr.popClip();

    // --- grid: note rows for a pattern, the waveform for a sample -----------
    rr.pushClip(grid);
    if (!midiClip) rr.rect(grid, pal::slotEmpty);
    for (int i = firstRow; i <= lastRow && midiClip; ++i) {
        const int p = rows.pitchAt(i);
        if (p < 0) continue;
        const f32 y = rowToY(pa, i);
        rr.rect({grid.x, y, grid.w, rowH}, isBlackKey(p) ? pal::appBg : pal::slotEmpty);
        // One separator per octave keeps the eye anchored without banding.
        if (p % 12 == 0) rr.rect({grid.x, y + rowH - 1.f * s, grid.w, 1.f * s}, pal::divider);
    }
    drawTimeGrid(rr, ta, grid, s);          // the shared grid (timeaxis.h)
    {   // Past the loop length is not editable, so dim it like Live does.
        const f32 endX = beatToX(ta, clip.lengthBeats);
        if (endX < grid.right())
            rr.rect({std::max(grid.x, endX), grid.y, grid.right() - std::max(grid.x, endX), grid.h},
                    pal::divider.alpha(0.55f));
    }
    if (!midiClip && clip.sample && clip.sample->peakBuckets > 0) {
        // The sample, drawn against the SAME time axis the lane below uses, so
        // a breakpoint can be put on a transient and stay on it through every
        // zoom and scroll. One column per pixel, like App::drawWaveform — the
        // peak buckets are what makes that cheap.
        const SampleBuffer& sb = *clip.sample;
        const f32 mid = grid.cy(), halfH = grid.h * 0.5f - 3.f * s;
        const Col wc = base.scale(0.85f);
        for (f32 x = grid.x; x < grid.right(); x += 1.f) {
            const f64 u = xToBeat(ta, x) / lenBeats;
            if (u < 0.0 || u >= 1.0) continue;
            const int bk = clampv((int)(u * (f64)sb.peakBuckets), 0, sb.peakBuckets - 1);
            const f32 lo = sb.peaks[(size_t)bk * 2 + 0];
            const f32 hi = sb.peaks[(size_t)bk * 2 + 1];
            const f32 y0 = mid - hi * halfH;
            rr.rect({x, y0, 1.f, std::max(1.f, (mid - lo * halfH) - y0)}, wc);
        }
    }
    for (size_t i = 0; midiClip && i < clip.notes.size(); ++i) {
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
        if (sel_.has((int)i)) rr.roundRectOutline(nr, 2.f * s, 1.f * s, pal::accent);
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

    // --- the lane: velocity stems or one envelope ---------------------------
    rr.rect({r.x, body.bottom(), r.w, 1.f * s}, pal::divider);
    rr.rect(lane, pal::appBg);
    rr.pushClip(lane);
    if (env) {
        // The lane, which is now a component and no longer a slice of this
        // function. It is handed the roll's OWN TimeAxis -- the whole property
        // the lane lived in here to preserve, now stated as a parameter -- and
        // the plain view of what its address names. Everything else about it,
        // including its selection and its two drags, is its own.
        //
        // Its interaction runs here rather than up in the interaction block,
        // which is the one ordering the move changes: nothing drawn between the
        // two points reads a breakpoint, and the frame still sees this frame's
        // edit.
        if (lane_.draw(ui, lane, env->points, ta, tgt ? tgt->lo : 0.f,
                       tgt ? tgt->hi : 1.f, tgt ? tgt->unit.c_str() : nullptr,
                       tgt ? tgt->def : 0.f, env->enabled, laneInert,
                       clip.lengthBeats, 0.0, tgt != nullptr,
                       targets.inertWhy.empty() ? nullptr : targets.inertWhy.c_str())) {
            changed = true;
            lastEdit_ = kEditAuto;
        }
    } else if (midiClip) {
        const f32 travel = std::max(1.f, lane.h - 6.f * s);
        const f32 foot = lane.bottom() - 2.f * s;
        for (size_t i = 0; i < clip.notes.size(); ++i) {
            const f32 x = std::round(beatToX(ta, clip.notes[i].beat));
            if (x < lane.x - 2.f * s || x > lane.right()) continue;
            const f32 top = foot - (f32)clip.notes[i].vel / 127.f * travel;
            // Every selected stem is accented, since a lane drag moves all of
            // them; the primary keeps the full accent so the note the gesture
            // is anchored on is still findable inside a large selection.
            const Col c = !sel_.has((int)i)        ? base.scale(0.8f)
                          : (int)i == sel_.primary ? pal::accent
                                                   : pal::accent.alpha(0.75f);
            rr.rect({x, top, std::max(1.f, 1.f * s), foot - top}, c);
            rr.circle(x + 0.5f * s, top, 2.5f * s, c);
        }
    } else if (ui.fSmall) {
        rr.textIn(*ui.fSmall, lane, "pick a target and press + to automate it",
                  pal::textFaint, Align::Center);
    }
    if (playing) {
        const f32 px = beatToX(ta, playheadBeats);
        if (px >= lane.x && px <= lane.right())
            rr.rect({px, lane.y, 1.5f * s, lane.h}, pal::playGreen);
    }
    rr.popClip();
    rr.rect({lane.x, lane.y, lane.w, 1.f * s}, pal::divider);

    // --- cursor ------------------------------------------------------------
    // The lane REPORTS rather than sets, so its answer keeps the place it had
    // in this ordered block instead of overtaking a grid drag that outranks it.
    if (drag_ == Drag::Resize)        ui.cursor = Cursor::ResizeH;
    else if (drag_ == Drag::Move)     ui.cursor = Cursor::Grab;
    else if (lane_.dragging())        ui.cursor = Cursor::Grab;
    else if (drag_ == Drag::Velocity) ui.cursor = Cursor::ResizeV;
    else if (hotLane && env)          ui.cursor = lane_.pointHovered() ? Cursor::Grab
                                                                      : Cursor::Hand;
    else if (hotLane)                 ui.cursor = Cursor::ResizeV;
    else if (hotGrid && midiClip) {
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
// True for a note selection OR a breakpoint selection: the caller asks one
// question ("does the editor own this key?") and the answer has to cover both
// halves of the editor, or Delete in the lane would clear the whole clip.
bool PianoRoll::hasSelection(const ClipModel& clip) const {
    if (!owns(clip)) return false;
    if (!sel_.empty() && sel_.items.back() < (int)clip.notes.size()) return true;
    const AutoLane* env = shownLane(clip);
    return env && lane_.hasSelection((int)env->points.size());
}

bool PianoRoll::clearSelection() {
    const bool any = !sel_.empty() || lane_.hasSelection();
    if (!any) return false;
    sel_.clear();
    lane_.clearSelection();
    return true;
}

// The lane takes the keys while it has a selection of its own: the arrows then
// nudge breakpoints by one grid step and one kValueNudge of the target's range
// instead of nudging notes, which is what the hand that just clicked a
// breakpoint means by "left".
bool PianoRoll::nudgeSelected(ClipModel& clip, int gridSteps, int semitones) {
    if (!owns(clip)) return false;
    AutoLane* env = shownLane(clip);
    if (env && lane_.hasSelection((int)env->points.size())) {
        if (!lane_.nudgeSelected(env->points, gridSteps, (f32)semitones, clip.lengthBeats))
            return false;
        lastEdit_ = kEditAuto;
        return true;
    }
    if (!hasSelection(clip) || sel_.empty()) return false;
    const NudgeResult res = nudgeGroup(clip.notes, sel_, gridSteps, semitones,
                                       clip.lengthBeats);
    if (!res.changed) return false;                // already against a clamp
    followSel_ = true;
    // One audition for the group: the primary note. A held arrow key on a
    // thirty-note chord would otherwise be a wall of retriggers.
    if (res.pitchChanged && sel_.primary >= 0 && sel_.primary < (int)clip.notes.size())
        preview_.push((int)clip.notes[(size_t)sel_.primary].pitch);
    lastEdit_ = kEditNote;
    return true;
}

bool PianoRoll::deleteSelected(ClipModel& clip) {
    if (!owns(clip)) return false;
    // Back to front in both cases: an index into a vector survives only until
    // something earlier than it is removed, and both sets are sorted.
    AutoLane* env = shownLane(clip);
    if (env && lane_.deleteSelected(env->points)) {
        lastEdit_ = kEditAuto;
        return true;
    }
    if (!hasSelection(clip) || sel_.empty()) return false;
    for (size_t k = sel_.items.size(); k-- > 0;) {
        const int i = sel_.items[k];
        if (i >= 0 && i < (int)clip.notes.size())
            clip.notes.erase(clip.notes.begin() + i);
    }
    sel_.clear();
    // A drag cannot be in flight (this arrives from the keyboard), but the
    // index would be stale if one ever were.
    dragNote_ = -1;
    drag_ = Drag::None;
    lastEdit_ = kEditNote;
    return true;
}

bool PianoRoll::duplicateLoop(ClipModel& clip) {
    if (!owns(clip)) return false;
    const DupResult res = duplicateLoopNotes(clip.notes, clip.lengthBeats, sel_);
    if (!res.changed) return false;                // already at the cap
    sel_ = res.sel;
    followSel_ = true;
    lastEdit_ = kEditNote;
    return true;
}

} // namespace lat
