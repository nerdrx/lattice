# Shattering `src/ui/app.cpp`

Design note, wave 7. `src/ui/app.cpp` is 3 728 lines and `src/ui/app.h` is
790. Together they are the single largest thing in the tree after the vendored
CLAP headers, and — more to the point — they are **the parallelism limiter**:
one agent per wave owns `app.cpp`, so every UI feature, every engine-facing
change and every bug fix in the front half of the product serialises through
one file.

This note proposes a decomposition into eight translation units, a header
strategy, a mechanical migration that compiles at every step, and the
ownership map it buys.

Nothing here is written to the repo.

---

## 1. What is actually in the file

The file is already sectioned by `// ---` banners, and the sections are
honest — they line up with responsibilities, not with when things were
written. That is most of the work already done.

| # | lines | count | section | what |
|---|---|---|---|---|
| 0 | 1–100 | 100 | preamble | includes, `namespace lay` (layout constants), file statics (`nowSeconds`, `homeDir`, `isAudioFile`, `icontains`), `kReturnLetter`/`kSendUndo`/`kReturnPlaceholder`, `kRecordSeconds`, `kRecordNotes`, `kArrowGesture`, `App::App`/`~App` |
| 1 | 101–263 | 163 | lifecycle | `init`, `shutdown`, `run` |
| 2 | 264–479 | 216 | engine plumbing | `send`, `publishNotes`, `pushClip`, `pushTrack`, `pushAll`, `releaseStaleSlots`, `pumpEngineEvents` |
| 3 | 480–660 | 181 | device chains | `chainOwner`, `ownerName`, `modelOwners`, `publishChain`, `addDevice`, `removeDevice`, `ensurePluginScan` |
| 4 | 661–1055 | 395 | project | `assignUids`, `serializeDevices`, `materializeDevices`, `releaseAllChains`, `adoptSession`, `openProject`, `saveProjectTo` |
| 5 | 1056–1424 | 369 | undo/redo | `stagingPath`, `writeAll`, `readAll`, `snapshotSession`, `undoCoalesce`, `pushUndoNow`, `undoPoint`, `clearUndo`, `cancelTakes`, `restoreEntry`, `undo`, `redo`, `debugUndoSelfTest` |
| 6 | 1425–1581 | 157 | transport + clip + selection helpers | `setTempo`, `togglePlay`, `loadClipInto`, `clearClip`, `trackHasNoteDevice`, `createMidiClip`, `selectChainOwner`, `selectTrack`, `addTrack`, `addScene` |
| 7 | 1582–1849 | 268 | recording | `startRecording`, `stopRecording`, `finishRecording`, `finishMidiRecording` |
| 8 | 1850–1887 | 38 | browser model | `browseTo`, `refreshBrowser` |
| 9 | 1888–2151 | 264 | frame | `frame`, `handleShortcuts`, `updateKbdPiano`, `toggleKbdMidi` |
| 10 | 2152–2224 | 73 | roll routing + preview | `visibleRoll`, `startPreview`, `updatePreviews`, `stopPreviews` |
| 11 | 2225–2392 | 168 | control bar | `drawControlBar` |
| 12 | 2393–2483 | 91 | browser draw | `drawBrowser` |
| 13 | 2484–3059 | 576 | session view | `drawSessionView`, `drawTrackHeaders`, `drawClipGrid`, `drawClipSlot`, `drawSceneColumn`, `drawMixer`, `drawReturnStrips`, `drawMasterStrip` |
| 14 | 3060–3328 | 269 | clip detail | `drawWaveform`, `drawDetailPanel`, `drawClipDetail` |
| 15 | 3329–3624 | 296 | device view | `drawDeviceDetail`, `drawPluginBrowser`, `drawDeviceStrip` |
| 16 | 3625–3728 | 104 | chrome | `drawArrangementView`, `drawStatusBar`, `drawDragGhost` |

Two observations that shape everything below:

- **The draw half (§11–16, 1 504 lines) and the model half (§2–8, 1 624 lines)
  barely overlap.** They meet at exactly three places: `undoPoint()`, the
  push-to-engine calls, and the selection variables. That is a clean fault
  line, and it is why this split is tractable at all.
- **Section 13 alone is 576 lines** and contains the two hottest editing
  surfaces in the product (the clip grid and the mixer). It has to be its own
  TU or the split achieves nothing for the agents most likely to collide.

---

## 2. The decomposition — eight translation units

Sized to keep every file under ~850 lines and, more importantly, to give each
one a single owner-shaped responsibility.

| file | from §§ | ~lines | owns |
|---|---|---|---|
| `app.cpp` | 0, 1, 9, 10 | 600 | the shell: construction, `init`/`shutdown`/`run`, `frame` layout, `handleShortcuts`, the computer piano, note previews, `visibleRoll` |
| `app_engine.cpp` | 2, 7, part of 6 | 500 | everything that talks to the engine: `send`, `pushClip`, `pushTrack`, `pushAll`, `releaseStaleSlots`, `pumpEngineEvents`, `setTempo`, `togglePlay`, and all four recording functions |
| `app_project.cpp` | 4 | 400 | `assignUids`, `serializeDevices`, `materializeDevices`, `releaseAllChains`, `adoptSession`, `openProject`, `saveProjectTo` |
| `app_undo.cpp` | 5 | 380 | the whole undo/redo system + `debugUndoSelfTest` |
| `app_session.cpp` | 13, rest of 6, `drawDragGhost` | 800 | session view, track headers, clip grid, clip slot, scene column, mixer, return strips, master strip, drag-and-drop, `loadClipInto`/`clearClip`/`createMidiClip`/`selectTrack`/`selectChainOwner`/`addTrack`/`addScene` |
| `app_detail.cpp` | 14 | 280 | `drawWaveform`, `drawDetailPanel`, `drawClipDetail`, hosting the `PianoRoll` |
| `app_devices.cpp` | 3, 15 | 490 | chain-owner addressing, `addDevice`/`removeDevice`/`publishChain`/`ensurePluginScan`, and the whole DEVICES tab |
| `app_chrome.cpp` | 8, 11, 12, 16 minus ghost | 400 | control bar, file browser (model + draw), status bar, arrangement placeholder |

Total ≈ 3 850 (the 120 over is per-file include blocks).

**Why these seams and not others:**

- **`app_devices.cpp` pairs §3 with §15 deliberately.** The chain-owner
  addressing (`ChainOwner`, `ownIsTrack`, `ownerName`, `modelOwners`) exists
  only to serve the device view and the project layer, and `drawDeviceStrip`
  is its heaviest client. Splitting model from view here would put
  `chainOwner()` and its only interesting caller in different files for no
  gain — and it is the seam the daemon migration (`gui_on_daemon.md` step 4)
  rewrites end to end, so one agent should own both halves.
- **Recording lives with engine plumbing, not on its own.** All four record
  functions are `Cmd::RecordSlot` producers and `Ev::Record*` consumers;
  `finishRecording` is called *from* `pumpEngineEvents`. They are one protocol
  and their daemon migration is one job (`gui_on_daemon.md` §7).
- **`app_session.cpp` keeps the clip-model helpers** (`loadClipInto`,
  `createMidiClip`, `selectTrack`) because they are what the grid's mouse
  handling calls, and separating a click from what it does buys nothing.
- **`app_chrome.cpp` is the leftovers bucket, and that is fine.** Control bar,
  browser, status bar and the arrangement placeholder share no state with each
  other, which is exactly why they can share a file without contention: two
  agents editing different functions in one file merge cleanly; two agents
  editing one 576-line function do not.

**Rejected splits, and why:** a `views/` subdirectory (the Makefile sweeps
`src/**/*.cpp`, so directories cost nothing structurally — but they imply an
interface boundary this refactor is explicitly not creating yet); splitting
§13 into `session_grid.cpp` + `mixer.cpp` (right eventually, premature now —
they share `drawSessionView`'s layout arithmetic and the `scrollX` threading);
a separate `app_drag.cpp` (25 lines of state and three touch points, see §5.3).

---

## 3. Header strategy

### 3.1 The choice

**(a) One `app.h`, every TU defines `App` members.** No new abstraction, no
access changes, every member stays reachable. `app.h` remains a large header
that all eight TUs include, so a member add rebuilds everything.

**(b) Per-view friend structs** — `struct MixerView { App& a; void draw(Rect); }`.
Requires either `friend` declarations (one per view, in `app.h` — so `app.h`
still changes for every new view) or promoting a large private surface to
public. Churn without isolation.

**(c) A context struct threaded down** — `struct ViewCtx { Ui&, Renderer&,
Fonts&, Session&, const EngineState&, Selection&, std::string& status; }`, views
become free functions. Genuinely decoupled and testable, and it is where this
should end up. Also a rewrite of every one of the ~1 500 lines of draw code in
one go, fighting a codebase whose entire style is members-everywhere.

**Recommendation: (a) now, shaped so that (c) is reachable incrementally.**

The reason is precise: the goal of this wave is *parallelism*, not decoupling.
Under (a), two agents adding members to `app.h` produce two one-line diffs in
different places — a trivial merge. Under today's arrangement they produce two
200-line diffs in one 3 728-line file. That is the entire win, and (a) buys all
of it for a fraction of (c)'s cost.

### 3.2 But split the header in two

`app.h` is currently two unrelated things stapled together:

- **lines 1–345: the session model** — `ClipKind`, `NoteModel`, `ClipModel`,
  `DeviceModel`, `SavedDevice`, `LiveDevice`, `ClipSample`, `TrackModel`,
  `ReturnModel`, `SceneModel`, `Session`, `MainView`, `DetailTab`,
  `BrowserEntry`, `DragState`, `KbdPiano`;
- **lines 347–790: `class App`**.

Two facts prove the staple is wrong: `src/core/project.h` forward-declares
`struct Session; // src/ui/app.h`, and `src/core/project.cpp` includes
`../ui/app.h` — the core layer reaching up into the GUI shell header to see a
model type. `src/ui/pianoroll.h` does the same, and it is the reason `App` has
to hold `PianoRoll` behind a `unique_ptr` with out-of-line ctor/dtor
(app.h:340–352).

So:

```
src/ui/session.h      ClipModel, TrackModel, Session, DragState, KbdPiano, …   (~345 lines)
src/ui/app.h          class App                                                 (~450 lines)
src/ui/app_internal.h shared-between-TUs private bits (see 3.3)                 (new, ~90)
```

`project.h`/`project.cpp` and `pianoroll.h` include `session.h` and stop seeing
`App` entirely. That is a real layering fix, it is ~20 minutes of moving text,
and it should be step 0 because everything else is easier afterwards.

### 3.3 `app_internal.h` — the mechanical prerequisite

Eight TUs cannot share `static` file-scope helpers. These must move out of
`app.cpp`'s preamble into an internal header, as `inline` (not `static`, which
would give each TU its own copy of `kReturnLetter` and defeat the point):

- `namespace lay` — the layout constants, used by `frame`, session view, mixer,
  chrome;
- `nowSeconds()` (frame + previews), `homeDir()` (init + browser),
  `isAudioFile()` (browser + drag drop), `icontains()` (plugin filter + the
  `LATTICE_DEBUG_ADDFX` hook in `init`);
- `kReturnLetter[]`, `kSendUndo[]`, `kReturnPlaceholder` (mixer, returns,
  device view, project);
- `kRecordSeconds`, `kRecordNotes` (recording only — leave them in
  `app_engine.cpp`);
- `kArrowGesture` (shortcuts + piano roll routing);
- **the `UiKind` enum** — see §5.2.

### 3.4 What state moves where

Under (a): **nothing moves.** Every member stays on `App`. That is the point —
it is what makes each extraction a pure text move with no semantic diff to
review.

What *should* be tracked, so that (c) is reachable later, is which members each
TU actually touches. From the census:

| member group | touched by |
|---|---|
| `win_ rend_ ui_ f*_` | all eight (via `ui_`, 95 sites; `win_`, 45) |
| `ses_` (181 sites) | engine, project, undo, session, detail, devices, chrome |
| `engine_ audio_ midi_` | app (init/shutdown), engine, project (rate), devices (rate), session (meters), detail, chrome — **this is what `gui_on_daemon.md` step 1 eliminates**, and after it only `app.cpp` and `app_engine.cpp` touch the handle at all |
| `selTrack_ selSlot_ devOwner_ selDevice_` | session (writer), devices, detail, engine, chrome |
| `status_` (49 sites) | all eight, write-only |
| `drag_` | session, chrome (browser), app (ghost) — see §5.3 |
| `undo_ redo_ undoGesture_` | undo (owner), app (`frame` clears the gesture), session/detail/devices/chrome (60 `undoPoint` calls) |
| `browser*_` | chrome only |
| `plugin*_ strip/paramScroll_` | devices only |
| `previews_ kbd_ roll_` | app + detail |
| `pendingRecs_ recIntent_ recTakeNo_ recStartBeat_` | engine only |
| `published*_ retiring_ registry_` | devices + project + app (shutdown) |
| `clipLive_ publishedNotes_ retiringNotes_` | engine only |

Four groups are already single-owner (`browser*_`, `plugin*_`, recording,
clip-publication bookkeeping) — those are free extractions later. The
cross-cutting four are `ui_`, `ses_`, selection and `status_`, which is exactly
the payload a `ViewCtx` would carry.

---

## 4. The mechanical migration

### 4.1 Ground rules

1. **One TU per commit, and the commit contains nothing else.** No
   reformatting, no renames, no "while I was in there". A move-only commit is
   what makes `git blame -C -C -w` and `git log -M -C --follow` find the
   origin; a commit that moves *and* edits is a commit whose blame is lost.
2. **Every step compiles and `make test` is green.** There is no half-state
   where a symbol lives nowhere.
3. **No `git mv` for most of it** — you cannot `git mv` part of a file. The
   one place it applies: if a TU takes the *majority* of the original file,
   rename the original to it and create the new `app.cpp` fresh. Here no TU
   takes a majority (the largest is 800 of 3 728), so: create eight new files,
   cut into them, and rely on move-only commits for blame. Verify after each
   with `git blame -C -C -w src/ui/app_session.cpp | head` — if it shows the
   original authors, the commit was clean.
4. **The Makefile needs no change.** `SRC := $(shell find src -name '*.cpp')`
   with `src/daemon` filtered out. New files under `src/ui/` are swept
   automatically. (Check `Makefile.mingw` too — it has its own sweep.)

### 4.2 The order, and why

Extract **leaves first** — the sections with the fewest inbound references —
so each step shrinks `app.cpp` without any other step having to be aware of it.

| step | file | from | net `app.cpp` | why here |
|---|---|---|---|---|
| 0 | `session.h` + `app.h` + `app_internal.h` | header split, statics → inline | 3 728 → 3 650 | prerequisite; no function moves; also fixes `project.cpp` including `ui/app.h` |
| 1 | `app_undo.cpp` | §5 | → 3 280 | most self-contained body in the file: one entry-point set (`undoPoint`, `undo`, `redo`, `clearUndo`, `cancelTakes`), three file-statics that go with it |
| 2 | `app_project.cpp` | §4 | → 2 890 | next most self-contained; `adoptSession` is undo's only downward call, already extracted |
| 3 | `app_devices.cpp` | §3 + §15 | → 2 415 | two contiguous blocks, one owner, and the block `gui_on_daemon.md` step 4 rewrites |
| 4 | `app_detail.cpp` | §14 | → 2 150 | contiguous; the only TU that needs `pianoroll.h` besides the shell |
| 5 | `app_chrome.cpp` | §8 + §11 + §12 + §16(part) | → 1 760 | four small independent blocks |
| 6 | `app_engine.cpp` | §2 + §7 + part of §6 | → 1 280 | left late on purpose: it is the block `gui_on_daemon.md` steps 1–3 rewrite, so extract it when its shape is already settled |
| 7 | `app_session.cpp` | §13 + rest of §6 + drag ghost | → ~600 | the big one, extracted last, when it is the only thing left and the merge surface is smallest |

Each step is 1–3 hours of careful cut-and-paste plus a build. **The whole
thing is one agent for one wave**, or split as 0–3 / 4–7 across two agents in
sequence (not in parallel — they would collide in `app.cpp` on every step).

That is the irony worth stating: *the file split cannot itself be
parallelised.* It must be one agent, and it must be the first thing in the
wave, because every other agent in the wave depends on its output.

### 4.3 Per-step recipe

```
1. create src/ui/app_X.cpp with: #include "app.h" (+ app_internal.h, + whatever
   that section's includes are — check the top-of-file list at app.cpp:1-13)
2. cut the section's lines verbatim into it, banner comment included
3. build; fix ONLY missing includes and statics that need to move to
   app_internal.h (a static that only this TU needs stays here, unstatic'd
   into an anonymous namespace)
4. make test; tools/headless_test.sh screenshots must be byte-identical
5. commit, message "src/ui: move <section> to app_X.cpp (no functional change)"
```

Step 3's "fix only" is the discipline that keeps blame intact. If a move
reveals a genuine problem, note it and fix it in a *following* commit.

---

## 5. The risky seams

### 5.1 Undo's reach into everything

**60 call sites** of `undoPoint`/`undoPointWith`, distributed across every
future TU: lines 1161, 1296–1385 (undo's own self-test), 1443, 1497, 1697,
1783, 1821 (engine/record), 2000–2075 (shortcuts), 2244–2286 (control bar),
2551–2558, 2736–2748, 2786–2799, 2832–2889, 2943, 2980 (session view/mixer),
3162–3304 (clip detail), 3423 (plugin browser), 3511–3605 (device strip).

Three specific hazards:

- **`undoPointWith` is a template** (app.h:757–764) and must stay in the
  header. It already is. Do not "tidy" it into `app_undo.cpp`.
- **The dependency is a cycle at the class level.** Views call `undoPoint()`
  → `pushUndoNow()` → `snapshotSession()` → `saveProject`/`loadProject`; and
  `restoreEntry()` → `adoptSession()` → `pushAll()` → `pushClip()`. Under
  header strategy (a) this is a non-issue (all one class, all one header).
  Under (c) it would need an explicit `UndoService` interface — noted as a
  cost of (c), not a reason against it.
- **The gesture latch is owned by `frame()`** (app.cpp:1917–1922) but *read*
  by every `undoPoint(what, gesture)` caller and by `undoCoalesce`. After the
  split, `undoGesture_`'s only writer outside `app_undo.cpp` is `app.cpp`'s
  frame preamble. Document that in one comment or someone will add a second
  writer.

### 5.2 `ui_` id kinds — a silent-collision hazard

`uiId(kind, a, b)` hashes an integer `kind` chosen by convention. The current
allocation, recovered by grep:

| kind | site | owner after the split |
|---|---|---|
| 1 | 2240 control bar | chrome |
| 2 | 2414 file browser | chrome |
| 3 | 2537 track headers | session |
| 4 | 2588 clip grid | session |
| 5 | 2774 scene column | session |
| 6 | 2831 mixer | session |
| 7 | 3004 master strip | session |
| 8 | 3161 clip detail | detail |
| 9 | 3094 detail panel tabs | detail |
| 10 | 3353 plugin browser | devices |
| 11 | 3488 device strip | devices |
| 12 | 3596 param knobs | devices |
| 13 | 2922 return strips | session |
| **14** | **unused** | — |
| 15 | 56 arrow gesture | app / detail |
| 16 | 2371 control bar (second) | chrome |

The convention that keeps these unique is a comment at app.cpp:53–56 —
"every `uiId` kind in use is listed at its call site". **That convention dies
the moment the call sites are in eight files.** Two agents both reaching for
`14` produce widget ids that collide, which manifests as `hot`/`active`
confusion between, say, a mixer fader and a device knob — intermittent,
unreproducible, and nobody's first suspect.

**Fix, in step 0:** a single enum in `app_internal.h`.

```c++
// Every uiId kind in the app. Adding a widget family means adding a line
// HERE — the ids are hashed, so a duplicate kind is silent misbehaviour and
// not a compile error.
enum UiKind : int {
    UiControlBar = 1, UiFileBrowser, UiTrackHead, UiClipGrid, UiSceneCol,
    UiMixer, UiMasterStrip, UiClipDetail, UiDetailTab, UiPluginBrowser,
    UiDeviceStrip, UiParamKnob, UiReturnStrip, UiUnused14, UiArrowGesture,
    UiTempo, UiKindCount
};
```

One shared line-append per new widget family, which is a mergeable conflict;
a silently duplicated integer is not.

### 5.3 Drag state

`drag_` is touched at 25 lines in four clusters:

- **1226** — `undo()` cancels an in-flight drag before restoring a session;
- **2474–2477** — `drawBrowser` starts a `BrowserFile` drag (→ `app_chrome.cpp`);
- **2729–2753** — `drawClipSlot` starts a `Clip` drag and resolves both drop
  kinds (→ `app_session.cpp`);
- **3703–3718** — `drawDragGhost` renders whichever is in flight (→ ?).

So a drag is *started* in two files and *resolved* in one. Recommendation:

- `drawDragGhost` goes to **`app_session.cpp`**, next to the resolution logic
  it visualises — not to `app_chrome.cpp` with the other chrome, and not to
  `app.cpp` where `frame()` calls it. `frame()` calling into `app_session.cpp`
  is already true (`drawSessionView`).
- Write the protocol down once, in `app.h` beside `DragState`: *"a producer
  sets `kind`, `path`/`srcTrack`/`srcSlot`, `startX/Y` and clears `armed`;
  only `drawClipSlot` and `drawDragGhost` read it; only `drawClipSlot`,
  `undo()` and `drawDragGhost`'s release path clear it."* Four lines that stop
  the third producer from being added blind.
- Do **not** extract `app_drag.cpp`. 25 lines of state with a two-file
  producer set is not a translation unit; it is a comment.

### 5.4 `chainOwner()` returns raw pointers into `ses_`

`ChainOwner` holds `std::vector<DeviceModel>*`, `std::vector<SavedDevice>*`
and `const RtChain**` straight into the session. Any `push_back` on the
owner's device vector invalidates the first two — and `addDevice()` does
exactly that (app.cpp:595) *while holding a `ChainOwner`* (it re-reads through
`*co.published`, which is fine, and through `devices`, which is a reference
bound before the `push_back`… and is therefore live only because
`co.devices` is a pointer to the vector, not into it). This is correct today
and it is fragile. After the split it will be called from two files.

**Required in the same wave:** a comment on `ChainOwner` stating that the
struct is valid only until the next mutation of the owner's `devices` vector,
and that `devices`/`saved` are pointers *to* the vectors precisely so a
`push_back` does not invalidate them. Cheap insurance on a seam two agents will
now share.

### 5.5 Selection

`selTrack_`, `selSlot_`, `devOwner_`, `selDevice_` are read by five TUs and
written by `selectTrack`/`selectChainOwner` (→ `app_session.cpp`), plus direct
writes at 3439 (`drawDeviceStrip` repairs `devOwner_` when its target
vanished), 3534 and 606/641–642 (`selDevice_` from device add/remove/click),
and 181–184 in `init()`'s debug hook.

The comment in `app.h` — "every path that moves the selection goes through
[`selectTrack`]" — is load-bearing for note previews (they sound on the armed
track, which only matches the visible clip because selection auto-arms). It is
already *slightly* untrue (`devOwner_`/`selDevice_` are written directly). After
the split, make it true or make it explicitly false in writing; a silent
half-invariant across eight files is how previews start sounding on the wrong
instrument.

### 5.6 `PianoRoll` ownership

`roll_` is created lazily inside `drawClipDetail` (3289) → `app_detail.cpp`;
read by `visibleRoll()` (2159) → `app.cpp`; cleared by `undo()` (1221) →
`app_undo.cpp`; and `App::~App` needs the complete type. So three TUs must
include `pianoroll.h`, and `pianoroll.h` includes `app.h` today — which becomes
`session.h` after step 0, removing the circularity that forced the
forward-declaration dance in the first place. Net simplification; just don't
forget the includes.

### 5.7 `debugUndoSelfTest`

169 lines (1266–1395) of headless test harness living in production code,
reaching into devices, params, the project layer and the session. It is the
widest single function in the file by dependency count. Keep it in
`app_undo.cpp`, accept that it makes that TU depend on `app_devices.cpp`'s
surface, and note it as the first candidate for a real test binary once
`gui_on_daemon.md` step 1's `EngineState` makes the app testable without a
window.

---

## 6. The ownership map this buys

After the split, a wave can run these concurrently with no shared file except
append-only headers:

| agent | files | typical work |
|---|---|---|
| **engine** | `app_engine.cpp`, `src/ipc/*`, `src/audio/*` | the daemon adoption spine (`gui_on_daemon.md` steps 0–3) |
| **session** | `app_session.cpp` | clip grid, mixer, drag/drop, track ops |
| **devices** | `app_devices.cpp` | device view, chain ops, the catalog browser |
| **detail** | `app_detail.cpp`, `pianoroll.cpp` | clip detail, piano roll, waveform |
| **project** | `app_project.cpp`, `core/project.cpp` | format versions, materialization |
| **undo** | `app_undo.cpp` | history, coalescing, snapshot cost |
| **chrome** | `app_chrome.cpp` | control bar, file browser, status, arrangement |
| **shell** | `app.cpp` | lifecycle, frame layout, shortcuts, keyboard piano, crash banners |

Shared, and therefore governed:

- `app.h` — **append-only**. New members go at the end of their section; nobody
  reorders, nobody reformats. A member add is a one-line diff and merges.
- `session.h` — model changes are a design decision; route through the wave
  lead, they ripple into `project.cpp` and the file format.
- `app_internal.h` — append-only for `UiKind`; layout constants are shared and
  changing one is a visual change to several views, so treat as governed.

**Realistic concurrency: 4–6 agents**, versus 1 today. The limit is not the
files, it is that `session`+`detail`+`chrome` all draw and a layout change in
`lay::` touches all three.

---

## 7. Sequencing against the daemon work

The two plans interlock at one point and it is worth being explicit:

- `gui_on_daemon.md` **step 1** (the `EngineState` snapshot + `EngineHandle`)
  removes `engine_` from the six *view* TUs. Until it lands, `app_session.cpp`,
  `app_detail.cpp` and `app_chrome.cpp` all include `audio/engine.h` and all
  read atomics — meaning the engine agent and the view agents still contend
  semantically even though they no longer contend textually.
- The split, in turn, is what lets the daemon steps 3/4/6 run concurrently
  instead of queueing on one file.

So the correct order inside the wave is:

```
1. header split + app_internal.h + UiKind          (shell agent, ~2h)
2. EngineState snapshot + EngineHandle, local only (engine agent, ~½ wave)
3. the seven file extractions, in the §4.2 order   (shell agent, rest of wave)
4. everything else, in parallel
```

Steps 1 and 2 can overlap: step 2 touches the 20 poll sites and the 7 command
sites, step 1 touches only headers and the preamble.

---

## 8. What this does not fix

Stated so nobody is surprised:

- **Build time.** All eight TUs include `app.h`, so touching it still rebuilds
  everything. Incremental builds get *worse* per-TU (each now re-parses the
  header). The win is human parallelism, not compile parallelism — though
  `make -j8` on a full build does get genuinely faster, since 3 728 lines in
  one TU currently serialise.
- **Coupling.** Eight files still share one class with ~70 members. Header
  strategy (c) is the fix and it is deliberately deferred.
- **`app.h` as the merge point.** It shrinks from 790 to ~450 lines and becomes
  append-only by convention, which is a big improvement over a shared 3 728-line
  `.cpp` — but it is still a shared file, and a wave with eight agents will see
  conflicts there. They will be one-line conflicts.
- **`ses_`.** 181 references across seven TUs. The session model is genuinely
  global to the app and no file boundary changes that.

---

## 9. Decomposition done (wave 7)

Executed exactly as planned. `app.cpp` (3 728 lines) became eleven files, all
moves verbatim (no logic edits, blame follows under `git blame -C`):

| file | ~lines | owns |
|---|---|---|
| `session.h` | 340 | the model (was app.h 1–345) |
| `app.h` | 469 | `class App` only; includes `session.h` |
| `app_internal.h` | 100 | `namespace lay`, the four helpers + `kReturnLetter`/`kSendUndo`/`kReturnPlaceholder`/`kArrowGesture` as `inline`, and the `UiKind` enum |
| `app.cpp` | 526 | shell: ctor/dtor, init/shutdown/run, frame, shortcuts, kbd piano, previews, `visibleRoll` |
| `app_engine.cpp` | 531 | send/push*/pumpEngineEvents, setTempo/togglePlay, recording |
| `app_project.cpp` | 417 | uids, (de)serialize, adoptSession, open/save |
| `app_undo.cpp` | 391 | undo/redo + `debugUndoSelfTest` |
| `app_session.cpp` | 776 | session view, mixer, drag/drop + `drawDragGhost`, clip helpers |
| `app_devices.cpp` | 501 | chain owners + add/remove/publish + DEVICES tab |
| `app_detail.cpp` | 291 | waveform, detail panel, clip detail (hosts `PianoRoll`) |
| `app_chrome.cpp` | 399 | control bar, file browser, status bar, arrangement |

Header split kept `project.cpp`, `pianoroll.h/.cpp` and `main.cpp` compiling
untouched (`app.h` includes `session.h`, so every downstream `#include "app.h"`
still sees `Session`). `UiKind` lives in `app_internal.h` as the single kind
registry. Seams preserved by co-location per §5: the `undoGesture_` latch stays
written only in `app.cpp`'s `frame`; `drawDragGhost` sits with its resolution in
`app_session.cpp`; `chainOwner`/add/removeDevice stay together in
`app_devices.cpp`; `selectTrack`/`selectChainOwner` stay in `app_session.cpp`.
Verified: `NXTAKT_DEBUG_UNDO` self-test prints "12 edits undone and redone
cleanly" and the headless screenshot is byte-identical (md5 unchanged) before
and after.
