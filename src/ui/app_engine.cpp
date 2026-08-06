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
#include <cmath>
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

// ---------------------------------------------------------------------------
// Deferred publication — the flow control pushAll needs
//
// docs/ARRANGEMENT.md §15 names the bug and the shape; app.h carries the design
// note. This is the whole mechanism: five short functions, and the property to
// keep hold of while reading them is that an entry names WORK, not a payload.
// Nothing is allocated when something is queued and nothing is retired; both
// happen when it drains, against the model as it stands then.
// ---------------------------------------------------------------------------

bool* App::pendSeen(PubKind k, i32 a, i32 b) {
    switch (k) {
        case PubKind::Clip:
            // Bounds are the TABLE's, not the session's: a cell can be queued
            // while it is inside the set and drain after a load has shrunk it,
            // which is precisely the case syncClipCell exists for.
            if (a < 0 || a >= kMaxTracks || b < 0 || b >= kMaxScenes) return nullptr;
            return &pendClip_[a][b];
        case PubKind::ArrLane: {
            const int cell = (a == -1) ? kMaxTracks : a;
            if (cell < 0 || cell > kMaxTracks) return nullptr;
            return &pendLane_[cell];
        }
        case PubKind::ArrAutos:
            if (a < 0 || a >= kMaxTracks) return nullptr;
            return &pendAutos_[a];
        case PubKind::Scalar:
        default:
            return nullptr;          // every scalar is its own entry, in order
    }
}

void App::queuePub(const PendingPub& p) {
    if (bool* seen = pendSeen(p.kind, p.a, p.b)) {
        // Already waiting. Queuing it twice would publish the same model twice;
        // the entry that is already there will read whatever the model says when
        // it drains, which is by definition at least as fresh as this request.
        if (*seen) return;
        *seen = true;
    }
    if (pending_.size() >= kMaxPending) {
        // The cap. This is the OLD failure, and it is reached only when the
        // engine has stopped draining altogether — no audio backend at all, or
        // a wedged audio thread — so it is said once per hundred rather than
        // once per command, and counted.
        if ((pendDropped_++ % 100) == 0)
            LOGW("engine: publication queue full at %zu - the audio thread is not "
                 "draining (%llu dropped)", pending_.size(),
                 (unsigned long long)pendDropped_);
        if (bool* seen = pendSeen(p.kind, p.a, p.b)) *seen = false;
        return;
    }
    pending_.push_back(p);
    if (pending_.size() > pendHigh_) pendHigh_ = pending_.size();
}

bool App::deferPub(PubKind k, i32 a, i32 b) {
    if (flushing_) return false;          // we are the drain; do the work
    if (pending_.empty()) return false;   // nothing waiting; the ring is ours
    PendingPub p; p.kind = k; p.a = a; p.b = b;
    queuePub(p);
    return true;
}

void App::refusePub(PubKind k, i32 a, i32 b) {
    if (flushing_) { pubRefused_ = true; return; }   // stays at the front
    PendingPub p; p.kind = k; p.a = a; p.b = b;
    queuePub(p);
}

void App::flushPending() {
    if (pending_.empty()) return;
    flushing_ = true;
    while (!pending_.empty()) {
        const PendingPub p = pending_.front();
        pubRefused_ = false;
        switch (p.kind) {
            case PubKind::Scalar:
                if (!eng_.send(p.type, p.a, p.b, p.x)) pubRefused_ = true;
                break;
            case PubKind::Clip:     syncClipCell(p.a, p.b); break;
            case PubKind::ArrLane:
                if (p.a == -1) publishTransportCell();
                else           publishArrangement(p.a);
                break;
            case PubKind::ArrAutos: publishArrangeAutos(p.a); break;
        }
        // Refused: the ring is full again. Everything still queued keeps its
        // order and its place, and the next frame tries again from here.
        if (pubRefused_) break;
        if (bool* seen = pendSeen(p.kind, p.a, p.b)) *seen = false;
        pending_.pop_front();
    }
    flushing_ = false;
}

// Every scalar command in the app comes through here. Two lines, and the order
// they are in is the point: while anything is queued, this one joins the back
// rather than overtaking it.
void App::send(Cmd t, i32 a, i32 b, f64 x) {
    if (pending_.empty() && eng_.send(t, a, b, x)) return;
    PendingPub p;
    p.kind = PubKind::Scalar;
    p.type = t; p.a = a; p.b = b; p.x = x;
    queuePub(p);
}

// ---------------------------------------------------------------------------
// NXTAKT_DEBUG_PUSHALL — the proof that the burst arrives whole
//
// The old failure was silent by construction: pushClip logged and gave up, so
// the engine went on playing a clip the model no longer believed in and nothing
// was scheduled to notice. A screenshot cannot see that, and neither can a
// warning count on its own, so this asks the engine directly.
//
// Cmd::LaunchScene queues a track's slot ONLY when the engine's own
// clips_[t][scene] is valid (engine.cpp) — an invalid one stops the track
// instead. So "launch scene s, then read activeSlot[t]" is a read of the
// engine's slot table, one column at a time, and comparing it with
// ses_.tracks[t].slots[s].valid() over every scene compares the whole table
// with the whole model.
//
// It runs across frames, because that is what the fix does: the queue drains
// over as many frames as the ring needs, and a check that ran in one would be
// checking the wrong thing.
// ---------------------------------------------------------------------------
void App::debugPushAllCheck() {
    if (!pushAllHook_ || pushAllDone_) return;

    // 1. Nothing is checked until every deferred publication has been sent.
    if (!pending_.empty()) {
        pushAllWait_ = 0;
        // Said once a second rather than once a frame: a queue that never
        // empties is itself the answer, and a silent hook would look like a
        // hook that never ran.
        if ((++pushAllStalls_ % 60) == 0)
            LOGI("NXTAKT_DEBUG_PUSHALL: waiting, %zu publications still queued",
                 pending_.size());
        return;
    }

    const int ns = (int)ses_.scenes.size();
    if (pushAllScene_ >= ns) {
        LOGI("NXTAKT_DEBUG_PUSHALL: %d cells over %d tracks x %d scenes agree with "
             "the model, %d disagree; queue high water %zu, dropped %llu",
             pushAllCells_, (int)ses_.tracks.size(), ns, pushAllFails_,
             pendHigh_, (unsigned long long)pendDropped_);
        if (pushAllFails_ == 0)
            LOGI("NXTAKT_DEBUG_PUSHALL: PASS - the engine's slot table matches the set");
        else
            LOGE("NXTAKT_DEBUG_PUSHALL: FAIL - %d cells the engine and the model "
                 "disagree about", pushAllFails_);
        pushAllDone_ = true;
        return;
    }

    if (!pushAllLaunched_) {
        // Unquantized, so the launch happens in the next block rather than on
        // the next bar line: this is a test of what the engine HOLDS, and
        // waiting two seconds a scene for 32 scenes is a different test.
        send(Cmd::SetQuantum, 0);
        send(Cmd::LaunchScene, pushAllScene_);
        pushAllLaunched_ = true;
        pushAllWait_ = 0;
        return;
    }
    // 2. Two frames for the command to be drained and the state republished.
    if (pushAllWait_ < 2) { ++pushAllWait_; return; }

    for (int t = 0; t < (int)ses_.tracks.size() && t < kMaxTracks; ++t) {
        const bool want = ses_.tracks[t].slots[pushAllScene_].valid();
        const bool got  = es_.activeSlot[t] == pushAllScene_;
        ++pushAllCells_;
        if (want == got) continue;
        ++pushAllFails_;
        if (pushAllFails_ <= 8)
            LOGE("NXTAKT_DEBUG_PUSHALL: track %d scene %d - model says %s, engine "
                 "says %s (activeSlot %d)", t, pushAllScene_,
                 want ? "clip" : "empty", got ? "clip" : "empty", es_.activeSlot[t]);
    }
    LOGI("NXTAKT_DEBUG_PUSHALL: scene %d checked (%d cells so far, %d bad)",
         pushAllScene_, pushAllCells_, pushAllFails_);
    ++pushAllScene_;
    pushAllLaunched_ = false;
    pushAllWait_ = 0;
}

void App::publishNotes(int track, int slot, const RtNote* fresh) {
    const RtNote* old = publishedNotes_[track][slot];
    publishedNotes_[track][slot] = fresh;
    // The engine only announces a *replaced* array, and only when it differs
    // from the incoming one; an entry that would never be announced must not be
    // queued for a retirement that will never arrive.
    if (old && old != fresh) retiringNotes_.push_back(old);
}

// ---------------------------------------------------------------------------
// warp marker arrays
//
// The publisher and the GUI-side gate. The state the two of them talk about --
// App::warpMaps_ -- moved onto App itself (app.h, beside publishedNotes_), which
// is where its own comment always said it belonged; it sat in this translation
// unit only because the wave that added it could not edit that header. The move
// is a change to the declaration and to nothing else: publishWarp() below is
// still the only writer and pumpEngineEvents the only other reader.
// ---------------------------------------------------------------------------

// publishNotes verbatim.
void App::publishWarp(int track, int slot, const WarpMarker* fresh) {
    if (track < 0 || track >= kMaxTracks || slot < 0 || slot >= kMaxScenes) return;
    const WarpMarker* old = warpMaps_.published[track][slot];
    warpMaps_.published[track][slot] = fresh;
    // The engine only announces a *replaced* array, and only when it differs
    // from the incoming one; an entry that would never be announced must not be
    // queued for a retirement that will never arrive.
    if (old && old != fresh) warpMaps_.retiring.push_back(old);
}

namespace {

// The GUI-side gate, run once here so the audio thread never pays for it. A map
// that is empty, or that has a single marker (a point pins, it does not tilt),
// or that breaks strict monotonicity is simply not published: the clip then
// warps at its single clipBpm/tempo ratio, which is a working clip and not a
// broken one. Returns null for all three.
WarpMarker* buildWarpMarkers(const ClipModel& m, int track, int slot) {
    const size_t n = m.markers.size();
    if (n < 2) {
        if (n == 1) LOGW("warp: slot %d/%d has a single marker, which pins "
                         "nothing - not published", track, slot);
        return nullptr;
    }
    if (!warpMapValid(m.markers.data(), (int)n)) {
        LOGW("warp: slot %d/%d has a non-monotone map (%zu markers) - not published",
             track, slot, n);
        return nullptr;
    }
    WarpMarker* fresh = new (std::nothrow) WarpMarker[n];
    if (!fresh) return nullptr;
    for (size_t i = 0; i < n; ++i) fresh[i] = m.markers[i];
    return fresh;
}

} // namespace

// ---------------------------------------------------------------------------
// automation: address resolution and publishing   (docs/AUTOMATION.md §4, §2.5)
//
// The model keeps a lane's address as TEXT and nothing else. Resolution to the
// hot fields the engine switches on happens HERE, at publish time, and is thrown
// away again on the next publish -- which is why §4.3's rule can afford to be
// blunt ("whenever a track's chain is republished, republish the envelopes of
// every clip on that track") instead of maintaining a dependency map with an
// invalidation problem.
//
// An address that resolves to nothing publishes NO lane. It is not an error and
// it is not repaired: PARAM-ADDRESS.md's "dangling addresses resolve to nothing
// and must fail soft" is the same promise ClipModel::path makes for a missing
// sample and DeviceModel::lostParams makes for a missing plugin. The clip plays,
// the envelope is still in the file, and installing the plugin brings it back.
// ---------------------------------------------------------------------------

bool App::resolveAutoLane(int track, const std::string& address, RtAutoLane& out) const {
    if (track < 0 || track >= (int)ses_.tracks.size()) return false;
    addr::Parsed p;
    if (!addr::parse(address, p)) return false;

    // Scope check (§4.2 step 2): a clip envelope may only automate its own
    // track's mixer and its own track's devices. `master/...` and another
    // track's address parse cleanly and resolve to nothing, which is exactly
    // what the restriction is supposed to feel like from here.
    const TrackModel& t = ses_.tracks[track];
    if (p.scope != addr::Parsed::Scope::Track || p.scopeUid != t.uid) return false;

    out = RtAutoLane{};
    switch (p.field) {
    case addr::Parsed::Field::Vol:
        // The model stores the fader POSITION, not a gain (§2.3), so the
        // mapping travels as data and the engine needs no table of which
        // target means which curve.
        out.target = (i32)AutoTarget::TrackVol;
        out.xform  = (i32)AutoXform::Fader;
        out.lo = 0.f; out.hi = 1.f;
        return true;
    case addr::Parsed::Field::Pan:
        out.target = (i32)AutoTarget::TrackPan;
        out.xform  = (i32)AutoXform::Direct;
        out.lo = -1.f; out.hi = 1.f;
        return true;
    case addr::Parsed::Field::Send:
        if (p.sendIndex < 0 || p.sendIndex >= kMaxReturns) return false;
        out.target = (i32)AutoTarget::TrackSend;
        out.index  = p.sendIndex;
        out.xform  = (i32)AutoXform::Direct;
        out.lo = 0.f; out.hi = 1.f;
        return true;
    case addr::Parsed::Field::DeviceParam: {
        // Two lookups, and the second one is the one people get wrong. `devSlot`
        // is the device's position in the PUBLISHED RtChain, which is not its
        // index in `devices`: publishChain() skips every device whose instance is
        // null (the plugin was missing when the set loaded) and stops at
        // kMaxChainFx. This loop counts the same way, deliberately line for line,
        // because a devSlot that disagrees with the chain automates the wrong
        // plugin -- a silent, audible, untraceable wrong answer.
        const DeviceModel* dev = nullptr;
        int chainSlot = -1, n = 0;
        for (const DeviceModel& d : t.devices) {
            if (!d.inst) continue;                 // no instance -> no chain slot
            if (n >= kMaxChainFx) break;
            if (d.uid == p.devUid) { dev = &d; chainSlot = n; break; }
            ++n;
        }
        if (!dev) return false;                    // deleted, or its plugin is gone
        const int pc = dev->inst->paramCount();
        int idx = -1;
        for (int i = 0; i < pc; ++i)
            if (dev->inst->paramInfo(i).id == p.paramId) { idx = i; break; }
        if (idx < 0) return false;                 // the plugin renumbered its params
        const ParamInfo& info = dev->inst->paramInfo(idx);
        out.target  = (i32)AutoTarget::DeviceParam;
        out.index   = idx;
        out.devSlot = chainSlot;
        out.xform   = (i32)AutoXform::Direct;
        // The engine clamps to these, so a stale envelope cannot drive a
        // parameter out of the range the plugin declares today. A backend that
        // reports them the wrong way round would otherwise clamp everything to
        // one value.
        out.lo = std::min(info.min, info.max);
        out.hi = std::max(info.min, info.max);
        return true;
    }
    // Reserved by AutoTarget but not implemented: mute/solo/arm (TrackMute),
    // clip fields (ClipGain), scene launch. They parse, and they publish
    // nothing, which is the same fail-soft an unknown device gets.
    case addr::Parsed::Field::Mute:
    case addr::Parsed::Field::Solo:
    case addr::Parsed::Field::Arm:
    case addr::Parsed::Field::ClipField:
    case addr::Parsed::Field::SceneLaunch:
    case addr::Parsed::Field::None:
    default:
        return false;
    }
}

// The points array starts immediately past the struct, so the struct's size has
// to leave it aligned. It does on every ABI in the tree (RtAutoSet is 8-aligned
// and a whole number of 8s long), and this says so out loud rather than leaving
// it to be discovered by a bus error on the first machine where it is not.
static_assert(sizeof(RtAutoSet) % alignof(RtAutoPoint) == 0,
              "RtAutoPoint[] follows RtAutoSet inside one allocation");

const RtAutoSet* App::buildAutos(int track, int slot) {
    const ClipModel& m = ses_.tracks[track].slots[slot];
    if (!m.valid() || m.envelopes.empty()) return nullptr;

    // Resolve first, allocate second: the number of lanes that survive
    // resolution is what decides whether there is anything to allocate at all.
    RtAutoLane lanes[kMaxRtAutoLanes];
    i32 modelLane[kMaxRtAutoLanes];
    int laneCount = 0, pointCount = 0;
    for (size_t i = 0; i < m.envelopes.size() && laneCount < kMaxRtAutoLanes; ++i) {
        const AutoLane& L = m.envelopes[i];
        // A deactivated lane and an empty one are both UI state rather than
        // content: neither has anything to apply, and an empty lane evaluates to
        // the fallback anyway (§2.4), so publishing one buys nothing.
        if (!L.enabled || L.points.empty()) continue;
        RtAutoLane rl;
        if (!resolveAutoLane(track, L.address, rl)) continue;
        int n = (int)L.points.size();
        if (pointCount + n > kMaxClipAutoPoints) {
            n = kMaxClipAutoPoints - pointCount;
            LOGW("clip '%s' has more than %d automation points - the tail of '%s' "
                 "will not be applied", m.name.c_str(), kMaxClipAutoPoints, L.address.c_str());
        }
        if (n <= 0) break;
        rl.first = pointCount;
        rl.count = n;
        rl.flags = 0;
        lanes[laneCount] = rl;
        modelLane[laneCount] = (i32)i;
        ++laneCount;
        pointCount += n;
    }
    if (laneCount == 0) return nullptr;

    // ONE allocation. Two -- a lane array and a point array -- would need two
    // retirement events, or a rule about which one implies the other; the RtNote
    // protocol is only simple because there is exactly one pointer per slot.
    const size_t bytes = sizeof(RtAutoSet) + (size_t)pointCount * sizeof(RtAutoPoint);
    char* mem = new (std::nothrow) char[bytes];
    if (!mem) {
        status_ = "Out of memory - automation not updated";
        return nullptr;
    }
    RtAutoSet* set = new (mem) RtAutoSet();
    RtAutoPoint* pts = (RtAutoPoint*)(mem + sizeof(RtAutoSet));

    int w = 0;
    for (int l = 0; l < laneCount; ++l) {
        const AutoLane& L = m.envelopes[modelLane[l]];
        for (int k = 0; k < lanes[l].count; ++k) {
            const AutoPoint& src = L.points[(size_t)k];
            // Copied in MODEL ORDER, not sorted (§4.2 step 5): the editor and the
            // recorder hold the sorted invariant, and the publisher preserving
            // what it is given is the same rule the file format follows, so a
            // hand-shuffled file evaluates to something ugly rather than to
            // something undefined. Non-finite values are the one thing replaced,
            // because a NaN beat is not ugly, it is a hang in a linear scan.
            pts[w].beat  = std::isfinite(src.beat) ? std::max(0.0, src.beat) : 0.0;
            pts[w].value = std::isfinite(src.value) ? src.value : 0.f;
            pts[w].curve = src.curve;
            ++w;
        }
        set->lanes[l] = lanes[l];
    }
    set->points     = pts;
    set->laneCount  = laneCount;
    set->pointCount = pointCount;

    AutoBlock b;
    b.mem = mem;
    b.set = set;
    b.clipUid = m.uid;
    b.modelLane.assign(modelLane, modelLane + laneCount);
    autoBlocks_.v.push_back(std::move(b));
    return set;
}

void App::dropAutos(const RtAutoSet* set) {
    if (!set) return;
    for (size_t i = 0; i < autoBlocks_.v.size(); ++i) {
        if (autoBlocks_.v[i].set != set) continue;
        delete[] autoBlocks_.v[i].mem;
        autoBlocks_.v.erase(autoBlocks_.v.begin() + (long)i);
        return;
    }
    LOGW("dropAutos for an unknown set %p - leaking it rather than freeing a "
         "pointer we do not own", (const void*)set);
}

void App::publishAutos(int track, int slot, const RtAutoSet* fresh) {
    const RtAutoSet* old = publishedAutos_[track][slot];
    publishedAutos_[track][slot] = fresh;
    // Verbatim publishNotes: the engine only announces a *replaced* set, and only
    // when it differs from the incoming one, so an entry that would never be
    // announced must not be queued for a retirement that will never arrive.
    if (old && old != fresh) retiringAutos_.push_back(old);
}

bool App::autoLaneInert(u64 clipUid, const std::string& address) const {
    for (const InertAuto& a : inertAutos_)
        if (a.clipUid == clipUid && a.address == address) return true;
    return false;
}

// One cell of the engine's slot table against the model, whatever the model now
// says. Inside the set that is pushClip; outside it -- a load or an undo made
// the set smaller while this cell was queued -- it is the ClearClip
// releaseStaleSlots would have sent. Only the deferred drain calls this, and it
// is the reason a queued cell is safe to survive a session swap.
void App::syncClipCell(int track, int slot) {
    if (track < 0 || track >= kMaxTracks || slot < 0 || slot >= kMaxScenes) return;
    if (track < (int)ses_.tracks.size() && slot < (int)ses_.scenes.size()) {
        pushClip(track, slot);
        return;
    }
    clearStaleSlot(track, slot);
}

void App::pushClip(int track, int slot) {
    // Something is already waiting for the ring. Take a place behind it rather
    // than overtaking it, and build nothing: the drain will read this cell out
    // of the model then, which is at least as fresh as now.
    if (deferPub(PubKind::Clip, track, slot)) return;

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
            // The generative pair (engine.h, RtNote::chance / velTo). The dice
            // are thrown on the audio thread, so all the GUI ever does with
            // these is carry them across intact.
            fresh[i].chance = n.chance;
            fresh[i].velTo  = n.velTo;
        }
    }

    // The envelopes, resolved and flattened into their one allocation. Same
    // timing as the notes and for the same reason: the engine reads the set for
    // as long as it holds the clip, so it cannot be the GUI's live vectors, and
    // it has to exist before anything is sent. Returns null for a clip with no
    // publishable lane, which is the ordinary case.
    const RtAutoSet* autos = buildAutos(track, slot);

    // And the warp map, on the same timing and for the same reason. Null for a
    // clip with no usable map, which is also the ordinary case. Only audio
    // clips can carry one: a MIDI clip is already in beats and has nothing to
    // warp, so a stray map on one is not published rather than ignored later.
    WarpMarker* warp = (m.valid() && !midi) ? buildWarpMarkers(m, track, slot) : nullptr;

    if (!m.valid()) {
        c.type = Cmd::ClearClip;
        if (!eng_.pushCommand(c)) {
            // Nothing was borrowed, so nothing is retired and the slot keeps
            // whatever the engine is already playing -- for as long as it takes
            // this cell to come round again in the queue, which is one frame.
            dropAutos(autos);
            delete[] warp;                 // null here, but the rule is the rule
            refusePub(PubKind::Clip, track, slot);
            return;
        }
        // A cleared MIDI slot retires its notes exactly like a replaced one, and
        // its envelopes and its warp map with them.
        publishNotes(track, slot, nullptr);
        publishAutos(track, slot, nullptr);
        publishWarp(track, slot, nullptr);
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
        // The warp map: GUI-owned, retired through Ev::WarpRetired. Null unless
        // buildWarpMarkers accepted one, and then always >= 2 entries.
        rc.markers      = warp;
        rc.markerCount  = warp ? (int)m.markers.size() : 0;
        // The transient list: BORROWED from the SampleBuffer and never retired.
        // SampleBuffer::transients is built once in loadSample and never
        // rebuilt, and the clip holds a SampleRef, so the pointer outlives every
        // RtClip that names it exactly as `data` does — which is the whole
        // reason it needs no retirement event. Beats-mode grain scheduling
        // aligns to these; a sample with none (the demo pad detects none at all)
        // grains on the fixed hop exactly as before.
        if (!m.sample->transients.empty()) {
            rc.transients     = m.sample->transients.data();
            rc.transientCount = (int)m.sample->transients.size();
        }
    }
    rc.lengthBeats  = m.lengthBeats;
    rc.gain         = m.gain;
    rc.loop         = m.loop;
    rc.quantumIdx   = m.quantumIdx;
    rc.prob         = m.prob;
    rc.followAction = (int)m.followAction;
    rc.followBeats  = m.followBeats;
    // Envelopes are not gated on clip kind: an audio clip's lane is a wave-8 UI
    // change and not a format or protocol one, so the plumbing carries it now.
    rc.autos        = autos;
    rc.valid        = true;
    c.clip = rc;
    if (!eng_.pushCommand(c)) {
        // The engine never saw any of the three allocations, so all three are
        // still solely ours and the slot keeps whatever it was already playing.
        // The cell goes back in the queue, so "the engine plays a clip the model
        // no longer believes in" now lasts one frame instead of forever.
        delete[] fresh;
        dropAutos(autos);
        delete[] warp;
        refusePub(PubKind::Clip, track, slot);
        return;
    }
    // Unconditional, not only for MIDI clips: a slot that just turned into an
    // audio clip still has an old note array to hand back — and one that just
    // turned into a MIDI clip still has an old warp map to hand back.
    publishNotes(track, slot, fresh);
    publishAutos(track, slot, autos);
    publishWarp(track, slot, warp);
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
    for (int t = 0; t < kMaxTracks; ++t)
        for (int s = 0; s < kMaxScenes; ++s)
            if (t >= nt || s >= ns) clearStaleSlot(t, s);
}

// One out-of-set cell. Split out of the loop above so the deferred drain can
// reach a single cell: a cell queued while it was inside the set has to be
// clearable when it drains after the set has shrunk (syncClipCell).
void App::clearStaleSlot(int t, int s) {
    if (!clipLive_[t][s] && !publishedNotes_[t][s] && !publishedAutos_[t][s] &&
        !warpMaps_.published[t][s]) return;
    if (deferPub(PubKind::Clip, t, s)) return;

    Command c;
    c.type = Cmd::ClearClip;
    c.a = t; c.b = s;
    if (!eng_.pushCommand(c)) {
        // The engine still holds it, so the flags stay set and the cell goes
        // back in the queue rather than waiting for something to happen to
        // republish it.
        refusePub(PubKind::Clip, t, s);
        return;
    }
    clipLive_[t][s] = false;
    publishNotes(t, s, nullptr);
    publishAutos(t, s, nullptr);
    publishWarp(t, s, nullptr);
}

void App::pumpEngineEvents() {
    // A recording pass ends when the thing driving it lets go. Ui::endFrame()
    // drops `active` on mouse-up, so by the time this runs -- first thing in the
    // next frame() -- the gesture that owned the pass is already gone, and this
    // is the earliest moment the simplification can run. A pass started with an
    // explicit gesture id (the self-test) is ended the same way; nothing here
    // runs during a synchronous test, which is why that pass survives to be
    // ended by hand.
    if (autoRec_.active() && autoRec_.gesture != ui_.active) autoRecFinish();

    // The record journal (§5), drained FIRST and unconditionally. First, because
    // a take that ended in the block this frame is reporting should be committed
    // before anything reads the arrangement; unconditionally, because the ring is
    // the thing that overflows and a consumer that only drained while armed would
    // arrive at its first take with the ring already full — which §5.4 would then
    // correctly refuse, for a reason that was the reader's fault.
    pumpJournal();

    Event e;
    while (eng_.popEvent(e)) {
        // Arrangement lanes and track-automation sets retire through their own
        // reaper (app_arrange.cpp), which owns the tables they live in. Without
        // this line every replaced lane leaks until shutdown, and an arrangement
        // republishes on every edit.
        if (reapArrangementEvent(e)) continue;
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
        if (e.type == Ev::AutosRetired) {
            // The audio thread has stopped reading this set. Third instance of
            // the same handshake (chains, note arrays, now envelopes) and the
            // same refusal to free a pointer we have no record of owning.
            //
            // The free is `delete[]` on the char[] the set was placement-new'd
            // into -- see AutoBlock in app.h and buildAutos above, which is the
            // other half of this bargain: one allocation, one pointer, one
            // delete[], and the RtAutoPoint array inside the same block.
            const RtAutoSet* old = (const RtAutoSet*)e.p;
            if (!old) continue;
            auto it = retiringAutos_.begin();
            for (; it != retiringAutos_.end(); ++it) if (*it == old) break;
            if (it == retiringAutos_.end()) {
                LOGW("AutosRetired for an unknown set %p - leaking it rather than "
                     "freeing a pointer we do not own", (const void*)old);
                continue;
            }
            retiringAutos_.erase(it);
            dropAutos(old);
            continue;
        }
        if (e.type == Ev::WarpRetired) {
            // The audio thread has stopped reading this map. Fourth instance of
            // the same handshake (chains, note arrays, envelopes, now warp maps)
            // and the same refusal to free a pointer we have no record of
            // owning: a bad free here would be a use-after-free in whoever does
            // own it, which is strictly worse than the leak this takes instead.
            const WarpMarker* old = (const WarpMarker*)e.p;
            if (!old) continue;
            auto it = warpMaps_.retiring.begin();
            for (; it != warpMaps_.retiring.end(); ++it) if (*it == old) break;
            if (it == warpMaps_.retiring.end()) {
                LOGW("WarpRetired for an unknown map %p - leaking it rather than "
                     "freeing a pointer we do not own", (const void*)old);
                continue;
            }
            delete[] *it;
            warpMaps_.retiring.erase(it);
            continue;
        }
        if (e.type == Ev::AutoLaneInert) {
            // §3.4: the backend behind this lane has no realtime parameter path,
            // so the engine has given up on it and will not call again for this
            // published set. A silently ignored lane would be the worst of both
            // worlds -- the envelope is drawn, the sound does not move, nothing
            // says why -- so it is recorded here and the editor greys it.
            //
            // `x` is the lane's index in the PUBLISHED array, which is not its
            // index in ClipModel::envelopes: a lane that resolved to nothing was
            // never published. AutoBlock::modelLane is the map, kept for exactly
            // this one question.
            const int t = e.a, s = e.b, li = (int)e.x;
            if (t < 0 || t >= kMaxTracks) continue;
            // b == -1 means the lane belongs to the TRACK's arrangement
            // automation rather than to a clip envelope (8b+8c's encoding —
            // lane index alone is ambiguous once two containers exist). It has
            // no slot and no ClipModel, so the clip path below cannot resolve
            // it; say it plainly instead of dropping it, which is the whole
            // point of the event.
            if (s == -1) {
                if (t < (int)ses_.tracks.size())
                    status_ = "Automation inert: an arrangement lane on " +
                              ses_.tracks[(size_t)t].name + " has no realtime path";
                continue;
            }
            if (s < 0 || s >= kMaxScenes) continue;
            const RtAutoSet* set = publishedAutos_[t][s];
            if (!set) continue;
            const AutoBlock* blk = nullptr;
            for (const AutoBlock& b : autoBlocks_.v) if (b.set == set) { blk = &b; break; }
            if (!blk || li < 0 || li >= (int)blk->modelLane.size()) continue;
            if (t >= (int)ses_.tracks.size() || s >= (int)ses_.scenes.size()) continue;
            const ClipModel& m = ses_.tracks[t].slots[s];
            if (m.uid != blk->clipUid) continue;          // the slot moved on
            const int mi = blk->modelLane[(size_t)li];
            if (mi < 0 || mi >= (int)m.envelopes.size()) continue;
            const std::string& address = m.envelopes[(size_t)mi].address;
            if (autoLaneInert(m.uid, address)) continue;  // said once, not per set
            inertAutos_.push_back(InertAuto{m.uid, address});
            status_ = "Automation inert: " + address + " has no realtime path";
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
    const bool p = es_.playing;
    // A recording pass is a statement about beats that are about to stop
    // advancing, so it ends here rather than being left open across the stop and
    // resumed against a beat several bars away.
    if (p) autoRecFinish();
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
    if (es_.recState[track] != 0) {
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
        const i64 cap = (i64)std::llround(eng_.sampleRate() * kRecordSeconds);
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

    if (!eng_.pushCommand(c)) {
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
        if (!eng_.pushCommand(c)) status_ = "Engine busy - still recording";
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
        SampleRef sb = sampleFromRecording(buf, frames, eng_.sampleRate(), ses_.tempo, name);
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
        endBeat = std::max(endBeat, es_.beat - recStartBeat_[track]);
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

// ---------------------------------------------------------------------------
// automation recording   (docs/AUTOMATION.md §5)
//
// While Automation Arm is lit and the transport is playing, a gesture on an
// automatable control writes into the envelope of the clip playing on that
// control's track. The controls themselves are elsewhere (the mixer strip, the
// device parameter grid); all they do is call autoCapture() with the address
// they represent and the value they just wrote into the model. Everything that
// is actually subtle -- which clip, which beat, how many points, what a second
// pass over the same span means, and how it all folds into one undo entry --
// is decided here, once.
// ---------------------------------------------------------------------------

// The value resolution below which two samples of a gesture are the same
// gesture: 1/1024 of the target's range, which is finer than any control the UI
// draws. It is both the online threshold and the Douglas-Peucker tolerance, and
// deliberately the same number for both -- a point the first stage thought worth
// keeping and the second thought worth dropping would be a contradiction.
static constexpr f32 kAutoValueEpsFrac = 1.f / 1024.f;
// The longest a pass may go without writing anything down. It is not there to
// capture the shape -- a held control has no shape -- it is there so that punch
// (§5.3) has a span to erase: holding a knob still for two laps must leave a
// flat envelope, and it only can if the pass keeps stating where it has been.
static constexpr f64 kAutoMinSpacing = 1.0 / 64.0;

// Douglas-Peucker over p[a..b] inclusive, measuring the vertical distance from
// the chord (beat is the x axis, value the y). Marks the points worth keeping.
// A linear fade -- the single most common automation gesture there is --
// collapses to its two endpoints, and so does a held control.
static void autoDouglasPeucker(const std::vector<AutoPoint>& p, int a, int b,
                               f32 tol, std::vector<char>& keep) {
    if (b <= a + 1) return;
    const f64 x0 = p[(size_t)a].beat, x1 = p[(size_t)b].beat;
    const f64 y0 = p[(size_t)a].value, y1 = p[(size_t)b].value;
    const f64 dx = x1 - x0;
    f64 worst = -1.0;
    int wi = -1;
    for (int i = a + 1; i < b; ++i) {
        const f64 t = dx > 1e-12 ? (p[(size_t)i].beat - x0) / dx : 0.0;
        const f64 d = std::fabs((f64)p[(size_t)i].value - (y0 + (y1 - y0) * t));
        if (d > worst) { worst = d; wi = i; }
    }
    if (wi < 0 || worst <= (f64)tol) return;
    keep[(size_t)wi] = 1;
    autoDouglasPeucker(p, a, wi, tol, keep);
    autoDouglasPeucker(p, wi, b, tol, keep);
}

void App::toggleAutoArm() {
    autoArm_ = !autoArm_;
    if (!autoArm_) autoRecFinish();
    autoNoClipHint_ = false;
    status_ = autoArm_ ? "Automation arm on - move a control while a clip plays"
                       : "Automation arm off";
}

void App::autoCapture(const std::string& address, f32 value, u64 gesture) {
    if (!autoArm_ || !es_.playing) return;

    // Which track: the address says so. Resolving it here as well as at publish
    // time is not duplication -- it is the same question ("does this name
    // anything today?") asked at the moment the answer decides whether there is
    // anything to record onto, and it is what supplies the range the epsilon is
    // a fraction of.
    addr::Parsed p;
    if (!addr::parse(address, p) || p.scope != addr::Parsed::Scope::Track) return;
    int track = -1;
    for (size_t i = 0; i < ses_.tracks.size(); ++i)
        if (ses_.tracks[i].uid == p.scopeUid) { track = (int)i; break; }
    if (track < 0) return;
    RtAutoLane resolved;
    if (!resolveAutoLane(track, address, resolved)) return;

    // Which clip: the one playing on that track, and only while it is actually
    // playing (§5.1). Queued, stopping and stopped all mean there is no
    // clip-relative beat to stamp against.
    const int slot = es_.activeSlot[track];
    if (slot < 0 || slot >= (int)ses_.scenes.size() ||
        es_.slotState[track] != (int)SlotState::Playing) {
        // Latched, like kbdNoArmHint_: the state holds until the user launches
        // something, and saying it every frame of a fader drag would bury the
        // rest of the status bar.
        if (!autoNoClipHint_) {
            autoNoClipHint_ = true;
            status_ = "Automation arm: no clip playing on " + ses_.tracks[track].name;
        }
        return;
    }
    autoNoClipHint_ = false;

    ClipModel& m = ses_.tracks[track].slots[slot];
    if (!m.valid()) return;
    const f64 len = m.lengthBeats > 0.0 ? m.lengthBeats : 4.0;
    // The same number §3.1 evaluates the envelope against, which is the whole
    // point of stamping from clipPhase rather than from a GUI-side clock: the
    // round trip through the atomic costs one block, and one block is well under
    // a tenth of the shortest deliberate gesture a hand makes.
    const f64 beat = clampv(es_.clipPhase[track], 0.0, 1.0) * len;

    // The gesture a pass is keyed on. A widget owns ui_.active for the whole of
    // a drag, which is exactly the span a pass should be. A caller with no widget
    // behind it (a key, a test) gets a sentinel instead of 0, because a pass
    // keyed on 0 could never be told apart from "no widget owns the mouse" and
    // would therefore never be ended by the tick in pumpEngineEvents.
    static const u64 kAutoRecGesture = uiId(UiArrowGesture, 0, 7);
    const u64 g = gesture ? gesture : (ui_.active ? ui_.active : kAutoRecGesture);

    // --- start of a pass ---------------------------------------------------
    if (!autoRec_.active() || autoRec_.gesture != g || autoRec_.track != track ||
        autoRec_.slot != slot || autoRec_.clipUid != m.uid || autoRec_.address != address) {
        autoRecFinish();                       // whatever was running is over

        // §5.4: ONE undo entry for the whole pass, taken before the envelope
        // moves. The widget that called us has already taken its own entry for
        // the same gesture (undoPointWith, with the pre-edit model value), so
        // this coalesces into it and the pass costs one snapshot -- containing
        // the envelope as it was, which is what "undo the recording" has to
        // mean. A caller that took no entry gets one here instead.
        const std::string what = "automation: " + address;
        undoPoint(what.c_str(), g);

        int lane = -1;
        for (size_t i = 0; i < m.envelopes.size(); ++i)
            if (m.envelopes[i].address == address) { lane = (int)i; break; }
        if (lane < 0) {
            if ((int)m.envelopes.size() >= kMaxClipLanes) {
                status_ = "Clip already has " + std::to_string(kMaxClipLanes) + " envelopes";
                return;
            }
            AutoLane fresh;
            fresh.address = address;
            m.envelopes.push_back(std::move(fresh));
            lane = (int)m.envelopes.size() - 1;
        }
        autoRec_ = AutoRec{};
        autoRec_.gesture = g;
        autoRec_.track = track;
        autoRec_.slot = slot;
        autoRec_.clipUid = m.uid;
        autoRec_.address = address;
        autoRec_.lane = lane;
        autoRec_.lo = resolved.lo;
        autoRec_.hi = resolved.hi;
    }

    AutoLane& L = m.envelopes[(size_t)autoRec_.lane];
    L.enabled = true;                          // recording into it re-enables it
    const f32 eps = std::max(1e-7f, (autoRec_.hi - autoRec_.lo) * kAutoValueEpsFrac);
    const bool first = autoRec_.lastBeat < 0.0;
    const bool wrapped = !first && beat < autoRec_.lastBeat;

    // --- stage one of the thinning (§5.2) ----------------------------------
    if (!first && !wrapped &&
        std::fabs(value - autoRec_.lastValue) <= eps &&
        (beat - autoRec_.lastBeat) <= kAutoMinSpacing) return;

    // --- punch (§5.3) ------------------------------------------------------
    // A pass REPLACES the envelope over the beat span it covered: everything in
    // (lastAppendBeat, beat] goes before the new point is inserted. The first
    // append of a gesture erases nothing -- there is no previous append to span
    // from -- so a gesture that starts mid-envelope leaves the material before it
    // intact and produces a step at its start, exactly as Live does.
    //
    // Erasing only ever touches points AFTER this pass's own, because ours all
    // sit at or before lastBeat and the vector is sorted; the span indices below
    // therefore survive it. A wrap is the one case where that is not true, so
    // the span is closed there and a new one begins on the far side.
    if (!first) {
        const auto inPunch = [&](const AutoPoint& q) {
            return wrapped ? (q.beat > autoRec_.lastBeat || q.beat <= beat)
                           : (q.beat > autoRec_.lastBeat && q.beat <= beat);
        };
        // The point at `beat` itself is replaced rather than duplicated, which is
        // also what keeps beats unique.
        L.points.erase(std::remove_if(L.points.begin(), L.points.end(), inPunch),
                       L.points.end());
        if (wrapped) { autoRec_.spanFirst = -1; autoRec_.spanCount = 0; }
    } else {
        for (size_t i = 0; i < L.points.size(); ++i) {
            if (std::fabs(L.points[i].beat - beat) >= 1e-9) continue;
            L.points.erase(L.points.begin() + (long)i);
            break;
        }
    }

    // Total points across the clip, which is the bound the wire form declares.
    // A ceiling a human cannot reach by hand and a recording pass only reaches
    // if the thinning has failed, so hitting it is a bug report rather than a
    // limitation (§2.1) -- but it is still a ceiling, and the pass stops writing
    // at it rather than growing an array the engine will not accept.
    size_t total = 0;
    for (const AutoLane& q : m.envelopes) total += q.points.size();
    if (total >= (size_t)kMaxClipAutoPoints) {
        status_ = "Clip is at its automation point limit";
        return;
    }

    // Insert in beat order. After the punch above this is always the position
    // immediately past the pass's last point, so the span stays contiguous.
    size_t at = L.points.size();
    for (size_t i = 0; i < L.points.size(); ++i)
        if (L.points[i].beat > beat) { at = i; break; }
    AutoPoint np;
    np.beat = beat;
    np.value = clampv(value, autoRec_.lo, autoRec_.hi);
    L.points.insert(L.points.begin() + (long)at, np);

    if (autoRec_.spanFirst < 0) { autoRec_.spanFirst = (int)at; autoRec_.spanCount = 1; }
    else                          ++autoRec_.spanCount;
    autoRec_.lastBeat = beat;
    autoRec_.lastValue = np.value;

    // The engine is playing this clip, so the new point has to reach it now --
    // and a republish is also §4.3's "any recorded point" re-resolution trigger.
    pushClip(track, slot);
}

void App::autoRecFinish() {
    if (!autoRec_.active()) { autoRec_ = AutoRec{}; return; }
    const int track = autoRec_.track, slot = autoRec_.slot;
    const int spanFirst = autoRec_.spanFirst, spanCount = autoRec_.spanCount;
    const f32 tol = std::max(1e-7f, (autoRec_.hi - autoRec_.lo) * kAutoValueEpsFrac);
    const int lane = autoRec_.lane;
    const u64 clipUid = autoRec_.clipUid;
    autoRec_ = AutoRec{};                      // cleared first: nothing below re-enters

    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;
    ClipModel& m = ses_.tracks[track].slots[slot];
    if (m.uid != clipUid || lane < 0 || lane >= (int)m.envelopes.size()) return;
    std::vector<AutoPoint>& pts = m.envelopes[(size_t)lane].points;
    if (spanCount < 3 || spanFirst < 0 || spanFirst + spanCount > (int)pts.size()) return;

    // Stage two (§5.2): only the span THIS pass wrote is simplified. Points that
    // were already in the lane are never touched by a pass that did not cross
    // them, which is what makes a punch a local edit rather than a rewrite.
    const int a = spanFirst, b = spanFirst + spanCount - 1;
    std::vector<char> keep(pts.size(), 0);
    keep[(size_t)a] = keep[(size_t)b] = 1;
    autoDouglasPeucker(pts, a, b, tol, keep);
    std::vector<AutoPoint> out;
    out.reserve(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        if ((int)i < a || (int)i > b || keep[i]) out.push_back(pts[i]);
    if (out.size() == pts.size()) return;      // nothing to collapse, nothing to push
    pts.swap(out);
    pushClip(track, slot);
}

} // namespace lat
