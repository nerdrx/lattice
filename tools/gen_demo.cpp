// Generates a demo set: four 2-bar loops at 120 BPM, a fifth track that plays
// NxTakt's own Pulse synth from a MIDI pattern, and a .lattice project that
// ties them together. Doubles as an end-to-end check of the whole chain --
// sndfile write -> loadSample -> Session -> saveProject -- and, because the
// keys track has no audio file behind it, of devices and MIDI clips surviving
// a round trip.
//
//   build/gen_demo [outdir]      (default ~/Music/NxTakt Demo)
#include "../src/ui/app.h"
#include "../src/core/project.h"
#include <sndfile.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace lat;

static constexpr f64 kSR    = 48000.0;
static constexpr f64 kBpm   = 120.0;
static constexpr int kBeats = 8;                       // two bars of 4/4
static const int    kFrames = (int)(kSR * 60.0 / kBpm * kBeats);

static void writeWav(const std::string& path, const std::vector<f32>& mono) {
    SF_INFO info{};
    info.samplerate = (int)kSR;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f) { std::fprintf(stderr, "cannot write %s: %s\n", path.c_str(), sf_strerror(nullptr)); return; }
    sf_writef_float(f, mono.data(), (sf_count_t)mono.size());
    sf_close(f);
    std::printf("  wrote %-12s %d frames  %.2fs\n",
                path.substr(path.find_last_of('/') + 1).c_str(),
                (int)mono.size(), mono.size() / kSR);
}

// One beat in frames.
static int beatFrames() { return (int)(kSR * 60.0 / kBpm); }

// --- the keys track --------------------------------------------------------
// The one track with no wav behind it: a Pulse instrument and a one-bar A minor
// line, which is all it takes to make a MIDI clip sound. Kept out of the intro
// scene so the demo still has a scene that is pure audio.
static constexpr int   kKeysTrack  = 4;
static constexpr int   kKeysColor  = 2;
static constexpr f64   kMelodyBeats = 4.0;
static const char*     kPulseUri   = "nxtakt:pulse";

struct DemoNote { f64 beat, len; u8 pitch, vel; };
// A3 C4 E4 D4 | C4 A3 G3 E3 -- the same line the hand-written jam set plays.
static const DemoNote kMelody[] = {
    {0.0, 0.5, 57, 100}, {0.5, 0.5, 60, 92},
    {1.0, 0.5, 64, 100}, {1.5, 0.5, 62, 88},
    {2.0, 0.5, 60, 100}, {2.5, 0.5, 57, 88},
    {3.0, 0.5, 55, 100}, {3.5, 0.5, 52, 88},
};
static constexpr int kMelodyNotes = (int)(sizeof kMelody / sizeof kMelody[0]);

static std::vector<f32> makeKick() {
    std::vector<f32> v((size_t)kFrames, 0.f);
    for (int b = 0; b < kBeats; ++b) {
        const int start = b * beatFrames();
        const int len = (int)(kSR * 0.35);
        for (int i = 0; i < len && start + i < kFrames; ++i) {
            const f64 t = i / kSR;
            // Pitch sweep from 110 Hz down to 45 Hz is what gives a kick its thump.
            const f64 f = 45.0 + 65.0 * std::exp(-t * 38.0);
            const f64 env = std::exp(-t * 9.0);
            v[(size_t)(start + i)] += (f32)(std::sin(2 * M_PI * f * t) * env * 0.9);
        }
    }
    return v;
}

static std::vector<f32> makeHats() {
    std::vector<f32> v((size_t)kFrames, 0.f);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<f32> noise(-1.f, 1.f);
    const int step = beatFrames() / 2;                   // eighth notes
    for (int s = 0; s * step < kFrames; ++s) {
        const int start = s * step;
        const bool accent = (s % 2) == 1;                // off-beats sit forward
        const int len = (int)(kSR * (accent ? 0.06 : 0.03));
        f32 hp = 0.f;
        for (int i = 0; i < len && start + i < kFrames; ++i) {
            const f64 env = std::exp(-(i / kSR) * (accent ? 70.0 : 130.0));
            const f32 n = noise(rng);
            hp = 0.85f * (hp + n - (i ? noise(rng) : 0.f));   // crude high-pass
            v[(size_t)(start + i)] += (f32)(hp * env * (accent ? 0.28 : 0.16));
        }
    }
    return v;
}

static std::vector<f32> makeBass() {
    std::vector<f32> v((size_t)kFrames, 0.f);
    // A minor walk: A1 A1 C2 E2 | A1 A1 G1 E1
    static const f64 notes[kBeats] = {55.0, 55.0, 65.41, 82.41, 55.0, 55.0, 49.0, 41.20};
    for (int b = 0; b < kBeats; ++b) {
        const int start = b * beatFrames();
        const int len = (int)(beatFrames() * 0.85);
        f64 phase = 0.0;
        for (int i = 0; i < len && start + i < kFrames; ++i) {
            const f64 t = i / kSR;
            const f64 env = std::min(1.0, t * 200.0) * std::exp(-t * 2.2);
            phase += 2 * M_PI * notes[b] / kSR;
            // Saw-ish: fundamental plus a couple of partials, gently filtered.
            const f64 s = std::sin(phase) + 0.35 * std::sin(2 * phase) + 0.15 * std::sin(3 * phase);
            v[(size_t)(start + i)] += (f32)(s * env * 0.30);
        }
    }
    return v;
}

static std::vector<f32> makeChord() {
    std::vector<f32> v((size_t)kFrames, 0.f);
    // Am9 pad, two bars, slow swell.
    static const f64 f[] = {220.0, 261.63, 329.63, 493.88};
    for (int i = 0; i < kFrames; ++i) {
        const f64 t = i / kSR;
        const f64 env = (1.0 - std::exp(-t * 1.6)) * std::exp(-t * 0.28);
        f64 s = 0.0;
        for (double fq : f) {
            s += std::sin(2 * M_PI * fq * t);
            s += 0.5 * std::sin(2 * M_PI * fq * 1.003 * t);   // slight detune
        }
        v[(size_t)i] += (f32)(s / 12.0 * env * 0.7);
    }
    return v;
}

int main(int argc, char** argv) {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    std::string dir = argc > 1 ? argv[1] : (home + "/Music/NxTakt Demo");
    mkdir((home + "/Music").c_str(), 0755);
    mkdir(dir.c_str(), 0755);

    std::printf("generating demo loops in %s\n", dir.c_str());
    struct { const char* name; std::vector<f32> data; int color; } loops[] = {
        {"kick",  makeKick(),  5},
        {"hats",  makeHats(),  8},
        {"bass",  makeBass(),  11},
        {"chord", makeChord(), 13},
    };
    for (auto& l : loops) writeWav(dir + "/" + l.name + ".wav", l.data);

    // Build a session that puts each loop on its own track, and stack a couple
    // of scenes so scene launch has something to do.
    Session s;
    s.name = "NxTakt Demo";
    s.tempo = kBpm;
    s.tracks.resize(5);                    // four loops plus the keys track
    s.scenes.resize(4);
    for (int i = 0; i < 4; ++i) {
        s.tracks[i].name = loops[i].name;
        s.tracks[i].colorIdx = loops[i].color;
        s.scenes[i].name = (i == 0) ? "Intro" : (i == 1) ? "Beat" : (i == 2) ? "Full" : "Break";
    }

    std::printf("loading them back through loadSample()\n");
    int loaded = 0, placed = 0;
    for (int t = 0; t < 4; ++t) {
        const std::string path = dir + "/" + loops[t].name + ".wav";
        SampleRef sb = loadSample(path, kSR);
        if (!sb) { std::fprintf(stderr, "  FAILED to load %s\n", path.c_str()); continue; }
        ++loaded;
        // Scene layout: intro = chord only, beat adds kick+hats, full = all.
        for (int sc = 0; sc < 4; ++sc) {
            const bool on = (sc == 0 && t == 3) ||
                            (sc == 1 && (t == 0 || t == 1)) ||
                            (sc == 2) ||
                            (sc == 3 && (t == 2 || t == 3));
            if (!on) continue;
            ClipModel& m = s.tracks[t].slots[sc];
            m.sample = sb;
            m.path = path;
            m.name = loops[t].name;
            m.colorIdx = loops[t].color;
            m.clipBpm = kBpm;                 // we know it exactly, no guessing
            m.lengthBeats = kBeats;
            m.loopStart = 0;
            m.loopEnd = sb->frames;
            m.loop = true;
            m.warp = Warp::Beats;
            ++placed;
        }
    }

    // The keys track. A SavedDevice is all a set carries for a plugin -- the
    // URI, and whatever parameters were touched -- so this is exactly what the
    // app writes and exactly what render materializes; Pulse's own defaults do
    // the rest, which is why no `param` lines are needed to make it sing.
    TrackModel& keys = s.tracks[kKeysTrack];
    keys.name = "keys";
    keys.colorIdx = kKeysColor;
    SavedDevice pulse;
    pulse.uid  = s.newUid();
    pulse.uri  = kPulseUri;
    pulse.name = "Pulse";
    keys.savedDevices.push_back(pulse);
    // Scenes 1-3: the intro stays a chord on its own, and the melody comes in
    // with the beat.
    for (int sc = 1; sc < 4; ++sc) {
        ClipModel& m = keys.slots[sc];
        m.uid = s.newUid();
        m.kind = ClipKind::Midi;
        m.name = "melody";
        m.colorIdx = kKeysColor;
        m.lengthBeats = kMelodyBeats;
        m.loop = true;
        for (const DemoNote& n : kMelody)
            m.notes.push_back(NoteModel{n.beat, n.len, n.pitch, n.vel});
        ++placed;
    }
    std::printf("  keys track: Pulse + %d notes in scenes 1-3\n", kMelodyNotes);

    const std::string proj = dir + "/demo.lattice";
    std::string err;
    if (!saveProject(s, proj, &err)) {
        std::fprintf(stderr, "saveProject failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("\n%d/4 loops loaded, project written to %s\n", loaded, proj.c_str());

    // Round-trip it so a broken project format fails here rather than in the UI.
    Session back;
    if (!loadProject(back, proj, kSR, &err)) {
        std::fprintf(stderr, "loadProject failed: %s\n", err.c_str());
        return 1;
    }
    int clips = 0, notes = 0;
    for (auto& t : back.tracks)
        for (int i = 0; i < kMaxScenes; ++i) {
            if (!t.slots[i].valid()) continue;
            ++clips;
            notes += (int)t.slots[i].notes.size();
        }
    // The device and the notes are the only part of the set with nothing on
    // disk backing it, so they are the part worth checking by hand.
    if (back.tracks.size() != 5) {
        std::fprintf(stderr, "reloaded %zu tracks, expected 5\n", back.tracks.size());
        return 1;
    }
    const TrackModel& bk = back.tracks[kKeysTrack];
    const bool keysOk = bk.savedDevices.size() == 1 &&
                        bk.savedDevices[0].uri == kPulseUri &&
                        notes == kMelodyNotes * 3;
    std::printf("reloaded: %zu tracks, %zu scenes, %d clips (placed %d), "
                "%d notes, %zu device(s) on '%s', tempo %.1f\n",
                back.tracks.size(), back.scenes.size(), clips, placed, notes,
                bk.savedDevices.size(), bk.name.c_str(), back.tempo);
    // The round-trip is the assertion: every clip we placed must come back, and
    // so must the instrument that makes three of them audible.
    return (loaded == 4 && clips == placed && keysOk) ? 0 : 1;
}
