# Wave 7 contract decisions (orchestrator)

Answers to the ten open questions in `automation_design.md`. These are binding
for wave-7 briefs; where a decision costs something, the cost is named.

1. **Header edits.** The orchestrator (main loop) makes ALL `engine.h` / `host.h`
   edits itself, before any wave-7 agent launches — same pattern as waves 3 and 6.
   Headers are frozen to agents again the moment the wave starts.

2. **Clip envelopes are restricted to the clip's own track.** Confirmed as
   designed. Cross-track targets from a clip envelope are an ordering hazard for
   a feature nobody asked for; arrangement lanes (deferred) are where global
   automation belongs.

3. **Block-size dependence accepted** (bounded 1e-3). Per-sample evaluation buys
   inaudible accuracy for a per-sample cost on every automated parameter. The
   render gate pins a fixed block size, so determinism — the property we actually
   sell — is unaffected. Document the bound in the render tool's `--help`.

4. **`send:` spelling confirmed** as `t:<uid>/send:<idx>` — indexed, because
   returns are a fixed array and have no uid worth spending. Goes into
   PARAM-ADDRESS.md when v5 lands.

5. **Values in the target's own units** with `AutoXform`. Confirmed, and the
   rejection reason is the right one: normalized storage would silently rescale a
   user's automation when a plugin's range changes, where units visibly clamp.
   Visible wrongness beats invisible wrongness.

6. **Protocol v4 gets its own daemon wave (7e).** Confirmed. A deliberate
   incompatibility on a shipped protocol deserves its own gate, its own tests and
   its own commit — not a rider on a feature wave.

7. **Override is runtime-only, cleared by clip relaunch.** Confirmed, matching
   Live. Add a one-line status hint the first time a user moves an automated knob
   ("automation overridden until relaunch") — the surprise is cheap to defuse.

8. **Bounds confirmed**: 16 lanes/clip, 4096 points/clip, 64 CLAP RT params.
   `kMaxClipLanes` being a fixed wire width is exactly why 16 (not 8) — the
   regret asymmetry favours the larger number.

9. **The automation lane replaces the velocity lane** in the piano roll, with a
   selector where the "VEL" gutter label is today. Live's choice, and the roll's
   geometry already assumes one lane.

10. **Automation Arm is its own control**, not implied by record-arm — a small
    toggle immediately right of the record circle in the control bar, using the
    same `accentHi`-on-dark treatment as the KBD chip. Recording notes and
    recording knob moves are genuinely different intents.

## Also folded into wave 7 from the audits

- **IPC F1–F4 (fail-closed violations)** are wave-7's first commit, before any
  feature work: bound `frames` like `noteCount` already is, validate the derived
  `rate`, and load-once the two re-read fields (`pool.h:338-348`,
  `latticed.cpp:938`). F6's missing magic check and null guard ride along.
- **F5/F8/F9 (handshake trust, pool liveness key, ftruncate/SIGBUS)** are
  design-level and get a documented threat-model section rather than a rushed
  fix; F8's wrong liveness key (`ShmHeader::creatorPid` on a region designed to
  outlive its creator) is a real bug and gets fixed with it.
- **The `app.cpp` decomposition runs FIRST in wave 7**, before automation UI.
  Every wave since the first has been throttled by one agent owning that file;
  paying it down once multiplies every wave after.
