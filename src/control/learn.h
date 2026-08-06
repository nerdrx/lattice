// MIDI-learn: the mapping table between a control surface and the canonical
// parameter address space (docs/PARAM-ADDRESS.md).
//
// This is the second consumer of the address grammar after automation, and it
// deliberately knows nothing about App, Session or the engine beyond MidiMsg.
// A Binding names a TRIGGER (status nibble + channel + data1) and an ADDRESS;
// what that address means today is resolved by the GUI, once, at apply time —
// exactly as PARAM-ADDRESS.md requires ("resolution lives GUI-side", "a MIDI
// mapping to a deleted device is silently inert, never a crash").
//
// UNITS. Everything in here is NORMALISED: a Hit carries a number in 0..1
// (or a delta in the same scale), never a gain, a pan or a plugin's own units.
// The target's real range is a property of the target and is only known after
// the address resolves, so the map cannot hold it without going stale the
// moment a plugin is swapped. Binding::lo/hi are therefore normalised LIMITS —
// "this knob only sweeps the middle third" — and lo > hi is a legal, inverted
// mapping rather than an error.
//
// THREADING. `midiTap()` is the one entry point callable from the ALSA reader
// thread; it does nothing but push into a lock-free ring. Everything else on
// this file — MidiMap, consume(), the learn state machine, load/save — is GUI
// thread only.
#pragma once
#include "../audio/engine.h"          // MidiMsg (and, through it, core/common.h)
#include <optional>
#include <string>
#include <vector>

namespace lat {
namespace ctl {

// How an incoming value becomes a change to the target.
enum class Mode : u8 {
    Absolute = 0,   // 0..127 sweeps lo..hi
    Relative,       // endless encoder: each message is a signed nudge
    Toggle          // a button: flips between lo and hi
};

// The two conventions endless encoders actually ship with. `Auto` decides on
// the first message that can only mean one of them and then latches, which is
// what the user expects from turning the knob once.
//
//   TwosComp   1..63 = +1..+63, 127..65 = -1..-63   (Live's "Relative (Signed)")
//   Offset64   65..127 = +1..+63, 63..1 = -1..-63   (Live's "Relative (Offset)")
//
// The two disagree about every value, but their SLOW-TURN values are disjoint:
// a signed encoder nudged one detent sends 1 or 127, an offset encoder sends 65
// or 63. That, and only that, is what the detector keys on.
enum class Rel : u8 { Auto = 0, TwosComp, Offset64 };

struct Binding {
    // Status is stored as the high nibble only (0xB0 control change, 0x90
    // note-on); the channel rides beside it so a binding is comparable without
    // unpacking. Nothing else is bindable: pitch bend and aftertouch are
    // performance data the engine already forwards to instruments, and stealing
    // them for a fader would break every keyboard.
    u8 status  = 0xB0;
    u8 data1   = 0;               // CC number, or note number
    u8 channel = 0;               // 0..15, matched exactly

    std::string address;          // canonical, docs/PARAM-ADDRESS.md
    f32  lo = 0.f, hi = 1.f;      // normalised limits; lo > hi inverts
    Mode mode = Mode::Absolute;
    Rel  rel  = Rel::Auto;        // configured convention, Auto = detect

    // Runtime only, never serialised.
    Rel rel_seen = Rel::Auto;     // what the detector has latched onto
    u64 hits = 0;                 // messages this binding has acted on

    bool isCC()   const { return status == 0xB0; }
    bool isNote() const { return status == 0x90; }
    bool matches(const MidiMsg& m) const {
        return (u8)(m.status & 0xF0) == status && (u8)(m.status & 0x0F) == channel &&
               m.d1 == data1;
    }
    // Stable id for the physical control, used by the GUI as an undo /
    // automation-capture gesture: every message from one knob coalesces into
    // one edit, exactly as a mouse drag does.
    u64 gesture() const {
        u64 h = 0x9E3779B97F4A7C15ull;
        h ^= (u64)status * 0x100000001B3ull;  h = (h << 7)  | (h >> 57);
        h ^= (u64)channel * 0xC2B2AE3D27D4EB4Full; h = (h << 11) | (h >> 53);
        h ^= (u64)data1 * 0x165667B19E3779F9ull;
        return h ? h : 1;
    }
};

// What one matched message asks the GUI to do. `norm` is normalised in every
// case: an absolute target position for Set, a signed increment for Nudge.
struct Hit {
    size_t      index = 0;        // into MidiMap::bindings()
    std::string address;
    enum class Act { Set, Nudge, Toggle } act = Act::Set;
    f32 norm = 0.f;
    f32 lo = 0.f, hi = 1.f;       // the binding's limits, for Toggle's two ends
    u64 gesture = 0;
};

// Whether an address is worth storing. The default is a LEXICAL check only —
// non-empty, printable ASCII, no spaces, no empty '/' segment — because the
// grammar lives in lat::addr, which is inside the UI's headers and would drag
// the whole session model in here (and out of the standalone harness). The GUI
// installs the real thing with setAddressCheck(&someGrammarCheck), which is how
// the config file gets structure-rejected against the actual grammar without
// this file owning a second copy of it.
using AddressCheck = bool (*)(const std::string&);
bool lexicalAddressOk(const std::string& a);

// ---------------------------------------------------------------------------
// The map.
//
// PERSISTENCE IS A SEPARATE FILE, NOT THE PROJECT: ~/.config/nxtakt/midimap.conf
//
// A control surface belongs to the MACHINE, not to the song. The same set opened
// on the studio desktop (with a 16-knob controller) and on a laptop (with none)
// must not carry, fight over, or lose the studio's mapping; and a set mailed to
// a collaborator must not silently rebind their hardware. Live keeps MIDI
// mappings in the set and it is a persistent annoyance for exactly this reason.
//
// The cost is real and accepted: the mapping references entity UIDs, which are
// only meaningful inside one set, so switching projects leaves most bindings
// dangling. Dangling is already the defined behaviour for a deleted device
// (PARAM-ADDRESS.md: "fail soft, silently inert"), so this costs nothing new —
// and a per-set mapping file is a future addition that can layer on top rather
// than a decision this forecloses.
//
// The format is the project format's discipline in miniature: one line-oriented
// record per binding, a header word carrying a version, STRUCTURE REJECTED and
// VALUES CLAMPED, clamps applied symmetrically on read and write so that
// load -> save is byte-identical for anything this program wrote.
//
//   nxtakt-midimap 1
//   bind cc 0 74 abs 0 1 t:7/vol
//   bind cc 0 16 rel64 0 1 t:7/dev:12/p:3
//   bind note 9 36 toggle 0 1 t:7/mute
//
// Fields: kind(cc|note) channel(0..15) data1(0..127)
//         mode(abs|rel|relc2|rel64|toggle) lo hi address
// The address is last because the grammar guarantees it contains no space, so
// it needs no quoting and the record stays a fixed token count.
// ---------------------------------------------------------------------------
class MidiMap {
public:
    static constexpr int kMaxBindings = 512;
    static constexpr int kVersion = 1;

    const std::vector<Binding>& bindings() const { return bindings_; }
    size_t size() const { return bindings_.size(); }
    void   clear() { bindings_.clear(); cancelLearn(); dirty_ = true; }

    void setAddressCheck(AddressCheck c) { check_ = c ? c : &lexicalAddressOk; }
    bool addressOk(const std::string& a) const { return check_ ? check_(a) : lexicalAddressOk(a); }

    // Adds or replaces. A physical control drives exactly one thing and one
    // address is driven by exactly one control, so both collisions are resolved
    // by eviction rather than by stacking: that is what "learn it again" means.
    // Returns the index, or -1 when the address is malformed or the table full.
    int  bind(const Binding& b);
    bool unbindAddress(const std::string& address);   // true when one went
    int  findAddress(const std::string& address) const;
    int  findTrigger(u8 status, u8 channel, u8 data1) const;
    Binding*       at(size_t i)       { return i < bindings_.size() ? &bindings_[i] : nullptr; }
    const Binding* at(size_t i) const { return i < bindings_.size() ? &bindings_[i] : nullptr; }

    // --- learn state machine ----------------------------------------------
    // beginLearn(address) -> the next inbound message that LOOKS LIKE A CONTROL
    // becomes a binding for that address -> cancelLearn() to give up. `mode` is
    // what the caller wants the control to do; a note-on always ends up a
    // Toggle regardless, because a key has no travel to be absolute about.
    void beginLearn(const std::string& address, Mode mode = Mode::Absolute);
    void cancelLearn() { learning_ = false; learnAddress_.clear(); }
    bool learning() const { return learning_; }
    bool learningFor(const std::string& a) const { return learning_ && learnAddress_ == a; }
    const std::string& learnAddress() const { return learnAddress_; }

    // THE MAPPING LAYER'S ONE ENTRY POINT. Feeds one inbound message through
    // learn-or-match. Returns nothing when the message was consumed by the
    // learn state machine, matched nothing, or matched a binding that this
    // particular message does not move (a button's release edge). `learned`,
    // when given, reports that a binding was just created.
    std::optional<Hit> consume(const MidiMsg& m, bool* learned = nullptr);

    // --- persistence -------------------------------------------------------
    std::string serialize() const;
    // Structure rejected, values clamped. False leaves the map UNTOUCHED and
    // fills `err` with "<line>: <what>" — a half-applied config is worse than
    // none, and refusing wholesale is what stops a save from overwriting a file
    // we only partly understood.
    bool parse(const std::string& text, std::string* err = nullptr);
    bool load(const std::string& path, std::string* err = nullptr);
    bool save(const std::string& path, std::string* err = nullptr) const;

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    std::vector<Binding> bindings_;
    AddressCheck check_ = &lexicalAddressOk;
    bool learning_ = false;
    std::string learnAddress_;
    Mode learnMode_ = Mode::Absolute;
    bool dirty_ = false;
};

// ~/.config/nxtakt/midimap.conf, honouring XDG_CONFIG_HOME. Creates nothing.
std::string defaultMapPath();
// mkdir -p of the containing directory. Called before a save.
bool ensureParentDir(const std::string& path);

// ---------------------------------------------------------------------------
// The reader-thread tap.
//
// Hardware MIDI reaches the engine from the ALSA reader thread and is never
// seen by the GUI (src/audio/midi_in.cpp -> Engine::pushMidi -> audio thread).
// The mapping layer has to run on the GUI thread, because applying a mapping
// touches the session model. So: the reader thread calls midiTap(), which does
// nothing but push into a lock-free SPSC ring, and the GUI drains it once a
// frame. One producer (the reader thread), one consumer (the GUI), no locks,
// no allocation, and a full ring drops rather than blocks — a dropped CC is a
// knob that moved a fraction less, not a stuck note.
//
// This costs ONE line in midi_in.cpp; see the report. Until that line exists
// the ring stays empty and every mapping is inert, which tapCount() vs. the
// MidiInput's own received() makes visible in the UI rather than mysterious.
// ---------------------------------------------------------------------------
void midiTap(const MidiMsg& m);       // reader thread (single producer)
bool midiTapPop(MidiMsg& out);        // GUI thread (single consumer)
u64  midiTapCount();                  // messages accepted into the ring
u64  midiTapDropped();                // messages the ring had no room for

} // namespace ctl
} // namespace lat
