# NxTakt IPC — shared-memory attack surface audit

Scope: `src/ipc/{shm,pool,control,client}.h`, `src/daemon/latticed.cpp` (every read of shm),
cross-referenced against `src/audio/engine.cpp` (what the scalars actually index) and
`tests/{ipc,daemon}_test.cpp` as spec.

Threat model as briefed: a malicious or buggy **local process running as the same uid** attaches
to `/dev/shm/lattice-engine-<s>` and `/dev/shm/lattice-pool-<s>` (both `0600`) and feeds hostile
data to the daemon and to the GUI client; plus a compromised/crashed peer mid-handshake.

**Framing.** Both regions are `0600`, so within a same-uid model there is no confidentiality or
integrity boundary — an attacker who can write the region can also `shm_unlink` it. So no finding
below is a privilege escalation, and none is expected to be. What matters, and what the code
itself claims (`latticed.cpp:13-24`, "no path through here may produce an RtClip whose `data` is
anything other than a pointer into the mapped pool"), is **fail-closed behaviour**: a hostile or
corrupt peer must produce a refusal, not a fault on the audio thread. Findings F1–F4 break that
claim. The same findings are reachable from a merely *buggy* GUI, which is the practical risk.

Read this as: the design is right and the bounds arithmetic in `poolValidate` is genuinely sound
(§F12). The holes are all at the seams — one unbounded scalar, one unbounded derived quantity, and
two places where a validated field is re-read from shared memory after validation.

---

## F1 — `frames` is unbounded and its byte-extent multiply overflows u64 (CRASH DAEMON)

**`src/daemon/latticed.cpp:741`**

```cpp
const u64 need = (u64)c.frames * (u64)c.channels * sizeof(f32);
```

`c.frames` is an `i64` straight off the wire. The only checks on it are
`latticed.cpp:709` (`c.frames < 0` → reject) and, when `c.valid && !c.isMidi`,
`latticed.cpp:731` (`c.frames <= 0` → reject) plus the loop window
`0 <= loopStart <= loopEnd <= frames` (`:733`). **There is no upper bound.**

Contrast `noteCount`, which *is* capped: `latticed.cpp:715`
`if (c.noteCount > (i64)INT32_MAX) reject`. That cap makes the notes multiply
(`:751`, `noteCount * 24`) overflow-free. The identical guard for `frames` is missing —
this reads as an oversight, not a decision.

### Exploit

Hostile `WireClip` in the clip table, everything else honest:

| field | value |
|---|---|
| `sampleRef` | a genuine, correctly-allocated 64-byte `PoolKindSamples` block |
| `frames` | `0x4000000000000000` (2^62) |
| `channels` | `1` |
| `loopStart` | `0` |
| `loopEnd` | `0x4000000000000000` |
| `loop`,`valid` | `1`, `1` |
| `isMidi` | `0` |
| `clipBpm` | `1e-5` (see F2 — optional, it only makes the crash immediate) |

`need = 2^62 * 1 * 4 = 2^64 ≡ 0`. `poolValidate` is then handed `needBytes = 0`, and its last
gate is `if (needBytes > b->bytes) return no(...)` (`pool.h:348`) — `0 > 64` is false. **Every
other check passes honestly**: the offset is aligned, inside the arena, under the bump, the
self-mixed magic matches, the block is `PoolKindSamples` and not free. The refusal never fires.

`latticed.cpp:764` then stores `rc.frames = 2^62` and `rc.data = base + ref` and hands it to
`Engine::pushCommand`.

The engine bounds the read **only by `c.frames`**:

```cpp
// src/audio/engine.cpp:319
if (pos < 0.0 || pos >= (f64)c.frames) { outL = outR = 0.f; return; }
...
const f32* p0 = c.data + (size_t)i0 * c.channels;   // :329
```

With `loopEnd == frames == 2^62` the wrap at `:313-317` keeps `pos` inside `[0, 2^62)`, so the
guard never fires. `srcPos` advances by `rate = tempo_/clipBpm` per output sample
(`engine.cpp:1272`); at `clipBpm = 1e-5` and `tempo_` clamped to ≤ 999 that is `~1e8` frames per
sample, so the **first** `fetch()` reads ~400 MB past a 64-byte block — past the end of the 256 MiB
pool mapping. Even at `rate = 1.0` it is a guaranteed walk off the end within seconds.

**Result:** SIGSEGV on the audio thread. `onFatal` (`latticed.cpp:104`) unlinks the control region
and re-raises, so the daemon dies and takes the session's region with it.

**Severity:** crash daemon (deterministic). Escalation: none. Also corrupts audio (arbitrary
in-pool memory rendered as samples) on the way out.

**Test gap:** `tests/daemon_test.cpp:1032-1041` covers seven hostile-offset cases including
`{"a valid block read past its end", good, 1<<20, 2}` — but `1<<20` does not overflow, so it is
caught by the `needBytes` gate. No case exercises a `frames` that overflows the multiply.

### Fix sketch

Two independent fixes; take both.

```cpp
// latticed.cpp, next to the noteCount cap at :715
if (c.frames > (i64)INT32_MAX) { reason = ipc::RejectBadClip; return false; }
// ...and make the multiply structurally incapable of wrapping:
if (c.frames > (i64)(pool_.bytes() / (sizeof(f32) * (u64)c.channels))) {
    reason = ipc::RejectBadClip; return false;
}
const u64 need = (u64)c.frames * (u64)c.channels * sizeof(f32);
```

Belt-and-braces in the engine: `fetch()`'s guard should be written as
`if (!(pos >= 0.0 && pos < (f64)c.frames))` so it is NaN-safe too (see F2).

---

## F2 — NaN/Inf laundering through `clipBpm` defeats the engine's own bounds guard (CRASH DAEMON)

**`src/daemon/latticed.cpp:705-706, 713`** and **`src/audio/engine.cpp:311-330, 1272, 1297`**

The daemon filters non-finite *inputs*:

```cpp
const f64 scalars[] = {c.clipBpm, c.lengthBeats, c.prob, c.followBeats, (f64)c.gain};
for (f64 v : scalars) if (!std::isfinite(v)) { reason = ipc::RejectNotFinite; return false; }
...
if (c.clipBpm <= 0.0 || c.lengthBeats < 0.0) { reason = ipc::RejectBadClip; return false; }
```

It does **not** bound the *derived* quantity. `engine.cpp:1272`:

```cpp
const f64 rate = (c.warp == (int)Warp::Off) ? 1.0 : (tempo_ / c.clipBpm);
```

`tempo_` is clamped to `[20, 999]` (`engine.cpp:709`), but `clipBpm` may be any positive finite
double including a denormal. `clipBpm = 1e-320` → `rate = +inf`. `warp` is validated only as
`0 <= warp <= Warp::Beats` (`latticed.cpp:710`), and `Warp::Beats == 2` is the last enumerator, so
every value passes and the `Off` short-circuit is easy to avoid.

Then `v.srcPos += rate` (`engine.cpp:1295`) → `+inf`, and `fetch()` at `engine.cpp:312-319`:

```cpp
if (c.loop && le > ls) {
    const f64 len = le - ls;
    pos = pos - ls;                       // inf
    pos -= std::floor(pos / len) * len;   // inf - inf  ==  NaN
    pos += ls;                            // NaN
}
if (pos < 0.0 || pos >= (f64)c.frames) { outL = outR = 0.f; return; }  // both false for NaN
const i64 i0 = (i64)pos;                  // NaN -> i64 is UB; x86-64 cvttsd2si -> INT64_MIN
...
const f32* p0 = c.data + (size_t)i0 * c.channels;   // c.data + 2^63 bytes
```

**Both comparisons against NaN are false, so the guard passes through.** `(i64)NaN` yields
`INT64_MIN` on every target this builds for, and the pointer arithmetic produces a wild address.

**This needs no lying `frames` and no bad pool offset** — a completely honest block, honest
`frames`, honest `loopEnd`, `valid = 1`. The clip is well-formed by every rule the daemon
enforces.

**Result:** SIGSEGV on the audio thread on the first rendered block. Same fatal path as F1.

**Severity:** crash daemon. Escalation: none.

Note the granular branch (`warp == Beats`, `engine.cpp:1279-1288`) reaches the same place through
`v.readB = v.srcPos`, so both warp modes are affected.

### Fix sketch

Bound the ratio at the boundary, where the untrusted number is:

```cpp
// latticed.cpp, replacing the clipBpm check at :713
if (!(c.clipBpm >= 1.0 && c.clipBpm <= 1.0e6)) { reason = ipc::RejectBadClip; return false; }
```

and make the engine's guard NaN-safe as noted in F1 (`!(pos >= 0.0 && pos < frames)`). The engine
fix is the load-bearing one — it closes the whole class, not just this instance.

---

## F3 — `poolValidate` re-reads `b->bytes` four times from peer-writable memory (CRASH DAEMON, TOCTOU)

**`src/ipc/pool.h:338-348`**

```cpp
if (b->bytes == 0 || b->bytes % kPoolAlign != 0) return no("block size is not a positive multiple of 64");
if (b->bytes > arenaHi - ref)            return no("block extends past the end of the arena");
if (ref + b->bytes > bump)               return no("block extends past the allocator's high-water mark");
const u32 st = b->state.load(std::memory_order_acquire);
...
if (needBytes > b->bytes)                return no("block is smaller than the clip claims");
```

Four separate non-atomic loads of a field the writer process can rewrite at any moment. The
surrounding code already knows better and does the right thing for every *other* untrusted field —
`arenaLo`, `arenaHi` and `bump` are all snapped into locals at `:317-328`, and `bump` explicitly
with an acquire load. `bytes` is the one outlier.

**Answer to the brief's question "which reads are load-once into locals vs re-read":** in
`poolValidate`, `arenaOffset`/`arenaEnd`/`bump` are load-once; `bytes` is not. In `translateClip`,
the whole `WireClip` **is** correctly snapshotted (`latticed.cpp:701`, `const ipc::WireClip c = *cell;`
— every subsequent check and every `rc` field reads the local, verified line by line) so the
classic clip-table TOCTOU is closed. In `readPoolString`, `bytes` is re-read after validation — F4.

### Exploit

The GUI owns the pool read/write. A thread in the writer flips `b->bytes` between 64 and `2^40` in
a tight loop while the pump validates. The daemon reads `bytes == 64` for the arena-bound check at
`:339` and `bytes == 2^40` for the `needBytes` check at `:348`, and accepts a clip whose declared
read extent is a terabyte against a 64-byte allocation. `rc.frames` then walks off the mapping
exactly as in F1.

Formally this is already a data race and therefore UB; the compiler may fold the loads (the acquire
load at `:342` sits between the third and fourth, which in practice makes GCC/Clang re-load). Do
not rely on either outcome — the correct code does not depend on codegen.

**Severity:** crash daemon. Escalation: none.

### Fix sketch

```cpp
const u64 bytes = b->bytes;   // one load, after the magic acquire, before any check
if (bytes == 0 || bytes % kPoolAlign != 0) return no(...);
if (bytes > arenaHi - ref)                 return no(...);
if (ref + bytes > bump)                    return no(...);
...
if (needBytes > bytes)                     return no(...);
```

Same treatment for `b->kind` (`:344`) for consistency. Consider making `bytes` a
`std::atomic<u64>` written with release before `magic`, which is what the header's own publication
argument (`pool.h:196-210`) already implies it should be.

---

## F4 — `readPoolString` re-reads `bytes` after validation and scans past the mapping (CRASH DAEMON, TOCTOU)

**`src/daemon/latticed.cpp:929-947`**

```cpp
if (!pool_.validate(ref, ipc::PoolKindString, 1, &why)) { ...; return false; }
u64 n = pool_.block(ref)->bytes;            // <-- fresh load, unvalidated
if (n > ipc::kMaxPoolString) n = ipc::kMaxPoolString;
const char* s = (const char*)pool_.at(ref);
u64 len = 0;
while (len < n && s[len]) ++len;             // <-- scans up to 1024 B, bound unrelated to validation
```

Note the deliberate `needBytes = 1` passed to `validate` — the validation proves only that *one*
byte is readable. The scan bound then comes from a completely fresh, unchecked read of the same
mutable field.

### Exploit

`PoolHeader::arenaEnd() == arenaOffset + arenaBytes`, and `PoolHeader::init` sets
`arenaBytes = payloadBytes - kPoolArenaOffset` (`pool.h:262`), so **the arena ends exactly at the
end of the payload, which is the end of the mapping.** Place a legitimate 64-byte
`PoolKindString` block at the very end of the arena (`ref = arenaEnd - 64`), pass validation
(`bytes = 64` fits), then flip `bytes` to 4096 before the pump's re-read. `n` clamps to 1024 and
the scan runs 960 bytes past the end of the mapping.

**Result:** SIGSEGV on the pump thread. (No stack overflow: `len < n <= 1024` and
`cap == kMaxPoolString == 1024`, so the `len + 1 > cap` gate at `:944` and the `memcpy` are sound —
this is purely an over-read.)

**Severity:** crash daemon. Escalation: none.

### Fix sketch

Have `poolValidate` hand the validated size back to the caller, and use that:

```cpp
u64 blockBytes = 0;
if (!pool_.validate(ref, ipc::PoolKindString, 1, &why, &blockBytes)) return false;
u64 n = blockBytes < ipc::kMaxPoolString ? blockBytes : ipc::kMaxPoolString;
```

---

## F5 — the pool handshake is unauthenticated and first-writer-wins (DoS + delivery vehicle)

**`src/daemon/latticed.cpp:494-535`**, **`src/ipc/control.h:641-645`**

`ControlHeader::poolName` / `poolBytes` / `poolEpoch` are the only fields the *client* writes, and
the header says so plainly. Any same-uid process can attach the control region R/W and publish its
own region name. The daemon maps whatever name it finds, and — by explicit design
(`latticed.cpp:486-493`) — **maps exactly one pool per daemon lifetime**:

```cpp
if (pool_.valid()) { ...log once, count a failure, return; }
```

So the attacker who wins the race owns the daemon's entire pool view for the rest of the session:

1. **Permanent DoS.** The real GUI's `publishPool()` is ignored forever (`poolAttachFailures`
   ticks, one `LOGW`). Every clip it publishes is refused `RejectBadPoolRef` / `RejectNoPool`.
   No sample or MIDI playback for the life of the daemon.
2. **Full control of the validated substrate.** The attacker's `PoolHeader` supplies `bump`,
   `arenaBytes`, and every `PoolBlock`'s `magic`/`bytes`/`kind`/`state`. `PoolReader::attach`
   (`pool.h:873-907`) checks magic, version, `arenaOffset == kPoolArenaOffset` and
   `arenaEnd() <= payloadBytes` — all trivially satisfiable. This is the delivery vehicle for
   F3/F4 (it removes any need to race a real GUI) and pairs with F9.

The daemon has no way to tell the real client from the attacker: the control region carries no
identity for the pool announcement, which the header acknowledges is a consequence of not having a
socket to pass an fd over (§3.2).

**Severity:** DoS (total loss of audio playback for the session), plus it converts F3/F4/F9 from
"race the GUI" into "just do it". Escalation: none.

### Fix sketch

No clean fix without the deferred socket + `SCM_RIGHTS` upgrade (§3.2) — that is the real answer,
because passing the fd removes the name from the protocol entirely. Interim hardening:
constrain `poolName` to the daemon's own `poolRegionName(session)` (the daemon knows its session,
and `control.h:747-749` notes the default is only a default), and cross-check
`PoolHeader::creatorPid` against the pid that is actually driving the control ring.

---

## F6 — forged `EvBlockRetired` → GUI free-list corruption → null deref (CRASH GUI)

Three weaknesses compose. All in the client, all reachable from any same-uid process that pushes
into the event ring.

**(a) `SamplePool::validRef` omits the magic check that `poolValidate` has.**
`src/ipc/pool.h:720-725`:

```cpp
bool validRef(u64 ref) const {
    if (!valid() || ref == 0 || ref % kPoolAlign != 0) return false;
    if (ref < hdr_->arenaOffset + sizeof(PoolBlock)) return false;
    if (ref > hdr_->bump.load(std::memory_order_relaxed)) return false;
    return true;
}
```

Bounds only. So `blockAt(ref)` returns an *interior* pointer for any 64-aligned offset under the
bump, reinterpreting 128 bytes of sample data as a `PoolBlock`. The daemon-side `poolValidate`
(`pool.h:332`) checks `magic == kPoolBlockMagic ^ ref` precisely to stop this; the writer side does
not.

**(b) the client trusts the daemon's echo unconditionally.** `src/ipc/client.h:538`:

```cpp
case EvBlockRetired: pool_.confirmRetired(e.ref); return true;
```

No check that this client ever published `e.ref`, no check that the block is one it believes is
`Retiring`.

**(c) the free-list walkers dereference `blockAt()` without a null check.**
`insertFree` (`pool.h:792`): `cur = blockAt(cur)->next;` — and the first `cur` is
`hdr_->freeHead`, straight out of shared memory.
`normalize()` (`pool.h:806`): `PoolBlock* b = blockAt(cur); const u64 next = b->next;`
`normalize()` (`pool.h:809`): `PoolBlock* n = blockAt(next); ... n->bytes`
`normalize()` (`pool.h:826`): `PoolBlock* b = blockAt(cur2); if (cur2 + b->bytes == bump)`

`takeFromFreeList` (`:735`), `largestFree` (`:495`) and `freeListLength` (`:713`) all *do* check
`if (!b) break;`. Three walkers check, two do not.

### Exploit chain

The fake `PoolBlock` at `ref` needs `state == BlockRetiring (3)` at `ref-72`, `live == 0` at
`ref-64` and `refs == 0` at `ref-68` (offsets from `PoolBlock`'s layout: `state` at +56, `refs` at
+60, `live` at +64). As `f32` bit patterns those are `4.2e-45` (a denormal, indistinguishable from
silence) and two zeros — **ordinary near-silent audio is full of qualifying offsets.**

Push a `WireEvent{type = EvBlockRetired, ref = <such an offset>}` into the event ring. The client's
`popEvent` → `observe` → `confirmRetired` (`pool.h:654`) sees `Retiring`, sets `Quiescent`, and
calls `free(ref)`. `free` decrements `liveBlocks`/`bytesUsed` by a garbage `bytes`, then
`insertFree(ref)` splices the fabricated block into the real free list, then `normalize()` walks it.
The `next` pointer read from arbitrary sample data will not be a valid ref → `blockAt` returns
null → **SIGSEGV in the GUI**. If it *is* coincidentally valid, `normalize`'s coalescing
(`b->bytes += sizeof(PoolBlock) + n->bytes`, `:810`) produces overlapping allocations, so the next
`alloc()` hands out a block that overlaps live audio — silent corruption instead of a crash.

Note the event ring is SPSC; a second producer also corrupts `w_`, but the masking (F12) keeps that
in-bounds, so it degrades to lost/duplicated events rather than OOB.

**Severity:** crash GUI, or corrupt audio. Escalation: none. The same failure is reachable from a
merely *buggy* daemon, which is the reason to fix it regardless of the same-uid argument.

### Fix sketch

All three, cheapest first:

1. `validRef`: add the magic check, mirroring `poolValidate` —
   `if (b->magic.load(acquire) != (kPoolBlockMagic ^ ref)) return false;`
2. `insertFree` / `normalize`: `if (!b) break;` at every `blockAt()` site, same as the other three
   walkers.
3. `EngineClient::observe`: only act on `EvBlockRetired` for a ref the client has itself marked
   `Retiring` (it already tracks the shadow/pending tables — a small `std::set<u64>` or a scan of
   `shadow_` closes it).

---

## F7 — unbounded retirement queue and unbudgeted drain loops (DoS)

**`src/daemon/latticed.cpp:1514-1567`, `:544-575`, `:815-823`**

`retiring_` (`std::vector<Retire>`) is appended by `considerRetire` on every displaced pool block,
and entries leave only when both the drain proof lands **and** `publishRetired` gets a slot in the
event ring (`:1563-1564`, `if (due && publishRetired(r)) continue;`). A client that simply never
calls `popEvent` makes `map_.evts->push` fail forever while it keeps issuing `SetClip`s.

Growth is deduplicated per ref (`:1519`), so the ceiling is "distinct pool blocks" — but the
attacker sizes the pool (F5), and tmpfs is sparse, so a 64 GiB region costs it nothing and offers
~350M distinct 192-byte block slots.

Two amplifiers make this bite long before memory does:

* `considerRetire` scans the full 32×32 shadow table **and** all of `retiring_` per call
  (`:1516-1519`) — O(1024 + n) per displaced block, so the pump is O(n²) in the queue length.
* `pumpCommands` (`:551`) and `pumpMidi` (`:817`) drain `while (ring->pop(...))` with **no
  per-tick budget**. A producer that refills as fast as the daemon drains keeps the loop from ever
  returning, so `pumpEvents`, `pumpRetirements` and the `heartbeat.fetch_add` at `:429` never run.

**Result:** the `ControlHeader::heartbeat` freezes → a legitimate client's `alive()` /
`SharedState::stale()` concludes the engine is wedged → §4.4 respawn logic fires against a daemon
that is actually alive and holding the audio device. Then RSS grows without bound.

**Severity:** DoS (pump livelock, false "engine wedged", eventual OOM). Escalation: none.

### Fix sketch

* Cap `retiring_` (a few thousand). On overflow, drop the oldest, bump a counter in
  `ControlHeader`, and log once — the failure mode of a dropped retirement is a client-side pool
  leak, which is strictly better than a dead daemon, and it is *observable*.
* Give `pumpCommands` / `pumpMidi` / `pumpEvents` a per-tick budget (e.g. one ring capacity) and
  return; the pump already resumes next tick and `pending_` already models exactly that.
* Replace the linear dedup scan with a hash set keyed on `ref`.

---

## F8 — the phase-1 "never reap on the way in" fix holds for the client but NOT for either create path (lifecycle)

The fix is documented at `src/ipc/client.h:94-99`:

> Stale regions are reaped on the way *out*, never on the way in. Reaping first looks tidier and is
> wrong: `reapIfStale()` treats a region that exists but is not yet sized as an orphan, which is
> exactly what a daemon between `shm_open()` and `ftruncate()` looks like.

`EngineClient::attach` honours it (`client.h:109-125`: attach first, reap only on failure). The
mechanism is `ShmRegion::reapIfStale` at `shm.h:388-394`:

```cpp
if (::fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(ShmHeader)) {
    ::close(fd); ::shm_unlink(nm); return true;      // "nobody can be using this"
}
```

**Two callers violate it.**

### F8a — the daemon pre-reaps its own control region

**`src/daemon/latticed.cpp:274-276`**

```cpp
ipc::controlRegionName(opt_.session, gRegionName, sizeof gRegionName);
ipc::ShmRegion::reapIfStale(gRegionName);          // <-- unconditional, pre-emptive
if (!region_.create(gRegionName, ipc::control::kBytes, ipc::control::kHash)) { ... }
```

This is redundant — `ShmRegion::create` already reaps on `EEXIST` (`shm.h:229-233`), which is the
correctly-guarded form — and it is the exact anti-pattern the comment names.

Two daemons starting concurrently on one session: A wins `O_CREAT|O_EXCL` and is between
`shm_open` and `ftruncate` (`shm.h:228-239`); B's pre-reap sees `st_size == 0 < sizeof(ShmHeader)`,
unlinks A's live name, and creates its own. Both now believe they own the session. The GUI attaches
to whichever is currently named. Worse, **A's `close()` unlinks B's region** — `unlink_` is true and
`name_` matches (`shm.h:346`), so A exiting silently destroys the live daemon's name, orphaning the
GUI's mapping with no shutdown flag set.

The race window is small but it is exactly the window the comment was written about, and the §4.1
"two daemons racing for one session resolve here" claim at `:270-273` depends on `O_EXCL` being the
only arbiter.

### F8b — the pool pre-reaps, and its liveness key is wrong by construction

**`src/ipc/pool.h:414-418`**

```cpp
// A crashed GUI leaves its pool behind by design (§4.3) — but a *stale* one, whose creator is
// provably gone and which nobody has adopted, would block this create() forever.
ShmRegion::reapIfStale(name);
if (!region_.create(name, payloadBytes, pool::kHash, kShmVersion, /*seal*/true)) { ... }
```

Same unsized-window race as F8a, with a much wider window: the `ftruncate` is 256 MiB.

But the deeper problem is in the comment's own words — **"and which nobody has adopted"** is a
condition the code never checks. `reapIfStale` keys liveness on `ShmHeader::creatorPid` +
`creatorStartTicks` (`shm.h:402-403`), and the pool is the one region in the system *designed* to
outlive its creator: `abandon()` (`pool.h:465`), `attachPool()` (`client.h:238`), the §4.3
crash-handoff, and the whole "republish is not a reload" property. **`creatorPid` is never updated
when a replacement GUI adopts the pool.**

So: GUI A creates the pool and dies. GUI B adopts it via `attachPool` and is happily playing clips
out of it. Any third process — or B's own `createPool` for a second session, or a
`lattice --clean-shm` maintenance path (`shm.h:381`) — calls `reapIfStale`, sees A's pid is gone,
and **unlinks the live, adopted pool.** The daemon's mapping survives (that part is designed
correctly), but the name is gone, so a daemon respawn can never re-attach, and a fresh `create()`
under the same name makes previously-published offsets name entirely different samples.

`PoolHeader` has its own `creatorPid` field (`pool.h:252`) which is written at `init` and, as far
as I can find, **never read by anything**. It looks like the intended hook for exactly this.

**Severity:** DoS / silent session corruption; two-daemon confusion. Escalation: none.

### Fix sketch

* Delete both pre-emptive `reapIfStale` calls. `ShmRegion::create`'s `EEXIST` path already does the
  right thing and is the only place that should.
* Narrow `reapIfStale`'s unsized-region rule: instead of unlinking immediately, retry the `fstat`
  after a short delay and only reap if it is *still* unsized — a creator between `shm_open` and
  `ftruncate` is microseconds wide, a corpse is forever.
* For the pool, stop keying liveness on the creator. Either have adopters stamp
  `PoolHeader::creatorPid` (and `creatorStartTicks`) on `attachPool`, or add a lease: the current
  owner refreshes a timestamp in the header, and reaping requires the lease to be stale as well as
  the pid dead.

---

## F9 — `ftruncate` on the pool → SIGBUS (CRASH DAEMON; known, but live under this threat model)

**`src/ipc/pool.h:32-61`, `src/ipc/shm.h:363-377`**

`SamplePool::create` asks for `F_SEAL_SHRINK` (`pool.h:418`, `seal=true`), but `trySeal` is
best-effort against a `shm_open` object, which Linux does not create sealable — so `sealed()` is
false on every real run, and `PoolHeader::flags` bit 0 records that honestly.

The residual guarantee is stated as "nothing else in the process can shrink it either: after create
the GUI closes its fd and no one re-opens the object O_RDWR". Under this threat model that premise
does not hold: the pool is `0600`, so **any same-uid process can `shm_open(..., O_RDWR)` and
`ftruncate` it smaller.** The daemon's mapping then faults SIGBUS on the first access to a page
past the new end — on the **audio thread**, inside `fetch()`. `onFatal` unlinks the control region
and re-raises.

The header documents this as a known weakness pending the memfd + `SCM_RIGHTS` upgrade (§3.2).
Recording it here because (a) it is squarely inside the stated threat model, (b) F5 lets an
attacker supply a pool it owns outright and shrink it at a chosen moment, and (c) it is the one
finding in this list with no cheap fix.

**Severity:** crash daemon. Escalation: none.

**Fix sketch:** the deferred one is the only real one — `memfd_create` + `F_SEAL_SHRINK` +
`SCM_RIGHTS` over the socket. Until then, `sealed()` is already reported; consider refusing to map
an unsealed pool when a hardened mode is requested, and logging the unsealed state at attach so it
appears in bug reports.

---

## F10 — `ControlHeader::poolName` is read as a C string without a terminator guarantee (LOW)

**`src/daemon/latticed.cpp:512-513`**

```cpp
char nm[sizeof map_.hdr->poolName + 1];
std::snprintf(nm, sizeof nm, "%s", map_.hdr->poolName);
```

The comment above it correctly worries about *truncation* ("a name truncated here would open a
different region, not fail") and sizes `nm` accordingly — but `poolName` is a `char[96]` written by
the peer, and nothing guarantees a NUL inside it. `%s` reads until it finds one, running past the
field into `poolBytes`/`poolEpoch`/`reserved[]` and onward through the control region.

In practice the header sits at payload offset 0 of a ~1.4 MiB region so this terminates almost
immediately on a zero byte, and the resulting garbage name is then rejected by
`ShmRegion::normalize` (`shm.h:454-462`, which requires a single path component). Writes to `nm`
are bounded by `snprintf`, so there is no overflow. It is an over-read of the mapping, not past it.

**Severity:** low (garbage log line; theoretically an over-read). Escalation: none.

**Fix sketch:** the codebase already has the right idiom — `EngineClient::fixed`
(`client.h:748-752`) walks at most `cap` bytes. Use the same shape here:

```cpp
char nm[sizeof map_.hdr->poolName + 1];
std::memcpy(nm, map_.hdr->poolName, sizeof map_.hdr->poolName);
nm[sizeof map_.hdr->poolName] = '\0';
```

---

## F11 — liveness and lifecycle state are peer-writable (trust-model note, LOW)

Every field the lifecycle logic keys on lives in a region any same-uid process can map R/W:

* `ShmHeader::creatorPid` / `creatorStartTicks` (`shm.h:167-168`) — read by
  `EngineClient::attach` (`client.h:118`), `alive()` (`:595`), and `reapIfStale` (`shm.h:403`).
  Writing a **dead** pid there makes `attach`'s failure path at `client.h:123` `reapIfStale` a
  **live** daemon's control region. Writing `1` (init, always alive) makes `alive()` never report
  death, so a client never respawns after a real crash.
* `ControlHeader::shutdown` (`control.h:589`) — read at `client.h:142` and `:593`. Setting it makes
  every client refuse to attach ("the engine that owns this region is shutting down"); clearing it
  hides a real shutdown.
* `ControlHeader::heartbeat` and `SharedState::heartbeatNs` (`shm.h:635`) — freezable, so
  `stale(toleranceNs)` is forgeable in both directions.

None of this is an escalation: a process that can write these can `shm_unlink` the region outright.
It is listed because it means **every** liveness decision in the protocol is attacker-steerable,
and because a *crashed* peer produces the same states by accident — the pid-plus-start-time
discipline (`shm.h:117-120`) defends against pid reuse, which is a different and real problem, but
it should not be read as defending against a hostile writer. Worth one sentence in
`docs/PROCESS-SPLIT.md` §4.4 so the guarantee is not over-claimed.

**Severity:** low / documentation. Escalation: none.

---

## F12 — verified sound (no action)

Recording these because the brief asked for "verify EVERY load", and a negative result is the
useful half of that.

**Ring indices — all masked, no OOB.** `shm.h:574-599`: `push` masks `w_` on load and derives
`next` masked; `pop` masks `r_` on load; `empty()` and `size()` mask both. `buf_[w]` / `buf_[r]` are
therefore always in `[0, N)` regardless of what a wild or crashed peer left in the index. A second
producer corrupts *ordering* (torn slots, lost or duplicated messages) but never memory safety.
Capacity is a compile-time power of two (`static_assert`, `:551`).

**`poolValidate` bounds arithmetic is overflow-safe.** `pool.h:308-350`. I tried to wrap it:
`arenaEnd() = arenaOffset + arenaBytes` with `arenaBytes = 2^64 - 4096` wraps to 0, but
`PoolReader::attach` pins `arenaOffset == kPoolArenaOffset` (`:894`) and `poolValidate` then
rejects everything via `ref >= arenaHi` — **fails closed**. `bytes > arenaHi - ref` cannot wrap
because `ref < arenaHi` is checked first. `ref + bytes` cannot wrap because `bytes <= arenaHi - ref`
already bounds it below `payloadBytes`. A wild `bump` (say `~0ull`) makes the two high-water checks
vacuous but does not widen the arena bound, so the mapping bound still holds. The `ref - sizeof(PoolBlock)`
header read is protected by `ref >= arenaLo + sizeof(PoolBlock)`. The only defects are the caller's
overflow (F1) and the re-read (F3).

**The clip-table TOCTOU is genuinely closed.** `latticed.cpp:701` snapshots the cell into a local
before any check, and I traced every subsequent read: all validations and all `rc` field
assignments (`:705-780`) use the local `c`, never `*cell`. Tearing during the snapshot gains an
attacker nothing, since it can write any 120-byte value directly.

**Table indexing is bounded on both sides.** `ControlMap::device`/`param` gate on `id < kMaxDevices`
(`control.h:771-782`); `clip(track, slot)` gates both (`:787-791`); `create`/`attach` additionally
prove the **last** element of each array fits, not just the first (`:807-808`, `:832-838`) — which
is the check `at<T>()` alone cannot make. Daemon side: `deviceById` (`latticed.cpp:969`),
`chainIndex` (`:986-991`), `pos` clamp in `doAddDevice` (`:1048`), `to` clamp in `doMoveDevice`
(`:1140-1141`). Client side: `client.h:458, 472, 528, 546, 551`. All 320-row accesses bounded.
`doRemoveDevice`'s `chainPush_.back()` (`:1116`) is safe — `publishChain` unconditionally
`push_back`s one entry immediately before (`:1221`).

**The param table's numeric path is clean.** `pumpParams` (`latticed.cpp:1313-1333`) filters
`!std::isfinite(v)` before any use, `d->paramCount` is clamped to `kMaxDevParams` at add time
(`:1044`) and `value[]` is exactly that long, and all three backends clamp to `[min, max]`
independently — `internal_devices.cpp:81`, `clap_host.cpp:480`, `lv2_host.cpp:402`. So a hostile
finite value cannot reach a plugin out of range. Generation stamping is sound: `deviceGen_[id]` is
`++`-only per slot (`:1040`, "never reused"), the client stamps from its own record rather than the
table (`client.h:459`), and the daemon compares before applying (`:1323`). Minor wrinkles, not
worth a finding: `seenParamGen` is advanced *before* the `deviceGeneration` test (`:1322-1323`), so
a forged same-generation write can swallow a legitimate batch (knob desync, self-correcting on the
next bump); and `resetParamRow`'s `generation.store(0)` (`:1395`) can race a client `fetch_add`.

**Command dispatch fails closed.** `commandIsKnown` (`control.h:540`) admits only
`type <= Cmd::RecordMidiSlot` or the device range; `commandIsScalar`'s switch has no `default` and
falls through to `return false` (`:516`), so an unnamed enumerator in range becomes
`RejectPointerPayload` rather than a forwarded command. `commandCarriesPointer` is the complement,
so nothing can be in none of the classes. `c.p = nullptr` is set unconditionally on every
translated command (`:648`, `:685`).

**Region handshake fails closed against a hostile *creator*.** A fake control region can forge
magic/version/`layoutHash`/`headerBytes` and set `totalBytes == st_size`, so `ShmRegion::validate`
(`shm.h:472-494`) passes — but if it is smaller than `control::kBytes`, `ControlMap::attach`'s
`at<T>()` calls return null and `client.h:127-132` reports "the sections do not fit" and detaches.
Correct.

**Miscellaneous verified:** `ShmRegion::at<T>` (`shm.h:433-438`) checks alignment and both bounds
without overflow. `normalize` (`:454-462`) sizes exactly (`strlen(body) + 2 > cap` → 1 + 94 + 1 = 96
fits `kNameMax`). `procStartTicks` (`:123-147`) bounds its `fread` and handles a `comm` containing
parens by scanning from the last `)`. `PoolReader::attach` checks size before touching the header
(`:882`), which is the one ordering that would otherwise fault. `offsetOf` (`:942-947`) bounds both
ends before converting, so `Ev::NotesRetired` carrying a foreign pointer is dropped and counted
(`latticed.cpp:1542-1550`) rather than echoed. The daemon maps the pool `PROT_READ`
(`pool.h:875`), so a daemon-side bug genuinely cannot corrupt the allocator — that claim holds.

---

## Latent, not currently exploitable

* **The loop window is unchecked when `isMidi` is set.** `latticed.cpp:727-737`: the
  `loopStart`/`loopEnd`/`frames` consistency check lives in the `else` branch, so a clip with
  `isMidi = 1` carries an arbitrary loop window into `RtClip`. Harmless today —
  `renderMidiVoice` uses only `notes`/`noteCount`/`lengthBeats`, and `loopStart` reaches nothing
  but the unused `v.srcPos` seed at `engine.cpp:635`. It becomes F1-shaped the moment the MIDI path
  gains a loop window. Cheap to close now: hoist the loop-window check out of the `else`.
* **Scalars are unchecked when `valid == 0`.** Same block, `if (c.valid)` at `:726`. An invalid
  cell's `frames`/`loopEnd` reach `RtClip` unvalidated, but `engine.cpp:716` (`if (!cl.valid) break;`)
  and `:1086` gate every launch path on `valid`, so nothing indexes them. Also worth hoisting.

---

## Suggested order of work

1. **F1** — one-line bound plus an overflow-safe multiply. Deterministic remote-ish crash today.
2. **F2** — one-line `clipBpm` bound at the boundary, plus the NaN-safe rewrite of `fetch()`'s
   guard, which closes the whole class rather than this instance.
3. **F3, F4** — load `bytes` once; thread the validated size out of `poolValidate`.
4. **F6** — magic check in `validRef`, null checks in the two walkers, client-side retirement set.
5. **F8** — delete both pre-emptive `reapIfStale` calls; fix the pool's liveness key.
6. **F7** — cap `retiring_`, budget the drain loops.
7. **F5, F9** — need the socket (§3.2); until then, constrain `poolName` to the session default.
8. **F10, F11** — bounded copy; one paragraph in §4.4.

Regression tests worth adding to `tests/daemon_test.cpp:1032`'s `BadCase` table, which is already
the right shape for all of them: `frames = 2^62`; `clipBpm = 1e-320`; a `bytes`-flipping writer
thread racing `poolValidate`; a forged `EvBlockRetired` against `EngineClient`.
