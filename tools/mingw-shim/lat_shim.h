// Shared abort path for the mingw compile shims. See README.md.
//
// Anything declared in this directory exists so a translation unit compiles,
// never so it runs. Reaching one of these bodies means the Windows suite has
// grown a test that genuinely needs the library, and the correct outcome is a
// loud immediate death -- not a return value the caller will mistake for real
// work.
#ifndef LAT_MINGW_SHIM_H
#define LAT_MINGW_SHIM_H

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// noreturn so callers with a non-void return type still compile without a
// bogus `return {}` after the call.
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static inline void lat_shim_die(const char* fn) {
    fprintf(stderr,
            "\n"
            "nxtakt mingw shim: %s is a stub with no implementation.\n"
            "  The Windows cross build has no libsndfile/libsamplerate; this\n"
            "  declaration exists only so src/audio/sample.cpp compiles for\n"
            "  detectTransients(). A test just called into file I/O, which\n"
            "  means either that test does not belong in the cross suite or\n"
            "  the cross build now needs the real library.\n"
            "  See tools/mingw-shim/README.md.\n",
            fn);
    fflush(stderr);
    abort();
}

#ifdef __cplusplus
}
#endif

#endif  // LAT_MINGW_SHIM_H
