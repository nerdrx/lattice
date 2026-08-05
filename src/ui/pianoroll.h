// MIDI note editor — the piano roll shown in the CLIP tab for MIDI clips.
//
// Self-contained by design: it edits ClipModel::notes in place and reports
// whether anything changed; the caller (App) owns pushing the result to the
// engine and everything else about clip lifetime. No engine types in here.
#pragma once
#include "widgets.h"
#include "app.h"

namespace lat {

class PianoRoll {
public:
    // Draws the editor into `r` and handles all interaction within it.
    // Returns true when `clip.notes` (or lengthBeats) changed this frame, in
    // which case the caller must re-push the clip to the engine.
    //
    //   playheadBeats — position within the clip loop, only meaningful while
    //                   `playing`; drawn as the moving line.
    //
    // Interaction contract (Live-style, screenshot-matched):
    //   * pitch rows, low at the bottom; keyboard column at the left with
    //     octave labels (C3...), black-key rows tinted darker
    //   * FOLD toggle: show only pitches the clip uses (falls back to unfolded
    //     when the clip is empty)
    //   * fixed 1/16 grid for now; beats/bars accented like the ruler
    //   * click empty cell = add note (grid-quantized, default length one grid
    //     step, velocity = last-used); click note = select; drag note = move
    //     (pitch + beat); drag right edge = resize; right-click note = delete;
    //     double-click empty = add, double-click note = delete (Live habit)
    //   * velocity lane at the bottom: one stem per note, drag to set
    //   * wheel = vertical scroll, Shift+wheel = horizontal, Ctrl+wheel
    //     reserved for zoom (unimplemented is fine this wave)
    //   * loop length readout + drag at the top-right of the ruler
    //     (whole-beat steps, min 1)
    // Notes must stay sorted by beat after every edit.
    bool draw(Ui& ui, const Rect& r, ClipModel& clip, f64 playheadBeats, bool playing);

private:
    f32  scrollY_ = 0.f;         // pixels, pitch axis
    f32  scrollX_ = 0.f;         // pixels, time axis
    bool fold_ = false;
    int  selected_ = -1;         // index into clip.notes, -1 none
    u8   lastVel_ = 100;
    // Drag state
    enum class Drag { None, Move, Resize, Velocity } drag_ = Drag::None;
    int  dragNote_ = -1;
    f32  dragX_ = 0.f, dragY_ = 0.f;
    f64  dragBeat_ = 0.0;
    int  dragPitch_ = 0;
};

} // namespace lat
