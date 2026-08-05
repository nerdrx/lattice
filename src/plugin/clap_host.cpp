// CLAP backend.
//
// Scope of what we support today:
//   * clap.audio-ports (float32 / data32 only), clap.params, clap.note-ports.
//     MIDI handed to midi() is translated into note/MIDI events on the
//     plugin's first note input port, in whichever dialect that port prefers.
//   * clap.latency: queried once after activate() and served from
//     latencyFrames(). The host side of the extension is accepted and ignored;
//     see kHostLatency for why dynamic latency is out of scope.
//   * host extensions offered to plugins: clap.log, clap.thread-check and
//     clap.latency. Everything else returns null, which is always a legal
//     answer.
//   * parameter changes travel as CLAP_EVENT_PARAM_VALUE through in_events.
//     CLAP forbids writing parameter memory behind the plugin's back, so the
//     GUI thread posts into a lock-free ring that process() drains.
//
// DSO lifetime: entries are dlopen()ed once and cached by path for the process
// lifetime; we never call deinit()/dlclose(). Two reasons. First, instantiate()
// reuses the scan's factory, so unloading after the scan would mean a second
// dlopen per instantiation and a window where a descriptor pointer (owned by
// the DSO and only valid until deinit()) dangles. Second, dlclose() of a plugin
// binary is actively dangerous when its dependency chain owns live threads —
// see the pinPluginLibrary() note in lv2_host.cpp for the failure mode. The
// cost is bounded: one mapping per .clap file that was found on disk once.
//
// Realtime rules: process() only memcpy/memsets into buffers allocated in
// prepare(), drains a fixed-size ring into a preallocated event array, and
// calls the plugin. Nothing there allocates, locks or throws.
#include "host.h"
#include "../core/ring.h"

#include <clap/clap.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lat {
namespace detail {
namespace {

namespace fs = std::filesystem;

// --- thread identity -------------------------------------------------------
// clap.thread-check must answer honestly or plugins that assert on it will
// abort. "Main" is whichever thread first entered the backend, which by the
// contract in host.h is the GUI thread; "audio" is any thread currently inside
// ClapInstance::process(). Constant-initialised so reading it emits no guard.
thread_local bool tlsInProcess = false;

std::thread::id& mainThreadId() {
    static std::thread::id id;
    return id;
}
// Called from scanCLAP()/instantiateCLAP(), both documented GUI-thread only.
void rememberMainThread() {
    if (mainThreadId() == std::thread::id{}) mainThreadId() = std::this_thread::get_id();
}

bool CLAP_ABI hostIsMainThread(const clap_host_t*) {
    return !tlsInProcess && std::this_thread::get_id() == mainThreadId();
}
bool CLAP_ABI hostIsAudioThread(const clap_host_t*) { return tlsInProcess; }

const clap_host_thread_check_t kThreadCheck = { hostIsMainThread, hostIsAudioThread };

// The spec forbids logging from the audio thread, so this is main-thread only
// in practice and may take the same liberties LV2's log feature does.
void CLAP_ABI hostLogFn(const clap_host_t*, clap_log_severity sev, const char* msg) {
    if (!msg) return;
    switch (sev) {
        case CLAP_LOG_ERROR:
        case CLAP_LOG_FATAL:
        case CLAP_LOG_PLUGIN_MISBEHAVING:
        case CLAP_LOG_HOST_MISBEHAVING: LOGE("clap: %s", msg); break;
        case CLAP_LOG_WARNING:          LOGW("clap: %s", msg); break;
        default:                        LOGI("clap: %s", msg); break;
    }
}
const clap_host_log_t kHostLog = { hostLogFn };

// clap.latency, host side. The spec allows latency to change only during
// activate(), and requires an already-active plugin to follow the notification
// with request_restart(). Acting on either would mean tearing the instance
// down, re-preparing it and republishing the chain -- and then re-aligning
// every parallel path feeding the master sum, because a changed latency
// invalidates the compensation the engine computed. None of that machinery
// exists yet, and PluginInstance::latencyFrames() is documented as constant
// after prepare(), so the honest thing is to accept the call and do nothing
// with it. The value we keep is the one read at prepare() time.
//
// This is still worth exposing rather than answering null: a plugin that finds
// no clap.latency host extension may conclude the host does not do delay
// compensation at all and quietly change what it reports.
//
// TODO(dynamic latency): re-query the extension on restart, republish the
// chain, and recompute the PDC alignment. Until then a plugin that moves its
// latency after activation is misaligned until the chain is rebuilt.
void CLAP_ABI hostLatencyChanged(const clap_host_t*) {}
const clap_host_latency_t kHostLatency = { hostLatencyChanged };

const void* CLAP_ABI hostGetExtension(const clap_host_t*, const char* id) {
    if (!id) return nullptr;
    if (strcmp(id, CLAP_EXT_LOG) == 0)          return &kHostLog;
    if (strcmp(id, CLAP_EXT_THREAD_CHECK) == 0) return &kThreadCheck;
    if (strcmp(id, CLAP_EXT_LATENCY) == 0)      return &kHostLatency;
    return nullptr;   // params/gui/state/... not wired up yet
}

// request_* can arrive from any thread including the audio thread, so they may
// not log or allocate. The instance stores the request as a flag; acting on it
// needs a main-thread pump the app does not have yet. TODO: drain these.
void CLAP_ABI hostRequestRestart(const clap_host_t* h);
void CLAP_ABI hostRequestProcess(const clap_host_t* h);
void CLAP_ABI hostRequestCallback(const clap_host_t* h);

// --- entry cache -----------------------------------------------------------
struct Entry {
    void*                        handle  = nullptr;
    const clap_plugin_entry_t*   entry   = nullptr;
    const clap_plugin_factory_t* factory = nullptr;   // null => this file is unusable
};

// GUI-thread only (scan/instantiate are, per host.h), so no lock is needed.
std::unordered_map<std::string, Entry>& entryCache() {
    static std::unordered_map<std::string, Entry> c;
    return c;
}

// The host handed to create_plugin() during scanning. Must outlive every probe
// instance, hence static; it carries no host_data because nothing it exposes
// needs per-instance state.
const clap_host_t kProbeHost = {
    CLAP_VERSION_INIT, nullptr, "Lattice", "Lattice", "", "1.0",
    hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback,
};

// Opens (or returns the cached) entry for a plugin DSO. Failures are cached as
// a null factory so a broken file is dlopen()ed at most once per process.
const Entry* openEntry(const std::string& path) {
    auto& cache = entryCache();
    auto it = cache.find(path);
    if (it != cache.end()) return it->second.factory ? &it->second : nullptr;

    Entry e;
    // RTLD_LOCAL: plugin symbols must not leak into the global namespace, where
    // two plugins statically linking different builds of the same library would
    // resolve into each other.
    e.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!e.handle) {
        // dlerror() clears itself, so it may only be read once.
        const char* err = dlerror();
        LOGW("clap: dlopen failed for %s (%s)", path.c_str(), err ? err : "?");
        cache.emplace(path, e);
        return nullptr;
    }

    e.entry = (const clap_plugin_entry_t*)dlsym(e.handle, "clap_entry");
    if (!e.entry) {
        LOGW("clap: %s exports no clap_entry", path.c_str());
        dlclose(e.handle);
        e.handle = nullptr;
        cache.emplace(path, e);
        return nullptr;
    }
    if (!clap_version_is_compatible(e.entry->clap_version) || !e.entry->init || !e.entry->get_factory) {
        LOGW("clap: %s has an incompatible entry (v%u.%u.%u)", path.c_str(),
             e.entry->clap_version.major, e.entry->clap_version.minor, e.entry->clap_version.revision);
        dlclose(e.handle);
        e.handle = nullptr;
        e.entry  = nullptr;
        cache.emplace(path, e);
        return nullptr;
    }

    if (!e.entry->init(path.c_str())) {
        LOGW("clap: init() failed for %s", path.c_str());
        // init() returning false means deinit() must not be called; just drop it.
        dlclose(e.handle);
        e.handle = nullptr;
        e.entry  = nullptr;
        cache.emplace(path, e);
        return nullptr;
    }

    e.factory = (const clap_plugin_factory_t*)e.entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!e.factory || !e.factory->get_plugin_count || !e.factory->get_plugin_descriptor ||
        !e.factory->create_plugin) {
        LOGW("clap: %s provides no usable plugin factory", path.c_str());
        e.factory = nullptr;   // keep the DSO mapped; see the file header
    }

    auto ins = cache.emplace(path, e);
    return ins.first->second.factory ? &ins.first->second : nullptr;
}

// --- descriptor helpers ----------------------------------------------------
bool hasFeature(const clap_plugin_descriptor_t* d, const char* f) {
    if (!d->features) return false;
    for (const char* const* p = d->features; *p; ++p)
        if (strcmp(*p, f) == 0) return true;
    return false;
}

// The main-category features carry the kind; anything else is a sub-category
// and makes a better browser label ("reverb" beats "audio-effect").
bool isMainCategory(const char* f) {
    static const char* kMain[] = {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
        CLAP_PLUGIN_FEATURE_NOTE_EFFECT, CLAP_PLUGIN_FEATURE_NOTE_DETECTOR,
        CLAP_PLUGIN_FEATURE_ANALYZER,
    };
    for (const char* m : kMain)
        if (strcmp(m, f) == 0) return true;
    return false;
}

std::string categoryOf(const clap_plugin_descriptor_t* d) {
    std::string firstMain;
    if (d->features) {
        for (const char* const* p = d->features; *p; ++p) {
            if (!**p) continue;
            if (!isMainCategory(*p)) return *p;
            if (firstMain.empty()) firstMain = *p;
        }
    }
    return firstMain;
}

std::string safeStr(const char* s) { return s ? std::string(s) : std::string(); }

// PluginDesc::uri is documented as "path:index" for non-LV2 formats. The index
// is digits, so splitting on the *last* colon keeps paths containing colons
// working.
std::string makeUri(const std::string& path, uint32_t index) {
    return path + ":" + std::to_string(index);
}
bool splitUri(const std::string& uri, std::string& path, uint32_t& index) {
    const size_t c = uri.rfind(':');
    if (c == std::string::npos || c + 1 >= uri.size()) return false;
    for (size_t i = c + 1; i < uri.size(); ++i)
        if (uri[i] < '0' || uri[i] > '9') return false;
    path  = uri.substr(0, c);
    index = (uint32_t)strtoul(uri.c_str() + c + 1, nullptr, 10);
    return true;
}

// --- filesystem walk -------------------------------------------------------
// A symlink cycle under a search root would otherwise walk forever.
constexpr int kMaxScanDepth = 8;

// A Linux .clap can be a bare shared object or a bundle directory; in the
// bundle case the DSO lives somewhere under it (Contents/<arch>/x.so).
std::string bundleDso(const std::string& bundle) {
    std::vector<std::pair<std::string, int>> stack{{bundle, 0}};
    while (!stack.empty()) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            std::error_code e2;
            if (fs::is_directory(p, e2)) {
                if (depth < kMaxScanDepth) stack.emplace_back(p.string(), depth + 1);
            } else if (p.extension() == ".so") {
                return p.string();
            }
        }
    }
    return {};
}

void walkForClap(const std::string& root, std::vector<std::string>& out) {
    std::vector<std::pair<std::string, int>> stack{{root, 0}};
    while (!stack.empty()) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            std::error_code e2;
            const bool isDir = fs::is_directory(p, e2);
            if (p.extension() == ".clap") {
                if (!isDir) { out.push_back(p.string()); continue; }
                const std::string so = bundleDso(p.string());
                if (!so.empty()) out.push_back(so);
                else LOGW("clap: bundle %s contains no shared object", p.string().c_str());
                continue;               // never descend into a bundle
            }
            if (isDir && depth < kMaxScanDepth) stack.emplace_back(p.string(), depth + 1);
        }
    }
}

std::vector<std::string> searchPaths() {
    std::vector<std::string> dirs;
    auto add = [&dirs](std::string p) {
        if (p.empty()) return;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        for (const std::string& e : dirs)
            if (e == p) return;
        dirs.push_back(std::move(p));
    };
    // CLAP_PATH wins so a developer build shadows the installed copy.
    if (const char* env = std::getenv("CLAP_PATH")) {
        const std::string s = env;
        size_t start = 0;
        for (;;) {
            const size_t sep = s.find(':', start);
            add(s.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
    }
    if (const char* home = std::getenv("HOME")) add(std::string(home) + "/.clap");
    add("/usr/lib/clap");
    add("/usr/local/lib/clap");
    return dirs;
}

// --- instance --------------------------------------------------------------
class ClapInstance final : public PluginInstance {
public:
    ClapInstance(const PluginDesc& d, const clap_plugin_factory_t* factory, std::string id)
        : desc_(d), factory_(factory), id_(std::move(id)) {
        // create_plugin() takes this pointer and the plugin may keep it until
        // destroy(), so the host struct has to live in the instance.
        host_ = kProbeHost;
        host_.host_data = this;
        inEvents_  = { this, evSize, evGet };
        outEvents_ = { this, evTryPush };
    }

    ~ClapInstance() override { teardown(); }

    bool prepare(f64 sampleRate, int maxBlock) override {
        teardown();
        if (maxBlock <= 0) maxBlock = kMaxBlock;
        sr_ = sampleRate;
        maxBlock_ = maxBlock;

        plug_ = factory_->create_plugin(factory_, &host_, id_.c_str());
        if (!plug_) {
            LOGE("clap: create_plugin failed for %s", id_.c_str());
            return false;
        }
        if (!plug_->init || !plug_->init(plug_)) {
            LOGE("clap: init failed for %s", id_.c_str());
            if (plug_->destroy) plug_->destroy(plug_);
            plug_ = nullptr;
            return false;
        }

        // Port layout is fixed once activated, so both queries happen first.
        buildPorts();
        buildParams();

        if (!plug_->activate(plug_, sr_, 1, (uint32_t)maxBlock_)) {
            LOGE("clap: activate failed for %s at %.0f Hz / %d frames",
                 id_.c_str(), sr_, maxBlock_);
            teardown();
            return false;
        }
        active_ = true;
        readLatency();

        if (!plug_->start_processing(plug_)) {
            LOGE("clap: start_processing failed for %s", id_.c_str());
            teardown();
            return false;
        }
        processing_ = true;
        return true;
    }

    // REALTIME. memcpy in, drain the param ring into the preallocated event
    // array, call process(), memcpy out. No allocation, no locks.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;

        // Bypass, a dead instance, or an oversized block all degrade to a
        // straight copy: growing buffers here would mean allocating on the
        // audio thread, so we refuse instead. Same rule as the LV2 backend.
        if (bypassed_ || !processing_ || nframes > maxBlock_ || outChans_ == 0) {
            midiCount_ = 0;                   // events belong to this block only
            passthrough(in, out, channels, nframes);
            return;
        }

        tlsInProcess = true;                  // makes clap.thread-check honest

        const size_t bytes = (size_t)nframes * sizeof(f32);

        // A plugin input with no matching track channel gets channel 0
        // duplicated (stereo plugin on a mono source); with no input at all it
        // gets silence (instruments). Identical to lv2_host.cpp.
        for (int p = 0; p < inChans_; ++p) {
            const f32* src = nullptr;
            if (in) src = (p < channels) ? in[p] : in[0];
            if (src) std::memcpy(inPtrs_[(size_t)p], src, bytes);
            else     std::memset(inPtrs_[(size_t)p], 0, bytes);
        }

        buildEventList();

        for (clap_audio_buffer_t& b : inBufs_)  b.constant_mask = 0;
        for (clap_audio_buffer_t& b : outBufs_) b.constant_mask = 0;

        clap_process_t px{};
        px.steady_time         = steady_;
        px.frames_count        = (uint32_t)nframes;
        px.transport           = nullptr;      // free-running host, no transport yet
        px.audio_inputs        = inBufs_.empty()  ? nullptr : inBufs_.data();
        px.audio_outputs       = outBufs_.empty() ? nullptr : outBufs_.data();
        px.audio_inputs_count  = (uint32_t)inBufs_.size();
        px.audio_outputs_count = (uint32_t)outBufs_.size();
        px.in_events           = &inEvents_;
        px.out_events          = &outEvents_;

        const clap_process_status st = plug_->process(plug_, &px);
        eventCount_ = 0;
        steady_ += nframes;
        tlsInProcess = false;

        // "The output buffer must be discarded" — pass the input through
        // instead of shipping whatever the plugin left behind.
        if (st == CLAP_PROCESS_ERROR) {
            passthrough(in, out, channels, nframes);
            return;
        }

        // A mono plugin (1 out) on a stereo track is run once and its output is
        // copied to both channels; this is the documented behaviour, not a
        // second instance. Again matching lv2_host.cpp.
        for (int c = 0; c < channels; ++c) {
            if (!out[c]) continue;
            std::memcpy(out[c], outPtrs_[(size_t)(c < outChans_ ? c : 0)], bytes);
        }
    }

    // REALTIME. Buffered into a fixed array and turned into CLAP events at the
    // top of the next process(). midi() and process() are both audio-thread
    // only and midi() is documented to run first for the block, so unlike the
    // GUI-to-audio parameter path this needs no ring and no atomics — a plain
    // array plus a count is correct.
    void midi(const u8* data, int len, int frameOffset) override {
        if (!data || len < 1 || len > 3) return;
        if (midiCount_ >= kMaxMidiEvents) return;      // dropped, not grown
        MidiMsg& m = midi_[midiCount_++];
        m.len = (u8)len;
        for (int i = 0; i < 3; ++i) m.data[i] = i < len ? data[i] : 0;
        m.frame = frameOffset < 0 ? 0 : (u32)frameOffset;
    }

    int              paramCount() const override     { return (int)params_.size(); }
    const ParamInfo& paramInfo(int i) const override { return params_[(size_t)i]; }

    f32 getParam(int i) const override {
        if (i < 0 || i >= (int)values_.size()) return 0.f;
        return values_[(size_t)i];
    }

    // GUI thread, concurrent with process(). CLAP parameters may only be
    // changed through the event stream, so unlike the LV2 backend this cannot
    // just store a float: it posts to a lock-free ring the audio thread drains.
    // values_ is still written here so getParam() reflects the UI immediately.
    void setParam(int i, f32 v) override {
        if (i < 0 || i >= (int)params_.size()) return;
        const ParamInfo& pi = params_[(size_t)i];
        const f32 clamped = clampv(v, pi.min, pi.max);
        values_[(size_t)i] = clamped;
        // A full ring means the audio thread is not running (or is wedged). The
        // value is kept above, so the UI stays coherent; the plugin picks it up
        // only when a later change gets through. TODO: params->flush() when the
        // engine is idle.
        if (!queue_.push(ParamMsg{ (u32)i, clamped }))
            LOGW("clap: param queue full, %s dropped", pi.name.c_str());
    }

    const PluginDesc& desc() const override { return desc_; }

    // Cached by readLatency() during prepare() and constant afterwards, so this
    // is a plain load with no synchronisation. See the note on kHostLatency for
    // why a plugin that changes its latency later is not followed.
    int latencyFrames() const override      { return latency_; }

    void setBypassed(bool b) override       { bypassed_ = b; }
    bool bypassed() const override          { return bypassed_; }

    void requestRestart()  { restartRequested_ = true; }
    void requestProcess()  { processRequested_ = true; }
    void requestCallback() { callbackRequested_ = true; }

private:
    struct ParamMsg { u32 index; f32 value; };
    // Raw MIDI as the engine hands it over: status plus up to two data bytes.
    struct MidiMsg { u8 data[3]; u8 len; u32 frame; };

    // Bounded because process() may not allocate. 256 gestures per block is far
    // more than a human or a UI redraw can produce, and the same goes for notes.
    static constexpr int kQueueSize     = 256;
    static constexpr int kMaxMidiEvents = 256;
    static constexpr int kMaxEvents     = kQueueSize + kMaxMidiEvents;
    // ~22 s at 48 kHz. A plugin claiming more than this is writing garbage, and
    // honouring it would make delay compensation reserve a buffer nobody asked
    // for. Matches the LV2 backend's ceiling so both formats lie the same way.
    static constexpr int kMaxLatencyFrames = 1 << 20;

    // in_events is one flat, time-ordered list, so every event type shares one
    // array. The members overlap on clap_event_header_t, which the spec
    // guarantees is the first field of every event struct.
    union Event {
        clap_event_header_t      header;
        clap_event_param_value_t param;
        clap_event_note_t        note;
        clap_event_midi_t        midi;
    };

    static void passthrough(const f32* const* in, f32* const* out, int channels, int nframes) {
        const size_t bytes = (size_t)nframes * sizeof(f32);
        for (int c = 0; c < channels; ++c) {
            if (!out[c]) continue;
            const f32* src = in ? in[c] : nullptr;
            if (src == out[c]) continue;              // in-place: already correct
            if (src) std::memcpy(out[c], src, bytes);
            else     std::memset(out[c], 0, bytes);
        }
    }

    // --- event lists handed to the plugin ---------------------------------
    static uint32_t CLAP_ABI evSize(const clap_input_events_t* list) {
        return ((const ClapInstance*)list->ctx)->eventCount_;
    }
    static const clap_event_header_t* CLAP_ABI evGet(const clap_input_events_t* list, uint32_t i) {
        const ClapInstance* self = (const ClapInstance*)list->ctx;
        if (i >= self->eventCount_) return nullptr;
        return &self->events_[i].header;   // union member; see Event
    }
    // We accept and drop: there is no param-feedback path to the GUI yet, and
    // returning false would make well-behaved plugins retry every block.
    static bool CLAP_ABI evTryPush(const clap_output_events_t*, const clap_event_header_t*) {
        return true;
    }

    // REALTIME. Builds this block's in_events list. Parameters go first at time
    // 0 and note traffic follows in arrival order, which keeps the list sorted
    // by time as the spec demands (a MIDI frame offset is never negative, and
    // out-of-order offsets from the caller are clamped rather than trusted).
    void buildEventList() {
        eventCount_ = 0;

        ParamMsg m;
        while (eventCount_ < (uint32_t)kMaxEvents && queue_.pop(m)) {
            if (m.index >= paramIds_.size()) continue;
            clap_event_param_value_t& e = events_[eventCount_++].param;
            e.header.size     = sizeof(clap_event_param_value_t);
            e.header.time     = 0;            // whole-block resolution for now
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type     = CLAP_EVENT_PARAM_VALUE;
            e.header.flags    = 0;
            e.param_id        = paramIds_[m.index];
            e.cookie          = cookies_[m.index];
            e.note_id         = -1;
            e.port_index      = -1;
            e.channel         = -1;
            e.key             = -1;
            e.value           = (double)m.value;
        }

        uint32_t lastFrame = 0;
        for (int i = 0; i < midiCount_ && eventCount_ < (uint32_t)kMaxEvents; ++i) {
            const MidiMsg& msg = midi_[i];
            if (msg.frame > lastFrame) lastFrame = msg.frame;
            emitMidi(msg, lastFrame);
        }
        midiCount_ = 0;
    }

    // REALTIME. Note on/off become CLAP note events when the plugin's note port
    // speaks that dialect, because they carry the full 0..1 velocity resolution
    // and a note_id; ports that only speak MIDI get the raw bytes. Everything
    // else (CC, pitch bend, aftertouch) has no CLAP equivalent we model, so it
    // always travels as CLAP_EVENT_MIDI.
    void emitMidi(const MidiMsg& msg, uint32_t frame) {
        const u8 status  = (u8)(msg.data[0] & 0xF0u);
        const int16_t ch = (int16_t)(msg.data[0] & 0x0Fu);
        const bool isNoteOn  = status == 0x90 && msg.len >= 3 && msg.data[2] > 0;
        const bool isNoteOff = status == 0x80 || (status == 0x90 && !isNoteOn);

        if (noteDialectClap_ && (isNoteOn || isNoteOff) && msg.len >= 2) {
            clap_event_note_t& e = events_[eventCount_++].note;
            e.header.size     = sizeof(clap_event_note_t);
            e.header.time     = frame;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type     = isNoteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
            e.header.flags    = 0;
            // -1 note_id means "match by port/channel/key", which is what a MIDI
            // source without note ids gives us.
            e.note_id    = -1;
            e.port_index = notePort_;
            e.channel    = ch;
            e.key        = (int16_t)(msg.data[1] & 0x7Fu);
            e.velocity   = isNoteOn ? (double)(msg.data[2] & 0x7Fu) / 127.0 : 0.0;
            return;
        }

        clap_event_midi_t& e = events_[eventCount_++].midi;
        e.header.size     = sizeof(clap_event_midi_t);
        e.header.time     = frame;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type     = CLAP_EVENT_MIDI;
        e.header.flags    = 0;
        e.port_index      = (uint16_t)(notePort_ < 0 ? 0 : notePort_);
        e.data[0]         = msg.data[0];
        e.data[1]         = msg.len > 1 ? msg.data[1] : 0;
        e.data[2]         = msg.len > 2 ? msg.data[2] : 0;
    }

    // clap.latency is documented as [main-thread & (being-activated | active)],
    // so it is asked exactly once: here, on the GUI thread, immediately after a
    // successful activate() and before processing starts. No extension (the
    // common case) means no latency, which is also what the spec says a silent
    // plugin is claiming.
    void readLatency() {
        latency_ = 0;
        const auto* ext = (const clap_plugin_latency_t*)plug_->get_extension(plug_, CLAP_EXT_LATENCY);
        if (!ext || !ext->get) return;
        const uint32_t frames = ext->get(plug_);
        // uint32_t means a negative value is impossible, but a plugin returning
        // something absurd is not, and the engine would have to reserve a delay
        // line for it. Same ceiling and same reasoning as the LV2 backend.
        if (frames > (uint32_t)kMaxLatencyFrames) {
            LOGW("clap: %s reports an implausible latency (%u frames), treating it as 0",
                 id_.c_str(), frames);
            return;
        }
        latency_ = (int)frames;
        if (latency_ > 0) LOGI("clap: %s reports %d frames of latency", id_.c_str(), latency_);
    }

    void teardown() {
        if (plug_) {
            if (processing_) plug_->stop_processing(plug_);
            if (active_)     plug_->deactivate(plug_);
            plug_->destroy(plug_);
            plug_ = nullptr;
        }
        processing_ = false;
        active_ = false;
        inBufs_.clear();  outBufs_.clear();
        inPtrs_.clear();  outPtrs_.clear();
        inStore_.clear(); outStore_.clear();
        inChans_ = outChans_ = 0;
        eventCount_ = 0;
        midiCount_ = 0;
        notePort_ = -1;
        noteDialectClap_ = false;
        latency_ = 0;
        steady_ = 0;
    }

    // Flatten every audio port into one channel list, exactly as the LV2
    // backend flattens ports, so the channel-mismatch rules in process() mean
    // the same thing in both backends.
    void buildPorts() {
        const auto* ap = (const clap_plugin_audio_ports_t*)plug_->get_extension(plug_, CLAP_EXT_AUDIO_PORTS);
        std::vector<uint32_t> inCounts, outCounts;
        if (ap && ap->count && ap->get) {
            const uint32_t nIn = ap->count(plug_, true), nOut = ap->count(plug_, false);
            clap_audio_port_info_t info{};
            for (uint32_t i = 0; i < nIn; ++i)
                if (ap->get(plug_, i, true, &info)) inCounts.push_back(info.channel_count);
            for (uint32_t i = 0; i < nOut; ++i)
                if (ap->get(plug_, i, false, &info)) outCounts.push_back(info.channel_count);
        }

        for (uint32_t c : inCounts)  inChans_  += (int)c;
        for (uint32_t c : outCounts) outChans_ += (int)c;

        const size_t frames = (size_t)maxBlock_;
        inStore_.assign((size_t)inChans_ * frames, 0.f);
        outStore_.assign((size_t)outChans_ * frames, 0.f);
        inPtrs_.resize((size_t)inChans_);
        outPtrs_.resize((size_t)outChans_);
        for (int i = 0; i < inChans_; ++i)  inPtrs_[(size_t)i]  = inStore_.data() + (size_t)i * frames;
        for (int i = 0; i < outChans_; ++i) outPtrs_[(size_t)i] = outStore_.data() + (size_t)i * frames;

        // The buffer structs point into the pointer vectors, which are never
        // resized after this, so process() can hand them over unchanged.
        int off = 0;
        inBufs_.resize(inCounts.size());
        for (size_t p = 0; p < inCounts.size(); ++p) {
            inBufs_[p] = clap_audio_buffer_t{};
            inBufs_[p].data32        = inCounts[p] ? &inPtrs_[(size_t)off] : nullptr;
            inBufs_[p].channel_count = inCounts[p];
            off += (int)inCounts[p];
        }
        off = 0;
        outBufs_.resize(outCounts.size());
        for (size_t p = 0; p < outCounts.size(); ++p) {
            outBufs_[p] = clap_audio_buffer_t{};
            outBufs_[p].data32        = outCounts[p] ? &outPtrs_[(size_t)off] : nullptr;
            outBufs_[p].channel_count = outCounts[p];
            off += (int)outCounts[p];
        }

        // The descriptor may have come from an older scan; the live instance is
        // authoritative.
        desc_.audioIn  = inChans_;
        desc_.audioOut = outChans_;

        buildNotePort();
    }

    // Which port do we send notes to, and in which dialect? Port 0 is the only
    // one we use: nothing upstream distinguishes note destinations yet.
    void buildNotePort() {
        notePort_ = -1;
        noteDialectClap_ = false;

        const auto* np = (const clap_plugin_note_ports_t*)plug_->get_extension(plug_, CLAP_EXT_NOTE_PORTS);
        if (!np || !np->count || !np->get || np->count(plug_, true) == 0) return;

        clap_note_port_info_t info{};
        if (!np->get(plug_, 0, true, &info)) return;
        notePort_ = 0;
        // preferred_dialect is advisory and some plugins leave it at 0, so the
        // supported mask is what actually decides.
        noteDialectClap_ = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
        desc_.hasMidiIn  = true;
    }

    void buildParams() {
        params_.clear();
        paramIds_.clear();
        cookies_.clear();
        values_.clear();

        const auto* pe = (const clap_plugin_params_t*)plug_->get_extension(plug_, CLAP_EXT_PARAMS);
        if (!pe || !pe->count || !pe->get_info) { desc_.paramCount = 0; return; }

        const uint32_t n = pe->count(plug_);
        params_.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            clap_param_info_t ci{};
            if (!pe->get_info(plug_, i, &ci)) continue;

            ParamInfo pi;
            pi.id   = ci.id;
            pi.name = ci.name;
            if (pi.name.empty()) pi.name = "param " + std::to_string(i);
            pi.min  = (f32)ci.min_value;
            pi.max  = (f32)ci.max_value;
            pi.def  = (f32)ci.default_value;

            // CLAP has no bool type: a stepped parameter over exactly 0..1 is
            // the idiomatic toggle. Anything else stepped is an int/enum.
            const bool stepped = (ci.flags & CLAP_PARAM_IS_STEPPED) != 0;
            pi.isBool = stepped && ci.min_value == 0.0 && ci.max_value == 1.0;
            pi.isInt  = stepped && !pi.isBool;
            // CLAP carries no scale hint at all; every value is plain and
            // linear by definition, so nothing is ever flagged logarithmic.
            pi.isLogarithmic = false;
            // ...and no unit metadata either. value_to_text() returns a
            // formatted string with the unit baked in, which is not what
            // ParamInfo::unit is for, so we leave it empty.

            if (!(pi.max > pi.min)) pi.max = pi.min + 1.f;   // degenerate metadata
            pi.def = clampv(pi.def, pi.min, pi.max);

            double v = ci.default_value;
            if (pe->get_value) pe->get_value(plug_, ci.id, &v);

            paramIds_.push_back(ci.id);
            cookies_.push_back(ci.cookie);
            values_.push_back(clampv((f32)v, pi.min, pi.max));
            params_.push_back(std::move(pi));
        }
        desc_.paramCount = (int)params_.size();
    }

    PluginDesc                   desc_;
    const clap_plugin_factory_t* factory_ = nullptr;
    std::string                  id_;
    const clap_plugin_t*         plug_ = nullptr;
    clap_host_t                  host_{};

    f64  sr_ = 48000.0;
    int  maxBlock_ = kMaxBlock;
    bool bypassed_ = false;
    bool active_ = false, processing_ = false;
    int  latency_ = 0;                 // frames, read once during prepare()
    i64  steady_ = 0;

    int inChans_ = 0, outChans_ = 0;
    std::vector<f32>  inStore_, outStore_;
    std::vector<f32*> inPtrs_, outPtrs_;
    std::vector<clap_audio_buffer_t> inBufs_, outBufs_;

    std::vector<ParamInfo> params_;
    std::vector<clap_id>   paramIds_;
    std::vector<void*>     cookies_;
    std::vector<f32>       values_;

    Ring<ParamMsg, kQueueSize> queue_;
    Event                      events_[kMaxEvents]{};
    uint32_t                   eventCount_ = 0;
    MidiMsg                    midi_[kMaxMidiEvents]{};
    int                        midiCount_ = 0;
    int16_t                    notePort_ = -1;      // -1 = plugin takes no notes
    bool                       noteDialectClap_ = false;
    clap_input_events_t        inEvents_{};
    clap_output_events_t       outEvents_{};

    std::atomic<bool> restartRequested_{false};
    std::atomic<bool> processRequested_{false};
    std::atomic<bool> callbackRequested_{false};
};

void CLAP_ABI hostRequestRestart(const clap_host_t* h) {
    if (h && h->host_data) ((ClapInstance*)h->host_data)->requestRestart();
}
void CLAP_ABI hostRequestProcess(const clap_host_t* h) {
    if (h && h->host_data) ((ClapInstance*)h->host_data)->requestProcess();
}
void CLAP_ABI hostRequestCallback(const clap_host_t* h) {
    if (h && h->host_data) ((ClapInstance*)h->host_data)->requestCallback();
}

// The CLAP descriptor carries no port or parameter counts, so the only way to
// fill those columns in the browser is to build the plugin, ask, and throw it
// away. create_plugin()+init() is cheap for well-behaved plugins and a failure
// here only costs us the counts, never the entry itself.
void probeCounts(const clap_plugin_factory_t* factory, PluginDesc& d, const char* id) {
    const clap_plugin_t* p = factory->create_plugin(factory, &kProbeHost, id);
    if (!p) return;
    if (!p->init || !p->init(p)) { if (p->destroy) p->destroy(p); return; }

    if (const auto* ap = (const clap_plugin_audio_ports_t*)p->get_extension(p, CLAP_EXT_AUDIO_PORTS)) {
        if (ap->count && ap->get) {
            clap_audio_port_info_t info{};
            const uint32_t nIn = ap->count(p, true), nOut = ap->count(p, false);
            for (uint32_t i = 0; i < nIn; ++i)
                if (ap->get(p, i, true, &info)) d.audioIn += (int)info.channel_count;
            for (uint32_t i = 0; i < nOut; ++i)
                if (ap->get(p, i, false, &info)) d.audioOut += (int)info.channel_count;
        }
    }
    if (const auto* np = (const clap_plugin_note_ports_t*)p->get_extension(p, CLAP_EXT_NOTE_PORTS)) {
        if (np->count && np->count(p, true) > 0) d.hasMidiIn = true;
    }
    if (const auto* pe = (const clap_plugin_params_t*)p->get_extension(p, CLAP_EXT_PARAMS)) {
        if (pe->count) d.paramCount = (int)pe->count(p);
    }

    p->destroy(p);
}

void scanFile(const std::string& path, std::vector<PluginDesc>& out, int& found, int& skipped) {
    const Entry* e = openEntry(path);
    if (!e) { ++skipped; return; }

    const uint32_t n = e->factory->get_plugin_count(e->factory);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_plugin_descriptor_t* cd = e->factory->get_plugin_descriptor(e->factory, i);
        if (!cd || !cd->id || !*cd->id) {
            LOGW("clap: %s plugin %u has no id", path.c_str(), i);
            ++skipped;
            continue;
        }
        if (!clap_version_is_compatible(cd->clap_version)) {
            LOGW("clap: skipping %s (descriptor v%u.%u.%u)", cd->id,
                 cd->clap_version.major, cd->clap_version.minor, cd->clap_version.revision);
            ++skipped;
            continue;
        }

        PluginDesc d;
        d.format   = PluginFormat::CLAP;
        d.uri      = makeUri(path, i);
        d.name     = safeStr(cd->name);
        if (d.name.empty()) d.name = cd->id;
        d.vendor   = safeStr(cd->vendor);
        d.category = categoryOf(cd);

        const bool instrument = hasFeature(cd, CLAP_PLUGIN_FEATURE_INSTRUMENT);
        const bool noteFx     = hasFeature(cd, CLAP_PLUGIN_FEATURE_NOTE_EFFECT);
        if (instrument)                                     d.kind = PluginKind::Instrument;
        else if (hasFeature(cd, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT)) d.kind = PluginKind::Effect;
        // A note effect makes no audio, and PluginKind has no MIDI-effect
        // member, so Unknown is the honest answer rather than mislabelling it.
        else                                                d.kind = PluginKind::Unknown;
        if (instrument || noteFx) d.hasMidiIn = true;

        probeCounts(e->factory, d, cd->id);

        // Features are advisory; fall back to the port layout the way the LV2
        // scanner falls back to its port counts.
        if (d.kind == PluginKind::Unknown && !noteFx) {
            if (d.hasMidiIn && d.audioIn == 0 && d.audioOut > 0) d.kind = PluginKind::Instrument;
            else if (d.audioIn > 0 || d.audioOut > 0)            d.kind = PluginKind::Effect;
        }

        out.push_back(std::move(d));
        ++found;
    }
}

} // namespace

// --- scan ------------------------------------------------------------------
void scanCLAP(std::vector<PluginDesc>& out) {
    rememberMainThread();

    std::vector<std::string> files;
    for (const std::string& dir : searchPaths()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        walkForClap(dir, files);
    }

    int found = 0, skipped = 0;
    for (const std::string& f : files) scanFile(f, out, found, skipped);
    LOGI("clap: %d usable in %zu file(s), %d skipped", found, files.size(), skipped);
}

std::unique_ptr<PluginInstance> instantiateCLAP(const PluginDesc& d, f64 sampleRate, int maxBlock) {
    rememberMainThread();

    std::string path;
    uint32_t index = 0;
    if (!splitUri(d.uri, path, index)) {
        LOGE("clap: malformed uri %s", d.uri.c_str());
        return nullptr;
    }

    const Entry* e = openEntry(path);
    if (!e) {
        LOGE("clap: %s not loadable (rescan needed?)", path.c_str());
        return nullptr;
    }
    if (index >= e->factory->get_plugin_count(e->factory)) {
        LOGE("clap: %s has no plugin %u (rescan needed?)", path.c_str(), index);
        return nullptr;
    }
    const clap_plugin_descriptor_t* cd = e->factory->get_plugin_descriptor(e->factory, index);
    if (!cd || !cd->id) {
        LOGE("clap: %s plugin %u has no descriptor", path.c_str(), index);
        return nullptr;
    }

    auto inst = std::make_unique<ClapInstance>(d, e->factory, std::string(cd->id));
    if (!inst->prepare(sampleRate, maxBlock)) return nullptr;
    return inst;
}

} // namespace detail
} // namespace lat
