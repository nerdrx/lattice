# Parameter automation

Draft for `docs/AUTOMATION.md`. Status: design only — nothing in this document
is implemented. It is written to be handed to wave-7 agents as a specification,
so where a decision could go two ways the decision is made here and the
rejected alternative is written down beside it.

## Contents

1. [What this is, and the one rule everything follows](#1-what-this-is)
2. [Data model](#2-data-model)
3. [Engine application](#3-engine-application)
4. [Address resolution](#4-address-resolution)
5. [Recording automation](#5-recording-automation)
6. [UI](#6-ui)
7. [Persistence: format v5](#7-persistence-format-v5)
8. [Daemon composition](#8-daemon-composition)
9. [Phasing and test strategy](#9-phasing-and-test-strategy)
10. [Deliberately deferred](#10-deliberately-deferred)
11. [Open questions for the orchestrator](#11-open-questions-for-the-orchestrator)

---

## 1. What this is

Automation is a time-varying value written onto a canonical parameter address
(`docs/PARAM-ADDRESS.md`). Two containers hold those values in Live, and the
model here has to admit both:

- **Clip envelopes** — per clip, clip-relative, looping with the clip. This is
  what wave 7 builds. It is the one that matters for a session-first DAW: the
  filter sweep belongs to the loop, not to a position on a timeline nobody is
  looking at.
- **Arrangement lanes** — per track, absolute-timeline, one lane per address.
  Deferred (§10), but §2 and §7 are shaped so that adding them later is
  additive rather than a rewrite.

### The rule

> **The engine never writes an automated value into the field it is
> automating.**

`Track::vol` stays whatever the user's fader last said. The envelope produces
an *effective* gain for the block, and only the effective gain reaches the
mixdown. Three hard problems disappear at once:

- **Stopping the clip** needs no cleanup. The effective value stops being
  computed and the field was never touched.
- **Override** (the user grabs a knob under a running envelope) is a decision
  about which of two values to use, not a fight over one memory location.
- **Undo and save** cannot be corrupted by playback. `Session` is exactly what
  the user typed; automation is a separate, additive thing.

There is exactly one target class where this rule cannot hold — device
parameters, because `PluginInstance` has one storage slot per parameter and no
concept of "effective". §3.4 handles that case explicitly, with a
restore-on-stop obligation that exists *only* because the rule cannot be
honoured there. Every other target obeys it without exception.

---

## 2. Data model

### 2.1 GUI model (`src/ui/app.h`)

```c++
// One breakpoint in an envelope, in clip-relative beats.
//
// `curve` is reserved and must be 0 in wave 7: the segment from this point to
// the next is linear. It is a byte rather than a bool or a float because the
// shapes worth having are an enumeration (linear, ease-in, ease-out, S, hold)
// and not a continuum, and because a byte costs nothing here while a second
// f64 control-point per breakpoint would double the wire form for a feature
// nobody has asked for yet. A reader that meets a non-zero curve renders it as
// linear and writes it back unchanged (§7).
struct AutoPoint {
    f64 beat  = 0.0;
    f32 value = 0.f;      // in the TARGET'S OWN UNITS -- see §2.3
    u8  curve = 0;
    u8  pad[3] = {};
};

// One address's worth of automation inside one clip.
//
// The address is kept as TEXT and not as a resolved target, and that is the
// whole of PARAM-ADDRESS.md's "resolution lives GUI-side" rule applied here: a
// lane naming a device that is not loaded today must survive a save/load cycle
// intact, exactly as ClipModel::path survives a missing sample and
// DeviceModel::lostParams survives a missing plugin. Resolution happens at
// publish time, is thrown away on every structural change, and is never the
// thing that is stored.
struct AutoLane {
    std::string address;              // canonical, see PARAM-ADDRESS.md
    std::vector<AutoPoint> points;    // sorted by beat, unique beats
    bool enabled = true;              // Live's "deactivate envelope"
};

struct ClipModel {
    // ... existing fields ...
    std::vector<AutoLane> envelopes;
};
```

Bounds, enforced by the editor and by the publisher, never by the parser
(§7 keeps the reader's "structure rejected, values clamped" split):

| constant | value | why |
|---|---|---|
| `kMaxClipLanes` | 16 | more addresses than one clip is ever *about*; also the width of the published lane array, which must be fixed for the engine |
| `kMaxClipAutoPoints` | 4096 | total across all lanes in one clip; ~4 points per 1/64 note over 16 bars |

Both are ceilings a human cannot reach by hand and a recording pass reaches
only if the thinning in §5 has failed, which makes hitting them a bug report
rather than a limitation.

### 2.2 Wire form (`src/audio/engine.h`, beside `RtNote`)

```c++
// What an envelope automates. The engine switches on this; nothing else does.
enum class AutoTarget : i32 {
    None = 0,
    TrackVol,       // Track::vol,  transform Fader
    TrackPan,       // Track::pan,  transform Direct
    TrackSend,      // Track::send[index], Direct
    DeviceParam,    // chain[devSlot] parameter `index`, Direct
    // Reserved, in this order, so the numbering never moves:
    // ClipGain, MasterVol, ReturnVol, TrackMute.
};

// How the stored value becomes the value the engine uses. The model stores
// what the UI edits (§2.3), and for the volume fader that is a 0..1 fader
// position, not a gain -- so the mapping has to live somewhere. It lives here,
// as data, so that the engine needs no table of which target means which curve
// and the GUI needs no second copy of faderToGain's inverse.
enum class AutoXform : i32 { Direct = 0, Fader = 1 };

struct RtAutoPoint {           // 16 B
    f64 beat;
    f32 value;
    u8  curve;
    u8  pad[3];
};

struct RtAutoLane {
    i32 target;                // AutoTarget
    i32 index;                 // return index / device param index; else 0
    i32 devSlot;               // chain position, or -1 for engine scalars
    i32 xform;                 // AutoXform
    i32 first, count;          // [first, first+count) into RtAutoSet::points
    f32 lo, hi;                // clamp, resolved GUI-side from ParamInfo
    u32 flags;                 // kAutoOverridden | kAutoInert
};

inline constexpr u32 kAutoOverridden = 1u << 0;   // user grabbed the control
inline constexpr u32 kAutoInert      = 1u << 1;   // engine gave up on it (§3.4)

inline constexpr int kMaxRtAutoLanes = 16;        // == kMaxClipLanes

// One allocation, always. `points` addresses memory inside this same block,
// immediately past the struct, so the whole set is a single new[] and a single
// delete[] and the retirement protocol has exactly one pointer to talk about.
// Two allocations (a lane array and a point array) would need two retirement
// events or a rule about which one implies the other, and the RtNote protocol
// is only simple because there is one pointer per slot.
struct RtAutoSet {
    const RtAutoPoint* points = nullptr;
    RtAutoLane lanes[kMaxRtAutoLanes] = {};
    int laneCount = 0;
    int pointCount = 0;
};
```

`RtClip` gains one pointer and nothing else:

```c++
struct RtClip {
    // ... existing fields ...
    // Clip envelopes. Same lifetime protocol as `notes`, for the same reason:
    // an envelope can be edited, and recorded into, while the clip is playing.
    // A replaced set travels back to the GUI in Ev::AutosRetired before it may
    // be freed.
    const RtAutoSet* autos = nullptr;
};
```

### 2.3 The value domain, and why it is not normalized

A breakpoint stores **the value the UI edits**: a fader position in 0..1 for
`vol`, -1..1 for `pan`, a linear 0..1 for a send, and the plugin's own units
(Hz, dB, whatever `ParamInfo::min`/`max` say) for a device parameter.

The alternative — normalize everything to 0..1 against the target's range at
write time — was rejected for one reason that outweighs its convenience: a
plugin update that changes a parameter's range would silently *rescale* every
breakpoint in every set, turning a 200 Hz sweep into a 2 kHz one with nothing
in the file to notice it by. Storing plugin units means the same update makes
the envelope clamp at the new range's edge — visibly wrong in the one place the
user is looking, and repairable. `ParamInfo::id` renumbering is already
documented as "the plugin's fault, not ours"; silent rescaling would make it
ours.

The cost is `AutoXform`: the volume fader's stored 0..1 is not a gain, so
something must call `faderToGain`. That is one enum field and one branch in the
evaluator, and it keeps the number in the file identical to the number under
the fader — which is also what makes a hand-edited `.lattice` file legible.

### 2.4 The evaluator, and why it is shared

```c++
// Value of one lane at a clip-relative beat. Pure, header-inline, allocation-
// free, and safe to call from the audio thread.
//
// It is deliberately the SAME function the GUI calls to draw the moving knob.
// The alternative -- the engine evaluating and reporting the applied value back
// through an event or an atomic -- costs a channel, and buys a way for the
// displayed value and the applied value to disagree. They cannot disagree if
// there is one function reading one set of points against one beat.
//
// Semantics, all three of which are load-bearing:
//   * before the first point: the first point's value (no ramp in from
//     nowhere -- there is no "nowhere" to ramp from at clip start);
//   * after the last point: the last point's value, held to the loop end;
//   * a lane with no points evaluates to `fallback`, which the caller supplies
//     as the un-automated value. An empty lane is UI state, not content, and
//     must be a no-op rather than a jump to zero.
inline f32 autoValueAt(const RtAutoSet& s, const RtAutoLane& l, f64 beat, f32 fallback);
```

The lookup is a linear scan from a per-lane cursor the engine keeps, not a
binary search: beats advance monotonically inside a loop and reset at the wrap,
so the scan is O(1) amortized with an O(count) worst case exactly once per
wrap. A binary search would be O(log n) *every* block for no gain at the sizes
involved.

### 2.5 Retirement protocol

Verbatim the `RtNote` protocol, and the doc for it should say so rather than
re-derive it:

```
App::publishAutos(track, slot, const RtAutoSet* fresh)
    old = publishedAutos_[track][slot]
    publishedAutos_[track][slot] = fresh
    if (old && old != fresh) retiringAutos_.push_back(old)
```

- `Ev::AutosRetired` carries `p = the RtAutoSet* now safe to free`.
- The engine pushes it from `Cmd::SetClip` / `Cmd::ClearClip` handling, and
  only when the displaced pointer differs from the incoming one — the same
  "an entry that would never be announced must not be queued" condition
  `publishNotes` already documents.
- `pumpEngineEvents()` frees on receipt, and refuses to free a pointer it has
  no record of owning, with the same `LOGW` and the same reasoning.
- Freed as `delete[] (char*)set` because the allocation is a `char[]` holding a
  placement-new'd `RtAutoSet` followed by the points. The publisher and the
  reaper must agree on that; put both in `app.cpp` next to each other with a
  comment saying why the cast is what it is.

### 2.6 What the model does *not* preclude

Arrangement lanes are a `std::vector<AutoLane>` on `TrackModel` with absolute
beats instead of clip-relative ones, evaluated against `Engine::beat` instead of
`Voice::beatPos`, published per track instead of per clip. Every type in §2.2
serves them unchanged; what they need is a second publish path
(`Cmd::SetTrackAutos`, `Ev::TrackAutosRetired`) and a second evaluation site.
Nothing here has to move for that to happen, which is the only requirement the
brief put on the model.

---

## 3. Engine application

### 3.1 Where the beat comes from

An envelope is clip-relative, so it needs the playing clip's position.

- **MIDI voices** already carry it: `Voice::beatPos`, wrapped into the loop by
  `renderMidiVoice`.
- **Audio voices** carry `srcPos` in source frames. The clip beat is the same
  quantity `clipPhase` is published from:
  `(srcPos - loopStart) / (loopEnd - loopStart) * lengthBeats`. Factor it out
  as `static f64 voiceClipBeat(const Voice&, const RtClip&)` and have
  `publish()`'s existing `clipPhase` store go through it too, so the number the
  UI draws the playhead at and the number the envelope is evaluated against are
  provably the same one.

**Only the primary voice drives envelopes.** `Track::prev` — the outgoing clip
during a switch — does not. Two clips' envelopes fighting over one track gain
across a 6 ms crossfade produces a value that is neither, and the audible
result of ignoring the outgoing one is that its last applied value is held for
the length of the fade, which is correct.

**A track with no active primary voice applies no envelopes.** Its scalars are
whatever the user set. This is Live's behaviour and it is what makes §1's rule
free.

### 3.2 The two target classes

| class | targets | evaluated | applied |
|---|---|---|---|
| **A — engine-owned scalars** | `TrackVol`, `TrackPan`, `TrackSend` | at the callback's start beat and end beat | as an intra-block linear ramp of the *derived* value |
| **B — device parameters** | `DeviceParam` | at the callback's start beat only | one `setParamRT` per block, before the chain runs |

Class A is ramped because a step in a gain once per callback is a zipper, and
because the ramp is free: the per-track mixdown loop in `process()` already
runs per sample and already multiplies by `gL`/`gR`. It becomes

```c++
const f32 dL = (gL1 - gL0) / (f32)n, dR = (gR1 - gR0) / (f32)n;
f32 gL = gL0, gR = gR0;
for (int i = 0; i < n; ++i) { ... gL += dL; gR += dR; }
```

— two adds per sample, and only on tracks that actually carry a class-A lane.
Untouched tracks keep the constant-gain path they have today, byte for byte.
That "the ordinary case must stay free" discipline is the same one the delay
compensation code states for `comp == false`, and it should be honoured the
same way: a `bool anyAuto[kMaxTracks]` decided once per block.

The ramp interpolates the **derived** value (the gain after `faderToGain`), not
the stored one. Interpolating the fader position and mapping per sample would
mean a `pow` and a `log10` per sample, and would also make the ramp's shape
depend on the block size in a way the derived-value ramp does not.

Class B is not ramped because there is nothing to ramp: a plugin parameter is a
single value handed over once, and every backend's own smoothing (or lack of
it) is the plugin's business. 5.3 ms of granularity at 256 frames / 48 kHz is
what every host that does not do sample-accurate parameter events gives, and
CLAP's event stream is the door to doing better later (§3.4).

### 3.3 Where it runs, exactly

In `Engine::process()`, after `drainCommands()` and after the `live[]` decision,
one pass computes automation for the block:

```
drainCommands()
  -> live[] decision, scratch cleared
  -> AUTOMATION PASS         <-- new
       for each live track with an active primary voice whose clip has autos:
         b0 = voiceClipBeat(voice, clip)                  // start of block
         b1 = b0 + n * bps  (wrapped into lengthBeats)    // end of block
         for each lane (skipping kAutoOverridden / kAutoInert):
           class A: store (v0, v1) into Track::autoA[...]
           class B: setParamRT on chain->fx[devSlot]
  -> sub-block launch loop (renderRange / captureRange / captureMidiRange)
  -> per-track post stage: chain, PDC, ramped fader/pan/sends, meters
```

Two placement facts are not negotiable:

1. **Class B must be applied before `fx->process()`** for the block it belongs
   to, which is why the pass runs before the sub-block loop rather than inside
   the post stage. It is the same ordering constraint MIDI already has ("a note
   event has to reach an instrument in time for the block it belongs to").
2. **`b1` is computed from the block start, not read back after rendering.**
   Reading the voice's position after `renderRange` would fold the loop wrap
   into the ramp and produce a jump. Computing it forward and wrapping it
   explicitly makes the wrap a known case, handled by clamping the ramp to the
   loop end (the last block of a lap ramps to the envelope's end value, and the
   next block starts from its start value — one block of discontinuity at the
   wrap, which is exactly what the loop *is*).

A clip launched part-way through a block gets its envelope applied from the
block's start beat, which is a few milliseconds early. It is inaudible because
the voice is inside its 3 ms attack ramp for that whole window, and it is
documented here rather than fixed, because fixing it means evaluating class A
per sub-block and paying the split cost on every track that has a lane.

### 3.4 Device parameters: `setParamRT`

`host.h` is explicit that `setParam()` is called *from the GUI thread while the
audio thread is inside `process()`*. The engine cannot call it. But the reason
it cannot is per-backend, not universal, so the contract gets one addition and
each backend gets the honest answer:

```c++
// host.h, on PluginInstance:

    // REALTIME. The automation path: called from inside the audio callback,
    // before this block's process(), to apply a value the engine computed from
    // a clip envelope.
    //
    // It is a separate entry point from setParam() and not a relaxation of it,
    // because the two callers differ in the one way that matters -- setParam()
    // is the ONLY writer on the non-realtime side and may therefore use a
    // single-producer queue, which is exactly what the CLAP backend does. A
    // second producer on that queue would be a data race, so a backend whose
    // parameter path is a queue must give the audio thread a path of its own.
    //
    // Returns false when this backend has no realtime parameter path. The
    // engine then marks the lane inert (kAutoInert), emits Ev::AutoLaneInert
    // once so the UI can grey it, and never calls again for that published set.
    // A silently ignored automation lane would be the worst of both worlds:
    // the envelope is drawn, the sound does not move, and nothing says why.
    //
    // Same rules as process(): no allocation, no locks, no exceptions.
    virtual bool setParamRT(int i, f32 v) { (void)i; (void)v; return false; }

    // getParam() is realtime-safe to call. It is a plain load in every backend
    // in the tree (LV2 reads its control port array, CLAP its value vector,
    // internal devices their float array), and the engine needs it to remember
    // what a parameter was before an envelope took it over (§3.5).
```

Per-backend reality:

- **Internal devices** (`internal_devices.cpp`): `setParamRT` is `setParam`'s
  body — clamp and store into `pv_[]`. Safe because it is a 4-byte aligned
  plain store, which is precisely what the file header already argues for the
  GUI-side writer. Two writers instead of one changes nothing about tearing;
  the value is always one of the two written, and §3.6's override rule means
  the two are not normally writing at the same time anyway.
- **LV2** (`lv2_host.cpp`): identical — clamp against `ParamInfo` and store into
  `ctrl_[paramPort_[i]]`. Same argument.
- **CLAP** (`clap_host.cpp`): **not** a plain store. `setParam` pushes to
  `queue_`, a `Ring` whose single producer is the GUI thread and whose single
  consumer is the audio thread inside `buildEventList()`. The audio thread must
  not become a second producer. So `setParamRT` writes into an array the audio
  thread owns outright, exactly as `midi()` already does:

  ```c++
  struct RtParamMsg { u32 index; f32 value; };
  static constexpr int kMaxRtParams = 64;      // >= kMaxRtAutoLanes, with room
  RtParamMsg rtParams_[kMaxRtParams]{};
  int        rtParamCount_ = 0;
  ```

  `setParamRT` appends (dropping past the cap, never growing), writes
  `values_[i]` so `getParam` stays truthful, and returns true.
  `buildEventList()` emits the GUI queue first and the RT array **second**, then
  zeroes `rtParamCount_` the way it already zeroes `midiCount_`. Emitting the RT
  array last is the precedence decision: if a stale queued knob value and an
  automated value land in the same block, the automated one is the one the
  plugin ends the block with. And because CLAP events carry `header.time`, this
  is the backend where sub-block-accurate automation becomes available for free
  the day someone wants it — the array entries just need a frame field.
- **VST3**, when it lands: must implement `setParamRT` or its parameters are
  not automatable, and the default `return false` makes that a visible fact
  rather than a mystery.

### 3.5 Restore-on-stop, the one place §1's rule breaks

A device parameter has one storage slot, so applying an envelope to it
destroys the user's value. The engine therefore owes a restore:

```c++
struct Track {
    // ... existing ...
    // Device parameters this track's envelopes have taken over, and what they
    // were before. Captured with getParam() the first time a lane writes a
    // given (devSlot, param), written back when the lane stops applying --
    // the clip stopped, the clip changed, the lane was overridden or removed,
    // the transport stopped, the chain was replaced.
    //
    // This exists ONLY because a plugin parameter cannot have an "effective"
    // value the way Track::vol can (§1). It is the engine cleaning up what the
    // engine started, which is the same obligation flushOffs() discharges for
    // note-offs, and it fails the same way if skipped: silently, and only
    // sometimes.
    struct AutoHold { i32 devSlot = -1; i32 param = -1; f32 was = 0.f; bool used = false; };
    AutoHold autoHold[kMaxRtAutoLanes];
};
```

The alternative — the GUI notices `Ev::ClipStopped` and re-sends the parameter
— was rejected: it is a round trip, so the automated value is audible for a
frame or two after the clip stops, and it stops working the moment the GUI is
slow or dead, which is a state the process split explicitly designs for.

`Cmd::SetChain` on a track with holds outstanding must restore *before* the
chain pointer is swapped, because after the swap the instance may be one the
engine no longer references. That is one line in `drainCommands`'s `SetChain`
case and it is the kind of line that gets forgotten; the test in §9 checks it.

### 3.6 Override and takeover — Live's semantics, stated exactly

The behaviour to reproduce: while an envelope is driving a control, the control
moves on screen. If the user grabs it, the envelope stops applying to it and
the control obeys the hand ("override"). A "Re-enable Automation" affordance
puts the envelope back in charge, and so does relaunching the clip.

Implementation, and it is deliberately the cheapest thing that is correct:

- **The override lives in the published lane's `flags`**, not in a separate
  channel. The GUI holds the truth in a runtime-only list
  `std::vector<std::pair<u64 /*clipUid*/, std::string /*address*/>> autoOverrides_`
  and sets `kAutoOverridden` at publish time. Turning override on or off is
  therefore a clip republish — which is already a supported, cheap, once-per-
  gesture operation (it is what editing a note does) and which needs no new
  command, no new event and no new engine state.
- **`autoOverrides_` is runtime state.** It is not serialized (§7 has no line
  for it), it is not in the undo snapshot (the snapshot is the project text, so
  this falls out for free), and it is cleared by: the clip being relaunched
  (`Ev::ClipStarted` for that track/slot), the transport stopping, and the user
  clicking Re-enable. That matches Live, where an override does not survive the
  set.
- **The knob's displayed value** while a lane is applying and not overridden is
  `autoValueAt()` evaluated GUI-side against the polled `clipPhase` — §2.4. The
  widget draws that value and refuses to be the model's mirror for that frame.
  When the user starts a drag on such a widget, the drag's starting value is
  the automated one (so the control does not jump), the override is set, and
  from there it is an ordinary edit of the model field.
- **A device parameter's override additionally triggers the restore in §3.5** —
  but to the value the user is now dragging, not to the captured one, because
  the user's hand is the newer statement. Concretely: the hold is dropped
  (`used = false`) without writing back, and the GUI's normal param path takes
  over.

What is explicitly *not* implemented: Live's global "Back to Arrangement"
button and per-track override latching for arrangement automation. Those belong
with arrangement lanes.

---

## 4. Address resolution

### 4.1 A shared parser

`docs/PARAM-ADDRESS.md` names automation, MIDI-learn, OSC and the undo system as
consumers of one grammar. Automation is the first to arrive, so it writes the
parser, and it writes it somewhere all four can reach:

```c++
// src/core/address.h -- no engine types, no plugin types, no UI types.
struct ParsedAddr {
    enum class Scope { Master, Track, Scene } scope = Scope::Master;
    u64 scopeUid = 0;              // track or scene uid; 0 for master
    enum class Field {
        None, Vol, Pan, Mute, Solo, Arm,
        DeviceParam,               // devUid + paramId
        ClipField,                 // clipUid + clipField
        SceneLaunch
    } field = Field::None;
    u64 devUid = 0;   u32 paramId = 0;
    u64 clipUid = 0;  int clipField = 0;
    int sendIndex = -1;            // reserved: "t:7/send:2" is not in the
                                   // grammar yet; see §11
};

bool parseAddress(const std::string& s, ParsedAddr& out);   // false = malformed
std::string formatAddress(const ParsedAddr&);               // round-trips
```

`formatAddress(parseAddress(x)) == x` for every well-formed `x` is a test, not
a hope: the project format writes the string it was given, so a normalizing
round trip that changed the text would break byte-stable save/load/save.

**The grammar needs one addition for sends.** `docs/PARAM-ADDRESS.md` lists
`vol | pan | mute | solo | arm` and no send. Automating a send is table stakes,
so the segment `send:` INDEX joins the grammar (`t:7/send:2`), indexed rather
than uid'd because return buses are a fixed array, not a list — the same reason
`ReturnModel` is `returns[kMaxReturns]` and the project format writes
`send <idx> <level>`. That is a one-line edit to PARAM-ADDRESS.md and it should
land in the same wave as the parser.

### 4.2 Publish-time resolution

`App::publishAutos(track, slot)` builds the `RtAutoSet`. Per lane:

1. `parseAddress`. Malformed → the lane is skipped and logged once. (It cannot
   normally happen: §7 rejects malformed addresses at load and the editor only
   emits well-formed ones. It is checked anyway because the model is public.)
2. **Scope check.** The address's track uid must equal the clip's own track's
   uid. A clip envelope may only automate its own track's mixer and its own
   track's devices — see §11 for why this restriction exists and what it costs.
3. `Field::Vol` → `TrackVol` / `AutoXform::Fader`, `lo=0, hi=1`.
   `Field::Pan` → `TrackPan` / `Direct`, `lo=-1, hi=1`.
   `send:i` → `TrackSend`, `index=i`, `Direct`, `lo=0, hi=1`.
4. `Field::DeviceParam` → two lookups, and **the second one is the one people
   get wrong**:
   - `devUid` → the `DeviceModel` in `TrackModel::devices` with that uid. Not
     found (deleted device) → lane skipped, kept in the model.
   - that model → its **position in the published `RtChain`**, which is *not*
     its index in `devices`: `publishChain()` skips devices whose `inst` is null
     (the plugin was missing when the set loaded). `devSlot` must be counted the
     same way `publishChain` counts, and the cleanest way to guarantee that is
     to have `publishChain()` return (or record) the model-index → chain-slot
     map and have `publishAutos` read it. A device with no instance yields no
     chain slot, so its lanes are skipped — correct, and the same "silently
     inert, never a crash" that PARAM-ADDRESS demands.
   - `paramId` → the parameter *index* `i` where
     `inst->paramInfo(i).id == paramId`. Not found → skipped. `lo`/`hi` come
     from that `ParamInfo`; the engine clamps to them so a stale envelope
     cannot drive a parameter out of its declared range.
5. Points are copied in model order into the flat point array, `first`/`count`
   filled in. Points are **not** sorted here — see §7; the editor holds the
   sorted invariant, the publisher and the file format both preserve what they
   are given, and a lane whose points arrived unsorted evaluates to something
   ugly rather than something undefined.

### 4.3 Re-resolution triggers

The resolution is a cache with no invalidation signal of its own, so the rule is
blunt on purpose:

> **Whenever a track's chain is republished, republish the envelopes of every
> clip on that track.**

That covers `addDevice`, `removeDevice`, a future `moveDevice`,
`materializeDevices`, `adoptSession` (load and undo), and `releaseAllChains`.
Plus the obvious per-clip triggers: any envelope edit, any recorded point, a
clip being loaded or cleared, `assignUids` filling in a uid an address could
name.

Cost: `kMaxScenes` small allocations and the same number of retirement entries
per chain change. A chain change is a user action; 32 allocations is nothing.
The alternative — a dependency map from device uid to the clips whose lanes
name it — is a cache with an invalidation problem, which is the bug this rule
exists to not have.

---

## 5. Recording automation

### 5.1 The gesture

A new latching control beside the record button: **Automation Arm** (Live's
name). While it is lit *and* the transport is playing, a gesture on an
automatable control writes into the envelope of the clip currently playing on
that control's track.

Which clip: `engine_.activeSlot[track]`, and only when
`engine_.slotState[track] == SlotState::Playing`. Which beat:
`engine_.clipPhase[track] * clip.lengthBeats`, which is the same number §3.1
evaluates against — the round trip through the atomic costs one block of
latency, which at 5 ms is under a tenth of the shortest deliberate gesture a
hand makes.

No clip playing on that track → nothing is recorded, and the status bar says so
**once**, latched exactly like `kbdNoArmHint_`. Repeating it every frame would
bury everything else.

### 5.2 Thinning, in two stages

A gesture sampled per frame at 60 Hz over four bars at 120 BPM is 480 points for
one fader move. Two stages fix that, and both are necessary:

- **While the gesture runs**, append a point only when the value has moved by
  more than `kAutoValueEps` (1/1024 of the target's range — below the
  resolution of any control the UI draws) *or* more than `kAutoMinSpacing`
  (1/64 beat) has elapsed since the last append. A held fader produces two
  points, not four hundred.
- **At gesture end**, run a Douglas–Peucker simplification over the points this
  gesture wrote, with the same `kAutoValueEps` as the tolerance. A linear fade —
  the single most common automation gesture there is — collapses to its two
  endpoints. Only the current gesture's span is simplified; points that were
  already in the lane are never touched by a pass that did not cross them.

### 5.3 Punch semantics

A recording pass **replaces** the envelope over the beat span it covered. On
each append at beat `b`, points in `(lastAppendBeat, b]` are erased before the
new one is inserted. A pass that runs longer than the clip wraps and keeps
erasing — so holding a knob still for two laps leaves a flat envelope, which is
what the user asked for.

The first append of a gesture erases nothing (there is no previous append to
span from), so a gesture that starts mid-envelope leaves the material before it
intact and produces a step at its start. Live behaves the same way; smoothing
that step is a "fade in the punch" refinement nobody has asked for.

### 5.4 Undo

**No new undo machinery.** The envelope lives in `ClipModel`, `ClipModel` is
serialized by the project writer, and the undo entry *is* the project text. So:

- `undoPointWith("automation: <address>", field, before, gestureId)` at the
  **start** of the gesture, with `gestureId` = the widget's `uiId`. The existing
  `undoCoalesce` gives exactly one entry per continuous drag, which is the
  behaviour a fader drag already has.
- The pre-edit value handed back to `undoPointWith` is the **model field**
  (`t.fader`, the device's param value), because that is what the widget wrote
  before reporting. The envelope append happens *after* the snapshot is taken,
  so the snapshot contains the envelope as it was before the pass — which is
  what "undo the recording" has to mean.
- One subtlety worth a comment in the code: the snapshot is of the whole
  session, so an automation pass that spans several seconds and touches one
  clip still costs one text serialization at its start. That is the same cost a
  note drag pays today.

---

## 6. UI

### 6.1 Where the lane goes

The piano roll's bottom lane becomes a **chooser**, exactly as in Live: it shows
either the velocity stems it shows today, or one envelope. Not both, and not a
stack.

The rejected alternative — a third lane below velocity — loses on vertical
space (the roll already caps the lane at 32% of its height) and on the fact
that the user is editing one thing at a time. The rejected alternative *two* —
an overlay drawn on the note grid — loses because the value axis and the pitch
axis are unrelated and superimposing them makes both unreadable.

```
┌─ ruler ───────────────────────────────────────────── loop: 4 ─┐
│                                                                │
│  keys │            note grid                                   │
│       │                                                        │
├───────┼────────────────────────────────────────────────────────┤
│ ▾ Sat │  ╭──────╮                                              │  <- lane
│ ▾Drive│ ╱        ╲___________                                  │
└───────┴────────────────────────────────────────────────────────┘
```

The lane's key block (today a static "VEL" label, `laneKey` in
`pianoroll.cpp`) becomes two stacked `Ui::selector`s: **device** (with
"Clip / Velocity" as entry 0, then each device on the track) and **parameter**
(the mixer fields for entry 0's sibling, or the chosen device's controls).
`selector` is used rather than a new dropdown widget because the codebase has no
dropdown and inventing one is a separate piece of work; click cycles forward,
right-click cycles back, which is already the documented idiom.

### 6.2 Interaction

Deliberately the same verbs as notes, because a user who has learned the note
grid should not have to learn a second editor eight pixels below it:

| gesture | effect |
|---|---|
| click empty lane | add a breakpoint at the grid-quantized beat, value from the cursor's height |
| drag a breakpoint | move it in beat and value; beat is clamped strictly between its neighbours so the list stays sorted by construction |
| Shift+drag | unquantized beat |
| Ctrl+drag | value only, beat frozen |
| right-click / double-click a breakpoint | delete |
| Delete | delete the selected breakpoint |
| ← → ↑ ↓ | nudge the selected breakpoint by one grid step / one `kAutoValueEps`×16 |
| Escape | cancel the drag in flight |
| wheel / Shift+wheel / Ctrl+wheel | shared with the grid — the lane uses the roll's `scrollX_` and `zoom_`, so the two axes can never drift apart |

Selection in the lane is **a single breakpoint** in v1, not a set. The note
grid's set machinery (`sel_`, `primary_`, band select) is genuinely more code
than the lane needs to be useful, and the index spaces are different, so
sharing it would be a merge rather than a reuse. Multi-select is §10.

### 6.3 Drawing

- The polyline through the breakpoints in `pal::accent`, with a low-alpha fill
  down to the lane's floor (Live's look, and it is the thing that makes a
  glance tell you the shape).
- Breakpoints as 5 px squares; the selected one filled, the rest outlined.
- A dashed horizontal rule at the target's default value (`ParamInfo::def`, or
  the fader's 0.85, or pan centre), because "where is unity" is the question
  the eye asks first.
- The value readout at the left of the lane, formatted with `ParamInfo::unit`.
- An **empty lane draws a flat line at the target's current model value** and
  nothing else. Clicking it inserts the first breakpoint. A lane with one
  breakpoint is a legal constant envelope, not an error.
- An **overridden** lane draws desaturated with a small "RE-ENABLE" button in
  its key block. An **inert** lane (`kAutoInert`, §3.4) draws desaturated with
  a tooltip naming the backend that refused.

### 6.4 Finding automation you cannot see

Three affordances, because an envelope on a parameter three devices deep in a
clip nobody has selected is otherwise invisible:

- the parameter entries in the lane's selector carry a dot when they have
  points;
- the device knob in the DEVICES tab draws its arc in `pal::accent` when
  automated and in a warning colour when overridden;
- the clip slot in the session grid carries a small envelope glyph.

### 6.5 The seam with `PianoRoll`

`PianoRoll` is documented as knowing nothing about engine or plugin types, and
that must survive. The parameter *names, ranges and units* come from the
track's devices, which it must not see. So the caller passes a plain view:

```c++
// Built by App::drawClipDetail each frame from the selected track's devices.
// Strings and floats only -- no PluginInstance, no ParamInfo, no DeviceModel --
// which is what keeps PianoRoll compilable against app.h alone.
struct AutoTargets {
    struct Entry {
        std::string group;      // "Clip" or the device's display name
        std::string label;      // "Volume", "Drive", ...
        std::string address;    // canonical, what a lane stores
        std::string unit;
        f32 lo = 0.f, hi = 1.f, def = 0.f;
        bool automated = false; // has points in this clip -- drives the dot
    };
    std::vector<Entry> entries;
};

bool PianoRoll::draw(Ui&, const Rect&, ClipModel&, const AutoTargets&,
                     f64 playheadBeats, bool playing);
```

**Factor the lane into `src/ui/autolane.{h,cpp}`** (`class AutoLaneView` with
its own `draw(Ui&, const Rect&, ClipModel&, const AutoTargets&, ...)`) rather
than growing `pianoroll.cpp`. Not for tidiness: an *audio* clip has no piano
roll and wants exactly this lane under its waveform (§10), and a lane that was
born inside the roll would have to be extracted then, with a live editor's
state to carry across.

---

## 7. Persistence: format v5

### 7.1 The shape

```
  clip 0
    uid 31
    kind midi
    name Bassline
    ...
    note 0 0.25 36 100
    env t:7/dev:12/p:3
      pt 0 200
      pt 2 4000 0
      pt 4 200
    endenv
    env t:7/vol
      off
      pt 0 0.85
      pt 3.5 0.2
    endenv
  endclip
```

- `env <address>` opens a block; `endenv` closes it back to the clip.
- `pt <beat> <value> [curve]` — the curve byte is written only when non-zero,
  the same sparseness discipline `prob`/`follow`/`send` use, and for the same
  round-trip reason: the value a missing field loads as is exactly the value
  that suppresses it.
- `off` marks `enabled == false`. Written only when false.
- Envelope blocks come **after** the notes, so a clip block still reads
  header-then-content and a clip with 400 notes and 3 envelopes shows its
  settings at the top.
- `env` is **not** gated on clip kind. Notes are (an audio clip has nowhere to
  keep them), but an envelope is meaningful on both kinds — the reader must
  accept one inside an audio clip today so that wave 8's audio-clip lanes are a
  UI change and not a format change.

### 7.2 Clamps and the structure/value split

Symmetric on save and load, as everywhere in `project.cpp`:

```c++
f64 clEnvBeat(f64 v)  { return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }
f32 clEnvValue(f32 v) { return std::isfinite(v) ? v : 0.f; }   // == clParam
int clCurve(int v)    { return clampv(v, 0, 255); }
```

The address is the interesting case, and it splits exactly the way `send`'s
index and level already split:

- **Syntactically malformed** (`parseAddress` returns false) → `return fail(...)`.
  It is structure, there is no right answer for what it meant, and repairing it
  would silently move somebody's automation onto a different parameter.
- **Syntactically valid but naming a uid that does not exist** → **kept**, and
  written back unchanged on the next save. This is `PARAM-ADDRESS.md`'s "dangling
  addresses resolve to nothing and must fail soft", and it is the same promise
  `ClipModel::path` makes for a missing sample and `DeviceModel::lostParams`
  makes for a missing plugin. Losing a filter sweep because the user opened the
  set on a machine without the plugin would be the worst bug this feature can
  have.

### 7.3 Ordering and round-trip stability

Lanes are written in model order and points in vector order. Neither end sorts —
the same rule and the same reasoning as `note`: the editor holds the
sorted-by-beat invariant, the writer emits what it is given, the reader
preserves what it finds, so a hand-shuffled file still round-trips
byte-identically and merely loads as a session whose vectors are unsorted.

An **empty lane is dropped on save**. A lane with an address and no points is UI
state (the user picked a parameter in the chooser and has not drawn anything),
not content. Dropping it is round-trip stable because nothing reads it back.

### 7.4 Version discipline

`kFormatVersion` 4 → **5**. `kMinFormatVersion` stays 1, and there is still one
parser: `env` is a new key with a default (absent = no envelopes), so a v1..v4
file loads with empty envelope vectors and re-saves with only the header line
changed. That property — "a set that uses none of the new constructs saves
exactly the bytes the previous version saved apart from the header" — is the
one the three previous version bumps each state explicitly, and it must be
asserted by a test, not assumed.

New parser state `St::Env`, opened from `St::Clip`, closing back to `St::Clip`.
An unexpected key inside `env` fails the load, like every other block.

---

## 8. Daemon composition

### 8.1 Who evaluates: the engine, in both topologies

**Automation is evaluated inside `Engine::process()` and nowhere else.** The
in-process build and the split build run the same code against the same
`RtAutoSet`; the daemon's only extra job is turning a pool offset into a
pointer. That is the whole of the compatibility story, and it is the reason
§3 is written against `Engine` and not against `App`.

The rejected alternative deserves naming because it is superficially attractive:
the param table (§3.7 / §11.4 of PROCESS-SPLIT.md) already carries values from
the client to `setParam` at a 1 ms cadence, and automation is "just values". It
would be wrong three times over — the client does not own the beat clock, so
every value would be stamped with a position it inferred; 1 ms of pump jitter
would land on the audio as timing error; and a GUI hiccup or a GUI *crash*
(§4.3, the scenario the whole split exists for) would stop the automation while
the audio kept playing. **The param table stays the human's path. Automation is
the clock's path, and the clock is in the engine.**

### 8.2 Crossing: a pool block, not a table

Envelopes ride the sample pool as a new block kind, for the three reasons
§11.2 gives for strings and which apply here verbatim:

```
PoolKindAutomation = 4        // pool version 3
```

The block's contents are the *same single-allocation layout* the in-process side
uses — a header, then `WireAutoLane[laneCount]`, then `WireAutoPoint[pointCount]`
— which is what lets the daemon hand `(const RtAutoSet*)(poolBase + autoRef)`
to the engine with no copy and no translation pass, exactly as `WireNote` does
for notes. `static_assert`s mirroring `RtAutoPoint`/`RtAutoLane` field for field
are what make that cast honest; if the engine's structs change, the daemon stops
compiling instead of quietly reinterpreting an envelope.

**Retirement is free.** An automation blob is a pool block, so free-after-confirm
(§10.3) applies with no new states and no new rules, and it retires on the
clip cell's generation exactly as `notesRef` does. This is the whole argument
for the pool over a control-region table: no new validator, no new budget, no
new lifecycle.

### 8.3 `WireClip` grows, and the protocol version moves

```c++
struct WireClip {
    // ... existing ...
    u64 autoRef;         // pool offset of the automation blob, 0 = none
    i64 autoLaneCount;
    i64 autoPointCount;
};
```

`sizeof(WireClip)` goes 120 → 144, the region layout hash moves, and the
protocol version goes 3 → **4** with pool version 2 → **3**. A phase-3 binary
and a phase-4 binary then refuse each other at `attach()` with a specific
message, which is the mechanism working as designed. The `static_assert` on
`sizeof(WireClip)` must be updated in the same commit — it exists precisely so
this cannot happen by accident.

### 8.4 Validation

One new reject reason, `RejectBadAutomation = 15`, and the validation happens
in the same place every other untrusted `u64` is checked:

1. `poolValidate(autoRef, PoolKindAutomation, needBytes)` where `needBytes` is
   computed from the *declared* `autoLaneCount` / `autoPointCount` in the
   `WireClip` cell — so a blob smaller than its own declared contents is caught
   before a pointer exists, not after.
2. `laneCount <= kMaxRtAutoLanes`, `pointCount <= kMaxClipAutoPoints`.
3. Every lane: `first >= 0`, `count >= 0`, `first + count <= pointCount`,
   `target` in range, `devSlot` in `[-1, kMaxChainFx)`, `lo <= hi`, both finite.
4. Every point: `beat` finite and `>= 0`, `value` finite.

A malformed set **refuses the whole `SetClip`**, it does not silently strip the
automation — consistent with `RejectBadClip`, and for the same reason: a client
that sent something impossible should be told, not partially obeyed.

### 8.5 Two writers on one plugin parameter, across a process boundary

In the split, the pump thread calls `setParam` (the human's path) and the audio
thread calls `setParamRT` (the clock's path), on the same instance, potentially
on the same parameter. Resolution:

- LV2 and internal: both are plain aligned stores; last writer wins; the value
  is always one of the two written. This is the property `host.h` already
  relies on, with one more writer.
- CLAP: two disjoint queues merged in `buildEventList()` with the realtime
  array emitted last, so automation wins within a block (§3.4).
- In normal operation they are not both writing, because override (§3.6) means
  the user's hand and the envelope are never in charge of the same parameter at
  the same time. The above is what happens in the window where they are.

### 8.6 Recording across the split

Nothing new. The GUI records into its own `ClipModel` and republishes the clip;
the republish carries the new envelope blob through the existing `SetClip` /
`EvClipAck` path. The beat it stamps against comes from `SharedState`'s mirrored
`clipPhase`, which already crosses.

---

## 9. Phasing and test strategy

Five waves, each independently shippable, each with a gate that fails loudly.

### 7a — model, addresses, persistence, evaluator (no engine)

Ships: `AutoPoint`/`AutoLane`/`ClipModel::envelopes`; `src/core/address.{h,cpp}`;
the `send:` grammar addition to PARAM-ADDRESS.md; format v5 read and write;
`autoValueAt()` and the Rt structs in `engine.h` (compiled, not yet used).

**Gate — `tests/automation_test.cpp` (new):**
- `parseAddress`/`formatAddress` round-trip over every form in the grammar,
  plus a table of malformed inputs that must all return false.
- Evaluator: before the first point, after the last, exactly on a point, mid
  segment, single point, zero points (returns the fallback), a point at beat 0,
  a point at `lengthBeats`, unsorted input (defined, not crashing).
- Format: save → load → save is **byte-identical** for (a) a set with no
  envelopes, which must equal its v4 bytes apart from the header line, (b) a set
  with several lanes and hundreds of points, (c) a set with a dangling address,
  which must come back unchanged.
- A malformed address in a file fails the load and leaves the caller's session
  untouched.

### 7b — engine application, class A

Ships: `RtClip::autos`, `Ev::AutosRetired`, `App::publishAutos`, the automation
pass, the ramped mixdown, `Track::autoA`.

**Gate — the deterministic render, in `tests/engine_test.cpp`:**

Build a 4-beat MIDI clip on track 0 with a `t:<uid>/vol` envelope from 0.0 at
beat 0 to 1.0 at beat 4. Render 8 bars at 48 kHz / 256 frames through the
existing headless harness with a known-amplitude internal generator, then:

1. **Match the oracle.** Per-beat RMS must track `faderToGain(autoValueAt(...))`
   at the beat's midpoint within 1e-4. The oracle is the same function the
   engine called, which makes this a test of *application*, not of arithmetic —
   which is the correct division, because the arithmetic is tested in 7a.
2. **Bit-identity across runs.** Two renders at the same block size must be
   byte-identical. Automation must not introduce a source of nondeterminism.
3. **Bounded divergence across block sizes.** A render at 64 frames and a render
   at 256 frames must agree within 1e-3. They will *not* be bit-identical, and
   that is a real, documented property of block-boundary evaluation with an
   intra-block ramp: the ramp approximates the envelope with a different
   piecewise-linear fit at each block size. Every host that does not evaluate
   per sample has this property. The test pins the tolerance so a regression
   that makes it worse is caught.
4. **The loop wrap.** Eight bars over a 4-beat clip means seven wraps; assert
   the envelope restarts at each and that no wrap produces a sample outside
   `[min(env), max(env)]` (i.e. no overshoot from a ramp that spanned the wrap).
5. **Retirement.** Republish the clip 100 times while it plays; assert every
   `RtAutoSet` came back through `Ev::AutosRetired` and none leaked (run under
   ASan, as `daemon_test` already is).

### 7c — device parameters

Ships: `setParamRT` on `host.h` and all three backends, the `getParam`
realtime-safety note, `Track::autoHold` and restore-on-stop,
`Ev::AutoLaneInert`.

**Gate:**
- **Equivalence.** Feed a DC input of 0.2 through the internal Saturator with a
  `Drive` envelope. Render it twice: once automated, once with the test itself
  calling `setParam` at the value the envelope has at each block's start beat.
  The two renders must be **bit-identical**. That is the strongest available
  statement that `setParamRT` and `setParam` are the same operation.
- **Restore.** Set `Drive` to a known value by hand, play the automated clip,
  stop it, assert `getParam` returns the hand-set value. Repeat for: clip
  changed under the voice, lane overridden, transport stopped, `Cmd::SetChain`
  replacing the chain mid-envelope (the case §3.5 warns is easy to forget).
- **Inert.** A stub `PluginInstance` that does not override `setParamRT` must
  produce exactly one `Ev::AutoLaneInert` per published set and no audio change.
- CLAP specifically, in `tests/internal_device_test.cpp` against
  `fake_clap_plugin.cpp`: a knob write and an automation write in the same block
  must both reach the plugin, with the automated value last.

### 7d — recording and the UI lane

Ships: `AutoLaneView`, the chooser, `AutoTargets`, Automation Arm, capture,
thinning, punch, the override affordances.

**Gate — `LATTICE_DEBUG_AUTOMATION`, a headless self-test in the shape of
`debugUndoSelfTest`:** nothing can drag a fader inside gamescope, and this is
the part a screenshot cannot check. Against a loaded set with a live engine:
- synthesize a linear gesture over 4 beats; assert the resulting lane thins to
  exactly 2 points, and that `autoValueAt` over it matches the gesture within
  `kAutoValueEps` at 100 sample beats;
- synthesize a stepped gesture; assert point count is within 10% of the number
  of distinct steps;
- assert exactly one undo entry was taken for the whole gesture and that undo
  restores the pre-gesture envelope exactly (compare serialized text);
- assert a second pass over the same span replaces rather than merges;
- assert a gesture with no clip playing writes nothing and latches the hint
  once.

### 7e — the daemon

Ships: `PoolKindAutomation`, `WireAutoSet`/`WireAutoLane`/`WireAutoPoint` with
their mirror asserts, `WireClip` growth, protocol v4 / pool v3,
`RejectBadAutomation`, client-side publish and retirement.

**Gate — `daemon_test` §16:** render the *same* automated set twice — once
through an in-process `Engine`, once through a spawned `latticed --driver null`
— and assert the two outputs are **bit-identical**. That is the single
assertion that proves "engine-side evaluation works identically in-process and
split", which is the whole claim of §8. Plus: a table of malformed blobs (bad
`first + count`, a lane past `kMaxRtAutoLanes`, a NaN beat, a blob shorter than
its declared size, an offset into the middle of another block) each answered
with `RejectBadAutomation` and each leaving `/dev/shm` clean.

---

## 10. Deliberately deferred

Named, with what unblocks them, so nobody has to guess whether they were
forgotten:

- **Arrangement lanes.** The model admits them (§2.6); they need a second
  publish path and an arrangement view worth automating into. The format
  reserves nothing for them on purpose — a v6 `automation <address>` block at
  top level is additive, and reserving a keyword that no parser accepts would
  only produce a file the current reader rejects.
- **Curve shapes.** The byte is reserved and round-trips. The evaluator and the
  lane view both treat non-zero as linear.
- **Clip gain automation** (`AutoTarget::ClipGain`). The enum value is reserved.
  It needs `renderRange` to take a per-sample gain, which is the one edit in §3
  that touches the voice renderer, and it is not worth taking in the same wave
  as everything else.
- **Master and return automation.** `MasterVol`/`ReturnVol` reserved; blocked on
  the same scope question as §11's item 2.
- **Audio-clip envelopes.** The lane is factored out for exactly this (§6.5);
  what is missing is a host for it in the audio clip's detail panel.
- **Multi-select in the lane**, and box-select over breakpoints.
- **Sub-block-accurate device automation.** CLAP can do it today with a frame
  field on `rtParams_[]`; LV2 and internal cannot without splitting `process()`.
- **Modulation** (LFOs, envelope followers, macro knobs). A different feature
  that happens to write to the same addresses; nothing here forecloses it.

---

## 11. Open questions for the orchestrator

These are contract calls, not implementation details. Each one changes what a
wave-7 agent writes.

1. **`engine.h` and `host.h` are being edited.** `engine.h` was frozen for the
   daemon waves and the daemon builds against it; this design adds `RtClip::autos`,
   the Rt automation structs, `Ev::AutosRetired`, `Ev::AutoLaneInert` and fields on
   the private `Track`. `host.h` gains `setParamRT` and a realtime-safety note on
   `getParam`. Both need a slot in the schedule where the daemon is not
   mid-flight against the old shape. **Which wave owns the header edits?**

2. **Clip-envelope scope is restricted to the clip's own track** (§4.2 step 2).
   This forbids "the clip on track 1 automates the master filter" and "…automates
   track 4's send". Live allows the latter only through arrangement automation,
   which is the reasoning, but the restriction is a product decision and it is
   cheap to lift for mixer targets (harder for device targets, which would need
   the engine to reach into another track's chain during that track's block —
   an ordering hazard). **Confirm the restriction, or lift it for mixer targets
   only?**

3. **Block-size-dependent renders** (§9, gate 3). An automated set does not
   render bit-identically at 64 and 256 frames; the divergence is bounded at
   1e-3. Accepting this is standard; refusing it means per-sample envelope
   evaluation, which the sub-block splitter could be extended to do at real
   cost on every automated track. **Accept the tolerance, or require per-sample?**

4. **The `send:` addition to the address grammar** (§4.1). `docs/PARAM-ADDRESS.md`
   has no send segment and one is required. Proposed: `t:7/send:2`, indexed
   because returns are a fixed array. **Confirm the spelling** — it is in the
   file format from v5 on and is therefore not cheaply changed later.

5. **Value domain: model units + `AutoXform`, not normalized 0..1** (§2.3).
   This is the decision with the longest tail — it is in the file, on the wire,
   and in every test oracle. **Confirm.**

6. **Protocol v4 / pool v3** (§8.3). `WireClip` grows, the layout hash moves,
   and phase-3 and phase-4 binaries stop talking to each other. That is a
   deliberate incompatibility on a shipped protocol. **Confirm it lands as its
   own daemon wave (7e) rather than being folded into an earlier one.**

7. **Override state is runtime-only** (§3.6): not saved, not undoable, cleared
   by clip relaunch and by transport stop. **Confirm**, especially "cleared by
   relaunch" — it is the one place a user could reasonably expect the opposite.

8. **Bounds:** `kMaxClipLanes = 16`, `kMaxClipAutoPoints = 4096`,
   `kMaxRtParams = 64` (CLAP). All are "a human cannot reach this" numbers.
   **Confirm, or name different ones now** — `kMaxClipLanes` is the width of a
   fixed array in a wire type and is not free to change later.

9. **The lane replaces the velocity lane** rather than stacking below it
   (§6.1). Live's choice, and the one the roll's existing geometry supports.
   **Confirm.**

10. **Automation Arm is its own control**, independent of the transport record
    button (§5.1). The alternative — record-arm implies automation-arm — is
    fewer controls and more surprises. **Confirm**, and say where it goes on
    the control bar.
