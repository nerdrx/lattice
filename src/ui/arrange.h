// The arrangement editor: tracks as lanes, a bar ruler, clips as rectangles on
// a timeline (docs/ARRANGEMENT.md §7).
//
// On the piano roll's seam, and for the roll's reasons. What the editor is
// allowed to see is the plain ArrangeContext below — model pointers and values
// only, no Engine, no Command, no RtClip, no PluginInstance — which is what
// keeps this header compilable against app.h alone and the editor testable
// without a window, exactly as pianoroll.h states for the roll and as
// AutoTargets does for the roll's devices.
//
// The caller (App::drawArrangementView) owns everything the view does not:
// building the context, arrangeRepair, publishArrangementFor, the undo point
// and the transport commands the ruler generates. That division is the roll's,
// and it is why the roll is as short as it is.
#pragma once
#include "autolane.h"
#include "pianoroll.h"      // AutoTargets — the same plain view of a target
#include "timeaxis.h"
#include "widgets.h"
#include <string>
#include <vector>

namespace lat {

// Layout, in logical px (multiply by the DPI scale).
inline constexpr f32 kArrHeaderW   = 138.f;  // the track column on the left
inline constexpr f32 kArrRulerH    = 20.f;
// An automation lane needs enough height to aim at and no more (§7.4).
inline constexpr f32 kArrAutoLaneH = 44.f;
inline constexpr f32 kArrMinLaneH  = 26.f;
inline constexpr f32 kArrMaxLaneH  = 320.f;
// The grab band on an item's edges, and on a lane's bottom edge.
inline constexpr f32 kArrEdgeGrab  = 5.f;
inline constexpr f32 kArrLaneGrab  = 4.f;
// Default zoom: one bar is 64 logical px, which fits about 25 bars in a
// 1600 px window and is the scale an arrangement is usually looked at.
inline constexpr f32 kArrZoomDefault = 16.f;
// The finest grid line the arrangement will draw. 1/16 lines four pixels apart
// are a texture and not a grid, so below this the grid thins to beats and then
// to bars -- the same ladder the ruler's labels thin along, so the two never
// disagree about which lines are the accented ones.
inline constexpr f32 kArrMinGridPx = 7.f;

// What the arrangement editor is allowed to see. Built by
// App::drawArrangementView each frame.
struct ArrangeContext {
    struct Lane {
        std::string name;
        int   colorIdx = 0;
        std::vector<ArrangeClip>* items  = nullptr;   // edited IN PLACE
        std::vector<AutoLane>*    autos  = nullptr;   // ditto
        f32*  height = nullptr;                       // TrackModel::arrHeight
        const AutoTargets* targets = nullptr;         // what its lanes may name
        bool  overridden = false;                     // Engine::arrOverride bit
        bool  armed = false;
        // A POINTER, where §7.1 has a value: the disclosure triangle is one of
        // the view's own gestures, so it has to be able to write the answer
        // back, and the bit is view state that belongs to no model field 8a
        // froze. The caller owns the storage (App::arrExpanded_).
        bool* expanded = nullptr;                     // automation lanes shown
    };
    std::vector<Lane> lanes;
    // Session::nextUid, so the view can stamp an item it creates -- a split, a
    // duplicate, a Ctrl+drag copy -- in the same breath. §7.1 does not have this
    // and the cost of not having it is a copy with no identity: the selection
    // cannot follow into it, the publisher's envelope table cannot key on it,
    // and the detail panel cannot find it. It is a plain u64 counter, so the
    // view still sees no model type it did not already see.
    u64*  nextUid   = nullptr;
    f64*  loopStart = nullptr;
    f64*  loopEnd   = nullptr;
    bool* loopOn    = nullptr;
    f64   playhead  = 0.0;
    bool  playing   = false;
    // The signature map, BORROWED from the session (timeaxis.h). It replaced a
    // plain `int sigNum`, and the replacement is the whole point of this wave:
    // one number can only describe a set whose bars are all the same width, so a
    // ruler holding one had no way to be wrong and no way to be right either.
    // Still a plain value the view may only read -- no Session crosses the seam.
    SigMap sig;
    // The selected item, as a (track, item uid) pair -- uid and not index,
    // because an insert renumbers indices between frames and a stale index is a
    // wrong-clip edit. Owned by the caller so the detail panel can read it.
    int   selTrack = -1;
    u64   selItem  = 0;

    // --- what the view asks the caller for, written back each frame ---------
    // §7.1 gives the view a Changed mask and nothing else. These four exist
    // because three of the gestures §7.5 lists are not edits to the model at
    // all -- they are requests -- and a mask cannot carry a beat, a track or a
    // drop point.
    //
    // The tracks whose lanes were mutated, so the caller republishes those and
    // not all of them: a lane is up to 1.6 MB, and "republish everything on
    // every edit" would make a nudge cost the whole set.
    std::vector<int> dirty;
    // >= 0: the ruler was clicked at this beat and wants a locate.
    f64  locateBeat = -1.0;
    // >= 0: this track's override tint was clicked and wants Back to Arrangement.
    int  backToArrTrack = -1;
    // >= 0: the ruler was right-clicked at this BAR and wants the signature
    // change there added, or -- if one is already there -- removed. The view
    // reports the bar and not the verb: add and remove are both edits to a map
    // it does not have, the clamps and the dedupe live in session.h, and a view
    // that decided which one it was would have had to keep its own copy of the
    // map to decide with. Bar 0 is reported like any other; the caller refuses
    // to remove it, because Session::removeSignature does.
    i64  sigBar = -1;
    // A drag in flight from elsewhere in the app (the browser, the session
    // grid). The caller sets `dropActive`; the view answers with where it was
    // let go, and the caller decides what to put there -- the view knows
    // nothing about samples, slots or clip loading.
    bool dropActive = false;
    bool dropped    = false;
    int  dropTrack  = -1;
    f64  dropBeat   = 0.0;
    // A one-shot verb the MOUSE asked for (right-click deletes, double-click
    // splits). It is a request and not an edit: the undo point has to be taken
    // before the model moves, and only the caller can take one -- so the view
    // asks, the caller takes its point and calls the verb.
    bool wantDelete = false;
    bool wantSplit  = false;
};

class ArrangeView {
public:
    // Draws into `r`, handles every interaction inside it, and returns a mask of
    // what changed so the caller knows what to re-push and what to take an undo
    // point for.
    enum Changed : u32 { None = 0, Items = 1u << 0, Autos = 1u << 1,
                         Loop = 1u << 2, Layout = 1u << 3, Selection = 1u << 4 };
    u32 draw(Ui& ui, const Rect& r, ArrangeContext& ctx);

    // The caller's undo label for whatever last reported a change.
    const char* lastEdit() const { return lastEdit_; }

    // THE UNDO HANDSHAKE. A drag mutates the model over many frames, and an
    // undo entry has to be taken BEFORE the first of them -- but whether a
    // gesture is going to mutate anything is not knowable at the press (a click
    // that only selects must not cost an entry). So the view arms a drag on the
    // press, waits for the cursor to actually move, and on that frame -- before
    // touching the model -- names the edit here. The caller takes its point and
    // the mutation starts on the next frame, which is one frame of latency at
    // the very start of a gesture and the same shape DragState::armed already
    // has. Cleared by the caller through takePendingEdit().
    const char* pendingEdit() const { return pendingEdit_; }
    const char* takePendingEdit() {
        const char* p = pendingEdit_;
        pendingEdit_ = nullptr;
        return p;
    }
    // The gesture id the caller should coalesce its undo entry on, so one drag
    // is one entry. 0 when nothing is being dragged.
    u64 gesture() const { return gesture_; }

    // --- one-shot verbs, for the caller's keyboard routing -----------------
    // Each takes the context rather than trusting the last one drawn: the
    // caller can rebuild it between frames, and a stale pointer is a wrong-lane
    // edit. The caller takes its undo point BEFORE calling (these are one-shot,
    // so there is no gesture to coalesce on) and reads `dirty` afterwards.
    bool hasSelection() const { return selTrack_ >= 0 && selItem_ != 0; }
    u32  deleteSelected(ArrangeContext& ctx);
    // Splits the selected item at the grid-quantized cursor beat. A split
    // exactly on an edge, or one that would leave either half under
    // kMinArrBeats, does nothing.
    u32  splitSelected(ArrangeContext& ctx);
    // Live's duplicate: a copy of the selection one length later, and the
    // selection follows into the copy, because the copy is what the user is
    // about to move.
    u32  duplicateSelected(ArrangeContext& ctx);
    // Where the split lands and where a drop would go. The cursor beat while
    // the pointer is over the lanes, otherwise the playhead as of the last
    // draw -- so Ctrl+E works with the mouse outside the window.
    f64  cursorBeat() const { return cursorBeat_; }

    // --- headless hooks (§7.7) ---------------------------------------------
    // Nothing inside gamescope can click an item, so the selection is settable.
    void selectItem(int track, u64 uid) { selTrack_ = track; selItem_ = uid; }
    int  selectedTrack() const { return selTrack_; }
    u64  selectedItem() const { return selItem_; }
    // The zoom and scroll, so a screenshot can be taken at a known place on the
    // timeline rather than wherever the last gesture left it.
    void setView(f32 pxPerBeat, f32 scrollX) { zoom_ = pxPerBeat; scrollX_ = scrollX; }
    // Where a split would land. Nothing inside gamescope can put a cursor
    // somewhere, and a split at beat 0 is a split the verb correctly refuses.
    void setCursorBeat(f64 b) { cursorBeat_ = b; }

private:
    // Where an item's index lives for a uid, or -1. Everything that edits goes
    // through here: an insert renumbers, and a stale index is a wrong-clip edit.
    static int indexOf(const std::vector<ArrangeClip>& v, u64 uid);

    // One automation lane's view, kept across frames so its own selection and
    // drag survive. Indexed [track][lane]; resized rather than rebuilt, so an
    // edit to a lane does not drop the point the hand is holding.
    std::vector<std::vector<AutoLaneView>> laneViews_;
    // Which target each track's "+" would add a lane for. Per track, because
    // the chooser is per track; kept across frames, because a selector the user
    // has cycled must stay where they left it.
    std::vector<int> targetSel_;

    // The item selection, as the (track, uid) pair the context carries.
    int selTrack_ = -1;
    u64 selItem_  = 0;

    // The view.
    f32 zoom_    = kArrZoomDefault;   // logical px per beat
    f32 scrollX_ = 0.f;               // content px along time
    f32 scrollY_ = 0.f;               // content px down the lane stack
    f64 cursorBeat_ = 0.0;

    // The gestures. Each is a drag on one thing, and every one of them is
    // measured from the values the item had at the press -- so a drag is
    // absolute against its own start and cannot accumulate rounding.
    enum class Drag {
        None, Move, TrimL, TrimR, FadeIn, FadeOut, LaneH, Loop
    } drag_ = Drag::None;
    bool moved_ = false;              // past the movement threshold; see pendingEdit()
    u64  gesture_ = 0;
    int  dragTrack_ = -1;             // the item's track at the press
    u64  dragUid_ = 0;
    f64  grabBeat_ = 0.0;             // cursor beat - item.start, at the press
    f32  grabY_ = 0.f;
    // The item as it was at the press. A drag reads these and writes absolute
    // values, so it never integrates its own error.
    f64  origStart_ = 0.0, origOffset_ = 0.0, origLength_ = 0.0;
    f64  origFadeIn_ = 0.0, origFadeOut_ = 0.0;
    f32  origHeight_ = 0.f;
    bool dupMade_ = false;            // Ctrl+drag: the copy has been made
    // The loop drag's anchor beat, and whether it has travelled far enough to
    // be a brace rather than a locate.
    f64  loopAnchor_ = 0.0;
    bool loopMoved_ = false;

    const char* lastEdit_ = "arrangement edit";
    const char* pendingEdit_ = nullptr;
};

// ---------------------------------------------------------------------------
// PUBLISHING THE SIGNATURE MAP  (session.h, publishSignatures)
//
// A set that never publishes plays in 4/4 no matter what the ruler draws, which
// is the one thing about this wave that must not go silently wrong -- so these
// three are declared where both callers can see them rather than left to a call
// site somebody has to remember.
//
// WHERE THE STATE LIVES, and why not on App. The published pointer, the arrays
// still in flight and the last map published are App's business by every other
// precedent in this program (publishedNotes_/retiringNotes_, warpMaps_). They
// are file-static in app_arrange.cpp instead because this wave does not own
// app.h -- exactly the reason warpMaps_ spent a wave in a translation unit
// before it moved, and the same note applies: this belongs on App and should go
// there the moment one agent owns both files.
//
// GUI THREAD ONLY, like everything else in src/ui.
// ---------------------------------------------------------------------------

// The session's map, as a view sees it. ONE place, so a ruler, a readout and a
// grid cannot end up holding three differently-built views of the same vector --
// and one place for the rule that an EMPTY `sigs` is not "4/4" but "one entry,
// sigNum/sigDen, at bar 0", which is what a set that has never been re-barred
// and every v1..v6 file mean (session.h says the same over Session::sigAtBar).
inline SigMap sigMapOf(const Session& s) {
    SigMap m;
    m.v     = s.sigs.empty() ? nullptr : s.sigs.data();
    m.n     = (int)s.sigs.size();
    m.fbNum = s.sigNum;
    m.fbDen = s.sigDen;
    return m;
}

// Publish `s`'s map if it differs from the one last published, and do nothing at
// all if it does not. Called once per frame from the control bar -- which is
// drawn unconditionally and first -- so a load, an undo, a redo and every
// signature edit are all covered by one call site and none of them can be
// forgotten. A publication the ring refuses is simply retried next frame.
void syncSignatures(Engine& eng, const Session& s);

// Ev::SigsRetired. Returns true when `p` was an array we published and have now
// freed, false when it is not one of ours -- in which case the caller must leak
// it rather than free a pointer nobody here owns, which is the rule every other
// retirement in this program follows.
bool reapSignatures(const void* p);

// Everything still held, freed. Safe ONLY once the audio thread is joined, i.e.
// after EngineHandle::close(), beside App::shutdown()'s existing sweep over the
// chains and the note arrays.
//
// NOT CALLED YET, and deliberately so rather than by omission: App::shutdown()
// is in app.cpp, which this wave does not own. Nothing leaks meanwhile -- the
// owning object has static storage duration and frees in its destructor, which
// runs after main() returns and therefore after the audio thread is long gone,
// and a leak checker sees nothing. This is the tidier place for it and the one
// line App::shutdown() is owed.
void dropSignatures();

// How many displaced arrays are still waiting for their Ev::SigsRetired, and how
// many have come home. For the headless hook only: a retirement protocol that
// publishes and never reaps looks exactly like one that works, until the leak
// checker or the machine runs out of memory hours later.
int  sigsInFlight();
int  sigsReaped();

} // namespace lat
