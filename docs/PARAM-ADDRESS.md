# Canonical parameter addresses

One naming scheme for every controllable value in a set. MIDI-learn,
automation, OSC remote control, undo and the process-split param table all
reference parameters through these addresses — never through raw indices,
which break the moment a track or device moves.

## Grammar

```
address   := scope ( "/" segment )*
scope     := "master" | "t:" UID | "s:" UID
segment   := "vol" | "pan" | "mute" | "solo" | "arm"
           | "dev:" UID "/p:" PARAMID
           | "clip:" UID "/" clipfield
clipfield := "gain" | "prob" | "follow" | "followBeats" | "warp" | "loop"
UID       := decimal u64, the entity's Session uid (stable, serialized)
PARAMID   := decimal u32, ParamInfo::id (backend-defined, stable per plugin)
```

## Examples

```
master/vol                  the master fader
t:7/vol                     volume of the track with uid 7
t:7/dev:12/p:3              param id 3 on device uid 12 on track uid 7
t:7/clip:31/gain            clip gain of clip uid 31
s:4/launch                  (reserved) scene-launch trigger target
```

## Rules

- Addresses use **uids**, not positions. Deleting and re-adding an entity
  yields a *new* uid; dangling addresses resolve to nothing and must fail
  soft (a MIDI mapping to a deleted device is silently inert, never a crash).
- Addresses are ASCII, no spaces, `/`-separated — safe for the text project
  format, OSC paths (`/nxtakt/t:7/vol`), and log lines as-is.
- `ParamInfo::id` is the plugin backend's stable identifier (LV2 port index,
  CLAP param id). It survives sessions for the same plugin version; a plugin
  update that renumbers params invalidates mappings, which is the plugin's
  fault, not ours.
- Resolution lives GUI-side (uid -> current index) — the engine keeps working
  in hot indices; addresses are resolved to indices at mapping time and
  re-resolved on any structural change.

## Status

Wave 2 reserves the scheme (uids exist on every entity and serialize).
First consumers: MIDI-learn and automation (wave 3+).
