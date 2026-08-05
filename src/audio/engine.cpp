#include "engine.h"
#include "../plugin/host.h"
#include <chrono>
#include <cstring>

namespace lat {

static constexpr f64 kPi = 3.14159265358979323846;
static constexpr f64 kEps = 1e-9;

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

void Engine::prepare(f64 sampleRate, int /*maxBlock*/) {
    sr_ = sampleRate;
    for (int t = 0; t < kMaxTracks; ++t) {
        tracks_[t] = Track{};
        activeSlot[t].store(-1);
        pendingSlot[t].store(-2);
        slotState[t].store((int)SlotState::Stopped);
        clipPhase[t].store(0.0);
        meterL[t].store(0.f); meterR[t].store(0.f);
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
                for (auto& t : tracks_) {
                    if (t.voice.active) t.voice.releasing = true;
                    if (t.prev.active)  t.prev.releasing = true;
                    t.playing = -1; t.queued = -2;
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
                if (t.playing == c.b) { t.voice.releasing = true; t.playing = -1; }
                if (t.queued  == c.b) t.queued = -2;
                clips_[c.a][c.b] = RtClip{};
            }
            break;

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
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        if (t.queued == -2) continue;
        if (t.fireBeat > atBeat + kEps) continue;

        if (t.queued == -1) {
            if (t.voice.active) t.voice.releasing = true;
            t.playing = -1;
            evts_.push({Ev::TrackStopped, ti, 0, atBeat});
        } else {
            const RtClip& cl = clips_[ti][t.queued];
            if (cl.valid) {
                startVoice(t, cl);
                t.playing = t.queued;
                evts_.push({Ev::ClipStarted, ti, t.queued, atBeat});
            } else {
                if (t.voice.active) t.voice.releasing = true;
                t.playing = -1;
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

void Engine::process(f32* outL, f32* outR, int nframes) {
    const auto t0 = std::chrono::steady_clock::now();

    drainCommands();
    std::memset(outL, 0, (size_t)nframes * sizeof(f32));
    std::memset(outR, 0, (size_t)nframes * sizeof(f32));

    // The per-track scratch is sized kMaxBlock. Growing it here would mean
    // allocating on the audio thread, so an oversized block renders what fits
    // and leaves the remainder silent rather than running off the end.
    const int n = nframes < kMaxBlock ? nframes : kMaxBlock;
    if (n <= 0) { publish(); return; }

    // Decide up front which tracks take part in this block, and clear their
    // scratch before any voice writes into it. A track is live if it has audio
    // now, will have audio before the block ends (a launch is queued), or owns
    // a chain — a chain has to keep running on silence so reverb tails and
    // monitoring survive both the transport stopping and the clip ending.
    bool live[kMaxTracks];
    for (int ti = 0; ti < kMaxTracks; ++ti) {
        Track& t = tracks_[ti];
        live[ti] = t.voice.active || t.prev.active || t.queued != -2 ||
                   (t.chain && t.chain->count > 0);
        if (live[ti]) {
            std::memset(t.fxL, 0, (size_t)n * sizeof(f32));
            std::memset(t.fxR, 0, (size_t)n * sizeof(f32));
        }
    }

    if (playing_) {
        const f64 bps = tempo_ / 60.0 / sr_;
        const f64 blockEnd = beat_ + (f64)n * bps;
        int pos = 0;
        while (pos < n) {
            const f64 curBeat = beat_ + (f64)pos * bps;
            fireDue(curBeat);

            // Next launch boundary inside this block, if any.
            f64 nextB = blockEnd;
            for (const auto& t : tracks_)
                if (t.queued != -2 && t.fireBeat > curBeat && t.fireBeat < nextB) nextB = t.fireBeat;

            int upto = (int)std::ceil((nextB - beat_) / bps);
            upto = clampv(upto, pos + 1, n);
            renderRange(outL, outR, pos, upto);
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

        if (t.chain && t.chain->count > 0) {
            // In-place is part of the PluginInstance contract, so the whole
            // chain runs through the one scratch pair with no copies.
            f32* bufs[2] = {t.fxL, t.fxR};
            const int cnt = t.chain->count < kMaxChainFx ? t.chain->count : kMaxChainFx;
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
    }
    masterMeterL.store(masL_, std::memory_order_relaxed);
    masterMeterR.store(masR_, std::memory_order_relaxed);
    masL_ *= kDecay; masR_ *= kDecay;
}

} // namespace lat
