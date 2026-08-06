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
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace lat {

// ---------------------------------------------------------------------------
// constants (logical px unless noted; multiply by the DPI scale)
// ---------------------------------------------------------------------------

inline constexpr f64 kGridStep    = 0.25;   // 1/16 note, the only grid this wave
inline constexpr int kBeatsPerBar = 4;      // no time signature reaches us; assume 4/4
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
inline void drawTimeGrid(Renderer& rr, const TimeAxis& ta, const Rect& r, f32 s,
                         int beatsPerBar = kBeatsPerBar, f32 minStepPx = 0.5f) {
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
        rr.rect({std::round(x), r.y, 1.f * s, r.h},
                onBar ? pal::ridge.scale(1.45f) : (onBeat ? pal::ridge : pal::divider));
    }
}

} // namespace lat
