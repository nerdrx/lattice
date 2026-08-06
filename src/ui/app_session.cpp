// Session view: track headers, the clip grid, clip slots, the scene column,
// the mixer, return + master strips, drag/drop (and drawDragGhost, kept
// beside the resolution logic it visualises), plus the clip-model helpers
// the grid's mouse handling drives (loadClipInto / createMidiClip /
// selectTrack / addTrack / …). Moved verbatim from app.cpp.
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

// One decoded file, as a clip. Factored out of loadClipInto because the
// arrangement can be dropped on directly (docs/ARRANGEMENT.md §7.5) and an item
// owns its clip by value, so there is no slot in the middle -- and because two
// places building a clip from a file two ways is how they end up disagreeing
// about the default loop, the guessed tempo or the warp mode.
//
// Deliberately does NOT touch the session: no uid, no undo point, no push. The
// caller decides what the clip becomes.
bool App::makeClipFromFile(const std::string& path, int colorIdx, ClipModel& out) {
    SampleRef sb = loadSample(path, eng_.sampleRate());
    if (!sb) return false;
    out = ClipModel{};
    out.kind = ClipKind::Audio;
    out.sample = sb;
    out.path = path;
    out.name = sb->name;
    const size_t dot = out.name.find_last_of('.');
    if (dot != std::string::npos) out.name = out.name.substr(0, dot);
    out.colorIdx = colorIdx;
    out.clipBpm = sb->guessedBpm;
    out.lengthBeats = sb->guessedBeats;
    out.loopStart = 0;
    out.loopEnd = sb->frames;
    out.gain = 1.f;
    out.warp = Warp::Beats;
    out.loop = true;
    return true;
}

void App::loadClipInto(int track, int slot, const std::string& path) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;
    ClipModel fresh;
    if (!makeClipFromFile(path, ses_.tracks[track].colorIdx, fresh)) {
        status_ = "Could not load " + path;
        return;
    }
    // After the decode, so a file that could not be read leaves no history
    // behind, and before the slot is touched.
    undoPoint("load clip");

    ClipModel& m = ses_.tracks[track].slots[slot];
    // A slot that already held a clip keeps its identity: the material behind
    // it changed, but anything pointing at the clip (automation, a controller
    // mapping) still means this clip. Kept across the assignment for the same
    // reason, since `fresh` has no uid of its own.
    const u64 keep = m.uid ? m.uid : ses_.newUid();
    // Dropping a sample onto a pattern turns the slot back into an audio clip;
    // pushClip retires the notes the engine was holding for it.
    m = std::move(fresh);
    m.uid = keep;
    pushClip(track, slot);
    selectTrack(track); selSlot_ = slot;
    status_ = "Loaded " + m.name;
}

void App::clearClip(int track, int slot) {
    ses_.tracks[track].slots[slot] = ClipModel{};
    // Through pushClip rather than a bare ClearClip: an emptied slot still has
    // to hand its note array back before anything frees it.
    pushClip(track, slot);
}

// Note-capable means the chain can be *played*: an instrument, or an effect
// that takes MIDI in (an arpeggiator, a MIDI-controlled filter). Either makes
// the track's empty slots MIDI targets rather than audio ones.
bool App::trackHasNoteDevice(int track) const {
    if (track < 0 || track >= (int)ses_.tracks.size()) return false;
    for (const DeviceModel& d : ses_.tracks[track].devices)
        if (d.desc.kind == PluginKind::Instrument || d.desc.hasMidiIn) return true;
    return false;
}

// An empty MIDI clip is a real, launchable, editable entity — Live's "create
// empty clip", and the only way to get a pattern without playing one in.
void App::createMidiClip(int track, int slot) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;

    // Here rather than at the (single) call site: this is the whole edit, and
    // the slot is untouched until the next line.
    undoPoint("new clip");

    ClipModel& m = ses_.tracks[track].slots[slot];
    m = ClipModel{};
    m.uid = ses_.newUid();
    m.kind = ClipKind::Midi;
    char buf[32];
    snprintf(buf, sizeof buf, "MIDI %d", midiClipNo_++);
    m.name = buf;
    m.colorIdx = ses_.tracks[track].colorIdx;
    m.lengthBeats = 4.0;                       // one bar in 4/4, like Live
    m.loop = true;
    pushClip(track, slot);
    selectTrack(track); selSlot_ = slot;
    detailTab_ = DetailTab::Clip;
    status_ = "New " + m.name;
}

// Points the DEVICES tab somewhere. Not an edit and not undoable -- it is the
// same kind of move as selecting a track, which is explicitly outside the
// history (see app.h).
void App::selectChainOwner(int owner) {
    if (!chainOwner(owner).valid()) return;
    if (devOwner_ != owner) {
        selDevice_ = -1;
        stripScroll_ = 0.f;
        paramScroll_ = 0.f;
    }
    devOwner_ = owner;
    // A bus has no clips, so the CLIP tab has nothing to show for it and the
    // panel would sit there looking at the last track's clip instead. Only the
    // tab is switched: a hidden panel stays hidden.
    if (!ownIsTrack(owner) && detailTab_ != DetailTab::Devices) {
        detailTab_ = DetailTab::Devices;
        ensurePluginScan();
    }
}

// Live's exclusive record-arm, which is what makes the computer keyboard and a
// controller play the track you just clicked on without a second gesture. The
// arm this hands out is ours to take back; one the user set by hand is not.
void App::selectTrack(int track) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    selTrack_ = track;
    // The device view follows the selection back off a bus. Guarded so that
    // clicking around the grid on the track already selected does not reset the
    // chain the user is editing every frame.
    if (devOwner_ != track) selectChainOwner(track);
    if (autoArmed_ == track) return;

    if (autoArmed_ >= 0 && autoArmed_ < (int)ses_.tracks.size()) {
        TrackModel& prev = ses_.tracks[autoArmed_];
        if (prev.arm) { prev.arm = false; send(Cmd::TrackArm, autoArmed_, 0); }
    }
    autoArmed_ = -1;

    TrackModel& t = ses_.tracks[track];
    if (t.arm) return;              // armed by hand: leave it, and do not claim it
    t.arm = true;
    send(Cmd::TrackArm, track, 1);
    autoArmed_ = track;
}

void App::addTrack() {
    if (ses_.tracks.size() >= kMaxTracks) return;
    TrackModel t;
    char buf[32];
    snprintf(buf, sizeof buf, "%zu Audio", ses_.tracks.size() + 1);
    t.uid = ses_.newUid();
    t.name = buf;
    t.colorIdx = (int)(ses_.tracks.size() * 3 + 4) % pal::clipColorCount;
    ses_.tracks.push_back(std::move(t));   // TrackModel is move-only (devices)
    pushTrack((int)ses_.tracks.size() - 1);
}

void App::addScene() {
    if (ses_.scenes.size() >= kMaxScenes) return;
    SceneModel s;
    char buf[32];
    snprintf(buf, sizeof buf, "Scene %zu", ses_.scenes.size() + 1);
    s.uid = ses_.newUid();
    s.name = buf;
    ses_.scenes.push_back(s);
}


// ---------------------------------------------------------------------------
// session view
// ---------------------------------------------------------------------------

void App::drawSessionView(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::appBg);

    // Right-hand furniture, in Live's order: the scene launchers stay against
    // the clip grid (their rows line up with it), then the return buses, then
    // the master. Everything the mix ends up in reads left to right.
    const f32 masterW = lay::masterW * s;
    const f32 sceneW  = lay::sceneColW * s;
    const f32 retW    = lay::returnW * s * kMaxReturns;
    Rect masterCol{r.right() - masterW, r.y, masterW, r.h};
    Rect retCol{masterCol.x - retW, r.y, retW, r.h};
    Rect sceneCol{retCol.x - sceneW, r.y, sceneW, r.h};
    Rect tracksCol{r.x, r.y, std::max(0.f, sceneCol.x - r.x), r.h};

    // Horizontal scroll over the track area.
    f32 totalW = 0.f;
    for (const auto& t : ses_.tracks) totalW += t.width * s + lay::gutter * s;
    const f32 maxScroll = std::max(0.f, totalW - tracksCol.w);
    if (tracksCol.contains(in.mx, in.my) && in.wheel != 0.f && in.shift())
        gridScrollX_ = clampv(gridScrollX_ - in.wheel * 60.f * s, 0.f, maxScroll);
    gridScrollX_ = clampv(gridScrollX_, 0.f, maxScroll);

    rend_.pushClip(tracksCol);
    drawTrackHeaders(tracksCol, gridScrollX_);
    drawClipGrid(tracksCol, gridScrollX_);
    drawMixer(tracksCol, gridScrollX_);
    rend_.popClip();

    drawSceneColumn(sceneCol);
    drawReturnStrips(retCol);
    drawMasterStrip(masterCol);
}

void App::drawTrackHeaders(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const f32 h = lay::trackHeadH * s;
    f32 x = r.x - scrollX;

    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        TrackModel& t = ses_.tracks[i];
        const f32 w = t.width * s;
        Rect cell{x, r.y, w - lay::gutter * s, h};
        x += w;
        if (cell.right() < r.x || cell.x > r.right()) continue;

        const bool sel = (int)i == selTrack_;
        const u64 id = uiId(3, (int)i);
        const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
        rend_.rect(cell, sel ? pal::gridBg : (hot ? pal::slotHover : pal::panel));
        // Colour chip so the track's identity reads at a glance, as in Live.
        rend_.rect({cell.x, cell.y, cell.w, 2 * s}, pal::clipColors[t.colorIdx % pal::clipColorCount]);

        // textField writes the new name and only then says it committed, and it
        // can only commit on a frame where it already owns the caret -- so the
        // old name is captured then, and only then.
        const u64 nameId = uiId(3, 1000 + (int)i);
        std::string wasName;
        if (ui_.editId == nameId) wasName = t.name;
        if (ui_.textField(nameId, cell, &t.name,
                          Col(0, 0, 0, 0), sel ? pal::text : pal::textDim, Align::Left))
            undoPointWith("rename track", t.name, wasName);
        if (hot && in.pressed[0]) selectTrack((int)i);
    }

    // "+" to append a track.
    Rect add{x, r.y, 22 * s, h};
    if (add.x < r.right()) {
        if (ui_.button(uiId(3, 900), add, "+")) { undoPoint("add track"); addTrack(); }
    }
}

void App::drawClipGrid(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    const f32 slotH = lay::slotH * s;
    const f32 top = r.y + lay::trackHeadH * s;
    const f32 mixerTop = r.bottom() - lay::mixerH * s;
    const int ns = (int)ses_.scenes.size();

    Rect grid{r.x, top, r.w, mixerTop - top};
    rend_.pushClip(grid);
    rend_.rect(grid, pal::appBg);

    f32 x = r.x - scrollX;
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        const f32 w = ses_.tracks[ti].width * s;
        // Each track reads as a continuous lane all the way down to the mixer,
        // otherwise the grid ends in a hard shelf under the last scene.
        rend_.rect({x, top, w - lay::gutter * s, grid.h},
                   (int)ti == selTrack_ ? pal::appBg.scale(1.35f) : pal::appBg.scale(1.15f));
        for (int si = 0; si < ns; ++si) {
            Rect cell{x, top + si * slotH, w - lay::gutter * s, slotH - lay::gutter * s};
            if (cell.bottom() > mixerTop) break;
            if (cell.right() >= r.x && cell.x <= r.right()) drawClipSlot(cell, (int)ti, si);
        }
        // Per-track stop button, directly under the last scene row.
        Rect stopCell{x, top + ns * slotH, w - lay::gutter * s, slotH - lay::gutter * s};
        if (stopCell.bottom() <= mixerTop && stopCell.right() >= r.x && stopCell.x <= r.right()) {
            const u64 id = uiId(4, 5000 + (int)ti);
            const bool hot = ui_.setHot(id, stopCell) && ui_.isHot(id);
            rend_.roundRect(stopCell, 2 * s, hot ? pal::slotHover : pal::slotEmpty);
            ui_.stopSquare(stopCell, pal::textDim);
            if (hot) ui_.cursor = Cursor::Hand;
            if (hot && win_.input().pressed[0]) send(Cmd::StopTrack, (int)ti);
        }
        x += w;
    }
    rend_.popClip();
}

void App::drawClipSlot(const Rect& cell, int ti, int si) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const ClipModel& m = ses_.tracks[ti].slots[si];
    const u64 id = uiId(4, ti, si);
    const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
    const bool sel = ti == selTrack_ && si == selSlot_;

    // ONE sample of the engine, taken at the top of the frame (engine_state.h).
    // These four used to be four independent relaxed loads made microseconds to
    // milliseconds apart, and two of them could straddle a publish: a slot drawn
    // Playing with activeSlot == -1 is the shape that produced.
    const int state = es_.slotState[ti];
    const int active = es_.activeSlot[ti];
    const int pending = es_.pendingSlot[ti];
    const bool playing = (state == (int)SlotState::Playing || state == (int)SlotState::StopQueued) && active == si;
    const bool queued  = pending == si;

    // Recording truth comes from the engine, not from what we asked for: the
    // start is quantized, so a slot can sit queued for a bar before it captures.
    const int recPhase = es_.recState[ti];
    const bool recHere = recPhase != 0 && es_.recSlotIdx[ti] == si;

    if (!m.valid()) {
        const bool target = recIntent_ && ses_.tracks[ti].arm;
        if (recHere && recPhase >= 2) {
            // Capturing. Solid red, with the beats it has been running for.
            rend_.roundRect(cell, 2 * s, pal::recRed);
            rend_.circle(cell.x + 8 * s, cell.cy(), 3.5f * s, pal::textOnClip);
            char buf[24];
            snprintf(buf, sizeof buf, "%.1f",
                     std::max(0.0, es_.beat - recStartBeat_[ti]));
            rend_.textIn(fSmall_, {cell.x + 14 * s, cell.y, cell.w - 18 * s, cell.h},
                         buf, pal::textOnClip, Align::Right, 0);
        } else if (recHere) {
            // Queued: a pulsing ring, the record-side counterpart of the
            // blinking clip a launch shows while it waits for the quantum.
            const f32 ph = (f32)(0.5 + 0.5 * std::sin(nowSeconds() * 8.0));
            rend_.roundRect(cell, 2 * s, pal::slotEmpty);
            rend_.roundRectOutline(cell, 2 * s, 1.5f * s,
                                   pal::recRed.scale(0.35f + 0.4f * ph));
        } else {
            rend_.roundRect(cell, 2 * s, hot ? pal::slotHover : pal::slotEmpty);
            // Armed track, record intent lit: this slot is a take waiting to
            // happen, so say so before the click rather than after.
            if (target) rend_.circle(cell.x + 8 * s, cell.cy(), 3 * s,
                                     pal::recRed.scale(hot ? 0.9f : 0.55f));
        }
        if (sel) rend_.roundRectOutline(cell, 2 * s, 1 * s, pal::accent);
        if (hot) {
            ui_.cursor = Cursor::Hand;
            if (in.pressed[0]) {
                selectTrack(ti); selSlot_ = si;
                if (recHere)      stopRecording(ti);       // second click stops
                else if (target)  startRecording(ti, si);
            }
            // Double-click on an empty slot of a note-capable track makes an
            // empty pattern to draw into. Only when the record button is unlit:
            // with it lit the same slot is a take waiting to happen, and the
            // first click of the double has already started one.
            if (in.dblClick && !recIntent_ && !recHere && trackHasNoteDevice(ti))
                createMidiClip(ti, si);
        }
        return;
    }

    const Col base = pal::clipColors[m.colorIdx % pal::clipColorCount];
    Col fill = base.scale(playing ? 1.0f : (hot ? 0.88f : 0.76f));
    if (queued) {
        // Pulse while waiting for the launch quantum, like Live's blinking slot.
        const f32 ph = (f32)(0.5 + 0.5 * std::sin(nowSeconds() * 8.0));
        fill = base.scale(0.55f + 0.45f * ph);
    }
    rend_.roundRect(cell, 2 * s, fill);

    // Launch button zone on the left.
    const f32 btnW = 14 * s;
    Rect btn{cell.x, cell.y, btnW, cell.h};
    if (playing) ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), pal::playGreen.scale(0.85f));
    else         ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), pal::textOnClip.alpha(0.55f));

    // Recording into a slot that already holds a clip is an overdub, so the
    // slot keeps its playing look and gains the record dot rather than turning
    // solid red the way a slot being captured into from empty does: what is on
    // screen is still the clip, and it is still playing.
    f32 nameW = cell.w - btnW - 2 * s;
    f32 markRight = cell.right();
    if (recHere) {
        // Pulsing while the take waits for its quantum, solid once it is
        // capturing - the same two states the empty-slot look has, said quietly.
        const f32 a = recPhase >= 2 ? 1.f : (f32)(0.45 + 0.45 * std::sin(nowSeconds() * 8.0));
        rend_.circle(markRight - 7 * s, cell.cy(), 3.5f * s, pal::recRed.alpha(a));
        markRight -= 13 * s;
        nameW -= 13 * s;
    }

    // A MIDI clip gets a three-dot mark on the right: at 21px of row height a
    // real piano glyph is a smudge, and the dots read as "notes, not audio"
    // without competing with the name.
    if (m.kind == ClipKind::Midi) {
        const f32 d = 1.6f * s;
        const f32 dx0 = markRight - 12 * s;
        for (int i = 0; i < 3; ++i)
            rend_.rect({dx0 + i * 3.5f * s, cell.cy() - d * 0.5f - (i == 1 ? 2 * s : 0.f), d, d},
                       pal::textOnClip.alpha(0.6f));
        nameW -= 14 * s;
    }
    rend_.textIn(fBody_, {cell.x + btnW, cell.y, std::max(4 * s, nameW), cell.h},
                 m.name.c_str(), pal::textOnClip, Align::Left, 2 * s);

    // Playback progress along the bottom edge. The engine publishes clipPhase
    // for a MIDI clip exactly as for an audio one, so this needs no special case.
    if (playing) {
        const f64 ph = clampv(es_.clipPhase[ti], 0.0, 1.0);
        rend_.rect({cell.x, cell.bottom() - 2 * s, cell.w * (f32)ph, 2 * s}, pal::textOnClip.alpha(0.45f));
    }
    if (sel) rend_.roundRectOutline(cell, 2 * s, 1 * s, pal::accent);

    if (hot) {
        ui_.cursor = Cursor::Hand;
        if (in.pressed[0]) {
            selectTrack(ti); selSlot_ = si;
            // With the record button lit, a MIDI clip on an armed track is an
            // overdub target and not just something to launch: the engine
            // relaunches it at the record boundary and captures another pass
            // into it (see the Cmd::RecordMidiSlot contract). A second click
            // stops the take, exactly as on an empty slot. Audio clips are
            // untouched by this - there is no overdub for a sample.
            const bool overdub = recIntent_ && ses_.tracks[ti].arm &&
                                 m.kind == ClipKind::Midi && trackHasNoteDevice(ti);
            if (recHere)       stopRecording(ti);
            else if (overdub)  startRecording(ti, si);
            else               send(Cmd::LaunchClip, ti, si);
            drag_.kind = DragState::Kind::Clip;
            drag_.srcTrack = ti; drag_.srcSlot = si;
            drag_.startX = in.mx; drag_.startY = in.my;
            drag_.armed = false;
        }
        if (in.pressed[2]) {
            selectTrack(ti); selSlot_ = si;
            undoPoint("clear clip");
            clearClip(ti, si);
        }
    }

    // Drop target for a drag in flight.
    if (drag_.kind != DragState::Kind::None && drag_.armed && hot && in.released[0]) {
        if (drag_.kind == DragState::Kind::BrowserFile) {
            loadClipInto(ti, si, drag_.path);   // takes its own entry, after the decode
        } else if (drag_.srcTrack != ti || drag_.srcSlot != si) {
            // One entry for the whole move: the destination write and the
            // source clear are halves of the same edit.
            undoPoint(in.ctrl() ? "copy clip" : "move clip");
            ses_.tracks[ti].slots[si] = ses_.tracks[drag_.srcTrack].slots[drag_.srcSlot];
            if (!in.ctrl()) clearClip(drag_.srcTrack, drag_.srcSlot);
            pushClip(ti, si);
        }
        drag_ = DragState{};
    }
}

void App::drawSceneColumn(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const f32 slotH = lay::slotH * s;
    const f32 top = r.y + lay::trackHeadH * s;
    const int ns = (int)ses_.scenes.size();

    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, 1 * s, r.h}, pal::divider);

    Rect head{r.x, r.y, r.w, lay::trackHeadH * s};
    rend_.rect(head, pal::panelAlt);
    rend_.textIn(fBold_, head, "SCENES", pal::textDim, Align::Center);

    for (int si = 0; si < ns; ++si) {
        Rect cell{r.x + 2 * s, top + si * slotH, r.w - 4 * s, slotH - lay::gutter * s};
        if (cell.bottom() > r.bottom() - lay::mixerH * s) break;
        const u64 id = uiId(5, si);
        const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
        const bool sel = si == selSlot_;
        rend_.roundRect(cell, 2 * s, sel ? pal::gridBg : (hot ? pal::slotHover : pal::panelAlt));

        Rect btn{cell.x, cell.y, 14 * s, cell.h};
        ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), pal::textDim);
        const u64 nameId = uiId(5, 1000 + si);
        std::string wasName;                     // see drawTrackHeaders
        if (ui_.editId == nameId) wasName = ses_.scenes[si].name;
        if (ui_.textField(nameId, {cell.x + 14 * s, cell.y, cell.w - 16 * s, cell.h},
                          &ses_.scenes[si].name, Col(0, 0, 0, 0), pal::text, Align::Left))
            undoPointWith("rename scene", ses_.scenes[si].name, wasName);

        if (hot) ui_.cursor = Cursor::Hand;
        if (hot && in.pressed[0]) { selSlot_ = si; send(Cmd::LaunchScene, si); }
    }

    Rect stopAll{r.x + 2 * s, top + ns * slotH, r.w - 4 * s, slotH - lay::gutter * s};
    if (stopAll.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 900), stopAll, "STOP ALL")) send(Cmd::StopAll);
    }

    Rect add{r.x + 2 * s, stopAll.bottom() + 4 * s, r.w - 4 * s, 18 * s};
    if (add.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 901), add, "+ Scene")) { undoPoint("add scene"); addScene(); }
    }
}

void App::drawMixer(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    const f32 top = r.bottom() - lay::mixerH * s;
    Rect mix{r.x, top, r.w, lay::mixerH * s};
    rend_.pushClip(mix);
    rend_.rect(mix, pal::panel);
    rend_.rect({mix.x, mix.y, mix.w, 1 * s}, pal::divider);

    f32 x = r.x - scrollX;
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        TrackModel& t = ses_.tracks[ti];
        const f32 w = t.width * s;
        Rect col{x, top, w - lay::gutter * s, mix.h};
        x += w;
        if (col.right() < r.x || col.x > r.right()) continue;
        if ((int)ti == selTrack_) rend_.rect(col, pal::panelAlt);

        f32 y = col.y + 6 * s;

        // M / S / arm row
        const f32 bw = (col.w - 16 * s) / 3.f;
        Rect mr{col.x + 6 * s, y, bw - 2 * s, 15 * s};
        Rect sr{mr.right() + 2 * s, y, bw - 2 * s, 15 * s};
        Rect ar{sr.right() + 2 * s, y, bw - 2 * s, 15 * s};
        // Every control in this strip is bound straight to the model and writes
        // before it reports, so each hands its previous value to the entry.
        const bool wasMute = t.mute, wasSolo = t.solo, wasArm = t.arm;
        const f32  wasPan = t.pan, wasFader = t.fader;
        // Every automatable control on this strip reports its move to
        // autoCapture as well as to the engine (docs/AUTOMATION.md §5.1). The
        // call is unconditional by design: whether anything is recorded — the
        // arm, the transport, which clip is playing, the beat, the thinning —
        // is one decision and it lives in autoCapture, so a second copy of it
        // here could only ever come to disagree. The value handed over is what
        // the widget just wrote into the model, in the target's own units
        // (§2.3), and the widget's id is the gesture, so one drag is one pass
        // and one undo entry.
        if (ui_.squareToggle(uiId(6, (int)ti, 0), mr, "M", &t.mute, pal::meterAmber)) {
            undoPointWith("mute", t.mute, wasMute);
            send(Cmd::TrackMute, (int)ti, t.mute ? 1 : 0);
            // Mute has no AutoTarget yet (it is reserved), so this records into
            // a lane the publisher will skip until it does. Spelled anyway: the
            // call site is the part that is easy to forget when it lands.
            autoCapture(addr::trackField(t.uid, "mute"), t.mute ? 1.f : 0.f,
                        uiId(6, (int)ti, 0));
        }
        if (ui_.squareToggle(uiId(6, (int)ti, 1), sr, "S", &t.solo, pal::soloBlue)) {
            undoPointWith("solo", t.solo, wasSolo);
            send(Cmd::TrackSolo, (int)ti, t.solo ? 1 : 0);
        }
        // Record-arm is a filled dot in Live, and the glyph atlas is ASCII-only,
        // so draw the dot rather than trying to letter it.
        if (ui_.squareToggle(uiId(6, (int)ti, 2), ar, "", &t.arm, pal::armRed)) {
            // Arming by hand is an edit; the auto-arm that follows the
            // selection is not, and takes no entry of its own.
            undoPointWith("arm", t.arm, wasArm);
            send(Cmd::TrackArm, (int)ti, t.arm ? 1 : 0);
            // Touched by hand: this arm is the user's now, so selecting another
            // track must not take it away again.
            if ((int)ti == autoArmed_) autoArmed_ = -1;
        }
        rend_.circle(ar.cx(), ar.cy(), 3.5f * s, t.arm ? pal::textOnClip : pal::armRed);
        y += 20 * s;

        // Sends A-D, above the pan knob as a 2x2 grid. A strip is 94px wide, so
        // four knobs in a row would be 12px across and unusable; two rows of two
        // leave room for a 15px knob with its letter beside it, which is the
        // smallest thing here that still reads as a send and not as a dot.
        // Anything the user has dialled in also shows as an arc, so a track with
        // send on it is visible without hovering.
        {
            const f32 cellW = (col.w - 12 * s) * 0.5f;
            const f32 rowH  = 18 * s;
            for (int rn = 0; rn < kMaxReturns; ++rn) {
                Rect cell{col.x + 6 * s + (rn % 2) * cellW, y + (rn / 2) * rowH, cellW, rowH};
                rend_.textIn(fSmall_, {cell.x, cell.y, 9 * s, cell.h}, kReturnLetter[rn],
                             pal::textFaint, Align::Left, 0);
                Rect kr{cell.x + 10 * s, cell.y + 1 * s, 15 * s, 15 * s};
                const f32 wasSend = t.sends[rn];
                if (ui_.knob(uiId(6, (int)ti, 10 + rn), kr, &t.sends[rn], 0.f, 1.f, 0.f)) {
                    undoPointWith(kSendUndo[rn], t.sends[rn], wasSend);
                    send(Cmd::SendLevel, (int)ti, rn, t.sends[rn]);
                    autoCapture(addr::trackSend(t.uid, rn), t.sends[rn],
                                uiId(6, (int)ti, 10 + rn));
                }
            }
            y += 2 * rowH + 3 * s;
        }

        // Pan
        Rect pan{col.cx() - 11 * s, y, 22 * s, 22 * s};
        if (ui_.knob(uiId(6, (int)ti, 3), pan, &t.pan, -1.f, 1.f, 0.f)) {
            undoPointWith("pan", t.pan, wasPan);
            send(Cmd::TrackPan, (int)ti, 0, t.pan);
            autoCapture(addr::trackField(t.uid, "pan"), t.pan, uiId(6, (int)ti, 3));
        }
        y += 26 * s;

        // Fader + meter
        const f32 fh = col.bottom() - y - 6 * s;
        Rect fader{col.x + 10 * s, y, 16 * s, fh};
        Rect meter{fader.right() + 5 * s, y, 9 * s, fh};
        if (ui_.vFader(uiId(6, (int)ti, 4), fader, &t.fader)) {
            undoPointWith("volume", t.fader, wasFader);
            send(Cmd::TrackVol, (int)ti, 0, faderToGain(t.fader));
            // The FADER POSITION, not the gain: the envelope stores what the UI
            // edits and AutoXform::Fader is what turns it into a gain (§2.3).
            autoCapture(addr::trackField(t.uid, "vol"), t.fader, uiId(6, (int)ti, 4));
        }

        const f32 lvl = std::max(es_.meterL[ti], es_.meterR[ti]);
        peakHoldT_[ti] = std::max(lvl, peakHoldT_[ti] * 0.985f);
        ui_.meterV(meter, lvl, peakHoldT_[ti]);
    }
    rend_.popClip();
}

// The A-D buses. No clips, no M/S/arm, no pan: a return is a name, a chain and
// a level, so the strip is a header, the chain's device names where a track has
// its grid, and a fader with its meter. Clicking anywhere that is not a control
// points the DEVICES tab at the bus, which is the only way to edit its chain.
void App::drawReturnStrips(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    if (r.w <= 0.f) return;
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, 1 * s, r.h}, pal::divider);

    const f32 colW = r.w / (f32)kMaxReturns;
    const f32 top  = r.bottom() - lay::mixerH * s;

    for (int i = 0; i < kMaxReturns; ++i) {
        ReturnModel& rt = ses_.returns[i];
        const int owner = ownReturn(i);
        const bool sel  = devOwner_ == owner;
        Rect col{r.x + i * colW, r.y, colW - lay::gutter * s, r.h};

        // Claimed first so the fader and the name field can take hot back --
        // the same last-setHot-wins trick the device boxes use.
        const u64 id = uiId(13, i, 0);
        const bool hot = ui_.setHot(id, col) && ui_.isHot(id);
        rend_.rect(col, sel ? pal::panelAlt : pal::panel);

        Rect head{col.x, col.y, col.w, lay::trackHeadH * s};
        rend_.rect(head, sel ? pal::gridBg : pal::panelAlt);
        rend_.rect({head.x, head.y, head.w, 2 * s}, pal::soloBlue);
        rend_.textIn(fBold_, {head.x + 3 * s, head.y, 10 * s, head.h}, kReturnLetter[i],
                     sel ? pal::text : pal::textDim, Align::Left, 0);
        // The model's placeholder name is "Return" for all four buses, which
        // says nothing in a strip this narrow and would be clipped to "Retu"
        // anyway -- so the letter carries the identity and the field stays
        // blank until the bus is named. A DISPLAY choice, deliberately: writing
        // a letter into the model would make every set on disk carry four
        // return blocks it has no reason to (see project.cpp's `interesting`).
        const u64 nameId = uiId(13, i, 1);
        std::string shown = (rt.name == kReturnPlaceholder) ? std::string() : rt.name;
        if (ui_.textField(nameId, {head.x + 13 * s, head.y, head.w - 15 * s, head.h},
                          &shown, Col(0, 0, 0, 0), sel ? pal::text : pal::textDim, Align::Left)) {
            const std::string was = rt.name;
            rt.name = shown.empty() ? std::string(kReturnPlaceholder) : shown;
            undoPointWith("rename return", rt.name, was);
        }

        // What the bus is made of, in the space a track spends on clips. A
        // return with an empty chain is inert, and saying so beats an empty
        // column the user has no reason to click on.
        Rect body{col.x, head.bottom(), col.w, top - head.bottom()};
        rend_.pushClip(body);
        if (rt.devices.empty()) {
            rend_.textIn(fSmall_, {body.x, body.y + 6 * s, body.w, 12 * s}, "no fx",
                         pal::textFaint, Align::Center, 0);
        } else {
            f32 dy = body.y + 4 * s;
            for (const DeviceModel& d : rt.devices) {
                if (dy + 12 * s > body.bottom()) break;
                Rect row{body.x + 3 * s, dy, body.w - 6 * s, 12 * s};
                rend_.roundRect(row, 2 * s, pal::panelAlt);
                rend_.pushClip(row);
                rend_.textIn(fSmall_, row, d.desc.name.c_str(),
                             d.inst ? pal::textDim : pal::armRed, Align::Left, 3 * s);
                rend_.popClip();
                dy += 14 * s;
            }
        }
        rend_.popClip();

        Rect mix{col.x, top, col.w, r.bottom() - top};
        rend_.rect({mix.x, mix.y, mix.w, 1 * s}, pal::divider);
        // The same top inset the master strip uses, so the buses and the mix
        // they land in read as one row of faders rather than a staircase.
        f32 y = mix.y + 26 * s;
        const f32 fh = mix.bottom() - y - 6 * s;
        Rect fader{mix.x + 10 * s, y, 15 * s, fh};
        Rect meter{fader.right() + 5 * s, y, 9 * s, fh};

        const f32 wasFader = rt.fader;
        if (ui_.vFader(uiId(13, i, 2), fader, &rt.fader)) {
            undoPointWith("return volume", rt.fader, wasFader);
            send(Cmd::ReturnVol, i, 0, faderToGain(rt.fader));
        }
        const f32 lvl = std::max(es_.returnMeterL[i], es_.returnMeterR[i]);
        peakHoldR_[i] = std::max(lvl, peakHoldR_[i] * 0.985f);
        ui_.meterV(meter, lvl, peakHoldR_[i]);

        if (sel) rend_.roundRectOutline(col, 2 * s, 1 * s, pal::accent);
        if (hot) {
            ui_.cursor = Cursor::Hand;
            if (in.pressed[0]) selectChainOwner(owner);
        }
    }
}

void App::drawMasterStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const bool sel = devOwner_ == kOwnMaster;
    rend_.rect(r, pal::panelAlt);
    rend_.rect({r.x, r.y, 1 * s, r.h}, pal::divider);

    // Same deal as a return: the strip is the handle for the master chain, so
    // the whole column is a click target that the controls in it take back.
    const u64 id = uiId(7, 10);
    const bool hot = ui_.setHot(id, r) && ui_.isHot(id);

    Rect head{r.x, r.y, r.w, lay::trackHeadH * s};
    rend_.rect(head, sel ? pal::gridBg : pal::panel);
    rend_.textIn(fBold_, head, "MASTER", pal::text, Align::Center);

    const f32 top = r.bottom() - lay::mixerH * s;
    Rect mix{r.x, top, r.w, lay::mixerH * s};
    rend_.rect({mix.x, mix.y, mix.w, 1 * s}, pal::divider);

    // The master chain, where a return lists its own: this is where a bus
    // compressor or a saturator across the whole mix lives.
    {
        Rect body{r.x, head.bottom(), r.w, top - head.bottom()};
        rend_.pushClip(body);
        f32 dy = body.y + 4 * s;
        for (const DeviceModel& d : ses_.masterDevices) {
            if (dy + 12 * s > body.bottom()) break;
            Rect row{body.x + 4 * s, dy, body.w - 8 * s, 12 * s};
            rend_.roundRect(row, 2 * s, pal::panel);
            rend_.pushClip(row);
            rend_.textIn(fSmall_, row, d.desc.name.c_str(),
                         d.inst ? pal::textDim : pal::armRed, Align::Left, 3 * s);
            rend_.popClip();
            dy += 14 * s;
        }
        if (ses_.masterDevices.empty())
            rend_.textIn(fSmall_, {body.x, body.y + 6 * s, body.w, 12 * s}, "no fx",
                         pal::textFaint, Align::Center, 0);
        rend_.popClip();
    }

    static f32 masterFader = 0.85f;
    f32 y = mix.y + 26 * s;
    const f32 fh = mix.bottom() - y - 6 * s;
    Rect fader{mix.x + 12 * s, y, 16 * s, fh};
    Rect meterL{fader.right() + 6 * s, y, 9 * s, fh};
    Rect meterR{meterL.right() + 3 * s, y, 9 * s, fh};

    if (ui_.vFader(uiId(7, 0), fader, &masterFader))
        send(Cmd::MasterVol, 0, 0, faderToGain(masterFader));

    const f32 l = es_.masterMeterL, rr = es_.masterMeterR;
    peakHoldM_[0] = std::max(l, peakHoldM_[0] * 0.985f);
    peakHoldM_[1] = std::max(rr, peakHoldM_[1] * 0.985f);
    ui_.meterV(meterL, l, peakHoldM_[0]);
    ui_.meterV(meterR, rr, peakHoldM_[1]);

    if (sel) rend_.roundRectOutline(r, 2 * s, 1 * s, pal::accent);
    if (hot) {
        ui_.cursor = Cursor::Hand;
        if (in.pressed[0]) selectChainOwner(kOwnMaster);
    }
}


void App::drawDragGhost() {
    Input& in = win_.input();
    if (drag_.kind == DragState::Kind::None) return;
    if (!in.down[0]) { drag_ = DragState{}; return; }

    const f32 dx = in.mx - drag_.startX, dy = in.my - drag_.startY;
    if (!drag_.armed && (dx * dx + dy * dy) > 25.f) drag_.armed = true;
    if (!drag_.armed) return;

    const f32 s = win_.dpiScale();
    ui_.cursor = Cursor::Grab;
    std::string label;
    Col c = pal::accent;
    if (drag_.kind == DragState::Kind::BrowserFile) {
        const size_t sl = drag_.path.find_last_of('/');
        label = sl == std::string::npos ? drag_.path : drag_.path.substr(sl + 1);
    } else {
        const ClipModel& m = ses_.tracks[drag_.srcTrack].slots[drag_.srcSlot];
        label = m.name;
        c = pal::clipColors[m.colorIdx % pal::clipColorCount];
    }
    const f32 w = fBody_.measure(label.c_str()) + 16 * s;
    Rect ghost{in.mx + 10 * s, in.my + 8 * s, w, 18 * s};
    rend_.roundRect(ghost, 2 * s, c.alpha(0.9f));
    rend_.textIn(fBody_, ghost, label.c_str(), pal::textOnClip, Align::Center);
}


} // namespace lat
