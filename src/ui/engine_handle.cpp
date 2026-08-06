// EngineHandle: the local path. See engine_handle.h for the shape and
// docs/GUI-ON-DAEMON.md §2 for why it is a concrete class rather than an
// interface.
#include "engine_handle.h"
#include <new>

namespace lat {

bool EngineHandle::openLocal(const char* driver) {
    engine_ = std::unique_ptr<Engine>(new (std::nothrow) Engine());
    if (!engine_) { LOGE("could not allocate the engine"); return false; }

    audio_ = createBackend(*engine_, driver);
    if (!audio_) {
        // Not an error. A set can be edited, saved and looked at with no audio
        // device at all, and refusing to start would make a broken ALSA
        // configuration on somebody else's machine fatal.
        LOGW("no audio backend available - running silent");
        engine_->prepare(48000.0, 1024);
    }

    // MIDI comes up after the audio backend: the reader thread pushes straight
    // into the engine's ring, so the engine must already be prepared. Missing
    // hardware or a missing sequencer device is not an error - a set can be
    // played entirely from the mouse.
    if (midi_.start(*engine_)) LOGI("midi in: alsa seq client %d:0", midi_.clientId());
    else                       LOGW("no MIDI input - continuing without it");
    return true;
}

void EngineHandle::close() {
    // Order matters, and it is the order App::shutdown() used to spell out
    // inline. The MIDI reader goes first: it pushes into the engine's ring from
    // its own thread, so it has to be joined before anything else starts
    // tearing the engine down, or a push could land in a ring nobody owns any
    // more. Stopping the backend then joins the audio thread.
    midi_.stop();
    if (audio_) { audio_->stop(); audio_.reset(); }
    // engine_ is deliberately NOT released here: App frees the chains, note
    // arrays and capture buffers the engine was borrowing *after* this returns,
    // and one of the debug hooks still reaches it through local(). It dies with
    // the handle, which dies with App.
}

// ---------------------------------------------------------------------------
// The snapshot
// ---------------------------------------------------------------------------
//
// One tight copy per frame. What this fixes and what it does not, precisely:
//
//   It removes the INTRA-FRAME incoherence, which is the one that showed. The
//   four reads drawClipSlot used to make were separated by whatever the draw
//   code did in between — easily a millisecond, i.e. several audio blocks at
//   256 frames — so a slot could be drawn Playing with activeSlot == -1. After
//   this they are one copy taken microseconds apart.
//
//   It does NOT make the copy itself atomic against Engine::publish(). Locally
//   there is nothing to gate on: publish() bumps no generation counter, and
//   blocksRendered is incremented at the TOP of process() while publish() runs
//   at the bottom, so a reader that saw the same blocksRendered either side of
//   its copy could still have straddled the publish for that block. engine.h is
//   frozen this wave, so adding one is not on the table — and the remaining
//   window is the duration of this function.
//
//   Cross-process it closes completely and for free: ipc::SharedState has
//   `generation`, bumped last with release, and the daemon branch of this
//   function is a read-generation / copy / re-read-generation retry loop (§2.1).
void EngineHandle::poll(EngineState& out) const {
    const Engine* e = engine_.get();
    if (!e) { out = EngineState{}; return; }        // detached: a stopped transport

    out.beat    = e->beat.load(std::memory_order_relaxed);
    out.tempo   = e->tempo.load(std::memory_order_relaxed);
    out.playing = e->playing.load(std::memory_order_relaxed);
    out.cpu     = e->cpu.load(std::memory_order_relaxed);

    out.sampleRate    = e->sampleRate();
    out.blockSize     = audio_ ? (u32)audio_->bufferSize() : 0u;
    out.latencyFrames = e->latencyFrames.load(std::memory_order_relaxed);

    for (int t = 0; t < kMaxTracks; ++t) {
        out.slotState[t]   = e->slotState[t].load(std::memory_order_relaxed);
        out.activeSlot[t]  = e->activeSlot[t].load(std::memory_order_relaxed);
        out.pendingSlot[t] = e->pendingSlot[t].load(std::memory_order_relaxed);
        out.clipPhase[t]   = e->clipPhase[t].load(std::memory_order_relaxed);
        out.meterL[t]      = e->meterL[t].load(std::memory_order_relaxed);
        out.meterR[t]      = e->meterR[t].load(std::memory_order_relaxed);
        out.recState[t]    = e->recState[t].load(std::memory_order_relaxed);
        out.recSlotIdx[t]  = e->recSlotIdx[t].load(std::memory_order_relaxed);
    }
    for (int i = 0; i < kMaxReturns; ++i) {
        out.returnMeterL[i] = e->returnMeterL[i].load(std::memory_order_relaxed);
        out.returnMeterR[i] = e->returnMeterR[i].load(std::memory_order_relaxed);
    }
    out.masterMeterL = e->masterMeterL.load(std::memory_order_relaxed);
    out.masterMeterR = e->masterMeterR.load(std::memory_order_relaxed);

    out.arrOverride    = e->arrOverride.load(std::memory_order_relaxed);
    out.journalDropped = e->journalDropped.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool EngineHandle::send(Cmd t, i32 a, i32 b, f64 x) {
    Command c;
    c.type = t; c.a = a; c.b = b; c.x = x;
    return pushCommand(c);
}

bool EngineHandle::pushCommand(const Command& c) {
    return engine_ ? engine_->pushCommand(c) : false;
}

bool EngineHandle::pushMidi(const MidiMsg& m) {
    return engine_ ? engine_->pushMidiFromGui(m) : false;
}

bool EngineHandle::popEvent(Event& e) {
    return engine_ ? engine_->popEvent(e) : false;
}

f64 EngineHandle::sampleRate() const {
    return engine_ ? engine_->sampleRate() : 48000.0;
}

u32 EngineHandle::journalDropped() const {
    return engine_ ? engine_->journalDropped.load(std::memory_order_relaxed) : 0u;
}

} // namespace lat
