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
#include <vector>

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
    //   * the selection is a SET. Shift+click a note toggles it in or out;
    //     plain-clicking a note that is already part of a multi-selection keeps
    //     the set (so the click can start a group drag) and otherwise reduces
    //     the selection to that one note. Shift+drag from empty grid space
    //     rubber-bands: an accent-outlined rect that adds every note it touches
    //     to the selection as it is dragged. (Plain drag from empty space still
    //     adds a note — the press-drag-add gesture is unchanged, which is why
    //     the band needs a modifier at all.) Every group edit — move, nudge,
    //     delete, velocity, duplicate — acts on the whole set; see the keyboard
    //     API below.
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
    // selection is a set of indices, the caller can put a different clip in
    // front of the roll between frames, and a stale index is a wrong-note edit.
    // They are no-ops for a clip this roll has not drawn (uid mismatch), and
    // return the same "the clip changed, re-push it" as draw().
    //
    // These signatures are the caller's contract and did not change when the
    // selection became a set: each one extends to the whole set transparently.
    bool hasSelection(const ClipModel& clip) const;   // true for a set of any size
    bool clearSelection();                          // clears the whole set
    // Left/right by `gridSteps` grid steps, up/down by `semitones`, applied to
    // EVERY selected note. The delta is clamped once for the group rather than
    // per note — the group stops when its extreme member reaches the start of
    // the clip, its end, pitch 0 or pitch 127 — so relative spacing inside the
    // selection is never squashed by a wall. A pitch change auditions the
    // primary note only (see kPreviewMax: a thirty-note chord is not an
    // audition, it is noise).
    bool nudgeSelected(ClipModel& clip, int gridSteps, int semitones);
    bool deleteSelected(ClipModel& clip);           // deletes the whole set
    // Live's duplicate-loop (Cmd+D on the loop brace): doubles lengthBeats up
    // to kMaxLoopBeats and appends a copy of every note one old-length later.
    // The selection follows into the copy — the whole set does, note for note —
    // so a duplicate can be edited at once without hunting for the new notes.
    bool duplicateLoop(ClipModel& clip);
    // Pitches to audition this frame; see PreviewQueue. Empties the queue.
    int  drainPreview(u8* out, int max) { return preview_.drain(out, max); }

private:
    // True when `clip` is the clip this roll last drew. UID 0 (never assigned)
    // compares equal to a roll that has drawn nothing, which is as much
    // identity as an unsaved, un-uid'd clip has to offer.
    bool owns(const ClipModel& clip) const { return clip.uid == clipUid_; }

    // --- selection set -----------------------------------------------------
    // `sel_` is kept sorted ascending and free of duplicates: sorted because a
    // delete has to run back to front and a bounds check only needs the last
    // element, unique because every group edit would otherwise apply twice to
    // the same note. A vector rather than a bitset because it is the *order* we
    // keep needing, and because a selection is small in every session anyone
    // has ever played back — but it is not bounded in principle, so it grows.
    // GUI thread only; no allocation happens on any audio path.
    //
    // `primary_` is one member of `sel_` (or -1 when the set is empty): the
    // note the last gesture was actually about. It is what the view follows,
    // what the audition plays, and the anchor a group move is measured from.
    // Everything else about the set is symmetric.
    bool selHas(int i) const;
    void selClear();
    void selOne(int i);          // the set becomes {i}, primary_ = i
    void selAdd(int i);          // no-op when already in
    void selToggle(int i);
    // Rewrites the set after `at` was erased from clip.notes.
    void selErased(int at);
    // Drops anything past the end of a clip that changed under us.
    void selPrune(int noteCount);

    std::vector<int> sel_;       // indices into clip.notes, sorted + unique
    int  primary_ = -1;          // index into clip.notes, always in sel_ or -1

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
    u8   lastVel_ = 100;
    // "The press before this one created a note." Clicking empty space adds,
    // so without this the second click of a double-click on empty space would
    // delete what the first click made.
    bool addedLastPress_ = false;
    // Set by the keyboard edits: the next draw scrolls the selected note back
    // into view, so nudging a note off the top does not lose it.
    bool followSel_ = false;
    PreviewQueue preview_{};
    // Drag state. Band is the rubber band, and is the one drag with no note
    // under it — hence the dragNote_ checks that exclude it.
    enum class Drag { None, Move, Resize, Velocity, Band } drag_ = Drag::None;
    int  dragNote_ = -1;
    f32  dragY_ = 0.f;
    f64  dragBeat_ = 0.0;
    int  dragPitch_ = 0;
    // Rubber-band anchor, held in CONTENT space rather than screen space
    // (a beat, and a pixel offset down the row stack) so that scrolling or
    // zooming mid-band leaves the corner on the material it was put on.
    f64  bandBeat_ = 0.0;
    f32  bandY_ = 0.f;
    // The selection as it stood when the band started. The band adds to it
    // rather than replacing it — Shift means "and also" here as everywhere —
    // which also means a band that touches nothing takes nothing away.
    std::vector<int> bandBase_;
};

} // namespace lat
