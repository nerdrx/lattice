# Splitting Lattice into an engine process and a GUI process

Status: **design, wave 2 not started.** Wave 1 landed the transport
(`src/ipc/shm.h`, `tests/ipc_test.cpp`) and this document. Nothing in
`src/audio` or `src/ui` has been touched yet.

## Contents

1. [Why](#1-why)
2. [The boundary today](#2-the-boundary-today)
   - 2.1 [Commands: GUI to engine](#21-commands-gui-to-engine)
   - 2.2 [Events: engine to GUI](#22-events-engine-to-gui)
   - 2.3 [The polled atomics block](#23-the-polled-atomics-block)
   - 2.4 [Pointer smuggling 1: `RtClip::data`](#24-pointer-smuggling-1-rtclipdata)
   - 2.5 [Pointer smuggling 2: `RtChain::fx`](#25-pointer-smuggling-2-rtchainfx)
   - 2.6 [What else crosses without being a message](#26-what-else-crosses-without-being-a-message)
3. [Target architecture](#3-target-architecture)
   - 3.1 [Processes and who owns what](#31-processes-and-who-owns-what)
   - 3.2 [Two channels: rings for the hot path, a socket for the rest](#32-two-channels-rings-for-the-hot-path-a-socket-for-the-rest)
   - 3.3 [The control region map](#33-the-control-region-map)
   - 3.4 [The wire types](#34-the-wire-types)
   - 3.5 [The sample pool](#35-the-sample-pool)
   - 3.6 [Plugins move into the engine](#36-plugins-move-into-the-engine)
   - 3.7 [The param table](#37-the-param-table)
4. [Lifecycle](#4-lifecycle)
   - 4.1 [Startup and discovery](#41-startup-and-discovery)
   - 4.2 [Version handshake](#42-version-handshake)
   - 4.3 [GUI crash: the engine keeps playing](#43-gui-crash-the-engine-keeps-playing)
   - 4.4 [Engine crash: detect, respawn, republish](#44-engine-crash-detect-respawn-republish)
   - 4.5 [Clean shutdown](#45-clean-shutdown)
5. [Failure modes and hardening](#5-failure-modes-and-hardening)
6. [Phased migration](#6-phased-migration)
7. [Open questions](#7-open-questions)
8. [What wave 1 actually delivered](#8-what-wave-1-actually-delivered)

---

## 1. Why

Three things the current single-process design cannot do, in order of how much
they hurt:

- **A GUI bug kills the audio.** A segfault in a widget, a bad OpenGL driver, a
  window-system hiccup — any of them takes the transport down mid-set. Every
  serious live tool separates these.
- **A plugin bug kills the GUI.** Third-party LV2/CLAP code runs in the same
  address space as the renderer and the session model. Today a misbehaving
  plugin can corrupt the project in memory before anything notices.
- **The GUI cannot be restarted.** No headless mode, no "reconnect a UI to the
  running engine", no remote control surface, no crash-recovery story.

The split also makes the realtime contract enforceable rather than documented:
once the GUI cannot reach the engine's memory except through a fixed-size
pointer-free struct, "the audio thread never allocates" stops being a comment.

---

## 2. The boundary today

Everything the two halves say to each other goes through `src/audio/engine.h`.
It is a small surface, which is why this split is tractable. Current sizes on
x86-64: `Command` 112 B, `Event` 32 B, `RtClip` 80 B, `RtChain` 72 B,
`Ring<Command,1024>` 112 KiB.

### 2.1 Commands: GUI to engine

`Ring<Command, 1024>`, one producer (GUI thread), one consumer (audio thread,
drained at the top of `Engine::process`). Twenty command types:

| Cmd | fields used | crosses cleanly? |
|---|---|---|
| `SetPlaying` | `a` = 0/1 | yes |
| `SetTempo` | `x` = BPM | yes |
| `SetQuantum` | `a` = index into `kQuantumBeats` | yes |
| `SetMetronome` | `a` = 0/1 | yes |
| `LaunchClip` | `a` = track, `b` = slot | yes |
| `StopTrack` | `a` = track | yes |
| `LaunchScene` | `a` = scene | yes |
| `StopAll` | — | yes |
| `SetClip` | `a` = track, `b` = slot, `clip` = `RtClip` | **no — `RtClip::data`** |
| `ClearClip` | `a` = track, `b` = slot | yes |
| `TrackVol` | `a` = track, `x` = gain | yes |
| `TrackPan` | `a` = track, `x` = -1..1 | yes |
| `TrackMute` | `a` = track, `b` = 0/1 | yes |
| `TrackSolo` | `a` = track, `b` = 0/1 | yes |
| `TrackArm` | `a` = track, `b` = 0/1 | yes |
| `MasterVol` | `x` = gain | yes |
| `ClipGain` | `a` = track, `b` = slot, `x` = gain | yes |
| `ClipWarp` | `a` = track, `b` = slot, `x` = `Warp` | yes |
| `ClipLoop` | `a` = track, `b` = slot, `x` = 0/1 | yes |
| `SetChain` | `a` = track, `p` = `RtChain*` | **no — `p` and `RtChain::fx`** |

Eighteen of twenty are already pure scalars. The two that are not are the whole
job.

`Command` also carries a full 80-byte `RtClip` inline, used by exactly one
command type. That is 71 % of the message wasted on nineteen of twenty sends.
Worth fixing in passing (a `SetClip` variant, or a union), but not required.

### 2.2 Events: engine to GUI

`Ring<Event, 1024>`, produced on the audio thread, drained by the GUI once per
frame.

| Ev | fields | crosses cleanly? |
|---|---|---|
| `ClipStarted` | `a` = track, `b` = slot, `x` = beat | yes |
| `ClipStopped` | `a` = track | yes |
| `TrackStopped` | `a` = track, `x` = beat | yes |
| `Xrun` | — | yes (currently never pushed) |
| `TransportStopped` | — | yes |
| `ChainRetired` | `a` = track, `p` = `RtChain*` now safe to free | **no — `p`** |

Five of six are clean. `ChainRetired` exists only because ownership is split
across the thread boundary; once the engine owns its plugins outright
(§3.6) the event disappears rather than being ported.

### 2.3 The polled atomics block

Public `std::atomic` members on `Engine`, written by `Engine::publish()` once
per block with relaxed stores, read by the GUI once per frame:

| member | type | notes |
|---|---|---|
| `beat` | `f64` | absolute beats since transport start |
| `playing` | `bool` | **`std::atomic<bool>` has implementation-defined size — must become `u32` on the wire** |
| `tempo` | `f64` | |
| `cpu` | `f32` | smoothed block load, % |
| `slotState[32]` | `int` | `SlotState` of the active slot |
| `activeSlot[32]` | `int` | -1 = none |
| `pendingSlot[32]` | `int` | -1 = queued stop, -2 = nothing queued |
| `clipPhase[32]` | `f64` | 0..1 through the running clip |
| `meterL/R[32]` | `f32` | peak, decayed by 0.72 per block |
| `masterMeterL/R` | `f32` | |

This is ~1.3 KiB of scalars with no cross-field invariants, which is why
`SharedStateT` in `src/ipc/shm.h` is a straight relocation: the same relaxed
stores into shared memory, plus a `generation`/`heartbeatNs` pair the current
design has no need for (§4.4).

### 2.4 Pointer smuggling 1: `RtClip::data`

```c++
struct RtClip {
    const f32* data = nullptr;   // interleaved, already at engine rate
    ...
};
```

The GUI loads a file into a `SampleBuffer` (`std::vector<f32>` on the GUI heap),
keeps it alive in `ClipModel::sample` (a `shared_ptr`), and ships the raw
`data()` pointer into the engine inside `Cmd::SetClip`. The engine reads it
directly from voices for the lifetime of the session. The GUI's obligation —
"keep the buffer alive for as long as the engine might reference it" — is
enforced by nothing but the `SampleRef` staying in `ClipModel`.

Across a process boundary this pointer is meaningless. The fix is a shared
sample pool and an offset handle (§3.5).

### 2.5 Pointer smuggling 2: `RtChain::fx`

```c++
struct RtChain {
    PluginInstance* fx[kMaxChainFx] = {};
    int count = 0;
};
```

`App::publishChain(t)` heap-allocates an `RtChain` on the GUI side, fills it
with pointers to `PluginInstance`s owned by `DeviceModel::inst`
(`std::unique_ptr`), and sends the chain pointer through `Command::p`. The audio
thread swaps the pointer and never frees. The old pointer comes back as
`Ev::ChainRetired`, and only then may the GUI free the chain and any instances
the new chain dropped (`App::retiring_`).

So the boundary smuggles *two* levels of pointer: the chain itself, and eight
plugin instances inside it whose `process()` runs on the audio thread but whose
`setParam()` is called concurrently from the GUI. None of that can survive a
process split — the plugin has to move (§3.6).

### 2.6 What else crosses without being a message

- `Engine::prepare(sampleRate, maxBlock)` is called by the backend
  (`backend.cpp`) at start and from JACK's sample-rate callback. In the split,
  the backend lives in the daemon, so this stops crossing entirely — but the
  GUI needs to *learn* the rate, because `loadSample()` resamples to it. It
  becomes `SharedState::sampleRate` / `blockSize`, published at handshake and on
  every change, plus an `Ev::FormatChanged` so the GUI can re-resample.
- `Engine::sampleRate()` is read directly by GUI code. Same replacement.
- `PluginRegistry` and `PluginInstance::setParam/getParam/paramInfo` are called
  straight from GUI code with no message at all. §3.6 and §3.7.

---

## 3. Target architecture

### 3.1 Processes and who owns what

```
  ┌─────────────────────────────┐            ┌──────────────────────────────┐
  │ lattice   (GUI client)      │            │ latticed  (engine daemon)    │
  │                             │            │                              │
  │  Window / Renderer / Ui     │            │  AudioBackend (JACK/ALSA)    │
  │  Session model (strings,    │            │  Engine  (RT thread)         │
  │    paths, undo, project io) │            │  DeviceTable (PluginInstance)│
  │  SampleBuffer decode        │            │  PluginRegistry (scan)       │
  │  sample pool ALLOCATOR      │            │  sample pool READER          │
  └───────┬─────────────┬───────┘            └───────┬───────────────┬──────┘
          │             │                            │               │
          │      ┌──────┴────────────────────────────┴──────┐        │
          │      │ control region  (shm, created by latticed)│        │
          │      │   SharedState │ cmd ring │ evt ring       │        │
          │      └───────────────────────────────────────────┘        │
          │                                                            │
   ┌──────┴────────────────────────────────────────────────────────────┴────┐
   │ session region  (shm, created by the GUI): sample pool + param table   │
   └────────────────────────────────────────────────────────────────────────┘
                    AF_UNIX socket: handshake, fd passing, strings
```

Two regions, not one, because their lifetimes genuinely differ:

- The **control region** lives exactly as long as the engine. `latticed` creates
  it, initialises the rings and `SharedState`, calls `publishReady()`, and
  unlinks it on exit. If the engine dies the region is stale and the next
  daemon reaps it (`ShmRegion::reapIfStale`).
- The **session region** (sample pool + param table) lives as long as the
  *session*, which must outlive a GUI crash (§4.3) — so the GUI creates it and
  the daemon attaches. On a GUI crash the region is not unlinked (no
  destructor ran), the daemon keeps its mapping and keeps playing, and the
  replacement GUI attaches to the same name and takes ownership back
  (`ShmRegion::adoptOwnership()`, one of two small additions wave 2 needs).

This is exactly why `ShmRegion` has an explicit creator/attacher role instead of
assuming the server creates everything.

### 3.2 Two channels: rings for the hot path, a socket for the rest

The shm rings carry fixed-size, pointer-free, high-rate messages. They cannot
carry a file path, a plugin URI, a project, or an error string, and stretching
them to do so (chunked string messages) would be a mistake.

So there is a second channel: an `AF_UNIX` `SOCK_SEQPACKET` socket at
`$XDG_RUNTIME_DIR/lattice/engine-<session>.sock`. It carries:

- the handshake (versions, build ids, the control region's name),
- file descriptors via `SCM_RIGHTS` (the session region, once it is a `memfd`),
- the plugin catalog (`PluginDesc` list: URIs, names, vendors, param counts),
- project load/save notifications and paths,
- human-readable errors.

It also gives crash detection for free: the peer's death closes the socket and
the survivor gets EOF immediately, with no polling. Heartbeats
(`SharedState::heartbeatNs`) remain necessary for the *other* failure — an
engine that is alive but no longer rendering.

Nothing on the socket is realtime; it is serviced from a normal thread on both
sides.

### 3.3 The control region map

Offsets are computed from `constexpr` sizes on both sides and folded into the
layout hash, so a mismatched build fails at `attach()` rather than reading a
ring through the wrong offset. `tests/ipc_test.cpp` already does exactly this;
the real map is the same shape:

| offset | contents | size |
|---|---|---|
| 0 | `ShmHeader` (magic, version, layout hash, creator pid + start ticks, ready flag) | 256 B fixed |
| `kState` | `ipc::SharedState` | ~1.4 KiB |
| `kCmds` | `ShmSpscRing<WireCommand, 4096>` | ~256 KiB |
| `kEvts` | `ShmSpscRing<WireEvent, 4096>` | ~128 KiB |

Ring capacity goes from 1024 to 4096 because a process boundary makes bursts
worse, not better: the GUI can now be descheduled for a whole frame while a
scene launch queues 32 commands. The engine drains once per audio block
regardless.

### 3.4 The wire types

```c++
struct WireCommand {          // 48 B, no pointers, no padding surprises
    u32 type;                 // Cmd
    u32 flags;
    i32 a, b;
    f64 x;
    u64 ref;                  // pool offset / device id / chain generation
};

struct WireEvent {            // 32 B
    u32 type;                 // Ev
    u32 flags;
    i32 a, b;
    f64 x;
    u64 ref;
};

struct WireClip {             // sent by Cmd::SetClip through a side channel
    u64 poolRef;              // byte offset into the sample pool  <-- was const f32*
    i64 frames, loopStart, loopEnd;
    f64 clipBpm, lengthBeats;
    f32 gain;
    i32 channels, warp, quantumIdx;
    u32 loop, valid;          // u32, not bool
};
```

`SetClip` is the one command with a payload that does not fit in 48 bytes. Two
options, both fine: a dedicated `ShmSpscRing<WireClip, 256>` paired with a
`Cmd::SetClip` carrying the clip-ring index, or a clip *table* in the control
region (`WireClip[32][32]`, written by the GUI, with `Cmd::SetClip` telling the
engine which cell changed). The table is simpler, is naturally idempotent, and
makes republish-after-engine-restart (§4.4) a memcpy — prefer it.

`std::atomic<bool>` never appears; `bool` never appears. Fixed width only, and
`static_assert(std::is_trivially_copyable_v<T>)` on every wire type — the ring
enforces this already.

### 3.5 The sample pool

**Shape.** One shm object per session. The GUI reserves an address range with
`mmap(PROT_NONE)` at startup (8 GiB of *address space*, no commit) so the base
never moves, then grows the file with `ftruncate` and commits sub-ranges with
`MAP_FIXED`. The engine does the same reservation and commits on its control
thread when told to. Because the base never moves, a handle is a single `u64`
byte offset and the RT read path is `pool_ + poolRef` — one add.

The alternative, a chain of fixed 64 MiB segments with `(segment, offset)`
handles, avoids the reservation trick at the cost of one extra indirection in
the RT path. Keep it in reserve for platforms where the reservation is awkward.

**Growth is never lazy on the audio thread.** `mmap` in the audio callback is
forbidden. Sequence: GUI grows the file → `Cmd::PoolGrow{newBytes}` → engine's
control thread commits the new range → `Ev::PoolGrown{newBytes}` → only now may
the GUI publish a clip whose data lives past the old end. A clip referencing an
uncommitted offset is a bug the engine rejects at command-drain time (bounds
check against the committed size, cheap, non-RT-hostile).

**`memfd` and sealing.** Once the socket exists, prefer `memfd_create` + fd
passing over `shm_open` by name, and apply `F_SEAL_SHRINK` after creation. This
is not paranoia: with a plain shm object either side can `ftruncate` it smaller,
and the audio thread then takes `SIGBUS` on the next read. Sealing makes that
unrepresentable while still allowing growth. `ShmRegion` needs an
`adopt(int fd)` entry point for this — the second of the two small additions.

**Allocation.** The allocator lives in the GUI (it is the side that decodes
files), but its *metadata lives inside the pool region*, because a replacement
GUI after a crash has to be able to adopt it (§4.3):

```
pool region:
  [ PoolHeader  | BlockDesc[NBlocks] | ... sample data ... ]
```

`PoolHeader` holds the committed size, the bump pointer, and a free-list head.
Each `BlockDesc` holds `{offset, bytes, frames, channels, rate, refs, state,
key}` where `key` is a hash of `(path, size, mtime)` — that is what lets a
reattaching GUI match a reloaded project back onto blocks that are already in
memory and still playing. The GUI is the only writer of allocator metadata;
the engine only ever reads block extents. Single-writer keeps this lock-free
without any cleverness.

**Free discipline — the retirement pattern, extended.** This is the same rule
`ChainRetired` already encodes, applied to samples:

1. The GUI's `ClipModel` drops its last reference to a block, so the block's
   GUI-side refcount hits 0. **The block is not freed.**
2. GUI sends `Cmd::ReleaseSample{poolRef}`. The block goes to state `Retiring`.
3. The engine drops it from `clips_[t][s]`. A `Voice` may still be reading it
   (a clip that was just stopped is in its 6 ms declick release), so the engine
   keeps an incremental per-block reference count: +1 when a block is installed
   into `clips_`, +1 per voice that starts on it, decremented on replace and on
   voice end. Plus a fixed deferral of N blocks past zero as belt-and-braces,
   N covering the longest release tail.
4. When the count reaches zero the engine pushes `Ev::SampleRetired{poolRef}`.
5. Only on receiving that does the GUI return the block to the free list.

Freed blocks are coalesced by the GUI. The pool never shrinks during a session;
fragmentation is bounded in practice because clip buffers are large and few
(hundreds, not millions). If that stops being true, the escape hatch is a
compaction pass at a point where the transport is stopped and all blocks are
quiescent — never online.

**One large win falls out of this.** Today `loadProject` decodes every sample
into a GUI-heap vector. With the pool, decode writes straight into shared
memory: no copy at hand-off, and the engine reads the same pages the decoder
wrote.

### 3.6 Plugins move into the engine

Plugins must live in the engine process. `PluginInstance::process()` is called
from the audio callback, which is in the daemon; there is no way to keep the
instance on the GUI side without an RPC per block, which is absurd.

Consequences, in order of size:

**The chain protocol becomes id-based, and gets simpler.**

```c++
struct RtChain {
    u32 device[kMaxChainFx];   // ids into the engine's DeviceTable  <-- was PluginInstance*
    int count;
};
```

- `Cmd::AddDevice{track, uriIndex}` → the daemon's control thread instantiates
  (slow, allocates, may fail) → `Ev::DeviceReady{deviceId, paramCount}` or
  `Ev::DeviceFailed{reason}`. Instantiation is asynchronous now, which is
  honest: it always was slow, the GUI just blocked on it.
- `Cmd::SetChain{track, generation}` + an ordered id list written into a chain
  table in the control region. Same idempotent-table trick as `SetClip`.
- `Cmd::RemoveDevice{deviceId}` → `Ev::DeviceDestroyed{deviceId}` once the
  engine has actually torn it down.
- **`Ev::ChainRetired` disappears from the protocol.** Inside the daemon the
  existing pointer-swap-plus-retire dance is still exactly right, and is kept
  verbatim between the daemon's control thread and its audio thread. It simply
  stops being a cross-process concern, because the owner and the user are
  finally in the same address space. `App::retiring_`, `App::published_` and
  `DeviceModel::inst` all go away.

**Scanning moves too.** `PluginRegistry::scan()` walks every bundle on the
system with lilv; it is slow and it crashes on bad plugins. It runs in the
daemon and the catalog is shipped to the GUI over the socket
(`PluginDesc` is all strings, so it never belongs in a ring). Better still, and
cheap once we are already spawning processes: run the scan in a short-lived
`lattice-scan` child so a plugin that segfaults during discovery takes down
neither the GUI nor the engine. That is the standard arrangement and it is
strictly easier after the split than before.

**Plugin editor windows are the one genuinely hard part.** Native LV2/CLAP UIs
must run in the process that owns the instance, i.e. the daemon, and then be
embedded into the GUI's window — X11 `XEmbed` or a Wayland subsurface across a
process boundary. Lattice currently draws only its own generic knob UI from
`ParamInfo`, which keeps working unchanged through the param table (§3.7), so
this is deferred to phase 5 and does not block anything.

### 3.7 The param table

`PluginInstance::setParam()` is documented as "GUI thread writes a plain float,
the RT thread reads it, no lock" — which is already a shared-memory design. It
relocates almost unchanged.

In the session region:

```c++
struct DeviceParams {
    std::atomic<u32> paramCount;
    std::atomic<u32> generation;      // bumped by whoever writes
    std::atomic<u32> engineGeneration;// bumped only by the engine
    std::atomic<u32> bypass;
    std::atomic<f32> value[kMaxParams];
};
DeviceParams params[kMaxTracks * kMaxChainFx];   // 32*8 devices
```

At 256 params per device that is 256 KiB — trivial, and it must be preallocated
because the audio thread cannot wait for an allocation.

- **GUI → engine, continuous params** (knob drags, ~120 writes/s per knob):
  the GUI stores directly into `value[i]` and bumps `generation`. The engine
  reads the whole dirty device at the top of the block and calls
  `PluginInstance::setParam` for changed entries. No ring, therefore **no
  drops** — a dropped param write would leave the knob and the plugin
  permanently disagreeing, which is the single worst bug class here.
- **GUI → engine, structural changes** (bypass, preset load, param that must
  land in order relative to a chain change): commands, because ordering
  matters. `Cmd::SetBypass{deviceId, 0/1}`, `Cmd::LoadPreset{deviceId, ref}`.
- **Engine → GUI**: plugins change their own params (host automation, plugin
  presets, native UIs). The engine writes `value[i]` and bumps
  `engineGeneration`; the GUI compares against the generation it last saw and
  rereads the device's values. Drop-free, no event storm, and it costs nothing
  when nothing changes. Events stay reserved for structural news.
- Ownership conflict (both sides write the same param in the same block) is
  resolved last-writer-wins, exactly as today. There is no correct alternative
  short of a full automation model, and that is a separate feature.

`ParamInfo` (name, unit, min/max, flags) is static per instance and arrives once
over the socket with the `Ev::DeviceReady`.

---

## 4. Lifecycle

### 4.1 Startup and discovery

```
GUI start
  ├─ compute session id + socket path ($XDG_RUNTIME_DIR/lattice/engine-<id>.sock)
  ├─ connect()
  │    ├─ success ────────────────────► handshake (§4.2), attach, done
  │    └─ ENOENT / ECONNREFUSED
  │         ├─ ShmRegion::reapIfStale("/lattice-engine-<id>")   (crash orphan)
  │         ├─ unlink a stale socket inode
  │         ├─ posix_spawn("latticed", ...)
  │         └─ retry connect() with a 2 s deadline
  └─ on failure: report "could not start the audio engine: <reason>" and offer
     to run in-process (the phase-1..3 code path stays compiled in)
```

Two GUIs racing to spawn the daemon is resolved by the daemon itself: it binds
the socket with an exclusive create and calls `ShmRegion::create()`, which uses
`O_CREAT|O_EXCL`. The loser exits, its GUI's retry loop finds the winner. This
is why `create()` refuses a name a live process owns instead of taking it —
tested in `ipc_test` section 2.

`systemd --user` socket activation is a natural later addition and changes none
of the above; the GUI's `connect()` path is already the fast path.

### 4.2 Version handshake

Three layers, deliberately:

1. **Region**: `ShmRegion::attach()` checks magic, `kShmVersion` and the layout
   hash before it returns a mapping. A mismatch is a clean, specific failure —
   `ipc_test` section 2 asserts exactly this.
2. **Protocol**: over the socket, both sides exchange `kShmVersion` plus a build
   id. This exists so the failure can be *explained*: "engine is protocol v3,
   this build speaks v2 — restart the engine with `lattice --restart-engine`"
   beats a silent refusal to attach.
3. **Audio format**: the daemon reports `sampleRate`/`blockSize` in the
   handshake. The GUI resamples on load, so it must know the rate before it
   decodes anything.

Policy: **no compatibility window within a release.** The daemon ships in the
same binary as the GUI (`lattice --engine`, or a separate `latticed` built from
the same tree), so a version mismatch means a half-upgraded install, and the
right response is to restart the engine, not to negotiate. Bump `kShmVersion`
on every layout change; the layout hash catches the times we forget.

### 4.3 GUI crash: the engine keeps playing

The headline feature.

- The daemon gets EOF on the socket. It does **not** stop the transport.
  Default policy is keep playing (live use: the show goes on); a
  `--gui-loss=stop|fade|keep` option covers studio use. The relevant state —
  transport, chains, plugin instances, the sample pool mapping — is all daemon-
  side or in the session region, and none of it depended on the GUI being alive.
- The session region survives: the crashed GUI never ran a destructor, so it
  never unlinked the name, and the daemon still holds its mapping. Pages stay
  resident.
- The daemon marks `SharedState::engineState` unchanged (it is still running)
  and starts accepting a new client.

Reattach:

1. New GUI connects, handshakes, and learns that a session is already live
   (`sessionEpoch`, `sessionId`, the session region's name/fd).
2. It attaches to the session region as an attacher, then calls
   `adoptOwnership()` once the daemon has confirmed it is the sole client — so
   the unlink obligation transfers and the region is not leaked forever.
3. It reloads the project from the GUI's own journal
   (`$XDG_RUNTIME_DIR/lattice/session-<id>.lattice`, written by the existing
   `saveProject` on every mutation — cheap, it is line-oriented text).
4. It walks `BlockDesc[]` and matches blocks to clips by
   `key = hash(path, size, mtime)`. Matched clips **do not reload from disk**
   and, crucially, do not have their `poolRef` changed, so anything currently
   playing keeps playing without a glitch. Unmatched clips are decoded into
   fresh blocks.
5. It adopts the engine's transport state from `SharedState` — `playing`,
   `beat`, `tempo`, `activeSlot[]`, `pendingSlot[]` — rather than pushing its
   own. The engine is the authority on what is sounding; the GUI is the
   authority on what exists.
6. Params come back from the param table, which also survived.

Note what makes step 5 possible: `SharedState` already contains everything a UI
needs to render the correct picture, because it is the same block the in-process
GUI polls today.

### 4.4 Engine crash: detect, respawn, republish

Audio stops. Nothing can prevent that; the goal is to make it a hiccup rather
than a lost session.

**Detection**, two mechanisms because there are two failures:

- *Dead*: socket EOF, immediate, no polling. Also `SharedState::enginePid` no
  longer names a live process with the recorded start time
  (`ipc::processAlive`).
- *Wedged* (alive but not rendering — a hung plugin, a stalled JACK server, a
  priority-inversion deadlock): `SharedState::stale(toleranceNs)`. The tolerance
  must be generous, several hundred ms at least: a laptop resuming from suspend
  or a JACK server being restarted is not a dead engine, and respawning a second
  daemon under a live one is the worst possible outcome. Show "engine not
  responding" in the UI well before acting on it, and require either a long
  timeout or an explicit user click before killing.

**Recovery**:

1. `ShmRegion::reapIfStale("/lattice-engine-<id>")` — the crashed daemon left
   its control region behind. This is the crash-orphan case
   `ipc_test` section 5 exercises with a real `SIGKILL`.
2. Respawn `latticed`, attach to the new control region.
3. Republish the session. The sample pool is in the *session* region and the GUI
   created it, so **samples survive an engine restart** — republish is not a
   reload. Concretely: `memcpy` the clip table, push the chain table, push
   mixer state (vol/pan/mute/solo/arm/master), tempo, quantum, metronome, then
   re-add every device and re-push its params from the param table.
4. Restore transport: relaunch whatever `SharedState` said was playing, at the
   same beat if the user asked for continuation, or stopped by default. The
   engine cannot resume mid-clip meaningfully, so "stopped, with the session
   intact" is the honest default.

Total gap: dominated by plugin re-instantiation, so O(100 ms) to a few seconds
with heavy plugins. Acceptable for a crash; unacceptable as a routine path,
which is why respawn requires either a very stale heartbeat or a user click.

### 4.5 Clean shutdown

- GUI quits normally: `Cmd::Shutdown` → the daemon fades the master over ~20 ms
  (never a hard stop into the DAC), stops the backend, destroys plugins,
  `publishReady`-era region is unlinked by its creator, socket unlinked. The
  GUI unlinks the session region last.
- Daemon told to quit while a GUI is attached: `Ev::EngineStopping` +
  `engineState = StateStopping`, GUI shows it, then the same sequence.
- Either way the invariant `ipc_test` section 6 checks holds: **nothing is left
  in `/dev/shm`.** A leaked region is not just wasted RAM; it makes the next
  run's `create()` take the stale-reap path and mask real bugs.

---

## 5. Failure modes and hardening

| failure | mitigation |
|---|---|
| Engine reads an out-of-range pool offset | Bounds-check every `poolRef` against the committed pool size at command-drain time. A bad offset drops the command and pushes an event; it never reaches a voice. |
| Peer shrinks the pool under the audio thread → `SIGBUS` | `memfd` + `F_SEAL_SHRINK`. |
| Corrupt ring index from a crashed peer | `ShmSpscRing` masks indices on load, so a wild index reads the wrong slot instead of reading past the mapping. |
| Half-initialised region observed by an attacher | The `ready` flag with release/acquire; `attach()` refuses until it is set (tested). |
| Stale region blocks a new engine | `reapIfStale()` — pid **plus** `/proc` start time, never pid alone, because pid reuse would make us unlink a live session's region. |
| Wrong-version peer | magic + version + layout hash at attach; specific error text. |
| Another user reads the session | Regions are created `0600`; the socket lives under `$XDG_RUNTIME_DIR`. |
| A plugin hangs the audio thread | Already fatal today; after the split it costs the audio but not the GUI or the project. A watchdog that reports (not kills) is phase 5. |
| Message loss under burst | Rings return `false`; the GUI must handle a refused push (retry next frame) rather than dropping user intent silently. Today's code mostly ignores the return value — that has to be fixed as part of phase 1. |

---

## 6. Phased migration

Each phase is shippable on its own and leaves the app working. Note that
phases 1–3 all happen **in a single process**: by the time the processes
actually separate, the protocol change is already done and tested.

**Phase 1 — engine in-process, but everything already flows through shm.**
`App` creates one `ShmRegion` at startup and the `Engine` attaches to it (same
process, two mappings, or one mapping shared — either is fine). `Ring<Command>`
and `Ring<Event>` are replaced by `ShmSpscRing<WireCommand>` /
`ShmSpscRing<WireEvent>`; the public atomics on `Engine` are replaced by reads
and writes of `ipc::SharedState`. `SetClip` moves to the clip table; `SetChain`
keeps its pointer *for now* via a side table indexed by `Command::ref`.
Deliverable: identical behaviour, `engine_test` green, and every "the GUI passes
a pointer" site is now visible and enumerated. Risk: low. Reversible: yes.

**Phase 2 — the sample pool.** Session region, allocator with in-region
metadata, `RtClip::data` → `poolRef`, `Cmd::ReleaseSample` /
`Ev::SampleRetired`, decode-straight-into-shm. `tools/render.cpp` and
`tools/gen_demo.cpp` get a trivial in-process pool so they keep working
headless. Deliverable: no raw sample pointer crosses the boundary. Risk: medium
— this is where use-after-free would hurt. Mitigation: an ASan build of the
headless tools plus a soak test that adds and removes clips under playback.

**Phase 3 — devices by id.** `DeviceTable` in the engine, `RtChain::fx` →
`u32 device[]`, `AddDevice`/`RemoveDevice`/`DeviceReady`/`DeviceDestroyed`, the
param table, `DeviceModel::inst` deleted, `App::retiring_` deleted. The
retire-on-swap dance stays, internal to the engine side. Deliverable: no plugin
pointer crosses the boundary; the GUI no longer owns anything the audio thread
touches. Risk: medium (async instantiation changes UI flow: an "instantiating…"
device state has to exist).

**Phase 4 — actually split.** New `src/main_engine.cpp` → `latticed`: creates
the control region, opens the backend, runs `Engine`, serves the socket. The GUI
learns to spawn/attach. Because phases 1–3 removed every pointer, this phase is
build system + lifecycle, not protocol. Ship behind
`LATTICE_ENGINE=inproc|daemon`, default `inproc`, until it has real hours on it.
Deliverable: a GUI crash no longer stops the audio. Risk: medium, concentrated
entirely in lifecycle.

**Phase 5 — hardening and the good parts.** Crash recovery both directions
(§4.3, §4.4), out-of-process plugin scanning, the eventfd doorbell for a
low-power idle mode, plugin editor window embedding, and the things the split
unlocks: headless `latticed` with no GUI at all, a second UI attached for
control-surface duty, remote control.

---

## 7. Open questions

- **Session identity.** One daemon per user, per project, or per GUI? Per user
  with multiple sessions is the most useful and the most work (the audio
  backend is a shared resource). Starting point: one daemon per session id,
  where the session id defaults to a hash of the project path.
- **Who owns the audio device on a GUI-less start?** If the daemon can be
  started by systemd before any GUI, it holds the JACK client for the whole
  login session. Probably right, possibly surprising.
- **MIDI input** does not exist yet. When it does, it lands in the daemon and
  needs a third ring (engine → GUI, high rate, lossy is acceptable for display
  but not for recording).
- **Recording/arming** (`Cmd::TrackArm` exists but does nothing) will want to
  write audio from the engine process, which raises "who owns the file" —
  probably the daemon writes, the GUI is told the path.
- **Compaction of the sample pool** after a long session with many clip swaps.
  Deferred until measured.
- **`Command` is 112 B for a 16 B payload** in 19 of 20 cases. Phase 1 fixes
  this for free by moving to `WireCommand` + tables.

---

## 8. What wave 1 actually delivered

- `src/ipc/shm.h` — header-only.
  - `ShmRegion`: `shm_open` + `mmap` with a validated header (magic, protocol
    version, layout hash, total size, creator pid + `/proc` start time, ready
    flag). Explicit creator/attacher roles; the creator unlinks and attachers
    never do; `create()` refuses a name a live process owns and reclaims one
    whose creator is gone; `reapIfStale()` / `forceUnlink()` are the crash-
    orphan cleanup hooks; `at<T>(offset)` is bounds- and alignment-checked so a
    layout mistake is a startup failure instead of a SIGSEGV on the audio
    thread.
  - `ShmSpscRing<T, N>`: `lat::Ring`'s contract, relocated into a region at a
    caller-given offset. Static assertions for lock-free indices and trivially
    copyable `T`; indices masked on load so a wild peer cannot walk off the
    mapping. No doorbell, deliberately — both ends are already poll-based (GUI
    at frame rate, engine at block rate), so parity needs none, and a blocking
    wait must never enter the audio callback. `eventfd`/futex is noted as the
    future low-power idle path.
  - `SharedStateT<NTracks>` / `SharedState`: the polled atomics block as plain
    relaxed atomics in shared memory, mirroring `Engine::publish()`, with fixed
    width types only (no `std::atomic<bool>`), Engine's sentinels preserved
    (`activeSlot` -1, `pendingSlot` -2), plus `generation` / `heartbeatNs` /
    `enginePid` for liveness.
- `tests/ipc_test.cpp` — 54 checks: region create/attach/validate, mismatch
  handling (version, layout, missing, malformed, squatting on a live name),
  full-ring backpressure with post-saturation integrity, a `fork()`ed
  parent/child exchanging 100 000 commands and 100 000 events **concurrently**
  through two rings with strict FIFO and payload verification on both sides,
  cross-process `SharedState` publication, crash-orphan reaping via a real
  `SIGKILL`, and a final assertion that `/dev/shm` is clean.
- `Makefile`: `build/ipc_test`, wired into `make test`.

Two additions `ShmRegion` will need in wave 2, both small and both already
called out above: `adopt(int fd)` for the `memfd` + `SCM_RIGHTS` path (§3.5),
and `adoptOwnership()` for transferring the unlink obligation to a replacement
GUI (§4.3).
