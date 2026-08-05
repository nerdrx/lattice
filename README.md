# Lattice

[![CI](https://github.com/nerdrx/lattice/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/nerdrx/lattice/actions/workflows/ci.yml)

A native, session-first DAW for Linux. Written from scratch in C++20 — no
framework, no toolkit, no runtime. Wayland-first, Windows secondary.

The workflow is Ableton Live's: a grid of clips you launch against a global
tempo grid, quantised to musical boundaries. The architecture is not Live's,
and the differences are the point — see **Why this exists** below.

```
make          # build            -> build/lattice
make test     # headless checks  (engine unit tests + render + plugin scan)
make tools    # gen_demo, render, pitch_check, plugin_scan
make config   # show detected Wayland protocols
```

## Current state

**Works today**

- Realtime engine: lock-free, allocation-free, no locks on the audio thread.
  Clip launch is sample-accurate — sub-block splitting means a launch lands on
  the exact frame of its quantum, not the start of the next buffer.
- Session View: clip grid, scenes, per-track stop, scene launch, stop-all,
  track headers, mixer strip (pan, fader, peak meters, mute/solo/arm), master
  strip, clip detail with waveform, file browser with drag-to-slot.
- Warping. **Beats** mode is a two-grain overlap-add stretcher that follows the
  session tempo while preserving pitch; **Repitch** transposes with the tempo;
  **Off** ignores it. Verified: a 55.0 Hz bass reads 55.11 Hz at 120 BPM and
  56.21 Hz at 180 BPM under Beats, and 82.62 Hz under Repitch (exactly 1.5×).
- Audio backends: JACK (auto-connects playback *and* capture), ALSA fallback
  with capture.
- Plugin hosting in the signal path: per-track device chains with a filterable
  browser, bypass, and parameter knobs. **LV2** via lilv (400+ plugins on a
  stock Arch box, ASan-clean) and **CLAP**, both with working note input —
  LV2 instruments sound via real atom sequences. VST3 not started (licensing).
- Stock devices: `Saturator` (compensated tanh shaper) and `Pulse` (8-voice
  PolyBLEP morph synth) ride the same PluginInstance machinery as everything
  else.
- **Audio recording**: arm a track, click an empty slot — quantized start and
  stop on the launch grid, takes come back as warped clips at the session
  tempo, with pre-chain input monitoring.
- **MIDI in**: ALSA-sequencer port (`aconnect <source> <Lattice:in>`) plus an
  FL-Studio-style computer keyboard (Ctrl+Shift+K; Z-row and Q-row are two
  octaves, PgUp/PgDn shifts), routed per-block to note-capable devices on
  armed tracks — with Live-style auto-arm on select.
- **MIDI clips + piano roll**: double-click an empty slot on an instrument
  track for a pattern; fold, 1/16 grid, drag/resize/delete, velocity lane,
  live playhead. Sample-accurate playback with no stuck notes across loop
  wraps and clip switches. MIDI takes record into slots on the launch grid.
  Notes serialize as plain `note <beat> <len> <pitch> <vel>` lines — a melody
  is a diffable text block.
- **Generative clips**: launch probability and follow actions (Stop / Again /
  Next / Prev / First / Random), scheduled through the same quantized path and
  deterministic under offline render.
- Project format v2: line-oriented plain text, byte-identical round-trip,
  stable per-entity uids, device chains with parameter snapshots; v1 files
  still load.
- Offline render: deterministic, no device and no GUI — CI renders the demo
  set on every push.
- Process-split groundwork: shared-memory SPSC rings + crash-orphan reaping
  (`src/ipc/`, 54 tests, ~10M msgs/sec), design in `docs/PROCESS-SPLIT.md`.

**Not done yet**

- Arrangement View is a navigable placeholder — no recording or timeline edits.
- No undo, no automation (the parameter address space is reserved:
  `docs/PARAM-ADDRESS.md`), no sends/returns, no time-signature changes, no
  overdub. The offline renderer does not yet materialize plugins, so MIDI
  clips render silent outside the app.
- The engine/GUI process split is designed and transport-tested but not yet
  integrated.
- Windows backends are written but have never been compiled or run — see
  `docs/PORTING.md`.

## Why this exists

"Ableton but on Linux" is a feature, not a reason; Bitwig already fills that
slot. The architectural bets that make this a different tool:

1. **The engine is meant to outlive its GUI.** The audio side already talks to
   the UI only through a lock-free command ring and atomics — no shared objects,
   no pointers into GUI memory. Promoting that boundary to a process boundary
   gives a DAW whose interface can crash mid-set without dropping a sample.
2. **Sets are text.** `.lattice` is line-oriented and diffable. Branch an
   arrangement, merge two mixes, review a track, generate sets programmatically.
3. **Headless is first-class.** `build/render` drives the whole engine with no
   window and no audio device, and `make test` runs the entire audio path in CI.
   The same binary is an installation runtime or a Pi groovebox.
4. **Deterministic render.** No allocation and sample-accurate scheduling mean a
   render is reproducible frame-for-frame across machines.
5. **GPU-native UI.** Everything is SDF quads in one shader, so the interface is
   resolution-independent and runs at hundreds of fps. The renderer has an
   explicit foreign-pass fence, so a plugin editor or a 3D/spectral view can
   render into an FBO and be composited inline.

## Layout

```
src/core/     types, lock-free ring, project format
src/audio/    engine (RT), sample loading, JACK/ALSA/WASAPI backends
src/gfx/      batched SDF renderer, FreeType atlas, palette
src/ui/       window backends (Wayland/X11/Win32), widgets, app + views
src/plugin/   format-agnostic host, LV2 and CLAP backends
tools/        gen_demo, render, pitch_check, plugin_scan, headless_test.sh
tests/        engine_test, fake_clap_plugin
```

## Testing without a visible window

`tools/headless_test.sh` runs the app inside a headless gamescope compositor and
captures a screenshot, so UI checks never open a window on your desktop:

```bash
tools/headless_test.sh -o /tmp/shot.png -- ~/Music/"Lattice Demo"/demo.lattice
tools/headless_test.sh --wayland -o /tmp/shot.png    # exercise the native path
```

Without `--wayland` the child gets XWayland and takes the X11 backend; with it,
gamescope exposes its own Wayland socket. Both paths are worth testing.

Every push and pull request runs [the CI
workflow](.github/workflows/ci.yml): it builds the full application including
the Wayland backend, runs the whole headless suite (`make test` — engine unit
tests, lock-free IPC tests, project round-trip, a render that fails if it comes
out silent, and a plugin scan), and then renders the demo set. All four scenes
plus a 16-bar pass are rendered to FLAC and uploaded as an artifact, with the
peak and RMS of each printed into the run summary — the CI does not just compile
this DAW, it plays its output and hands you the audio.

## Environment

| Variable | Effect |
|---|---|
| `LATTICE_BACKEND` | `wayland` or `x11` — force a window backend |
| `LATTICE_AUDIO` | `jack` or `alsa` — force an audio backend |
| `LATTICE_SCALE` | override UI scale, e.g. `1.5` |
| `CLAP_PATH` | extra CLAP search paths |

## Keys

| | | | |
|---|---|---|---|
| `Space` | play / stop | `Esc` | stop all clips |
| `Tab` | Session / Arrangement | `Enter` | launch selected clip |
| Arrows | move selection | `Del` | clear selected clip |
| `M` | metronome | `Ctrl+S` | save |
| `Ctrl+B` | browser | `Ctrl+D` | clip detail |
| `Ctrl+T` | add track | `Ctrl+Enter` | add scene |

## Dependencies

Build: `gcc`/`clang` with C++20, `make`, `pkg-config`.
Libraries: `libjack`, `alsa-lib`, `libsndfile`, `libsamplerate`, `freetype2`,
`fontconfig`, `libGL`, `libX11`, `lilv`.
Wayland (optional but preferred): `wayland-client`, `wayland-egl`,
`wayland-cursor`, `egl`, `libxkbcommon`, `wayland-scanner`, plus the xdg-shell
XML — from `wayland-protocols`, or Qt6's copy, which `make config` will find.
CLAP headers are vendored at `vendor/clap` (MIT).

No GLEW: it is built against GLX on most distros and refuses to initialise under
the EGL context the Wayland backend creates. libGL exports the core profile
directly, so the prototypes are declared and that is that.

## Licence

GPL-3.0-or-later — see [LICENSE](LICENSE). This also keeps the VST3 door open:
Steinberg's SDK is dual GPLv3/commercial, so a GPL host can vendor it without a
signed agreement. The vendored CLAP headers are MIT and compatible.
