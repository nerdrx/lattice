// The automation lane, extracted from pianoroll.cpp as a move
// (docs/ARRANGEMENT.md §7.3, docs/AUTOMATION.md §6.5).
//
// Everything above AutoLaneView::draw is pure: no Ui, no Renderer, no member
// state. That is deliberate and is why it was worth moving as a block — the
// value mapping and the edit clamps are the parts worth being able to reason
// about without a window. The TIME axis is deliberately not repeated here: the
// lane is handed the axis its neighbour above is drawn against, which is the
// whole property this class exists to preserve.
#include "autolane.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lat {
namespace {

// Value <-> pixels down the lane. `y0` is the top of the drawable band and `h`
// its height, so `hi` sits at the top and `lo` at the bottom: the value axis
// points up, like every fader in the program.
struct ValAxis {
    f32 y0 = 0.f, h = 1.f;
    f32 lo = 0.f, hi = 1.f;
};
inline f32 valToY(const ValAxis& a, f32 v) {
    const f32 span = a.hi - a.lo;
    const f32 t = span > 1e-9f ? clampv((v - a.lo) / span, 0.f, 1.f) : 0.f;
    return a.y0 + (1.f - t) * a.h;
}
inline f32 yToVal(const ValAxis& a, f32 y) {
    const f32 t = clampv((a.y0 + a.h - y) / std::max(1.f, a.h), 0.f, 1.f);
    return a.lo + t * (a.hi - a.lo);
}

// Points are ordered by beat; the value breaks ties so two points a group move
// stacked on one beat still have a defined order rather than a coin toss.
inline bool ptLess(const AutoPoint& a, const AutoPoint& b) {
    return a.beat != b.beat ? a.beat < b.beat : a.value < b.value;
}
inline bool samePt(const AutoPoint& a, const AutoPoint& b) {
    return a.beat == b.beat && a.value == b.value && a.curve == b.curve;
}

// The selection expressed as POINTS rather than indices — the only form of it
// that survives the re-sort a move forces.
struct PtKeys {
    std::vector<AutoPoint> pts;
    int primary = -1;
};

// Restores the sorted-by-beat invariant and re-derives the selection through
// it. Each key claims a slot of its own so a selection holding two identical
// points never collapses onto one index, and the search starts at the key's
// sorted position so a group drag of the whole lane stays near-linear.
void sortTrackingPts(std::vector<AutoPoint>& v, const PtKeys& keys, IndexSel& out) {
    std::sort(v.begin(), v.end(), ptLess);
    out.clear();
    const size_t n = v.size(), k = keys.pts.size();
    if (k == 0 || n == 0) return;
    std::vector<bool> taken(n, false);
    for (size_t j = 0; j < k; ++j) {
        const AutoPoint& key = keys.pts[j];
        size_t i = (size_t)(std::lower_bound(v.begin(), v.end(), key, ptLess) - v.begin());
        for (; i < n; ++i) {
            if (ptLess(key, v[i])) break;            // past every equal-ordering point
            if (taken[i] || !samePt(v[i], key)) continue;
            taken[i] = true;
            out.items.push_back((int)i);
            if ((int)j == keys.primary) out.primary = (int)i;
            break;
        }
    }
    std::sort(out.items.begin(), out.items.end());
}

// A group move of breakpoints, already clamped: both fields are deltas. The
// walls are the lane at both ends and the target's own range top and bottom,
// and the group is clamped as a group — a fade dragged into the floor keeps its
// shape instead of flattening against it, exactly like a chord in the grid.
struct PtDelta {
    f64 beats = 0.0;
    f32 value = 0.f;
};
PtDelta clampPtDelta(const std::vector<AutoPoint>& pts, const IndexSel& sel,
                     f64 dBeats, f32 dVal, f64 lengthBeats, f32 lo, f32 hi) {
    PtDelta d;
    bool any = false;
    f64 minB = 0.0, maxB = 0.0;
    f32 minV = 0.f, maxV = 0.f;
    for (int i : sel.items) {
        if (i < 0 || i >= (int)pts.size()) continue;
        const AutoPoint& p = pts[(size_t)i];
        if (!any) { minB = maxB = p.beat; minV = maxV = p.value; any = true; }
        else {
            minB = std::min(minB, p.beat);  maxB = std::max(maxB, p.beat);
            minV = std::min(minV, p.value); maxV = std::max(maxV, p.value);
        }
    }
    if (!any) return d;
    const f64 len = std::max(0.0, lengthBeats);
    // A group that already hangs past the end (a loop dragged shorter under it)
    // has negative room to the right, and taking that as the clamp pulls it
    // back inside — which is what the single-point clamp does too.
    d.beats = clampv(dBeats, -minB, std::max(-minB, len - maxB));
    d.value = clampv(dVal, lo - minV, std::max(lo - minV, hi - maxV));
    return d;
}

// Applies a clamped delta to every selected point and restores the sorted-by-
// beat invariant, re-deriving the selection through it. False when the delta
// was zero, in which case nothing was touched and the caller must not report a
// change.
bool applyPtDelta(std::vector<AutoPoint>& pts, IndexSel& sel, const PtDelta& d) {
    if (d.beats == 0.0 && d.value == 0.f) return false;
    PtKeys keys;
    keys.pts.reserve(sel.items.size());
    for (int i : sel.items) {
        if (i < 0 || i >= (int)pts.size()) continue;
        AutoPoint& p = pts[(size_t)i];
        p.beat  = p.beat + d.beats;
        p.value = p.value + d.value;
        if (i == sel.primary) keys.primary = (int)keys.pts.size();
        keys.pts.push_back(p);
    }
    if (keys.pts.empty()) return false;
    sortTrackingPts(pts, keys, sel);
    return true;
}

// Screen position of a breakpoint. Draw and hit test both come through here,
// so they cannot disagree about where a point is. `base` is what the point's
// beat is relative to; it is 0 for every caller today (see AutoLaneView::draw).
inline void ptScreen(const AutoPoint& p, const TimeAxis& ta, const ValAxis& va, f64 base,
                     f32& x, f32& y) {
    x = beatToX(ta, base + p.beat);
    y = valToY(va, p.value);
}

// Nearest breakpoint within `rad` of the cursor, or -1. Later points win ties,
// matching the draw order, and the distance is Chebyshev because the target is
// a square and that is what "inside the square, or nearly" means.
int pointAt(const std::vector<AutoPoint>& pts, const TimeAxis& ta, const ValAxis& va, f64 base,
            f32 mx, f32 my, f32 rad) {
    int found = -1;
    f32 best = rad;
    for (size_t i = 0; i < pts.size(); ++i) {
        f32 x = 0.f, y = 0.f;
        ptScreen(pts[i], ta, va, base, x, y);
        const f32 d = std::max(std::fabs(mx - x), std::fabs(my - y));
        if (d <= best) { best = d; found = (int)i; }
    }
    return found;
}

// Every breakpoint the band touches, in index order. Touching counts, and a
// band with no height still takes what it swept — dragging straight along a
// flat run of points is the commonest way to select one.
void pointsInBand(const std::vector<AutoPoint>& pts, const TimeAxis& ta, const ValAxis& va,
                  f64 base, const Rect& band, f32 half, std::vector<int>& out) {
    out.clear();
    for (size_t i = 0; i < pts.size(); ++i) {
        f32 x = 0.f, y = 0.f;
        ptScreen(pts[i], ta, va, base, x, y);
        if (x + half < band.x || x - half > band.right()) continue;
        if (y + half < band.y || y - half > band.bottom()) continue;
        out.push_back((int)i);
    }
}

// Inserts a breakpoint, keeping the vector sorted, and returns its index. A
// point already sitting on that beat is REPLACED rather than joined: two points
// on one beat are a step the evaluator has no way to render and the user has no
// way to grab separately. (The caller only reaches this for a click on empty
// lane, so the replaced point is one the cursor was not near.)
int insertPoint(std::vector<AutoPoint>& pts, const AutoPoint& p) {
    for (size_t i = 0; i < pts.size(); ++i) {
        if (pts[i].beat == p.beat) { pts[i] = p; return (int)i; }
    }
    pts.push_back(p);
    std::sort(pts.begin(), pts.end(), ptLess);
    for (size_t i = 0; i < pts.size(); ++i)
        if (samePt(pts[i], p)) return (int)i;
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// IndexSel: the selection set, shared by the note grid and the automation lane
//
// Small, sorted and unique, with one member singled out as the primary — the
// member the last gesture was about, which is what the view follows, what the
// audition plays and the anchor a group move is measured from. Every path that
// can invalidate an index goes through one of these, so there is exactly one
// place where a set can get out of step with the vector it indexes — and one
// implementation for both index spaces, which is why a band select, a group
// drag and a multi-delete behave identically in the grid and in the lane.
// ---------------------------------------------------------------------------

bool IndexSel::has(int i) const {
    return i >= 0 && std::binary_search(items.begin(), items.end(), i);
}

void IndexSel::clear() {
    items.clear();
    primary = -1;
}

void IndexSel::one(int i) {
    items.clear();
    if (i >= 0) items.push_back(i);
    primary = i >= 0 ? i : -1;
}

void IndexSel::add(int i) {
    if (i < 0) return;
    const auto it = std::lower_bound(items.begin(), items.end(), i);
    if (it != items.end() && *it == i) return;
    items.insert(it, i);
    if (primary < 0) primary = i;
}

void IndexSel::toggle(int i) {
    if (i < 0) return;
    const auto it = std::lower_bound(items.begin(), items.end(), i);
    if (it != items.end() && *it == i) {
        items.erase(it);
        // The primary has to stay inside the set; which member inherits it does
        // not matter, only that one does while there is one to have it.
        if (primary == i) primary = items.empty() ? -1 : items.front();
        return;
    }
    items.insert(it, i);
    primary = i;                        // the one just added is under the hand
}

void IndexSel::erased(int at) {
    for (size_t k = 0; k < items.size();) {
        if (items[k] == at)      items.erase(items.begin() + (long)k);
        else                   { if (items[k] > at) --items[k]; ++k; }
    }
    if (primary == at)     primary = items.empty() ? -1 : items.front();
    else if (primary > at) --primary;
}

void IndexSel::prune(int n) {
    while (!items.empty() && items.back() >= n) items.pop_back();
    if (items.empty())    primary = -1;
    else if (!has(primary)) primary = items.front();
}

void IndexSel::adopt(const std::vector<int>& v) {
    items = v;
    if (!has(primary)) primary = items.empty() ? -1 : items.front();
}

// ---------------------------------------------------------------------------
// AutoLaneView
//
// Interaction then drawing, in that order and inside one call, exactly as the
// lane behaved when it lived in the roll. The caller has already filled the
// lane's background and pushed a clip for it: the roll draws a playhead over
// the result and a divider along the top, and both belong to whatever owns the
// lane rather than to the lane.
// ---------------------------------------------------------------------------

bool AutoLaneView::draw(Ui& ui, const Rect& r, std::vector<AutoPoint>& pts,
                        const TimeAxis& ta, f32 lo, f32 hi, const char* unit, f32 def,
                        bool enabled, bool inert, f64 lengthBeats, f64 beatBase,
                        bool resolved, const char* inertWhy) {
    if (!ui.r || !ui.in) return false;
    Renderer& rr = *ui.r;
    Input& in = *ui.in;
    const f32 s = dpiOf(ui);
    bool changed = false;

    // Greyed for the three states in which the envelope is drawn but is not
    // driving anything: switched off by hand, given up on by the engine, or
    // naming a target this set cannot resolve today.
    const bool dim = inert || !enabled || !resolved;

    const ValAxis va{r.y + 3.f * s, std::max(1.f, r.h - 6.f * s),
                     resolved ? lo : 0.f, resolved ? hi : 1.f};
    lo_ = va.lo;
    hi_ = va.hi;

    // --- interaction -------------------------------------------------------
    ui.setHot(id_, r);
    const bool hot = ui.isHot(id_);
    hover_ = false;

    auto erasePt = [&](int i) {
        if (i < 0 || i >= (int)pts.size()) return;
        pts.erase(pts.begin() + i);
        sel_.erased(i);
        if (dragPt_ == i) { dragPt_ = -1; drag_ = Drag::None; }
        else if (dragPt_ > i) --dragPt_;
        changed = true;
    };

    Rect bandRect{};
    bool showBand = false;

    if (drag_ != Drag::None) {
        if (!in.down[0]) {
            drag_ = Drag::None;
            dragPt_ = -1;
            bandBase_.clear();
            if (ui.active == id_) ui.active = 0;
        } else if (drag_ == Drag::Point) {
            // The point under the hand is always part of what moves, for the
            // same reason the note under the hand is: Escape can empty the set
            // from the keyboard between frames while the button is still down.
            if (!sel_.has(dragPt_)) sel_.one(dragPt_);
            if (dragPt_ < 0 || dragPt_ >= (int)pts.size()) {
                drag_ = Drag::None;
                dragPt_ = -1;
            } else {
                const AutoPoint p = pts[(size_t)dragPt_];        // copy: we sort below
                // Time is quantized like everything else; Alt frees it for a
                // gesture that has to land between the lines, Ctrl freezes it so
                // a value can be trimmed without the beat sliding.
                f64 dBeats = 0.0;
                if (!in.ctrl()) {
                    const f64 raw = xToBeat(ta, in.mx) - beatBase - dragPtBeat_;
                    dBeats = (in.alt() ? raw : quantNear(raw)) - p.beat;
                }
                const f32 dVal = yToVal(va, in.my) - dragPtVal_ - p.value;
                // Measured on the point under the hand, applied to the whole
                // selection as one delta — so a fade keeps its shape and the
                // group stops when its extreme member reaches a wall.
                const PtDelta d = clampPtDelta(pts, sel_, dBeats, dVal, lengthBeats,
                                               va.lo, va.hi);
                if (applyPtDelta(pts, sel_, d)) changed = true;
                dragPt_ = sel_.primary;
                if (dragPt_ < 0) drag_ = Drag::None;
            }
        } else if (drag_ == Drag::Band) {
            const f32 ax = beatToX(ta, beatBase + bandBeat_);
            const f32 ay = valToY(va, bandVal_);
            bandRect = Rect{std::min(ax, in.mx), std::min(ay, in.my),
                            std::fabs(in.mx - ax), std::fabs(in.my - ay)};
            showBand = true;
            std::vector<int> hits;
            pointsInBand(pts, ta, va, beatBase, bandRect, kPtSize * 0.5f * s, hits);
            sel_.adopt(bandBase_);
            for (int i : hits) sel_.add(i);
        }
    } else if (hot && (in.pressed[0] || in.pressed[2])) {
        // The lane's verbs are the note grid's verbs, deliberately: a user who
        // has learned one editor should not have to learn a second one eight
        // pixels below it.
        const int hit = pointAt(pts, ta, va, beatBase, in.mx, in.my, kPtGrab * s);
        const bool prevAdded = addedLastPress_;
        addedLastPress_ = false;

        if (in.pressed[2]) {
            if (hit >= 0) erasePt(hit);                   // right-click deletes
        } else if (hit >= 0 && in.dblClick && !prevAdded) {
            erasePt(hit);                                 // double-click deletes
        } else if (hit >= 0 && in.shift()) {
            sel_.toggle(hit);                             // membership, no drag
        } else if (hit >= 0) {
            if (!sel_.has(hit)) sel_.one(hit);
            else                sel_.primary = hit;
            const AutoPoint& p = pts[(size_t)hit];
            dragPt_ = hit;
            drag_ = Drag::Point;
            dragPtBeat_ = xToBeat(ta, in.mx) - beatBase - p.beat;
            dragPtVal_  = yToVal(va, in.my) - p.value;
            ui.active = id_;
        } else if (in.shift()) {
            // Rubber band. Plain empty-drag still adds a breakpoint, so the band
            // is what Shift buys on empty lane. The anchor is a beat and a
            // value, not two pixels, so a wheel mid-band leaves the corner on
            // the material it was put on.
            drag_ = Drag::Band;
            dragPt_ = -1;
            bandBeat_ = xToBeat(ta, in.mx) - beatBase;
            bandVal_ = yToVal(va, in.my);
            bandBase_ = sel_.items;
            ui.active = id_;
        } else if (in.pressed[0]) {
            const f64 raw = xToBeat(ta, in.mx) - beatBase;
            const f64 b = in.alt() ? raw : quantNear(raw);
            if (b >= 0.0 && b <= lengthBeats) {
                AutoPoint np;
                np.beat = b;
                np.value = clampv(yToVal(va, in.my), va.lo, va.hi);
                const int idx = insertPoint(pts, np);
                sel_.one(idx);                   // a fresh point is the selection
                dragPt_ = idx;
                // Press-drag-add, as in the grid: the new point is grabbed by
                // the same gesture, so placing it is one movement.
                drag_ = Drag::Point;
                dragPtBeat_ = raw - np.beat;
                dragPtVal_ = yToVal(va, in.my) - np.value;
                addedLastPress_ = true;
                ui.active = id_;
                changed = true;
            }
        }
    }

    // --- drawing -----------------------------------------------------------
    const Col ink = dim ? pal::textDim : pal::accentHi;
    const Col fillC = ink.alpha(dim ? 0.07f : 0.14f);
    const f32 floorY = va.y0 + va.h;

    // "Where is unity" is the question the eye asks first.
    {
        const f32 dy = std::round(valToY(va, resolved ? def : 0.f));
        for (f32 x = r.x; x < r.right(); x += 7.f * s)
            rr.rect({x, dy, 3.f * s, 1.f * s}, pal::ridge);
    }

    // One segment of the polyline, with Live's low-alpha fill down to the
    // lane's floor: the fill is what makes a glance tell you the shape.
    auto seg = [&](f32 x0, f32 y0, f32 x1, f32 y1) {
        const f32 a = std::max(x0, r.x), b = std::min(x1, r.right());
        if (b <= a || x1 <= x0) return;
        const f32 span = x1 - x0;
        for (f32 x = a; x < b; x += 1.f) {
            const f32 y = y0 + (y1 - y0) * ((x - x0) / span);
            rr.rect({x, y, 1.f, std::max(0.f, floorY - y)}, fillC);
        }
        rr.line(a, y0 + (y1 - y0) * ((a - x0) / span),
                b, y0 + (y1 - y0) * ((b - x0) / span), 1.5f * s, ink);
    };

    if (pts.empty()) {
        // An empty lane is UI state, not content: it draws a flat line at the
        // target's default and waits to be clicked on. One breakpoint is a legal
        // constant envelope, not an error.
        const f32 y = std::round(valToY(va, resolved ? def : 0.f));
        for (f32 x = r.x; x < r.right(); x += 5.f * s)
            rr.rect({x, y, 2.5f * s, 1.f * s}, ink.alpha(0.55f));
        if (ui.fSmall && !dim)
            rr.textIn(*ui.fSmall, r, "click to draw a breakpoint", pal::textFaint,
                      Align::Center);
    } else {
        // Before the first point and after the last, the envelope holds — there
        // is no "nowhere" to ramp in from at the start, and nothing past the
        // last point but the end.
        f32 px = r.x, py = valToY(va, pts.front().value);
        for (const AutoPoint& p : pts) {
            f32 x = 0.f, y = 0.f;
            ptScreen(p, ta, va, beatBase, x, y);
            seg(px, py, x, y);
            px = x; py = y;
        }
        seg(px, py, r.right(), py);

        const f32 half = kPtSize * 0.5f * s;
        for (size_t i = 0; i < pts.size(); ++i) {
            f32 x = 0.f, y = 0.f;
            ptScreen(pts[i], ta, va, beatBase, x, y);
            if (x < r.x - half || x > r.right() + half) continue;
            const Rect pr{std::round(x - half), std::round(y - half),
                          kPtSize * s, kPtSize * s};
            if (sel_.has((int)i)) {
                rr.rect(pr, ink);
                if ((int)i == sel_.primary)
                    rr.roundRectOutline(pr.inset(-1.f * s), 0.f, 1.f * s, pal::text);
            } else {
                rr.rect(pr, pal::appBg);
                rr.roundRectOutline(pr, 0.f, 1.f * s, ink);
            }
        }
        // What the hand is actually setting, in the target's own units.
        if (drag_ == Drag::Point && ui.fSmall && sel_.primary >= 0 &&
            sel_.primary < (int)pts.size()) {
            const AutoPoint& p = pts[(size_t)sel_.primary];
            f32 x = 0.f, y = 0.f;
            ptScreen(p, ta, va, beatBase, x, y);
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.3g %s", (double)p.value, unit ? unit : "");
            rr.textIn(*ui.fSmall, {x + 6.f * s, y - 6.f * s, 90.f * s, 12.f * s}, buf,
                      pal::text, Align::Left, 0);
        }
    }
    if (showBand) {
        rr.rect(bandRect, pal::accent.alpha(0.12f));
        rr.roundRectOutline(bandRect, 0.f, 1.f * s, pal::accent);
    }
    if (ui.fSmall) {
        const char* why = nullptr;
        if (inert)          why = (inertWhy && *inertWhy)
                                ? inertWhy
                                : "inert - this device has no realtime parameter path";
        else if (!resolved) why = "target not in this set - the envelope is kept";
        else if (!enabled)  why = "envelope off";
        if (why)
            rr.textIn(*ui.fSmall, {r.x, r.y + 1.f * s, r.w, 11.f * s}, why,
                      pal::textFaint, Align::Left, 5.f * s);
    }

    // What the cursor should be, reported rather than set: the caller decides
    // the cursor for its whole rect in one ordered block, and a lane hover must
    // not outrank a drag that owns the mouse.
    if (hot && drag_ == Drag::None)
        hover_ = pointAt(pts, ta, va, beatBase, in.mx, in.my, kPtGrab * s) >= 0;

    return changed;
}

// ---------------------------------------------------------------------------
// keyboard API
//
// These run from the caller's shortcut handling, i.e. *before* the frame's
// draw(), and act on the state the last draw left behind. Every one takes the
// vector rather than trusting the last one drawn.
// ---------------------------------------------------------------------------

// The arrows nudge breakpoints by one grid step and one kValueNudge of the
// target's range. The value nudge is a fraction of the target's OWN range,
// taken from the last draw, so the arrows move the same visible amount whether
// the lane is a 0..1 fader or a 20 Hz..20 kHz cutoff.
bool AutoLaneView::nudgeSelected(std::vector<AutoPoint>& pts, int gridSteps, f32 valueSteps,
                                 f64 lengthBeats) {
    if (!hasSelection((int)pts.size())) return false;
    const int anchor = sel_.has(sel_.primary) ? sel_.primary : sel_.items.front();
    if (anchor < 0 || anchor >= (int)pts.size()) return false;
    const f64 aBeat = pts[(size_t)anchor].beat;
    // Through the same snap a mouse move uses, so nudging a recorded point pulls
    // it onto the grid instead of carrying its offset forever.
    const f64 want = gridSteps != 0 ? quantNear(aBeat + (f64)gridSteps * kGridStep) - aBeat
                                    : 0.0;
    const f32 step = valueSteps * kValueNudge * (hi_ - lo_);
    const PtDelta d = clampPtDelta(pts, sel_, want, step, lengthBeats, lo_, hi_);
    return applyPtDelta(pts, sel_, d);
}

bool AutoLaneView::deleteSelected(std::vector<AutoPoint>& pts) {
    if (!hasSelection((int)pts.size())) return false;
    // Back to front: an index into a vector survives only until something
    // earlier than it is removed, and the set is sorted.
    for (size_t k = sel_.items.size(); k-- > 0;) {
        const int i = sel_.items[k];
        if (i >= 0 && i < (int)pts.size()) pts.erase(pts.begin() + i);
    }
    sel_.clear();
    dragPt_ = -1;
    drag_ = Drag::None;
    return true;
}

bool AutoLaneView::clearSelection() {
    if (sel_.empty()) return false;
    sel_.clear();
    return true;
}

} // namespace lat
