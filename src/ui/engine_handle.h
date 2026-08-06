// EngineHandle — the one thing in the UI that knows where the engine is.
//
// docs/GUI-ON-DAEMON.md §2.1, option (b)+(c): a CONCRETE class holding whichever
// backing it was opened with, no vtable. The doc rejects a virtual IEngine
// explicitly and the reason is worth keeping in view: the dominant call class
// used to be twenty polled atomics read ~3 000 times a frame, so a virtual
// getter per field would have been twenty virtuals on the hot path plus twenty
// pairs of near-identical one-line overrides to keep in sync — an abstraction
// paying for the wrong axis. The snapshot (poll(), engine_state.h) removes the
// hot path entirely, and what is left is a handful of calls a frame where a
// branch on a member that never changes after init() predicts perfectly.
//
// THIS WAVE IS LOCAL ONLY. openLocal() does exactly what App::init() used to do
// inline — an Engine, an audio backend and the ALSA MIDI reader. openDaemon()
// (ipc::EngineClient) is step 2; every place it will branch is marked below so
// the next agent can see the shape without reading the whole app.
//
// Threading: GUI thread only, like everything in src/ui. The Engine it owns is
// the one the audio thread runs; the ring pushes here are the producer side of
// the SPSC contract in core/ring.h.
#pragma once
#include "engine_state.h"
#include "../audio/backend.h"
#include "../audio/engine.h"
#include "../audio/midi_in.h"
#include <memory>

namespace lat {

// engine_state.h cannot see kMaxReturns (it includes core/ and nothing else, so
// that a view translation unit gets no engine header with it). This file has
// both, so this is where the duplicated number is held to account.
static_assert(kEsReturns == kMaxReturns,
              "EngineState's return arrays must be exactly kMaxReturns wide");

class EngineHandle {
public:
    // --- lifecycle ---------------------------------------------------------

    // Engine + audio backend + MIDI reader, in that order and with the same
    // fallbacks App::init() had: a missing backend is not an error (the engine
    // is prepared anyway and the set is silent), and missing MIDI hardware is
    // not an error either. Returns false only if the Engine itself could not be
    // allocated.
    //
    // `driver` is "jack", "alsa" or null for auto, i.e. NXTAKT_AUDIO.
    bool openLocal(const char* driver);

    // Joins everything that touches the engine from another thread, in the one
    // order that is safe: the MIDI reader first (it is a producer on the
    // engine's ring, so it has to be gone before anything starts tearing the
    // engine down), then the audio backend (whose stop() joins the audio
    // thread). Once this returns, nothing can be inside process() and nothing
    // can be following a published chain or writing into a capture buffer —
    // which is what lets App::shutdown() free them without their handshakes.
    //
    // The Engine itself outlives this call: App frees chains and note arrays
    // after it, and a couple of them are still reachable through local().
    void close();

    bool localOpen() const { return engine_ != nullptr; }

    // The in-process Engine, or null once there is a daemon path and it is the
    // one in use. Deliberately narrow: everything a view needs is in
    // EngineState, and everything a command needs is below. What is left is the
    // two places that genuinely hand an Engine to somebody else — the record
    // journal's pump and the headless hooks — and they are named at their call
    // sites.
    Engine* local() { return engine_.get(); }

    // --- the per-frame snapshot (§2.1) -------------------------------------
    //
    // Called ONCE at the top of frame(). Everything the UI draws from comes out
    // of `out`; nothing else reads an engine atomic.
    void poll(EngineState& out) const;

    // --- commands (GUI -> engine) ------------------------------------------
    //
    // Every one returns whether the ring accepted it. App does not call these
    // directly for the bursty paths — App::send()/pushClip() go through the
    // pending queue in app_engine.cpp, which is what stops a project load from
    // outrunning a 1024-deep ring (docs/ARRANGEMENT.md §15).
    bool send(Cmd t, i32 a = 0, i32 b = 0, f64 x = 0.0);
    bool pushCommand(const Command& c);
    // The GUI's MIDI ring, which is NOT the hardware reader's: Ring tolerates
    // one producer and there are two (engine.h, pushMidi / pushMidiFromGui).
    bool pushMidi(const MidiMsg& m);
    bool popEvent(Event& e);

    // --- live scalars -------------------------------------------------------
    //
    // Not part of the frame snapshot, deliberately. sampleRate() is read by
    // paths that decode or resample, where "as of this frame" is the wrong
    // question and a zero from a snapshot taken before the engine was prepared
    // would silently resample everything wrong. journalDropped() is read at the
    // moment a take is committed, where §5.4 wants the count *now* and a value
    // one frame old could commit a take that should have been refused.
    f64 sampleRate() const;
    u32 journalDropped() const;

    // --- what the status bar prints about the backend ----------------------
    // In daemon mode these come off ControlHeader::driver and SharedState
    // instead; the call sites do not change.
    const char* driverName() const { return audio_ ? audio_->name() : nullptr; }
    f64  driverSampleRate() const  { return audio_ ? audio_->sampleRate() : 0.0; }
    int  driverBufferSize() const  { return audio_ ? audio_->bufferSize() : 0; }
    bool midiRunning() const       { return midi_.running(); }
    int  midiClientId() const      { return midi_.clientId(); }
    u64  midiReceived() const      { return midi_.received(); }

private:
    // Heap, not by value. Engine is ~2.3 MB of scratch buffers — it was already
    // a member of App and therefore already wherever App lives, but a pointer
    // is what lets local() answer "there is no in-process engine" in step 2,
    // and it takes the GUI's largest object off whatever stack App sits on.
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<AudioBackend> audio_;
    // Started after the backend and stopped before it: the reader thread pushes
    // into the engine's ring from its own thread.
    MidiInput midi_;
};

} // namespace lat
