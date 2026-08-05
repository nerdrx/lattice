// Immediate-mode widget implementations. Every widget follows the same
// hot/active protocol: claim hot each frame from its bounds, take `active` on
// a press that lands inside, consume Input::dx/dy only while it owns `active`,
// and let Ui::endFrame() release ownership on mouse-up.
#include "widgets.h"
#include <cstdio>
#include <cmath>

namespace lat {

// Text that sits ON a colored fill: dark ink on bright fills (the pastel clip
// colors), light ink on dark fills (the purple accent). Rec.601 luma.
static Col inkOn(const Col& fill) {
    const f32 luma = 0.299f * fill.r + 0.587f * fill.g + 0.114f * fill.b;
    return luma > 0.45f ? pal::textOnClip : Col(0.94f, 0.92f, 1.f, 1.f);
}

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDeg = kPi / 180.f;

// Knob sweep, measured from 12 o'clock: -135deg .. +135deg. The `arc` helper
// works in screen angles where 0 points right and the angle grows clockwise,
// so straight up is -90deg and the sweep becomes -225deg .. +45deg.
constexpr f32 kKnobA0 = -225.f * kDeg;
constexpr f32 kKnobA1 = 45.f * kDeg;
constexpr f32 kKnobTop = -90.f * kDeg;

// Pixels of vertical travel that cover a knob's full range.
constexpr f32 kKnobTravel = 150.f;

inline f32 fineScale(const Input* in) { return in->shift() ? 0.25f : 1.f; }

inline f32 norm01(f32 v, f32 lo, f32 hi) {
    return (hi - lo) > 1e-9f ? clampv((v - lo) / (hi - lo), 0.f, 1.f) : 0.f;
}

} // namespace

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void Ui::arc(f32 cx, f32 cy, f32 rad, f32 a0, f32 a1, f32 th, const Col& c) {
    if (!r || rad <= 0.f) return;
    const f32 span = a1 - a0;
    if (std::fabs(span) < 1e-4f) return;
    // ~2.5 degrees per segment, so the polyline reads as a curve at any size.
    int segs = (int)std::ceil(std::fabs(span) / (2.5f * kDeg));
    segs = clampv(segs, 2, 256);
    const f32 step = span / (f32)segs;
    f32 px = cx + std::cos(a0) * rad;
    f32 py = cy + std::sin(a0) * rad;
    for (int i = 1; i <= segs; ++i) {
        const f32 a = a0 + step * (f32)i;
        const f32 nx = cx + std::cos(a) * rad;
        const f32 ny = cy + std::sin(a) * rad;
        r->line(px, py, nx, ny, th, c);
        px = nx; py = ny;
    }
}

void Ui::bevel(const Rect& b, f32 radius, const Col& fill, f32 lightness) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    r->roundRect(b, radius, fill);
    // A single bright pixel row across the top edge; enough to read as raised
    // without the cost (or the banding) of a full gradient.
    const f32 inset = std::min(radius, b.w * 0.5f);
    const f32 hw = b.w - inset * 2.f;
    if (hw > 0.f) r->rect({b.x + inset, b.y, hw, 1.f}, fill.scale(1.f + lightness));
}

void Ui::playTriangle(const Rect& b, const Col& c) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    r->triangle(b.x, b.y, b.x, b.bottom(), b.right(), b.cy(), c);
}

void Ui::stopSquare(const Rect& b, const Col& c) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    const f32 s = std::min(b.w, b.h) * 0.6f;
    r->rect({std::round(b.cx() - s * 0.5f), std::round(b.cy() - s * 0.5f), s, s}, c);
}

void Ui::meterV(const Rect& b, f32 lvl, f32 peak) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    r->rect(b, pal::appBg);

    auto mapped = [](f32 g) {
        return clampv((gainToDb(clampv(g, 0.f, 8.f)) + 60.f) / 66.f, 0.f, 1.f);
    };

    const f32 n = mapped(lvl);
    if (n > 0.f) {
        // Three bands stacked bottom-up: green, amber, red. Each segment is
        // clipped to how far the level actually reached.
        const f32 bands[3] = {0.75f, 0.92f, 1.f};
        const Col cols[3] = {pal::meterGreen, pal::meterAmber, pal::meterRed};
        f32 from = 0.f;
        for (int i = 0; i < 3; ++i) {
            const f32 to = std::min(n, bands[i]);
            if (to > from) {
                const f32 y0 = b.bottom() - to * b.h;
                const f32 y1 = b.bottom() - from * b.h;
                r->rect({b.x, y0, b.w, y1 - y0}, cols[i]);
            }
            from = bands[i];
            if (n <= bands[i]) break;
        }
    }

    const f32 p = mapped(peak);
    if (p > 0.f) {
        const f32 y = clampv(b.bottom() - p * b.h, b.y, b.bottom() - 1.f);
        const Col pc = p > 0.92f ? pal::meterRed : (p > 0.75f ? pal::meterAmber : pal::text);
        r->rect({b.x, std::round(y), b.w, 1.f}, pc);
    }
}

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------

bool Ui::button(u64 id, const Rect& b, const char* label, bool on, Col onCol, f32 radius) {
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool clicked = false;

    if (in->pressed[0] && hotNow) active = id;
    if (in->released[0] && active == id) {
        if (over) clicked = true;
        active = 0;
    }

    const bool held = (active == id) && over;

    Col fill = pal::panelAlt;
    Col fg = pal::text;
    if (on) {
        fill = held ? onCol.scale(0.85f) : onCol;
        fg = inkOn(fill);
    } else if (held) {
        fill = pal::panelAlt.scale(0.7f);
    } else if (hotNow) {
        fill = pal::slotHover;
    }

    bevel(b, radius, fill, held ? 0.f : 0.06f);

    if (label && *label && fBody) r->textIn(*fBody, b, label, fg, Align::Center, 3.f);
    if (hotNow) cursor = Cursor::Hand;
    return clicked;
}

// ---------------------------------------------------------------------------
// Square toggle (M / S / arm)
// ---------------------------------------------------------------------------

bool Ui::squareToggle(u64 id, const Rect& b, const char* label, bool* value, Col onCol) {
    if (!value) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    if (in->pressed[0] && hotNow) active = id;
    if (in->released[0] && active == id) {
        if (over) { *value = !*value; changed = true; }
        active = 0;
    }

    const bool held = (active == id) && over;

    Col fill = pal::panelAlt;
    Col fg = pal::textDim;
    if (*value) {
        fill = held ? onCol.scale(0.85f) : onCol;
        fg = inkOn(fill);
    } else if (held) {
        fill = pal::panelAlt.scale(0.7f);
    } else if (hotNow) {
        fill = pal::slotHover;
        fg = pal::text;
    }

    r->roundRect(b, 2.f, fill);
    if (!*value) r->roundRectOutline(b, 2.f, 1.f, pal::divider);

    if (label && *label) {
        Font* f = fSmall ? fSmall : fBody;
        if (f) r->textIn(*f, b, label, fg, Align::Center, 1.f);
    }
    if (hotNow) cursor = Cursor::Hand;
    return changed;
}

// ---------------------------------------------------------------------------
// Knob
// ---------------------------------------------------------------------------

bool Ui::knob(u64 id, const Rect& b, f32* v, f32 lo, f32 hi, f32 def, const char* fmt) {
    if (!v || !r) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    if (in->pressed[0] && hotNow) {
        active = id;
        dragAccum = 0.f;
        dragStart = (f64)*v;
    }
    if (in->dblClick && over) {
        *v = clampv(def, lo, hi);
        dragStart = (f64)*v;
        dragAccum = 0.f;
        changed = true;
    }
    if (active == id && in->dy != 0.f) {
        // Up is more. Accumulate in pixels so a fine-drag modifier can be
        // toggled mid-gesture without the value jumping.
        dragAccum += -in->dy * fineScale(in);
        const f32 nv = (f32)dragStart + (dragAccum / kKnobTravel) * (hi - lo);
        const f32 cl = clampv(nv, lo, hi);
        if (cl != *v) { *v = cl; changed = true; }
    }
    if (in->released[0] && active == id) active = 0;

    // --- layout ---
    Font* vf = fSmall ? fSmall : fBody;
    const f32 textH = (fmt && vf) ? vf->height() : 0.f;
    const f32 avail = std::min(b.w, b.h - textH);
    const f32 rad = avail * 0.5f - 1.f;
    if (rad <= 1.f) return changed;
    const f32 cx = b.cx();
    const f32 cy = b.y + 1.f + rad;

    const f32 t = norm01(*v, lo, hi);
    const f32 ang = kKnobA0 + (kKnobA1 - kKnobA0) * t;

    // Body.
    r->circle(cx, cy, rad, pal::panelAlt);
    r->circle(cx, cy, rad - 1.f, pal::panelAlt.scale(0.78f));

    // Track + value arc, drawn just outside the body.
    const f32 aRad = rad - 1.5f;
    const f32 aTh = std::max(1.5f, rad * 0.18f);
    arc(cx, cy, aRad, kKnobA0, kKnobA1, aTh, pal::divider);

    const bool bipolar = (lo < 0.f && hi > 0.f);
    const Col arcCol = (hotNow || active == id) ? pal::accent : pal::accent.scale(0.85f);
    if (bipolar) {
        // Grow out of 12 o'clock in whichever direction the value sits.
        const f32 centre = kKnobA0 + (kKnobA1 - kKnobA0) * norm01(0.f, lo, hi);
        if (std::fabs(ang - centre) > 1e-3f) arc(cx, cy, aRad, centre, ang, aTh, arcCol);
        r->line(cx + std::cos(kKnobTop) * (aRad - aTh * 0.5f),
                cy + std::sin(kKnobTop) * (aRad - aTh * 0.5f),
                cx + std::cos(kKnobTop) * (aRad + aTh * 0.5f),
                cy + std::sin(kKnobTop) * (aRad + aTh * 0.5f), 1.f, pal::ridge);
    } else if (t > 0.001f) {
        arc(cx, cy, aRad, kKnobA0, ang, aTh, arcCol);
    }

    // Indicator from the middle outward.
    const f32 i0 = rad * 0.22f, i1 = rad - aTh - 1.f;
    if (i1 > i0) {
        r->line(cx + std::cos(ang) * i0, cy + std::sin(ang) * i0,
                cx + std::cos(ang) * i1, cy + std::sin(ang) * i1, 1.5f, pal::text);
    }

    if (fmt && vf) {
        char buf[64];
        std::snprintf(buf, sizeof buf, fmt, (double)*v);
        const Rect tr{b.x, b.bottom() - textH, b.w, textH};
        r->textIn(*vf, tr, buf, (hotNow || active == id) ? pal::text : pal::textDim,
                  Align::Center, 0.f);
    }

    if (hotNow || active == id) cursor = Cursor::ResizeV;
    return changed;
}

// ---------------------------------------------------------------------------
// Vertical fader
// ---------------------------------------------------------------------------

bool Ui::vFader(u64 id, const Rect& b, f32* t) {
    if (!t || !r) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    const f32 handleH = 11.f;
    const f32 travel = std::max(1.f, b.h - handleH);
    // t = 1 at the top of the travel.
    auto handleY = [&](f32 tv) { return b.y + (1.f - clampv(tv, 0.f, 1.f)) * travel; };

    if (in->pressed[0] && hotNow) {
        active = id;
        dragAccum = 0.f;
        const Rect h{b.x, handleY(*t), b.w, handleH};
        if (!h.contains(in->mx, in->my)) {
            // Clicking the track jumps the handle under the cursor.
            const f32 nv = clampv(1.f - (in->my - b.y - handleH * 0.5f) / travel, 0.f, 1.f);
            if (nv != *t) { *t = nv; changed = true; }
        }
        dragStart = (f64)*t;
    }
    if (in->dblClick && over) {
        *t = 0.85f;                       // unity
        dragStart = (f64)*t;
        dragAccum = 0.f;
        changed = true;
    }
    if (active == id && in->dy != 0.f) {
        dragAccum += -in->dy * fineScale(in);
        const f32 nv = clampv((f32)dragStart + dragAccum / travel, 0.f, 1.f);
        if (nv != *t) { *t = nv; changed = true; }
    }
    if (in->released[0] && active == id) active = 0;

    // --- draw ---
    const f32 trackW = std::min(4.f, std::max(2.f, b.w * 0.22f));
    const Rect track{std::round(b.cx() - trackW * 0.5f), b.y + handleH * 0.5f,
                     trackW, b.h - handleH};
    r->rect(track, pal::appBg);
    r->rect({track.x, track.y, 1.f, track.h}, pal::divider);

    // Unity tick.
    const f32 unityY = std::round(handleY(0.85f) + handleH * 0.5f);
    r->rect({b.x, unityY, b.w, 1.f}, pal::ridge.alpha(0.5f));

    const Rect handle{b.x, std::round(handleY(*t)), b.w, handleH};
    Col hc = pal::ridge;
    if (active == id) hc = pal::ridge.scale(1.25f);
    else if (hotNow) hc = pal::ridge.scale(1.12f);
    bevel(handle, 2.f, hc, 0.18f);
    r->rect({handle.x + 1.f, std::round(handle.cy()), handle.w - 2.f, 1.f},
            pal::appBg.alpha(0.75f));

    if (hotNow || active == id) cursor = Cursor::ResizeV;
    return changed;
}

// ---------------------------------------------------------------------------
// Draggable number
// ---------------------------------------------------------------------------

bool Ui::dragNumber(u64 id, const Rect& b, f64* v, f64 lo, f64 hi, f64 perPixel,
                    const char* fmt, Align align, const char* zeroLabel, f64 step) {
    if (!v || !r) return false;
    setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    if (in->pressed[0] && hotNow) {
        active = id;
        dragAccum = 0.f;
        dragStart = *v;
    }
    if (active == id && in->dy != 0.f) {
        dragAccum += -in->dy * (in->shift() ? 0.1f : 1.f);   // drag up = increase
        f64 nv = dragStart + (f64)dragAccum * perPixel;
        // Snap before clamping, so the endpoints of the range stay reachable
        // even when they are not multiples of the step.
        if (step > 0.0) nv = std::floor(nv / step + 0.5) * step;
        nv = clampv(nv, lo, hi);
        if (nv != *v) { *v = nv; changed = true; }
    }
    if (in->released[0] && active == id) active = 0;

    if (hotNow || active == id) r->rect(b, pal::slotHover);

    Font* f = fBody ? fBody : fSmall;
    if (f) {
        char buf[80];
        if (zeroLabel && std::fabs(*v) < 1e-9) std::snprintf(buf, sizeof buf, "%s", zeroLabel);
        else                                   std::snprintf(buf, sizeof buf, fmt ? fmt : "%.2f", *v);
        r->textIn(*f, b, buf, (hotNow || active == id) ? pal::text : pal::textDim, align, 3.f);
    }

    if (hotNow || active == id) cursor = Cursor::ResizeV;
    return changed;
}

// ---------------------------------------------------------------------------
// Selector
// ---------------------------------------------------------------------------

bool Ui::selector(u64 id, const Rect& b, int* idx, const char* const* options, int count) {
    if (!idx || !options || count <= 0 || !r) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    auto step = [&](int d) {
        const int n = ((*idx + d) % count + count) % count;
        if (n != *idx) { *idx = n; changed = true; }
    };

    if (in->pressed[0] && hotNow) active = id;
    if (in->released[0] && active == id) {
        if (over) step(+1);
        active = 0;
    }
    if (in->pressed[2] && hotNow) step(-1);
    if (hotNow && in->wheel != 0.f) step(in->wheel > 0.f ? +1 : -1);

    r->roundRect(b, 2.f, hotNow ? pal::slotHover : pal::panelAlt);

    *idx = clampv(*idx, 0, count - 1);
    const char* label = options[*idx] ? options[*idx] : "";
    Font* f = fBody ? fBody : fSmall;
    if (f) r->textIn(*f, b, label, hotNow ? pal::text : pal::textDim, Align::Center, 3.f);

    if (hotNow) cursor = Cursor::Hand;
    return changed;
}

// ---------------------------------------------------------------------------
// Text field
// ---------------------------------------------------------------------------

bool Ui::textField(u64 id, const Rect& b, std::string* value, Col bg, Col fg,
                   Align align, bool activateOnDouble) {
    if (!value || !r) return false;
    static int blink = 0;

    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    const bool editing = (editId == id);
    bool committed = false;

    auto beginEdit = [&]() {
        editId = id;
        editBuf = *value;
        caret = (int)editBuf.size();
        active = id;
        blink = 0;
        editCommitted = false;
    };
    auto commit = [&]() {
        *value = editBuf;
        editId = 0;
        if (active == id) active = 0;
        editCommitted = true;
        committed = true;
    };
    auto cancel = [&]() {
        editId = 0;
        if (active == id) active = 0;
        editCommitted = false;
    };

    if (!editing) {
        if (activateOnDouble) {
            if (in->dblClick && over) beginEdit();
        } else if (in->pressed[0] && hotNow) {
            beginEdit();
        }
    } else {
        // A press anywhere else takes the value as typed, matching Live.
        if (in->pressed[0] && !over) commit();
    }

    if (editId == id) {
        ++blink;
        caret = clampv(caret, 0, (int)editBuf.size());

        if (!in->textInput.empty()) {
            std::string filtered;
            filtered.reserve(in->textInput.size());
            for (char c : in->textInput)
                if ((unsigned char)c >= 0x20 && c != 0x7F) filtered.push_back(c);
            if (!filtered.empty()) {
                editBuf.insert((size_t)caret, filtered);
                caret += (int)filtered.size();
                blink = 0;
            }
        }
        if (in->keyPressed[KeyBackspace] && caret > 0) {
            editBuf.erase((size_t)(caret - 1), 1);
            --caret;
            blink = 0;
        }
        if (in->keyPressed[KeyDelete] && caret < (int)editBuf.size()) {
            editBuf.erase((size_t)caret, 1);
            blink = 0;
        }
        if (in->keyPressed[KeyLeft])  { caret = clampv(caret - 1, 0, (int)editBuf.size()); blink = 0; }
        if (in->keyPressed[KeyRight]) { caret = clampv(caret + 1, 0, (int)editBuf.size()); blink = 0; }
        if (in->keyPressed[KeyHome])  { caret = 0; blink = 0; }
        if (in->keyPressed[KeyEnd])   { caret = (int)editBuf.size(); blink = 0; }
        if (in->keyPressed[KeyEscape]) cancel();
        else if (in->keyPressed[KeyEnter]) commit();
    }

    // --- draw ---
    const bool nowEditing = (editId == id);
    Col fill = bg;
    if (nowEditing) fill = pal::appBg;
    else if (hotNow) fill = bg.mix(pal::slotHover, 0.5f);
    if (fill.a > 0.f) r->rect(b, fill);
    if (nowEditing) r->roundRectOutline(b, 2.f, 1.f, pal::accent);

    Font* f = fBody ? fBody : fSmall;
    if (f) {
        const char* s = nowEditing ? editBuf.c_str() : value->c_str();
        const Rect inner = b.insetXY(3.f, 0.f);
        if (!nowEditing) {
            r->textIn(*f, b, s, fg, align, 3.f);
        } else {
            r->pushClip(b);
            const f32 tw = f->measure(s);
            f32 tx = inner.x;
            if (align == Align::Center)     tx = b.x + (b.w - tw) * 0.5f;
            else if (align == Align::Right) tx = inner.right() - tw;
            // Keep the caret on screen when the string overflows the box.
            const f32 caretRel = f->measure(s, caret);
            if (tx + caretRel > inner.right()) tx = inner.right() - caretRel;
            if (tx + caretRel < inner.x)       tx = inner.x - caretRel;
            const f32 ty = b.y + (b.h - f->height()) * 0.5f;
            r->text(*f, tx, ty, s, fg);
            if (((blink / 30) & 1) == 0) {
                r->rect({std::round(tx + caretRel), std::round(ty + 1.f), 1.f,
                         std::max(2.f, f->height() - 2.f)}, pal::accent);
            }
            r->popClip();
        }
    }

    if (hotNow) cursor = Cursor::Text;
    return committed;
}

} // namespace lat
