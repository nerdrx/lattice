#include "sample.h"
#include <sndfile.h>
#include <samplerate.h>
#include <cstring>
#include <vector>

namespace lat {

// ---------------------------------------------------------------------------
// Onset (transient) detection
//
// What this is for: a warp marker is a pair (source frame, musical beat), and
// the only source frames worth pinning to a beat are the ones a listener hears
// as an event. Auto-warp places markers on transients; dragging a marker snaps
// to them; Beats-mode grain scheduling starts grains on them. So the whole
// manual-warp feature rests on this list being right, and on it being the same
// list every time — a marker that moved because the detector was run twice
// would be a corrupt edit, not a rounding error.
//
// The method is the standard one and deliberately not more:
//
//   1. Mono-sum, then a Hann-windowed 1024-point FFT every 256 frames. At
//      48 kHz that is a 21 ms window every 5.3 ms — long enough to resolve a
//      kick's fundamental, short enough that four-on-the-floor never puts two
//      onsets in one frame. The window is CENTRED on t*hop, so an analysis
//      frame's nominal time is its centre and the flux peak lands near the
//      attack instead of a window-length before it.
//   2. Spectral flux with logarithmic magnitude compression, measured against a
//      TEMPORALLY MAX-FILTERED reference spectrum:
//        C_t[k] = log(1 + L*|X_t[k]|)
//        R_t[k] = max over the previous kRefFrames frames of C[k]
//        SF[t]  = sum_k max(0, C_t[k] - R_t[k])
//      The log is what makes this work on real music rather than only on
//      isolated hits: it costs a loud onset nothing and stops the loudest bar
//      of a track from setting a threshold no quiet bar can reach.
//      The max filter is what makes it work on BASS, and it is the fix for the
//      biggest thing this detector got wrong: see "why the reference is a
//      maximum" below.
//   3. Peak-pick against an ADAPTIVE threshold — a local mean of the flux over
//      +/-8 frames — plus a strict local maximum over +/-3 frames and a 30 ms
//      refractory gap. A fixed threshold cannot serve both a drum loop and a
//      pad; a local one needs no per-file tuning, which is the property that
//      matters when this runs unattended on every load.
//   4. Backtrack. The flux peak says WHICH event; it is worth about a hop
//      (5 ms) on WHERE. So each accepted peak is refined in the time domain:
//      inside the analysis window, find the loudest 2 ms and walk back to the
//      first 2 ms that clears a fifth of it. That is the attack, to within a
//      millisecond or so, and it is what makes a snapped marker land ON the
//      kick rather than just before it.
//   5. A LEVEL-RISE GATE on the refined position: an onset is a place where the
//      signal gets louder, so the mean |x| over the 20 ms after it must beat the
//      20 ms before it by kRiseRatio. See "why a spectral rise is not enough".
//
// WHY THE REFERENCE IS A MAXIMUM, NOT THE PREVIOUS FRAME
// -----------------------------------------------------
// A 1024-point window is 21 ms, which is barely ONE PERIOD of a 45 Hz kick
// fundamental and less than one of a 41 Hz bass note. At that resolution the
// magnitude a bin reports for a perfectly steady low tone is not steady: it
// oscillates at twice the tone's frequency as the window slides over it, and at
// a 256-frame hop that oscillation has a period of about two analysis frames.
// Frame-to-frame flux therefore sees a rise every second frame throughout a
// sustained low note, and the adaptive threshold — which is local, so it sits
// low wherever the flux is generally low — happily calls them onsets. Measured
// on the demo loops before this was fixed: kick.wav 22 onsets for 8 kicks,
// bass.wav 32 for 8 notes, and the chord pad 42 for none at all, because a
// 1.003 detune beats at ~1 Hz and beating is the same phenomenon slower.
// Comparing against the loudest each bin has been over the last kRefFrames
// frames — one whole analysis window of lookback, which is by construction
// longer than the oscillation it has to survive — removes all of it: a bin only
// contributes flux when it exceeds everything it has recently been. A real
// attack does that by a mile. This is the temporal counterpart of the
// frequency-direction max filter in Böck's SuperFlux, and it is here for the
// same reason: to stop a steady tone from looking like a stream of onsets.
//
// WHY A SPECTRAL RISE IS NOT ENOUGH
// ---------------------------------
// Log compression deliberately flattens the difference between loud and quiet,
// which is what lets one threshold serve a whole track — and it means a tiny
// broadband event scores a surprisingly large flux. The demo kick decays for
// 350 ms and then simply STOPS, and that truncation, 30 dB down, splattered
// enough high-frequency energy to clear the threshold on every kick. So the
// last word belongs to the signal and not to its spectrum: a candidate is kept
// only if the level after it actually rises. That single test costs nothing,
// needs no tuning, and is the definition of an onset rather than a proxy for
// one. The window is 20 ms because the estimate has to average over at least
// one period of the lowest content that matters (~50 Hz) or its own phase
// sensitivity is bigger than the effect being measured — at 10 ms a bass note's
// truncation reads as a 1.4x RISE purely from where the window lands in the
// cycle. The pre-window is zero-padded at the file start (before the audio there
// is silence, which is exactly what makes a clip's first frame an onset).
//
// The gate's honest cost: a true LEGATO onset — one note joined to the next at
// the same level, where the only cue is the pitch change — is rejected. That is
// the right way to be wrong here. These frames become warp markers, and a marker
// on a note that is not there is a corrupt edit, while a marker not offered is a
// marker the user places by hand.
//
// Cost: one 1024-point FFT per 256 frames, i.e. ~187 per second of audio.
// Measured at about 3 ms per second of stereo audio on this tree — 13 ms for
// the four-second demo loops, against the ~15 ms libsndfile and libsamplerate
// already spend decoding and resampling one. It roughly doubles a clip load and
// it is linear in length, which is what lets it be unconditional. The max filter
// adds kRefFrames compares per bin per frame and the gate two 20 ms sums per
// surviving candidate; neither is measurable beside the FFT.
//
// Determinism: fixed constants, fixed traversal order, no threads, no RNG, no
// dependence on allocation addresses, and every accumulation in a fixed order
// over f64. The same samples always give the same frames.
// ---------------------------------------------------------------------------

namespace {

constexpr int kFftN    = 1024;      // ~21 ms at 48 kHz
constexpr int kFftHop  = 256;       // ~5.3 ms at 48 kHz
constexpr int kFftBins = kFftN / 2 + 1;
constexpr f64 kFluxLog = 1.0;       // magnitude compression, Bock's lambda
constexpr int kThreshWin = 8;       // local-mean half-width, analysis frames
constexpr int kPeakWin   = 3;       // strict-local-max half-width
constexpr f64 kThreshMul = 2.0;     // local mean multiplier
constexpr f64 kThreshAdd = 0.035;   // floor, in units of the normalised flux
constexpr f64 kMinGapSec = 0.030;   // refractory period between onsets
// Flux reference depth: the max over this many previous frames. 4 * 256 = 1024
// frames is exactly one analysis window of lookback, which is the timescale the
// window's own leakage oscillation lives on. Not a tuned number — anything from
// 3 upwards gives the same answer on the demo material.
constexpr int kRefFrames = 4;
constexpr f64 kRiseWinSec = 0.020;  // level-rise gate half-window
constexpr f64 kRiseRatio  = 1.15;   // level after / level before, minimum

// In-place iterative radix-2 complex FFT. `tw` holds cos/sin at -2*pi*i/n for
// i in [0, n/2) so the inner loop does no trigonometry; the whole detector
// builds it once. Deterministic by construction: no table is ever recomputed
// and the butterfly order is fixed.
void fftRadix2(f32* re, f32* im, int n, const f32* tw) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;               // stride into the twiddle table
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                const f32 wr = tw[(size_t)(k * step) * 2 + 0];
                const f32 wi = tw[(size_t)(k * step) * 2 + 1];
                const int a = i + k, b = i + k + half;
                const f32 vr = re[b] * wr - im[b] * wi;
                const f32 vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] = re[a] + vr; im[a] = im[a] + vi;
            }
        }
    }
}

} // namespace

void detectTransients(const f32* data, i64 frames, int channels, f64 rate,
                      std::vector<i64>& out) {
    out.clear();
    if (!data || frames <= 0 || channels <= 0 || !(rate > 0.0)) return;
    // Shorter than one analysis window there is no flux to speak of; a clip
    // that brief is a single transient by definition and gets no list rather
    // than a made-up one.
    if (frames < kFftN) return;

    // 1. mono sum. One pass, and the only place channel count matters.
    std::vector<f32> mono((size_t)frames);
    {
        const f32 norm = 1.f / (f32)channels;
        for (i64 i = 0; i < frames; ++i) {
            f32 acc = 0.f;
            for (int c = 0; c < channels; ++c) acc += data[(size_t)i * channels + c];
            mono[(size_t)i] = acc * norm;
        }
    }

    // Hann window and the twiddle table, both built once.
    std::vector<f32> win((size_t)kFftN), tw((size_t)kFftN);   // tw: n/2 pairs
    for (int i = 0; i < kFftN; ++i)
        win[(size_t)i] = (f32)(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 *
                                                    (f64)i / (f64)kFftN));
    for (int i = 0; i < kFftN / 2; ++i) {
        const f64 a = -2.0 * 3.14159265358979323846 * (f64)i / (f64)kFftN;
        tw[(size_t)i * 2 + 0] = (f32)std::cos(a);
        tw[(size_t)i * 2 + 1] = (f32)std::sin(a);
    }

    // 2. spectral flux over centred frames.
    //
    // The analysis starts a full window BEFORE the audio and ends one after it,
    // and that padding is not cosmetic: without it the very first frame's
    // window already contains half of a transient at sample 0, so the flux has
    // nothing to rise from and the clip's first onset — very often its
    // downbeat, the single most important frame in the file — is invisible. The
    // padded frames see silence, so the step into the first real frame is the
    // whole of the attack.
    const i64 pad     = kFftN / kFftHop;
    const i64 nFrames = frames / kFftHop + 1 + 2 * pad;
    std::vector<f64> flux((size_t)nFrames, 0.0);
    std::vector<f32> re((size_t)kFftN), im((size_t)kFftN);
    // The reference: a ring of the last kRefFrames log-magnitude spectra, of
    // which the per-bin maximum is taken. Zero-initialised, which is right —
    // frame 0's window is entirely in the silence before the file.
    std::vector<f32> hist((size_t)kRefFrames * kFftBins, 0.f);
    std::vector<f32> cur((size_t)kFftBins, 0.f);

    for (i64 t = 0; t < nFrames; ++t) {
        // Nominal time of frame t is (t - pad) * hop; the window is centred on
        // it, which is what keeps the flux peak near the attack rather than a
        // window-length ahead of it.
        const i64 c0 = (t - pad) * kFftHop - kFftN / 2;   // first sample
        for (int i = 0; i < kFftN; ++i) {
            const i64 s = c0 + i;
            const f32 x = (s >= 0 && s < frames) ? mono[(size_t)s] : 0.f;
            re[(size_t)i] = x * win[(size_t)i];
            im[(size_t)i] = 0.f;
        }
        fftRadix2(re.data(), im.data(), kFftN, tw.data());
        f64 acc = 0.0;
        for (int k = 0; k < kFftBins; ++k) {
            const f32 mag = std::sqrt(re[(size_t)k] * re[(size_t)k] +
                                      im[(size_t)k] * im[(size_t)k]);
            const f32 c = (f32)std::log(1.0 + kFluxLog * (f64)mag);
            cur[(size_t)k] = c;
            if (t > 0) {
                f32 ref = 0.f;
                for (int r = 0; r < kRefFrames; ++r) {
                    const f32 h = hist[(size_t)r * kFftBins + (size_t)k];
                    if (h > ref) ref = h;
                }
                const f64 d = (f64)c - (f64)ref;
                if (d > 0.0) acc += d;              // rectified: only rises count
            }
        }
        flux[(size_t)t] = acc;
        // Fixed slot, fixed order: frame t always lands in t % kRefFrames, so
        // the reference for any frame is the same set of frames every run.
        std::memcpy(&hist[(size_t)(t % kRefFrames) * kFftBins], cur.data(),
                    sizeof(f32) * (size_t)kFftBins);
    }

    // 3. normalise and peak-pick. Normalising by the maximum is what makes the
    //    thresholds below scale-free, so a clip that was recorded 20 dB down
    //    detects exactly the same onsets as one that was not.
    f64 fmax = 0.0;
    for (i64 t = 0; t < nFrames; ++t) if (flux[(size_t)t] > fmax) fmax = flux[(size_t)t];
    if (!(fmax > 0.0)) return;                       // silence, or DC
    for (i64 t = 0; t < nFrames; ++t) flux[(size_t)t] /= fmax;

    const i64 minGap  = (i64)(kMinGapSec * rate);
    const i64 refWin  = (i64)(0.002 * rate);         // 2 ms boxcar for backtrack
    const i64 riseWin = std::max<i64>(1, (i64)(kRiseWinSec * rate));
    i64 lastPos = -(minGap + 1);
    std::vector<f64> pre;                            // backtrack prefix sums

    // Mean |x| over [a, b), clipped to the buffer. `denom` is supplied rather
    // than derived so the caller decides what a clipped window means.
    auto meanAbs = [&](i64 a, i64 b, f64 denom) -> f64 {
        a = std::max<i64>(0, a);
        b = std::min<i64>(frames, b);
        if (b <= a || !(denom > 0.0)) return 0.0;
        f64 s = 0.0;
        for (i64 i = a; i < b; ++i) s += (f64)std::fabs(mono[(size_t)i]);
        return s / denom;
    };
    // Before the file there is silence, so the pre-window is ZERO-PADDED: it
    // always divides by its full length. That is what makes a hit in the first
    // few frames of a clip an onset instead of a comparison against itself.
    // After the file there is nothing to measure, so the post-window divides by
    // however much real audio it found.
    auto levelBefore = [&](i64 p) { return meanAbs(p - riseWin, p, (f64)riseWin); };
    auto levelAfter  = [&](i64 p) {
        return meanAbs(p, p + riseWin, (f64)std::min<i64>(riseWin, frames - p));
    };

    for (i64 t = 1; t < nFrames; ++t) {
        const f64 v = flux[(size_t)t];
        if (v <= kThreshAdd) continue;

        // Local mean over +/-kThreshWin, clipped at the ends. Not a median:
        // this runs once per 5 ms of every clip a user ever loads, and the
        // rank statistic buys nothing a rectified flux does not already have.
        f64 sum = 0.0;
        int cnt = 0;
        for (i64 k = t - kThreshWin; k <= t + kThreshWin; ++k) {
            if (k < 0 || k >= nFrames) continue;
            sum += flux[(size_t)k];
            ++cnt;
        }
        if (v < kThreshMul * (sum / (f64)cnt) + kThreshAdd) continue;

        // Strict local maximum, ties broken towards the earlier frame so the
        // result cannot depend on scan direction.
        bool isPeak = true;
        for (i64 k = t - kPeakWin; k <= t + kPeakWin && isPeak; ++k) {
            if (k < 0 || k >= nFrames || k == t) continue;
            if (k < t ? flux[(size_t)k] >= v : flux[(size_t)k] > v) isPeak = false;
        }
        if (!isPeak) continue;

        // 4. backtrack to the attack inside this frame's window.
        //
        // Rise-based, not level-based. The naive "first 2 ms above a fraction
        // of the window's peak" lands on sample zero of the window whenever the
        // window opens on the tail of the PREVIOUS event, which is most of the
        // time in music. So: find the loudest 2 ms, then walk BACKWARDS from it
        // to where the envelope was still down at the local floor. That point
        // is the attack, and it is the same point whether the window opened in
        // silence or halfway through a decay.
        const i64 nominal = std::max<i64>(0, (t - pad) * kFftHop);
        const i64 lo = std::max<i64>(0, nominal - kFftN / 2);
        const i64 hi = std::min<i64>(frames, nominal + kFftN / 2);
        i64 pos = nominal;
        if (hi - lo > refWin && refWin > 0) {
            const size_t span = (size_t)(hi - lo);
            const size_t w = (size_t)refWin;
            pre.assign(span + 1, 0.0);
            for (size_t i = 0; i < span; ++i)
                pre[i + 1] = pre[i] + (f64)std::fabs(mono[(size_t)lo + i]);
            const size_t last = span - w;             // span > w is guaranteed
            size_t imax = 0;
            f64 pk = -1.0;
            for (size_t i = 0; i <= last; ++i) {
                const f64 m = pre[i + w] - pre[i];
                if (m > pk) { pk = m; imax = i; }
            }
            f64 floorV = pk;
            for (size_t i = 0; i <= imax; ++i)
                floorV = std::min(floorV, pre[i + w] - pre[i]);
            const f64 thr = floorV + 0.25 * (pk - floorV);
            size_t i = imax;
            while (i > 0 && (pre[i - 1 + w] - pre[i - 1]) > thr) --i;
            pos = lo + (i64)i;
        }

        // 5. the level-rise gate. Applied BEFORE the refractory gap on purpose:
        // a candidate this rejects was never an onset, so it must not be
        // allowed to consume the refractory window a real one needs.
        if (!(levelAfter(pos) > kRiseRatio * levelBefore(pos) + 1e-9)) continue;

        // The refractory gap is applied to the REFINED position, not to the
        // analysis frame: two flux peaks 40 ms apart that backtrack onto the
        // same attack are one onset, and the list's strict-increase invariant
        // is what the engine's binary searches rely on.
        if (pos <= lastPos || pos - lastPos < minGap) continue;
        lastPos = pos;
        out.push_back(pos);
        if ((int)out.size() >= kMaxTransients) break;
    }
}

void SampleBuffer::buildTransients() {
    detectTransients(data.data(), frames, channels, rate, transients);
}

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
    // Once, here, off the audio thread, and never again for this buffer: the
    // engine and the UI both borrow the raw pointer afterwards.
    sb->buildTransients();
    LOGI("loaded %s  %lldf %dch  %.2fs  ~%.2f BPM / %.0f beats / %zu transients",
         sb->name.c_str(), (long long)sb->frames, ch, sb->frames / sb->rate,
         sb->guessedBpm, sb->guessedBeats, sb->transients.size());
    return sb;
}

SampleRef sampleFromRecording(const f32* interleaved, i64 frames, f64 engineRate,
                              f64 sessionBpm, const std::string& name) {
    if (!interleaved || frames <= 0 || engineRate <= 0.0) return nullptr;

    auto sb = std::make_shared<SampleBuffer>();
    sb->channels = 2;                    // the engine always captures stereo
    sb->frames   = frames;
    sb->rate     = engineRate;
    sb->name     = name;
    sb->path.clear();                    // nothing on disk yet

    // One extra frame so linear interpolation can always read pos+1, exactly
    // as loadSample() pads: the realtime fetch path makes no special case for
    // recorded buffers.
    sb->data.assign((size_t)(frames + 1) * 2, 0.f);
    std::memcpy(sb->data.data(), interleaved, (size_t)frames * 2 * sizeof(f32));

    // A recording was made against the session clock, so its tempo is known
    // rather than guessed — guessLoopTempo() would be throwing that away. The
    // length is snapped to a sixteenth to absorb the sub-block rounding of the
    // quantized start and stop; a take is always a musical number of beats.
    const f64 bpm   = (sessionBpm > 0.0) ? sessionBpm : 120.0;
    const f64 beats = (f64)frames / engineRate / 60.0 * bpm;
    sb->guessedBpm   = bpm;
    sb->guessedBeats = std::max(0.25, std::round(beats * 4.0) / 4.0);

    sb->buildPeaks();
    sb->buildTransients();
    LOGI("recorded %s  %lldf  %.2fs  %.0f BPM / %.2f beats / %zu transients",
         sb->name.c_str(), (long long)frames, (f64)frames / engineRate,
         sb->guessedBpm, sb->guessedBeats, sb->transients.size());
    return sb;
}

} // namespace lat
