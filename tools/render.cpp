// Offline renderer: drives the engine with no audio device and no GUI, then
// writes the result to a wav.
//
//   build/render <project.lattice> <out.wav> [--scene N] [--bars N] [--tempo BPM]
//
// Because the engine never allocates and its scheduling is sample-accurate,
// this render is deterministic: the same project and arguments produce a
// bit-identical file on any machine. That is also what makes it a usable
// regression check for the audio path.
#include "../src/ui/app.h"
#include "../src/core/project.h"
#include <sndfile.h>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace lat;

static void pushClip(Engine& e, const Session& s, int t, int slot) {
    const ClipModel& m = s.tracks[t].slots[slot];
    Command c;
    c.a = t; c.b = slot;
    if (!m.valid()) { c.type = Cmd::ClearClip; e.pushCommand(c); return; }
    c.type = Cmd::SetClip;
    RtClip rc;
    rc.data        = m.sample->data.data();
    rc.frames      = m.sample->frames;
    rc.channels    = m.sample->channels;
    rc.loopStart   = m.loopStart;
    rc.loopEnd     = m.loopEnd > m.loopStart ? m.loopEnd : m.sample->frames;
    rc.clipBpm     = m.clipBpm;
    rc.lengthBeats = m.lengthBeats;
    rc.gain        = m.gain;
    rc.warp        = (int)m.warp;
    rc.loop        = m.loop;
    rc.quantumIdx  = m.quantumIdx;
    rc.valid       = true;
    c.clip = rc;
    e.pushCommand(c);
}

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    std::setlocale(LC_NUMERIC, "C");

    if (argc < 3) {
        std::fprintf(stderr,
            "usage: render <project.lattice> <out.wav> [--scene N] [--bars N] [--tempo BPM]\n");
        return 2;
    }
    const char* projPath = argv[1];
    const char* outPath  = argv[2];
    int scene = 0, bars = 8, solo = -1, warpOverride = -1;
    f64 tempoOverride = 0.0;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) scene = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--bars") && i + 1 < argc) bars = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tempo") && i + 1 < argc) tempoOverride = atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--solo") && i + 1 < argc) solo = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--warp") && i + 1 < argc) warpOverride = atoi(argv[++i]);
    }

    const f64 sr = 48000.0;
    Session s;
    std::string err;
    if (!loadProject(s, projPath, sr, &err)) {
        std::fprintf(stderr, "render: %s\n", err.c_str());
        return 1;
    }
    if (tempoOverride > 0.0) s.tempo = tempoOverride;
    if (solo >= 0)
        for (size_t t = 0; t < s.tracks.size(); ++t) s.tracks[t].solo = ((int)t == solo);
    if (warpOverride >= 0)
        for (auto& tr : s.tracks)
            for (int sl = 0; sl < kMaxScenes; ++sl) tr.slots[sl].warp = (Warp)warpOverride;

    Engine eng;
    eng.prepare(sr, 512);
    Command c;
    c.type = Cmd::SetTempo; c.x = s.tempo; eng.pushCommand(c);
    c = Command{}; c.type = Cmd::SetQuantum; c.a = s.quantumIdx; eng.pushCommand(c);
    for (size_t t = 0; t < s.tracks.size(); ++t) {
        const TrackModel& tr = s.tracks[t];
        c = Command{}; c.type = Cmd::TrackVol;  c.a = (i32)t; c.x = faderToGain(tr.fader); eng.pushCommand(c);
        c = Command{}; c.type = Cmd::TrackPan;  c.a = (i32)t; c.x = tr.pan;  eng.pushCommand(c);
        c = Command{}; c.type = Cmd::TrackMute; c.a = (i32)t; c.b = tr.mute; eng.pushCommand(c);
        c = Command{}; c.type = Cmd::TrackSolo; c.a = (i32)t; c.b = tr.solo; eng.pushCommand(c);
        for (int sl = 0; sl < (int)s.scenes.size(); ++sl) pushClip(eng, s, (int)t, sl);
    }
    c = Command{}; c.type = Cmd::LaunchScene; c.a = scene; eng.pushCommand(c);

    const i64 total = (i64)(sr * 60.0 / s.tempo * s.sigNum * bars);
    const int block = 512;
    std::vector<f32> l(block), r(block), inter;
    inter.reserve((size_t)total * 2);

    f32 peak = 0.f;
    f64 sumSq = 0.0;
    for (i64 done = 0; done < total; done += block) {
        const int n = (int)std::min<i64>(block, total - done);
        eng.process(nullptr, nullptr, l.data(), r.data(), n);
        for (int i = 0; i < n; ++i) {
            inter.push_back(l[i]);
            inter.push_back(r[i]);
            peak = std::max({peak, std::fabs(l[i]), std::fabs(r[i])});
            sumSq += (f64)l[i] * l[i] + (f64)r[i] * r[i];
        }
    }

    SF_INFO info{};
    info.samplerate = (int)sr;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
    SNDFILE* f = sf_open(outPath, SFM_WRITE, &info);
    if (!f) { std::fprintf(stderr, "render: cannot write %s\n", outPath); return 1; }
    sf_writef_float(f, inter.data(), (sf_count_t)(inter.size() / 2));
    sf_close(f);

    const f64 rms = std::sqrt(sumSq / (f64)std::max<i64>(1, total * 2));
    std::printf("rendered %lld frames (%.2fs) @ %.1f BPM scene %d -> %s\n",
                (long long)total, total / sr, s.tempo, scene, outPath);
    std::printf("  peak %.4f (%.1f dBFS)   rms %.4f (%.1f dBFS)\n",
                peak, gainToDb(peak), rms, gainToDb((f32)rms));
    if (peak <= 1e-6f) {
        std::fprintf(stderr, "  FAIL: render is silent\n");
        return 1;
    }
    return 0;
}
