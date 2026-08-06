# The GUI adopts the daemon — splitting phase 4

Design note, wave 7. Scope: `src/ui` stops owning an `Engine` and starts
talking to `latticed` through `ipc::EngineClient`. This is §9-step-4 /
§6-phase-4 of `docs/PROCESS-SPLIT.md`, which that document sizes as one
step ("point the GUI at `EngineClient`"). It is not one step. This note
enumerates the actual surface, cuts it into seven, and says which parts
have no protocol behind them yet.

Nothing here is written to the repo; it is a plan for the agents who will.

---

## 0. Where the boundary actually is today

`docs/PROCESS-SPLIT.md` §2 enumerated the boundary as it existed before
phases 1–3. Phases 1–3 moved the *daemon* to the far side of a pointer-free
protocol; they did not move the GUI. So the boundary the GUI has to cross is
still the in-process one, and it is bigger than §2's table, because §2 counted
message types and the GUI's coupling is not made of message types. It is made
of **72 call sites in `src/ui/app.cpp`**, in seven categories.

The far side is done and tested (300 checks in `tests/daemon_test.cpp`
against a real spawned daemon). The near side has never had a caller:
`EngineClient` has no user outside the test. That asymmetry is the single
most important fact about sizing this work — every step below is "write the
client half of something whose server half already passes tests", which is
the good case, except where §7 says otherwise.

---

## 1. The complete call-site census

Line numbers are `src/ui/app.cpp` at `6db2564`. This is the checklist; a
migration is done when every row is struck out.

### 1.1 Commands (GUI → engine) — 7 sites, 3 shapes

| line | site | shape | daemon status |
|---|---|---|---|
| 268–272 | `App::send()` | the funnel: 17 scalar `Cmd`s | **accepted** (`commandIsScalar`) |
| 283–351 | `pushClip()` | `Cmd::SetClip` carrying an `RtClip` with `data`/`notes` pointers | needs the **pool + clip table** |
| 390–409 | `releaseStaleSlots()` | `Cmd::ClearClip` | accepted (pooled class) |
| 537–571 | `publishChain()` | `Cmd::SetChain`/`SetReturnChain`/`SetMasterChain` with an `RtChain*` | **refused permanently** (§11.7.6) — deleted, not ported |
| 867 | `releaseAllChains()` | the same three, with a null chain | same — deleted |
| 1636 | `startRecording()` | `Cmd::RecordSlot`/`RecordMidiSlot` with a GUI buffer | **refused, no replacement protocol exists** |
| 1661 | `stopRecording()` | the same, as the stop toggle | same |

`send()` is the good news: seventeen of twenty commands already cross, and
they all funnel through four lines. Of the rest, two shapes are protocol
work that is *finished on the daemon side* (clips, devices) and one is
protocol work that does not exist (recording, §7).

### 1.2 Polled atomics (engine → GUI) — 20 sites

Every one is `engine_.<member>.load()`, read during a draw:

| line | member(s) | reader |
|---|---|---|
| 1431 | `playing` | `togglePlay()` |
| 1595 | `recState[t]` | `startRecording()` guard |
| 1814 | `beat` | `finishMidiRecording()` |
| 2293, 2311, 2322, 2347 | `playing`, `recState[]`, `beat`, `cpu` | `drawControlBar()` |
| 2608–2610 | `slotState[]`, `activeSlot[]`, `pendingSlot[]` | `drawClipSlot()` |
| 2616–2617, 2627 | `recState[]`, `recSlotIdx[]`, `beat` | `drawClipSlot()` |
| 2709 | `clipPhase[]` | `drawClipSlot()` |
| 2893 | `meterL[]`, `meterR[]` | `drawMixer()` |
| 2983 | `returnMeterL[]`, `returnMeterR[]` | `drawReturnStrips()` |
| 3047 | `masterMeterL/R` | `drawMasterStrip()` |
| 3285–3286 | `activeSlot[]`, `clipPhase[]` | `drawClipDetail()` |
| 3661 | `beat` | `drawStatusBar()` |
| 3686 | `latencyFrames` | `drawStatusBar()` |

**Two of these have no home in `SharedState`.** `SharedStateT` (`src/ipc/shm.h`)
publishes `beat tempo playing cpu sampleRate blockSize slotState activeSlot
pendingSlot clipPhase meterL meterR masterMeterL masterMeterR recState
recSlotIdx` — and **not** `returnMeterL/R` and **not** `latencyFrames`. Both
landed in `Engine` during wave 6 (returns, PDC) after `SharedStateT` was
written in wave 1. So the return meters and the PDC readout are the first
concrete regression the daemon path would ship with, and closing it is a
`kShmVersion` bump (2 → 3) plus a layout-hash move. Cheap, but it edits
`src/ipc/shm.h`, `src/audio/engine.cpp`'s `publish()` and the daemon's
mirror, and it invalidates every prebuilt binary at once. Do it first, alone.

Also worth publishing while that file is open, per §10.8: the mixer scalars
(`vol/pan/mute/solo/arm`, send levels, return levels). Nothing reads them
today, but §4.3 step 5's reattach does, and adding a field later costs the
same version bump again.

### 1.3 `pushMidi` — 8 sites, and a latent bug

| line | site |
|---|---|
| 222 | `shutdown()` — `kbd_.allNotesOff` |
| 2114 | `updateKbdPiano()` |
| 2147 | `toggleKbdMidi()` — all-notes-off on the way out |
| 2182–2183, 2190, 2193 | `startPreview()` — steal-oldest + the new on |
| 2211 | `updatePreviews()` — the offs that came due |
| 2220 | `stopPreviews()` |

All eight are GUI-thread and map one-for-one onto
`EngineClient::pushMidi(status, d1, d2, frame)`. The piano-roll audition path
is therefore *free*: it is already a MIDI-ring push and nothing about it
changes.

**The bug:** `MidiInput` (`src/audio/midi_in.h`) runs a reader thread whose
only job is `Engine::pushMidi()`. So `Ring<MidiMsg,1024>` has **two
producers** today — the ALSA reader thread and the GUI thread — and
`lat::Ring` is documented single-producer, with a plain relaxed load of `w_`
on the producer side. Two concurrent pushes can write the same slot and
publish one index. It is rare (you have to play the computer keyboard and a
hardware controller in the same microsecond) and it is real.

`EngineClient` inherits the same contract verbatim ("pushMidi may be a
*different* single thread from pushCommand" — one, not two). So the migration
must resolve it rather than reproduce it. Three options:

1. **Move `MidiInput` into the daemon.** Correct end state (§7 open
   questions already says MIDI input "lands in the daemon"), and it removes
   the ring push from the GUI's reader thread entirely. Costs: the daemon
   grows an ALSA seq dependency, and `drawStatusBar` has to learn the client
   id from `ControlHeader` instead of `midi_.clientId()`.
2. **Hand off through a GUI-owned SPSC queue**: the reader thread pushes into
   a `Ring<MidiMsg>` the GUI drains once per frame and forwards. One frame of
   added latency on hardware MIDI — unacceptable for playing an instrument.
3. **A mutex around the two producers.** Correct, non-realtime on both sides
   (neither producer is the audio thread), ~10 lines.

Recommend (3) *now* as a bug fix independent of this work, and (1) as part of
the lifecycle step, because a daemon that keeps playing after a GUI crash
must keep answering the keyboard too.

### 1.4 Sample-rate query — 7 sites

`engine_.sampleRate()` at 585, 795 (`instantiate`), 1037, 1200
(`loadProject`), 1439 (`loadSample`), 1620 (record capacity), 1695
(`sampleFromRecording`). Replacement is `client.sampleRate()` reading
`SharedState::sampleRate` — but with a sequencing constraint the in-process
build does not have:

> **The GUI must not decode anything before the handshake.** `loadSample()`
> resamples to the engine rate. Detached, `state()` returns a zeroed block and
> `sampleRate()` is `0.0`. A project on the command line (`init()`, line 162)
> currently loads *after* `createBackend()` has prepared the engine; in daemon
> mode it must load after `attach()` succeeds.

So `init()` grows an ordering: spawn/attach → read rate → create pool →
decode. And a rate *change* (JACK reconfigured under a running daemon) needs
`Ev::FormatChanged` and a re-resample pass; §2.6 called for this and no such
event exists yet. Ship without it — a rate change mid-session currently
detunes the in-process build too — but log it loudly.

### 1.5 Events (engine → GUI) — 1 site, 5 types

`pumpEngineEvents()` (411–478) handles `ChainRetired`, `RecordStarted`,
`RecordFinished`, `MidiRecordFinished`, `NotesRetired`.

Of these, **`ChainRetired` and `NotesRetired` disappear from the GUI
entirely** — the daemon consumes both as retirement proofs (§11.5), and
`App::retiring_`, `published_[]`, `publishedReturn_[]`, `publishedMaster_`,
`publishedNotes_[][]`, `retiringNotes_` and `RetiredChain` all get deleted.
That is roughly −250 lines of the hardest ownership code in the app, and it
is the largest single win of the whole migration.

What replaces them is a *different* set the client must handle:
`EvClipAck`, `EvBlockRetired`, `EvDeviceAdded/Failed/Removed/Changed`,
`EvScanComplete`, `EvPoolAttached`, `EvCommandRejected`, `EvEngineStopping`.
`EngineClient::popEvent()` already does the bookkeeping for the first two
kinds; the GUI has to react to the rest (status text, device state, banners).

The three record events have no daemon-side equivalent (§7).

### 1.6 Direct `PluginInstance` use — 6 sites, and this is the metadata mirror

| line | site | calls |
|---|---|---|
| 725–728 | `serializeDevices()` | `paramCount()`, `paramInfo(i).id`, `getParam(i)` |
| 817–825 | `materializeDevices()` | `paramCount()`, `paramInfo(i).id`, `setParam()`, `setBypassed()` |
| 1348–1365 | `debugUndoSelfTest()` | all four, on device 0 of the first track that has one |
| 3512 | `drawDeviceStrip()` | `setBypassed()` |
| 3555, 3567 | `drawDeviceStrip()` | `paramCount()` (the "N params" label, the knob loop bound) |
| 3586–3606 | `drawDeviceStrip()` | `paramInfo(p)`, `getParam(p)`, `setParam(p, v)` — the knobs |

Cross-process this is exactly two mechanisms, both shipped:

- **static metadata** → `DeviceMirror` / `ParamMirror` from
  `EngineClient::readDevice(id)`, filled from the daemon-written
  `WireDeviceInfo` row. `paramInfo(i)` → `mirror.params[i]`. Note the caps:
  **64 params per device** (`kMaxDevParams`), with the overflow reported in
  `truncatedParams` — the knob grid must draw "…and N more controls this
  build cannot reach" rather than silently showing 64 of 300.
- **values** → `setDeviceParam(id, i, v)` / `deviceParam(id, i)` on the param
  table. Drop-free by construction, which is exactly what a knob drag needs.

`getParam()` and `setParam()` are *not* symmetric across the boundary the way
they are in-process. In-process, `setParam` then `getParam` returns what you
wrote, because it is the same float. Across the table, `deviceParam()` reads
the **client's own** slot in the param table — the daemon does not currently
mirror `getParam()` back (§11.9), so a plugin that moves its own controls
will not be seen. For the generic knob UI that is fine (the client is the only
writer). It stops being fine the day presets or native UIs land, and the
field (`engineGeneration`) is already there for it.

`setBypassed()` is **not** a param write: it is `Cmd::SetBypass`, a command,
because it has to order against chain edits (§3.7). One-line change, easy to
get wrong by reflex.

### 1.7 `PluginRegistry` — 5 sites, and the one thing with no protocol

| line | site |
|---|---|
| 176, 197 | `init()` — `LATTICE_DEBUG_ADDFX` / `MASTERFX` scan the list by name |
| 585 | `addDevice()` — `registry_.instantiate(desc, rate, kMaxBlock)` |
| 654, 657 | `ensurePluginScan()` — `scan()`, `plugins().size()` |
| 794–795 | `materializeDevices()` — `find(uri)`, `instantiate()` |
| 3366 | `drawPluginBrowser()` — `registry_.plugins()` is the whole browser list |

`instantiate()` and `find()` are answered by `EngineClient::addDevice(target,
idx, pos, uri)`. **`plugins()` is not answered by anything.** §11.9 states it
plainly: "The catalog is scanned in the daemon and is currently only reachable
one URI at a time — a GUI browser needs the socket or a catalog table."

See §3 for the design. This is the one place where phase 4 must *add* protocol
rather than consume it.

### 1.8 Construction and the backend — 3 sites

Lines 125–135: `createBackend(engine_, …)`, `engine_.prepare(48000, 1024)`,
`midi_.start(engine_)`. In daemon mode all three go away; the GUI opens no
audio device at all. Consequence for `drawStatusBar` (3690–3698): it prints
`audio_->name()`, `audio_->sampleRate()`, `audio_->bufferSize()` and
`midi_.clientId()`. All four must come from the wire instead —
`ControlHeader::driver` (already published), `SharedState::sampleRate` /
`blockSize` (already published), and the MIDI client id (not published; add it
with the §1.2 version bump).

---

## 2. The seam: `EngineHandle`

### 2.1 What shape

Three candidates, and the choice matters more for the *view code* than for
the engine code.

**(a) A virtual `IEngine` with `LocalEngine` / `RemoteEngine`.** Idiomatic,
and wrong here: the dominant call class is the twenty polled atomics, and a
virtual getter per atomic field is twenty virtuals called ~3 000 times a
frame, plus twenty pairs of near-identical one-line overrides to keep in sync.
The abstraction is paying for the wrong axis.

**(b) A concrete `EngineHandle` holding both.** One class, no vtable, both
paths compiled in, `if (client_) … else …` at each entry. Matches the
"`LATTICE_ENGINE=inproc|daemon`, default `inproc`" ship plan §6 asks for, and
the branch is on a member that never changes after `init()`, so it predicts
perfectly.

**(c) (b), plus a per-frame state snapshot.**

Recommend **(c)**, and the snapshot is the load-bearing half:

```c++
// src/ui/engine_state.h — pure data, no atomics, no engine, no ipc.
struct EngineState {
    f64 beat = 0, tempo = 120; bool playing = false;
    f32 cpu = 0; f64 sampleRate = 48000; u32 blockSize = 0;
    i32 latencyFrames = 0;
    i32 slotState[kMaxTracks], activeSlot[kMaxTracks], pendingSlot[kMaxTracks];
    f64 clipPhase[kMaxTracks];
    f32 meterL[kMaxTracks], meterR[kMaxTracks];
    f32 returnMeterL[kMaxReturns], returnMeterR[kMaxReturns];
    f32 masterMeterL = 0, masterMeterR = 0;
    i32 recState[kMaxTracks], recSlotIdx[kMaxTracks];
};

class EngineHandle {
public:
    bool openLocal();                       // Engine + backend + MidiInput
    bool openDaemon(const char* session);   // reap, spawn, attach, pool
    void poll(EngineState& out);            // once per frame, top of frame()

    bool send(Cmd t, i32 a=0, i32 b=0, f64 x=0.0);   // 17 scalars
    bool setClip(int t, int s, const ClipModel&);    // pool + table, or RtClip
    bool clearClip(int t, int s);
    bool pushMidi(const MidiMsg&);
    // devices
    bool addDevice(int owner, const std::string& uri);
    bool removeDevice(int owner, int idx);
    bool setBypass(u32 dev, bool);
    bool setParam(u32 dev, u32 index, f32 v);
    const DeviceMirror* device(u32 id) const;
    const std::vector<PluginDesc>& catalog() const;
    // lifecycle / events
    template <class F> void drainEvents(F&&);
    EngineLink link() const;   // Detached / Starting / Live / Stale / Stopping
    f64 sampleRate() const;
};
```

Why the snapshot earns its keep, beyond killing twenty call sites:

- **It removes `Engine` from every view translation unit.** After it lands,
  `drawMixer`, `drawClipSlot`, `drawControlBar`, `drawStatusBar`,
  `drawClipDetail`, `drawReturnStrips`, `drawMasterStrip` include no engine
  header at all. That is the precondition for the `app.cpp` split
  (`appcpp_split.md`) and it is why this step should land *before* the split
  or in the same wave as its step 0.
- **It fixes a real inconsistency.** `drawClipSlot` reads `slotState`,
  `activeSlot`, `pendingSlot` and later `clipPhase` as four independent
  relaxed loads. Across a process boundary those can straddle a publish and
  disagree — a slot drawn as `Playing` with `activeSlot == -1`. Snapshotting
  once per frame against `SharedState::generation` makes the frame coherent.
  (A full seqlock is available if it ever matters; a single generation
  compare-and-retry is enough here and costs nothing.)
- It makes the views testable without an engine, an audio device, or a daemon.

### 2.2 The refused-push problem, which is new

In-process, `Ring::push` failing is a 1024-deep ring on a 4 ms drain: it
never happens, and `App::send()` (268) does not even check the return. Across
the boundary, three things can refuse:

1. the command ring is full (4096 deep, drained at 1 ms — still rare);
2. **the clip cell is un-acknowledged** — `setClip()` returns false until the
   daemon's `EvClipAck` arrives, which is up to one pump tick. A user
   dragging a clip gain fader hits this *every frame*;
3. the pool is not attached yet (`poolReady()` false).

`docs/PROCESS-SPLIT.md` §5 already names this as phase 1's outstanding
hardening debt. Concretely the GUI needs:

- **A dirty-cell set.** `pushClip(t,s)` marks `clipDirty_[t][s]` instead of
  sending; `EngineHandle::flush()` at the top of `frame()` walks the dirty set
  and retries. A cell that is refused stays dirty. This also collapses the
  common case where a fader drag marks the same cell 60 times a second into
  one write per acknowledgement — which is not a workaround, it is the correct
  rate for a table protocol.
- **A small scalar outbox.** `send()` returns false → push onto a ≤64-entry
  deque, retried in the same flush, in order. Overflow is a status-bar error,
  never a silent drop.
- **Nothing else may be optimistic.** `addDevice`'s current pattern — mutate
  the model, publish, compare pointers, roll back on failure (597–605) —
  becomes "mutate nothing, send, wait for `EvDeviceAdded`". See §5.

---

## 3. The plugin browser: a catalog table

The registry lives in the daemon. `drawPluginBrowser()` needs a list of
(name, vendor, uri, format, kind) it can filter and double-click.

Three ways, in increasing order of correctness:

**A. The GUI keeps its own `PluginRegistry` for browsing only.** It already
links `src/plugin`. `plugins()` for the list, `EngineClient::addDevice(uri)`
for the load. Zero protocol work.
*Cost:* the scan runs twice (4.3 s each here, 410 plugins), the two catalogs
can disagree (different `LV2_PATH`, different user), and — the real
objection — it keeps `src/plugin` linked into the GUI, which is the whole
thing phase 3 was for. A plugin that segfaults `lilv` during discovery would
take down the GUI it was supposed to have been isolated from.
*Verdict:* acceptable as a **one-wave bridge** so the device UI can be
exercised end to end before the table exists. Not a destination.

**B. A catalog table in the control region.** The recommended design, and it
is the device table pattern reused verbatim — which is why it is cheap:

```c++
struct WirePluginDesc {           // ~448 B
    char uri[256], name[96], vendor[64];
    u32  format;      // PluginFormat
    u32  kind;        // PluginKind
    u32  audioIn, audioOut;
    u32  hasMidiIn;
    u32  paramCount;
    std::atomic<u32> state;       // Free / Live — stored LAST, release
};
inline constexpr u32 kMaxCatalog = 2048;      // ~900 KiB, ninth region section
```

- **Daemon writes, client reads**, same direction and the same release-store
  discipline as `WireDeviceInfo` (§11.3): `state` last, so a reader that sees
  `Live` has seen every byte.
- `ControlHeader::scanState` / `scanPlugins` already exist and already say
  when the scan is running and how many it found; `EvScanComplete` already
  fires. The client's read is: on `EvScanComplete` (or on attach with
  `scanState == Done`), walk `[0, scanPlugins)` and mirror into a
  `std::vector<PluginDesc>`-shaped local list, once. A few hundred kB memcpy,
  one time.
- Overflow (>2048 plugins) is reported in a `catalogTruncated` counter and
  drawn in the browser, never silent.
- Bumps `kProtocolVersion` 3 → 4 and moves the layout hash. Nothing else.

*Sizing:* ~120 lines in `control.h`, ~60 in `latticed.cpp` (the scan already
produces exactly this data), ~40 in `client.h`, ~15 in the GUI. Half a wave.

**C. The AF_UNIX socket** (§3.2's answer). Strictly better — variable-length,
no fixed budget, and it is the same channel `memfd`/`SCM_RIGHTS` and the
out-of-process scanner need. Also three or four times the work, and it is the
one piece of the original design that four phases have managed to avoid
needing. **Defer.** B is not a detour on the way to C: when the socket lands,
the catalog moves onto it and the table is deleted, and B will have cost half
a wave to get a working browser two waves earlier.

**Recommend B**, with A permitted for exactly one wave as a bridge if the
device-UI agent would otherwise be blocked.

One consequence either way: `ensurePluginScan()` stops being a synchronous
lazy scan and becomes "send `CmdScanPlugins`, show a spinner". The daemon
scans on its own thread with the heartbeat still advancing (§11.6, asserted),
so the GUI stays live throughout — which is strictly better than today, where
the first click on the DEVICES tab freezes the UI for four seconds.

---

## 4. What stays local, forever

- **Everything that draws.** Window, `Renderer`, `Ui`, fonts, all seventeen
  `draw*` functions.
- **The session model.** `Session`, `TrackModel`, `ClipModel`, `SceneModel`,
  `ReturnModel`, `SavedDevice`. The GUI is the authority on what *exists*; the
  engine is the authority on what is *sounding* (§4.3 step 5). Keep that
  sentence; it settles most arguments about where a field belongs.
- **Project I/O and undo.** `src/core/project.cpp` never touches the engine
  and does not change. `App::adoptSession()` does, heavily — it is the
  republish path (§5.3).
- **Sample decode.** `loadSample()` stays in the GUI. Where the *bytes* end up
  changes (§5.3).
- **The browser (file), the piano roll, the keyboard piano, previews.**
  `KbdPiano` already "knows nothing about App, the window or the engine" and
  emits through a callback — swapping `engine_.pushMidi` for
  `client.pushMidi` in the two lambdas (2114, 2147, 222) is the entire change.
- **Note previews.** `startPreview` / `updatePreviews` / `stopPreviews` are
  pure `pushMidi` + a deadline in the frame loop. No change beyond the target.
  The header comment's caveat still holds and still matters: previews sound on
  *armed* tracks, so they only line up with the clip on screen because
  `selectTrack()` auto-arms. Unchanged by the split, but note that `arm` is a
  scalar command now, so the auto-arm has a millisecond of latency it did not
  have before. Inaudible; worth knowing when a preview seems to go missing on
  the very first click after a track change.

---

## 5. Migration order

Seven steps. Each leaves the tree green and shippable, each is one agent, and
steps 3/4/5 are mutually independent *after* the `app.cpp` split lands.

### Step 0 — close the `SharedState` gaps *(half a wave, risk: low)*

`returnMeterL/R[4]`, `latencyFrames`, the MIDI client id, and (while the file
is open) the mixer scalars §10.8 has wanted since phase 1. Publish them from
`Engine::publish()` directly into an optional `ipc::SharedState*`, which is
§9's step 1 and also deletes the daemon's mirror thread and its 4 ms
staleness. `kShmVersion` 2 → 3.

Touches `src/audio/engine.{h,cpp}`, `src/ipc/shm.h`, `src/daemon/latticed.cpp`,
`tests/daemon_test.cpp`. ~200 lines. Do it alone and first: it invalidates
every binary, and no other step wants to be rebasing across it.

### Step 1 — `EngineState` + `EngineHandle`, local path only *(half a wave, risk: very low)*

Introduce both. `openLocal()` does exactly what `init()` does today.
Rewrite the 20 poll sites to read `es_` and the 7 command sites to call
`eng_.send()`. **No behaviour change whatsoever** — the acceptance test is
that `tools/headless_test.sh` produces identical screenshots.

This is the step that unlocks `appcpp_split.md`, so it is worth doing even if
the daemon work stops here.

~300 lines moved, ~120 new. One agent, mechanical.

### Step 2 — the remote path for scalars, transport and MIDI *(one wave, risk: medium)*

`openDaemon()`: `EngineClient::reapStale` → `attach` → on failure
`spawnDaemon("latticed", {"--session", id})` → `attach` with a 2 s deadline
(§4.1, and `EngineClient` already implements every piece). `poll()` reads
`state()`. `send()` maps `Cmd` → `pushCommand` and grows the outbox. `pushMidi`
forwards. `drainEvents` handles `EvCommandRejected` / `EvEngineStopping` into
the status bar.

`LATTICE_ENGINE=daemon` now gives you: transport, tempo, quantum, metronome,
mixer, meters, CPU, the computer keyboard, previews. **No clips, no devices,
no recording** — the grid draws, launching does nothing audible, and the
status bar says so. That is a legitimate ship: it is exactly what phase 1's
daemon could do, now with a UI on it.

Risk is concentrated in `init()`/`shutdown()` ordering and in the session id
(§7: default it to a hash of the project path, `$LATTICE_SESSION` overriding).

### Step 3 — clips through the pool *(one wave, risk: high)*

The largest single step, and the one with a use-after-free failure mode.

- `createPool(session)` in `init()`, before any decode. 256 MiB default,
  sparse, costs one page.
- `loadClipInto()` decodes as today, then `poolWrite()`s the interleaved
  floats and stores the `u64` in the clip.
- `pushClip()` builds a `WireClip` instead of an `RtClip` and calls
  `client.setClip(t, s, c)`; refusals go through the dirty-cell set (§2.2).
- `publishNotes` / `retiringNotes_` / `Ev::NotesRetired` **delete**; notes are
  a `PoolKindNotes` block and `WireNote` is asserted to mirror `RtNote` field
  for field, so a notes array is a `poolWriteNotes()` and nothing else.
- `releaseStaleSlots()` → `clearClip()`.
- Retirement: `popEvent` already runs `observe()`, so `EvBlockRetired` frees
  automatically; the GUI's job is only to call `poolRelease(ref)` when a
  `ClipModel` drops its last reference.

**Two design decisions this step must make explicitly:**

*(i) Does `SampleBuffer` survive?* `drawWaveform()` needs `peakBuckets` and
the samples. Options: keep the GUI-heap `SampleBuffer` **and** a pool copy
(2× RAM per clip, zero risk, keeps every draw path working); or decode
straight into the pool and keep only the peak summary GUI-side, with
`SampleRef` becoming `{poolRef, frames, channels, peaks}`. The second is what
§3.5 means by "decode writes straight into shared memory: no copy at
hand-off" and it is the right end state. Recommend: **ship (i-a) the double
copy in this step**, convert to (i-b) as a follow-up, because the conversion
touches `src/audio/sample.h`, `project.cpp`, the undo snapshot and
`tools/render.cpp`, and bundling it here would make a high-risk step
unreviewable.

*(ii) Undo pins pool blocks.* `UndoEntry::samples` holds a `SampleRef` per
clip precisely so an undo can give back a take that was never a file. In the
pool world those are pool blocks, held `Live` by the history. `kUndoDepth` is
128. A session that records twenty two-minute takes and undoes around them can
pin far more than 256 MiB. Mitigations, pick one: size the pool from a
setting; evict the oldest undo entry's samples on pool pressure (undo becomes
lossy at depth, which the header comment already half-accepts); or keep undo
samples on the GUI heap and re-`poolWrite` on restore (a copy on an already
slow path — probably the right answer). **This must be decided in the design,
not discovered in a soak test.**

Acceptance: an ASan soak that adds, edits and removes clips under playback,
per §6 phase 2's own mitigation.

### Step 4 — devices by id *(one wave, risk: medium-high)*

Deletes more than it adds. Gone: `DeviceModel::inst`, `LiveDevice`,
`retiring_`, `RetiredChain`, `published_[]`, `publishedReturn_[]`,
`publishedMaster_`, `publishChain()`, `releaseAllChains()`, the
`Ev::ChainRetired` arm of `pumpEngineEvents()`, and `registry_.instantiate`.
Arrives: `DeviceModel { u64 uid; u32 deviceId; DeviceState state; std::string
uri, name; }` plus a `DeviceMirror` cache.

The genuine UI change is **asynchronous instantiation**. Today `addDevice()`
instantiates inline and either succeeds or sets a status string. Now:

```
click → CmdAddDevice(uri) → device slot appears in "loading" state
      → EvDeviceAdded  → readDevice() → mirror → knobs appear
      → EvDeviceFailed → slot turns red with the reject reason, or vanishes
```

`addDevice`'s optimistic mutate-then-roll-back (597–605) must be replaced by
"nothing is in the model until the event arrives", or the model can claim a
device that failed to load. A pending-request map keyed by a client-side
request id is the honest structure; the protocol's `EvDeviceAdded`/`Failed`
answer-exactly-once discipline (§11.3) makes it safe.

Knobs: `paramInfo(p)` → `mirror.params[p]`, `getParam` → local knob value,
`setParam` → `client.setDeviceParam`. `setBypassed` → `Cmd::SetBypass`.
Honour `truncatedParams`.

`serializeDevices()` reads values out of the param table instead of the
instance; `materializeDevices()` becomes a batch of `addDevice(uri)` followed
by param writes once each `EvDeviceAdded` lands — i.e. **project load becomes
asynchronous too**, which is the sharpest edge in this step. A load that
returns before its devices exist will have `pushAll()` running against a
half-built chain. Recommend an explicit `sessionSyncing_` state with a
progress line in the status bar, and no undo point taken until it settles.

### Step 5 — the catalog table and the browser *(half a wave, risk: low)*

§3, option B. Independent of steps 3 and 4 in the code, dependent on step 4
in the UI (a browser you cannot double-click into a chain is a list).

### Step 6 — lifecycle and its UX *(half a wave of code, several weeks of hours, risk: medium)*

Code is small; confidence is not. See §6.

### Step 7 — recording *(a wave of design, then a wave of work, risk: high)*

See §7. Until it lands, `LATTICE_ENGINE=daemon` cannot be the default,
because recording is not a nice-to-have in a session DAW.

---

## 6. Lifecycle UX

`EngineClient` supplies the mechanism (`attach`, `alive(tolerance)`,
`reapStale`, `spawnDaemon`, `waitFor`, `republishClips`, automatic pool
re-announce on attach). What the GUI owes is a state machine and honest
chrome.

```
Detached ──spawn/attach──► Starting ──ready──► Live
   ▲                          │                 │  alive() false, pid gone
   │                          │ timeout         ▼
   └──────────────────────────┴──────────── Lost ──respawn──► Resyncing ──► Live
                                              │
                          EvEngineStopping ──►Stopping──► Detached
```

**Startup.** §4.1's ladder, already implemented in `attach()`'s reap-on-the-
way-out ordering (§9 deviation 7 — do not "improve" it). Session id defaults
to a hash of the project path; `$LATTICE_SESSION` overrides; `--session`
forwards to the daemon. Spawning uses `fork`+`execv` with no shell, which
matters because the session id can come from a filename.

**Banners, and the discipline for them.** One line under the control bar,
never a modal:

| state | text | actions |
|---|---|---|
| Starting | "Starting the audio engine…" | — |
| Live | *(nothing)* | — |
| Stale (heartbeat > 500 ms) | "Engine not responding" | Restart engine |
| Stale (> 5 s) | same, emphasised | Restart engine |
| Lost (pid gone) | "The audio engine stopped. Your set is intact." | Restart engine |
| Stopping | "The audio engine is shutting down." | — |

The rule §4.4 insists on and that a UI is most likely to violate: **never
respawn automatically on a stale heartbeat.** A laptop resuming from suspend
and a JACK restart both look exactly like a wedged engine for hundreds of
milliseconds, and a second daemon under a live one is the worst available
outcome. Show the banner early; act only on a *dead* engine
(`processAlive()` false) or on a user click.

**Recovery**, which is the pleasant part because the pool survives: reap the
orphan region, respawn, attach (which re-announces the pool automatically),
`republishClips()` — a `memcpy` plus one `SetClip` per occupied cell, no
decode, **no offset changes** — then re-issue `AddDevice` for every device
(ids do not survive; §11.4) and re-push their params, then push mixer scalars
and tempo. Transport comes back **stopped**, per §4.4's honest default.
`daemon_test` §13 already asserts this whole round trip from the client side;
the GUI's version is the same calls in the same order.

**GUI crash / reattach (§4.3)** is the headline feature and it is *not*
reachable yet: nothing negotiates the unlink obligation back
(`ShmRegion::adoptOwnership()` is still missing, §10.6.6), and there is no
journal to reload from. Ship the engine-side half (a GUI death does not stop
the transport — that is free, the daemon simply never notices) and defer
reattach. Say so in the release note rather than implying it works.

**Clean shutdown.** `shutdown()`'s ordering comment (224–249) is about joining
the audio thread before freeing what it borrows. In daemon mode the GUI
borrows nothing, so the ordering collapses to: stop previews and held notes →
`Cmd::Shutdown` (or leave the daemon running, see below) → `closePool()`
last. **The policy question:** does quitting the GUI stop the engine? §4.5
says the GUI sends `Cmd::Shutdown`; §4.3 says the engine surviving is the
point. Both are right for different users. Recommend: **quitting the GUI
stops the daemon it spawned, and leaves alone a daemon it merely attached
to.** Parent-of-record is a clean rule, matches every editor/language-server
pair, and `spawnDaemon()` already returns the pid that decides it.

---

## 7. Recording: the gap with no floor under it

`Cmd::RecordSlot` and `Cmd::RecordMidiSlot` carry a GUI-owned buffer that the
engine appends into and hands back. They are refused at the boundary
(`commandCarriesPointer`) and §11.9 lists them as still deferred with no
design attached. This is not a small hole: it is the entire recording feature,
the record-intent button, take naming, overdub, and `finishRecording` /
`finishMidiRecording` (268 lines of `app.cpp`).

Why it is harder than clips: **the pool goes the wrong way.** `PoolReader`
maps the pool `PROT_READ` in the daemon — deliberately, so "the engine only
reads the pool" is a page permission (§10.1, §11.3). A take is the daemon
*writing* audio. So one of:

1. **A take region**: a second shared region created by the *daemon*, mapped
   read-only by the client, with the same block/magic/validate machinery
   inverted. Symmetric with what exists, and the retirement direction inverts
   too (the client says "I have copied it out", the daemon frees). Probably
   ~600 lines across `control.h`, a new `take.h`, `latticed.cpp` and `app.cpp`.
2. **The daemon writes takes to disk** and tells the client the path. §7 of
   the design doc already floats this ("probably the daemon writes, the GUI is
   told the path"). Much simpler, and it has an independent virtue: a take
   that survives a crash. Costs a decode on the GUI side to draw the waveform,
   and it makes every take a file — which is arguably correct for a DAW and
   arguably wrong for a scratch loop.
3. **Punch a write window into the pool.** Do not. It trades the one hard
   guarantee this design has for convenience.

Recommend **(2)**, with the take written to
`$XDG_RUNTIME_DIR/lattice/takes/<session>/<uid>.wav`, promoted into the
project directory on save. It is less code, it is crash-safe, and it turns
`PendingRec`'s "the buffer is GUI-owned for its whole life and freed only on
the handshake" — the most delicate ownership rule left in `app.cpp` — into a
filename.

Size this as its own wave, design first. Until it lands, `inproc` stays the
default and the daemon path advertises itself as preview.

---

## 8. The fallback story

**Transitional, not forever — with one exception.**

Keeping both paths alive indefinitely costs more than it looks. It is not one
`if`: it is two representations of a clip (`SampleRef` vs `poolRef`), two of a
device (`unique_ptr<PluginInstance>` vs `deviceId`), two of a chain (published
`RtChain` vs daemon-side), two retirement protocols, two recording paths, and
two failure modes per feature. Every future wave pays that tax twice. The
`retiring_`/`published_`/`publishedNotes_` machinery that step 4 deletes is
precisely the code that a permanent dual path would force us to keep.

Recommended policy:

1. Through steps 2–6, `LATTICE_ENGINE=inproc` is the default and both paths
   are supported. This is §6 phase 4's own instruction and it is right.
2. When recording lands over the wire (step 7) and the daemon path has real
   hours, flip the default to `daemon`.
3. One release later, **delete the in-process path from `App`.** Not from the
   tree: `Engine` keeps its direct users in `tools/render.cpp`,
   `tools/gen_demo.cpp` and `tests/engine_test.cpp`, none of which go through
   `App`, so deleting `App`'s branch costs those nothing.
4. The **exception**: keep a genuinely engine-free degraded mode. If the
   daemon cannot be started, the GUI should open, load the project, browse,
   edit and save — silently, with a banner — rather than refuse to run. That
   is far cheaper than a second engine (it is `EngineHandle` with both members
   null and every `send()` a no-op) and it covers the case a fallback is
   actually for: a broken audio setup on someone else's machine.

The honest counter-argument for keeping `inproc` forever is startup latency
and one less moving part for a single-user desktop session. It does not
survive contact with the maintenance cost above, and the degraded mode in (4)
answers the fear that motivates it.

---

## 9. Sizing, honestly

Per step: one agent, and the wave figures assume the `app.cpp` split has
landed (otherwise every step serialises on one file).

| step | new/changed lines | deleted | wave | risk | blocked by |
|---|---|---|---|---|---|
| 0 `SharedState` gaps | ~200 | — | ½ | low | — |
| 1 `EngineState`/`EngineHandle` | ~420 | ~180 | ½ | very low | 0 |
| 2 remote scalars/transport/MIDI | ~250 | ~40 | 1 | medium | 1 |
| 3 clips through the pool | ~500 | ~200 | 1 | **high** | 2 |
| 4 devices by id | ~600 | ~450 | 1 | med-high | 2 |
| 5 catalog table + browser | ~240 | ~30 | ½ | low | 4 |
| 6 lifecycle UX | ~250 | ~60 | ½ + hours | medium | 2 |
| 7 recording | ~600 + design | ~270 | 2 | **high** | 3 |

Total ≈ 3 000 lines changed, ≈ 1 200 deleted, **5–7 agent-waves**, of which
steps 3, 4 and 6 can run concurrently once step 2 has landed and the file
split has given them disjoint translation units.

The two numbers worth remembering: **the far side is already tested** (300
checks against a spawned daemon), and **the migration is net-negative on the
scariest code in the app** — the whole GUI-owns-what-the-audio-thread-borrows
protocol goes away, which is the reason to do this even setting the crash
isolation aside.

---

## 10. Steps 0 and 1 shipped

### Step 0 — `SharedState` v4

`SharedStateT` gained `latencyFrames` (beside `blockSize`) and
`returnMeterL/R[kShmReturns]` (beside the master meters); `kShmVersion` 2 → 3
had already been spent by wave 8g, so this is **3 → 4**. Growing the struct moves
`control::kState`'s successors and therefore `control::kHash` on its own, so the
layout-hash half of §1.2 needed no edit: a v3 binary and a v4 binary refuse each
other at `attach()` with a specific message, which is the mechanism working.

`shm.h` deliberately does **not** include `audio/engine.h`, so `kMaxReturns` is
not reachable there. The width is written out as `ipc::kShmReturns` and
`control.h` — which has both headers — carries the `static_assert` that holds the
two numbers together. `src/ui/engine_state.h` repeats the trick for the same
reason (`kEsReturns`, asserted in `engine_handle.h`).

**One line this wave could not write.** `src/daemon/nxtaktd.cpp` was owned by
another agent, so `mirrorLoop()` still publishes everything *except* the two new
fields — they read as zero across the boundary until it does. The daemon
compiles and `daemon_test` is green (523) because nothing asserts on them yet.
What is owed, immediately after the `masterMeterR` store at what is currently
line 2449:

```c++
for (int i = 0; i < kMaxReturns; ++i) {
    s.returnMeterL[i].store(e.returnMeterL[i].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    s.returnMeterR[i].store(e.returnMeterR[i].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
}
s.latencyFrames.store(e.latencyFrames.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
```

§5 step 0 also asked for the MIDI client id and §10.8's mixer scalars while the
file was open. Both were left out on purpose: this wave's brief was the two
fields §1.2 proved are a *regression*, and the mixer scalars are wanted by step
5's reattach, which is where they can be tested by something. They are still one
version bump each, and the bump they would ride is the next one.

### Step 1 — the seam, local only

`src/ui/engine_state.h` (`EngineState`, pure data), `src/ui/engine_handle.h/.cpp`
(`EngineHandle`, concrete, no vtable). `App` no longer holds an `Engine`, an
`AudioBackend` or a `MidiInput`; it holds `EngineHandle eng_` and
`EngineState es_`, and `frame()` opens with `eng_.poll(es_)`.

**32 atomic loads on 29 source lines became one.** §1.2 counted 20 *sites*; the
loads are more, because four of the sites read arrays element-wise. All of
`app_session.cpp`, `app_chrome.cpp`, `app_detail.cpp` and the polled half of
`app_engine.cpp` now read `es_`, and `grep -c 'engine_' src/ui` is 0.

Three things §2 did not say, each found by contact with the code:

1. **Locally there is nothing to gate the snapshot on.** §2.1 says to snapshot
   "against `SharedState::generation`", which is right cross-process and has no
   local twin: `Engine::publish()` bumps no counter, and `blocksRendered` is
   incremented at the *top* of `process()` while `publish()` runs at the bottom,
   so equal values either side of a copy do not prove the copy did not straddle
   a publish. `engine.h` is frozen, so the local `poll()` is a tight unguarded
   copy and says so. That still fixes the failure §2.1 actually names — the four
   reads in `drawClipSlot` were separated by a draw, i.e. by several audio
   blocks, and are now separated by a few hundred nanoseconds — and the daemon
   branch closes it completely for free.
2. **Two values must not come from the snapshot.** `sampleRate()` is read by
   paths that decode and resample, where "as of this frame" is the wrong
   question and a zero would silently resample a whole set wrong; and
   `journalDropped()` is read at the instant a take is committed, where §5.4 of
   ARRANGEMENT.md wants the count *now*. Both are live accessors on the handle,
   which is also where their daemon versions belong.
3. **`Engine` moved to the heap** inside the handle. It is ~2.3 MB of scratch,
   it was already wherever `App` was, and a pointer is what lets `local()`
   answer "there is no in-process engine" in step 2 without a second flag.

`EngineState` does not remove `audio/engine.h` from the view translation units
yet — they reach it through `app.h`, and `SlotState`, `Cmd` and `kMaxReturns` all
still live there. What has happened is the harder half: no view *reads* an engine
object any more, so the include is the only thing left to cut.

### The `pushAll` flow-control bug (ARRANGEMENT.md §15)

Fixed here rather than left queued, because §15 put it in these files.

`pushAll()` sends `3 + tracks*9 + tracks*scenes + 4` commands in one burst —
about 1320 for a 32×32 set against a 1023-deep ring — and `adoptSession()` runs
`publishArrangementAll()` (65 more) straight after it. The fix is **one FIFO of
deferred work, drained at the top of every frame**, and the two properties that
make it correct are:

- **it queues intent, not payload.** An entry says "cell (3,7) needs
  publishing". The allocation, the address resolution and the retirement
  bookkeeping all happen at drain time from the model as it stands *then* — so a
  queued cell can be edited, cleared, or fall outside a freshly loaded set before
  it drains and the right thing still happens, and a fader dragged for a second
  collapses into one publish. That is why the drain goes through `syncClipCell`
  rather than `pushClip`: a cell queued while it was inside the set has to be
  *clearable* when it drains after a load shrank the set.
- **it is one FIFO.** While anything is waiting, every new command joins the back
  rather than going to the ring, so nothing overtakes something already queued.
  Two queues would have needed a rule for a queued `Cmd::ClipGain` against a
  fresh `Cmd::SetClip` on the same cell, and there is no good one.

Scalars, clip cells, arrangement lanes, the transport cell and arrangement
automation ride it. Chain publishes deliberately do not: there are at most 37,
they are independent of clips and scalars, and they carry an `RtChain` whose
retirement is already a hand-built protocol.

One consequence worth knowing: `adoptSession()` now clears `sampleGrace_` only
when the queue is empty. Deferred publication means a restore can arrive before
the one before it has drained — the undo self-test does exactly that,
synchronously, inside one frame — and the engine would still be reading a buffer
that line was about to drop.

**Proof** (`NXTAKT_DEBUG_PUSHALL`, in the shape of the undo self-test's hook).
It waits for the queue to drain, then launches every scene in turn and compares
each track against the model. `Cmd::LaunchScene` queues a track's slot *only*
when the engine's own `clips_[t][scene]` is valid, so this reads the engine's
slot table rather than anything the GUI believes about it.

| | ring-full warnings | cells disagreeing |
|---|---|---|
| 8×8 undo self-test, before | 1040 | — |
| 8×8 undo self-test, after | 0 | — |
| 32×32 load, flow control **off** (negative control) | — | **254 of 1024** |
| 32×32 load, flow control on | 0 | **0 of 1024** |

The negative control is the important row: it is the shipped behaviour of the
last four waves, and it is silent. 254 cells where the engine plays a clip the
model does not believe in, with nothing scheduled to notice.

Two related overflows this wave did **not** fix, both outside its files:
`tools/render.cpp` pushes every clip before its first `process()` and so renders
a 32×32 set as **silence** for exactly the same reason, and `Engine::journal_`
overflow is already handled correctly (§5.4 refuses the take).

---

## 11. Steps 2 and 3 shipped

`NXTAKT_ENGINE=daemon` (or `LATTICE_ENGINE=daemon`) now runs the shipping GUI
against `nxtaktd`. `NXTAKT_SESSION` names the session and defaults to
`default`; `NXTAKT_AUDIO` is forwarded as `--driver` to a daemon we spawn;
`NXTAKT_DAEMON` overrides the binary, which is otherwise found next to
`/proc/self/exe`. Unset, everything behaves exactly as before.

```
NXTAKT_ENGINE=daemon NXTAKT_SESSION=mysession ./build/nxtakt myset.lattice
```

### 11.1 What crosses, and what is refused

| | daemon mode |
|---|---|
| transport, tempo, quantum, metronome | **yes** |
| mixer scalars, sends, return levels | **yes** |
| every polled meter, indicator and the playhead | **yes** |
| computer-MIDI keyboard, note previews | **yes** |
| session clips, audio and MIDI, through the pool | **yes** (step 3) |
| device chains | refused — step 4 |
| recording | refused — §7 |
| the arrangement and its automation | consumed and reported — see below |
| time signatures | refused by the daemon — `commandIsKnown`'s bound is unmoved |
| hardware MIDI input | not connected — §1.3 |
| clip envelopes, warp markers, transients | dropped, and logged, per clip |

`Cmd::SetSignatures` is deliberately still outside `ipc::commandIsKnown`'s
bound, so `nxtaktd` answers `RejectUnknownCommand` and **plays every set in
4/4**. Steps 2 and 3 did not make it carryable: the map is a `const RtSig*`
with its own `Ev::SigsRetired` handshake, so honouring it means a pool kind, a
translate-and-validate pass, a retirement layer and an ack event — the
arrangement's shape, not the clip table's. Refused-and-visible beats
accepted-and-ignored, so it stays refused. (It cannot even be *sent* today:
`session.h`'s `publishSignatures()` takes an `Engine&` and there is no Engine
in daemon mode. Routing it through `EngineHandle` is a prerequisite for
whoever picks this up.)

### 11.2 `false` means "try again", and nothing else

The sharpest thing this step learned, and the one most likely to be undone by
someone tidying up. `App::flushPending()` re-queues a refused publication and
retries it every frame, in order, and nothing behind it in the FIFO moves until
it succeeds. So a command daemon mode can *never* carry must not answer
`false`: a permanent `false` wedges the queue and with it the transport.

`EngineHandle` therefore splits refusals in two:

* **transient** (`false`): the ring is full, the clip cell is still
  un-acknowledged, or nothing is attached. §2.2 asks for a dirty-cell set and a
  scalar outbox for exactly this — and both already exist, because the wave-8
  `pushAll` FIFO is precisely that mechanism. Nothing new was needed.
* **permanent** (`true`, and loud): consumed, counted in `remoteRefusals()`,
  and logged once per command type with the reason. `close()` prints the tally.

The chain family and the two Record commands answer `false` safely, because
`publishChain()` and `startRecording()` are deliberately *not* on the FIFO —
they free what they built and report, which is the behaviour we want anyway.

### 11.3 The retirement stand-in

A GUI-heap `RtNote[]` is **copied into the pool**, so the engine never holds it
and can never send `Ev::NotesRetired` for it. The handle keeps the same
per-cell "last published" table the engine keeps, applies the same rule
(announce the displaced pointer, and only when it differs from the incoming
one), and synthesises the event. The same goes for `Ev::AutosRetired`,
`Ev::WarpRetired`, `Ev::ArrangementRetired` and `Ev::TrackAutosRetired`.

Without it `App::retiringNotes_`, `retiringAutos_` and `arr_.retiring` grow for
the life of the session and nothing ever comes home. `tests/handle_test.cpp`
asserts the note case directly, because it is the one a normal edit session
hits sixty times a minute.

### 11.4 The double copy, and how a stale address is caught

§5 step 3 decision (i-a): the GUI keeps its `SampleBuffer` **and** the pool
keeps a copy, so `drawWaveform()` and every other draw path keep working
untouched. The cache is keyed by the source address — and the address alone is
not enough, because a `SampleBuffer` can be freed and a different one allocated
at the same address (undo does exactly this), which would publish the wrong
audio under the right offset. So each entry carries a fingerprint: 256 strided
words plus the shape, recomputed per push and constant-time in the buffer size.
A mismatch releases the old block and writes a new one.

Converting to (i-b) — decode straight into the pool, keep only the peak summary
GUI-side — still touches `src/audio/sample.h`, `project.cpp`, the undo snapshot
and `tools/render.cpp`, and is still the right end state.

### 11.5 The snapshot is genuinely coherent now, and §2.1's recipe was not enough

`SharedState::generation` is a **seqlock sequence** as of `kShmVersion` 5: odd
while the daemon's mirror is publishing, even when it is not, bumped twice per
pass by `publishBegin()`/`publishEnd()`. `EngineHandle::poll()`'s daemon branch
runs its copy inside `SharedStateT::readCoherent()`, which retries until the
sequence is even and unchanged either side.

§2.1 says "read generation, copy, re-read, retry on change" and that is not
sufficient on its own, which is worth writing down because the shorter version
looks obviously correct. Against a writer that bumps only at the END of a
publish, a reader that samples entirely *inside* one publish sees the same
counter either side of a copy it has already torn. The parity is what closes
it: one extra relaxed increment and two fences per publish, on a non-realtime
thread.

Two writers would break it — `fetch_add` from two threads takes the sequence
from odd straight back to even — so `shutdown()`'s `engineState` store is
deliberately left unbracketed and says why at the call site; it runs while the
mirror thread is still alive.

`readCoherent()` is bounded at eight tries and then hands the copy over anyway,
counted in `EngineHandle::snapshotTears()`. A UI that spun until a SIGSTOPped
daemon released the sequence would hang on a breakpoint in another process; a
stale frame plus a banner is the right failure.

**Proved, both directions.** `ipc_test` §4b runs a writer that parks 200 µs
mid-publish: an unguarded copy of the same block tears (2.9 M of 12 M samples
with `publishBegin()` removed; 233 of 400 publishes with it in place), and
`readCoherent()` returned 18 820 439 of 18 820 439 snapshots provably coherent
and none torn. `daemon_test` §7c proves `nxtaktd` itself brackets its mirror:
the sequence is observed odd, every snapshot is proved coherent, and a second
daemon spawned with `NXTAKT_DEBUG_MIRRORSTALL=150000` — which parks the mirror
in the *middle* of the pass, deliberately, so that a straddle is what a reader
would catch — makes `readCoherent()` refuse 200 of 200.

Removing `publishBegin()` from `mirrorLoop()` turns three of those checks red;
removing `publishEnd()` turns one red. Both were removed and watched.

### 11.6 `SharedState` v6: the musician's playhead

`posBar`/`posBeat`/`posSixteenth` and `posSigNum`/`posSigDen`, mirrored from
Engine's own published atomics into `SharedState` and into `EngineState` on
both branches. They are carried rather than derived, and the reason is a
disagreement that would otherwise be rendered with total confidence: the
transport readout used to compute bars from the *session's* signature map, and
`sigMapValid` **refuses** a map whose bar lines do not follow from its own bar
lengths, leaving the engine in 4/4. The session's copy still says 7/8. Reading
the engine's counters makes that state unrenderable.

`daemon_test` §7d tests all five by **poisoning the field and watching the
mirror put it back**, and that formulation is not decoration. `posSigNum` and
`posSigDen` read 4/4 across the wire — because the daemon refuses the signature
map — and 4/4 is also what an *unwritten* field reads, since `init()` seeds it.
A check that asserted "posSigNum == 4" would pass against a mirror that never
stored it, which is §7b's bug wearing a different hat. Each of the five stores
was removed from `mirrorLoop()` in turn and its check watched go red.

### 11.7 Lifecycle

`openLocal()` — the name is step 1's and now dispatches; see below — reaps,
attaches, and on failure spawns `nxtaktd` and attaches with a 2 s deadline. If
that fails it opens **nothing**, per §8's exception: the GUI still loads,
edits and saves, and every `send()` is a no-op. It deliberately does not fall
back to a local engine, because a second engine under a wedged one is §4.4's
worst available outcome.

`close()` follows §6's parent-of-record rule: a daemon we spawned is SIGTERMed
(escalating to SIGKILL), a daemon we merely attached to is left running. The
pool is unlinked **first**, which is not the obvious order: stopping the daemon
is a signal plus a wait of up to three seconds, and anything that kills the GUI
inside that window leaves 256 MiB named in `/dev/shm`. Unlinking first is safe
because `shm_unlink` removes the name and not the mapping — the daemon keeps
playing the samples right up until it exits.

A GUI that is *killed* still leaves the pool behind, and that is the design
(§4.3: the samples outlive the GUI so a replacement can adopt them). It
self-heals: the next `SamplePool::create()` reaps it, because its creator is
provably gone.

### 11.8 Evidence

* `tests/handle_test.cpp` — 28 checks against a real spawned daemon: `local()`
  null, the live sample rate before any decode, scalars, MIDI, a DC clip
  through the pool metering exactly 0.500 on the master, a MIDI clip's notes as
  a pool block, `Ev::NotesRetired` coming home for the *displaced* array, the
  two refusal classes, and `/dev/shm` clean after `close()`.
* `tools/headless_test.sh` with `NXTAKT_ENGINE=daemon` and
  `NXTAKT_DEBUG_PUSHALL=1`, on the four-scene demo set: **"PASS — the engine's
  slot table matches the set"**, 20 of 20 cells, queue high water 0, and the
  daemon's own counters reading `89 commands applied, 0 rejected, 20 clips
  applied`. The hook reads `es_`, so it is testing the snapshot coming back off
  the wire as much as the clips going out.
* A second, independent `EngineClient` attached to the same session while the
  GUI ran: silent baseline 0.0000, then master peak 0.1870 after
  `LaunchScene 0`, with the beat advancing. That process never touched a
  sample; everything it measured was decoded by the GUI, written into the GUI's
  pool and installed by the daemon.

### 11.9 Owed

* **The rename.** `EngineHandle::openLocal()` dispatches on `NXTAKT_ENGINE` and
  should be `open()`. `App::init()` has exactly one call to it
  (`src/ui/app.cpp:54`) and that file was another agent's this wave, so the
  dispatch went inside the existing entry point rather than into a rename
  nobody could apply. `openLocalEngine()` is the honest local-only spelling and
  already exists.
* **A `Makefile` target for `tests/handle_test.cpp`**, so `make test` runs it.
  The recipe is in the file's header comment.
* **The arrangement over the wire.** The daemon can already take one — it has
  `translateArrangement()` and `daemon_test` §16b proves it plays — but as a
  pool blob, and the GUI hands the handle an already-built `RtArrangement`. The
  conversion is a blob encoder over the same pointer→pool-ref cache step 3
  built, so it is the cheapest remaining feature by some distance.
* **Hardware MIDI**, §1.3 option 1: move `MidiInput` into `nxtaktd`. It cannot
  be done from this side — `MidiInput::start()` takes an `Engine&`.

---

## 12. Steps 4, 5 and 6 shipped

`NXTAKT_ENGINE=daemon` now loads plugins. A device the user adds instantiates in
`nxtaktd`, renders there, follows its knobs, follows its bypass, comes back after
an engine restart, and is refused *with a reason* when the engine has never heard
of it. The browser lists what the daemon can load rather than what the GUI's own
process happened to find, and the link between the two has a state machine with a
number on it.

### 12.1 The seam moved down one level, and no App file was touched

§5 step 4 describes rewriting `App` around device ids: `DeviceModel::inst`
deleted, `addDevice()` becoming "send and wait for the event", the knobs reading
a `DeviceMirror`. **That is not what shipped, and the deviation is deliberate.**
It edits `src/ui/session.h` and `src/ui/app_devices.cpp`, both owned by other
agents this wave; more importantly it *deletes the in-process path*, which §8
says stays supported through step 6 and which `tools/render.cpp`,
`tools/gen_demo.cpp` and `engine_test` all still go through.

So the seam went one level lower, at the single call every chain edit already
funnels through:

```
App::publishChain(owner)
  -> builds an RtChain* full of PluginInstance*
  -> eng_.pushCommand({SetChain, owner, p = chain})
       -> [daemon]  RemoteEngine::setChain(): read the chain's DESCRIPTION off
                    those instances, reconcile the daemon toward it
       -> [local]   Engine::pushCommand(), exactly as before
```

An `RtChain` cannot cross a process boundary. But **everything the daemon needs
in order to build its own is readable off one through `PluginInstance`'s
virtuals** — `desc().uri`, `paramInfo(i).id`, `getParam(i)`, `bypassed()` — and
those four are the entire input. The GUI's instance stops being a thing that
renders audio and becomes exactly what §4 says it should be: the model. The GUI
is the authority on what exists; the engine on what sounds.

Three consequences worth stating rather than discovering:

1. **`Cmd::SetChain` now answers `true`.** §11.2's rule is unchanged and this is
   not an exception to it — the command is *consumed and acted on*, not
   swallowed. It has to answer true because `App::addDevice()` compares
   `*co.published` before and after and **rolls the device back out of the
   model** on a refusal ("Engine busy — device not added"). A `false` here was
   the whole of the visible-and-silent bug.
2. **The displaced chain is retired by a synthesised `Ev::ChainRetired`**, the
   same stand-in §11.3 built for notes and for the same reason: there is no
   engine on this path to send one, and without it `App::retiring_` grows for
   the life of the session and every removed plugin leaks.
3. **Parameters are POLLED, not hooked.** A knob drag calls
   `PluginInstance::setParam()` on the GUI's own instance and there is no
   command anywhere to hang a hook on. `syncParams()` compares the model against
   what it last wrote, once a frame, and pushes the difference. One virtual
   `getParam()` per mapped control per frame: ten devices of twenty controls is
   200 calls, two orders of magnitude below a single draw call. It carries the
   knobs, a project load's restored parameters, an undo, and a rack macro
   driving its targets — none of which have a command of their own.

Bypass is **not** on that path: it is `Cmd::SetBypass`, because it has to order
against the chain edits around it (§3.7). That is the one-line change §1.6 warns
is easy to get wrong by reflex.

### 12.2 The reconciler, and the one invariant it rests on

`live[chain]` is what the daemon's chain **will be** once everything already sent
has been applied — placeholders for un-answered adds included. Because the daemon
dequeues device commands strictly in the order it took them off the ring, a
position computed against `live` is the position the daemon will use, even for
commands whose answers have not come back. That is what lets a project load fire
every `AddDevice` for a chain in one pass instead of one per frame.

Order within a pass: **removals first** (so every position below is a position in
the chain as it will be), then moves, then adds at their target positions. A
chain with an unanswered add is skipped entirely for one frame, because its
`live` holds an entry with no device id and a removal or a move could not name
it.

Failures are **tombstoned**, not retried: an `AddDevice` the daemon refuses would
otherwise be re-sent sixty times a second for the life of the session. The
tombstone is dropped the moment the GUI stops asking for that instance, so
removing a device that would not load and adding it again genuinely retries.

`docs/RACKS.md` §1 — a rack's `latencyFrames()` is its chain's sum and is not
constant after `prepare()` — needs nothing special here: the daemon owns the
instances, so it recomputes and republishes its own chain on every add, remove
and move, and `WireDeviceInfo::latencyFrames` is its figure rather than ours.
Republication is free because a chain is a *declaration*: publish it again and
the reconciler works out the difference.

**Parameters are addressed by `ParamInfo::id`, per `docs/PARAM-ADDRESS.md`**, and
never by index. The two sides load the same plugin build so the ids match, but
neither promises the other an index *ordering*, and a positional guess that was
wrong would move the wrong knob silently — the worst failure this corner of the
system has. A control with no counterpart is counted in
`RemoteDevice::paramsUnmapped` and logged, never guessed at.

### 12.3 What a rack does, and it is not good

`RACKS.md` §4: a rack's descriptor is static and does not describe its contents,
and there is no wire field that could carry them. So the daemon instantiates
`nxtakt:rack` and gets an **empty** one — eight macros driving nothing — and the
rack sounds as a passthrough while the GUI draws its contents. The macros
themselves cross (they are ordinary `ParamInfo` knobs) and drive nothing.

Logged once per chain rather than left silent, because "my rack went quiet" is
otherwise unattributable. The fix is a `PoolKindRackState` blob carrying
`rackStateToString()`'s output and an `AddDevice` that can name one — the string
already exists, is already one line of printable ASCII, and is already
version-tagged. It is the cheapest remaining device feature.

### 12.4 The catalog: `kProtocolVersion` 6, a tenth section

§3 option B, as specified. `WirePluginDesc[2048]` at 512 B each — 1 MiB,
one resident page until a scan fills it — written once by the daemon's
`publishCatalog()` from the pump thread *before* the `scanState = ScanDone`
release store, which is the publication edge for the whole table. Read once by
the client on `EvScanComplete`, or on attach if the scan is already done (a GUI
that attaches to somebody else's daemon will never see the event).

Same release discipline as `WireDeviceInfo`: `state` stored last, so a reader
that sees a row `Live` has seen every byte. `catalogCount`/`catalogTruncated`
came out of `ControlHeader::reserved`, so the header keeps its size and every
section offset below `kCatalog` is unchanged — the only reason a v5 and a v6
binary refuse each other is the hash and the version.

A plugin whose URI does not fit 256 bytes is **dropped rather than truncated**: a
truncated URI is not a shorter name for the same plugin, it is a different string
that `AddDevice` would answer `RejectUnknownUri` for, and a browser row that
cannot be loaded is worse than a row that is not there. Dropped rows are counted
in `catalogTruncated`, which a browser must draw.

The measured cost on this machine: 415 plugins, ~210 KiB of the table used, one
memcpy-shaped pass at the end of a scan that already took 4 s.

### 12.5 Lifecycle, and a bug that only a GUI could have found

`EngineLink` (`src/ui/engine_state.h`) is §6's state machine, carried in the
frame snapshot so a banner cannot flicker between two draws inside one frame.
`engineLinkBanner()` and `engineLinkOffersRestart()` are §6's table, in the
header, so the view has no policy in it.

**`processAlive()` is not enough when the daemon is your own child.** A daemon we
spawned is our child; a child nobody waits for is a **zombie**; a zombie still
has a `/proc` entry with the same start ticks. So a GUI that started its own
engine would watch it be SIGKILLed and go on reporting it alive for the rest of
the session — and "the process is gone" is precisely the one state §4.4 permits
acting on automatically. `RemoteEngine::reapChild()` does a `WNOHANG` `waitpid`
from `poll()`. The bug is invisible to a client that merely *attached* (no child,
no zombie), which is why 546 daemon_test checks never saw it.

Stale and Lost are now distinguishable and both are tested against a real daemon:
`SIGSTOP` gives Stale with a measured silence and provokes nothing; `SIGKILL`
gives Lost, and only then is a restart offered.

**`restartEngine()`** is §6's recovery and the only things that may call it are a
user click and a provably dead engine. It reaps, respawns, re-attaches (which
re-announces the pool — `attach()` calls `publishPool()`), republishes every clip
cell from the client's shadow, replays the scalars, and re-issues `AddDevice` for
every device on every chain because ids do not survive an engine (§11.4). The
transport comes back **stopped**.

The scalar replay is new and is what makes the restart complete without App
knowing anything happened: the handle keeps a `(type, a, b) -> Command` shadow of
every **state-bearing** scalar it forwarded — tempo, quantum, metronome, the
mixer, sends, return levels, clip gain/warp/loop. Actions are deliberately not in
it. Replaying a `LaunchScene` after a crash would be the GUI inventing a
performance the user did not give, and §4.4's honest default is a stopped
transport. Clips are republished first and scalars second, so a `Cmd::ClipGain`
moved after the cell was published wins over the cell's own copy.

### 12.6 Evidence

`tests/handle_test.cpp` is 28 checks -> **84**, all against a real spawned
`nxtaktd`. The ones that matter:

* **A plugin added in daemon mode makes sound from the daemon.** A DC 0.5 clip
  meters 0.5000 dry; publish a chain holding one device and it meters 0.4621,
  which is `tanh(0.5)` — the Saturator's own shaper, computed in another process.
* **A knob turned on the GUI's own instance changes what the daemon renders.**
  `sat.setParam(kOutput, -12.f)` — exactly what `drawDeviceStrip()` does, with
  nothing sent — takes it 0.4621 -> 0.1161, the ×0.251 that −12 dB is.
  `setBypassed(true)` takes it back to the dry 0.5000 and un-bypassing restores
  0.4621.
* Removing the device retires the displaced chain by pointer, and the track
  meters 0.5000 again.
* An unknown URI is answered `EvDeviceFailed` with a reason, once — the failure
  count is unchanged over sixty further frames.
* The catalog carries 415 plugins with their real shape (Pulse is an instrument
  and takes MIDI), and `daemon_test` §11b loads a device straight off catalog
  row 0, which is the property the table exists for.
* `SIGSTOP` -> Stale, 504 ms of measured silence, nothing restarted itself;
  `SIGCONT` -> Live; `SIGKILL` -> Lost; `restartEngine()` -> a different pid, the
  chain rebuilt, the clip sounding again with no decode at the 0.5 master the
  scalar shadow put back.

**Removal tests, run and watched go red** (the discipline §11.6 established):

| removed | red |
|---|---|
| `publishCatalog()` from `registryReady()` | daemon_test ×3 |
| the per-row `state.store(CatalogSlotLive)` | daemon_test ×3, incl. "every row the header claims parses Live (0 of 415)" |
| `out.link` (daemon branch) | handle_test ×4 |
| `out.link` (local branch) | handle_test ×1 |
| `out.linkSilentMs` | handle_test ×1 |
| `out.devicesPending` | handle_test ×1 |
| `reapChild()` | handle_test ×1 — the zombie bug |
| `syncParams()` | handle_test ×2 |

`catalogTruncated` reads 0 on any sane machine, so asserting it *equals* 0 would
pass against a daemon that never stored it. The discriminating check is the sum:
`catalogCount + catalogTruncated == scanPlugins` can only hold if both halves
were written. Same trick as §7d's poisoning, one step sideways.

### 12.7 Owed, and what step 7 still has to do

**Diffs to files this wave did not own.** None of these is required for the
above to work; each moves a capability from "reachable through the handle" to
"visible in the UI".

1. **The browser should list the daemon's catalog** (`src/ui/app_devices.cpp`).
   `ensurePluginScan()` becomes "if `eng_.remoteOpen()`, `eng_.requestScan()`
   and show a spinner while `eng_.scanRunning()`"; `drawPluginBrowser()`'s
   `const std::vector<PluginDesc>& all = registry_.plugins();` becomes
   `= eng_.catalogReady() ? eng_.catalog() : registry_.plugins();`, and
   `eng_.catalogTruncated()` must be drawn when non-zero. Until this lands the
   browser is §3 option A — the GUI's own scan — which is the bridge §3 permits
   for exactly one wave.
   *Caveat worth carrying into the diff:* `App::addDevice()` still calls
   `registry_.instantiate()` to build the model's instance, so a row only the
   daemon can see would be listed and then fail locally. Both processes scan the
   same machine, so this is the divergence the catalog exists to make visible
   rather than a new failure — it disappears when `DeviceModel::inst` does.
2. **The banner** (`src/ui/app_chrome.cpp`), one line under the control bar:
   `if (const char* b = engineLinkBanner(es_.link)) …`, plus a Restart button
   gated on `engineLinkOffersRestart(es_.link)` calling `eng_.restartEngine()`.
   Everything it needs is in the snapshot; there is no policy to get wrong.
3. **"Loading…" and the reject reason on a device slot**
   (`drawDeviceStrip`, `src/ui/app_devices.cpp`):
   `const RemoteDevice* rd = eng_.remoteDevice(d.inst.get());` — null or
   `!rd->live` means the engine has not made it yet, `rd->failed` means it will
   not and `rd->error` says why, and `rd->paramsTruncated` is §1.6's "…and N more
   controls this build cannot reach". Today a slot the engine refused draws as an
   ordinary device that happens to be silent.
4. **The status bar** should read `es_.devicesPending` and
   `eng_.remoteDevice(...)->latencyFrames`, and §5 step 4's `sessionSyncing_` is
   exactly `es_.devicesPending != 0`.
5. **The rename**, still owed from §11.9: `eng_.openLocal(...)` at
   `src/ui/app.cpp:54` should be `eng_.open(...)`.

**Still genuinely missing, for step 7's wave:**

* **Rack contents** (§12.3). A `PoolKindRackState` blob and one field on
  `CmdAddDevice`.
* **`DeviceModel::inst` deleted**, which is §5 step 4 as written and which is
  what makes the browser caveat in (1) go away. It is a step-7-era change
  because it is the moment the in-process path leaves `App`, per §8 (3).
* **Recording** (§7). Unchanged and still the reason `inproc` is the default.
* **The arrangement over the wire** (§11.9). Unchanged.
* **Hardware MIDI** (§11.9). Unchanged.
* **`deviceParamEngineGeneration()` is still not read.** §1.6 predicted this
  becomes load-bearing the day presets or native plugin UIs land: the daemon does
  not mirror a plugin moving its own controls back, so `syncParams()` would
  overwrite it on the next frame. The field is there; nothing consumes it yet.
* **A second GUI on the same session would fight over the chains.** The
  reconciler assumes it is the only writer of the device tables, which it is
  today (§4.3's reattach is not reachable). Worth a sentence in the release note
  rather than a mechanism.
