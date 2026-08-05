# Porting Lattice

Linux is the primary target. Windows is secondary and is **not yet buildable
end to end** — see "Not done" at the bottom.

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

Portable but **not currently clean** on Windows:

| File | Problem |
|---|---|
| `src/ui/window.cpp` | Backend selection is hardcoded to X11/Wayland; no Win32 branch |
| `src/ui/app.cpp` | `dirent.h`, `sys/stat.h`, `unistd.h`, `pwd.h`, `$HOME`, `/usr/share/sounds` |
| `src/gfx/font.cpp` | `unistd.h` `access()`, hardcoded `/usr/share/fonts/...` paths |

Genuinely portable, no changes expected: `src/core/*` (`common`, `ring.h`,
`project.cpp` — stdio only), `src/audio/engine.cpp`, `src/audio/sample.cpp`,
`src/gfx/renderer.cpp`, `src/ui/widgets.cpp`, `src/plugin/host.cpp`.
`src/main.cpp` uses `<xmmintrin.h>`/`<pmmintrin.h>` for FTZ/DAZ, which is fine
on x86-64 Windows.

## Windows dependencies

Four libraries have no Windows system equivalent and must be supplied:

| Library | Used by | Sourcing |
|---|---|---|
| freetype | `gfx/font.cpp` | **vendor** |
| glew | `gfx/renderer.cpp`, `ui/app.cpp` | **vendor** (`-DGLEW_STATIC` + `glew32s`, or ship `glew32.dll`) |
| libsndfile | `audio/sample.cpp` | **vendor** |
| libsamplerate | `audio/sample.cpp` | **vendor** |

There is no system pkg-config on a Windows target. `Makefile.mingw` tries
`x86_64-w64-mingw32-pkg-config` first (present if you build inside MSYS2 or
install a cross pkg-config), and otherwise falls back to a hand-populated
sysroot at `third_party/win32/{include,lib}`. Override with
`make -f Makefile.mingw WIN_DEPS=/path/to/sysroot`.

Everything else comes from the OS: `opengl32`, `gdi32`, `ole32`, `avrt`,
`ksuser`, `winmm`. Not needed on Windows at all: jack, alsa, x11, xcursor,
fontconfig, wayland-\*, egl, xkbcommon.

Cross build:

```
make -f Makefile.mingw config     # show what was detected
make -f Makefile.mingw            # -> build/win/lattice.exe
```

## Plugin formats

| Format | Linux | Windows | State |
|---|---|---|---|
| LV2 | native | rare | Implemented (`plugin/lv2_host.cpp`). Uses `dlfcn.h`; needs a lilv/Windows port before it cross-compiles. |
| CLAP | yes | yes | Not implemented. `TODO(clap)` in `plugin/host.h` and `host.cpp`. Cheapest next format: header-only ABI, permissive licence, identical on both platforms. |
| VST3 | some | dominant | Not implemented. `TODO(vst3)` in `plugin/host.h`. Steinberg SDK is dual GPLv3/proprietary and cannot be vendored here. |
| AU | — | — | macOS only, out of scope. |

`PluginRegistry::scan()` already dispatches on `PluginDesc::format`, so adding a
format touches only the three entry points in `namespace lat::detail`.

Audio drivers are a separate axis. Windows currently gets WASAPI shared mode
(~10 ms round trip — fine for playback, not for tracking). ASIO is the next
step and is what every Windows DAW ships; the SDK is licensed and cannot be
vendored, so it has to be an opt-in build. See the `TODO(asio)` block at the
top of `src/audio/backend_win32.cpp`.

## Not done

Nothing in this port has been run on Windows. No cross-compiler was available
on the machine where the Windows backends were written, so neither Windows
file has ever been compiled by a Windows-targeting toolchain — only confirmed
to be a valid empty TU under Linux `g++`.

Concrete blockers, roughly in the order you hit them:

1. **`window.cpp` will not link.** It calls `createX11Backend()`
   unconditionally, and `window_backend.h` does not declare
   `createWin32Backend()`. Both need a `#if defined(_WIN32)` branch:

   ```cpp
   // window_backend.h
   #if defined(_WIN32)
   IWindowBackend* createWin32Backend();
   #else
   IWindowBackend* createX11Backend();
   #endif
   ```

   with the matching branch at the top of `Window::create()`. These two files
   were deliberately left untouched.
2. **`app.cpp`** — the sample browser and `$HOME` lookup need a Win32 path
   (`SHGetKnownFolderPath`, `FindFirstFileW`) or a small POSIX shim.
3. **`font.cpp`** — replace the `/usr/share/fonts` list with `%WINDIR%\Fonts`
   (Segoe UI / Arial), or bundle a font in `assets/` and drop the search
   entirely. Bundling is the better answer on both platforms.
4. **`lv2_host.cpp` will not compile under mingw** — it needs `dlfcn.h` and a
   Linux lilv. It is deliberately still in `Makefile.mingw`'s source list
   rather than filtered out, because dropping it only moves the failure to the
   link step (`detail::scanLV2` / `instantiateLV2` are called unconditionally
   from `plugin/host.cpp`). The real fix is CLAP, or a lilv/Windows port, or a
   stubbed `lv2_host_stub.cpp` for the Windows build.
5. **No MIDI input on Windows.** There is no WinMM/WinRT MIDI path.
6. **No ASIO, no WASAPI exclusive mode.** Latency is whatever the shared mix
   engine gives you.
7. **Multi-channel output is stereo-in-a-wider-buffer.** `writeOut()` silences
   everything past L/R on surround endpoints instead of folding down.
8. **No device-change recovery.** `AUDCLNT_E_DEVICE_INVALIDATED` (default
   device switched, USB interface unplugged) kills the render thread and audio
   stops for good until restart. Needs an `IMMNotificationClient` and a
   re-Initialize path.
9. **No packaging.** No icon, no `.rc` manifest, no installer. Without an
   application manifest the DPI awareness relies entirely on the runtime
   `SetProcessDpiAwarenessContext` call in `window_win32.cpp`.
10. **Timer resolution.** Nothing calls `timeBeginPeriod`; if the UI loop's
   sleep granularity turns out to matter, that is where to look.
