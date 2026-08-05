// Reports the dominant fundamental of a wav via autocorrelation.
//
//   pitch_check <file.wav> [startSec] [lenSec]
//
// Used to verify that Beats-mode warping is a genuine time-stretch: the same
// material rendered at two tempos must keep the same fundamental, whereas
// Repitch mode must shift it by the tempo ratio.
#include <sndfile.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: pitch_check <file.wav> [startSec] [lenSec]\n"); return 2; }
    const double startSec = argc > 2 ? atof(argv[2]) : 0.25;
    const double lenSec   = argc > 3 ? atof(argv[3]) : 0.40;

    SF_INFO info{};
    SNDFILE* f = sf_open(argv[1], SFM_READ, &info);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    std::vector<float> all((size_t)info.frames * info.channels);
    sf_readf_float(f, all.data(), info.frames);
    sf_close(f);

    const long start = (long)(startSec * info.samplerate);
    const long n     = (long)(lenSec * info.samplerate);
    if (start + n > info.frames) { std::fprintf(stderr, "window past end of file\n"); return 1; }

    // Mono-sum the window and remove DC, which otherwise dominates lag 0.
    std::vector<double> x((size_t)n);
    double mean = 0.0;
    for (long i = 0; i < n; ++i) {
        double s = 0.0;
        for (int c = 0; c < info.channels; ++c) s += all[(size_t)(start + i) * info.channels + c];
        x[(size_t)i] = s / info.channels;
        mean += x[(size_t)i];
    }
    mean /= n;
    for (auto& v : x) v -= mean;

    // Search 30..500 Hz, which covers the bass range we care about.
    const long minLag = info.samplerate / 500;
    const long maxLag = info.samplerate / 30;
    double best = -1e30;
    long bestLag = 0;
    for (long lag = minLag; lag <= maxLag && lag < n; ++lag) {
        double num = 0.0, den = 0.0;
        for (long i = 0; i + lag < n; ++i) { num += x[(size_t)i] * x[(size_t)(i + lag)]; den += x[(size_t)i] * x[(size_t)i]; }
        const double r = den > 0.0 ? num / den : 0.0;
        if (r > best) { best = r; bestLag = lag; }
    }
    if (bestLag == 0) { std::fprintf(stderr, "no periodicity found\n"); return 1; }

    const double hz = (double)info.samplerate / bestLag;
    std::printf("%-28s  f0 = %7.2f Hz   (corr %.3f, window %.2f-%.2fs)\n",
                argv[1], hz, best, startSec, startSec + lenSec);
    return 0;
}
