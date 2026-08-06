# Racks

A rack is a `PluginInstance` that owns `PluginInstance`s.

That sentence is the entire design. It is why racks needed no new model, no new
UI and no format change to be *usable*: `nxtakt:rack` appears in the ordinary
plugin browser, goes into an ordinary device chain, and its eight macros are
ordinary `ParamInfo` knobs in the ordinary device strip. Everything below is
consequence.

Up to 8 sub-devices in series, behind 8 macro parameters.

## The macro rule

```
target = min + (max - min) * macro        macro in [0,1], min/max in the TARGET's units
```

One line, and every property people expect from a macro knob falls out of it
rather than being a case in a switch:

| Property | Why it works |
|---|---|
| Partial range | `min`/`max` inside the parameter's range sweeps only that slice. Drive mapped 6→30 dB reads 18 at macro 0.5 — the midpoint of the *mapped* range, not the parameter's. |
| Inversion | `min > max` makes `(max - min)` negative, so the target walks down as the macro walks up. No branch. |
| Inversion survives clamping | Endpoints are clamped into the target's range *independently*, at edit time. `5 → -5` on a 0..1 Mix becomes `1 → 0` — still inverted. |
| One macro, many targets | Each in its own units, across devices. |
| Many macros, one target | Applied in mapping order, last write wins. "Which knob owns this parameter" has exactly one answer. |

Clamping at *edit* time is what keeps the audio-thread path a pure lerp.

`setParam` drives targets through the sub-device's `setParam`; `setParamRT`
through its `setParamRT`. Not cosmetic: `host.h` documents `setParam` as the
single producer on queue-based backends, so a rack calling it from the audio
thread would be precisely the race `setParamRT` exists to prevent.

## Instantiation

`PluginRegistry::instantiate` passes `this` down to
`detail::instantiateInternal(..., PluginRegistry* reg = nullptr)`, and the rack
is the one device that keeps it. No globals, no thread-locals, no back-channel.
A rack built through `detail::` outside a registry gets `nullptr`, still works,
and refuses to be filled while logging why.

`PluginInstance` gained `virtual RackControl* rack() { return nullptr; }`.
A `dynamic_cast` was not an option — the concrete `Rack` lives in an anonymous
namespace and is not a type any caller can name — and a virtual accessor also
lets a future CLAP or VST3 container answer the same question without being our
class. `RackControl` is a pure interface holding the editing API; the audio
thread never touches it.

## Four clauses of the contract a container cannot honestly satisfy

Written down rather than quietly worked around, because each is a silently wrong
answer if a caller assumes otherwise.

### 1. `latencyFrames()` is no longer constant after `prepare()`

**This one has a caller obligation, and ignoring it desynchronises the whole
project's delay compensation.**

A rack's latency is the **sum** of its chain — reporting 0 would be a lie the
engine acts on, and a rack containing a lookahead limiter would smear every
parallel path in the set by exactly the amount we failed to declare. Verified
against a real plugin: LSP Limiter Mono reports 240 frames; the rack reports 240
for one, 240 with a zero-latency EQ Three added, **480 for two**, 240 after a
removal.

That first proof depended on a third-party plugin happening to be installed,
which made it a claim that could only be checked on some machines. The stock
`nxtakt:limiter` reports 240 frames at 48 kHz for the same reason — 5 ms of
lookahead — so the chain-sum property is now testable in-tree, on any machine,
with no plugins installed at all.

**A bug this section did not survive first contact with.** `latencyFrames()`
originally cached the chain sum into the published layout. A rack *nested*
inside a rack could then gain a latent device and republish its own layout while
the outer rack — the only figure the engine ever reads — stayed at 0. The sum is
computed on demand now: one acquire load and at most eight virtual calls per
level, at most four levels, which is realtime-safe and cannot go stale.

But `engine.cpp` caches the figure when a chain is published, which is correct
per the contract as written. So:

> **After any structural edit to a live rack, the caller must republish the
> track's chain to the engine.**

There is no way to honour the clause from inside: the alternatives are lying
about the sum or forbidding edits, and both are worse than an obligation stated
in one line.

### 2. Removing a sub-device is not realtime-safe; unlinking it is

New topologies are published with one release store into a 4-deep ring, so
`process`/`midi`/`setParamRT` always read a structure nobody will touch again.
But **nothing inside a `PluginInstance` can know when the audio thread last
dereferenced a pointer.** The plugin layer has no deferred-destruction channel;
the engine does, since displaced chains ride an event back to the GUI.

So removed instances are **retired, not deleted** — owned by the rack until it
dies or `reclaim()` is called, bounded at 64 retired and failing loudly rather
than eating memory. The honest fix is to route retired sub-devices through the
engine's existing displacement path, which needs a contract addition that was
deliberately not invented here.

### 3. `setParamRT` applies partially

A macro returns `false` if *any* target lacks a realtime path — correct, so the
engine greys the lane rather than drawing a dead envelope — but targets that did
accept were already written before the `false` returned. **A rack macro is only
as automatable as the worst device it drives.** All-internal racks always accept.

### 4. `desc()` is static and does not describe the contents

A rack containing Pulse still reports `kind = Effect`, `audioIn = 2`, and
`hasMidiIn = true` unconditionally — the engine reads the descriptor once, when
the device is added, so a descriptor that only became note-capable after the
user dropped an instrument in would be read too late to matter. Cost is one
branch per event on an instrument-free rack. A UI wanting an instrument icon on
a rack must ask `rack()->device(i)->desc()`.

Also: `nframes > maxBlock` degrades to passthrough (the scratch pair is sized in
`prepare()`), the same degradation Pulse already makes for the same reason; and
bypass short-circuits the chain, so it drops MIDI rather than leaving a bypassed
rack holding voices through a phrase.

## Persistence: what to call, and the one ordering trap

The state is a passive description — `RackState` plus
`rackStateToString`/`rackStateFromString` — deliberately not a serializer that
knows about `project.cpp`.

The compact form is one line of printable ASCII with no whitespace, quotes or
newline, so it drops into `kv()` as an opaque scalar:

```
nxrack1;m=0,0,0.4,0,0,0,0,0;d=nxtakt%3Aeq3,0,-,0:100,1:0,...;x=2,1,0,24,3
```

Version-tagged; `;`-separated records tagged by first character, so an older
reader skips records it does not know; device records positional, so their order
*is* chain order; URIs percent-escaped over the five structural characters, so
an LV2 `http://` URI with a query string is safe. Nested racks ride as one more
escaped field, each level escaping the level below exactly once, capped at
`kRackMaxDepth = 4` so a hostile file cannot recurse the loader off the stack.
Floats go through the same shortest-round-tripping `snprintf`/`strtod` idiom as
`fmtF32` — and the de_DE decimal-comma trap is handled by the same
`LC_NUMERIC="C"` pin `main.cpp` already sets, which is why the idiom was
reproduced rather than reinvented.

Wiring it up:

1. `SavedDevice` (`src/ui/session.h`) gains `std::string state;` — nothing else
   changes shape.
2. `project.cpp` `writeDevice`: `if (!d.state.empty()) kv(o, "    ", "state", d.state);`
   Sparse, so a set with no rack in it stays byte-identical to what v6 writes
   today — the same discipline every version from v2 on has followed. Reader:
   one `state` key inside the `device` block.
3. On save: `if (RackControl* rc = inst->rack()) sd.state = rackStateToString(rc->state());`
4. On load, **in this order**:

```cpp
inst = registry.instantiate(desc, sr, block);
restoreParams(inst, sd.params);              // FIRST
if (auto* rc = inst->rack()) {               // THEN
    RackState s;
    if (rackStateFromString(sd.state, s)) rc->setState(s);
}
```

**Params first is the trap.** A rack's 8 macros are ordinary `SavedDevice`
params, and writing them goes through `Rack::setParam`, which *drives its
targets*. `setState` finishes by writing the macros **without** re-applying
them, so doing it last leaves sub-device parameters exactly as saved rather than
as re-derived from macro positions. **The reverse order would silently round
every mapped parameter on every single load** — a set that drifts a little each
time you open it, which is the kind of bug that takes months to attribute.

## UI

Nothing is required to make racks usable — that was the point. To show the
inside: `inst->rack()` non-null means this device has one, then `deviceCount()`,
`device(i)`, `addDevice`/`insertDevice`/`removeDevice`/`moveDevice`. For a
mapping editor: `addMapping`/`removeMapping`/`clearMacro`/`mappingCount`/
`mapping(i)`, with `min`/`max` in the target's units and read back clamped, so
the editor renders exactly what the macro will do.

Two obligations: republish the chain after a structural edit (§1), and call
`reclaim()` only when the rack is not in a chain the engine is processing — the
natural moment being when the engine hands a displaced chain back. The registry
that instantiated a rack must outlive it; the raw pointer is deliberate, since a
rack is not a plausible owner of the registry.

`LiveDevice` and undo rebinding by uid work unchanged: a rack carried through an
undo keeps its contents, because the *instance* is carried rather than rebuilt.
