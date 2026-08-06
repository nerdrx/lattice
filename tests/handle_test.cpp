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

using namespace lat;

static int gPass = 0, gFail = 0;
#define CHECK(c, ...) do { if (c) { ++gPass; std::printf("  PASS  "); } \
    else { ++gFail; std::printf("  FAIL  "); } std::printf(__VA_ARGS__); std::printf("\n"); \
    std::fflush(stdout); } while (0)

static void sleepMs(int ms) { timespec t{ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&t,nullptr); }

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

    // --- refusals are counted, not silent ----------------------------------
    const u64 before = eng.remoteRefusals();
    Command chain;
    chain.type = Cmd::SetChain; chain.a = 0; chain.p = nullptr;
    CHECK(!eng.pushCommand(chain), "Cmd::SetChain is refused (devices are step 4)");
    Command arr;
    arr.type = Cmd::SetArrangement; arr.a = 0; arr.p = (void*)0x1;
    CHECK(eng.pushCommand(arr),
          "Cmd::SetArrangement is CONSUMED, not answered false — a permanent "
          "false would wedge App's retry FIFO for ever");
    CHECK(eng.remoteRefusals() >= before + 2,
          "and both are counted (%llu -> %llu)",
          (unsigned long long)before, (unsigned long long)eng.remoteRefusals());
    CHECK(eng.snapshotTears() == 0, "no snapshot failed the seqlock (%llu)",
          (unsigned long long)eng.snapshotTears());

    // --- close --------------------------------------------------------------
    eng.close();
    sleepMs(500);
    CHECK(countShm(session) == 0,
          "close() stopped the daemon it spawned and unlinked both regions "
          "(%d left in /dev/shm)", countShm(session));

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
