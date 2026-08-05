#include "engine.h"
#include "../plugin/host.h"
#include <chrono>
#include <cstring>

namespace lat {

static constexpr f64 kPi = 3.14159265358979323846;
static constexpr f64 kEps = 1e-9;

// Sentinel for "this clip has no follow action pending". See the note on
// Track::fireBeat's double duty below.
static constexpr f64 kNoFollow = 1e300;

// Messages pulled off the MIDI ring per block. The ring holds 1024; taking a
// quarter of it at a time bounds the stack cost (2 KB) and the work a single
// callback can be handed, and anything left over is simply one block late.
static constexpr int kMidiPerBlock = 256;

// ---------------------------------------------------------------------------
// generative scheduling: deterministic "randomness"
//
// engine.h is a frozen contract with no room for an RNG state member, but a
// stateful generator would have been the wrong tool anyway: its output depends
// on how many times it has been called, which depends on the buffer size and
// on the order commands happened to arrive in. Hashing the *musical event*
// instead — track, slot, and the beat the launch was scheduled for — gives a
// value that is uniform, allocation free, branch-light, and identical for the
// same event no matter how the audio was chopped up. That is what makes an
// offline render of a probabilistic set reproducible, and what lets a test
// assert that two runs agree sample for sample.
// ---------------------------------------------------------------------------

static inline u64 mix64(u64 x) {                  // splitmix64 finaliser
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// `domain` separates independent draws that share an event (the launch roll and
// the Random follow pick must not correlate).
static inline f64 randUnit(int domain, int track, int slot, f64 beat) {
    // 960 ticks per beat is finer than the smallest quantum we schedule on
    // (1/32 note = 0.125 beats = 120 ticks), so distinct events never collide.
    const u64 ticks = (u64)(i64)std::llround(beat * 960.0);
    const u64 k = mix64(((u64)(u32)domain << 56) ^ ((u64)(u32)track << 44) ^
                        ((u64)(u32)(slot + 1) << 32) ^ ticks);
    return (f64)(k >> 11) * (1.0 / 9007199254740992.0);   // 53 bits -> [0,1)
}

// Live semantics: a failed roll cancels the launch and leaves whatever was
// playing alone. prob >= 1 skips the draw entirely so the common case is free.
static inline bool rollLaunch(const RtClip& c, int track, int slot, f64 beat) {
    if (c.prob >= 1.0) return true;
    if (c.prob <= 0.0) return false;
    return randUnit(1, track, slot, beat) < c.prob;
}

// Beat at which `c`'s follow action comes due, given it launched at `at`.
static inline f64 followDueBeat(const RtClip& c, f64 at) {
    if (c.followAction <= (int)Follow::None || c.followAction >= kFollowCount) return kNoFollow;
    const f64 len = c.followBeats > 0.0 ? c.followBeats : c.lengthBeats;
    if (len <= 0.0) return kNoFollow;
    return at + len;
}

// Which slot a follow action lands on, or -1 for "nothing to launch". `row` is
// one track's slots. Stop is handled by the caller; it queues a stop, not a
// launch.
static int followTarget(const RtClip* row, int cur, int action, int track, f64 beat) {
    switch ((Follow)action) {
    case Follow::Again: return row[cur].valid ? cur : -1;
    case Follow::First: return row[0].valid ? 0 : -1;
    case Follow::Next:
    case Follow::Previous: {
        // Walking backwards is the same walk with a step of -1 taken mod the
        // slot count, so both directions share one wrap-around loop.
        const int step = (action == (int)Follow::Next) ? 1 : kMaxScenes - 1;
        for (int k = 1; k < kMaxScenes; ++k) {
            const int s = (cur + step * k) % kMaxScenes;
            if (row[s].valid) return s;
        }
        return -1;
    }
    case Follow::Random: {
        // Uniform over the valid slots, repeats included: Live's Random can
        // pick the clip that is already playing and that is musically useful.
        int valid[kMaxScenes];
        int nv = 0;
        for (int s = 0; s < kMaxScenes; ++s) if (row[s].valid) valid[nv++] = s;
        if (nv == 0) return -1;
        int idx = (int)(randUnit(2, track, cur, beat) * (f64)nv);
        if (idx >= nv) idx = nv - 1;              // guards the 1.0 corner case
        return valid[idx];
    }
    default: return -1;
    }
}

// ---------------------------------------------------------------------------
// recording: the one piece of state engine.h has no room for
//
// A RecordSlot aimed at a *different* slot while a take is running has to keep
// two buffers alive at once: the old one until the quantized boundary, the new
// one from it. Track carries exactly one set of recording fields and the header
// is frozen, so the extra request lives here. It is written and consumed on the
// audio thread only, never allocates, is bounded by the track count, and is
// tagged with its Engine so two instances in one process (which only happens in
// tests) can never read each other's pending take.
// ---------------------------------------------------------------------------
namespace {
struct PendingRec {
    const void* owner = nullptr;
    f32* buf  = nullptr;
    i64  cap  = 0;
    int  slot = -1;
};
PendingRec gPendingRec[kMaxTracks];
}

// Hands a finished take back to the GUI and returns the track to idle. Track is
// a private nested type and engine.h cannot be touched, so this deduces it
// through a template rather than naming it.
template <class TrackT, class EvRing>
static void finishRec(int ti, TrackT& t, EvRing& evts) {
    evts.push({Ev::RecordFinished, ti, t.recSlot, (f64)t.recLen, (void*)t.recBuf});
    t.recBuf = nullptr;
    t.recCap = 0;
    t.recLen = 0;
    t.recSlot = -1;
    t.recPhase = 0;
    t.recFireBeat = 0.0;
}

// ---------------------------------------------------------------------------
// sample fetch
// ---------------------------------------------------------------------------

// Reads one interpolated stereo frame at `pos` (source frames), wrapping into
// the clip's loop region. Realtime safe, branch-light.
static inline void fetch(const RtClip& c, f64 pos, f32& outL, f32& outR) {
    const f64 ls = (f64)c.loopStart, le = (f64)c.loopEnd;
    if (c.loop && le > ls) {
        const f64 len = le - ls;
        pos = pos - ls;
        pos -= std::floor(pos / len) * len;
        pos += ls;
    }
    if (pos < 0.0 || pos >= (f64)c.frames) { outL = outR = 0.f; return; }
    const i64 i0 = (i64)pos;
    // The partner sample for interpolation has to wrap back to the loop start,
    // otherwise the last frame before every wrap interpolates towards silence
    // and short loops tick once per cycle.
    i64 i1 = i0 + 1;
    if (c.loop && le > ls && i1 >= c.loopEnd) i1 = c.loopStart;
    if (i1 >= c.frames) i1 = i0;

    const f32 fr = (f32)(pos - (f64)i0);
    const f32* p0 = c.data + (size_t)i0 * c.channels;
    const f32* p1 = c.data + (size_t)i1 * c.channels;
    if (c.channels >= 2) {
        outL = p0[0] + (p1[0] - p0[0]) * fr;
        outR = p0[1] + (p1[1] - p0[1]) * fr;
    } else {
        outL = outR = p0[0] + (p1[0] - p0[0]) * fr;
    }
}

// ---------------------------------------------------------------------------

// Track::fireBeat does double duty, and this is the one place to look for why.
// While something is queued (Track::queued != -2) it is the beat that queued
// action fires on, exactly as before. While nothing is queued and a clip is
// playing it holds the beat that clip's *follow action* comes due on, or
// kNoFollow when it has none. The two never overlap: queuing anything (a user
// launch, a scene, a stop) supersedes a pending follow, which is the behaviour
// we want anyway. engine.h is frozen, so a dedicated member was not an option.
void Engine::prepare(f64 sampleRate, int /*maxBlock*/) {
    sr_ = sampleRate;
    for (int t = 0; t < kMaxTracks; ++t) {
        tracks_[t] = Track{};
        tracks_[t].fireBeat = kNoFollow;
        gPendingRec[t] = PendingRec{};
        activeSlot[t].store(-1);
        pendingSlot[t].store(-2);
        slotState[t].store((int)SlotState::Stopped);
        clipPhase[t].store(0.0);
        meterL[t].store(0.f); meterR[t].store(0.f);
        recState[t].store(0);
        recSlotIdx[t].store(-1);
        for (int s = 0; s < kMaxScenes; ++s) clips_[t][s] = RtClip{};
    }
    LOGI("engine prepared @ %.0f Hz", sr_);
}

f64 Engine::nextQuantum(f64 fromBeat, int qIdx) const {
    const int idx = (qIdx < 0) ? quantum_ : qIdx;
    if (idx <= 0 || idx >= kQuantumCount) return fromBeat;
    const f64 q = kQuantumBeats[idx];
    if (q <= 0.0) return fromBeat;
    return std::ceil(fromBeat / q - kEps) * q;
}

void Engine::startVoice(Track& t, const RtClip& c) {
    // Hand the outgoing clip to the release slot so a same-track switch
    // crossfades instead of hard-cutting mid-waveform.
    if (t.voice.active) {
        t.prev = t.voice;
        t.prev.releasing = true;
    }
    Voice& v = t.voice;
    v.clip   = &c;
    v.active = true;
    v.srcPos = (f64)c.loopStart;
    v.readA  = v.srcPos;
    v.readB  = v.srcPos;
    v.phase  = 0;
    v.env    = 0.f;
    v.releasing = false;
    // Grain hop of one 1/16 note keeps transients intact, which is what makes
    // Beats-mode warping sound like a beat repeat rather than a smear.
    const f64 sixteenth = (60.0 / tempo_) * 0.25 * sr_;
    v.hop = (int)clampv(sixteenth, 512.0, 16384.0);
}

void Engine::drainCommands() {
    Command c;
    while (cmds_.pop(c)) {
        switch (c.type) {
        case Cmd::SetPlaying:
            if (!c.a) {
                playing_ = false;
                beat_ = 0.0;
                for (int ti = 0; ti < kMaxTracks; ++ti) {
                    Track& t = tracks_[ti];
                    if (t.voice.active) t.voice.releasing = true;
                    if (t.prev.active)  t.prev.releasing = true;
                    t.playing = -1; t.queued = -2;
                    t.fireBeat = kNoFollow;
                    // Recording needs the clock, so stopping the transport ends
                    // any take on the spot rather than at some boundary that is
                    // never going to arrive. The GUI still gets whatever was
                    // captured; a short take beats a lost one.
                    gPendingRec[ti] = PendingRec{};
                    if (t.recPhase == 2 || t.recPhase == 3) finishRec(ti, t, evts_);
                    else if (t.recPhase == 1) { t.recPhase = 0; t.recBuf = nullptr;
                                                t.recCap = 0; t.recSlot = -1; }
                }
                evts_.push({Ev::TransportStopped, 0, 0, 0.0});
            } else if (!playing_) {
                playing_ = true;
                beat_ = 0.0;
            }
            break;
        case Cmd::SetTempo:     tempo_ = clampv(c.x, 20.0, 999.0); break;
        case Cmd::SetQuantum:   quantum_ = clampv(c.a, 0, kQuantumCount - 1); break;
        case Cmd::SetMetronome: metronome_ = c.a != 0; break;

        case Cmd::LaunchClip: {
            if (c.a < 0 || c.a >= kMaxTracks || c.b < 0 || c.b >= kMaxScenes) break;
            const RtClip& cl = clips_[c.a][c.b];
            if (!cl.valid) break;
            if (!playing_) { playing_ = true; beat_ = 0.0; }
            Track& t = tracks_[c.a];
            t.queued = c.b;
            t.fireBeat = nextQuantum(beat_, cl.quantumIdx);
            break;
        }
        case Cmd::StopTrack: {
            if (c.a < 0 || c.a >= kMaxTracks) break;
            Track& t = tracks_[c.a];
            // Stopping a track is also how you end a take on it, so this runs
            // before the "nothing to stop" bail-out below.
            if (t.recPhase == 1) {
                t.recPhase = 0; t.recBuf = nullptr; t.recCap = 0; t.recSlot = -1;
                gPendingRec[c.a] = PendingRec{};
            } else if (t.recPhase == 2) {
                t.recPhase = 3;
                t.recFireBeat = nextQuantum(beat_, -1);
            }
            if (t.playing < 0 && t.queued == -2) break;
            t.queued = -1;
            t.fireBeat = nextQuantum(beat_, -1);
            break;
        }
        case Cmd::LaunchScene: {
            if (c.a < 0 || c.a >= kMaxScenes) break;
            if (!playing_) { playing_ = true; beat_ = 0.0; }
            const f64 fire = nextQuantum(beat_, -1);
            for (int ti = 0; ti < kMaxTracks; ++ti) {
                Track& t = tracks_[ti];
                const RtClip& cl = clips_[ti][c.a];
                // An empty slot in the scene stops that track, matching Live.
                if (cl.valid)                            { t.queued = c.a; t.fireBeat = fire; }
                else if (t.playing >= 0 || t.queued >= 0) { t.queued = -1;  t.fireBeat = fire; }
            }
            break;
        }
        case Cmd::StopAll: {
            const f64 fire = nextQuantum(beat_, -1);
            for (auto& t : tracks_)
                if (t.playing >= 0 || t.queued >= 0) { t.queued = -1; t.fireBeat = fire; }
            break;
        }

        case Cmd::SetClip:
            if (c.a >= 0 && c.a < kMaxTracks && c.b >= 0 && c.b < kMaxScenes)
                clips_[c.a][c.b] = c.clip;
            break;
        // A pointer swap and nothing else. The audio thread must never free a
        // chain or a PluginInstance, so the displaced chain rides an event back
        // to the GUI, which owns the memory and is the only side allowed to
        // release it — and only once this event proves we are no longer in it.
        case Cmd::SetChain: {
            if (c.a < 0 || c.a >= kMaxTracks) break;
            Track& t = tracks_[c.a];
            const RtChain* old = t.chain;
            t.chain = (const RtChain*)c.p;
            if (old) evts_.push({Ev::ChainRetired, c.a, 0, 0.0, (void*)old});
            break;
        }

        case Cmd::ClearClip:
            if (c.a >= 0 && c.a < kMaxTracks && c.b >= 0 && c.b < kMaxScenes) {
                Track& t = tracks_[c.a];
                if (t.playing == c.b) { t.voice.releasing = true; t.playing = -1;
                                        t.fireBeat = kNoFollow; }
                if (t.queued  == c.b) { t.queued = -2; t.fireBeat = kNoFollow; }
                clips_[c.a][c.b] = RtClip{};
            }
            break;

        // Toggle protocol, per the contract in engine.h. Everything here only
        // *schedules*; the phase changes themselves happen on the grid line in
        // fireDue(), so a take always starts and ends in time.
        case Cmd::RecordSlot: {
            if (c.a < 0 || c.a >= kMaxTracks || c.b < 0 || c.b >= kMaxScenes) break;
            Track& t   = tracks_[c.a];
            f32*   buf = (f32*)c.p;
            const i64 cap = (i64)c.x;

            if (t.recPhase == 0) {
                if (!buf || cap <= 0) break;
                // A take needs a running clock. Arm the transport exactly the
                // way LaunchClip does so the first grid line is beat 0.
                if (!playing_) { playing_ = true; beat_ = 0.0; }
                t.recBuf = buf; t.recCap = cap; t.recLen = 0;
                t.recSlot = c.b; t.recPhase = 1;
                t.recFireBeat = nextQuantum(beat_, -1);
            } else if (t.recSlot == c.b) {
                // Toggling a take that has not begun cancels it. There is no
                // buffer to hand back, so no event goes out either.
                if (t.recPhase == 1) {
                    t.recPhase = 0; t.recBuf = nullptr; t.recCap = 0; t.recSlot = -1;
                    gPendingRec[c.a] = PendingRec{};
                } else if (t.recPhase == 2) {
                    t.recPhase = 3;
                    t.recFireBeat = nextQuantum(beat_, -1);
                }
                // phase 3: a stop is already on the grid, nothing to add.
            } else {
                if (!buf || cap <= 0) break;
                if (t.recPhase == 1) {
                    // Nothing captured yet, so this is just a retarget.
                    t.recBuf = buf; t.recCap = cap; t.recSlot = c.b;
                    t.recFireBeat = nextQuantum(beat_, -1);
                } else {
                    // Hand-over: the running take ends on the same grid line
                    // the new one begins, so the two are gapless and both land
                    // on the beat.
                    t.recPhase = 3;
                    t.recFireBeat = nextQuantum(beat_, -1);
                    gPendingRec[c.a] = PendingRec{this, buf, cap, c.b};
                }
            }
            break;
        }

        // Both ends are checked: a negative index here would write out of
        // bounds on the audio thread, which is not survivable.
        default: {
            const bool trackOk = c.a >= 0 && c.a < kMaxTracks;
            const bool slotOk  = trackOk && c.b >= 0 && c.b < kMaxScenes;
            switch (c.type) {
            case Cmd::TrackVol:  if (trackOk) tracks_[c.a].vol  = (f32)c.x; break;
            case Cmd::TrackPan:  if (trackOk) tracks_[c.a].pan  = (f32)clampv(c.x, -1.0, 1.0); break;
            case Cmd::TrackMute: if (trackOk) tracks_[c.a].mute = c.b != 0; break;
            case Cmd::TrackSolo: if (trackOk) tracks_[c.a].solo = c.b != 0; break;
            case Cmd::TrackArm:  if (trackOk) tracks_[c.a].arm  = c.b != 0; break;
            case Cmd::MasterVol: masterVol_ = (f32)c.x; break;
            case Cmd::ClipGain:  if (slotOk) clips_[c.a][c.b].gain = (f32)c.x; break;
            case Cmd::ClipWarp:  if (slotOk) clips_[c.a][c.b].warp = (int)c.x; break;
            case Cmd::ClipLoop:  if (slotOk) clips_[c.a][c.b].loop = c.x != 0.0; break;
            default: break;
            }
            break;
        }
        }
    }
}

void Engine::fireDue(f64 atBeat) {
    // 1. Recording boundaries. Independent of clip scheduling, but on the same
    //    grid, so they are resolved in the same sub-block pass.
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (t.recPhase != 1 && t.recPhase != 3) continue;
        if (t.recFireBeat > atBeat + kEps) continue;

        if (t.recPhase == 1) {
            t.recPhase = 2;
            t.recLen = 0;
            // The *scheduled* beat, not the sub-block one: they differ by a
            // fraction of a frame and the GUI wants the grid line.
            evts_.push({Ev::RecordStarted, ti, t.recSlot, t.recFireBeat});
        } else {
            const f64 boundary = t.recFireBeat;
            finishRec(ti, t, evts_);
            // A take displaced by a RecordSlot into another slot hands over
            // here, on the very same grid line it stopped on.
            PendingRec& p = gPendingRec[ti];
            if (p.owner == this && p.buf) {
                t.recBuf = p.buf; t.recCap = p.cap; t.recLen = 0;
                t.recSlot = p.slot; t.recPhase = 2; t.recFireBeat = boundary;
                p = PendingRec{};
                evts_.push({Ev::RecordStarted, ti, t.recSlot, boundary});
            }
        }
    }

    // 2. Follow actions. A due follow *schedules* like any user launch — same
    //    quantum, same probability gate — rather than switching clips itself,
    //    which is what keeps a chain of follows locked to the grid.
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (t.queued != -2) continue;             // a queued action supersedes
        if (t.playing < 0 || t.playing >= kMaxScenes) continue;
        if (t.fireBeat >= kNoFollow || t.fireBeat > atBeat + kEps) continue;

        // Quantize from the exact due beat rather than from the sub-block beat
        // we happened to notice it on: with quantum None a chain of Again
        // follows would otherwise creep forward by a fraction of a frame per
        // repeat and slowly walk off the grid.
        const f64 due = t.fireBeat;
        const RtClip& cur = clips_[ti][t.playing];
        const int action = cur.followAction;
        if (action == (int)Follow::Stop) {
            t.queued = -1;
            t.fireBeat = nextQuantum(due, -1);
            continue;
        }
        const int target = followTarget(clips_[ti], t.playing, action, ti, due);
        if (target < 0) {
            t.fireBeat = kNoFollow;               // nowhere to go: stop asking
            continue;
        }
        t.queued = target;
        t.fireBeat = nextQuantum(due, clips_[ti][target].quantumIdx);
    }

    // 3. Queued launches and stops.
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (t.queued == -2) continue;
        if (t.fireBeat > atBeat + kEps) continue;

        // The beat the action was scheduled for, not the sub-block beat we
        // noticed it on. Probability rolls and follow timers both key off this
        // so they stay independent of the buffer size.
        const f64 sched = t.fireBeat;

        if (t.queued == -1) {
            if (t.voice.active) t.voice.releasing = true;
            t.playing = -1;
            t.fireBeat = kNoFollow;
            evts_.push({Ev::TrackStopped, ti, 0, atBeat});
        } else {
            const RtClip& cl = clips_[ti][t.queued];
            if (!cl.valid) {
                if (t.voice.active) t.voice.releasing = true;
                t.playing = -1;
                t.fireBeat = kNoFollow;
            } else if (!rollLaunch(cl, ti, t.queued, sched)) {
                // A failed roll is a no-op, not a stop: whatever is playing
                // keeps playing. Restart that clip's follow timer from this
                // grid line so the dice are rolled again next time round
                // instead of the track going quiet for good.
                t.fireBeat = (t.playing >= 0 && clips_[ti][t.playing].valid)
                                 ? followDueBeat(clips_[ti][t.playing], sched)
                                 : kNoFollow;
                t.queued = -2;
                continue;
            } else {
                startVoice(t, cl);
                t.playing = t.queued;
                t.fireBeat = followDueBeat(cl, sched);
                evts_.push({Ev::ClipStarted, ti, t.queued, atBeat});
            }
        }
        t.queued = -2;
    }
}

// Renders voices into each track's pre-fader scratch for the sub-range
// [from, to). Only clip gain and the declick envelope are applied here: volume,
// pan and mute/solo sit *after* the device chain, and the chain runs once over
// the whole block, so those stages cannot live in this per-sub-block path.
// The metronome is the one thing that goes straight to the master, since it is
// not on any track and must not be coloured by a track's plugins.
void Engine::renderRange(f32* outL, f32* outR, int from, int to) {
    if (to <= from) return;
    const f64 bps = tempo_ / 60.0 / sr_;

    // Ramp lengths for click-free starts and stops.
    const f32 attack  = 1.f / (f32)std::max(1.0, 0.003 * sr_);
    const f32 release = 1.f / (f32)std::max(1.0, 0.006 * sr_);

    // Renders one voice into the track scratch. Called for the live voice and,
    // during a clip switch, for the outgoing one that is still fading out.
    auto renderVoice = [&](Track& t, Voice& v, int ti, bool primary) {
        if (!v.active || !v.clip) return;

        const RtClip& c = *v.clip;

        // Fitting material recorded at clipBpm onto a grid running at tempo_
        // means consuming source frames at tempo_/clipBpm: a 120 BPM loop in a
        // 240 BPM set has to be read twice as fast to cover the same bar.
        const f64 rate = (c.warp == (int)Warp::Off) ? 1.0 : (tempo_ / c.clipBpm);
        const bool granular = (c.warp == (int)Warp::Beats) && std::fabs(rate - 1.0) > 1e-4;
        const f64 loopLen = (f64)(c.loopEnd - c.loopStart);

        for (int i = from; i < to; ++i) {
            f32 l, r;
            if (granular) {
                // Two-grain overlap-add with a complementary raised cosine.
                // Read heads run at natural speed; only the grain *origin*
                // moves at `rate`, so pitch is preserved.
                const f32 w  = 0.5f - 0.5f * std::cos((f32)(kPi * v.phase / (f64)v.hop));
                f32 aL, aR, bL, bR;
                fetch(c, v.readA, aL, aR);
                fetch(c, v.readB, bL, bR);
                l = aL * (1.f - w) + bL * w;
                r = aR * (1.f - w) + bR * w;
                v.readA += 1.0;
                v.readB += 1.0;
                if (++v.phase >= v.hop) { v.readA = v.readB; v.readB = v.srcPos; v.phase = 0; }
            } else {
                fetch(c, v.srcPos, l, r);
            }

            // Advance the musical position.
            v.srcPos += rate;
            if (v.srcPos >= (f64)c.loopEnd) {
                if (c.loop && loopLen > 0.0) {
                    v.srcPos = (f64)c.loopStart + std::fmod(v.srcPos - (f64)c.loopStart, loopLen);
                    if (!granular) { v.readA = v.srcPos; v.readB = v.srcPos; v.phase = 0; }
                } else {
                    v.releasing = true;
                }
            }

            // Declick envelope.
            if (v.releasing) { v.env -= release; if (v.env <= 0.f) { v.env = 0.f; } }
            else if (v.env < 1.f) { v.env += attack; if (v.env > 1.f) v.env = 1.f; }

            // Pre-fader: clip gain and declick only. Both voices of a track sum
            // into the same scratch, which is what makes the crossfade work.
            const f32 g = c.gain * v.env;
            t.fxL[i] += l * g;
            t.fxR[i] += r * g;
        }

        if (v.releasing && v.env <= 0.f) {
            v.active = false;
            v.clip = nullptr;
            if (primary) evts_.push({Ev::ClipStopped, ti, 0, 0.0});
        }
        // Only the live voice drives the UI progress bar; the fading one is
        // already off-screen as far as the grid is concerned.
        if (primary && loopLen > 0.0)
            clipPhase[ti].store((v.srcPos - (f64)c.loopStart) / loopLen, std::memory_order_relaxed);
    };

    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        renderVoice(t, t.prev, ti, false);
        renderVoice(t, t.voice, ti, true);
    }

    // Metronome, rendered last so it sits on top of the mix.
    if (metronome_) {
        for (int i = from; i < to; ++i) {
            const f64 b = beat_ + (f64)i * bps;
            const i64 fl = (i64)std::floor(b);
            if (fl != (i64)std::floor(b - bps)) {
                metCountdown_ = (int)(0.03 * sr_);
                metPhase_ = 0.0;
                metFreq_ = (fl % sigNum_ == 0) ? 1600.f : 800.f;
            }
            if (metCountdown_ > 0) {
                const f32 decay = (f32)metCountdown_ / (f32)(0.03 * sr_);
                const f32 s = (f32)std::sin(metPhase_) * 0.25f * decay * decay;
                metPhase_ += 2.0 * kPi * metFreq_ / sr_;
                outL[i] += s; outR[i] += s;
                --metCountdown_;
            }
        }
    }
}

void Engine::process(const f32* inL, const f32* inR, f32* outL, f32* outR, int nframes) {
    const auto t0 = std::chrono::steady_clock::now();

    drainCommands();
    std::memset(outL, 0, (size_t)nframes * sizeof(f32));
    std::memset(outR, 0, (size_t)nframes * sizeof(f32));

    // The per-track scratch is sized kMaxBlock. Growing it here would mean
    // allocating on the audio thread, so an oversized block renders what fits
    // and leaves the remainder silent rather than running off the end.
    const int n = nframes < kMaxBlock ? nframes : kMaxBlock;
    if (n <= 0) { publish(); return; }

    // One MIDI drain for the whole block: every armed track sees the same list,
    // so pulling it per track would only cost the ring an extra pass. Anything
    // past the cap stays queued and arrives one block later.
    MidiMsg midi[kMidiPerBlock];
    int midiCount = 0;
    { MidiMsg m; while (midiCount < kMidiPerBlock && midi_.pop(m)) midi[midiCount++] = m; }

    // Decide up front which tracks take part in this block, and clear their
    // scratch before any voice writes into it. A track is live if it has audio
    // now, will have audio before the block ends (a launch is queued, or a
    // follow action is pending and may queue one part-way through this very
    // block), owns a chain, or is armed — a chain has to keep running on
    // silence so reverb tails and monitoring survive both the transport
    // stopping and the clip ending, and an armed track has to run so its input
    // is heard. Getting the follow case wrong here is silent: the scratch would
    // go uncleared and the whole track would be skipped for the block the
    // follow fires in.
    bool live[kMaxTracks];
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        live[ti] = t.voice.active || t.prev.active || t.queued != -2 || t.arm ||
                   (t.playing >= 0 && t.fireBeat < kNoFollow) ||
                   (t.chain && t.chain->count > 0);
        if (live[ti]) {
            std::memset(t.fxL, 0, (size_t)n * sizeof(f32));
            std::memset(t.fxR, 0, (size_t)n * sizeof(f32));
        }
    }

    // Appends [from, to) of the capture input to every take in progress. This
    // runs inside the sub-block loop so a take starts and ends on the exact
    // frame its grid line falls on, not merely on a block boundary. What lands
    // in the buffer is the raw input: monitoring is a listening path, the take
    // is the source, and putting the chain between them would bake the devices
    // into the recording.
    auto captureRange = [&](int from, int to) {
        for (int ti = 0; ti < kMaxTracks; ++ti) {
            Track& t = tracks_[ti];
            // Phase 3 is "stop queued", not "stopped": the take keeps taking
            // until the grid line actually arrives, which is the whole point of
            // quantizing the stop.
            if ((t.recPhase != 2 && t.recPhase != 3) || !t.recBuf) continue;
            int i = from;
            for (; i < to && t.recLen < t.recCap; ++i) {
                const size_t o = (size_t)t.recLen * 2;
                t.recBuf[o]     = inL ? inL[i] : 0.f;
                t.recBuf[o + 1] = inR ? inR[i] : 0.f;
                ++t.recLen;
            }
            // The engine cannot grow a GUI-owned buffer and must not write past
            // it, so a full buffer ends the take here and now.
            if (t.recLen >= t.recCap) finishRec(ti, t, evts_);
        }
    };

    if (playing_) {
        const f64 bps = tempo_ / 60.0 / sr_;
        const f64 blockEnd = beat_ + (f64)n * bps;
        int pos = 0;
        while (pos < n) {
            const f64 curBeat = beat_ + (f64)pos * bps;
            fireDue(curBeat);

            // Next scheduled boundary inside this block, if any: a queued
            // launch or stop, a due follow action, or a record start/stop.
            f64 nextB = blockEnd;
            auto consider = [&](f64 b) { if (b > curBeat && b < nextB) nextB = b; };
            for (const auto& t : tracks_) {
                if (t.queued != -2) consider(t.fireBeat);
                else if (t.playing >= 0 && t.fireBeat < kNoFollow) consider(t.fireBeat);
                if (t.recPhase == 1 || t.recPhase == 3) consider(t.recFireBeat);
            }

            int upto = (int)std::ceil((nextB - beat_) / bps);
            upto = clampv(upto, pos + 1, n);
            renderRange(outL, outR, pos, upto);
            captureRange(pos, upto);
            pos = upto;
        }
        beat_ += (f64)n * bps;
    } else {
        // Voices still get a release tail so stopping never clicks.
        bool anyTail = false;
        for (auto& t : tracks_) if (t.voice.active || t.prev.active) { anyTail = true; break; }
        if (anyTail) renderRange(outL, outR, 0, n);
    }

    // Per-track post stage. The launch-boundary loop above splits *voice*
    // rendering only; everything from here runs exactly once over the whole
    // block, because a plugin must see one contiguous run per callback and
    // because a fader change mid-block would be a click either way.
    bool anySolo = false;
    for (const auto& t : tracks_) if (t.solo) { anySolo = true; break; }

    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (!live[ti]) continue;

        // Input monitoring, pre-chain: an armed track hears its input through
        // its own devices, so what you hear while recording is what the take
        // will sound like once it is played back through the same chain.
        if (t.arm && (inL || inR)) {
            for (int i = 0; i < n; ++i) {
                t.fxL[i] += inL ? inL[i] : 0.f;
                t.fxR[i] += inR ? inR[i] : 0.f;
            }
        }

        if (t.chain && t.chain->count > 0) {
            const int cnt = t.chain->count < kMaxChainFx ? t.chain->count : kMaxChainFx;

            // MIDI goes in before the chain runs, because a note event has to
            // reach an instrument in time for the block it belongs to. Only
            // armed tracks receive, and only devices that asked for notes:
            // handing a reverb a note-on would be noise on the wire.
            if (midiCount > 0 && t.arm) {
                for (int fi = 0; fi < cnt; ++fi) {
                    PluginInstance* fx = t.chain->fx[fi];
                    if (!fx) continue;
                    const PluginDesc& d = fx->desc();
                    if (!d.hasMidiIn && d.kind != PluginKind::Instrument) continue;
                    for (int mi = 0; mi < midiCount; ++mi) {
                        const MidiMsg& m = midi[mi];
                        const u8 bytes[3] = {m.status, m.d1, m.d2};
                        // Program change and channel pressure are the only two
                        // channel messages with a single data byte.
                        const u8 hi = (u8)(m.status & 0xF0);
                        const int len = (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
                        fx->midi(bytes, len, clampv((int)m.frame, 0, n - 1));
                    }
                }
            }

            // In-place is part of the PluginInstance contract, so the whole
            // chain runs through the one scratch pair with no copies.
            f32* bufs[2] = {t.fxL, t.fxR};
            for (int fi = 0; fi < cnt; ++fi)
                if (PluginInstance* fx = t.chain->fx[fi])
                    fx->process(bufs, bufs, 2, n);
        }

        const bool audible = !t.mute && (!anySolo || t.solo);
        const f32 pgL = t.pan <= 0.f ? 1.f : 1.f - t.pan;
        const f32 pgR = t.pan >= 0.f ? 1.f : 1.f + t.pan;
        const f32 gL = audible ? t.vol * pgL : 0.f;
        const f32 gR = audible ? t.vol * pgR : 0.f;

        // Meters are post-fader: what the user sees is what the master gets.
        f32 pkL = 0.f, pkR = 0.f;
        for (int i = 0; i < n; ++i) {
            const f32 l = t.fxL[i] * gL;
            const f32 r = t.fxR[i] * gR;
            outL[i] += l;
            outR[i] += r;
            const f32 al = std::fabs(l), ar = std::fabs(r);
            if (al > pkL) pkL = al;
            if (ar > pkR) pkR = ar;
        }
        if (pkL > t.mL) t.mL = pkL;
        if (pkR > t.mR) t.mR = pkR;
    }

    // Master bus.
    f32 pkL = 0.f, pkR = 0.f;
    for (int i = 0; i < n; ++i) {
        f32 l = outL[i] * masterVol_;
        f32 r = outR[i] * masterVol_;
        l = clampv(l, -1.f, 1.f);
        r = clampv(r, -1.f, 1.f);
        outL[i] = l; outR[i] = r;
        const f32 al = std::fabs(l), ar = std::fabs(r);
        if (al > pkL) pkL = al;
        if (ar > pkR) pkR = ar;
    }
    if (pkL > masL_) masL_ = pkL;
    if (pkR > masR_) masR_ = pkR;

    publish();

    const auto t1 = std::chrono::steady_clock::now();
    const f64 elapsed = std::chrono::duration<f64>(t1 - t0).count();
    const f64 budget  = (f64)n / sr_;
    const f32 load = (f32)(elapsed / budget * 100.0);
    cpu.store(cpu.load(std::memory_order_relaxed) * 0.9f + load * 0.1f, std::memory_order_relaxed);
}

void Engine::publish() {
    beat.store(beat_, std::memory_order_relaxed);
    playing.store(playing_, std::memory_order_relaxed);
    tempo.store(tempo_, std::memory_order_relaxed);

    constexpr f32 kDecay = 0.72f;
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        meterL[ti].store(t.mL, std::memory_order_relaxed);
        meterR[ti].store(t.mR, std::memory_order_relaxed);
        t.mL *= kDecay; t.mR *= kDecay;

        activeSlot[ti].store(t.playing, std::memory_order_relaxed);
        pendingSlot[ti].store(t.queued, std::memory_order_relaxed);
        SlotState st = SlotState::Stopped;
        if (t.queued >= 0)      st = SlotState::Queued;
        else if (t.queued == -1) st = SlotState::StopQueued;
        else if (t.playing >= 0) st = SlotState::Playing;
        slotState[ti].store((int)st, std::memory_order_relaxed);

        // The published state has three values, not four: a take with a stop
        // already queued (phase 3) is still recording as far as the UI and the
        // user are concerned.
        recState[ti].store(t.recPhase == 3 ? 2 : t.recPhase, std::memory_order_relaxed);
        recSlotIdx[ti].store(t.recSlot, std::memory_order_relaxed);
    }
    masterMeterL.store(masL_, std::memory_order_relaxed);
    masterMeterR.store(masR_, std::memory_order_relaxed);
    masL_ *= kDecay; masR_ *= kDecay;
}

} // namespace lat
