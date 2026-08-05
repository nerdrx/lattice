// Project layer: uid assignment, device (de)serialization, chain release,
// adoptSession, open / save. Moved verbatim from app.cpp.
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
// project: identity, device materialization, load / save
// ---------------------------------------------------------------------------

// Anything that reaches the App without an identity gets one here: entities
// built before UIDs existed, and anything a future loader forgets to stamp.
// The counter is also pulled past every UID actually in use, so a legacy file
// with hand-written IDs can never collide with the ones we hand out next.
void App::assignUids() {
    u64 seen = 0;
    auto note = [&](u64 id) { if (id > seen) seen = id; };
    // Devices are looked at through the owner list, so a return's chain and the
    // master's are stamped by the same pass a track's is.
    const std::vector<int> owners = modelOwners();
    for (const TrackModel& t : ses_.tracks) {
        note(t.uid);
        for (int s = 0; s < kMaxScenes; ++s) note(t.slots[s].uid);
    }
    for (const ReturnModel& r : ses_.returns) note(r.uid);
    for (int o : owners) {
        ChainOwner co = chainOwner(o);
        if (!co.devices) continue;
        for (const DeviceModel& d : *co.devices) note(d.uid);
        for (const SavedDevice& d : *co.saved)   note(d.uid);
    }
    for (const SceneModel& s : ses_.scenes) note(s.uid);
    if (ses_.nextUid <= seen) ses_.nextUid = seen + 1;

    for (TrackModel& t : ses_.tracks) {
        if (!t.uid) t.uid = ses_.newUid();
        for (int s = 0; s < kMaxScenes; ++s) {
            ClipModel& c = t.slots[s];
            // An empty slot is not an entity: only a clip that exists, or one
            // whose audio went missing but whose reference survived, gets one.
            if (!c.uid && (c.valid() || !c.path.empty())) c.uid = ses_.newUid();
        }
    }
    for (ReturnModel& r : ses_.returns) if (!r.uid) r.uid = ses_.newUid();
    for (int o : owners) {
        ChainOwner co = chainOwner(o);
        if (!co.devices) continue;
        for (DeviceModel& d : *co.devices)      if (!d.uid) d.uid = ses_.newUid();
        for (SavedDevice& d : *co.saved)        if (!d.uid) d.uid = ses_.newUid();
    }
    for (SceneModel& s : ses_.scenes) if (!s.uid) s.uid = ses_.newUid();
}

// devices -> savedDevices. The project layer only ever sees the passive form,
// so this is the one place that reads a live instance for persistence.
void App::serializeDevices() {
    for (int o : modelOwners()) {
        ChainOwner co = chainOwner(o);
        if (!co.devices) continue;
        std::vector<SavedDevice>& out = *co.saved;
        out.clear();
        out.reserve(co.devices->size());
        for (DeviceModel& d : *co.devices) {
            if (!d.uid) d.uid = ses_.newUid();
            SavedDevice sd;
            sd.uid = d.uid;
            sd.uri = d.desc.uri;
            sd.name = d.desc.name;
            sd.bypass = d.bypass;
            if (d.inst) {
                const int n = d.inst->paramCount();
                sd.params.reserve((size_t)n);
                for (int i = 0; i < n; ++i)
                    sd.params.push_back({d.inst->paramInfo(i).id, d.inst->getParam(i)});
            } else {
                // A device whose plugin was missing at load time. Its saved
                // values were parked on the model rather than thrown away, so
                // the set round-trips unchanged on a machine that does not have
                // the plugin and works again on one that does.
                sd.params = d.lostParams;
            }
            out.push_back(std::move(sd));
        }
    }
}

// savedDevices -> devices. Every entry keeps its slot in the chain even if the
// plugin is gone, so the order a set was saved with is the order it comes back
// with once the missing plugin is installed.
//
// `reuse` (see the declaration) turns this into a *rebind* for everything an
// undo restore already has running. That is not an optimisation for its own
// sake: a plugin reload loses everything the plugin holds that its parameters
// do not describe, so undoing a note edit would silently reset every synth in
// the set. Instantiation stays the fallback, for a device the pool has no
// match for -- one that was removed and is coming back, or a set being loaded.
void App::materializeDevices(std::vector<LiveDevice>* reuse) {
    const std::vector<int> owners = modelOwners();
    bool any = false;
    for (int o : owners) {
        ChainOwner co = chainOwner(o);
        if (co.saved && !co.saved->empty()) { any = true; break; }
    }
    if (!any) return;
    // The scan is chatty about its progress in the status bar; a load that goes
    // through cleanly should still read as a load when it is done. It is also
    // deferred to the first device that actually needs the registry: a restore
    // that rebinds the whole chain touches no plugin, and making the first undo
    // of a session pay for a full LV2 + CLAP scan would be a bizarre place to
    // spend the better part of a second.
    const std::string prevStatus = status_;

    int missing = 0;
    for (int owner : owners) {
        ChainOwner co = chainOwner(owner);
        if (!co.saved || co.saved->empty()) continue;

        for (SavedDevice& sd : *co.saved) {
            DeviceModel dm;
            dm.uid = sd.uid;
            dm.bypass = sd.bypass;

            // A live instance for this uid is this device, still running; the
            // uri is checked too, because a uid only means "the same device"
            // while the plugin behind it is the same plugin.
            std::unique_ptr<PluginInstance> inst;
            if (reuse) {
                for (LiveDevice& ld : *reuse) {
                    if (!ld.inst || ld.uid != sd.uid || ld.uri != sd.uri) continue;
                    dm.desc = ld.desc;
                    inst = std::move(ld.inst);
                    break;
                }
            }
            const bool rebound = inst != nullptr;

            const PluginDesc* found = nullptr;
            if (!rebound) {
                ensurePluginScan();
                found = registry_.find(sd.uri);
                if (found) inst = registry_.instantiate(*found, engine_.sampleRate(), kMaxBlock);
            }

            if (!inst) {
                ++missing;
                LOGW("plugin not available: %s (%s)", sd.name.c_str(), sd.uri.c_str());
                if (found) dm.desc = *found;
                else {
                    dm.desc.uri = sd.uri;
                    dm.desc.name = sd.name;
                }
                dm.lostParams = sd.params;
                co.devices->push_back(std::move(dm));
                continue;
            }

            // Parameters are matched on ParamInfo::id, not on index: a plugin
            // can gain or reorder controls between versions, and dropping the
            // ones we no longer recognise beats applying them to the wrong
            // control. A rebound instance goes through exactly the same loop:
            // its live values are whatever the user has since dragged them to,
            // and the snapshot's are the ones being restored.
            const int n = inst->paramCount();
            for (const std::pair<u32, f32>& pv : sd.params) {
                for (int i = 0; i < n; ++i) {
                    if (inst->paramInfo(i).id != pv.first) continue;
                    inst->setParam(i, pv.second);
                    break;
                }
            }
            inst->setBypassed(sd.bypass);

            if (!rebound) dm.desc = *found;
            dm.inst = std::move(inst);
            co.devices->push_back(std::move(dm));
        }

        // The live models are now the truth; the passive copies are rebuilt
        // from them at the next save, missing plugins included.
        co.saved->clear();
        publishChain(owner);
    }

    if (missing > 0) {
        char buf[80];
        snprintf(buf, sizeof buf, "%d device%s could not be loaded - kept in the set",
                 missing, missing == 1 ? "" : "s");
        status_ = buf;
    } else {
        status_ = prevStatus;
    }
}

// Hands every published chain and every instance over to the retirement flow.
// Used before a load replaces the session wholesale: the tracks are about to be
// destroyed, and destroying a PluginInstance the audio thread is still running
// is the one thing the chain protocol exists to prevent.
void App::releaseAllChains() {
    // Every published slot, not only the ones the session still has a model
    // for: a set that shrank leaves the engine running a chain for a track
    // index that no longer exists, and that chain has to be let go of too.
    auto release = [&](int owner) {
        ChainOwner co = chainOwner(owner);
        if (!co.valid()) return;
        const bool hasDevices = co.devices && !co.devices->empty();
        if (!*co.published && !hasDevices) return;

        RtChain* empty = new RtChain();
        Command c;
        c.type = co.cmd;
        c.a = co.addr;
        c.p = empty;
        const bool sent = engine_.pushCommand(c);
        if (!sent) {
            LOGW("command ring full - %s keeps running its old chain",
                 ownerName(owner).c_str());
            delete empty;
        }

        RetiredChain rc;
        if (hasDevices)
            for (DeviceModel& d : *co.devices)
                if (d.inst) rc.dying.push_back(std::move(d.inst));

        if (sent) {
            rc.chain = *co.published;       // may be null: nothing was published
            *co.published = empty;
        }
        // rc.chain stays null when nothing was ever published, or when the send
        // failed and the engine is therefore still following the old chain.
        // Either way no Ev::ChainRetired will ever match this entry, so it sits
        // in retiring_ until shutdown() - which is after the audio thread is
        // joined, and the only moment freeing it unilaterally is safe.
        if (rc.chain || !rc.dying.empty()) retiring_.push_back(std::move(rc));
    };

    for (int t = 0; t < kMaxTracks; ++t) release(t);
    for (int i = 0; i < kMaxReturns; ++i) release(ownReturn(i));
    release(kOwnMaster);
}

// The whole session is being replaced -- by a file, or by an undo snapshot.
// The two differ only in where `next` came from and in what may be carried
// across; everything about *how* a session is torn down and stood back up is
// here, once, because the ownership rules it has to respect are the hardest
// thing in this file.
void App::adoptSession(Session&& next, const std::vector<ClipSample>* restore) {
    const bool restoring = restore != nullptr;
    // 1. Instances the incoming session names and this one is already running.
    //    Harvested before releaseAllChains, which would otherwise hand every
    //    instance to the retirement flow and destroy it. Anything NOT taken
    //    here stays on its track and dies the ordinary way, which is exactly
    //    what should happen to a device the snapshot does not have.
    std::vector<LiveDevice> reuse;
    if (restoring) {
        // "Does the incoming set still name this device?", asked of every chain
        // it has -- tracks, the four returns, the master.
        auto wantedBy = [&next](u64 uid, const std::string& uri) {
            auto hit = [&](const std::vector<SavedDevice>& v) {
                for (const SavedDevice& sd : v)
                    if (sd.uid == uid && sd.uri == uri) return true;
                return false;
            };
            for (const TrackModel& nt : next.tracks) if (hit(nt.savedDevices)) return true;
            for (const ReturnModel& nr : next.returns) if (hit(nr.savedDevices)) return true;
            return hit(next.masterSavedDevices);
        };
        for (int o : modelOwners()) {
            ChainOwner co = chainOwner(o);
            if (!co.devices) continue;
            for (DeviceModel& d : *co.devices) {
                if (!d.inst) continue;
                if (!wantedBy(d.uid, d.desc.uri)) continue;
                LiveDevice ld;
                ld.uid = d.uid;
                ld.uri = d.desc.uri;
                ld.desc = d.desc;
                ld.inst = std::move(d.inst);          // off the track, into the pool
                reuse.push_back(std::move(ld));
            }
        }
    }

    // 2. Every published chain and every instance still on a track goes into
    //    the retirement flow. This also publishes an empty chain per track,
    //    which is what makes the audio thread let go of the *rebound*
    //    instances as well before they turn up again in a new chain.
    releaseAllChains();

    // 3. Samples. Two separate jobs, both about pointers the engine holds.
    //
    //    Reuse: on a restore every clip takes back the SampleBuffer it was
    //    playing when the snapshot was taken, matched on clip uid. loadProject
    //    has unavoidably decoded the files again (it only knows how to read
    //    from disk) and those copies are dropped here, so an undo does not
    //    double every sample in the set. More importantly this is the only way
    //    a clip with no file behind it -- a take that has been recorded and
    //    not exported -- survives an undo at all: the text can name a file and
    //    nothing else. See ClipSample.
    //
    //    Grace: whatever the outgoing session owned is held for one more
    //    generation. The engine can still be running a clip that points into
    //    one of those buffers for the few milliseconds it takes to drain the
    //    Cmd::SetClip below, and there is no "the audio thread has let go of
    //    this buffer" event the way there is for chains and note arrays. This
    //    is not that handshake; it is a window measured in user actions rather
    //    than in samples, which is the best this side of the boundary can do.
    if (restoring) {
        for (TrackModel& t : next.tracks)
            for (ClipModel& c : t.slots) {
                if (!c.uid) continue;
                for (const ClipSample& cs : *restore)
                    if (cs.uid == c.uid) { c.sample = cs.sample; break; }
            }
    }
    sampleGrace_.clear();          // the previous generation, long since idle
    for (const TrackModel& t : ses_.tracks)
        for (const ClipModel& c : t.slots)
            if (c.sample) sampleGrace_.push_back(c.sample);

    const int wasTracks = (int)ses_.tracks.size();
    ses_ = std::move(next);

    // Identity. On a load, everything below hands out fresh UIDs and they must
    // come from a counter already pulled past whatever the file used. On a
    // restore there is nothing to fill in: the snapshot carries every uid and
    // the counter itself, and handing out new ones would break the very
    // identities (clip uid, device uid) the restore is matching on.
    if (!restoring) assignUids();

    // A set with nothing in it would leave the views indexing past the end.
    if (ses_.tracks.empty()) addTrack();
    if (ses_.scenes.empty()) addScene();
    selTrack_ = clampv(selTrack_, 0, (int)ses_.tracks.size() - 1);
    selSlot_  = clampv(selSlot_,  0, (int)ses_.scenes.size() - 1);
    selDevice_ = -1;
    // A return or the master is still there whatever the incoming set looks
    // like; a track index may not be, and the device view must not be left
    // pointing past the end of the new track list.
    if (ownIsTrack(devOwner_)) devOwner_ = selTrack_;
    // The tracks this index referred to are gone; the arms in the incoming set
    // are its own, not ours to take back.
    autoArmed_ = -1;

    materializeDevices(restoring ? &reuse : nullptr);   // may set its own status

    // A pool entry nothing adopted (two saved devices sharing a uid, a plugin
    // swapped under one) is still an instance the outgoing chain borrowed, so
    // it cannot simply be dropped here. It rides out with the newest chain that
    // has a retirement coming; if there is none -- the send failed, or nothing
    // was ever published -- it waits for shutdown, which is the same bargain
    // releaseAllChains makes for the same reason.
    RetiredChain* host = nullptr;
    for (auto it = retiring_.rbegin(); it != retiring_.rend(); ++it)
        if (it->chain) { host = &*it; break; }
    for (LiveDevice& ld : reuse) {
        if (!ld.inst) continue;
        if (!host) { retiring_.push_back(RetiredChain{}); host = &retiring_.back(); }
        host->dying.push_back(std::move(ld.inst));
    }

    pushAll();                     // also clears the slots outside the new set

    // The mixer flags of tracks the new set does not have. Their clips are
    // gone and their chains are empty, so volume and pan no longer describe
    // anything -- but solo is global by nature, and one left standing on a
    // track nobody can see any more would silence the whole set with no
    // visible cause. Bounded by how far the set actually shrank.
    for (int t = (int)ses_.tracks.size(); t < wasTracks; ++t) {
        send(Cmd::TrackSolo, t, 0);
        send(Cmd::TrackMute, t, 0);
        send(Cmd::TrackArm,  t, 0);
    }
}

bool App::openProject(const std::string& path) {
    // Load into a scratch session first. loadProject() leaves its target alone
    // on a parse error, but the session is only *ours* to throw away once we
    // know a replacement exists - and throwing it away means retiring chains,
    // which is not something to do speculatively.
    Session next;
    std::string err;
    if (!loadProject(next, path, engine_.sampleRate(), &err)) {
        status_ = "Load failed: " + err;
        return false;
    }

    status_ = "Loaded " + path;
    adoptSession(std::move(next), nullptr);   // may replace the status with a warning
    // The history belonged to the set that was open a moment ago. Undoing into
    // it would silently overwrite the one just loaded.
    clearUndo();
    return true;
}

void App::saveProjectTo(const std::string& path) {
    serializeDevices();
    std::string err;
    status_ = saveProject(ses_, path, &err) ? ("Saved " + path) : ("Save failed: " + err);
}


} // namespace lat
