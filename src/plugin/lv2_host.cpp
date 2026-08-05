// LV2 backend, built on lilv.
//
// Scope of what we support today:
//   * audio in/out ports, control input/output ports
//   * atom ports are connected to owned scratch buffers. MIDI arriving through
//     midi() is forged into a real atom:Sequence on every atom input port that
//     declares midi:MidiEvent support, so instruments actually play. Atom
//     *outputs* are still discarded: nothing downstream consumes notes yet.
//   * the latency report: a control output designated lv2:latency (or carrying
//     the older lv2:reportsLatency property) is read once, after activation and
//     one silent block, and served from latencyFrames(). See settleLatency().
//   * features: urid:map, urid:unmap, options:options, log:log,
//     buf-size:boundedBlockLength. A plugin whose *required* feature list
//     contains anything else is logged and skipped rather than loaded and
//     crashed (worker:schedule and state:loadDefaultState are the common ones).
//
// Realtime rules: process() only memcpy/memsets into buffers allocated in
// prepare(), then calls lilv_instance_run(). Nothing here allocates, locks or
// throws once prepare() has returned.
//
// Known third-party hazard: Calf corrupts process-wide state when one of its
// instances is torn down, so freeing several Calf plugins in a row and then
// running another one can crash inside Calf. This reproduces in a plain lilv
// program with none of our code in the picture. pinPluginLibrary() below
// removes the dlclose half of the problem; the rest is Calf's.
#include "host.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/port-props/port-props.h>
#include <lv2/units/units.h>
#include <lv2/urid/urid.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace lat {
namespace detail {
namespace {

// --- URID map --------------------------------------------------------------
// Shared by every instance so URIDs stay comparable across plugins, which the
// spec requires.
//
// RT-AUDIT §1.4: the old map() took a std::mutex and allocated (std::string,
// unordered_map/deque nodes) unconditionally — and that mutex is shared with the
// plugin *scanner* running on another thread, so an audio thread mapping a URID
// from run() could block on a scan of every bundle on the system: unbounded
// priority inversion.
//
// Fix: the read path is now lock-free. An open-addressed table (published under
// the mutex off the RT thread, read with acquire loads and no lock) answers
// every already-mapped lookup — which is what run()-time maps almost always are,
// since a well-behaved plugin maps each URID it needs during instantiate. A
// genuine miss on the audio thread (tlsLv2InRun) neither locks nor allocates: it
// returns 0 ("no URID"), a legal answer strictly better than blocking, and bumps
// rtMisses so the rare offender is visible. Writes (instantiate + scan) still
// serialise on the mutex and are the only place the heap is touched.
//
// Set true only around lilv_instance_run() on the audio thread (see process()).
thread_local bool tlsLv2InRun = false;

struct UridStore {
    std::mutex mtx;
    std::deque<std::string> uris;                     // deque: element addresses are stable
    std::unordered_map<std::string, LV2_URID> index;

    // Lock-free read cache. Keys are interned pointers from `uris` (stable for
    // the process lifetime, never freed), published with release and read with
    // acquire. Power-of-two for masking; 16k slots is ample for any plugin set.
    static constexpr size_t kCap = 1u << 14;
    struct Slot { std::atomic<const char*> key{nullptr}; LV2_URID id = 0; };
    Slot table[kCap];
    std::atomic<u64> rtMisses{0};

    static size_t hashStr(const char* s) {            // FNV-1a
        size_t h = 1469598103934665603ull;
        for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
        return h;
    }
    LV2_URID lookup(const char* uri) const {
        const size_t h = hashStr(uri) & (kCap - 1);
        for (size_t probe = 0; probe < kCap; ++probe) {
            const size_t i = (h + probe) & (kCap - 1);
            const char* k = table[i].key.load(std::memory_order_acquire);
            if (!k) return 0;                          // empty slot: not present
            if (std::strcmp(k, uri) == 0) return table[i].id;
        }
        return 0;
    }
    void publish(const char* internedKey, LV2_URID id) {   // caller holds mtx
        const size_t h = hashStr(internedKey) & (kCap - 1);
        for (size_t probe = 0; probe < kCap; ++probe) {
            const size_t i = (h + probe) & (kCap - 1);
            if (table[i].key.load(std::memory_order_relaxed) == nullptr) {
                table[i].id = id;                      // id visible before key,
                table[i].key.store(internedKey, std::memory_order_release);  // via release
                return;
            }
        }
    }

    LV2_URID map(const char* uri) {
        if (LV2_URID id = lookup(uri)) return id;      // fast path: lock-free
        if (tlsLv2InRun) {                             // RT miss: never lock/alloc
            rtMisses.fetch_add(1, std::memory_order_relaxed);
            return 0;                                  // legal "no URID"
        }
        std::lock_guard<std::mutex> lk(mtx);
        auto it = index.find(uri);                     // recheck under the lock
        if (it != index.end()) return it->second;
        uris.emplace_back(uri);
        const LV2_URID id = (LV2_URID)uris.size();     // 0 is reserved as "no URID"
        index.emplace(uris.back(), id);
        publish(uris.back().c_str(), id);
        return id;
    }
    const char* unmap(LV2_URID id) {
        // Resolving a URID back to its string is a main-thread operation in
        // practice; keep it simple under the same lock.
        std::lock_guard<std::mutex> lk(mtx);
        if (id == 0 || id > uris.size()) return nullptr;
        return uris[id - 1].c_str();
    }
};

UridStore& urids() { static UridStore s; return s; }

LV2_URID uridMapFn(LV2_URID_Map_Handle, const char* uri)      { return urids().map(uri); }
const char* uridUnmapFn(LV2_URID_Unmap_Handle, LV2_URID id)   { return urids().unmap(id); }

int logVprintfFn(LV2_Log_Handle, LV2_URID, const char* fmt, va_list ap) {
    char msg[1024];
    const int n = vsnprintf(msg, sizeof msg, fmt, ap);
    // Plugins tend to append their own newline; strip it so our log stays tidy.
    size_t len = strlen(msg);
    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) msg[--len] = 0;
    if (len) LOGI("lv2: %s", msg);
    return n;
}
int logPrintfFn(LV2_Log_Handle h, LV2_URID type, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = logVprintfFn(h, type, fmt, ap);
    va_end(ap);
    return n;
}

// --- world -----------------------------------------------------------------
// lilv_world_load_all() walks every bundle on the system and takes on the
// order of a second, so the world is created once and kept for the process
// lifetime. It is intentionally never freed: LilvPlugin pointers held by live
// instances must outlive any static destruction order we could arrange.
struct World {
    LilvWorld* world = nullptr;
    const LilvPlugins* plugins = nullptr;

    LilvNode *audioPort = nullptr, *controlPort = nullptr, *cvPort = nullptr;
    LilvNode *atomPort = nullptr, *eventPort = nullptr;
    LilvNode *inputPort = nullptr, *outputPort = nullptr;
    LilvNode *midiEvent = nullptr, *instrument = nullptr;
    LilvNode *toggled = nullptr, *integer = nullptr, *enumeration = nullptr;
    LilvNode *sampleRateProp = nullptr, *logarithmic = nullptr;
    LilvNode *unitsUnit = nullptr, *unitsSymbol = nullptr;
    LilvNode *reportsLatency = nullptr, *designation = nullptr, *latency = nullptr;

    void init() {
        world = lilv_world_new();
        lilv_world_load_all(world);
        plugins = lilv_world_get_all_plugins(world);

        auto n = [this](const char* uri) { return lilv_new_uri(world, uri); };
        audioPort      = n(LILV_URI_AUDIO_PORT);
        controlPort    = n(LILV_URI_CONTROL_PORT);
        cvPort         = n(LILV_URI_CV_PORT);
        atomPort       = n(LILV_URI_ATOM_PORT);
        eventPort      = n(LILV_URI_EVENT_PORT);
        inputPort      = n(LILV_URI_INPUT_PORT);
        outputPort     = n(LILV_URI_OUTPUT_PORT);
        midiEvent      = n(LILV_URI_MIDI_EVENT);
        instrument     = n(LV2_CORE__InstrumentPlugin);
        toggled        = n(LV2_CORE__toggled);
        integer        = n(LV2_CORE__integer);
        enumeration    = n(LV2_CORE__enumeration);
        sampleRateProp = n(LV2_CORE__sampleRate);
        logarithmic    = n(LV2_PORT_PROPS__logarithmic);
        unitsUnit      = n(LV2_UNITS__unit);
        unitsSymbol    = n(LV2_UNITS__symbol);
        // Latency reporting. Two spellings are in the wild: the old
        // lv2:portProperty lv2:reportsLatency, and lv2:designation lv2:latency
        // (which the spec now prefers). Both are looked for; see latencyPortOf.
        reportsLatency = n(LV2_CORE__reportsLatency);
        designation    = n(LV2_CORE__designation);
        latency        = n(LV2_CORE__latency);
    }
};

World& world() {
    static World w;
    static std::once_flag once;
    std::call_once(once, [] { w.init(); });
    return w;
}

// Features we can actually honour. Anything else in a plugin's *required*
// list means we cannot host it correctly, so we skip it at scan time.
bool featureSupported(const char* uri) {
    static const char* kOk[] = {
        LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options,
        LV2_LOG__log, LV2_BUF_SIZE__boundedBlockLength,
    };
    for (const char* s : kOk)
        if (strcmp(s, uri) == 0) return true;
    return false;
}

// Returns the URI of the first unsupported required feature, or empty.
std::string unsupportedRequiredFeature(const LilvPlugin* p) {
    std::string bad;
    LilvNodes* feats = lilv_plugin_get_required_features(p);
    if (feats) {
        LILV_FOREACH (nodes, i, feats) {
            const LilvNode* f = lilv_nodes_get(feats, i);
            const char* uri = lilv_node_is_uri(f) ? lilv_node_as_uri(f) : nullptr;
            if (!uri) { bad = "<non-uri feature>"; break; }
            if (!featureSupported(uri)) { bad = uri; break; }
        }
        lilv_nodes_free(feats);
    }
    return bad;
}

std::string nodeString(const LilvNode* n) {
    return (n && lilv_node_is_string(n)) ? std::string(lilv_node_as_string(n)) : std::string();
}

// lilv_instance_free() dlclose()s the plugin binary. That is fatal for any
// plugin whose dependency chain owns live threads: Calf pulls in libgomp, whose
// worker pool never exits, so unmapping the library leaves those threads
// executing freed pages and the process dies inside dlclose. Pinning the
// library with RTLD_NODELETE before instantiation keeps the mapping alive for
// the process lifetime; the refcount lilv drops is simply no longer the last
// one. Cost is bounded — one mapping per distinct plugin binary ever loaded.
void pinPluginLibrary(const LilvPlugin* p) {
    const LilvNode* lib = lilv_plugin_get_library_uri(p);
    if (!lib || !lilv_node_is_uri(lib)) return;
    char* path = lilv_file_uri_parse(lilv_node_as_uri(lib), nullptr);
    if (!path) return;

    static std::mutex mtx;
    static std::unordered_map<std::string, void*> pinned;
    std::lock_guard<std::mutex> lk(mtx);
    if (pinned.find(path) == pinned.end())
        pinned.emplace(path, dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE));
    lilv_free(path);
}

// Port ranges are routinely written as bare integers in Turtle ("lv2:maximum 15"),
// which lilv reports as xsd:integer, not float. Accept both or half the ranges
// on the system silently fall back to 0..1.
bool nodeFloat(const LilvNode* n, f32& out) {
    if (!n) return false;
    if (lilv_node_is_float(n) || lilv_node_is_int(n)) { out = (f32)lilv_node_as_float(n); return true; }
    if (lilv_node_is_bool(n)) { out = lilv_node_as_bool(n) ? 1.f : 0.f; return true; }
    return false;
}

// The index of the control-output port a plugin reports its latency on, or -1.
//
// LV2 has two spellings for this and both are still in the wild: the original
// lv2:portProperty lv2:reportsLatency, and lv2:designation lv2:latency, which
// the spec now prefers. Both are accepted here. lilv's own answer is consulted
// last so a bundle using a spelling we have not thought of still works, and it
// is range-checked because lilv documents get_latency_port_index() as
// meaningful only when has_latency() is true.
//
// Only control *outputs* qualify. An input port carrying the same property
// would be a host-to-plugin channel and writing a latency into it is not our
// job; treating one as a report would also mean reading back whatever default
// we had just put there.
int latencyPortOf(const LilvPlugin* p) {
    World& w = world();
    const uint32_t n = lilv_plugin_get_num_ports(p);
    for (uint32_t i = 0; i < n; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(p, i);
        if (!lilv_port_is_a(p, port, w.controlPort) || !lilv_port_is_a(p, port, w.outputPort))
            continue;
        if (lilv_port_has_property(p, port, w.reportsLatency)) return (int)i;
        LilvNodes* des = lilv_port_get_value(p, port, w.designation);
        bool isLatency = false;
        if (des) {
            LILV_FOREACH (nodes, it, des) {
                const LilvNode* d = lilv_nodes_get(des, it);
                if (d && lilv_node_equals(d, w.latency)) { isLatency = true; break; }
            }
            lilv_nodes_free(des);
        }
        if (isLatency) return (int)i;
    }
    if (lilv_plugin_has_latency(p)) {
        const uint32_t i = lilv_plugin_get_latency_port_index(p);
        if (i < n) return (int)i;
    }
    return -1;
}

// Where each LV2 port index points once we have connected it.
enum class PortKind : u8 { Ignored, ControlIn, ControlOut, AudioIn, AudioOut, Cv, AtomIn, AtomOut };
struct PortSlot {
    PortKind kind = PortKind::Ignored;
    int      slot = 0;      // index into the buffer vector for that kind
};

// --- instance --------------------------------------------------------------
class Lv2Instance final : public PluginInstance {
public:
    Lv2Instance(const PluginDesc& d, const LilvPlugin* p) : desc_(d), plug_(p) {}

    ~Lv2Instance() override { teardown(); }

    bool prepare(f64 sampleRate, int maxBlock) override {
        teardown();
        if (maxBlock <= 0) maxBlock = kMaxBlock;
        sr_ = sampleRate;
        maxBlock_ = maxBlock;

        buildPorts();
        buildParams();

        // Options must outlive the instance: some plugins keep the pointer.
        const LV2_URID uInt   = urids().map(LV2_ATOM__Int);
        const LV2_URID uFloat = urids().map(LV2_ATOM__Float);
        blockOpt_ = (i32)maxBlock_;
        minBlockOpt_ = 1;
        seqOpt_ = (i32)kAtomBufBytes;
        srOpt_ = (f32)sr_;
        options_ = {
            { LV2_OPTIONS_INSTANCE, 0, urids().map(LV2_BUF_SIZE__maxBlockLength),
              sizeof(i32), uInt, &blockOpt_ },
            { LV2_OPTIONS_INSTANCE, 0, urids().map(LV2_BUF_SIZE__minBlockLength),
              sizeof(i32), uInt, &minBlockOpt_ },
            { LV2_OPTIONS_INSTANCE, 0, urids().map(LV2_BUF_SIZE__sequenceSize),
              sizeof(i32), uInt, &seqOpt_ },
            { LV2_OPTIONS_INSTANCE, 0, urids().map(LV2_PARAMETERS__sampleRate),
              sizeof(f32), uFloat, &srOpt_ },
            { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr },
        };

        map_   = { nullptr, uridMapFn };
        unmap_ = { nullptr, uridUnmapFn };
        log_   = { nullptr, logPrintfFn, logVprintfFn };

        fMap_     = { LV2_URID__map, &map_ };
        fUnmap_   = { LV2_URID__unmap, &unmap_ };
        fLog_     = { LV2_LOG__log, &log_ };
        fOptions_ = { LV2_OPTIONS__options, options_.data() };
        fBounded_ = { LV2_BUF_SIZE__boundedBlockLength, nullptr };
        // log:log is deliberately NOT advertised (RT-AUDIT §1.3): our log sink is
        // fprintf(stderr) — an unbuffered flockfile()+write(2) syscall — and the
        // LV2 log extension carries no thread restriction, so a plugin calling it
        // from run() (parameter-clamp / denormal / buffer-size gripes fire from
        // exactly there) would do a blocking syscall on the audio thread. log:log
        // is an *optional* feature (never listed as required by any conformant
        // plugin), so withholding it is legal; we drop plugin log messages, which
        // is acceptable. The feature struct (fLog_/log_) is kept only so its
        // callbacks stay available if a non-RT diagnostic build wants them.
        (void)fLog_;
        const LV2_Feature* feats[] = { &fMap_, &fUnmap_, &fOptions_, &fBounded_, nullptr };

        pinPluginLibrary(plug_);
        inst_ = lilv_plugin_instantiate(plug_, sr_, feats);
        if (!inst_) {
            LOGE("lv2: instantiate failed for %s", desc_.uri.c_str());
            return false;
        }

        connectPorts();
        lilv_instance_activate(inst_);
        settleLatency();
        return true;
    }

    // REALTIME. See the class comment: memcpy + run, nothing else.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;

        // Bypass, a dead instance, or an oversized block all degrade to a
        // straight copy. Growing buffers here would mean allocating on the
        // audio thread, so we refuse instead.
        if (bypassed_ || !inst_ || nframes > maxBlock_) {
            midiCount_ = 0;                     // events belong to this block only
            passthrough(in, out, channels, nframes);
            return;
        }

        const int nIn  = (int)audioIn_.size();
        const int nOut = (int)audioOut_.size();
        if (nOut == 0) { midiCount_ = 0; passthrough(in, out, channels, nframes); return; }

        const size_t bytes = (size_t)nframes * sizeof(f32);

        // Feed the plugin's inputs. A plugin input with no matching track
        // channel gets channel 0 duplicated (stereo plugin on a mono source);
        // with no input at all it gets silence (instruments).
        for (int p = 0; p < nIn; ++p) {
            const f32* src = nullptr;
            if (in) src = (p < channels) ? in[p] : in[0];
            if (src) std::memcpy(audioIn_[p], src, bytes);
            else     std::memset(audioIn_[p], 0, bytes);
        }

        resetAtomBuffers();
        forgeMidi();
        // Mark the audio thread so a urid:map miss from inside run() returns 0
        // instead of locking/allocating (see UridStore::map, RT-AUDIT §1.4).
        tlsLv2InRun = true;
        lilv_instance_run(inst_, (uint32_t)nframes);
        tlsLv2InRun = false;
        // The plugin has consumed this block's events. Emptying the sequences
        // now (rather than only before the next run) means a plugin that peeks
        // at its input port outside run() never sees stale notes, and a missed
        // process() call cannot replay them.
        midiCount_ = 0;
        resetAtomBuffers();

        // Pull the outputs back. A mono plugin (1 out) on a stereo track is
        // run once on channel 0 and its output is copied to both channels;
        // this is the documented behaviour, not a second instance.
        for (int c = 0; c < channels; ++c) {
            if (!out[c]) continue;
            const f32* src = audioOut_[c < nOut ? c : 0];
            std::memcpy(out[c], src, bytes);
        }
    }

    // REALTIME. Queued into a fixed array; forgeMidi() turns it into an atom
    // sequence just before run(). midi() and process() are both audio-thread
    // only and midi() is documented to run first for the block, so a plain
    // array plus a count needs no synchronisation.
    void midi(const u8* data, int len, int frameOffset) override {
        if (!data || len < 1 || len > (int)sizeof(MidiMsg::data)) return;
        if (midiCount_ >= kMaxMidiEvents) return;      // dropped, not grown
        MidiMsg& m = midi_[midiCount_++];
        m.len = (u8)len;
        for (int i = 0; i < len; ++i) m.data[i] = data[i];
        m.frame = frameOffset < 0 ? 0 : (u32)frameOffset;
    }

    int              paramCount() const override        { return (int)params_.size(); }
    const ParamInfo& paramInfo(int i) const override    { return params_[(size_t)i]; }

    f32 getParam(int i) const override {
        if (i < 0 || i >= (int)paramPort_.size()) return 0.f;
        return ctrl_[(size_t)paramPort_[(size_t)i]];
    }

    // GUI thread, concurrent with process(). The plugin reads this float once
    // per run(); a plain store is atomic in practice for a 4-byte aligned
    // float on every target we build for, and the worst case is that the
    // change lands one block later. No lock, so the audio thread never waits.
    void setParam(int i, f32 v) override {
        if (i < 0 || i >= (int)paramPort_.size()) return;
        const ParamInfo& pi = params_[(size_t)i];
        ctrl_[(size_t)paramPort_[(size_t)i]] = clampv(v, pi.min, pi.max);
    }

    const PluginDesc& desc() const override { return desc_; }

    // Filled in by settleLatency() at the end of prepare() and constant
    // afterwards, so this is a plain load with no synchronisation, exactly as
    // the contract in host.h asks for.
    int latencyFrames() const override      { return latency_; }

    void setBypassed(bool b) override       { bypassed_ = b; }
    bool bypassed() const override          { return bypassed_; }

private:
    static constexpr size_t kAtomBufBytes = 8192;
    // Sanity ceiling on what a plugin may claim, ~22 s at 48 kHz. Real latency
    // is milliseconds (lookahead) to a few hundred ms (long convolution); a
    // number past this is a plugin writing garbage into its port, and honouring
    // it would make the engine's delay compensation reserve a buffer nobody
    // asked for. Rejected values become 0 and are logged, which is the same
    // answer we would have given before the plugin said anything.
    static constexpr int kMaxLatencyFrames = 1 << 20;
    // One block's worth of note traffic. A human plus an arpeggiator cannot
    // produce 256 messages in 5 ms; anything beyond that is a runaway sender and
    // dropping is better than allocating on the audio thread.
    static constexpr int kMaxMidiEvents = 256;

    // Raw MIDI as the engine hands it over: status plus up to two data bytes.
    struct MidiMsg {
        u8  data[3];
        u8  len;
        u32 frame;
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

    void teardown() {
        if (inst_) {
            lilv_instance_deactivate(inst_);
            lilv_instance_free(inst_);
            inst_ = nullptr;
        }
        slots_.clear();
        latencyPort_ = -1;
        latency_ = 0;
        audioIn_.clear();
        audioOut_.clear();
        atomIn_.clear();
        atomOut_.clear();
        midiPorts_.clear();
        midiCount_ = 0;
        audioStore_.clear();
        cvStore_.clear();
        atomStore_.clear();
    }

    // Classify every port and size the buffers. Buffers are allocated once and
    // never resized afterwards, so the pointers we hand to lilv stay valid.
    void buildPorts() {
        World& w = world();
        const uint32_t n = lilv_plugin_get_num_ports(plug_);
        slots_.assign(n, PortSlot{});
        ctrl_.assign(n, 0.f);
        midiPorts_.clear();
        midiCount_ = 0;

        int nAudioIn = 0, nAudioOut = 0, nCv = 0, nAtomIn = 0, nAtomOut = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plug_, i);
            const bool isIn  = lilv_port_is_a(plug_, port, w.inputPort);
            const bool isOut = lilv_port_is_a(plug_, port, w.outputPort);
            PortSlot s;
            if (lilv_port_is_a(plug_, port, w.audioPort)) {
                s.kind = isIn ? PortKind::AudioIn : PortKind::AudioOut;
                s.slot = isIn ? nAudioIn++ : nAudioOut++;
            } else if (lilv_port_is_a(plug_, port, w.controlPort)) {
                s.kind = isIn ? PortKind::ControlIn : PortKind::ControlOut;
                s.slot = (int)i;
            } else if (lilv_port_is_a(plug_, port, w.cvPort)) {
                s.kind = PortKind::Cv;
                s.slot = nCv++;
            } else if (lilv_port_is_a(plug_, port, w.atomPort) ||
                       lilv_port_is_a(plug_, port, w.eventPort)) {
                s.kind = isIn ? PortKind::AtomIn : PortKind::AtomOut;
                s.slot = isIn ? nAtomIn++ : nAtomOut++;
                if (isIn && lilv_port_supports_event(plug_, port, w.midiEvent))
                    midiPorts_.push_back(s.slot);
            } else {
                // Unknown port type. Still needs a non-null connection or the
                // plugin may dereference it, so park it on a CV-sized buffer.
                s.kind = PortKind::Cv;
                s.slot = nCv++;
                (void)isOut;
            }
            slots_[i] = s;
        }

        const size_t frames = (size_t)maxBlock_;
        audioStore_.assign((size_t)(nAudioIn + nAudioOut) * frames, 0.f);
        cvStore_.assign((size_t)nCv * frames, 0.f);
        atomStore_.assign((size_t)(nAtomIn + nAtomOut) * kAtomBufBytes, 0);

        audioIn_.resize((size_t)nAudioIn);
        audioOut_.resize((size_t)nAudioOut);
        for (int i = 0; i < nAudioIn; ++i)  audioIn_[(size_t)i]  = audioStore_.data() + (size_t)i * frames;
        for (int i = 0; i < nAudioOut; ++i) audioOut_[(size_t)i] = audioStore_.data() + (size_t)(nAudioIn + i) * frames;
        atomIn_.resize((size_t)nAtomIn);
        atomOut_.resize((size_t)nAtomOut);
        for (int i = 0; i < nAtomIn; ++i)
            atomIn_[(size_t)i] = atomStore_.data() + (size_t)i * kAtomBufBytes;
        for (int i = 0; i < nAtomOut; ++i)
            atomOut_[(size_t)i] = atomStore_.data() + (size_t)(nAtomIn + i) * kAtomBufBytes;

        // Mapped here, at instantiation time, because forgeMidi() runs on the
        // audio thread and the URID store takes a mutex.
        seqUrid_   = urids().map(LV2_ATOM__Sequence);
        chunkUrid_ = urids().map(LV2_ATOM__Chunk);
        midiUrid_  = urids().map(LV2_MIDI__MidiEvent);

        // A few plugins declare a single atom input without spelling out
        // midi:MidiEvent support in their Turtle. If that is the only atom input
        // there is, it is the MIDI port by elimination; guessing beats being
        // silent, and a plugin that really wanted patch messages only will
        // ignore an event type it does not know.
        if (midiPorts_.empty() && nAtomIn == 1 && desc_.hasMidiIn)
            midiPorts_.push_back(0);
        // Which port (if any) the plugin reports its latency on. Only a port we
        // actually classified as a control output counts: the fallback inside
        // latencyPortOf() can name a port we chose to treat differently, and
        // reading a float out of, say, an atom buffer would be nonsense.
        latencyPort_ = latencyPortOf(plug_);
        if (latencyPort_ >= 0 &&
            (size_t)latencyPort_ < slots_.size() &&
            slots_[(size_t)latencyPort_].kind != PortKind::ControlOut)
            latencyPort_ = -1;

        // The descriptor may have been produced by an older scan; keep the
        // authoritative counts from the actual instance.
        desc_.audioIn  = nAudioIn;
        desc_.audioOut = nAudioOut;
    }

    // Reads the plugin's reported latency once, at the end of prepare().
    //
    // The value lives in the control-out buffer we connected, and a plugin only
    // ever writes it from run(), so before the first run the port still holds
    // the 0 buildParams() put there. One silent block at the prepared block
    // size settles it -- which is exactly what "pre-roll to compute latency"
    // means in the LV2 spec's own words -- and after that latencyFrames() is
    // the constant the engine's delay compensation needs.
    //
    // Deliberate limitation: a plugin that varies its latency at run time (an
    // oversampling toggle, a lookahead knob) is recorded at its prepare-time
    // value and never re-read. Following it would mean republishing chains and
    // re-aligning every parallel path from the audio thread; the contract in
    // host.h says constant-after-prepare, so the prepare-time value is the
    // honest answer and a plugin that moves is a plugin that is misaligned
    // until the chain is rebuilt.
    //
    // Nothing runs at all when there is no latency port: the overwhelming
    // majority of plugins have none, and they must not pay a block of DSP (or
    // have their state advanced by one block) for a question they never answer.
    void settleLatency() {
        latency_ = 0;
        if (!inst_ || latencyPort_ < 0) return;

        // Silence: the audio buffers were zeroed when they were allocated and
        // connectPorts() left every atom input as a valid empty sequence.
        lilv_instance_run(inst_, (uint32_t)maxBlock_);
        resetAtomBuffers();

        const f32 v = ctrl_[(size_t)latencyPort_];
        // The negated comparison also catches NaN, which some plugins leave in
        // an output port they never got round to writing.
        if (!(v >= 0.f) || v > (f32)kMaxLatencyFrames) {
            LOGW("lv2: %s reports an implausible latency (%.3f frames), treating it as 0",
                 desc_.uri.c_str(), (double)v);
            return;
        }
        latency_ = (int)(v + 0.5f);
        if (latency_ > 0)
            LOGI("lv2: %s reports %d frames of latency", desc_.uri.c_str(), latency_);
    }

    // Control input ports become ParamInfo entries, in port order.
    void buildParams() {
        World& w = world();
        params_.clear();
        paramPort_.clear();
        for (uint32_t i = 0; i < (uint32_t)slots_.size(); ++i) {
            if (slots_[i].kind != PortKind::ControlIn) {
                if (slots_[i].kind == PortKind::ControlOut) ctrl_[i] = 0.f;
                continue;
            }
            const LilvPort* port = lilv_plugin_get_port_by_index(plug_, i);

            ParamInfo pi;
            pi.id = i;
            LilvNode* nm = lilv_port_get_name(plug_, port);
            pi.name = nodeString(nm);
            lilv_node_free(nm);
            if (pi.name.empty()) pi.name = nodeString(lilv_port_get_symbol(plug_, port));

            LilvNode *dn = nullptr, *mn = nullptr, *xn = nullptr;
            lilv_port_get_range(plug_, port, &dn, &mn, &xn);
            nodeFloat(mn, pi.min);
            nodeFloat(xn, pi.max);
            if (!nodeFloat(dn, pi.def)) pi.def = pi.min;
            lilv_node_free(dn); lilv_node_free(mn); lilv_node_free(xn);

            pi.isBool = lilv_port_has_property(plug_, port, w.toggled);
            pi.isInt  = lilv_port_has_property(plug_, port, w.integer) ||
                        lilv_port_has_property(plug_, port, w.enumeration);
            pi.isLogarithmic = lilv_port_has_property(plug_, port, w.logarithmic);

            // lv2:sampleRate on a port means its range is expressed as a
            // fraction of the sample rate, so scale it now that we know sr_.
            if (lilv_port_has_property(plug_, port, w.sampleRateProp)) {
                pi.min *= (f32)sr_; pi.max *= (f32)sr_; pi.def *= (f32)sr_;
            }
            if (pi.isBool) { pi.min = 0.f; pi.max = 1.f; }
            if (!(pi.max > pi.min)) pi.max = pi.min + 1.f;   // degenerate metadata
            pi.def = clampv(pi.def, pi.min, pi.max);
            pi.unit = portUnit(port);

            ctrl_[i] = pi.def;
            paramPort_.push_back((int)i);
            params_.push_back(std::move(pi));
        }
        desc_.paramCount = (int)params_.size();
    }

    std::string portUnit(const LilvPort* port) const {
        World& w = world();
        std::string sym;
        LilvNodes* units = lilv_port_get_value(plug_, port, w.unitsUnit);
        if (units) {
            const LilvNode* u = lilv_nodes_get_first(units);
            if (u) {
                LilvNode* s = lilv_world_get(w.world, u, w.unitsSymbol, nullptr);
                sym = nodeString(s);
                lilv_node_free(s);
                // A bare unit URI with no symbol (units:db etc.) still has a
                // usable tail, e.g. ".../units#db" -> "db".
                if (sym.empty() && lilv_node_is_uri(u)) {
                    const std::string uri = lilv_node_as_uri(u);
                    const size_t h = uri.find_last_of("#/");
                    if (h != std::string::npos) sym = uri.substr(h + 1);
                }
            }
            lilv_nodes_free(units);
        }
        return sym;
    }

    void connectPorts() {
        const size_t frames = (size_t)maxBlock_;
        for (uint32_t i = 0; i < (uint32_t)slots_.size(); ++i) {
            const PortSlot& s = slots_[i];
            void* buf = nullptr;
            switch (s.kind) {
                case PortKind::ControlIn:
                case PortKind::ControlOut: buf = &ctrl_[i]; break;
                case PortKind::AudioIn:    buf = audioIn_[(size_t)s.slot]; break;
                case PortKind::AudioOut:   buf = audioOut_[(size_t)s.slot]; break;
                case PortKind::Cv:         buf = cvStore_.data() + (size_t)s.slot * frames; break;
                case PortKind::AtomIn:     buf = atomIn_[(size_t)s.slot]; break;
                case PortKind::AtomOut:    buf = atomOut_[(size_t)s.slot]; break;
                case PortKind::Ignored:    break;
            }
            lilv_instance_connect_port(inst_, i, buf);
        }
        resetAtomBuffers();
    }

    // REALTIME. Inputs must present an empty but valid Sequence; outputs must
    // advertise their capacity in atom.size before every run().
    void resetAtomBuffers() {
        for (u8* p : atomIn_) {
            LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)p;
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
            seq->atom.type = seqUrid_;
            seq->body.unit = 0;
            seq->body.pad  = 0;
        }
        for (u8* p : atomOut_) {
            LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)p;
            seq->atom.size = (uint32_t)(kAtomBufBytes - sizeof(LV2_Atom));
            seq->atom.type = chunkUrid_;
        }
    }

    // REALTIME. Appends this block's messages to every MIDI-capable atom input
    // port as LV2_Atom_Events. The port already holds an empty, valid sequence
    // (resetAtomBuffers ran first), so this only has to grow atom.size and lay
    // events out behind the header. Events must be in non-decreasing time
    // order, which is enforced here rather than trusted from the caller.
    void forgeMidi() {
        if (midiCount_ == 0 || midiPorts_.empty()) return;

        for (int slot : midiPorts_) {
            u8* buf = atomIn_[(size_t)slot];
            LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)buf;
            u32 size = seq->atom.size;                 // == sizeof(body) when empty
            u32 lastFrame = 0;

            for (int i = 0; i < midiCount_; ++i) {
                const MidiMsg& m = midi_[i];
                const u32 pad  = lv2_atom_pad_size(m.len);
                const u32 need = (u32)sizeof(LV2_Atom_Event) + pad;
                if (sizeof(LV2_Atom) + size + need > kAtomBufBytes) break;

                if (m.frame > lastFrame) lastFrame = m.frame;
                LV2_Atom_Event* ev = (LV2_Atom_Event*)(buf + sizeof(LV2_Atom) + size);
                ev->time.frames = lastFrame;
                ev->body.size   = m.len;
                ev->body.type   = midiUrid_;
                std::memcpy(ev + 1, m.data, m.len);
                // Pad bytes are part of the event but never read; zero them so a
                // strict plugin scanning the buffer sees nothing but the events.
                if (pad > m.len) std::memset((u8*)(ev + 1) + m.len, 0, pad - m.len);
                size += need;
            }
            seq->atom.size = size;
        }
    }

    PluginDesc        desc_;
    const LilvPlugin* plug_ = nullptr;
    LilvInstance*     inst_ = nullptr;

    f64 sr_ = 48000.0;
    int maxBlock_ = kMaxBlock;
    bool bypassed_ = false;

    std::vector<PortSlot>  slots_;
    int latencyPort_ = -1;               // port index of the latency report, or -1
    int latency_ = 0;                    // frames, settled at the end of prepare()
    std::vector<f32>       ctrl_;        // one float per port index; control ports point here
    std::vector<ParamInfo> params_;
    std::vector<int>       paramPort_;   // param index -> LV2 port index

    std::vector<f32> audioStore_, cvStore_;
    std::vector<u8>  atomStore_;
    std::vector<f32*> audioIn_, audioOut_;
    std::vector<u8*>  atomIn_, atomOut_;

    LV2_URID seqUrid_ = 0, chunkUrid_ = 0, midiUrid_ = 0;
    std::vector<int> midiPorts_;                 // atomIn_ slots that take MIDI
    MidiMsg midi_[kMaxMidiEvents]{};
    int     midiCount_ = 0;

    LV2_URID_Map   map_{};
    LV2_URID_Unmap unmap_{};
    LV2_Log_Log    log_{};
    std::vector<LV2_Options_Option> options_;
    i32 blockOpt_ = 0, minBlockOpt_ = 0, seqOpt_ = 0;
    f32 srOpt_ = 0.f;
    LV2_Feature fMap_{}, fUnmap_{}, fLog_{}, fOptions_{}, fBounded_{};
};

} // namespace

// --- scan ------------------------------------------------------------------
void scanLV2(std::vector<PluginDesc>& out) {
    World& w = world();
    if (!w.plugins) { LOGW("lv2: no plugins collection"); return; }

    int skipped = 0;
    LILV_FOREACH (plugins, it, w.plugins) {
        const LilvPlugin* p = lilv_plugins_get(w.plugins, it);
        const LilvNode* uriNode = lilv_plugin_get_uri(p);
        if (!uriNode) continue;
        const std::string uri = lilv_node_as_uri(uriNode);

        const std::string bad = unsupportedRequiredFeature(p);
        if (!bad.empty()) {
            LOGW("lv2: skipping %s (requires %s)", uri.c_str(), bad.c_str());
            ++skipped;
            continue;
        }

        PluginDesc d;
        d.format = PluginFormat::LV2;
        d.uri    = uri;

        LilvNode* nameNode = lilv_plugin_get_name(p);
        d.name = nodeString(nameNode);
        lilv_node_free(nameNode);
        if (d.name.empty()) d.name = uri;

        LilvNode* author = lilv_plugin_get_author_name(p);
        d.vendor = nodeString(author);
        lilv_node_free(author);

        const LilvPluginClass* cls = lilv_plugin_get_class(p);
        bool isInstrumentClass = false;
        if (cls) {
            d.category = nodeString(lilv_plugin_class_get_label(cls));
            const LilvNode* cu = lilv_plugin_class_get_uri(cls);
            isInstrumentClass = cu && lilv_node_is_uri(cu) &&
                                strcmp(lilv_node_as_uri(cu), LV2_CORE__InstrumentPlugin) == 0;
        }

        const uint32_t n = lilv_plugin_get_num_ports(p);
        for (uint32_t i = 0; i < n; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(p, i);
            const bool isIn = lilv_port_is_a(p, port, w.inputPort);
            if (lilv_port_is_a(p, port, w.audioPort)) {
                if (isIn) ++d.audioIn; else ++d.audioOut;
            } else if (lilv_port_is_a(p, port, w.controlPort)) {
                if (isIn) ++d.paramCount;
            } else if (isIn && (lilv_port_is_a(p, port, w.atomPort) ||
                                lilv_port_is_a(p, port, w.eventPort))) {
                if (lilv_port_supports_event(p, port, w.midiEvent)) d.hasMidiIn = true;
            }
        }

        if (isInstrumentClass || (d.hasMidiIn && d.audioIn == 0 && d.audioOut > 0))
            d.kind = PluginKind::Instrument;
        else if (d.audioIn > 0 || d.audioOut > 0)
            d.kind = PluginKind::Effect;
        else
            d.kind = PluginKind::Unknown;

        out.push_back(std::move(d));
    }
    LOGI("lv2: %zu usable, %d skipped", out.size(), skipped);
}

std::unique_ptr<PluginInstance> instantiateLV2(const PluginDesc& d, f64 sampleRate, int maxBlock) {
    World& w = world();
    if (!w.plugins) return nullptr;

    LilvNode* uri = lilv_new_uri(w.world, d.uri.c_str());
    if (!uri) return nullptr;
    const LilvPlugin* p = lilv_plugins_get_by_uri(w.plugins, uri);
    lilv_node_free(uri);
    if (!p) {
        LOGE("lv2: %s not found (rescan needed?)", d.uri.c_str());
        return nullptr;
    }

    const std::string bad = unsupportedRequiredFeature(p);
    if (!bad.empty()) {
        LOGE("lv2: refusing %s, requires %s", d.uri.c_str(), bad.c_str());
        return nullptr;
    }

    auto inst = std::make_unique<Lv2Instance>(d, p);
    if (!inst->prepare(sampleRate, maxBlock)) return nullptr;
    return inst;
}

} // namespace detail
} // namespace lat
