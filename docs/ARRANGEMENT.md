# Arrangement view

Draft for `docs/ARRANGEMENT.md`. Status: design only — nothing in this document
is implemented. It is written to be handed to wave-8 agents as a specification,
so where a decision could go two ways the decision is made here and the
rejected alternative is written down beside it. It is the sibling of
`docs/AUTOMATION.md` and follows the same discipline; where the two overlap,
this one defers.

## Contents

1. [What this is, and the two rules everything follows](#1-what-this-is)
2. [Data model](#2-data-model)
3. [Engine: scheduling and playback](#3-engine-scheduling-and-playback)
4. [Session ↔ Arrangement](#4-session--arrangement)
5. [Recording into the arrangement](#5-recording-into-the-arrangement)
6. [Arrangement automation](#6-arrangement-automation)
7. [UI](#7-ui)
8. [Persistence: format v6](#8-persistence-format-v6)
9. [Daemon composition](#9-daemon-composition)
10. [Phasing, file ownership and test strategy](#10-phasing-file-ownership-and-test-strategy)
11. [Deliberately deferred](#11-deliberately-deferred)
12. [Open questions for the orchestrator](#12-open-questions-for-the-orchestrator)

---

## 1. What this is

The Session view is a grid of loops a performer launches. The Arrangement view
is a linear timeline those launches get written onto — Live's split, and the
reason Live is the reference: the two views are not two documents, they are two
ways of holding one set, and a DAW that has only the first is a sketchpad while
one that has only the second is a tape machine.

`MainView::Arrangement` already exists (`src/ui/session.h`), Tab already
switches to it, and `App::drawArrangementView` (`src/ui/app_chrome.cpp:424`)
already draws a bar ruler, one empty lane per track and a playhead, under a line
of text that says arrangement recording is not wired up yet. This document is
what replaces that line.

### The two rules

> **Rule 1 — Placement copies content.** Putting a clip on the timeline makes a
> copy of it. The arrangement never points at the session.

> **Rule 2 — Playback is a scheduler, not a renderer.** An arrangement item
> starting is the same event a `Cmd::LaunchClip` is, at the same sub-block
> boundary, through the same `startVoice`, on the same `RtClip`. Nothing about
> how a voice sounds changes.

Rule 1 is §2 and it is what makes the model small: because an item owns its
clip outright, per-instance gain, loop, warp, envelopes and notes need no
fields of their own — they are already there, inside the copy. Rule 2 is §3 and
it is what makes the headline gate reachable:

> **An arrangement and a scripted session performance of the same music must
> render BIT-IDENTICALLY.**

That is the acceptance criterion for the whole wave, and every design choice
below that looks conservative is conservative because of it. It is a strong
statement and it is deliberately strong: it says the arrangement adds no signal
path, no second mixdown, no alternate voice, and no rounding. If a wave-8 change
makes the two renders differ by one LSB, the change is wrong, not the test.

Two supporting gates fall out of it and are named here so nobody has to derive
them: a clip split N times must render bit-identically to the unsplit clip
(§3.5), and a set with no arrangement at all must render bit-identically to what
it renders today (§3.4's fade gate, §6's pass ordering).

---

## 2. Data model

### 2.1 GUI model (`src/ui/session.h`)

```c++
// One clip placed on the timeline.
//
// `src` is BY VALUE, and that is decision one of this document: an arrangement
// item OWNS its clip. Everything a user wants to differ between two placements
// of the same material -- gain, loop, warp, the clip's own envelopes, its notes
// -- is already a field of ClipModel, so copying costs nothing in schema and
// saves declaring the whole of ClipModel a second time as "the per-instance
// overrides". See §2.2 for the two alternatives and why neither survives.
struct ArrangeClip {
    u64 uid = 0;            // stable identity; Session::newUid()
    f64 start  = 0.0;       // absolute timeline beats
    f64 length = 4.0;       // beats occupied on the timeline
    f64 offset = 0.0;       // clip-relative beat this item begins at
    f64 fadeIn  = 0.0;      // beats, from `start`
    f64 fadeOut = 0.0;      // beats, back from `start + length`
    u8  fadeShape = 0;      // reserved, exactly as AutoPoint::curve is; see §8.3
    u8  pad[7] = {};
    // PROVENANCE ONLY. The uid of the ClipModel this item was made from, kept so
    // that "select every instance of this loop" and a future "update from
    // source" have something to match on. It is never resolved during playback,
    // never during save, and never during load: it DANGLES SOFT, exactly as a
    // parameter address naming a deleted device does (PARAM-ADDRESS.md), and is
    // written back unchanged forever.
    u64 sourceUid = 0;
    ClipModel src;          // the copy -- see above
};

struct TrackModel {
    // ... existing fields ...
    std::vector<ArrangeClip> arrange;       // sorted by start; invariant in §2.3
    std::vector<AutoLane>    arrangeAutos;  // ABSOLUTE-beat lanes; §6
    f32 arrHeight = 68.f;                   // this track's lane height, logical px
};

struct Session {
    // ... existing fields ...
    // The arrangement loop brace. Session-wide, like tempo and the quantum,
    // because there is one timeline.
    f64  loopStart = 0.0;
    f64  loopEnd   = 16.0;
    bool loopOn    = false;
};
```

`ClipModel` itself does not change. That is worth stating plainly: the piano
roll, the clip detail panel, `pushClip`, the automation publisher and the
project writer all already know how to handle a `ClipModel`, and an arrangement
item hands them one.

### 2.2 Why placement copies, and what was rejected

**Rejected: reference by uid.** `ArrangeClip{..., u64 clipUid}` resolving into
`TrackModel::slots[]` at publish time. It is the smallest change and it is
wrong, for one reason that is not about implementation at all:

> A session scratchpad must not retroactively rewrite the record.

The Session view is where a performer tries things. Nudging the gain on a loop
at 2 a.m. must not silently move the gain of the eight places that loop was
committed to the timeline three hours earlier. Under reference-by-uid it does,
and the user has no affordance that would tell them so. Live copies, Bitwig
copies, Pro Tools copies; the one DAW-shaped thing that references is a *linked*
clip, and linking is an explicit gesture with its own visual language, which is
a feature and not a default.

The mechanical consequences reinforce it. Under reference-by-uid, the moment
someone wants a different gain on one placement, `ArrangeClip` grows `gain`; then
`loop`; then `warp`; then `loopStart`/`loopEnd`; then a per-instance envelope
vector; then a per-instance note vector because they trimmed a note. The end
state of that road is `ClipModel` re-declared field by field with an "is it
overridden" bit beside each one. `src` by value *is* that end state, reached in
one step, with no override bits.

**Rejected: a shared payload.** A pool of `ClipModel`s owned by the `Session`,
with both slots and arrangement items holding indices into it, copy-on-write on
edit. This is the technically elegant one and it dies on the format:

> The project format has no way to say "same object as" without a second
> identity space.

`project.cpp` has exactly one identity space, `uid`, and it is *entity* identity
— a track, a scene, a clip, a device. A payload pool needs a second one: a
`clippool` block, `poolref <n>` lines, and a rule for what happens when a
`poolref` names nothing. That is a second parser's worth of resolution inside a
file whose governing comment says there is deliberately ONE parser for all
versions. It is decisive, not merely inconvenient: the format is the undo
snapshot (`App::UndoEntry::text`), so a second identity space is also a second
thing every undo has to round-trip correctly.

**What the copy actually costs.**

- **Audio: nothing.** `ClipModel::sample` is a `SampleRef`, which is
  `std::shared_ptr<SampleBuffer>` (`src/audio/sample.h:110`). Copying an
  `ArrangeClip` bumps a refcount; the samples are shared, exactly as two session
  slots pointing at one file already share them. A 40-item arrangement over one
  drum loop holds one decoded buffer.
- **MIDI: real, and bounded.** `ClipModel::notes` is a `std::vector<NoteModel>`
  and it is duplicated per item. The bound is `kMaxArrNotes` (§2.4), and §3.3's
  publisher dedupes identical payloads so that N splits of one clip cost one
  copy on the wire even though they cost N in the model.
- **Envelopes: real, and small.** Each item's `src.envelopes` are its own, which
  is the point — a clip envelope drawn on one placement must not appear on
  another.

### 2.3 The overlap invariant, and the one place we diverge from Live

Live's arrangement lane is strictly non-overlapping: dropping a clip over
another trims or replaces the other. That rule is worth keeping almost entirely,
because it is what makes a lane a *sequence* — sorted, one thing at a time,
navigable with a cursor rather than a search.

We diverge from it by exactly one bounded case, and the bound is what makes the
divergence free.

> **INVARIANT.** For a track's `arrange` vector, with items indexed in vector
> order:
>
> 1. **Sorted.** `arrange[i].start <= arrange[i+1].start`.
> 2. **Minimum length.** `arrange[i].length >= kMinArrBeats`.
> 3. **Fades fit.** `fadeIn >= 0`, `fadeOut >= 0`, `fadeIn + fadeOut <= length`.
> 4. **Neighbours overlap only as a crossfade.** With `a = arrange[i]`,
>    `b = arrange[i+1]` and `a.end = a.start + a.length`: either
>    `b.start >= a.end`, or **all** of
>    - `a.end - b.start <= kMaxOverlapBeats` (== **4**),
>    - `a.fadeOut >= a.end - b.start` (the outgoing item fades across the whole
>      overlap), and
>    - `b.fadeIn >= a.end - b.start` (the incoming item fades in across it).
> 5. **At most two at once.** For every three consecutive items
>    `a, b, c`: `c.start >= a.end`.

Rule 5 is the load-bearing one and rules 4's bound exists to make it cheap to
uphold. Two simultaneous items per track is **exactly `Track::voice` and
`Track::prev`** — the pair the engine already keeps so that a same-track clip
switch crossfades instead of hard-cutting (`Engine::startVoice`,
`src/audio/engine.cpp`). So the divergence costs the engine **nothing**: no
third voice, no per-track voice pool, no change to the mixdown, no change to
`renderRange`'s shape. A crossfade in the arrangement runs down the same two
slots a session clip switch runs down.

**Why 4 beats and not a duration.** A crossfade is a musical gesture and the
timeline is in beats; a bound in milliseconds would mean the invariant a file
satisfies at 120 BPM is violated at 60. Four beats is one bar in 4/4 and two
seconds at 120 BPM — longer than any crossfade anyone draws with a mouse, short
enough that "two items at once" never becomes "an arrangement of stacked
layers", which is what rule 5 exists to forbid.

**Why fades must cover the overlap.** An uncovered overlap is two clips summing
at full level, which is a mix decision the timeline has no way to express and
the user has no way to see. Requiring the fades makes every overlap *visibly* a
crossfade.

### 2.4 Bounds

Enforced by the editor and by the publisher, never by the parser — the same
split `kMaxClipLanes` and `kMaxClipAutoPoints` already make (AUTOMATION.md §2.1,
and `project.cpp`'s `St::Env` comment says why).

| constant | value | why |
|---|---|---|
| `kMaxArrItems` | 512 | per track. A 512-item lane at one item per bar is 128 bars of continuously different material on one track; past that the user wants a second track, not a longer lane. It is also the width the daemon validates against, so it must be a number and not a hope. |
| `kMaxArrNotes` | 65536 | per track, summed over every item's `src.notes`. At `sizeof(RtNote) == 24` that is **1.6 MB** — the number §3.2 quotes as the reason clip envelopes do not ride in the same allocation. |
| `kMaxArrLanes` | 32 | arrangement automation lanes per track. Twice `kMaxClipLanes`, because a clip is *about* one or two parameters while a track's timeline accumulates every automated parameter in its chain. |
| `kMaxArrPoints` | 65536 | breakpoints per track across all its arrangement lanes. 16× `kMaxClipAutoPoints`, for the same reason: a lane spans the whole set, not one loop. |
| `kMaxOverlapBeats` | 4 | §2.3. |
| `kMinArrBeats` | 1/64 | the shortest item. Below this an item is not grabbable in the editor at any zoom and cannot carry a fade; a trim that would go under it deletes instead (§2.5). It is the arrangement's `kMinNoteLen` (`project.cpp` uses 1/32 for a note) — finer, because an item may be a one-shot transient and a note may not. |

All are "a human cannot reach this by hand" numbers; hitting one is a bug report
rather than a limitation. `kMaxArrItems`, `kMaxArrNotes` and `kMaxArrLanes` are
all validated at the process boundary (§9.4), so changing them later is a
protocol change, not a constant change.

### 2.5 The repair pass

Every edit that moves, trims, drops or splits goes through one function:

```c++
// src/ui/session.h -- declared here, defined in src/ui/app_arrange.cpp.
//
// Restores §2.3's invariant on one track's lane and reports whether it changed
// anything. Idempotent: repair(repair(x)) == repair(x), which is what makes it
// safe to call after every edit and again after a load.
bool arrangeRepair(std::vector<ArrangeClip>& lane);
```

What it does, in order, and each step is a decision:

1. **Stable-sort by `start`.** Stable, so two items that genuinely begin on the
   same beat keep the order the user made them in rather than a coin toss.
2. **Delete items shorter than `kMinArrBeats`.** Including ones a trim just made
   short. The alternative — clamping up to the minimum — leaves a sliver the
   user did not ask for exactly where they were trying to remove one.
3. **Resolve overlaps, newest wins.** The item being edited is the newest
   statement, so it keeps its span and its neighbours give way:
   - a neighbour **entirely covered** is deleted;
   - a neighbour overlapped **at its tail** is shortened (`length` reduced);
   - a neighbour overlapped **at its head** is moved forward, and `offset` moves
     with `start` by the same number of beats — this is the whole of "trimming
     the front of a clip does not change which audio is under the rest of it",
     and getting it wrong is the classic arrangement-editor bug;
   - either trim that lands under `kMinArrBeats` deletes instead (step 2 again,
     which is why this function must be run to a fixed point).
4. **Admit the crossfade.** An overlap is *kept* rather than trimmed when it
   satisfies §2.3 rule 4 — which the two drag gestures that mean "crossfade"
   (dragging an item's head into its predecessor while holding the crossfade
   modifier, or dragging a fade handle past the neighbour's edge) set up by
   writing the fades before calling repair.
5. **Enforce rule 5.** If three consecutive items still violate `c.start >=
   a.end`, the *middle* one is deleted. That case can only arise from a
   hand-edited file or a hostile client; a user gesture cannot produce it,
   because step 3 runs first.
6. **Clamp fades.** `fadeIn + fadeOut <= length`, both `>= 0`.

The caller pattern is `arrangeRepair(t.arrange); publishArrangement(track);`,
and the second is skipped when the first returned false.

---

## 3. Engine: scheduling and playback

### 3.1 What does not change

Stated first, because it is the larger half. Voice rendering, warping, grain
scheduling, the declick envelope, note-off bookkeeping, the fx chain, delay
compensation, the mixdown loop, sends, returns, the master chain, meters,
`Engine::publish()` — none of it changes. An arrangement item starting calls the
same `startVoice` that `fireDue` calls for a queued session launch, at a
sub-block boundary computed by the same `consider`/`fireDue` loop, against the
same `RtClip` layout.

The engine's whole arrangement job is to answer, per track, "which clip should
be on the primary voice right now, and at what offset". That is a scheduler.

### 3.2 One allocation per track's lane

```c++
// src/audio/engine.h, beside RtClip.

// One placed item, as the audio thread sees it.
struct RtArrItem {
    f64 start  = 0.0;      // absolute timeline beats
    f64 length = 0.0;
    f64 offset = 0.0;      // clip-relative beat the item begins at
    f32 fadeIn = 0.f, fadeOut = 0.f;   // beats
    i32 fadeShape = 0;
    i32 clip = -1;         // index into RtArrangement::clips
};

// One track's lane, or -- for the cell addressed as track -1 -- the transport's
// loop brace (§3.6). ONE ALLOCATION, always:
//
//   [RtArrangement][RtArrItem[itemCount]][RtClip[clipCount]][RtNote[noteCount]]
//
// `items`, `clips` and every RtClip::notes inside it address memory in this
// same block, so the whole lane is one new[] and one delete[] and the
// retirement protocol has exactly ONE pointer to talk about. That is the
// RtAutoSet argument (AUTOMATION.md §2.2) extended by one more array: two
// allocations would need two retirement events or a rule about which one
// implies the other, and the RtNote protocol is only simple because there is
// one pointer per slot.
//
// A replaced lane travels back to the owner in Ev::ArrangementRetired before it
// may be freed.
struct RtArrangement {
    const RtArrItem* items = nullptr;
    const RtClip*    clips = nullptr;
    int itemCount = 0;
    int clipCount = 0;
    int noteCount = 0;                 // for the daemon's bounds arithmetic
    // Transport cell only (a = -1); zero on every track's lane.
    f64 loopStart = 0.0, loopEnd = 0.0;
    u32 loopOn = 0;
};
```

`Cmd::SetArrangement { a = track, p = const RtArrangement* }` — with
`a = -1` naming the transport cell, which is deliberately `Ev::ChainRetired`'s
own addressing (`engine.h`: `a = kMaxTracks + returnIdx`, `a = -1` for the
master chain) so that a reader who knows one knows the other. Null clears.

`Ev::ArrangementRetired { a = track, p = the RtArrangement* now safe to free }`,
pushed from inside `drainCommands()` when the displaced pointer differs from the
incoming one — the same "an entry that would never be announced must not be
queued" condition `publishNotes` documents and `publishAutos` inherits.

**Clip envelopes stay in separate `RtAutoSet` allocations.** An item's
`src.envelopes` are published exactly as a session clip's are: their own
one-allocation `RtAutoSet`, pointed at by `RtClip::autos`, retired through
`Ev::AutosRetired`. They are *not* folded into the lane block, and the reason is
a number: dragging one breakpoint would otherwise republish the lane, and the
lane is up to **1.6 MB of notes** (`kMaxArrNotes` × `sizeof(RtNote)`). A 60 Hz
drag would move 96 MB/s to change sixteen bytes. Separate allocations make the
same drag move one `RtAutoSet`.

The bookkeeping is keyed by **item uid, not item index**:

```c++
// src/ui/app.h, in the arrangement block.
struct ArrAutoPub { u64 itemUid = 0; const RtAutoSet* set = nullptr; };
std::vector<ArrAutoPub> publishedArrAutos_[kMaxTracks];
```

Keyed by uid because inserting an item renumbers every index after it, and a
retirement matched on a renumbered index frees the wrong set. Uids are stable by
construction (`Session::newUid()`, never reused).

### 3.3 Publishing, and the dedupe that makes §3.5 possible

```c++
// src/ui/app_arrange.cpp
const RtArrangement* App::buildArrangement(int track);
void  App::publishArrangement(int track);      // the RtNote protocol, verbatim
void  App::dropArrangement(const RtArrangement*);   // never reached the engine
```

`buildArrangement` walks `TrackModel::arrange` and:

1. **Dedupes payloads into `RtClip`s.** Two items whose `src` produces an
   identical `RtClip` share one entry in the `clips[]` array and two entries in
   `items[]`. Equality is on everything that reaches the wire: the sample
   pointer, `frames`, `channels`, `loopStart`, `loopEnd`, `clipBpm`,
   `lengthBeats`, `gain`, `warp`, `loop`, `isMidi`, the published `autos`
   pointer, the marker array, the transient array, and the *contents* of the
   note vector.
2. Emits one `RtArrItem` per model item, in lane order, with `clip` indexing the
   deduped array.
3. Copies each distinct clip's notes once into the block's `RtNote` tail and
   points that `RtClip::notes` at it.

The dedupe is not an optimisation; it is a **correctness precondition** for
§3.5. Splitting an item produces two model items each holding a full copy of the
`ClipModel` (§2 rule 1), so `RtClip` pointer equality — which is what the
continuation rule tests — can only ever hold if the publisher notices that two
copies are the same content. Without the dedupe, no split could ever satisfy
R3 and the headline gate would be unreachable.

The comparison is O(n) over a small `clips[]` (a lane rarely has more than a
handful of distinct payloads) with a cheap pre-filter on `(sample.get(),
noteCount, lengthBeats)`. It runs on the GUI thread, once per republish.

**It is also what makes `kMaxArrNotes` reachable.** 64 splits of a 10 000-note
clip cost 10 000 notes on the wire, not 640 000.

### 3.4 The scheduler

Engine-side state lives in a side table keyed by the `Engine*`, **exactly as
`AutoState`, `Pdc` and `WarpState` already do** (`src/audio/engine.cpp`), and
for exactly the reason those three give: `engine.h` is the daemon's contract and
does not thaw casually. Claimed in `prepare()`, read and written only by the
audio thread afterwards.

```c++
struct ArrTrack {
    const RtArrangement* arr = nullptr;
    int next    = 0;      // index of the next item that will start
    int playing = -1;     // index the primary voice is on, -1 = none
    int prev    = -1;     // index Track::prev is on, -1 = none
    bool override_ = false;   // §4
};
struct ArrState {
    ArrTrack t[kMaxTracks];
    f64 loopStart = 0.0, loopEnd = 0.0;
    bool loopOn = false;
};
```

**Cost, and it is the whole point:** O(1) per block and O(log n) per
discontinuity.

- Per block, per track: compare `beat_` against `items[next].start` and against
  `items[playing].end`. Two doubles. `next` only ever advances.
- Per discontinuity — a locate, a loop wrap, a Back-to-Arrangement — re-seek
  with a bisection over `items[].start`. That is O(log n), it is legal only
  because §2.3 rule 1 holds, and it is why the invariant is validated at the
  process boundary (§9.4) rather than assumed.

**Where it runs: a fourth `fireDue` step.** The sub-block loop in
`Engine::process()` (`engine.cpp:2548`) already splits the block at every
scheduled boundary and already has three producers of boundaries — a queued
launch or stop, a due follow action, a record start or stop. Arrangement item
starts and ends are the fourth, and the loop brace is a fifth (§3.6):

```
while (pos < n) {
    curBeat = beat_ + pos * bps;
    fireDue(curBeat);                       // + arrangement starts/ends
    nextB = blockEnd;
    for each track: consider(fireBeat), consider(recFireBeat)       // today
    for each track: consider(arrNextBoundary(as->t[ti], curBeat))   // NEW
    if (loopOn && loopEnd > curBeat) consider(loopEnd)              // NEW, §3.6
    renderRange / captureRange / captureMidiRange(pos, upto)
}
```

`arrNextBoundary` is the smaller of `items[next].start` and the end of the item
currently playing, or `kNoFollow` when neither exists. `fireDue`'s new step, per
track with a lane and without the override:

- **An item ended** (`beat_ >= items[playing].end`) and nothing starts here:
  release the primary voice exactly as a `Cmd::StopTrack` at a boundary does —
  `flushOffs` for a MIDI clip, `releasing = true`, the existing declick tail.
- **An item starts here**: if §3.5's continuation test passes, move `playing`
  and touch nothing else. Otherwise `startVoiceAt(t, clips[item.clip],
  item.offset)`, which hands the outgoing voice to `prev` the way `startVoice`
  already does — so a crossfade overlap is *already* the mechanism that exists.
- `next` advances past every item whose `start <= curBeat`. Items skipped
  entirely (a locate landed past them) are skipped, not fired.

**Seeking into a clip: `startVoiceAt`.** An item with `offset != 0` — every
second half of a split, and every item a locate lands in the middle of — must
start the voice *inside* the clip. `startVoice` seeds `srcPos = loopStart`;
`startVoiceAt(Track&, const RtClip&, f64 clipBeat)` is the same body with the
beat seeded. The warped path already exists and is reused verbatim
(`warpSrcAt(a.m, a.n, beat)`, `engine.cpp`'s marker block in `startVoice`); the
unwarped path is `loopStart + clipBeat / lengthBeats * (loopEnd - loopStart)`,
and the MIDI path is `beatPos = clipBeat` plus a bisection for `nextNote`.
`startVoice(t, c)` becomes `startVoiceAt(t, c, 0.0)`, which is why a session
launch stays bit-identical: it is the same function with the argument the old
one implied.

**Fades: two floats on `Voice`.**

```c++
struct Voice {
    // ... existing ...
    // Arrangement item fades. 1.0 in the ordinary case -- a session clip, an
    // item with no fades, an item past its fade regions -- and a session render
    // therefore takes the same arithmetic it takes today.
    f32 fade   = 1.f;    // multiplier at the start of the sub-block
    f32 fadeTo = 1.f;    // multiplier at its end
};
```

These are the one part of §3 that cannot live in a side table, and the reason is
specific: `startVoice` copies a `Voice` wholesale (`t.prev = t.voice`), so a
fade parked in a table keyed by track index would lose its association with the
voice at the exact moment the voice becomes the outgoing half of a crossfade —
which is the only moment a fade-out is interesting. They must travel with the
voice, so they are fields on the voice. This is the edit that makes `engine.h`
part of wave 8a (§12, question 1).

Applied in `renderRange`, at the same place `Voice::env` is applied and by the
same per-sample increment `env` already uses. The gate is
`fade == 1.f && fadeTo == 1.f`, which keeps the untouched path literally the
code it is today — the "the ordinary case must stay free" discipline the delay
compensation states for `comp == false` and the automation pass states for a
track with no lane. Note that the gate is a *performance* decision and not a
correctness one: multiplying by exactly `1.0f` is bit-exact in IEEE-754 for
every finite value, so the gated and ungated paths produce identical samples.
That is what lets the headline gate be **bit**-identity rather than a tolerance.

`fadeTo` is computed once per sub-block from the item's fade regions against the
sub-block's end beat, and the shape is applied to the *multiplier* (so
`fadeShape` 0 is a linear gain ramp), which is the same choice AUTOMATION.md
§3.2 makes for class-A automation: ramp the derived value, not the stored one.

### 3.5 R3 — continuation, not relaunch

> **R3.** Splitting an arrangement item must not change the sound.

A naive scheduler fails this immediately. A split makes two items; the second
item's start is a boundary; a boundary calls `startVoice`; `startVoice` resets
`srcPos`, zeroes `env` (so the voice re-attacks through its 3 ms declick ramp),
resets the grain phase, and hands the old voice to `prev` where it fades out.
The result is a click and a re-attack in the middle of a note. Sixty-four splits
would be sixty-four of them, and the headline gate would be dead on arrival.

**The rule.** At a boundary where item `out` ends and item `in` begins on the
same track, the voice **continues** — `playing` is reassigned and *nothing else
happens at all* — when all three hold:

1. `&clips[in.clip] == &clips[out.clip]` — pointer equality inside the block,
   which is what §3.3's dedupe exists to make achievable;
2. `|in.offset - (out.offset + (in.start - out.start))| <= kContinuityEps`
   (`1e-9` beats) — the incoming item resumes the source exactly where the
   outgoing one left it;
3. `out.fadeOut == 0 && in.fadeIn == 0` — neither side asked for a shape.

Otherwise it is an ordinary relaunch through `startVoiceAt`.

Each condition earns its place. (1) is identity of material. (2) is
contiguity — a second item at a *different* offset is a jump-cut and must sound
like one. (3) is intent: a fade is the user saying "put a shape here", and
honouring it means the two items are two events. The comparison in (2) is on
**beats and not frames**, so a warped clip continues if and only if its beat map
is continuous across the boundary, which is the correct question to ask of a
warped clip.

Because a continuation touches no voice state, the fact that a boundary happened
is unobservable in the output: the sub-block split at that beat still occurs (the
loop splits at every boundary regardless), and splitting a block does not change
the samples a voice renders — `renderRange(from, to)` is deterministic in
`(srcPos, phase)`, both of which carry across. **That is the argument the 64×
split gate tests.**

### 3.6 The timeline: `beat_`, locate, and the loop brace

**`beat_` becomes the timeline.** Today it is documented as "absolute beats
since transport start", and `Cmd::SetPlaying 0` rewinds it to 0
(`engine.cpp:1407`, `:1434`). That is coherent for a session-only DAW and
incoherent the moment there is a timeline: you cannot stop to fix a fill and
resume where you were.

> **Stop no longer rewinds.** `Cmd::SetPlaying 0` leaves `beat_` where it is.

Rewinding becomes an explicit gesture:

```c++
// a = 0 (reserved), x = the beat to go to.
Cmd::Locate,
```

`Cmd::Locate` does three things and deliberately not a fourth:

1. **Flush offs.** Every sounding note-off on every track is emitted at frame 0
   — `flushOffs`, the obligation the engine already discharges for every other
   discontinuity. A locate that left notes hanging would be the worst kind of
   bug: intermittent and silent until it is not.
2. **Re-seek the arrangement lanes.** Every `ArrTrack::next` is re-bisected
   against the new beat, and any item that *covers* the new beat starts
   immediately at the right offset via `startVoiceAt`. A locate into the middle
   of a clip lands in the middle of the clip, which is the whole reason
   `startVoiceAt` is not optional.
3. **Assign `beat_`**, it does not add to it. Sixty-four laps of a four-bar loop
   accumulate exactly zero drift, which is a testable property (§10).

And what it does **not** do: **it leaves session voices alone.** A session clip
is a loop a performer has launched and is playing; a locate is a statement about
the timeline, not about the performance. Live behaves this way and the
alternative is unusable on stage — moving the playhead to check a transition
would silence everything the performer had running. The same rule holds for the
loop wrap.

**The loop brace.** `Session::loopStart`/`loopEnd`/`loopOn` reach the engine
through the **transport cell of the arrangement table** (`Cmd::SetArrangement
{a = -1}`), and not through a second `f64` on `Command`.

That is worth stating as a decision because the obvious thing is
`Cmd::SetLoop{x = start, ...}` and there is no second `x`. `Command` has one
`f64`; a loop range is two numbers plus a flag. Widening `Command` grows every
message in a 1024-entry ring *and* `WireCommand`, which is 32 B, pointer-free
and load-bearing in the region layout hash — a protocol break for a feature that
already has a table to ride in. The arrangement table crosses anyway (§9), the
cell is already validated, and it is already idempotent, which makes
republish-after-engine-restart a `memcpy` exactly as `WireClip` does. One
mechanism, no new message.

At the brace:

- `loopEnd` joins `consider()`, so the block splits at it exactly;
- crossing it performs an **internal locate to `loopStart`**: the same three
  steps above, run from inside the sub-block loop. Not "wrap the cursors" —
  wrapping cursors while a voice keeps reading would leave that voice playing
  past its item's end for the rest of the lap.

`loopStart >= loopEnd` disables the loop rather than being clamped; a zero-length
loop is a request the engine cannot honour and clamping it would invent a length
the user did not ask for.

### 3.7 Retirement, verbatim

The `RtNote` protocol, said once more because saying "the same as `publishAutos`"
is the whole point:

```
App::publishArrangement(track)
    fresh = buildArrangement(track)                 // may be null
    old   = publishedArr_[track]
    if (!engine_.pushCommand({Cmd::SetArrangement, track, 0, 0.0, (void*)fresh}))
        { dropArrangement(fresh); return; }         // nobody borrowed it
    publishedArr_[track] = fresh
    if (old && old != fresh) retiringArr_.push_back(old)
```

- `Ev::ArrangementRetired` carries `p = the RtArrangement* now safe to free`.
- `pumpEngineEvents()` frees on receipt and refuses to free a pointer it has no
  record of owning, with the same `LOGW` and the same reasoning.
- Freed as `delete[] (char*)arr`, because the allocation is a `char[]` holding a
  placement-new'd `RtArrangement` followed by three arrays. The publisher and
  the reaper must agree on that; both live in `src/ui/app_arrange.cpp`, next to
  each other, with a comment saying why the cast is what it is — exactly as
  `buildAutos` and its reaper sit together in `app_engine.cpp`.
- The `RtAutoSet`s an item's envelopes published are retired *separately* and
  *first*: an item removed from the lane retires its autos through
  `Ev::AutosRetired` and the lane through `Ev::ArrangementRetired`, and the
  autos block outlives nothing (the lane block points at it, so the lane's
  `RtClip::autos` must not be dereferenced after the autos block is freed — hence
  the autos are freed only after the lane that named them has come home).

---

## 4. Session ↔ Arrangement

### 4.1 The behaviour to reproduce

Live's rule, exactly: launching a session clip on a track takes **that track**
out of the arrangement. The arrangement keeps running everywhere else, the
playhead keeps moving, and a "Back to Arrangement" button puts every overridden
track back at once. The overridden tracks are shown as such.

### 4.2 The override is engine-owned

```c++
// ArrState, §3.4
bool ArrTrack::override_;
// Published for the UI: bit i set == track i is overridden.
std::atomic<u32> Engine::arrOverride{0};
```

**Set at the quantized launch the engine computes**, in `fireDue`, at the
boundary — not in `drainCommands` when the command arrives, and emphatically not
in the GUI when the user clicks.

That is the whole argument for engine ownership, and it is the same shape as
§5's argument about the journal. The GUI asks for "launch clip 3"; the *engine*
decides which bar line that lands on, from `quantum_`, the clip's own
`quantumIdx`, and `beat_`. If the GUI set the flag at click time, the
arrangement on that track would go silent up to a whole bar before the session
clip started — an audible hole, in the one gesture a performer makes most. The
GUI does not own the clock, so it cannot own a flag whose meaning is "as of a
particular beat".

Rejected: the GUI mirrors the engine's decision by watching `Ev::ClipStarted`.
It is a round trip, so the hole is a frame or two instead of a bar — smaller and
still wrong — and it stops working when the GUI is slow or dead, which is a
state `docs/PROCESS-SPLIT.md` §4.3 explicitly designs for. The daemon must
answer this question with no GUI attached at all.

### 4.3 The rules, each with its alternative

| event | override |
|---|---|
| `Cmd::LaunchClip` on track *t* | **set** on *t*, at the boundary |
| `Cmd::LaunchScene` | **set** on every track the scene actually launches a clip on |
| `Cmd::StopTrack` on *t* | **KEPT** |
| transport stop | **KEPT** |
| `Cmd::Locate` | **KEPT** |
| `Cmd::BackToArrangement {a = track, or -1 for all}` | **cleared**, unquantized |
| the track's lane being cleared (`SetArrangement` with null) | cleared |

**`StopTrack` keeps it**, and this is the one people argue about. Stopping a
session clip means silence on that track — the performer stopped it to make
room. Having the arrangement leap back in under them is the surprise Live
avoids, and the recovery from the other choice ("stop the clip, then also stop
the arrangement") requires a second gesture that does not exist.

**Transport stop keeps it.** The flag is performance state; a stop is not a
statement about the arrangement. It also means the state the user left the set
in survives a stop/start, which is what a rehearsal loop is made of.

**`Locate` keeps it.** A locate is a timeline gesture; a track the performer put
in session mode stays in session mode. The alternative — a locate is a
"reset" — would make scrubbing the timeline silently undo the performance.

**Back to Arrangement is unquantized.** It is a corrective gesture, and a
correction that waits a bar is the wrong feel; Live's is unquantized too. At the
clear, the track's cursor re-seeks to `beat_` and starts whatever item covers it
*mid-item*, at the right offset — §3.6 step 2, which is the third caller of
`startVoiceAt` and the reason it is a general facility and not a split-specific
hack.

### 4.4 What the override gates

Two things, and the second is the evidence the flag is in the right place:

1. **The lane.** An overridden track fires no item starts. Its cursor still
   advances (`next` moves past items whose start passes), so a later Back to
   Arrangement lands correctly instead of replaying the set from wherever the
   override began.
2. **Arrangement automation.** §6's pass is applied only when
   `playing_ && !override_`. Which is exactly the point: the automation pass
   runs in the engine, on the audio thread, once per block, and needs a per-track
   answer to "is the arrangement in charge here". The flag is already there,
   already per track, already updated at the right instant. Had the override
   lived in the GUI it would have to cross the ring every block to be usable
   here — and that it does not is the argument that it lives where it belongs.

The published `arrOverride` bitmask exists for the UI only: the Back to
Arrangement button lights when it is non-zero, and an overridden lane is drawn
desaturated (§7.5).

---

## 5. Recording into the arrangement

### 5.1 The gesture

**ARR arm** (§7.6) lit, the transport rolling, one or more tracks record-armed:
what those tracks hear is written onto the timeline as it plays, and committed
into `ArrangeClip`s when the pass ends.

### 5.2 Why `Ev::ClipStarted` cannot be the record

The tempting design is to reuse the event stream that already reports what the
engine did. It fails three ways, and any one of them is fatal:

1. **Events drop.** `Ring<Event, 1024>` is bounded and lock-free; `push` returns
   false when full and the engine's ordinary `evts_.push` discards. Even the
   hardened path — `emitCritical` with its per-`Engine` parking buffer
   (`engine.cpp:98`) — is explicitly best-effort: the buffer has a `kCap` and a
   `dropped` counter, because parking is a bounded delay, not a guarantee. A
   channel that is *designed* to drop under load cannot be the thing a
   recording is made of.
2. **The event does not carry the information.** `Ev::ClipStopped` is pushed as
   `{Ev::ClipStopped, ti, 0, 0.0}` at four sites in `engine.cpp` — `b` is 0, not
   the slot, and `x` is **0.0, not a beat**. The stop event has never carried a
   position, because nothing has ever needed one from it.
3. **The GUI is not the clock.** Even a complete, position-carrying event stream
   is *stamped when the GUI pops it*, at frame cadence, with jitter that is
   unbounded when the compositor stalls. Under the process split it is stamped
   after a 1 ms pump hop on top of that. A recording timestamped by the reader
   is a recording of the reader.

### 5.3 A dedicated journal ring

```c++
// src/audio/engine.h
enum class JournalKind : u32 {
    None = 0, TakeStart, TakeEnd, ClipOn, ClipOff, NoteOn, NoteOff, Locate, LoopWrap
};

struct ArrJournal {            // 32 B, pointer-free, trivially copyable
    u32 kind  = 0;             // JournalKind
    u32 seq   = 0;             // monotonic per engine run; a gap means a drop
    i32 track = 0;
    i32 a     = 0;             // slot / pitch / velocity, per kind
    f64 beat  = 0.0;           // the ENGINE's beat, exact
};

class Engine {
    // Audio thread -> GUI thread, SPSC, the same lat::Ring the commands use.
    bool popJournal(ArrJournal& j) { return journal_.pop(j); }
    std::atomic<u32> journalDropped{0};
private:
    Ring<ArrJournal, 4096> journal_;
    u32 journalSeq_ = 0;       // audio thread only
};
```

- Every entry is stamped with the engine's own `beat_` at the sub-block boundary
  it happened on — the same number the scheduler used, not a number anyone
  inferred.
- `seq` increments on every *attempted* push. A consumer that sees `seq` jump
  knows exactly how many entries it lost and where.
- `journalDropped` counts refused pushes. It is the same information as the seq
  gap and it is published separately because a consumer that has not yet drained
  the ring can still read it.
- Capacity 4096. A dense performance — sixteen notes per beat across eight
  armed tracks at 140 BPM — is about 300 entries per second, so the ring holds
  roughly thirteen seconds of the worst case anyone plays against a GUI draining
  it sixty times a second. It is sized so that overflow means "the GUI stopped",
  not "the player played fast".

### 5.4 Refuse, do not commit short

> **A pass whose journal has a gap is REFUSED. The take is discarded, nothing is
> committed, and no undo point is taken.**

The alternative is committing what arrived. It is worse, and not marginally: a
recording silently missing four bars is indistinguishable from a performance
that had four bars of rest in it. The user does not know to do it again, and by
the time they find out, the take is the only take. Refusing loses the same four
bars *and says so*, in the status bar, with the drop count.

`commitTake()` therefore checks, over the span between the pass's `TakeStart`
and `TakeEnd`:

- every `seq` between them is present and contiguous;
- `Engine::journalDropped` did not change across the pass (read before
  `TakeStart` was consumed and after `TakeEnd`);
- under the split, the daemon's own `journalDropped` (§9.5) did not change
  either — there are two hops, and the check has to cover both.

On failure: discard, `LOGW`, and a status line naming the number of lost
entries. On success: build the clips, and take **one** undo point, **at commit**.

**One undo point, at commit**, because:

- a take *in flight* is not a state anyone wants to undo to — there is no
  coherent "half a recording", which is the same call `App::cancelTakes` already
  makes for session recording;
- an entry per journal entry would exhaust `kUndoDepth` (128) inside two bars;
- the entry is the whole session text (`App::UndoEntry`), so one entry taken at
  commit *is* the state before the take, exactly as AUTOMATION.md §5.4's
  gesture-scoped entry is the state before an automation pass.

### 5.5 What the GUI builds

- **MIDI take.** `NoteOn`/`NoteOff` pairs (matched per track and pitch, with
  unmatched ons closed at `TakeEnd`) become `NoteModel`s in one fresh
  `ClipModel` per track, with beats made clip-relative against the take's start.
  One `ArrangeClip` per track, `start` = the take's start beat, `length` = its
  span, `offset` = 0, `sourceUid` = 0 (this material came from nowhere).
- **Audio take.** The buffer path does not change: `Cmd::RecordSlot` already
  lends a GUI-owned buffer to the engine and hands it back through
  `Ev::RecordFinished` (`engine.h`). What the journal supplies is the *timeline*
  position that the buffer's frame 0 corresponds to — which `Ev::RecordStarted`
  reports today as `x = beat it began`, but only for one take and only at one
  instant. The journal's `TakeStart` carries the same number and, unlike the
  event, is ordered against every other thing that happened.
- **Loop wraps.** A `LoopWrap` entry inside a take means the pass crossed the
  brace. Overdub semantics onto an existing arrangement are §11; for wave 8 a
  wrap **ends the take** at the brace and commits it there, which is honest and
  is what a first version can defend.
- After building, `arrangeRepair` (§2.5) runs — a take that landed on top of
  existing material trims it, exactly as a drop does.

---

## 6. Arrangement automation

This is AUTOMATION.md §2.6 and §10's first deferred item, cashed in. That
section promised "a second publish path and a second evaluation site, and
nothing here has to move". This section is what it costs to make that true.

### 6.1 Model

`TrackModel::arrangeAutos` — `std::vector<AutoLane>`, unchanged type, with two
differences of meaning:

- **beats are absolute**, on the timeline, evaluated against `beat_`;
- **one lane per address per track**, not per clip.

`AutoLane`, `AutoPoint`, the address grammar, the value domain (target units +
`AutoXform`), the curve byte and the "address is text, resolution is GUI-side"
rule are all inherited verbatim. Nothing in `src/ui/session.h`'s automation
block changes.

### 6.2 Wire form: `RtAutoSetN`

```c++
// src/audio/engine.h, beside RtAutoSet.
//
// Same one-allocation layout, same retirement protocol, ONE difference: the
// lane array is variable-width and lives in the block rather than being a fixed
// member.
//
//   [RtAutoSetN][RtAutoLane[laneCount]][RtAutoPoint[pointCount]]
struct RtAutoSetN {
    const RtAutoPoint* points = nullptr;
    const RtAutoLane*  lanes  = nullptr;
    int laneCount  = 0;
    int pointCount = 0;
};

inline constexpr int kMaxRtArrLanes = 32;    // == kMaxArrLanes
```

`Cmd::SetTrackAutos { a = track, p = const RtAutoSetN* }`,
`Ev::TrackAutosRetired { a = track, p = ... }` — the names AUTOMATION.md §2.6
predicted, kept.

**Why not widen `RtAutoSet` to 32 lanes.** Two reasons, and the first is
decisive:

1. `RtAutoSet::lanes` is a **fixed array by value**, so its width is baked into
   `sizeof(RtAutoSet)` on both sides of the process boundary and into the
   region's layout hash. Widening it changes the size of every published clip
   envelope set — a shipped, validated, protocol-versioned shape — for a feature
   that clips do not use. A phase-4 and a phase-5 binary would already refuse
   each other (§9.1), but the churn would be in the wrong file for the wrong
   reason.
2. The right ceilings genuinely differ. Sixteen is right for a clip, which is
   *about* one or two parameters; a track's timeline accumulates every automated
   parameter in its chain over the length of a song, and thirty-two is the
   honest number there. Picking one constant for both would make one of them
   wrong.

The cost is one more pointer in the struct. The one-allocation property, which
is the thing that makes the retirement protocol simple, is unchanged.

### 6.3 One evaluator, still

`autoValueAt` refactors to a pointer form. This is a mechanical change with one
non-negotiable property attached:

```c++
// src/audio/engine.h
//
// THE evaluator. Both containers call it; so does the GUI, to draw the moving
// knob. AUTOMATION.md §2.4's claim -- "they cannot disagree if there is one
// function reading one set of points against one beat" -- is only true while
// this stays one function, so the two convenience overloads below are inline
// forwarders and must never grow a body of their own.
f32 autoValueAt(const RtAutoPoint* points, int pointCount, const RtAutoLane& lane,
                f64 beat, f32 fallback);

inline f32 autoValueAt(const RtAutoSet& s, const RtAutoLane& l, f64 b, f32 f) {
    return autoValueAt(s.points, s.pointCount, l, b, f);
}
inline f32 autoValueAt(const RtAutoSetN& s, const RtAutoLane& l, f64 b, f32 f) {
    return autoValueAt(s.points, s.pointCount, l, b, f);
}
```

Every existing call site compiles unchanged. The body — the window validation,
the `lo`/`hi` normalisation, the NaN handling, the bisection, the linear
interpolation, the hold-before-first and hold-after-last semantics — moves
verbatim. 7a's evaluator test table must pass against it untouched, which is the
gate (§10).

### 6.4 Application, and precedence by ordering

Two passes, in this order, both inside `Engine::process()` where the existing
one is:

```
drainCommands()
  -> live[] decision, scratch cleared
  -> ARRANGEMENT AUTOMATION PASS      <-- NEW, first
       for each track with a published RtAutoSetN, playing_ && !override_:
         b0 = beat_,  b1 = beat_ + n * bps          // absolute, no wrap
         class A: store (v0, v1) into autoA[ti]
         class B: setParamRT
  -> CLIP AUTOMATION PASS             <-- existing autoPass(false), UNCHANGED
       ... stores into the SAME autoA[ti], overwriting
  -> sub-block launch loop
  -> autoPass(true) for clips launched inside the block   // existing
  -> per-track post stage
```

> **Precedence: the clip envelope wins, and it is implemented purely as pass
> ordering.** No priority field, no per-lane arbitration, no merge rule. The
> second pass stores over the first. A parameter automated in both places ends
> the block with the clip's value because the clip's pass ran second.

**Why the clip wins.** The clip envelope is attached to the *material*: it
travels when the clip is dragged, it was drawn while the user was looking at
that clip, and it is the more local of two statements. An arrangement lane is a
statement about the timeline. When two statements about one value disagree, the
more local one wins — the same principle AUTOMATION.md §3.6 applies when the
user's hand overrides an envelope.

**Why ordering is enough.** Class A is a pair of floats per target in
`autoA[ti]`; a second store is a complete overwrite of the first. Class B is a
`setParamRT` call, and the clip pass's call is the later one in the same block,
so the plugin ends the block with the clip's value — which is exactly the
precedence rule CLAP's event list already implements for the GUI queue versus
the realtime array (AUTOMATION.md §3.4).

**Not gated on a voice.** A track with no clip playing still applies its
arrangement automation. That is the difference from a clip envelope and it is
the point: an arrangement lane says what the fader does at bar 33, whether or not
something happens to be sounding there. Gated on `playing_` and on
`!override_`, per §4.4.

### 6.5 The device-parameter hold, merged

Class B carries a restore obligation (AUTOMATION.md §3.5): the engine captured
what a parameter was before an envelope took it over and owes a write-back. With
two containers, one parameter can be claimed by both, and the existing
`AutoTrack::Hold hold[kMaxRtAutoLanes]` is indexed **by lane index**, which is
now ambiguous.

**Resolution: one merged hold table per track, keyed by the parameter and not by
the lane, with a claim mask.**

```c++
struct AutoTrack {
    const RtAutoSet*  set    = nullptr;    // the clip set, as today
    const RtAutoSetN* arrSet = nullptr;    // the track set
    u32 inert = 0;                         // clip lanes, as today
    u32 arrInert = 0;                      // track lanes
    struct Hold {
        i32 devSlot = -1, param = -1;
        f32 was = 0.f;
        u32 claims = 0;                    // bit0 = clip pass, bit1 = arrangement pass
    };
    Hold hold[kMaxRtAutoLanes + kMaxRtArrLanes];
    bool anyHold = false;
};
```

- A pass claiming `(devSlot, param)` finds or creates its entry, captures `was`
  with `getParam()` **only when `claims == 0`** (so the captured value is the
  user's, never the other pass's output), and sets its bit.
- A pass that stops applying clears its bit. The write-back happens when
  `claims` reaches 0 — and, as today, is skipped when the incoming set marks the
  parameter `kAutoOverridden`, because the user's hand is the newer statement.
- `autoRestore` takes a mask argument saying which pass is releasing.

This is the one place where the "second evaluation site costs nothing" promise
of AUTOMATION.md §2.6 is not quite true, and it is stated here rather than
discovered in 8d. It is ~30 lines and it is the price of a plugin parameter
having one storage slot.

### 6.6 Publishing

`buildArrangeAutos(track)` is `buildAutos(track, slot)` with three differences:
it reads `TrackModel::arrangeAutos`, it resolves against the track itself rather
than against a clip's track (so the scope check of AUTOMATION.md §4.2 step 2 is
trivially satisfied), and it emits a variable-width lane array. The
re-resolution triggers are identical and the rule is the same blunt one:

> **Whenever a track's chain is republished, republish that track's arrangement
> lanes** — alongside the envelopes of every clip on it, which is what the rule
> already says.

---

## 7. UI

### 7.1 A new file, on the piano roll's seam

`src/ui/arrange.{h,cpp}` — `class ArrangeView`, with the same contract
`PianoRoll` holds and for the same reason:

```c++
// What the arrangement editor is allowed to see. Built by
// App::drawArrangementView each frame. Model pointers and plain values only --
// no Engine, no Command, no RtClip, no PluginInstance -- which is what keeps
// this header compilable against app.h alone and the editor testable without a
// window, exactly as pianoroll.h states for the roll.
struct ArrangeContext {
    struct Lane {
        std::string name;
        int   colorIdx = 0;
        std::vector<ArrangeClip>* items  = nullptr;   // edited IN PLACE
        std::vector<AutoLane>*    autos  = nullptr;   // ditto
        f32*  height = nullptr;                       // TrackModel::arrHeight
        const AutoTargets* targets = nullptr;         // what its lanes may name
        bool  overridden = false;                     // Engine::arrOverride bit
        bool  armed = false;
        bool  expanded = false;                       // automation lanes shown
    };
    std::vector<Lane> lanes;
    f64*  loopStart = nullptr;
    f64*  loopEnd   = nullptr;
    bool* loopOn    = nullptr;
    f64   playhead  = 0.0;
    bool  playing   = false;
    int   sigNum    = 4;
    // The selected item, as a (track, item uid) pair -- uid and not index,
    // because an insert renumbers indices between frames and a stale index is a
    // wrong-clip edit. Owned by the caller so the detail panel can read it.
    int   selTrack = -1;
    u64   selItem  = 0;
};

class ArrangeView {
public:
    // Draws into `r`, handles every interaction inside it, and returns a mask of
    // what changed so the caller knows what to re-push and what to take an undo
    // point for.
    enum Changed : u32 { None = 0, Items = 1u << 0, Autos = 1u << 1,
                         Loop = 1u << 2, Layout = 1u << 3, Selection = 1u << 4 };
    u32 draw(Ui& ui, const Rect& r, ArrangeContext& ctx);
    const char* lastEdit() const;      // the caller's undo label
    // Headless hooks; see §7.7.
    void selectItem(int track, u64 uid);
};
```

The caller (`App::drawArrangementView`, `app_chrome.cpp`) owns everything the
view does not: building the context, `arrangeRepair`, `publishArrangement`,
`publishArrangeAutos`, the undo point, and the transport commands the ruler
generates. That division is exactly `PianoRoll`'s and it is why the roll is as
short as it is.

### 7.2 `src/ui/timeaxis.h`

`TimeAxis`, `beatToX`, `xToBeat` and `zoomView` currently live in
`pianoroll.cpp`'s anonymous namespace (lines 61–76). They move, verbatim, into a
new header both editors include, together with the constants that are about
*time*: `kGridStep`, `kZoomMin`, `kZoomMax`, `kZoomPerNotch`, `kPxPerBeatMin`,
`kPxPerBeatMax`, and a shared ruler-drawing helper.

What stays in `pianoroll.cpp`: `kKeyW`, `kRulerH`, `kLaneH`, `kRowH`,
`kMinFoldRows`, `kCentrePitch`, `PitchAxis`, `RowMap` — everything that is about
a piano roll rather than about a timeline.

**Extraction, not duplication**, for two reasons:

1. **Two implementations drift, and the drift is invisible.** The bug it
   produces is a note and a clip disagreeing about where beat 12 is, at some
   zooms, after some scrolls — which is the hardest class of bug this codebase
   could grow, because both halves look right in isolation.
2. **Ctrl+wheel must feel identical in both.** `zoomView`'s anchoring — "the
   beat under the cursor stays under the cursor, clamped at the content edges" —
   is a feel decision with an off-by-a-clamp failure mode. Two copies would be
   two feels in one program.

The mechanical cost: the functions leave an anonymous namespace, so they become
`inline` in `namespace lat`. Nothing else about them changes, and
`pianoroll.cpp` must produce byte-identical rendering after the move — which is
checkable, because the move is a move.

Rejected: templating `PianoRoll` over an axis policy, and making `PianoRoll`
draw the arrangement with the note grid swapped out. Both are larger than the
duplication they avoid, and the second one confuses two editors whose selection
models, undo labels and drag verbs genuinely differ.

### 7.3 `AutoLaneView`, extracted at last

AUTOMATION.md §6.5 specified `src/ui/autolane.{h,cpp}` holding
`class AutoLaneView`, and argued for it in advance: *"an audio clip has no piano
roll and wants exactly this lane under its waveform, and a lane that was born
inside the roll would have to be extracted then, with a live editor's state to
carry across."*

**That extraction did not happen.** The lane shipped inside `pianoroll.cpp` —
the pure helpers at lines 426–601 (`ValAxis`, `ptLess`/`samePt`, `PtKeys`,
`sortTrackingPts`, `PtDelta`/`clampPtDelta`/`applyPtDelta`, `ptScreen`,
`pointAt`, `pointsInBand`, `insertPoint`) and `PianoRoll::drawLaneKey` plus the
lane's slice of `PianoRoll::draw`, with its state on `PianoRoll` (`psel_`,
`laneSel_`, `targetSel_`, `pendingLane_`, `laneLo_`/`laneHi_`, `dragPt_`,
`bandVal_`). So decision 11's "`AutoLaneView` reused for expandable lanes"
references a class that does not exist yet.

**Resolution: 8e does the extraction AUTOMATION.md already argued for, as a
move.** `src/ui/autolane.{h,cpp}` gets `class AutoLaneView` with

```c++
class AutoLaneView {
public:
    // Draws one automation lane against a caller-supplied time axis, so the lane
    // and whatever is above it can never disagree about where a beat is.
    // `beatBase` is what the lane's point beats are relative to: 0 for a clip
    // envelope (clip-relative) and 0 for an arrangement lane (already absolute)
    // -- the parameter exists so a future clip-lane-on-the-timeline can offset.
    bool draw(Ui& ui, const Rect& r, std::vector<AutoPoint>& pts,
              const TimeAxis& ta, f32 lo, f32 hi, const char* unit, f32 def,
              bool enabled, bool inert, f64 lengthBeats, f64 beatBase);
    bool nudgeSelected(std::vector<AutoPoint>&, int gridSteps, f32 valueSteps);
    bool deleteSelected(std::vector<AutoPoint>&);
    bool clearSelection();
    bool hasSelection() const;
private:
    IndexSel sel_;
    // ... the drag/band state that is today PianoRoll's psel_/dragPt_/bandVal_
};
```

`PianoRoll` becomes its first caller and keeps its chooser (`drawLaneKey`,
which is roll-specific: it is what *adds* a lane to a clip). `ArrangeView`
becomes its second. Because it is a move, the roll's behaviour must be
unchanged, and that is what the 8e gate asserts.

### 7.4 Layout

```
┌─ ruler: bar numbers · loop brace · playhead ────────────────────────────┐
├──────────┬──────────────────────────────────────────────────────────────┤
│ ▾ Bass  ▮│  ▓▓▓▓▓▓▓▓▓╲___╱▓▓▓▓▓▓▓▓   ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓                  │
│  t:3/vol │  ╭─────╮                                                     │  <- AutoLaneView
│          │ ╱       ╲__________________                                  │
├──────────┼──────────────────────────────────────────────────────────────┤
│ ▸ Drums ▮│  ▓▓▓▓ ▓▓▓▓ ▓▓▓▓ ▓▓▓▓ ▓▓▓▓ ▓▓▓▓ ▓▓▓▓ ▓▓▓▓                     │
├──────────┴──────────────────────────────────────────────────────────────┤
```

- A **header column** on the left, `kArrHeaderW` wide, carrying the track name,
  its colour chip, the arm indicator, an override tint, and the disclosure
  triangle. Scrolls vertically with the lanes, never horizontally — the same
  relationship `drawTrackHeaders` has with the clip grid.
- One **clip lane** per track, `TrackModel::arrHeight` tall, resized by dragging
  its bottom edge. Per track, because tracks are not equally interesting and a
  single global height means either wasted space or an unreadable lane.
- Zero or more **automation lanes** under it when the track is expanded, each
  `kArrAutoLaneH` tall (a fixed 44 logical px — an automation lane needs enough
  height to aim at and no more), each an `AutoLaneView` sharing the
  arrangement's `TimeAxis`.
- An item draws as a rounded rect in the track's colour, with its name, its
  waveform or its note stems drawn against the shared axis, and its fades as
  filled triangles at the corners with a draggable midpoint for `fadeShape`.
- An item on an **overridden** track draws desaturated, which is the same visual
  grammar an overridden automation lane already uses (AUTOMATION.md §6.3).

### 7.5 Interaction

Deliberately the same verbs the roll uses, because a user who has learned the
note grid should not have to learn a second editor one panel up.

| gesture | effect |
|---|---|
| click an item | select it; the detail panel follows (§7.6) |
| drag an item body | move in time and across tracks; grid-quantized |
| Shift+drag | unquantized |
| Ctrl+drag | duplicate |
| drag the left edge | trim the head — `start` and `offset` move **together** |
| drag the right edge | trim the tail — `length` only |
| drag a top corner | `fadeIn` / `fadeOut` |
| drag a fade's midpoint | `fadeShape` |
| Ctrl+E, or double-click | split at the grid-quantized cursor beat |
| right-click, or Delete | delete the selection |
| Shift+drag from empty space | rubber-band select, adding to the set |
| drag in the ruler | set the loop brace; a plain click locates |
| drag a lane's bottom edge | `arrHeight` |
| click the disclosure triangle | expand / collapse the automation lanes |
| wheel / Shift+wheel / Ctrl+wheel | vertical / horizontal / zoom, through `timeaxis.h` |

Every gesture that changes the model calls `arrangeRepair` before republishing,
and takes one undo point per gesture through `undoPointWith(..., gestureId)` —
the coalescing that already gives a fader drag one entry.

### 7.6 The detail panel, un-gated

`App::frame` currently draws the detail panel only in Session view:

```c++
if (showDetail_ && view_ == MainView::Session) { ... }     // src/ui/app.cpp:233, :247
```

That condition goes. In Arrangement view the CLIP tab shows the selected
`ArrangeClip::src` — the piano roll for a MIDI item, the waveform-plus-lane
editor for an audio one — and edits it **in place**, exactly as it edits a
session clip.

This is Rule 1 paying for itself. Because `src` is by value, the roll editing
"the clip" edits precisely the one item the user selected, with no possibility
of the edit leaking to another placement, and with no code in the roll that
knows an arrangement exists.

The DEVICES tab is unchanged and follows `devOwner_` as it does today.

Republish cost, stated honestly: an edit inside an item republishes that track's
whole lane, which is up to 1.6 MB. Coalesced to at most once per frame, that is
up to ~96 MB/s of `memcpy` **while dragging a note inside an arrangement clip on
a track carrying 65 000 notes** — a corner of a corner, measurable and not
fatal. The escape hatch, if it ever matters, is a dirty-item republish that
reuses the block when only note contents changed and the layout did not; it is
§11, not wave 8.

### 7.7 The ARR chip, and two headless hooks

**ARR arm** is a **third independent chip** on the control bar, beside REC (the
session record intent) and AUTO (the automation arm), at `uiId(1, 12)`,
immediately right of AUTO and before the position readout — drawn in AUTO's
style (accent on dark, because it is a *mode* the transport row reports and not
a transport action), with the tip "Arrangement arm: record armed tracks onto the
timeline".

Rejected: folding it into REC. "Record into the session grid" and "record onto
the timeline" are different destinations, and one button would have to pick
between them from `view_` — which means the same click does two different things
depending on which tab is open. That is the surprise AUTOMATION.md §5.1 refused
when it made the automation arm its own control, and it is refused here for the
same reason.

**Two headless hooks, in the `NXTAKT_DEBUG_*` shape** (`debugUndoSelfTest`,
`NXTAKT_DEBUG_ADDFX`, `NXTAKT_DEBUG_AUTOLANE`, `NXTAKT_DEBUG_MIDIMAP`), because
nothing inside gamescope can drag a clip and this is the part a screenshot
cannot check:

- **`NXTAKT_DEBUG_ARRANGE=<track>`** — once per run, on the first Arrangement
  frame: seeds the named track's lane from its slot-0 clip with a scripted
  figure that exercises every case at once — four items, one of them split, one
  crossfade pair at exactly `kMaxOverlapBeats`, one fade-in, one item at a
  non-zero `offset` — switches `view_` to Arrangement, expands the track, and
  puts the selection on item 1 so the detail panel has something to draw. What a
  screenshot then checks is layout, colour, fades and the detail panel; what the
  *log line* it prints checks is that `arrangeRepair` left the invariant intact.
- **`NXTAKT_DEBUG_ARRRENDER=<path>`** — the headline gate, runnable with no
  window and no mouse: renders the seeded arrangement offline through the
  existing headless harness, renders the equivalent scripted session performance
  (the same launches, queued at the same beats), compares the two buffers byte
  for byte, writes both to `<path>.arr.wav` / `<path>.ses.wav` and prints a
  verdict line with the first differing sample index. This is the one hook that
  makes the wave's acceptance criterion something a CI box can answer.

---

## 8. Persistence: format v6

### 8.1 The shape

```
nxtakt 6
tempo 120
sig 4 4
quantum 4
metronome 0
nextuid 240
name Sketch
loop 4 20
loopon 1
track 0
  uid 3
  name Bass
  ...
  device
    ...
  enddevice
  clip 0
    uid 31
    kind midi
    ...
  endclip
  arrangement
    arrheight 96
    aclip
      uid 91
      at 0
      len 8
      off 0
      fadein 0.25
      source 31
      kind midi
      name Bassline
      color 2
      gain 1
      loop 1
      beats 4
      quantum -1
      note 0 0.25 36 100
      env t:3/vol
        pt 0 0.85
        pt 3.5 0.2
      endenv
    endaclip
    aclip
      uid 92
      at 8
      len 8
      off 4
      fadein 0.25
      fadeout 0.25
      fadeshape 1
      source 31
      ...
    endaclip
    autolane t:3/dev:12/p:3
      pt 0 200
      pt 16 4000
    endautolane
  endarrangement
endtrack
scene 0
  ...
endscene
```

### 8.2 The grammar decisions

**`arrangement` is a block inside a track, after the clips.** Everything in it
is per-track, and a top-level block would have to re-state which track it
belongs to — a second way of naming a track, which is a second way to get it
wrong. Position within the track: after `clip`, for the same reason notes come
after a clip's scalars — a block reads header-then-content, and a track's mixer
settings should not be below two hundred lines of timeline.

**`loop` and `loopon` are top level**, beside `tempo` and `quantum`, because
there is one timeline and one brace. `loop <start> <end>` on one line because it
is one range; `loopon <0|1>` separate because a disabled brace still remembers
where it was, which is what makes toggling it useful. Both **sparse**: `loopon 0`
and a default range write nothing.

**`aclip` carries no index.** It is positional, exactly like `device`, and for
exactly the reason `writeDevice`'s comment gives: *"Blocks are positional, so no
index is written — load order is chain order."* Here load order is timeline
order (after §8.5's sort). Contrast `clip <idx>`, `return <idx>` and `scene
<idx>`, which index into fixed arrays and therefore must say which slot they
mean. The arrangement is a list. An index on a list is a second statement of the
ordering and therefore a second place for it to disagree with the first.

**The item's own fields**, all after `uid` and before the clip body:

| key | meaning | sparse? |
|---|---|---|
| `at <beats>` | `start` | no — every item has one, and a missing `at` silently meaning 0 is exactly the failure a required field prevents |
| `len <beats>` | `length` | no, same |
| `off <beats>` | `offset` | yes, omitted at 0 |
| `fadein <beats>` | `fadeIn` | yes, omitted at 0 |
| `fadeout <beats>` | `fadeOut` | yes, omitted at 0 |
| `fadeshape <n>` | `fadeShape` | yes, omitted at 0 |
| `source <uid>` | `sourceUid` | yes, omitted at 0 |

`off` inside an `aclip` means *offset*; `off` inside an `env` block means *this
lane is deactivated*. They never collide, because the parser is in different
states (`St::AClip` versus `St::Env`) and `off` is not a legal key in the other
— but a human reading a file sees two `off`s meaning two things, which is worth
one comment in `project.cpp` and one line in §12's confirmation request.

**`autolane <address>` … `endautolane`**, with `pt <beat> <value> [curve]` and
`off` lines identical to `env`'s. Not spelled `env`, because these lanes are
absolute-timeline and a reader meeting `env` at arrangement scope would have to
infer which beat space it was in from context. Two names, two meanings, no
inference.

**`arrheight <px>`**, sparse, omitted at the default.

### 8.3 One shared clip body

> The `aclip` body is written by **one shared `writeClipBody`** and read by one
> shared key handler.

```c++
// project.cpp
void writeClipBody(std::string& o, const ClipModel& c, const char* indent);
// true when `key` was a clip-body key and was consumed.
bool clipBodyKey(ClipModel& c, const std::string& key, const std::string& rest,
                 Scan& sc, ClipReadState& crs, St& st, std::string* err);
```

`writeClip` becomes "`clip <idx>`, `uid`, `writeClipBody`, `endclip`" and
`writeAClip` becomes "`aclip`, `uid`, the seven item fields, `writeClipBody`,
`endaclip`". Likewise `St::Clip` and `St::AClip` both dispatch through
`clipBodyKey` and handle only their own keys and their own terminator.

This is not tidiness. It is what makes an `aclip`'s payload **provably** the
same grammar as a `clip`'s — including `env` blocks, including the kind gating,
including the "a midi clip cannot have a `file` line" check at the terminator —
rather than a second copy that agrees today and drifts at the next format
addition. `project.cpp`'s governing comment says there is deliberately one
parser; two clip bodies would be one and a half.

One consequence falls out and is worth naming because it is easy to miss:
`St::Env` is now reachable from **two** states, so it needs the same
"remember what to close back to" trick `St::Device` already uses (`devPrev`,
set by `openDevice`). Call it `envPrev`.

`fadeShape` is clamped with `clFadeShape`, which is `clCurve` verbatim: the byte
is **preserved, not normalized**, for the reason `clCurve`'s comment spells out
at length — a newer build writes `fadeshape 3`, this build must load it, draw
the fade straight, and hand the 3 back rather than silently flattening somebody's
curve into a file that can never say so again.

### 8.4 Clamps

Symmetric on save and load, as everywhere in `project.cpp`:

```c++
f64 clArrBeat(f64 v)  { return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }
f64 clArrLen(f64 v)   { return std::isfinite(v) ? clampv(v, kMinArrBeats, 1e7) : 1.0; }
f64 clFade(f64 v)     { return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }
u8  clFadeShape(i64 v){ return (u8)clampv(v, (i64)0, (i64)255); }   // == clCurve
```

And the structure/value split, unchanged from every other line in the file:

- **Structure is rejected.** An `aclip` with no `at`, an `autolane` whose
  address is malformed (`validAddress`, reused verbatim), an `arrangement` block
  outside a track, a key that belongs to no state.
- **Values are clamped.** A negative `at`, a `len` of NaN, a `fadeshape` of 900.
- **Dangling is kept.** `source 31` naming a clip that no longer exists is
  written back unchanged forever, exactly as a dangling parameter address is.
  This is `sourceUid`'s entire contract, and it is `PARAM-ADDRESS.md`'s
  "dangling addresses resolve to nothing and must fail soft" applied to a
  provenance field.

**No count caps in the parser.** `kMaxArrItems`, `kMaxArrNotes`, `kMaxArrLanes`
and `kMaxArrPoints` belong to the editor and the publisher, never to the reader
— the same call `device` makes ("No cap is imposed here — silently dropping the
tail of a user's chain at load time would be the worse failure") and the same
call `St::Env` makes for lanes and points.

### 8.5 Version discipline, and the one exception to byte-identity

`kFormatVersion` 5 → **6**. `kMinFormatVersion` stays 1. Still one parser:
`arrangement`, `loop` and `loopon` are new keys with defaults, so a v1..v5 file
loads with an empty arrangement and a disabled brace.

> **The v5 → v6 no-arrangement diff is exactly the header line.**

Every construct is sparse: no items → no `arrangement` block; no track lanes →
no `autolane`; default height → no `arrheight`; brace off at the default range →
no `loop`, no `loopon`. That property is asserted by a test, not assumed — the
four previous version bumps each state it explicitly and this one must too.

> **One stated exception: `adoptSession` sorts `aclip` order on load.**

Everywhere else in this format the writer emits the order it is given and the
reader preserves the order it finds, so a hand-shuffled file round-trips
byte-identically and merely loads as a session whose vectors are unsorted
(`writeClip`'s note comment, §7.3 of AUTOMATION.md for lanes and points). The
arrangement cannot afford that, because the sorted order is not cosmetic:

- the engine's cursor is O(1) *because* the lane is sorted (§3.4);
- the overlap invariant is stated in terms of **adjacency** (§2.3), so it is
  meaningless on an unsorted vector;
- the daemon validates the invariant before handing a pointer to the engine
  (§9.4), so an unsorted lane is a refused blob, not merely an ugly one.

So the sort happens — and it happens in **`App::adoptSession`, not in the
parser**, which is exactly where `project.cpp`'s own comment says it belongs:
*"If that ever becomes a requirement it belongs in the App, next to the editing
code that upholds the invariant, and not here."* `adoptSession` already runs on
both a load and an undo restore, which is precisely the set of paths that
introduce a Session from text.

The consequence, stated plainly rather than discovered: **a hand-shuffled
`arrangement` block does not round-trip byte-identically; it round-trips
sorted.** Sorted input is identity. Shuffled input is idempotent after one pass.
Both are asserted (§10, 8a's gate).

### 8.6 Parser states

`St::Arrange` (from `St::Track`, closing back to it), `St::AClip` (from
`St::Arrange`), `St::AutoLane` (from `St::Arrange`), and `St::Env` gaining a
second entry point (§8.3). An unexpected key inside any of them fails the load,
like every other block in the file.

---

## 9. Daemon composition

### 9.1 Versions

**Protocol version 4 → 5. Pool version 3 → 4.** (Automation took protocol 4 /
pool 3 in AUTOMATION.md §8.3; this wave takes the next of each.) `kShmVersion`
moves only if `SharedStateT` grows — it does, by the published `arrOverride`
bitmask and the journal counters, so it moves too. The control region gains a
section (§9.5) and its layout hash therefore moves regardless, so a phase-4
binary and a phase-5 binary refuse each other at `attach()` with a specific
message. That is the mechanism working as designed, and the `static_assert`s on
the wire sizes must move in the same commit — they exist precisely so this
cannot happen by accident.

### 9.2 Crossing: a pool block

```
PoolKindArrangement = 5           // pool version 4
```

One blob per track, allocated and written by the client, referenced by
`Cmd::SetArrangement { a = track, ref = poolOffset }`. Everything AUTOMATION.md
§8.2 says about strings and envelopes riding the pool applies verbatim: one
validator, no fixed budget, and free-after-confirm with no new states.

```c++
struct WireArrHeader { i64 itemCount, clipCount, noteCount; f64 loopStart, loopEnd; u32 loopOn, pad; };
struct WireArrItem   { f64 start, length, offset; f32 fadeIn, fadeOut; i32 fadeShape, clip; };
// WireClip as it already is, per clip in the blob, with sampleRef/notesRef/autoRef.
```

Layout: `[WireArrHeader][WireArrItem[]][WireClip[]]`. The notes are **not** in
the blob — each `WireClip` names its own `notesRef` into the pool, exactly as a
session clip's does today, so the existing `WireNote` reinterpretation and the
existing per-block retirement both keep working unchanged.

### 9.3 Translated, not reinterpreted

This is the one structural difference from every other pooled payload and it
must be stated, because getting it wrong is a security bug rather than a
correctness bug:

> **A `WireNote` blob is *reinterpreted*: `WireNote` mirrors `RtNote` field for
> field and holds no pointers, so `(const RtNote*)(poolBase + ref)` is honest
> and a 10 000-note clip costs nothing at the boundary. An arrangement blob
> cannot be. `RtClip` holds five pointers** — `data`, `notes`, `autos`,
> `markers`, `transients` — **and a pointer in a client-writable region is a
> pointer the client chose.**

So the daemon **builds**. `translateArrangement(track, ref)`:

1. validates the blob (§9.4);
2. allocates its own `[RtArrangement][RtArrItem[]][RtClip[]]` on the daemon
   heap — note the block has no `RtNote` tail, because the notes stay in the
   pool;
3. for each `WireClip`, runs the **existing `translateClip()`** — every check in
   PROCESS-SPLIT §10.5's table, unchanged — to produce an `RtClip` whose
   pointers are `poolBase + validated offset` and nothing else;
4. copies the items across, bounds-checking `clip` against `clipCount`;
5. hands the built pointer to `Engine::pushCommand(Cmd::SetArrangement)`.

`translateArrangement` is therefore `translateClip` in a loop plus §9.4's
structural checks, which is a small amount of new code guarding a large amount
of reviewed code — the right ratio.

### 9.4 Validation

One new reject reason, `RejectBadArrangement`, and the whole-blob refusal
discipline of `RejectBadClip`: a client that sent something impossible is told,
not partially obeyed.

| check | catches |
|---|---|
| `poolValidate(ref, PoolKindArrangement, needBytes)` with `needBytes` from the **declared** counts | a blob smaller than its own declared contents, caught before a pointer exists |
| `itemCount <= kMaxArrItems`, `clipCount <= itemCount`, `noteCount <= kMaxArrNotes` | absurd counts |
| every item: `start`/`length`/`offset` finite, `length >= kMinArrBeats`, `fadeIn >= 0`, `fadeOut >= 0`, `fadeIn + fadeOut <= length`, `clip` in `[0, clipCount)` | wild scalars and an out-of-range clip index |
| **items sorted by `start`** | an unsorted lane, which makes the engine's bisection read outside the array |
| **`c.start >= a.end` for every consecutive triple** | more than two simultaneous items, i.e. a third voice the engine does not have |
| **every overlap `<= kMaxOverlapBeats` and covered by both fades** | §2.3 rule 4 |
| each `WireClip`: the entire §10.5 table, verbatim | everything that table already catches |
| the transport cell: `loopStart`, `loopEnd` finite and `>= 0`; `loopEnd > loopStart` or `loopOn == 0` | a zero-length or inverted brace, which would make the internal locate a spin |

The three bolded rows are the ones that would not exist if the invariant were
merely an editor convention. They are here because the **engine's per-block cost
is O(1) only while the invariant holds**, and the engine is downstream of an
untrusted process. It is exactly the argument PROCESS-SPLIT §10.5 makes for
`loopEnd`: *"a wild `loopEnd` walks `fetch()` off the end just as effectively as
a wild offset."*

### 9.5 Two-layer retirement

Two owners, two layers, one proof:

- **Layer 1, the daemon's built block.** Freed when the engine has provably
  drained past the replacement. Two accepted proofs, either sufficient, exactly
  as chains already have: `Ev::ArrangementRetired` naming the exact pointer
  (pushed from *inside* `drainCommands()`, so its arrival **is** the drain, and
  it beats the counter by up to one block), or `drains >= k + 2` with `k` read
  immediately after `pushCommand` returned true (PROCESS-SPLIT §11.5, whose
  argument for the 2 is unchanged and is not re-derived here).
- **Layer 2, the pool blocks the blob referenced.** Echoed to the client as
  `EvBlockRetired` per block, which is the existing mechanism and needs nothing
  new.

**Ordering between the two layers is not optional.** The built `RtClip::notes`,
`RtClip::data` and `RtClip::autos` all point **into the pool**, so a pool block
outlives the built block that names it. Therefore the pool echoes are **queued
behind** the built-block free: one proof gates both, and `EvBlockRetired` is
emitted only after the built block has been freed. Echoing earlier would tell
the client it may reuse memory the daemon's own struct still points at — a
use-after-free with the free happening in another process, which is the worst
shape this bug class comes in.

The daemon's shadow bookkeeping mirrors §10.3's rule for cells: a pool block is
retiring when **no** built arrangement and **no** clip-table cell still names it.
A sample legitimately backs a session clip and six arrangement items at once, and
losing one of the seven is not a retirement.

### 9.6 The journal on its own ring

```
ShmSpscRing<WireJournal, 4096>       // a NINTH control-region section
```

Written by the daemon's **pump** (draining `Engine::popJournal` at its 1 ms
cadence), read by the client. `WireJournal` is `ArrJournal` field for field, 32 B,
pointer-free, `static_assert`ed to mirror.

**Not the event ring.** Mixing them lets a burst of journal entries evict
events, and events carry `EvClipAck` — a lost ack wedges a clip-table cell for
the rest of the session (§10.4). The two channels have different failure
budgets: an event must not be lost, a journal entry must be *known* to be lost.

**Not the pool.** The journal is fixed-size, high-rate and continuous, which is
what a ring is for and what an allocate-write-publish-retire lifecycle is
emphatically not.

There are now **two hops** where an entry can be lost — the engine's ring into
the pump, the pump's ring into the client — so the daemon publishes its own
`ControlHeader::journalDropped` beside the engine's mirrored count, and the
client's contiguity check (§5.4) runs on the **engine's** `seq`, which covers
both hops by construction.

### 9.7 Why 8g runs concurrently with everything

> **8g touches neither `src/audio` nor `src/ui`.**

Every one of the three previous daemon phases makes the same claim and keeps it,
and this one keeps it for the same reason: the daemon builds against the *wire
shapes* and the *format*, not against the engine's internals. What it needs from
the rest of wave 8 is exactly the definitions of `RtArrangement`, `RtArrItem`,
`RtAutoSetN`, `Ev::ArrangementRetired` and the new `Cmd`s — all of which land in
`engine.h` in **8a**, compiled and unused, precisely as 7a landed the Rt
automation structs before anything evaluated them.

So 8g starts the moment 8a is merged and runs beside 8b/8c and 8d without a
single shared line. That is the whole reason 8a is called the wave's
serialization point.

---

## 10. Phasing, file ownership and test strategy

Seven milestones. 8a is a barrier; after it, three run concurrently; then two in
sequence.

```
        8a  (model, format, header types -- THE SERIALIZATION POINT)
         │
    ┌────┼──────────────┬─────────────────┐
 {8b+8c} │             8d                8g
 engine  │      arrangement automation   daemon        (concurrent)
    └────┼──────────────┴─────────────────┘
        8e  (UI)
         │
        8f  (recording / journal)
```

### 10.1 File ownership

**Single-owner rule:** a file with an owner in a column is written by that
milestone alone for its duration. Two milestones in one row that are not
concurrent are sequential and therefore fine.

| file | 8a | 8b+8c | 8d | 8e | 8f | 8g |
|---|---|---|---|---|---|---|
| `src/ui/session.h` | **own** | | | | | |
| `src/core/project.cpp` | **own** | | | | | |
| `src/audio/engine.h` | **own** | | | | | |
| `src/audio/engine.cpp` | | **own** | | | **own** (after) | |
| `src/ui/app.h` | append block | | append block | append block | append block | |
| `src/ui/app_engine.cpp` | **own** | | | **own** (after) | **own** (after) | |
| `src/ui/app_arrange.cpp` *(new)* | | | **own** | | | |
| `src/ui/arrange.{h,cpp}` *(new)* | | | | **own** | | |
| `src/ui/timeaxis.h` *(new)* | | | | **own** | | |
| `src/ui/autolane.{h,cpp}` *(new)* | | | | **own** | | |
| `src/ui/pianoroll.{h,cpp}` | | | | **own** | | |
| `src/ui/app.cpp` | | **own** | | **own** (after) | | |
| `src/ui/app_chrome.cpp` | | **own** | | **own** (after) | **own** (after) | |
| `src/ui/app_detail.cpp` | | | | **own** | | |
| `src/daemon/nxtaktd.cpp`, `src/ipc/*.h` | | | | | | **own** |
| `tests/arrangement_test.cpp` *(new)* | **own** | | **own** | | | |
| `tests/engine_test.cpp` | | **own** | | | **own** (after) | |
| `tests/daemon_test.cpp` | | | | | | **own** |

Two conflict lines are called out because they are the ones that would
otherwise be discovered by a merge:

- **`engine.cpp` across 8b, 8c and 8f.** 8b and 8c are bracketed into one
  milestone *because* they both need it; 8f writes the journal from the audio
  thread and runs after them.
- **`app_engine.cpp` across 8a, 8e and 8f.** 8a adds the constants and the
  one-line retirement hooks in `pumpEngineEvents`; 8e adds the selection and
  target plumbing; 8f adds the journal drain and `commitTake`. All sequential.

And one gap in the original decision list, resolved here: **decision 13 does not
say where the arrangement publisher lives**, and its conflict list asserts 8d
does not touch `engine.cpp`. The resolution is that
**`src/ui/app_arrange.cpp` is new and owned by 8d** (both publishers, the item
blob and the automation blob, plus their reapers), reached through an
append-only block in `app.h` — which is exactly how this codebase already stages
parallel feature work (`app.h` carries an append-only block for the automation
publisher, another for the automation UI, and another for remote control). The
~40-line arrangement-automation *evaluation* pass inside `Engine::process()` is
landed by the **8b+8c** bracket, because `engine.cpp` is a single-owner file and
the pass is mechanical against types 8a froze.

### 10.2 8a — model, format, header types

Ships: `ArrangeClip`, the `TrackModel`/`Session` fields, the bounds constants,
`arrangeRepair`; format v6 read and write with `writeClipBody`/`clipBodyKey`
factored and the three new parser states; `RtArrangement`, `RtArrItem`,
`RtAutoSetN`, `ArrJournal`, `Ev::ArrangementRetired`, `Ev::TrackAutosRetired`,
`Cmd::SetArrangement`/`Locate`/`BackToArrangement`/`SetTrackAutos`,
`Voice::fade`/`fadeTo`, `Engine::arrOverride`, the journal ring and
`popJournal`, and the `autoValueAt` pointer refactor — **all compiled, none
used**.

**Gate — `tests/arrangement_test.cpp` (new):**

- **Round-trip byte-identity** for: (a) a set with no arrangement, which must
  equal its v5 bytes apart from the header line; (b) a dense set — 200 items
  across 8 tracks, splits, crossfades, fades, non-zero offsets, per-item
  envelopes, track lanes; (c) a set with a dangling `source`, which must come
  back unchanged; (d) an item with a non-zero `fadeshape`, which must come back
  unchanged.
- **The sort exception, both directions.** Sorted input → byte-identical output.
  Shuffled input → sorted output, and a second save of that is byte-identical
  (idempotence after one pass).
- **Structure rejected, values clamped**: an `aclip` with no `at`; an `autolane`
  with a malformed address; `arrangement` at top level; `at` of NaN clamped;
  `fadeshape 900` clamped to 255 and preserved. Every failure must leave the
  caller's session untouched.
- **`arrangeRepair` as a property test.** Ten thousand random sequences of drop /
  move / trim / split against a random starting lane: after every operation the
  invariant holds, `repair(repair(x)) == repair(x)`, and no item is ever shorter
  than `kMinArrBeats`.
- **`autoValueAt` unchanged.** 7a's entire evaluator table, re-run against the
  refactored pointer form, must produce identical results — this is a refactor
  and the test is what says so.

### 10.3 8b + 8c — the engine

Ships: `ArrState` and its side table, `startVoiceAt`, the fourth `fireDue` step,
the continuation rule, the fade ramp, `Cmd::SetArrangement` and its retirement,
`beat_` as the timeline, stop-no-rewind, `Cmd::Locate`, the loop brace and its
internal locate, `arrOverride` and `Cmd::BackToArrangement`, plus the §6.4
arrangement automation pass wired against 8a's types.

**Gate — `tests/engine_test.cpp`:**

1. **THE HEADLINE.** Build a 32-bar arrangement on four tracks: audio and MIDI,
   splits, one crossfade, non-zero offsets. Render it. Then script the identical
   music as a session performance — the same clips launched at the same beats,
   with the quantum set so each launch lands exactly where the item started —
   and render that. **The two buffers must be byte-identical.**
2. **R3.** One 8-beat clip, rendered whole; then the same clip split 64 times at
   irregular beats, rendered. **Byte-identical.** Then assert the negative
   control: the same 64 splits with a 1/64-beat fade on one of them must *not*
   be identical, which is what proves condition (3) of §3.5 is actually
   consulted.
3. **The empty case.** A set with no arrangement renders byte-identically to
   what `HEAD~1` renders. The fade fields must cost nothing.
4. **Boundary exactness.** 512 items whose starts fall on irrational beats,
   rendered at 64 and at 8192 frames per block: every item's first non-zero
   sample must be at the same absolute frame in both, and the sub-block splitter
   must produce the same number of `startVoice` calls.
5. **Locate.** A locate into the middle of an item starts that item at the right
   offset (compare against a render of an item authored at that offset); a
   locate flushes every sounding note-off; a locate leaves a launched session
   clip playing, at the same phase.
6. **Loop drift.** A four-bar brace over 64 laps: `beat_` at each wrap equals
   `loopStart` exactly, bit for bit. The internal locate assigns; it must not
   accumulate.
7. **Override timing.** With a 1-bar quantum, `Cmd::LaunchClip` sent mid-bar
   must leave the arrangement audible until the bar line and not one sample
   less. `StopTrack` and transport stop must both leave the bit set;
   `BackToArrangement` must clear it within one block and start the covering
   item mid-item.
8. **Retirement.** Republish a 512-item lane 100 times while it plays; every
   `RtArrangement` must come home through `Ev::ArrangementRetired` and nothing
   may leak. Under ASan, as `daemon_test` already runs.

### 10.4 8d — arrangement automation

Ships: `TrackModel::arrangeAutos` end to end — `buildArrangeAutos`,
`publishArrangeAutos`, the retirement bookkeeping, the merged hold table's
GUI-side half, and the `autolane` UI in the expandable lanes' model.

**Gate — `tests/arrangement_test.cpp` §2:**

- **The oracle.** A `t:<uid>/vol` track lane from 0.0 at beat 0 to 1.0 at beat
  32, rendered across 8 bars with *no clip playing at all for the middle four*:
  per-beat RMS must track `faderToGain(autoValueAt(...))` throughout, including
  the silent stretch's effect on a droning oscillator device. This is the test
  that says an arrangement lane is not gated on a voice.
- **Precedence.** The same address automated in both a track lane (constant 0.2)
  and a clip envelope (constant 0.8) on a clip that plays for bars 3–5: the
  rendered gain must be 0.8 for exactly those bars and 0.2 either side, with the
  transition landing on the block the clip starts and ends on. Then reverse
  which container holds which value and assert it flips — proving the result
  comes from ordering and not from the numbers.
- **The hold.** A device parameter claimed by a track lane, then additionally by
  a clip envelope, then released by the clip, then released by the track: the
  parameter must return to the *user's* hand-set value and never to the track
  lane's last output. Repeat with the releases in the other order.
- **The override gate.** Launch a session clip on an automated track: the track
  lane stops applying at the quantized boundary, and Back to Arrangement resumes
  it at the value the lane has *now*, not at the value it had when the override
  began.
- **Retirement**, as 7b's: 100 republishes under ASan, every `RtAutoSetN` home.

### 10.5 8e — the UI

Ships: `src/ui/timeaxis.h`, `src/ui/autolane.{h,cpp}`,
`src/ui/arrange.{h,cpp}`, the un-gated detail panel, the ARR chip, and the two
headless hooks.

**Gate:**

- **The extractions are moves.** After `timeaxis.h` and `autolane.{h,cpp}` land,
  the piano roll's existing behaviour must be unchanged: 7d's
  `NXTAKT_DEBUG_AUTOLANE` self-test passes verbatim, and a screenshot of the
  roll at a fixed zoom and scroll is pixel-identical to one taken before the
  move. A move that changes rendering is not a move.
- **`NXTAKT_DEBUG_ARRANGE=<track>`**, headless: seeds the scripted figure,
  prints the lane's invariant status, and produces a screenshot showing four
  items, a crossfade with both fades drawn, one expanded automation lane, and
  the detail panel showing item 1's notes — which is the whole of §7.4 in one
  frame.
- **`NXTAKT_DEBUG_ARRRENDER=<path>`**, headless: runs §10.3's headline gate
  through the app rather than through the test harness, and prints
  `IDENTICAL` or the first differing sample index. This is the hook that keeps
  the gate honest once the UI is what builds the arrangement.
- **Editing through the panel.** Drive `PianoRoll` against the selected item's
  `src` from the hook, assert exactly one undo entry per gesture, assert the
  edit reached that item and **only** that item (the copy-on-place property,
  asserted rather than assumed), and assert the lane republished once.
- **Axis agreement.** With the roll and the arrangement at the same zoom and
  scroll, `beatToX` must agree to the pixel for 1000 random beats — the property
  the extraction exists to guarantee.

### 10.6 8f — recording

Ships: the journal ring's producer side in `engine.cpp`, `commitTake`, the
contiguity check, the refusal path, and the ARR arm's behaviour.

**Gate:**

- **A gapless take commits.** A scripted 8-bar MIDI performance driven through
  `pushMidiFromGui` on three armed tracks: every note lands at the beat it was
  sent at, within one block, and the committed `ArrangeClip`s render
  byte-identically to a render of the same notes authored by hand.
- **A gapped take is REFUSED.** Force the journal ring full (stop draining it
  for two seconds while notes pour in), then end the take: `commitTake` must
  discard, take **no** undo point, leave the session byte-identical to its
  pre-take text, and report the drop count. This is the single most important
  assertion in this milestone, because the failure it guards is silent.
- **One undo point, at commit.** Not at take start, not per note. Undo must
  restore the exact pre-take text.
- **The GUI is not the clock.** Stall the GUI's drain for 500 ms mid-take (a
  scripted sleep), then finish: every note's beat must be unaffected, because
  the beats came from the engine. That is the assertion that would have failed
  under an event-based design.
- **Take on top of material trims it**, per `arrangeRepair`.

### 10.7 8g — the daemon

Ships: `PoolKindArrangement`, the wire types with their mirror asserts,
`translateArrangement`, `RejectBadArrangement`, the two-layer retirement, the
journal ring as a ninth region section, protocol v5 / pool v4, and the client
side of all of it.

**Gate — `daemon_test` §16:**

- **The split gate.** Render the *same* arrangement twice — once through an
  in-process `Engine`, once through a spawned `nxtaktd --driver null` — and
  assert the two outputs are **bit-identical**. This is the single assertion
  that proves engine-side scheduling works identically in-process and split,
  which is the whole claim of §9.
- **A table of malformed blobs**, each answered with `RejectBadArrangement` and
  each leaving `/dev/shm` clean: an unsorted lane; three items violating
  `c.start >= a.end`; an overlap of 5 beats; an overlap of 2 beats with only one
  fade; a `clip` index past `clipCount`; a blob shorter than its declared
  counts; an item `length` of 0; a `WireClip` inside it carrying every one of
  §10.5's seven bad offsets.
- **Two-layer retirement, measured.** Displace a lane whose clips reference three
  pool blocks; assert the daemon's built block is freed on the proof, that no
  `EvBlockRetired` is emitted before that, and that all three arrive after —
  with the `drains` counter observed to advance by at least 2.
- **Shared blocks.** One sample backing a session clip and six arrangement
  items: losing one item is not a retirement.
- **The journal across the boundary.** A scripted performance through the daemon,
  with the client asserting `seq` contiguity end to end and both drop counters
  at zero; then a deliberate stall, and the client correctly refusing the take.
- **Survival.** `SIGKILL` with an arrangement playing: the pool survives, the
  blobs are still `Live`, `republishClips()` plus `republishArrangements()` puts
  the session back with a `memcpy` and one command per track, and the same
  meter reading comes back.

---

## 11. Deliberately deferred

Named, with what unblocks them, so nobody has to guess whether they were
forgotten.

- **A tempo map and time-signature changes.** The timeline is in beats and the
  tempo is one number. A tempo track is a second mapping (beats ↔ seconds), a
  second thing to serialize, and a second thing every `consider()` has to
  respect. It is the largest deferred item and it is genuinely a wave of its own.
- **Overdub onto existing arrangement material.** §5.5 ends a take at the loop
  brace. Comping, take lanes and punch-in/punch-out ranges all live here.
- **Consolidate / flatten / freeze.** Rendering a span of a track to one audio
  item. It needs an offline render path, which does not exist.
- **"Update from source."** `ArrangeClip::sourceUid` is written, round-trips and
  dangles soft; the *gesture* that would use it is not built. This is the one
  place the reference-by-uid design's advantage could be recovered later, as an
  explicit user action rather than a default.
- **Linked / ghost items.** Same territory, opposite direction: an item that
  deliberately tracks another. It needs the second identity space §2.2 rejected,
  so it needs a format design of its own.
- **Curved fades.** `fadeShape` is reserved, clamped to its own width and
  round-tripped, exactly as `AutoPoint::curve` is; every non-zero value renders
  linear.
- **Follow actions on arrangement items.** `Follow` is session semantics — "what
  this loop does when it has played" — and an item's end is its end.
- **Per-item warp-marker editing.** Each item's `src` carries its own marker
  array; editing them in the arrangement needs the marker editor, which is the
  clip detail panel's business.
- **Cross-track item groups**, and moving a selection that spans tracks as a
  rigid body with respect to track *identity* rather than track *index*.
- **A dirty-item republish.** §7.6's escape hatch, if the 1.6 MB worst case ever
  shows up in a profile.
- **Locators and a marker track.**

---

## 12. Open questions for the orchestrator

These are contract calls, not implementation details. Each one changes what a
wave-8 agent writes.

1. **`engine.h` is being edited, and by more than one wave's worth of things.**
   This design adds `RtArrangement`, `RtArrItem`, `RtAutoSetN`, `ArrJournal`,
   `Ev::ArrangementRetired`, `Ev::TrackAutosRetired`, four `Cmd` values,
   `Engine::arrOverride`, the journal ring and `popJournal`, the `autoValueAt`
   pointer refactor, **and two floats on the private `Voice`** — which is the
   one part that cannot go in a side table, because `t.prev = t.voice` copies a
   voice wholesale and a fade must travel with it (§3.4). The header is the
   daemon's contract and three shipped phases build against it.
   **Proposal: 8a owns every header edit, lands them compiled-and-unused, and is
   the wave's serialization point — exactly as 7a did for the Rt automation
   structs. Confirm, or name a different owner.**

2. **Placement copies content** (§2). `ArrangeClip::src` is a `ClipModel` by
   value; MIDI notes are duplicated per item and audio is shared through
   `SampleRef`. The alternatives — reference-by-uid and a shared payload pool —
   are rejected in §2.2 for reasons that are about product behaviour and about
   the file format respectively, not about performance. It is the decision with
   the longest tail: it is in the model, in the format, in the wire form, in the
   publisher's dedupe and in the UI's editability story. **Confirm.**

3. **The bounded-crossfade divergence from Live, and `kMaxOverlapBeats = 4`**
   (§2.3). Live forbids overlap; we allow exactly one bounded case, and the
   bound (`c.start >= a.end`, at most two items at once) is what makes it free —
   two items is `Track::voice` + `Track::prev`, which already exists.
   **Confirm the divergence, and confirm 4 beats** — it is validated at the
   process boundary from v6 on, so it is not cheaply changed later.

4. **Stop no longer rewinds** (§3.6). `Cmd::SetPlaying 0` leaves `beat_` where
   it is; rewinding becomes `Cmd::Locate`. This changes the behaviour of the
   existing stop button for every existing set, including session-only ones.
   **Confirm**, and say whether a second press of stop should rewind (Live's
   behaviour) or whether Home is the only rewind.

5. **Clip envelope beats arrangement lane** (§6.4), implemented purely as pass
   ordering with no priority field. **Confirm the precedence**, and note that
   flipping it later is a one-line change *today* and a compatibility question
   once sets exist that rely on it.

6. **A take with a journal gap is refused, not committed short** (§5.4). The
   user loses the take and is told; the alternative loses four bars and is
   silent. **Confirm** — it is the decision most likely to generate a "why did
   it throw away my performance" report, and the answer to that report is this
   line.

7. **The v6 spellings, and the `aclip`-sort exception** (§8). Specifically:
   `arrangement`/`endarrangement`, `aclip`/`endaclip` with **no index**,
   `at`/`len`/`off`/`fadein`/`fadeout`/`fadeshape`/`source`,
   `autolane`/`endautolane`, `arrheight`, `loop <a> <b>`, `loopon`. Note that
   `off` means *offset* inside `aclip` and *deactivated* inside `env` — legal,
   because the parser is in different states, and confusing to read. And note
   the one stated exception to byte-identity: **`adoptSession` sorts `aclip`
   order on load**, so a hand-shuffled arrangement block round-trips sorted
   rather than identically. **Confirm both** — they are in the file format from
   v6 on.

8. **Bounds** (§2.4): `kMaxArrItems = 512` per track, `kMaxArrNotes = 65536` per
   track, `kMaxArrLanes = 32`, `kMaxArrPoints = 65536`, `kMinArrBeats = 1/64`.
   All are "a human cannot reach this" numbers, and all four of the first are
   validated at the process boundary, so they are protocol and not merely
   constants. **Confirm, or name different ones now.**

9. **Protocol v5 / pool v4 as its own wave** (§9.1). The control region grows a
   ninth section, `SharedStateT` grows, the layout hash moves, and a phase-4 and
   a phase-5 binary stop talking to each other. That is a deliberate
   incompatibility on a shipped protocol, and 8g is the wave that takes it.
   **Confirm it lands as 8g rather than being folded into an earlier one** —
   and note that 8g can start the moment 8a merges, because it touches neither
   `src/audio` nor `src/ui`.

10. **The detail panel is shown in Arrangement view** (§7.6), un-gating
    `view_ == MainView::Session` in `App::frame`, so an arrangement clip is
    editable with the same roll that edits a session clip. **Confirm** — and
    say whether `detailH_` is **per view** or shared. Shared is one field and
    one surprise (opening the arrangement resizes the session's panel);
    per-view is two fields, two lines of view state, and is *not* serialized
    either way, since view state is explicitly outside the undo snapshot.

11. **`src/ui/timeaxis.h` is extracted from `pianoroll.cpp` rather than
    duplicated** (§7.2), and — the part that is not in the surviving decision
    list — **`src/ui/autolane.{h,cpp}` is finally extracted too** (§7.3).
    AUTOMATION.md §6.5 specified `AutoLaneView` and argued for it in advance;
    it was never done, and decision 11 refers to it as though it exists. The
    proposal is that 8e does both extractions **as moves**, with the piano
    roll's rendering asserted pixel-identical afterwards. **Confirm the
    extractions, and confirm that 8e owns `pianoroll.{h,cpp}` for the duration**
    — a move of this shape cannot share the file.

12. **ARR arm is a third independent chip** (§7.7), beside REC and AUTO, at
    `uiId(1, 12)`, immediately right of AUTO and left of the position readout,
    drawn in AUTO's mode style. The alternative — REC means "record wherever
    the current view is" — makes one button do two things depending on a tab.
    **Confirm the chip and its position.**

---

## 13. The orchestrator's answers

Binding for wave 8. Where an answer differs from the proposal, the reason is
given; where it matches, the confirmation is still recorded so an agent never
has to guess whether a question was seen.

1. **Confirmed: 8a owns every `engine.h` edit**, lands them compiled-and-unused,
   and is the serialization point. This is what 7a did for the automation
   structs and it worked; the daemon builds against this header and three
   shipped phases depend on it, so exactly one wave may touch it. The two
   `Voice` floats are the proof the rule is right rather than an exception to
   it: §3.4 shows they *cannot* live in a side table, so the header edit is
   load-bearing and belongs with the wave that owns the header.

2. **Confirmed: placement copies content.** The rejected alternatives are
   rejected for the right kind of reason — reference-by-uid lets a scratchpad
   retroactively rewrite a finished record, which is a product bug, not a
   performance one. The duplication cost is bounded by `kMaxArrNotes` and audio
   is shared through `SampleRef` anyway.

3. **Confirmed: the bounded-crossfade divergence, and 4 beats.** Diverging from
   Live is justified here precisely because it costs nothing: two simultaneous
   items is `Track::voice` + `Track::prev`, which the engine already has. A
   divergence that needs no new machinery is a divergence worth taking.

4. **Confirmed with a modification: stop does not rewind, and a SECOND stop
   does.** Live's behaviour, and it is the right compromise — the timeline
   semantics are correct, and the muscle memory that expects a rewind still
   finds one. Home also locates to zero. The double-press window is not a
   timeout: a second `SetPlaying 0` while already stopped locates to zero, so it
   is state, not timing, and it cannot misfire on a slow hand.

5. **Confirmed: clip envelope beats arrangement lane.** A clip envelope is the
   more specific statement and the one the user drew most recently in context.
   Noted for the record that this is a one-line change today and a
   compatibility question the moment sets exist that depend on it — which is
   itself the argument for deciding it now rather than discovering it.

6. **Confirmed: a take with a journal gap is refused.** Silently committing a
   performance with four bars missing is the worse failure, because the user
   cannot see what is absent. The refusal must name what happened — "take
   discarded: N journal entries dropped" — since this is the line that will be
   quoted back in the bug report.

7. **Confirmed: the v6 spellings and the `aclip` sort exception.** The `off`
   collision is accepted: the parser is in different states, and renaming either
   use would be worse than the ambiguity — `off` is the natural word in both
   places, and the format's readers are the parser and an occasional human with
   the grammar in front of them. The sort-on-load exception is correct: an
   ordering invariant the engine depends on must be established by the loader,
   not assumed of the file.

8. **Confirmed, all five bounds.** They are protocol from v6 on, and every one
   is a "a human cannot reach this" number with room to spare. `kMaxArrItems =
   512` per track is the only one worth a second look — a dense edit could
   approach it — and 512 items on one track is already past what an arrangement
   built by hand contains.

9. **Confirmed: protocol v5 / pool v4 lands as 8g, on its own.** A deliberate
   incompatibility on a shipped protocol deserves its own gate and its own
   commit, exactly as phase 3 got. Starting it the moment 8a merges is right,
   since it touches neither `src/audio` nor `src/ui`.

10. **Confirmed: the detail panel is shown in Arrangement view. `detailH_` is
    PER VIEW.** Two fields against one surprise is a trade worth taking: the
    arrangement wants a tall panel for envelope lanes and the session wants a
    short one for the grid, and a shared height means every switch between views
    silently resizes the other. Neither is serialized; view state stays outside
    the undo snapshot.

11. **Confirmed: both extractions, as moves, and 8e owns `pianoroll.{h,cpp}`
    for the duration.** `autolane.{h,cpp}` should have been extracted when
    AUTOMATION.md §6.5 argued for it; a design that refers to a component as
    though it exists is a debt that has now been called in twice, which is
    exactly when to pay it. Pixel-identical assertion afterwards, the same gate
    the `app.cpp` decomposition used.

12. **Confirmed: ARR is a third independent chip** at `uiId(1, 12)`, right of
    AUTO. One button whose meaning depends on which tab is open is the kind of
    modality that produces a lost take and no explanation for it.

### Two additions to wave 8's scope, from the warp wave

- **`ClipModel::markers` must be serialized in v6** and included in the undo
  snapshot. It is not today: the warp wave could touch neither
  `src/core/project.cpp` nor `app_undo.cpp`, and nothing populates the vector
  yet. This has to land *before* any marker UI, or the first thing a user does
  with warp markers is lose them on save.
- **`publishedWarp_` / `retiringWarp_` move onto `App`.** They live as a
  file-scope publisher in `app_engine.cpp` for the same ownership reason; the
  move is a change to their declaration and nothing else, and 8a is already in
  that file.

---

## 14. 8a shipped

The serialization point is in. What landed, and the four places a later
milestone has to read this section rather than the one above it:

**`src/audio/engine.h`** — `RtArrItem`, `RtArrangement`, `RtAutoSetN`
(+ `kMaxRtArrLanes`), `JournalKind`, `ArrJournal`, `Cmd::SetArrangement` /
`SetTrackAutos` / `Locate` / `BackToArrangement` (appended, values 26–29),
`Ev::ArrangementRetired` / `TrackAutosRetired` (appended, values 13–14),
`Voice::fade` / `fadeTo`, `Engine::arrOverride`, `Engine::journalDropped`,
`popJournal()` and the `Ring<ArrJournal, 4096>` behind it. All compiled, none
used: `tests/engine_test.cpp` §33 renders a set twice, once having pushed all
four commands (including the `a = -1` transport cell and the null-clears form)
and once not, and asserts the two are bit-identical.

**`autoValueAt` is now the pointer form** with two inline forwarders, one per
container, and the body moved verbatim. 7a's evaluator table passes untouched;
§33 additionally evaluates the same points through `RtAutoSetN` and asserts
bit-identity with the `RtAutoSet` path.

**`src/ui/session.h`** — `ArrangeClip`, `TrackModel::arrange` /
`arrangeAutos` / `arrHeight`, `Session::loopStart` / `loopEnd` / `loopOn`, the
five bounds, and `arrangeRepair`.

**`src/core/project.cpp`** — format v6 exactly as §8 specifies, with
`writeClipBody` / `clipBodyKey` factored and `St::Arrange` / `St::AClip` /
`St::AutoLane` added. The v5 → v6 no-arrangement diff is exactly the header
line, asserted against a hand-written v5 document and confirmed against the
regenerated demo set.

Four deviations, each deliberate and none of them a change of decision:

1. **`ArrJournal` is 24 bytes, not the 32 §5.3 quotes.** Four 4-byte integers
   and one `f64` is 24 with natural alignment. The field list is what that
   section actually specifies, so the fields are kept and the arithmetic is
   corrected rather than padded up to match the prose.
2. **`arrangeRepair` is defined inline in `session.h`**, not in
   `src/ui/app_arrange.cpp` as §2.5 says. That file belongs to 8d and does not
   exist yet, while 8a has to ship *and test* the function. It is a pure
   transform of a vector with no dependency on `App`, so the only thing that
   changes if 8d moves it is which file it is in.
3. **The warp-marker grammar is `wm <srcFrame> <beat>`, not a `warp` block.**
   `warp` is already a clip key — `warp 2` is the warp mode — so a block by that
   name would have to be told apart from the scalar by whether the rest of the
   line is empty, which is an ambiguity this format has nowhere else. Audio
   clips only, gated exactly as `note` is gated on MIDI, and refused at the
   terminator inside a MIDI clip for the same reason `note` is refused inside an
   audio one.
4. **`clipBodyKey` returns a three-valued `BodyKey`** (`No` / `Yes` / `Bad`)
   rather than §8.3's `bool` + `std::string* err`. Offering a line to a shared
   handler has three outcomes — consumed, not mine, mine and broken — and
   folding two of them together is precisely where the format's
   structure-rejected / values-clamped split lives.

And one thing 8a deliberately did **not** do, which the next milestone in that
file owns: **§8.5's sort-on-load is not implemented.** The parser preserves file
order, as §8.5 requires it to; the sort belongs to `App::adoptSession`
(`src/ui/app_project.cpp`), which is not 8a's file. `arrangeRepair` is the hook
— it stable-sorts as step one — and `assignUids` will need to learn about
`ArrangeClip::uid` at the same time, since an item loaded from a file that never
had one comes back as 0.

### 8a shipped — and what it hands to the milestones after it

Landed: the header types (compiled and unused), the model, `arrangeRepair`,
format v6, warp markers in the format and the undo snapshot, and
`publishedWarp_`/`retiringWarp_` moved onto `App`. 497 engine checks, the four
demo renders bit-identical, `make test` green with and without system plugins.

Three hand-offs, each belonging to a file 8a did not own. They are recorded
here because a note in a finished agent's report is not a place work survives:

- **`App::adoptSession` must call `arrangeRepair`** (`src/ui/app_project.cpp`).
  §8.5's sort-on-load is deliberately NOT in the parser — the parser preserves
  file order, and the loader establishes the invariant, because an ordering the
  engine depends on must be established rather than assumed of the file.
  `arrangeRepair` stable-sorts as its first step, so calling it is the whole
  fix. **`assignUids` must also learn about `ArrangeClip::uid`**: an item from
  a file written without one loads as 0. Owner: whoever takes 8e, or an earlier
  milestone that touches that file.
- **8g must extend the wire classifiers.** `commandIsKnown` bounds at
  `type <= Cmd::RecordMidiSlot`, so `SetArrangement`/`SetTrackAutos`/`Locate`/
  `BackToArrangement` (26–29) currently classify as unknown. That fails CLOSED
  — the daemon rejects them with `RejectUnknownCommand`, verified — so the tree
  is safe today and this is not urgent, only necessary before arrangement
  crosses the process boundary. `Ev::ArrangementRetired`/`TrackAutosRetired`
  need cases in `eventIsScalar` at the same time.
- **`ArrangeClip::src.uid` is deliberately not serialized.** The `uid` inside an
  `aclip` is the *item's*. Two placements of one loop would otherwise both claim
  one identity, which is the same reasoning that made placement copy content.

Two corrections to this document, from contact with the code:

- **`ArrJournal` is 24 bytes, not the 32 §5.3 states.** Four 4-byte integers and
  one `f64` is 24 with natural alignment. The fields are as specified; the
  arithmetic in the prose was wrong.
- **`arrangeRepair` ships inline in `session.h`**, not in `app_arrange.cpp` as
  §2.5 places it. That file is 8d's and does not exist yet, and 8a had to both
  ship and test the function. It is a pure vector transform; moving it later
  changes which file it is in and nothing else.
