// The time axis, shared by every editor that draws beats along x.
//
// This is a MOVE, not a new component (docs/ARRANGEMENT.md §7.2): `TimeAxis`,
// `beatToX`, `xToBeat` and `zoomView` lived in `pianoroll.cpp`'s anonymous
// namespace, together with the constants that are about *time*. The piano roll
// and the arrangement need the identical mapping, and two copies of it would
// drift invisibly -- the bug being a note and a clip disagreeing about where
// beat 12 is, at some zooms, after some scrolls, with both halves looking right
// in isolation. Ctrl+wheel is the other half of the argument: `zoomView`'s
// anchoring is a feel decision with an off-by-a-clamp failure mode, and two
// copies would be two feels in one program.
//
// What deliberately did NOT move: kKeyW, kRulerH, kLaneH, kRowH, kMinFoldRows,
// kCentrePitch, PitchAxis and RowMap. Those are about a piano roll rather than
// about a timeline, and they stay in pianoroll.cpp.
//
// The functions left an anonymous namespace, so they are `inline` in namespace
// lat. Nothing else about them changed, which is what makes the move checkable:
// the roll must render pixel-identically afterwards.
#pragma once
#include "../gfx/renderer.h"
// engine.h, for RtSig and the sig* conversions ONLY. This header is the bottom
// of the view stack and it now has to answer "where does bar 17 start", which is
// a question with exactly one right answer -- the one the metronome, the launch
// quantum and the position readout already use. Re-deriving it here from a
// copied map would produce a ruler that agrees with the engine on every set
// anybody tested and disagrees on the first one they did not; forwarding to
// sigBeatOfBar/sigBarOfBeat/sigPosAt makes a drawn bar line and a played one the
// SAME computation rather than two that happen to match. No Engine object, no
// Command and no RtClip cross this seam -- see SigMap below.
#include "../audio/engine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace lat {

// ---------------------------------------------------------------------------
// constants (logical px unless noted; multiply by the DPI scale)
// ---------------------------------------------------------------------------

inline constexpr f64 kGridStep    = 0.25;   // 1/16 note, the only grid this wave
// The bar length assumed by every caller that has no signature map: the piano
// roll, whose bars are CLIP-local, and the two grid helpers' defaults. A caller
// that has a map passes a SigMap instead and this is not consulted.
inline constexpr int kBeatsPerBar = 4;
// Fit-to-width, the zoom a clip is first shown at, is kept inside a sane band:
// a two-beat sketch must not draw beats a hand-span apart, and a 64-bar clip
// must not open at one pixel per bar.
inline constexpr f32 kPxPerBeatMin = 44.f;
inline constexpr f32 kPxPerBeatMax = 128.f;
// Ctrl+wheel reaches much further in both directions than the fit ever does:
// far enough out to see a long pattern whole, far enough in to place a note
// against the grid line rather than near it.
inline constexpr f32 kZoomMin      = 8.f;
inline constexpr f32 kZoomMax      = 512.f;
inline constexpr f32 kZoomPerNotch = 0.25f;  // octaves of zoom per wheel notch

// ---------------------------------------------------------------------------
// the axis
// ---------------------------------------------------------------------------

// `view` is the scroll offset in content pixels; `x0` the screen origin of the
// grid. Both directions are affine, so one struct covers draw and hit.
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

// Snapping, on the one grid this wave has. Moved with the axis because a snap
// is a statement about the time axis and nothing else, and because the
// arrangement quantizes its drags against exactly the grid the roll draws.
inline f64 quantFloor(f64 b) { return std::floor(b / kGridStep) * kGridStep; }
inline f64 quantNear(f64 b)  { return std::floor(b / kGridStep + 0.5) * kGridStep; }

// ---------------------------------------------------------------------------
// the signature map, as a view sees it
//
// A BORROWED pointer into the session's own normalized vector, plus the one
// entry an empty map stands for. Deliberately the same shape as Session's own
// forwarders (session.h): the same `lone()` fallback, the same four questions,
// the same three functions underneath. What it adds is that a view can hold one
// without holding a Session -- ArrangeContext's whole rule -- and that copying
// it is copying a pointer and two ints, so nothing here can outlive an edit by
// caching a bar length.
//
// NOTHING IN HERE MULTIPLIES. Bars stop being the same width in beats the moment
// a set has two signatures in it, so "bar N starts at N * sigNum" is not an
// optimisation to keep for the common case -- it is the bug, and it is invisible
// until somebody re-bars a piece.
// ---------------------------------------------------------------------------
struct SigMap {
    const RtSig* v = nullptr;      // the session's vector, or null
    int          n = 0;
    int          fbNum = 4, fbDen = 4;   // what an EMPTY map means (Session::lone)

    // The one-entry map an empty `v` stands for. Built on the stack every time
    // rather than stored, so a copy of this struct never points into the copy
    // it was made from.
    RtSig lone() const {
        RtSig c{};
        c.num = fbNum > 0 ? fbNum : 4;
        c.den = fbDen > 0 ? fbDen : 4;
        return c;
    }
    bool  empty() const { return !v || n <= 0; }
    int   count() const { return empty() ? 1 : n; }
    RtSig entry(int i) const {
        if (empty()) return lone();
        return v[clampv(i, 0, n - 1)];
    }
    // "One signature, and its denominator is 4" -- the shape every set that has
    // never been re-barred has, and the one the uniform grid below draws.
    bool uniform() const { return count() == 1; }

    f64 beatOfBar(f64 bar) const {
        const RtSig o = lone();
        return empty() ? sigBeatOfBar(&o, 1, bar) : sigBeatOfBar(v, n, bar);
    }
    f64 barOfBeat(f64 beat) const {
        const RtSig o = lone();
        return empty() ? sigBarOfBeat(&o, 1, beat) : sigBarOfBeat(v, n, beat);
    }
    RtSig sigAtBar(i64 bar) const {
        const RtSig o = lone();
        return empty() ? o : v[sigIndexAtBar(v, n, bar)];
    }
    BarPos posAt(f64 beat) const {
        const RtSig o = lone();
        return empty() ? sigPosAt(&o, 1, beat) : sigPosAt(v, n, beat);
    }
    // The index of the entry that STARTS at `bar`, or -1. Not sigIndexAtBar,
    // which answers "which entry covers it": the ruler's marker and the editor's
    // remove both mean "is there a change exactly here".
    int entryStartingAt(i64 bar) const {
        if (empty()) return bar == 0 ? 0 : -1;
        for (int i = 0; i < n; ++i) if ((i64)v[i].bar == bar) return i;
        return -1;
    }
    // The shortest bar anywhere in the map, in beats. What the bar-line
    // thinning has to be chosen against: a stride that leaves the widest bar
    // legible still crowds the narrowest.
    f64 shortestBar() const {
        f64 m = sigBarBeats(entry(0).num, entry(0).den);
        for (int i = 1; i < count(); ++i)
            m = std::min(m, sigBarBeats(entry(i).num, entry(i).den));
        return m > 0.0 ? m : 4.0;
    }
};

// ---------------------------------------------------------------------------
// the shared ruler and grid
//
// Both bodies are lifted verbatim out of PianoRoll::draw, parameterised only in
// the colours and the bar length. They are what makes "the arrangement's ruler
// and the roll's ruler are the same ruler" a fact rather than an intention.
// ---------------------------------------------------------------------------

// The coarsest step that is still at least `minPx` wide on screen, taken from a
// musical ladder rather than from a continuum: a grid that thins by halving
// stays on the bar lines, and one that thins by pixels lands between them.
// Returns `finest` when even that is wide enough, which is what makes the
// default behaviour of both helpers below exactly what it was before they
// learned to thin (docs/ARRANGEMENT.md §7.2 — the move must not change the
// roll's rendering, and the roll passes no minimum).
inline f64 stepAtLeast(f32 pxPerBeat, f32 minPx, f64 finest, int beatsPerBar) {
    if (finest * (f64)pxPerBeat >= (f64)minPx) return finest;
    const f64 bar = (f64)(beatsPerBar > 0 ? beatsPerBar : kBeatsPerBar);
    const f64 ladder[] = {1.0, bar, bar * 2, bar * 4, bar * 8, bar * 16, bar * 32, bar * 64};
    for (f64 c : ladder)
        if (c > finest && c * (f64)pxPerBeat >= (f64)minPx) return c;
    return std::max(finest, ladder[7]);
}

// bar.beat numbers along `ta`, between `x0` and `x1`, with their baseline at
// `ty`. The caller has already pushed whatever clip the labels belong inside.
//
// `minGapPx` is what an arrangement needs and a piano roll does not: at 16
// logical px per beat a label on every beat is four numbers in the space of one
// and reads as noise. Zero -- the roll's value -- labels every beat exactly as
// before.
inline void drawRulerLabels(Renderer& rr, const Font& f, const TimeAxis& ta,
                            f32 x0, f32 x1, f32 ty, f32 s,
                            const Col& onBar, const Col& offBar,
                            int beatsPerBar = kBeatsPerBar, f32 minGapPx = 0.f) {
    const int bpb = beatsPerBar > 0 ? beatsPerBar : kBeatsPerBar;
    const f64 step = minGapPx > 0.f ? stepAtLeast(ta.pxPerBeat, minGapPx, 1.0, bpb) : 1.0;
    const i64 sb = (i64)std::llround(step);
    const i64 b0 = std::max<i64>(0, (i64)std::floor(xToBeat(ta, x0)));
    const i64 b1 = (i64)std::ceil(xToBeat(ta, x1));
    for (i64 b = (b0 / sb) * sb; b <= b1; b += sb) {
        if (b < b0) continue;
        char buf[48];
        // Once the step is a whole bar or more the beat part is always 1, and a
        // ruler that says "5.1 9.1 13.1" is spending half its width saying
        // nothing. Bar numbers alone, then.
        if (sb >= bpb) std::snprintf(buf, sizeof buf, "%lld", (long long)(b / bpb + 1));
        else           std::snprintf(buf, sizeof buf, "%lld.%lld",
                                     (long long)(b / bpb + 1), (long long)(b % bpb + 1));
        rr.text(f, std::round(beatToX(ta, (f64)b)) + 3.f * s, ty, buf,
                (b % bpb) == 0 ? onBar : offBar);
    }
}

// The vertical grid: one line per kGridStep, accented on beats and again on
// bars. `r` is the band the lines are drawn down.
//
// `minStepPx` thins it the same way, and for the same reason: 1/16 lines four
// pixels apart are a texture and not a grid. The default is the threshold the
// roll has always used, so the roll draws exactly what it drew.
// The walk itself, so a test can read the lines this draws without a GL context
// (tests/timesig_view_test.cpp compares it against the map-walking version
// below). Extracted, not rewritten: same expressions in the same order, so the
// rects come out where they always did.
template <class F>
inline void forEachUniformGridLine(const TimeAxis& ta, const Rect& r, int beatsPerBar,
                                   f32 minStepPx, F&& emit) {
    const int bpb = beatsPerBar > 0 ? beatsPerBar : kBeatsPerBar;
    const f64 step = stepAtLeast(ta.pxPerBeat, minStepPx, kGridStep, bpb);
    const f64 startB = std::max(0.0, std::floor(xToBeat(ta, r.x) / step) * step);
    const f32 stepPx = ta.pxPerBeat * (f32)step;
    const int steps = stepPx > 0.5f ? (int)(r.w / stepPx) + 2 : 0;
    for (int k = 0; k <= steps; ++k) {
        const f64 b = startB + (f64)k * step;
        const f32 x = beatToX(ta, b);
        if (x > r.right()) break;
        const bool onBeat = std::fabs(b - std::round(b)) < 1e-6;
        const bool onBar  = onBeat && ((i64)std::llround(b) % bpb) == 0;
        emit(b, x, onBar ? 2 : (onBeat ? 1 : 0));
    }
}

inline void drawTimeGrid(Renderer& rr, const TimeAxis& ta, const Rect& r, f32 s,
                         int beatsPerBar = kBeatsPerBar, f32 minStepPx = 0.5f) {
    forEachUniformGridLine(ta, r, beatsPerBar, minStepPx, [&](f64, f32 x, int level) {
        rr.rect({std::round(x), r.y, 1.f * s, r.h},
                level == 2 ? pal::ridge.scale(1.45f)
                           : (level == 1 ? pal::ridge : pal::divider));
    });
}

// ---------------------------------------------------------------------------
// the same ruler and grid, over a SIGNATURE MAP
//
// The two above multiply: one step in beats, chosen once, walked from zero.
// That is exactly right for a set in one signature and exactly wrong for a set
// with two, because the second bar of a 7/8 passage does not start at a multiple
// of anything. The two below WALK the map instead -- bar N begins where
// beatOfBar puts it, and its beat subdivisions come from the signature covering
// that bar -- so a bar line the ruler draws and a bar line the metronome strikes
// are the same number out of the same function.
//
// THE UNIFORM CASE IS DELEGATED, NOT REIMPLEMENTED. A map with one entry is a
// uniform grid of `num` units per bar, which is the grid the loops above have
// always drawn; handing it straight to them is what makes "nothing changed for a
// set that has never been re-barred" a bit-identical screenshot rather than a
// judgement about rounding and draw order. tests/timesig_view_test.cpp asserts
// the general walk puts its lines in exactly the same places for those maps, so
// the delegation is a shortcut and not a second definition.
// ---------------------------------------------------------------------------

// How many bars apart the bar lines are drawn, given how narrow the narrowest
// bar in the map is. Powers of two counted FROM BAR 0, which is the rule
// sigNextBarLine already states for phrase boundaries: "every 4th bar" has to
// mean the same bars everywhere or the grid slides when a signature changes.
inline i64 barStrideFor(const SigMap& map, f32 pxPerBeat, f32 minPx) {
    const f64 shortest = map.shortestBar();
    i64 stride = 1;
    while ((f64)stride * shortest * (f64)pxPerBeat < (f64)minPx && stride < (i64)1 << 20)
        stride *= 2;
    return stride;
}

// Every grid line between beats `b0` and `b1`, in INCREASING beat order, handed
// to `emit(beat, level)` with level 2 = bar, 1 = signature unit, 0 = 1/16.
// One walk, called by the grid, by the ruler's labels and by the tests -- the
// reason a label and a line cannot end up on different bars.
template <class F>
inline void forEachGridLine(const SigMap& map, f32 pxPerBeat, f32 minStepPx,
                            f64 b0, f64 b1, F&& emit) {
    if (!(b1 > b0) || !(pxPerBeat > 0.f)) return;
    if (b0 < 0.0) b0 = 0.0;
    const i64 stride = barStrideFor(map, pxPerBeat, minStepPx);
    i64 bar = (i64)std::floor(map.barOfBeat(b0));
    if (bar < 0) bar = 0;
    bar = (bar / stride) * stride;
    // A hard ceiling on the walk. Every caller clips to a rect and breaks on the
    // right edge, but a degenerate map plus a degenerate zoom must not be able to
    // spin the GUI thread instead of drawing a wrong grid.
    int guard = 0;
    for (; guard < 100000; bar += stride) {
        const f64 barStart = map.beatOfBar((f64)bar);
        if (barStart > b1) break;
        ++guard;
        emit(barStart, 2);
        if (stride != 1) continue;
        const RtSig sg = map.sigAtBar(bar);
        const f64 unit = sigBarBeats(1, sg.den);          // 4/den
        const int units = sg.num > 0 ? sg.num : 1;
        const bool beats = units > 1 && unit * (f64)pxPerBeat >= (f64)minStepPx;
        // Only where a 1/16 is actually SHORTER than the unit: a 1/32 signature
        // has units narrower than kGridStep, and a "subdivision" coarser than
        // what it subdivides is not a grid, it is a second set of bar lines in
        // the wrong colour. Independent of `beats` on purpose -- a 1/4 bar has
        // one unit and therefore no beat lines, and it still wants its 1/16s.
        const bool subs = kGridStep < unit - 1e-9 &&
                          kGridStep * (f64)pxPerBeat >= (f64)minStepPx;
        for (int u = 0; u < units; ++u) {
            const f64 ub = barStart + (f64)u * unit;
            if (ub > b1) break;
            if (u > 0) {
                if (!beats) break;
                emit(ub, 1);
            }
            if (!subs) continue;
            for (f64 sb = ub + kGridStep; sb < ub + unit - 1e-9; sb += kGridStep) {
                if (sb > b1) break;
                emit(sb, 0);
            }
        }
    }
}

// The vertical grid over a map. Signature of the uniform version plus the map.
inline void drawTimeGrid(Renderer& rr, const TimeAxis& ta, const Rect& r, f32 s,
                         const SigMap& map, f32 minStepPx = 0.5f) {
    if (map.uniform()) {
        // The one-signature case, drawn by the loop that has always drawn it.
        // A denominator other than 4 makes a "beat" shorter or longer than a
        // quarter note, which that loop cannot express, so only 4 delegates.
        const RtSig sg = map.entry(0);
        if (sg.den == 4) { drawTimeGrid(rr, ta, r, s, sg.num, minStepPx); return; }
    }
    forEachGridLine(map, ta.pxPerBeat, minStepPx,
                    (f64)xToBeat(ta, r.x), (f64)xToBeat(ta, r.right()),
                    [&](f64 b, int level) {
        const f32 x = beatToX(ta, b);
        if (x < r.x - 1.f || x > r.right()) return;
        rr.rect({std::round(x), r.y, 1.f * s, r.h},
                level == 2 ? pal::ridge.scale(1.45f)
                           : (level == 1 ? pal::ridge : pal::divider));
    });
}

// bar.beat numbers over a map. Bar numbers are ONE-BASED for the reader, exactly
// as the uniform version's are, and the beat part counts signature units -- so a
// 7/8 bar reads 5.1 .. 5.7 and not 5.1 .. 5.4 with three of them missing.
inline void drawRulerLabels(Renderer& rr, const Font& f, const TimeAxis& ta,
                            f32 x0, f32 x1, f32 ty, f32 s,
                            const Col& onBar, const Col& offBar,
                            const SigMap& map, f32 minGapPx = 0.f) {
    if (map.uniform()) {
        const RtSig sg = map.entry(0);
        if (sg.den == 4) {
            drawRulerLabels(rr, f, ta, x0, x1, ty, s, onBar, offBar, sg.num, minGapPx);
            return;
        }
    }
    const f32 gap = minGapPx > 0.f ? minGapPx : 1.f;
    forEachGridLine(map, ta.pxPerBeat, gap,
                    (f64)xToBeat(ta, x0), (f64)xToBeat(ta, x1),
                    [&](f64 b, int level) {
        if (level == 0) return;                     // never a label on a 1/16
        const f32 x = beatToX(ta, b);
        if (x < x0 - 40.f * s || x > x1) return;
        const BarPos p = map.posAt(b + 1e-9);       // off the line, into the bar
        char buf[48];
        if (level == 2) std::snprintf(buf, sizeof buf, "%lld", (long long)p.bar + 1);
        else            std::snprintf(buf, sizeof buf, "%lld.%d",
                                      (long long)p.bar + 1, (int)p.beat + 1);
        rr.text(f, std::round(x) + 3.f * s, ty, buf, level == 2 ? onBar : offBar);
    });
}

} // namespace lat
