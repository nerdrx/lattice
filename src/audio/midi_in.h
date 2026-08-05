// ALSA sequencer MIDI input.
//
// Threading contract:
//   * start()/stop() are GUI-thread only. They open/close the sequencer client
//     and own the reader thread.
//   * the reader thread does nothing but block in snd_seq_event_input() and
//     hand translated messages to Engine::pushMidi(), which is the producer
//     side of a lock-free ring. It never touches engine state directly.
//
// Nothing is auto-connected: a DAW that grabs every keyboard on the system is a
// nuisance in a JACK/PipeWire graph. The client:port id is logged so the user
// can wire it up with aconnect or qpwgraph.
//
// Constructing a MidiInput does nothing at all; until start() is called there
// is no client, no thread and no cost.
#pragma once
#include "../core/common.h"
#include <atomic>
#include <thread>

namespace lat {

class Engine;

class MidiInput {
public:
    ~MidiInput() { stop(); }

    // Opens the sequencer client and spins up the reader thread. Returns false
    // (and stays fully inert) if ALSA sequencer support is unavailable, which
    // is the normal case in containers and on kernels without snd-seq.
    bool start(Engine& e);
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }
    // ALSA client id of our sequencer client, -1 when not running. The input
    // port is always port 0, so the wiring target is "<clientId()>:0".
    int  clientId() const { return client_; }
    // Messages accepted since start(); the ring drops on overflow, so this is
    // the count that actually reached the engine.
    u64  received() const { return received_.load(std::memory_order_relaxed); }

private:
    void run();

    void*             seq_     = nullptr;   // snd_seq_t*, opaque here so this
                                            // header pulls in no ALSA headers.
    Engine*           engine_  = nullptr;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<u64>  received_{0};
    int               client_  = -1;
    int               port_    = -1;
};

} // namespace lat
