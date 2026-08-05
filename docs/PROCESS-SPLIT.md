# Splitting NxTakt into an engine process and a GUI process

Status: **phase 3 shipped — `nxtaktd` hosts the plugins (§11).**
Wave 1 landed the transport (`src/ipc/shm.h`, `tests/ipc_test.cpp`) and this
document; wave 2 landed the daemon and a scalar control plane (§9); wave 3
landed the sample pool, the clip table and protocol v2 (§10); wave 4 moved the
plugin layer into the daemon — devices, the param table, protocol v3, and the
exact sample retirement §10 asked for (§11). `src/ui` still runs an in-process
`Engine` and is still the shipping path.

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
9. [Phase 1 shipped](#9-phase-1-shipped)
10. [Phase 2 shipped — the sample pool](#10-phase-2-shipped--the-sample-pool)
11. [Phase 3 shipped — the plugins move in](#11-phase-3-shipped--the-plugins-move-in)

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
  │ nxtakt    (GUI client)      │            │ nxtaktd   (engine daemon)    │
  │                             │            │                              │
  │  Window / Renderer / Ui     │            │  AudioBackend (JACK/ALSA)    │
  │  Session model (strings,    │            │  Engine  (RT thread)         │
  │    paths, undo, project io) │            │  DeviceTable (PluginInstance)│
  │  SampleBuffer decode        │            │  PluginRegistry (scan)       │
  │  sample pool ALLOCATOR      │            │  sample pool READER          │
  └───────┬─────────────┬───────┘            └───────┬───────────────┬──────┘
          │             │                            │               │
          │      ┌──────┴────────────────────────────┴──────┐        │
          │      │ control region  (shm, created by nxtaktd) │        │
          │      │   SharedState │ cmd ring │ evt ring       │        │
          │      └───────────────────────────────────────────┘        │
          │                                                            │
   ┌──────┴────────────────────────────────────────────────────────────┴────┐
   │ session region  (shm, created by the GUI): sample pool + param table   │
   └────────────────────────────────────────────────────────────────────────┘
                    AF_UNIX socket: handshake, fd passing, strings
```

Two regions, not one, because their lifetimes genuinely differ:

- The **control region** lives exactly as long as the engine. `nxtaktd` creates
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
`$XDG_RUNTIME_DIR/nxtakt/engine-<session>.sock`. It carries:

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
`nxtakt-scan` child so a plugin that segfaults during discovery takes down
neither the GUI nor the engine. That is the standard arrangement and it is
strictly easier after the split than before.

**Plugin editor windows are the one genuinely hard part.** Native LV2/CLAP UIs
must run in the process that owns the instance, i.e. the daemon, and then be
embedded into the GUI's window — X11 `XEmbed` or a Wayland subsurface across a
process boundary. NxTakt currently draws only its own generic knob UI from
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
  ├─ compute session id + socket path ($XDG_RUNTIME_DIR/nxtakt/engine-<id>.sock)
  ├─ connect()
  │    ├─ success ────────────────────► handshake (§4.2), attach, done
  │    └─ ENOENT / ECONNREFUSED
  │         ├─ ShmRegion::reapIfStale("/nxtakt-engine-<id>")   (crash orphan)
  │         ├─ unlink a stale socket inode
  │         ├─ posix_spawn("nxtaktd", ...)
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
   this build speaks v2 — restart the engine with `nxtakt --restart-engine`"
   beats a silent refusal to attach.
3. **Audio format**: the daemon reports `sampleRate`/`blockSize` in the
   handshake. The GUI resamples on load, so it must know the rate before it
   decodes anything.

Policy: **no compatibility window within a release.** The daemon ships in the
same binary as the GUI (`nxtakt --engine`, or a separate `nxtaktd` built from
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
   (`$XDG_RUNTIME_DIR/nxtakt/session-<id>.lattice`, written by the existing
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

1. `ShmRegion::reapIfStale("/nxtakt-engine-<id>")` — the crashed daemon left
   its control region behind. This is the crash-orphan case
   `ipc_test` section 5 exercises with a real `SIGKILL`.
2. Respawn `nxtaktd`, attach to the new control region.
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

**Phase 4 — actually split.** New `src/main_engine.cpp` → `nxtaktd`: creates
the control region, opens the backend, runs `Engine`, serves the socket. The GUI
learns to spawn/attach. Because phases 1–3 removed every pointer, this phase is
build system + lifecycle, not protocol. Ship behind
`NXTAKT_ENGINE=inproc|daemon`, default `inproc`, until it has real hours on it.
Deliverable: a GUI crash no longer stops the audio. Risk: medium, concentrated
entirely in lifecycle.

**Phase 5 — hardening and the good parts.** Crash recovery both directions
(§4.3, §4.4), out-of-process plugin scanning, the eventfd doorbell for a
low-power idle mode, plugin editor window embedding, and the things the split
unlocks: headless `nxtaktd` with no GUI at all, a second UI attached for
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

---

## 9. Phase 1 shipped

Wave 2 built the daemon rather than the in-process shm swap. That is a
deliberate reordering of §6: the doc's phase 1 rewires `App`/`Engine` onto
`ShmSpscRing` inside one process, and its phase 4 splits the processes. We did
the *lifecycle* first — `nxtaktd` exists, creates the control region, and
serves a real second process — with the protocol restricted to the eighteen
commands that already cross cleanly. The reason is that the risky half of the
split turned out to be lifecycle (who creates, who unlinks, who notices a
death), not message shape, and lifecycle is far cheaper to get right against a
protocol with nothing in it that needs a sample pool. Nothing in `src/audio`,
`src/ui`, `src/core` or `src/plugin` changed; the in-process GUI is untouched
and still the shipping path.

### What exists now

- **`src/daemon/nxtaktd.cpp`** — the engine daemon.
  - `nxtaktd [--session NAME] [--driver null|auto|jack|alsa] [--rate HZ]
    [--block FRAMES] [--verbose]`. The region is `/nxtakt-engine-<session>`;
    the session defaults to `$NXTAKT_SESSION`, then `"default"`.
  - Owns an `Engine` and either a real backend via `createBackend()` (honouring
    `NXTAKT_AUDIO`, same knob the GUI uses) or the **null driver**: no audio
    device, one thread calling `Engine::process(nullptr, nullptr, l, r, 256)`
    against `CLOCK_MONOTONIC` at block cadence. Jitter is expected and
    irrelevant; what the null driver guarantees is the *average* rate, because
    the beat clock is derived from frames rendered. It is deadline-based and
    renders the blocks it owes after a scheduling hiccup (up to 32 at once,
    then it resynchronises and logs) rather than quietly losing musical time.
    That is what makes "beats per wall-clock second == tempo/60" a testable
    property on a loaded CI box.
  - Three threads: the audio thread (backend callback or null driver), the
    **pump** (main, 1 ms) and the **mirror** (4 ms). The pump drains the command
    ring into `Engine::pushCommand`, the MIDI ring into `Engine::pushMidi`, and
    `Engine::popEvent` into the event ring. The mirror copies Engine's published
    atomics into `SharedState` and stamps the heartbeat.
  - Claims the region with `create()` (`O_CREAT|O_EXCL`) *before* opening the
    audio device, so two daemons racing for one session resolve exactly as §4.1
    describes: the loser exits 1 and says so.
  - `SIGTERM`/`SIGINT`/`SIGHUP` → stop the mirror, stop the backend, publish
    `engineState = StateStopping` plus `ControlHeader::shutdown`, push
    `EvEngineStopping`, unlink. A still-attached client keeps its mapping and
    can therefore tell "went away cleanly" from "died" with no socket. A fatal
    signal unlinks the name from the handler and re-raises, so a crash leaves no
    orphan either — and when even that is impossible (`SIGKILL`), the next
    `create()`/`attach()` reaps it via `reapIfStale()`.
- **`src/ipc/control.h`** — the protocol: `WireCommand`/`WireEvent` (32 B each,
  pointer-free, with the `ref` field the pool handle and device id will use),
  `WireMidi`, `ControlHeader`, the region map, and the layout hash. Also the
  phase-1 policy tables (`commandIsScalar`, `eventIsScalar`) that both sides
  agree on by construction.
- **`src/ipc/client.h`** — `EngineClient`: `attach()` with the three-layer
  handshake, `pushCommand`/`popEvent`/`pushMidi`, `state()`, heartbeat- and
  pid-based `alive(tolerance)`, `reapStale()`, and a `fork`+`execv`
  `spawnDaemon()`/`waitFor()` pair. Dependency-light on purpose: `core/`,
  `audio/engine.h` for the enums, libc. This is the API the GUI adopts next.
- **`tests/daemon_test.cpp`** — 85 checks against a real spawned daemon:
  handshake, beat clock vs wall clock, metronome and `MasterVol` round-tripped
  through the *rendered audio* via the published master meter, every refusal
  path, a 3000-command burst that must be deferred rather than dropped, `SIGKILL`
  → `alive()` false → orphan reaped → respawn, `SIGTERM` → exit 0 → `/dev/shm`
  clean. Runs under `make test`; clean under ASan+UBSan.
- **`Makefile`** — `build/nxtaktd` (daemon + `engine.cpp` + `backend.cpp` +
  `common.cpp`, no GUI and no plugin sources: `engine.cpp`'s use of
  `PluginInstance` is header-only virtual dispatch) and `build/daemon_test`,
  both in `test`. `src/daemon` is filtered out of the GUI app's `SRC` sweep.

### Deliberate deviations from the design above

1. **No AF_UNIX socket yet (§3.2).** The handshake lives in `ControlHeader`
   (protocol version, pid, driver name) and the audio format in `SharedState`.
   Everything the socket is *for* — strings, fds, the plugin catalog — belongs
   to features phase 1 does not have. Crash detection therefore uses the
   heartbeat and `processAlive()` rather than EOF; that path had to exist
   anyway for the wedged-but-alive case.
2. **The region map has five sections, not three** (§3.3): `ControlHeader`
   ahead of `SharedState`, and a third ring for MIDI. `ShmHeader` is the
   transport's header and must not grow protocol fields — `kShmVersion` and
   `kProtocolVersion` change for different reasons. The MIDI ring exists
   because `Engine::pushMidi()` already does, and a daemon that could not carry
   MIDI would be a regression against the in-process build.
3. **`Engine` keeps its own `lat::Ring`s.** The daemon bridges shm ring →
   engine ring instead of the engine consuming the shm ring directly, and the
   mirror thread copies the published atomics instead of `Engine::publish()`
   writing `SharedState`. Both are the doc's phase 1 proper and both require
   editing `src/audio`, which this wave did not own. The cost is one extra copy
   per command and a ≤4 ms staleness on the state block; the wire protocol the
   GUI will speak is already the final one, so this is an internal detail to
   delete later, not a design decision to revisit.
4. **`Cmd::ClearClip` is refused** even though §2.1 lists it as clean. In phase
   1 no clip can exist (its `SetClip` is refused), so it has nothing to clear,
   and clearing a clip with notes pushes `Ev::NotesRetired`, which hands a GUI
   pointer back across the boundary. It becomes legal in phase 2 with the clip
   table. **Superseded by §10:** `SetClip` and `ClearClip` are both accepted
   now, and `Ev::NotesRetired` is translated into an offset rather than
   dropped.
5. **`WireCommand` is 32 B, not the 48 B sketched in §3.4** — same fields, real
   packing.
6. **`SharedStateT` gained `recState[]`/`recSlotIdx[]`** so it mirrors Engine's
   published atomics exactly; `kShmVersion` is now 2.
7. **`EngineClient::attach()` reaps stale regions after a failed attach, never
   before it.** `reapIfStale()` treats an existing-but-unsized region as an
   orphan, which is precisely what a daemon between `shm_open()` and
   `ftruncate()` looks like, so the tidy-looking pre-emptive reap can unlink a
   live engine's region microseconds after it claimed the name. §4.1's ordering
   should be read as "reap when connect fails", which is what this does.
8. **`generation` is bumped with release ordering**, not relaxed, so a reader
   that samples on a generation change sees the values that went with it. That
   is what lets `daemon_test` measure the beat clock without a seqlock, and it
   costs one store barrier per 4 ms on a non-RT thread.

### Explicitly deferred

*(This list is phase 1's. §10 says which items it closed.)*

- **The sample pool and the clip table** (§3.5, §3.4). `Cmd::SetClip` is
  refused at the boundary with `RejectPointerPayload`; no audio material can
  reach the daemon, so the only sound it can make is the metronome.
  **— done in §10.**
- **Chains, devices and the param table** (§3.6, §3.7). `Cmd::SetChain` is
  refused; `Ev::ChainRetired` is dropped and counted. The daemon hosts no
  plugins and runs no scan.
- **Recording.** `RecordSlot`/`RecordMidiSlot` carry GUI-owned buffers and are
  refused. `SharedState` already carries the record indicators, unused.
- **The session region**, `ShmRegion::adopt(int fd)`, `adoptOwnership()`,
  `memfd` + `F_SEAL_SHRINK`, and GUI-crash reattach (§4.3). None of it is
  reachable until there is a pool to put in it. **— the session region is done
  in §10; the `memfd`/fd-passing and reattach parts are not, and §10 says why.**
- **`SharedState::xruns` and `blocksRendered` for device backends.** The null
  driver publishes its block count; a real backend's is not observable without
  editing `src/audio`.
- **Mixer scalars in `SharedState`.** `Engine::publish()` does not publish
  vol/pan/mute/solo/arm, so `TrackVol`/`TrackMute` are only observable at the
  boundary's own counters today. Phase 2 should publish them, since a
  reattaching GUI needs them anyway (§4.3 step 5).
- **The GUI itself.** `src/ui` still owns an in-process `Engine`. `EngineClient`
  has no callers outside the test.

### The exact next step for phase 2

*(Written before phase 2 landed. Steps 2 and 3 shipped; step 1 did not, because
it edits `src/audio`, and step 4 is now the next thing. §10 has the details.)*

In order, each step leaving the tree green:

1. **Publish from the engine, not the mirror.** Give `Engine` an optional
   `ipc::SharedState*` and have `Engine::publish()` write it directly (adding
   vol/pan/mute/solo/arm and the xrun counter while the file is open). Delete
   the daemon's mirror thread. This is the first edit inside `src/audio` and it
   is a pure relocation of stores that already exist.
2. **The clip table.** `WireClip clips[32][32]` in the control region, written
   by the client, with `Cmd::SetClip{track, slot}` telling the engine which
   cell changed — the idempotent-table form §3.4 prefers, because
   republish-after-engine-restart then becomes a `memcpy`. `poolRef` stays 0
   until step 3, so this step ships with silence and a bounds check.
3. **The sample pool.** Session region created by the client, `PoolHeader` +
   `BlockDesc[]` + bump allocator with in-region metadata,
   `Cmd::PoolGrow`/`Ev::PoolGrown`, `Cmd::ReleaseSample`/`Ev::SampleRetired`,
   and `RtClip::data` = `pool_ + poolRef` resolved at command-drain time with a
   bounds check against the committed size. Un-refuse `SetClip`/`ClearClip`.
   ASan soak: add and remove clips under playback.
4. **Point the GUI at `EngineClient` behind `NXTAKT_ENGINE=inproc|daemon`,
   default `inproc`.** `App` keeps its `Engine` for the in-process path; the
   daemon path spawns/attaches per §4.1. This is where the "GUI crash no longer
   stops the audio" claim first becomes true, and it needs real hours before it
   becomes the default.

Devices by id, the param table and the socket follow as the doc's phases 3–5,
unchanged.

---

## 10. Phase 2 shipped — the sample pool

`nxtaktd` can play clips. A clip's audio is decoded (or, in the tests,
synthesised) into a shared region the *client* owns, published as a `u64` byte
offset, translated back into a `const f32*` by the daemon, and rendered by an
`Engine` in another process. `src/audio` is still untouched, `src/ui` is still
untouched, and no pointer crosses the boundary in either direction.

Protocol version 2. `kShmVersion` stays at 2 — `ShmHeader`, `ShmSpscRing` and
`SharedStateT` did not change — but the control region's layout hash did, so a
phase-1 binary and a phase-2 binary refuse each other at `attach()` with a
specific message rather than reading a clip table that is not there.

### 10.1 What exists now

- **`src/ipc/pool.h`** — header-only, the session region.
  - `PoolHeader` + inline `PoolBlock` descriptors + a bump/free-list arena.
  - `SamplePool`: the writer side. Create, attach, allocate, write samples or
    notes, and the free-after-confirm state machine (§10.3).
  - `PoolReader`: the reader side. Attaches **`PROT_READ`**, so "the engine only
    reads the pool" is a page permission and not a comment. It can validate an
    offset and turn it into a `const` pointer; it has no way to acquire
    allocator state, because it has none.
  - `poolValidate()` — the single place an untrusted `u64` becomes a pointer.
  - `WireNote`, asserted to mirror `RtNote` field for field, so a notes block is
    reinterpreted rather than converted and a 10 000-note clip costs nothing at
    the boundary.
- **`src/ipc/control.h`** — protocol v2.
  - `WireClip` (120 B): every `RtClip` scalar, with `sampleRef`/`notesRef` where
    `data`/`notes` used to be, plus a per-cell `generation`.
  - A `WireClip[32][32]` **clip table** as a sixth region section — the
    idempotent form §3.4 prefers — written by the client, read by the daemon.
    `Cmd::SetClip{a=track, b=slot, ref=generation}` says which cell moved.
  - The pool handshake in `ControlHeader` (`poolName`, `poolBytes`, `poolEpoch`
    written by the *client*; `poolAttachedEpoch`, `poolAttachFailures` written
    by the daemon), and the counters `clipsApplied` / `blocksRetired`.
  - New events: `EvClipAck`, `EvBlockRetired`, `EvPoolAttached`. New reject
    reasons: `RejectNoPool`, `RejectBadPoolRef`, `RejectBadClip`.
  - The policy table grew a third class: `commandIsScalar` (17),
    `commandIsPooled` (`SetClip`, `ClearClip`), `commandCarriesPointer`
    (`SetChain`, `RecordSlot`, `RecordMidiSlot` — still refused, phase 3).
- **`src/ipc/client.h`** — `EngineClient` gained `createPool`/`attachPool`/
  `publishPool`/`closePool`/`abandonPool`, `poolWrite`/`poolWriteNotes`/
  `poolRelease`, `setClip`/`clearClip`/`clipShadow`/`clipBusy`, and
  `republishClips()`. `popEvent()` now runs `observe()` on every event so the
  client-side bookkeeping cannot be forgotten.
- **`src/ipc/shm.h`** — three additive changes, no layout change:
  `attach(..., readOnly)`, `create(..., seal)` with `trySeal()`/`sealed()`, and
  `release()` (detach without unlinking, for a hand-off).
- **`src/daemon/nxtaktd.cpp`** — `pumpPool()`, `translateClip()`, the clip
  shadow table, and the retirement queue.
- **`tests/daemon_test.cpp`** — 200 checks, thirteen sections.

### 10.2 The pool region

| | |
|---|---|
| name | `/nxtakt-pool-<session>` |
| creator | the **client**, and the client unlinks it |
| daemon | attaches `PROT_READ`, once per process lifetime |
| size | fixed at create, default 256 MiB, sparse |
| layout | `[PoolHeader][… 4 KiB …][PoolBlock][data][PoolBlock][data]…` |
| handle | one `u64` byte offset of the *data*; `poolBase + ref` is the RT read path |

**The ownership asymmetry is the feature.** The control region belongs to the
engine and dies with it; the pool belongs to the session and outlives engine
restarts, which is precisely what makes §4.4's "republish is not a reload" true.
`daemon_test` asserts it directly: `SIGKILL` the daemon with a clip playing, and
the pool is still in `/dev/shm`, still mapped, its block still `Live`, its
samples still readable, and `findByKey()` still finds it. The replacement daemon
attaches to the same region and `republishClips()` puts the session back with a
`memcpy` plus one `SetClip` per occupied cell — no decode, no offset change.

**Sizing, `ftruncate` and SIGBUS.** `/dev/shm` is tmpfs, so `ftruncate` to
256 MiB creates a sparse file that costs one page until something writes to it.
That gets §3.5's reservation trick for free and removes the growth handshake
entirely: there is no `Cmd::PoolGrow`, no window where the engine holds an
offset past the committed end, and no `mmap` on any thread but the client's.
SIGBUS has two causes and both are the writer's: writing past the object (the
allocator bounds every block, and the daemon re-checks) and writing a page tmpfs
cannot back, which faults the *decoding* thread, never the audio thread —
because a block is fully written before its offset is published, and publication
is the release store on `PoolBlock::magic`.

**Sealing.** `create(..., seal=true)` asks for `F_SEAL_SHRINK`. On a plain
`shm_open` object Linux says no — sealing is a memfd property — and
`sealed()` reports that rather than pretending. What remains is weaker and is
documented as such: after create the client closes its fd and nobody re-opens
the object `O_RDWR`, so nothing in the system can shrink it. The real fix is
`memfd_create` + `SCM_RIGHTS`, which needs the socket (§3.2).

**Allocator.** Bump pointer plus an address-ordered free list; first fit,
split when the leftover can hold a header and a line, coalesce adjacent free
blocks on every free, and **retract the bump pointer when the freed block ends
at the high-water mark**. That last rule is why freeing the pool's only block
and allocating the same size again returns the same offset — the
edit-a-clip-and-repush loop reuses one block forever instead of walking the
arena. `daemon_test` §8 asserts the offset equality, the retraction and the
empty free list, because an allocator whose behaviour is only *probably* stable
is one nobody can reason about.

Blocks are 64-byte aligned, header included (`sizeof(PoolBlock) == 128`), so
every `const f32*` the engine gets is cache-line and SIMD aligned and
`ref % 64 == 0` is a free first check on an untrusted number.

**Descriptors are inline, not a `BlockDesc[]` table** as §3.5 sketched. The
reason is validation: the daemon can check an offset knowing nothing but the
offset, because the header lives at `ref - 128` and its magic is
`kPoolBlockMagic ^ ref`. A self-mixed magic means a plausible-but-wrong offset —
off by one block, stale from a previous allocation, invented by a corrupted GUI
— fails on its own terms instead of yielding a self-consistent header
describing somebody else's samples. A side table would have to be indexed by a
number that is exactly as untrusted. §4.3's reattach key lives in the same
header, so `findByKey()` still works.

### 10.3 The free-after-confirm rule, exactly as shipped

> **A pool block may be returned to the free list only when both:**
> **(a)** the GUI holds no references of its own (`PoolBlock::refs == 0`), and
> **(b)** the block is `Quiescent` — either it was never published to a clip
> cell, or the daemon has echoed its offset back in an `EvBlockRetired` event.

Four states enforce it, and `SamplePool::free()` refuses outright on the two
middle ones:

```
Free ──alloc──► Quiescent ──markLive (a cell now names it)──► Live
                    ▲                                          │
                    │                          markDisplaced (the cell changed)
   confirmRetired (EvBlockRetired arrived)                      │
                    └──────────────── Retiring ◄────────────────┘
```

Note what is *not* in that diagram: no edge from `Live` or `Retiring` to
`Free`. A GUI that drops its last reference to a displaced block does not free
it; the free happens later, when the echo arrives, inside `popEvent()`.

**What the daemon asserts when it sends the echo.** `EvBlockRetired{ref}` means:

1. the command that displaced the block — a `SetClip` installing something else
   in that cell, or a `ClearClip` — has been handed to `Engine::pushCommand`.
   Not "the client sent it": handed over, which is why the retirement is queued
   from `commit()` and not from `translate()`;
2. no other cell of the daemon's shadow clip table still names the offset. A
   block may legitimately back several slots, and losing one of them is not a
   retirement — `daemon_test` §8 covers exactly this;
3. the audio thread has since run `drainCommands()` at least once.

**(3) is the interesting one, and it is what makes (1) sufficient.** A `Voice`
does not hold a copy of its `RtClip`; it holds `&clips_[t][s]`, and
`drainCommands()` overwrites that cell in place. So the instant the engine
drains the displacing command, every voice reading that slot is reading the
*new* clip. Unlike the `ChainRetired` case this pattern comes from, there is no
release tail over the old buffer to wait out — the 6 ms declick runs over the
new cell contents. One completed drain is the whole proof.

**How the daemon knows a drain happened**, given that `src/audio` is frozen and
cannot be asked:

- **Exactly, when the engine tells it.** Replacing or clearing a MIDI clip makes
  `Engine` push `Ev::NotesRetired` from *inside* `drainCommands`, so the event's
  arrival **is** the drain. The daemon turns that pointer back into an offset
  (`PoolReader::offsetOf`, which answers 0 for anything outside the pool, so a
  foreign pointer can never be echoed as a block) and releases the entry at
  once. `Ev::NotesRetired` is therefore translated, not dropped — the one line
  of §9's event policy that changed.
- **Conservatively otherwise, by deadline.** There is no equivalent event for
  sample data, so a sample block waits `max(100 ms, 8 block periods)` and,
  under the null driver where they can be counted, four actually-rendered
  blocks.

The deadline is the weak half and is called out rather than buried: a wedged
backend does not drain, and the deadline fires anyway. **What bounds that is the
pool's design, not the timer.** The region stays mapped for the daemon's entire
life and never shrinks, so a premature free cannot produce a wild pointer or a
segfault — the worst case is a voice reading bytes that have been reallocated to
another clip. Audible, findable, and not a crash, which is a strictly better
failure than any of the ownership bugs this phase removed. The exact version
needs `Engine` to publish a drain counter: two lines in `src/audio`, and the
first thing phase 3 should take.

> **Superseded by §11.5.** Phase 3 took it. `Engine::drains` exists,
> `nxtaktd` retires on the counter, and the deadline is now reachable only
> against an engine that does not count — a state the daemon latches and
> publishes as `ControlHeader::drainsExact` rather than leaving to inference.
> The exact rule is `drains >= k + 2` where `k` is read *after* the displacing
> `pushCommand` returned true; §11.5 has the argument for the 2.

At clean shutdown the daemon flushes every outstanding retirement after the
driver has stopped, since with no audio thread left they are all trivially true.
A client that is still attached can then free its pool cleanly instead of
leaving blocks stuck in `Retiring` — which matters, because the pool outlives
the daemon and may well be handed to the next one.

### 10.4 The clip table and the generation handshake

`SetClip`'s payload does not fit in a 32-byte message, so it travels in a table:
`WireClip clips[32][32]` in the control region, written by the client, with
`Cmd::SetClip{track, slot}` naming the cell. That is §3.4's preferred form, and
it makes republish-after-restart a `memcpy`.

A mutable table shared with a peer that reads it *later* has one hazard, and it
is not the obvious one. If the client wrote a cell twice before the daemon
popped the first command, the daemon would read the second value for both and
never learn what the first one displaced — a block retired on the client's books
and never retired on the daemon's, i.e. a permanent leak of a live-looking
block. The fix is a per-cell `generation`:

- the client bumps it on every write and refuses to overwrite a cell whose
  previous write has not been acknowledged (`setClip()` returns false, the
  caller retries next frame — the same "handle a refused push" discipline §5
  already demands of every ring push);
- the daemon answers **every** `SetClip`/`ClearClip` with exactly one
  `EvClipAck{track, slot, ref=generation, flags, x=reason}`, accepted or
  refused. A silent refusal would wedge that cell for the rest of the session.

Cost: a back-to-back edit of the same cell within one 1 ms pump tick waits one
frame. Benefit: the daemon's displacement diff is exact by construction.

The client keeps two copies of the table in its own memory — `shadow_` (what
the engine is believed to hold) and `pending_` (written, not yet acknowledged) —
because the control region dies with the engine and the whole point of the
shadow is to outlive one. On a refused write the client unwinds: the blocks it
optimistically marked `Live` go back to `Quiescent`, nothing is displaced, and
the cell unblocks.

### 10.5 Validation: the one place a number becomes a pointer

`translateClip()` runs on 120 bytes another process last stored at a shared
address. Under a crashed or compromised GUI that is any 120 bytes at all, so the
rule is absolute: **no path may produce an `RtClip` whose `data` or `notes` is
anything but a pointer into the mapped pool, backed by a block that is
allocated, of the right kind, and at least as large as the clip says it will
read.** In order:

| check | catches |
|---|---|
| `ref != 0`, `ref % 64 == 0` | null and misaligned handles |
| `ref` inside `[arenaStart + 128, arenaEnd)` | anything outside the arena |
| `ref <= bump` | offsets past the allocator's high-water mark, where a stale magic could survive in a reused page |
| `magic == kPoolBlockMagic ^ ref` | wrong-offset, stale and invented handles |
| `bytes` positive, 64-aligned, inside the arena and inside `bump` | a wild size field turning a valid block into an arbitrary read |
| `state != Free` | a freed block being republished |
| `kind` matches | a notes offset arriving where sample data was expected |
| `needBytes <= bytes` | `frames * channels` reading past the block |
| clip scalars: `channels ∈ [1,2]`, `frames ≥ 0`, `0 ≤ loopStart ≤ loopEnd ≤ frames`, finite `f64`s, `warp`/`follow`/`quantumIdx` in range, `prob ∈ [0,1]` | the multipliers — a wild `loopEnd` walks `fetch()` off the end just as effectively as a wild offset |

Every failure is a refusal with a named reason, a rate-limited log line, an
`EvCommandRejected` and an `EvClipAck{refused}`; never a clamped pointer.
`daemon_test` §10 fires seven of them — including an offset one block past a
valid one, a misaligned offset, `~0ull`, and a *valid* block asked to yield a
megabyte — and then checks that a good clip still works, which is what proves
the refusals left nothing broken behind them.

### 10.6 Deliberate deviations from §3.5

1. **No `Cmd::PoolGrow`/`Ev::PoolGrown`, and no `PROT_NONE` reservation.** The
   pool is sized once. On tmpfs a large sparse file is free, so growth buys
   nothing and costs a handshake that has to be got right on the audio side.
2. **Descriptors are inline, not a `BlockDesc[]` table** (§10.2).
3. **No `memfd`, no `SCM_RIGHTS`, no `adopt(int fd)`.** They need the socket.
   `ShmRegion::create(..., seal)` asks for `F_SEAL_SHRINK` anyway so the
   upgrade is a one-line change when the fd arrives from `memfd_create`.
4. **No `Cmd::ReleaseSample`.** §3.5 step 2 has the GUI announce a release and
   the engine count references. With `src/audio` frozen the engine cannot count
   anything, so the daemon derives the same information from the clip table it
   already has to shadow: a block is retiring when no cell names it. Same rule,
   one fewer message, and the count lives on the side that can see all of it.
5. **The retirement echo is one event per block, not per cell.** `EvBlockRetired`
   carries the offset, the kind, and the cell it was displaced from.
6. **`ShmRegion::adoptOwnership()` is still not there.** `release()` (detach
   without unlinking) and `SamplePool::attach()` are the two halves it would be
   built from, and both ship here; the missing piece is the daemon confirming
   sole-client status, which is a §4.3 concern with no socket to have it over.

### 10.7 Test coverage

`tests/daemon_test.cpp`, 200 checks, all against a real spawned
`nxtaktd --driver null`:

| § | what |
|---|---|
| 1–5 | phase 1's: handshake, beat clock vs wall clock, metronome/master through the rendered audio, the refusal paths, a 3000-command burst |
| 6 | pool create, `/dev/shm` presence, daemon attach, a second handle onto the same region, layout-hash and version mismatches refused |
| 7 | a DC clip synthesised here → `poolWrite` → `SetClip` → `LaunchClip` → `slotState` Playing, `clipPhase` advancing, **and the track meter reading 0.5000 for a 0.5 DC clip** — a measurement, not a liveness check |
| 8 | `free()` refused on a live block; `ClearClip` → `EvBlockRetired` echoing the offset → `Quiescent` → `poolRelease` frees → bump retracted, free list empty, **reallocating the same size returns the same offset**; and a block shared by two cells surviving the loss of one |
| 9 | a MIDI clip through the notes pool: Playing, `clipPhase` advancing, then a notes repush retired through the *exact* `Ev::NotesRetired` path with `eventsDropped` still zero |
| 10 | seven bad offsets refused, daemon still alive, a good clip still works afterwards |
| 11 | `SIGKILL` with the pool attached: pool survives in `/dev/shm`, block still `Live`, samples intact, content key still matches; orphan control region reaped without touching the pool; respawn, automatic pool re-announce, `republishClips()`, relaunch, **same offset, same 0.5 at the meter** |
| 12 | `SIGTERM`: control region unlinked, **pool still there** (it is the session's), nothing else in `/dev/shm`; then `closePool()` unlinks it |
| 13 | `/dev/shm` clean |

Green over six consecutive runs, and clean under ASan+UBSan with both the daemon
and the test sanitised. `make test` is green end to end (252 + 54 + 200).

### 10.8 Still deferred

- **Publishing from the engine instead of the mirror thread** (§9's step 1). It
  edits `src/audio`; the mirror still costs one copy per 4 ms.
- **Mixer scalars in `SharedState`.** Still unpublished by `Engine::publish()`,
  so a reattaching GUI still cannot read back vol/pan/mute/solo/arm.
- **Chains, devices, the param table, recording** — phase 3, unchanged.
  **— chains, devices and the param table are done in §11; recording is not.**
- **The AF_UNIX socket**, and with it `memfd` + `SCM_RIGHTS` and the plugin
  catalog.
- **GUI-crash reattach** (§4.3). The region survives a GUI crash by
  construction and `SamplePool::attach()` can adopt it, but nothing negotiates
  the unlink obligation back.
- **`Engine` counting its own command drains**, which would replace the
  retirement deadline with a proof (§10.3). **— done in §11.5.**
- **The GUI itself.** `src/ui` still owns an in-process `Engine`;
  `EngineClient` still has no callers outside the test. That is the next step —
  §9's step 4, now unblocked.

---

## 11. Phase 3 shipped — the plugins move in

`nxtaktd` links `src/plugin` and owns every `PluginInstance` in the system. A
client names a plugin by URI; the daemon scans, instantiates, prepares, builds
an `RtChain` out of *its own* instances and hands it to its `Engine`. What comes
back is a device id and a table row. **No pointer crosses the boundary in either
direction, and none is left to remove:** `RtClip::data` went in §10 and
`RtChain::fx` goes here, which was the whole of §2.4 and §2.5.

Protocol version 3, pool version 2. `kShmVersion` stays at 2 — `ShmHeader`,
`ShmSpscRing` and `SharedStateT` did not change — but the control region grew
two sections and its layout hash moved, so a phase-2 binary and a phase-3 binary
refuse each other at `attach()` with a specific message.

`src/audio` and `src/ui` were not edited by this wave. `Engine::drains`,
`SetReturnChain`/`SetMasterChain` and the delay compensation this phase builds
on landed in parallel, against the frozen `engine.h`.

### 11.1 What exists now

- **`src/daemon/nxtaktd.cpp`** — the device layer.
  - `PluginRegistry`, scanned lazily on the first device command, on a thread of
    its own (§11.6). `PluginInstance` instantiation, preparation and destruction
    on the pump thread.
  - A device table (`Device[kMaxDevices]`), one flat chain array covering
    32 tracks + 4 returns + the master, and the queue that turns
    `AddDevice`/`RemoveDevice`/`MoveDevice` into `Cmd::SetChain` /
    `SetReturnChain` / `SetMasterChain`.
  - `pumpParams()`: the param table applied to `setParam` at 1 ms cadence.
  - `drainProof()` / `drainProven()`: exact retirement for both sample blocks
    and chains (§11.5).
- **`src/ipc/control.h`** — protocol v3. `CmdAddDevice`/`RemoveDevice`/
  `MoveDevice`/`SetBypass`/`ScanPlugins` in a daemon-command space clear of
  `lat::Cmd`; `EvDeviceAdded`/`Failed`/`Removed`/`Changed`/`EvScanComplete`;
  seven new reject reasons; `WireDeviceInfo[320]` and `WireDeviceParams[320]` as
  the seventh and eighth region sections; device, param, chain and drain
  counters in `ControlHeader`.
- **`src/ipc/pool.h`** — `PoolKindString` and `SamplePool::writeString()`, the
  string-crossing mechanism (§11.2). `PoolReader::block()` so the daemon can
  bound a read by the block's declared size.
- **`src/ipc/client.h`** — `addDevice`/`removeDevice`/`moveDevice`/`setBypass`/
  `scanPlugins`, `setDeviceParam`, `readDevice` into a `DeviceMirror`, and the
  slot-generation bookkeeping that guards a stale param write.
- **`Makefile`** — `build/nxtaktd` links `src/plugin/*.cpp` plus `lilv-0` and
  `-ldl`. Still no GUI, no window system, no sndfile.
- **`tests/daemon_test.cpp`** — 300 checks, fifteen sections.

### 11.2 Strings across the boundary: the pool, not a table

The rings carry 32 bytes and §3.2 is right that stretching them to carry a
plugin URI would be a mistake. §3.2's answer is the AF_UNIX socket, and there
still is not one. So a string travels in **the region that already exists for
variable-length payloads**: the client allocates a `PoolKindString` block,
writes the bytes, and `CmdAddDevice{ref}` carries the offset.

Three properties made this the choice over a fixed string table in the control
region:

1. **One validator, not two.** `poolValidate()` is already the single place in
   the tree where an untrusted `u64` becomes a pointer, and it is already
   reviewed as such. A string table would be a second such place, indexed by a
   number exactly as untrusted, with its own slot-reuse protocol to get wrong.
2. **No fixed budget.** A table needs a slot count and a maximum length chosen
   in advance; the pool needs neither.
3. **The lifecycle already exists.** A string blob is a pool block, so
   free-after-confirm applies unchanged — and it applies in its tightest
   possible form.

**Retirement of a string is instant, and that is not a shortcut.** The rule
`EvBlockRetired` encodes is "the engine can no longer reach this". A string is
copied into a stack buffer on the pump thread and is *never handed to the
engine*, so there is nothing to be quiescent of: the daemon echoes the offset
back in the same tick it reads it. Concretely, the client's half is

```
writeString  -> Quiescent, refs = 1
markLive     -> Live               (so a concurrent free cannot take it)
push AddDevice
markDisplaced -> Retiring
release       -> refs = 0, still Retiring, still not freed
EvBlockRetired -> Quiescent -> refs == 0 -> freed
```

which is the existing state machine with no new states and no new rules.

**Exactly one retirement echo per blob, accepted or refused.** The daemon copies
the string and retires it *before* it looks at whether the URI resolves, so an
`AddDevice` that fails does not leave the client holding a block it can never
free. That is the same discipline `EvClipAck` established for cells and for the
same reason: a silent refusal wedges the sender.

Bound: `kMaxPoolString` = 1024 bytes, and the terminator must be **inside the
block's own declared size**. A "string" whose NUL is past its allocation is not
a truncation, it is a read off the end of somebody else's data, and it is
refused with `RejectBadString`.

### 11.3 Device metadata: a table, written by the daemon

The brief for this phase said the daemon should write a metadata blob *into a
pool block*. It cannot, and the reason is a feature: **`PoolReader` maps the
pool `PROT_READ`** (§10.1), so "the engine only reads the pool" is a page
permission and not a comment. Making the pool writable by the daemon to ship
metadata would trade a hard guarantee for a soft convenience.

So metadata goes the other way round from everything else, in a table the daemon
owns:

| table | in | writer | reader |
|---|---|---|---|
| clip table | control region | client | daemon |
| param table | control region | client | daemon |
| **device table** | control region | **daemon** | **client** |
| sample pool | session region | client | daemon (`PROT_READ`) |

`WireDeviceInfo` is 4416 B: uri/name/vendor at fixed widths, format, kind,
channel counts, `latencyFrames()`, chain position, and `WireParamInfo params[64]`
— every `ParamInfo` field with the two `std::string`s truncated to 32 and 8
bytes. `EvDeviceAdded` carries the id; the client reads the row once and mirrors
it into its own `DeviceMirror`/`ParamMirror` (`src/ipc/client.h`), which are
`PluginDesc`/`ParamInfo` under different names because the client no longer
links `src/plugin` and that is the point.

`state` is stored **last, with release**, exactly as `PoolBlock::magic` is: a
reader that sees the slot live has seen every byte describing it.

Two limits, both visible rather than silent: 320 devices (every addressable
chain position is 296, so "full" means a leak) and 64 controls per device
(`truncatedParams` reports the overflow and the daemon logs it). §3.7 sketched
256 params, which would have made the table 5.5 MiB instead of 1.4 for no plugin
anyone has.

### 11.4 The param table, and why `setParam` on the pump thread is the contract

`WireDeviceParams` is §3.7 relocated and otherwise unchanged: the client stores
a plain float into `value[i]` and bumps `generation` with release; the daemon's
pump samples the generation every millisecond and calls
`PluginInstance::setParam` for whatever moved. No ring, therefore **no drops** —
a dropped param write leaves the knob and the plugin permanently disagreeing,
which is the worst bug class in this corner of the system.

**Why the pump may call `setParam`.** `src/plugin/host.h` documents it as "GUI
thread, concurrent with `process()`". That wording names a caller, but the
guarantee it describes has three parts and none of them is the thread's name:

1. **not the audio thread** — so it may not be inside `process()`, and it need
   not be realtime-safe itself;
2. **a plain aligned scalar store**, loaded by the plugin without
   synchronisation, so a torn read is unrepresentable on every target we build
   for and a stale read costs at most one block of latency;
3. **exactly one writer**, so two callers cannot interleave on one parameter.

The pump satisfies all three identically, and (3) more strongly than the
in-process build did. In-process, `setParam` and `instantiate` were both "the
GUI thread" by convention — two roles that happened to share a thread. In the
daemon the pump *is* the thread that instantiates the plugin, that owns it, and
that destroys it; there is no second candidate. Nothing about the contract was
relaxed. The name of the thread changed, and the property it was standing in for
became structural.

What genuinely is new is that the values arrive from another process, so two
things are checked before `setParam` is reached:

- a non-finite float is dropped;
- a write stamped with a **device generation** the slot no longer has is
  dropped. Device ids are reused; without this, a knob drag in flight when a
  device was removed would land on its replacement. The client stamps the
  generation *it* believes in (from its own `EvDeviceAdded`/`EvDeviceRemoved`
  bookkeeping), never the one currently in the table — stamping the table's
  value would make the guard tautological.

Cost when nothing moves: one acquire load per live device per millisecond.

**Deviation from §3.7:** the param table is in the *control* region, not the
session region. §3.7 puts it in the session region so it survives an engine
restart. It cannot usefully: the table is indexed by device ids the daemon
issues, and a respawned daemon re-instantiates from scratch and numbers from
zero, so a surviving table would be indexed by numbers that no longer name
anything. Device state is rebuilt by re-issuing `AddDevice`, which the client
can do because the URI is a string and strings ride the pool that *did* survive.
`daemon_test` §13 asserts exactly this round trip.

### 11.5 Exact retirement: `drains >= k + 2`

§10.3 shipped a retirement that waited `max(100 ms, 8 block periods)` plus four
rendered blocks and said plainly that this was the weak half — a wedged backend
does not drain, and the deadline fires anyway, announcing a block free while a
voice could still be reading it. `Engine::drains`, bumped at the end of every
`drainCommands()`, replaces it with a proof.

**The rule.** Read `k = drains` *immediately after* `Engine::pushCommand`
returned true. The command is provably consumed once `drains >= k + 2`.

**Why 2 and not 1**, because this is the whole of the correctness argument and
an off-by-one here would produce a retirement that is right almost always:

- Let `P` be the moment the push's release store lands, and `R > P` the moment
  we read `k`.
- By definition drain #`k` completed before `R`.
- Drain #`k+1` completes after `R` — but it may have **started** before `P`, in
  which case it never saw the command. So `drains >= k+1` proves nothing.
- Drain #`k+2` cannot start until #`k+1` finished, which is after `R` and
  therefore after `P`. It must observe the push.

Two is the smallest safe number. Reading `k` *before* the push does not help and
needs 3 in the worst case, which is why `considerRetire()` is called from
`commit()` and not from `translate()` — that ordering was already required for a
different reason in §10.3 and now carries this one too.

**What this buys.** At 256 frames / 48 kHz the wait is at most two block
periods, ~10.7 ms, against phase 2's 100 ms floor. `daemon_test` §12 measures it
end to end with no sleep in the loop: **8.2 ms, one rendered block, and the
counter observed to move by exactly the two the proof requires.** More
importantly it is a proof and not a guess — no clock is involved, and a wedged
backend simply never satisfies it, which is the correct answer to "has the audio
thread passed this point" when the audio thread has not moved.

**The same proof retires chains and instances.** `RemoveDevice` does not destroy
the `PluginInstance`; the instance and the displaced `RtChain` ride the proof
together, because until the engine has drained past the new chain the audio
thread may still be inside the old one, and the old one names that instance.
This is `App::retiring_`, moved into the daemon and made exact. Two proofs are
accepted and either suffices: `Ev::ChainRetired` naming the exact pointer (it is
pushed from *inside* `drainCommands()`, so its arrival **is** the drain, and it
beats the counter by up to one block), or the counter. The counter is what makes
the return and master chains safe, since those are newer than any assumption
about which events the engine emits.

**The deadline is still compiled in**, for exactly one case: an `Engine` built
without the counter, where `drains` stays 0 forever. The daemon latches
`drainsExact_` the first time it sees the counter move and publishes the answer
in `ControlHeader::drainsExact`, so which rule is in force is *observable from
outside* rather than inferred — a client, or a test, cannot see the daemon's
`Engine`, and a silent difference in retirement semantics would be worse than a
slow one. Against this tree the timer is never consulted.

### 11.6 The scan, and keeping the pump alive

`PluginRegistry::scan()` walks every LV2 bundle on the system: **4.3 s here, 410
plugins.** It is lazy — nothing is scanned until a device command asks for a
plugin — and it runs on a thread of its own.

The thread is not an optimisation. `pumpLoop()` runs on `main()`, and a pump
that blocked for four seconds would stop the heartbeat; a client watching
`SharedState::stale()` would conclude the engine had wedged and start thinking
about respawning it. Losing the audio because we went looking for plugins would
be a remarkable own goal. `daemon_test` §11 asserts the heartbeat advanced by
thousands of ticks across the scan and that `alive()` never went false.

The registry is still touched by **exactly one thread at a time**: the scanning
thread owns its own `PluginRegistry` until it sets a release flag, and the pump
joins before it reads. host.h's "GUI thread only" means "the one non-realtime
thread that owns this object", and here that is the pump, briefly delegated.

Device commands arriving mid-scan are **queued, in order**, in the daemon (bound
256; overflow is answered with `RejectScanBusy`, never dropped). Ordering
between device commands and scalar commands is not preserved across the scan
window — a `LaunchClip` can overtake a queued `AddDevice` — which is a few
milliseconds of dry audio and is documented rather than defended.

§3.6 asks for the scan in a short-lived `nxtakt-scan` **child process**, so a
plugin that segfaults during discovery takes down neither the GUI nor the
engine. That is strictly better and it is still deferred: it needs a way to ship
the catalog back, which is the socket.

### 11.7 Deliberate deviations from §3.6 / §3.7

1. **No chain table on the wire.** §3.6 sketches `Cmd::SetChain{track,
   generation}` plus an ordered id list in a control-region table. There is
   none, because there is nothing for it to carry: `AddDevice` carries a
   position, `MoveDevice` carries a position, and the daemon is the only thing
   that needs to know what a chain looks like. The client never names an
   ordering, so there is no ordering to synchronise.
2. **`RtChain::fx` did not become `u32 device[]`.** It did not need to. §3.6
   proposed ids in `RtChain` because it assumed the chain still crossed the
   boundary; it does not, so the engine keeps `PluginInstance*` and `src/audio`
   was not touched at all.
3. **Device metadata is a daemon-written table, not a pool blob** (§11.3), to
   keep the pool `PROT_READ`.
4. **The param table is in the control region, not the session region**
   (§11.4), because device ids do not survive an engine restart.
5. **`Ev::ChainRetired` did not disappear** — it disappeared *from the wire*.
   §3.6 predicted the event would go away entirely; in fact it is still exactly
   the right message from the audio thread to whoever owns the chain, and now
   both are in one process. The daemon consumes it as one of two accepted
   proofs.
6. **`Cmd::SetChain` and its two siblings are refused permanently**, not
   pending a phase. A client has no business naming an `RtChain` because it has
   no `RtChain`s. `commandCarriesPointer()` still returns true for them and the
   reject reason was reworded from "phase 3" to what it now means.
7. **No `Ev::DeviceDestroyed` distinct from the request.** §3.6 has
   `RemoveDevice` answered by `DeviceDestroyed` "once the engine has actually
   torn it down". `EvDeviceRemoved` is sent when the id is free and the chain is
   republished; the *destruction* happens later, on the proof, and no client has
   anything to do with that interval — the id is already unusable and the
   instance is already unreachable. Publishing an event for a moment nobody can
   act on would be noise.
8. **`Cmd::SendLevel` and `Cmd::ReturnVol` joined the scalars.** They are plain
   numbers into the engine's mixer and always were; they only look new because
   the engine grew return buses in the same wave.

### 11.8 Test coverage

`tests/daemon_test.cpp`, 300 checks, all against a real spawned
`nxtaktd --driver null`:

| § | what |
|---|---|
| 1–5 | phase 1's: handshake, beat clock, metronome/master through rendered audio, refusals, a 3000-command burst |
| 6–10 | phase 2's: the pool, a DC clip measured at the meter, retirement, MIDI clips, seven bad offsets |
| **11** | **devices.** Lazy scan (410 plugins, 4.3 s) with the heartbeat advancing throughout; `AddDevice "lattice:saturator"` on track 0 — deliberately the PRE-RENAME URI, so the permanent `lattice:` -> `nxtakt:` alias is exercised on every run, and the metadata row is asserted to come back carrying the canonical `nxtakt:saturator`; the metadata row parsed into the client mirror — 3 params, `Drive` dB [0..36] flagged logarithmic, `Output` [-24..24], `Mix` [0..1] def 1; the URI blob freed by its echo; a 0.2 DC clip measured dry at 0.2000, driven to 0.4621 by a **param-table write**, back to 0.2000 under `SetBypass`, up again when it is cleared, and back to 0.2000 after `RemoveDevice` — with the displaced chain and its instance freed only after the proof; insert-at-position, `MoveDevice`, and the positional shifts the table reports; a garbage URI answered with `EvDeviceFailed{RejectUnknownUri}` and its blob retired anyway; `RemoveDevice` on an empty slot answered with `RejectBadDevice`; a saturator on **return 0** and on the **master**, both retiring through the same proof; `eventsDropped == 0`, because `Ev::ChainRetired` is consumed and not dropped |
| **12** | **exact retirement.** `drainsExact == 1`; a block displaced and confirmed quiescent in **8.2 ms with no sleep in the loop**, within **one** rendered block, with the drain counter observed to advance by ≥ 2 — the `k+2` proof, measured |
| **13** | `SIGKILL` **with a device loaded**: the pool survives, the block is still `Live`, samples intact, key still matches; the orphan control region reaped; respawn, pool re-announced, `republishClips()`, same offset, same 0.5 at the meter; **devices did not survive and the daemon says so** (`devicesLive == 0`, the table row free, the client's generation record dropped); re-`AddDevice` on the fresh engine works, metadata comes back, and a param write shapes 0.5 DC to 0.4621 |
| 14 | `SIGTERM`: control region unlinked, pool still there, nothing else in `/dev/shm`; `closePool()` unlinks it |
| 15 | `/dev/shm` clean |

Green over five consecutive runs (14.8–23.3 s each, dominated by the scan) and
twice under ASan + UBSan with both the daemon and the test sanitised — no leak,
no runtime error. `/dev/shm` clean after every run.

### 11.9 Still deferred

- **Plugin editor windows** (§3.6). NxTakt draws its own generic knob UI from
  `ParamInfo`, which keeps working unchanged through the param table, so this is
  phase 5 and blocks nothing.
- **Out-of-process scanning** (§11.6): a crash in a third-party bundle during
  discovery still takes the daemon with it.
- **Engine → GUI param changes.** `WireDeviceParams::engineGeneration` exists
  and is bumped when the daemon initialises a row, but nothing continuously
  mirrors `getParam()` back: no in-tree plugin moves its own controls, and a
  mirror that fought the client's writes every millisecond would be worse than
  none. The field and the protocol are in place for the day a native UI needs
  it.
- **Presets.** `Cmd::LoadPreset{deviceId, ref}` (§3.7) is not implemented; the
  string mechanism it would use is (§11.2).
- **Recording.** `RecordSlot`/`RecordMidiSlot` still carry client-owned buffers
  and are still refused.
- **The AF_UNIX socket**, and with it `memfd` + `SCM_RIGHTS` and shipping the
  plugin catalog to the client. The catalog is scanned in the daemon and is
  currently only reachable one URI at a time — a GUI browser needs the socket
  or a catalog table.
- **Publishing from the engine instead of the mirror thread**, and mixer scalars
  in `SharedState`. Both edit `src/audio`.
- **GUI-crash reattach** (§4.3).
- **The GUI itself.** `src/ui` still owns an in-process `Engine`;
  `EngineClient` still has no callers outside the test. That remains the next
  step, and it is now the only thing between this and "a GUI crash no longer
  stops the audio".
