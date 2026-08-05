// Format-agnostic plugin hosting.
//
// Threading contract (mirrors audio/engine.h):
//   * PluginRegistry::scan() and instantiate() are GUI-thread only. They are
//     slow, they allocate, and they touch std::string.
//   * PluginInstance::process() is audio-thread only: no allocation, no locks,
//     no exceptions, no std::string.
//   * setParam()/setBypassed() are called from the GUI thread *while* the audio
//     thread is inside process(). Both write plain scalars that the backend
//     reads without synchronisation. See the note on PluginInstance::setParam.
//
// Only LV2 is implemented today. CLAP and VST3 slot into the same three
// entry points in namespace detail; the dispatch in host.cpp already branches
// on PluginDesc::format so adding a backend touches nothing else.
#pragma once
#include "../core/common.h"
#include <memory>
#include <string>
#include <vector>

namespace lat {

// Internal = NxTakt's own stock devices. They implement PluginInstance like
// any other backend, so they inherit the browser, knobs, bypass, chains and
// persistence without special cases anywhere else.
enum class PluginFormat : int { LV2 = 0, CLAP, VST3, Internal };
enum class PluginKind   : int { Effect = 0, Instrument, Unknown };

const char* formatName(PluginFormat f);
const char* kindName(PluginKind k);

// A plugin the scanner found on disk. Cheap to copy and to keep in a list;
// nothing here has been loaded yet.
struct PluginDesc {
    std::string  uri;                 // LV2: the plugin URI. Other formats: "path:index".
    std::string  name;
    std::string  vendor;
    std::string  category;
    PluginFormat format    = PluginFormat::LV2;
    PluginKind   kind      = PluginKind::Unknown;
    int          audioIn   = 0;
    int          audioOut  = 0;
    bool         hasMidiIn = false;
    int          paramCount = 0;      // control inputs, filled in by the scanner
};

// One automatable control. Ranges come from the plugin's own metadata, so
// min/max can be anything (including inverted or degenerate) — the backend
// normalises them before we get here.
struct ParamInfo {
    std::string name;
    std::string unit;                 // display symbol, e.g. "dB", "Hz". May be empty.
    f32  min = 0.f, max = 1.f, def = 0.f;
    bool isBool = false;
    bool isInt  = false;
    bool isLogarithmic = false;
    u32  id = 0;                      // backend-defined; LV2 uses the port index
};

// One loaded plugin sitting on one track.
class PluginInstance {
public:
    virtual ~PluginInstance() = default;

    // GUI thread, before the instance is handed to the engine. Returns false if
    // the plugin refused to activate at this rate/block size.
    virtual bool prepare(f64 sampleRate, int maxBlock) = 0;

    // REALTIME. `in` and `out` are arrays of `channels` pointers, each nframes
    // long. Aliasing (in[c] == out[c]) is allowed. Never allocates or locks.
    virtual void process(const f32* const* in, f32* const* out, int channels, int nframes) = 0;

    // REALTIME. Raw MIDI (status + up to two data bytes), delivered by the
    // engine before this block's process(); `frameOffset` is the sample
    // position within that block. Default no-op so effects ignore it; note-
    // capable backends (CLAP note events, LV2 atom sequences, internal
    // instruments) override. Same rules as process(): no allocation, no locks.
    virtual void midi(const u8* data, int len, int frameOffset) { (void)data; (void)len; (void)frameOffset; }

    virtual int              paramCount() const = 0;
    virtual const ParamInfo& paramInfo(int i) const = 0;
    virtual f32              getParam(int i) const = 0;

    // REALTIME. The automation path: called from inside the audio callback,
    // before this block's process(), to apply a value the engine computed from
    // a clip envelope.
    //
    // A separate entry point from setParam(), not a relaxation of it, because
    // the callers differ in the one way that matters: setParam() is the ONLY
    // writer on the non-realtime side and may therefore use a single-producer
    // queue — which is exactly what the CLAP backend does. A second producer on
    // that queue would be a data race, so a backend whose parameter path is a
    // queue must give the audio thread a path of its own.
    //
    // Returns false when this backend has no realtime parameter path. The
    // engine then marks the lane inert, emits Ev::AutoLaneInert once so the UI
    // can grey it, and never calls again for that published set. A silently
    // ignored lane would be the worst outcome: the envelope is drawn, the sound
    // does not move, and nothing says why.
    //
    // Same rules as process(): no allocation, no locks, no exceptions.
    virtual bool setParamRT(int i, f32 v) { (void)i; (void)v; return false; }

    // getParam() is realtime-safe to call: a plain load in every backend in the
    // tree. The engine needs it to remember what a parameter was before an
    // envelope took it over, so it can be restored when playback stops.

    // GUI thread, concurrent with process(). Backends store parameters as
    // plain floats that the plugin reads once per run(), so a torn read is
    // impossible on every architecture we target and a stale read costs at
    // most one block of latency. No lock is taken.
    virtual void setParam(int i, f32 v) = 0;

    virtual const PluginDesc& desc() const = 0;

    // Processing latency in frames at the prepared rate/block size. Constant
    // after prepare() and audio-thread-safe to read; the engine uses it for
    // delay compensation, so a lying plugin smears transients across parallel
    // paths. LV2: the reportsLatency control-out port. CLAP: the latency
    // extension. Internal devices: 0.
    virtual int latencyFrames() const { return 0; }

    // REALTIME-safe to read; set from the GUI thread. When bypassed, process()
    // copies input to output and does not call into the plugin at all.
    virtual void setBypassed(bool b) = 0;
    virtual bool bypassed() const = 0;
};

// Backend entry points. One pair per format; host.cpp dispatches to them.
namespace detail {
    void scanLV2(std::vector<PluginDesc>& out);
    std::unique_ptr<PluginInstance> instantiateLV2(const PluginDesc& d, f64 sampleRate, int maxBlock);
    void scanInternal(std::vector<PluginDesc>& out);
    std::unique_ptr<PluginInstance> instantiateInternal(const PluginDesc& d, f64 sampleRate, int maxBlock);
    // TODO(vst3): void scanVST3(std::vector<PluginDesc>&);
}

class PluginRegistry {
public:
    // GUI thread. Slow (lilv walks every bundle on the system) and allocates.
    // Replaces the previous result wholesale; already-instantiated plugins are
    // unaffected because they hold their own copy of the descriptor.
    void scan();

    const std::vector<PluginDesc>& plugins() const { return plugins_; }

    // Exact URI first, then the permanent alias table in host.cpp: a set saved
    // before the Lattice -> NxTakt rename names its stock devices `lattice:*`
    // and must keep resolving to the `nxtakt:*` descriptors forever. Returns
    // the CANONICAL descriptor either way, so whatever the caller saves next
    // carries the current spelling.
    const PluginDesc* find(const std::string& uri) const;

    // GUI thread. Returns null if the plugin failed to load or activate.
    std::unique_ptr<PluginInstance> instantiate(const PluginDesc& d, f64 sampleRate, int maxBlock);

private:
    std::vector<PluginDesc> plugins_;
};

} // namespace lat
