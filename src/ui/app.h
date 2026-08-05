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

// A live plugin instance lifted out of a session that is about to be replaced,
// so an undo can rebind it instead of loading the plugin again. Identity is the
// uid; the uri is carried too because a uid only means "the same device" if the
// plugin behind it is still the same one. See App::adoptSession.
struct LiveDevice {
    u64 uid = 0;
    std::string uri;
    PluginDesc desc;
    std::unique_ptr<PluginInstance> inst;
};

// One clip's audio at the moment an undo snapshot was taken, keyed by clip uid.
//
// Keyed by uid and not by source path because the path is not always the
// truth: a take that has been recorded but not exported has no file behind it
// at all, and a project document can only name files. Restoring such a set
// from its text alone would quietly turn a recording into an empty clip, so an
// undo entry carries the audio itself -- a shared_ptr each, so the cost is a
// pointer per clip and the history pins the buffers it may still have to give
// back. See App::UndoEntry.
struct ClipSample {
    u64 uid = 0;
    SampleRef sample;
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
    f32   sends[kMaxReturns] = {};     // post-fader send levels, 0..1 linear
    bool  mute = false, solo = false, arm = false;
    f32   width = 94.f;
};

// A return bus (Live's A/B/... return tracks): a device chain and a level,
// no clips. Same instance-ownership rules as TrackModel::devices.
struct ReturnModel {
    u64 uid = 0;
    std::string name = "Return";
    std::vector<DeviceModel> devices;
    std::vector<SavedDevice> savedDevices;
    f32 fader = 0.85f;
};

struct SceneModel {
    u64 uid = 0;
    std::string name;
    f64 tempo = 0.0;                   // 0 => no tempo change on launch
};

struct Session {
    std::vector<TrackModel> tracks;
    std::vector<SceneModel> scenes;
    ReturnModel returns[kMaxReturns];   // fixed buses; empty chains = inert
    std::vector<DeviceModel> masterDevices;
    std::vector<SavedDevice> masterSavedDevices;
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
    // Headless verification hook (LATTICE_DEBUG_UNDO). Nothing can click a
    // fader inside gamescope, and the restore path is the part of this feature
    // a screenshot cannot check -- so it is driven from here instead, against
    // whatever set was loaded, with a live engine and real plugins.
    void debugUndoSelfTest();

    // per-frame UI feedback
    std::string status_;
    f32  peakHoldT_[kMaxTracks]{};
    f32  peakHoldR_[kMaxReturns]{};
    f32  peakHoldM_[2]{};
    f64  lastFrameTime_ = 0.0;
    f32  fps_ = 0.f;
};

} // namespace lat
