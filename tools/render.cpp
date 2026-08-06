// Offline renderer: drives the engine with no audio device and no GUI, then
// writes the result to a wav.
//
//   build/render <project.lattice> <out.wav> [--scene N] [--bars N] [--tempo BPM]
//                                            [--solo T] [--stem T] [--warp N]
//
// Devices and MIDI clips are part of the render. The set's saved plugins are
// instantiated and published as chains, and MIDI clips ship their notes across,
// exactly as the app does after a load -- so a pattern that sings in the UI
// sings here too, and a set that is nothing but instruments is not silent.
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
#include <memory>
#include <string>
#include <vector>

using namespace lat;

static constexpr int kBlock = 512;

// Everything this render owns on the engine's behalf. The audio thread only
// ever borrows chains and note arrays (see the RtChain / RtNote protocols in
// engine.h), and a synchronous offline loop has no audio thread to race: the
// last process() has returned by the time this is destroyed, so freeing here --
// once, at the end -- is exactly as safe as the retirement handshake the app
// needs and a great deal shorter.
struct Owned {
    std::vector<std::unique_ptr<PluginInstance>> insts;
    std::vector<RtChain*> chains;
    std::vector<RtNote*>  notes;
    ~Owned() {
        for (RtChain* c : chains) delete c;
        for (RtNote* n : notes) delete[] n;
    }
};

// Offline command pump.
//
// The engine's command ring holds 1024 entries and this tool pushes an entire
// set before the first process(): four mixer scalars per track plus one
// SetClip or ClearClip per cell, so a full 32x32 set needs about 1155. Past
// 1024 pushCommand returned false and every caller here ignored it, so a set
// that large simply could not be rendered.
//
// It failed loudly rather than quietly, and by luck rather than design:
// LaunchScene is pushed last, so it is always among the commands an overflow
// drops, nothing is ever launched, and the whole render comes out at digital
// zero -- which the silence guard at the bottom of this file catches and
// reports with a non-zero exit. Confirmed against the pre-fix binary on a
// 32x32 set: peak 0.0000, "FAIL: render is silent", exit 1.
//
// That is a thin thing to depend on. The guard fires because the *scene* never
// started; it says nothing about a set that starts and is missing some of its
// clips, and it does not survive anyone reordering these pushes so LaunchScene
// is not last. The same overflow the GUI hit; the GUI's fix is flow control
// across frames, because it has frames.
//
// Offline there is nothing to schedule around: no audio thread, no deadline.
// So when the ring is full, just run a block to drain it and push again.
// Everything here happens before LaunchScene, and the transport is stopped
// until then, so a drain block renders silence and advances no musical time.
//
// The retry is bounded and loud. drainCommands() empties the ring completely,
// so one block always clears it and a second failure means something is wrong
// that a third attempt will not fix.
static bool pumpCommand(Engine& e, const Command& c) {
    if (e.pushCommand(c)) return true;
    f32 l[kBlock], r[kBlock];
    e.process(nullptr, nullptr, l, r, kBlock);        // drains the ring
    if (e.pushCommand(c)) return true;
    std::fprintf(stderr,
                 "render: command ring still full after a drain block "
                 "(command type %u) -- this is a bug, not a large set\n",
                 (unsigned)c.type);
    return false;
}

static void pushClip(Engine& e, const Session& s, int t, int slot, Owned& own) {
    const ClipModel& m = s.tracks[t].slots[slot];
    Command c;
    c.a = t; c.b = slot;
    if (!m.valid()) { c.type = Cmd::ClearClip; pumpCommand(e, c); return; }
    c.type = Cmd::SetClip;
    RtClip rc;
    if (m.kind == ClipKind::Midi) {
        // The engine reads this array for as long as it holds the clip, so it
        // cannot be the session's live vector.
        if (!m.notes.empty()) {
            RtNote* fresh = new RtNote[m.notes.size()];
            for (size_t i = 0; i < m.notes.size(); ++i) {
                const NoteModel& n = m.notes[i];
                fresh[i].beat  = n.beat;
                fresh[i].len   = n.len;
                fresh[i].pitch = n.pitch;
                fresh[i].vel   = n.vel;
            }
            own.notes.push_back(fresh);
            rc.notes     = fresh;
            rc.noteCount = (int)m.notes.size();
        }
        rc.isMidi = true;
    } else {
        rc.data      = m.sample->data.data();
        rc.frames    = m.sample->frames;
        rc.channels  = m.sample->channels;
        rc.loopStart = m.loopStart;
        rc.loopEnd   = m.loopEnd > m.loopStart ? m.loopEnd : m.sample->frames;
        rc.clipBpm   = m.clipBpm;
        rc.warp      = (int)m.warp;
    }
    rc.lengthBeats = m.lengthBeats;
    rc.gain        = m.gain;
    rc.loop        = m.loop;
    rc.quantumIdx  = m.quantumIdx;
    rc.valid       = true;
    c.clip = rc;
    pumpCommand(e, c);
}

// savedDevices -> live instances -> one RtChain per track: the essence of
// App::materializeDevices followed by App::publishChain, with no GUI around it.
// A device whose plugin is not on this machine is reported and dropped, rather
// than keeping its slot -- nothing here will ever save the set back out, so the
// only thing a placeholder could do is confuse the chain order.
static void materializeDevices(Engine& e, const Session& s, PluginRegistry& reg,
                               f64 sr, Owned& own) {
    for (size_t t = 0; t < s.tracks.size(); ++t) {
        const TrackModel& tr = s.tracks[t];
        if (tr.savedDevices.empty()) continue;

        RtChain* chain = new RtChain();
        std::string line;
        for (const SavedDevice& sd : tr.savedDevices) {
            if (chain->count >= kMaxChainFx) {
                std::fprintf(stderr, "render: track %zu has more than %d devices"
                             " - the extras will not sound\n", t, kMaxChainFx);
                break;
            }
            const PluginDesc* found = reg.find(sd.uri);
            std::unique_ptr<PluginInstance> inst;
            if (found) inst = reg.instantiate(*found, sr, kBlock);
            if (!inst) {
                std::fprintf(stderr, "render: plugin not available, skipping: %s (%s)\n",
                             sd.name.empty() ? "?" : sd.name.c_str(), sd.uri.c_str());
                continue;
            }

            // Parameters are matched on ParamInfo::id, not on index: a plugin
            // can gain or reorder controls between versions, and dropping the
            // ones we no longer recognise beats applying them to the wrong one.
            const int n = inst->paramCount();
            int applied = 0;
            for (const std::pair<u32, f32>& pv : sd.params)
                for (int i = 0; i < n; ++i) {
                    if (inst->paramInfo(i).id != pv.first) continue;
                    inst->setParam(i, pv.second);
                    ++applied;
                    break;
                }
            inst->setBypassed(sd.bypass);

            // SavedDevice::state, AFTER the parameters and never before -- the
            // same ordering App::materializeDevices documents at length. A rack
            // restores its contents verbatim here; applying the state first
            // would let the macro writes above re-derive every mapped
            // parameter, and the render would not be what the set sounds like.
            std::string rackNote;
            if (RackControl* rc = inst->rack()) {
                RackState rs;
                if (rackStateFromString(sd.state, rs)) {
                    rc->setState(rs);
                    char buf[48];
                    std::snprintf(buf, sizeof buf, " {%d inside}", rc->deviceCount());
                    rackNote = buf;
                } else if (!sd.state.empty()) {
                    std::fprintf(stderr, "render: rack state did not parse for %s\n",
                                 sd.uri.c_str());
                }
            }

            if (!line.empty()) line += ", ";
            line += found->name.empty() ? sd.uri : found->name;
            if (applied > 0) {
                char buf[32];
                std::snprintf(buf, sizeof buf, " [%d param%s]", applied, applied == 1 ? "" : "s");
                line += buf;
            }
            line += rackNote;
            if (sd.bypass) line += " (bypassed)";

            chain->fx[chain->count++] = inst.get();
            own.insts.push_back(std::move(inst));
        }

        if (chain->count == 0) { delete chain; continue; }
        Command c;
        c.type = Cmd::SetChain;
        c.a = (i32)t;
        c.p = chain;
        // Through the pump like everything else. This one already reported the
        // overflow instead of swallowing it, which is why "track N has no
        // chain" was the only visible symptom of a bug whose other half --
        // every clip past the 1024th command -- was completely silent.
        if (!pumpCommand(e, c)) {
            std::fprintf(stderr, "render: track %zu has no chain\n", t);
            delete chain;
            continue;
        }
        own.chains.push_back(chain);
        std::printf("  %s: %s\n", tr.name.c_str(), line.c_str());
    }
}

static void usage(FILE* f) {
    std::fprintf(f,
        "usage: render <project.lattice> <out.wav> [options]\n"
        "  --scene N     scene to launch (default 0)\n"
        "  --bars N      bars to render (default 8)\n"
        "  --tempo BPM   render at this tempo instead of the project's\n"
        "  --solo T      render track T alone\n"
        "  --stem T      the same as --solo, named for what it is for:\n"
        "                one track on its own, to be mixed elsewhere\n"
        "  --warp N      force every clip to warp mode N\n");
}

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    std::setlocale(LC_NUMERIC, "C");

    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) { usage(stdout); return 0; }
    if (argc < 3) { usage(stderr); return 2; }
    const char* projPath = argv[1];
    const char* outPath  = argv[2];
    int scene = 0, bars = 8, solo = -1, warpOverride = -1;
    f64 tempoOverride = 0.0;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) scene = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--bars") && i + 1 < argc) bars = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tempo") && i + 1 < argc) tempoOverride = atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--solo") && i + 1 < argc) solo = atoi(argv[++i]);
        // A stem is one track rendered on its own, which is a solo render with
        // a name that says why you asked for it.
        else if (!std::strcmp(argv[i], "--stem") && i + 1 < argc) solo = atoi(argv[++i]);
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
    eng.prepare(sr, kBlock);
    Owned own;

    // The scan walks every LV2 bundle on the system, which costs the better
    // part of a second; a set with no devices in it should not pay that.
    bool anyDevices = false;
    for (const TrackModel& tr : s.tracks) if (!tr.savedDevices.empty()) { anyDevices = true; break; }
    if (anyDevices) {
        PluginRegistry reg;
        reg.scan();
        std::printf("devices:\n");
        materializeDevices(eng, s, reg, sr, own);
    }

    Command c;
    c.type = Cmd::SetTempo; c.x = s.tempo; pumpCommand(eng, c);
    c = Command{}; c.type = Cmd::SetQuantum; c.a = s.quantumIdx; pumpCommand(eng, c);

    // The signature map, before anything is launched. Without this the engine
    // renders every set in 4/4 while the length below is computed from the map,
    // and the two disagree by exactly the amount the set is not in 4/4 -- a
    // render that stops mid-phrase, with nothing to say why. The pointer is
    // owned for the life of the render, like the chains and note arrays above;
    // Ev::SigsRetired is drained with the rest at the end.
    const RtSig* sigs = publishSignatures(eng, s);
    for (size_t t = 0; t < s.tracks.size(); ++t) {
        const TrackModel& tr = s.tracks[t];
        c = Command{}; c.type = Cmd::TrackVol;  c.a = (i32)t; c.x = faderToGain(tr.fader); pumpCommand(eng, c);
        c = Command{}; c.type = Cmd::TrackPan;  c.a = (i32)t; c.x = tr.pan;  pumpCommand(eng, c);
        c = Command{}; c.type = Cmd::TrackMute; c.a = (i32)t; c.b = tr.mute; pumpCommand(eng, c);
        c = Command{}; c.type = Cmd::TrackSolo; c.a = (i32)t; c.b = tr.solo; pumpCommand(eng, c);
        for (int sl = 0; sl < (int)s.scenes.size(); ++sl) pushClip(eng, s, (int)t, sl, own);
    }
    c = Command{}; c.type = Cmd::LaunchScene; c.a = scene; pumpCommand(eng, c);

    // Bars to beats through the signature map, not `sigNum * bars`. Those agree
    // exactly when the set never changes signature, which is why the demo
    // renders stay bit-identical -- and disagree the moment it does. A set that
    // opens in 7/8 rendered as though every bar were 4/4 comes out long by
    // 12.5% and cut off at a place that looks like a bug in the scheduler.
    const f64 beats  = s.beatOfBar((f64)bars) - s.beatOfBar(0.0);
    const i64 total  = (i64)(sr * 60.0 / s.tempo * beats);
    std::vector<f32> l(kBlock), r(kBlock), inter;
    inter.reserve((size_t)total * 2);

    f32 peak = 0.f;
    f64 sumSq = 0.0;
    for (i64 done = 0; done < total; done += kBlock) {
        const int n = (int)std::min<i64>(kBlock, total - done);
        eng.process(nullptr, nullptr, l.data(), r.data(), n);
        for (int i = 0; i < n; ++i) {
            inter.push_back(l[i]);
            inter.push_back(r[i]);
            peak = std::max({peak, std::fabs(l[i]), std::fabs(r[i])});
            sumSq += (f64)l[i] * l[i] + (f64)r[i] * r[i];
        }
    }

    // Nothing will call process() again, so every borrow is over. Drain the
    // event ring first anyway: retirement events are how the engine says it is
    // done with a chain or a note array, and reading them keeps this loop
    // honest about the protocol it is short-circuiting. `own` frees the rest
    // when it goes out of scope.
    Event ev;
    while (eng.popEvent(ev)) {}
    delete[] sigs;   // published above; nothing will read it again

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
