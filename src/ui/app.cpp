#include "app.h"
#include "pianoroll.h"
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
// A return bus has no clips and no M/S/arm, so its strip is barely wider than
// a fader and a meter side by side.
constexpr f32 returnW     = 54.f;
// Tall enough for the M/S/arm row, the 2x2 send grid, pan, and a fader with
// enough travel left to mix with. The clip grid gives up the difference and
// still shows twice the scenes a default set has.
constexpr f32 mixerH      = 186.f;
constexpr f32 gutter      = 1.f;
}

// The return buses, as the UI says them. Letters for the strips and the send
// knobs; the undo labels are spelled out because that is what the status bar
// reads back after an undo.
static const char* const kReturnLetter[kMaxReturns] = {"A", "B", "C", "D"};
static const char* const kSendUndo[kMaxReturns] = {"send A", "send B", "send C", "send D"};
static_assert(kMaxReturns == 4, "the return strips are lettered A-D by hand");
// ReturnModel's default name. A bus still wearing it has not been named, and
// the strip shows its letter instead; the project format leans on the same
// value to decide a return is worth writing at all.
static const char* const kReturnPlaceholder = "Return";

// How much audio a single take can hold. Two minutes of interleaved stereo
// floats is ~46 MB at 48 kHz: cheap enough to allocate up front, long enough
// that no realistic loop or verse runs out of room. The engine stops at the
// capacity it was given, so overrunning truncates rather than corrupts.
constexpr f64 kRecordSeconds = 120.0;

// The undo gesture the auto-repeating arrow keys hold while a note is being
// nudged. Widget gestures are identified by the widget's own id, so this only
// has to avoid colliding with one: every uiId `kind` in use is listed at its
// call site, and 15 is not one of them.
static const u64 kArrowGesture = uiId(15, 0);

// How many notes a single MIDI take can hold. Four thousand is more than an
// hour of dense playing, and the array is 24 bytes a note, so the whole buffer
// is under 100 kB — cheap enough not to bother sizing it to the material.
constexpr int kRecordNotes = 4096;

// App owns a PianoRoll through a unique_ptr, so the two functions that have to
// see the whole type live here rather than in the header.
App::App()  = default;
App::~App() = default;

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
// NXTAKT_DEBUG_ADDFX hook, both of which match on what the user typed rather
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
    if (!win_.create("NxTakt", 1360, 860)) return false;
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

    audio_ = createBackend(engine_, env("AUDIO"));
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

    // Headless verification hook. With NXTAKT_DEBUG_ADDFX=<substring> set, the
    // first scanned plugin whose name matches is loaded onto track 0 and the
    // DEVICES tab is opened, so tools/headless_test.sh can screenshot a
    // populated device chain without anything driving the mouse.
    if (const char* want = env("DEBUG_ADDFX")) {
        ensurePluginScan();
        const PluginDesc* hit = nullptr;
        for (const PluginDesc& d : registry_.plugins())
            if (icontains(d.name, want)) { hit = &d; break; }
        if (!hit) {
            LOGW("NXTAKT_DEBUG_ADDFX: no plugin matching \"%s\"", want);
        } else if (!ses_.tracks.empty()) {
            selTrack_ = 0;
            devOwner_ = 0;
            addDevice(0, *hit);
            selDevice_ = (int)ses_.tracks[0].devices.size() - 1;
            detailTab_ = DetailTab::Devices;
            showDetail_ = true;
        }
    }

    // The same hook for the master chain -- a saturator or a bus compressor
    // across the whole mix, which is what a master chain is for. It also parks
    // the DEVICES tab on the master, so a screenshot shows the one part of the
    // chain-owner selection nothing inside gamescope can click on.
    if (const char* want = env("DEBUG_MASTERFX")) {
        ensurePluginScan();
        const PluginDesc* hit = nullptr;
        for (const PluginDesc& d : registry_.plugins())
            if (icontains(d.name, want)) { hit = &d; break; }
        if (!hit) {
            LOGW("NXTAKT_DEBUG_MASTERFX: no plugin matching \"%s\"", want);
        } else {
            selectChainOwner(kOwnMaster);
            addDevice(kOwnMaster, *hit);
            showDetail_ = true;
        }
    }

    // The other headless hook: undo cannot be clicked inside gamescope, so
    // NXTAKT_DEBUG_UNDO drives the restore path here instead. See
    // debugUndoSelfTest, and note that it puts the set back as it found it.
    if (env("DEBUG_UNDO")) debugUndoSelfTest();

    LOGI("backend: %s   audio: %s", win_.backendName(), audio_ ? audio_->name() : "none");
    return true;
}

void App::shutdown() {
    // Anything the UI left sounding is ended while there is still an engine to
    // hear it: previews and held keyboard notes both outlive the state that
    // started them, and a plugin does not know the app is closing.
    stopPreviews();
    kbd_.allNotesOff([this](const MidiMsg& m) { engine_.pushMidiFromGui(m); });

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
    for (const RtChain*& c : publishedReturn_) { delete c; c = nullptr; }
    delete publishedMaster_; publishedMaster_ = nullptr;
    for (RetiredChain& rc : retiring_) delete rc.chain;
    retiring_.clear();          // frees the instances the chains had dropped
    for (auto& row : publishedNotes_)
        for (const RtNote*& n : row) { delete[] n; n = nullptr; }
    for (const RtNote* n : retiringNotes_) delete[] n;
    retiringNotes_.clear();
    for (PendingRec& p : pendingRecs_) { delete[] p.buf; delete[] p.notes; }
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

void App::publishNotes(int track, int slot, const RtNote* fresh) {
    const RtNote* old = publishedNotes_[track][slot];
    publishedNotes_[track][slot] = fresh;
    // The engine only announces a *replaced* array, and only when it differs
    // from the incoming one; an entry that would never be announced must not be
    // queued for a retirement that will never arrive.
    if (old && old != fresh) retiringNotes_.push_back(old);
}

void App::pushClip(int track, int slot) {
    Command c;
    c.type = Cmd::SetClip;
    c.a = track; c.b = slot;
    const ClipModel& m = ses_.tracks[track].slots[slot];
    const bool midi = m.valid() && m.kind == ClipKind::Midi;

    // Snapshot the notes before anything is sent: the engine reads this array
    // for as long as it holds the clip, so it cannot be the GUI's live vector.
    RtNote* fresh = nullptr;
    if (midi && !m.notes.empty()) {
        fresh = new (std::nothrow) RtNote[m.notes.size()];
        if (!fresh) { status_ = "Out of memory - clip not updated"; return; }
        for (size_t i = 0; i < m.notes.size(); ++i) {
            const NoteModel& n = m.notes[i];
            fresh[i].beat  = n.beat;
            fresh[i].len   = n.len;
            fresh[i].pitch = n.pitch;
            fresh[i].vel   = n.vel;
        }
    }

    if (!m.valid()) {
        c.type = Cmd::ClearClip;
        if (!engine_.pushCommand(c)) {
            LOGW("command ring full - slot %d/%d not cleared", track, slot);
            return;
        }
        // A cleared MIDI slot retires its notes exactly like a replaced one.
        publishNotes(track, slot, nullptr);
        clipLive_[track][slot] = false;
        return;
    }

    RtClip rc;
    if (midi) {
        rc.isMidi     = true;
        rc.notes      = fresh;
        rc.noteCount  = (int)m.notes.size();
    } else {
        rc.data       = m.sample->data.data();
        rc.frames     = m.sample->frames;
        rc.channels   = m.sample->channels;
        rc.loopStart  = m.loopStart;
        rc.loopEnd    = m.loopEnd > m.loopStart ? m.loopEnd : m.sample->frames;
        rc.clipBpm    = m.clipBpm;
        rc.warp       = (int)m.warp;
    }
    rc.lengthBeats  = m.lengthBeats;
    rc.gain         = m.gain;
    rc.loop         = m.loop;
    rc.quantumIdx   = m.quantumIdx;
    rc.prob         = m.prob;
    rc.followAction = (int)m.followAction;
    rc.followBeats  = m.followBeats;
    rc.valid        = true;
    c.clip = rc;
    if (!engine_.pushCommand(c)) {
        // The engine never saw the array, so it is still solely ours and the
        // slot keeps whatever it was already playing.
        LOGW("command ring full - slot %d/%d not updated", track, slot);
        delete[] fresh;
        return;
    }
    // Unconditional, not only for MIDI clips: a slot that just turned into an
    // audio clip still has an old note array to hand back.
    publishNotes(track, slot, fresh);
    clipLive_[track][slot] = true;
}

void App::pushTrack(int t) {
    const TrackModel& tr = ses_.tracks[t];
    send(Cmd::TrackVol,  t, 0, faderToGain(tr.fader));
    send(Cmd::TrackPan,  t, 0, tr.pan);
    send(Cmd::TrackMute, t, tr.mute ? 1 : 0);
    send(Cmd::TrackSolo, t, tr.solo ? 1 : 0);
    send(Cmd::TrackArm,  t, tr.arm ? 1 : 0);
    // Sends are part of a track's mixer state like volume and pan are, so they
    // ride the same path: a load, an undo restore and a fresh set all arrive
    // here and nowhere else. The model holds the linear level the engine wants
    // (TrackModel::sends), so nothing is mapped on the way across.
    for (int rn = 0; rn < kMaxReturns; ++rn)
        send(Cmd::SendLevel, t, rn, tr.sends[rn]);
}

void App::pushAll() {
    send(Cmd::SetTempo, 0, 0, ses_.tempo);
    send(Cmd::SetQuantum, ses_.quantumIdx);
    send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    for (size_t t = 0; t < ses_.tracks.size(); ++t) {
        pushTrack((int)t);
        for (int s = 0; s < (int)ses_.scenes.size(); ++s) pushClip((int)t, s);
    }
    // Return levels. Their chains are published by materializeDevices (or by
    // releaseAllChains, which empties them) -- this is the scalar half.
    for (int i = 0; i < kMaxReturns; ++i)
        send(Cmd::ReturnVol, i, 0, faderToGain(ses_.returns[i].fader));
    releaseStaleSlots();
}

// The loop above only reaches slots the current session has a track and a
// scene for. A load or an undo can make the set *smaller*, and a slot that
// falls outside the new one keeps whatever was last pushed into it: an RtClip
// whose sample data belongs to a session we have just stopped owning, and a
// note array nothing will ever retire. Neither is reachable from the grid, but
// Cmd::StopAll and scene launches walk the engine's own tables, so "unreachable
// from the UI" is not the same as "cannot sound".
void App::releaseStaleSlots() {
    const int nt = (int)ses_.tracks.size(), ns = (int)ses_.scenes.size();
    for (int t = 0; t < kMaxTracks; ++t) {
        for (int s = 0; s < kMaxScenes; ++s) {
            if (t < nt && s < ns) continue;
            if (!clipLive_[t][s] && !publishedNotes_[t][s]) continue;
            Command c;
            c.type = Cmd::ClearClip;
            c.a = t; c.b = s;
            if (!engine_.pushCommand(c)) {
                // The engine still holds it, so the flags stay set and the next
                // push (or the next restore) tries again.
                LOGW("command ring full - stale slot %d/%d not cleared", t, s);
                continue;
            }
            clipLive_[t][s] = false;
            publishNotes(t, s, nullptr);
        }
    }
}

void App::pumpEngineEvents() {
    Event e;
    while (engine_.popEvent(e)) {
        if (e.type == Ev::ChainRetired) {
            // The audio thread has swapped this chain out and will never look
            // at it again, so the struct and every instance it was the last
            // reference to can finally go.
            //
            // `e.a` says which owner it came off (a track, kMaxTracks+i for a
            // return, -1 for the master) and is used for nothing but the
            // message below: the pool is keyed on the POINTER, every chain is
            // its own allocation, and no chain is ever published to two owners,
            // so the address adds no information the lookup needs.
            const RtChain* old = (const RtChain*)e.p;
            if (!old) continue;
            auto it = retiring_.begin();
            for (; it != retiring_.end(); ++it) if (it->chain == old) break;
            if (it == retiring_.end()) {
                LOGW("ChainRetired for an unknown chain %p (from %s) - leaking it "
                     "rather than freeing a pointer we do not own",
                     (const void*)old, ownerName(e.a).c_str());
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
        if (e.type == Ev::MidiRecordFinished) {
            finishMidiRecording(e);
            continue;
        }
        if (e.type == Ev::NotesRetired) {
            // The audio thread has stopped reading this array. Same handshake
            // as ChainRetired, and the same refusal to free a pointer we have
            // no record of owning.
            const RtNote* old = (const RtNote*)e.p;
            if (!old) continue;
            auto it = retiringNotes_.begin();
            for (; it != retiringNotes_.end(); ++it) if (*it == old) break;
            if (it == retiringNotes_.end()) {
                LOGW("NotesRetired for an unknown array %p - leaking it rather "
                     "than freeing a pointer we do not own", (const void*)old);
                continue;
            }
            delete[] *it;
            retiringNotes_.erase(it);
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

// Where one owner's chain lives. The three cases differ in nothing else, which
// is the whole point: past this function no code below knows what a return is.
App::ChainOwner App::chainOwner(int owner) {
    ChainOwner co;
    if (owner == kOwnMaster) {
        co.devices   = &ses_.masterDevices;
        co.saved     = &ses_.masterSavedDevices;
        co.published = &publishedMaster_;
        co.cmd       = Cmd::SetMasterChain;
        co.addr      = -1;                      // as the retirement event says it
    } else if (ownIsReturn(owner)) {
        ReturnModel& rm = ses_.returns[owner - kOwnReturn0];
        co.devices   = &rm.devices;
        co.saved     = &rm.savedDevices;
        co.published = &publishedReturn_[owner - kOwnReturn0];
        co.cmd       = Cmd::SetReturnChain;
        co.addr      = owner - kOwnReturn0;
    } else if (ownIsTrack(owner)) {
        co.published = &published_[owner];
        co.cmd       = Cmd::SetChain;
        co.addr      = owner;
        // A published slot outlives the track model: a set that shrank leaves
        // the engine running a chain for an index the session no longer has.
        if (owner < (int)ses_.tracks.size()) {
            co.devices = &ses_.tracks[owner].devices;
            co.saved   = &ses_.tracks[owner].savedDevices;
        }
    }
    return co;
}

std::string App::ownerName(int owner) const {
    if (owner == kOwnMaster) return "Master";
    if (ownIsReturn(owner)) return std::string("Return ") + kReturnLetter[owner - kOwnReturn0];
    if (owner >= 0 && owner < (int)ses_.tracks.size()) return ses_.tracks[owner].name;
    char buf[32];
    snprintf(buf, sizeof buf, "track %d", owner);
    return buf;
}

std::vector<int> App::modelOwners() const {
    std::vector<int> v;
    v.reserve(ses_.tracks.size() + kMaxReturns + 1);
    for (int t = 0; t < (int)ses_.tracks.size(); ++t) v.push_back(t);
    for (int i = 0; i < kMaxReturns; ++i) v.push_back(ownReturn(i));
    v.push_back(kOwnMaster);
    return v;
}

void App::publishChain(int owner) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;

    RtChain* chain = new RtChain();
    int n = 0;
    for (const DeviceModel& d : *co.devices) {
        if (!d.inst) continue;
        if (n >= kMaxChainFx) {
            LOGW("%s has more than %d devices - the extras will not sound",
                 ownerName(owner).c_str(), kMaxChainFx);
            break;
        }
        // Bypassed devices stay in the chain: the instance itself short-circuits
        // in process(), which keeps the chain stable across a bypass toggle.
        chain->fx[n++] = d.inst.get();
    }
    chain->count = n;

    Command c;
    c.type = co.cmd;
    c.a = co.addr;
    c.p = chain;
    if (!engine_.pushCommand(c)) {
        // The ring is full, so the engine never saw this chain. It is still
        // solely ours, and the previously published one is still live: drop the
        // new one and leave every piece of state exactly as it was.
        LOGW("command ring full - chain for %s not published", ownerName(owner).c_str());
        delete chain;
        return;
    }

    if (*co.published) retiring_.push_back(RetiredChain{*co.published, {}});
    *co.published = chain;
}

void App::addDevice(int owner, const PluginDesc& d) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;
    std::vector<DeviceModel>& devices = *co.devices;
    if ((int)devices.size() >= kMaxChainFx) {
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
    devices.push_back(std::move(dm));

    const RtChain* before = *co.published;
    publishChain(owner);
    if (*co.published == before) {
        // Publish failed. The engine never referenced this instance, so it is
        // safe to destroy right here and leave the model matching the engine.
        devices.pop_back();
        status_ = "Engine busy - device not added";
        return;
    }
    selDevice_ = (int)devices.size() - 1;
    paramScroll_ = 0.f;
    status_ = "Added " + d.name;
}

void App::removeDevice(int owner, int idx) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;
    std::vector<DeviceModel>& devices = *co.devices;
    if (idx < 0 || idx >= (int)devices.size()) return;

    // Move the instance out of the model rather than letting erase() destroy
    // it: the audio thread is still running the *outgoing* chain, which points
    // straight at it. It may only die once that chain comes back to us.
    DeviceModel dead = std::move(devices[idx]);
    devices.erase(devices.begin() + idx);

    const RtChain* outgoing = *co.published;
    publishChain(owner);

    if (*co.published == outgoing) {
        // Publish failed; the engine still runs the old chain, so the device
        // has to go back where it was or the model would lie about what sounds.
        devices.insert(devices.begin() + idx, std::move(dead));
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

    if (devices.empty())                        selDevice_ = -1;
    else if (selDevice_ >= (int)devices.size()) selDevice_ = (int)devices.size() - 1;
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
    // Devices are looked at through the owner list, so a return's chain and the
    // master's are stamped by the same pass a track's is.
    const std::vector<int> owners = modelOwners();
    for (const TrackModel& t : ses_.tracks) {
        note(t.uid);
        for (int s = 0; s < kMaxScenes; ++s) note(t.slots[s].uid);
    }
    for (const ReturnModel& r : ses_.returns) note(r.uid);
    for (int o : owners) {
        ChainOwner co = chainOwner(o);
        if (!co.devices) continue;
        for (const DeviceModel& d : *co.devices) note(d.uid);
        for (const SavedDevice& d : *co.saved)   note(d.uid);
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
    }
    for (ReturnModel& r : ses_.returns) if (!r.uid) r.uid = ses_.newUid();
    for (int o : owners) {
        ChainOwner co = chainOwner(o);
        if (!co.devices) continue;
        for (DeviceModel& d : *co.devices)      if (!d.uid) d.uid = ses_.newUid();
        for (SavedDevice& d : *co.saved)        if (!d.uid) d.uid = ses_.newUid();
    }
    for (SceneModel& s : ses_.scenes) if (!s.uid) s.uid = ses_.newUid();
}

// devices -> savedDevices. The project layer only ever sees the passive form,
// so this is the one place that reads a live instance for persistence.
void App::serializeDevices() {
    for (int o : modelOwners()) {
        ChainOwner co = chainOwner(o);
        if (!co.devices) continue;
        std::vector<SavedDevice>& out = *co.saved;
        out.clear();
        out.reserve(co.devices->size());
        for (DeviceModel& d : *co.devices) {
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
            out.push_back(std::move(sd));
        }
    }
}

// savedDevices -> devices. Every entry keeps its slot in the chain even if the
// plugin is gone, so the order a set was saved with is the order it comes back
// with once the missing plugin is installed.
//
// `reuse` (see the declaration) turns this into a *rebind* for everything an
// undo restore already has running. That is not an optimisation for its own
// sake: a plugin reload loses everything the plugin holds that its parameters
// do not describe, so undoing a note edit would silently reset every synth in
// the set. Instantiation stays the fallback, for a device the pool has no
// match for -- one that was removed and is coming back, or a set being loaded.
void App::materializeDevices(std::vector<LiveDevice>* reuse) {
    const std::vector<int> owners = modelOwners();
    bool any = false;
    for (int o : owners) {
        ChainOwner co = chainOwner(o);
        if (co.saved && !co.saved->empty()) { any = true; break; }
    }
    if (!any) return;
    // The scan is chatty about its progress in the status bar; a load that goes
    // through cleanly should still read as a load when it is done. It is also
    // deferred to the first device that actually needs the registry: a restore
    // that rebinds the whole chain touches no plugin, and making the first undo
    // of a session pay for a full LV2 + CLAP scan would be a bizarre place to
    // spend the better part of a second.
    const std::string prevStatus = status_;

    int missing = 0;
    for (int owner : owners) {
        ChainOwner co = chainOwner(owner);
        if (!co.saved || co.saved->empty()) continue;

        for (SavedDevice& sd : *co.saved) {
            DeviceModel dm;
            dm.uid = sd.uid;
            dm.bypass = sd.bypass;

            // A live instance for this uid is this device, still running; the
            // uri is checked too, because a uid only means "the same device"
            // while the plugin behind it is the same plugin.
            std::unique_ptr<PluginInstance> inst;
            if (reuse) {
                for (LiveDevice& ld : *reuse) {
                    if (!ld.inst || ld.uid != sd.uid || ld.uri != sd.uri) continue;
                    dm.desc = ld.desc;
                    inst = std::move(ld.inst);
                    break;
                }
            }
            const bool rebound = inst != nullptr;

            const PluginDesc* found = nullptr;
            if (!rebound) {
                ensurePluginScan();
                found = registry_.find(sd.uri);
                if (found) inst = registry_.instantiate(*found, engine_.sampleRate(), kMaxBlock);
            }

            if (!inst) {
                ++missing;
                LOGW("plugin not available: %s (%s)", sd.name.c_str(), sd.uri.c_str());
                if (found) dm.desc = *found;
                else {
                    dm.desc.uri = sd.uri;
                    dm.desc.name = sd.name;
                }
                dm.lostParams = sd.params;
                co.devices->push_back(std::move(dm));
                continue;
            }

            // Parameters are matched on ParamInfo::id, not on index: a plugin
            // can gain or reorder controls between versions, and dropping the
            // ones we no longer recognise beats applying them to the wrong
            // control. A rebound instance goes through exactly the same loop:
            // its live values are whatever the user has since dragged them to,
            // and the snapshot's are the ones being restored.
            const int n = inst->paramCount();
            for (const std::pair<u32, f32>& pv : sd.params) {
                for (int i = 0; i < n; ++i) {
                    if (inst->paramInfo(i).id != pv.first) continue;
                    inst->setParam(i, pv.second);
                    break;
                }
            }
            inst->setBypassed(sd.bypass);

            if (!rebound) dm.desc = *found;
            dm.inst = std::move(inst);
            co.devices->push_back(std::move(dm));
        }

        // The live models are now the truth; the passive copies are rebuilt
        // from them at the next save, missing plugins included.
        co.saved->clear();
        publishChain(owner);
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
    // Every published slot, not only the ones the session still has a model
    // for: a set that shrank leaves the engine running a chain for a track
    // index that no longer exists, and that chain has to be let go of too.
    auto release = [&](int owner) {
        ChainOwner co = chainOwner(owner);
        if (!co.valid()) return;
        const bool hasDevices = co.devices && !co.devices->empty();
        if (!*co.published && !hasDevices) return;

        RtChain* empty = new RtChain();
        Command c;
        c.type = co.cmd;
        c.a = co.addr;
        c.p = empty;
        const bool sent = engine_.pushCommand(c);
        if (!sent) {
            LOGW("command ring full - %s keeps running its old chain",
                 ownerName(owner).c_str());
            delete empty;
        }

        RetiredChain rc;
        if (hasDevices)
            for (DeviceModel& d : *co.devices)
                if (d.inst) rc.dying.push_back(std::move(d.inst));

        if (sent) {
            rc.chain = *co.published;       // may be null: nothing was published
            *co.published = empty;
        }
        // rc.chain stays null when nothing was ever published, or when the send
        // failed and the engine is therefore still following the old chain.
        // Either way no Ev::ChainRetired will ever match this entry, so it sits
        // in retiring_ until shutdown() - which is after the audio thread is
        // joined, and the only moment freeing it unilaterally is safe.
        if (rc.chain || !rc.dying.empty()) retiring_.push_back(std::move(rc));
    };

    for (int t = 0; t < kMaxTracks; ++t) release(t);
    for (int i = 0; i < kMaxReturns; ++i) release(ownReturn(i));
    release(kOwnMaster);
}

// The whole session is being replaced -- by a file, or by an undo snapshot.
// The two differ only in where `next` came from and in what may be carried
// across; everything about *how* a session is torn down and stood back up is
// here, once, because the ownership rules it has to respect are the hardest
// thing in this file.
void App::adoptSession(Session&& next, const std::vector<ClipSample>* restore) {
    const bool restoring = restore != nullptr;
    // 1. Instances the incoming session names and this one is already running.
    //    Harvested before releaseAllChains, which would otherwise hand every
    //    instance to the retirement flow and destroy it. Anything NOT taken
    //    here stays on its track and dies the ordinary way, which is exactly
    //    what should happen to a device the snapshot does not have.
    std::vector<LiveDevice> reuse;
    if (restoring) {
        // "Does the incoming set still name this device?", asked of every chain
        // it has -- tracks, the four returns, the master.
        auto wantedBy = [&next](u64 uid, const std::string& uri) {
            auto hit = [&](const std::vector<SavedDevice>& v) {
                for (const SavedDevice& sd : v)
                    if (sd.uid == uid && sd.uri == uri) return true;
                return false;
            };
            for (const TrackModel& nt : next.tracks) if (hit(nt.savedDevices)) return true;
            for (const ReturnModel& nr : next.returns) if (hit(nr.savedDevices)) return true;
            return hit(next.masterSavedDevices);
        };
        for (int o : modelOwners()) {
            ChainOwner co = chainOwner(o);
            if (!co.devices) continue;
            for (DeviceModel& d : *co.devices) {
                if (!d.inst) continue;
                if (!wantedBy(d.uid, d.desc.uri)) continue;
                LiveDevice ld;
                ld.uid = d.uid;
                ld.uri = d.desc.uri;
                ld.desc = d.desc;
                ld.inst = std::move(d.inst);          // off the track, into the pool
                reuse.push_back(std::move(ld));
            }
        }
    }

    // 2. Every published chain and every instance still on a track goes into
    //    the retirement flow. This also publishes an empty chain per track,
    //    which is what makes the audio thread let go of the *rebound*
    //    instances as well before they turn up again in a new chain.
    releaseAllChains();

    // 3. Samples. Two separate jobs, both about pointers the engine holds.
    //
    //    Reuse: on a restore every clip takes back the SampleBuffer it was
    //    playing when the snapshot was taken, matched on clip uid. loadProject
    //    has unavoidably decoded the files again (it only knows how to read
    //    from disk) and those copies are dropped here, so an undo does not
    //    double every sample in the set. More importantly this is the only way
    //    a clip with no file behind it -- a take that has been recorded and
    //    not exported -- survives an undo at all: the text can name a file and
    //    nothing else. See ClipSample.
    //
    //    Grace: whatever the outgoing session owned is held for one more
    //    generation. The engine can still be running a clip that points into
    //    one of those buffers for the few milliseconds it takes to drain the
    //    Cmd::SetClip below, and there is no "the audio thread has let go of
    //    this buffer" event the way there is for chains and note arrays. This
    //    is not that handshake; it is a window measured in user actions rather
    //    than in samples, which is the best this side of the boundary can do.
    if (restoring) {
        for (TrackModel& t : next.tracks)
            for (ClipModel& c : t.slots) {
                if (!c.uid) continue;
                for (const ClipSample& cs : *restore)
                    if (cs.uid == c.uid) { c.sample = cs.sample; break; }
            }
    }
    sampleGrace_.clear();          // the previous generation, long since idle
    for (const TrackModel& t : ses_.tracks)
        for (const ClipModel& c : t.slots)
            if (c.sample) sampleGrace_.push_back(c.sample);

    const int wasTracks = (int)ses_.tracks.size();
    ses_ = std::move(next);

    // Identity. On a load, everything below hands out fresh UIDs and they must
    // come from a counter already pulled past whatever the file used. On a
    // restore there is nothing to fill in: the snapshot carries every uid and
    // the counter itself, and handing out new ones would break the very
    // identities (clip uid, device uid) the restore is matching on.
    if (!restoring) assignUids();

    // A set with nothing in it would leave the views indexing past the end.
    if (ses_.tracks.empty()) addTrack();
    if (ses_.scenes.empty()) addScene();
    selTrack_ = clampv(selTrack_, 0, (int)ses_.tracks.size() - 1);
    selSlot_  = clampv(selSlot_,  0, (int)ses_.scenes.size() - 1);
    selDevice_ = -1;
    // A return or the master is still there whatever the incoming set looks
    // like; a track index may not be, and the device view must not be left
    // pointing past the end of the new track list.
    if (ownIsTrack(devOwner_)) devOwner_ = selTrack_;
    // The tracks this index referred to are gone; the arms in the incoming set
    // are its own, not ours to take back.
    autoArmed_ = -1;

    materializeDevices(restoring ? &reuse : nullptr);   // may set its own status

    // A pool entry nothing adopted (two saved devices sharing a uid, a plugin
    // swapped under one) is still an instance the outgoing chain borrowed, so
    // it cannot simply be dropped here. It rides out with the newest chain that
    // has a retirement coming; if there is none -- the send failed, or nothing
    // was ever published -- it waits for shutdown, which is the same bargain
    // releaseAllChains makes for the same reason.
    RetiredChain* host = nullptr;
    for (auto it = retiring_.rbegin(); it != retiring_.rend(); ++it)
        if (it->chain) { host = &*it; break; }
    for (LiveDevice& ld : reuse) {
        if (!ld.inst) continue;
        if (!host) { retiring_.push_back(RetiredChain{}); host = &retiring_.back(); }
        host->dying.push_back(std::move(ld.inst));
    }

    pushAll();                     // also clears the slots outside the new set

    // The mixer flags of tracks the new set does not have. Their clips are
    // gone and their chains are empty, so volume and pan no longer describe
    // anything -- but solo is global by nature, and one left standing on a
    // track nobody can see any more would silence the whole set with no
    // visible cause. Bounded by how far the set actually shrank.
    for (int t = (int)ses_.tracks.size(); t < wasTracks; ++t) {
        send(Cmd::TrackSolo, t, 0);
        send(Cmd::TrackMute, t, 0);
        send(Cmd::TrackArm,  t, 0);
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

    status_ = "Loaded " + path;
    adoptSession(std::move(next), nullptr);   // may replace the status with a warning
    // The history belonged to the set that was open a moment ago. Undoing into
    // it would silently overwrite the one just loaded.
    clearUndo();
    return true;
}

void App::saveProjectTo(const std::string& path) {
    serializeDevices();
    std::string err;
    status_ = saveProject(ses_, path, &err) ? ("Saved " + path) : ("Save failed: " + err);
}

// ---------------------------------------------------------------------------
// undo / redo
//
// See the block in app.h for what an entry is, how gestures coalesce into one,
// and what is deliberately outside all of this. Everything here is GUI thread.
// ---------------------------------------------------------------------------

// Where a snapshot is staged on its way through the project serializer.
// XDG_RUNTIME_DIR is a per-user tmpfs, which is the right home for a file that
// exists for the length of one write and one read; /tmp is the fallback.
static std::string stagingPath() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    const std::string dir = (rt && *rt) ? rt : "/tmp";
    char buf[64];
    snprintf(buf, sizeof buf, "/nxtakt-undo-%d.lattice", (int)getpid());
    return dir + buf;
}

static bool writeAll(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t n = fwrite(text.data(), 1, text.size(), f);
    const bool ok = (n == text.size()) && (fflush(f) == 0);
    fclose(f);
    if (!ok) remove(path.c_str());
    return ok;
}

static bool readAll(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool bad = ferror(f) != 0;
    fclose(f);
    return !bad;
}

// The session as a project document. There is exactly one serializer in this
// program and it writes to a path -- there is no string form of saveProject,
// and a second writer for undo is precisely how the two would drift apart
// until an undo restored something a save could not express. So the snapshot
// goes out to tmpfs and comes straight back in.
bool App::snapshotSession(std::string& out, std::vector<ClipSample>& samples) {
    if (undoTmp_.empty()) undoTmp_ = stagingPath();
    // The audio first, because it is the half the text cannot carry.
    samples.clear();
    for (const TrackModel& t : ses_.tracks)
        for (const ClipModel& c : t.slots)
            if (c.sample && c.uid) samples.push_back(ClipSample{c.uid, c.sample});
    serializeDevices();            // live instances -> the passive form the writer reads
    // saveProject records where it wrote as the set's home. The staging file is
    // not where this set lives.
    const std::string home = ses_.path;
    std::string err;
    const bool ok = saveProject(ses_, undoTmp_, &err);
    ses_.path = home;
    if (!ok) {
        LOGW("undo: could not stage a snapshot: %s", err.c_str());
        return false;
    }
    out.clear();
    const bool got = readAll(undoTmp_, out);
    remove(undoTmp_.c_str());
    if (!got || out.empty()) {
        LOGW("undo: could not read the staged snapshot back");
        return false;
    }
    return true;
}

bool App::undoCoalesce(u64 gesture) {
    // A widget owns `active` for the whole of a drag, so the second and every
    // later frame of one gesture lands here and is refused. A one-shot edit
    // (a button, a key, a menu) has no active widget and always takes a point.
    const u64 g = gesture ? gesture : ui_.active;
    if (g && g == undoGesture_) return true;
    undoGesture_ = g;
    return false;
}

void App::pushUndoNow(const char* what) {
    std::string text;
    std::vector<ClipSample> samples;
    if (!snapshotSession(text, samples)) return;
    // An "edit" that changed nothing -- nudging a note already against the edge
    // of its clip, retyping the same name -- would otherwise leave an entry
    // that appears to do nothing when it comes back.
    if (!undo_.empty() && undo_.back().text == text) {
        redo_.clear();
        return;
    }
    UndoEntry e;
    e.text = std::move(text);
    e.samples = std::move(samples);
    e.what = what ? what : "edit";
    e.path = ses_.path;
    e.selTrack = selTrack_;
    e.selSlot = selSlot_;
    if ((int)undo_.size() >= kUndoDepth) undo_.erase(undo_.begin());
    undo_.push_back(std::move(e));
    redo_.clear();                 // the future this edit branched away from
}

void App::undoPoint(const char* what, u64 gesture) {
    if (undoCoalesce(gesture)) return;
    pushUndoNow(what);
}

void App::clearUndo() {
    undo_.clear();
    redo_.clear();
    undoGesture_ = 0;
}

// A take in flight has no coherent place in a session about to be replaced:
// the slot it is aimed at may not exist a moment from now, and half a
// recording is not a state to restore to. The buffer is NOT freed here -- the
// engine may still be appending to it -- so the stop goes out and the finish
// handler drops the material when it comes back.
void App::cancelTakes(const char* why) {
    bool any = false;
    for (PendingRec& p : pendingRecs_) {
        if (p.cancelled) continue;
        p.cancelled = true;
        stopRecording(p.track);
        any = true;
    }
    if (any && why) status_ = why;
}

// openProject's body, minus the disk. The snapshot is written back out to the
// staging file only because loadProject, like saveProject, speaks paths.
bool App::restoreEntry(const UndoEntry& e) {
    cancelTakes("Recording cancelled by undo");

    if (undoTmp_.empty()) undoTmp_ = stagingPath();
    if (!writeAll(undoTmp_, e.text)) {
        status_ = "Undo failed: cannot stage the snapshot";
        return false;
    }
    Session next;
    std::string err;
    const bool ok = loadProject(next, undoTmp_, engine_.sampleRate(), &err);
    remove(undoTmp_.c_str());
    if (!ok) {
        // Our own text failed to parse: a bug, not a user error. The session is
        // untouched (loadProject leaves its target alone on failure) and this
        // entry is not usable, so say so rather than pretending.
        LOGW("undo: snapshot did not parse: %s", err.c_str());
        status_ = "Undo failed: " + err;
        return false;
    }
    next.path = e.path;            // the staging file is not the set's home

    adoptSession(std::move(next), &e.samples);

    // Cursor, after adoptSession's own clamping: the point of carrying it is
    // that an undo lands where the edit happened.
    selTrack_ = clampv(e.selTrack, 0, (int)ses_.tracks.size() - 1);
    selSlot_  = clampv(e.selSlot,  0, (int)ses_.scenes.size() - 1);
    // The roll's selection is an index into a note vector that has just been
    // replaced wholesale, and a sounding preview belongs to the clip that was
    // on screen before the restore.
    if (roll_) roll_->clearSelection();
    stopPreviews();
    // A drag in flight names a source track and slot that the restored set may
    // not have. Nothing about a mouse gesture survives the model it was
    // dragging, and drawDragGhost would index straight past the end.
    drag_ = DragState{};
    return true;
}

void App::undo() {
    if (undo_.empty()) { status_ = "Nothing to undo"; return; }

    UndoEntry e = std::move(undo_.back());
    undo_.pop_back();

    // What is being left behind becomes the redo entry, under the same label:
    // it is the same edit, seen from the other side.
    UndoEntry back;
    const bool haveBack = snapshotSession(back.text, back.samples);
    if (haveBack) {
        back.what = e.what;
        back.path = ses_.path;
        back.selTrack = selTrack_;
        back.selSlot = selSlot_;
    }

    if (!restoreEntry(e)) {
        undo_.push_back(std::move(e));           // still the state to go back to
        return;
    }
    if (haveBack) {
        if ((int)redo_.size() >= kUndoDepth) redo_.erase(redo_.begin());
        redo_.push_back(std::move(back));
    }
    undoGesture_ = 0;
    status_ = "Undo: " + e.what;
}

// Drives one edit of each shape that reaches a different corner of the restore
// path -- a session scalar, a track flag, notes, the track list, a plugin
// parameter, a device removed and brought back -- and checks that undo and redo
// land on exactly the state they claim to. The serialized session is the
// comparison because it is the same thing an entry is made of: if a rebound
// plugin came back with the wrong parameter, or a clip lost its audio, the text
// says so.
void App::debugUndoSelfTest() {
    auto text = [this]() {
        std::string t;
        std::vector<ClipSample> s;
        snapshotSession(t, s);
        return t;
    };

    int fails = 0, ran = 0;
    // Returns false when the edit left the serialized set identical -- the
    // format cannot express it yet, so there was nothing to undo. The caller
    // may then have to put the set back itself; everything else is untouched.
    auto step = [&](const char* name, auto&& edit) {
        const std::string before = text();
        edit();
        const std::string after = text();
        if (after == before) {
            LOGW("undo self-test: %s changed nothing - not exercised", name);
            return false;
        }
        ++ran;
        undo();
        if (text() != before) { LOGE("undo self-test: %s did not undo", name); ++fails; }
        redo();
        if (text() != after)  { LOGE("undo self-test: %s did not redo", name); ++fails; }
        undo();                        // leave the set as this step found it
        if (text() != before) { LOGE("undo self-test: %s did not undo twice", name); ++fails; }
        return true;
    };

    step("tempo", [&] { undoPoint("tempo"); setTempo(ses_.tempo + 7.0); });
    step("rename track", [&] {
        std::string was = ses_.tracks[0].name;
        std::string now = was + " (edited)";
        std::swap(ses_.tracks[0].name, now);
        undoPointWith("rename track", ses_.tracks[0].name, was);
    });
    step("solo", [&] {
        const bool was = ses_.tracks[0].solo;
        ses_.tracks[0].solo = !was;
        undoPointWith("solo", ses_.tracks[0].solo, was);
        send(Cmd::TrackSolo, 0, ses_.tracks[0].solo ? 1 : 0);
    });
    step("add track", [&] { undoPoint("add track"); addTrack(); });
    // A send is a per-track array the mixer writes straight into, so it goes
    // through the same before-value path a fader does.
    step("send level", [&] {
        const f32 was = ses_.tracks[0].sends[0];
        ses_.tracks[0].sends[0] = was > 0.5f ? 0.f : 0.7f;
        undoPointWith(kSendUndo[0], ses_.tracks[0].sends[0], was);
        send(Cmd::SendLevel, 0, 0, ses_.tracks[0].sends[0]);
    });
    step("return volume", [&] {
        const f32 was = ses_.returns[0].fader;
        ses_.returns[0].fader = was > 0.5f ? 0.4f : 0.9f;
        undoPointWith("return volume", ses_.returns[0].fader, was);
        send(Cmd::ReturnVol, 0, 0, faderToGain(ses_.returns[0].fader));
    });

    // Notes, through the same before-value path the roll uses.
    int mt = -1, msl = -1;
    for (int t = 0; t < (int)ses_.tracks.size() && mt < 0; ++t)
        for (int s = 0; s < (int)ses_.scenes.size(); ++s)
            if (ses_.tracks[t].slots[s].kind == ClipKind::Midi &&
                ses_.tracks[t].slots[s].valid()) { mt = t; msl = s; break; }
    if (mt >= 0) {
        step("note edit", [&] {
            ClipModel& m = ses_.tracks[mt].slots[msl];
            const ClipModel was = m;
            m.notes.push_back(NoteModel{0.0, 0.25, 61, 99});
            undoPointWith("note edit", m, was);
            pushClip(mt, msl);
        });
    } else {
        LOGW("undo self-test: no MIDI clip in this set - notes not exercised");
    }

    // Devices: a parameter (rebound instance, value re-applied) and a removal
    // (instance retired, then instantiated again from the registry).
    int dt = -1;
    for (int t = 0; t < (int)ses_.tracks.size() && dt < 0; ++t)
        for (const DeviceModel& d : ses_.tracks[t].devices)
            if (d.inst && d.inst->paramCount() > 0) { dt = t; break; }
    if (dt >= 0) {
        step("device param", [&] {
            PluginInstance* in = ses_.tracks[dt].devices[0].inst.get();
            const ParamInfo& pi = in->paramInfo(0);
            const f32 v = in->getParam(0);
            undoPoint("param");
            in->setParam(0, v == pi.max ? pi.min : pi.max);
        });
        // The master chain, which reaches materializeDevices and the retirement
        // flow through the owner id rather than a track index. The plugin is
        // one the set already has loaded, so this costs no extra scan and works
        // on any machine the set itself works on.
        // The two chains that are not a track's, which reach materializeDevices
        // and the retirement flow through an owner id rather than a track
        // index. The plugin is one the set already has loaded, so this costs no
        // extra scan and works wherever the set itself does.
        const PluginDesc mdesc = ses_.tracks[dt].devices[0].desc;
        const int busOwners[2] = {kOwnMaster, ownReturn(0)};
        const char* busNames[2] = {"master device", "return device"};
        for (int k = 0; k < 2; ++k) {
            const int own = busOwners[k];
            if (step(busNames[k], [&] { undoPoint("add device"); addDevice(own, mdesc); })) continue;
            // The set text could not express this chain, so the undo had
            // nothing to take back and the device is still there. Put the set
            // back by hand: a self-test must not leave the session it borrowed
            // in a state the user did not ask for.
            ChainOwner co = chainOwner(own);
            if (co.devices && !co.devices->empty())
                removeDevice(own, (int)co.devices->size() - 1);
        }
        step("remove device", [&] { undoPoint("remove device"); removeDevice(dt, 0); });
    } else {
        LOGW("undo self-test: no device with parameters - devices not exercised");
    }

    step("clear clip", [&] {
        undoPoint("clear clip");
        clearClip(selTrack_, selSlot_);
    });

    // The history, and the status line, are the self-test's and not the user's.
    clearUndo();
    status_ = "Ready";
    if (fails) LOGE("undo self-test: %d FAILURE(S) across %d edits", fails, ran);
    else       LOGI("undo self-test: %d edits undone and redone cleanly", ran);
}

void App::redo() {
    if (redo_.empty()) { status_ = "Nothing to redo"; return; }

    UndoEntry e = std::move(redo_.back());
    redo_.pop_back();

    UndoEntry back;
    const bool haveBack = snapshotSession(back.text, back.samples);
    if (haveBack) {
        back.what = e.what;
        back.path = ses_.path;
        back.selTrack = selTrack_;
        back.selSlot = selSlot_;
    }

    if (!restoreEntry(e)) {
        redo_.push_back(std::move(e));
        return;
    }
    // Straight onto the undo stack, and without clearing the redo stack: this
    // is a walk back along the same history, not a new edit.
    if (haveBack) {
        if ((int)undo_.size() >= kUndoDepth) undo_.erase(undo_.begin());
        undo_.push_back(std::move(back));
    }
    undoGesture_ = 0;
    status_ = "Redo: " + e.what;
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
    // After the decode, so a file that could not be read leaves no history
    // behind, and before the slot is touched.
    undoPoint("load clip");

    ClipModel& m = ses_.tracks[track].slots[slot];
    // A slot that already held a clip keeps its identity: the material behind
    // it changed, but anything pointing at the clip (automation, a controller
    // mapping) still means this clip.
    if (!m.uid) m.uid = ses_.newUid();
    // Dropping a sample onto a pattern turns the slot back into an audio clip;
    // pushClip retires the notes the engine was holding for it.
    m.kind = ClipKind::Audio;
    m.notes.clear();
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
    selectTrack(track); selSlot_ = slot;
    status_ = "Loaded " + m.name;
}

void App::clearClip(int track, int slot) {
    ses_.tracks[track].slots[slot] = ClipModel{};
    // Through pushClip rather than a bare ClearClip: an emptied slot still has
    // to hand its note array back before anything frees it.
    pushClip(track, slot);
}

// Note-capable means the chain can be *played*: an instrument, or an effect
// that takes MIDI in (an arpeggiator, a MIDI-controlled filter). Either makes
// the track's empty slots MIDI targets rather than audio ones.
bool App::trackHasNoteDevice(int track) const {
    if (track < 0 || track >= (int)ses_.tracks.size()) return false;
    for (const DeviceModel& d : ses_.tracks[track].devices)
        if (d.desc.kind == PluginKind::Instrument || d.desc.hasMidiIn) return true;
    return false;
}

// An empty MIDI clip is a real, launchable, editable entity — Live's "create
// empty clip", and the only way to get a pattern without playing one in.
void App::createMidiClip(int track, int slot) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;

    // Here rather than at the (single) call site: this is the whole edit, and
    // the slot is untouched until the next line.
    undoPoint("new clip");

    ClipModel& m = ses_.tracks[track].slots[slot];
    m = ClipModel{};
    m.uid = ses_.newUid();
    m.kind = ClipKind::Midi;
    char buf[32];
    snprintf(buf, sizeof buf, "MIDI %d", midiClipNo_++);
    m.name = buf;
    m.colorIdx = ses_.tracks[track].colorIdx;
    m.lengthBeats = 4.0;                       // one bar in 4/4, like Live
    m.loop = true;
    pushClip(track, slot);
    selectTrack(track); selSlot_ = slot;
    detailTab_ = DetailTab::Clip;
    status_ = "New " + m.name;
}

// Points the DEVICES tab somewhere. Not an edit and not undoable -- it is the
// same kind of move as selecting a track, which is explicitly outside the
// history (see app.h).
void App::selectChainOwner(int owner) {
    if (!chainOwner(owner).valid()) return;
    if (devOwner_ != owner) {
        selDevice_ = -1;
        stripScroll_ = 0.f;
        paramScroll_ = 0.f;
    }
    devOwner_ = owner;
    // A bus has no clips, so the CLIP tab has nothing to show for it and the
    // panel would sit there looking at the last track's clip instead. Only the
    // tab is switched: a hidden panel stays hidden.
    if (!ownIsTrack(owner) && detailTab_ != DetailTab::Devices) {
        detailTab_ = DetailTab::Devices;
        ensurePluginScan();
    }
}

// Live's exclusive record-arm, which is what makes the computer keyboard and a
// controller play the track you just clicked on without a second gesture. The
// arm this hands out is ours to take back; one the user set by hand is not.
void App::selectTrack(int track) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    selTrack_ = track;
    // The device view follows the selection back off a bus. Guarded so that
    // clicking around the grid on the track already selected does not reset the
    // chain the user is editing every frame.
    if (devOwner_ != track) selectChainOwner(track);
    if (autoArmed_ == track) return;

    if (autoArmed_ >= 0 && autoArmed_ < (int)ses_.tracks.size()) {
        TrackModel& prev = ses_.tracks[autoArmed_];
        if (prev.arm) { prev.arm = false; send(Cmd::TrackArm, autoArmed_, 0); }
    }
    autoArmed_ = -1;

    TrackModel& t = ses_.tracks[track];
    if (t.arm) return;              // armed by hand: leave it, and do not claim it
    t.arm = true;
    send(Cmd::TrackArm, track, 1);
    autoArmed_ = track;
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

    // What the track can play decides what the take captures. A chain with an
    // instrument on it wants the notes, not the microphone.
    const bool midi = trackHasNoteDevice(track);

    PendingRec pr;
    pr.track = track; pr.slot = slot; pr.midi = midi;
    Command c;
    c.a = track; c.b = slot;

    if (midi) {
        RtNote* notes = new (std::nothrow) RtNote[kRecordNotes]();
        if (!notes) {
            status_ = "Out of memory - recording not started";
            return;
        }
        pr.notes = notes;
        pr.cap = kRecordNotes;
        c.type = Cmd::RecordMidiSlot;
        c.p = notes;
    } else {
        const i64 cap = (i64)std::llround(engine_.sampleRate() * kRecordSeconds);
        // Zeroed rather than raw: a take that stops early leaves the tail
        // unwritten, and silence is a far better failure than whatever was on
        // that page.
        f32* buf = new (std::nothrow) f32[(size_t)cap * 2]();
        if (!buf) {
            status_ = "Out of memory - recording not started";
            return;
        }
        pr.buf = buf;
        pr.cap = cap;
        c.type = Cmd::RecordSlot;
        c.p = buf;
    }
    c.x = (f64)pr.cap;

    if (!engine_.pushCommand(c)) {
        // The engine never saw the buffer, so it is still solely ours.
        delete[] pr.buf;
        delete[] pr.notes;
        status_ = "Engine busy - recording not started";
        return;
    }

    pendingRecs_.push_back(pr);
    selectTrack(track); selSlot_ = slot;
    status_ = midi ? "Record armed (MIDI)" : "Record armed";
}

void App::stopRecording(int track) {
    for (const PendingRec& p : pendingRecs_) {
        if (p.track != track) continue;
        // The same command toggles. Resend the buffer it was started with
        // rather than a null: the stop is a second Record*Slot for this slot,
        // and repeating the payload means an engine that simply reassigns
        // recBuf lands on exactly what it already had.
        Command c;
        c.type = p.midi ? Cmd::RecordMidiSlot : Cmd::RecordSlot;
        c.a = track; c.b = p.slot;
        c.p = p.midi ? (void*)p.notes : (void*)p.buf;
        c.x = (f64)p.cap;
        if (!engine_.pushCommand(c)) status_ = "Engine busy - still recording";
        return;
    }
}

void App::finishRecording(const Event& e) {
    f32* buf = (f32*)e.p;
    if (!buf) return;

    auto it = pendingRecs_.begin();
    for (; it != pendingRecs_.end(); ++it) if (!it->midi && it->buf == buf) break;
    if (it == pendingRecs_.end()) {
        LOGW("RecordFinished for an unknown buffer %p - leaking it rather than "
             "freeing a pointer we do not own", (const void*)buf);
        return;
    }
    if (it->cancelled) {
        // An undo replaced the session this take was aimed at (see
        // cancelTakes). The buffer coming home is the only thing that still had
        // to happen; the material is dropped rather than written into a
        // session that never asked for it.
        delete[] buf;
        pendingRecs_.erase(it);
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
            // pushUndoNow rather than undoPoint: this runs while engine events
            // are drained, so a widget the user happens to be dragging still
            // owns ui_.active and would coalesce a take away.
            pushUndoNow("record");
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
            selectTrack(track); selSlot_ = slot;
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

// Ev::MidiRecordFinished: the same hand-back as an audio take, but the payload
// is the note buffer and turning it into a clip is a copy rather than a resample.
void App::finishMidiRecording(const Event& e) {
    RtNote* buf = (RtNote*)e.p;
    if (!buf) return;

    auto it = pendingRecs_.begin();
    for (; it != pendingRecs_.end(); ++it) if (it->midi && it->notes == buf) break;
    if (it == pendingRecs_.end()) {
        LOGW("MidiRecordFinished for an unknown buffer %p - leaking it rather "
             "than freeing a pointer we do not own", (const void*)buf);
        return;
    }
    if (it->cancelled) {          // see finishRecording
        delete[] buf;
        pendingRecs_.erase(it);
        return;
    }

    const int track = e.a, slot = e.b;
    const int count = clampv((int)e.x, 0, (int)it->cap);
    const bool inRange = track >= 0 && track < (int)ses_.tracks.size() &&
                         slot  >= 0 && slot  < (int)ses_.scenes.size();

    // One note of the take, in the model's form. Both paths below want exactly
    // this, and the clamps are the ones the format applies anyway.
    const auto asNote = [](const RtNote& r) {
        NoteModel n;
        n.beat  = std::max(0.0, r.beat);
        n.len   = std::max(1.0 / 64.0, r.len);
        n.pitch = (u8)clampv((int)r.pitch, 0, 127);
        n.vel   = (u8)clampv((int)r.vel, 1, 127);
        return n;
    };
    // Everything downstream -- the roll, the RtNote array, the engine's own
    // scheduler -- wants notes by beat. The engine pairs ons with offs as they
    // arrive, so a short note inside a long one comes back out of order, and an
    // overdub arrives after everything already in the clip regardless.
    const auto sortByBeat = [](std::vector<NoteModel>& v) {
        std::stable_sort(v.begin(), v.end(),
                         [](const NoteModel& a, const NoteModel& b) { return a.beat < b.beat; });
    };

    // Overdub. The slot already held a pattern, so this take was a looper pass
    // over it: the engine kept the clip playing, wrapped each captured note's
    // beat into the clip's loop, and returned only the NEW notes (see the
    // Cmd::RecordMidiSlot contract in engine.h). Merging is therefore the whole
    // job -- the length stays the clip's, and nothing already in it is touched.
    if (count > 0 && inRange &&
        ses_.tracks[track].slots[slot].valid() &&
        ses_.tracks[track].slots[slot].kind == ClipKind::Midi) {
        pushUndoNow("overdub");             // not undoPoint: see finishRecording
        ClipModel& m = ses_.tracks[track].slots[slot];
        m.notes.reserve(m.notes.size() + (size_t)count);
        for (int i = 0; i < count; ++i) m.notes.push_back(asNote(buf[i]));
        sortByBeat(m.notes);

        pushClip(track, slot);
        selectTrack(track); selSlot_ = slot;
        detailTab_ = DetailTab::Clip;
        char st[80];
        snprintf(st, sizeof st, "Overdubbed %d note%s  -  %s now has %zu",
                 count, count == 1 ? "" : "s", m.name.c_str(), m.notes.size());
        status_ = st;

        delete[] buf;
        pendingRecs_.erase(it);
        return;
    }

    if (count > 0 && inRange) {
        // Length. The engine hands back notes and a count, not a duration, so
        // the take's musical length is derived from two GUI-side facts and the
        // longer one wins: how far the transport has moved since the quantized
        // start reached us in Ev::RecordStarted, and where the last note ends.
        // (The first is the honest answer for a take that ends in silence; the
        // second covers a note still sounding as the stop boundary lands, and
        // is the only one available if RecordStarted was missed.) That is then
        // rounded UP to a whole bar, so a pattern loops in time with the set
        // instead of at whatever instant the second click happened.
        f64 endBeat = 0.0;
        for (int i = 0; i < count; ++i) endBeat = std::max(endBeat, buf[i].beat + buf[i].len);
        endBeat = std::max(endBeat, engine_.beat.load() - recStartBeat_[track]);
        const f64 barBeats = std::max(1, ses_.sigNum);
        const f64 bars = std::max(1.0, std::ceil(endBeat / barBeats - 1e-9));

        char name[32];
        snprintf(name, sizeof name, "Rec %d", recTakeNo_++);

        pushUndoNow("record");              // not undoPoint: see finishRecording
        ClipModel& m = ses_.tracks[track].slots[slot];
        m = ClipModel{};
        m.uid = ses_.newUid();
        m.kind = ClipKind::Midi;
        m.name = name;
        m.colorIdx = ses_.tracks[track].colorIdx;
        m.clipBpm = ses_.tempo;
        m.lengthBeats = bars * barBeats;
        m.gain = 1.f;
        m.loop = true;
        m.notes.reserve((size_t)count);
        for (int i = 0; i < count; ++i) m.notes.push_back(asNote(buf[i]));
        sortByBeat(m.notes);

        pushClip(track, slot);
        selectTrack(track); selSlot_ = slot;
        detailTab_ = DetailTab::Clip;
        char st[64];
        snprintf(st, sizeof st, "Recorded %s  -  %d notes", name, count);
        status_ = st;
    } else if (count <= 0) {
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
    // Note previews are ended by the frame loop, not by a timer: whatever else
    // this frame does, an audition started a moment ago has to be allowed to
    // stop. Ahead of the UI so a preview retired here can be restarted below.
    updatePreviews();

    const f32 s = win_.dpiScale();
    const f32 W = (f32)win_.width(), H = (f32)win_.height();

    rend_.begin(win_.width(), win_.height(), s);
    glClearColor(pal::appBg.r, pal::appBg.g, pal::appBg.b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ui_.beginFrame();

    // A gesture ends when what was driving it lets go, and the next one has to
    // take an entry of its own even when it is the same fader being dragged a
    // second time. Ui::endFrame() drops `active` on mouse-up, so by now it is
    // already gone; the arrows are the one gesture not held by a widget.
    {
        const Input& k = win_.input();
        const bool arrows = k.keyDown[KeyLeft] || k.keyDown[KeyRight] ||
                            k.keyDown[KeyUp]   || k.keyDown[KeyDown];
        if (!ui_.active && !(arrows && undoGesture_ == kArrowGesture)) undoGesture_ = 0;
    }

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

    // Undo / redo, edge-detected for the same reason: a held Ctrl+Z would run
    // a full session restore every frame. Ctrl+Shift+Z and Ctrl+Y both redo,
    // which is the split the rest of the world never settled.
    const bool undoChord = in.keyDown['z'] && in.ctrl() && !in.shift();
    const bool redoChord = (in.keyDown['z'] && in.ctrl() && in.shift()) ||
                           (in.keyDown['y'] && in.ctrl());
    if (undoChord && !undoKeyPrev_) undo();
    if (redoChord && !redoKeyPrev_) redo();
    undoKeyPrev_ = undoChord;
    redoKeyPrev_ = redoChord;

    // While the piano is on it owns the printable keys, so an unmodified letter
    // is a note and not a shortcut — see KbdPiano::consumes for why this is now
    // the whole block rather than the mapped keys: the piano reads positions
    // and shortcuts read keysyms, and on a non-US layout the two disagree.
    // Ctrl- and Alt-modified chords are unaffected: notes only fire unmodified.
    const auto plain = [&](int k) {
        return in.keyPressed[k] && !in.ctrl() && !(kbdMidi_ && KbdPiano::consumes(k));
    };

    if (in.keyPressed[' ']) togglePlay();
    if (in.keyPressed[KeyTab])
        view_ = (view_ == MainView::Session) ? MainView::Arrangement : MainView::Session;
    if (in.keyPressed['b'] && in.ctrl()) showBrowser_ = !showBrowser_;
    if (in.keyPressed['d'] && in.ctrl()) showDetail_ = !showDetail_;
    if (plain('m')) {
        undoPoint("metronome");
        ses_.metronome = !ses_.metronome;
        send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    }
    if (in.keyPressed['t'] && in.ctrl()) { undoPoint("add track"); addTrack(); }
    if (in.keyPressed[KeyEnter] && in.ctrl()) { undoPoint("add scene"); addScene(); }

    // --- keys the piano roll can claim --------------------------------------
    // The roll only claims a key while it is on screen for the selected clip,
    // and the note-scoped keys only while a note is selected in it. Everything
    // else keeps its session-wide meaning, so the editor never steals a key it
    // has no use for. The selection is read once, before anything below can
    // change it, and the clip is only reached through the roll — visibleRoll()
    // has already bounds-checked the indices it would be read with.
    PianoRoll* const roll = visibleRoll();
    ClipModel* const selClip = roll ? &ses_.tracks[selTrack_].slots[selSlot_] : nullptr;
    const bool noteSel = roll && roll->hasSelection(*selClip);

    // Escape is layered rather than overridden: with a note selected it drops
    // that selection (the editor's own scope) and nothing else; pressing it
    // again — or with nothing selected — reaches the global stop, which is
    // what it has always done and what a panicking user expects of it.
    if (in.keyPressed[KeyEscape]) {
        if (noteSel) roll->clearSelection();
        else         send(Cmd::StopAll);
    }
    // Delete with a note selected removes the note, not the clip that contains
    // it. Clearing the whole pattern from under an active note edit would be a
    // spectacular way to lose work.
    if (in.keyPressed[KeyDelete] || (in.keyPressed[KeyBackspace] && !in.ctrl())) {
        if (noteSel) {
            // The roll edits the clip in place, so the entry has to be taken
            // with the clip as it was -- and only if the edit happened at all,
            // which is not knowable until the call returns. Copying a clip is
            // a note vector and two strings; see undoPointWith.
            const ClipModel before = *selClip;
            if (roll->deleteSelected(*selClip)) {
                undoPointWith("delete note", *selClip, before);
                pushClip(selTrack_, selSlot_);
            }
        } else {
            undoPoint("clear clip");
            clearClip(selTrack_, selSlot_);
        }
    }
    // Live's duplicate-loop (Cmd+D there; Ctrl+D is already the detail panel
    // here, so Ctrl+U). Clip-scoped, not note-scoped: no selection needed.
    if (in.keyPressed['u'] && in.ctrl() && roll) {
        const ClipModel before = *selClip;
        if (roll->duplicateLoop(*selClip)) {
            undoPointWith("duplicate loop", *selClip, before);
            pushClip(selTrack_, selSlot_);
            char buf[64];
            snprintf(buf, sizeof buf, "Loop duplicated — %.0f beats", selClip->lengthBeats);
            status_ = buf;
        }
    }

    const int nt = (int)ses_.tracks.size(), ns = (int)ses_.scenes.size();
    if (noteSel) {
        // Arrows nudge the note and do NOT move the clip selection: with an
        // editor open on a note, "left" means that note, and having the panel
        // switch to another clip mid-edit is the trap this avoids.
        int steps = 0, semis = 0;
        if (in.keyPressed[KeyLeft])  --steps;
        if (in.keyPressed[KeyRight]) ++steps;
        const int step = in.shift() ? 12 : 1;      // Shift = octave, as everywhere
        if (in.keyPressed[KeyUp])    semis += step;
        if (in.keyPressed[KeyDown])  semis -= step;
        if (steps || semis) {
            const ClipModel before = *selClip;
            if (roll->nudgeSelected(*selClip, steps, semis)) {
                // The arrows auto-repeat, so sliding a note across two beats is
                // one gesture and not thirty entries. It ends when the key
                // comes up (see frame()).
                undoPointWith("nudge note", *selClip, before, kArrowGesture);
                pushClip(selTrack_, selSlot_);
            }
        }
    } else {
        // Through selectTrack so arrowing across the grid arms what it lands
        // on, exactly as clicking does.
        if (in.keyPressed[KeyLeft])  selectTrack(clampv(selTrack_ - 1, 0, nt - 1));
        if (in.keyPressed[KeyRight]) selectTrack(clampv(selTrack_ + 1, 0, nt - 1));
        if (in.keyPressed[KeyUp])    selSlot_  = clampv(selSlot_ - 1, 0, ns - 1);
        if (in.keyPressed[KeyDown])  selSlot_  = clampv(selSlot_ + 1, 0, ns - 1);
    }
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

    // scanDown[] rather than keyDown[]: the piano is a set of key *positions*
    // (KbdPiano::semiFor), so it plays the same on QWERTZ and AZERTY as on
    // QWERTY. The octave keys are passed separately because they are labelled
    // keys and follow the layout like any other named shortcut.
    const KbdPiano::Result res = kbd_.update(in.scanDown, in.keyDown[KeyPageUp],
        in.keyDown[KeyPageDown], live,
        [this](const MidiMsg& m) { engine_.pushMidiFromGui(m); });

    if (res.baseChanged) {
        char buf[96];
        snprintf(buf, sizeof buf, "Keyboard octave C%d · velocity %d", kbd_.octave(), kbd_.velocity());
        status_ = buf;
    }

    // The commonest way to conclude the keyboard is broken is to switch it on
    // with nothing armed: the notes reach the engine and go nowhere, silently.
    // Said once when the condition arrives, not once a frame.
    bool anyArm = false;
    for (const TrackModel& t : ses_.tracks) if (t.arm) { anyArm = true; break; }
    const bool hint = kbdMidi_ && !anyArm;
    if (hint != kbdNoArmHint_) {
        kbdNoArmHint_ = hint;
        if (hint) status_ = "Arm a track to hear the keyboard (auto-arm: click a track)";
    }
}

void App::toggleKbdMidi() {
    kbdMidi_ = !kbdMidi_;
    if (kbdMidi_) {
        char buf[192];
        snprintf(buf, sizeof buf,
                 "Computer MIDI Keyboard on — ZXCVBNM lower octave (C%d), QWERTYU / IOP above, "
                 "SDGHJ + 23567 90 black, PgUp/PgDn octave, Ctrl+Shift+K off",
                 kbd_.octave());
        status_ = buf;
    } else {
        // Anything still held has to be let go here: the key release that would
        // normally end the note is about to be ignored, and a hung note would
        // sit in the instrument with nothing left to stop it.
        kbd_.allNotesOff([this](const MidiMsg& m) { engine_.pushMidiFromGui(m); });
        status_ = "Computer MIDI Keyboard off";
    }
}

// ---------------------------------------------------------------------------
// piano roll: key routing and note preview
// ---------------------------------------------------------------------------

// On screen and showing the selected clip's notes — the only state in which the
// roll may claim a key or hold a meaningful selection. Note that it also
// answers "was the roll ever drawn", since roll_ is created by drawClipDetail.
PianoRoll* App::visibleRoll() {
    if (!roll_ || view_ != MainView::Session || !showDetail_ || detailTab_ != DetailTab::Clip)
        return nullptr;
    if (selTrack_ < 0 || selTrack_ >= (int)ses_.tracks.size()) return nullptr;
    if (selSlot_ < 0 || selSlot_ >= kMaxScenes) return nullptr;
    const ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
    return m.kind == ClipKind::Midi ? roll_.get() : nullptr;
}

void App::startPreview(int pitch, u64 clipUid) {
    if (pitch < 0 || pitch > 127) return;
    // Previews belong to one clip at a time: the moment the panel shows a
    // different one, the old clip's notes are stopped rather than left ringing
    // under the new one (updatePreviews does the checking).
    if (clipUid != previewClip_) {
        stopPreviews();
        previewClip_ = clipUid;
    }
    const f64 off = nowSeconds() + kPreviewSecs;
    // Same pitch again: retrigger rather than stack, so a repeated nudge is
    // audible as repeated notes and never leaves two offs chasing one on.
    for (Preview& p : previews_) {
        if (p.pitch != (u8)pitch) continue;
        engine_.pushMidiFromGui(MidiMsg{0x80, p.pitch, 0, 0, 0});
        engine_.pushMidiFromGui(MidiMsg{0x90, p.pitch, (u8)kPreviewVel, 0, 0});
        p.offAt = off;
        return;
    }
    // Full: the oldest audition gives way. A dropped preview would be a note
    // that never sounds; a hung one would be a note that never stops.
    if ((int)previews_.size() >= kMaxPreviews) {
        engine_.pushMidiFromGui(MidiMsg{0x80, previews_.front().pitch, 0, 0, 0});
        previews_.erase(previews_.begin());
    }
    engine_.pushMidiFromGui(MidiMsg{0x90, (u8)pitch, (u8)kPreviewVel, 0, 0});
    previews_.push_back(Preview{(u8)pitch, off});
}

void App::updatePreviews() {
    if (previews_.empty()) return;
    // Context check first. A preview outlives whatever started it, and both the
    // clip and the panel can vanish between frames (another slot selected,
    // Ctrl+D, the DEVICES tab, Arrangement). Nothing downstream will ever end
    // these notes if this does not.
    const PianoRoll* live = visibleRoll();
    if (!live || ses_.tracks[selTrack_].slots[selSlot_].uid != previewClip_) {
        stopPreviews();
        return;
    }
    const f64 now = nowSeconds();
    for (size_t i = 0; i < previews_.size();) {
        if (previews_[i].offAt <= now) {
            engine_.pushMidiFromGui(MidiMsg{0x80, previews_[i].pitch, 0, 0, 0});
            previews_.erase(previews_.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

void App::stopPreviews() {
    for (const Preview& p : previews_) engine_.pushMidiFromGui(MidiMsg{0x80, p.pitch, 0, 0, 0});
    previews_.clear();
    previewClip_ = 0;
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
        if (now - lastTap < 3.0) {
            undoPoint("tempo");
            setTempo(clampv(60.0 / (now - lastTap), 20.0, 999.0));
        }
        lastTap = now;
    }
    x += tapR.w + 4 * s;

    Rect tempoR{x, cy, 62 * s, h};
    f64 bpm = ses_.tempo;
    // The number is edited through a copy, so the session still holds the old
    // tempo here and a plain undoPoint is enough; the drag coalesces on the
    // widget's id.
    if (ui_.dragNumber(uiId(1, 1), tempoR, &bpm, 20.0, 999.0, 0.15, "%.2f")) {
        undoPoint("tempo");
        setTempo(bpm);
    }
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
        undoPoint("metronome");
        ses_.metronome = !ses_.metronome;
        send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    }
    x += metR.w + 12 * s;

    // --- global launch quantum ---
    rend_.textIn(fSmall_, {x, cy, 26 * s, h}, "Q", pal::textFaint, Align::Left, 0);
    Rect quantR{x + 16 * s, cy, 62 * s, h};
    // The selector writes into the session and only then reports the change,
    // so the entry needs the index handed back to it.
    const int wasQuantum = ses_.quantumIdx;
    if (ui_.selector(uiId(1, 3), quantR, &ses_.quantumIdx, kQuantumNames, kQuantumCount)) {
        undoPointWith("launch quantum", ses_.quantumIdx, wasQuantum);
        send(Cmd::SetQuantum, ses_.quantumIdx);
    }
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
    // carries the octave so PgUp / PgDn have somewhere to show their work, and
    // velocity sits next to it as a number: the FL layout spends C and V on
    // notes, so there are no keys left to nudge it with.
    {
        f64 vel = (f64)kbd_.velocity();
        Rect vr{rx - 34 * s, cy, 34 * s, h};
        if (ui_.dragNumber(uiId(16, 0), vr, &vel, 1.0, 127.0, 0.35, "%.0f")) {
            kbd_.setVelocity((int)std::lround(vel));
            char buf[64];
            snprintf(buf, sizeof buf, "Keyboard velocity %d", kbd_.velocity());
            status_ = buf;
        }
        rx = vr.x - 3 * s;

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

    // Right-hand furniture, in Live's order: the scene launchers stay against
    // the clip grid (their rows line up with it), then the return buses, then
    // the master. Everything the mix ends up in reads left to right.
    const f32 masterW = lay::masterW * s;
    const f32 sceneW  = lay::sceneColW * s;
    const f32 retW    = lay::returnW * s * kMaxReturns;
    Rect masterCol{r.right() - masterW, r.y, masterW, r.h};
    Rect retCol{masterCol.x - retW, r.y, retW, r.h};
    Rect sceneCol{retCol.x - sceneW, r.y, sceneW, r.h};
    Rect tracksCol{r.x, r.y, std::max(0.f, sceneCol.x - r.x), r.h};

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
    drawReturnStrips(retCol);
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

        // textField writes the new name and only then says it committed, and it
        // can only commit on a frame where it already owns the caret -- so the
        // old name is captured then, and only then.
        const u64 nameId = uiId(3, 1000 + (int)i);
        std::string wasName;
        if (ui_.editId == nameId) wasName = t.name;
        if (ui_.textField(nameId, cell, &t.name,
                          Col(0, 0, 0, 0), sel ? pal::text : pal::textDim, Align::Left))
            undoPointWith("rename track", t.name, wasName);
        if (hot && in.pressed[0]) selectTrack((int)i);
    }

    // "+" to append a track.
    Rect add{x, r.y, 22 * s, h};
    if (add.x < r.right()) {
        if (ui_.button(uiId(3, 900), add, "+")) { undoPoint("add track"); addTrack(); }
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
                selectTrack(ti); selSlot_ = si;
                if (recHere)      stopRecording(ti);       // second click stops
                else if (target)  startRecording(ti, si);
            }
            // Double-click on an empty slot of a note-capable track makes an
            // empty pattern to draw into. Only when the record button is unlit:
            // with it lit the same slot is a take waiting to happen, and the
            // first click of the double has already started one.
            if (in.dblClick && !recIntent_ && !recHere && trackHasNoteDevice(ti))
                createMidiClip(ti, si);
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

    // Recording into a slot that already holds a clip is an overdub, so the
    // slot keeps its playing look and gains the record dot rather than turning
    // solid red the way a slot being captured into from empty does: what is on
    // screen is still the clip, and it is still playing.
    f32 nameW = cell.w - btnW - 2 * s;
    f32 markRight = cell.right();
    if (recHere) {
        // Pulsing while the take waits for its quantum, solid once it is
        // capturing - the same two states the empty-slot look has, said quietly.
        const f32 a = recPhase >= 2 ? 1.f : (f32)(0.45 + 0.45 * std::sin(nowSeconds() * 8.0));
        rend_.circle(markRight - 7 * s, cell.cy(), 3.5f * s, pal::recRed.alpha(a));
        markRight -= 13 * s;
        nameW -= 13 * s;
    }

    // A MIDI clip gets a three-dot mark on the right: at 21px of row height a
    // real piano glyph is a smudge, and the dots read as "notes, not audio"
    // without competing with the name.
    if (m.kind == ClipKind::Midi) {
        const f32 d = 1.6f * s;
        const f32 dx0 = markRight - 12 * s;
        for (int i = 0; i < 3; ++i)
            rend_.rect({dx0 + i * 3.5f * s, cell.cy() - d * 0.5f - (i == 1 ? 2 * s : 0.f), d, d},
                       pal::textOnClip.alpha(0.6f));
        nameW -= 14 * s;
    }
    rend_.textIn(fBody_, {cell.x + btnW, cell.y, std::max(4 * s, nameW), cell.h},
                 m.name.c_str(), pal::textOnClip, Align::Left, 2 * s);

    // Playback progress along the bottom edge. The engine publishes clipPhase
    // for a MIDI clip exactly as for an audio one, so this needs no special case.
    if (playing) {
        const f64 ph = clampv(engine_.clipPhase[ti].load(), 0.0, 1.0);
        rend_.rect({cell.x, cell.bottom() - 2 * s, cell.w * (f32)ph, 2 * s}, pal::textOnClip.alpha(0.45f));
    }
    if (sel) rend_.roundRectOutline(cell, 2 * s, 1 * s, pal::accent);

    if (hot) {
        ui_.cursor = Cursor::Hand;
        if (in.pressed[0]) {
            selectTrack(ti); selSlot_ = si;
            // With the record button lit, a MIDI clip on an armed track is an
            // overdub target and not just something to launch: the engine
            // relaunches it at the record boundary and captures another pass
            // into it (see the Cmd::RecordMidiSlot contract). A second click
            // stops the take, exactly as on an empty slot. Audio clips are
            // untouched by this - there is no overdub for a sample.
            const bool overdub = recIntent_ && ses_.tracks[ti].arm &&
                                 m.kind == ClipKind::Midi && trackHasNoteDevice(ti);
            if (recHere)       stopRecording(ti);
            else if (overdub)  startRecording(ti, si);
            else               send(Cmd::LaunchClip, ti, si);
            drag_.kind = DragState::Kind::Clip;
            drag_.srcTrack = ti; drag_.srcSlot = si;
            drag_.startX = in.mx; drag_.startY = in.my;
            drag_.armed = false;
        }
        if (in.pressed[2]) {
            selectTrack(ti); selSlot_ = si;
            undoPoint("clear clip");
            clearClip(ti, si);
        }
    }

    // Drop target for a drag in flight.
    if (drag_.kind != DragState::Kind::None && drag_.armed && hot && in.released[0]) {
        if (drag_.kind == DragState::Kind::BrowserFile) {
            loadClipInto(ti, si, drag_.path);   // takes its own entry, after the decode
        } else if (drag_.srcTrack != ti || drag_.srcSlot != si) {
            // One entry for the whole move: the destination write and the
            // source clear are halves of the same edit.
            undoPoint(in.ctrl() ? "copy clip" : "move clip");
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
        const u64 nameId = uiId(5, 1000 + si);
        std::string wasName;                     // see drawTrackHeaders
        if (ui_.editId == nameId) wasName = ses_.scenes[si].name;
        if (ui_.textField(nameId, {cell.x + 14 * s, cell.y, cell.w - 16 * s, cell.h},
                          &ses_.scenes[si].name, Col(0, 0, 0, 0), pal::text, Align::Left))
            undoPointWith("rename scene", ses_.scenes[si].name, wasName);

        if (hot) ui_.cursor = Cursor::Hand;
        if (hot && in.pressed[0]) { selSlot_ = si; send(Cmd::LaunchScene, si); }
    }

    Rect stopAll{r.x + 2 * s, top + ns * slotH, r.w - 4 * s, slotH - lay::gutter * s};
    if (stopAll.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 900), stopAll, "STOP ALL")) send(Cmd::StopAll);
    }

    Rect add{r.x + 2 * s, stopAll.bottom() + 4 * s, r.w - 4 * s, 18 * s};
    if (add.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 901), add, "+ Scene")) { undoPoint("add scene"); addScene(); }
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
        // Every control in this strip is bound straight to the model and writes
        // before it reports, so each hands its previous value to the entry.
        const bool wasMute = t.mute, wasSolo = t.solo, wasArm = t.arm;
        const f32  wasPan = t.pan, wasFader = t.fader;
        if (ui_.squareToggle(uiId(6, (int)ti, 0), mr, "M", &t.mute, pal::meterAmber)) {
            undoPointWith("mute", t.mute, wasMute);
            send(Cmd::TrackMute, (int)ti, t.mute ? 1 : 0);
        }
        if (ui_.squareToggle(uiId(6, (int)ti, 1), sr, "S", &t.solo, pal::soloBlue)) {
            undoPointWith("solo", t.solo, wasSolo);
            send(Cmd::TrackSolo, (int)ti, t.solo ? 1 : 0);
        }
        // Record-arm is a filled dot in Live, and the glyph atlas is ASCII-only,
        // so draw the dot rather than trying to letter it.
        if (ui_.squareToggle(uiId(6, (int)ti, 2), ar, "", &t.arm, pal::armRed)) {
            // Arming by hand is an edit; the auto-arm that follows the
            // selection is not, and takes no entry of its own.
            undoPointWith("arm", t.arm, wasArm);
            send(Cmd::TrackArm, (int)ti, t.arm ? 1 : 0);
            // Touched by hand: this arm is the user's now, so selecting another
            // track must not take it away again.
            if ((int)ti == autoArmed_) autoArmed_ = -1;
        }
        rend_.circle(ar.cx(), ar.cy(), 3.5f * s, t.arm ? pal::textOnClip : pal::armRed);
        y += 20 * s;

        // Sends A-D, above the pan knob as a 2x2 grid. A strip is 94px wide, so
        // four knobs in a row would be 12px across and unusable; two rows of two
        // leave room for a 15px knob with its letter beside it, which is the
        // smallest thing here that still reads as a send and not as a dot.
        // Anything the user has dialled in also shows as an arc, so a track with
        // send on it is visible without hovering.
        {
            const f32 cellW = (col.w - 12 * s) * 0.5f;
            const f32 rowH  = 18 * s;
            for (int rn = 0; rn < kMaxReturns; ++rn) {
                Rect cell{col.x + 6 * s + (rn % 2) * cellW, y + (rn / 2) * rowH, cellW, rowH};
                rend_.textIn(fSmall_, {cell.x, cell.y, 9 * s, cell.h}, kReturnLetter[rn],
                             pal::textFaint, Align::Left, 0);
                Rect kr{cell.x + 10 * s, cell.y + 1 * s, 15 * s, 15 * s};
                const f32 wasSend = t.sends[rn];
                if (ui_.knob(uiId(6, (int)ti, 10 + rn), kr, &t.sends[rn], 0.f, 1.f, 0.f)) {
                    undoPointWith(kSendUndo[rn], t.sends[rn], wasSend);
                    send(Cmd::SendLevel, (int)ti, rn, t.sends[rn]);
                }
            }
            y += 2 * rowH + 3 * s;
        }

        // Pan
        Rect pan{col.cx() - 11 * s, y, 22 * s, 22 * s};
        if (ui_.knob(uiId(6, (int)ti, 3), pan, &t.pan, -1.f, 1.f, 0.f)) {
            undoPointWith("pan", t.pan, wasPan);
            send(Cmd::TrackPan, (int)ti, 0, t.pan);
        }
        y += 26 * s;

        // Fader + meter
        const f32 fh = col.bottom() - y - 6 * s;
        Rect fader{col.x + 10 * s, y, 16 * s, fh};
        Rect meter{fader.right() + 5 * s, y, 9 * s, fh};
        if (ui_.vFader(uiId(6, (int)ti, 4), fader, &t.fader)) {
            undoPointWith("volume", t.fader, wasFader);
            send(Cmd::TrackVol, (int)ti, 0, faderToGain(t.fader));
        }

        const f32 lvl = std::max(engine_.meterL[ti].load(), engine_.meterR[ti].load());
        peakHoldT_[ti] = std::max(lvl, peakHoldT_[ti] * 0.985f);
        ui_.meterV(meter, lvl, peakHoldT_[ti]);
    }
    rend_.popClip();
}

// The A-D buses. No clips, no M/S/arm, no pan: a return is a name, a chain and
// a level, so the strip is a header, the chain's device names where a track has
// its grid, and a fader with its meter. Clicking anywhere that is not a control
// points the DEVICES tab at the bus, which is the only way to edit its chain.
void App::drawReturnStrips(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    if (r.w <= 0.f) return;
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, 1 * s, r.h}, pal::divider);

    const f32 colW = r.w / (f32)kMaxReturns;
    const f32 top  = r.bottom() - lay::mixerH * s;

    for (int i = 0; i < kMaxReturns; ++i) {
        ReturnModel& rt = ses_.returns[i];
        const int owner = ownReturn(i);
        const bool sel  = devOwner_ == owner;
        Rect col{r.x + i * colW, r.y, colW - lay::gutter * s, r.h};

        // Claimed first so the fader and the name field can take hot back --
        // the same last-setHot-wins trick the device boxes use.
        const u64 id = uiId(13, i, 0);
        const bool hot = ui_.setHot(id, col) && ui_.isHot(id);
        rend_.rect(col, sel ? pal::panelAlt : pal::panel);

        Rect head{col.x, col.y, col.w, lay::trackHeadH * s};
        rend_.rect(head, sel ? pal::gridBg : pal::panelAlt);
        rend_.rect({head.x, head.y, head.w, 2 * s}, pal::soloBlue);
        rend_.textIn(fBold_, {head.x + 3 * s, head.y, 10 * s, head.h}, kReturnLetter[i],
                     sel ? pal::text : pal::textDim, Align::Left, 0);
        // The model's placeholder name is "Return" for all four buses, which
        // says nothing in a strip this narrow and would be clipped to "Retu"
        // anyway -- so the letter carries the identity and the field stays
        // blank until the bus is named. A DISPLAY choice, deliberately: writing
        // a letter into the model would make every set on disk carry four
        // return blocks it has no reason to (see project.cpp's `interesting`).
        const u64 nameId = uiId(13, i, 1);
        std::string shown = (rt.name == kReturnPlaceholder) ? std::string() : rt.name;
        if (ui_.textField(nameId, {head.x + 13 * s, head.y, head.w - 15 * s, head.h},
                          &shown, Col(0, 0, 0, 0), sel ? pal::text : pal::textDim, Align::Left)) {
            const std::string was = rt.name;
            rt.name = shown.empty() ? std::string(kReturnPlaceholder) : shown;
            undoPointWith("rename return", rt.name, was);
        }

        // What the bus is made of, in the space a track spends on clips. A
        // return with an empty chain is inert, and saying so beats an empty
        // column the user has no reason to click on.
        Rect body{col.x, head.bottom(), col.w, top - head.bottom()};
        rend_.pushClip(body);
        if (rt.devices.empty()) {
            rend_.textIn(fSmall_, {body.x, body.y + 6 * s, body.w, 12 * s}, "no fx",
                         pal::textFaint, Align::Center, 0);
        } else {
            f32 dy = body.y + 4 * s;
            for (const DeviceModel& d : rt.devices) {
                if (dy + 12 * s > body.bottom()) break;
                Rect row{body.x + 3 * s, dy, body.w - 6 * s, 12 * s};
                rend_.roundRect(row, 2 * s, pal::panelAlt);
                rend_.pushClip(row);
                rend_.textIn(fSmall_, row, d.desc.name.c_str(),
                             d.inst ? pal::textDim : pal::armRed, Align::Left, 3 * s);
                rend_.popClip();
                dy += 14 * s;
            }
        }
        rend_.popClip();

        Rect mix{col.x, top, col.w, r.bottom() - top};
        rend_.rect({mix.x, mix.y, mix.w, 1 * s}, pal::divider);
        // The same top inset the master strip uses, so the buses and the mix
        // they land in read as one row of faders rather than a staircase.
        f32 y = mix.y + 26 * s;
        const f32 fh = mix.bottom() - y - 6 * s;
        Rect fader{mix.x + 10 * s, y, 15 * s, fh};
        Rect meter{fader.right() + 5 * s, y, 9 * s, fh};

        const f32 wasFader = rt.fader;
        if (ui_.vFader(uiId(13, i, 2), fader, &rt.fader)) {
            undoPointWith("return volume", rt.fader, wasFader);
            send(Cmd::ReturnVol, i, 0, faderToGain(rt.fader));
        }
        const f32 lvl = std::max(engine_.returnMeterL[i].load(), engine_.returnMeterR[i].load());
        peakHoldR_[i] = std::max(lvl, peakHoldR_[i] * 0.985f);
        ui_.meterV(meter, lvl, peakHoldR_[i]);

        if (sel) rend_.roundRectOutline(col, 2 * s, 1 * s, pal::accent);
        if (hot) {
            ui_.cursor = Cursor::Hand;
            if (in.pressed[0]) selectChainOwner(owner);
        }
    }
}

void App::drawMasterStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const bool sel = devOwner_ == kOwnMaster;
    rend_.rect(r, pal::panelAlt);
    rend_.rect({r.x, r.y, 1 * s, r.h}, pal::divider);

    // Same deal as a return: the strip is the handle for the master chain, so
    // the whole column is a click target that the controls in it take back.
    const u64 id = uiId(7, 10);
    const bool hot = ui_.setHot(id, r) && ui_.isHot(id);

    Rect head{r.x, r.y, r.w, lay::trackHeadH * s};
    rend_.rect(head, sel ? pal::gridBg : pal::panel);
    rend_.textIn(fBold_, head, "MASTER", pal::text, Align::Center);

    const f32 top = r.bottom() - lay::mixerH * s;
    Rect mix{r.x, top, r.w, lay::mixerH * s};
    rend_.rect({mix.x, mix.y, mix.w, 1 * s}, pal::divider);

    // The master chain, where a return lists its own: this is where a bus
    // compressor or a saturator across the whole mix lives.
    {
        Rect body{r.x, head.bottom(), r.w, top - head.bottom()};
        rend_.pushClip(body);
        f32 dy = body.y + 4 * s;
        for (const DeviceModel& d : ses_.masterDevices) {
            if (dy + 12 * s > body.bottom()) break;
            Rect row{body.x + 4 * s, dy, body.w - 8 * s, 12 * s};
            rend_.roundRect(row, 2 * s, pal::panel);
            rend_.pushClip(row);
            rend_.textIn(fSmall_, row, d.desc.name.c_str(),
                         d.inst ? pal::textDim : pal::armRed, Align::Left, 3 * s);
            rend_.popClip();
            dy += 14 * s;
        }
        if (ses_.masterDevices.empty())
            rend_.textIn(fSmall_, {body.x, body.y + 6 * s, body.w, 12 * s}, "no fx",
                         pal::textFaint, Align::Center, 0);
        rend_.popClip();
    }

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

    if (sel) rend_.roundRectOutline(r, 2 * s, 1 * s, pal::accent);
    if (hot) {
        ui_.cursor = Cursor::Hand;
        if (in.pressed[0]) selectChainOwner(kOwnMaster);
    }
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
            ChainOwner co = chainOwner(devOwner_);
            const size_t n = co.devices ? co.devices->size() : 0;
            snprintf(buf, sizeof buf, "%s  -  %zu device%s", ownerName(devOwner_).c_str(),
                     n, n == 1 ? "" : "s");
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
    // A pattern has no sample behind it, so warp, clip tempo and the loop
    // *range* have nothing to act on; everything else on this panel is about
    // launching, which a MIDI clip does exactly like an audio one.
    const bool midi = m.kind == ClipKind::Midi;

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

    {   // Warp mode (audio only) + loop, which both kinds have
        Rect row{ctrl.x, y, ctrl.w, rowH};
        Rect lp{row.x + lblW, row.y, 52 * s, row.h};
        if (!midi) {
            label("WARP", row);
            static const char* warpNames[] = {"Off", "Repitch", "Beats"};
            int wi = (int)m.warp;
            Rect sel{row.x + lblW, row.y, 84 * s, row.h};
            if (ui_.selector(uiId(8, 0), sel, &wi, warpNames, 3)) {
                undoPoint("warp mode");
                m.warp = (Warp)wi;
                send(Cmd::ClipWarp, selTrack_, selSlot_, (f64)wi);
            }
            lp = {sel.right() + 6 * s, row.y, 52 * s, row.h};
        } else {
            label("PLAY", row);
        }
        if (ui_.button(uiId(8, 1), lp, "LOOP", m.loop, pal::accent)) {
            undoPoint("clip loop");
            m.loop = !m.loop;
            send(Cmd::ClipLoop, selTrack_, selSlot_, m.loop ? 1.0 : 0.0);
        }
        y += rowH + 4 * s;
    }
    if (!midi) {   // Clip tempo
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("CLIP BPM", row);
        f64 bpm = m.clipBpm;
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 2), dn, &bpm, 20.0, 400.0, 0.1, "%.2f")) {
            undoPoint("clip tempo");
            m.clipBpm = bpm;
            pushClip(selTrack_, selSlot_);
        }
        // Halve / double, exactly like Live's :2 and *2 buttons.
        Rect h2{dn.right() + 6 * s, row.y, 26 * s, row.h};
        Rect d2{h2.right() + 3 * s, row.y, 26 * s, row.h};
        if (ui_.button(uiId(8, 3), h2, ":2")) {
            undoPoint("clip tempo");
            m.clipBpm *= 0.5;
            pushClip(selTrack_, selSlot_);
        }
        if (ui_.button(uiId(8, 4), d2, "*2")) {
            undoPoint("clip tempo");
            m.clipBpm *= 2.0;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Gain
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("GAIN", row);
        f64 db = gainToDb(m.gain);
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 5), dn, &db, -70.0, 12.0, 0.1, "%.1f dB")) {
            undoPoint("clip gain");
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
            undoPoint("clip quantum");
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
            undoPoint("launch probability");
            m.prob = clampv(pct * 0.01, 0.0, 1.0);
            pushClip(selTrack_, selSlot_);
        }

        int fa = (int)m.followAction;
        Rect fr{pr.right() + 6 * s, row.y, 58 * s, row.h};
        if (ui_.selector(uiId(13, 1), fr, &fa, kFollowNames, kFollowCount)) {
            undoPoint("follow action");
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
            undoPoint("follow length");
            m.followBeats = fb;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Read-out of what the engine will actually do
        Rect row{ctrl.x, y, ctrl.w, rowH};
        char buf[96];
        if (midi) {
            snprintf(buf, sizeof buf, "%.2f beats  ·  %zu note%s", m.lengthBeats,
                     m.notes.size(), m.notes.size() == 1 ? "" : "s");
        } else {
            const f64 rate = (m.warp == Warp::Off) ? 1.0 : m.clipBpm / ses_.tempo;
            snprintf(buf, sizeof buf, "%.2f beats  ·  rate %.3fx  ·  %d ch",
                     m.lengthBeats, rate, m.sample->channels);
        }
        rend_.textIn(fSmall_, row, buf, pal::textFaint, Align::Left, 0);
    }

    // --- the material: a piano roll for a pattern, a waveform for a sample ---
    Rect wave{ctrl.right() + 12 * s, head.bottom() + 6 * s,
              r.right() - ctrl.right() - 20 * s, r.bottom() - head.bottom() - 12 * s};

    // Where the clip is, in its own beats, so both editors can draw the same
    // playhead from the same number.
    const bool active = engine_.activeSlot[selTrack_].load() == selSlot_;
    const f64  phase  = clampv(engine_.clipPhase[selTrack_].load(), 0.0, 1.0);

    if (midi) {
        if (!roll_) roll_ = std::make_unique<PianoRoll>();
        // The roll edits m.notes (and its length) in place and says whether it
        // touched anything; republishing is ours, and pushClip is what retires
        // the array the engine is still reading from.
        //
        // The undo entry therefore needs the clip as it was *before* the call,
        // which is why the copy is taken unconditionally: whether an edit
        // happens is not knowable until draw() returns, and by then m already
        // has it. A clip is a note vector, two strings and a shared pointer --
        // cheap enough to copy once a frame, and it buys the one thing that
        // matters here, which is that a click that adds a note can be undone.
        // The roll owns ui_.active for the length of a drag, so a note dragged
        // across the grid leaves one entry and not one per frame.
        const ClipModel before = m;
        if (roll_->draw(ui_, wave, m, active ? phase * m.lengthBeats : 0.0, active)) {
            undoPointWith("note edit", m, before);
            pushClip(selTrack_, selSlot_);
        }
        // Auditioning is the caller's job: the roll only names the pitches that
        // want to be heard (from this draw, and from any keyboard edit earlier
        // in the frame — handleShortcuts runs first). See previews_ for why
        // these reach the right instrument.
        u8 pv[PianoRoll::kPreviewMax];
        const int np = roll_->drainPreview(pv, PianoRoll::kPreviewMax);
        for (int i = 0; i < np; ++i) startPreview((int)pv[i], m.uid);
        return;
    }

    rend_.roundRect(wave, 2 * s, pal::appBg);
    rend_.pushClip(wave.inset(2 * s));
    drawWaveform(wave.inset(3 * s), *m.sample, ccol.scale(0.85f));

    // Playhead, when this clip is the one sounding on its track.
    if (active) {
        const f32 px = wave.x + 3 * s + (wave.w - 6 * s) * (f32)phase;
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
        // The entry is taken here rather than inside addDevice, which
        // init() also calls through the NXTAKT_DEBUG_ADDFX hook: nothing that
        // happens while the app is starting up belongs in the history.
        if (hot && in.dblClick) {
            undoPoint("add device");
            addDevice(devOwner_, d);
        }
    }
    rend_.popClip();
}

void App::drawDeviceStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::panel);

    // The chain being edited belongs to a track, a return or the master; past
    // this point the only difference is the colour of the identity chip.
    ChainOwner co = chainOwner(devOwner_);
    if (!co.devices) {                       // the target went away under us
        devOwner_ = selTrack_;
        co = chainOwner(devOwner_);
        if (!co.devices) return;             // nothing is clipped yet
    }
    std::vector<DeviceModel>& devices = *co.devices;
    const Col tc = ownIsTrack(devOwner_)
                 ? pal::clipColors[ses_.tracks[devOwner_].colorIdx % pal::clipColorCount]
                 : (ownIsReturn(devOwner_) ? pal::soloBlue : pal::accent);

    Rect head{r.x, r.y, r.w, 16 * s};
    rend_.rect(head, pal::panelAlt);
    rend_.rect({head.x, head.y, 4 * s, head.h}, tc);       // owner identity chip
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, 220 * s, head.h},
                 ownerName(devOwner_).c_str(), pal::text, Align::Left, 0);
    rend_.textIn(fSmall_, head, "double-click a plugin to add it to this chain",
                 pal::textFaint, Align::Right, 8 * s);

    Rect area{r.x, head.bottom(), r.w, r.bottom() - head.bottom()};
    rend_.pushClip(area);

    // Keep the selection honest: the target can be switched under it, and a
    // device can have been removed since the last frame.
    if (devices.empty()) selDevice_ = -1;
    else selDevice_ = clampv(selDevice_ < 0 ? 0 : selDevice_, 0, (int)devices.size() - 1);

    if (devices.empty()) {
        char msg[80];
        snprintf(msg, sizeof msg, "No devices on %s", ownerName(devOwner_).c_str());
        rend_.textIn(fBody_, area, msg, pal::textFaint, Align::Center);
        rend_.popClip();
        return;
    }

    const f32 boxW = 150 * s, gap = 5 * s;
    const f32 total = devices.size() * (boxW + gap) + 6 * s;
    const f32 maxScroll = std::max(0.f, total - area.w);
    stripScroll_ = clampv(stripScroll_, 0.f, maxScroll);
    bool wheelUsed = false;

    f32 x = area.x + 6 * s - stripScroll_;
    for (size_t i = 0; i < devices.size(); ++i) {
        DeviceModel& d = devices[i];
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
        const bool wasBypass = d.bypass;
        if (ui_.squareToggle(uiId(11, (int)i, 0), br, "", &d.bypass, pal::meterAmber)) {
            undoPointWith("bypass", d.bypass, wasBypass);
            if (d.inst) d.inst->setBypassed(d.bypass);
        }
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
            // The instance is retired with the outgoing chain, so undoing this
            // loads the plugin again and applies the parameters the snapshot
            // carries - see materializeDevices. What a plugin holds beyond its
            // parameters does not survive, which is the same trade a saved set
            // makes and is documented as such in app.h.
            undoPoint("remove device");
            removeDevice(devOwner_, (int)i);
            rend_.popClip();
            return;                       // the device list changed under us
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

            // Both controls edit a copy and hand the result to the instance, so
            // the value the snapshot reads (serializeDevices asks the instance)
            // is still the old one when the entry is taken. A knob drag
            // coalesces on the widget's id, as everywhere else.
            if (info.isBool) {
                Rect tg{cell.cx() - 11 * s, cell.y + 8 * s, 22 * s, 14 * s};
                bool on = d.inst->getParam(p) > 0.5f;
                if (ui_.squareToggle(uiId(12, (int)i * 256 + p, 0), tg, "", &on, pal::accent)) {
                    undoPoint(info.name.c_str());
                    d.inst->setParam(p, on ? info.max : info.min);
                }
            } else {
                Rect kr{cell.cx() - 16 * s, cell.y + 2 * s, 32 * s, 32 * s};
                f32 v = d.inst->getParam(p);
                if (ui_.knob(uiId(12, (int)i * 256 + p, 0), kr, &v, info.min, info.max,
                             info.def, info.isInt ? "%.0f" : "%.2f")) {
                    undoPoint(info.name.c_str());
                    d.inst->setParam(p, v);
                }
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

    // Delay compensation, when the engine is applying any. It is latency the
    // user did not ask for and cannot see anywhere else, and it moves when a
    // plugin is added to a chain, so it belongs beside the buffer size.
    char pdcTag[24] = "";
    const int pdc = engine_.latencyFrames.load();
    if (pdc > 0) snprintf(pdcTag, sizeof pdcTag, " · PDC %d", pdc);

    char buf[224];
    snprintf(buf, sizeof buf, "%s · %s %.0f Hz / %d fr%s%s · %.0f fps · %d draws",
             win_.backendName(),
             audio_ ? audio_->name() : "silent",
             audio_ ? audio_->sampleRate() : 0.0,
             audio_ ? audio_->bufferSize() : 0,
             pdcTag,
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
