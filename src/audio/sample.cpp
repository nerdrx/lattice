#include "sample.h"
#include <sndfile.h>
#include <samplerate.h>
#include <cstring>

namespace lat {

void guessLoopTempo(f64 dur, int sigNum, f64* outBpm, f64* outBeats) {
    if (dur <= 0.0) { *outBpm = 120.0; *outBeats = 4.0; return; }
    f64 bestBpm = 0.0, bestBeats = 0.0, bestScore = 1e30;
    // Whole bar counts first (these are what loop libraries actually ship),
    // then odd beat counts as a fallback.
    static const int barCounts[] = {1, 2, 4, 8, 16, 32, 64, 3, 6, 12};
    for (int bars : barCounts) {
        const f64 beats = (f64)bars * sigNum;
        const f64 bpm   = beats * 60.0 / dur;
        if (bpm < 60.0 || bpm > 200.0) continue;
        // Prefer tempi near 120 and, mildly, power-of-two bar counts.
        f64 score = std::fabs(std::log(bpm / 120.0));
        if (bars & (bars - 1)) score += 0.15;
        if (score < bestScore) { bestScore = score; bestBpm = bpm; bestBeats = beats; }
    }
    if (bestBpm == 0.0) {           // one-shot or very long file: no loop guess
        *outBpm   = 120.0;
        *outBeats = dur * 120.0 / 60.0;
    } else {
        *outBpm = bestBpm; *outBeats = bestBeats;
    }
}

void SampleBuffer::buildPeaks(int buckets) {
    if (frames <= 0) { peaks.clear(); peakBuckets = 0; return; }
    peakBuckets = buckets;
    peaks.assign((size_t)buckets * 2, 0.f);
    const f64 per = (f64)frames / buckets;
    for (int b = 0; b < buckets; ++b) {
        const i64 s = (i64)(b * per);
        const i64 e = std::min<i64>(frames, (i64)((b + 1) * per) + 1);
        f32 lo = 0.f, hi = 0.f;
        for (i64 i = s; i < e; ++i) {
            // Mono-sum for display; that is what Live shows for stereo clips.
            f32 v = 0.f;
            for (int c = 0; c < channels; ++c) v += data[(size_t)i * channels + c];
            v /= (f32)channels;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        peaks[(size_t)b * 2 + 0] = lo;
        peaks[(size_t)b * 2 + 1] = hi;
    }
}

SampleRef loadSample(const std::string& path, f64 engineRate) {
    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) { LOGW("cannot open %s: %s", path.c_str(), sf_strerror(nullptr)); return nullptr; }
    if (info.frames <= 0 || info.channels <= 0) { sf_close(f); return nullptr; }

    const int ch = std::min(info.channels, 2);
    std::vector<f32> raw((size_t)info.frames * info.channels);
    const sf_count_t got = sf_readf_float(f, raw.data(), info.frames);
    sf_close(f);
    if (got <= 0) return nullptr;

    // Downmix anything above stereo to stereo.
    std::vector<f32> src;
    if (info.channels == ch) {
        src = std::move(raw);
    } else {
        src.resize((size_t)got * ch);
        for (sf_count_t i = 0; i < got; ++i) {
            f32 acc[2] = {0.f, 0.f};
            for (int c = 0; c < info.channels; ++c) acc[c & 1] += raw[(size_t)i * info.channels + c];
            const f32 norm = 2.f / (f32)info.channels;
            src[(size_t)i * ch + 0] = acc[0] * norm;
            if (ch > 1) src[(size_t)i * ch + 1] = acc[1] * norm;
        }
    }

    auto sb = std::make_shared<SampleBuffer>();
    sb->channels = ch;
    sb->rate = engineRate;
    sb->path = path;
    const size_t slash = path.find_last_of('/');
    sb->name = slash == std::string::npos ? path : path.substr(slash + 1);

    if ((f64)info.samplerate != engineRate) {
        const f64 ratio = engineRate / (f64)info.samplerate;
        std::vector<f32> dst((size_t)((f64)got * ratio + 64) * ch);
        SRC_DATA d{};
        d.data_in = src.data();
        d.input_frames = got;
        d.data_out = dst.data();
        d.output_frames = (long)(dst.size() / ch);
        d.src_ratio = ratio;
        const int err = src_simple(&d, SRC_SINC_MEDIUM_QUALITY, ch);
        if (err) {
            LOGW("resample failed for %s: %s", sb->name.c_str(), src_strerror(err));
            sb->data = std::move(src);
            sb->frames = got;
            sb->rate = (f64)info.samplerate;
        } else {
            dst.resize((size_t)d.output_frames_gen * ch);
            sb->data = std::move(dst);
            sb->frames = d.output_frames_gen;
        }
    } else {
        sb->data = std::move(src);
        sb->frames = got;
    }

    // Pad by one frame so linear interpolation can always read pos+1.
    sb->data.resize(sb->data.size() + (size_t)ch, 0.f);

    guessLoopTempo(sb->frames / sb->rate, 4, &sb->guessedBpm, &sb->guessedBeats);
    sb->buildPeaks();
    LOGI("loaded %s  %lldf %dch  %.2fs  ~%.2f BPM / %.0f beats",
         sb->name.c_str(), (long long)sb->frames, ch, sb->frames / sb->rate,
         sb->guessedBpm, sb->guessedBeats);
    return sb;
}

} // namespace lat
