#pragma once
#include "../core/common.h"
#include <memory>

namespace lat {

// Routes libasound's own stderr diagnostics through our logger, printing each
// distinct message once. Call early in main(), before anything can touch ALSA
// -- plugins do, whether or not ALSA is our audio backend. NXTAKT_ALSA_VERBOSE=1
// leaves libasound's handler in place, unfiltered.
void alsaInstallLogHandler();

class Engine;

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual bool start(Engine& e) = 0;
    virtual void stop() = 0;
    virtual f64  sampleRate() const = 0;
    virtual int  bufferSize() const = 0;
    virtual const char* name() const = 0;
};

// `prefer` is "jack", "alsa" or null for auto (JACK first, ALSA fallback).
std::unique_ptr<AudioBackend> createBackend(Engine& e, const char* prefer);

} // namespace lat
