// Session model (GUI-side truth) and the application shell.
//
// The model here is the editable, allocating, std::string-carrying version.
// Whenever something changes that the audio thread needs, it is pushed across
// as a Command; the engine keeps its own realtime-safe mirror.
#pragma once
#include "../audio/engine.h"
#include "../audio/sample.h"
#include "../audio/backend.h"
#include "../audio/midi_in.h"
#include "../plugin/host.h"
#include "../gfx/renderer.h"
#include "widgets.h"
#include "window.h"
#include <memory>
#include <string>
#include <vector>

namespace lat {

enum class ClipKind { Audio, Midi };

// One note in a MIDI clip, clip-relative beats. The GUI edits these freely;
// pushClip snapshots them into a heap RtNote array for the engine, and the
// old array comes back via Ev::NotesRetired before it is freed.
struct NoteModel {
    f64 beat = 0.0;
    f64 len  = 0.25;
    u8  pitch = 60, vel = 100;
};

struct ClipModel {
    // Stable identity. UIDs never change once assigned and are serialized, so
    // undo, set-diff, automation targets and collaboration can reference an
    // entity across moves and renames. 0 = unassigned (Session::newUid()).
    u64 uid = 0;
    ClipKind kind = ClipKind::Audio;
    std::vector<NoteModel> notes;      // Midi clips only; kept sorted by beat
    SampleRef sample;
    std::string name;
    // Source file. Authoritative for save/load: it survives a missing sample,
    // so a set whose media is offline does not silently lose the reference.
    std::string path;
    int  colorIdx = 0;
    f32  gain = 1.0f;
    Warp warp = Warp::Beats;
    bool loop = true;
    f64  clipBpm = 120.0;
    f64  lengthBeats = 4.0;
    i64  loopStart = 0, loopEnd = 0;
    int  quantumIdx = -1;              // -1 => follow the global quantum
    // Generative scheduling, mirrored into RtClip (see engine.h Follow).
    f64  prob = 1.0;
    Follow followAction = Follow::None;
    f64  followBeats = 0.0;            // 0 => the clip's own length
    // A MIDI clip is valid even while empty — an empty pattern is editable
    // and launchable (it plays silence at its loop length, exactly like Live).
    bool valid() const { return kind == ClipKind::Midi ? true : sample != nullptr; }
};

// One loaded plugin on a track. The instance is GUI-owned; the audio thread
// borrows it through the RtChain currently published to the engine, so it may
// only be destroyed after that chain has come back via Ev::ChainRetired.
struct DeviceModel {
    u64 uid = 0;
    PluginDesc desc;
    std::unique_ptr<PluginInstance> inst;
    bool bypass = false;
    // Set only when `inst` is null: the plugin named by desc.uri was not on
    // this machine when the set loaded. The device keeps its slot in the chain
    // (silent, skipped by publishChain) and carries its saved parameters here
    // so that saving the set again does not throw them away.
    std::vector<std::pair<u32, f32>> lostParams;
};

// A device as it sits in a saved set: no instance, just what's needed to
// rebuild one. The project layer reads/writes ONLY this passive form; the App
// materializes savedDevices -> devices (instantiate, apply params, publish)
// after load and serializes devices -> savedDevices before save. That keeps
// plugin instantiation out of src/core entirely.
struct SavedDevice {
    u64 uid = 0;
    std::string uri;                   // PluginDesc::uri
    std::string name;                  // display fallback if the plugin is gone
    bool bypass = false;
    std::vector<std::pair<u32, f32>> params;   // (ParamInfo::id, value)
};

struct TrackModel {
    u64 uid = 0;
    std::string name = "Track";
    int   colorIdx = 0;
    ClipModel slots[kMaxScenes];
    std::vector<DeviceModel> devices;   // makes TrackModel move-only
    std::vector<SavedDevice> savedDevices;
    f32   fader = 0.85f;               // 0..1, mapped through faderToGain
    f32   pan   = 0.f;                 // -1..1
    bool  mute = false, solo = false, arm = false;
    f32   width = 94.f;
};

struct SceneModel {
    u64 uid = 0;
    std::string name;
    f64 tempo = 0.0;                   // 0 => no tempo change on launch
};

struct Session {
    std::vector<TrackModel> tracks;
    std::vector<SceneModel> scenes;
    // Monotonic UID source for every entity in this set. Serialized, so IDs
    // stay unique across save/load. Assign at creation; never reuse.
    u64 nextUid = 1;
    u64 newUid() { return nextUid++; }
    f64  tempo = 120.0;
    int  sigNum = 4, sigDen = 4;
    int  quantumIdx = 4;               // index into kQuantumBeats -> "1 Bar"
    bool metronome = false;
    std::string name = "Untitled";
    std::string path;                  // last saved location, empty if never
};

enum class MainView { Session, Arrangement };

// The bottom panel shows one of two things at a time, like Live's Clip / Device
// view toggle. Ctrl+D still hides the whole panel.
enum class DetailTab { Clip, Devices };

struct BrowserEntry {
    std::string name, path;
    bool isDir = false;
    bool isAudio = false;
};

// A drag in flight, either from the browser or between clip slots.
struct DragState {
    enum class Kind { None, BrowserFile, Clip } kind = Kind::None;
    std::string path;                  // BrowserFile
    int srcTrack = -1, srcSlot = -1;   // Clip
    f32 startX = 0, startY = 0;
    bool armed = false;                // past the movement threshold
};

// Live's "Computer MIDI Keyboard": the letter rows become a piano feeding the
// same MIDI ring a hardware controller uses.
//
// Deliberately knows nothing about App, the window or the engine: update() is
// handed a raw physical-key snapshot and emits through a callback. That keeps
// the part with the actual subtlety in it — edge detection and note ownership —
// testable without a GUI, an audio device or a keyboard to press.
//
// The subtlety, part one: Input::keyPressed[] includes auto-repeat, so a held
// key would machine-gun note-ons. Notes therefore come from *edges* of held
// state, and each key remembers the note it started so a note-off is always the
// note that sounded, even if the octave moved while the key was down.
//
// Part two, and the reason this maps Input::scanDown[] rather than keyDown[]:
// a piano layout is POSITIONAL. The keys under the fingers form two and a bit
// octaves whatever the locale prints on them; on the reporter's German QWERTZ
// board the keysym map put the bottom row's C on 'y' and its A on 'z', which
// is not a keyboard anyone can play. Scancodes are evdev's (KEY_Z = 44), so
// the bottom row is the bottom row everywhere. Shortcuts keep using keysyms —
// Ctrl+S should be the key labelled S — which is why the two are now
// unrelated, and why consumes() had to change with them.
struct KbdPiano {
    static constexpr int kScanCount   = 256;   // Input::scanDown[]
    static constexpr int kHighestSemi = 28;    // top row's last key, the top of the range
    static constexpr int kDefaultBase = 48;    // C3
    static constexpr int kDefaultVel  = 100;
    static constexpr int kOctave      = 12;

    struct Result { bool baseChanged = false; };

    KbdPiano() { for (int i = 0; i < kScanCount; ++i) active_[i] = -1; }

    // Semitones above the base note for a physical key, -1 if it is not one.
    // FL Studio's layout, which lays two and a bit octaves across the board
    // instead of Live's one. Positions are named by their US-QWERTY legends
    // purely because that is how the layout is documented everywhere; the codes
    // are what matters, and they are evdev's (linux/input-event-codes.h).
    static int semiFor(int scan) {
        switch (scan) {
        // lower octave — white (Z X C V B N M row, KEY_Z = 44)
        case 44: return 0;    case 45: return 2;    case 46: return 4;
        case 47: return 5;    case 48: return 7;    case 49: return 9;
        case 50: return 11;
        // lower octave — black (S D _ G H J on the home row, KEY_A = 30)
        case 31: return 1;    case 32: return 3;    case 34: return 6;
        case 35: return 8;    case 36: return 10;
        // upper octave — white (Q W E R T Y U row, KEY_Q = 16)
        case 16: return 12;   case 17: return 14;   case 18: return 16;
        case 19: return 17;   case 20: return 19;   case 21: return 21;
        case 22: return 23;
        // upper octave — black (2 3 _ 5 6 7 on the digit row, KEY_1 = 2)
        case 3:  return 13;   case 4:  return 15;   case 6:  return 18;
        case 7:  return 20;   case 8:  return 22;
        // and on into the third (I O P, with 9 0 as its blacks)
        case 23: return 24;   case 24: return 26;   case 25: return 28;
        case 10: return 25;   case 11: return 27;
        default: return -1;
        }
    }

    // Shortcut gating, and no longer a per-key question. Shortcuts are keysym-
    // based while the piano is scancode-based, so on a non-US layout there is
    // no correspondence left to consult: the key that plays a C types 'y' on
    // QWERTZ, 'w' on AZERTY. While the piano is live it therefore owns the
    // whole printable block — every unmodified letter/digit shortcut is
    // suppressed, which is also what Live does. Modified chords are unaffected
    // (notes only fire unmodified, so Ctrl+S still saves), and space is left
    // out on purpose: transport works while playing, on every DAW there is.
    static bool consumes(int key) { return key > 32 && key <= 126; }

    // `enabled` is the whole gate: feature on, no text field focused, no
    // command modifier down. When it is false the piano still runs, because
    // that is what releases notes held across losing the gate, and because it
    // has to re-adopt the physical key state: a key already down when the gate
    // returns (the 'k' of Ctrl+Shift+K, a letter typed into a field) must not
    // read as a fresh press.
    //
    // The octave keys come in as their own flags rather than through the
    // scancode array: PageUp/PageDown are *labelled* keys, not positions on a
    // keyboard-shaped instrument, so they follow the layout like every other
    // named shortcut. They moved off Z and X when those became notes; velocity
    // lost its keys entirely (C and V are notes) and lives on the control bar.
    template <class Emit>
    Result update(const bool* scanDown, bool octaveUp, bool octaveDown, bool enabled,
                  const Emit& emit) {
        Result res;
        if (!enabled) {
            allNotesOff(emit);
            for (int k = 0; k < kScanCount; ++k) prev_[k] = scanDown[k];
            prevOctUp_ = octaveUp;
            prevOctDown_ = octaveDown;
            return res;
        }
        // Down edges only: the octave moves once per physical press.
        if (octaveUp   && !prevOctUp_)   res.baseChanged |= shiftOctave(kOctave);
        if (octaveDown && !prevOctDown_) res.baseChanged |= shiftOctave(-kOctave);
        prevOctUp_ = octaveUp;
        prevOctDown_ = octaveDown;

        for (int k = 0; k < kScanCount; ++k) {
            const bool now = scanDown[k], was = prev_[k];
            prev_[k] = now;
            if (now == was) continue;              // held: no event, no repeat
            if (!now) {                            // up edge -> the note this key started
                if (active_[k] >= 0) {
                    emit(MidiMsg{0x80, (u8)active_[k], 0, 0, 0});
                    active_[k] = -1;
                }
                continue;
            }
            const int semi = semiFor(k);
            if (semi < 0) continue;
            const int note = base_ + semi;         // shiftOctave keeps this in 0..127
            active_[k] = (i8)note;
            emit(MidiMsg{0x90, (u8)note, (u8)vel_, 0, 0});
        }
        return res;
    }

    // Ends every sounding note. Called when the piano is switched off, and from
    // update() whenever the gate closes: a hung note outlives the UI state that
    // started it, and nothing downstream will clean it up.
    template <class Emit>
    void allNotesOff(const Emit& emit) {
        for (int k = 0; k < kScanCount; ++k) {
            if (active_[k] < 0) continue;
            emit(MidiMsg{0x80, (u8)active_[k], 0, 0, 0});
            active_[k] = -1;
        }
    }

    int base() const { return base_; }
    int velocity() const { return vel_; }
    // Velocity is a control-bar number now rather than a pair of keys, so the
    // owner sets it directly. Only notes started after this point take it: a
    // sounding note keeps the velocity it was struck with.
    void setVelocity(int v) { vel_ = clampv(v, 1, 127); }
    // The base is always a C (it only ever moves by whole octaves from C3).
    int octave() const { return base_ / kOctave - 1; }

private:
    // Clamped so the whole mapped span stays inside 0..127: the lowest key must
    // not go under 0 and 'p', twenty-eight semitones up, must not go over 127.
    bool shiftOctave(int by) {
        const int b = base_ + by;
        if (b < 0 || b + kHighestSemi > 127) return false;
        base_ = b;
        return true;
    }

    int  base_ = kDefaultBase;
    int  vel_  = kDefaultVel;
    bool prev_[kScanCount]{};      // scanDown[] as of last update, for edges
    i8   active_[kScanCount];      // note each key started, -1 = silent
    bool prevOctUp_ = false, prevOctDown_ = false;
};

// The MIDI note editor. Declared rather than included: pianoroll.h includes
// *this* header (it edits ClipModel directly), so pulling it in from here would
// leave PianoRoll incomplete for any translation unit that reached pianoroll.h
// first. App therefore holds it behind a unique_ptr and defines its destructor
// out of line, in the one .cpp that has both definitions.
class PianoRoll;

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
    void  clearClip(int track, int slot);
    void  pushClip(int track, int slot);          // sync one slot to the engine
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
    void  materializeDevices();                  // savedDevices -> devices
    void  releaseAllChains();                    // hand every instance to retiring_

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
    Engine   engine_;
    std::unique_ptr<AudioBackend> audio_;
    // MIDI reader. Started after the audio backend and stopped before it: the
    // reader pushes into the engine's ring from its own thread, so it must be
    // joined before anything begins tearing the engine down.
    MidiInput midi_;

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
    void publishChain(int track);
    void ensurePluginScan();                      // lazy, first time DEVICES opens
    void addDeviceToTrack(int track, const PluginDesc& d);
    void removeDevice(int track, int idx);

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
    int  selDevice_ = -1;              // index into the selected track's devices
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
    // Where they go: engine_.pushMidi, the same ring the computer keyboard and
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

    // per-frame UI feedback
    std::string status_;
    f32  peakHoldT_[kMaxTracks]{};
    f32  peakHoldM_[2]{};
    f64  lastFrameTime_ = 0.0;
    f32  fps_ = 0.f;
};

} // namespace lat
