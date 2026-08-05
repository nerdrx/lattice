// MIDI note editor — the piano roll shown in the CLIP tab, and the clip
// automation lane that shares its time axis.
//
// Self-contained by design: it edits ClipModel::notes and ClipModel::envelopes
// in place and reports whether anything changed; the caller (App) owns pushing
// the result to the engine and everything else about clip lifetime. No engine
// types in here — not even for the note preview, which the roll only *asks*
// for (see PreviewQueue) and never performs itself, and not for automation,
// whose parameter names, ranges and units arrive as the plain AutoTargets view
// below rather than as devices the roll would have to know about.
#pragma once
#include "widgets.h"
#include "app.h"
#include <string>
#include <vector>

namespace lat {

// What a clip's envelopes are allowed to name, as the roll sees it: strings and
// floats only. Built by App::drawClipDetail each frame from the selected
// track's mixer fields and devices — no PluginInstance, no ParamInfo, no
// DeviceModel — which is what keeps this header compilable against app.h alone
// (docs/AUTOMATION.md §6.5).
struct AutoTargets {
    struct Entry {
        std::string group;          // "Track" or the device's display name
        std::string label;          // "Volume", "Drive", ...
        std::string address;        // canonical, what an AutoLane stores
        std::string unit;
        f32  lo = 0.f, hi = 1.f, def = 0.f;
        bool automated = false;     // this clip already has a lane for it
    };
    std::vector<Entry> entries;
    // Lanes the engine gave up on: bit i is clip.envelopes[i], set from
    // Ev::AutoLaneInert. An inert lane is drawn greyed with a one-line reason
    // — the envelope is visible, the sound does not move, and something says
    // why (docs/AUTOMATION.md §3.4).
    u32 inert = 0;
    // What to say about an inert lane; a default is used when this is empty.
    std::string inertWhy;

    const Entry* find(const std::string& address) const {
        for (const Entry& e : entries)
            if (e.address == address) return &e;
        return nullptr;
    }
};

// A set of indices into some vector, one member singled out as the primary —
// the one the last gesture was actually about. Extracted from the note
// selection so the automation lane's breakpoints can use it unchanged: the two
// index spaces are different, but every rule about them is the same. Sorted
// because a multi-delete has to run back to front and a bounds check only needs
// the last element; unique because every group edit would otherwise apply twice
// to the same member; the primary is always inside the set while there is one
// to have it. GUI thread only.
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
    //   * the bottom lane is a CHOOSER (docs/AUTOMATION.md §6.1, decision #9):
    //     it shows either the velocity stems or ONE of the clip's envelopes,
    //     never both. The selectors sit in the lane's key block, where the
    //     static "VEL" label used to be: what the lane shows, what "+" would
    //     add, and the shown lane's on/off toggle. Envelope editing uses the
    //     grid's own verbs — click empty space adds a breakpoint (grid-
    //     quantized in time, free in value), drag moves it, Alt frees the beat,
    //     Ctrl freezes it, right-click or double-click deletes, Shift+drag from
    //     empty space rubber-bands a set that then moves as one, Delete and the
    //     arrows act on the whole set. The lane uses the roll's OWN time axis,
    //     so zoom and scroll can never drift apart from the notes above.
    //   * an AUDIO clip gets the same editor with the note grid replaced by its
    //     waveform, drawn against that same time axis, so a sample's envelopes
    //     are edited where its transients are.
    //   * wheel = vertical scroll, Shift+wheel = horizontal, Ctrl+wheel = zoom
    //     the time axis about the cursor (kZoomMin..kZoomMax logical px/beat).
    //     A clip is first shown fit to the width; from the first Ctrl+wheel on,
    //     the zoom is the user's and is kept until the clip changes.
    //   * loop length readout + drag at the top-right of the ruler
    //     (whole-beat steps, min 1)
    // Notes must stay sorted by beat after every edit, and so must the points
    // of every envelope.
    bool draw(Ui& ui, const Rect& r, ClipModel& clip, const AutoTargets& targets,
              f64 playheadBeats, bool playing);

    // What the last edit that returned true was about, for the caller's undo
    // label. Valid only immediately after a call that reported a change.
    const char* lastEdit() const { return lastEdit_; }

    // Puts the lane on `idx` (0 = velocity, n = the clip's nth envelope) for
    // the clip that is about to be drawn — including one this roll has not seen
    // yet, whose identity reset would otherwise take the choice straight back.
    // The headless hook is the only caller: nothing inside gamescope can work a
    // selector, and a lane nobody can select is a lane no screenshot can check.
    void showLane(int idx) { laneSel_ = idx; pendingLane_ = idx; }

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
    // selection became a set, nor when the automation lane arrived: each one
    // extends to the whole set transparently, and each acts on the BREAKPOINTS
    // instead of the notes while the lane is showing an envelope and something
    // is selected in it. That is what makes Delete, Escape and the arrows work
    // in the lane without the caller having to know the lane exists.
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

    // The envelope the lane is showing, or null when it is showing the
    // velocity stems — or when laneSel_ names a lane a clip that changed under
    // us no longer has, which is why every caller goes through here rather than
    // indexing envelopes itself.
    AutoLane* shownLane(ClipModel& clip) const {
        return (laneSel_ > 0 && laneSel_ <= (int)clip.envelopes.size())
                   ? &clip.envelopes[(size_t)laneSel_ - 1]
                   : nullptr;
    }
    // The lane's key block: the chooser, the target "+" would add, the add
    // button and the shown lane's on/off toggle. Split out of draw() because it
    // is the one part that can ADD a lane, and therefore the one part that must
    // run before anything takes a pointer into clip.envelopes.
    void drawLaneKey(Ui& ui, const Rect& b, ClipModel& clip, const AutoTargets& targets,
                     f32 s, bool& changed);

    const AutoLane* shownLane(const ClipModel& clip) const {
        return (laneSel_ > 0 && laneSel_ <= (int)clip.envelopes.size())
                   ? &clip.envelopes[(size_t)laneSel_ - 1]
                   : nullptr;
    }

    // --- selection sets ----------------------------------------------------
    // Two of them, one machine: `sel_` indexes clip.notes, `psel_` indexes the
    // points of whichever envelope the lane is showing. Both are IndexSel, so
    // "sorted, unique, primary stays inside, an erase renumbers what follows"
    // is written once and the lane inherits the note grid's behaviour for free
    // — a group drag, a band, a multi-delete all work the same way in both.
    // GUI thread only; no allocation happens on any audio path.
    IndexSel sel_;               // notes
    IndexSel psel_;              // breakpoints of the shown envelope

    // --- the lane ----------------------------------------------------------
    // 0 = the velocity stems, n = clip.envelopes[n-1]. Clamped on every draw:
    // the caller can delete a lane (undo, a project load) between frames.
    int  laneSel_ = 0;
    // Which AutoTargets entry the "+" button would add a lane for.
    int  targetSel_ = 0;
    // A lane choice made from outside for a clip not yet drawn; -1 = none. See
    // showLane(): it survives exactly one identity reset and is then forgotten.
    int  pendingLane_ = -1;
    // The shown lane's value range as of the last draw. The keyboard API has no
    // AutoTargets to consult — it runs before the frame that would hand one over
    // — so the range the lane was last drawn against is what an arrow nudge is
    // measured and clamped against. 0..1 until a lane has been drawn, which is
    // the range of every mixer target anyway.
    f32  laneLo_ = 0.f, laneHi_ = 1.f;
    // The label the caller should put on the undo entry for the last change.
    const char* lastEdit_ = "note edit";

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
    // Drag state. Band and PointBand are the two drags with nothing under them
    // — hence the dragNote_ checks that exclude them. Point/PointBand are the
    // lane's, and are deliberately the same two shapes as the grid's.
    enum class Drag { None, Move, Resize, Velocity, Band, Point, PointBand } drag_ = Drag::None;
    int  dragNote_ = -1;
    f32  dragY_ = 0.f;
    f64  dragBeat_ = 0.0;
    int  dragPitch_ = 0;
    // Breakpoint being dragged, and the grab offsets that keep it under the
    // cursor: where inside the point the press landed, in beats and in value.
    int  dragPt_ = -1;
    f64  dragPtBeat_ = 0.0;
    f32  dragPtVal_ = 0.f;
    // Rubber-band anchor, held in CONTENT space rather than screen space
    // (a beat, and a pixel offset down the row stack) so that scrolling or
    // zooming mid-band leaves the corner on the material it was put on.
    f64  bandBeat_ = 0.0;
    f32  bandY_ = 0.f;
    // The lane's band anchor. Its second coordinate is a VALUE and not a pixel
    // offset, for the same reason bandY_ is a content offset: the lane's value
    // axis does not move, but the corner must stay on the material it was put
    // on when the time axis under it does.
    f32  bandVal_ = 0.f;
    // The selection as it stood when the band started. The band adds to it
    // rather than replacing it — Shift means "and also" here as everywhere —
    // which also means a band that touches nothing takes nothing away.
    std::vector<int> bandBase_;
};

} // namespace lat
