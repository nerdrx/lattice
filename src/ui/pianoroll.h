// MIDI note editor — the piano roll shown in the CLIP tab for MIDI clips.
//
// Self-contained by design: it edits ClipModel::notes in place and reports
// whether anything changed; the caller (App) owns pushing the result to the
// engine and everything else about clip lifetime. No engine types in here —
// not even for the note preview, which the roll only *asks* for (see
// PreviewQueue) and never performs itself.
#pragma once
#include "widgets.h"
#include "app.h"

namespace lat {

class PianoRoll {
public:
    // --- audition ----------------------------------------------------------
    // Editing a note the user cannot hear is guesswork, so every edit that
    // implies a pitch — a note added, a note clicked, a drag or a keyboard
    // nudge that changes pitch — names that pitch here. The caller drains the
    // queue each frame and decides how to sound it (App: a short note through
    // the engine's MIDI ring).
    //
    // Fixed size and non-allocating: it holds one frame of edits, and one
    // frame cannot produce more pitches than a hand can play. A pitch already
    // queued is not queued twice, and a full queue drops the newest rather
    // than grow — an audition that arrives late is worse than one that never
    // arrives.
    struct PreviewQueue {
        static constexpr int kMax = 8;
        u8  pitch[kMax]{};
        int n = 0;
        void push(int p) {
            if (p < 0 || p > 127 || n >= kMax) return;
            for (int i = 0; i < n; ++i)
                if (pitch[i] == (u8)p) return;
            pitch[n++] = (u8)p;
        }
        // Moves up to `max` pitches into `out` and empties the queue, so a
        // caller that drains with too small a buffer still cannot be handed
        // the same note twice.
        int drain(u8* out, int max) {
            const int c = n < max ? n : max;
            for (int i = 0; i < c; ++i) out[i] = pitch[i];
            n = 0;
            return c;
        }
        void clear() { n = 0; }
    };
    static constexpr int kPreviewMax = PreviewQueue::kMax;

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
    //   * wheel = vertical scroll, Shift+wheel = horizontal, Ctrl+wheel = zoom
    //     the time axis about the cursor (kZoomMin..kZoomMax logical px/beat).
    //     A clip is first shown fit to the width; from the first Ctrl+wheel on,
    //     the zoom is the user's and is kept until the clip changes.
    //   * loop length readout + drag at the top-right of the ruler
    //     (whole-beat steps, min 1)
    // Notes must stay sorted by beat after every edit.
    bool draw(Ui& ui, const Rect& r, ClipModel& clip, f64 playheadBeats, bool playing);

    // --- keyboard API ------------------------------------------------------
    // Driven by App::handleShortcuts, which routes the arrows, Delete, Escape
    // and Ctrl+U here *only* while the roll is on screen for the selected clip
    // and (for the note-scoped ones) something is selected; otherwise those
    // keys keep their session-wide meaning. See App::visibleRoll().
    //
    // Every call takes the clip rather than trusting the last one drawn: the
    // selection is an index, the caller can put a different clip in front of
    // the roll between frames, and a stale index is a wrong-note edit. They
    // are no-ops for a clip this roll has not drawn (uid mismatch), and return
    // the same "the clip changed, re-push it" as draw().
    bool hasSelection(const ClipModel& clip) const;
    bool clearSelection();                          // true when it cleared one
    // Left/right by `gridSteps` grid steps, up/down by `semitones`; both
    // clamped into the clip and into 0..127, and a pitch change auditions.
    bool nudgeSelected(ClipModel& clip, int gridSteps, int semitones);
    bool deleteSelected(ClipModel& clip);
    // Live's duplicate-loop (Cmd+D on the loop brace): doubles lengthBeats up
    // to kMaxLoopBeats and appends a copy of every note one old-length later.
    // The selection follows into the copy, so a duplicate can be edited at
    // once without hunting for the new note.
    bool duplicateLoop(ClipModel& clip);
    // Pitches to audition this frame; see PreviewQueue. Empties the queue.
    int  drainPreview(u8* out, int max) { return preview_.drain(out, max); }

private:
    // True when `clip` is the clip this roll last drew. UID 0 (never assigned)
    // compares equal to a roll that has drawn nothing, which is as much
    // identity as an unsaved, un-uid'd clip has to offer.
    bool owns(const ClipModel& clip) const { return clip.uid == clipUid_; }

    f32  scrollY_ = 0.f;         // pixels, pitch axis (relative, see draw())
    f32  scrollX_ = 0.f;         // pixels, time axis
    // Logical (DPI-independent) px per beat. 0 means "not chosen yet": the
    // next draw fits the clip to the width, which is how every clip starts.
    f32  zoom_ = 0.f;
    // Identity of the clip drawn last frame. The caller swaps clips under us
    // freely (selecting another slot), and selection/scroll/zoom are all about
    // one particular clip, so they reset when this changes.
    u64  clipUid_ = 0;
    bool fold_ = false;
    int  selected_ = -1;         // index into clip.notes, -1 none
    u8   lastVel_ = 100;
    // "The press before this one created a note." Clicking empty space adds,
    // so without this the second click of a double-click on empty space would
    // delete what the first click made.
    bool addedLastPress_ = false;
    // Set by the keyboard edits: the next draw scrolls the selected note back
    // into view, so nudging a note off the top does not lose it.
    bool followSel_ = false;
    PreviewQueue preview_{};
    // Drag state
    enum class Drag { None, Move, Resize, Velocity } drag_ = Drag::None;
    int  dragNote_ = -1;
    f32  dragY_ = 0.f;
    f64  dragBeat_ = 0.0;
    int  dragPitch_ = 0;
};

} // namespace lat
