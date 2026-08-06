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
#include <utility>
#include <vector>

namespace lat {

class PluginRegistry;
class RackControl;

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

    // GUI thread. Non-null only for a device that hosts a chain of its own --
    // today exactly one, `nxtakt:rack`. This is the whole of the contract
    // addition racks needed: a rack IS a PluginInstance, so the browser, the
    // device strip, bypass, automation and the chain scheduler already work on
    // it, and the only thing they cannot express is "and it has an inside".
    //
    // A virtual accessor rather than a dynamic_cast because the concrete Rack
    // lives in an anonymous namespace in internal_devices.cpp and is not a type
    // any caller can name -- and because a future CLAP/VST3 container could
    // answer the same question without being our class at all.
    virtual RackControl* rack() { return nullptr; }
};

// ---------------------------------------------------------------------------
// Racks
//
// A Rack is a PluginInstance that contains a chain of PluginInstances in series
// and exposes eight macro parameters, each of which drives zero or more
// parameters on the devices inside it. It needs no UI of its own to be usable:
// it appears in the browser like any other device and its macros appear in the
// device strip like any other knobs.
//
// Everything below is GUI-THREAD ONLY. The audio thread reaches a rack through
// PluginInstance alone.
// ---------------------------------------------------------------------------

inline constexpr int kRackMacros      = 8;
inline constexpr int kRackMaxDevices  = 8;
inline constexpr int kRackMaxMappings = 64;
// Racks nest (a rack is a device like any other). The cap exists so a corrupt
// or hostile saved state cannot recurse the loader off the stack.
inline constexpr int kRackMaxDepth    = 4;

// One macro -> one target parameter, over the slice of that parameter's range
// the macro should sweep.
//
// Entirely passive: no pointers, no identity, trivially copyable, safe to write
// to a file. `min > max` is legal and INVERTS the mapping -- the target moves
// down as the macro moves up. That is not an edge case to tolerate, it is half
// the reason macro knobs exist (one knob that opens a filter while it closes a
// send). min == max pins the target to a constant.
//
// The parameter is named by ParamInfo::id, not by index, because the id is what
// SavedDevice already persists and what survives a plugin gaining a parameter.
// The rack resolves id -> index once per edit, never in process().
struct RackMapping {
    int macro  = 0;      // 0 .. kRackMacros-1
    int device = 0;      // index into the rack's chain, in processing order
    u32 param  = 0;      // ParamInfo::id on that device
    f32 min    = 0.f;    // target value when the macro reads 0
    f32 max    = 1.f;    // target value when the macro reads 1
};

// The complete contents of a rack in passive form: what persistence has to
// carry, with no live object in it anywhere.
//
// Device order is chain order. `params` is deliberately the same shape as
// SavedDevice::params, (ParamInfo::id, value), so the project layer can reuse
// the code it already has for a device's parameters.
struct RackState {
    struct Device {
        std::string uri;                             // PluginDesc::uri
        std::vector<std::pair<u32, f32>> params;     // (ParamInfo::id, value)
        bool        bypass = false;
        std::string state;                           // nested rack, compact form; else empty
    };
    std::vector<Device>      devices;
    f32                      macros[kRackMacros] = {};
    std::vector<RackMapping> mappings;
};

// The compact form: one line of printable ASCII with no whitespace, no quotes
// and no newline, so it drops into any line-oriented format (project.cpp's
// `kv`) without needing that format to know anything about racks. Round-trips
// exactly; nested racks are escaped once per level.
//
// Numbers are written and read through the C locale, like the rest of the
// project format -- main.cpp pins LC_NUMERIC to "C" precisely so this is true.
std::string rackStateToString(const RackState& s);
bool        rackStateFromString(const std::string& text, RackState& out);

// The editing face of a rack. GUI THREAD ONLY, every method.
//
// Sub-device creation goes through PluginRegistry::instantiate, which is
// GUI-thread-only and allocating -- so it happens here, at edit time, and never
// in process(). A rack holds a pointer to the registry that instantiated it;
// the registry must outlive the rack.
class RackControl {
public:
    virtual ~RackControl() = default;

    virtual int             deviceCount() const = 0;
    virtual PluginInstance* device(int i) const = 0;   // null if i is out of range

    // Instantiate and splice in. `at` is clamped into the chain. Returns false
    // if the chain is full, the plugin would not load, or this rack has no
    // registry behind it. Mappings that pointed past the insertion point are
    // renumbered so they keep pointing at the same device.
    virtual bool addDevice(const PluginDesc& d) = 0;
    virtual bool insertDevice(int at, const PluginDesc& d) = 0;

    // Unlinks the device. Mappings that targeted it are dropped and the rest
    // renumbered. The instance itself is RETIRED, not destroyed -- see
    // reclaim() for why.
    virtual bool removeDevice(int i) = 0;
    virtual bool moveDevice(int from, int to) = 0;

    virtual int                mappingCount() const = 0;
    virtual const RackMapping& mapping(int i) const = 0;

    // Returns the new mapping's index, or -1 if the macro, the device or the
    // parameter does not exist. `min`/`max` are clamped into the target
    // parameter's own range on the way in (which preserves inversion), so what
    // mapping() reports back is what the macro will actually do.
    virtual int  addMapping(const RackMapping& m) = 0;
    virtual bool removeMapping(int i) = 0;
    virtual void clearMacro(int macro) = 0;

    virtual RackState state() const = 0;
    // Replaces the entire contents. Restored parameter values are written
    // verbatim and macros are NOT re-applied over them, because a state that
    // was saved from a consistent rack already reflects its macro positions.
    virtual bool      setState(const RackState& s) = 0;

    // Frees the instances that removeDevice()/setState() unlinked.
    //
    // They are retained rather than deleted because a rack can be edited while
    // it is live in the engine, and nothing inside a PluginInstance can know
    // when the audio thread has finished with a pointer. Unlinking is safe (the
    // new topology is published atomically); deleting is not. Call this only
    // when the rack is NOT in a chain the engine is processing.
    virtual void reclaim() = 0;
};

// Backend entry points. One pair per format; host.cpp dispatches to them.
namespace detail {
    void scanLV2(std::vector<PluginDesc>& out);
    std::unique_ptr<PluginInstance> instantiateLV2(const PluginDesc& d, f64 sampleRate, int maxBlock);
    void scanInternal(std::vector<PluginDesc>& out);
    // `reg` is the registry doing the instantiating, and is handed on to the
    // one internal device that needs to create devices of its own (the rack).
    // A null registry yields a rack that works but cannot be filled -- which is
    // what a caller reaching for detail:: directly, outside a registry, gets.
    std::unique_ptr<PluginInstance> instantiateInternal(const PluginDesc& d, f64 sampleRate,
                                                        int maxBlock, PluginRegistry* reg = nullptr);
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
