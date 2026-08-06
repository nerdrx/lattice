// Audio file loading. Everything is converted to the engine sample rate up
// front so the realtime thread only ever does interpolation, never conversion.
#pragma once
#include "../core/common.h"
#include <memory>
#include <vector>

namespace lat {

// ---------------------------------------------------------------------------
// Warp markers.
//
// A warp marker pins ONE source frame to ONE musical beat. A clip's markers are
// a sorted array whose invariant is that BOTH sequences are strictly
// increasing: that is exactly what makes beat -> source a bijection, makes the
// local rate (a segment's slope) finite and positive everywhere, and lets the
// engine binary-search either direction. A clip with no markers is not a
// special case in the map, it is a map with a single implicit segment whose
// slope is the clip's own tempo ratio — see engine.cpp's warp section.
//
// HOME: engine.h names this type on RtClip (as an incomplete type, so it need
// not include this header) and engine.cpp defines the evaluators, because
// nxtaktd links engine.cpp and deliberately not sample.cpp. The struct itself
// lives HERE because the sample layer is where markers are *derived*, from the
// transients below: a marker is a transient a user, or auto-warp, has decided
// belongs on a particular beat. If the two headers ever merge their warp
// sections this POD moves to engine.h beside RtNote and RtAutoSet, whose
// borrow-until-retired lifetime it already shares — see engine.cpp's "warp map"
// section for the whole published contract.
struct WarpMarker {
    i64 srcFrame = 0;           // source position, engine-rate frames
    f64 beat     = 0.0;         // clip-relative musical beat
};

// The evaluator. Pure, allocation-free and realtime-safe, and DEFINED IN
// engine.cpp rather than sample.cpp for one hard reason: nxtaktd links
// engine.cpp and does NOT link sample.cpp (see the Makefile's DAEMON_SRC), so
// anything the audio thread calls has to sit on the engine side of that line.
// The prototypes are here so a reader finds them beside the struct they
// operate on; they move to engine.h with WarpMarker if it ever adopts it.
//
// Deliberately one shared implementation for the engine and the UI, so a warp
// line the user drags and the position the engine plays cannot disagree — the
// same rule autoValueAt() serves for envelopes.
//
// All three treat the array as untrusted public memory: a map that breaks the
// monotonicity invariant yields *some* defined point on it rather than a
// division by zero or a read past the end.
//
//   warpSrcAt   beat  -> source frame.  Extrapolates the first/last segment's
//                        slope before the first and after the last marker,
//                        which is what lets a marker set cover only the middle
//                        of a clip. n == 1 holds that marker's frame; n == 0
//                        has nothing to say and returns `beat` unchanged.
//   warpSlopeAt beat  -> source frames per beat at that beat (the local rate,
//                        before the tempo scaling the engine applies).
//   warpBeatAt  frame -> beat. The inverse, for marker snapping and for
//                        turning a loop region in frames into one in beats.
f64  warpSrcAt  (const WarpMarker* m, int n, f64 beat);
f64  warpSlopeAt(const WarpMarker* m, int n, f64 beat);
f64  warpBeatAt (const WarpMarker* m, int n, f64 srcFrame);
// True when the array satisfies the invariant above. GUI-side gate: the engine
// must never be handed a map this rejects.
bool warpMapValid(const WarpMarker* m, int n);

// Onset detection cap. A transient list is a fixed-size realtime payload once
// it reaches the engine, and 8192 onsets is ~17 minutes of straight sixteenths
// at 120 BPM — past anything a clip is. Detection stops adding after this.
inline constexpr int kMaxTransients = 8192;

struct SampleBuffer {
    std::vector<f32> data;      // interleaved
    int  channels  = 2;
    i64  frames    = 0;
    f64  rate      = 48000.0;   // always the engine rate after load
    f64  guessedBpm = 120.0;
    f64  guessedBeats = 4.0;
    std::string name;
    std::string path;

    f64 durationSec() const { return frames / rate; }
    // Peak envelope for waveform drawing: min/max pairs per bucket.
    std::vector<f32> peaks;     // [lo0,hi0, lo1,hi1, ...]
    int peakBuckets = 0;
    void buildPeaks(int buckets = 2048);

    // Candidate transients, in source frames, STRICTLY increasing. Built once
    // at load beside `peaks` and never touched again, which is what lets the
    // GUI hand the raw pointer to the engine: it is const, immutable, and
    // outlives the clip that borrows it. Auto-warp reads these to place
    // markers; dragging a marker snaps to them; Beats-mode grain scheduling
    // aligns to them.
    std::vector<i64> transients;
    void buildTransients();
};

// The detector itself, exposed so it can be run on a buffer that is not a
// SampleBuffer (a recording still in its capture array, a test signal) and so
// it can be tested without a file. Off the audio thread only: it allocates.
//
// Spectral flux over a Hann-windowed 1024-point FFT at a 256-frame hop, with
// logarithmic magnitude compression, an adaptive local threshold, a 30 ms
// refractory gap, and a time-domain backtrack that puts each onset on the
// actual attack rather than on the analysis frame that noticed it. Cheap
// (it runs on every clip load), deterministic (no randomness, no
// parallelism, no dependence on allocation addresses), and monotone: `out`
// comes back sorted and strictly increasing.
void detectTransients(const f32* data, i64 frames, int channels, f64 rate,
                      std::vector<i64>& out);

using SampleRef = std::shared_ptr<SampleBuffer>;

// Loads and resamples to `engineRate`. Returns null on failure.
SampleRef loadSample(const std::string& path, f64 engineRate);

// Wraps a just-recorded interleaved stereo buffer (already at engine rate) in
// a SampleBuffer: copies the data, builds peaks, and derives lengthBeats from
// the session tempo instead of guessing. GUI thread.
SampleRef sampleFromRecording(const f32* interleaved, i64 frames, f64 engineRate,
                              f64 sessionBpm, const std::string& name);

// Live's loop-tempo heuristic: assume the file is a whole number of bars
// (favouring powers of two) and pick the reading that lands nearest 120 BPM.
void guessLoopTempo(f64 durationSec, int sigNum, f64* outBpm, f64* outBeats);

} // namespace lat
