// Undo / redo: history, coalescing, snapshot, restore, and the headless
// self-test. See app.h for the entry model and what is deliberately not
// undoable. Moved verbatim from app.cpp (no functional change).
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

// ---------------------------------------------------------------------------
// undo / redo
//
// See the block in app.h for what an entry is, how gestures coalesce into one,
// and what is deliberately outside all of this. Everything here is GUI thread.
// ---------------------------------------------------------------------------

// Where a snapshot is staged on its way through the project serializer.
// XDG_RUNTIME_DIR is a per-user tmpfs, which is the right home for a file that
// exists for the length of one write and one read; /tmp is the fallback.
static std::string stagingPath() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    const std::string dir = (rt && *rt) ? rt : "/tmp";
    char buf[64];
    snprintf(buf, sizeof buf, "/nxtakt-undo-%d.lattice", (int)getpid());
    return dir + buf;
}

static bool writeAll(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t n = fwrite(text.data(), 1, text.size(), f);
    const bool ok = (n == text.size()) && (fflush(f) == 0);
    fclose(f);
    if (!ok) remove(path.c_str());
    return ok;
}

static bool readAll(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool bad = ferror(f) != 0;
    fclose(f);
    return !bad;
}

// The session as a project document. There is exactly one serializer in this
// program and it writes to a path -- there is no string form of saveProject,
// and a second writer for undo is precisely how the two would drift apart
// until an undo restored something a save could not express. So the snapshot
// goes out to tmpfs and comes straight back in.
bool App::snapshotSession(std::string& out, std::vector<ClipSample>& samples) {
    if (undoTmp_.empty()) undoTmp_ = stagingPath();
    // The audio first, because it is the half the text cannot carry.
    samples.clear();
    for (const TrackModel& t : ses_.tracks)
        for (const ClipModel& c : t.slots)
            if (c.sample && c.uid) samples.push_back(ClipSample{c.uid, c.sample});
    serializeDevices();            // live instances -> the passive form the writer reads
    // saveProject records where it wrote as the set's home. The staging file is
    // not where this set lives.
    const std::string home = ses_.path;
    std::string err;
    const bool ok = saveProject(ses_, undoTmp_, &err);
    ses_.path = home;
    if (!ok) {
        LOGW("undo: could not stage a snapshot: %s", err.c_str());
        return false;
    }
    out.clear();
    const bool got = readAll(undoTmp_, out);
    remove(undoTmp_.c_str());
    if (!got || out.empty()) {
        LOGW("undo: could not read the staged snapshot back");
        return false;
    }
    return true;
}

bool App::undoCoalesce(u64 gesture) {
    // A widget owns `active` for the whole of a drag, so the second and every
    // later frame of one gesture lands here and is refused. A one-shot edit
    // (a button, a key, a menu) has no active widget and always takes a point.
    const u64 g = gesture ? gesture : ui_.active;
    if (g && g == undoGesture_) return true;
    undoGesture_ = g;
    return false;
}

void App::pushUndoNow(const char* what) {
    std::string text;
    std::vector<ClipSample> samples;
    if (!snapshotSession(text, samples)) return;
    // An "edit" that changed nothing -- nudging a note already against the edge
    // of its clip, retyping the same name -- would otherwise leave an entry
    // that appears to do nothing when it comes back.
    if (!undo_.empty() && undo_.back().text == text) {
        redo_.clear();
        return;
    }
    UndoEntry e;
    e.text = std::move(text);
    e.samples = std::move(samples);
    e.what = what ? what : "edit";
    e.path = ses_.path;
    e.selTrack = selTrack_;
    e.selSlot = selSlot_;
    if ((int)undo_.size() >= kUndoDepth) undo_.erase(undo_.begin());
    undo_.push_back(std::move(e));
    redo_.clear();                 // the future this edit branched away from
}

void App::undoPoint(const char* what, u64 gesture) {
    if (undoCoalesce(gesture)) return;
    pushUndoNow(what);
}

void App::clearUndo() {
    undo_.clear();
    redo_.clear();
    undoGesture_ = 0;
}

// A take in flight has no coherent place in a session about to be replaced:
// the slot it is aimed at may not exist a moment from now, and half a
// recording is not a state to restore to. The buffer is NOT freed here -- the
// engine may still be appending to it -- so the stop goes out and the finish
// handler drops the material when it comes back.
void App::cancelTakes(const char* why) {
    bool any = false;
    for (PendingRec& p : pendingRecs_) {
        if (p.cancelled) continue;
        p.cancelled = true;
        stopRecording(p.track);
        any = true;
    }
    if (any && why) status_ = why;
}

// openProject's body, minus the disk. The snapshot is written back out to the
// staging file only because loadProject, like saveProject, speaks paths.
bool App::restoreEntry(const UndoEntry& e) {
    cancelTakes("Recording cancelled by undo");

    if (undoTmp_.empty()) undoTmp_ = stagingPath();
    if (!writeAll(undoTmp_, e.text)) {
        status_ = "Undo failed: cannot stage the snapshot";
        return false;
    }
    Session next;
    std::string err;
    const bool ok = loadProject(next, undoTmp_, engine_.sampleRate(), &err);
    remove(undoTmp_.c_str());
    if (!ok) {
        // Our own text failed to parse: a bug, not a user error. The session is
        // untouched (loadProject leaves its target alone on failure) and this
        // entry is not usable, so say so rather than pretending.
        LOGW("undo: snapshot did not parse: %s", err.c_str());
        status_ = "Undo failed: " + err;
        return false;
    }
    next.path = e.path;            // the staging file is not the set's home

    adoptSession(std::move(next), &e.samples);

    // Cursor, after adoptSession's own clamping: the point of carrying it is
    // that an undo lands where the edit happened.
    selTrack_ = clampv(e.selTrack, 0, (int)ses_.tracks.size() - 1);
    selSlot_  = clampv(e.selSlot,  0, (int)ses_.scenes.size() - 1);
    // The roll's selection is an index into a note vector that has just been
    // replaced wholesale, and a sounding preview belongs to the clip that was
    // on screen before the restore.
    if (roll_) roll_->clearSelection();
    stopPreviews();
    // A drag in flight names a source track and slot that the restored set may
    // not have. Nothing about a mouse gesture survives the model it was
    // dragging, and drawDragGhost would index straight past the end.
    drag_ = DragState{};
    return true;
}

void App::undo() {
    if (undo_.empty()) { status_ = "Nothing to undo"; return; }

    UndoEntry e = std::move(undo_.back());
    undo_.pop_back();

    // What is being left behind becomes the redo entry, under the same label:
    // it is the same edit, seen from the other side.
    UndoEntry back;
    const bool haveBack = snapshotSession(back.text, back.samples);
    if (haveBack) {
        back.what = e.what;
        back.path = ses_.path;
        back.selTrack = selTrack_;
        back.selSlot = selSlot_;
    }

    if (!restoreEntry(e)) {
        undo_.push_back(std::move(e));           // still the state to go back to
        return;
    }
    if (haveBack) {
        if ((int)redo_.size() >= kUndoDepth) redo_.erase(redo_.begin());
        redo_.push_back(std::move(back));
    }
    undoGesture_ = 0;
    status_ = "Undo: " + e.what;
}

// Drives one edit of each shape that reaches a different corner of the restore
// path -- a session scalar, a track flag, notes, the track list, a plugin
// parameter, a device removed and brought back -- and checks that undo and redo
// land on exactly the state they claim to. The serialized session is the
// comparison because it is the same thing an entry is made of: if a rebound
// plugin came back with the wrong parameter, or a clip lost its audio, the text
// says so.
void App::debugUndoSelfTest() {
    auto text = [this]() {
        std::string t;
        std::vector<ClipSample> s;
        snapshotSession(t, s);
        return t;
    };

    int fails = 0, ran = 0;
    // Returns false when the edit left the serialized set identical -- the
    // format cannot express it yet, so there was nothing to undo. The caller
    // may then have to put the set back itself; everything else is untouched.
    auto step = [&](const char* name, auto&& edit) {
        const std::string before = text();
        edit();
        const std::string after = text();
        if (after == before) {
            LOGW("undo self-test: %s changed nothing - not exercised", name);
            return false;
        }
        ++ran;
        undo();
        if (text() != before) { LOGE("undo self-test: %s did not undo", name); ++fails; }
        redo();
        if (text() != after)  { LOGE("undo self-test: %s did not redo", name); ++fails; }
        undo();                        // leave the set as this step found it
        if (text() != before) { LOGE("undo self-test: %s did not undo twice", name); ++fails; }
        return true;
    };

    step("tempo", [&] { undoPoint("tempo"); setTempo(ses_.tempo + 7.0); });
    step("rename track", [&] {
        std::string was = ses_.tracks[0].name;
        std::string now = was + " (edited)";
        std::swap(ses_.tracks[0].name, now);
        undoPointWith("rename track", ses_.tracks[0].name, was);
    });
    step("solo", [&] {
        const bool was = ses_.tracks[0].solo;
        ses_.tracks[0].solo = !was;
        undoPointWith("solo", ses_.tracks[0].solo, was);
        send(Cmd::TrackSolo, 0, ses_.tracks[0].solo ? 1 : 0);
    });
    step("add track", [&] { undoPoint("add track"); addTrack(); });
    // A send is a per-track array the mixer writes straight into, so it goes
    // through the same before-value path a fader does.
    step("send level", [&] {
        const f32 was = ses_.tracks[0].sends[0];
        ses_.tracks[0].sends[0] = was > 0.5f ? 0.f : 0.7f;
        undoPointWith(kSendUndo[0], ses_.tracks[0].sends[0], was);
        send(Cmd::SendLevel, 0, 0, ses_.tracks[0].sends[0]);
    });
    step("return volume", [&] {
        const f32 was = ses_.returns[0].fader;
        ses_.returns[0].fader = was > 0.5f ? 0.4f : 0.9f;
        undoPointWith("return volume", ses_.returns[0].fader, was);
        send(Cmd::ReturnVol, 0, 0, faderToGain(ses_.returns[0].fader));
    });

    // Notes, through the same before-value path the roll uses.
    int mt = -1, msl = -1;
    for (int t = 0; t < (int)ses_.tracks.size() && mt < 0; ++t)
        for (int s = 0; s < (int)ses_.scenes.size(); ++s)
            if (ses_.tracks[t].slots[s].kind == ClipKind::Midi &&
                ses_.tracks[t].slots[s].valid()) { mt = t; msl = s; break; }
    if (mt >= 0) {
        step("note edit", [&] {
            ClipModel& m = ses_.tracks[mt].slots[msl];
            const ClipModel was = m;
            m.notes.push_back(NoteModel{0.0, 0.25, 61, 99});
            undoPointWith("note edit", m, was);
            pushClip(mt, msl);
        });
    } else {
        LOGW("undo self-test: no MIDI clip in this set - notes not exercised");
    }

    // Devices: a parameter (rebound instance, value re-applied) and a removal
    // (instance retired, then instantiated again from the registry).
    int dt = -1;
    for (int t = 0; t < (int)ses_.tracks.size() && dt < 0; ++t)
        for (const DeviceModel& d : ses_.tracks[t].devices)
            if (d.inst && d.inst->paramCount() > 0) { dt = t; break; }
    if (dt >= 0) {
        step("device param", [&] {
            PluginInstance* in = ses_.tracks[dt].devices[0].inst.get();
            const ParamInfo& pi = in->paramInfo(0);
            const f32 v = in->getParam(0);
            undoPoint("param");
            in->setParam(0, v == pi.max ? pi.min : pi.max);
        });
        // The master chain, which reaches materializeDevices and the retirement
        // flow through the owner id rather than a track index. The plugin is
        // one the set already has loaded, so this costs no extra scan and works
        // on any machine the set itself works on.
        // The two chains that are not a track's, which reach materializeDevices
        // and the retirement flow through an owner id rather than a track
        // index. The plugin is one the set already has loaded, so this costs no
        // extra scan and works wherever the set itself does.
        const PluginDesc mdesc = ses_.tracks[dt].devices[0].desc;
        const int busOwners[2] = {kOwnMaster, ownReturn(0)};
        const char* busNames[2] = {"master device", "return device"};
        for (int k = 0; k < 2; ++k) {
            const int own = busOwners[k];
            if (step(busNames[k], [&] { undoPoint("add device"); addDevice(own, mdesc); })) continue;
            // The set text could not express this chain, so the undo had
            // nothing to take back and the device is still there. Put the set
            // back by hand: a self-test must not leave the session it borrowed
            // in a state the user did not ask for.
            ChainOwner co = chainOwner(own);
            if (co.devices && !co.devices->empty())
                removeDevice(own, (int)co.devices->size() - 1);
        }
        step("remove device", [&] { undoPoint("remove device"); removeDevice(dt, 0); });
    } else {
        LOGW("undo self-test: no device with parameters - devices not exercised");
    }

    step("clear clip", [&] {
        undoPoint("clear clip");
        clearClip(selTrack_, selSlot_);
    });

    // The history, and the status line, are the self-test's and not the user's.
    clearUndo();
    status_ = "Ready";
    if (fails) LOGE("undo self-test: %d FAILURE(S) across %d edits", fails, ran);
    else       LOGI("undo self-test: %d edits undone and redone cleanly", ran);
}

void App::redo() {
    if (redo_.empty()) { status_ = "Nothing to redo"; return; }

    UndoEntry e = std::move(redo_.back());
    redo_.pop_back();

    UndoEntry back;
    const bool haveBack = snapshotSession(back.text, back.samples);
    if (haveBack) {
        back.what = e.what;
        back.path = ses_.path;
        back.selTrack = selTrack_;
        back.selSlot = selSlot_;
    }

    if (!restoreEntry(e)) {
        redo_.push_back(std::move(e));
        return;
    }
    // Straight onto the undo stack, and without clearing the redo stack: this
    // is a walk back along the same history, not a new edit.
    if (haveBack) {
        if ((int)undo_.size() >= kUndoDepth) undo_.erase(undo_.begin());
        undo_.push_back(std::move(back));
    }
    undoGesture_ = 0;
    status_ = "Redo: " + e.what;
}

} // namespace lat
