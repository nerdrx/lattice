// Engine plumbing: everything that talks to the audio engine — send,
// publishNotes, pushClip/Track/All, releaseStaleSlots, pumpEngineEvents,
// setTempo/togglePlay, and the four recording functions. Moved verbatim
// from app.cpp.
//
#include "app.h"
#include "app_internal.h"
#include "pianoroll.h"
#include "../core/project.h"
#include "../gfx/gl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

namespace lat {

// How much audio a single take can hold. Two minutes of interleaved stereo
// floats is ~46 MB at 48 kHz: cheap enough to allocate up front, long enough
// that no realistic loop or verse runs out of room. The engine stops at the
// capacity it was given, so overrunning truncates rather than corrupts.
constexpr f64 kRecordSeconds = 120.0;

// How many notes a single MIDI take can hold. Four thousand is more than an
// hour of dense playing, and the array is 24 bytes a note, so the whole buffer
// is under 100 kB — cheap enough not to bother sizing it to the material.
constexpr int kRecordNotes = 4096;

// ---------------------------------------------------------------------------
// engine plumbing
// ---------------------------------------------------------------------------

void App::send(Cmd t, i32 a, i32 b, f64 x) {
    Command c;
    c.type = t; c.a = a; c.b = b; c.x = x;
    engine_.pushCommand(c);
}

void App::publishNotes(int track, int slot, const RtNote* fresh) {
    const RtNote* old = publishedNotes_[track][slot];
    publishedNotes_[track][slot] = fresh;
    // The engine only announces a *replaced* array, and only when it differs
    // from the incoming one; an entry that would never be announced must not be
    // queued for a retirement that will never arrive.
    if (old && old != fresh) retiringNotes_.push_back(old);
}

void App::pushClip(int track, int slot) {
    Command c;
    c.type = Cmd::SetClip;
    c.a = track; c.b = slot;
    const ClipModel& m = ses_.tracks[track].slots[slot];
    const bool midi = m.valid() && m.kind == ClipKind::Midi;

    // Snapshot the notes before anything is sent: the engine reads this array
    // for as long as it holds the clip, so it cannot be the GUI's live vector.
    RtNote* fresh = nullptr;
    if (midi && !m.notes.empty()) {
        fresh = new (std::nothrow) RtNote[m.notes.size()];
        if (!fresh) { status_ = "Out of memory - clip not updated"; return; }
        for (size_t i = 0; i < m.notes.size(); ++i) {
            const NoteModel& n = m.notes[i];
            fresh[i].beat  = n.beat;
            fresh[i].len   = n.len;
            fresh[i].pitch = n.pitch;
            fresh[i].vel   = n.vel;
        }
    }

    if (!m.valid()) {
        c.type = Cmd::ClearClip;
        if (!engine_.pushCommand(c)) {
            LOGW("command ring full - slot %d/%d not cleared", track, slot);
            return;
        }
        // A cleared MIDI slot retires its notes exactly like a replaced one.
        publishNotes(track, slot, nullptr);
        clipLive_[track][slot] = false;
        return;
    }

    RtClip rc;
    if (midi) {
        rc.isMidi     = true;
        rc.notes      = fresh;
        rc.noteCount  = (int)m.notes.size();
    } else {
        rc.data       = m.sample->data.data();
        rc.frames     = m.sample->frames;
        rc.channels   = m.sample->channels;
        rc.loopStart  = m.loopStart;
        rc.loopEnd    = m.loopEnd > m.loopStart ? m.loopEnd : m.sample->frames;
        rc.clipBpm    = m.clipBpm;
        rc.warp       = (int)m.warp;
    }
    rc.lengthBeats  = m.lengthBeats;
    rc.gain         = m.gain;
    rc.loop         = m.loop;
    rc.quantumIdx   = m.quantumIdx;
    rc.prob         = m.prob;
    rc.followAction = (int)m.followAction;
    rc.followBeats  = m.followBeats;
    rc.valid        = true;
    c.clip = rc;
    if (!engine_.pushCommand(c)) {
        // The engine never saw the array, so it is still solely ours and the
        // slot keeps whatever it was already playing.
        LOGW("command ring full - slot %d/%d not updated", track, slot);
        delete[] fresh;
        return;
    }
    // Unconditional, not only for MIDI clips: a slot that just turned into an
    // audio clip still has an old note array to hand back.
    publishNotes(track, slot, fresh);
    clipLive_[track][slot] = true;
}

void App::pushTrack(int t) {
    const TrackModel& tr = ses_.tracks[t];
    send(Cmd::TrackVol,  t, 0, faderToGain(tr.fader));
    send(Cmd::TrackPan,  t, 0, tr.pan);
    send(Cmd::TrackMute, t, tr.mute ? 1 : 0);
    send(Cmd::TrackSolo, t, tr.solo ? 1 : 0);
    send(Cmd::TrackArm,  t, tr.arm ? 1 : 0);
    // Sends are part of a track's mixer state like volume and pan are, so they
    // ride the same path: a load, an undo restore and a fresh set all arrive
    // here and nowhere else. The model holds the linear level the engine wants
    // (TrackModel::sends), so nothing is mapped on the way across.
    for (int rn = 0; rn < kMaxReturns; ++rn)
        send(Cmd::SendLevel, t, rn, tr.sends[rn]);
}

void App::pushAll() {
    send(Cmd::SetTempo, 0, 0, ses_.tempo);
    send(Cmd::SetQuantum, ses_.quantumIdx);
    send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    for (size_t t = 0; t < ses_.tracks.size(); ++t) {
        pushTrack((int)t);
        for (int s = 0; s < (int)ses_.scenes.size(); ++s) pushClip((int)t, s);
    }
    // Return levels. Their chains are published by materializeDevices (or by
    // releaseAllChains, which empties them) -- this is the scalar half.
    for (int i = 0; i < kMaxReturns; ++i)
        send(Cmd::ReturnVol, i, 0, faderToGain(ses_.returns[i].fader));
    releaseStaleSlots();
}

// The loop above only reaches slots the current session has a track and a
// scene for. A load or an undo can make the set *smaller*, and a slot that
// falls outside the new one keeps whatever was last pushed into it: an RtClip
// whose sample data belongs to a session we have just stopped owning, and a
// note array nothing will ever retire. Neither is reachable from the grid, but
// Cmd::StopAll and scene launches walk the engine's own tables, so "unreachable
// from the UI" is not the same as "cannot sound".
void App::releaseStaleSlots() {
    const int nt = (int)ses_.tracks.size(), ns = (int)ses_.scenes.size();
    for (int t = 0; t < kMaxTracks; ++t) {
        for (int s = 0; s < kMaxScenes; ++s) {
            if (t < nt && s < ns) continue;
            if (!clipLive_[t][s] && !publishedNotes_[t][s]) continue;
            Command c;
            c.type = Cmd::ClearClip;
            c.a = t; c.b = s;
            if (!engine_.pushCommand(c)) {
                // The engine still holds it, so the flags stay set and the next
                // push (or the next restore) tries again.
                LOGW("command ring full - stale slot %d/%d not cleared", t, s);
                continue;
            }
            clipLive_[t][s] = false;
            publishNotes(t, s, nullptr);
        }
    }
}

void App::pumpEngineEvents() {
    Event e;
    while (engine_.popEvent(e)) {
        if (e.type == Ev::ChainRetired) {
            // The audio thread has swapped this chain out and will never look
            // at it again, so the struct and every instance it was the last
            // reference to can finally go.
            //
            // `e.a` says which owner it came off (a track, kMaxTracks+i for a
            // return, -1 for the master) and is used for nothing but the
            // message below: the pool is keyed on the POINTER, every chain is
            // its own allocation, and no chain is ever published to two owners,
            // so the address adds no information the lookup needs.
            const RtChain* old = (const RtChain*)e.p;
            if (!old) continue;
            auto it = retiring_.begin();
            for (; it != retiring_.end(); ++it) if (it->chain == old) break;
            if (it == retiring_.end()) {
                LOGW("ChainRetired for an unknown chain %p (from %s) - leaking it "
                     "rather than freeing a pointer we do not own",
                     (const void*)old, ownerName(e.a).c_str());
                continue;
            }
            delete it->chain;
            retiring_.erase(it);
            continue;
        }
        if (e.type == Ev::RecordStarted) {
            // The quantized start has fired. Remember the beat it began on so
            // the slot can count elapsed beats without the engine having to
            // publish another atomic.
            if (e.a >= 0 && e.a < kMaxTracks) recStartBeat_[e.a] = e.x;
            char buf[80];
            snprintf(buf, sizeof buf, "Recording %s  scene %d",
                     (e.a >= 0 && e.a < (int)ses_.tracks.size()) ? ses_.tracks[e.a].name.c_str() : "?",
                     e.b + 1);
            status_ = buf;
            continue;
        }
        if (e.type == Ev::RecordFinished) {
            finishRecording(e);
            continue;
        }
        if (e.type == Ev::MidiRecordFinished) {
            finishMidiRecording(e);
            continue;
        }
        if (e.type == Ev::NotesRetired) {
            // The audio thread has stopped reading this array. Same handshake
            // as ChainRetired, and the same refusal to free a pointer we have
            // no record of owning.
            const RtNote* old = (const RtNote*)e.p;
            if (!old) continue;
            auto it = retiringNotes_.begin();
            for (; it != retiringNotes_.end(); ++it) if (*it == old) break;
            if (it == retiringNotes_.end()) {
                LOGW("NotesRetired for an unknown array %p - leaking it rather "
                     "than freeing a pointer we do not own", (const void*)old);
                continue;
            }
            delete[] *it;
            retiringNotes_.erase(it);
            continue;
        }
        // Everything else is reserved for undo hooks; the UI polls atomics for
        // transport and clip state.
    }
}


void App::setTempo(f64 bpm) {
    ses_.tempo = clampv(bpm, 20.0, 999.0);
    send(Cmd::SetTempo, 0, 0, ses_.tempo);
}

void App::togglePlay() {
    const bool p = engine_.playing.load();
    send(Cmd::SetPlaying, p ? 0 : 1);
    status_ = p ? "Stopped" : "Playing";
}

// ---------------------------------------------------------------------------
// recording
//
// Live's semantics: an empty slot on an armed track is a record target while
// the global record button is lit. The first click queues a take (quantized by
// the engine like any launch), the second stops it. The capture buffer is
// allocated here and stays ours until Ev::RecordFinished brings it back; see
// the Cmd::RecordSlot contract in engine.h.
// ---------------------------------------------------------------------------

void App::startRecording(int track, int slot) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;
    if (engine_.recState[track].load() != 0) {
        status_ = "Track is already recording";
        return;
    }

    // What the track can play decides what the take captures. A chain with an
    // instrument on it wants the notes, not the microphone.
    const bool midi = trackHasNoteDevice(track);

    PendingRec pr;
    pr.track = track; pr.slot = slot; pr.midi = midi;
    Command c;
    c.a = track; c.b = slot;

    if (midi) {
        RtNote* notes = new (std::nothrow) RtNote[kRecordNotes]();
        if (!notes) {
            status_ = "Out of memory - recording not started";
            return;
        }
        pr.notes = notes;
        pr.cap = kRecordNotes;
        c.type = Cmd::RecordMidiSlot;
        c.p = notes;
    } else {
        const i64 cap = (i64)std::llround(engine_.sampleRate() * kRecordSeconds);
        // Zeroed rather than raw: a take that stops early leaves the tail
        // unwritten, and silence is a far better failure than whatever was on
        // that page.
        f32* buf = new (std::nothrow) f32[(size_t)cap * 2]();
        if (!buf) {
            status_ = "Out of memory - recording not started";
            return;
        }
        pr.buf = buf;
        pr.cap = cap;
        c.type = Cmd::RecordSlot;
        c.p = buf;
    }
    c.x = (f64)pr.cap;

    if (!engine_.pushCommand(c)) {
        // The engine never saw the buffer, so it is still solely ours.
        delete[] pr.buf;
        delete[] pr.notes;
        status_ = "Engine busy - recording not started";
        return;
    }

    pendingRecs_.push_back(pr);
    selectTrack(track); selSlot_ = slot;
    status_ = midi ? "Record armed (MIDI)" : "Record armed";
}

void App::stopRecording(int track) {
    for (const PendingRec& p : pendingRecs_) {
        if (p.track != track) continue;
        // The same command toggles. Resend the buffer it was started with
        // rather than a null: the stop is a second Record*Slot for this slot,
        // and repeating the payload means an engine that simply reassigns
        // recBuf lands on exactly what it already had.
        Command c;
        c.type = p.midi ? Cmd::RecordMidiSlot : Cmd::RecordSlot;
        c.a = track; c.b = p.slot;
        c.p = p.midi ? (void*)p.notes : (void*)p.buf;
        c.x = (f64)p.cap;
        if (!engine_.pushCommand(c)) status_ = "Engine busy - still recording";
        return;
    }
}

void App::finishRecording(const Event& e) {
    f32* buf = (f32*)e.p;
    if (!buf) return;

    auto it = pendingRecs_.begin();
    for (; it != pendingRecs_.end(); ++it) if (!it->midi && it->buf == buf) break;
    if (it == pendingRecs_.end()) {
        LOGW("RecordFinished for an unknown buffer %p - leaking it rather than "
             "freeing a pointer we do not own", (const void*)buf);
        return;
    }
    if (it->cancelled) {
        // An undo replaced the session this take was aimed at (see
        // cancelTakes). The buffer coming home is the only thing that still had
        // to happen; the material is dropped rather than written into a
        // session that never asked for it.
        delete[] buf;
        pendingRecs_.erase(it);
        return;
    }

    const int track = e.a, slot = e.b;
    const i64 frames = (i64)e.x;
    const bool inRange = track >= 0 && track < (int)ses_.tracks.size() &&
                         slot  >= 0 && slot  < (int)ses_.scenes.size();

    if (frames > 0 && inRange) {
        char name[32];
        snprintf(name, sizeof name, "Rec %d", recTakeNo_++);
        SampleRef sb = sampleFromRecording(buf, frames, engine_.sampleRate(), ses_.tempo, name);
        if (sb) {
            // pushUndoNow rather than undoPoint: this runs while engine events
            // are drained, so a widget the user happens to be dragging still
            // owns ui_.active and would coalesce a take away.
            pushUndoNow("record");
            ClipModel& m = ses_.tracks[track].slots[slot];
            m = ClipModel{};
            m.uid = ses_.newUid();
            m.sample = sb;
            m.name = name;
            m.colorIdx = ses_.tracks[track].colorIdx;
            // The take was played to the session clock, so its tempo is the
            // session tempo by construction - nothing to guess here.
            m.clipBpm = ses_.tempo;
            m.lengthBeats = sb->guessedBeats;
            m.loopStart = 0;
            m.loopEnd = sb->frames;
            m.gain = 1.f;
            m.warp = Warp::Beats;
            m.loop = true;
            pushClip(track, slot);
            selectTrack(track); selSlot_ = slot;
            status_ = std::string("Recorded ") + name;
        } else {
            status_ = "Recording failed";
        }
    } else if (frames <= 0) {
        // Stopped before the quantized start ever fired: nothing was captured,
        // so there is no clip to make and the slot stays empty.
        status_ = "Recording cancelled";
    }

    delete[] buf;
    pendingRecs_.erase(it);
}

// Ev::MidiRecordFinished: the same hand-back as an audio take, but the payload
// is the note buffer and turning it into a clip is a copy rather than a resample.
void App::finishMidiRecording(const Event& e) {
    RtNote* buf = (RtNote*)e.p;
    if (!buf) return;

    auto it = pendingRecs_.begin();
    for (; it != pendingRecs_.end(); ++it) if (it->midi && it->notes == buf) break;
    if (it == pendingRecs_.end()) {
        LOGW("MidiRecordFinished for an unknown buffer %p - leaking it rather "
             "than freeing a pointer we do not own", (const void*)buf);
        return;
    }
    if (it->cancelled) {          // see finishRecording
        delete[] buf;
        pendingRecs_.erase(it);
        return;
    }

    const int track = e.a, slot = e.b;
    const int count = clampv((int)e.x, 0, (int)it->cap);
    const bool inRange = track >= 0 && track < (int)ses_.tracks.size() &&
                         slot  >= 0 && slot  < (int)ses_.scenes.size();

    // One note of the take, in the model's form. Both paths below want exactly
    // this, and the clamps are the ones the format applies anyway.
    const auto asNote = [](const RtNote& r) {
        NoteModel n;
        n.beat  = std::max(0.0, r.beat);
        n.len   = std::max(1.0 / 64.0, r.len);
        n.pitch = (u8)clampv((int)r.pitch, 0, 127);
        n.vel   = (u8)clampv((int)r.vel, 1, 127);
        return n;
    };
    // Everything downstream -- the roll, the RtNote array, the engine's own
    // scheduler -- wants notes by beat. The engine pairs ons with offs as they
    // arrive, so a short note inside a long one comes back out of order, and an
    // overdub arrives after everything already in the clip regardless.
    const auto sortByBeat = [](std::vector<NoteModel>& v) {
        std::stable_sort(v.begin(), v.end(),
                         [](const NoteModel& a, const NoteModel& b) { return a.beat < b.beat; });
    };

    // Overdub. The slot already held a pattern, so this take was a looper pass
    // over it: the engine kept the clip playing, wrapped each captured note's
    // beat into the clip's loop, and returned only the NEW notes (see the
    // Cmd::RecordMidiSlot contract in engine.h). Merging is therefore the whole
    // job -- the length stays the clip's, and nothing already in it is touched.
    if (count > 0 && inRange &&
        ses_.tracks[track].slots[slot].valid() &&
        ses_.tracks[track].slots[slot].kind == ClipKind::Midi) {
        pushUndoNow("overdub");             // not undoPoint: see finishRecording
        ClipModel& m = ses_.tracks[track].slots[slot];
        m.notes.reserve(m.notes.size() + (size_t)count);
        for (int i = 0; i < count; ++i) m.notes.push_back(asNote(buf[i]));
        sortByBeat(m.notes);

        pushClip(track, slot);
        selectTrack(track); selSlot_ = slot;
        detailTab_ = DetailTab::Clip;
        char st[80];
        snprintf(st, sizeof st, "Overdubbed %d note%s  -  %s now has %zu",
                 count, count == 1 ? "" : "s", m.name.c_str(), m.notes.size());
        status_ = st;

        delete[] buf;
        pendingRecs_.erase(it);
        return;
    }

    if (count > 0 && inRange) {
        // Length. The engine hands back notes and a count, not a duration, so
        // the take's musical length is derived from two GUI-side facts and the
        // longer one wins: how far the transport has moved since the quantized
        // start reached us in Ev::RecordStarted, and where the last note ends.
        // (The first is the honest answer for a take that ends in silence; the
        // second covers a note still sounding as the stop boundary lands, and
        // is the only one available if RecordStarted was missed.) That is then
        // rounded UP to a whole bar, so a pattern loops in time with the set
        // instead of at whatever instant the second click happened.
        f64 endBeat = 0.0;
        for (int i = 0; i < count; ++i) endBeat = std::max(endBeat, buf[i].beat + buf[i].len);
        endBeat = std::max(endBeat, engine_.beat.load() - recStartBeat_[track]);
        const f64 barBeats = std::max(1, ses_.sigNum);
        const f64 bars = std::max(1.0, std::ceil(endBeat / barBeats - 1e-9));

        char name[32];
        snprintf(name, sizeof name, "Rec %d", recTakeNo_++);

        pushUndoNow("record");              // not undoPoint: see finishRecording
        ClipModel& m = ses_.tracks[track].slots[slot];
        m = ClipModel{};
        m.uid = ses_.newUid();
        m.kind = ClipKind::Midi;
        m.name = name;
        m.colorIdx = ses_.tracks[track].colorIdx;
        m.clipBpm = ses_.tempo;
        m.lengthBeats = bars * barBeats;
        m.gain = 1.f;
        m.loop = true;
        m.notes.reserve((size_t)count);
        for (int i = 0; i < count; ++i) m.notes.push_back(asNote(buf[i]));
        sortByBeat(m.notes);

        pushClip(track, slot);
        selectTrack(track); selSlot_ = slot;
        detailTab_ = DetailTab::Clip;
        char st[64];
        snprintf(st, sizeof st, "Recorded %s  -  %d notes", name, count);
        status_ = st;
    } else if (count <= 0) {
        status_ = "Recording cancelled";
    }

    delete[] buf;
    pendingRecs_.erase(it);
}


} // namespace lat
