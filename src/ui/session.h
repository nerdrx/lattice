// Session model — the GUI-side truth for a set.
//
// The model here is the editable, allocating, std::string-carrying version.
// Whenever something changes that the audio thread needs, it is pushed across
// as a Command; the engine keeps its own realtime-safe mirror.
//
// This header is ONLY the model (clips, tracks, returns, scenes, the Session
// aggregate) plus the small UI value-types that ride with it. It is what
// src/core/project.* and src/ui/pianoroll.* need to see — deliberately nothing
// about class App. app.h includes this; nothing here includes app.h.
#pragma once
#include "../audio/engine.h"
#include "../audio/sample.h"
#include "../plugin/host.h"
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

// One breakpoint in an envelope, in clip-relative beats. `curve` is reserved
// and must be 0 for now: the segment to the next point is linear. It is a byte
// because the shapes worth having are an enumeration (linear, ease, S, hold)
// rather than a continuum, and a byte costs nothing where a second control
// point would double the wire form for a feature nobody has asked for.
struct AutoPoint {
    f64 beat  = 0.0;
    f32 value = 0.f;                  // in the target's own units
    u8  curve = 0;
    u8  pad[3] = {};
};

// One address's worth of automation inside one clip.
//
// The address is kept as TEXT, never as a resolved target — PARAM-ADDRESS.md's
// "resolution lives GUI-side" applied here. A lane naming a device that is not
// loaded today survives a save/load intact, exactly as ClipModel::path survives
// a missing sample. Resolution happens at publish time and is thrown away on
// every structural change.
struct AutoLane {
    std::string address;              // canonical, see docs/PARAM-ADDRESS.md
    std::vector<AutoPoint> points;    // sorted by beat, unique beats
    bool enabled = true;              // Live's "deactivate envelope"
};

inline constexpr int kMaxClipLanes      = 16;    // == kMaxRtAutoLanes
inline constexpr int kMaxClipAutoPoints = 4096;  // total across a clip's lanes

struct ClipModel {
    // Stable identity. UIDs never change once assigned and are serialized, so
    // undo, set-diff, automation targets and collaboration can reference an
    // entity across moves and renames. 0 = unassigned (Session::newUid()).
    u64 uid = 0;
    ClipKind kind = ClipKind::Audio;
    std::vector<NoteModel> notes;      // Midi clips only; kept sorted by beat
    std::vector<AutoLane>  envelopes;  // clip automation; audio and MIDI alike
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

} // namespace lat
