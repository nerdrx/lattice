// Immediate-mode widget layer. Each widget owns a stable id so hot/active
// tracking survives layout changes between frames.
#pragma once
#include "../gfx/renderer.h"
#include "window.h"
#include <string>

namespace lat {

inline u64 uiId(int kind, int a = 0, int b = 0) {
    u64 h = 0x9E3779B97F4A7C15ull;
    h ^= (u64)(u32)kind * 0x100000001B3ull;
    h = (h << 7) | (h >> 57);
    h ^= (u64)(u32)a * 0xC2B2AE3D27D4EB4Full;
    h = (h << 11) | (h >> 53);
    h ^= (u64)(u32)b * 0x165667B19E3779F9ull;
    return h ? h : 1;
}

struct Ui {
    Renderer* r = nullptr;
    Input* in = nullptr;
    Font* fSmall = nullptr;    // 10px, labels
    Font* fBody  = nullptr;    // 11px, general
    Font* fBold  = nullptr;    // 11px bold, headers
    Font* fBig   = nullptr;    // 15px, tempo / position readouts

    u64  hot = 0, active = 0;
    u64  hotNext = 0;
    Cursor cursor = Cursor::Arrow;
    f32  dragAccum = 0.f;
    f64  dragStart = 0.0;

    // Inline text editing.
    u64  editId = 0;
    std::string editBuf;
    int  caret = 0;
    bool editCommitted = false;

    // Tooltip requested this frame.
    std::string tip;

    void beginFrame() { hotNext = 0; cursor = Cursor::Arrow; tip.clear(); }
    void endFrame()   { hot = hotNext; if (!in->down[0] && active && active != editId) active = 0; }

    bool hovered(const Rect& b) const { return b.contains(in->mx, in->my); }
    bool setHot(u64 id, const Rect& b) {
        if (hovered(b) && r->currentClip().contains(in->mx, in->my)) { hotNext = id; return true; }
        return false;
    }
    bool isHot(u64 id) const { return hot == id; }

    // --- widgets ----------------------------------------------------------
    // Returns true on click (release inside).
    bool button(u64 id, const Rect& b, const char* label, bool on = false,
                Col onCol = pal::accent, f32 radius = 2.f);
    // Small square toggle, Live's M / S / arm buttons.
    bool squareToggle(u64 id, const Rect& b, const char* label, bool* value, Col onCol);
    // Circular knob with an arc; `v` in [lo,hi]. Returns true while changing.
    bool knob(u64 id, const Rect& b, f32* v, f32 lo, f32 hi, f32 def, const char* fmt = nullptr);
    // Vertical fader with a stepped scale, like Live's mixer.
    bool vFader(u64 id, const Rect& b, f32* t);
    // Draggable numeric readout (tempo, gain, ...). Vertical drag.
    // `zeroLabel`, when given, replaces the formatted number at exactly zero:
    // a DAW is full of fields where 0 means "follow something else" and reads
    // as "Auto" or "Off" rather than as a quantity. `step`, when > 0, snaps the
    // value to a multiple of itself, which also keeps such a field landing on
    // an exact 0 instead of drifting past it.
    bool dragNumber(u64 id, const Rect& b, f64* v, f64 lo, f64 hi, f64 perPixel,
                    const char* fmt, Align align = Align::Center,
                    const char* zeroLabel = nullptr, f64 step = 0.0);
    // Click cycles through `options`; right-click steps backwards.
    bool selector(u64 id, const Rect& b, int* idx, const char* const* options, int count);
    // Editable text. Returns true when the value was committed.
    bool textField(u64 id, const Rect& b, std::string* value, Col bg, Col fg,
                   Align align = Align::Left, bool activateOnDouble = true);
    // What `id` currently has in the edit buffer, or null when it does not own
    // the caret. textField only writes back on commit, so a field whose owner
    // has to react per keystroke (a filter narrowing as you type) reads the
    // live text through here rather than reaching into editBuf itself.
    const std::string* liveText(u64 id) const { return editId == id ? &editBuf : nullptr; }

    // --- drawing helpers --------------------------------------------------
    void meterV(const Rect& b, f32 lvl, f32 peak);   // vertical peak meter
    void arc(f32 cx, f32 cy, f32 rad, f32 a0, f32 a1, f32 th, const Col& c);
    void bevel(const Rect& b, f32 radius, const Col& fill, f32 lightness = 0.06f);
    void playTriangle(const Rect& b, const Col& c);
    void stopSquare(const Rect& b, const Col& c);
};

} // namespace lat
