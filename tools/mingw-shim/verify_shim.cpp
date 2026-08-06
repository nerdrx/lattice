// Proves the mingw shim headers still describe the real libraries.
//
// Compiled TWICE by `make -f Makefile.mingw verify-shim`:
//
//   1. against tools/mingw-shim/{sndfile,samplerate}.h  -- the stubs
//   2. against the real headers, via pkg-config          -- upstream
//
// Both must compile. That is the whole point: a check that only ever sees the
// stubs proves nothing, because the stubs are what we wrote. Compiling the
// identical assertions against upstream is what makes a drift detectable --
// if libsndfile renames a member or changes a type, pass 2 fails here, with
// the member named, instead of failing inside src/audio/sample.cpp under a
// cross-compiler nobody has installed locally.
//
// Only the members src/audio/sample.cpp actually touches are asserted. Adding
// a use there means adding an assertion here; that is the intended friction.

#include <sndfile.h>
#include <samplerate.h>

#include <type_traits>

// ---- SF_INFO ---------------------------------------------------------------
// sample.cpp reads info.frames (compared against 0 and passed as a count),
// info.samplerate and info.channels (both cast to f64/int).
static_assert(std::is_same_v<decltype(SF_INFO::frames), sf_count_t>,
              "SF_INFO::frames is no longer sf_count_t");
static_assert(std::is_same_v<decltype(SF_INFO::samplerate), int>,
              "SF_INFO::samplerate is no longer int");
static_assert(std::is_same_v<decltype(SF_INFO::channels), int>,
              "SF_INFO::channels is no longer int");
static_assert(std::is_integral_v<sf_count_t> && sizeof(sf_count_t) == 8,
              "sf_count_t is no longer a 64-bit integer");

// ---- SRC_DATA --------------------------------------------------------------
// sample.cpp assigns data_in, input_frames, data_out, output_frames and
// src_ratio, then reads output_frames_gen. Types matter: input_frames is long
// upstream, and the assignment from an sf_count_t is a narrowing the real
// build already performs.
static_assert(std::is_same_v<decltype(SRC_DATA::data_in), const float*>,
              "SRC_DATA::data_in is no longer const float*");
static_assert(std::is_same_v<decltype(SRC_DATA::data_out), float*>,
              "SRC_DATA::data_out is no longer float*");
static_assert(std::is_same_v<decltype(SRC_DATA::input_frames), long>,
              "SRC_DATA::input_frames is no longer long");
static_assert(std::is_same_v<decltype(SRC_DATA::output_frames), long>,
              "SRC_DATA::output_frames is no longer long");
static_assert(std::is_same_v<decltype(SRC_DATA::output_frames_gen), long>,
              "SRC_DATA::output_frames_gen is no longer long");
static_assert(std::is_same_v<decltype(SRC_DATA::src_ratio), double>,
              "SRC_DATA::src_ratio is no longer double");

// ---- entry points ----------------------------------------------------------
// Signatures, not just names: a changed parameter list is exactly the kind of
// drift that compiles fine against a stale stub and breaks against upstream.
static_assert(std::is_invocable_r_v<SNDFILE*, decltype(sf_open)&,
                                    const char*, int, SF_INFO*>,
              "sf_open's signature changed");
static_assert(std::is_invocable_r_v<sf_count_t, decltype(sf_readf_float)&,
                                    SNDFILE*, float*, sf_count_t>,
              "sf_readf_float's signature changed");
static_assert(std::is_invocable_r_v<int, decltype(sf_close)&, SNDFILE*>,
              "sf_close's signature changed");
static_assert(std::is_invocable_r_v<const char*, decltype(sf_strerror)&, SNDFILE*>,
              "sf_strerror's signature changed");
static_assert(std::is_invocable_r_v<int, decltype(src_simple)&,
                                    SRC_DATA*, int, int>,
              "src_simple's signature changed");
static_assert(std::is_invocable_r_v<const char*, decltype(src_strerror)&, int>,
              "src_strerror's signature changed");

// SFM_READ and the converter id are carried at their upstream values so that
// swapping the stubs for the real headers is a no-op at every call site.
static_assert(SFM_READ == 0x10, "SFM_READ drifted from the upstream value");
static_assert(SRC_SINC_MEDIUM_QUALITY == 1,
              "SRC_SINC_MEDIUM_QUALITY drifted from the upstream value");

int main() { return 0; }
