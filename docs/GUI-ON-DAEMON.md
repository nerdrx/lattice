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
