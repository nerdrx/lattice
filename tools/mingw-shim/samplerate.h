// <samplerate.h> — compile-only stub for the Windows cross build. See README.md.
//
// Surface is exactly what src/audio/sample.cpp references:
//   SRC_DATA{data_in,data_out,input_frames,output_frames,output_frames_gen,
//   src_ratio}, SRC_SINC_MEDIUM_QUALITY, src_simple, src_strerror.
// Member spellings follow upstream libsamplerate; sample.cpp assigns them by
// name, so verify-shim static-asserts each one.
#ifndef LAT_SHIM_SAMPLERATE_H
#define LAT_SHIM_SAMPLERATE_H

#include "lat_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SRC_DATA {
    const float* data_in;
    float* data_out;
    long input_frames;
    long output_frames;
    long input_frames_used;
    long output_frames_gen;
    int end_of_input;
    double src_ratio;
} SRC_DATA;

// Upstream converter ids, kept faithful for the same reason as SFM_READ.
#define SRC_SINC_BEST_QUALITY 0
#define SRC_SINC_MEDIUM_QUALITY 1
#define SRC_SINC_FASTEST 2
#define SRC_ZERO_ORDER_HOLD 3
#define SRC_LINEAR 4

static inline int src_simple(SRC_DATA* d, int converter, int channels) {
    (void)d; (void)converter; (void)channels;
    lat_shim_die("src_simple");
}

static inline const char* src_strerror(int err) {
    (void)err;
    lat_shim_die("src_strerror");
}

#ifdef __cplusplus
}
#endif

#endif  // LAT_SHIM_SAMPLERATE_H
