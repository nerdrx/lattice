// Clip detail: waveform, the detail-panel tab header, and the CLIP tab that
// hosts the PianoRoll. Moved verbatim from app.cpp.
//
#include "app.h"
#include "app_internal.h"
#include "arrange.h"
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
// what a clip may automate
//
// The roll must not see a DeviceModel or a ParamInfo (docs/AUTOMATION.md §6.5),
// so this flattens the selected track's mixer fields and device parameters into
// strings and floats once a frame. It is a few dozen string builds for the one
// track whose clip is on screen — the same order of cost as the panel's own
// labels, and it means the lane's chooser can never name a target the publisher
// would refuse.
//
// Deliberately NOT offered: mute, solo and arm. They are in the address grammar
// and the capture path spells them, but AutoTarget has no case for them yet, so
// a lane built from one would draw and never sound. Offering a control that
// cannot work is worse than not offering it.
// ---------------------------------------------------------------------------

void App::buildAutoTargets(int track, const ClipModel& clip, AutoTargets& out) const {
    out.entries.clear();
    out.inert = 0;
    out.inertWhy.clear();
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    const TrackModel& t = ses_.tracks[track];

    auto add = [&](const char* group, const char* label, std::string address,
                   const char* unit, f32 lo, f32 hi, f32 def) {
        AutoTargets::Entry e;
        e.group = group;
        e.label = label;
        e.address = std::move(address);
        e.unit = unit;
        e.lo = lo; e.hi = hi; e.def = def;
        for (const AutoLane& l : clip.envelopes)
            if (l.address == e.address) { e.automated = true; break; }
        out.entries.push_back(std::move(e));
    };

    // The mixer, in the order the strip has it. The fader's value is its 0..1
    // POSITION and not a gain -- §2.3, and why AutoXform exists.
    add("Track", "Volume", addr::trackField(t.uid, "vol"), "", 0.f, 1.f, 0.85f);
    add("Track", "Pan",    addr::trackField(t.uid, "pan"), "", -1.f, 1.f, 0.f);
    for (int i = 0; i < kMaxReturns; ++i) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "Send %s", kReturnLetter[i]);
        add("Track", lbl, addr::trackSend(t.uid, i), "", 0.f, 1.f, 0.f);
    }
    // Every parameter of every loaded device on this track. A device whose
    // plugin is missing contributes nothing -- it has no ParamInfo to describe a
    // range with -- but any lane already naming it keeps its points and says so
    // in the lane instead.
    for (const DeviceModel& d : t.devices) {
        if (!d.inst) continue;
        const int n = d.inst->paramCount();
        for (int p = 0; p < n; ++p) {
            const ParamInfo& info = d.inst->paramInfo(p);
            add(d.desc.name.c_str(), info.name.c_str(),
                addr::deviceParam(t.uid, d.uid, info.id), info.unit.c_str(),
                info.min, info.max, info.def);
        }
    }

    // Which of this clip's lanes the engine has given up on (§3.4). The mask is
    // by MODEL lane index, which is what the roll draws; inertAutos_ is keyed by
    // address, which is what survives a lane being reordered.
    for (size_t i = 0; i < clip.envelopes.size() && i < 32; ++i)
        if (autoLaneInert(clip.uid, clip.envelopes[i].address)) out.inert |= 1u << (u32)i;
    if (out.inert) out.inertWhy = "inert - this device has no realtime parameter path";
}

// ---------------------------------------------------------------------------
// clip detail
// ---------------------------------------------------------------------------

void App::drawWaveform(const Rect& r, const SampleBuffer& sb, const Col& c, f64 t0, f64 t1) {
    if (sb.peakBuckets <= 0) return;
    const f32 mid = r.cy();
    const f32 halfH = r.h * 0.5f - 1.f;
    const int cols = (int)r.w;
    for (int i = 0; i < cols; ++i) {
        const f64 u = t0 + (t1 - t0) * ((f64)i / std::max(1, cols - 1));
        const int b = clampv((int)(u * sb.peakBuckets), 0, sb.peakBuckets - 1);
        const f32 lo = sb.peaks[(size_t)b * 2 + 0];
        const f32 hi = sb.peaks[(size_t)b * 2 + 1];
        const f32 y0 = mid - hi * halfH;
        const f32 y1 = mid - lo * halfH;
        rend_.rect({r.x + i, y0, 1.f, std::max(1.f, y1 - y0)}, c);
    }
}

// The panel chrome: a Live-style tab strip along the top, then whichever view
// the tab selects. Ctrl+D still hides the panel as a whole.
void App::drawDetailPanel(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, r.w, 1 * s}, pal::divider);

    Rect head{r.x, r.y + 1 * s, r.w, 19 * s};
    rend_.rect(head, pal::panelAlt);
    rend_.rect({head.x, head.bottom() - 1 * s, head.w, 1 * s}, pal::divider);

    const f32 tabW = 62 * s, tabH = 15 * s;
    Rect clipTab{head.x + 6 * s, head.y + (head.h - tabH) * 0.5f, tabW, tabH};
    Rect devTab{clipTab.right() + 3 * s, clipTab.y, tabW, tabH};
    if (ui_.button(uiId(9, 0), clipTab, "CLIP", detailTab_ == DetailTab::Clip, pal::accent))
        detailTab_ = DetailTab::Clip;
    if (ui_.button(uiId(9, 1), devTab, "DEVICES", detailTab_ == DetailTab::Devices, pal::accent)) {
        detailTab_ = DetailTab::Devices;
        ensurePluginScan();
    }

    // Context label on the right of the tab strip, so the panel says what it is
    // looking at even when the content area is empty.
    {
        char buf[128];
        if (detailTab_ == DetailTab::Clip && view_ == MainView::Arrangement) {
            // In Arrangement view the CLIP tab is about the selected ITEM, so
            // the context label says where on the timeline it is rather than
            // which scene it is in -- it is not in one.
            const ArrangeClip* it = selectedArrItem();
            // Through Session::barOfBeat, not `start / sigNum`: dividing by the
            // bar-0 numerator is only right while a set never changes
            // signature, and from the first change on it prints a wrong bar
            // silently and plausibly. barOfBeat forwards to the same function
            // the engine's metronome and the ruler use, so this label cannot
            // disagree with the grid it is describing.
            if (it) snprintf(buf, sizeof buf, "%s  -  bar %.2f",
                             it->src.name.c_str(), ses_.barOfBeat(it->start) + 1.0);
            else    snprintf(buf, sizeof buf, "no item selected");
        } else if (detailTab_ == DetailTab::Clip) {
            const ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
            // ASCII only: the glyph atlas has no dashes or middots.
            snprintf(buf, sizeof buf, "%s  -  scene %d", m.valid() ? m.name.c_str() : "no clip",
                     selSlot_ + 1);
        } else {
            ChainOwner co = chainOwner(devOwner_);
            const size_t n = co.devices ? co.devices->size() : 0;
            snprintf(buf, sizeof buf, "%s  -  %zu device%s", ownerName(devOwner_).c_str(),
                     n, n == 1 ? "" : "s");
        }
        rend_.textIn(fSmall_, head, buf, pal::textFaint, Align::Right, 8 * s);
    }

    Rect content{r.x, head.bottom(), r.w, r.bottom() - head.bottom()};
    if (detailTab_ != DetailTab::Clip)          drawDeviceDetail(content);
    else if (view_ == MainView::Arrangement)    drawArrangeClipDetail(content);
    else                                        drawClipDetail(content);
}

// ---------------------------------------------------------------------------
// the CLIP tab in Arrangement view (docs/ARRANGEMENT.md §7.6)
//
// The same editor on a different clip, and that is the whole of it: the roll
// edits ArrangeClip::src IN PLACE and knows nothing about the arrangement.
//
// This is Rule 1 paying for itself. Because `src` is BY VALUE, the roll editing
// "the clip" edits precisely the one item the user selected, with no
// possibility of the edit leaking to another placement of the same material and
// no code in the roll that knows an arrangement exists.
//
// What the controls column shows is deliberately NOT drawClipDetail's. Launch
// quantum, probability and follow action are statements about how a clip is
// LAUNCHED, and an item on the timeline is not launched -- it is placed. So the
// column shows the placement instead: where the item starts, how long it is,
// which beat of its clip it begins on, and its two fades.
// ---------------------------------------------------------------------------

void App::drawArrangeClipDetail(const Rect& r) {
    const f32 s = win_.dpiScale();
    ArrangeClip* const it = selectedArrItem();
    if (!it || !it->src.valid()) {
        rend_.textIn(fBody_, r,
                     "No item selected  -  click a clip on the timeline, or drag one onto it",
                     pal::textFaint, Align::Center);
        return;
    }
    ClipModel& m = it->src;
    const int track = arrSelTrack_;

    const Col ccol = pal::clipColors[m.colorIdx % pal::clipColorCount];
    Rect head{r.x, r.y + 1 * s, r.w, 20 * s};
    rend_.rect({head.x, head.y, 4 * s, head.h}, ccol);
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, 260 * s, head.h}, m.name.c_str(),
                 pal::text, Align::Left, 0);

    const f32 panelW = 250 * s;
    Rect ctrl{r.x + 8 * s, head.bottom() + 6 * s, panelW, r.bottom() - head.bottom() - 12 * s};
    f32 y = ctrl.y;
    const f32 rowH = 20 * s, lblW = 62 * s;
    auto label = [&](const char* tx, const Rect& row) {
        rend_.textIn(fSmall_, {row.x, row.y, lblW, row.h}, tx, pal::textFaint, Align::Left, 0);
    };
    // Every one of these is a placement field, so every one of them goes through
    // the same commit: repair the lane, republish that track, one undo entry per
    // drag (the widget's own id is the gesture).
    bool placed = false;
    auto num = [&](int id, const char* lbl, f64* v, f64 lo, f64 hi, const char* fmt) {
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label(lbl, row);
        f64 tmp = *v;
        Rect dn{row.x + lblW, row.y, 90 * s, row.h};
        if (ui_.dragNumber(uiId(27, id), dn, &tmp, lo, hi, 0.02, fmt)) {
            undoPoint("clip placement");
            *v = tmp;
            placed = true;
        }
        y += rowH + 4 * s;
    };
    num(0, "START",  &it->start,  0.0, 1e6, "%.3f bt");
    num(1, "LENGTH", &it->length, kMinArrBeats, 1e6, "%.3f bt");
    num(2, "OFFSET", &it->offset, 0.0, 1e6, "%.3f bt");
    num(3, "FADE IN",  &it->fadeIn,  0.0, kMaxOverlapBeats, "%.3f bt");
    num(4, "FADE OUT", &it->fadeOut, 0.0, kMaxOverlapBeats, "%.3f bt");
    {   // Gain and loop, which mean the same thing here as anywhere.
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("GAIN", row);
        f64 db = gainToDb(m.gain);
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(27, 5), dn, &db, -70.0, 12.0, 0.1, "%.1f dB")) {
            undoPoint("clip gain");
            m.gain = dbToGain((f32)db);
            placed = true;
        }
        Rect lp{dn.right() + 6 * s, row.y, 52 * s, row.h};
        if (ui_.button(uiId(27, 6), lp, "LOOP", m.loop, pal::accent)) {
            undoPoint("clip loop");
            m.loop = !m.loop;
            placed = true;
        }
        y += rowH + 4 * s;
    }
    if (ui_.fSmall) {
        Rect row{ctrl.x, y, ctrl.w, rowH};
        char buf[128];
        snprintf(buf, sizeof buf, "%s  -  %.2f .. %.2f bt", ownerName(track).c_str(),
                 it->start, it->end());
        rend_.textIn(fSmall_, row, buf, pal::textFaint, Align::Left, 0);
    }

    Rect wave{ctrl.right() + 12 * s, head.bottom() + 6 * s,
              r.right() - ctrl.right() - 20 * s, r.bottom() - head.bottom() - 12 * s};

    if (!arrRoll_) arrRoll_ = std::make_unique<PianoRoll>();
    AutoTargets targets;
    buildAutoTargets(track, m, targets);

    // Where the playhead is INSIDE this item, so the roll's line means the same
    // thing it means for a session clip. Only while the transport is inside the
    // item at all -- an item that is not sounding has no phase to show.
    const f64 beat = es_.beat;
    const bool inside = es_.playing && beat >= it->start && beat < it->end();
    const f64 phase = inside ? (it->offset + (beat - it->start)) : 0.0;

    const ClipModel before = m;
    if (arrRoll_->draw(ui_, wave, m, targets, phase, inside)) {
        undoPointWith(arrRoll_->lastEdit(), m, before);
        placed = true;
    }
    if (placed) {
        // arrangeRepair after EVERY mutation, which is the rule the whole model
        // rests on: a start or a length typed into this panel can overlap a
        // neighbour exactly as a drag can.
        if (track >= 0 && track < (int)ses_.tracks.size()) {
            arrangeRepair(ses_.tracks[(size_t)track].arrange);
            publishArrangementFor(track);
        }
    }
    u8 pv[PianoRoll::kPreviewMax];
    const int np = arrRoll_->drainPreview(pv, PianoRoll::kPreviewMax);
    for (int i = 0; i < np; ++i) startPreview((int)pv[i], m.uid);
}

void App::drawClipDetail(const Rect& r) {
    const f32 s = win_.dpiScale();

    // Headless verification hook (see app.h): NXTAKT_DEBUG_AUTOLANE=<track>
    // selects that track's first clip, seeds one volume envelope on it and puts
    // the lane on that envelope, once per run. Nothing inside gamescope can
    // work a chooser or drag a breakpoint, so this is what lets a screenshot
    // check that the lane draws real points against the roll's own time axis.
    if (!autoDebugSeeded_) {
        if (const char* want = env("DEBUG_AUTOLANE")) {
            autoDebugSeeded_ = true;
            int t = 0, laneNo = 1;
            sscanf(want, "%d:%d", &t, &laneNo);       // "<track>[:<lane>]"
            if (t > 0 && t < (int)ses_.tracks.size()) { selTrack_ = t; selSlot_ = 0; }
            ClipModel& c = ses_.tracks[selTrack_].slots[selSlot_];
            if (c.valid()) {
                if (c.envelopes.empty()) {
                    AutoLane l;
                    l.address = addr::trackField(ses_.tracks[selTrack_].uid, "vol");
                    const f64 len = c.lengthBeats > 0.0 ? c.lengthBeats : 4.0;
                    l.points.push_back(AutoPoint{0.0,         0.30f, 0, {}});
                    l.points.push_back(AutoPoint{len * 0.25,  0.95f, 0, {}});
                    l.points.push_back(AutoPoint{len * 0.5,   0.55f, 0, {}});
                    l.points.push_back(AutoPoint{len * 0.875, 0.85f, 0, {}});
                    c.envelopes.push_back(std::move(l));
                    pushClip(selTrack_, selSlot_);
                    status_ = "NXTAKT_DEBUG_AUTOLANE: seeded a volume envelope";
                } else {
                    status_ = "NXTAKT_DEBUG_AUTOLANE: showing lane " + std::to_string(laneNo);
                }
                if (!roll_) roll_ = std::make_unique<PianoRoll>();
                roll_->showLane(clampv(laneNo, 0, (int)c.envelopes.size()));
            }
        }
    }

    ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
    if (!m.valid()) {
        rend_.textIn(fBody_, r, "No clip selected  —  drag a file from the browser onto a slot",
                     pal::textFaint, Align::Center);
        return;
    }
    // A pattern has no sample behind it, so warp, clip tempo and the loop
    // *range* have nothing to act on; everything else on this panel is about
    // launching, which a MIDI clip does exactly like an audio one.
    const bool midi = m.kind == ClipKind::Midi;

    const Col ccol = pal::clipColors[m.colorIdx % pal::clipColorCount];
    Rect head{r.x, r.y + 1 * s, r.w, 20 * s};
    rend_.rect({head.x, head.y, 4 * s, head.h}, ccol);
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, 260 * s, head.h}, m.name.c_str(), pal::text, Align::Left, 0);

    // --- controls column ---
    const f32 panelW = 250 * s;
    Rect ctrl{r.x + 8 * s, head.bottom() + 6 * s, panelW, r.bottom() - head.bottom() - 12 * s};
    f32 y = ctrl.y;
    const f32 rowH = 20 * s, lblW = 62 * s;

    auto label = [&](const char* t, const Rect& row) {
        rend_.textIn(fSmall_, {row.x, row.y, lblW, row.h}, t, pal::textFaint, Align::Left, 0);
    };

    {   // Warp mode (audio only) + loop, which both kinds have
        Rect row{ctrl.x, y, ctrl.w, rowH};
        Rect lp{row.x + lblW, row.y, 52 * s, row.h};
        if (!midi) {
            label("WARP", row);
            static const char* warpNames[] = {"Off", "Repitch", "Beats"};
            int wi = (int)m.warp;
            Rect sel{row.x + lblW, row.y, 84 * s, row.h};
            if (ui_.selector(uiId(8, 0), sel, &wi, warpNames, 3)) {
                undoPoint("warp mode");
                m.warp = (Warp)wi;
                send(Cmd::ClipWarp, selTrack_, selSlot_, (f64)wi);
            }
            lp = {sel.right() + 6 * s, row.y, 52 * s, row.h};
        } else {
            label("PLAY", row);
        }
        if (ui_.button(uiId(8, 1), lp, "LOOP", m.loop, pal::accent)) {
            undoPoint("clip loop");
            m.loop = !m.loop;
            send(Cmd::ClipLoop, selTrack_, selSlot_, m.loop ? 1.0 : 0.0);
        }
        y += rowH + 4 * s;
    }
    if (!midi) {   // Clip tempo
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("CLIP BPM", row);
        f64 bpm = m.clipBpm;
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 2), dn, &bpm, 20.0, 400.0, 0.1, "%.2f")) {
            undoPoint("clip tempo");
            m.clipBpm = bpm;
            pushClip(selTrack_, selSlot_);
        }
        // Halve / double, exactly like Live's :2 and *2 buttons.
        Rect h2{dn.right() + 6 * s, row.y, 26 * s, row.h};
        Rect d2{h2.right() + 3 * s, row.y, 26 * s, row.h};
        if (ui_.button(uiId(8, 3), h2, ":2")) {
            undoPoint("clip tempo");
            m.clipBpm *= 0.5;
            pushClip(selTrack_, selSlot_);
        }
        if (ui_.button(uiId(8, 4), d2, "*2")) {
            undoPoint("clip tempo");
            m.clipBpm *= 2.0;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Gain
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("GAIN", row);
        f64 db = gainToDb(m.gain);
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 5), dn, &db, -70.0, 12.0, 0.1, "%.1f dB")) {
            undoPoint("clip gain");
            m.gain = dbToGain((f32)db);
            send(Cmd::ClipGain, selTrack_, selSlot_, m.gain);
        }
        y += rowH + 4 * s;
    }
    {   // Launch quantum override
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("LAUNCH Q", row);
        static const char* qn[kQuantumCount + 1] = {"Global"};
        static bool qnInit = false;
        if (!qnInit) { for (int i = 0; i < kQuantumCount; ++i) qn[i + 1] = kQuantumNames[i]; qnInit = true; }
        int qi = m.quantumIdx + 1;
        Rect sel{row.x + lblW, row.y, 84 * s, row.h};
        if (ui_.selector(uiId(8, 6), sel, &qi, qn, kQuantumCount + 1)) {
            undoPoint("clip quantum");
            m.quantumIdx = qi - 1;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Generative launch: probability, follow action, follow length.
        // The engine rolls `prob` on every launch and fires the follow action
        // after `followBeats` of playback, so all three are pure clip state and
        // ride across in the same RtClip as everything else here.
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("LAUNCH", row);

        f64 pct = m.prob * 100.0;
        Rect pr{row.x + lblW, row.y, 48 * s, row.h};
        if (ui_.dragNumber(uiId(13, 0), pr, &pct, 0.0, 100.0, 0.4, "%.0f%%")) {
            undoPoint("launch probability");
            m.prob = clampv(pct * 0.01, 0.0, 1.0);
            pushClip(selTrack_, selSlot_);
        }

        int fa = (int)m.followAction;
        Rect fr{pr.right() + 6 * s, row.y, 58 * s, row.h};
        if (ui_.selector(uiId(13, 1), fr, &fa, kFollowNames, kFollowCount)) {
            undoPoint("follow action");
            m.followAction = (Follow)clampv(fa, 0, kFollowCount - 1);
            pushClip(selTrack_, selSlot_);
        }

        // 0 beats means "when the clip itself ends", which reads as Auto rather
        // than as a length. Whole beats only: a follow length between beats is
        // a tempo problem, not a musical choice.
        f64 fb = m.followBeats;
        Rect br{fr.right() + 6 * s, row.y, 52 * s, row.h};
        if (ui_.dragNumber(uiId(13, 2), br, &fb, 0.0, 128.0, 0.06, "%.0f bt",
                           Align::Center, "Auto", 1.0)) {
            undoPoint("follow length");
            m.followBeats = fb;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Read-out of what the engine will actually do
        Rect row{ctrl.x, y, ctrl.w, rowH};
        char buf[96];
        if (midi) {
            snprintf(buf, sizeof buf, "%.2f beats  ·  %zu note%s", m.lengthBeats,
                     m.notes.size(), m.notes.size() == 1 ? "" : "s");
        } else {
            const f64 rate = (m.warp == Warp::Off) ? 1.0 : m.clipBpm / ses_.tempo;
            snprintf(buf, sizeof buf, "%.2f beats  ·  rate %.3fx  ·  %d ch",
                     m.lengthBeats, rate, m.sample->channels);
        }
        rend_.textIn(fSmall_, row, buf, pal::textFaint, Align::Left, 0);
    }

    // --- the material: the note grid for a pattern, the waveform for a sample,
    //     and under either of them the automation lane -----------------------
    //
    // Both kinds go through the roll now, and the choice is worth stating: an
    // audio clip's envelopes need a TIME axis, and the only correct time axis
    // is the one its material is drawn against. Giving the audio clip its own
    // lane widget under the old fixed-width waveform would have meant two
    // beat<->pixel mappings that agree only while nothing is zoomed. So the
    // roll draws the waveform where the note grid goes (PianoRoll::draw,
    // `midiClip`), the lane keeps the axis it already shares with the ruler,
    // and there is exactly one mapping in the program. The note-specific
    // furniture (FOLD, the keyboard column, the loop-length drag) is suppressed
    // rather than faked.
    Rect wave{ctrl.right() + 12 * s, head.bottom() + 6 * s,
              r.right() - ctrl.right() - 20 * s, r.bottom() - head.bottom() - 12 * s};

    // Where the clip is, in its own beats, so the grid and the lane draw the
    // same playhead from the same number.
    const bool active = es_.activeSlot[selTrack_] == selSlot_;
    const f64  phase  = clampv(es_.clipPhase[selTrack_], 0.0, 1.0);

    if (!roll_) roll_ = std::make_unique<PianoRoll>();

    AutoTargets targets;
    buildAutoTargets(selTrack_, m, targets);

    // The roll edits m.notes and m.envelopes in place and says whether it
    // touched anything; republishing is ours, and pushClip is what retires the
    // arrays the engine is still reading from.
    //
    // The undo entry therefore needs the clip as it was *before* the call, which
    // is why the copy is taken unconditionally: whether an edit happens is not
    // knowable until draw() returns, and by then m already has it. A clip is a
    // note vector, an envelope vector, two strings and a shared pointer -- cheap
    // enough to copy once a frame, and it buys the one thing that matters here,
    // which is that a click that adds a note or a breakpoint can be undone. The
    // roll owns ui_.active for the length of a drag, so a note or a breakpoint
    // dragged across the editor leaves one entry and not one per frame.
    const ClipModel before = m;
    if (roll_->draw(ui_, wave, m, targets, active ? phase * m.lengthBeats : 0.0, active)) {
        undoPointWith(roll_->lastEdit(), m, before);
        pushClip(selTrack_, selSlot_);
    }
    // Auditioning is the caller's job: the roll only names the pitches that want
    // to be heard (from this draw, and from any keyboard edit earlier in the
    // frame — handleShortcuts runs first). See previews_ for why these reach the
    // right instrument.
    u8 pv[PianoRoll::kPreviewMax];
    const int np = roll_->drainPreview(pv, PianoRoll::kPreviewMax);
    for (int i = 0; i < np; ++i) startPreview((int)pv[i], m.uid);
}


} // namespace lat
