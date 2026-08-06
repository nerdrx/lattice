// The arrangement editor (docs/ARRANGEMENT.md §7.4, §7.5).
//
// Layout, left to right: a header column carrying each track's name, colour,
// arm and disclosure triangle, then the lanes themselves under a shared bar
// ruler. Every pixel <-> beat conversion goes through timeaxis.h, which is the
// same TimeAxis the piano roll uses — so a note inside an item and the item
// itself can never disagree about where a beat is, at any zoom, after any
// scroll. That is the whole reason the axis was extracted rather than copied.
//
// WHEN THE MODEL MOVES, AND WHEN IT IS REPAIRED. A drag mutates the model live,
// so the item follows the hand, but reports nothing and repairs nothing until
// the button comes up. Two things fall out of that and both are deliberate:
// the engine never sees a lane that has not been through arrangeRepair, and a
// sweep across a neighbour does not eat it a frame at a time — the invariant is
// restored once, against the position the hand actually finished on.
#include "arrange.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lat {
namespace {

// Undo labels, which is what the caller reads back off lastEdit().
constexpr const char* kEditMove  = "move clip";
constexpr const char* kEditTrim  = "trim clip";
constexpr const char* kEditFade  = "clip fade";
constexpr const char* kEditDup   = "duplicate clip";
constexpr const char* kEditDel   = "delete clip";
constexpr const char* kEditSplit = "split clip";
constexpr const char* kEditAuto  = "automation edit";
constexpr const char* kEditLayout = "lane height";

// How far past the last thing on the timeline the view may scroll. A view that
// stopped dead at the last item would give nowhere to drop the next one.
constexpr f64 kArrTailBeats = 32.0;
constexpr f64 kArrMinContent = 64.0;      // 16 bars, so an empty set has a ruler

// A MIDI item's note preview is drawn against a fixed pitch window rather than
// the notes' own range: an item that happens to hold one note would otherwise
// draw it in the middle of a lane that says nothing about the part's register.
constexpr int kPreviewLoPitch = 24;
constexpr int kPreviewHiPitch = 96;
// A looping item repeats its material; the preview stops after this many, which
// is far past what is legible at any zoom a hand works at.
constexpr int kMaxPreviewReps = 128;

// The colour an item draws in. An item on an OVERRIDDEN track is desaturated,
// which is the same visual grammar an overridden automation lane already uses
// (docs/AUTOMATION.md §6.3): the material is still there, it is just not what
// you are hearing.
Col itemColour(int colorIdx, bool overridden) {
    const int n = pal::clipColorCount;
    const Col c = pal::clipColors[((colorIdx % n) + n) % n];
    if (!overridden) return c;
    const f32 lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    return c.mix(Col(lum, lum, lum, c.a), 0.72f).scale(0.7f);
}

// Where a beat inside an item lands in its source clip, as a fraction of the
// clip's length. Returns false when the item has run off the end of a
// non-looping clip, which is silence and must not draw a waveform.
bool srcFraction(const ClipModel& src, f64 clipBeat, f64& outU) {
    const f64 len = src.lengthBeats > 1e-9 ? src.lengthBeats : 1.0;
    f64 b = clipBeat;
    if (src.loop) {
        b = std::fmod(b, len);
        if (b < 0.0) b += len;
    }
    if (b < 0.0 || b >= len) return false;
    outU = b / len;
    return true;
}

// A fresh identity for an item the view has just made. Zero when the caller did
// not hand the counter over, which every path below then treats as "unstamped"
// and leaves for the caller's assignUids.
u64 newUid(ArrangeContext& ctx) { return ctx.nextUid ? (*ctx.nextUid)++ : 0; }

} // namespace

int ArrangeView::indexOf(const std::vector<ArrangeClip>& v, u64 uid) {
    if (!uid) return -1;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].uid == uid) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// the one-shot verbs
//
// Each is what a keyboard or a right-click asks for, and each leaves the lane
// repaired and named in ctx.dirty. The caller has already taken its undo point:
// these are one-shot, so there is no gesture to coalesce on and no way for the
// view to warn ahead.
// ---------------------------------------------------------------------------

u32 ArrangeView::deleteSelected(ArrangeContext& ctx) {
    if (selTrack_ < 0 || selTrack_ >= (int)ctx.lanes.size()) return Changed::None;
    std::vector<ArrangeClip>* items = ctx.lanes[(size_t)selTrack_].items;
    if (!items) return Changed::None;
    const int i = indexOf(*items, selItem_);
    if (i < 0) return Changed::None;
    items->erase(items->begin() + i);
    arrangeRepair(*items);
    ctx.dirty.push_back(selTrack_);
    selItem_ = 0;
    ctx.selItem = 0;
    lastEdit_ = kEditDel;
    return Changed::Items | Changed::Selection;
}

u32 ArrangeView::splitSelected(ArrangeContext& ctx) {
    if (selTrack_ < 0 || selTrack_ >= (int)ctx.lanes.size()) return Changed::None;
    std::vector<ArrangeClip>* items = ctx.lanes[(size_t)selTrack_].items;
    if (!items) return Changed::None;
    const int i = indexOf(*items, selItem_);
    if (i < 0) return Changed::None;
    ArrangeClip& a = (*items)[(size_t)i];
    const f64 at = quantNear(cursorBeat_);
    // A split that would leave either half unreachable is not a split. The
    // floor is kMinArrBeats on BOTH halves, because arrangeRepair would delete
    // whichever fell under it and the user would have asked for two items and
    // been given one, shorter than the one they started with.
    if (at <= a.start + kMinArrBeats || at >= a.end() - kMinArrBeats) return Changed::None;

    ArrangeClip b = a;                      // the copy is the whole item, §2.2
    b.uid = newUid(ctx);                    // stamped now: see ArrangeContext
    const f64 cut = at - a.start;
    b.start  = at;
    b.offset = a.offset + cut;              // the head trim rule, applied to a split
    b.length = a.length - cut;
    // The fades belong to the outer edges: the new inner edges are a butt joint,
    // which is exactly what R3 requires to be inaudible (§3.5).
    b.fadeIn = 0.0;
    a.length = cut;
    a.fadeOut = 0.0;
    if (a.fadeIn > a.length) a.fadeIn = a.length;
    if (b.fadeOut > b.length) b.fadeOut = b.length;
    const u64 keep = a.uid;
    items->insert(items->begin() + (long)i + 1, std::move(b));
    arrangeRepair(*items);
    selItem_ = keep;                        // the head keeps the selection
    ctx.selItem = keep;
    ctx.dirty.push_back(selTrack_);
    lastEdit_ = kEditSplit;
    return Changed::Items;
}

u32 ArrangeView::duplicateSelected(ArrangeContext& ctx) {
    if (selTrack_ < 0 || selTrack_ >= (int)ctx.lanes.size()) return Changed::None;
    std::vector<ArrangeClip>* items = ctx.lanes[(size_t)selTrack_].items;
    if (!items || (int)items->size() >= kMaxArrItems) return Changed::None;
    const int i = indexOf(*items, selItem_);
    if (i < 0) return Changed::None;
    ArrangeClip b = (*items)[(size_t)i];
    b.uid = newUid(ctx);
    b.start = b.end();
    const u64 copy = b.uid;
    items->insert(items->begin() + (long)i + 1, std::move(b));
    arrangeRepair(*items);
    ctx.dirty.push_back(selTrack_);
    // The selection follows into the COPY, for duplicateLoop's reason: the copy
    // is what the user is about to move. It can only do so because the copy has
    // an identity the moment it is made.
    if (copy) { selItem_ = copy; ctx.selItem = copy; }
    lastEdit_ = kEditDup;
    return Changed::Items | Changed::Selection;
}

// ---------------------------------------------------------------------------
// draw
// ---------------------------------------------------------------------------

u32 ArrangeView::draw(Ui& ui, const Rect& r, ArrangeContext& ctx) {
    if (!ui.r || !ui.in) return Changed::None;
    Renderer& rr = *ui.r;
    Input& in = *ui.in;
    const f32 s = dpiOf(ui);
    u32 changed = Changed::None;

    // The caller owns the selection between frames (it is what the detail panel
    // reads), so it is adopted here rather than assumed: a project load or an
    // undo clears it, and a view that kept its own copy would go on drawing an
    // outline around an item the set no longer has.
    selTrack_ = ctx.selTrack;
    selItem_  = ctx.selItem;

    rr.rect(r, pal::appBg);
    const f32 headW = kArrHeaderW * s;
    if (r.w < headW + 80.f * s || r.h < 60.f * s) return changed;

    const Rect ruler{r.x + headW, r.y, r.w - headW, kArrRulerH * s};
    const Rect corner{r.x, r.y, headW, ruler.h};
    const Rect body{r.x, ruler.bottom(), r.w, r.h - ruler.h};
    const Rect heads{body.x, body.y, headW, body.h};
    const Rect lanes{body.x + headW, body.y, body.w - headW, body.h};

    // --- geometry ----------------------------------------------------------
    // One pass over the lanes to work out how tall the stack is and where each
    // track's band starts. Done before anything is drawn or hit-tested, so the
    // two can never disagree about which lane a y is in.
    struct Row { f32 y = 0, h = 0, autoY = 0, autoH = 0; int autos = 0; };
    std::vector<Row> rows(ctx.lanes.size());
    f32 contentH = 0.f;
    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        const ArrangeContext::Lane& L = ctx.lanes[i];
        Row& row = rows[i];
        row.y = contentH;
        row.h = clampv(L.height ? *L.height : kArrHeightDefault,
                       kArrMinLaneH, kArrMaxLaneH) * s;
        // One row per existing lane plus one for the chooser that adds the next.
        row.autos = (L.expanded && *L.expanded && L.autos)
                        ? (int)L.autos->size() + 1
                        : 0;
        row.autoY = row.y + row.h;
        row.autoH = (f32)row.autos * kArrAutoLaneH * s;
        contentH += row.h + row.autoH + 1.f * s;
    }

    f64 contentBeats = kArrMinContent;
    for (const ArrangeContext::Lane& L : ctx.lanes)
        if (L.items && !L.items->empty())
            contentBeats = std::max(contentBeats, L.items->back().end() + kArrTailBeats);
    if (ctx.loopEnd) contentBeats = std::max(contentBeats, *ctx.loopEnd + kArrTailBeats);
    contentBeats = std::max(contentBeats, ctx.playhead + kArrTailBeats);

    // --- wheel -------------------------------------------------------------
    const bool overBody = (lanes.contains(in.mx, in.my) || heads.contains(in.mx, in.my) ||
                           ruler.contains(in.mx, in.my)) &&
                          rr.currentClip().contains(in.mx, in.my);
    f32 pxPerBeat = zoom_ * s;
    if (overBody && in.wheel != 0.f) {
        if (in.ctrl()) {
            // Zoom about the cursor, through the roll's own zoomView: the beat
            // under the hand stays under the hand, and a notch is the same
            // proportion at every scale. Identical feel in both editors is not a
            // nicety — it is the reason timeaxis.h exists.
            const f32 nz = clampv(zoom_ * std::pow(2.f, in.wheel * kZoomPerNotch),
                                  kZoomMin, kZoomMax);
            if (nz != zoom_) {
                const TimeAxis prev{lanes.x, pxPerBeat, scrollX_};
                zoom_ = nz;
                pxPerBeat = zoom_ * s;
                scrollX_ = zoomView(prev, pxPerBeat, clampv(in.mx, lanes.x, lanes.right()),
                                    contentBeats, lanes.w);
            }
        } else if (in.shift()) {
            scrollX_ -= in.wheel * pxPerBeat * 0.5f;
        } else {
            scrollY_ -= in.wheel * 40.f * s;
        }
    }
    const f32 contentW = (f32)(contentBeats * (f64)pxPerBeat);
    scrollX_ = clampv(scrollX_, 0.f, std::max(0.f, contentW - lanes.w));
    scrollY_ = clampv(scrollY_, 0.f, std::max(0.f, contentH - lanes.h));

    const TimeAxis ta{lanes.x, pxPerBeat, scrollX_};
    const f32 topY = lanes.y - scrollY_;

    // The clip lanes claim their hot rect HERE, before the automation lanes are
    // drawn: setHot is last-writer-wins, so an expanded lane's own rect has to
    // be claimed after this one or it could never be hot.
    const u64 lanesId = uiId(24, 7);
    ui.setHot(lanesId, lanes);
    const bool hotLanes = ui.isHot(lanesId);
    if (lanes.contains(in.mx, in.my)) cursorBeat_ = std::max(0.0, xToBeat(ta, in.mx));
    else if (!lanes.contains(in.mx, in.my) && drag_ == Drag::None) cursorBeat_ = ctx.playhead;

    // Which track a y lands in, and -1 for the gaps. Both the drop target and a
    // cross-track move go through this, so a move cannot land somewhere the eye
    // says is a different lane.
    const auto trackAtY = [&](f32 y) {
        for (size_t i = 0; i < rows.size(); ++i) {
            const f32 y0 = topY + rows[i].y;
            if (y >= y0 && y < y0 + rows[i].h + rows[i].autoH) return (int)i;
        }
        return -1;
    };

    // --- ruler: bar numbers, the loop brace, the playhead -------------------
    rr.rect(ruler, pal::panel);
    rr.rect(corner, pal::panel);
    rr.rect({ruler.x, ruler.bottom() - 1.f * s, ruler.w, 1.f * s}, pal::divider);
    rr.rect({corner.right() - 1.f * s, corner.y, 1.f * s, corner.h}, pal::divider);

    const u64 rulerId = uiId(24, 0);
    ui.setHot(rulerId, ruler);
    const bool hotRuler = ui.isHot(rulerId);

    if (in.pressed[0] && hotRuler && drag_ == Drag::None) {
        drag_ = Drag::Loop;
        gesture_ = rulerId;
        loopAnchor_ = std::max(0.0, quantNear(xToBeat(ta, in.mx)));
        loopMoved_ = false;
        moved_ = true;              // the brace is not an edit to any lane
        ui.active = rulerId;
    }
    // THE SIGNATURE EDITOR, half one: right-click on the ruler adds a change at
    // the bar under the cursor, or removes the one already there. The NEAREST
    // bar line and not the bar the cursor is inside -- a signature change is a
    // thing that sits on a bar line, and "the bar I clicked in" would make the
    // right half of every bar unable to reach the line on its right.
    //
    // Right-click, because both of the left button's jobs on this ruler are
    // already spoken for (a click locates, a drag is the loop brace) and because
    // it is the button this program already spends on "the other verb" -- a
    // right-click deletes an item in the lanes below.
    if (in.pressed[2] && hotRuler && drag_ == Drag::None) {
        const f64 cb = std::max(0.0, (f64)xToBeat(ta, in.mx));
        i64 bar = (i64)std::floor(ctx.sig.barOfBeat(cb) + 0.5);
        ctx.sigBar = bar < 0 ? 0 : bar;
    }

    if (drag_ == Drag::Loop) {
        if (!in.down[0]) {
            if (!loopMoved_) ctx.locateBeat = loopAnchor_;
            drag_ = Drag::None;
            gesture_ = 0;
            if (ui.active == rulerId) ui.active = 0;
        } else {
            const f64 b = std::max(0.0, quantNear(xToBeat(ta, in.mx)));
            if (!loopMoved_ &&
                std::fabs(beatToX(ta, b) - beatToX(ta, loopAnchor_)) > 3.f * s)
                loopMoved_ = true;
            if (loopMoved_ && ctx.loopStart && ctx.loopEnd && ctx.loopOn) {
                *ctx.loopStart = std::min(loopAnchor_, b);
                *ctx.loopEnd   = std::max(loopAnchor_, b);
                *ctx.loopOn    = true;
                changed |= Changed::Loop;
            }
        }
    }

    rr.pushClip(ruler);
    if (ui.fSmall)
        drawRulerLabels(rr, *ui.fSmall, ta, ruler.x, ruler.right(),
                        ruler.y + (ruler.h - ui.fSmall->height()) * 0.5f, s,
                        pal::textDim, pal::textFaint, ctx.sig, 44.f * s);
    // The signature markers. Drawn ONLY for a map that has more than one entry,
    // which is not a shortcut: a set in one signature has nothing to mark -- the
    // control bar already says what it is -- and a tag on bar 1 of every existing
    // set would be a change to a ruler this wave promised not to change.
    //
    // Over the bar number rather than beside it, because at the bar a signature
    // starts, "7/8" is the more useful of the two things that want that space.
    if (ui.fSmall && ctx.sig.count() > 1) {
        for (int i = 0; i < ctx.sig.count(); ++i) {
            const RtSig sg = ctx.sig.entry(i);
            const f32 mx = beatToX(ta, ctx.sig.beatOfBar((f64)sg.bar));
            if (mx < ruler.x - 40.f * s || mx > ruler.right()) continue;
            char buf[24];
            std::snprintf(buf, sizeof buf, "%d/%d", sg.num, sg.den);
            const f32 tw = ui.fSmall->measure(buf);
            const Rect tag{mx + 1.f * s, ruler.y + 3.f * s,
                           tw + 7.f * s, ruler.h - 6.f * s};
            rr.roundRect(tag, 2.f * s, pal::accent.alpha(0.85f));
            rr.textIn(*ui.fSmall, tag, buf, pal::textOnClip, Align::Center, 0.f);
            rr.rect({mx, ruler.y, 1.f * s, ruler.h}, pal::accentHi);
        }
    }
    // The brace. A disabled loop still remembers where it was (session.h), so
    // it is drawn faintly rather than not at all: a brace that vanished when it
    // was switched off would make the toggle look like it erased something.
    if (ctx.loopStart && ctx.loopEnd && *ctx.loopEnd > *ctx.loopStart) {
        const f32 x0 = beatToX(ta, *ctx.loopStart), x1 = beatToX(ta, *ctx.loopEnd);
        const bool on = ctx.loopOn && *ctx.loopOn;
        const Col c = on ? pal::accentHi : pal::textFaint;
        rr.rect({x0, ruler.y, std::max(1.f * s, x1 - x0), 3.f * s}, c.alpha(on ? 0.9f : 0.5f));
        rr.rect({x0, ruler.y, 1.5f * s, ruler.h - 1.f * s}, c);
        rr.rect({x1 - 1.5f * s, ruler.y, 1.5f * s, ruler.h - 1.f * s}, c);
    }
    {   // The playhead's head, in the ruler, where a hand looks for it.
        const f32 px = beatToX(ta, ctx.playhead);
        if (px >= ruler.x && px <= ruler.right())
            rr.rect({px - 1.f * s, ruler.y, 2.f * s, ruler.h}, pal::playGreen);
    }
    rr.popClip();

    if (ui.fSmall) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f px/beat", (double)zoom_);
        rr.textIn(*ui.fSmall, corner, buf, pal::textFaint, Align::Left, 6.f * s);
    }

    // --- the lanes ---------------------------------------------------------
    rr.pushClip(lanes);
    rr.rect(lanes, pal::appBg);
    drawTimeGrid(rr, ta, lanes, s, ctx.sig, kArrMinGridPx * s);
    // Past the loop end is still editable, so it is NOT dimmed the way the roll
    // dims past a clip's length: an arrangement has no end until something is
    // put there.

    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        const ArrangeContext::Lane& L = ctx.lanes[i];
        const Row& row = rows[i];
        const Rect band{lanes.x, topY + row.y, lanes.w, row.h};
        if (band.bottom() < lanes.y || band.y > lanes.bottom()) continue;

        rr.rect(band, (i % 2) ? pal::appBg.scale(1.10f) : pal::appBg);
        drawTimeGrid(rr, ta, band, s, ctx.sig, kArrMinGridPx * s);
        rr.rect({band.x, band.bottom() + row.autoH, band.w, 1.f * s}, pal::divider);

        if (!L.items) continue;
        const Col base = itemColour(L.colorIdx, L.overridden);
        for (const ArrangeClip& it : *L.items) {
            const f32 x0 = beatToX(ta, it.start);
            const f32 x1 = beatToX(ta, it.end());
            if (x1 < lanes.x - 2.f || x0 > lanes.right() + 2.f) continue;
            const Rect box{x0, band.y + 1.f * s, std::max(2.f * s, x1 - x0), band.h - 2.f * s};
            const bool sel = (int)i == selTrack_ && it.uid == selItem_ && it.uid != 0;

            rr.roundRect(box, 2.f * s, base.scale(0.34f));
            rr.rect({box.x, box.y, box.w, std::min(box.h, 12.f * s)}, base.scale(0.62f));

            rr.pushClip(box.intersect(lanes));
            // The material, drawn against the SHARED axis: a waveform for a
            // sample, note stems for a pattern. Both honour `offset`, so an item
            // that begins a bar into its clip shows the bar it begins on.
            const f32 mid = box.y + 12.f * s + (box.h - 12.f * s) * 0.5f;
            const f32 halfH = std::max(2.f * s, (box.h - 12.f * s) * 0.5f - 2.f * s);
            if (it.src.kind == ClipKind::Audio && it.src.sample &&
                it.src.sample->peakBuckets > 0) {
                const SampleBuffer& sb = *it.src.sample;
                const Col wc = base.scale(0.95f);
                const f32 xa = std::max(box.x, lanes.x), xb = std::min(box.right(), lanes.right());
                for (f32 x = xa; x < xb; x += 1.f) {
                    f64 u = 0.0;
                    if (!srcFraction(it.src, it.offset + (xToBeat(ta, x) - it.start), u)) continue;
                    const int bk = clampv((int)(u * (f64)sb.peakBuckets), 0, sb.peakBuckets - 1);
                    const f32 lo = sb.peaks[(size_t)bk * 2 + 0];
                    const f32 hi = sb.peaks[(size_t)bk * 2 + 1];
                    const f32 y0 = mid - hi * halfH;
                    rr.rect({x, y0, 1.f, std::max(1.f, (mid - lo * halfH) - y0)}, wc);
                }
            } else if (it.src.kind == ClipKind::Midi && !it.src.notes.empty()) {
                const f64 clen = it.src.lengthBeats > 1e-9 ? it.src.lengthBeats : 1.0;
                const int k0 = it.src.loop ? (int)std::floor(it.offset / clen) : 0;
                const int k1 = it.src.loop
                                   ? (int)std::floor((it.offset + it.length) / clen)
                                   : 0;
                const f32 top = box.y + 13.f * s;
                const f32 hgt = std::max(3.f * s, box.bottom() - top - 2.f * s);
                const Col nc = base.scale(1.0f);
                for (int k = k0; k <= k1 && k - k0 < kMaxPreviewReps; ++k) {
                    for (const NoteModel& n : it.src.notes) {
                        const f64 b = it.start + ((f64)k * clen + n.beat - it.offset);
                        if (b + n.len <= it.start || b >= it.end()) continue;
                        const f32 nx0 = std::max(beatToX(ta, std::max(b, it.start)), box.x);
                        const f32 nx1 = std::min(beatToX(ta, std::min(b + n.len, it.end())),
                                                 box.right());
                        if (nx1 <= nx0) continue;
                        const f32 t = clampv(((f32)n.pitch - (f32)kPreviewLoPitch) /
                                                 (f32)(kPreviewHiPitch - kPreviewLoPitch),
                                             0.f, 1.f);
                        const f32 y = top + (1.f - t) * (hgt - 2.f * s);
                        rr.rect({nx0, y, std::max(1.f, nx1 - nx0), 2.f * s}, nc);
                    }
                }
            }

            // The fades, as filled wedges at the corners. Drawn per column
            // rather than as a triangle so the wedge is clipped to the item at
            // both ends whatever the zoom.
            const Col shade = pal::appBg.alpha(0.78f);
            if (it.fadeIn > 0.0) {
                const f32 fx1 = beatToX(ta, std::min(it.start + it.fadeIn, it.end()));
                for (f32 x = std::max(box.x, lanes.x); x < std::min(fx1, lanes.right()); x += 1.f) {
                    const f32 t = clampv((x - box.x) / std::max(1.f, fx1 - box.x), 0.f, 1.f);
                    rr.rect({x, box.y, 1.f, box.h * (1.f - t)}, shade);
                }
                rr.line(box.x, box.y, fx1, box.bottom(), 1.f * s, pal::text.alpha(0.5f));
            }
            if (it.fadeOut > 0.0) {
                const f32 fx0 = beatToX(ta, std::max(it.end() - it.fadeOut, it.start));
                for (f32 x = std::max(fx0, lanes.x); x < std::min(box.right(), lanes.right()); x += 1.f) {
                    const f32 t = clampv((x - fx0) / std::max(1.f, box.right() - fx0), 0.f, 1.f);
                    rr.rect({x, box.y, 1.f, box.h * t}, shade);
                }
                rr.line(fx0, box.bottom(), box.right(), box.y, 1.f * s, pal::text.alpha(0.5f));
            }

            if (ui.fSmall && box.w > 24.f * s)
                rr.textIn(*ui.fSmall, {box.x, box.y, box.w, 12.f * s},
                          it.src.name.c_str(), pal::textOnClip, Align::Left, 4.f * s);
            rr.popClip();
            (void)sel;
        }
        // The selection outline in a SECOND pass, after every item on the lane.
        // A crossfade pair overlaps by design, so the later item's body is drawn
        // over the earlier one -- and an outline drawn inside the first pass
        // would be painted out by exactly the neighbour that makes the pair
        // interesting.
        if ((int)i != selTrack_) continue;
        for (const ArrangeClip& it : *L.items) {
            if (it.uid == 0 || it.uid != selItem_) continue;
            const f32 x0 = beatToX(ta, it.start), x1 = beatToX(ta, it.end());
            if (x1 < lanes.x - 2.f || x0 > lanes.right() + 2.f) continue;
            rr.roundRectOutline({x0, band.y + 1.f * s, std::max(2.f * s, x1 - x0),
                                 band.h - 2.f * s},
                                2.f * s, 1.5f * s, pal::accent);
        }
    }

    // The playhead, over everything the lanes hold.
    {
        const f32 px = beatToX(ta, ctx.playhead);
        if (px >= lanes.x && px <= lanes.right())
            rr.rect({px, lanes.y, 1.5f * s, lanes.h},
                    ctx.playing ? pal::playGreen : pal::playGreen.alpha(0.45f));
    }
    rr.popClip();

    // --- the automation lanes ----------------------------------------------
    // Drawn after the clip lanes and outside their clip, because each is its own
    // AutoLaneView with its own hot rect. Every one is handed the SAME TimeAxis
    // the items above it are drawn against, which is the property §7.3 exists
    // to guarantee.
    laneViews_.resize(ctx.lanes.size());
    targetSel_.resize(ctx.lanes.size(), 0);
    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        const ArrangeContext::Lane& L = ctx.lanes[i];
        const Row& row = rows[i];
        if (row.autos <= 0 || !L.autos) continue;
        std::vector<AutoLaneView>& views = laneViews_[i];
        if (views.size() < L.autos->size()) views.resize(L.autos->size());

        for (size_t j = 0; j < L.autos->size(); ++j) {
            const Rect lr{lanes.x, topY + row.autoY + (f32)j * kArrAutoLaneH * s,
                          lanes.w, kArrAutoLaneH * s};
            if (lr.bottom() < lanes.y || lr.y > lanes.bottom()) continue;
            AutoLane& al = (*L.autos)[j];
            const AutoTargets::Entry* tgt = L.targets ? L.targets->find(al.address) : nullptr;

            rr.rect(lr, pal::panel.scale(0.72f));
            rr.pushClip(lr.intersect(lanes));
            drawTimeGrid(rr, ta, lr, s, ctx.sig, kArrMinGridPx * s);
            AutoLaneView& v = views[j];
            v.setId(uiId(25, (int)i, (int)j));
            v.prune((int)al.points.size());
            // beatBase 0: an arrangement lane's points are ALREADY absolute
            // timeline beats (session.h, TrackModel::arrangeAutos), which is the
            // one thing that differs from a clip envelope.
            if (v.draw(ui, lr, al.points, ta, tgt ? tgt->lo : 0.f, tgt ? tgt->hi : 1.f,
                       tgt ? tgt->unit.c_str() : nullptr, tgt ? tgt->def : 0.f,
                       al.enabled, false, contentBeats, 0.0, tgt != nullptr,
                       nullptr)) {
                changed |= Changed::Autos;
                ctx.dirty.push_back((int)i);
                lastEdit_ = kEditAuto;
            }
            const f32 px = beatToX(ta, ctx.playhead);
            if (px >= lr.x && px <= lr.right())
                rr.rect({px, lr.y, 1.5f * s, lr.h},
                        ctx.playing ? pal::playGreen : pal::playGreen.alpha(0.45f));
            rr.popClip();
            rr.rect({lr.x, lr.y, lr.w, 1.f * s}, pal::divider.alpha(0.7f));
        }
    }

    // --- the header column -------------------------------------------------
    // Scrolls vertically with the lanes and never horizontally, the same
    // relationship drawTrackHeaders has with the clip grid.
    rr.pushClip(heads);
    rr.rect(heads, pal::panel);
    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        ArrangeContext::Lane& L = ctx.lanes[i];
        const Row& row = rows[i];
        const Rect hb{heads.x, topY + row.y, heads.w, row.h};
        if (hb.bottom() < heads.y || hb.y > heads.bottom()) continue;

        rr.rect(hb, (i % 2) ? pal::panel.scale(1.08f) : pal::panel);
        rr.rect({hb.x, hb.y, 3.f * s, hb.h}, itemColour(L.colorIdx, false));

        // The disclosure triangle. A filled triangle rather than a glyph: the
        // atlas has no arrows, and a rotated triangle is the same control
        // everywhere it appears.
        const Rect tri{hb.x + 6.f * s, hb.y + 3.f * s, 12.f * s, 12.f * s};
        const bool open = L.expanded && *L.expanded;
        const u64 triId = uiId(24, 1, (int)i);
        const bool hotTri = ui.setHot(triId, tri) && ui.isHot(triId);
        const Col tc = hotTri ? pal::text : pal::textDim;
        if (open) rr.triangle(tri.x + 1.f * s, tri.cy() - 2.5f * s,
                              tri.x + 11.f * s, tri.cy() - 2.5f * s,
                              tri.cx(), tri.cy() + 4.f * s, tc);
        else      rr.triangle(tri.x + 2.5f * s, tri.cy() - 5.f * s,
                              tri.x + 2.5f * s, tri.cy() + 5.f * s,
                              tri.x + 9.5f * s, tri.cy(), tc);
        if (hotTri) {
            ui.cursor = Cursor::Hand;
            ui.tip = open ? "hide this track's automation lanes"
                          : "show this track's automation lanes";
            if (in.pressed[0] && L.expanded) {
                *L.expanded = !open;
                changed |= Changed::Layout;
            }
        }

        if (ui.fBody)
            rr.textIn(*ui.fBody, {tri.right() + 4.f * s, hb.y, hb.w - 46.f * s, 16.f * s},
                      L.name.c_str(), pal::text, Align::Left, 0);
        if (L.armed)
            rr.circle(hb.right() - 10.f * s, hb.y + 9.f * s, 3.5f * s, pal::armRed);

        // The override tint, and the way out of it. An overridden track is
        // playing a session clip instead of its lane; the chip both says so and
        // is the Back to Arrangement gesture for that track.
        if (L.overridden) {
            const Rect ov{hb.x + 6.f * s, hb.y + 18.f * s, 58.f * s, 11.f * s};
            const u64 ovId = uiId(24, 2, (int)i);
            const bool hotOv = ui.setHot(ovId, ov) && ui.isHot(ovId);
            rr.roundRect(ov, 2.f * s, pal::meterAmber.alpha(hotOv ? 0.34f : 0.18f));
            if (ui.fSmall)
                rr.textIn(*ui.fSmall, ov, "SESSION", pal::meterAmber, Align::Center, 0);
            if (hotOv) {
                ui.cursor = Cursor::Hand;
                ui.tip = "this track is playing the session - click to give the "
                         "arrangement its lane back";
                if (in.pressed[0]) ctx.backToArrTrack = (int)i;
            }
        }

        // The lane's bottom edge resizes it, which is the only place `arrHeight`
        // can be set: a per-track height is what stops one global one from being
        // either wasted space or an unreadable lane (§7.4).
        const Rect grip{hb.x, hb.bottom() + row.autoH - kArrLaneGrab * s,
                        hb.w, kArrLaneGrab * 2.f * s};
        const u64 gripId = uiId(24, 3, (int)i);
        const bool hotGrip = ui.setHot(gripId, grip) && ui.isHot(gripId);
        if (hotGrip && drag_ == Drag::None) {
            ui.cursor = Cursor::ResizeV;
            if (in.pressed[0] && L.height) {
                drag_ = Drag::LaneH;
                gesture_ = gripId;
                dragTrack_ = (int)i;
                grabY_ = in.my;
                origHeight_ = *L.height;
                moved_ = false;
                ui.active = gripId;
            }
        }

        // The automation lanes' own header rows: what each names, its on/off,
        // and one chooser row that adds the next one.
        if (row.autos > 0 && L.autos) {
            for (size_t j = 0; j < L.autos->size(); ++j) {
                AutoLane& al = (*L.autos)[j];
                const Rect ab{heads.x, topY + row.autoY + (f32)j * kArrAutoLaneH * s,
                              heads.w, kArrAutoLaneH * s};
                if (ab.bottom() < heads.y || ab.y > heads.bottom()) continue;
                rr.rect(ab, pal::panel.scale(0.86f));
                rr.rect({ab.x, ab.y, ab.w, 1.f * s}, pal::divider.alpha(0.7f));
                const AutoTargets::Entry* tgt = L.targets ? L.targets->find(al.address) : nullptr;
                if (ui.fSmall) {
                    // The target's short label where the address resolves and the
                    // address itself where it does not: a lane naming a missing
                    // device must still be findable.
                    const std::string& lbl = tgt ? tgt->label : al.address;
                    rr.textIn(*ui.fSmall, {ab.x + 6.f * s, ab.y + 4.f * s, ab.w - 30.f * s, 11.f * s},
                              lbl.c_str(), tgt ? pal::textDim : pal::textFaint, Align::Left, 0);
                }
                const Rect onR{ab.right() - 20.f * s, ab.y + 4.f * s, 14.f * s, 12.f * s};
                const u64 onId = uiId(26, (int)i, (int)j);
                bool on = al.enabled;
                if (ui.squareToggle(onId, onR, "", &on, pal::accent)) {
                    al.enabled = on;
                    changed |= Changed::Autos;
                    ctx.dirty.push_back((int)i);
                    lastEdit_ = kEditAuto;
                }
                rr.circle(onR.cx(), onR.cy(), 3.f * s,
                          al.enabled ? pal::textOnClip : pal::textFaint);
                if (ui.hovered(onR)) ui.tip = al.address;
                // Removing a lane is the same right-click that removes anything
                // else in this program.
                if (ui.hovered(ab) && in.pressed[2]) {
                    L.autos->erase(L.autos->begin() + (long)j);
                    if (j < laneViews_[i].size())
                        laneViews_[i].erase(laneViews_[i].begin() + (long)j);
                    changed |= Changed::Autos;
                    ctx.dirty.push_back((int)i);
                    lastEdit_ = kEditAuto;
                    break;
                }
            }
            // The chooser row. `selector` and not a dropdown, for the reason the
            // roll's lane key gives: this codebase has no dropdown, and click
            // cycles / right-click cycles back / wheel scrubs is the idiom
            // everywhere else in it.
            const Rect cb{heads.x, topY + row.autoY + (f32)L.autos->size() * kArrAutoLaneH * s,
                          heads.w, kArrAutoLaneH * s};
            if (cb.bottom() >= heads.y && cb.y <= heads.bottom()) {
                rr.rect(cb, pal::panel.scale(0.78f));
                rr.rect({cb.x, cb.y, cb.w, 1.f * s}, pal::divider.alpha(0.7f));
                if (L.targets && !L.targets->entries.empty()) {
                    std::vector<const char*> names;
                    names.reserve(L.targets->entries.size());
                    for (const AutoTargets::Entry& e : L.targets->entries)
                        names.push_back(e.label.c_str());
                    int& tsel = targetSel_[i];
                    tsel = clampv(tsel, 0, (int)names.size() - 1);
                    const Rect selR{cb.x + 6.f * s, cb.y + 5.f * s, cb.w - 34.f * s, 14.f * s};
                    ui.selector(uiId(24, 5, (int)i), selR, &tsel, names.data(), (int)names.size());
                    if (ui.hovered(selR))
                        ui.tip = L.targets->entries[(size_t)tsel].group + " " +
                                 L.targets->entries[(size_t)tsel].label + "  " +
                                 L.targets->entries[(size_t)tsel].address;
                    const Rect addR{selR.right() + 4.f * s, selR.y, 20.f * s, selR.h};
                    if (ui.button(uiId(24, 6, (int)i), addR, "+") &&
                        (int)L.autos->size() < kMaxArrLanes) {
                        const std::string& addr = L.targets->entries[(size_t)tsel].address;
                        int found = -1;
                        for (size_t k = 0; k < L.autos->size(); ++k)
                            if ((*L.autos)[k].address == addr) { found = (int)k; break; }
                        if (found < 0) {
                            AutoLane nl;
                            nl.address = addr;
                            L.autos->push_back(std::move(nl));
                            changed |= Changed::Autos | Changed::Layout;
                            ctx.dirty.push_back((int)i);
                            lastEdit_ = kEditAuto;
                        }
                    }
                } else if (ui.fSmall) {
                    rr.textIn(*ui.fSmall, cb, "no targets", pal::textFaint, Align::Center, 0);
                }
            }
        }
    }
    rr.rect({heads.right() - 1.f * s, heads.y, 1.f * s, heads.h}, pal::divider);
    rr.popClip();

    // --- item interaction --------------------------------------------------
    // What is under the cursor, and where inside it. One answer, used by the
    // press, the cursor shape and the fade grabs alike, so they cannot disagree.
    int hitTrack = -1, hitIdx = -1;
    enum class Zone { Body, Left, Right, FadeIn, FadeOut } hitZone = Zone::Body;
    if (hotLanes && drag_ == Drag::None) {
        const int t = trackAtY(in.my);
        if (t >= 0 && t < (int)ctx.lanes.size() && ctx.lanes[(size_t)t].items &&
            in.my < topY + rows[(size_t)t].y + rows[(size_t)t].h) {
            const std::vector<ArrangeClip>& v = *ctx.lanes[(size_t)t].items;
            for (size_t k = v.size(); k-- > 0;) {
                const f32 x0 = beatToX(ta, v[k].start), x1 = beatToX(ta, v[k].end());
                if (in.mx < x0 || in.mx >= x1) continue;
                hitTrack = t;
                hitIdx = (int)k;
                const f32 e = std::min(kArrEdgeGrab * s, (x1 - x0) * 0.3f);
                const f32 topBand = topY + rows[(size_t)t].y + 13.f * s;
                if (in.my < topBand && in.mx < x0 + 14.f * s)        hitZone = Zone::FadeIn;
                else if (in.my < topBand && in.mx > x1 - 14.f * s)   hitZone = Zone::FadeOut;
                else if (in.mx < x0 + e)                             hitZone = Zone::Left;
                else if (in.mx > x1 - e)                             hitZone = Zone::Right;
                else                                                 hitZone = Zone::Body;
                break;
            }
        }
    }

    if (drag_ == Drag::None && hotLanes && (in.pressed[0] || in.pressed[2])) {
        if (hitIdx >= 0) {
            std::vector<ArrangeClip>& v = *ctx.lanes[(size_t)hitTrack].items;
            const ArrangeClip& it = v[(size_t)hitIdx];
            if (selTrack_ != hitTrack || selItem_ != it.uid) {
                selTrack_ = hitTrack;
                selItem_  = it.uid;
                changed |= Changed::Selection;
            }
            if (in.pressed[2]) {
                ctx.wantDelete = true;      // right-click deletes, as in the roll
            } else if (in.dblClick) {
                cursorBeat_ = xToBeat(ta, in.mx);
                ctx.wantSplit = true;       // double-click splits under the cursor
            } else {
                drag_ = hitZone == Zone::Left    ? Drag::TrimL
                        : hitZone == Zone::Right ? Drag::TrimR
                        : hitZone == Zone::FadeIn  ? Drag::FadeIn
                        : hitZone == Zone::FadeOut ? Drag::FadeOut
                                                   : Drag::Move;
                gesture_ = uiId(24, 8, hitTrack);
                dragTrack_ = hitTrack;
                dragUid_ = it.uid;
                grabBeat_ = xToBeat(ta, in.mx) - it.start;
                grabY_ = in.my;
                origStart_ = it.start;
                origOffset_ = it.offset;
                origLength_ = it.length;
                origFadeIn_ = it.fadeIn;
                origFadeOut_ = it.fadeOut;
                moved_ = false;
                dupMade_ = false;
                ui.active = gesture_;
            }
        } else if (in.pressed[0]) {
            if (selItem_ != 0) changed |= Changed::Selection;
            selTrack_ = -1;
            selItem_ = 0;
        }
    }

    if (drag_ == Drag::LaneH) {
        if (!in.down[0]) {
            drag_ = Drag::None;
            gesture_ = 0;
            if (dragTrack_ >= 0) changed |= Changed::Layout;
        } else if (dragTrack_ >= 0 && dragTrack_ < (int)ctx.lanes.size() &&
                   ctx.lanes[(size_t)dragTrack_].height) {
            if (!moved_ && std::fabs(in.my - grabY_) > 2.f * s) {
                moved_ = true;
                pendingEdit_ = kEditLayout;
            } else if (moved_) {
                *ctx.lanes[(size_t)dragTrack_].height =
                    clampv(origHeight_ + (in.my - grabY_) / s, kArrMinLaneH, kArrMaxLaneH);
            }
        }
    } else if (drag_ != Drag::None && drag_ != Drag::Loop) {
        // Every item drag is measured from the values the item had at the press
        // and written absolutely, so it never integrates its own error and a
        // wheel mid-drag cannot walk the item away from the hand.
        if (!in.down[0]) {
            // COMMIT. This is the one place a lane is repaired and the only
            // place a drag reports Items, which is what keeps the engine from
            // ever seeing an unrepaired lane and what stops a sweep from eating
            // a neighbour one frame at a time.
            if (moved_ && dragTrack_ >= 0 && dragTrack_ < (int)ctx.lanes.size() &&
                ctx.lanes[(size_t)dragTrack_].items) {
                arrangeRepair(*ctx.lanes[(size_t)dragTrack_].items);
                ctx.dirty.push_back(dragTrack_);
                changed |= Changed::Items;
            }
            if (ui.active == gesture_) ui.active = 0;
            drag_ = Drag::None;
            gesture_ = 0;
            dragUid_ = 0;
        } else {
            int t = dragTrack_;
            int idx = (t >= 0 && t < (int)ctx.lanes.size() && ctx.lanes[(size_t)t].items)
                          ? indexOf(*ctx.lanes[(size_t)t].items, dragUid_)
                          : -1;
            if (idx < 0) {
                drag_ = Drag::None;
                gesture_ = 0;
            } else if (!moved_) {
                const bool far = std::fabs(in.mx - beatToX(ta, origStart_ + grabBeat_)) > 2.f * s ||
                                 (drag_ == Drag::Move && std::fabs(in.my - grabY_) > 4.f * s);
                if (far) {
                    moved_ = true;
                    pendingEdit_ = drag_ == Drag::Move    ? kEditMove
                                   : drag_ == Drag::FadeIn || drag_ == Drag::FadeOut ? kEditFade
                                                                                     : kEditTrim;
                }
            } else {
                std::vector<ArrangeClip>* items = ctx.lanes[(size_t)t].items;
                // Ctrl+drag duplicates: the copy is made once, on the first
                // frame that actually mutates, so it is taken with the values
                // the item had at the press. It is the COPY that stays behind at
                // the original position and the original that travels, which is
                // what Ctrl+drag means everywhere. The copy has no uid yet; the
                // caller's assignUids stamps it on commit.
                if (drag_ == Drag::Move && in.ctrl() && !dupMade_ &&
                    (int)items->size() < kMaxArrItems) {
                    ArrangeClip copy = (*items)[(size_t)idx];
                    copy.uid = newUid(ctx);
                    items->insert(items->begin() + (long)idx, std::move(copy));
                    // The inserted copy takes index `idx`; the item under the
                    // hand is now one later and still carries dragUid_.
                    dupMade_ = true;
                    idx = indexOf(*items, dragUid_);
                    if (idx < 0) { drag_ = Drag::None; gesture_ = 0; }
                    lastEdit_ = kEditDup;
                }
                if (idx >= 0) {
                    ArrangeClip& it = (*items)[(size_t)idx];
                    const f64 raw = xToBeat(ta, in.mx);
                    // Shift is unquantized, exactly as it is for a note.
                    const auto snap = [&](f64 b) { return in.shift() ? b : quantNear(b); };
                    switch (drag_) {
                    case Drag::Move: {
                        it.start = std::max(0.0, snap(raw - grabBeat_));
                        // Across tracks: the item is moved bodily into the other
                        // lane, because an ArrangeClip owns its clip (§2.2) and
                        // there is nothing else to carry.
                        const int nt = trackAtY(in.my);
                        if (nt >= 0 && nt != t && nt < (int)ctx.lanes.size() &&
                            ctx.lanes[(size_t)nt].items &&
                            (int)ctx.lanes[(size_t)nt].items->size() < kMaxArrItems) {
                            ArrangeClip moved = std::move((*items)[(size_t)idx]);
                            items->erase(items->begin() + (long)idx);
                            arrangeRepair(*items);
                            ctx.dirty.push_back(t);
                            ctx.lanes[(size_t)nt].items->push_back(std::move(moved));
                            dragTrack_ = nt;
                            selTrack_ = nt;
                            changed |= Changed::Selection | Changed::Items;
                        }
                        break;
                    }
                    case Drag::TrimL: {
                        // The head trim: `start` and `offset` move TOGETHER, so
                        // the audio under the rest of the item does not slide.
                        // Getting this wrong is the classic arrangement-editor
                        // bug, which is why it is one expression and not two.
                        const f64 want = snap(raw - grabBeat_);
                        const f64 lo = std::max(0.0, origStart_ - origOffset_);
                        const f64 hi = origStart_ + origLength_ - kMinArrBeats;
                        const f64 ns = clampv(want, lo, hi);
                        const f64 d = ns - origStart_;
                        it.start  = ns;
                        it.offset = origOffset_ + d;
                        it.length = origLength_ - d;
                        if (it.fadeIn > it.length) it.fadeIn = it.length;
                        if (it.fadeOut > it.length) it.fadeOut = it.length;
                        break;
                    }
                    case Drag::TrimR: {
                        const f64 want = snap(raw);
                        it.length = std::max(kMinArrBeats, want - it.start);
                        if (it.fadeOut > it.length) it.fadeOut = it.length;
                        if (it.fadeIn > it.length) it.fadeIn = it.length;
                        break;
                    }
                    case Drag::FadeIn:
                        it.fadeIn = clampv(raw - it.start, 0.0,
                                           std::max(0.0, it.length - it.fadeOut));
                        break;
                    case Drag::FadeOut:
                        it.fadeOut = clampv(it.end() - raw, 0.0,
                                            std::max(0.0, it.length - it.fadeIn));
                        break;
                    default: break;
                    }
                    // dragUid_ and not `it.uid`: a cross-track move has just
                    // moved that object into another vector and the reference is
                    // dead. The uid is the handle precisely so that it survives.
                    selItem_ = dragUid_;
                }
            }
        }
    }

    // --- the drop target ---------------------------------------------------
    if (ctx.dropActive && hotLanes) {
        const int t = trackAtY(in.my);
        if (t >= 0) {
            ctx.dropTrack = t;
            ctx.dropBeat = std::max(0.0, quantNear(xToBeat(ta, in.mx)));
            const f32 x = beatToX(ta, ctx.dropBeat);
            rr.pushClip(lanes);
            rr.rect({x, topY + rows[(size_t)t].y, 2.f * s, rows[(size_t)t].h}, pal::accentHi);
            rr.popClip();
            if (in.released[0]) ctx.dropped = true;
        }
    }

    // --- the cursor --------------------------------------------------------
    if (drag_ == Drag::TrimL || drag_ == Drag::TrimR)      ui.cursor = Cursor::ResizeH;
    else if (drag_ == Drag::Move)                          ui.cursor = Cursor::Grab;
    else if (drag_ == Drag::LaneH)                         ui.cursor = Cursor::ResizeV;
    else if (hotRuler && drag_ == Drag::None)              ui.cursor = Cursor::Hand;
    else if (hitIdx >= 0) {
        ui.cursor = (hitZone == Zone::Left || hitZone == Zone::Right) ? Cursor::ResizeH
                    : (hitZone == Zone::FadeIn || hitZone == Zone::FadeOut) ? Cursor::ResizeH
                                                                            : Cursor::Grab;
    }

    ctx.selTrack = selTrack_;
    ctx.selItem  = selItem_;
    return changed;
}

} // namespace lat
