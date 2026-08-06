// One automation lane: a breakpoint editor drawn against a caller-supplied
// time axis.
//
// This is the extraction docs/AUTOMATION.md §6.5 specified and argued for in
// advance — *"an audio clip has no piano roll and wants exactly this lane under
// its waveform, and a lane that was born inside the roll would have to be
// extracted then, with a live editor's state to carry across."* It did not
// happen when the lane shipped; it happens here, as a MOVE, because the
// arrangement's expandable lanes are the second caller decision 11 already
// referred to as though this class existed (docs/ARRANGEMENT.md §7.3).
//
// The lane owns NO time axis of its own. It is handed one, so the lane and
// whatever is above it can never disagree about where a beat is — which is the
// same reason the lane lived inside the roll in the first place, now stated as
// a parameter instead of as a file boundary.
//
// Deliberately knows nothing about App, ClipModel, devices or the engine: it
// edits a std::vector<AutoPoint> in place and reports whether it changed. What
// the lane may *name* — the address, the chooser, the "+" that adds one — is
// the caller's, because that is the part that differs between a clip envelope
// and a track lane.
#pragma once
#include "session.h"
#include "timeaxis.h"
#include "widgets.h"
#include <vector>

namespace lat {

// A set of indices into some vector, one member singled out as the primary —
// the one the last gesture was actually about. It lived in pianoroll.h and
// moves here with the lane that is its second user: the two index spaces (notes
// and breakpoints) are different, but every rule about them is the same, and
// this is now the lower of the two headers. Sorted because a multi-delete has
// to run back to front and a bounds check only needs the last element; unique
// because every group edit would otherwise apply twice to the same member; the
// primary is always inside the set while there is one to have it.
// GUI thread only.
struct IndexSel {
    std::vector<int> items;          // sorted ascending, unique
    int primary = -1;                // a member of `items`, or -1

    bool empty() const { return items.empty(); }
    int  count() const { return (int)items.size(); }
    bool has(int i) const;
    void clear();
    void one(int i);                 // the set becomes {i}, primary = i
    void add(int i);                 // no-op when already in
    void toggle(int i);
    void erased(int at);             // after `at` was erased from the vector
    void prune(int n);               // drops anything past the end
    // Adopts a band's base set and re-anchors the primary inside it.
    void adopt(const std::vector<int>& v);
};

// The renderer keeps its DPI scale private and a view is handed only a Rect, so
// recover the scale from the font: App loads fSmall at round(9 * dpiScale) px
// and fBody at round(11 * dpiScale). Rounding costs at most ~5% at 1x, which is
// invisible in layout maths and cheaper than widening the frozen interface.
// Moved out of pianoroll.cpp with the lane, because the lane and the
// arrangement both need it and two copies would be two answers.
inline f32 dpiOf(const Ui& ui) {
    if (ui.fSmall && ui.fSmall->size() > 0) return std::max(0.5f, (f32)ui.fSmall->size() / 9.f);
    if (ui.fBody  && ui.fBody->size()  > 0) return std::max(0.5f, (f32)ui.fBody->size()  / 11.f);
    return 1.f;
}

// A breakpoint is a 5 px square, grabbed from a little further out so it can be
// picked up without the cursor having to land exactly on it.
inline constexpr f32 kPtSize = 5.f;
inline constexpr f32 kPtGrab = 7.f;
// One arrow press of value, as a fraction of the target's range: coarse enough
// to see, fine enough to trim a fade with.
inline constexpr f32 kValueNudge = 1.f / 64.f;

class AutoLaneView {
public:
    // Draws one automation lane against a caller-supplied time axis and handles
    // every interaction inside `r`. Returns true when `pts` changed, in which
    // case the caller must re-push whatever owns them.
    //
    //   lo/hi/def/unit  the target's range, its resting value and its units —
    //                   the plain view of a target, never the target itself.
    //   enabled         the lane's own on/off (Live's "deactivate envelope").
    //   inert           the engine has given up on this lane (§3.4).
    //   resolved        the address names something in this set TODAY. False
    //                   keeps the points and greys the lane, because losing a
    //                   filter sweep because a plugin is missing is the worst
    //                   bug this feature could have.
    //   lengthBeats     the wall a drag clamps against.
    //   beatBase        what the lane's point beats are relative to: 0 for a
    //                   clip envelope (clip-relative) and 0 for an arrangement
    //                   lane (already absolute) — the parameter exists so a
    //                   future clip-lane-on-the-timeline can offset.
    //
    // `resolved` and `inertWhy` are two parameters more than §7.3's sketch has.
    // They are what the roll's own lane already consulted (a lane naming a
    // deleted device is drawn differently from one that is merely switched off,
    // and each says which in one line), so a move that dropped them would have
    // changed the rendering — and a move that changes rendering is not a move.
    bool draw(Ui& ui, const Rect& r, std::vector<AutoPoint>& pts,
              const TimeAxis& ta, f32 lo, f32 hi, const char* unit, f32 def,
              bool enabled, bool inert, f64 lengthBeats, f64 beatBase,
              bool resolved = true, const char* inertWhy = nullptr);

    // The widget id this lane claims for hover and drag. Defaults to the id the
    // roll's single lane has always used, so the roll's hot/active behaviour is
    // unchanged by the move; the arrangement gives each of its lanes one of its
    // own, because there are many and they must not share a hot slot.
    void setId(u64 id) { id_ = id; }
    u64  id() const { return id_; }

    // For the caller's cursor logic. The roll decides the cursor for its whole
    // rect in one ordered block at the end of its draw, so the lane reports
    // rather than sets: setting it here would put the lane's answer ahead of a
    // grid drag that outranks it.
    bool dragging() const { return drag_ != Drag::None; }
    bool pointHovered() const { return hover_; }

    // Keyboard verbs, for a caller routing Delete / Escape / the arrows. Each
    // takes the vector rather than trusting the last one drawn: the caller can
    // put a different lane in front of this view between frames, and a stale
    // index is a wrong-point edit.
    // `lengthBeats` is the wall, exactly as it is for a mouse move: §7.3's
    // sketch omits it, and omitting it would let an arrow key push a breakpoint
    // past the end of the thing that owns it — a behaviour change, which a move
    // is not allowed to make.
    bool nudgeSelected(std::vector<AutoPoint>& pts, int gridSteps, f32 valueSteps,
                       f64 lengthBeats);
    bool deleteSelected(std::vector<AutoPoint>& pts);
    bool clearSelection();
    bool hasSelection() const { return !sel_.empty(); }
    // The same question asked safely: a set of any size answers yes, and one
    // index out of range condemns the lot, because the set is only ever rebuilt
    // as a whole. (It is sorted, so the last element is the only one to check.)
    bool hasSelection(int n) const { return !sel_.empty() && sel_.items.back() < n; }
    // Drops any index past `n`, and a point drag that no longer points at a
    // point. The caller's vector can shrink under a live selection (an undo, a
    // project load), and an index is only ever as good as the frame it was made
    // in. A band drag survives a shrink: it holds no index.
    void prune(int n) {
        sel_.prune(n);
        if (drag_ == Drag::Point && (dragPt_ < 0 || dragPt_ >= n)) {
            dragPt_ = -1;
            drag_ = Drag::None;
        }
    }
    // No lane on show at all. Everything an index could refer to is gone, so
    // both drags end and the selection with them.
    void detach() {
        sel_.clear();
        dragPt_ = -1;
        drag_ = Drag::None;
        bandBase_.clear();
    }
    // The value range the lane was last drawn against, which is what a keyboard
    // nudge — which runs before the frame that would hand a range over — is
    // measured and clamped against. 0..1 until a lane has been drawn, which is
    // the range of every mixer target anyway.
    f32 rangeLo() const { return lo_; }
    f32 rangeHi() const { return hi_; }

private:
    IndexSel sel_;               // breakpoints of the vector last drawn
    u64  id_ = uiId(20, 1);
    // Point / Band are deliberately the same two shapes the note grid's drags
    // have, and for the same reason: the lane's verbs are the grid's verbs.
    enum class Drag { None, Point, Band } drag_ = Drag::None;
    int  dragPt_ = -1;
    f64  dragPtBeat_ = 0.0;
    f32  dragPtVal_ = 0.f;
    // The band's anchor. Its coordinates are a BEAT and a VALUE rather than two
    // pixels, so a wheel mid-band leaves the corner on the material it was put
    // on rather than on a pixel.
    f64  bandBeat_ = 0.0;
    f32  bandVal_ = 0.f;
    // The selection as it stood when the band started. The band adds to it
    // rather than replacing it — Shift means "and also" here as everywhere —
    // which also means a band that touches nothing takes nothing away.
    std::vector<int> bandBase_;
    // "The press before this one created a breakpoint." Clicking empty lane
    // adds, so without this the second click of a double-click on empty lane
    // would delete what the first click made.
    bool addedLastPress_ = false;
    bool hover_ = false;         // a point is under the cursor, for the cursor
    f32  lo_ = 0.f, hi_ = 1.f;
};

} // namespace lat
