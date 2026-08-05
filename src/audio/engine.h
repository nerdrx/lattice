// The realtime engine.
//
// Threading contract:
//   * GUI thread  -> pushCommand()  -> lock-free ring -> audio thread
//   * audio thread -> pushEvent()   -> lock-free ring -> popEvent() on GUI
//   * scalar state the GUI polls (meters, playhead, clip states) lives in
//     std::atomic members.
// The audio thread never allocates, locks, or touches std::string.
#pragma once
#include "../core/common.h"
#include "../core/ring.h"
#include <atomic>

namespace lat {

enum class Warp : int { Off = 0, Repitch = 1, Beats = 2 };

// Clip slot state as seen by the UI.
enum class SlotState : int { Empty = 0, Stopped, Queued, Playing, StopQueued };

// Realtime view of a clip. The GUI fills one of these and ships it across;
// the audio thread only reads. `data` points into a SampleBuffer the GUI
// keeps alive for the lifetime of the session.
struct RtClip {
    const f32* data   = nullptr;   // interleaved, already at engine rate
    i64  frames       = 0;
    int  channels     = 1;
    i64  loopStart    = 0;
    i64  loopEnd      = 0;
    f64  clipBpm      = 120.0;     // tempo of the recorded material
    f64  lengthBeats  = 4.0;       // musical length of [loopStart, loopEnd)
    f32  gain         = 1.0f;
    int  warp         = (int)Warp::Beats;
    bool loop         = true;
    int  quantumIdx   = -1;        // -1 => follow global quantum
    bool valid        = false;
};

enum class Cmd : u32 {
    SetPlaying, SetTempo, SetQuantum, SetMetronome,
    LaunchClip, StopTrack, LaunchScene, StopAll,
    SetClip, ClearClip,
    TrackVol, TrackPan, TrackMute, TrackSolo, TrackArm,
    MasterVol,
    ClipGain, ClipWarp, ClipLoop,
};

struct Command {
    Cmd    type = Cmd::SetPlaying;
    i32    a = 0, b = 0;
    f64    x = 0.0;
    RtClip clip{};
};

enum class Ev : u32 { ClipStarted, ClipStopped, TrackStopped, Xrun, TransportStopped };
struct Event { Ev type = Ev::Xrun; i32 a = 0, b = 0; f64 x = 0.0; };

// Global launch quantum choices, in beats. Index 0 is "None".
inline constexpr f64 kQuantumBeats[] = {0.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125};
inline constexpr const char* kQuantumNames[] = {
    "None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32"
};
inline constexpr int kQuantumCount = 10;

class Engine {
public:
    void prepare(f64 sampleRate, int maxBlock);
    void process(f32* outL, f32* outR, int nframes);   // audio thread only

    bool pushCommand(const Command& c) { return cmds_.push(c); }   // GUI thread
    bool popEvent(Event& e)            { return evts_.pop(e); }    // GUI thread

    // --- polled by the GUI ---------------------------------------------
    std::atomic<f64>  beat{0.0};            // absolute beats since transport start
    std::atomic<bool> playing{false};
    std::atomic<f64>  tempo{120.0};
    std::atomic<f32>  cpu{0.f};
    std::atomic<int>  slotState[kMaxTracks]{};    // SlotState of the *active* slot
    std::atomic<int>  activeSlot[kMaxTracks]{};   // playing slot, -1 if none
    std::atomic<int>  pendingSlot[kMaxTracks]{};  // queued slot, -1 stop, -2 none
    std::atomic<f64>  clipPhase[kMaxTracks]{};    // 0..1 through the running clip
    std::atomic<f32>  meterL[kMaxTracks]{}, meterR[kMaxTracks]{};
    std::atomic<f32>  masterMeterL{0.f}, masterMeterR{0.f};

    f64 sampleRate() const { return sr_; }

private:
    struct Voice {
        const RtClip* clip = nullptr;
        bool  active = false;
        f64   srcPos = 0.0;          // ideal read position, source frames
        f64   readA = 0.0, readB = 0.0;
        int   phase = 0, hop = 1024;
        f32   env = 0.f;             // declick ramp, 0..1
        bool  releasing = false;
    };
    struct Track {
        f32  vol = faderToGain(0.85f);
        f32  pan = 0.f;
        bool mute = false, solo = false, arm = false;
        int  playing = -1;           // slot index currently sounding
        int  queued  = -2;           // -2 none, -1 queued stop, >=0 queued slot
        f64  fireBeat = 0.0;
        Voice voice;                 // the clip currently launched
        Voice prev;                  // outgoing clip, fading out across a switch
        f32  mL = 0.f, mR = 0.f;
    };

    void  drainCommands();
    void  renderRange(f32* outL, f32* outR, int from, int to);
    void  fireDue(f64 atBeat);
    f64   nextQuantum(f64 fromBeat, int qIdx) const;
    void  startVoice(Track& t, const RtClip& c);
    void  publish();

    f64 sr_ = 48000.0;
    f64 tempo_ = 120.0;
    int sigNum_ = 4;
    int quantum_ = 4;               // index into kQuantumBeats -> 1 Bar
    bool playing_ = false;
    bool metronome_ = false;
    f64 beat_ = 0.0;
    f64 metPhase_ = 0.0;
    int metCountdown_ = 0;
    f32 metFreq_ = 0.f;

    f32 masterVol_ = faderToGain(0.85f);
    f32 masL_ = 0.f, masR_ = 0.f;

    RtClip clips_[kMaxTracks][kMaxScenes];
    Track  tracks_[kMaxTracks];

    Ring<Command, 1024> cmds_;
    Ring<Event, 1024>   evts_;
};

} // namespace lat
