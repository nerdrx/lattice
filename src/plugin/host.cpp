// Registry and per-format dispatch. Everything here is GUI-thread only.
#include "host.h"
#include <algorithm>

namespace lat {

// CLAP backend entry points. Declared here rather than alongside the LV2 pair
// in host.h so that adding a backend keeps touching exactly one file.
namespace detail {
    void scanCLAP(std::vector<PluginDesc>& out);
    std::unique_ptr<PluginInstance> instantiateCLAP(const PluginDesc& d, f64 sampleRate, int maxBlock);
}

const char* formatName(PluginFormat f) {
    switch (f) {
        case PluginFormat::LV2:  return "LV2";
        case PluginFormat::CLAP: return "CLAP";
        case PluginFormat::VST3: return "VST3";
        case PluginFormat::Internal: return "Internal";
    }
    return "?";
}

const char* kindName(PluginKind k) {
    switch (k) {
        case PluginKind::Effect:     return "effect";
        case PluginKind::Instrument: return "instrument";
        case PluginKind::Unknown:    return "unknown";
    }
    return "?";
}

void PluginRegistry::scan() {
    plugins_.clear();

    detail::scanInternal(plugins_);              // stock devices, no filesystem
    detail::scanLV2(plugins_);
    detail::scanCLAP(plugins_);                  // $CLAP_PATH, ~/.clap, /usr/lib/clap
    // TODO(vst3): detail::scanVST3(plugins_);   ~/.vst3, /usr/lib/vst3

    // Stable, case-insensitive order so the browser list does not reshuffle
    // between scans just because the filesystem walk changed. Stock devices sort
    // ahead of everything else: they are the ones a user reaches for without
    // knowing a name, and there are a handful of them against hundreds of
    // third-party plugins, so alphabetical order would bury them.
    std::sort(plugins_.begin(), plugins_.end(), [](const PluginDesc& a, const PluginDesc& b) {
        const bool ai = a.format == PluginFormat::Internal;
        const bool bi = b.format == PluginFormat::Internal;
        if (ai != bi) return ai;

        auto lower = [](const std::string& s) {
            std::string r = s;
            std::transform(r.begin(), r.end(), r.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return r;
        };
        const std::string la = lower(a.name), lb = lower(b.name);
        if (la != lb) return la < lb;
        return a.uri < b.uri;
    });

    LOGI("plugin scan: %zu plugins", plugins_.size());
}

const PluginDesc* PluginRegistry::find(const std::string& uri) const {
    for (const PluginDesc& d : plugins_)
        if (d.uri == uri) return &d;
    return nullptr;
}

std::unique_ptr<PluginInstance> PluginRegistry::instantiate(const PluginDesc& d,
                                                            f64 sampleRate, int maxBlock) {
    switch (d.format) {
        case PluginFormat::LV2:
            return detail::instantiateLV2(d, sampleRate, maxBlock);
        case PluginFormat::CLAP:
            return detail::instantiateCLAP(d, sampleRate, maxBlock);
        case PluginFormat::Internal:
            return detail::instantiateInternal(d, sampleRate, maxBlock);
        case PluginFormat::VST3:
            // TODO(vst3): return detail::instantiateVST3(d, sampleRate, maxBlock);
            LOGE("VST3 hosting not implemented (%s)", d.name.c_str());
            return nullptr;
    }
    return nullptr;
}

} // namespace lat
