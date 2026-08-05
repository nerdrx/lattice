#include "app.h"
#include "../core/project.h"
#include "../gfx/gl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

namespace lat {

// Layout constants, in logical px before the DPI scale is applied.
namespace lay {
constexpr f32 controlBarH = 38.f;
constexpr f32 statusH     = 20.f;
constexpr f32 trackHeadH  = 21.f;
constexpr f32 slotH       = 21.f;
constexpr f32 sceneColW   = 96.f;
constexpr f32 masterW     = 92.f;
constexpr f32 mixerH      = 152.f;
constexpr f32 gutter      = 1.f;
}

// How much audio a single take can hold. Two minutes of interleaved stereo
// floats is ~46 MB at 48 kHz: cheap enough to allocate up front, long enough
// that no realistic loop or verse runs out of room. The engine stops at the
// capacity it was given, so overrunning truncates rather than corrupts.
constexpr f64 kRecordSeconds = 120.0;

static f64 nowSeconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

static std::string homeDir() {
    if (const char* h = getenv("HOME")) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

static bool isAudioFile(const std::string& n) {
    static const char* ext[] = {".wav", ".flac", ".aiff", ".aif", ".ogg", ".mp3", ".opus", ".w64", nullptr};
    const size_t dot = n.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = n.substr(dot);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    for (int i = 0; ext[i]; ++i) if (e == ext[i]) return true;
    return false;
}

// Case-insensitive substring test. Used by the plugin filter and by the
// LATTICE_DEBUG_ADDFX hook, both of which match on what the user typed rather
// than on an exact name.
static bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    const size_t n = hay.size() - needle.size();
    for (size_t i = 0; i <= n; ++i)
        if (strncasecmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool App::init(int argc, char** argv) {
    if (!win_.create("Lattice", 1360, 860)) return false;
    if (!rend_.init()) return false;

    const f32 s = win_.dpiScale();
    const std::string reg = findSystemFont(false);
    const std::string bold = findSystemFont(true);
    if (reg.empty()) { LOGE("no usable system font found"); return false; }
    fSmall_.load(reg.c_str(),  (int)std::lround(9.f * s));
    fBody_.load(reg.c_str(),   (int)std::lround(11.f * s));
    fBold_.load(bold.empty() ? reg.c_str() : bold.c_str(), (int)std::lround(11.f * s));
    fBig_.load(bold.empty() ? reg.c_str() : bold.c_str(),  (int)std::lround(15.f * s));

    ui_.r = &rend_;
    ui_.in = &win_.input();
    ui_.fSmall = &fSmall_;
    ui_.fBody = &fBody_;
    ui_.fBold = &fBold_;
    ui_.fBig = &fBig_;

    audio_ = createBackend(engine_, getenv("LATTICE_AUDIO"));
    if (!audio_) {
        LOGW("no audio backend available - running silent");
        engine_.prepare(48000.0, 1024);
    }

    // MIDI comes up after the audio backend: the reader thread pushes straight
    // into the engine's ring, so the engine must already be prepared. Missing
    // hardware or a missing sequencer device is not an error - a set can be
    // played entirely from the mouse.
    if (midi_.start(engine_)) LOGI("midi in: alsa seq client %d:0", midi_.clientId());
    else                      LOGW("no MIDI input - continuing without it");

    // Default set: eight audio tracks, eight scenes, same as a fresh Live set.
    ses_.tracks.resize(8);
    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        char buf[32];
        snprintf(buf, sizeof buf, "%zu Audio", i + 1);
        ses_.tracks[i].uid = ses_.newUid();
        ses_.tracks[i].name = buf;
        ses_.tracks[i].colorIdx = (int)(i * 3 + 4) % pal::clipColorCount;
    }
    ses_.scenes.resize(8);
    for (size_t i = 0; i < ses_.scenes.size(); ++i) {
        char buf[32];
        snprintf(buf, sizeof buf, "Scene %zu", i + 1);
        ses_.scenes[i].uid = ses_.newUid();
        ses_.scenes[i].name = buf;
    }

    browserPlaces_ = {homeDir() + "/Music", homeDir() + "/Downloads", homeDir(), "/usr/share/sounds"};
    browseTo(browserPlaces_[0]);
    if (browserItems_.empty()) browseTo(homeDir());

    // A project path on the command line loads instead of the default set.
    // openProject() pushes the whole restored set to the engine itself, so only
    // the default-set path needs the initial sync here.
    if (argc > 1) {
        if (!openProject(argv[1])) LOGW("could not load %s: %s", argv[1], status_.c_str());
    } else {
        pushAll();
    }
    status_ = "Ready";

    // Headless verification hook. With LATTICE_DEBUG_ADDFX=<substring> set, the
    // first scanned plugin whose name matches is loaded onto track 0 and the
    // DEVICES tab is opened, so tools/headless_test.sh can screenshot a
    // populated device chain without anything driving the mouse.
    if (const char* want = getenv("LATTICE_DEBUG_ADDFX")) {
        ensurePluginScan();
        const PluginDesc* hit = nullptr;
        for (const PluginDesc& d : registry_.plugins())
            if (icontains(d.name, want)) { hit = &d; break; }
        if (!hit) {
            LOGW("LATTICE_DEBUG_ADDFX: no plugin matching \"%s\"", want);
        } else if (!ses_.tracks.empty()) {
            selTrack_ = 0;
            addDeviceToTrack(0, *hit);
            selDevice_ = (int)ses_.tracks[0].devices.size() - 1;
            detailTab_ = DetailTab::Devices;
            showDetail_ = true;
        }
    }

    LOGI("backend: %s   audio: %s", win_.backendName(), audio_ ? audio_->name() : "none");
    return true;
}

void App::shutdown() {
    // Order matters. The MIDI reader goes first: it pushes into the engine's
    // ring from its own thread, so it has to be joined before anything else
    // starts tearing the engine down, or a push could land in a ring nobody
    // owns any more.
    midi_.stop();

    // Stopping the backend joins the audio thread, so once it returns nothing
    // can be inside process() and nothing can be following a published chain
    // or writing into a capture buffer. Only then is it safe to free either
    // without the Ev::ChainRetired / Ev::RecordFinished handshake — the events
    // still sitting in the ring will never be drained, so waiting for them here
    // would deadlock or leak.
    if (audio_) { audio_->stop(); audio_.reset(); }
    for (const RtChain*& c : published_) { delete c; c = nullptr; }
    for (RetiredChain& rc : retiring_) delete rc.chain;
    retiring_.clear();          // frees the instances the chains had dropped
    for (PendingRec& p : pendingRecs_) delete[] p.buf;
    pendingRecs_.clear();
    // Instances still on tracks die with ses_ when App is destroyed, which is
    // after this point and therefore also after the audio thread is gone.

    fSmall_.destroy(); fBody_.destroy(); fBold_.destroy(); fBig_.destroy();
    rend_.shutdown();
    win_.destroy();
}

void App::run() {
    lastFrameTime_ = nowSeconds();
    while (running_ && win_.pump()) {
        frame();
        win_.swap();
    }
}

// ---------------------------------------------------------------------------
// engine plumbing
// ---------------------------------------------------------------------------

void App::send(Cmd t, i32 a, i32 b, f64 x) {
    Command c;
    c.type = t; c.a = a; c.b = b; c.x = x;
    engine_.pushCommand(c);
}

void App::pushClip(int track, int slot) {
    Command c;
    c.type = Cmd::SetClip;
    c.a = track; c.b = slot;
    const ClipModel& m = ses_.tracks[track].slots[slot];
    if (!m.valid()) { c.type = Cmd::ClearClip; engine_.pushCommand(c); return; }

    RtClip rc;
    rc.data         = m.sample->data.data();
    rc.frames       = m.sample->frames;
    rc.channels     = m.sample->channels;
    rc.loopStart    = m.loopStart;
    rc.loopEnd      = m.loopEnd > m.loopStart ? m.loopEnd : m.sample->frames;
    rc.clipBpm      = m.clipBpm;
    rc.lengthBeats  = m.lengthBeats;
    rc.gain         = m.gain;
    rc.warp         = (int)m.warp;
    rc.loop         = m.loop;
    rc.quantumIdx   = m.quantumIdx;
    rc.prob         = m.prob;
    rc.followAction = (int)m.followAction;
    rc.followBeats  = m.followBeats;
    rc.valid        = true;
    c.clip = rc;
    engine_.pushCommand(c);
}

void App::pushTrack(int t) {
    const TrackModel& tr = ses_.tracks[t];
    send(Cmd::TrackVol,  t, 0, faderToGain(tr.fader));
    send(Cmd::TrackPan,  t, 0, tr.pan);
    send(Cmd::TrackMute, t, tr.mute ? 1 : 0);
    send(Cmd::TrackSolo, t, tr.solo ? 1 : 0);
    send(Cmd::TrackArm,  t, tr.arm ? 1 : 0);
}

void App::pushAll() {
    send(Cmd::SetTempo, 0, 0, ses_.tempo);
    send(Cmd::SetQuantum, ses_.quantumIdx);
    send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    for (size_t t = 0; t < ses_.tracks.size(); ++t) {
        pushTrack((int)t);
        for (int s = 0; s < (int)ses_.scenes.size(); ++s) pushClip((int)t, s);
    }
}

void App::pumpEngineEvents() {
    Event e;
    while (engine_.popEvent(e)) {
        if (e.type == Ev::ChainRetired) {
            // The audio thread has swapped this chain out and will never look
            // at it again, so the struct and every instance it was the last
            // reference to can finally go.
            const RtChain* old = (const RtChain*)e.p;
            if (!old) continue;
            auto it = retiring_.begin();
            for (; it != retiring_.end(); ++it) if (it->chain == old) break;
            if (it == retiring_.end()) {
                LOGW("ChainRetired for an unknown chain %p - leaking it rather "
                     "than freeing a pointer we do not own", (const void*)old);
                continue;
            }
            delete it->chain;
            retiring_.erase(it);
            continue;
        }
        if (e.type == Ev::RecordStarted) {
            // The quantized start has fired. Remember the beat it began on so
            // the slot can count elapsed beats without the engine having to
            // publish another atomic.
            if (e.a >= 0 && e.a < kMaxTracks) recStartBeat_[e.a] = e.x;
            char buf[80];
            snprintf(buf, sizeof buf, "Recording %s  scene %d",
                     (e.a >= 0 && e.a < (int)ses_.tracks.size()) ? ses_.tracks[e.a].name.c_str() : "?",
                     e.b + 1);
            status_ = buf;
            continue;
        }
        if (e.type == Ev::RecordFinished) {
            finishRecording(e);
            continue;
        }
        // Everything else is reserved for undo hooks; the UI polls atomics for
        // transport and clip state.
    }
}

// ---------------------------------------------------------------------------
// device chains
//
// See the lifecycle comment in app.h: the GUI owns every RtChain and every
// PluginInstance, the audio thread only borrows them, and the handshake below
// is the only path on which anything is freed while audio runs.
// ---------------------------------------------------------------------------

void App::publishChain(int track) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    TrackModel& t = ses_.tracks[track];

    RtChain* chain = new RtChain();
    int n = 0;
    for (const DeviceModel& d : t.devices) {
        if (!d.inst) continue;
        if (n >= kMaxChainFx) {
            LOGW("track %d has more than %d devices - the extras will not sound",
                 track, kMaxChainFx);
            break;
        }
        // Bypassed devices stay in the chain: the instance itself short-circuits
        // in process(), which keeps the chain stable across a bypass toggle.
        chain->fx[n++] = d.inst.get();
    }
    chain->count = n;

    Command c;
    c.type = Cmd::SetChain;
    c.a = track;
    c.p = chain;
    if (!engine_.pushCommand(c)) {
        // The ring is full, so the engine never saw this chain. It is still
        // solely ours, and the previously published one is still live: drop the
        // new one and leave every piece of state exactly as it was.
        LOGW("command ring full - chain for track %d not published", track);
        delete chain;
        return;
    }

    if (published_[track]) retiring_.push_back(RetiredChain{published_[track], {}});
    published_[track] = chain;
}

void App::addDeviceToTrack(int track, const PluginDesc& d) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    TrackModel& t = ses_.tracks[track];
    if ((int)t.devices.size() >= kMaxChainFx) {
        status_ = "Chain is full";
        return;
    }

    // instantiate() already calls prepare() on the instance (see the tail of
    // instantiateLV2/instantiateCLAP), so a non-null return is ready to run.
    std::unique_ptr<PluginInstance> inst =
        registry_.instantiate(d, engine_.sampleRate(), kMaxBlock);
    if (!inst) {
        status_ = "Could not load " + d.name;
        return;
    }

    DeviceModel dm;
    dm.uid = ses_.newUid();
    dm.desc = d;
    dm.inst = std::move(inst);
    t.devices.push_back(std::move(dm));

    const RtChain* before = published_[track];
    publishChain(track);
    if (published_[track] == before) {
        // Publish failed. The engine never referenced this instance, so it is
        // safe to destroy right here and leave the model matching the engine.
        t.devices.pop_back();
        status_ = "Engine busy - device not added";
        return;
    }
    selDevice_ = (int)t.devices.size() - 1;
    paramScroll_ = 0.f;
    status_ = "Added " + d.name;
}

void App::removeDevice(int track, int idx) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    TrackModel& t = ses_.tracks[track];
    if (idx < 0 || idx >= (int)t.devices.size()) return;

    // Move the instance out of the model rather than letting erase() destroy
    // it: the audio thread is still running the *outgoing* chain, which points
    // straight at it. It may only die once that chain comes back to us.
    DeviceModel dead = std::move(t.devices[idx]);
    t.devices.erase(t.devices.begin() + idx);

    const RtChain* outgoing = published_[track];
    publishChain(track);

    if (published_[track] == outgoing) {
        // Publish failed; the engine still runs the old chain, so the device
        // has to go back where it was or the model would lie about what sounds.
        t.devices.insert(t.devices.begin() + idx, std::move(dead));
        status_ = "Engine busy - device not removed";
        return;
    }
    if (outgoing) {
        // publishChain() just appended the entry for `outgoing`; the instance
        // rides along in it and is freed when Ev::ChainRetired arrives.
        retiring_.back().dying.push_back(std::move(dead.inst));
    }
    // Otherwise nothing was ever published, so nothing borrowed the instance
    // and it is freed as `dead` goes out of scope.

    if (t.devices.empty())                selDevice_ = -1;
    else if (selDevice_ >= (int)t.devices.size()) selDevice_ = (int)t.devices.size() - 1;
    paramScroll_ = 0.f;
    status_ = "Removed " + dead.desc.name;
}

void App::ensurePluginScan() {
    if (registryScanned_) return;
    // lilv walks every bundle on the system and a CLAP scan dlopens each
    // binary, which costs the better part of a second. Deferring it to the
    // first time the DEVICES tab opens keeps startup snappy for anyone who
    // never touches a plugin.
    status_ = "Scanning plugins...";
    registry_.scan();
    registryScanned_ = true;
    char buf[48];
    snprintf(buf, sizeof buf, "%zu plugins", registry_.plugins().size());
    status_ = buf;
}

// ---------------------------------------------------------------------------
// project: identity, device materialization, load / save
// ---------------------------------------------------------------------------

// Anything that reaches the App without an identity gets one here: entities
// built before UIDs existed, and anything a future loader forgets to stamp.
// The counter is also pulled past every UID actually in use, so a legacy file
// with hand-written IDs can never collide with the ones we hand out next.
void App::assignUids() {
    u64 seen = 0;
    auto note = [&](u64 id) { if (id > seen) seen = id; };
    for (const TrackModel& t : ses_.tracks) {
        note(t.uid);
        for (int s = 0; s < kMaxScenes; ++s) note(t.slots[s].uid);
        for (const DeviceModel& d : t.devices)   note(d.uid);
        for (const SavedDevice& d : t.savedDevices) note(d.uid);
    }
    for (const SceneModel& s : ses_.scenes) note(s.uid);
    if (ses_.nextUid <= seen) ses_.nextUid = seen + 1;

    for (TrackModel& t : ses_.tracks) {
        if (!t.uid) t.uid = ses_.newUid();
        for (int s = 0; s < kMaxScenes; ++s) {
            ClipModel& c = t.slots[s];
            // An empty slot is not an entity: only a clip that exists, or one
            // whose audio went missing but whose reference survived, gets one.
            if (!c.uid && (c.valid() || !c.path.empty())) c.uid = ses_.newUid();
        }
        for (DeviceModel& d : t.devices)        if (!d.uid) d.uid = ses_.newUid();
        for (SavedDevice& d : t.savedDevices)   if (!d.uid) d.uid = ses_.newUid();
    }
    for (SceneModel& s : ses_.scenes) if (!s.uid) s.uid = ses_.newUid();
}

// devices -> savedDevices. The project layer only ever sees the passive form,
// so this is the one place that reads a live instance for persistence.
void App::serializeDevices() {
    for (TrackModel& t : ses_.tracks) {
        t.savedDevices.clear();
        t.savedDevices.reserve(t.devices.size());
        for (DeviceModel& d : t.devices) {
            if (!d.uid) d.uid = ses_.newUid();
            SavedDevice sd;
            sd.uid = d.uid;
            sd.uri = d.desc.uri;
            sd.name = d.desc.name;
            sd.bypass = d.bypass;
            if (d.inst) {
                const int n = d.inst->paramCount();
                sd.params.reserve((size_t)n);
                for (int i = 0; i < n; ++i)
                    sd.params.push_back({d.inst->paramInfo(i).id, d.inst->getParam(i)});
            } else {
                // A device whose plugin was missing at load time. Its saved
                // values were parked on the model rather than thrown away, so
                // the set round-trips unchanged on a machine that does not have
                // the plugin and works again on one that does.
                sd.params = d.lostParams;
            }
            t.savedDevices.push_back(std::move(sd));
        }
    }
}

// savedDevices -> devices. Every entry keeps its slot in the chain even if the
// plugin is gone, so the order a set was saved with is the order it comes back
// with once the missing plugin is installed.
void App::materializeDevices() {
    bool any = false;
    for (const TrackModel& t : ses_.tracks) if (!t.savedDevices.empty()) { any = true; break; }
    if (!any) return;
    // The scan is chatty about its progress in the status bar; a load that goes
    // through cleanly should still read as a load when it is done.
    const std::string prevStatus = status_;
    ensurePluginScan();

    int missing = 0;
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        TrackModel& t = ses_.tracks[ti];
        if (t.savedDevices.empty()) continue;

        for (SavedDevice& sd : t.savedDevices) {
            DeviceModel dm;
            dm.uid = sd.uid;
            dm.bypass = sd.bypass;

            const PluginDesc* found = registry_.find(sd.uri);
            std::unique_ptr<PluginInstance> inst;
            if (found) inst = registry_.instantiate(*found, engine_.sampleRate(), kMaxBlock);

            if (!inst) {
                ++missing;
                LOGW("plugin not available: %s (%s)", sd.name.c_str(), sd.uri.c_str());
                if (found) dm.desc = *found;
                else {
                    dm.desc.uri = sd.uri;
                    dm.desc.name = sd.name;
                }
                dm.lostParams = sd.params;
                t.devices.push_back(std::move(dm));
                continue;
            }

            // Parameters are matched on ParamInfo::id, not on index: a plugin
            // can gain or reorder controls between versions, and dropping the
            // ones we no longer recognise beats applying them to the wrong
            // control.
            const int n = inst->paramCount();
            for (const std::pair<u32, f32>& pv : sd.params) {
                for (int i = 0; i < n; ++i) {
                    if (inst->paramInfo(i).id != pv.first) continue;
                    inst->setParam(i, pv.second);
                    break;
                }
            }
            inst->setBypassed(sd.bypass);

            dm.desc = *found;
            dm.inst = std::move(inst);
            t.devices.push_back(std::move(dm));
        }

        // The live models are now the truth; the passive copies are rebuilt
        // from them at the next save, missing plugins included.
        t.savedDevices.clear();
        publishChain((int)ti);
    }

    if (missing > 0) {
        char buf[80];
        snprintf(buf, sizeof buf, "%d device%s could not be loaded - kept in the set",
                 missing, missing == 1 ? "" : "s");
        status_ = buf;
    } else {
        status_ = prevStatus;
    }
}

// Hands every published chain and every instance over to the retirement flow.
// Used before a load replaces the session wholesale: the tracks are about to be
// destroyed, and destroying a PluginInstance the audio thread is still running
// is the one thing the chain protocol exists to prevent.
void App::releaseAllChains() {
    for (int t = 0; t < kMaxTracks; ++t) {
        const bool hasDevices = t < (int)ses_.tracks.size() && !ses_.tracks[t].devices.empty();
        if (!published_[t] && !hasDevices) continue;

        RtChain* empty = new RtChain();
        Command c;
        c.type = Cmd::SetChain;
        c.a = t;
        c.p = empty;
        const bool sent = engine_.pushCommand(c);
        if (!sent) {
            LOGW("command ring full - track %d keeps running its old chain", t);
            delete empty;
        }

        RetiredChain rc;
        if (hasDevices)
            for (DeviceModel& d : ses_.tracks[t].devices)
                if (d.inst) rc.dying.push_back(std::move(d.inst));

        if (sent) {
            rc.chain = published_[t];       // may be null: nothing was published
            published_[t] = empty;
        }
        // rc.chain stays null when nothing was ever published, or when the send
        // failed and the engine is therefore still following the old chain.
        // Either way no Ev::ChainRetired will ever match this entry, so it sits
        // in retiring_ until shutdown() - which is after the audio thread is
        // joined, and the only moment freeing it unilaterally is safe.
        if (rc.chain || !rc.dying.empty()) retiring_.push_back(std::move(rc));
    }
}

bool App::openProject(const std::string& path) {
    // Load into a scratch session first. loadProject() leaves its target alone
    // on a parse error, but the session is only *ours* to throw away once we
    // know a replacement exists - and throwing it away means retiring chains,
    // which is not something to do speculatively.
    Session next;
    std::string err;
    if (!loadProject(next, path, engine_.sampleRate(), &err)) {
        status_ = "Load failed: " + err;
        return false;
    }

    releaseAllChains();
    ses_ = std::move(next);

    // Identity first: everything below hands out fresh UIDs, and they must come
    // from a counter that has already been pulled past whatever the file used.
    assignUids();

    // A set with nothing in it would leave the views indexing past the end.
    if (ses_.tracks.empty()) addTrack();
    if (ses_.scenes.empty()) addScene();
    selTrack_ = clampv(selTrack_, 0, (int)ses_.tracks.size() - 1);
    selSlot_  = clampv(selSlot_,  0, (int)ses_.scenes.size() - 1);
    selDevice_ = -1;

    status_ = "Loaded " + path;
    materializeDevices();          // may replace the status with its own warning
    pushAll();
    return true;
}

void App::saveProjectTo(const std::string& path) {
    serializeDevices();
    std::string err;
    status_ = saveProject(ses_, path, &err) ? ("Saved " + path) : ("Save failed: " + err);
}

void App::setTempo(f64 bpm) {
    ses_.tempo = clampv(bpm, 20.0, 999.0);
    send(Cmd::SetTempo, 0, 0, ses_.tempo);
}

void App::togglePlay() {
    const bool p = engine_.playing.load();
    send(Cmd::SetPlaying, p ? 0 : 1);
    status_ = p ? "Stopped" : "Playing";
}

void App::loadClipInto(int track, int slot, const std::string& path) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;
    SampleRef sb = loadSample(path, engine_.sampleRate());
    if (!sb) { status_ = "Could not load " + path; return; }

    ClipModel& m = ses_.tracks[track].slots[slot];
    // A slot that already held a clip keeps its identity: the material behind
    // it changed, but anything pointing at the clip (automation, a controller
    // mapping) still means this clip.
    if (!m.uid) m.uid = ses_.newUid();
    m.sample = sb;
    m.path = path;
    m.name = sb->name;
    const size_t dot = m.name.find_last_of('.');
    if (dot != std::string::npos) m.name = m.name.substr(0, dot);
    m.colorIdx = ses_.tracks[track].colorIdx;
    m.clipBpm = sb->guessedBpm;
    m.lengthBeats = sb->guessedBeats;
    m.loopStart = 0;
    m.loopEnd = sb->frames;
    m.gain = 1.f;
    m.warp = Warp::Beats;
    m.loop = true;
    pushClip(track, slot);
    selTrack_ = track; selSlot_ = slot;
    status_ = "Loaded " + m.name;
}

void App::clearClip(int track, int slot) {
    ses_.tracks[track].slots[slot] = ClipModel{};
    send(Cmd::ClearClip, track, slot);
}

void App::addTrack() {
    if (ses_.tracks.size() >= kMaxTracks) return;
    TrackModel t;
    char buf[32];
    snprintf(buf, sizeof buf, "%zu Audio", ses_.tracks.size() + 1);
    t.uid = ses_.newUid();
    t.name = buf;
    t.colorIdx = (int)(ses_.tracks.size() * 3 + 4) % pal::clipColorCount;
    ses_.tracks.push_back(std::move(t));   // TrackModel is move-only (devices)
    pushTrack((int)ses_.tracks.size() - 1);
}

void App::addScene() {
    if (ses_.scenes.size() >= kMaxScenes) return;
    SceneModel s;
    char buf[32];
    snprintf(buf, sizeof buf, "Scene %zu", ses_.scenes.size() + 1);
    s.uid = ses_.newUid();
    s.name = buf;
    ses_.scenes.push_back(s);
}

// ---------------------------------------------------------------------------
// recording
//
// Live's semantics: an empty slot on an armed track is a record target while
// the global record button is lit. The first click queues a take (quantized by
// the engine like any launch), the second stops it. The capture buffer is
// allocated here and stays ours until Ev::RecordFinished brings it back; see
// the Cmd::RecordSlot contract in engine.h.
// ---------------------------------------------------------------------------

void App::startRecording(int track, int slot) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;
    if (engine_.recState[track].load() != 0) {
        status_ = "Track is already recording";
        return;
    }

    const i64 cap = (i64)std::llround(engine_.sampleRate() * kRecordSeconds);
    // Zeroed rather than raw: a take that stops early leaves the tail unwritten,
    // and silence is a far better failure than whatever was on that page.
    f32* buf = new (std::nothrow) f32[(size_t)cap * 2]();
    if (!buf) {
        status_ = "Out of memory - recording not started";
        return;
    }

    Command c;
    c.type = Cmd::RecordSlot;
    c.a = track; c.b = slot;
    c.p = buf;
    c.x = (f64)cap;
    if (!engine_.pushCommand(c)) {
        // The engine never saw the buffer, so it is still solely ours.
        delete[] buf;
        status_ = "Engine busy - recording not started";
        return;
    }

    pendingRecs_.push_back(PendingRec{buf, cap, track, slot});
    selTrack_ = track; selSlot_ = slot;
    status_ = "Record armed";
}

void App::stopRecording(int track) {
    for (const PendingRec& p : pendingRecs_) {
        if (p.track != track) continue;
        // The same command toggles. Resend the buffer it was started with
        // rather than a null: the stop is a second RecordSlot for this slot,
        // and repeating the payload means an engine that simply reassigns
        // recBuf lands on exactly what it already had.
        Command c;
        c.type = Cmd::RecordSlot;
        c.a = track; c.b = p.slot;
        c.p = p.buf;
        c.x = (f64)p.capFrames;
        if (!engine_.pushCommand(c)) status_ = "Engine busy - still recording";
        return;
    }
}

void App::finishRecording(const Event& e) {
    f32* buf = (f32*)e.p;
    if (!buf) return;

    auto it = pendingRecs_.begin();
    for (; it != pendingRecs_.end(); ++it) if (it->buf == buf) break;
    if (it == pendingRecs_.end()) {
        LOGW("RecordFinished for an unknown buffer %p - leaking it rather than "
             "freeing a pointer we do not own", (const void*)buf);
        return;
    }

    const int track = e.a, slot = e.b;
    const i64 frames = (i64)e.x;
    const bool inRange = track >= 0 && track < (int)ses_.tracks.size() &&
                         slot  >= 0 && slot  < (int)ses_.scenes.size();

    if (frames > 0 && inRange) {
        char name[32];
        snprintf(name, sizeof name, "Rec %d", recTakeNo_++);
        SampleRef sb = sampleFromRecording(buf, frames, engine_.sampleRate(), ses_.tempo, name);
        if (sb) {
            ClipModel& m = ses_.tracks[track].slots[slot];
            m = ClipModel{};
            m.uid = ses_.newUid();
            m.sample = sb;
            m.name = name;
            m.colorIdx = ses_.tracks[track].colorIdx;
            // The take was played to the session clock, so its tempo is the
            // session tempo by construction - nothing to guess here.
            m.clipBpm = ses_.tempo;
            m.lengthBeats = sb->guessedBeats;
            m.loopStart = 0;
            m.loopEnd = sb->frames;
            m.gain = 1.f;
            m.warp = Warp::Beats;
            m.loop = true;
            pushClip(track, slot);
            selTrack_ = track; selSlot_ = slot;
            status_ = std::string("Recorded ") + name;
        } else {
            status_ = "Recording failed";
        }
    } else if (frames <= 0) {
        // Stopped before the quantized start ever fired: nothing was captured,
        // so there is no clip to make and the slot stays empty.
        status_ = "Recording cancelled";
    }

    delete[] buf;
    pendingRecs_.erase(it);
}

// ---------------------------------------------------------------------------
// browser
// ---------------------------------------------------------------------------

void App::browseTo(const std::string& dir) {
    browserDir_ = dir;
    refreshBrowser();
    browserScroll_ = 0.f;
}

void App::refreshBrowser() {
    browserItems_.clear();
    DIR* d = opendir(browserDir_.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." ) continue;
        if (n != ".." && n[0] == '.') continue;          // skip dotfiles
        const std::string full = browserDir_ + "/" + n;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        BrowserEntry be;
        be.name = n;
        be.path = full;
        be.isDir = S_ISDIR(st.st_mode);
        be.isAudio = !be.isDir && isAudioFile(n);
        if (!be.isDir && !be.isAudio) continue;          // only show what we can use
        browserItems_.push_back(be);
    }
    closedir(d);
    std::sort(browserItems_.begin(), browserItems_.end(), [](const BrowserEntry& a, const BrowserEntry& b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.isDir != b.isDir) return a.isDir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

void App::frame() {
    const f64 t = nowSeconds();
    const f32 dt = (f32)(t - lastFrameTime_);
    lastFrameTime_ = t;
    fps_ = fps_ * 0.92f + (dt > 0.f ? 1.f / dt : 0.f) * 0.08f;

    pumpEngineEvents();

    const f32 s = win_.dpiScale();
    const f32 W = (f32)win_.width(), H = (f32)win_.height();

    rend_.begin(win_.width(), win_.height(), s);
    glClearColor(pal::appBg.r, pal::appBg.g, pal::appBg.b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ui_.beginFrame();
    handleShortcuts();

    Rect full{0, 0, W, H};
    Rect bar = {0, 0, W, lay::controlBarH * s};
    Rect status = {0, H - lay::statusH * s, W, lay::statusH * s};
    Rect body = {0, bar.bottom(), W, status.y - bar.bottom()};

    drawControlBar(bar);

    Rect detail{};
    if (showDetail_ && view_ == MainView::Session) {
        detail = {0, body.bottom() - detailH_ * s, W, detailH_ * s};
        body.h -= detail.h;
    }

    Rect main = body;
    if (showBrowser_) {
        Rect br = {0, body.y, browserW_ * s, body.h};
        drawBrowser(br);
        main = {br.right(), body.y, W - br.right(), body.h};
    }

    if (view_ == MainView::Session) drawSessionView(main);
    else                            drawArrangementView(main);

    if (showDetail_ && view_ == MainView::Session) drawDetailPanel(detail);
    drawStatusBar(status);
    drawDragGhost();

    ui_.endFrame();
    win_.setCursor(ui_.cursor);
    rend_.end();
    (void)full;
}

void App::handleShortcuts() {
    Input& in = win_.input();

    // Ahead of the edit guard on purpose: when a text field takes focus while a
    // piano key is still held, this call is what releases the note.
    updateKbdPiano();

    if (ui_.editId) return;                      // typing takes precedence

    // Live's Computer MIDI Keyboard toggle. Edge-detected on keyDown[] rather
    // than keyPressed[], which repeats.
    const bool tgl = in.keyDown['k'] && in.ctrl() && in.shift();
    if (tgl && !kbdTogglePrev_) toggleKbdMidi();
    kbdTogglePrev_ = tgl;

    // An unmodified key that the piano owns is a note, not a shortcut. Ctrl-
    // and Alt-modified chords are unaffected: notes only fire unmodified.
    const auto plain = [&](int k) {
        return in.keyPressed[k] && !in.ctrl() && !(kbdMidi_ && KbdPiano::consumes(k));
    };

    if (in.keyPressed[' ']) togglePlay();
    if (in.keyPressed[KeyTab])
        view_ = (view_ == MainView::Session) ? MainView::Arrangement : MainView::Session;
    if (in.keyPressed['b'] && in.ctrl()) showBrowser_ = !showBrowser_;
    if (in.keyPressed['d'] && in.ctrl()) showDetail_ = !showDetail_;
    if (plain('m')) {
        ses_.metronome = !ses_.metronome;
        send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    }
    if (in.keyPressed['t'] && in.ctrl()) addTrack();
    if (in.keyPressed[KeyEnter] && in.ctrl()) addScene();

    if (in.keyPressed[KeyEscape]) send(Cmd::StopAll);
    if (in.keyPressed[KeyDelete] || (in.keyPressed[KeyBackspace] && !in.ctrl()))
        clearClip(selTrack_, selSlot_);

    const int nt = (int)ses_.tracks.size(), ns = (int)ses_.scenes.size();
    if (in.keyPressed[KeyLeft])  selTrack_ = clampv(selTrack_ - 1, 0, nt - 1);
    if (in.keyPressed[KeyRight]) selTrack_ = clampv(selTrack_ + 1, 0, nt - 1);
    if (in.keyPressed[KeyUp])    selSlot_  = clampv(selSlot_ - 1, 0, ns - 1);
    if (in.keyPressed[KeyDown])  selSlot_  = clampv(selSlot_ + 1, 0, ns - 1);
    if (in.keyPressed[KeyEnter] && !in.ctrl()) {
        if (ses_.tracks[selTrack_].slots[selSlot_].valid())
            send(Cmd::LaunchClip, selTrack_, selSlot_);
    }

    if (in.keyPressed['s'] && in.ctrl()) {
        const std::string p = ses_.path.empty() ? (homeDir() + "/" + ses_.name + ".lattice") : ses_.path;
        saveProjectTo(p);
    }
}

// The QWERTY piano, run once per frame. Everything hard about it lives in
// KbdPiano; this only decides whether the gate is open and where the notes go.
void App::updateKbdPiano() {
    Input& in = win_.input();
    // A focused text field must type, and a modified chord must stay a command
    // (Ctrl+S saves; it does not play a G). Closing the gate mid-hold releases
    // whatever is sounding, and reopening it never retriggers a still-held key.
    const bool live = kbdMidi_ && !ui_.editId &&
                      !in.ctrl() && !in.alt() && !(in.mods & ModSuper);

    const KbdPiano::Result res = kbd_.update(in.keyDown, live,
        [this](const MidiMsg& m) { engine_.pushMidi(m); });

    if (res.baseChanged || res.velChanged) {
        char buf[96];
        snprintf(buf, sizeof buf, "Keyboard octave C%d · velocity %d", kbd_.octave(), kbd_.velocity());
        status_ = buf;
    }
}

void App::toggleKbdMidi() {
    kbdMidi_ = !kbdMidi_;
    if (kbdMidi_) {
        char buf[192];
        snprintf(buf, sizeof buf,
                 "Computer MIDI Keyboard on — ASDFGHJKL white / WETYUOP black, "
                 "Z X octave (C%d), C V velocity (%d), Ctrl+Shift+K off",
                 kbd_.octave(), kbd_.velocity());
        status_ = buf;
    } else {
        // Anything still held has to be let go here: the key release that would
        // normally end the note is about to be ignored, and a hung note would
        // sit in the instrument with nothing left to stop it.
        kbd_.allNotesOff([this](const MidiMsg& m) { engine_.pushMidi(m); });
        status_ = "Computer MIDI Keyboard off";
    }
}

// ---------------------------------------------------------------------------
// control bar
// ---------------------------------------------------------------------------

void App::drawControlBar(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.bottom() - 1 * s, r.w, 1 * s}, pal::divider);

    const f32 pad = 8 * s, h = 20 * s;
    const f32 cy = r.y + (r.h - h) * 0.5f;
    f32 x = pad;

    // --- tempo ---
    Rect tapR{x, cy, 32 * s, h};
    if (ui_.button(uiId(1, 0), tapR, "TAP")) {
        static f64 lastTap = 0.0;
        const f64 now = nowSeconds();
        if (now - lastTap < 3.0) setTempo(clampv(60.0 / (now - lastTap), 20.0, 999.0));
        lastTap = now;
    }
    x += tapR.w + 4 * s;

    Rect tempoR{x, cy, 62 * s, h};
    f64 bpm = ses_.tempo;
    if (ui_.dragNumber(uiId(1, 1), tempoR, &bpm, 20.0, 999.0, 0.15, "%.2f")) setTempo(bpm);
    x += tempoR.w + 6 * s;

    Rect sigR{x, cy, 44 * s, h};
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%d / %d", ses_.sigNum, ses_.sigDen);
        rend_.roundRect(sigR, 2 * s, pal::panelAlt);
        rend_.textIn(fBody_, sigR, buf, pal::textDim, Align::Center);
    }
    x += sigR.w + 6 * s;

    Rect metR{x, cy, 36 * s, h};
    if (ui_.button(uiId(1, 2), metR, "MET", ses_.metronome, pal::accent)) {
        ses_.metronome = !ses_.metronome;
        send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    }
    x += metR.w + 12 * s;

    // --- global launch quantum ---
    rend_.textIn(fSmall_, {x, cy, 26 * s, h}, "Q", pal::textFaint, Align::Left, 0);
    Rect quantR{x + 16 * s, cy, 62 * s, h};
    if (ui_.selector(uiId(1, 3), quantR, &ses_.quantumIdx, kQuantumNames, kQuantumCount))
        send(Cmd::SetQuantum, ses_.quantumIdx);
    x = quantR.right() + 16 * s;

    // --- transport ---
    Rect playR{x, cy, 30 * s, h};
    const bool playing = engine_.playing.load();
    if (ui_.button(uiId(1, 4), playR, "", playing, pal::playGreen)) togglePlay();
    ui_.playTriangle(playR.insetXY(11 * s, 5 * s), playing ? pal::textOnClip : pal::text);
    x += playR.w + 3 * s;

    Rect stopR{x, cy, 30 * s, h};
    if (ui_.button(uiId(1, 5), stopR, "")) send(Cmd::SetPlaying, 0);
    ui_.stopSquare(stopR, pal::text);
    x += stopR.w + 3 * s;

    // Session record. This is an *intent*, not a transport action: while it is
    // lit, clicking an empty slot on an armed track starts a take in that slot;
    // while it is unlit, the same click only moves the selection. The circle
    // additionally lights while any track is actually capturing, so the bar
    // says what the engine is doing and not just what was asked for.
    Rect recR{x, cy, 30 * s, h};
    bool anyRec = false;
    for (size_t t = 0; t < ses_.tracks.size(); ++t)
        if (engine_.recState[t].load() != 0) { anyRec = true; break; }
    // A dark plate under a bright circle while capturing; the plain armed plate
    // otherwise, so the two states never read as the same light.
    const Col recPlate = anyRec ? pal::recRed.scale(0.4f) : pal::armRed;
    if (ui_.button(uiId(1, 6), recR, "", recIntent_ || anyRec, recPlate)) recIntent_ = !recIntent_;
    rend_.circle(recR.cx(), recR.cy(), 5 * s,
                 anyRec ? pal::recRed : (recIntent_ ? pal::textOnClip : pal::recRed.scale(0.55f)));
    x += recR.w + 12 * s;

    // --- position readout ---
    {
        const f64 beat = engine_.beat.load();
        const int bar_ = (int)std::floor(beat / ses_.sigNum) + 1;
        const int bt   = (int)std::floor(std::fmod(beat, (f64)ses_.sigNum)) + 1;
        const int sx   = (int)std::floor(std::fmod(beat, 1.0) * 4.0) + 1;
        char buf[48];
        snprintf(buf, sizeof buf, "%d.%d.%d", bar_, bt, sx);
        Rect posR{x, cy, 92 * s, h};
        rend_.roundRect(posR, 2 * s, pal::appBg);
        rend_.textIn(fBig_, posR, buf, playing ? pal::playGreen : pal::text, Align::Center);
        x += posR.w + 8 * s;
    }

    // --- right side: CPU + view switch ---
    f32 rx = r.right() - pad;
    {
        Rect vs{rx - 150 * s, cy, 150 * s, h};
        const f32 halfW = vs.w * 0.5f;
        Rect a{vs.x, vs.y, halfW, vs.h}, b{vs.x + halfW, vs.y, halfW, vs.h};
        if (ui_.button(uiId(1, 7), a, "SESSION", view_ == MainView::Session, pal::accent))
            view_ = MainView::Session;
        if (ui_.button(uiId(1, 8), b, "ARRANGE", view_ == MainView::Arrangement, pal::accent))
            view_ = MainView::Arrangement;
        rx = vs.x - 10 * s;
    }
    {
        const f32 cpu = engine_.cpu.load();
        char buf[32];
        snprintf(buf, sizeof buf, "%.0f%%", cpu);
        Rect cr{rx - 44 * s, cy, 44 * s, h};
        rend_.roundRect(cr, 2 * s, pal::appBg);
        const Col c = cpu > 85.f ? pal::meterRed : cpu > 60.f ? pal::meterAmber : pal::textDim;
        rend_.textIn(fSmall_, cr, buf, c, Align::Center);
        rx = cr.x - 8 * s;
    }
    {
        Rect br{rx - 60 * s, cy, 60 * s, h};
        rend_.textIn(fSmall_, br, audio_ ? audio_->name() : "no audio",
                     audio_ ? pal::textFaint : pal::recRed, Align::Right, 0);
        rx = br.x - 8 * s;
    }
    // Computer MIDI keyboard. It belongs with the audio/MIDI readouts because
    // it is an input status: while it is lit the letter keys are notes and not
    // shortcuts, and that must be visible without opening anything. The label
    // carries the octave so Z / X have somewhere to show their work.
    {
        char buf[24];
        snprintf(buf, sizeof buf, "KBD C%d", kbd_.octave());
        Rect kr{rx - 58 * s, cy, 58 * s, h};
        const u64 id = uiId(1, 9);
        const bool hot = ui_.setHot(id, kr) && ui_.isHot(id);
        if (hot) ui_.cursor = Cursor::Hand;
        rend_.roundRect(kr, 2 * s, kbdMidi_ ? pal::accent.alpha(0.18f)
                                            : (hot ? pal::slotHover : pal::appBg));
        rend_.textIn(fSmall_, kr, buf, kbdMidi_ ? pal::accent : pal::textFaint, Align::Center);
        if (hot && win_.input().pressed[0]) toggleKbdMidi();
        rx = kr.x - 8 * s;
    }
}

// ---------------------------------------------------------------------------
// browser
// ---------------------------------------------------------------------------

void App::drawBrowser(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::panel);
    rend_.rect({r.right() - 1 * s, r.y, 1 * s, r.h}, pal::divider);

    const f32 rowH = 19 * s;
    Rect head{r.x, r.y, r.w, 22 * s};
    rend_.rect(head, pal::panelAlt);
    rend_.textIn(fBold_, head, "BROWSER", pal::textDim, Align::Left, 8 * s);

    // Places
    f32 y = head.bottom();
    for (size_t i = 0; i < browserPlaces_.size(); ++i) {
        Rect row{r.x, y, r.w, rowH};
        const std::string& p = browserPlaces_[i];
        const bool sel = p == browserDir_;
        const bool hot = ui_.setHot(uiId(2, 100 + (int)i), row) && ui_.isHot(uiId(2, 100 + (int)i));
        if (sel)      rend_.rect(row, pal::gridBg);
        else if (hot) rend_.rect(row, pal::slotHover);
        if (hot) ui_.cursor = Cursor::Hand;
        const size_t slash = p.find_last_of('/');
        rend_.textIn(fBody_, row, (slash == std::string::npos ? p : p.substr(slash + 1)).c_str(),
                     sel ? pal::accent : pal::text, Align::Left, 14 * s);
        if (hot && in.pressed[0]) browseTo(p);
        y += rowH;
    }

    rend_.rect({r.x + 6 * s, y + 3 * s, r.w - 12 * s, 1 * s}, pal::divider);
    y += 8 * s;

    // Current directory label
    Rect dirRow{r.x, y, r.w, rowH};
    rend_.textIn(fSmall_, dirRow, browserDir_.c_str(), pal::textFaint, Align::Left, 8 * s);
    y += rowH;

    // File list
    Rect list{r.x, y, r.w, r.bottom() - y};
    rend_.pushClip(list);
    if (ui_.setHot(uiId(2, 1), list) && in.wheel != 0.f) {
        browserScroll_ -= in.wheel * rowH * 3.f;
        const f32 maxScroll = std::max(0.f, browserItems_.size() * rowH - list.h);
        browserScroll_ = clampv(browserScroll_, 0.f, maxScroll);
    }

    f32 iy = list.y - browserScroll_;
    for (size_t i = 0; i < browserItems_.size(); ++i) {
        Rect row{list.x, iy, list.w, rowH};
        iy += rowH;
        if (row.bottom() < list.y || row.y > list.bottom()) continue;
        const BrowserEntry& e = browserItems_[i];
        const u64 id = uiId(2, 200 + (int)i);
        const bool hot = ui_.setHot(id, row) && ui_.isHot(id);
        if ((int)i == browserSel_) rend_.rect(row, pal::gridBg);
        else if (hot)              rend_.rect(row, pal::slotHover);
        if (hot) ui_.cursor = Cursor::Hand;

        // Folder/file glyph
        const Col ic = e.isDir ? pal::textDim : pal::accent.mix(pal::text, 0.4f);
        if (e.isDir) rend_.roundRect({row.x + 8 * s, row.cy() - 4 * s, 9 * s, 8 * s}, 1.5f * s, ic);
        else         rend_.circle(row.x + 12 * s, row.cy(), 3 * s, ic);

        rend_.textIn(fBody_, {row.x + 22 * s, row.y, row.w - 26 * s, row.h}, e.name.c_str(),
                     e.isDir ? pal::text : pal::textDim, Align::Left, 0);

        if (hot && in.pressed[0]) {
            browserSel_ = (int)i;
            if (e.isDir) {
                // Resolve ".." rather than letting the path grow unbounded.
                if (e.name == "..") {
                    const size_t sl = browserDir_.find_last_of('/');
                    browseTo(sl == 0 ? "/" : (sl == std::string::npos ? browserDir_ : browserDir_.substr(0, sl)));
                } else {
                    browseTo(e.path);
                }
                break;
            }
            drag_.kind = DragState::Kind::BrowserFile;
            drag_.path = e.path;
            drag_.startX = in.mx; drag_.startY = in.my;
            drag_.armed = false;
        }
        if (hot && in.dblClick && !e.isDir) loadClipInto(selTrack_, selSlot_, e.path);
    }
    rend_.popClip();
}

// ---------------------------------------------------------------------------
// session view
// ---------------------------------------------------------------------------

void App::drawSessionView(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::appBg);

    const f32 masterW = lay::masterW * s;
    const f32 sceneW  = lay::sceneColW * s;
    Rect masterCol{r.right() - masterW, r.y, masterW, r.h};
    Rect sceneCol{masterCol.x - sceneW, r.y, sceneW, r.h};
    Rect tracksCol{r.x, r.y, sceneCol.x - r.x, r.h};

    // Horizontal scroll over the track area.
    f32 totalW = 0.f;
    for (const auto& t : ses_.tracks) totalW += t.width * s + lay::gutter * s;
    const f32 maxScroll = std::max(0.f, totalW - tracksCol.w);
    if (tracksCol.contains(in.mx, in.my) && in.wheel != 0.f && in.shift())
        gridScrollX_ = clampv(gridScrollX_ - in.wheel * 60.f * s, 0.f, maxScroll);
    gridScrollX_ = clampv(gridScrollX_, 0.f, maxScroll);

    rend_.pushClip(tracksCol);
    drawTrackHeaders(tracksCol, gridScrollX_);
    drawClipGrid(tracksCol, gridScrollX_);
    drawMixer(tracksCol, gridScrollX_);
    rend_.popClip();

    drawSceneColumn(sceneCol);
    drawMasterStrip(masterCol);
}

void App::drawTrackHeaders(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const f32 h = lay::trackHeadH * s;
    f32 x = r.x - scrollX;

    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        TrackModel& t = ses_.tracks[i];
        const f32 w = t.width * s;
        Rect cell{x, r.y, w - lay::gutter * s, h};
        x += w;
        if (cell.right() < r.x || cell.x > r.right()) continue;

        const bool sel = (int)i == selTrack_;
        const u64 id = uiId(3, (int)i);
        const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
        rend_.rect(cell, sel ? pal::gridBg : (hot ? pal::slotHover : pal::panel));
        // Colour chip so the track's identity reads at a glance, as in Live.
        rend_.rect({cell.x, cell.y, cell.w, 2 * s}, pal::clipColors[t.colorIdx % pal::clipColorCount]);

        if (ui_.textField(uiId(3, 1000 + (int)i), cell, &t.name,
                          Col(0, 0, 0, 0), sel ? pal::text : pal::textDim, Align::Left)) {}
        if (hot && in.pressed[0]) selTrack_ = (int)i;
    }

    // "+" to append a track.
    Rect add{x, r.y, 22 * s, h};
    if (add.x < r.right()) {
        if (ui_.button(uiId(3, 900), add, "+")) addTrack();
    }
}

void App::drawClipGrid(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    const f32 slotH = lay::slotH * s;
    const f32 top = r.y + lay::trackHeadH * s;
    const f32 mixerTop = r.bottom() - lay::mixerH * s;
    const int ns = (int)ses_.scenes.size();

    Rect grid{r.x, top, r.w, mixerTop - top};
    rend_.pushClip(grid);
    rend_.rect(grid, pal::appBg);

    f32 x = r.x - scrollX;
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        const f32 w = ses_.tracks[ti].width * s;
        // Each track reads as a continuous lane all the way down to the mixer,
        // otherwise the grid ends in a hard shelf under the last scene.
        rend_.rect({x, top, w - lay::gutter * s, grid.h},
                   (int)ti == selTrack_ ? pal::appBg.scale(1.35f) : pal::appBg.scale(1.15f));
        for (int si = 0; si < ns; ++si) {
            Rect cell{x, top + si * slotH, w - lay::gutter * s, slotH - lay::gutter * s};
            if (cell.bottom() > mixerTop) break;
            if (cell.right() >= r.x && cell.x <= r.right()) drawClipSlot(cell, (int)ti, si);
        }
        // Per-track stop button, directly under the last scene row.
        Rect stopCell{x, top + ns * slotH, w - lay::gutter * s, slotH - lay::gutter * s};
        if (stopCell.bottom() <= mixerTop && stopCell.right() >= r.x && stopCell.x <= r.right()) {
            const u64 id = uiId(4, 5000 + (int)ti);
            const bool hot = ui_.setHot(id, stopCell) && ui_.isHot(id);
            rend_.roundRect(stopCell, 2 * s, hot ? pal::slotHover : pal::slotEmpty);
            ui_.stopSquare(stopCell, pal::textDim);
            if (hot) ui_.cursor = Cursor::Hand;
            if (hot && win_.input().pressed[0]) send(Cmd::StopTrack, (int)ti);
        }
        x += w;
    }
    rend_.popClip();
}

void App::drawClipSlot(const Rect& cell, int ti, int si) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const ClipModel& m = ses_.tracks[ti].slots[si];
    const u64 id = uiId(4, ti, si);
    const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
    const bool sel = ti == selTrack_ && si == selSlot_;

    const int state = engine_.slotState[ti].load();
    const int active = engine_.activeSlot[ti].load();
    const int pending = engine_.pendingSlot[ti].load();
    const bool playing = (state == (int)SlotState::Playing || state == (int)SlotState::StopQueued) && active == si;
    const bool queued  = pending == si;

    // Recording truth comes from the engine, not from what we asked for: the
    // start is quantized, so a slot can sit queued for a bar before it captures.
    const int recPhase = engine_.recState[ti].load();
    const bool recHere = recPhase != 0 && engine_.recSlotIdx[ti].load() == si;

    if (!m.valid()) {
        const bool target = recIntent_ && ses_.tracks[ti].arm;
        if (recHere && recPhase >= 2) {
            // Capturing. Solid red, with the beats it has been running for.
            rend_.roundRect(cell, 2 * s, pal::recRed);
            rend_.circle(cell.x + 8 * s, cell.cy(), 3.5f * s, pal::textOnClip);
            char buf[24];
            snprintf(buf, sizeof buf, "%.1f",
                     std::max(0.0, engine_.beat.load() - recStartBeat_[ti]));
            rend_.textIn(fSmall_, {cell.x + 14 * s, cell.y, cell.w - 18 * s, cell.h},
                         buf, pal::textOnClip, Align::Right, 0);
        } else if (recHere) {
            // Queued: a pulsing ring, the record-side counterpart of the
            // blinking clip a launch shows while it waits for the quantum.
            const f32 ph = (f32)(0.5 + 0.5 * std::sin(nowSeconds() * 8.0));
            rend_.roundRect(cell, 2 * s, pal::slotEmpty);
            rend_.roundRectOutline(cell, 2 * s, 1.5f * s,
                                   pal::recRed.scale(0.35f + 0.4f * ph));
        } else {
            rend_.roundRect(cell, 2 * s, hot ? pal::slotHover : pal::slotEmpty);
            // Armed track, record intent lit: this slot is a take waiting to
            // happen, so say so before the click rather than after.
            if (target) rend_.circle(cell.x + 8 * s, cell.cy(), 3 * s,
                                     pal::recRed.scale(hot ? 0.9f : 0.55f));
        }
        if (sel) rend_.roundRectOutline(cell, 2 * s, 1 * s, pal::accent);
        if (hot) {
            ui_.cursor = Cursor::Hand;
            if (in.pressed[0]) {
                selTrack_ = ti; selSlot_ = si;
                if (recHere)      stopRecording(ti);       // second click stops
                else if (target)  startRecording(ti, si);
            }
        }
        return;
    }

    const Col base = pal::clipColors[m.colorIdx % pal::clipColorCount];
    Col fill = base.scale(playing ? 1.0f : (hot ? 0.88f : 0.76f));
    if (queued) {
        // Pulse while waiting for the launch quantum, like Live's blinking slot.
        const f32 ph = (f32)(0.5 + 0.5 * std::sin(nowSeconds() * 8.0));
        fill = base.scale(0.55f + 0.45f * ph);
    }
    rend_.roundRect(cell, 2 * s, fill);

    // Launch button zone on the left.
    const f32 btnW = 14 * s;
    Rect btn{cell.x, cell.y, btnW, cell.h};
    if (playing) ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), pal::playGreen.scale(0.85f));
    else         ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), pal::textOnClip.alpha(0.55f));

    rend_.textIn(fBody_, {cell.x + btnW, cell.y, cell.w - btnW - 2 * s, cell.h},
                 m.name.c_str(), pal::textOnClip, Align::Left, 2 * s);

    // Playback progress along the bottom edge.
    if (playing) {
        const f64 ph = clampv(engine_.clipPhase[ti].load(), 0.0, 1.0);
        rend_.rect({cell.x, cell.bottom() - 2 * s, cell.w * (f32)ph, 2 * s}, pal::textOnClip.alpha(0.45f));
    }
    if (sel) rend_.roundRectOutline(cell, 2 * s, 1 * s, pal::accent);

    if (hot) {
        ui_.cursor = Cursor::Hand;
        if (in.pressed[0]) {
            selTrack_ = ti; selSlot_ = si;
            send(Cmd::LaunchClip, ti, si);
            drag_.kind = DragState::Kind::Clip;
            drag_.srcTrack = ti; drag_.srcSlot = si;
            drag_.startX = in.mx; drag_.startY = in.my;
            drag_.armed = false;
        }
        if (in.pressed[2]) { selTrack_ = ti; selSlot_ = si; clearClip(ti, si); }
    }

    // Drop target for a drag in flight.
    if (drag_.kind != DragState::Kind::None && drag_.armed && hot && in.released[0]) {
        if (drag_.kind == DragState::Kind::BrowserFile) loadClipInto(ti, si, drag_.path);
        else if (drag_.srcTrack != ti || drag_.srcSlot != si) {
            ses_.tracks[ti].slots[si] = ses_.tracks[drag_.srcTrack].slots[drag_.srcSlot];
            if (!in.ctrl()) clearClip(drag_.srcTrack, drag_.srcSlot);
            pushClip(ti, si);
        }
        drag_ = DragState{};
    }
}

void App::drawSceneColumn(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const f32 slotH = lay::slotH * s;
    const f32 top = r.y + lay::trackHeadH * s;
    const int ns = (int)ses_.scenes.size();

    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, 1 * s, r.h}, pal::divider);

    Rect head{r.x, r.y, r.w, lay::trackHeadH * s};
    rend_.rect(head, pal::panelAlt);
    rend_.textIn(fBold_, head, "SCENES", pal::textDim, Align::Center);

    for (int si = 0; si < ns; ++si) {
        Rect cell{r.x + 2 * s, top + si * slotH, r.w - 4 * s, slotH - lay::gutter * s};
        if (cell.bottom() > r.bottom() - lay::mixerH * s) break;
        const u64 id = uiId(5, si);
        const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
        const bool sel = si == selSlot_;
        rend_.roundRect(cell, 2 * s, sel ? pal::gridBg : (hot ? pal::slotHover : pal::panelAlt));

        Rect btn{cell.x, cell.y, 14 * s, cell.h};
        ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), pal::textDim);
        ui_.textField(uiId(5, 1000 + si), {cell.x + 14 * s, cell.y, cell.w - 16 * s, cell.h},
                      &ses_.scenes[si].name, Col(0, 0, 0, 0), pal::text, Align::Left);

        if (hot) ui_.cursor = Cursor::Hand;
        if (hot && in.pressed[0]) { selSlot_ = si; send(Cmd::LaunchScene, si); }
    }

    Rect stopAll{r.x + 2 * s, top + ns * slotH, r.w - 4 * s, slotH - lay::gutter * s};
    if (stopAll.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 900), stopAll, "STOP ALL")) send(Cmd::StopAll);
    }

    Rect add{r.x + 2 * s, stopAll.bottom() + 4 * s, r.w - 4 * s, 18 * s};
    if (add.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 901), add, "+ Scene")) addScene();
    }
}

void App::drawMixer(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    const f32 top = r.bottom() - lay::mixerH * s;
    Rect mix{r.x, top, r.w, lay::mixerH * s};
    rend_.pushClip(mix);
    rend_.rect(mix, pal::panel);
    rend_.rect({mix.x, mix.y, mix.w, 1 * s}, pal::divider);

    f32 x = r.x - scrollX;
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        TrackModel& t = ses_.tracks[ti];
        const f32 w = t.width * s;
        Rect col{x, top, w - lay::gutter * s, mix.h};
        x += w;
        if (col.right() < r.x || col.x > r.right()) continue;
        if ((int)ti == selTrack_) rend_.rect(col, pal::panelAlt);

        f32 y = col.y + 6 * s;

        // M / S / arm row
        const f32 bw = (col.w - 16 * s) / 3.f;
        Rect mr{col.x + 6 * s, y, bw - 2 * s, 15 * s};
        Rect sr{mr.right() + 2 * s, y, bw - 2 * s, 15 * s};
        Rect ar{sr.right() + 2 * s, y, bw - 2 * s, 15 * s};
        if (ui_.squareToggle(uiId(6, (int)ti, 0), mr, "M", &t.mute, pal::meterAmber))
            send(Cmd::TrackMute, (int)ti, t.mute ? 1 : 0);
        if (ui_.squareToggle(uiId(6, (int)ti, 1), sr, "S", &t.solo, pal::soloBlue))
            send(Cmd::TrackSolo, (int)ti, t.solo ? 1 : 0);
        // Record-arm is a filled dot in Live, and the glyph atlas is ASCII-only,
        // so draw the dot rather than trying to letter it.
        if (ui_.squareToggle(uiId(6, (int)ti, 2), ar, "", &t.arm, pal::armRed))
            send(Cmd::TrackArm, (int)ti, t.arm ? 1 : 0);
        rend_.circle(ar.cx(), ar.cy(), 3.5f * s, t.arm ? pal::textOnClip : pal::armRed);
        y += 20 * s;

        // Pan
        Rect pan{col.cx() - 11 * s, y, 22 * s, 22 * s};
        if (ui_.knob(uiId(6, (int)ti, 3), pan, &t.pan, -1.f, 1.f, 0.f))
            send(Cmd::TrackPan, (int)ti, 0, t.pan);
        y += 26 * s;

        // Fader + meter
        const f32 fh = col.bottom() - y - 6 * s;
        Rect fader{col.x + 10 * s, y, 16 * s, fh};
        Rect meter{fader.right() + 5 * s, y, 9 * s, fh};
        if (ui_.vFader(uiId(6, (int)ti, 4), fader, &t.fader))
            send(Cmd::TrackVol, (int)ti, 0, faderToGain(t.fader));

        const f32 lvl = std::max(engine_.meterL[ti].load(), engine_.meterR[ti].load());
        peakHoldT_[ti] = std::max(lvl, peakHoldT_[ti] * 0.985f);
        ui_.meterV(meter, lvl, peakHoldT_[ti]);
    }
    rend_.popClip();
}

void App::drawMasterStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::panelAlt);
    rend_.rect({r.x, r.y, 1 * s, r.h}, pal::divider);

    Rect head{r.x, r.y, r.w, lay::trackHeadH * s};
    rend_.rect(head, pal::panel);
    rend_.textIn(fBold_, head, "MASTER", pal::text, Align::Center);

    const f32 top = r.bottom() - lay::mixerH * s;
    Rect mix{r.x, top, r.w, lay::mixerH * s};
    rend_.rect({mix.x, mix.y, mix.w, 1 * s}, pal::divider);

    static f32 masterFader = 0.85f;
    f32 y = mix.y + 26 * s;
    const f32 fh = mix.bottom() - y - 6 * s;
    Rect fader{mix.x + 12 * s, y, 16 * s, fh};
    Rect meterL{fader.right() + 6 * s, y, 9 * s, fh};
    Rect meterR{meterL.right() + 3 * s, y, 9 * s, fh};

    if (ui_.vFader(uiId(7, 0), fader, &masterFader))
        send(Cmd::MasterVol, 0, 0, faderToGain(masterFader));

    const f32 l = engine_.masterMeterL.load(), rr = engine_.masterMeterR.load();
    peakHoldM_[0] = std::max(l, peakHoldM_[0] * 0.985f);
    peakHoldM_[1] = std::max(rr, peakHoldM_[1] * 0.985f);
    ui_.meterV(meterL, l, peakHoldM_[0]);
    ui_.meterV(meterR, rr, peakHoldM_[1]);
}

// ---------------------------------------------------------------------------
// clip detail
// ---------------------------------------------------------------------------

void App::drawWaveform(const Rect& r, const SampleBuffer& sb, const Col& c, f64 t0, f64 t1) {
    if (sb.peakBuckets <= 0) return;
    const f32 mid = r.cy();
    const f32 halfH = r.h * 0.5f - 1.f;
    const int cols = (int)r.w;
    for (int i = 0; i < cols; ++i) {
        const f64 u = t0 + (t1 - t0) * ((f64)i / std::max(1, cols - 1));
        const int b = clampv((int)(u * sb.peakBuckets), 0, sb.peakBuckets - 1);
        const f32 lo = sb.peaks[(size_t)b * 2 + 0];
        const f32 hi = sb.peaks[(size_t)b * 2 + 1];
        const f32 y0 = mid - hi * halfH;
        const f32 y1 = mid - lo * halfH;
        rend_.rect({r.x + i, y0, 1.f, std::max(1.f, y1 - y0)}, c);
    }
}

// The panel chrome: a Live-style tab strip along the top, then whichever view
// the tab selects. Ctrl+D still hides the panel as a whole.
void App::drawDetailPanel(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, r.w, 1 * s}, pal::divider);

    Rect head{r.x, r.y + 1 * s, r.w, 19 * s};
    rend_.rect(head, pal::panelAlt);
    rend_.rect({head.x, head.bottom() - 1 * s, head.w, 1 * s}, pal::divider);

    const f32 tabW = 62 * s, tabH = 15 * s;
    Rect clipTab{head.x + 6 * s, head.y + (head.h - tabH) * 0.5f, tabW, tabH};
    Rect devTab{clipTab.right() + 3 * s, clipTab.y, tabW, tabH};
    if (ui_.button(uiId(9, 0), clipTab, "CLIP", detailTab_ == DetailTab::Clip, pal::accent))
        detailTab_ = DetailTab::Clip;
    if (ui_.button(uiId(9, 1), devTab, "DEVICES", detailTab_ == DetailTab::Devices, pal::accent)) {
        detailTab_ = DetailTab::Devices;
        ensurePluginScan();
    }

    // Context label on the right of the tab strip, so the panel says what it is
    // looking at even when the content area is empty.
    {
        char buf[128];
        if (detailTab_ == DetailTab::Clip) {
            const ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
            // ASCII only: the glyph atlas has no dashes or middots.
            snprintf(buf, sizeof buf, "%s  -  scene %d", m.valid() ? m.name.c_str() : "no clip",
                     selSlot_ + 1);
        } else {
            snprintf(buf, sizeof buf, "%s  -  %zu device%s", ses_.tracks[selTrack_].name.c_str(),
                     ses_.tracks[selTrack_].devices.size(),
                     ses_.tracks[selTrack_].devices.size() == 1 ? "" : "s");
        }
        rend_.textIn(fSmall_, head, buf, pal::textFaint, Align::Right, 8 * s);
    }

    Rect content{r.x, head.bottom(), r.w, r.bottom() - head.bottom()};
    if (detailTab_ == DetailTab::Clip) drawClipDetail(content);
    else                               drawDeviceDetail(content);
}

void App::drawClipDetail(const Rect& r) {
    const f32 s = win_.dpiScale();

    ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
    if (!m.valid()) {
        rend_.textIn(fBody_, r, "No clip selected  —  drag a file from the browser onto a slot",
                     pal::textFaint, Align::Center);
        return;
    }

    const Col ccol = pal::clipColors[m.colorIdx % pal::clipColorCount];
    Rect head{r.x, r.y + 1 * s, r.w, 20 * s};
    rend_.rect({head.x, head.y, 4 * s, head.h}, ccol);
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, 260 * s, head.h}, m.name.c_str(), pal::text, Align::Left, 0);

    // --- controls column ---
    const f32 panelW = 250 * s;
    Rect ctrl{r.x + 8 * s, head.bottom() + 6 * s, panelW, r.bottom() - head.bottom() - 12 * s};
    f32 y = ctrl.y;
    const f32 rowH = 20 * s, lblW = 62 * s;

    auto label = [&](const char* t, const Rect& row) {
        rend_.textIn(fSmall_, {row.x, row.y, lblW, row.h}, t, pal::textFaint, Align::Left, 0);
    };

    {   // Warp mode
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("WARP", row);
        static const char* warpNames[] = {"Off", "Repitch", "Beats"};
        int wi = (int)m.warp;
        Rect sel{row.x + lblW, row.y, 84 * s, row.h};
        if (ui_.selector(uiId(8, 0), sel, &wi, warpNames, 3)) {
            m.warp = (Warp)wi;
            send(Cmd::ClipWarp, selTrack_, selSlot_, (f64)wi);
        }
        Rect lp{sel.right() + 6 * s, row.y, 52 * s, row.h};
        if (ui_.button(uiId(8, 1), lp, "LOOP", m.loop, pal::accent)) {
            m.loop = !m.loop;
            send(Cmd::ClipLoop, selTrack_, selSlot_, m.loop ? 1.0 : 0.0);
        }
        y += rowH + 4 * s;
    }
    {   // Clip tempo
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("CLIP BPM", row);
        f64 bpm = m.clipBpm;
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 2), dn, &bpm, 20.0, 400.0, 0.1, "%.2f")) {
            m.clipBpm = bpm;
            pushClip(selTrack_, selSlot_);
        }
        // Halve / double, exactly like Live's :2 and *2 buttons.
        Rect h2{dn.right() + 6 * s, row.y, 26 * s, row.h};
        Rect d2{h2.right() + 3 * s, row.y, 26 * s, row.h};
        if (ui_.button(uiId(8, 3), h2, ":2")) { m.clipBpm *= 0.5; pushClip(selTrack_, selSlot_); }
        if (ui_.button(uiId(8, 4), d2, "*2")) { m.clipBpm *= 2.0; pushClip(selTrack_, selSlot_); }
        y += rowH + 4 * s;
    }
    {   // Gain
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("GAIN", row);
        f64 db = gainToDb(m.gain);
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 5), dn, &db, -70.0, 12.0, 0.1, "%.1f dB")) {
            m.gain = dbToGain((f32)db);
            send(Cmd::ClipGain, selTrack_, selSlot_, m.gain);
        }
        y += rowH + 4 * s;
    }
    {   // Launch quantum override
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("LAUNCH Q", row);
        static const char* qn[kQuantumCount + 1] = {"Global"};
        static bool qnInit = false;
        if (!qnInit) { for (int i = 0; i < kQuantumCount; ++i) qn[i + 1] = kQuantumNames[i]; qnInit = true; }
        int qi = m.quantumIdx + 1;
        Rect sel{row.x + lblW, row.y, 84 * s, row.h};
        if (ui_.selector(uiId(8, 6), sel, &qi, qn, kQuantumCount + 1)) {
            m.quantumIdx = qi - 1;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Generative launch: probability, follow action, follow length.
        // The engine rolls `prob` on every launch and fires the follow action
        // after `followBeats` of playback, so all three are pure clip state and
        // ride across in the same RtClip as everything else here.
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("LAUNCH", row);

        f64 pct = m.prob * 100.0;
        Rect pr{row.x + lblW, row.y, 48 * s, row.h};
        if (ui_.dragNumber(uiId(13, 0), pr, &pct, 0.0, 100.0, 0.4, "%.0f%%")) {
            m.prob = clampv(pct * 0.01, 0.0, 1.0);
            pushClip(selTrack_, selSlot_);
        }

        int fa = (int)m.followAction;
        Rect fr{pr.right() + 6 * s, row.y, 58 * s, row.h};
        if (ui_.selector(uiId(13, 1), fr, &fa, kFollowNames, kFollowCount)) {
            m.followAction = (Follow)clampv(fa, 0, kFollowCount - 1);
            pushClip(selTrack_, selSlot_);
        }

        // 0 beats means "when the clip itself ends", which reads as Auto rather
        // than as a length. Whole beats only: a follow length between beats is
        // a tempo problem, not a musical choice.
        f64 fb = m.followBeats;
        Rect br{fr.right() + 6 * s, row.y, 52 * s, row.h};
        if (ui_.dragNumber(uiId(13, 2), br, &fb, 0.0, 128.0, 0.06, "%.0f bt",
                           Align::Center, "Auto", 1.0)) {
            m.followBeats = fb;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Read-out of what the engine will actually do
        Rect row{ctrl.x, y, ctrl.w, rowH};
        char buf[96];
        const f64 rate = (m.warp == Warp::Off) ? 1.0 : m.clipBpm / ses_.tempo;
        snprintf(buf, sizeof buf, "%.2f beats  ·  rate %.3fx  ·  %d ch",
                 m.lengthBeats, rate, m.sample->channels);
        rend_.textIn(fSmall_, row, buf, pal::textFaint, Align::Left, 0);
    }

    // --- waveform ---
    Rect wave{ctrl.right() + 12 * s, head.bottom() + 6 * s,
              r.right() - ctrl.right() - 20 * s, r.bottom() - head.bottom() - 12 * s};
    rend_.roundRect(wave, 2 * s, pal::appBg);
    rend_.pushClip(wave.inset(2 * s));
    drawWaveform(wave.inset(3 * s), *m.sample, ccol.scale(0.85f));

    // Playhead, when this clip is the one sounding on its track.
    if (engine_.activeSlot[selTrack_].load() == selSlot_) {
        const f64 ph = clampv(engine_.clipPhase[selTrack_].load(), 0.0, 1.0);
        const f32 px = wave.x + 3 * s + (wave.w - 6 * s) * (f32)ph;
        rend_.rect({px, wave.y + 2 * s, 1.5f * s, wave.h - 4 * s}, pal::playGreen);
    }
    rend_.popClip();
}

// ---------------------------------------------------------------------------
// device view: plugin browser on the left, the selected track's chain right
// ---------------------------------------------------------------------------

void App::drawDeviceDetail(const Rect& r) {
    const f32 s = win_.dpiScale();
    // The scan is lazy, and the tab can also be reached by restoring a session
    // with the tab already active, so make sure it has happened.
    ensurePluginScan();

    const f32 listW = 236 * s;
    Rect list{r.x, r.y, listW, r.h};
    Rect strip{list.right() + 1 * s, r.y, r.right() - list.right() - 1 * s, r.h};
    drawPluginBrowser(list);
    rend_.rect({list.right(), r.y, 1 * s, r.h}, pal::divider);
    drawDeviceStrip(strip);
}

void App::drawPluginBrowser(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::panelAlt);

    // --- filter ---
    const u64 fid = uiId(10, 0);
    Rect filter{r.x + 6 * s, r.y + 5 * s, r.w - 12 * s, 17 * s};
    ui_.textField(fid, filter, &pluginFilter_, pal::appBg, pal::text, Align::Left, false);
    // textField only writes back on commit, but a filter has to narrow as you
    // type, so read the live edit buffer while this field owns the caret.
    const std::string* live = ui_.liveText(fid);
    const std::string& query = live ? *live : pluginFilter_;
    if (query.empty())
        rend_.textIn(fSmall_, filter, "filter plugins", pal::textFaint, Align::Left, 5 * s);

    // --- filtered index, rebuilt each frame: a few hundred string compares ---
    static std::vector<int> shown;                  // reused to avoid churn
    shown.clear();
    const std::vector<PluginDesc>& all = registry_.plugins();
    for (int i = 0; i < (int)all.size(); ++i)
        if (icontains(all[i].name, query)) shown.push_back(i);

    const f32 rowH = 17 * s;
    Rect listR{r.x, filter.bottom() + 4 * s, r.w, r.bottom() - filter.bottom() - 4 * s};
    rend_.pushClip(listR);
    rend_.rect(listR, pal::appBg.scale(1.05f));

    if (ui_.setHot(uiId(10, 1), listR) && in.wheel != 0.f) {
        pluginScroll_ -= in.wheel * rowH * 3.f;
    }
    const f32 maxScroll = std::max(0.f, shown.size() * rowH - listR.h);
    pluginScroll_ = clampv(pluginScroll_, 0.f, maxScroll);

    if (shown.empty()) {
        rend_.textIn(fSmall_, listR, all.empty() ? "no plugins found" : "no match",
                     pal::textFaint, Align::Center);
    }

    f32 y = listR.y - pluginScroll_;
    for (size_t k = 0; k < shown.size(); ++k) {
        Rect row{listR.x, y, listR.w, rowH};
        y += rowH;
        if (row.bottom() < listR.y || row.y > listR.bottom()) continue;

        const int pi = shown[k];
        const PluginDesc& d = all[pi];
        const u64 id = uiId(10, 100 + pi);
        const bool hot = ui_.setHot(id, row) && ui_.isHot(id);
        if (pi == pluginSel_) rend_.rect(row, pal::gridBg);
        else if (hot)         rend_.rect(row, pal::slotHover);
        if (hot) ui_.cursor = Cursor::Hand;

        Rect tag{row.right() - 34 * s, row.cy() - 6 * s, 28 * s, 12 * s};
        rend_.roundRect(tag, 2 * s, pal::panel);
        rend_.textIn(fSmall_, tag, formatName(d.format), pal::textFaint, Align::Center, 0);

        Rect vendor{tag.x - 74 * s, row.y, 70 * s, row.h};
        if (!d.vendor.empty()) {
            rend_.pushClip(vendor);
            rend_.textIn(fSmall_, vendor, d.vendor.c_str(), pal::textDim, Align::Right, 0);
            rend_.popClip();
        }

        Rect name{row.x + 8 * s, row.y, vendor.x - row.x - 12 * s, row.h};
        rend_.pushClip(name);
        rend_.textIn(fBody_, name, d.name.c_str(), hot || pi == pluginSel_ ? pal::text : pal::textDim,
                     Align::Left, 0);
        rend_.popClip();

        if (hot && in.pressed[0]) pluginSel_ = pi;
        // Double-click loads, matching how the file browser drops a sample.
        if (hot && in.dblClick) addDeviceToTrack(selTrack_, d);
    }
    rend_.popClip();
}

void App::drawDeviceStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::panel);

    TrackModel& t = ses_.tracks[selTrack_];
    const Col tc = pal::clipColors[t.colorIdx % pal::clipColorCount];

    Rect head{r.x, r.y, r.w, 16 * s};
    rend_.rect(head, pal::panelAlt);
    rend_.rect({head.x, head.y, 4 * s, head.h}, tc);       // track identity chip
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, 220 * s, head.h}, t.name.c_str(),
                 pal::text, Align::Left, 0);
    rend_.textIn(fSmall_, head, "double-click a plugin to add it to this track",
                 pal::textFaint, Align::Right, 8 * s);

    Rect area{r.x, head.bottom(), r.w, r.bottom() - head.bottom()};
    rend_.pushClip(area);

    // Keep the selection honest: tracks can be switched under it, and a device
    // can have been removed since the last frame.
    if (t.devices.empty()) selDevice_ = -1;
    else selDevice_ = clampv(selDevice_ < 0 ? 0 : selDevice_, 0, (int)t.devices.size() - 1);

    if (t.devices.empty()) {
        rend_.textIn(fBody_, area, "No devices on this track", pal::textFaint, Align::Center);
        rend_.popClip();
        return;
    }

    const f32 boxW = 150 * s, gap = 5 * s;
    const f32 total = t.devices.size() * (boxW + gap) + 6 * s;
    const f32 maxScroll = std::max(0.f, total - area.w);
    stripScroll_ = clampv(stripScroll_, 0.f, maxScroll);
    bool wheelUsed = false;

    f32 x = area.x + 6 * s - stripScroll_;
    for (size_t i = 0; i < t.devices.size(); ++i) {
        DeviceModel& d = t.devices[i];
        Rect box{x, area.y + 4 * s, boxW, area.h - 9 * s};
        x += boxW + gap;
        if (box.right() < area.x || box.x > area.right()) continue;

        const bool sel = (int)i == selDevice_;
        // Claim hot for the whole box first so the controls drawn afterwards
        // can take it back — last setHot() of the frame wins.
        const u64 bid = uiId(11, (int)i, 2);
        const bool hotBox = ui_.setHot(bid, box) && ui_.isHot(bid);
        rend_.roundRect(box, 3 * s, sel ? pal::gridBg : pal::panelAlt);
        if (sel) rend_.roundRectOutline(box, 3 * s, 1 * s, pal::accent);

        Rect title{box.x, box.y, box.w, 16 * s};
        rend_.rect({title.x + 2 * s, title.y + 3 * s, 3 * s, title.h - 6 * s}, tc);

        // Both controls are glyph-drawn rather than lettered: at this size the
        // font ellipsises anything longer than a character or two.
        Rect xr{title.right() - 17 * s, title.y + 2 * s, 14 * s, 12 * s};
        Rect br{xr.x - 20 * s, title.y + 2 * s, 18 * s, 12 * s};

        Rect nameR{title.x + 9 * s, title.y, br.x - title.x - 11 * s, title.h};
        rend_.pushClip(nameR);
        rend_.textIn(fBold_, nameR, d.desc.name.c_str(), sel ? pal::text : pal::textDim,
                     Align::Left, 0);
        rend_.popClip();

        // Bypass lives on the instance, so the chain does not have to be
        // republished; setBypassed() is GUI-safe per the host contract.
        if (ui_.squareToggle(uiId(11, (int)i, 0), br, "", &d.bypass, pal::meterAmber))
            if (d.inst) d.inst->setBypassed(d.bypass);
        rend_.circle(br.cx(), br.cy(), 3.5f * s,
                     d.bypass ? pal::textOnClip : pal::playGreen);   // lit = active
        const bool xHot = ui_.button(uiId(11, (int)i, 1), xr, "");
        {
            const f32 k = 3.f * s;
            const Col xc = pal::textDim;
            rend_.line(xr.cx() - k, xr.cy() - k, xr.cx() + k, xr.cy() + k, 1.2f * s, xc);
            rend_.line(xr.cx() - k, xr.cy() + k, xr.cx() + k, xr.cy() - k, 1.2f * s, xc);
        }
        if (xHot) {
            removeDevice(selTrack_, (int)i);
            rend_.popClip();
            return;                       // t.devices changed under us
        }
        if (hotBox && in.pressed[0]) { selDevice_ = (int)i; paramScroll_ = 0.f; }

        Rect body{box.x + 4 * s, title.bottom() + 2 * s, box.w - 8 * s,
                  box.bottom() - title.bottom() - 6 * s};
        if (!d.inst) {
            // A device restored from a set whose plugin is not installed here.
            // It holds its place and its saved values (see DeviceModel), so the
            // chain comes back intact on a machine that has the plugin.
            rend_.pushClip(body);
            rend_.textIn(fSmall_, {body.x, body.y + 2 * s, body.w, 12 * s},
                         "plugin not installed", pal::armRed, Align::Left, 0);
            rend_.textIn(fSmall_, {body.x, body.y + 15 * s, body.w, 12 * s},
                         d.desc.uri.c_str(), pal::textFaint, Align::Left, 0);
            rend_.popClip();
            continue;
        }

        if (!sel) {
            // Unselected devices stay compact; only one chain slot is edited at
            // a time, like Live collapsing the devices you are not touching.
            char buf[64];
            snprintf(buf, sizeof buf, "%d params", d.inst->paramCount());
            rend_.pushClip(body);
            if (!d.desc.vendor.empty())
                rend_.textIn(fSmall_, {body.x, body.y + 2 * s, body.w, 12 * s},
                             d.desc.vendor.c_str(), pal::textFaint, Align::Left, 0);
            rend_.textIn(fSmall_, {body.x, body.y + 15 * s, body.w, 12 * s}, buf,
                         pal::textFaint, Align::Left, 0);
            rend_.popClip();
            continue;
        }

        // --- parameters of the selected device ---
        const int n = d.inst->paramCount();
        const int cols = 3;
        // 43px is knob (32) + label (11): three rows land exactly inside the
        // panel, so a device with nine or fewer controls never has to scroll.
        const f32 cw = body.w / (f32)cols, chh = 43 * s;
        const int rows = (n + cols - 1) / cols;
        const f32 pMax = std::max(0.f, rows * chh - body.h);
        if (ui_.hovered(body) && in.wheel != 0.f) {
            paramScroll_ -= in.wheel * chh * 0.5f;
            wheelUsed = true;
        }
        paramScroll_ = clampv(paramScroll_, 0.f, pMax);

        rend_.pushClip(body);
        if (n == 0)
            rend_.textIn(fSmall_, body, "no parameters", pal::textFaint, Align::Center);
        for (int p = 0; p < n; ++p) {
            Rect cell{body.x + (p % cols) * cw, body.y - paramScroll_ + (p / cols) * chh, cw, chh};
            if (cell.bottom() < body.y || cell.y > body.bottom()) continue;
            const ParamInfo& info = d.inst->paramInfo(p);
            Rect lbl{cell.x, cell.bottom() - 11 * s, cell.w, 10 * s};

            if (info.isBool) {
                Rect tg{cell.cx() - 11 * s, cell.y + 8 * s, 22 * s, 14 * s};
                bool on = d.inst->getParam(p) > 0.5f;
                if (ui_.squareToggle(uiId(12, (int)i * 256 + p, 0), tg, "", &on, pal::accent))
                    d.inst->setParam(p, on ? info.max : info.min);
            } else {
                Rect kr{cell.cx() - 16 * s, cell.y + 2 * s, 32 * s, 32 * s};
                f32 v = d.inst->getParam(p);
                if (ui_.knob(uiId(12, (int)i * 256 + p, 0), kr, &v, info.min, info.max,
                             info.def, info.isInt ? "%.0f" : "%.2f"))
                    d.inst->setParam(p, v);
            }

            rend_.pushClip(lbl);
            rend_.textIn(fSmall_, lbl, info.name.c_str(), pal::textDim, Align::Center, 0);
            rend_.popClip();
        }
        rend_.popClip();
    }

    // The strip scrolls horizontally on a plain wheel, unless the pointer was
    // over a parameter grid that wanted the notch for itself.
    if (!wheelUsed && maxScroll > 0.f && ui_.hovered(area) && in.wheel != 0.f)
        stripScroll_ = clampv(stripScroll_ - in.wheel * 60.f * s, 0.f, maxScroll);

    rend_.popClip();
}

// ---------------------------------------------------------------------------
// arrangement placeholder + chrome
// ---------------------------------------------------------------------------

void App::drawArrangementView(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::appBg);

    // Timeline ruler so the view is oriented even before it does anything.
    Rect ruler{r.x, r.y, r.w, 22 * s};
    rend_.rect(ruler, pal::panel);
    const f32 pxPerBar = 48 * s;
    for (int bar_ = 0; bar_ * pxPerBar < r.w; ++bar_) {
        const f32 x = r.x + bar_ * pxPerBar;
        rend_.rect({x, ruler.y, 1 * s, ruler.h}, pal::ridge);
        if (bar_ % 4 == 0) {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", bar_ + 1);
            rend_.text(fSmall_, x + 3 * s, ruler.y + 4 * s, buf, pal::textFaint);
        }
    }

    f32 y = ruler.bottom();
    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        Rect lane{r.x, y, r.w, 44 * s};
        rend_.rect({lane.x, lane.y, lane.w, lane.h - 1 * s},
                   i % 2 ? pal::appBg : pal::appBg.scale(1.12f));
        rend_.rect({lane.x, lane.y, 3 * s, lane.h - 1 * s},
                   pal::clipColors[ses_.tracks[i].colorIdx % pal::clipColorCount]);
        rend_.textIn(fBody_, {lane.x + 10 * s, lane.y, 160 * s, lane.h},
                     ses_.tracks[i].name.c_str(), pal::textDim, Align::Left, 0);
        y += 44 * s;
        if (y > r.bottom()) break;
    }

    // Playhead against the same bar grid.
    const f64 beat = engine_.beat.load();
    const f32 px = r.x + (f32)(beat / ses_.sigNum) * pxPerBar;
    if (px >= r.x && px <= r.right())
        rend_.rect({px, ruler.bottom(), 1.5f * s, r.bottom() - ruler.bottom()}, pal::playGreen);

    rend_.textIn(fBody_, {r.x, r.bottom() - 40 * s, r.w, 20 * s},
                 "Arrangement recording is not wired up yet — Tab returns to Session",
                 pal::textFaint, Align::Center);
}

void App::drawStatusBar(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, r.w, 1 * s}, pal::divider);
    rend_.textIn(fSmall_, r, status_.c_str(), pal::textFaint, Align::Left, 8 * s);

    // The MIDI tag carries the sequencer client id: nothing is auto-connected,
    // so the number is what the user needs to hand aconnect or qpwgraph.
    char midiTag[32] = "";
    if (midi_.running()) snprintf(midiTag, sizeof midiTag, " · MIDI %d:0", midi_.clientId());

    char buf[192];
    snprintf(buf, sizeof buf, "%s · %s %.0f Hz / %d fr%s · %.0f fps · %d draws",
             win_.backendName(),
             audio_ ? audio_->name() : "silent",
             audio_ ? audio_->sampleRate() : 0.0,
             audio_ ? audio_->bufferSize() : 0,
             midiTag,
             fps_, rend_.drawCalls());
    rend_.textIn(fSmall_, r, buf, pal::textFaint, Align::Right, 8 * s);
}

void App::drawDragGhost() {
    Input& in = win_.input();
    if (drag_.kind == DragState::Kind::None) return;
    if (!in.down[0]) { drag_ = DragState{}; return; }

    const f32 dx = in.mx - drag_.startX, dy = in.my - drag_.startY;
    if (!drag_.armed && (dx * dx + dy * dy) > 25.f) drag_.armed = true;
    if (!drag_.armed) return;

    const f32 s = win_.dpiScale();
    ui_.cursor = Cursor::Grab;
    std::string label;
    Col c = pal::accent;
    if (drag_.kind == DragState::Kind::BrowserFile) {
        const size_t sl = drag_.path.find_last_of('/');
        label = sl == std::string::npos ? drag_.path : drag_.path.substr(sl + 1);
    } else {
        const ClipModel& m = ses_.tracks[drag_.srcTrack].slots[drag_.srcSlot];
        label = m.name;
        c = pal::clipColors[m.colorIdx % pal::clipColorCount];
    }
    const f32 w = fBody_.measure(label.c_str()) + 16 * s;
    Rect ghost{in.mx + 10 * s, in.my + 8 * s, w, 18 * s};
    rend_.roundRect(ghost, 2 * s, c.alpha(0.9f));
    rend_.textIn(fBody_, ghost, label.c_str(), pal::textOnClip, Align::Center);
}

} // namespace lat
