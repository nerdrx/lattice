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
// TWO BACKINGS, AS OF STEPS 2 AND 3
// ---------------------------------
//   local    an Engine, an audio backend and the ALSA MIDI reader, all in this
//            process. What step 1 shipped and still the default.
//   daemon   an ipc::EngineClient talking to nxtaktd over the control region,
//            with the GUI's samples living in an ipc::SamplePool it owns.
//            NXTAKT_ENGINE=daemon (or LATTICE_ENGINE=daemon) selects it.
//
// `local()` answers null in daemon mode and that is the load-bearing test: no
// caller may assume there is an in-process Engine to reach.
//
// WHAT DAEMON MODE CARRIES, AND WHAT IT DOES NOT
// ----------------------------------------------
// Carried: transport, tempo, quantum, metronome, the mixer scalars, sends and
// return levels, every polled meter and indicator, the computer-MIDI keyboard
// and note previews, and — step 3 — session clips, both audio and MIDI, through
// the sample pool.
//
// Not carried yet, and refused *with a reason* rather than dropped (see
// remoteRefusals()): device chains (§5 step 4), recording (§7), the arrangement
// and its automation (which the daemon can take, but only as a pool blob this
// wave does not build), time signatures (Cmd::SetSignatures is outside
// ipc::commandIsKnown's bound, so the daemon answers RejectUnknownCommand and
// plays the set in 4/4), and hardware MIDI input (MidiInput pushes straight into
// an Engine, and in daemon mode there is not one — §1.3 moves it into nxtaktd).
//
// Threading: GUI thread only, like everything in src/ui. The Engine it owns in
// local mode is the one the audio thread runs; the ring pushes here are the
// producer side of the SPSC contract in core/ring.h. In daemon mode the same
// contract holds against the shared-memory rings, with this thread as the sole
// producer of commands and of MIDI.
#pragma once
#include "engine_state.h"
#include "../audio/backend.h"
#include "../audio/engine.h"
#include "../audio/midi_in.h"
#include "../plugin/host.h"      // PluginDesc, and PluginInstance as an identity
#include <memory>
#include <string>
#include <vector>

namespace lat {

// engine_state.h cannot see kMaxReturns (it includes core/ and nothing else, so
// that a view translation unit gets no engine header with it). This file has
// both, so this is where the duplicated number is held to account.
static_assert(kEsReturns == kMaxReturns,
              "EngineState's return arrays must be exactly kMaxReturns wide");

// The daemon half, defined in engine_handle.cpp. Held behind a pointer on
// purpose: src/ipc pulls in shm.h, pool.h, control.h and client.h — some four
// thousand lines and a <vector>/<string> dependency — and every view translation
// unit in src/ui reaches this header through app.h. Step 1's whole direction of
// travel was to get engine internals *out* of the view TUs, and swapping an
// Engine for an EngineClient there would have undone it.
struct RemoteEngine;

// What the daemon made of one device the GUI published. Pure data, no ipc: a
// caller that wants to draw "loading…", a reject reason or the real latency
// asks for this rather than for a DeviceMirror.
//
// Keyed by the GUI's own PluginInstance*, because that is the identity the
// model already has. §5 step 4 asks for a `DeviceModel { u64 uid; u32 deviceId;
// ... }` instead, which is the right end state and is a change to
// src/ui/session.h — see the note on publishChain() below for why this wave
// could not make it.
struct RemoteDevice {
    u32  id         = 0;        // the daemon's device id
    u32  generation = 0;        // its slot generation, the param-write guard
    bool live       = false;    // EvDeviceAdded has landed
    bool failed     = false;    // EvDeviceFailed did; `error` says why
    i32  latencyFrames = 0;     // the DAEMON's figure, which is the one that sounds
    u32  paramsMapped   = 0;    // GUI controls matched to a daemon control by id
    u32  paramsUnmapped = 0;    // GUI controls with no counterpart over there
    u32  paramsTruncated = 0;   // controls past ipc::kMaxDevParams (64)
    std::string uri, name, error;
};

class EngineHandle {
public:
    EngineHandle();
    ~EngineHandle();                      // out-of-line: RemoteEngine is opaque
    EngineHandle(const EngineHandle&)            = delete;
    EngineHandle& operator=(const EngineHandle&) = delete;

    // --- lifecycle ---------------------------------------------------------

    // Opens the engine this run is configured for: `NXTAKT_ENGINE=daemon`
    // (falling back to `LATTICE_ENGINE`) gives the remote path, anything else
    // — including unset — gives the in-process one. `NXTAKT_SESSION` names the
    // session; it defaults to "default".
    //
    // ABOUT THE NAME. This is `open()` wearing step 1's spelling. App::init()
    // has exactly one call to it and that file belongs to another agent this
    // wave, so the dispatch went inside the existing entry point rather than
    // into a rename nobody could apply. The one-line change — `eng_.open(...)`
    // at src/ui/app.cpp:54 — is owed and is the whole of it.
    //
    // `driver` is "jack", "alsa" or null for auto, i.e. NXTAKT_AUDIO. In daemon
    // mode it is forwarded to a daemon we spawn and ignored for one we merely
    // attach to, which already has a driver.
    //
    // Returns false only if nothing could be opened at all. A missing audio
    // backend is not an error (local mode prepares the engine anyway and the set
    // is silent), missing MIDI hardware is not an error, and neither is a daemon
    // that will not start: §8's degraded mode says a GUI with no engine should
    // still open, load, edit and save rather than refuse to run.
    bool openLocal(const char* driver);

    // The two halves openLocal() dispatches between, exposed for callers that
    // genuinely mean one of them (and for tests).
    bool openLocalEngine(const char* driver);
    bool openDaemon(const char* session, const char* driver);

    // Joins everything that touches the engine from another thread, in the one
    // order that is safe: the MIDI reader first (it is a producer on the
    // engine's ring, so it has to be gone before anything starts tearing the
    // engine down), then the audio backend (whose stop() joins the audio
    // thread). Once this returns, nothing can be inside process() and nothing
    // can be following a published chain or writing into a capture buffer —
    // which is what lets App::shutdown() free them without their handshakes.
    //
    // In daemon mode the ordering collapses (the GUI lends the engine nothing
    // it owns) and the question becomes a policy one, answered per §6: a daemon
    // we SPAWNED is stopped with us, a daemon we merely attached to is left
    // running. Parent-of-record, the same rule every editor/language-server
    // pair uses.
    //
    // The Engine itself outlives this call: App frees chains and note arrays
    // after it, and a couple of them are still reachable through local().
    void close();

    bool localOpen() const { return engine_ != nullptr; }
    bool remoteOpen() const { return remote_ != nullptr; }

    // The in-process Engine, or **null in daemon mode**. Deliberately narrow:
    // everything a view needs is in EngineState, and everything a command needs
    // is below. What is left is the two places that genuinely hand an Engine to
    // somebody else — the record journal's pump and the headless hooks — and
    // they are named at their call sites. Every one of them must cope with null.
    Engine* local() { return engine_.get(); }

    // --- the per-frame snapshot (§2.1) -------------------------------------
    //
    // Called ONCE at the top of frame(). Everything the UI draws from comes out
    // of `out`; nothing else reads an engine atomic.
    //
    // Not const: in daemon mode this is also the frame's housekeeping hook (link
    // state, retry counters), which is exactly where the doc's §2.2 wants it.
    void poll(EngineState& out);

    // --- commands (GUI -> engine) ------------------------------------------
    //
    // Every one returns whether the ring accepted it. App does not call these
    // directly for the bursty paths — App::send()/pushClip() go through the
    // pending queue in app_engine.cpp, which is what stops a project load from
    // outrunning a 1024-deep ring (docs/ARRANGEMENT.md §15) and which doubles as
    // §2.2's dirty-cell set and scalar outbox for the daemon path: a refused
    // push is re-queued and retried on the next frame, in order.
    //
    // WHICH MEANS `false` HAS ONE MEANING AND ONLY ONE: "try again". A command
    // daemon mode can never carry must NOT answer false, or the caller re-queues
    // it forever and nothing behind it in the FIFO ever moves again. Those are
    // consumed, counted in remoteRefusals(), and logged once each with a reason.
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
    const char* driverName() const;
    f64  driverSampleRate() const;
    int  driverBufferSize() const;
    bool midiRunning() const;
    int  midiClientId() const;
    u64  midiReceived() const;

    // --- devices (§5 step 4) ------------------------------------------------
    //
    // THERE IS NO addDevice() HERE, AND THAT IS THE DESIGN OF THIS STEP.
    //
    // §5 step 4 describes rewriting App's device code around device ids:
    // `DeviceModel::inst` deleted, `addDevice()` becoming "send CmdAddDevice and
    // wait for the event", the knobs reading a DeviceMirror. That is the right
    // end state and it is unreachable from these files — it edits
    // src/ui/session.h and src/ui/app_devices.cpp, and it would delete the
    // in-process path that §8 says stays supported through step 6.
    //
    // So the seam is put one level lower, at the one call every chain edit
    // already funnels through: App::publishChain() builds an RtChain and hands
    // it to pushCommand(). An RtChain cannot cross a process boundary — but
    // everything the daemon needs in order to BUILD ITS OWN is readable off it
    // through PluginInstance's virtuals: desc().uri, paramInfo(i).id,
    // getParam(i), bypassed(). So the remote path reads the chain the GUI
    // declared and reconciles the daemon toward it with AddDevice /
    // RemoveDevice / MoveDevice / SetBypass, then mirrors params every frame.
    //
    // Two consequences worth stating rather than discovering:
    //
    //   * The GUI's PluginInstance is not silent-and-pointless in daemon mode,
    //     it is the MODEL. It holds the parameter values, the bypass flag and
    //     the rack contents; it just never renders audio, because there is no
    //     in-process engine to call process(). That is exactly §4's split — the
    //     GUI is the authority on what exists, the engine on what sounds.
    //   * Instantiation is asynchronous, and the GUI does not currently know
    //     that. A device appears in the model at once and starts sounding a
    //     frame or several later (or, on the very first one, after the daemon's
    //     plugin scan). devicesPending() is how a status line says so.
    //
    // Params are addressed by ParamInfo::id, per docs/PARAM-ADDRESS.md: the GUI
    // and the daemon load the same plugin build, so the ids match, but the
    // INDEX ordering is not something either side promises the other. A control
    // whose id has no counterpart is counted, never guessed at.

    // What the daemon made of a device the GUI published, or null if this is
    // not the daemon path or the instance is not in any published chain.
    const RemoteDevice* remoteDevice(const PluginInstance* gui) const;
    // Devices asked for whose answer has not arrived. Also in EngineState.
    u32 devicesPending() const;
    u64 devicesAdded() const;
    u64 devicesFailed() const;

    // --- the plugin catalog (§5 step 5) -------------------------------------
    //
    // What the DAEMON can instantiate, which is not in general what this
    // process can find: a different LV2_PATH, a different user, a bundle that
    // crashes lilv here and not there. A browser drawn from the local
    // PluginRegistry can therefore offer a row whose double-click can only ever
    // fail, which is the whole reason §3 asked for a catalog table.
    //
    // Empty in local mode — there the local registry IS the daemon's — and
    // empty in daemon mode until the scan completes. Ask with requestScan();
    // scanRunning() drives the spinner.
    const std::vector<PluginDesc>& catalog() const;
    // Plugins the daemon found that did not fit the table. Non-zero must be
    // drawn: "…and N more this build cannot list" beats a silently short list.
    u32  catalogTruncated() const;
    bool catalogReady() const;
    bool scanRunning() const;
    bool requestScan();

    // --- lifecycle (§6) -----------------------------------------------------
    EngineLink link() const;
    // The engine's process id, or -1 when there is no daemon. Status-bar
    // material, and the only handle on the daemon a test has.
    i32 enginePid() const;

    // §6's recovery, and the ONLY thing that may run it is a user click or a
    // provably dead engine — never a stale heartbeat (§4.4). Reaps the orphan
    // region, respawns, re-attaches (which re-announces the pool), republishes
    // every clip cell from the client's shadow — a memcpy, no decode, no offset
    // changes — replays the mixer scalars and the tempo, and re-issues
    // AddDevice for every device on every chain, because device ids do not
    // survive an engine (§11.4).
    //
    // The transport deliberately comes back STOPPED, per §4.4's honest default.
    //
    // False if nothing could be started; the handle is then Detached and the
    // set is still editable and saveable.
    bool restartEngine();
    u64  resyncs() const;      // completed restartEngine()s

    // --- daemon-mode diagnostics -------------------------------------------
    //
    // How many commands were consumed because the remote path cannot carry them
    // yet. Must be readable, because "refused with a reason" is only true if
    // somebody can see the reason: every distinct command type is also logged
    // once, and close() prints the tally.
    u64 remoteRefusals() const;
    // Snapshots that exhausted readCoherent()'s retry budget, i.e. frames drawn
    // from a copy that may have straddled a publish. Zero unless the daemon is
    // stopped mid-publish.
    u64 snapshotTears() const;

private:
    // Heap, not by value. Engine is ~2.3 MB of scratch buffers — it was already
    // a member of App and therefore already wherever App lives, but a pointer
    // is what lets local() answer "there is no in-process engine", and it takes
    // the GUI's largest object off whatever stack App sits on.
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<AudioBackend> audio_;
    // Started after the backend and stopped before it: the reader thread pushes
    // into the engine's ring from its own thread.
    MidiInput midi_;

    // Null unless openDaemon() succeeded. Exactly one of engine_ and remote_ is
    // ever set; both null is §8's degraded mode and is a supported state.
    std::unique_ptr<RemoteEngine> remote_;
};

} // namespace lat
