#pragma once
#include "../core/common.h"
#include <memory>

namespace lat {

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
