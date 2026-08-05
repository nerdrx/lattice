// A minimal CLAP plugin used to exercise the CLAP backend end to end: there is
// no reason to require a real CLAP plugin on the machine just to prove that
// discovery, instantiation, parameter reporting and the event path work.
//
// Stereo in / stereo out, one parameter ("Gain", 0..2, default 1). Build:
//   g++ -std=c++20 -O2 -fPIC -shared -Ivendor/clap/include
//       tests/fake_clap_plugin.cpp -o /tmp/clapdir/test.clap
//   CLAP_PATH=/tmp/clapdir ./plugin_scan
#include <clap/clap.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr clap_id kParamGain = 0;

const char* const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
    CLAP_PLUGIN_FEATURE_STEREO, nullptr,
};

const clap_plugin_descriptor_t kDesc = {
    CLAP_VERSION_INIT,
    "com.nxtakt.test.gain",
    "NxTakt Test Gain",
    "NxTakt",
    "", "", "",
    "1.0.0",
    "Test fixture: a gain stage.",
    kFeatures,
};

struct Gain {
    clap_plugin_t plugin{};
    double gain = 1.0;
};

Gain* self(const clap_plugin_t* p) { return (Gain*)p->plugin_data; }

// --- params ----------------------------------------------------------------
uint32_t CLAP_ABI paramsCount(const clap_plugin_t*) { return 1; }

bool CLAP_ABI paramsGetInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index != 0) return false;
    *info = clap_param_info_t{};
    info->id            = kParamGain;
    info->flags         = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
    info->cookie        = nullptr;
    info->min_value     = 0.0;
    info->max_value     = 2.0;
    info->default_value = 1.0;
    std::snprintf(info->name, sizeof info->name, "Gain");
    std::snprintf(info->module, sizeof info->module, "/");
    return true;
}

bool CLAP_ABI paramsGetValue(const clap_plugin_t* p, clap_id id, double* out) {
    if (id != kParamGain) return false;
    *out = self(p)->gain;
    return true;
}

bool CLAP_ABI paramsValueToText(const clap_plugin_t*, clap_id id, double v,
                                char* buf, uint32_t cap) {
    if (id != kParamGain) return false;
    std::snprintf(buf, cap, "%.2f x", v);
    return true;
}

bool CLAP_ABI paramsTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* out) {
    if (id != kParamGain) return false;
    *out = std::atof(text);
    return true;
}

void applyEvent(Gain* g, const clap_event_header_t* h) {
    if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (h->type != CLAP_EVENT_PARAM_VALUE) return;
    const auto* e = (const clap_event_param_value_t*)h;
    if (e->param_id == kParamGain) g->gain = e->value;
}

void CLAP_ABI paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in,
                          const clap_output_events_t*) {
    if (!in) return;
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) applyEvent(self(p), in->get(in, i));
}

const clap_plugin_params_t kParams = {
    paramsCount, paramsGetInfo, paramsGetValue, paramsValueToText, paramsTextToValue, paramsFlush,
};

// --- audio ports -----------------------------------------------------------
uint32_t CLAP_ABI portsCount(const clap_plugin_t*, bool) { return 1; }

bool CLAP_ABI portsGet(const clap_plugin_t*, uint32_t index, bool is_input,
                       clap_audio_port_info_t* info) {
    if (index != 0) return false;
    *info = clap_audio_port_info_t{};
    info->id            = is_input ? 0 : 1;
    info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type     = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    std::snprintf(info->name, sizeof info->name, "%s", is_input ? "In" : "Out");
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = { portsCount, portsGet };

// --- plugin ----------------------------------------------------------------
bool CLAP_ABI pluginInit(const clap_plugin_t*) { return true; }

void CLAP_ABI pluginDestroy(const clap_plugin_t* p) { delete self(p); }

bool CLAP_ABI pluginActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void CLAP_ABI pluginDeactivate(const clap_plugin_t*) {}
bool CLAP_ABI pluginStartProcessing(const clap_plugin_t*) { return true; }
void CLAP_ABI pluginStopProcessing(const clap_plugin_t*) {}
void CLAP_ABI pluginReset(const clap_plugin_t*) {}

clap_process_status CLAP_ABI pluginProcess(const clap_plugin_t* p, const clap_process_t* px) {
    Gain* g = self(p);
    if (!px) return CLAP_PROCESS_ERROR;

    if (px->in_events) {
        const uint32_t n = px->in_events->size(px->in_events);
        for (uint32_t i = 0; i < n; ++i) applyEvent(g, px->in_events->get(px->in_events, i));
    }
    if (px->audio_inputs_count < 1 || px->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;

    const clap_audio_buffer_t& in  = px->audio_inputs[0];
    clap_audio_buffer_t&       out = px->audio_outputs[0];
    const float gain = (float)g->gain;
    for (uint32_t c = 0; c < out.channel_count; ++c) {
        float* o = out.data32[c];
        const float* i = (c < in.channel_count) ? in.data32[c] : nullptr;
        for (uint32_t f = 0; f < px->frames_count; ++f) o[f] = i ? i[f] * gain : 0.f;
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginGetExtension(const clap_plugin_t*, const char* id) {
    if (strcmp(id, CLAP_EXT_PARAMS) == 0)      return &kParams;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    return nullptr;
}

void CLAP_ABI pluginOnMainThread(const clap_plugin_t*) {}

// --- factory ---------------------------------------------------------------
uint32_t CLAP_ABI factoryCount(const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor_t* CLAP_ABI factoryDescriptor(const clap_plugin_factory*, uint32_t i) {
    return i == 0 ? &kDesc : nullptr;
}

const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory*, const clap_host_t* host,
                                            const char* id) {
    if (!host || !id || strcmp(id, kDesc.id) != 0) return nullptr;
    Gain* g = new Gain();
    g->plugin.desc             = &kDesc;
    g->plugin.plugin_data      = g;
    g->plugin.init             = pluginInit;
    g->plugin.destroy          = pluginDestroy;
    g->plugin.activate         = pluginActivate;
    g->plugin.deactivate       = pluginDeactivate;
    g->plugin.start_processing = pluginStartProcessing;
    g->plugin.stop_processing  = pluginStopProcessing;
    g->plugin.reset            = pluginReset;
    g->plugin.process          = pluginProcess;
    g->plugin.get_extension    = pluginGetExtension;
    g->plugin.on_main_thread   = pluginOnMainThread;
    return &g->plugin;
}

const clap_plugin_factory_t kFactory = { factoryCount, factoryDescriptor, factoryCreate };

bool CLAP_ABI entryInit(const char*) { return true; }
void CLAP_ABI entryDeinit() {}

const void* CLAP_ABI entryGetFactory(const char* id) {
    return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory,
};
