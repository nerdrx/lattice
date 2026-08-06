// EngineState — everything the UI polls off the engine, as one plain struct.
//
// docs/GUI-ON-DAEMON.md §2.1, option (c). The GUI used to read ~20 std::atomic
// members off Engine during a draw, scattered across seven translation units.
// This is the value those reads produce, sampled ONCE per frame by
// EngineHandle::poll() and read from everywhere else.
//
// Three things it buys, in the order they matter:
//
//   1. A frame is internally coherent. drawClipSlot read slotState, activeSlot,
//      pendingSlot and clipPhase as four independent relaxed loads, milliseconds
//      apart in wall-clock terms; two of them could straddle a publish and
//      disagree — a slot drawn as Playing with activeSlot == -1. One sample
//      cannot.
//   2. It is the seam the daemon needs. When the state comes off
//      ipc::SharedState instead of off Engine, only poll() changes; not one draw
//      function does.
//   3. It makes the views testable with no engine, no audio device and no
//      daemon: fill one of these in and draw.
//
// PURE DATA. No atomics, no Engine, no ipc — that is the whole point, and the
// one thing not to "improve" here. It is also why the return count is written
// out below instead of included from audio/engine.h.
#pragma once
#include "../core/common.h"

namespace lat {

// kMaxReturns from audio/engine.h, duplicated so this header can stay free of
// it (kMaxTracks and kMaxScenes are only reachable because they live in
// core/common.h). engine_handle.h has both headers and static_asserts that the
// two numbers agree, exactly as ipc/control.h does for ipc::kShmReturns — so
// the duplication cannot drift without failing a build.
inline constexpr int kEsReturns = 4;

struct EngineState {
    // --- transport -----------------------------------------------------
    f64 beat       = 0.0;      // absolute beats since transport start
    f64 tempo      = 120.0;
    bool playing   = false;
    f32 cpu        = 0.f;

    // The playhead as a musician reads it: bars.beats.sixteenths, ONE-BASED,
    // and the signature IN FORCE AT THE PLAYHEAD (which is not in general the
    // set's — a set in 7/8 with a 4/4 section reads 4/4 while the playhead is
    // inside it).
    //
    // Carried rather than derived, and the reason is a disagreement the UI would
    // otherwise render with total confidence. The transport readout used to
    // compute these from the SESSION's signature map. The two agree exactly
    // until a publication is refused — sigMapValid re-derives every entry's
    // bar-start beat on arrival and rejects a map whose bar lines do not follow
    // from its own bar lengths, and a refused map leaves the ENGINE at 4/4. The
    // session's map still says 7/8, so the readout would say 7/8, and nothing
    // anywhere would indicate that what is playing is 4/4. Reading the engine's
    // own counters makes that state unrenderable.
    i32 posBar = 1, posBeat = 1, posSixteenth = 1;
    i32 posSigNum = 4, posSigDen = 4;

    // --- engine configuration ------------------------------------------
    // Carried here so a status bar can draw them from the snapshot like
    // everything else. The paths that need the value *now* rather than as of
    // this frame — decoding a sample, sizing a capture buffer — ask
    // EngineHandle::sampleRate() instead; see the note there.
    f64 sampleRate = 48000.0;
    u32 blockSize  = 0;
    i32 latencyFrames = 0;     // total plugin delay compensation

    // --- per track ------------------------------------------------------
    // Sentinels match Engine's: activeSlot -1 = nothing playing, pendingSlot
    // -2 = nothing queued (-1 means a queued *stop*), recSlotIdx -1 = idle.
    i32 slotState[kMaxTracks]   = {};    // lat::SlotState
    i32 activeSlot[kMaxTracks]  = {};
    i32 pendingSlot[kMaxTracks] = {};
    f64 clipPhase[kMaxTracks]   = {};
    f32 meterL[kMaxTracks]      = {};
    f32 meterR[kMaxTracks]      = {};
    i32 recState[kMaxTracks]    = {};    // 0 idle, 1 queued, 2 recording
    i32 recSlotIdx[kMaxTracks]  = {};

    // --- buses ----------------------------------------------------------
    f32 returnMeterL[kEsReturns] = {};
    f32 returnMeterR[kEsReturns] = {};
    f32 masterMeterL = 0.f, masterMeterR = 0.f;

    // --- the arrangement -------------------------------------------------
    // Bit i set == track i's arrangement lane is suspended because a session
    // clip was launched on it (ARRANGEMENT.md §4.2). Engine-owned: it is set at
    // the quantized launch the ENGINE computes, which is why the UI has to be
    // told rather than infer it.
    u32 arrOverride    = 0;
    u32 journalDropped = 0;    // engine-side refused journal pushes

    EngineState() {
        for (int i = 0; i < kMaxTracks; ++i) {
            activeSlot[i]  = -1;
            pendingSlot[i] = -2;
            recSlotIdx[i]  = -1;
        }
    }
};

} // namespace lat
