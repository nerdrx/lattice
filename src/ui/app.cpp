#include "app.h"
#include "arrange.h"
#include "pianoroll.h"
#include "../core/project.h"
#include "../gfx/gl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include "app_internal.h"

namespace lat {

// App owns a PianoRoll through a unique_ptr, so the two functions that have to
// see the whole type live here rather than in the header.
App::App()  = default;
App::~App() = default;

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool App::init(int argc, char** argv) {
    if (!win_.create("NxTakt", 1360, 860)) return false;
    if (!rend_.init()) return false;

    const f32 s = win_.dpiScale();
    const std::string reg = findSystemFont(false);
    const std::string bold = findSystemFont(true);
    if (reg.empty()) { LOGE("no usable system font found"); return false; }
    fSmall_.load(reg.c_str(),  (int)std::lround(9.f * s));
    fBody_.load(reg.c_str(),   (int)std::lround(11.f * s));
    fBold_.load(bold.empty() ? reg.c_str() : bold.c_str(), (int)std::lround(11.f * s));
    fBig_.load(bold.empty() ? reg.c_str() : bold.c_str(),  (int)std::lround(15.f * s));

    ui_.r = &rend_;
    ui_.in = &win_.input();
    ui_.fSmall = &fSmall_;
    ui_.fBody = &fBody_;
    ui_.fBold = &fBold_;
    ui_.fBig = &fBig_;

    audio_ = createBackend(engine_, env("AUDIO"));
    if (!audio_) {
        LOGW("no audio backend available - running silent");
        engine_.prepare(48000.0, 1024);
    }

    // MIDI comes up after the audio backend: the reader thread pushes straight
    // into the engine's ring, so the engine must already be prepared. Missing
    // hardware or a missing sequencer device is not an error - a set can be
    // played entirely from the mouse.
    if (midi_.start(engine_)) LOGI("midi in: alsa seq client %d:0", midi_.clientId());
    else                      LOGW("no MIDI input - continuing without it");

    // Default set: eight audio tracks, eight scenes, same as a fresh Live set.
    ses_.tracks.resize(8);
    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        char buf[32];
        snprintf(buf, sizeof buf, "%zu Audio", i + 1);
        ses_.tracks[i].uid = ses_.newUid();
        ses_.tracks[i].name = buf;
        ses_.tracks[i].colorIdx = (int)(i * 3 + 4) % pal::clipColorCount;
    }
    ses_.scenes.resize(8);
    for (size_t i = 0; i < ses_.scenes.size(); ++i) {
        char buf[32];
        snprintf(buf, sizeof buf, "Scene %zu", i + 1);
        ses_.scenes[i].uid = ses_.newUid();
        ses_.scenes[i].name = buf;
    }

    browserPlaces_ = {homeDir() + "/Music", homeDir() + "/Downloads", homeDir(), "/usr/share/sounds"};
    browseTo(browserPlaces_[0]);
    if (browserItems_.empty()) browseTo(homeDir());

    // A project path on the command line loads instead of the default set.
    // openProject() pushes the whole restored set to the engine itself, so only
    // the default-set path needs the initial sync here.
    if (argc > 1) {
        if (!openProject(argv[1])) LOGW("could not load %s: %s", argv[1], status_.c_str());
    } else {
        pushAll();
        // The arrangement's half of the initial sync. openProject reaches it
        // through adoptSession; the default set has no adoptSession to run, so
        // the transport cell -- which carries the loop brace -- would otherwise
        // never be published at all.
        publishArrangementAll();
    }
    status_ = "Ready";

    // Headless verification hook. With NXTAKT_DEBUG_ADDFX=<substring> set, the
    // first scanned plugin whose name matches is loaded onto track 0 and the
    // DEVICES tab is opened, so tools/headless_test.sh can screenshot a
    // populated device chain without anything driving the mouse.
    if (const char* want = env("DEBUG_ADDFX")) {
        ensurePluginScan();
        const PluginDesc* hit = nullptr;
        for (const PluginDesc& d : registry_.plugins())
            if (icontains(d.name, want)) { hit = &d; break; }
        if (!hit) {
            LOGW("NXTAKT_DEBUG_ADDFX: no plugin matching \"%s\"", want);
        } else if (!ses_.tracks.empty()) {
            selTrack_ = 0;
            devOwner_ = 0;
            addDevice(0, *hit);
            selDevice_ = (int)ses_.tracks[0].devices.size() - 1;
            detailTab_ = DetailTab::Devices;
            showDetail_ = true;
        }
    }

    // The same hook for the master chain -- a saturator or a bus compressor
    // across the whole mix, which is what a master chain is for. It also parks
    // the DEVICES tab on the master, so a screenshot shows the one part of the
    // chain-owner selection nothing inside gamescope can click on.
    if (const char* want = env("DEBUG_MASTERFX")) {
        ensurePluginScan();
        const PluginDesc* hit = nullptr;
        for (const PluginDesc& d : registry_.plugins())
            if (icontains(d.name, want)) { hit = &d; break; }
        if (!hit) {
            LOGW("NXTAKT_DEBUG_MASTERFX: no plugin matching \"%s\"", want);
        } else {
            selectChainOwner(kOwnMaster);
            addDevice(kOwnMaster, *hit);
            showDetail_ = true;
        }
    }

    // The arrangement's two hooks (docs/ARRANGEMENT.md §7.7). Here rather than
    // on the first Arrangement frame as §7.7 words it, because the seed is what
    // SWITCHES to that view: a hook that waits for the view it turns on would
    // never run. Once per run either way, and the guards inside them say so.
    if (!arrView_) arrView_ = std::make_unique<ArrangeView>();
    debugSeedArrangement();
    debugArrangeEdit();

    // The other headless hook: undo cannot be clicked inside gamescope, so
    // NXTAKT_DEBUG_UNDO drives the restore path here instead. See
    // debugUndoSelfTest, and note that it puts the set back as it found it.
    if (env("DEBUG_UNDO")) debugUndoSelfTest();

    LOGI("backend: %s   audio: %s", win_.backendName(), audio_ ? audio_->name() : "none");
    return true;
}

void App::shutdown() {
    // Anything the UI left sounding is ended while there is still an engine to
    // hear it: previews and held keyboard notes both outlive the state that
    // started them, and a plugin does not know the app is closing.
    stopPreviews();
    kbd_.allNotesOff([this](const MidiMsg& m) { engine_.pushMidiFromGui(m); });

    // Order matters. The MIDI reader goes first: it pushes into the engine's
    // ring from its own thread, so it has to be joined before anything else
    // starts tearing the engine down, or a push could land in a ring nobody
    // owns any more.
    midi_.stop();

    // Stopping the backend joins the audio thread, so once it returns nothing
    // can be inside process() and nothing can be following a published chain
    // or writing into a capture buffer. Only then is it safe to free either
    // without the Ev::ChainRetired / Ev::RecordFinished handshake — the events
    // still sitting in the ring will never be drained, so waiting for them here
    // would deadlock or leak.
    if (audio_) { audio_->stop(); audio_.reset(); }
    for (const RtChain*& c : published_) { delete c; c = nullptr; }
    for (const RtChain*& c : publishedReturn_) { delete c; c = nullptr; }
    delete publishedMaster_; publishedMaster_ = nullptr;
    for (RetiredChain& rc : retiring_) delete rc.chain;
    retiring_.clear();          // frees the instances the chains had dropped
    for (auto& row : publishedNotes_)
        for (const RtNote*& n : row) { delete[] n; n = nullptr; }
    for (const RtNote* n : retiringNotes_) delete[] n;
    retiringNotes_.clear();
    for (PendingRec& p : pendingRecs_) { delete[] p.buf; delete[] p.notes; }
    pendingRecs_.clear();
    // Instances still on tracks die with ses_ when App is destroyed, which is
    // after this point and therefore also after the audio thread is gone.

    fSmall_.destroy(); fBody_.destroy(); fBold_.destroy(); fBig_.destroy();
    rend_.shutdown();
    win_.destroy();
}

void App::run() {
    lastFrameTime_ = nowSeconds();
    while (running_ && win_.pump()) {
        frame();
        win_.swap();
    }
}


// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

void App::frame() {
    const f64 t = nowSeconds();
    const f32 dt = (f32)(t - lastFrameTime_);
    lastFrameTime_ = t;
    fps_ = fps_ * 0.92f + (dt > 0.f ? 1.f / dt : 0.f) * 0.08f;

    pumpEngineEvents();
    // Note previews are ended by the frame loop, not by a timer: whatever else
    // this frame does, an audition started a moment ago has to be allowed to
    // stop. Ahead of the UI so a preview retired here can be restarted below.
    updatePreviews();

    const f32 s = win_.dpiScale();
    const f32 W = (f32)win_.width(), H = (f32)win_.height();

    rend_.begin(win_.width(), win_.height(), s);
    glClearColor(pal::appBg.r, pal::appBg.g, pal::appBg.b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ui_.beginFrame();

    // A gesture ends when what was driving it lets go, and the next one has to
    // take an entry of its own even when it is the same fader being dragged a
    // second time. Ui::endFrame() drops `active` on mouse-up, so by now it is
    // already gone; the arrows are the one gesture not held by a widget.
    {
        const Input& k = win_.input();
        const bool arrows = k.keyDown[KeyLeft] || k.keyDown[KeyRight] ||
                            k.keyDown[KeyUp]   || k.keyDown[KeyDown];
        if (!ui_.active && !(arrows && undoGesture_ == kArrowGesture)) undoGesture_ = 0;
    }

    handleShortcuts();

    Rect full{0, 0, W, H};
    Rect bar = {0, 0, W, lay::controlBarH * s};
    Rect status = {0, H - lay::statusH * s, W, lay::statusH * s};
    Rect body = {0, bar.bottom(), W, status.y - bar.bottom()};

    drawControlBar(bar);

    // UN-GATED (docs/ARRANGEMENT.md §7.6, answer #10). In Arrangement view the
    // CLIP tab shows the selected item's own `src` and edits it IN PLACE, which
    // is Rule 1 paying for itself: because `src` is by value, the roll editing
    // "the clip" edits precisely the one item the user selected, with no
    // possibility of the edit leaking to another placement and no code in the
    // roll that knows an arrangement exists.
    //
    // The height is PER VIEW and not shared: the arrangement wants a tall panel
    // for envelope lanes and the session a short one for the grid, and one
    // field would mean every switch between views silently resized the other.
    Rect detail{};
    const f32 dH = detailHFor(view_);
    if (showDetail_) {
        detail = {0, body.bottom() - dH * s, W, dH * s};
        body.h -= detail.h;
    }

    Rect main = body;
    if (showBrowser_) {
        Rect br = {0, body.y, browserW_ * s, body.h};
        drawBrowser(br);
        main = {br.right(), body.y, W - br.right(), body.h};
    }

    if (view_ == MainView::Session) drawSessionView(main);
    else                            drawArrangementView(main);

    if (showDetail_) drawDetailPanel(detail);
    drawStatusBar(status);
    drawDragGhost();

    ui_.endFrame();
    win_.setCursor(ui_.cursor);
    rend_.end();
    (void)full;
}

void App::handleShortcuts() {
    Input& in = win_.input();

    // Ahead of the edit guard on purpose: when a text field takes focus while a
    // piano key is still held, this call is what releases the note.
    updateKbdPiano();

    if (ui_.editId) return;                      // typing takes precedence

    // Live's Computer MIDI Keyboard toggle. Edge-detected on keyDown[] rather
    // than keyPressed[], which repeats.
    const bool tgl = in.keyDown['k'] && in.ctrl() && in.shift();
    if (tgl && !kbdTogglePrev_) toggleKbdMidi();
    kbdTogglePrev_ = tgl;

    // Undo / redo, edge-detected for the same reason: a held Ctrl+Z would run
    // a full session restore every frame. Ctrl+Shift+Z and Ctrl+Y both redo,
    // which is the split the rest of the world never settled.
    const bool undoChord = in.keyDown['z'] && in.ctrl() && !in.shift();
    const bool redoChord = (in.keyDown['z'] && in.ctrl() && in.shift()) ||
                           (in.keyDown['y'] && in.ctrl());
    if (undoChord && !undoKeyPrev_) undo();
    if (redoChord && !redoKeyPrev_) redo();
    undoKeyPrev_ = undoChord;
    redoKeyPrev_ = redoChord;

    // While the piano is on it owns the printable keys, so an unmodified letter
    // is a note and not a shortcut — see KbdPiano::consumes for why this is now
    // the whole block rather than the mapped keys: the piano reads positions
    // and shortcuts read keysyms, and on a non-US layout the two disagree.
    // Ctrl- and Alt-modified chords are unaffected: notes only fire unmodified.
    const auto plain = [&](int k) {
        return in.keyPressed[k] && !in.ctrl() && !(kbdMidi_ && KbdPiano::consumes(k));
    };

    if (in.keyPressed[' ']) togglePlay();
    if (in.keyPressed[KeyTab])
        view_ = (view_ == MainView::Session) ? MainView::Arrangement : MainView::Session;
    if (in.keyPressed['b'] && in.ctrl()) showBrowser_ = !showBrowser_;
    if (in.keyPressed['d'] && in.ctrl()) showDetail_ = !showDetail_;
    if (plain('m')) {
        undoPoint("metronome");
        ses_.metronome = !ses_.metronome;
        send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    }
    if (in.keyPressed['t'] && in.ctrl()) { undoPoint("add track"); addTrack(); }
    if (in.keyPressed[KeyEnter] && in.ctrl()) { undoPoint("add scene"); addScene(); }

    // --- keys the ARRANGEMENT can claim -------------------------------------
    // Layered exactly as the roll's keys are, and ahead of them because in this
    // view the same keys mean something else: Delete removes the item under the
    // selection and not the session slot behind the grid nobody is looking at.
    // Everything the arrangement does not claim falls through untouched.
    if (view_ == MainView::Arrangement) {
        // The roll first, when it is showing the selected item's own clip: a
        // note selection inside an item outranks the item, for the reason a note
        // selection outranks the clip in Session view -- clearing the container
        // from under an active edit is a spectacular way to lose work.
        PianoRoll* const ar = visibleArrRoll();
        ArrangeClip* const item = ar ? selectedArrItem() : nullptr;
        const bool aNote = ar && item && ar->hasSelection(item->src);

        if (in.keyPressed[KeyEscape]) {
            if (aNote) ar->clearSelection();
            else       send(Cmd::StopAll);
        }
        if (in.keyPressed[KeyDelete] || (in.keyPressed[KeyBackspace] && !in.ctrl())) {
            if (aNote) {
                const ClipModel before = item->src;
                if (ar->deleteSelected(item->src)) {
                    undoPointWith("delete note", item->src, before);
                    publishArrangementFor(arrSelTrack_);
                }
            } else {
                arrangeKey(0, "delete clip");
            }
        }
        // Ctrl+E splits at the cursor -- Live's key, and free here. Ctrl+U
        // duplicates, which is the roll's own duplicate key applied to the thing
        // this view is about; inside an item's notes it keeps meaning the loop.
        if (in.keyPressed['e'] && in.ctrl()) arrangeKey(1, "split clip");
        if (in.keyPressed['u'] && in.ctrl()) {
            if (ar && item) {
                const ClipModel before = item->src;
                if (ar->duplicateLoop(item->src)) {
                    undoPointWith("duplicate loop", item->src, before);
                    publishArrangementFor(arrSelTrack_);
                }
            } else {
                arrangeKey(2, "duplicate clip");
            }
        }
        if (aNote) {
            int steps = 0, semis = 0;
            if (in.keyPressed[KeyLeft])  --steps;
            if (in.keyPressed[KeyRight]) ++steps;
            const int step = in.shift() ? 12 : 1;
            if (in.keyPressed[KeyUp])    semis += step;
            if (in.keyPressed[KeyDown])  semis -= step;
            if (steps || semis) {
                const ClipModel before = item->src;
                if (ar->nudgeSelected(item->src, steps, semis)) {
                    undoPointWith("nudge note", item->src, before, kArrowGesture);
                    publishArrangementFor(arrSelTrack_);
                }
            }
        }
        // Home locates to zero (answer #4 names it beside stop-does-not-rewind).
        if (in.keyPressed[KeyHome]) send(Cmd::Locate, 0, 0, 0.0);

        if (in.keyPressed['s'] && in.ctrl()) {
            const std::string p = ses_.path.empty()
                                      ? (homeDir() + "/" + ses_.name + ".lattice")
                                      : ses_.path;
            saveProjectTo(p);
        }
        return;
    }

    // --- keys the piano roll can claim --------------------------------------
    // The roll only claims a key while it is on screen for the selected clip,
    // and the note-scoped keys only while a note is selected in it. Everything
    // else keeps its session-wide meaning, so the editor never steals a key it
    // has no use for. The selection is read once, before anything below can
    // change it, and the clip is only reached through the roll — visibleRoll()
    // has already bounds-checked the indices it would be read with.
    PianoRoll* const roll = visibleRoll();
    ClipModel* const selClip = roll ? &ses_.tracks[selTrack_].slots[selSlot_] : nullptr;
    const bool noteSel = roll && roll->hasSelection(*selClip);

    // Escape is layered rather than overridden: with a note selected it drops
    // that selection (the editor's own scope) and nothing else; pressing it
    // again — or with nothing selected — reaches the global stop, which is
    // what it has always done and what a panicking user expects of it.
    if (in.keyPressed[KeyEscape]) {
        if (noteSel) roll->clearSelection();
        else         send(Cmd::StopAll);
    }
    // Delete with a note selected removes the note, not the clip that contains
    // it. Clearing the whole pattern from under an active note edit would be a
    // spectacular way to lose work.
    if (in.keyPressed[KeyDelete] || (in.keyPressed[KeyBackspace] && !in.ctrl())) {
        if (noteSel) {
            // The roll edits the clip in place, so the entry has to be taken
            // with the clip as it was -- and only if the edit happened at all,
            // which is not knowable until the call returns. Copying a clip is
            // a note vector and two strings; see undoPointWith.
            const ClipModel before = *selClip;
            if (roll->deleteSelected(*selClip)) {
                undoPointWith("delete note", *selClip, before);
                pushClip(selTrack_, selSlot_);
            }
        } else {
            undoPoint("clear clip");
            clearClip(selTrack_, selSlot_);
        }
    }
    // Live's duplicate-loop (Cmd+D there; Ctrl+D is already the detail panel
    // here, so Ctrl+U). Clip-scoped, not note-scoped: no selection needed.
    if (in.keyPressed['u'] && in.ctrl() && roll) {
        const ClipModel before = *selClip;
        if (roll->duplicateLoop(*selClip)) {
            undoPointWith("duplicate loop", *selClip, before);
            pushClip(selTrack_, selSlot_);
            char buf[64];
            snprintf(buf, sizeof buf, "Loop duplicated — %.0f beats", selClip->lengthBeats);
            status_ = buf;
        }
    }

    const int nt = (int)ses_.tracks.size(), ns = (int)ses_.scenes.size();
    if (noteSel) {
        // Arrows nudge the note and do NOT move the clip selection: with an
        // editor open on a note, "left" means that note, and having the panel
        // switch to another clip mid-edit is the trap this avoids.
        int steps = 0, semis = 0;
        if (in.keyPressed[KeyLeft])  --steps;
        if (in.keyPressed[KeyRight]) ++steps;
        const int step = in.shift() ? 12 : 1;      // Shift = octave, as everywhere
        if (in.keyPressed[KeyUp])    semis += step;
        if (in.keyPressed[KeyDown])  semis -= step;
        if (steps || semis) {
            const ClipModel before = *selClip;
            if (roll->nudgeSelected(*selClip, steps, semis)) {
                // The arrows auto-repeat, so sliding a note across two beats is
                // one gesture and not thirty entries. It ends when the key
                // comes up (see frame()).
                undoPointWith("nudge note", *selClip, before, kArrowGesture);
                pushClip(selTrack_, selSlot_);
            }
        }
    } else {
        // Through selectTrack so arrowing across the grid arms what it lands
        // on, exactly as clicking does.
        if (in.keyPressed[KeyLeft])  selectTrack(clampv(selTrack_ - 1, 0, nt - 1));
        if (in.keyPressed[KeyRight]) selectTrack(clampv(selTrack_ + 1, 0, nt - 1));
        if (in.keyPressed[KeyUp])    selSlot_  = clampv(selSlot_ - 1, 0, ns - 1);
        if (in.keyPressed[KeyDown])  selSlot_  = clampv(selSlot_ + 1, 0, ns - 1);
    }
    if (in.keyPressed[KeyEnter] && !in.ctrl()) {
        if (ses_.tracks[selTrack_].slots[selSlot_].valid())
            send(Cmd::LaunchClip, selTrack_, selSlot_);
    }

    if (in.keyPressed['s'] && in.ctrl()) {
        const std::string p = ses_.path.empty() ? (homeDir() + "/" + ses_.name + ".lattice") : ses_.path;
        saveProjectTo(p);
    }
}

// The QWERTY piano, run once per frame. Everything hard about it lives in
// KbdPiano; this only decides whether the gate is open and where the notes go.
void App::updateKbdPiano() {
    Input& in = win_.input();
    // A focused text field must type, and a modified chord must stay a command
    // (Ctrl+S saves; it does not play a G). Closing the gate mid-hold releases
    // whatever is sounding, and reopening it never retriggers a still-held key.
    const bool live = kbdMidi_ && !ui_.editId &&
                      !in.ctrl() && !in.alt() && !(in.mods & ModSuper);

    // scanDown[] rather than keyDown[]: the piano is a set of key *positions*
    // (KbdPiano::semiFor), so it plays the same on QWERTZ and AZERTY as on
    // QWERTY. The octave keys are passed separately because they are labelled
    // keys and follow the layout like any other named shortcut.
    const KbdPiano::Result res = kbd_.update(in.scanDown, in.keyDown[KeyPageUp],
        in.keyDown[KeyPageDown], live,
        [this](const MidiMsg& m) { engine_.pushMidiFromGui(m); });

    if (res.baseChanged) {
        char buf[96];
        snprintf(buf, sizeof buf, "Keyboard octave C%d · velocity %d", kbd_.octave(), kbd_.velocity());
        status_ = buf;
    }

    // The commonest way to conclude the keyboard is broken is to switch it on
    // with nothing armed: the notes reach the engine and go nowhere, silently.
    // Said once when the condition arrives, not once a frame.
    bool anyArm = false;
    for (const TrackModel& t : ses_.tracks) if (t.arm) { anyArm = true; break; }
    const bool hint = kbdMidi_ && !anyArm;
    if (hint != kbdNoArmHint_) {
        kbdNoArmHint_ = hint;
        if (hint) status_ = "Arm a track to hear the keyboard (auto-arm: click a track)";
    }
}

void App::toggleKbdMidi() {
    kbdMidi_ = !kbdMidi_;
    if (kbdMidi_) {
        char buf[192];
        snprintf(buf, sizeof buf,
                 "Computer MIDI Keyboard on — ZXCVBNM lower octave (C%d), QWERTYU / IOP above, "
                 "SDGHJ + 23567 90 black, PgUp/PgDn octave, Ctrl+Shift+K off",
                 kbd_.octave());
        status_ = buf;
    } else {
        // Anything still held has to be let go here: the key release that would
        // normally end the note is about to be ignored, and a hung note would
        // sit in the instrument with nothing left to stop it.
        kbd_.allNotesOff([this](const MidiMsg& m) { engine_.pushMidiFromGui(m); });
        status_ = "Computer MIDI Keyboard off";
    }
}


// ---------------------------------------------------------------------------
// piano roll: key routing and note preview
// ---------------------------------------------------------------------------

// On screen and showing the selected clip's notes — the only state in which the
// roll may claim a key or hold a meaningful selection. Note that it also
// answers "was the roll ever drawn", since roll_ is created by drawClipDetail.
PianoRoll* App::visibleRoll() {
    if (!roll_ || view_ != MainView::Session || !showDetail_ || detailTab_ != DetailTab::Clip)
        return nullptr;
    if (selTrack_ < 0 || selTrack_ >= (int)ses_.tracks.size()) return nullptr;
    if (selSlot_ < 0 || selSlot_ >= kMaxScenes) return nullptr;
    const ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
    // Audio clips reach the roll too, since it now hosts their envelope lane:
    // the keyboard verbs (Delete, arrows, Escape) route to whichever selection
    // the roll holds, and refusing here would have left an audio clip's
    // breakpoints mouse-editable but not keyboard-editable.
    return m.valid() ? roll_.get() : nullptr;
}

// visibleRoll's sibling. The roll only claims a key while it is on screen for
// the item the arrangement has selected -- and it is a DIFFERENT roll, because
// a roll's zoom, scroll and selection are about one particular clip and sharing
// one would reset the session clip's every time the view was switched.
PianoRoll* App::visibleArrRoll() {
    if (!arrRoll_ || view_ != MainView::Arrangement || !showDetail_ ||
        detailTab_ != DetailTab::Clip)
        return nullptr;
    const ArrangeClip* it = selectedArrItem();
    return (it && it->src.valid()) ? arrRoll_.get() : nullptr;
}

void App::startPreview(int pitch, u64 clipUid) {
    if (pitch < 0 || pitch > 127) return;
    // Previews belong to one clip at a time: the moment the panel shows a
    // different one, the old clip's notes are stopped rather than left ringing
    // under the new one (updatePreviews does the checking).
    if (clipUid != previewClip_) {
        stopPreviews();
        previewClip_ = clipUid;
    }
    const f64 off = nowSeconds() + kPreviewSecs;
    // Same pitch again: retrigger rather than stack, so a repeated nudge is
    // audible as repeated notes and never leaves two offs chasing one on.
    for (Preview& p : previews_) {
        if (p.pitch != (u8)pitch) continue;
        engine_.pushMidiFromGui(MidiMsg{0x80, p.pitch, 0, 0, 0});
        engine_.pushMidiFromGui(MidiMsg{0x90, p.pitch, (u8)kPreviewVel, 0, 0});
        p.offAt = off;
        return;
    }
    // Full: the oldest audition gives way. A dropped preview would be a note
    // that never sounds; a hung one would be a note that never stops.
    if ((int)previews_.size() >= kMaxPreviews) {
        engine_.pushMidiFromGui(MidiMsg{0x80, previews_.front().pitch, 0, 0, 0});
        previews_.erase(previews_.begin());
    }
    engine_.pushMidiFromGui(MidiMsg{0x90, (u8)pitch, (u8)kPreviewVel, 0, 0});
    previews_.push_back(Preview{(u8)pitch, off});
}

void App::updatePreviews() {
    if (previews_.empty()) return;
    // Context check first. A preview outlives whatever started it, and both the
    // clip and the panel can vanish between frames (another slot selected,
    // Ctrl+D, the DEVICES tab, Arrangement). Nothing downstream will ever end
    // these notes if this does not.
    // WHICH roll is on screen, and which clip it is showing. Two of them now
    // (§7.6 un-gated the panel), and the check has to cover both: with only the
    // session roll consulted, every audition started in Arrangement view was
    // ended on the very next frame -- a note-on and a note-off with nothing
    // audible in between.
    const PianoRoll* live = visibleRoll();
    u64 shown = 0;
    if (live) {
        shown = ses_.tracks[selTrack_].slots[selSlot_].uid;
    } else if (const PianoRoll* ar = visibleArrRoll()) {
        live = ar;
        const ArrangeClip* it = selectedArrItem();
        shown = it ? it->src.uid : 0;
    }
    if (!live || shown != previewClip_) {
        stopPreviews();
        return;
    }
    const f64 now = nowSeconds();
    for (size_t i = 0; i < previews_.size();) {
        if (previews_[i].offAt <= now) {
            engine_.pushMidiFromGui(MidiMsg{0x80, previews_[i].pitch, 0, 0, 0});
            previews_.erase(previews_.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

void App::stopPreviews() {
    for (const Preview& p : previews_) engine_.pushMidiFromGui(MidiMsg{0x80, p.pitch, 0, 0, 0});
    previews_.clear();
    previewClip_ = 0;
}


} // namespace lat
