// EngineHandle in daemon mode, with no window and no GUI.
//
// The other suites test the two ends of the boundary: ipc_test the transport,
// daemon_test the far side against a real spawned nxtaktd. This tests the NEAR
// side — the object src/ui actually holds — doing exactly what App does to it
// and nothing else: open, poll, send, publish a clip, drain events, close.
//
// Three things are only reachable from here:
//
//   * `local()` answering null, which is what every caller that used to assume
//     an in-process Engine has to cope with;
//   * the retirement stand-in. A GUI-heap RtNote[] is COPIED into the pool, so
//     the engine never holds it and can never send Ev::NotesRetired for it. The
//     handle has to. Without that, App::retiringNotes_ grows for the life of the
//     session and nothing ever comes home;
//   * close(). A headless gamescope run cannot reach it — the compositor kills
//     the GUI outright — so "the daemon we spawned is stopped and both regions
//     are unlinked" has no other test.
//
// Built by `make build/handle_test` and run by `make test`. It spawns its own
// daemon and cleans up after itself. To build it by hand:
//
//   g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -I.
//       tests/handle_test.cpp src/ui/engine_handle.cpp src/audio/engine.cpp
//       src/audio/backend.cpp src/audio/midi_in.cpp src/core/common.cpp
//       -o build/handle_test $(pkg-config --libs jack alsa) -lrt -lpthread -lm
//   (one line; the continuations are left off so this comment does not trip
//    -Wcomment, which the tree builds with)
//   ./build/handle_test          # needs build/nxtaktd
#include "src/ui/engine_handle.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <dirent.h>
#include <unistd.h>
#include <cstdlib>
#include <csignal>

using namespace lat;

static int gPass = 0, gFail = 0;
#define CHECK(c, ...) do { if (c) { ++gPass; std::printf("  PASS  "); } \
    else { ++gFail; std::printf("  FAIL  "); } std::printf(__VA_ARGS__); std::printf("\n"); \
    std::fflush(stdout); } while (0)

static void sleepMs(int ms) { timespec t{ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&t,nullptr); }

static void banner(const char* s) { std::printf("\n== %s\n", s); }

// The master peak over `frames` polls, with the event pump running — i.e. the
// frame loop App runs, with the meter read off the snapshot like every other
// indicator. poll() is also where the handle reconciles chains and mirrors
// params, so calling it is not incidental to what is being measured.
static f32 peakOver(lat::EngineHandle& eng, lat::EngineState& es, int frames) {
    lat::Event e;
    f32 peak = 0.f;
    // SETTLE FIRST, and the reason is not tidiness. This is a PEAK over a
    // window, and a change made just before the call — a param write that
    // poll() has not pushed yet, a chain edit the daemon has not applied — is
    // still audible during the first frames of it. A window that spans the
    // transition reports the level BEFORE the change as confidently as the one
    // after, which is exactly the class of check that passes against a dropped
    // write. A third of the window is thrown away, then the peak is measured.
    const int settle = frames / 3 + 10;
    for (int i = 0; i < settle; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    for (int i = 0; i < frames; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        peak = std::fmax(peak, es.masterMeterL);
        sleepMs(10);
    }
    return peak;
}

// ---------------------------------------------------------------------------
// A PluginInstance that describes a plugin and renders nothing
// ---------------------------------------------------------------------------
//
// In daemon mode the GUI's own instance renders nothing either — there is no
// in-process engine to call process() — so this is not a stand-in for the real
// thing, it is the real thing's job. What App's device code puts into an
// RtChain is an object that answers desc(), paramInfo(i).id, getParam(i) and
// bypassed(), and those four are the entire input the handle takes off a chain.
//
// Every method used here is virtual, which is what lets this suite go on
// linking no plugin backend: host.h is a header, and the vtable is ours.
static constexpr int kDrive = 0, kOutput = 1, kMix = 2;

struct FakeDevice : lat::PluginInstance {
    lat::PluginDesc d;
    lat::ParamInfo  pi[3];
    f32  v[3] = { 0.f, 0.f, 1.f };
    bool byp = false;

    explicit FakeDevice(const char* uri = "nxtakt:saturator", const char* name = "Saturator") {
        d.uri = uri; d.name = name; d.vendor = "NxTakt";
        d.format = lat::PluginFormat::Internal;
        d.kind   = lat::PluginKind::Effect;
        d.audioIn = 2; d.audioOut = 2; d.paramCount = 3;
        // The ids are what matter and they are the internal devices' own:
        // addParam() numbers them by ordinal (internal_devices.cpp). The NAMES
        // here are cosmetic — the handle matches on id, per PARAM-ADDRESS.md,
        // and never on a name or a position.
        pi[kDrive]  = { "Drive",  "dB", 0.f,  36.f, 0.f, false, false, true,  0u };
        pi[kOutput] = { "Output", "dB", -24.f, 24.f, 0.f, false, false, false, 1u };
        pi[kMix]    = { "Mix",    "",   0.f,   1.f, 1.f, false, false, false, 2u };
    }

    bool prepare(f64, int) override { return true; }
    void process(const f32* const*, f32* const*, int, int) override {}
    int  paramCount() const override { return 3; }
    const lat::ParamInfo& paramInfo(int i) const override { return pi[i < 0 || i > 2 ? 0 : i]; }
    f32  getParam(int i) const override { return (i < 0 || i > 2) ? 0.f : v[i]; }
    void setParam(int i, f32 x) override { if (i >= 0 && i <= 2) v[i] = x; }
    const lat::PluginDesc& desc() const override { return d; }
    void setBypassed(bool b) override { byp = b; }
    bool bypassed() const override { return byp; }
};

static int countShm(const char* needle) {
    DIR* d = ::opendir("/dev/shm");
    if (!d) return -1;
    int n = 0;
    while (dirent* e = ::readdir(d)) if (std::strstr(e->d_name, needle)) ++n;
    ::closedir(d);
    return n;
}

int main() {
    char session[64];
    std::snprintf(session, sizeof session, "htest-%d", (int)::getpid());
    ::setenv("NXTAKT_ENGINE", "daemon", 1);
    ::setenv("NXTAKT_SESSION", session, 1);
    ::setenv("NXTAKT_DAEMON", "build/nxtaktd", 0);   // an externally-set path wins, so the daemon can be sanitised too

    std::printf("== EngineHandle, daemon mode, session '%s'\n", session);

    EngineHandle eng;
    CHECK(eng.openLocal("null"), "openLocal() dispatched on NXTAKT_ENGINE and opened something");
    CHECK(!eng.localOpen(), "localOpen() is false: no in-process Engine was created");
    CHECK(eng.remoteOpen(), "remoteOpen() is true");
    CHECK(eng.local() == nullptr, "local() answers null, which is the load-bearing test");
    CHECK(std::fabs(eng.sampleRate() - 48000.0) < 1e-9,
          "sampleRate() is live off the wire before anything decodes (%.0f)", eng.sampleRate());
    CHECK(eng.driverName() && std::strstr(eng.driverName(), "null"),
          "driverName() comes off ControlHeader ('%s')", eng.driverName() ? eng.driverName() : "");
    CHECK(!eng.midiRunning(), "midiRunning() is false: hardware MIDI is not on this path");

    EngineState es;
    eng.poll(es);
    CHECK(std::fabs(es.sampleRate - 48000.0) < 1e-9 && es.blockSize == 256,
          "the snapshot carries the engine format (%.0f Hz / %u frames)",
          es.sampleRate, es.blockSize);

    // --- scalars (step 2) --------------------------------------------------
    CHECK(eng.send(Cmd::SetTempo, 0, 0, 140.0), "SetTempo crosses");
    CHECK(eng.send(Cmd::SetQuantum, 0), "SetQuantum crosses");
    CHECK(eng.send(Cmd::SetPlaying, 1), "SetPlaying crosses");
    bool tempoSeen = false, beatMoved = false;
    const f64 b0 = es.beat;
    for (int i = 0; i < 100 && !(tempoSeen && beatMoved); ++i) {
        sleepMs(20);
        eng.poll(es);
        if (std::fabs(es.tempo - 140.0) < 1e-6) tempoSeen = true;
        if (es.beat > b0 + 0.5) beatMoved = true;
    }
    CHECK(tempoSeen, "the snapshot reports the tempo the GUI set (%.1f)", es.tempo);
    CHECK(beatMoved && es.playing, "the transport runs in the daemon (beat %.2f, playing %d)",
          es.beat, (int)es.playing);

    // --- MIDI (step 2) -----------------------------------------------------
    MidiMsg m{}; m.status = 0x90; m.d1 = 60; m.d2 = 100;
    CHECK(eng.pushMidi(m), "pushMidi crosses on the MIDI ring");

    // --- a clip through the pool (step 3) ----------------------------------
    // A DC buffer on this process's heap, exactly as a decoded SampleBuffer is:
    // the handle is the thing that has to notice it cannot travel as a pointer.
    const i64 frames = 48000;
    std::vector<f32> dc((size_t)frames, 0.5f);

    Command c;
    c.type = Cmd::SetClip;
    c.a = 0; c.b = 0;
    c.clip.data        = dc.data();
    c.clip.frames      = frames;
    c.clip.channels    = 1;
    c.clip.loopStart   = 0;
    c.clip.loopEnd     = frames;
    c.clip.warp        = (int)Warp::Off;
    c.clip.loop        = true;
    c.clip.quantumIdx  = 0;
    c.clip.lengthBeats = 4.0;
    c.clip.gain        = 1.0f;
    c.clip.valid       = true;
    CHECK(eng.pushCommand(c), "SetClip with a GUI-heap f32* is accepted by the handle");

    // Drain a few frames so the ack comes home, as App::frame() would.
    Event e;
    for (int i = 0; i < 40; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(20); }

    CHECK(eng.send(Cmd::TrackVol, 0, 0, 1.0), "TrackVol crosses");
    CHECK(eng.send(Cmd::MasterVol, 0, 0, 1.0), "MasterVol crosses");
    CHECK(eng.send(Cmd::LaunchClip, 0, 0), "LaunchClip crosses");

    f32 peak = 0.f;
    for (int i = 0; i < 150; ++i) {
        eng.poll(es);
        peak = std::fmax(peak, es.masterMeterL);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    CHECK(peak > 0.3f && peak < 0.7f,
          "the daemon renders the clip the handle copied into the pool "
          "(master peak %.4f, expected ~0.5)", (double)peak);
    CHECK(es.activeSlot[0] == 0, "track 0 reports slot 0 active (%d)", es.activeSlot[0]);

    // --- a clip replaced: the retirement stand-in ---------------------------
    std::vector<RtNote> n1(4), n2(4);
    for (int i = 0; i < 4; ++i) { n1[i].beat = i; n1[i].len = 1; n1[i].pitch = (u8)(60+i); n1[i].vel = 100; }
    for (int i = 0; i < 4; ++i) { n2[i].beat = i; n2[i].len = 1; n2[i].pitch = (u8)(72+i); n2[i].vel = 100; }

    Command mc;
    mc.type = Cmd::SetClip; mc.a = 1; mc.b = 0;
    mc.clip.isMidi = true; mc.clip.notes = n1.data(); mc.clip.noteCount = 4;
    mc.clip.lengthBeats = 4.0; mc.clip.gain = 1.0f; mc.clip.valid = true;
    mc.clip.quantumIdx = 0;
    CHECK(eng.pushCommand(mc), "a MIDI clip's notes cross as a pool block");
    for (int i = 0; i < 20; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }

    mc.clip.notes = n2.data();
    bool pushed = false;
    for (int i = 0; i < 40 && !pushed; ++i) {
        pushed = eng.pushCommand(mc);
        if (!pushed) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(20); }
    }
    CHECK(pushed, "replacing that clip's notes is accepted (after the cell's ack)");

    void* retired = nullptr;
    for (int i = 0; i < 40 && !retired; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) if (e.type == Ev::NotesRetired) retired = e.p;
        sleepMs(10);
    }
    CHECK(retired == (void*)n1.data(),
          "Ev::NotesRetired came back for the DISPLACED array (%p, wanted %p) — "
          "without this App::retiringNotes_ grows for the life of the session",
          retired, (void*)n1.data());

    // =====================================================================
    // STEP 4: a device published as a chain reaches the engine and SOUNDS
    // =====================================================================
    //
    // This is the whole of step 4 from the near side. App::publishChain() hands
    // pushCommand() an RtChain full of PluginInstance*; the handle reads the
    // chain's DESCRIPTION off those instances and reconciles the daemon toward
    // it. Nothing about App changes, and nothing about the RtChain crosses.
    //
    // FakeDevice is a PluginInstance that renders nothing and describes
    // nxtakt:saturator. That is not a shortcut, it is the point: in daemon mode
    // the GUI's instance never renders anything either — it is the model, and
    // what the handle needs from it is exactly desc().uri, paramInfo(i).id,
    // getParam(i) and bypassed(). A fake that supplies those is the same input
    // a real one is. (It also lets this suite keep its promise of linking no
    // plugin backends: every one of those is a virtual call.)
    banner("step 4: a device chain, over the wire");

    const f32 dryPeak = peakOver(eng, es, 120);
    CHECK(dryPeak > 0.3f && dryPeak < 0.7f,
          "the bare clip still meters %.4f on the master", (double)dryPeak);

    FakeDevice sat;
    RtChain ch0;
    ch0.fx[0] = &sat;
    ch0.count = 1;
    Command chain;
    chain.type = Cmd::SetChain; chain.a = 0; chain.p = &ch0;
    CHECK(eng.pushCommand(chain),
          "Cmd::SetChain is ACCEPTED now — answering false would make "
          "App::addDevice() roll the device back out of the model as "
          "'engine busy', which is the visible-and-silent bug step 4 removes");

    // Asynchronous, and the GUI has to be able to say so: the chain is in the
    // model already and is not yet the chain that sounds. A `devicesPending`
    // that nobody ever saw non-zero would read as a plausible zero for ever,
    // which is why it is asserted here and not only at the end.
    eng.poll(es);
    CHECK(eng.devicesPending() == 1 && es.devicesPending == 1,
          "one device is outstanding while the engine loads it (%u / %u)",
          eng.devicesPending(), es.devicesPending);

    // The first AddDevice starts the daemon's plugin scan, so this is the one
    // place a device add can take seconds rather than a frame.
    const RemoteDevice* rd = nullptr;
    for (int i = 0; i < 1200 && !(rd && rd->live); ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        rd = eng.remoteDevice(&sat);
        sleepMs(10);
    }
    CHECK(rd && rd->live, "the engine instantiated it: device %u '%s'",
          rd ? rd->id : 0u, rd ? rd->name.c_str() : "-");
    CHECK(rd && rd->uri == "nxtakt:saturator",
          "and the engine's own table names it '%s'", rd ? rd->uri.c_str() : "-");
    CHECK(rd && rd->paramsMapped == 3 && rd->paramsUnmapped == 0,
          "all 3 controls matched by ParamInfo::id, none guessed at (mapped %u, "
          "unmapped %u) — docs/PARAM-ADDRESS.md",
          rd ? rd->paramsMapped : 0u, rd ? rd->paramsUnmapped : 0u);
    CHECK(eng.devicesAdded() == 1 && eng.devicesFailed() == 0,
          "one added, none failed (%llu / %llu)",
          (unsigned long long)eng.devicesAdded(), (unsigned long long)eng.devicesFailed());
    CHECK(eng.devicesPending() == 0 && es.devicesPending == 0,
          "and nothing is outstanding, so the chain on screen is the chain that sounds");

    // The device is in the chain, and the meter proves it: the clip is DC 0.5
    // and the saturator's shaper is y = tanh(g*x) * comp, so at the default
    // 0 dB drive it reads tanh(0.5) = 0.4621. Not "unchanged" — the point is
    // that it changed by exactly the amount the DEVICE would change it.
    //
    // (The obvious probe, turning Drive up, is deliberately not the one used
    // below: the device's gain compensation is written so that a large drive
    // tends to tanh(0.5) too — see internal_devices.cpp — so on a DC 0.5 the
    // two ends of that knob happen to meter identically. A test that cannot
    // tell a working param write from a dropped one is worse than no test.)
    const f32 satPeak = peakOver(eng, es, 120);
    CHECK(satPeak > 0.40f && satPeak < dryPeak * 0.98f,
          "the saturator is really in the chain: %.4f, which is tanh(0.5) on a "
          "DC 0.5 clip, against %.4f dry", (double)satPeak, (double)dryPeak);

    // --- THE PRIZE: a knob turned on the GUI's instance changes the audio ---
    //
    // Nothing is sent here. sat.setParam() is exactly what drawDeviceStrip()
    // does to the instance it holds; the handle notices on the next poll() and
    // writes the param table. That is the whole knob path in daemon mode — and
    // it is a poll rather than a hook because a knob drag has no command to
    // hang one on.
    //
    // Output trim, and not Drive, so the number is monotone: -12 dB is a factor
    // of 0.251. It is also the control with ParamInfo::id 1, so a mapping that
    // had guessed positionally and got it wrong would move Drive instead and
    // show up here as no change at all.
    sat.setParam(kOutput, -12.f);
    const f32 trimmed = peakOver(eng, es, 150);
    CHECK(trimmed < satPeak * 0.40f && trimmed > satPeak * 0.15f,
          "turning Output to -12 dB on the GUI's OWN instance changed what the "
          "daemon renders: %.4f -> %.4f (x0.251 expected). Nothing was sent",
          (double)satPeak, (double)trimmed);
    sat.setParam(kOutput, 0.f);

    // --- bypass is a command, not a param write ----------------------------
    //
    // A one-line change that is easy to get wrong by reflex: bypass has to
    // order against the chain edits around it, so it is Cmd::SetBypass and not
    // a slot in the param table (§3.7).
    sat.setBypassed(true);
    const f32 bypassed = peakOver(eng, es, 150);
    CHECK(std::fabs(bypassed - dryPeak) < 0.02f,
          "bypassing it on the model bypasses it in the engine: %.4f, back to "
          "the dry %.4f", (double)bypassed, (double)dryPeak);
    sat.setBypassed(false);
    const f32 unbypassed = peakOver(eng, es, 150);
    CHECK(std::fabs(unbypassed - satPeak) < 0.02f,
          "and un-bypassing puts it back: %.4f", (double)unbypassed);

    // --- removing it: the chain shrinks and the retirement comes home ------
    //
    // App::removeDevice() publishes the shorter chain and then hangs the dead
    // instance off the retiring_ entry publishChain() just made, freeing it
    // when Ev::ChainRetired arrives. There is no engine here to send that, so
    // the handle synthesises it for the DISPLACED chain — without which
    // App::retiring_ grows for the life of the session and every removed plugin
    // leaks.
    RtChain ch1;
    ch1.count = 0;
    Command clear;
    clear.type = Cmd::SetChain; clear.a = 0; clear.p = &ch1;
    CHECK(eng.pushCommand(clear), "an empty chain for track 0 is accepted");
    void* retiredChain = nullptr;
    for (int i = 0; i < 200 && !retiredChain; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) if (e.type == Ev::ChainRetired) retiredChain = e.p;
        sleepMs(10);
    }
    CHECK(retiredChain == (void*)&ch0,
          "Ev::ChainRetired came back for the DISPLACED chain (%p, wanted %p)",
          retiredChain, (void*)&ch0);
    CHECK(eng.remoteDevice(&sat) == nullptr,
          "and the device is gone from the mirror");
    const f32 afterPeak = peakOver(eng, es, 150);
    CHECK(std::fabs(afterPeak - dryPeak) < 0.02f,
          "the track is dry again and still sounding: %.4f (was %.4f with the "
          "device, %.4f before it)", (double)afterPeak, (double)satPeak, (double)dryPeak);

    // --- a plugin the engine does not have is refused ONCE -----------------
    //
    // Fail closed, and fail once. A failure that were retried every frame would
    // be a command per frame for the life of the session.
    FakeDevice ghost("nxtakt:no-such-device", "Ghost");
    RtChain ch2;
    ch2.fx[0] = &ghost;
    ch2.count = 1;
    Command bad;
    bad.type = Cmd::SetChain; bad.a = 0; bad.p = &ch2;
    CHECK(eng.pushCommand(bad), "a chain naming an unknown plugin is still accepted");
    const RemoteDevice* gd = nullptr;
    for (int i = 0; i < 300 && !(gd && gd->failed); ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        gd = eng.remoteDevice(&ghost);
        sleepMs(10);
    }
    CHECK(gd && gd->failed, "and the engine answered with a reason: '%s'",
          gd ? gd->error.c_str() : "-");
    const u64 failed0 = eng.devicesFailed();
    for (int i = 0; i < 60; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(5); }
    CHECK(eng.devicesFailed() == failed0,
          "it is not retried every frame (%llu failures, unchanged over 60 more "
          "polls)", (unsigned long long)eng.devicesFailed());

    // =====================================================================
    // STEP 5: the browser lists what the DAEMON can load
    // =====================================================================
    banner("step 5: the catalog");

    CHECK(eng.catalogReady(), "the catalog arrived with EvScanComplete");
    const std::vector<PluginDesc>& cat = eng.catalog();
    CHECK(cat.size() > 2, "it has %zu plugins", cat.size());
    bool sawSat = false, sawPulse = false;
    for (const PluginDesc& d : cat) {
        if (d.uri == "nxtakt:saturator") sawSat = true;
        if (d.uri == "nxtakt:pulse")     sawPulse = true;
    }
    CHECK(sawSat && sawPulse, "including the stock devices (saturator %d, pulse %d)",
          (int)sawSat, (int)sawPulse);
    // The reason a catalog exists at all: this list is the DAEMON's answer, so
    // a row the browser draws is a row AddDevice can load. A GUI browsing its
    // own PluginRegistry could offer one the daemon has never heard of.
    for (const PluginDesc& d : cat)
        if (d.uri == "nxtakt:pulse")
            CHECK(d.kind == PluginKind::Instrument && d.hasMidiIn,
                  "with their real shape: Pulse is an instrument that takes MIDI");
    CHECK(eng.catalogTruncated() == 0,
          "nothing was dropped for want of table space (%u)", eng.catalogTruncated());

    // =====================================================================
    // STEP 6: the link state, and a restart that puts the set back
    // =====================================================================
    banner("step 6: lifecycle");

    eng.poll(es);
    CHECK(eng.link() == EngineLink::Live && es.link == EngineLink::Live,
          "the link reads Live and the snapshot carries it");
    CHECK(engineLinkBanner(EngineLink::Live) == nullptr,
          "a live engine draws no banner");
    CHECK(engineLinkBanner(EngineLink::Lost) != nullptr &&
          engineLinkOffersRestart(EngineLink::Lost) &&
          !engineLinkOffersRestart(EngineLink::Starting),
          "a lost one draws '%s' and offers a restart; a starting one does not",
          engineLinkBanner(EngineLink::Lost));

    // Put the chain back so the restart has something to rebuild, and turn the
    // master down so the replay has something to prove.
    CHECK(eng.pushCommand(chain), "the saturator chain is published again");
    CHECK(eng.send(Cmd::MasterVol, 0, 0, 0.5), "and the master fader is moved to 0.5");
    for (int i = 0; i < 200 && !(eng.remoteDevice(&sat) && eng.remoteDevice(&sat)->live); ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    CHECK(eng.remoteDevice(&sat) && eng.remoteDevice(&sat)->live, "and it is live again");
    const u32 oldId  = eng.remoteDevice(&sat)->id;
    const i32 oldPid = eng.enginePid();
    CHECK(oldPid > 0, "the engine's pid is reachable (%d)", oldPid);

    // --- WEDGED IS NOT DEAD, and this is the distinction §4.4 turns on -----
    //
    // SIGSTOP is what a laptop resuming from suspend and a JACK restart both
    // look like: the process is there, it is simply not publishing. The rule
    // the UI is most likely to violate is respawning on this. So the handle has
    // to be able to SEE it — Stale, with a silence it can put a number on — and
    // must do nothing about it.
    ::kill(oldPid, SIGSTOP);
    for (int i = 0; i < 200 && eng.link() != EngineLink::Stale; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    eng.poll(es);
    CHECK(es.link == EngineLink::Stale,
          "a SIGSTOPped engine reads Stale, not Lost: the process is alive and "
          "a respawn under it would be the worst available outcome (§4.4)");
    CHECK(es.linkSilentMs > 300,
          "and the silence is measured, not guessed: %u ms", es.linkSilentMs);
    CHECK(eng.resyncs() == 0, "nothing restarted itself");
    ::kill(oldPid, SIGCONT);
    for (int i = 0; i < 200 && eng.link() != EngineLink::Live; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    eng.poll(es);
    CHECK(es.link == EngineLink::Live && es.linkSilentMs == 0,
          "and it comes back on its own when the engine does");

    // --- DEAD, and §6's recovery ------------------------------------------
    //
    // SIGKILL leaves an orphaned control region and a live sample pool, which
    // is exactly the state §4.3 designed for: the samples outlive the engine so
    // a replacement can adopt them.
    ::kill(oldPid, SIGKILL);
    for (int i = 0; i < 300 && eng.link() != EngineLink::Lost; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    eng.poll(es);
    CHECK(es.link == EngineLink::Lost,
          "a killed engine reads Lost — the creator pid is gone, checked with "
          "its start time and never the pid alone");
    CHECK(engineLinkBanner(es.link) != nullptr && engineLinkOffersRestart(es.link),
          "which draws '%s' and offers a restart", engineLinkBanner(es.link));

    // §6's recovery. Device ids do not survive an engine, the pool does, and
    // the transport comes back stopped.
    CHECK(eng.restartEngine(), "restartEngine() reaped, respawned and re-attached");
    CHECK(eng.enginePid() > 0 && eng.enginePid() != oldPid,
          "it is a different process (%d, was %d)", eng.enginePid(), oldPid);
    CHECK(eng.resyncs() == 1, "one resync (%llu)", (unsigned long long)eng.resyncs());
    CHECK(eng.link() == EngineLink::Starting || eng.link() == EngineLink::Live,
          "the link is up again");

    const RemoteDevice* rd2 = nullptr;
    for (int i = 0; i < 1200 && !(rd2 && rd2->live); ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        rd2 = eng.remoteDevice(&sat);
        sleepMs(10);
    }
    CHECK(rd2 && rd2->live, "the chain was rebuilt against the new engine: device %u",
          rd2 ? rd2->id : 0u);
    // The id is the NEW engine's. Nothing here asserts what it is, on purpose:
    // §11.4's point is that ids do not survive, so a client that expected a
    // particular one would be encoding the thing that is not true.
    CHECK(rd2 && rd2->live, "with an id issued by the engine that is running now "
          "(was %u, is %u)", oldId, rd2 ? rd2->id : 0u);
    CHECK(!es.playing, "and the transport came back STOPPED, per §4.4's honest default");

    // The clip table survived as a memcpy — no decode, no offset change — and
    // the master fader came back off the scalar shadow, which is the only
    // record of it on this path. Launch it again (the transport is stopped and
    // the launch was deliberately NOT replayed) and listen.
    CHECK(eng.send(Cmd::SetPlaying, 1), "start the transport again");
    CHECK(eng.send(Cmd::LaunchClip, 0, 0), "and relaunch the clip");
    const f32 rePeak = peakOver(eng, es, 250);
    CHECK(rePeak > 0.15f,
          "the republished clip sounds again with no decode: %.4f", (double)rePeak);
    CHECK(rePeak < dryPeak * 0.85f,
          "and at the 0.5 master the scalar shadow replayed: %.4f vs the 1.0 "
          "master's %.4f — a respawned engine is told the mixer again, because "
          "App has no idea one was replaced", (double)rePeak, (double)dryPeak);

    // --- refusals are counted, not silent ----------------------------------
    const u64 before = eng.remoteRefusals();
    Command arr;
    arr.type = Cmd::SetArrangement; arr.a = 0; arr.p = (void*)0x1;
    CHECK(eng.pushCommand(arr),
          "Cmd::SetArrangement is CONSUMED, not answered false — a permanent "
          "false would wedge App's retry FIFO for ever");
    CHECK(eng.remoteRefusals() >= before + 1,
          "and counted (%llu -> %llu)",
          (unsigned long long)before, (unsigned long long)eng.remoteRefusals());
    CHECK(eng.snapshotTears() == 0, "no snapshot failed the seqlock (%llu)",
          (unsigned long long)eng.snapshotTears());

    // --- close --------------------------------------------------------------
    eng.close();
    sleepMs(500);
    CHECK(countShm(session) == 0,
          "close() stopped the daemon it spawned and unlinked both regions "
          "(%d left in /dev/shm)", countShm(session));

    // =====================================================================
    // The other two backings, so the new accessors are not daemon-only
    // =====================================================================
    banner("the local and the degraded backings");
    {
        // §8's exception: a handle that opened NOTHING is a supported state,
        // not an error. The GUI still loads, edits and saves; every send() is a
        // no-op and the banner says why.
        EngineHandle none;
        EngineState nes;
        none.poll(nes);
        CHECK(none.link() == EngineLink::Detached && nes.link == EngineLink::Detached,
              "an unopened handle is Detached in both the accessor and the snapshot");
        CHECK(engineLinkBanner(nes.link) != nullptr,
              "which draws '%s'", engineLinkBanner(nes.link));
        CHECK(!none.send(Cmd::SetPlaying, 1), "and every send() is a no-op");
        CHECK(none.remoteDevice(nullptr) == nullptr && none.catalog().empty() &&
              !none.restartEngine(),
              "with no devices, no catalog and nothing to restart");
    }
    {
        // "null" matches neither backend name, so createBackend() returns
        // nothing and openLocalEngine() prepares the engine silent — which is
        // the whole of what this needs: an in-process Engine to be Live about.
        EngineHandle loc;
        EngineState les;
        CHECK(loc.openLocalEngine("null"), "openLocalEngine() opened an in-process engine");
        loc.poll(les);
        CHECK(loc.local() != nullptr && loc.link() == EngineLink::Live &&
              les.link == EngineLink::Live,
              "local mode is Live: an in-process engine cannot be stale (it is "
              "this process) and cannot be lost (it dies with us)");
        CHECK(les.devicesPending == 0 && loc.devicesPending() == 0,
              "nothing is ever pending locally — instantiation is synchronous there");
        CHECK(loc.catalog().empty() && !loc.catalogReady() && !loc.requestScan(),
              "and the catalog is empty on purpose: App's own PluginRegistry IS "
              "the local engine's registry, so a second copy would be two things "
              "to keep in step for no gain");
        CHECK(!loc.restartEngine(), "restartEngine() is a daemon-only idea");
        loc.close();
    }

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
