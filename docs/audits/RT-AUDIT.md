# Lattice — adversarial realtime-safety & performance audit

**Tree:** `/run/media/nerdrx/Lex/claude/lattice`
**Audited at:** `HEAD = 6db2564d7e6a45710c21c1ba2b279d6d2a602c42` ("NxTakt: brand identity and repo front page")
**Method:** read-only. All files snapshotted via `git show HEAD:<path>` into scratchpad so a parallel rename agent could not shift line numbers under me. **All line numbers below are HEAD line numbers.** No file in the repo was written or modified.

Scope: `src/audio/engine.{cpp,h}`, `src/audio/backend.cpp`, `src/plugin/{lv2_host,clap_host,internal_devices,host}.cpp`, `src/daemon/latticed.cpp`, `src/ipc/{shm,pool,control}.h`, plus `src/core/{common.cpp,ring.h}`, `src/audio/midi_in.cpp`, `src/main.cpp`, `src/ui/app.cpp` (call sites only) as they turned out to be load-bearing.

Confidence labels: **CONFIRMED** = I traced the code path end to end and the failure follows from what is written. **PLAUSIBLE** = the hazard is real but needs an input or a peer behaviour I could not prove reachable from this tree alone.

---

## Executive summary

The engine's *documented* contract ("the audio thread never allocates, locks, or touches std::string" — `engine.h:8`) is largely honoured **inside `Engine` itself**. The violations are almost all at the seams:

* **Through the plugin backends.** The LV2 `log:log` and `urid:map` features are handed to plugins as-is and are callable from `run()`; both do syscalls / locks / heap work on the audio thread.
* **Through `Engine::prepare()` being reachable from JACK's sample-rate callback** while `process()` is running.
* **Through the ring buffers' single-producer/single-consumer assumption being violated** on the MIDI ring in the in-process app.
* **Through silently-ignored `push()` failures**, which turn backpressure into lost takes and leaked chains.

Separately, the newly-added PDC path is correct but expensive, and the `Track` layout makes every per-block scan a page-stride walk.

The **k+2 drain proof** (`latticed.cpp:455-477`) is, as far as I can tell, **sound** — see §2.1 for the argument I checked it against and the one caveat.

---

## 1. Realtime violations

### 1.1 CONFIRMED — HIGH · `Engine::prepare()` runs from JACK's sample-rate callback, concurrently with `process()`

`src/audio/backend.cpp:92-97`
```cpp
static int srCb(jack_nframes_t n, void* arg) {
    auto* self = (JackBackend*)arg;
    self->sr_ = (f64)n;
    self->engine_->prepare(self->sr_, self->bs_);
    return 0;
}
```

`Engine::prepare()` (`src/audio/engine.cpp:573-608`) is not a light function. It:

* assigns `tracks_[t] = Track{}` for 32 tracks — **2.06 MB** of stores (measured: `sizeof(Track) == 67360`), including the 64 KB `fxL`/`fxR` scratch the audio thread is writing *right now*;
* assigns `clips_[t][s] = RtClip{}` for all 1024 cells — the very cells live `Voice::clip` pointers reference (`engine.h:204`, `v.clip = &c`);
* drops `t.chain`, `returns_[r].chain` and `masterChain_` to null;
* calls `pdcAcquire()` (`engine.cpp:481-507`), which on a first-use path does `new` + `std::calloc(19 MB)`;
* calls `LOGI`/`LOGW` → `logImpl()` → `fprintf(stderr, ...)` (`src/core/common.cpp:7-14`).

Failure scenarios, in order of severity:

1. **Segfault.** `RtClip` is ~120 bytes and is assigned non-atomically. A voice inside `fetch()` (`engine.cpp:311-337`) can observe a torn clip — `c.frames` still the old non-zero value, `c.data` already null. The guard at `engine.cpp:319` (`pos >= (f64)c.frames`) then passes, and line 329 computes `c.data + (size_t)i0 * c.channels` = `nullptr + offset`. Read fault on the audio thread.
2. **Every plugin goes permanently silent.** The chain pointers are nulled with **no `Ev::ChainRetired`** pushed. The comment at `engine.cpp:588-591` argues this is fine because "there is no audio thread running at this point" — that premise is false for `srCb`. Downstream, `latticed.cpp` still holds every `Chain::published` `unique_ptr` and every `Device::inst`, so nothing is freed (no crash) but nothing is re-published either. The mixer loses all devices for the rest of the session.
3. **PDC state race.** `p->reset()` (`engine.cpp:427-438`) writes `active`, `filled`, `wpos`, `dirty` as plain members while `process()` reads `pdc->active` (`engine.cpp:1584`) and `pdc->wpos` (1743).

Aggravating: JACK does **not** guarantee the sample-rate callback is serialised against the process callback the way it does for the buffer-size callback. Even if it did, the `calloc`/`fprintf` would then be running *inside* a suspended audio cycle.

**Fix sketch.** `srCb` must not call `prepare()`. Set a `std::atomic<f64> pendingSr_`; have `process()` observe it and either (a) refuse to run and output silence until a non-RT thread re-prepares, or (b) have the daemon's pump thread notice, stop the backend, re-prepare, re-instantiate every plugin at the new rate, re-publish every chain, and restart. (b) is the only correct answer — plugin instances are prepared at a fixed rate too (`latticed.cpp:1026`). A no-op `srCb` that logs and sets `SharedState::engineState = StateStopping` is a defensible interim.

---

### 1.2 CONFIRMED — HIGH · Two concurrent producers on `Engine`'s SPSC MIDI ring (in-process app)

`src/core/ring.h:10-38` is a textbook **single**-producer / **single**-consumer ring. `Engine::pushMidi` (`engine.h:175`) is its only producer entry point.

It has two live producers in the GUI build:

| producer | thread | site |
|---|---|---|
| ALSA sequencer reader | `MidiInput::run()`, its own `std::thread` | `src/audio/midi_in.cpp:142` |
| GUI | `App::update()` and piano-roll preview | `src/ui/app.cpp:222, 2114, 2147, 2182, 2183, 2190, 2193, 2211, 2220` |

`Ring::push` (`ring.h:15-22`) does `w_.load(relaxed)` → `buf_[w] = v` → `w_.store(next, release)`. Two threads interleaving there both read the same `w`, both write `buf_[w]`, and the second `w_.store` wins. Result: **one message is silently overwritten**, and the write index advances by one for two pushes.

Concrete failure: hold a key on the computer keyboard (GUI thread emits note-on) while a hardware controller sends a note-off for the same pitch (reader thread). The note-off is the message that gets dropped → **stuck note on the instrument**, which is precisely the failure the whole `flushOffs` / `PendingOff` apparatus in `engine.cpp:139-143` exists to prevent. It is also a formal data race (UB), not merely a lost message.

`src/ipc/client.h:16-17` gets this right for the cross-process path ("pushMidi may be a *different* single thread from pushCommand" — singular). The in-process app does not honour it. `src/ui/app.h:655` even documents the two sources sharing one ring, apparently without noticing the ring's contract.

**Fix sketch.** Give the GUI its own `Ring<MidiMsg, N>` and have `process()` drain both (the engine already merges into one fixed `midi[kMidiPerBlock]` array at `engine.cpp:1371-1373`, so a second drain loop is ~3 lines). Or funnel GUI-originated notes through `MidiInput`'s thread. A mutex around `pushMidi` is *not* acceptable — the GUI would then be able to block the reader thread, and vice versa.

---

### 1.3 CONFIRMED — HIGH · LV2 `log:log` is `fprintf(stderr)` and is reachable from `run()`

`src/plugin/lv2_host.cpp:84-99`
```cpp
int logVprintfFn(LV2_Log_Handle, LV2_URID, const char* fmt, va_list ap) {
    char msg[1024];
    const int n = vsnprintf(msg, sizeof msg, fmt, ap);
    ...
    if (len) LOGI("lv2: %s", msg);
    return n;
}
```

`LOGI` → `lat::logImpl` (`src/core/common.cpp:7-14`) → `std::fprintf(stderr, ...)`. `stderr` is unbuffered by C99 requirement, so that is a `flockfile()` plus a `write(2)` **syscall** per call.

This function is installed as the `log:log` feature (`lv2_host.cpp:303, 307, 310`) and handed to `lilv_plugin_instantiate`. LV2's log extension is explicitly usable from `run()` — `lv2:log` has no thread restriction, and the common real-world callers (parameter clamping warnings, denormal/NaN complaints, buffer-size gripes) fire from exactly there. `Lv2Instance::process()` calls `lilv_instance_run()` at `lv2_host.cpp:356`, on the audio thread.

Same shape in CLAP: `clap_host.cpp:77-88` defines `hostLogFn` → `LOGE/LOGW/LOGI`, and `hostGetExtension` (`clap_host.cpp:110-116`) hands `clap.log` out **unconditionally**, with no gate on `tlsInProcess`. The comment at `clap_host.cpp:76` ("the spec forbids logging from the audio thread, so this is main-thread only in practice") is an assumption about third-party code, and `CLAP_LOG_PLUGIN_MISBEHAVING` / `CLAP_LOG_HOST_MISBEHAVING` are precisely the severities a plugin emits when something goes wrong *in* `process()`.

**Fix sketch.** Both log callbacks should test "am I on the audio thread?" and, if so, write into a fixed-size lock-free SPSC ring of `char[256]` that a non-RT thread drains. CLAP already has the thread flag (`tlsInProcess`, `clap_host.cpp:57`); LV2 needs an equivalent `thread_local` set around `lilv_instance_run`. Dropping on ring-full is the correct behaviour.

---

### 1.4 CONFIRMED — HIGH · LV2 `urid:map` takes a `std::mutex` and allocates, from `run()`

`src/plugin/lv2_host.cpp:58-82`
```cpp
struct UridStore {
    std::mutex mtx;
    std::deque<std::string> uris;
    std::unordered_map<std::string, LV2_URID> index;

    LV2_URID map(const char* uri) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = index.find(uri);
        if (it != index.end()) return it->second;
        uris.emplace_back(uri);          // heap
        ...
        index.emplace(uris.back(), id);  // heap
```

`uridMapFn` is the `urid:map` feature handed to every plugin (`lv2_host.cpp:301, 305, 310`). The header comment at `lv2_host.cpp:55-57` acknowledges it: *"plugins that map during run() are rare and would take the mutex"*. Rare is not never — plugins that lazily map atom types on first use of a patch message do exactly this.

Three separate violations in one call: `std::mutex` lock, `std::string` construction, and `unordered_map`/`deque` node allocation. Worse, the lock is shared with the **scanner** (`scanLV2` → `buildPorts` → `urids().map`, `lv2_host.cpp:526-528`), which in the daemon runs on `scanThread_` (`latticed.cpp:859-862`). An audio thread blocking on a mutex held by a scan thread walking every bundle on the system is unbounded priority inversion.

Note the `unmap` path (`lv2_host.cpp:72-76`) takes the same lock and is also feature-exposed (`fUnmap_`, line 306).

**Fix sketch.** Make the map lock-free for the read path: an open-addressed table sized at startup, with insertion only permitted off the audio thread. Return `0` ("no URID") for an unmapped URI when `tlsInProcess`. That is a legal answer and strictly better than blocking.

---

### 1.5 CONFIRMED — MEDIUM · `insertNote()` is an O(N) insertion sort on the audio thread, and its stated precondition is false for overdub

`src/audio/engine.cpp:233-240`
```cpp
static bool insertNote(RtNote* buf, i64& len, i64 cap, const RtNote& n) {
    if (!buf || len >= cap) return false;
    i64 i = len;
    while (i > 0 && buf[i - 1].beat > n.beat) { buf[i] = buf[i - 1]; --i; }
    buf[i] = n;
    ++len;
    return true;
}
```

The comment at `engine.cpp:227-232` justifies this: *"notes close in note-off order, which for anything a human plays is almost start order, so the backwards scan below normally stops on its first compare."*

That holds for a plain take. It is **false for an overdub pass**, and overdub is the case the surrounding code was written for. In overdub, the stamp is `wrapBeat(...)` (`engine.cpp:1478`) — the beat is reduced modulo the clip loop. So a player looping over a 4-beat clip produces beats in essentially arbitrary order relative to arrival order: pass 3 might land a note at beat 0.5 after pass 2 landed one at beat 3.9.

Call sites: `engine.cpp:1502` and `1526` (per incoming note event, inside the per-sub-block `captureMidiRange`), and `engine.cpp:288` (per open note at the stop boundary).

Failure: a long overdub session accumulates `recLen` into the thousands. `RtNote` is 24 bytes. One note-off arriving with a small wrapped beat memmoves `recLen * 24` bytes inside `process()` — ~96 KB at 4000 notes. That is a multi-microsecond stall on a path budgeted at ~5 ms total, and it scales with take length.

**Fix sketch.** Append unsorted on the audio thread (O(1)) and sort in the GUI/pump after `Ev::MidiRecordFinished`. The engine has no reason to care about note order; only the consumer does. If sortedness must be preserved for the in-flight case, bound the backward scan (e.g. 32 compares) and mark the buffer "needs sort" in the finish event.

---

### 1.6 CONFIRMED — MEDIUM · Every `evts_.push()` return value is ignored

`Engine::process`/`drainCommands`/`fireDue`/`renderRange` push events at `engine.cpp:291, 293, 668, 703, 779, 796, 811, 818, 846, 992, 1012, 1032, 1083, 1104, 1146, 1244, 1320`. `Ring::push` returns `bool` (`ring.h:15`). **Not one call site checks it.**

The ring is 1024 deep (`engine.h:310`). The daemon drains it once per 1 ms pump tick (`latticed.cpp:1596-1625`). Overflow is reachable whenever the pump stalls — and §3.3 below shows it stalls for *seconds* during device loading.

What gets silently lost, in ascending order of how much it hurts:

* `Ev::ClipStarted` / `ClipStopped` / `TrackStopped` — GUI clip state desyncs, but the mirrored `slotState[]`/`activeSlot[]` atomics re-converge. Cosmetic.
* `Ev::ChainRetired` (`engine.cpp:796, 811, 818`) — `Daemon::confirmChainRetire` never fires. Recovered by the drain proof (`latticed.cpp:1256-1257`), so this degrades rather than breaks.
* `Ev::NotesRetired` (`engine.cpp:779, 846`) — same, recovered by the drain proof.
* **`Ev::RecordFinished` / `Ev::MidiRecordFinished`** (`engine.cpp:291, 293`) — **unrecoverable**. These are the *only* channel by which a finished take's buffer travels back. There is no timeout, no retry, no second announcement. The take is gone and the GUI-owned buffer is leaked (the engine has already nulled `t.recBuf` at `engine.cpp:295`). A user loses a recording, silently.

**Fix sketch.** Two changes. (a) Reserve headroom: refuse to start a take unless the event ring has ≥ N free slots. (b) Give the retirement/record events a sticky fallback — a small fixed array of "undelivered critical events" that `drainCommands()` retries at the top of every block. Also add a `std::atomic<u64> evtOverflow` the GUI can surface, so a drop is at least visible.

---

### 1.7 CONFIRMED — MEDIUM · The daemon never enables FTZ/DAZ; the app sets it once, on the wrong thread, for x86 only

`src/main.cpp:74-79`
```cpp
#if defined(__x86_64__) || defined(__i386__)
    // Denormals in feedback tails cost orders of magnitude on the audio thread.
    // These are per-thread, but the audio thread is created after this point.
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
```

Three problems, one of which is unambiguous:

1. **`latticed` never does this at all.** `grep` over `src/daemon/latticed.cpp` for `_MM_SET`/`FLUSH_ZERO`/`xmmintrin` returns **0 hits**, and the Makefile's `DAEMON_SRC` (`Makefile:209`) does **not** include `src/main.cpp` — the daemon has its own `main()` at `latticed.cpp:1822`. Since the "Split phase 3: plugins live in the daemon" commit, `latticed` is where *all* plugin DSP runs. **The daemon's audio thread runs with denormals fully enabled.** CONFIRMED, no caveats.
2. MXCSR is per-thread state. The comment's reasoning ("the audio thread is created after this point") relies on the new thread inheriting the creator's MXCSR. That happens to hold on Linux/x86-64 today, but it is an implementation detail, not a guarantee, and it silently does not apply to the JACK client thread if libjack ever spawns it differently.
3. Plugins are free to modify MXCSR and not restore it — many older ones do. Nothing re-arms it.
4. The guard is `__x86_64__ || __i386__` only. There is a `Makefile.mingw` and a `.github/workflows/windows.yml`, and no AArch64 `FPCR` equivalent.

The engine's own code makes this matter even with no plugins loaded: the meter decay at `engine.cpp:1797` (`t.mL *= kDecay` with `kDecay = 0.72f`), `1817`, and `1821` is a pure multiplicative decay with no flush. After a few seconds of silence all ~74 of those (32 tracks × 2 + 4 returns × 2 + 2 master) sit permanently in the denormal range. `internal_devices.cpp` does the right thing with an explicit `flushDenormal()` (`internal_devices.cpp:33-35, 177, 311`) — but that only covers Lattice's own two devices.

**Fix sketch.** Set FTZ/DAZ at the top of every audio callback, not once at startup: a tiny RAII guard in `JackBackend::processCb`, `AlsaBackend::loop`, and `NullDriver::loop`. That also re-arms after a plugin clobbers it. Add the AArch64 `FPCR` FZ bit. Separately, apply `flushDenormal` to the meter decays.

---

### 1.8 CONFIRMED — LOW · `std::tanh` per sample in the Saturator

`src/plugin/internal_devices.cpp:174` calls `std::tanh(g * x)` once per sample per channel inside the block loop. Not an RT violation (no allocation, no lock, bounded), but at ~40–80 cycles a call it dominates the device's cost by an order of magnitude. A degree-5 rational approximation is accurate to well under the noise floor here and is the standard fix. `std::exp2` at line 309 in Pulse's per-sample filter loop is the same story.

### 1.9 PLAUSIBLE — LOW · `t.recBuf` reinterpreted as `RtNote*`

`engine.cpp:278` (`RtNote* notes = (RtNote*)t.recBuf;`) and `engine.cpp:1449`. `recBuf` is declared `f32*` (`engine.h:250`) but for a MIDI take the GUI allocated an `RtNote[]` (per the `Cmd::RecordMidiSlot` contract, `engine.h:126-139`). Alignment is fine in practice; strict aliasing is not. Built with `-O2` (`Makefile:213`), which implies `-fstrict-aliasing`. Low probability of miscompilation today, but it is UB and it is the kind of thing that breaks on a compiler upgrade. A `union` or a `char*`-typed member with explicit `memcpy` would cost nothing.

---

## 2. Races, memory ordering, and the drain proof

### 2.1 The k+2 proof — CHECKED, sound, with one caveat

`src/daemon/latticed.cpp:455-477`
```cpp
u64 drainProof() const {
    return engine_->drains.load(std::memory_order_acquire) + 2;
}
bool drainProven(u64 due) const {
    return engine_->drains.load(std::memory_order_acquire) >= due;
}
```
paired with `engine.cpp:957`: `drains.fetch_add(1, std::memory_order_release);` as the last statement of `drainCommands()`.

I checked the argument as stated at `latticed.cpp:460-468` and it holds:

* `Ring::push` publishes with `release` on `w_` (`ring.h:20`); `Ring::pop` reads `w_` with `acquire` (`ring.h:26`). The command is visible to any drain that loads `w_` after P.
* `drains.fetch_add(release)` happens after every state write in `drainCommands()`; `drains.load(acquire)` on the pump side therefore sees those writes. Release/acquire correctly paired. ✔
* The +2 (rather than +1) reasoning is right: drain #(d0+1) may have loaded `w_` before P, so only #(d0+2) is guaranteed to observe the push. ✔

The ordering discipline at the call sites is also correct — `drainProof()` is read strictly *after* a successful `pushCommand`, at `latticed.cpp:1232` (chains) and `latticed.cpp:1531` via `commit()` → `installClip()` → `considerRetire()` (clips). The comment at `latticed.cpp:1526-1530` explicitly calls out why the retirement is queued from `commit()` and not `translate()`. ✔

For **chains** specifically, "the command was consumed" is also *sufficient* for "the old chain is unreachable", because `drainCommands()` runs at the very top of `process()` (`engine.cpp:1358`) — strictly before any `fx->process()` call in that same block (`engine.cpp:1657, 1718, 1760`). So the drain that consumes `Cmd::SetChain` guarantees the remainder of that block already uses the new pointer, and every earlier `process()` call has returned. ✔

**Caveat (PLAUSIBLE — MEDIUM).** `drainsExact_` is latched `true` on the first observation of `drains > 0` (`latticed.cpp:447-452`) and is **never un-latched**. If the audio thread subsequently stops — JACK server exits, the client is zombified, the ALSA loop breaks out at `backend.cpp:243` — `drains` freezes. From `latticed.cpp:1257` and `1560-1562`, once `drainsExact_` is set the legacy deadline is *never consulted*. So every pending retirement hangs forever: `retiring_` and `chainRetire_` (both `std::vector`, `latticed.cpp:1793, 1809`) grow without bound, the client is never told it may free its pool blocks, and the pool leaks for the life of the session. The comment at `latticed.cpp:1472-1476` argues this is *correct* ("a wedged backend simply never satisfies the proof"), and for safety it is — but the unbounded growth and the absent diagnostic are not. **Fix sketch:** detect a stalled `drains` (no movement across N pump ticks with a backend that claims to be running), publish `SharedState::engineState = StateStopping`, and stop accepting new work rather than queueing it forever.

### 2.2 CONFIRMED — MEDIUM · `pumpEvents()` drops an engine event under client backpressure

`src/daemon/latticed.cpp:1596-1625`
```cpp
while (engine_->popEvent(ev)) {
    ...
    if (!map_.evts->push(e)) return;     // client asleep; retry next tick
```

`ev` has **already been consumed** from the engine's ring by `popEvent`. The comment says "retry next tick", but nothing stages it — the event is gone. Contrast `pumpCommands` (`latticed.cpp:544-575`), which correctly parks the un-pushed item in `pending_`/`havePending_`.

The same bug in `publishRetired`'s caller is handled *correctly* (`latticed.cpp:1563-1564` keeps the entry), which shows the author knew the pattern. `pumpEvents` just missed it.

Failure: a GUI that stops draining for a few hundred ms (a long repaint, a modal file dialog) fills the 4096-slot event ring; the daemon then silently discards engine events one per tick. `Ev::RecordFinished` is filtered out earlier as non-scalar (`latticed.cpp:1607`), so what is lost here is transport/clip state — recoverable via the mirror, but the counters lie (`eventsForwarded` never increments for the dropped one, and `eventsDropped` is not incremented either).

**Fix sketch.** Add `Event pendingEv_; bool havePendingEv_;` mirroring the command path, and drain it at the top of `pumpEvents()`.

### 2.3 CONFIRMED — HIGH · CLAP `clap.thread-check` reports the wrong main thread in the daemon

`src/plugin/clap_host.cpp:59-73`
```cpp
std::thread::id& mainThreadId() { static std::thread::id id; return id; }
void rememberMainThread() {
    if (mainThreadId() == std::thread::id{}) mainThreadId() = std::this_thread::get_id();
}
bool CLAP_ABI hostIsMainThread(const clap_host_t*) {
    return !tlsInProcess && std::this_thread::get_id() == mainThreadId();
}
```

`rememberMainThread()` is called from `scanCLAP()` (`clap_host.cpp:923`) and `instantiateCLAP()` (`clap_host.cpp:938`). In the daemon, whichever runs **first** is `scanCLAP` — and it runs on `scanThread_`:

`src/daemon/latticed.cpp:859-862`
```cpp
scanThread_ = std::thread([this] {
    scanned_->scan();                     // -> PluginRegistry::scan() -> scanCLAP()
    scanDone_.store(true, std::memory_order_release);
});
```
(`PluginRegistry::scan()` calls `detail::scanCLAP` at `src/plugin/host.cpp:37`.)

So `mainThreadId()` latches the **scan thread's** id. That thread is then joined at `latticed.cpp:870` and destroyed. Every subsequent `create_plugin`, `init`, `activate`, `params->get_info`, `get_extension` and `setParam` happens on the **pump** thread (`doAddDevice`, `latticed.cpp:1026-1046`; `pumpParams`, `latticed.cpp:1313-1333`). For all of those, `hostIsMainThread()` returns **false**.

Failure: CLAP plugins routinely assert `host_thread_check->is_main_thread(host)` inside `init()`/`activate()`/`get_info()`. Many do so with `assert()`; some abort unconditionally. **The daemon dies while loading a plugin**, and the failure looks like the plugin's fault. A subtler variant: `std::thread::id` values are permitted to be reused after a thread terminates, so a later worker thread could coincidentally compare *equal* and be told it is the main thread.

**Fix sketch.** Call `rememberMainThread()` explicitly from the pump thread at daemon startup — before any scan is possible — rather than relying on first-touch. `Daemon::run()` (`latticed.cpp:267`) is the natural place. Better still, replace the latch with an explicitly-set `setMainThread(std::this_thread::get_id())` so the ownership decision is visible rather than incidental.

### 2.4 PLAUSIBLE — LOW · `pumpParams` advances `seenParamGen` before validating the device generation

`src/daemon/latticed.cpp:1320-1323`
```cpp
const u32 g = p->generation.load(std::memory_order_acquire);
if (g == d->seenParamGen) continue;
d->seenParamGen = g;                                                   // <-- advanced
if (p->deviceGeneration.load(std::memory_order_relaxed) != d->generation) continue;  // <-- then rejected
```

If the `deviceGeneration` check fails, the write is (correctly) discarded — but `seenParamGen` has already moved. If the client subsequently fixes `deviceGeneration` without bumping `generation`, those parameter values are never applied and the knob stays stale until the next move. Narrow, and self-correcting on the next gesture. Move the `seenParamGen` assignment below the check.

The acquire/relaxed pairing itself is fine: `generation` is loaded `acquire` and the `value[]` entries `relaxed` (line 1325), which is correct provided the client stores values before releasing `generation`.

### 2.5 PLAUSIBLE — LOW · Unchecked `chainIndex()` result

`latticed.cpp:1096-1097` (`doRemoveDevice`) and `latticed.cpp:1135` (`doMoveDevice`) both do `chains_[chainIndex(d->target, d->targetIdx)]` with no `< 0` check, whereas `doAddDevice` checks (`latticed.cpp:1008-1009`). Unreachable today because `target`/`targetIdx` were validated at add time and are never mutated. One `Device` corruption away from an out-of-bounds `Chain&`. Cheap to harden.

### 2.6 Noted — the pool TOCTOU is a contract, not an enforcement

`poolValidate` (`src/ipc/pool.h:308-350`) is genuinely thorough — bounds, 64-byte alignment, self-mixed block magic (`kPoolBlockMagic ^ ref`, so a block header cannot be relocated), high-water mark, `bytes` sanity, block state, kind, and declared size. It proves the block good **at translate time** (`latticed.cpp:743, 753`).

But the resulting raw pointer then lives inside an `RtClip` in the engine for the clip's lifetime, while the **client** owns and mutates the arena. The only thing preventing a use-after-free on the audio thread is the free-after-confirm rule (`pool.h:387`, `latticed.cpp:1431-1484`), enforced entirely in client code. A buggy or crashed GUI that frees early hands the RT thread recycled memory. This is a documented, deliberate design point rather than an oversight — I list it because §2.1's caveat makes it worse: when `drains` stalls, the client never gets its `EvBlockRetired` echoes and may be tempted to reclaim on a timeout.

---

## 3. Performance

### 3.1 CONFIRMED · `Track` layout puts the recording state ~65 KB past the header

Measured from a faithful reconstruction of the layout in `engine.h:220-269`:

```
sizeof(Track)     = 67360 bytes
offsetof(fxL)     =  1232 bytes   <- hot scalar header ends here
bytes after fxL   = 66128 bytes   <- rec*/pend* fields live out here
tracks_ total     = 2155520 bytes (2.06 MB)
```

`fxL[8192]` and `fxR[8192]` (64 KB of scratch, sized for `kMaxBlock`) sit **between** the hot scheduling scalars and the recording scalars. So any loop that reads both touches two cache lines a page-and-a-half apart, per track.

Loops that do exactly that, per block:

* the `live[]` scan, `engine.cpp:1386-1406` — reads `t.voice/prev/queued/arm/playing/fireBeat/chain` (header) **and** `t.recPhase/recSlot/recMidi/pendBuf/pendSlot/pendMidi` (far region), for all 32 tracks. 64 cache lines across ~64 pages.
* `publish()`, `engine.cpp:1793-1812` — meters and slot state (header) plus `recPhase`/`recSlot` (far region), 32 tracks.
* the `anySolo` scan, `engine.cpp:1578`.

And **per sub-block** (so potentially many times per block):

* `captureRange`, `engine.cpp:1414-1433` — 32-track scan reading only far-region fields.
* `captureMidiRange`, `engine.cpp:1440-1538` — same.
* the `consider` scan, `engine.cpp:1552-1556` — reads `t.queued/playing/fireBeat` **and** `t.recPhase/recFireBeat`.

**Fix sketch.** Move the scratch out of `Track` into a separate `f32 scratch_[kMaxTracks][2][kMaxBlock]` (or, better, a `prepare()`-sized allocation — `kMaxBlock = 8192` is 32× larger than a typical 256-frame block, so 2 MB is being reserved to serve 64 KB of live data). `Track` then drops to ~1.3 KB and 32 of them fit in 40 KB — comfortably L2-resident, and the header/rec split disappears. This is the single highest-leverage perf change in the audit and it is mechanical.

### 3.2 CONFIRMED · PDC: 19 MB of delay lines, page-strided, written every block for every idle path

`engine.cpp:396-403` gives `kPdcLines = kMaxTracks + kMaxReturns + 2 = 38` lines × 2 channels × `kPdcCap = 65536` frames × 4 bytes = **19.00 MB** (measured), `calloc`'d once at `engine.cpp:500`.

When `comp` is true (`engine.cpp:1584`, i.e. *any* device anywhere reports non-zero latency), every block:

* `pdcFlush` (`engine.cpp:554-562`) memsets `n` frames into **both channels of every non-live track** (`engine.cpp:1615`) and every non-live return (`1709`). With 30 idle tracks at n=256, that is 60 memsets of 1 KB each, scattered at 256 KB stride — 60 dTLB entries touched to write silence nobody will read.
* `pdcDelayChan` (`engine.cpp:536-542`) runs a **scalar per-sample loop** with a masked back-reference for every live track, every return, the dry bus and the click:
  ```cpp
  for (int i = 0; i < n; ++i) {
      const int w = (wpos + i) & kPdcMask;
      ring[w] = buf[i];
      buf[i]  = (filled + i >= d) ? ring[(w - d) & kPdcMask] : 0.f;
  }
  ```
  Two dependent, non-contiguous accesses per sample, plus a per-sample branch on `filled + i >= d` whose outcome is constant for all but at most one block in the line's life.

The design is *correct* and the "zero-latency sets pay nothing" property (`engine.cpp:382-384`) genuinely holds — I verified `comp` gates every one of these. But the moment one linear-phase EQ lands on one track, the whole mixer starts paying.

**Fix sketch.** Three independent wins:
1. Size each line to its actual delay (`maxTrackLat - trackLat[i]`), rounded up to a power of two, instead of a flat 64 Ki. Most lines need zero and can be skipped entirely.
2. Skip `pdcDelay` outright when `d == 0` — it is an exact passthrough by construction (`engine.cpp:531-534` says so), so the ring write is pure waste for the deepest-latency track, which is by definition always one of them.
3. Replace the scalar loop with at most two `memcpy` runs each direction, and hoist the `filled` branch out (it is loop-invariant except for the single straddling block).
4. Track a per-line "already silent" flag so `pdcFlush` stops re-zeroing a line that has been silent for longer than its delay.

### 3.3 CONFIRMED · `pumpDeviceQueue()` stalls the pump — and the heartbeat — for seconds, and instantiates every plugin twice

`src/daemon/latticed.cpp:903-917`
```cpp
void pumpDeviceQueue() {
    if (deviceQueue_.empty()) { registryReady(); return; }
    if (!registryReady()) return;
    while (!deviceQueue_.empty()) {          // <-- no budget, no yield
        const ipc::WireCommand w = deviceQueue_.front();
        deviceQueue_.pop_front();
        switch (w.type) { case ipc::CmdAddDevice: doAddDevice(w); break; ... }
    }
}
```

The queue holds up to `kDeviceQueueMax = 256` entries (`latticed.cpp:891`). Loading a project pushes one `CmdAddDevice` per device; a 32-track set with 4 devices each is 128 of them, drained **in a single pump tick**.

And each one is paid for twice:

`src/daemon/latticed.cpp:1026-1036`
```cpp
std::unique_ptr<PluginInstance> inst = registry_.instantiate(*desc, sr_, block_);
if (!inst) { ... }
if (!inst->prepare(sr_, block_)) { ... }     // <-- second full prepare
```

`PluginRegistry::instantiate` **already calls `prepare()`** — `instantiateLV2` at `lv2_host.cpp:861`, `instantiateCLAP` at `clap_host.cpp:963`, `instantiateInternal` at `internal_devices.cpp:500`. Calling it again is not idempotent:

* `Lv2Instance::prepare` (`lv2_host.cpp:273-323`) starts with `teardown()` → `lilv_instance_deactivate` + `lilv_instance_free`, then re-runs `buildPorts`, `buildParams`, `lilv_plugin_instantiate`, `activate`, and `settleLatency()` (which itself runs a full silent block, `lv2_host.cpp:579`).
* `ClapInstance::prepare` (`clap_host.cpp:346-384`) does `teardown()` → `stop_processing` + `deactivate` + `destroy`, then `create_plugin` again.

So **every plugin is constructed, activated, destroyed, and constructed again.** That doubles load time and doubles exposure to the exact third-party teardown hazards the file headers warn about at length (`lv2_host.cpp:21-25`, the Calf/libgomp `dlclose` problem; `clap_host.cpp:17-24`).

Meanwhile `map_.hdr->heartbeat.fetch_add(...)` (`latticed.cpp:429`) is at the *bottom* of the pump loop body. A multi-second `pumpDeviceQueue()` freezes the heartbeat, and `SharedState::stale()` (`shm.h:711-715`) is what the client uses to decide the daemon is dead. The daemon can get itself respawned while it is busy doing exactly what it was asked to do. The file's own header (`latticed.cpp:836-839`) identifies this hazard and solves it for the *scan* — but not for instantiation.

**Fix sketch.** (a) Delete the redundant `inst->prepare(sr_, block_)` at `latticed.cpp:1032`; check the instance is non-null and trust the factory, which already reports failure by returning null. (b) Give `pumpDeviceQueue` a per-tick budget — process until `monotonicNs()` exceeds a ~2 ms deadline, then return and resume next tick. (c) Move the heartbeat bump to the *top* of the pump loop, or into its own timer.

### 3.4 CONFIRMED · MIDI fan-out is O(tracks × chain × messages) in virtual calls, re-scanned per sub-block

Two separate quadratic-ish paths:

**Chain delivery**, `engine.cpp:1634-1649`: for each of 32 tracks, for each of up to `kMaxChainFx = 8` devices, for each of up to `kMidiPerBlock = 256` messages → `fx->midi(bytes, len, ...)`. Worst case **65,536 virtual calls per block**, each of which (`lv2_host.cpp:378-385`, `clap_host.cpp:456-463`) does a bounds check and a 3-byte copy into a per-instance array. The messages are identical for every track; the per-device filter (`d.hasMidiIn`, line 1639) is re-evaluated inside the loop nest.

**Capture**, `captureMidiRange` (`engine.cpp:1440-1538`): called **once per sub-block** (`engine.cpp:1562`), and each call re-walks all 32 tracks × all `midiCount` messages, discarding those outside `[from, to)` at line 1473. Total work is O(subblocks × 32 × midiCount) instead of O(32 × midiCount).

**Fix sketch.** Sort/partition the `midi[]` array by frame once per block (it is at most 256 entries and is already nearly sorted), then have each sub-block index a precomputed `[begin, end)` range instead of filtering. For chain delivery, hoist the note-capable device list per track out of the message loop and iterate messages in the outer position.

### 3.5 PLAUSIBLE · The sub-block loop admits pathological subdivision

`engine.cpp:1544-1564`. `upto = clampv(upto, pos + 1, n)` guarantees forward progress but only by **one frame** in the worst case, so the loop can run `n` times, each iteration doing `fireDue()` (three 32-track page-strided scans, `engine.cpp:997, 1041, 1069`) plus `renderRange` + `captureRange` + `captureMidiRange`.

Reaching that needs `nextB` values densely packed. One concrete route: a NaN `followBeats`. `followDueBeat` (`engine.cpp:68-73`) returns `at + len` = NaN, and NaN defeats **both** guards — `t.fireBeat > atBeat + kEps` is false and `t.fireBeat >= kNoFollow` is false — so the follow is "due" on every sub-block.

The daemon path is protected: `translateClip` validates `followBeats` finite (`latticed.cpp:705-706`). The **in-process app path is not** — `App` constructs `RtClip` directly and `Cmd::SetClip` in `drainCommands` (`engine.cpp:766-781`) does no finiteness check at all. So this is reachable from a corrupted project file in the GUI build.

**Fix sketch.** Reject non-finite `followBeats`/`lengthBeats`/`clipBpm` in `Cmd::SetClip` inside the engine, not only at the IPC boundary — the engine is the component with the hard realtime constraint, so it should not delegate its own input validation. Additionally, cap the sub-block count per block (e.g. 64) and let anything beyond that quantise to the block end.

### 3.6 CONFIRMED · `pdcFind()` is a 4-way acquire-load scan run three times per block

`engine.cpp:473-477`, called at `655` (`drainCommands`), `1583` (`process`), and `1826` (`publish`). Trivial cost, but it is three redundant lookups of an answer that cannot change within a block. Cache the result in a local at the top of `process()` and thread it through. (The `PdcTable` side-table exists only because `engine.h` is described as frozen — `engine.cpp:441-455`. Worth revisiting: a single `Pdc*` member would delete this entire mechanism, including the documented 4-slot eviction hazard at `engine.cpp:488-492` where two engines silently share one set of delay lines.)

---

## 4. Oversized blocks, rate changes, and buffer lifetimes

### 4.1 CONFIRMED — HIGH · No `jack_set_buffer_size_callback`; a buffer-size change makes the entire plugin graph transparent

`src/audio/backend.cpp:42-43` registers exactly two callbacks:
```cpp
jack_set_process_callback(client_, &JackBackend::processCb, this);
jack_set_sample_rate_callback(client_, &JackBackend::srCb, this);
```
No buffer-size callback. No xrun callback. No latency callback.

JACK permits the buffer size to change at runtime (`jack_set_buffer_size`, and PipeWire's shim renegotiates on its own). When it does:

1. `JackBackend::bs_` (`backend.cpp:104`) stays stale forever. The daemon read it once at `latticed.cpp:406` and passes it as `maxBlock` to every plugin (`latticed.cpp:1026`).
2. Every plugin was `prepare()`d at the **old** block size. Both backends then hit their oversized-block guard:
   * `lv2_host.cpp:332` — `if (bypassed_ || !inst_ || nframes > maxBlock_) { ... passthrough(...); return; }`
   * `clap_host.cpp:394` — identical.

   So **every device in the session silently becomes a passthrough, permanently.** No log, no event, no counter. The user hears the mix without any processing and has no way to find out why.
3. If the new size exceeds `kMaxBlock = 8192` (`common.h:20`), `Engine::process` clamps at `engine.cpp:1365` (`const int n = nframes < kMaxBlock ? nframes : kMaxBlock;`). This is memory-safe — the `memset` of `outL`/`outR` at lines 1359-1360 covers the full `nframes` first, so the tail is silence rather than garbage — but it is a hard, silent audio dropout for the tail of every block.

**Fix sketch.** Register `jack_set_buffer_size_callback`. JACK guarantees the process cycle is suspended for its duration and explicitly permits allocation there, so it is the right place to: update `bs_`, and signal the daemon's pump to re-prepare every `PluginInstance` and re-publish every chain. Until that machinery exists, the minimum viable version is to detect the change, log loudly, and set `SharedState::engineState`. Also register `jack_set_xrun_callback` (see §4.3) and `jack_set_latency_callback` (see §4.4).

### 4.2 CONFIRMED · GUI-owned buffer lifetimes — inventory

The engine assumes GUI/daemon ownership of externally-allocated memory in five places. Four are protected; one is not.

| buffer | protocol | protected? |
|---|---|---|
| `RtChain*` (`Cmd::SetChain/SetReturnChain/SetMasterChain`) | swap + `Ev::ChainRetired`, or the k+2 drain proof | ✔ (both paths; `latticed.cpp:1250-1271`) |
| `RtNote*` (`RtClip::notes`) | `Ev::NotesRetired` + drain proof | ✔ |
| `const f32* RtClip::data` (pool) | free-after-confirm via `EvBlockRetired` | ✔ by contract (§2.6) |
| `f32* recBuf` (`Cmd::RecordSlot`) | returned via `Ev::RecordFinished` — **the only channel** | ✘ **lost if the event ring is full** (§1.6) |
| `RtNote* recBuf` (`Cmd::RecordMidiSlot`) | returned via `Ev::MidiRecordFinished` — **the only channel** | ✘ same |
| all of the above | **dropped without retirement by `prepare()`** | ✘ see §1.1 |

The last row is worth restating: `prepare()` (`engine.cpp:575-597`) nulls every chain pointer and clears every `RtClip` — including `notes` — **without pushing a single retirement event**. The comment at `engine.cpp:588-591` justifies this on the grounds that no audio thread is running. That premise holds for the startup call and for the `NullDriver` (`latticed.cpp:149`, before the thread starts) and for `AlsaBackend::start` (`backend.cpp:135`, before the thread starts) — but **not** for `JackBackend::srCb`.

### 4.3 CONFIRMED — MEDIUM · The DAW cannot detect or report a single xrun

* `Ev::Xrun` is declared (`engine.h:150`) and classified as scalar (`control.h:564`) but **has no producer anywhere in `src/`**.
* `SharedState::xruns` (`shm.h:639`) is initialised to 0 (`shm.h:678`) and **never incremented**.
* `SharedState::blocksRendered` is only written for the null driver (`latticed.cpp:1672-1673`). With a real JACK or ALSA backend it stays 0 forever.
* `SharedState::stampHeartbeat()` (`shm.h:701-705`), which would have maintained both, has **zero callers**.
* JACK's own xrun notification is not subscribed (§4.1).
* ALSA's underrun path recovers silently: `snd_pcm_recover(pcm_, (int)w, 1)` at `backend.cpp:243`, and on the capture side at `backend.cpp:210` where a recovered xrun just zero-fills the block (`backend.cpp:217-218`).

So the one metric a DAW user cares about most during a session — "did I drop audio?" — is structurally unobservable. The `cpu` meter (`engine.cpp:1780-1784`) is the only signal, and it is a smoothed average that will not show a single-block spike.

**Fix sketch.** Register `jack_set_xrun_callback` → `Engine`-side `std::atomic<u64> xruns` increment (atomic increment from the notification thread is fine). Increment the same counter from `AlsaBackend`'s two recover paths. Have the mirror copy it into `SharedState::xruns`, and have `publish()` bump `blocksRendered` unconditionally.

### 4.4 CONFIRMED — LOW · PDC latency is computed but reported to nobody

`Engine::latencyFrames` is correctly maintained (`engine.cpp:1826-1828`) as `maxTrackLat + maxRetLat + masterLat`. Its only reader in the whole tree is `src/ui/app.cpp:3686` — the in-process GUI.

* No `jack_set_latency_callback` is registered, so JACK's graph-wide latency compensation is wrong by exactly this amount whenever a latent plugin is loaded.
* `SharedState` (`shm.h:631-716`) has **no latency field at all**, and `mirrorLoop` (`latticed.cpp:1639-1681`) does not mirror it. It also omits `returnMeterL`/`returnMeterR` (`engine.h:192`), so return-bus meters are dead in the split-process GUI.

**Fix sketch.** Add `std::atomic<i32> latencyFrames` and the four return-meter fields to `SharedStateT`; mirror them. Register `jack_set_latency_callback` and report `latencyFrames` as playback latency. Note this bumps `control::kHash` (`control.h:730-738`), which is by design — the hash exists precisely so a layout change cannot be silently mismatched across the boundary.

---

## 5. Things I checked that are correct

Recording these so a future reviewer does not re-derive them.

* **`ShmSpscRing` index masking** (`shm.h:574-589`) — every load is `& kMask` before use, so a wild or crashed peer's index yields "the wrong slot", never a read outside the mapping. Correct, and the right call for a cross-process ring. Release/acquire pairing on `w_`/`r_` is textbook.
* **`poolValidate`** (`pool.h:308-350`) — genuinely thorough. The self-mixed magic (`kPoolBlockMagic ^ ref`) is a nice touch: a block header copied to a different offset fails validation. Ordering (`bump` acquire before reading the block, `magic` acquire before trusting `bytes`) is correct.
* **The clip-shadow retirement logic** (`latticed.cpp:1489-1537`) — `cancelRetire` before `considerRetire`, the "still backing another slot" scan, and reading `drainProof()` after the push are all right.
* **`fetch()` bounds** (`engine.cpp:311-337`) — I tried to walk it off the end via `loopStart`/`loopEnd`/`channels` and could not; the `i1 >= c.frames` fallback at line 326 catches the interpolation partner, and the `pos >= c.frames` guard at 319 catches the primary. (The only route in is the torn-`RtClip` race of §1.1.)
* **`renderMidiVoice`'s lap loop** (`engine.cpp:1224`) — bounded at 4096 iterations *and* floored by `kMinLoopBeats` (`engine.cpp:25`, `1138`). Both guards, belt and braces.
* **CLAP `buildEventList`/`emitMidi`** (`clap_host.cpp:559-627`) — the `eventCount_ < kMaxEvents` guard is correctly re-evaluated per iteration and `emitMidi` writes exactly one event, so no overflow. `tlsInProcess` is restored on the `CLAP_PROCESS_ERROR` path (line 433 precedes the check at 437).
* **`Pulse` and `Saturator`** (`internal_devices.cpp`) — no allocation, fixed `acc_[kMaxBlock]`, explicit denormal flushing, oversized-block degradation to silence. These are the best-behaved RT code in the tree.
* **`busGain`** (`engine.cpp:513`) — the NaN-lands-on-zero formulation is correct and the reasoning in the comment is sound.
* **The `live[]` "recWillLaunch" case** (`engine.cpp:1395-1398`) — this is a subtle one the author clearly hunted down, and it is right.

---

## 6. Ranked findings

| # | Sev | Conf | Finding | Site |
|---|-----|------|---------|------|
| 1 | Critical | CONFIRMED | JACK `srCb` calls `Engine::prepare()` concurrently with `process()`: torn-`RtClip` → null deref, 2 MB of racing stores, `calloc`+`fprintf` from a callback, and every chain silently dropped without retirement | `backend.cpp:92-97` → `engine.cpp:573-608` |
| 2 | High | CONFIRMED | Two concurrent producers on the SPSC MIDI ring → lost note-offs / stuck notes / data race | `midi_in.cpp:142` + `app.cpp:222,2114,2147,2182-2220` vs `ring.h:15-22` |
| 3 | High | CONFIRMED | LV2 `log:log` (and CLAP `clap.log`) wired to `fprintf(stderr)`, callable from `run()`/`process()` | `lv2_host.cpp:84-99`, `clap_host.cpp:77-88,112` → `common.cpp:13` |
| 4 | High | CONFIRMED | LV2 `urid:map` takes a `std::mutex` and allocates from `run()`; lock is shared with the plugin scanner | `lv2_host.cpp:58-82` |
| 5 | High | CONFIRMED | No `jack_set_buffer_size_callback`: a buffer-size change silently turns every LV2/CLAP device into a permanent passthrough | `backend.cpp:42-43`, `lv2_host.cpp:332`, `clap_host.cpp:394` |
| 6 | High | CONFIRMED | CLAP `clap.thread-check` latches the *scan* thread as "main"; the pump thread is then told it is not, aborting plugins that assert | `clap_host.cpp:59-70` ← `latticed.cpp:859-862` |
| 7 | High | CONFIRMED | `latticed` never enables FTZ/DAZ — and it is where all plugin DSP now runs; the app sets it once on the wrong thread, x86-only | `latticed.cpp` (0 hits), `main.cpp:74-79`, `Makefile:209` |
| 8 | Med-High | CONFIRMED | Every `evts_.push()` return is ignored; a full ring silently destroys `Ev::RecordFinished` → **a recorded take is lost with no diagnostic** | `engine.cpp:291,293` (+15 more sites) |
| 9 | Medium | CONFIRMED | `insertNote()` O(N) insertion sort on the audio thread; its "almost sorted" premise is false for overdub, where beats are wrapped mod the loop | `engine.cpp:233-240` ← `1502,1526,288` |
| 10 | Medium | CONFIRMED | `pumpDeviceQueue()` drains 256 entries with no time budget, stalling the heartbeat, while `doAddDevice` instantiates each plugin **twice** | `latticed.cpp:903-917, 1026-1036` |
| 11 | Medium | CONFIRMED | `Track` layout puts recording state 65 KB past the header; every per-block and per-sub-block scan is a page-stride walk over 2.06 MB | `engine.h:220-269`; `engine.cpp:1386,1414,1440,1552,1793` |
| 12 | Medium | CONFIRMED | PDC costs 19 MB and memsets/scalar-loops 38 page-strided rings every block once *any* device reports latency | `engine.cpp:396-403,536-562,1615,1709` |
| 13 | Medium | CONFIRMED | `pumpEvents()` discards an already-popped engine event on client backpressure (comment claims a retry that does not exist) | `latticed.cpp:1622` |
| 14 | Medium | CONFIRMED | No xrun detection anywhere: `Ev::Xrun` has no producer, `xruns`/`blocksRendered` never move, `stampHeartbeat()` is dead code | `engine.h:150`, `shm.h:639,701-705`, `latticed.cpp:1672` |
| 15 | Medium | PLAUSIBLE | `drainsExact_` never un-latches: a stalled audio thread makes `retiring_`/`chainRetire_` grow unboundedly and leaks the client's pool forever | `latticed.cpp:447-452, 1257, 1560-1562` |
| 16 | Low-Med | CONFIRMED | MIDI fan-out is O(tracks × chain × msgs) virtual calls, and `captureMidiRange` re-scans all messages per sub-block | `engine.cpp:1634-1649, 1440-1538` |
| 17 | Low-Med | CONFIRMED | `Engine::latencyFrames` reported to nobody: no JACK latency callback, no `SharedState` field; return meters also unmirrored | `engine.cpp:1826`, `shm.h:631-716`, `latticed.cpp:1639-1681` |
| 18 | Low | PLAUSIBLE | Sub-block loop can subdivide to 1 frame; NaN `followBeats` defeats both guards and is unvalidated on the in-process path | `engine.cpp:1544-1564, 68-73, 766-781` |
| 19 | Low | CONFIRMED | Meter decay (`*= 0.72f`) has no denormal flush — with #7, parks ~74 denormals per block after any silence | `engine.cpp:1797, 1817, 1821` |
| 20 | Low | CONFIRMED | `pdcFind()` runs a 4-way acquire scan three times per block for an answer that cannot change | `engine.cpp:473-477, 655, 1583, 1826` |
| 21 | Low | PLAUSIBLE | `pumpParams` advances `seenParamGen` before the `deviceGeneration` check, so a rejected write can strand a parameter | `latticed.cpp:1320-1323` |
| 22 | Low | PLAUSIBLE | `chains_[chainIndex(...)]` unchecked for -1 in remove/move (checked in add) | `latticed.cpp:1096-1097, 1135` |
| 23 | Low | PLAUSIBLE | `f32*` → `RtNote*` reinterpretation is strict-aliasing UB under `-O2` | `engine.cpp:278, 1449` |
| 24 | Low | CONFIRMED | `std::tanh`/`std::exp2` per sample in the internal devices | `internal_devices.cpp:174, 309` |
| 25 | Info | — | Pool use-after-free is prevented by client-side contract only; #15 makes that contract harder to honour | `pool.h:308-350, 387` |

---

## 7. Suggested order of work

1. **#1** (rate-change race) and **#5** (buffer-size callback) together — they are the same missing mechanism: "the device reconfigured, tear down and rebuild off the audio thread."
2. **#2** (SPSC violation) — small, self-contained, and it is a correctness bug users will hit as stuck notes.
3. **#7** (FTZ in the daemon) — three lines, large effect.
4. **#8** (event-ring drops) and **#13** (pumpEvents) — both are "check the return value"; losing a take is the worst user-visible outcome in this list.
5. **#6** (CLAP thread-check) — one line moved, prevents hard aborts.
6. **#11** (Track layout) — mechanical, and the biggest single perf win.
7. **#3/#4** (plugin-facing log and urid:map) — needs a small lock-free logging ring; do them together.
8. Everything else as capacity allows.
