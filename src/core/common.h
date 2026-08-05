// NxTakt — core types and helpers.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <string>
#include <algorithm>

// The namespace stays `lat` after the Lattice -> NxTakt rename. It is an
// abbreviation, not the product name: it appears on every declaration in the
// codebase and in no user-visible string, no file format, no protocol and no
// installed artifact. Renaming it would rewrite every file in src/, tests/ and
// tools/ to change nothing a user or another program can observe, and would
// bury the parts of this pass that actually matter -- the format header and
// the device URIs -- in five hundred files of noise. If it is ever revisited,
// it is a mechanical pass of its own, on a quiet tree, and not before.
namespace lat {

using u8  = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using i8  = int8_t;   using i16 = int16_t;  using i32 = int32_t;  using i64 = int64_t;
using f32 = float;    using f64 = double;

// Fixed capacities. The realtime thread never allocates, so everything it
// touches is sized up front.
inline constexpr int kMaxTracks = 32;
inline constexpr int kMaxScenes = 32;
inline constexpr int kMaxBlock  = 8192;

template <typename T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

inline f32 dbToGain(f32 db) { return db <= -70.f ? 0.f : std::pow(10.f, db * 0.05f); }
inline f32 gainToDb(f32 g)  { return g <= 3.16e-5f ? -70.f : 20.f * std::log10(g); }

// Live's volume fader is not linear in dB; it packs resolution around 0 dB.
// t in [0,1] -> gain. t = 0.85 lands on unity, matching Live's default fader.
inline f32 faderToGain(f32 t) {
    if (t <= 0.f) return 0.f;
    const f32 db = (t < 0.85f) ? lerpf(-70.f, 0.f, std::pow(t / 0.85f, 2.2f))
                               : lerpf(0.f, 6.f, (t - 0.85f) / 0.15f);
    return dbToGain(db);
}
inline f32 gainToFader(f32 g) {
    const f32 db = gainToDb(g);
    if (db >= 0.f) return 0.85f + clampv(db / 6.f, 0.f, 1.f) * 0.15f;
    const f32 n = clampv((db + 70.f) / 70.f, 0.f, 1.f);
    return std::pow(n, 1.f / 2.2f) * 0.85f;
}

// Environment knobs, read under both spellings. The product was called Lattice
// before it was called NxTakt, and someone's shell profile, .desktop file or
// CI script still exports the old names; silently ignoring them would look
// like the knob had stopped working. Pass the bare suffix — env("AUDIO") reads
// NXTAKT_AUDIO first and falls back to LATTICE_AUDIO. New spelling always
// wins, so a profile that exports both is not ambiguous.
//
// Returns nullptr when neither is set, so it drops straight into the
// `if (const char* s = env("SCALE"))` shape getenv was used in. The returned
// pointer is environ-owned, exactly as getenv's is.
//
// GUI/startup thread only: getenv is not safe against a concurrent setenv, and
// nothing realtime reads configuration anyway.
const char* env(const char* suffix);

void logImpl(const char* lvl, const char* fmt, ...);
#define LOGI(...) ::lat::logImpl("info", __VA_ARGS__)
#define LOGW(...) ::lat::logImpl("warn", __VA_ARGS__)
#define LOGE(...) ::lat::logImpl("err ", __VA_ARGS__)

} // namespace lat
