# tools/mingw-shim — declarations, not implementations

The Windows cross build (`Makefile.mingw`) compiles `tests/engine_test.cpp`,
which `#include`s `src/audio/sample.cpp` to reach `detectTransients()` — the
transient detector the warp-marker tests exercise. That function is pure DSP
over an in-memory buffer. It needs nothing but `<cmath>`.

It merely happens to share a translation unit with `loadSample()`, which reads
a file with **libsndfile** and resamples with **libsamplerate**. Neither is
packaged cross-built for mingw on any runner image, so the whole cross build
stopped compiling the moment that `#include` landed — for a dependency the
Windows suite never calls.

This directory supplies just enough of `<sndfile.h>` and `<samplerate.h>` for
that TU to *compile*. It is on the include path for the mingw build only; the
native Linux build uses the real headers via `pkg-config` and is untouched.

## These stubs cannot fake a passing test

Every entry point aborts:

```c
static inline SNDFILE* sf_open(const char*, int, SF_INFO*) {
    lat_shim_die("sf_open");
}
```

`lat_shim_die` prints which function was reached and calls `abort()`. So the
only two outcomes are the honest ones:

* the Windows suite never touches file I/O — which is the case today — and the
  stubs are dead code that exists so the compiler has a declaration; or
* someone adds a test that does load a file, and it dies immediately with
  `nxtakt mingw shim: sf_open is a stub`, rather than quietly reporting a pass
  for work that never happened.

There is no third outcome where a stub returns a plausible-looking value.

## The layout must match, and that is checked

`sample.cpp` assigns `SRC_DATA` members by name and reads `SF_INFO::frames`,
`::samplerate` and `::channels`, so the field names here have to match upstream.
`make -f Makefile.mingw verify-shim` static-asserts the members these stubs
promise, so a rename upstream fails the build with a message instead of a
mysterious compile error inside `sample.cpp`.

## Deleting this directory

Delete it the day apt (or a vendored sysroot) ships cross-built libsndfile and
libsamplerate — the workflow already prints, on every run, whether that has
happened. Drop `-Itools/mingw-shim`, link the real libraries, and everything
here becomes unreachable. Nothing else depends on it.
