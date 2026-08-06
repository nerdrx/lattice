// <sndfile.h> — compile-only stub for the Windows cross build. See README.md.
//
// Surface is exactly what src/audio/sample.cpp references, no more:
//   SF_INFO{frames,samplerate,channels}, SNDFILE, sf_count_t, SFM_READ,
//   sf_open, sf_strerror, sf_readf_float, sf_close.
// Names and member spellings follow upstream libsndfile so sample.cpp compiles
// unmodified; verify-shim static-asserts the members it relies on.
#ifndef LAT_SHIM_SNDFILE_H
#define LAT_SHIM_SNDFILE_H

#include <stdint.h>

#include "lat_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t sf_count_t;

// Opaque upstream; opaque here. Only ever held as a pointer.
typedef struct SNDFILE_tag SNDFILE;

typedef struct SF_INFO {
    sf_count_t frames;
    int samplerate;
    int channels;
    int format;
    int sections;
    int seekable;
} SF_INFO;

// Upstream value. Nothing here depends on it numerically, but keeping the real
// constant means a future switch to the real header changes no behaviour.
#define SFM_READ 0x10

static inline SNDFILE* sf_open(const char* path, int mode, SF_INFO* info) {
    (void)path; (void)mode; (void)info;
    lat_shim_die("sf_open");
}

static inline const char* sf_strerror(SNDFILE* f) {
    (void)f;
    lat_shim_die("sf_strerror");
}

static inline sf_count_t sf_readf_float(SNDFILE* f, float* p, sf_count_t n) {
    (void)f; (void)p; (void)n;
    lat_shim_die("sf_readf_float");
}

static inline int sf_close(SNDFILE* f) {
    (void)f;
    lat_shim_die("sf_close");
}

#ifdef __cplusplus
}
#endif

#endif  // LAT_SHIM_SNDFILE_H
