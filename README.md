<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo.svg">
    <img alt="NxTakt" src="assets/logo-light.svg" width="84">
  </picture>
</p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/wordmark.svg">
    <img alt="NxTakt" src="assets/wordmark-light.svg" width="248">
  </picture>
</p>

<p align="center">
  <b>Session-first. Sample-accurate. Text on disk.</b><br>
  A native Linux DAW, written from scratch in C++20 — no framework, no toolkit, no runtime.
</p>

<p align="center">
  <a href="https://github.com/nerdrx/nxtakt/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/nerdrx/nxtakt/actions/workflows/ci.yml/badge.svg?branch=main"></a>
  <a href="https://github.com/nerdrx/nxtakt/actions/workflows/windows.yml"><img alt="Windows cross" src="https://github.com/nerdrx/nxtakt/actions/workflows/windows.yml/badge.svg?branch=main"></a>
  <a href="LICENSE"><img alt="Licence: GPL-3.0-or-later" src="https://img.shields.io/badge/licence-GPL--3.0--or--later-7700FF"></a>
</p>

<p align="center">
  <img alt="NxTakt Session View: an eight-track, sixteen-scene clip grid with the piano roll open on a MIDI clip" src="assets/hero.png" width="900">
</p>

The workflow is Ableton Live's: a grid of clips you launch against a global
tempo grid, quantised to musical boundaries. The architecture is not Live's,
and that is the point.

---

### Your set is text

A project is one line-oriented plain-text file with a byte-identical
round-trip and stable per-entity uids. A melody is eight `note <beat> <len>
<pitch> <vel>` lines. Diff a mix, branch an arrangement, review a track in a
pull request, generate a set from a script.

### The audio thread shares nothing

No allocation, no locks, no pointers into GUI memory: the interface reaches the
engine through a lock-free command ring and a block of atomics. Launches are
split inside the buffer, so a clip starts on the exact frame of its quantum
rather than at the top of the next block. `nxtaktd` already runs that engine
as its own process, playing clips out of a shared-memory sample pool.

### CI plays what it builds

Every push builds the whole application, runs the headless suite — 500+
assertions across the engine, the lock-free IPC layer and the daemon — and then
renders five passes of the demo set to FLAC, printing the peak and RMS of each
into the run summary. The pipeline does not just compile this DAW. It hands you
the audio.

---

## What it does

| | |
|---|---|
| **Session View** | Clip grid, scenes, per-track stop, scene launch, stop-all, mixer strip (pan, fader, peak meters, mute/solo/arm), master strip, file browser with drag-to-slot. |
| **Piano roll** | Fold, 1/16 grid, drag / resize / delete, multi-note selection, velocity lane, live playhead. Sample-accurate playback with no stuck notes across loop wraps or clip switches. |
| **MIDI in** | ALSA sequencer port plus an FL-Studio-style computer keyboard (`Ctrl+Shift+K`), routed per block to note-capable devices on armed tracks, with Live-style auto-arm on select. Overdub passes into a playing clip. |
| **Warping** | **Beats** is a two-grain overlap-add stretcher that follows the session tempo while preserving pitch; **Repitch** transposes with the tempo; **Off** ignores it. |
| **Recording** | Arm a track, click an empty slot. Quantised start and stop on the launch grid; takes come back as warped clips at the session tempo, with pre-chain input monitoring. |
| **Plugins** | Per-track device chains in the signal path — **LV2** via lilv and **CLAP**, both with working note input — plus a filterable browser, bypass and parameter knobs. 410 usable on a stock Arch box. |
| **Stock devices** | `Saturator` (compensated tanh shaper) and `Pulse` (8-voice PolyBLEP morph synth), riding the same machinery as every third-party plugin. |
| **Buses** | Post-fader sends into four return chains and a master chain, with plugin delay compensation aligning every parallel path into the master sum. |
| **Automation** | Clip envelopes on any track parameter or device knob, drawn in the piano roll on the notes' own time axis. Record a knob move while armed; the engine ramps within the block and never overwrites the value you set. |
| **Generative clips** | Launch probability and follow actions (Stop / Again / Next / Prev / First / Random), scheduled through the same quantised path and deterministic under offline render. |
| **Undo** | Snapshot history over the project serializer — including the audio of unsaved takes. Gestures coalesce into one entry per drag. |
| **Engine daemon** | `nxtaktd` hosts the transport and the sample pool in its own process, over shared-memory SPSC rings with crash-orphan reaping. |
| **Windows** | The headless engine cross-builds with mingw-w64 and its test suite runs under Wine on every push. |

<p align="center">
  <img alt="The device chain: a plugin browser beside Pulse and Calf Reverb on the keys track" src="assets/devices.png" width="820">
</p>

## Quickstart

```bash
make                    # build
make test               # headless suite: engine, IPC, daemon, render, plugin scan
make tools              # gen_demo, render, pitch_check, plugin_scan
make config             # show detected Wayland protocols

build/gen_demo ~/Music/Demo    # write a four-scene demo set
build/nxtakt ~/Music/Demo/demo.lattice
```

Audio comes up on JACK if it is running — playback *and* capture auto-connected
— and falls back to ALSA. `NXTAKT_AUDIO=alsa` forces the fallback.

## Under the hood

- **500+ assertions**, run headless on every push: the engine suite, the
  lock-free ring suite and the daemon suite, plus a render that fails if it
  comes out silent and a plugin scan that must find plugins.
- **Deterministic render.** No allocation and sample-accurate scheduling mean a
  render is reproducible frame-for-frame; delay compensation is proven by
  `cmp`-identical renders, and the zero-latency path is a hard bypass that
  leaves the old signal bit-exact.
- **Warping, measured.** A 55.0 Hz bass reads 55.11 Hz at 120 BPM and 56.21 Hz
  at 180 BPM under Beats, and 82.62 Hz under Repitch — exactly 1.5×.
- **410 LV2 plugins** discovered and instantiated on a stock Arch box,
  ASan-clean.
- **~10M messages/sec** across the shared-memory SPSC ring the process split
  runs on — design and wire format in [`docs/PROCESS-SPLIT.md`](docs/PROCESS-SPLIT.md).
- **Windows is tested, not assumed.** `engine_test.exe` is cross-compiled and
  then *run* under Wine in CI, gated on a floor of 199 passing checks, so "the
  Windows build works" is a claim about behaviour. Scope in
  [`docs/PORTING.md`](docs/PORTING.md).
- **GPU-native UI.** Everything is SDF quads in one shader: resolution
  independent, hundreds of frames per second, with an explicit foreign-pass
  fence so a plugin editor or a spectral view can be composited inline.
- Parameter addressing for automation, MIDI-learn and OSC is already specified:
  [`docs/PARAM-ADDRESS.md`](docs/PARAM-ADDRESS.md).

## Not done yet

- Arrangement View is a navigable placeholder — no recording, no timeline edits.
- No time-signature changes.
- VST3 is not started (licensing).
- The GUI still runs its own in-process engine; `nxtaktd` is not yet the
  shipping path.
- The Windows window and audio backends compile under a Windows-targeting
  compiler but have never driven a real window station or audio endpoint.

## Testing without a visible window

`tools/headless_test.sh` runs the app inside a headless gamescope compositor and
captures a screenshot, so UI checks never open a window on your desktop — every
screenshot on this page was taken that way:

```bash
tools/headless_test.sh -o /tmp/shot.png -- ~/Music/Demo/demo.lattice
tools/headless_test.sh --wayland -o /tmp/shot.png    # exercise the native path
```

Without `--wayland` the child gets XWayland and takes the X11 backend; with it,
gamescope exposes its own Wayland socket. Both paths are worth testing.

## Keys

| | | | |
|---|---|---|---|
| `Space` | play / stop | `Esc` | stop all clips |
| `Tab` | Session / Arrangement | `Enter` | launch selected clip |
| Arrows | move selection | `Del` | clear selected clip |
| `M` | metronome | `Ctrl+S` | save |
| `Ctrl+Z` | undo | `Ctrl+Shift+Z` | redo |
| `Ctrl+B` | browser | `Ctrl+D` | clip detail |
| `Ctrl+T` | add track | `Ctrl+Enter` | add scene |
| `Ctrl+Shift+K` | computer MIDI keyboard | | |

## Environment

| Variable | Effect |
|---|---|
| `NXTAKT_BACKEND` | `wayland` or `x11` — force a window backend |
| `NXTAKT_AUDIO` | `jack` or `alsa` — force an audio backend |
| `NXTAKT_SCALE` | override UI scale, e.g. `1.5` |
| `CLAP_PATH` | extra CLAP search paths |

The pre-rename `LATTICE_*` spellings are still read as a fallback, and sets
saved before the rename still load — they keep the `.lattice` extension, which
has not changed.

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

## Layout

```
src/core/     types, lock-free ring, project format
src/audio/    engine (RT), sample loading, JACK/ALSA/WASAPI backends
src/gfx/      batched SDF renderer, FreeType atlas, palette
src/ui/       window backends (Wayland/X11/Win32), widgets, app + views
src/ipc/      shared-memory rings, control region, sample pool
src/daemon/   nxtaktd, the engine as its own process
src/plugin/   format-agnostic host, LV2 and CLAP backends
tools/        gen_demo, render, pitch_check, plugin_scan, headless_test.sh
tests/        engine, ipc, daemon, internal devices, fake CLAP plugin
```

## Licence

GPL-3.0-or-later — see [LICENSE](LICENSE). This also keeps the VST3 door open:
Steinberg's SDK is dual GPLv3/commercial, so a GPL host can vendor it without a
signed agreement. The vendored CLAP headers are MIT and compatible.
