// Audio file loading. Everything is converted to the engine sample rate up
// front so the realtime thread only ever does interpolation, never conversion.
#pragma once
#include "../core/common.h"
#include <memory>
#include <vector>

namespace lat {

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
};

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
