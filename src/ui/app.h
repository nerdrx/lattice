// The application shell: class App and the small value-types it owns.
//
// The session model lives in session.h (ClipModel/TrackModel/Session/…). This
// header is class App, split for definition across the eight app_*.cpp
// translation units; a member add rebuilds those but not src/core or the roll.
#pragma once
#include "session.h"
// Where the engine is, and what it looked like this frame. App holds no Engine,
// no AudioBackend and no MidiInput of its own any more: EngineHandle owns all
// three (or, from step 2, an ipc::EngineClient instead), and everything that
// used to be an engine_.<atomic>.load() during a draw reads es_ instead.
// docs/GUI-ON-DAEMON.md §2.
#include "engine_handle.h"
#include "engine_state.h"
// Remote control (the append-only block at the end of class App). Both are
// self-contained: learn.h reaches no further than MidiMsg, osc.h no further
// than core/. Neither knows this header exists.
#include "../control/learn.h"
#include "../control/osc.h"
#include "../gfx/renderer.h"
#include "widgets.h"
#include "window.h"
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace lat {

// The MIDI note editor. Declared rather than included: pianoroll.h includes
// *this* header (it edits ClipModel directly), so pulling it in from here would
// leave PianoRoll incomplete for any translation unit that reached pianoroll.h
// first. App therefore holds it behind a unique_ptr and defines its destructor
// out of line, in the one .cpp that has both definitions.
class PianoRoll;
// Same reason, and the roll's other argument: the plain view of what a clip's
// envelopes may name (pianoroll.h). Only ever passed by reference from here.
struct AutoTargets;
// The arrangement editor and the plain view it is handed (arrange.h). Same
// argument again: arrange.h includes pianoroll.h, which includes this header.
class ArrangeView;
struct ArrangeContext;

class App {
public:
    // Both out of line: a defaulted constructor still has to be able to unwind
    // its members, so it needs PianoRoll complete just as the destructor does.
    App();
    ~App();
    bool init(int argc, char** argv);
    void run();
    void shutdown();

private:
    // --- frame ---
    void frame();
    void handleShortcuts();
    void updateKbdPiano();                        // computer piano -> engine MIDI
    void toggleKbdMidi();
    void pumpEngineEvents();

    // --- piano roll routing + note preview ---
    // The roll, but only while it is actually on screen for the selected clip.
    // That is the whole condition for it to own a key: arrows, Delete, Escape
    // and Ctrl+U keep their session-wide meaning everywhere else.
    PianoRoll* visibleRoll();
    // Audition one pitch for the clip identified by `clipUid`: note-on now, the
    // matching off scheduled kPreviewSecs out. See the note on previews_.
    void startPreview(int pitch, u64 clipUid);
    void updatePreviews();                        // send the offs that came due
    void stopPreviews();                          // every off, now

    // --- views ---
    void drawControlBar(const Rect& r);
    void drawBrowser(const Rect& r);
    void drawSessionView(const Rect& r);
    void drawTrackHeaders(const Rect& r, f32 scrollX);
    void drawClipGrid(const Rect& r, f32 scrollX);
    void drawSceneColumn(const Rect& r);
    void drawMixer(const Rect& r, f32 scrollX);
    void drawReturnStrips(const Rect& r);         // the A-D buses, beside MASTER
    void drawMasterStrip(const Rect& r);
    void drawDetailPanel(const Rect& r);          // tab header + active tab
    void drawClipDetail(const Rect& r);
    void drawDeviceDetail(const Rect& r);
    void drawPluginBrowser(const Rect& r);
    void drawDeviceStrip(const Rect& r);
    void drawArrangementView(const Rect& r);
    void drawStatusBar(const Rect& r);
    void drawDragGhost();

    // --- clip helpers ---
    void  drawClipSlot(const Rect& r, int track, int slot);
    void  drawWaveform(const Rect& r, const SampleBuffer& sb, const Col& c,
                       f64 t0 = 0.0, f64 t1 = 1.0);
    void  loadClipInto(int track, int slot, const std::string& path);
    // One decoded file as a clip, with nothing else touched. Shared by the slot
    // loader above and by the arrangement's drop target, which has no slot in
    // the middle because an item owns its clip by value.
    bool  makeClipFromFile(const std::string& path, int colorIdx, ClipModel& out);
    void  clearClip(int track, int slot);
    void  pushClip(int track, int slot);          // sync one slot to the engine
    // One cell of the engine's slot table, brought into line with the model
    // whatever the model now says about it: inside the set that is pushClip,
    // outside it that is the ClearClip releaseStaleSlots sends. The deferred
    // publication below retries cells through here rather than through either,
    // because a cell queued before a load can be outside the set by the time it
    // drains.
    void  syncClipCell(int track, int slot);
    void  clearStaleSlot(int track, int slot);
    // Hands `fresh` (which may be null) to publishedNotes_[track][slot] and
    // moves whatever was there into retiringNotes_. Only called once the engine
    // has actually accepted the clip carrying `fresh`.
    void  publishNotes(int track, int slot, const RtNote* fresh);
    void  pushTrack(int track);                   // sync mixer state
    void  pushAll();
    void  addTrack();
    void  addScene();
    // True when the track's chain can be played by notes, which is what makes a
    // slot a MIDI target rather than an audio one. Judged from the descriptor,
    // so a device whose plugin is missing today still declares the track's
    // intent and the set does not silently turn into an audio track.
    bool  trackHasNoteDevice(int track) const;
    void  createMidiClip(int track, int slot);    // empty pattern in an empty slot
    // Every path that moves the selection goes through here: selecting a track
    // also arms it (see autoArmed_).
    void  selectTrack(int track);
    // Points the DEVICES tab at a chain owner (see the addressing below).
    // Selecting a track goes through selectTrack, which calls this; a return or
    // the master has no clips, so clicking one also swings the detail panel to
    // DEVICES -- the CLIP tab has nothing to say about a bus.
    void  selectChainOwner(int owner);

    // --- recording ---
    void  startRecording(int track, int slot);   // allocate + arm a take
    void  stopRecording(int track);              // second RecordSlot = stop
    void  finishRecording(const Event& e);       // Ev::RecordFinished -> clip
    void  finishMidiRecording(const Event& e);   // Ev::MidiRecordFinished -> clip

    // --- project ---
    bool  openProject(const std::string& path);
    void  saveProjectTo(const std::string& path);
    void  assignUids();                          // fill in any uid still 0
    void  serializeDevices();                    // devices -> savedDevices
    // savedDevices -> devices. `reuse`, when given, is a pool of instances
    // lifted out of the session being replaced (undo only): a saved device
    // whose uid *and* uri are in the pool adopts that instance and has the
    // snapshot's parameter values applied to it, instead of loading the plugin
    // again. Everything with no match is instantiated exactly as before.
    void  materializeDevices(std::vector<LiveDevice>* reuse = nullptr);
    void  releaseAllChains();                    // hand every instance to retiring_
    // The shared tail of "the whole session is being replaced", used by both a
    // project load and an undo restore -- they differ only in where the Session
    // came from and in what may be carried across it. `restore` is what says
    // this is state the app already had: it carries the clips' audio (see
    // ClipSample), and its presence is also what turns on the plugin rebind
    // and turns off assignUids, because those are the same question.
    void  adoptSession(Session&& next, const std::vector<ClipSample>* restore);
    // Clears whatever the engine still holds for slots the new session has no
    // track or scene for. Called from pushAll, which is the only place that
    // knows the engine's slot table has just been rewritten.
    void  releaseStaleSlots();

    // --- transport helpers ---
    void  send(Cmd t, i32 a = 0, i32 b = 0, f64 x = 0.0);
    void  setTempo(f64 bpm);
    void  togglePlay();

    // --- browser ---
    void  refreshBrowser();
    void  browseTo(const std::string& dir);

    Window   win_;
    Renderer rend_;
    Ui       ui_{};
    Font     fSmall_, fBody_, fBold_, fBig_;

    // The engine, wherever it is. Owns the Engine, the audio backend and the
    // MIDI reader on the local path; owns an ipc::EngineClient on the daemon
    // path when that lands. Nothing outside this member and the six functions
    // that command it knows which.
    EngineHandle eng_;
    // What the engine looked like at the top of this frame. THE ONLY THING the
    // draw code may read about engine state — see engine_state.h for why one
    // sample per frame is not a convenience but a correctness fix.
    EngineState  es_;

    // =======================================================================
    // ENGINE FLOW CONTROL — deferred publication
    //   docs/ARRANGEMENT.md §15 (the bug), docs/GUI-ON-DAEMON.md §2.2 (the shape)
    //
    // pushAll() sends roughly 3 + tracks*9 + tracks*scenes commands in one
    // burst. A full 32x32 set is about 1320 of them against a 1024-deep ring
    // that the audio thread drains once per block, so an ordinary project load
    // or undo restore OVERFLOWED IT — and the failure mode was the bad kind:
    // pushClip logged and gave up, leaving the engine playing a clip the model
    // no longer believed in, with nothing scheduled to notice.
    //
    // The fix is flow control, not a bigger ring. A ring is a realtime
    // structure sized for the steady state; a burst that large wants to be
    // spread across frames.
    //
    // The shape, and the two properties that make it correct:
    //
    //   * IT QUEUES INTENT, NOT PAYLOAD. An entry says "cell (3,7) needs
    //     publishing", not "here is an RtClip". The allocation, the resolve and
    //     the retirement bookkeeping all happen at DRAIN time, from the model as
    //     it stands then. So a queued cell can be edited, cleared or fall
    //     outside a freshly loaded set before it drains and the right thing
    //     still happens — and a fader dragged for a second collapses into one
    //     publish instead of sixty.
    //   * IT IS ONE FIFO. While anything is waiting, every new command joins the
    //     back rather than going to the ring, so nothing can overtake something
    //     already queued. Two queues would have needed a rule about how they
    //     interleave, and the answer for (say) a queued Cmd::ClipGain against a
    //     fresh Cmd::SetClip for the same cell is not obvious.
    //
    // Chain publishes deliberately stay direct: there are at most 37 of them,
    // they are independent of clips and scalars, and they carry an RtChain whose
    // retirement is already a hand-built protocol.
    // =======================================================================
    enum class PubKind : u8 {
        Scalar,     // a pointer-free Command: type/a/b/x, replayed verbatim
        Clip,       // a = track, b = slot   -> syncClipCell
        ArrLane,    // a = track, or -1 for the transport cell
        ArrAutos,   // a = track
    };
    struct PendingPub {
        PubKind kind = PubKind::Scalar;
        Cmd     type = Cmd::SetPlaying;      // Scalar only
        i32     a = 0, b = 0;
        f64     x = 0.0;
    };
    // Deep enough for the worst burst this app can produce (a 32x32 load is
    // ~1400 entries) several times over, shallow enough that an engine which has
    // stopped draining entirely — no audio backend at all — cannot quietly eat
    // memory. Reaching it is counted and said out loud, exactly as the old
    // per-command warning was, because that case IS the old failure.
    static constexpr size_t kMaxPending = 8192;
    std::deque<PendingPub> pending_;
    // Membership, so a cell queued twice is queued once. Not for scalars: two
    // sends of the same command are two facts, and their order is the meaning.
    bool   pendClip_[kMaxTracks][kMaxScenes] = {};
    bool   pendLane_[kMaxTracks + 1] = {};    // index kMaxTracks == transport cell
    bool   pendAutos_[kMaxTracks] = {};
    bool   flushing_   = false;   // inside flushPending(): do the work, never re-queue
    bool   pubRefused_ = false;   // a publisher's answer to flushPending()
    size_t pendHigh_   = 0;       // deepest the queue has ever been, this run
    u64    pendDropped_ = 0;      // entries the cap threw away

    // Drains the queue into the ring, in order, until the ring refuses. Called
    // at the top of every frame, before anything else touches the engine.
    void flushPending();
    // Asked by every deferrable publisher before it does any work. True when the
    // work was queued instead — because something is already waiting, and going
    // straight to the ring would overtake it.
    bool deferPub(PubKind k, i32 a = 0, i32 b = 0);
    // The refusal path: the ring said no. Queues the work, or — when we ARE the
    // drain — tells flushPending to stop and leave it at the front.
    void refusePub(PubKind k, i32 a = 0, i32 b = 0);
    void queuePub(const PendingPub& p);
    // The membership flag for a piece of work, or null for a scalar.
    bool* pendSeen(PubKind k, i32 a, i32 b);

    // Headless verification hook (NXTAKT_DEBUG_PUSHALL), in the shape of the
    // undo self-test's: nothing inside gamescope can load a 32x32 set and then
    // look at what the engine ended up holding, and "the engine's slot table
    // agrees with the model" is exactly the property the old failure broke
    // silently. It waits for the queue to drain, then launches every scene in
    // turn and checks each track against ses_ — a launch only starts a voice
    // when the engine's own clips_[t][s] is valid (engine.cpp, Cmd::LaunchScene),
    // so this reads the engine's table rather than anything the GUI believes
    // about it. Runs across frames because that is what the fix does.
    void debugPushAllCheck();
    bool pushAllHook_ = false, pushAllDone_ = false, pushAllLaunched_ = false;
    int  pushAllScene_ = 0, pushAllWait_ = 0;
    int  pushAllCells_ = 0, pushAllFails_ = 0;
    u64  pushAllStalls_ = 0;

    // --- recording ---------------------------------------------------------
    // A take in flight. The capture buffer is GUI-owned for its whole life:
    // Cmd::RecordSlot only lends it to the engine, which appends into it and
    // hands the pointer back in Ev::RecordFinished. Nothing here is freed on
    // any other path while the audio thread runs (shutdown() being the
    // exception, and only after the backend has been joined).
    // A take is either audio or MIDI, and the two carry different buffer types
    // back on different events, so every entry says which it is rather than
    // leaving the pointer to be guessed from whichever event turned up.
    struct PendingRec {
        f32*    buf   = nullptr;      // audio take, interleaved stereo
        RtNote* notes = nullptr;      // MIDI take
        i64  cap = 0;                 // frames for audio, notes for MIDI
        int  track = -1, slot = -1;
        bool midi = false;
        // Set when an undo tore the session out from under a take in flight.
        // The buffer is NOT freed here -- the engine may still be appending to
        // it -- so the entry stays, the stop is sent, and the finish handler
        // throws the material away instead of building a clip in a session
        // that no longer expects one.
        bool cancelled = false;
        const void* payload() const {
            return midi ? (const void*)notes : (const void*)buf;
        }
    };
    std::vector<PendingRec> pendingRecs_;
    // The global record button arms the *intent* to record, exactly like Live's
    // session record: while it is lit, clicking an empty slot on an armed track
    // starts a take there; while it is unlit, the same click only selects the
    // slot. It is not itself a transport action.
    bool recIntent_ = false;
    int  recTakeNo_ = 1;                          // names takes "Rec 1", "Rec 2", ...
    f64  recStartBeat_[kMaxTracks]{};             // from Ev::RecordStarted
    int  midiClipNo_ = 1;                         // names patterns "MIDI 1", ...

    // --- MIDI clip note arrays ---------------------------------------------
    // Exactly the RtChain protocol, one array per slot: pushClip() allocates a
    // fresh RtNote[] for the clip it publishes, parks the pointer here, and
    // moves whatever it displaced into retiringNotes_. An entry is freed when
    // its Ev::NotesRetired arrives, and never on any other path while the audio
    // thread runs — a clip's notes can be edited while that clip is playing.
    const RtNote* publishedNotes_[kMaxTracks][kMaxScenes] = {};
    std::vector<const RtNote*> retiringNotes_;
    // "The engine currently holds a clip for this slot." Only pushClip writes
    // it, and only from a command the ring accepted. A load or an undo can
    // shrink the set, and the slots that fall outside the new one would
    // otherwise keep an RtClip pointing into a SampleBuffer this session has
    // stopped owning -- see releaseStaleSlots.
    bool clipLive_[kMaxTracks][kMaxScenes] = {};

    // --- warp maps ---------------------------------------------------------
    // The fourth instance of the RtChain / RtNote / RtAutoSet retirement
    // protocol, and deliberately the same one line for line: pushClip()
    // allocates a fresh WarpMarker[] for the clip it publishes, parks the
    // pointer here, and moves whatever it displaced into retiringWarp_. An entry
    // is freed when its Ev::WarpRetired arrives and on no other path while the
    // audio thread runs — a clip's warp map can be dragged while that clip is
    // playing.
    //
    // These lived as a file-scope publisher in app_engine.cpp, with a runtime
    // "is a second App using this table" check, purely because the wave that
    // added them could not edit this header. On App they are per-instance by
    // construction, which is what that check was defending, so it is gone;
    // nothing else about the behaviour changed.
    //
    // The destructor is the AutoBlocks argument verbatim: App is destroyed after
    // shutdown() has joined the audio thread, so this is the same moment
    // shutdown() frees the note arrays, and a destructor cannot be forgotten by
    // a later edit to shutdown(). What it frees is what the engine still held
    // when the process ended plus anything whose retirement event was never
    // drained.
    struct WarpMaps {
        const WarpMarker* published[kMaxTracks][kMaxScenes] = {};
        std::vector<const WarpMarker*> retiring;
        WarpMaps() = default;
        WarpMaps(const WarpMaps&) = delete;
        WarpMaps& operator=(const WarpMaps&) = delete;
        ~WarpMaps() {
            for (auto& row : published)
                for (const WarpMarker*& m : row) { delete[] m; m = nullptr; }
            for (const WarpMarker* m : retiring) delete[] m;
            retiring.clear();
        }
    };
    WarpMaps warpMaps_;
    // publishNotes verbatim, one slot's map at a time.
    void publishWarp(int track, int slot, const WarpMarker* fresh);

    // --- plugin hosting -------------------------------------------------
    // Chain lifecycle: publishChain(t) heap-allocates an RtChain from
    // ses_.tracks[t].devices, sends it via Cmd::SetChain, and moves the
    // previously published pointer into retiring_ together with any
    // instances the new chain no longer references. pumpEngineEvents()
    // frees a retiring_ entry when its Ev::ChainRetired arrives. Nothing is
    // freed on any other path while the audio thread runs.
    PluginRegistry registry_;
    bool registryScanned_ = false;
    struct RetiredChain {
        const RtChain* chain = nullptr;
        std::vector<std::unique_ptr<PluginInstance>> dying;
    };
    std::vector<RetiredChain> retiring_;
    const RtChain* published_[kMaxTracks] = {};
    const RtChain* publishedReturn_[kMaxReturns] = {};
    const RtChain* publishedMaster_ = nullptr;
    // One retirement pool for all three kinds of owner, and deliberately so: an
    // entry is matched on the chain POINTER, every published RtChain is its own
    // allocation, and no chain is ever published to two owners -- so the pointer
    // alone says which entry an Ev::ChainRetired means, whatever its `a` is.
    // Per-owner pools would only add a way for the two to disagree.

    // --- chain owners ------------------------------------------------------
    // A chain hangs off one of three things: a track, a return bus, or the
    // master. They differ only in *where* the device list, the saved list and
    // the published pointer live and in which command carries the chain across,
    // so everything that builds, publishes, saves, restores or draws a chain
    // takes an owner id and goes through ChainOwner rather than existing three
    // times. The id is the engine's own Ev::ChainRetired addressing (engine.h):
    //   0 .. kMaxTracks-1            a track
    //   kMaxTracks + i               return i
    //   -1                           the master
    static constexpr int kOwnMaster  = -1;
    static constexpr int kOwnReturn0 = kMaxTracks;
    static constexpr int ownReturn(int i)   { return kOwnReturn0 + i; }
    static constexpr bool ownIsTrack(int o) { return o >= 0 && o < kMaxTracks; }
    static constexpr bool ownIsReturn(int o) {
        return o >= kOwnReturn0 && o < kOwnReturn0 + kMaxReturns;
    }
    struct ChainOwner {
        // Null for a published slot the session has no model behind any more --
        // a track index past the end after the set shrank. The engine may still
        // be running its chain, so the slot outlives the model.
        std::vector<DeviceModel>* devices = nullptr;
        std::vector<SavedDevice>* saved   = nullptr;
        const RtChain** published = nullptr;      // null only for a bad id
        Cmd cmd  = Cmd::SetChain;
        i32 addr = 0;                             // Command::a, and the event's
        bool valid() const { return published != nullptr; }
    };
    ChainOwner chainOwner(int owner);
    std::string ownerName(int owner) const;       // for headers and messages
    // Every owner the current session has a model for, tracks first. The order
    // everything that walks all the chains at once uses.
    std::vector<int> modelOwners() const;

    void publishChain(int owner);
    // Named siblings, purely so call sites read as what they do; both are
    // publishChain with the owner id spelled out.
    void publishReturnChain(int idx) { publishChain(ownReturn(idx)); }
    void publishMasterChain()        { publishChain(kOwnMaster); }
    void ensurePluginScan();                      // lazy, first time DEVICES opens
    void addDevice(int owner, const PluginDesc& d);
    void removeDevice(int owner, int idx);

    Session  ses_;
    MainView view_ = MainView::Session;

    // selection + interaction
    int  selTrack_ = 0, selSlot_ = 0;
    bool running_ = true;
    DragState drag_{};
    f32  gridScrollX_ = 0.f;
    f32  browserW_ = 210.f;
    // Tall enough for three rows of device knobs under the tab header.
    f32  detailH_ = 200.f;
    bool showBrowser_ = true;
    bool showDetail_ = true;
    DetailTab detailTab_ = DetailTab::Clip;

    // browser state
    std::string browserDir_;
    std::vector<BrowserEntry> browserItems_;
    std::vector<std::string> browserPlaces_;
    f32  browserScroll_ = 0.f;
    int  browserSel_ = -1;

    // device view state
    std::string pluginFilter_;
    f32  pluginScroll_ = 0.f;
    int  pluginSel_ = -1;
    // What the DEVICES tab edits, in the owner addressing above. Tracks the
    // selection while it is a track; a click on a return or on MASTER parks it
    // there until the next track selection.
    int  devOwner_ = 0;
    int  selDevice_ = -1;              // index into the target chain's devices
    f32  stripScroll_ = 0.f;           // horizontal, device boxes
    f32  paramScroll_ = 0.f;           // vertical, inside the selected device

    // --- computer MIDI keyboard -------------------------------------------
    // Off by default: while it is on the letter keys are notes, so this is a
    // mode the user has to ask for (Ctrl+Shift+K, as in Live) and see.
    bool      kbdMidi_ = false;
    KbdPiano  kbd_;
    // The toggle chord's own down edge. keyPressed[] auto-repeats, and a held
    // Ctrl+Shift+K would otherwise flap the mode on and off.
    bool      kbdTogglePrev_ = false;
    // Latch for the "nothing is armed, so nothing will sound" hint: the state it
    // describes holds for as long as the user leaves it alone, and re-saying it
    // every frame would bury whatever else the status bar has to report.
    bool      kbdNoArmHint_ = false;

    // Live's exclusive arm. Selecting a track arms it and disarms whichever
    // track *this* mechanism armed last; a track the user armed by hand is not
    // ours to disarm, so it is left alone and never claimed here. -1 = we hold
    // no arm at the moment.
    int       autoArmed_ = -1;

    // The piano roll, shown in the CLIP tab for MIDI clips. Created on first
    // use; see the forward declaration above for why it is not a plain member.
    std::unique_ptr<PianoRoll> roll_;

    // --- piano roll note preview -------------------------------------------
    // Editing a note you cannot hear is guesswork, so the roll asks for pitches
    // to audition (PianoRoll::drainPreview) and this turns each into a short
    // note. There is no per-note timer anywhere in the app and no need for one:
    // a preview is a note-on now plus a deadline, and the frame loop — which
    // runs regardless — sends the off once the deadline passes.
    //
    // Where they go: EngineHandle::pushMidi, the same ring the computer keyboard
    // a hardware controller feed, which the engine forwards to note-capable
    // devices on *armed* tracks. That lines up with "the clip on screen" only
    // because selectTrack() auto-arms the selected track (Live's exclusive
    // arm), and every path that changes the selection goes through it. If that
    // ever stops being true, previews start sounding on the wrong instrument.
    //
    // A sounding preview outlives the thing that started it, so the offs are
    // unconditional: they are sent when the deadline passes, when the clip or
    // the panel goes away (updatePreviews checks), and at shutdown.
    struct Preview {
        u8  pitch = 0;
        f64 offAt = 0.0;              // nowSeconds() deadline for the note-off
    };
    static constexpr int kMaxPreviews  = 8;      // a chord's worth; oldest gives way
    static constexpr f64 kPreviewSecs  = 0.12;   // long enough to hear, short enough to edit over
    static constexpr int kPreviewVel   = 100;
    std::vector<Preview> previews_;
    u64  previewClip_ = 0;            // ClipModel::uid the sounding previews belong to

    // --- undo / redo --------------------------------------------------------
    // An undo entry is the whole session, serialized with the project writer.
    // That sounds extravagant and is the cheapest correct thing available: the
    // text format is complete (it is what a saved set is made of) and
    // byte-stable (save -> load -> save is identity, see project.cpp), so "the
    // state before this edit" needs no per-command inverse, no diff machinery,
    // and cannot drift out of sync with what the app can actually represent.
    // A restore is therefore a sibling of a project load and shares its body,
    // adoptSession(), retirement protocol included.
    //
    // The one thing the format does not carry is where the set lives on disk
    // (Session::path is bookkeeping about the last save, not content), and
    // undoing should not move the cursor to the other end of the grid, so the
    // path and the selection ride along beside the text.
    //
    // WHAT IS NOT UNDOABLE, and deliberately so:
    //   * transport -- playing, the playhead, which clips are launched or
    //     queued. Undo is an edit operation; a set that stopped playing because
    //     a note moved would be unusable on stage.
    //   * the record-intent button and per-track arm as such. Arm is session
    //     state and does come back with a restore, but clicking around the grid
    //     (which auto-arms, see selectTrack) never takes an undo point of its
    //     own -- selection is not an edit.
    //   * a take in flight. There is no coherent "half a recording" to restore
    //     to, so an undo during recording cancels the take first: the engine is
    //     told to stop, and the buffer it hands back is thrown away rather than
    //     turned into a clip (PendingRec::cancelled). The buffer itself is
    //     still freed only on the RecordFinished handshake.
    //   * plugin state beyond the parameters a plugin exposes. What is captured
    //     is exactly what a saved set captures -- id/value pairs -- so a
    //     plugin's internal editor state, its samples, its preset name are not
    //     restored. A rebound instance additionally keeps whatever it holds
    //     internally, which is the same trade a saved set makes.
    //   * view state: which panel is open, scroll positions, zoom, the browser,
    //     the plugin filter.
    struct UndoEntry {
        std::string text;             // a complete .lattice document
        std::string what;             // the edit this entry is the state before
        std::string path;             // Session::path, which the format omits
        int selTrack = 0, selSlot = 0;
        // The audio the set was playing, by clip uid. The text names files;
        // this is what makes an undo able to give back a take that has never
        // been one -- and, incidentally, what stops a restore from re-decoding
        // every sample in the set. See ClipSample.
        std::vector<ClipSample> samples;
    };
    // Deep enough to cover a working session, shallow enough that a big set
    // (a few hundred kB of text at the top end) cannot quietly eat a gigabyte.
    static constexpr int kUndoDepth = 128;
    std::vector<UndoEntry> undo_, redo_;
    // The widget id an entry was taken for, so one continuous gesture -- a
    // fader drag, a note dragged across the roll -- produces one entry and not
    // one per frame. 0 means "no gesture in progress"; a one-shot edit (a
    // button, a key) always takes an entry.
    u64  undoGesture_ = 0;
    // Snapshots go through the project writer, which only writes to a path, so
    // they land in a runtime-tmpfs file that is written, read back and removed
    // immediately. Not elegant; correct, and it keeps the *one* serializer.
    std::string undoTmp_;
    // Samples the outgoing session owned and the incoming one does not. The
    // engine may still be running a clip that points into one of them for the
    // few milliseconds it takes to drain our Cmd::SetClip, and there is no
    // event for "the audio thread has let go of this buffer" the way there is
    // for chains and note arrays. One generation of grace -- freed at the next
    // session swap, which is a user action away -- is not a handshake, but it
    // covers the window by many orders of magnitude. See adoptSession.
    std::vector<SampleRef> sampleGrace_;
    // Edge latches for the undo/redo chords: keyPressed[] auto-repeats, and a
    // held Ctrl+Z would run the whole restore path once a frame.
    bool undoKeyPrev_ = false, redoKeyPrev_ = false;

    // Takes an undo entry for the edit that is about to happen. Call it at the
    // START of a gesture and before the model changes. `gesture` names the
    // gesture explicitly for edits that are not driven by a widget (a held,
    // auto-repeating key); the default reads it from whichever widget owns the
    // mouse, which is what a drag is.
    void undoPoint(const char* what, u64 gesture = 0);
    // Same, for the widgets that write into the model and only then report the
    // change (squareToggle, textField, knob/vFader/selector bound straight to a
    // member) and for the piano roll, which edits the clip in place. An entry
    // that already contains the edit undoes nothing, so the caller hands the
    // pre-edit value back for the length of the snapshot.
    template <class T>
    void undoPointWith(const char* what, T& slot, const T& before, u64 gesture = 0) {
        if (undoCoalesce(gesture)) return;
        T now = std::move(slot);
        slot = before;
        pushUndoNow(what);
        slot = std::move(now);
    }
    // True when this frame continues a gesture that already has an entry.
    bool undoCoalesce(u64 gesture);
    void pushUndoNow(const char* what);          // serialize + push, no coalescing
    // The session as project text, plus the audio the text can only name.
    bool snapshotSession(std::string& out, std::vector<ClipSample>& samples);
    void undo();
    void redo();
    bool restoreEntry(const UndoEntry& e);
    void clearUndo();                            // a fresh set has no history
    void cancelTakes(const char* why);           // stop + discard every take in flight
    // Headless verification hook (NXTAKT_DEBUG_UNDO). Nothing can click a
    // fader inside gamescope, and the restore path is the part of this feature
    // a screenshot cannot check -- so it is driven from here instead, against
    // whatever set was loaded, with a live engine and real plugins.
    void debugUndoSelfTest();

    // =======================================================================
    // AUTOMATION: publish / retire / record   (docs/AUTOMATION.md §2.5, §4, §5)
    //
    // One block, appended whole, so it can be moved or merged in one piece.
    // Everything here is GUI thread. The editor half of the feature (the lane
    // view, the chooser, the override affordances) owns no state in here.
    // =======================================================================

    // One published automation set: the single allocation it lives in, the clip
    // it was built for, and the published-lane -> model-lane map.
    //
    // The allocation is a char[] holding a placement-new'd RtAutoSet followed by
    // its RtAutoPoint array, exactly as §2.2 specifies: `points` addresses memory
    // inside the same block, so the whole set is one new[] and one delete[] and
    // the retirement protocol has exactly one pointer to talk about. The reaper
    // in pumpEngineEvents() and the publisher in buildAutos() are the two halves
    // of that bargain and sit next to each other in app_engine.cpp.
    //
    // `modelLane` exists only to answer Ev::AutoLaneInert, which addresses a lane
    // by its index in the *published* array — which is not its index in
    // ClipModel::envelopes, because a lane that resolves to nothing publishes no
    // lane at all.
    struct AutoBlock {
        char* mem = nullptr;
        const RtAutoSet* set = nullptr;
        u64 clipUid = 0;
        std::vector<i32> modelLane;
    };
    // Owns every automation allocation that is still alive: the ones the engine
    // currently holds, and the ones waiting for their Ev::AutosRetired. Freed by
    // this destructor rather than by a line in shutdown() because App is
    // destroyed after shutdown() has joined the audio thread, which is the same
    // moment shutdown() frees the note arrays and for the same reason — and
    // because a destructor cannot be forgotten by a later edit to shutdown().
    struct AutoBlocks {
        std::vector<AutoBlock> v;
        AutoBlocks() = default;
        AutoBlocks(const AutoBlocks&) = delete;
        AutoBlocks& operator=(const AutoBlocks&) = delete;
        ~AutoBlocks() { for (AutoBlock& b : v) delete[] b.mem; }
    };
    AutoBlocks autoBlocks_;
    // Exactly the RtNote protocol (publishedNotes_/retiringNotes_ above): the
    // set the engine holds for a slot, and the ones it has displaced and not yet
    // announced. An entry is freed when its Ev::AutosRetired arrives and never on
    // any other path while the audio thread runs — an envelope can be edited, and
    // recorded into, while its clip is playing.
    const RtAutoSet* publishedAutos_[kMaxTracks][kMaxScenes] = {};
    std::vector<const RtAutoSet*> retiringAutos_;

    // Lanes the engine gave up on (§3.4: the backend has no realtime parameter
    // path). Runtime only — not serialized, not in the undo snapshot, cleared
    // when the session is replaced. Keyed by (clip uid, address) rather than by
    // a flag on AutoLane because session.h is the model the project format
    // writes, and "the engine refused this today" is not something a file says.
    struct InertAuto { u64 clipUid = 0; std::string address; };
    std::vector<InertAuto> inertAutos_;

    // Automation Arm (§5.1). Live's latching control: while it is lit and the
    // transport is playing, a gesture on an automatable control writes into the
    // envelope of the clip playing on that control's track. The control-bar
    // button that toggles it lives in app_chrome.cpp; this is the state it
    // toggles and the gate every capture consults.
    //
    // Not to be confused with autoArmed_ above, which is the exclusive
    // record-arm that follows the selection. Different feature, unlucky names.
    bool autoArm_ = false;
    // Latched "there is nothing playing to record into", exactly like
    // kbdNoArmHint_: the state holds until the user does something about it, and
    // re-saying it every frame would bury everything else.
    bool autoNoClipHint_ = false;

    // The recording pass in flight. One pass = one continuous gesture on one
    // control = one undo entry (§5.4) and one Douglas-Peucker span (§5.2).
    struct AutoRec {
        u64  gesture = 0;             // ui_.active at the start; 0 = no pass
        int  track = -1, slot = -1;
        u64  clipUid = 0;             // the clip the pass is writing into
        std::string address;
        int  lane = -1;               // index into ClipModel::envelopes
        f64  lastBeat = -1.0;         // beat of the last append; <0 = none yet
        f32  lastValue = 0.f;
        int  spanFirst = -1;          // this pass's first point, for the DP pass
        int  spanCount = 0;
        f32  lo = 0.f, hi = 1.f;      // the target's range, which sets the epsilon
        bool active() const { return lane >= 0; }
    };
    AutoRec autoRec_;

    // Publish-time resolution (§4.2). Fills the hot fields of `out` — target,
    // index, devSlot, xform, lo, hi — from the lane's TEXT address. False means
    // the address names nothing on this track *today*: malformed, another
    // track's, a deleted device, a plugin that is not loaded, a parameter id the
    // plugin no longer has, or a field with no AutoTarget yet. The lane is then
    // simply not published; the model keeps its text and the clip still works.
    bool resolveAutoLane(int track, const std::string& address, RtAutoLane& out) const;
    // Builds the one allocation for a slot's envelopes and registers it in
    // autoBlocks_. Null when the clip publishes no lanes at all.
    const RtAutoSet* buildAutos(int track, int slot);
    // Undoes buildAutos when the command that would have carried the set never
    // reached the engine, so nothing ever borrowed it.
    void dropAutos(const RtAutoSet* set);
    // Hands `fresh` (which may be null) to publishedAutos_[track][slot] and moves
    // whatever was there into retiringAutos_. The RtNote protocol verbatim; only
    // called once the engine has accepted the clip carrying `fresh`.
    void publishAutos(int track, int slot, const RtAutoSet* fresh);
    // True while the engine has told us this lane has no realtime path.
    bool autoLaneInert(u64 clipUid, const std::string& address) const;

    void toggleAutoArm();
    // THE CAPTURE API. Called by the widgets that own automatable controls —
    // they live in app_session.cpp and app_devices.cpp, which this half does not
    // own; see the report for the exact call sites. `value` is in the target's
    // own units (§2.3), i.e. whatever the widget just wrote into the model.
    // Everything else — whether a pass is running, which clip is playing, which
    // beat, thinning, punch, the undo point — is decided here.
    void autoCapture(const std::string& address, f32 value, u64 gesture = 0);
    // Ends the pass in flight: runs the Douglas-Peucker simplification over the
    // span it wrote and republishes. Called when the gesture lets go, when the
    // transport stops and when the arm is switched off.
    void autoRecFinish();
    // Drops the pass without touching the model. For the paths where the model
    // it was writing into has already gone (adoptSession).
    void autoRecCancel() { autoRec_ = AutoRec{}; }

    // === automation UI (wave 7d, the EDITOR half) — APPEND-ONLY BLOCK ======
    // The lane view owns almost no App state: the arm flag, the capture entry
    // point, the inert list and the address spellings all live in the publish /
    // record block above, and this half calls into them. What is left is the
    // one thing only the editor needs — the plain view of what a clip may
    // automate, handed to PianoRoll each frame (docs/AUTOMATION.md §6.5) — and
    // the headless hook that stands in for a mouse nothing can drive.
    //
    // Declared against a forward-declared AutoTargets: pianoroll.h includes
    // THIS header, so it cannot be included from here; the definition, in
    // app_detail.cpp, sees the complete type.
    void buildAutoTargets(int track, const ClipModel& clip, AutoTargets& out) const;
    // Headless verification hook (NXTAKT_DEBUG_AUTOLANE), in the shape of the
    // NXTAKT_DEBUG_ADDFX one: nothing can click a selector inside gamescope, so
    // the first CLIP-tab frame seeds the selected clip with an envelope and
    // puts the lane on it. Once per run, and only when the variable is set.
    bool autoDebugSeeded_ = false;
    // === end automation UI block ============================================

    // per-frame UI feedback
    std::string status_;
    f32  peakHoldT_[kMaxTracks]{};
    f32  peakHoldR_[kMaxReturns]{};
    f32  peakHoldM_[2]{};
    f64  lastFrameTime_ = 0.0;
    f32  fps_ = 0.f;

    // =======================================================================
    // REMOTE CONTROL: MIDI-learn + OSC   (src/control/learn.h, src/control/osc.h)
    //
    // One block, appended whole, so it can be moved or merged in one piece.
    // Everything here is GUI thread. The two transports each hand their work
    // across a lock-free ring; nothing off the GUI thread ever touches ses_.
    //
    // WHY THE APPLY PATH GOES THROUGH App AND NOT THROUGH THE ENGINE DIRECTLY:
    // a mapped knob has to behave exactly like a mouse-moved one, which means
    // the model is written, the engine command is sent, an undo entry is taken
    // for the gesture (one, not one per message) and autoCapture is told — in
    // that order and from one place. Sending Cmd::TrackVol straight from the
    // mapping layer would give a fader that moves, an undo that cannot bring it
    // back, an automation arm that records nothing, and a UI showing the old
    // value. See applyControl.
    // =======================================================================
public:
    // The MAPPING LAYER'S entry point, public so it can be driven either by
    // drainControlInput() below (which is what happens today) or straight from
    // a per-frame MIDI handler in a translation unit this half does not own.
    // Returns true when the control layer consumed the message.
    bool routeControlMidi(const MidiMsg& m);
    // Called once per frame from drawControlBar(): drains the reader-thread
    // MIDI tap and the OSC ring, and ages out a control gesture that has gone
    // quiet. Lives on the draw path deliberately — see the report; it is what
    // keeps this feature down to ONE line of wiring in a file it does not own.
    void drainControlInput();
    // The gesture id of the control move in flight, 0 when none. Exposed so
    // that app_engine.cpp's automation-pass tick can recognise a MIDI/OSC
    // gesture as a gesture — it currently only knows about ui_.active, so a
    // pass driven from here is finished and restarted once a frame. Harmless
    // but chatty; see the report for the one-line fix.
    u64  controlGesture() const { return ctlGesture_; }
    // Arms MIDI-learn for an address, or (when it is already armed for that
    // address, or already bound) cancels / clears it. The one gesture the
    // device panel offers, because this codebase has no popup-menu machinery
    // and inventing some for three states would be the larger change.
    void cycleMidiLearn(const std::string& address);
    const ctl::MidiMap& midiMap() const { return midiMap_; }

private:
    // A resolved control: everything applyControl needs, in the TARGET's own
    // units. Kinds this does not list are addresses that parse and resolve to
    // nothing, which PARAM-ADDRESS.md requires to be silently inert.
    struct ControlRef {
        enum class Kind {
            None, TrackVol, TrackPan, TrackSend, TrackMute, TrackSolo, TrackArm,
            DeviceParam, SceneLaunch
        } kind = Kind::None;
        int track = -1;                 // index into ses_.tracks
        int sendIndex = -1;
        int devIndex = -1, paramIndex = -1;
        int scene = -1;
        f32 lo = 0.f, hi = 1.f;         // the target's range, target units
        f32 value = 0.f;                // its value right now, target units
        bool isBool = false;
        const char* label = "control";  // the undo entry's name
    };
    bool resolveControl(const std::string& address, ControlRef& out) const;
    // Writes `value` (TARGET units) the way the widget for that control does.
    // `gesture` identifies the physical control, so a knob sweep coalesces into
    // one undo entry exactly as a drag does. False when the address names
    // nothing today.
    bool applyControl(const std::string& address, f32 value, u64 gesture);
    bool applyControlHit(const ctl::Hit& h);
    void routeControlOsc(const ctl::OscHit& h);
    void ctlEnsureInit();               // lazy: first frame loads the map, starts OSC
    void ctlSaveMap();
    // Headless verification hook (NXTAKT_DEBUG_MIDIMAP=<scratch conf path>), in
    // the shape of debugUndoSelfTest and the NXTAKT_DEBUG_ADDFX hook: nothing
    // can turn a knob on a controller inside gamescope, and the MIDI half of
    // this feature is otherwise unreachable from a screenshot. It drives the
    // REAL path — the reader-thread ring, consume(), applyControlHit — against
    // whatever set is loaded, and writes its map to the path the variable
    // names rather than to the user's own. Once per run.
    void debugMidiMapSelfTest();

    ctl::MidiMap   midiMap_;
    ctl::OscServer osc_;
    std::string    ctlMapPath_;
    bool ctlInit_ = false;
    // False once midimap.conf has failed to parse. A save would then overwrite
    // a file we did not understand with a table we built from the half of it we
    // did — which is how someone loses a mapping they spent an evening on.
    bool ctlMapReadable_ = true;
    // The control gesture in flight and when it was last fed. A MIDI knob has
    // no mouse-up, so a gesture ends by going quiet; kCtlGestureGap is longer
    // than the gap between two messages of one sweep and far shorter than the
    // pause between two deliberate moves.
    u64  ctlGesture_ = 0;
    f64  ctlGestureAt_ = 0.0;
    static constexpr f64 kCtlGestureGap = 0.35;
    u64  ctlApplied_ = 0;               // hits that moved something
    u64  ctlInert_   = 0;               // hits whose address resolved to nothing
    f64  ctlFlashAt_ = 0.0;             // last apply, for the status chip's blink
    // === end remote-control block ==========================================

    // =======================================================================
    // ARRANGEMENT: the two publishers and their reapers  (wave 8d)
    //   docs/ARRANGEMENT.md §3.2, §3.3, §3.6, §3.7, §6.2, §6.6
    //
    // One block, appended whole, so it can be moved or merged in one piece.
    // Everything here is GUI thread, and every definition lives in the one file
    // this milestone owns, src/ui/app_arrange.cpp — publishers and reapers next
    // to each other, exactly as buildAutos and its reaper sit together in
    // app_engine.cpp.
    //
    // Nothing in here is a new lifetime protocol. It is the RtNote protocol for
    // the fifth and sixth time (chains, note arrays, envelopes, warp maps, now
    // lanes and track lanes): the GUI allocates, the engine borrows, a displaced
    // pointer is announced back before it may be freed, and a pointer we have no
    // record of owning is LEAKED rather than freed.
    // =======================================================================
public:
    // --- the item lane (§3.2, §3.3) ----------------------------------------
    // Builds the ONE allocation §3.2 specifies, from ses_.tracks[track].arrange:
    //
    //   [RtArrangement][RtArrItem[itemCount]][RtClip[clipCount]][RtNote[noteCount]]
    //
    // `items`, `clips` and every RtClip::notes inside it address memory in this
    // same block, so the whole lane is one new[] and one delete[] and the
    // retirement protocol has exactly ONE pointer to talk about. Null for a track
    // with no placeable item, which is the ordinary case and is what clears the
    // engine's cell.
    const RtArrangement* buildArrangement(int track);
    // Undoes buildArrangement when the command that would have carried the lane
    // never reached the engine, so nothing ever borrowed it.
    void dropArrangement(const RtArrangement* arr);
    // §3.7 verbatim: build, push, park, retire the displaced pointer. Every edit
    // to a track's arrangement ends here, and nothing else sends
    // Cmd::SetArrangement for a track.
    void publishArrangement(int track);
    // The TRANSPORT CELL (§3.6): Cmd::SetArrangement with a = -1, whose
    // RtArrangement carries no items at all — only loopStart / loopEnd / loopOn,
    // read from ses_. A separate publish path with no item, clip or note tail,
    // because a loop brace is two numbers and a flag and `Command` has one f64.
    void publishTransportCell();

    // --- the automation lane (§6.2, §6.6) ----------------------------------
    // ses_.tracks[track].arrangeAutos, resolved and flattened into its own one
    // allocation:
    //
    //   [RtAutoSetN][RtAutoLane[laneCount]][RtAutoPoint[pointCount]]
    //
    // buildAutos with three differences (§6.6): it reads arrangeAutos, it
    // resolves against the track itself, and the lane array is variable-width
    // and lives inside the block. Address resolution is resolveAutoLane, the
    // SAME function the clip envelopes use — there is deliberately no second
    // resolver, because two would be two things to keep agreeing.
    const RtAutoSetN* buildArrangeAutos(int track);
    void dropArrangeAutos(const RtAutoSetN* set);
    void publishArrangeAutos(int track);

    // Republishes the whole feature for one track — the lane and its automation
    // — which is what every structural edit and every chain republish owes
    // (§6.6's blunt rule). One call so a file this milestone does not own needs
    // one line rather than three.
    void publishArrangementFor(int track);
    // Every track, plus the transport cell. The pushAll() half of the feature;
    // called from a file this milestone does not own (see the report).
    void publishArrangementAll();

    // THE REAPER for both retirement events. Returns true when it consumed `e`,
    // so the one line it needs inside pumpEngineEvents() — a file this milestone
    // does not own — is `if (reapArrangementEvent(e)) continue;`. Refuses to free
    // a pointer it has no record of owning, with the same LOGW and for the same
    // reason as every other reaper here: a bad free is a use-after-free in
    // whoever does own it, which is strictly worse than the leak taken instead.
    bool reapArrangementEvent(const Event& e);

private:
    // The published lane per cell, and the ones displaced and not yet announced.
    // ONE MORE CELL THAN THERE ARE TRACKS: index kMaxTracks is the transport
    // cell, which the wire addresses as a = -1 (engine.h, and deliberately
    // Ev::ChainRetired's own addressing). arrCell() is the only place that
    // mapping is written down.
    //
    // The destructor is the AutoBlocks / WarpMaps argument verbatim: App is
    // destroyed after shutdown() has joined the audio thread, so this is the same
    // moment shutdown() frees the note arrays — and a destructor cannot be
    // forgotten by a later edit to shutdown(), which is a file this milestone
    // does not own. What it frees is what the engine still held when the process
    // ended, plus anything whose retirement event was never drained.
    struct ArrPubs {
        const RtArrangement* published[kMaxTracks + 1] = {};
        std::vector<const RtArrangement*> retiring;
        ArrPubs() = default;
        ArrPubs(const ArrPubs&) = delete;
        ArrPubs& operator=(const ArrPubs&) = delete;
        ~ArrPubs();
    };
    ArrPubs arr_;
    // a = -1 (the transport cell) -> kMaxTracks; a track -> itself; anything
    // else -> -1, which every caller treats as "not a cell we published to".
    static int arrCell(int a) {
        if (a == -1) return kMaxTracks;
        return (a >= 0 && a < kMaxTracks) ? a : -1;
    }

    // The same, for the arrangement's automation. A separate table because it is
    // a separate command, a separate event and a separate allocation; sharing one
    // would mean a retirement that has to guess which kind of pointer it holds.
    struct ArrAutoPubs {
        const RtAutoSetN* published[kMaxTracks] = {};
        std::vector<const RtAutoSetN*> retiring;
        ArrAutoPubs() = default;
        ArrAutoPubs(const ArrAutoPubs&) = delete;
        ArrAutoPubs& operator=(const ArrAutoPubs&) = delete;
        ~ArrAutoPubs();
    };
    ArrAutoPubs arrAutos_;

    // One item's clip envelopes, published as their own RtAutoSet exactly as a
    // session clip's are (§3.2) — NOT folded into the lane block, because
    // dragging one breakpoint would then republish up to 1.6 MB of notes.
    //
    // Keyed by uid and not by index because inserting an item renumbers every
    // index after it. The uid recorded is that of the FIRST item whose payload
    // produced the set: §3.3's dedupe collapses identical payloads into one
    // RtClip, and an RtClip has one `autos` pointer, so a set belongs to a
    // payload rather than to an item. See the note in app_arrange.cpp — this is
    // the one place §3.2's wording and §3.3's dedupe have to be reconciled.
    struct ArrAutoPub { u64 itemUid = 0; const RtAutoSet* set = nullptr; };
    std::vector<ArrAutoPub> publishedArrAutos_[kMaxTracks];
    // The envelope table buildArrangement() has just built and that nothing has
    // adopted yet. It exists so that the two halves of §3.7 stay the two halves
    // §3.7 describes: buildArrangement allocates and publishArrangement decides,
    // and a lane the command ring refused must leave the track's envelope table
    // exactly as it found it. publishArrangement adopts this; dropArrangement
    // discards it.
    std::vector<ArrAutoPub> pendingArrPubs_;
    // Builds one item's envelopes into the shared autoBlocks_ pool, so that
    // dropAutos() and the existing Ev::AutosRetired reaper free them with no
    // change at all. buildAutos's body against an arbitrary ClipModel rather
    // than against a slot — see the report for why the two are not one function.
    const RtAutoSet* buildAutosFor(int track, const ClipModel& m);
    // === end arrangement block =============================================

    // =======================================================================
    // ARRANGEMENT VIEW (wave 8e)   docs/ARRANGEMENT.md §7
    //
    // One block, appended whole. Everything here is GUI thread. The EDITOR owns
    // none of it: ArrangeView is handed a plain ArrangeContext each frame and
    // hands back a mask, exactly as PianoRoll is handed a ClipModel and an
    // AutoTargets — so this is the state the context is built from and the
    // state the mask is applied to, and nothing else.
    // =======================================================================
private:
    // Declared rather than included, for PianoRoll's reason: arrange.h includes
    // pianoroll.h, which includes THIS header, so pulling either in from here
    // would leave the type incomplete for whichever translation unit reached it
    // first. App::App/~App are already out of line in the one .cpp that has the
    // definitions.
    std::unique_ptr<ArrangeView> arrView_;
    // A SECOND PianoRoll, for the detail panel in Arrangement view. Not shared
    // with roll_ because a roll is about one clip: sharing would reset the
    // session clip's zoom, scroll and selection every time the view was
    // switched, which is the state the roll deliberately keeps per clip.
    std::unique_ptr<PianoRoll> arrRoll_;
    // Which tracks have their automation lanes shown. VIEW STATE, so it is not
    // in TrackModel (which 8a froze and which the project format writes) and not
    // in the undo snapshot.
    bool arrExpanded_[kMaxTracks] = {};
    // The selection the context carries back and the detail panel reads: a
    // (track, item uid) pair, uid and not index, because an insert renumbers
    // indices between frames and a stale index is a wrong-clip edit.
    int  arrSelTrack_ = -1;
    u64  arrSelItem_  = 0;
    // ARR arm (§7.7): a THIRD independent chip beside REC and AUTO. "Record
    // into the session grid" and "record onto the timeline" are different
    // destinations, and one button that picked between them from view_ would be
    // the same click doing two things. 8f is what makes it do anything.
    bool arrArm_ = false;
    // detailH_ PER VIEW (answer #10). Two fields against one surprise: the
    // arrangement wants a tall panel for envelope lanes and the session a short
    // one for the grid, and a shared height means every switch between views
    // silently resizes the other. Neither is serialized.
    f32  detailHArr_ = 260.f;
    f32& detailHFor(MainView v) { return v == MainView::Session ? detailH_ : detailHArr_; }

    // Builds the plain view of the arrangement. `targets` is the caller's
    // storage for the per-track AutoTargets the context points into, and may be
    // null for a caller that only needs the lanes (the keyboard verbs).
    void buildArrangeContext(ArrangeContext& ctx, std::vector<AutoTargets>* targets);
    // Applies what the view asked for: the undo point its handshake named, the
    // transport commands its ruler generated, the one-shot verbs its mouse
    // requested, the drop it reported, and a republish of exactly the tracks it
    // says it touched.
    void arrangeCommit(ArrangeContext& ctx, u32 changed);
    // The Autos half of it, which needs a pre-draw copy of what it might edit.
    void arrangeCommitAutos(ArrangeContext& ctx, u32 changed);
    // That copy: the arrangement lanes of the EXPANDED tracks, as they stood at
    // the start of this frame. Only the expanded ones, because only their lanes
    // are on screen to be clicked, and a track's lanes can hold kMaxArrPoints
    // breakpoints.
    std::vector<std::pair<int, std::vector<AutoLane>>> autosBefore_;
    // The CLIP tab in Arrangement view: the selected item's placement, and the
    // roll editing its own `src` in place.
    void drawArrangeClipDetail(const Rect& r);
    // The selected item, or null. The detail panel's whole input.
    ArrangeClip* selectedArrItem();
    // The three one-shot verbs, by index: 0 delete, 1 split, 2 duplicate.
    // arrangeVerb runs one against an existing context and takes the undo entry
    // ONLY if it actually changed something -- a split on an item's own edge is
    // a no-op, and a no-op that costs an undo entry is an undo that appears to
    // do nothing. arrangeKey is the keyboard's wrapper: build a context, run the
    // verb, commit. Split out of handleShortcuts because three copies of that
    // sequence is where the bug lives.
    u32  arrangeVerb(ArrangeContext& ctx, int verb, const char* what);
    bool arrangeKey(int verb, const char* what);
    // The roll, but only while it is on screen for the SELECTED ARRANGEMENT
    // ITEM. visibleRoll()'s sibling, and separate for the same reason arrRoll_
    // is a second roll: the two are about different clips.
    PianoRoll* visibleArrRoll();
    // NXTAKT_DEBUG_ARRANGE=<track>: seeds the scripted figure §7.7 specifies,
    // once per run, on the first Arrangement frame. Nothing inside gamescope can
    // drag a clip, and this is the part a screenshot cannot check on its own.
    void debugSeedArrangement();
    // NXTAKT_DEBUG_ARREDIT=<track>:<verb>: drives ONE arrangement edit through
    // the real verbs and the real undo path, and prints what it did — the hook
    // that says a gesture takes exactly one undo entry and republishes once.
    void debugArrangeEdit();
    bool arrDebugSeeded_ = false;
    bool arrDebugEdited_ = false;
    // === end arrangement view block ========================================

    // =======================================================================
    // ARRANGEMENT RECORDING (wave 8f)   docs/ARRANGEMENT.md §5
    //
    // The consumer half of the record journal. The engine writes every launch,
    // stop and discontinuity it PERFORMS into its own ring, stamped with its own
    // beat; this drains that ring, accumulates the pass, and — on the stop —
    // turns it into ArrangeClips. One block, appended whole; everything here is
    // GUI thread and lives in src/ui/app_arrange.cpp beside the publishers,
    // because committing a take ends in exactly the publish an edit ends in.
    //
    // The rule the whole thing hangs on (§5.4, answer #6): a pass whose journal
    // has a GAP is REFUSED. Not committed short — a recording silently missing
    // four bars is indistinguishable from a performance that had four bars of
    // rest in it, and by the time the user finds out it is the only take.
    // =======================================================================
public:
    // Drains popJournal() dry, EVERY frame and armed or not. Draining
    // unconditionally is not tidiness: the ring is what overflows, and a
    // consumer that only drains while armed would arrive at its first take with
    // the ring already full and every entry of that take refused.
    // One of the two places that still needs the Engine itself rather than a
    // command or a snapshot field: the journal is its own ring, and there is no
    // reason to mirror popJournal() through the handle while the only other
    // caller is the headless hook's private offline engine. In daemon mode
    // local() is null and the journal comes off ipc::JournalRing instead.
    void pumpJournal() { if (Engine* e = eng_.local()) pumpJournal(*e); }
    // ...and the same body against any engine, which is what the headless hook
    // drives: ONE accumulate rule, so the hook cannot verify a path the app does
    // not take. `eng` is the app's own everywhere except there.
    void pumpJournal(Engine& eng);
    // Turns the accumulated pass into items on each track's timeline, or refuses
    // it. ONE undo point, at commit (§5.4): a take in flight is not a state
    // anyone wants to undo to, and an entry per journal entry would exhaust
    // kUndoDepth inside two bars. The argument is the producing engine's
    // journalDropped as of now — read by the caller, because the caller is the
    // one that knows which engine produced the pass.
    void commitTake(u32 droppedNow);
    // The LIVE count, not es_.journalDropped: §5.4 wants the drop count as of
    // the commit, and a value one frame old could commit a take that the last
    // few entries of were lost.
    void commitTake() { commitTake(eng_.journalDropped()); }

private:
    // The pass, as drained. The ring's capacity bounds what one frame delivers
    // and bounds nothing about a pass, so this carries its own ceiling —
    // 24 MB of entries, which is hours of dense playing. Reaching it is counted
    // as a loss and refuses the take, because a consumer that quietly stopped
    // recording is the same failure as a ring that quietly stopped accepting.
    static constexpr size_t kMaxTakeEntries = 1u << 20;
    std::vector<ArrJournal> takeLog_;
    bool takeOpen_ = false;             // a TakeStart has been seen while armed
    // Contiguity, checked over the WHOLE drained stream rather than only over
    // what is accumulated: `seq` is monotonic per engine run, so the cheapest
    // and strictest place to notice a gap is where the entries arrive.
    u32  takeLastSeq_ = 0;
    bool takeSeqValid_ = false;
    u32  takeGaps_ = 0;                 // entries lost since the pass opened
    // Engine::journalDropped as of the pass's opening. §5.4 wants BOTH signals —
    // the seq gap and the counter — because a consumer that has not drained the
    // ring can still read the counter, and because the last entries of a pass
    // can be lost with nothing arriving afterwards to show the jump.
    u32  takeDropBase_ = 0;
    // Set while a take is being committed, so the publish it ends in cannot be
    // mistaken for an edit by anything watching for one.
    bool arrDebugTook_ = false;
    // NXTAKT_DEBUG_ARRTAKE=<track>: scripts a session performance through a
    // private offline Engine, drains its journal, and commits the take through
    // the real path. The whole of §5 without a mouse, a window or an audio
    // device — see the note on the definition for why the offline engine rather
    // than the app's own.
    void debugArrangeTake();
    // === end arrangement recording block ===================================
};

} // namespace lat
