# Porting NxTakt

Linux is the primary target. Windows is secondary. As of the `windows-cross`
CI job the **engine is tested on Windows** — the real test suite, cross-built
with mingw-w64 and executed by Wine on every push — while the GUI is still not
buildable end to end.

That is a narrow claim, so here is exactly how narrow.

## Status

| Area | State |
|---|---|
| `src/audio/engine.cpp`, `src/core/common.cpp`, `tests/engine_test.cpp` | **Tested.** Cross-compiled to `engine_test.exe` and run under Wine in CI. All 199 checks pass, same as Linux. |
| `src/ui/window_win32.cpp` | **Compiles.** Built by a Windows-targeting compiler on every push (`-c`, never linked). Never executed — no window station on a runner. |
| `src/audio/backend_win32.cpp` | **Compiles.** Same deal. Never executed — no audio endpoint on a runner. |
| `src/ui/window.cpp`, `src/ui/window_backend.h` | **Fixed.** Backend selection now has a `_WIN32` branch; the Linux path is unchanged. |
| `src/core/project.cpp`, `src/core/ring.h`, `src/audio/sample.cpp`, `src/gfx/renderer.cpp`, `src/ui/widgets.cpp`, `src/plugin/host.cpp` | **Untouched, believed portable.** Standard C++ and libc only; nothing has compiled them for Windows yet. |
| `src/ui/app.cpp`, `src/gfx/font.cpp` | **Untouched, known non-portable.** POSIX directory walking, `$HOME`, `/usr/share/fonts`. |
| `src/plugin/lv2_host.cpp` | **Untouched, known non-portable.** `dlfcn.h` + a Linux lilv. |
| `src/ipc/*`, `tests/ipc_test.cpp` | **Untouched, out of scope.** POSIX shared memory. |
| ASIO, WASAPI exclusive mode, WinMM MIDI | **Not written.** |

"Compiles" is worth less than "tested" and worth a great deal more than what
these files had before, which was nothing: they were written against the Win32
API by eye on a machine with no cross-compiler, and had only ever been
confirmed to be a valid *empty* translation unit under Linux `g++`.

## Where the platform lives

Everything platform-specific is behind one of two interfaces: `IWindowBackend`
(`src/ui/window_backend.h`) and `AudioBackend` (`src/audio/backend.h`). Files
ending in `_win32.cpp` guard their entire contents with `#if defined(_WIN32)`,
so they compile to an empty translation unit on Linux and need no build-system
filtering.

| File | Platform | Notes |
|---|---|---|
| `src/ui/window_x11.cpp` | Linux | X11 + GLX |
| `src/ui/window_wayland.cpp` | Linux | Wayland + EGL, optional at build time |
| `src/ui/window_win32.cpp` | Windows | Win32 + WGL, OpenGL 3.3 core |
| `src/audio/backend.cpp` | Linux | JACK (also PipeWire) + ALSA fallback |
| `src/audio/backend_win32.cpp` | Windows | WASAPI shared mode |
| `src/plugin/lv2_host.cpp` | Linux | lilv + `dlfcn.h` |

The factory declarations in `window_backend.h` are now themselves per-platform.
Declaring `createX11Backend()` unconditionally is what let `window.cpp` call it
from a Windows build and only find out at link time; the header now exposes
`createWin32Backend()` under `_WIN32` and the X11/Wayland pair otherwise, so
the mistake is a compile error rather than a link error.

## Building it

```
make -f Makefile.mingw config     # toolchain, flags, thread model
make -f Makefile.mingw            # engine_test.exe + the two Win32 objects
make -f Makefile.mingw check      # ... then run the suite under wine
```

Needs `mingw-w64` and `wine` (64-bit; nothing here is 32-bit). No Windows
libraries, because the headless subset does not use any. `Makefile.mingw` has a
`verify-sources` target that asserts its file list and warning flags still match
the native `Makefile`, so the two builds cannot drift into testing different
code; CI runs it before building.

## What the first real cross-compile found

Worth recording, because the ratio is the interesting part.

**The hand-written Win32 code was essentially correct.** Both
`window_win32.cpp` and `backend_win32.cpp` compile with `-Wall -Wextra` and
produce zero diagnostics. Two conservative changes were made anyway:

* `SetProcessDpiAwarenessContext((HANDLE)-4)` now goes through `INT_PTR`. A
  bare `int` → 64-bit-pointer cast is implementation-defined and warns under
  `-Wint-to-pointer-cast` on some targets; widening first makes the sign
  extension explicit.
* `backend_win32.cpp` includes `<mmreg.h>` directly. `WIN32_LEAN_AND_MEAN`
  keeps `mmsystem.h` out of `windows.h`, and whether `audioclient.h` pulls
  `mmreg.h` in by itself differs between the Windows SDK and mingw-w64 —
  `WAVEFORMATEXTENSIBLE` and the `WAVE_FORMAT_*` tags used by `classify()` live
  there.

**The actual blocker was in the build, not the code.** `sizeof(lat::Engine)` is
about 2.35 MB — the realtime path never allocates, so every per-track buffer is
a fixed-size member — and `tests/engine_test.cpp` declares `Host h;`, which
embeds an `Engine` by value, as a *local* in roughly thirty test functions.
Linux hands a thread 8 MB of stack and nobody ever noticed. A PE gets 2 MB of
stack reserve by default under mingw-w64, and Windows itself defaults to 1 MB,
so the first test overflowed the stack and the process died with a bare access
violation inside `Engine::prepare` — after printing the banner and not one
result line.

`Makefile.mingw` links with `-Wl,--stack,16777216`, and CI reads
`SizeOfStackReserve` back out of the PE header so that a dropped flag is
reported as a dropped flag rather than as a mystery crash. Reproduce the
failure natively with:

```
ulimit -s 2048; ./build/engine_test     # SIGSEGV
ulimit -s 16384; ./build/engine_test    # 199 passed
```

The link flag is the right fix for the test binary. It is *not* a fix for the
application: `src/main.cpp` will have the same problem the moment it puts an
`Engine` anywhere near the stack, and 2 MB is also the default for every thread
the app creates. Either keep `--stack` in the Windows link for the app too, or
give `Engine` a heap-allocating factory. The second is better and is the one to
do if the GUI is ever built.

Also worth knowing: the cross build defines `__USE_MINGW_ANSI_STDIO=1`. Without
it `printf` resolves to msvcrt's implementation, which does not understand
`%zu` and is inconsistent about `%lld` — and the suite prints frame counts and
buffer offsets with exactly those in the text of ~30 assertions. That would not
have failed the run, it would have quietly printed garbage next to `PASS`.

## Windows dependencies

Four libraries have no Windows system equivalent. None of them are needed for
the headless subset; all four stand between here and a GUI build.

| Library | Used by | Needed for |
|---|---|---|
| freetype | `gfx/font.cpp` | GUI |
| glew | `gfx/renderer.cpp`, `ui/app.cpp` | GUI |
| libsndfile | `audio/sample.cpp` | GUI, and `tools/render` |
| libsamplerate | `audio/sample.cpp` | GUI, and `tools/render` |

Everything else comes from the OS: `opengl32`, `gdi32`, `ole32`, `avrt`,
`ksuser`, `winmm`. Not needed on Windows at all: jack, alsa, x11, xcursor,
fontconfig, wayland-\*, egl, xkbcommon.

### Recommendation: vcpkg, `x64-mingw-static`, consumed via pkg-config

Three options were on the table — vendor the sources, vcpkg, or an MSYS2
sysroot. **Use vcpkg.** The reasoning is almost entirely about one library:

* **libsndfile is the whole problem.** freetype, glew and libsamplerate are
  each a self-contained afternoon of vendoring. libsndfile is not: statically,
  it drags in FLAC, ogg, vorbis, vorbisenc, opus and mpg123, and the link order
  among them is the kind of thing you get wrong twice before you get it right.
  vcpkg resolves that transitive closure and emits `lib/pkgconfig/*.pc` files
  that already carry it in `Libs.private`.
* **It cross-builds on Linux with the same toolchain CI already has.** The
  `x64-mingw-static` triplet targets `x86_64-w64-mingw32`, so one apt install
  covers both the dependency build and NxTakt itself. No Windows runner, no
  second CI image.
* **It needs almost no build-system change.** `Makefile.mingw` already takes a
  `WIN_DEPS=/path/to/sysroot` with `include/` and `lib/` under it, which is
  exactly the shape of `vcpkg/installed/x64-mingw-static`. Swapping the
  hardcoded `-lsndfile -lsamplerate` for
  `PKG_CONFIG_LIBDIR=$(WIN_DEPS)/lib/pkgconfig x86_64-w64-mingw32-pkg-config
  --libs --static sndfile samplerate` is a two-line change, and is what picks
  up the FLAC/ogg/vorbis/opus closure automatically. NxTakt has no CMake,
  which is the usual reason to reach for
  vcpkg — here it is being used as a plain cross-build recipe collection, which
  it is perfectly good at.
* **MSYS2 was rejected** because pacman only runs on Windows, so it needs a
  `windows-latest` runner and a second toolchain that shares nothing with the
  Linux job.
* **Hand-vendoring was rejected** for libsndfile only. If the GUI ever drops
  the sample loader (unlikely) or moves to a single-format WAV reader (quite
  plausible, and about 200 lines), vendoring the remaining three becomes the
  better answer — fewer moving parts, no third-party package manager in the
  build. Revisit then.

CI cost is the obvious objection: a cold vcpkg build of that set is around ten
minutes. Cache `vcpkg/installed` keyed on the manifest hash and it is seconds
after the first run.

Whichever is chosen, `Makefile.mingw`'s optional `render.exe` target is the
cheapest place to prove it works: it needs exactly libsndfile and
libsamplerate, no window and no font.

```
make -f Makefile.mingw WIN_DEPS=/path/to/sysroot build-mingw/render.exe
```

The `windows-cross` job logs `apt-cache search mingw | grep -E 'sndfile|...'`
on every run, so if Debian ever starts cross-packaging these, the CI log will
say so and this whole section gets shorter.

## Plugin formats

| Format | Linux | Windows | State |
|---|---|---|---|
| LV2 | native | rare | Implemented (`plugin/lv2_host.cpp`). Uses `dlfcn.h`; needs a lilv/Windows port before it cross-compiles. |
| CLAP | yes | yes | Implemented (`plugin/clap_host.cpp`). Header-only ABI, identical on both platforms; the plugin loader is the only part that needs a `LoadLibraryW` path. |
| VST3 | some | dominant | Not implemented. `TODO(vst3)` in `plugin/host.h`. Steinberg SDK is dual GPLv3/proprietary and cannot be vendored here. |
| AU | — | — | macOS only, out of scope. |

`PluginRegistry::scan()` dispatches on `PluginDesc::format`, so adding a format
touches only the entry points in `namespace lat::detail`.

Audio drivers are a separate axis. Windows currently gets WASAPI shared mode
(~10 ms round trip — fine for playback, not for tracking). ASIO is the next
step and is what every Windows DAW ships; the SDK is licensed and cannot be
vendored, so it has to be an opt-in build. See the `TODO(asio)` block at the
top of `src/audio/backend_win32.cpp`.

## Still to do

Roughly in the order you hit them building a real Windows GUI.

1. **Dependencies.** See above. Nothing else on this list can be tested until
   something links.
2. **`app.cpp`** — the sample browser and `$HOME` lookup need a Win32 path
   (`SHGetKnownFolderPath`, `FindFirstFileW`) or a small POSIX shim.
3. **`font.cpp`** — replace the `/usr/share/fonts` list with `%WINDIR%\Fonts`
   (Segoe UI / Arial), or bundle a font in `assets/` and drop the search
   entirely. Bundling is the better answer on both platforms.
4. **`lv2_host.cpp` will not compile under mingw** — `dlfcn.h` and a Linux
   lilv. Dropping the file only moves the failure to the link step, because
   `detail::scanLV2` / `instantiateLV2` are called unconditionally from
   `plugin/host.cpp`. The fix is a stubbed `lv2_host_stub.cpp` for the Windows
   build; CLAP already covers the plugin story there.
5. **Stack sizes.** See the `--stack` discussion above. The application binary
   has the same 2 MB default the test binary had.
6. **No MIDI input on Windows.** There is no WinMM/WinRT MIDI path.
7. **No ASIO, no WASAPI exclusive mode.** Latency is whatever the shared mix
   engine gives you.
8. **Multi-channel output is stereo-in-a-wider-buffer.** `writeOut()` silences
   everything past L/R on surround endpoints instead of folding down.
9. **No device-change recovery.** `AUDCLNT_E_DEVICE_INVALIDATED` (default
   device switched, USB interface unplugged) kills the render thread and audio
   stops for good until restart. Needs an `IMMNotificationClient` and a
   re-Initialize path.
10. **No packaging.** No icon, no `.rc` manifest, no installer. Without an
    application manifest the DPI awareness relies entirely on the runtime
    `SetProcessDpiAwarenessContext` call in `window_win32.cpp`.
11. **`src/ipc` has no Windows implementation.** `shm.h` is POSIX `shm_open` +
    `mmap` + process-shared pthread mutexes. The Win32 equivalent is
    `CreateFileMappingW`/`MapViewOfFile` plus named mutexes — a second
    implementation, not a build fix, which is why `ipc_test` is excluded from
    `Makefile.mingw` rather than patched into it.
12. **Timer resolution.** Nothing calls `timeBeginPeriod`; if the UI loop's
    sleep granularity turns out to matter, that is where to look.
