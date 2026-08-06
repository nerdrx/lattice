// Chrome: the control bar, the file browser (model + draw), the status bar
// and the arrangement placeholder. Independent leftovers that share a file
// only because they share no state. Moved verbatim from app.cpp.
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
// control bar
// ---------------------------------------------------------------------------

// Pulse for anything that is waiting for the user: 0..1, about two cycles a
// second. Spelled once per file rather than promoted to app_internal.h, which
// this wave does not own.
static f32 ctlPulse01() {
    return 0.5f + 0.5f * (f32)std::sin(nowSeconds() * 6.2831853 * 1.6);
}

void App::drawControlBar(const Rect& r) {
    const f32 s = win_.dpiScale();
    // Remote control's per-frame tick. It rides the control bar because the
    // control bar is drawn unconditionally, first, on every frame — see the
    // report for why the drain does not live in App::frame().
    drainControlInput();
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.bottom() - 1 * s, r.w, 1 * s}, pal::divider);

    const f32 pad = 8 * s, h = 20 * s;
    const f32 cy = r.y + (r.h - h) * 0.5f;
    f32 x = pad;

    // --- tempo ---
    Rect tapR{x, cy, 32 * s, h};
    if (ui_.button(uiId(1, 0), tapR, "TAP")) {
        static f64 lastTap = 0.0;
        const f64 now = nowSeconds();
        if (now - lastTap < 3.0) {
            undoPoint("tempo");
            setTempo(clampv(60.0 / (now - lastTap), 20.0, 999.0));
        }
        lastTap = now;
    }
    x += tapR.w + 4 * s;

    Rect tempoR{x, cy, 62 * s, h};
    f64 bpm = ses_.tempo;
    // The number is edited through a copy, so the session still holds the old
    // tempo here and a plain undoPoint is enough; the drag coalesces on the
    // widget's id.
    if (ui_.dragNumber(uiId(1, 1), tempoR, &bpm, 20.0, 999.0, 0.15, "%.2f")) {
        undoPoint("tempo");
        setTempo(bpm);
    }
    x += tempoR.w + 6 * s;

    Rect sigR{x, cy, 44 * s, h};
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%d / %d", ses_.sigNum, ses_.sigDen);
        rend_.roundRect(sigR, 2 * s, pal::panelAlt);
        rend_.textIn(fBody_, sigR, buf, pal::textDim, Align::Center);
    }
    x += sigR.w + 6 * s;

    Rect metR{x, cy, 36 * s, h};
    if (ui_.button(uiId(1, 2), metR, "MET", ses_.metronome, pal::accent)) {
        undoPoint("metronome");
        ses_.metronome = !ses_.metronome;
        send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    }
    x += metR.w + 12 * s;

    // --- global launch quantum ---
    rend_.textIn(fSmall_, {x, cy, 26 * s, h}, "Q", pal::textFaint, Align::Left, 0);
    Rect quantR{x + 16 * s, cy, 62 * s, h};
    // The selector writes into the session and only then reports the change,
    // so the entry needs the index handed back to it.
    const int wasQuantum = ses_.quantumIdx;
    if (ui_.selector(uiId(1, 3), quantR, &ses_.quantumIdx, kQuantumNames, kQuantumCount)) {
        undoPointWith("launch quantum", ses_.quantumIdx, wasQuantum);
        send(Cmd::SetQuantum, ses_.quantumIdx);
    }
    x = quantR.right() + 16 * s;

    // --- transport ---
    Rect playR{x, cy, 30 * s, h};
    const bool playing = engine_.playing.load();
    if (ui_.button(uiId(1, 4), playR, "", playing, pal::playGreen)) togglePlay();
    ui_.playTriangle(playR.insetXY(11 * s, 5 * s), playing ? pal::textOnClip : pal::text);
    x += playR.w + 3 * s;

    Rect stopR{x, cy, 30 * s, h};
    if (ui_.button(uiId(1, 5), stopR, "")) send(Cmd::SetPlaying, 0);
    ui_.stopSquare(stopR, pal::text);
    x += stopR.w + 3 * s;

    // Session record. This is an *intent*, not a transport action: while it is
    // lit, clicking an empty slot on an armed track starts a take in that slot;
    // while it is unlit, the same click only moves the selection. The circle
    // additionally lights while any track is actually capturing, so the bar
    // says what the engine is doing and not just what was asked for.
    Rect recR{x, cy, 30 * s, h};
    bool anyRec = false;
    for (size_t t = 0; t < ses_.tracks.size(); ++t)
        if (engine_.recState[t].load() != 0) { anyRec = true; break; }
    // A dark plate under a bright circle while capturing; the plain armed plate
    // otherwise, so the two states never read as the same light.
    const Col recPlate = anyRec ? pal::recRed.scale(0.4f) : pal::armRed;
    if (ui_.button(uiId(1, 6), recR, "", recIntent_ || anyRec, recPlate)) recIntent_ = !recIntent_;
    rend_.circle(recR.cx(), recR.cy(), 5 * s,
                 anyRec ? pal::recRed : (recIntent_ ? pal::textOnClip : pal::recRed.scale(0.55f)));
    x += recR.w + 3 * s;

    // Automation Arm — its own control, immediately right of the record circle
    // (docs/AUTOMATION.md §5.1, decision #10). Not implied by record-arm:
    // recording notes and recording knob moves are genuinely different intents,
    // and one control for both would surprise in whichever direction it guessed.
    // Drawn as the KBD chip is — accentHi on dark rather than a filled plate —
    // because it is a MODE the transport row reports, not a transport action,
    // and the row already reads "the red thing is record".
    {
        Rect autoR{x, cy, 34 * s, h};
        const u64 id = uiId(1, 10);
        const bool hot = ui_.setHot(id, autoR) && ui_.isHot(id);
        if (hot) ui_.cursor = Cursor::Hand;
        rend_.roundRect(autoR, 2 * s, autoArm_ ? pal::accent.alpha(0.18f)
                                               : (hot ? pal::slotHover : pal::appBg));
        rend_.textIn(fSmall_, autoR, "AUTO", autoArm_ ? pal::accentHi : pal::textFaint,
                     Align::Center);
        if (hot) ui_.tip = "Automation arm: record control moves into the playing clip";
        if (hot && win_.input().pressed[0]) toggleAutoArm();
        x = autoR.right() + 4 * s;
    }

    // Arrangement arm -- a THIRD independent chip, immediately right of AUTO and
    // before the position readout (docs/ARRANGEMENT.md §7.7, answer #12), in
    // AUTO's own style because it is likewise a MODE the transport row reports
    // and not a transport action.
    //
    // Rejected, and worth recording at the call site: folding it into REC.
    // "Record into the session grid" and "record onto the timeline" are
    // different destinations, and one button would have to pick between them
    // from view_ -- which means the same click does two different things
    // depending on which tab is open. That is the modality AUTOMATION.md §5.1
    // refused when it made the automation arm its own control.
    {
        Rect arrR{x, cy, 30 * s, h};
        const u64 id = uiId(1, 12);
        const bool hot = ui_.setHot(id, arrR) && ui_.isHot(id);
        if (hot) ui_.cursor = Cursor::Hand;
        rend_.roundRect(arrR, 2 * s, arrArm_ ? pal::accent.alpha(0.18f)
                                             : (hot ? pal::slotHover : pal::appBg));
        rend_.textIn(fSmall_, arrR, "ARR", arrArm_ ? pal::accentHi : pal::textFaint,
                     Align::Center);
        if (hot) ui_.tip = "Arrangement arm: record armed tracks onto the timeline";
        if (hot && win_.input().pressed[0]) {
            arrArm_ = !arrArm_;
            if (arrArm_) {
                status_ = "Arrangement arm on - recording lands on the timeline";
            } else if (takeOpen_) {
                // Disarming mid-pass is "stop recording", not "throw the take
                // away": what has been played has been played, and the journal
                // already holds it stamped by the engine. commitTake reports
                // what it did, so this sets no status of its own.
                commitTake();
            } else {
                status_ = "Arrangement arm off";
            }
        }
        x = arrR.right() + 12 * s;
    }

    // --- position readout ---
    {
        const f64 beat = engine_.beat.load();
        const int bar_ = (int)std::floor(beat / ses_.sigNum) + 1;
        const int bt   = (int)std::floor(std::fmod(beat, (f64)ses_.sigNum)) + 1;
        const int sx   = (int)std::floor(std::fmod(beat, 1.0) * 4.0) + 1;
        char buf[48];
        snprintf(buf, sizeof buf, "%d.%d.%d", bar_, bt, sx);
        Rect posR{x, cy, 92 * s, h};
        rend_.roundRect(posR, 2 * s, pal::appBg);
        rend_.textIn(fBig_, posR, buf, playing ? pal::playGreen : pal::text, Align::Center);
        x += posR.w + 8 * s;
    }

    // --- right side: CPU + view switch ---
    f32 rx = r.right() - pad;
    {
        Rect vs{rx - 150 * s, cy, 150 * s, h};
        const f32 halfW = vs.w * 0.5f;
        Rect a{vs.x, vs.y, halfW, vs.h}, b{vs.x + halfW, vs.y, halfW, vs.h};
        if (ui_.button(uiId(1, 7), a, "SESSION", view_ == MainView::Session, pal::accent))
            view_ = MainView::Session;
        if (ui_.button(uiId(1, 8), b, "ARRANGE", view_ == MainView::Arrangement, pal::accent))
            view_ = MainView::Arrangement;
        rx = vs.x - 10 * s;
    }
    {
        const f32 cpu = engine_.cpu.load();
        char buf[32];
        snprintf(buf, sizeof buf, "%.0f%%", cpu);
        Rect cr{rx - 44 * s, cy, 44 * s, h};
        rend_.roundRect(cr, 2 * s, pal::appBg);
        const Col c = cpu > 85.f ? pal::meterRed : cpu > 60.f ? pal::meterAmber : pal::textDim;
        rend_.textIn(fSmall_, cr, buf, c, Align::Center);
        rx = cr.x - 8 * s;
    }
    {
        Rect br{rx - 60 * s, cy, 60 * s, h};
        rend_.textIn(fSmall_, br, audio_ ? audio_->name() : "no audio",
                     audio_ ? pal::textFaint : pal::recRed, Align::Right, 0);
        rx = br.x - 8 * s;
    }
    // Computer MIDI keyboard. It belongs with the audio/MIDI readouts because
    // it is an input status: while it is lit the letter keys are notes and not
    // shortcuts, and that must be visible without opening anything. The label
    // carries the octave so PgUp / PgDn have somewhere to show their work, and
    // velocity sits next to it as a number: the FL layout spends C and V on
    // notes, so there are no keys left to nudge it with.
    {
        f64 vel = (f64)kbd_.velocity();
        Rect vr{rx - 34 * s, cy, 34 * s, h};
        if (ui_.dragNumber(uiId(16, 0), vr, &vel, 1.0, 127.0, 0.35, "%.0f")) {
            kbd_.setVelocity((int)std::lround(vel));
            char buf[64];
            snprintf(buf, sizeof buf, "Keyboard velocity %d", kbd_.velocity());
            status_ = buf;
        }
        rx = vr.x - 3 * s;

        char buf[24];
        snprintf(buf, sizeof buf, "KBD C%d", kbd_.octave());
        Rect kr{rx - 58 * s, cy, 58 * s, h};
        const u64 id = uiId(1, 9);
        const bool hot = ui_.setHot(id, kr) && ui_.isHot(id);
        if (hot) ui_.cursor = Cursor::Hand;
        rend_.roundRect(kr, 2 * s, kbdMidi_ ? pal::accent.alpha(0.18f)
                                            : (hot ? pal::slotHover : pal::appBg));
        rend_.textIn(fSmall_, kr, buf, kbdMidi_ ? pal::accent : pal::textFaint, Align::Center);
        if (hot && win_.input().pressed[0]) toggleKbdMidi();
        rx = kr.x - 8 * s;
    }

    // Remote control. It belongs with KBD and the MIDI client id for the same
    // reason those do: it says what, other than this window, can currently move
    // something in this set. A chip and not a panel, because the answer is
    // three numbers and there is no fourth thing to say about it.
    {
        const size_t nb = midiMap_.size();
        const bool learning = midiMap_.learning();
        char buf[24];
        if (learning) snprintf(buf, sizeof buf, "LEARN");
        else          snprintf(buf, sizeof buf, "MAP %zu", nb);

        Rect mr{rx - 56 * s, cy, 56 * s, h};
        const u64 id = uiId(1, 11);
        const bool hot = ui_.setHot(id, mr) && ui_.isHot(id);
        if (hot) ui_.cursor = Cursor::Hand;

        // Accent purple, pulsing, while a control is waiting to be learned —
        // the same light the armed knob in the device panel is wearing, so the
        // two read as one state and not as two coincidences.
        const f64 sinceHit = nowSeconds() - ctlFlashAt_;
        Col bg = pal::appBg;
        if (learning)            bg = pal::accent.alpha(0.10f + 0.32f * ctlPulse01());
        else if (sinceHit < 0.1) bg = pal::accent.alpha(0.18f);   // blink per applied hit
        else if (hot)            bg = pal::slotHover;
        rend_.roundRect(mr, 2 * s, bg);
        rend_.textIn(fSmall_, mr, buf,
                     learning ? pal::accentHi
                              : (nb || osc_.running() ? pal::textDim : pal::textFaint),
                     Align::Center);
        // A dot, not a word: "is anything listening on the network" is a yes/no
        // and the bar has no room for a sentence. Amber when the socket is not
        // on loopback, which is the one fact about it worth a colour.
        if (osc_.running())
            rend_.circle(mr.right() - 5 * s, mr.y + 5 * s, 2.2f * s,
                         osc_.wide() ? pal::meterAmber : pal::playGreen);

        if (hot) {
            char tip[320];
            char oscPart[96] = " · OSC off";
            if (osc_.running())
                snprintf(oscPart, sizeof oscPart, " · OSC %s:%d%s", osc_.addr().c_str(),
                         osc_.port(), osc_.wide() ? " (OPEN TO THE NETWORK)" : "");
            // The one failure mode a user could never guess at: the mapping
            // table is fine, the controller is connected, and nothing moves
            // because the reader thread's tap is not wired up.
            const bool untapped = midi_.received() > 0 && ctl::midiTapCount() == 0;
            if (learning)
                snprintf(tip, sizeof tip, "MIDI learn: move a control to map %s  (click to cancel)",
                         midiMap_.learnAddress().c_str());
            else if (untapped)
                snprintf(tip, sizeof tip,
                         "%zu MIDI bindings%s — but MIDI input is not tapped, so nothing is routed",
                         nb, oscPart);
            else
                snprintf(tip, sizeof tip, "%zu MIDI bindings · %llu applied, %llu inert%s",
                         nb, (unsigned long long)ctlApplied_, (unsigned long long)ctlInert_,
                         oscPart);
            ui_.tip = tip;
        }
        if (hot && win_.input().pressed[0] && learning) {
            midiMap_.cancelLearn();
            status_ = "MIDI learn cancelled";
        }
        rx = mr.x - 8 * s;
    }
}


// ---------------------------------------------------------------------------
// browser
// ---------------------------------------------------------------------------

void App::drawBrowser(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::panel);
    rend_.rect({r.right() - 1 * s, r.y, 1 * s, r.h}, pal::divider);

    const f32 rowH = 19 * s;
    Rect head{r.x, r.y, r.w, 22 * s};
    rend_.rect(head, pal::panelAlt);
    rend_.textIn(fBold_, head, "BROWSER", pal::textDim, Align::Left, 8 * s);

    // Places
    f32 y = head.bottom();
    for (size_t i = 0; i < browserPlaces_.size(); ++i) {
        Rect row{r.x, y, r.w, rowH};
        const std::string& p = browserPlaces_[i];
        const bool sel = p == browserDir_;
        const bool hot = ui_.setHot(uiId(2, 100 + (int)i), row) && ui_.isHot(uiId(2, 100 + (int)i));
        if (sel)      rend_.rect(row, pal::gridBg);
        else if (hot) rend_.rect(row, pal::slotHover);
        if (hot) ui_.cursor = Cursor::Hand;
        const size_t slash = p.find_last_of('/');
        rend_.textIn(fBody_, row, (slash == std::string::npos ? p : p.substr(slash + 1)).c_str(),
                     sel ? pal::accent : pal::text, Align::Left, 14 * s);
        if (hot && in.pressed[0]) browseTo(p);
        y += rowH;
    }

    rend_.rect({r.x + 6 * s, y + 3 * s, r.w - 12 * s, 1 * s}, pal::divider);
    y += 8 * s;

    // Current directory label
    Rect dirRow{r.x, y, r.w, rowH};
    rend_.textIn(fSmall_, dirRow, browserDir_.c_str(), pal::textFaint, Align::Left, 8 * s);
    y += rowH;

    // File list
    Rect list{r.x, y, r.w, r.bottom() - y};
    rend_.pushClip(list);
    if (ui_.setHot(uiId(2, 1), list) && in.wheel != 0.f) {
        browserScroll_ -= in.wheel * rowH * 3.f;
        const f32 maxScroll = std::max(0.f, browserItems_.size() * rowH - list.h);
        browserScroll_ = clampv(browserScroll_, 0.f, maxScroll);
    }

    f32 iy = list.y - browserScroll_;
    for (size_t i = 0; i < browserItems_.size(); ++i) {
        Rect row{list.x, iy, list.w, rowH};
        iy += rowH;
        if (row.bottom() < list.y || row.y > list.bottom()) continue;
        const BrowserEntry& e = browserItems_[i];
        const u64 id = uiId(2, 200 + (int)i);
        const bool hot = ui_.setHot(id, row) && ui_.isHot(id);
        if ((int)i == browserSel_) rend_.rect(row, pal::gridBg);
        else if (hot)              rend_.rect(row, pal::slotHover);
        if (hot) ui_.cursor = Cursor::Hand;

        // Folder/file glyph
        const Col ic = e.isDir ? pal::textDim : pal::accent.mix(pal::text, 0.4f);
        if (e.isDir) rend_.roundRect({row.x + 8 * s, row.cy() - 4 * s, 9 * s, 8 * s}, 1.5f * s, ic);
        else         rend_.circle(row.x + 12 * s, row.cy(), 3 * s, ic);

        rend_.textIn(fBody_, {row.x + 22 * s, row.y, row.w - 26 * s, row.h}, e.name.c_str(),
                     e.isDir ? pal::text : pal::textDim, Align::Left, 0);

        if (hot && in.pressed[0]) {
            browserSel_ = (int)i;
            if (e.isDir) {
                // Resolve ".." rather than letting the path grow unbounded.
                if (e.name == "..") {
                    const size_t sl = browserDir_.find_last_of('/');
                    browseTo(sl == 0 ? "/" : (sl == std::string::npos ? browserDir_ : browserDir_.substr(0, sl)));
                } else {
                    browseTo(e.path);
                }
                break;
            }
            drag_.kind = DragState::Kind::BrowserFile;
            drag_.path = e.path;
            drag_.startX = in.mx; drag_.startY = in.my;
            drag_.armed = false;
        }
        if (hot && in.dblClick && !e.isDir) loadClipInto(selTrack_, selSlot_, e.path);
    }
    rend_.popClip();
}


// ---------------------------------------------------------------------------
// browser
// ---------------------------------------------------------------------------

void App::browseTo(const std::string& dir) {
    browserDir_ = dir;
    refreshBrowser();
    browserScroll_ = 0.f;
}

void App::refreshBrowser() {
    browserItems_.clear();
    DIR* d = opendir(browserDir_.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." ) continue;
        if (n != ".." && n[0] == '.') continue;          // skip dotfiles
        const std::string full = browserDir_ + "/" + n;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        BrowserEntry be;
        be.name = n;
        be.path = full;
        be.isDir = S_ISDIR(st.st_mode);
        be.isAudio = !be.isDir && isAudioFile(n);
        if (!be.isDir && !be.isAudio) continue;          // only show what we can use
        browserItems_.push_back(be);
    }
    closedir(d);
    std::sort(browserItems_.begin(), browserItems_.end(), [](const BrowserEntry& a, const BrowserEntry& b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.isDir != b.isDir) return a.isDir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}


// ---------------------------------------------------------------------------
// arrangement placeholder + chrome
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// the arrangement view  (docs/ARRANGEMENT.md §7)
//
// The placeholder is gone. What is here is the division §7.1 specifies and the
// piano roll already lives by: the VIEW draws, hit-tests and edits, and the
// CALLER -- this -- owns everything else. Building the context, arrangeRepair,
// publishArrangementFor, the undo point, and the transport commands the ruler
// generates are all on this side of the seam, which is why arrange.cpp knows
// nothing about Engine, Command, RtClip or a PluginInstance.
// ---------------------------------------------------------------------------

void App::buildArrangeContext(ArrangeContext& ctx, std::vector<AutoTargets>* targets) {
    ctx.lanes.clear();
    ctx.lanes.reserve(ses_.tracks.size());
    if (targets) targets->assign(ses_.tracks.size(), AutoTargets{});
    const u32 ovr = engine_.arrOverride.load();
    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        TrackModel& t = ses_.tracks[i];
        ArrangeContext::Lane L;
        L.name = t.name;
        L.colorIdx = t.colorIdx;
        L.items = &t.arrange;
        L.autos = &t.arrangeAutos;
        L.height = &t.arrHeight;
        L.armed = t.arm;
        // ENGINE-OWNED, and set at the quantized launch the engine computed
        // rather than when the click happened (engine.h): the flag says what is
        // actually sounding, which is the only thing worth desaturating a lane
        // for.
        L.overridden = i < kMaxTracks && (ovr & (1u << (u32)i)) != 0u;
        L.expanded = i < kMaxTracks ? &arrExpanded_[i] : nullptr;
        if (targets) {
            // What this track's arrangement lanes may name. The SAME builder the
            // clip envelopes use, handed an empty clip: buildAutoTargets's only
            // use of the clip is to mark which targets are already automated, and
            // for a track lane that question is answered against arrangeAutos
            // instead -- which is done right after, in place.
            AutoTargets& at = (*targets)[i];
            buildAutoTargets((int)i, ClipModel{}, at);
            for (AutoTargets::Entry& e : at.entries)
                for (const AutoLane& al : t.arrangeAutos)
                    if (al.address == e.address) { e.automated = true; break; }
            L.targets = &at;
        }
        ctx.lanes.push_back(std::move(L));
    }
    ctx.nextUid   = &ses_.nextUid;
    ctx.loopStart = &ses_.loopStart;
    ctx.loopEnd   = &ses_.loopEnd;
    ctx.loopOn    = &ses_.loopOn;
    ctx.playhead  = engine_.beat.load();
    ctx.playing   = engine_.playing.load();
    ctx.sigNum    = ses_.sigNum;
    ctx.selTrack  = arrSelTrack_;
    ctx.selItem   = arrSelItem_;
}

ArrangeClip* App::selectedArrItem() {
    if (arrSelTrack_ < 0 || arrSelTrack_ >= (int)ses_.tracks.size() || !arrSelItem_)
        return nullptr;
    for (ArrangeClip& c : ses_.tracks[(size_t)arrSelTrack_].arrange)
        if (c.uid == arrSelItem_) return &c;
    return nullptr;
}

void App::arrangeCommit(ArrangeContext& ctx, u32 changed) {
    if (!arrView_) return;
    arrangeCommitAutos(ctx, changed);

    // THE UNDO HANDSHAKE, first and before anything else can move the model: the
    // view names the edit on the frame its drag arms itself and mutates from the
    // NEXT frame on, so this is taken against the set as it still stands. One
    // entry per gesture, coalesced on the view's own gesture id -- the same
    // machinery a fader drag uses.
    if (const char* what = arrView_->takePendingEdit())
        undoPoint(what, arrView_->gesture());

    // The transport commands the ruler generated. Neither is an edit: a locate
    // is where the timeline is, not what is on it, and Back to Arrangement
    // clears an engine-owned flag that undo deliberately does not carry.
    if (ctx.locateBeat >= 0.0) send(Cmd::Locate, 0, 0, ctx.locateBeat);
    if (ctx.backToArrTrack >= 0) {
        send(Cmd::BackToArrangement, ctx.backToArrTrack);
        status_ = "Back to Arrangement: " + ses_.tracks[(size_t)ctx.backToArrTrack].name;
    }
    if (changed & ArrangeView::Loop) publishTransportCell();

    // The one-shot verbs the mouse asked for. The undo point goes FIRST, because
    // the verb is what moves the model and an entry that already contains the
    // edit undoes nothing.
    if (ctx.wantDelete) {
        undoPoint("delete clip");
        changed |= arrView_->deleteSelected(ctx);
    }
    if (ctx.wantSplit) {
        undoPoint("split clip");
        changed |= arrView_->splitSelected(ctx);
    }

    // A drop from the browser or from the session grid. The VIEW reported where;
    // what to put there is this side's question, because the view knows nothing
    // about samples, slots or clip loading.
    if (ctx.dropped && ctx.dropTrack >= 0 && ctx.dropTrack < (int)ses_.tracks.size()) {
        ClipModel src;
        bool have = false;
        if (drag_.kind == DragState::Kind::Clip &&
            drag_.srcTrack >= 0 && drag_.srcTrack < (int)ses_.tracks.size() &&
            drag_.srcSlot >= 0 && drag_.srcSlot < kMaxScenes) {
            const ClipModel& m = ses_.tracks[(size_t)drag_.srcTrack].slots[drag_.srcSlot];
            if (m.valid()) { src = m; have = true; }
        } else if (drag_.kind == DragState::Kind::BrowserFile) {
            have = makeClipFromFile(drag_.path, ses_.tracks[(size_t)ctx.dropTrack].colorIdx, src);
            if (!have) status_ = "Could not load " + drag_.path;
        }
        if (have) {
            // After the decode, so a file that could not be read leaves no
            // history behind, and before the lane is touched.
            undoPoint("drop clip");
            ArrangeClip it;
            it.start = ctx.dropBeat;
            it.length = src.lengthBeats > kMinArrBeats ? src.lengthBeats : 4.0;
            it.sourceUid = src.uid;          // PROVENANCE ONLY; it dangles soft
            it.src = std::move(src);
            std::vector<ArrangeClip>& lane = ses_.tracks[(size_t)ctx.dropTrack].arrange;
            if ((int)lane.size() < kMaxArrItems) {
                lane.push_back(std::move(it));
                arrangeRepair(lane);
                ctx.dirty.push_back(ctx.dropTrack);
                changed |= ArrangeView::Items;
                status_ = "Dropped onto " + ses_.tracks[(size_t)ctx.dropTrack].name;
            }
        }
        drag_ = DragState{};
    }

    if (ctx.dirty.empty()) {
        arrSelTrack_ = ctx.selTrack;
        arrSelItem_  = ctx.selItem;
        return;
    }

    // Identity, as a belt to the view's own stamping: the view mints a uid for
    // everything it creates (ArrangeContext::nextUid), and this catches an item
    // that arrived from a file written before uids existed. It is also what
    // keeps ClipModel::src.uid equal to the item's, which is the identity the
    // detail panel's roll keys its zoom and selection on.
    assignUids();

    // Exactly the tracks the view says it touched, and each of them once. A lane
    // is up to 1.6 MB (§7.6), so "republish everything on every edit" would make
    // a nudge cost the whole set.
    std::sort(ctx.dirty.begin(), ctx.dirty.end());
    ctx.dirty.erase(std::unique(ctx.dirty.begin(), ctx.dirty.end()), ctx.dirty.end());
    for (int t : ctx.dirty)
        if (t >= 0 && t < (int)ses_.tracks.size()) publishArrangementFor(t);

    arrSelTrack_ = ctx.selTrack;
    arrSelItem_  = ctx.selItem;
}

// ---------------------------------------------------------------------------
// the one-shot verbs, and the keyboard's wrapper around them
// ---------------------------------------------------------------------------

u32 App::arrangeVerb(ArrangeContext& ctx, int verb, const char* what) {
    if (!arrView_) return 0;
    const int t = arrView_->selectedTrack();
    if (t < 0 || t >= (int)ses_.tracks.size()) return 0;
    // The entry has to be taken with the lane as it WAS, and only if the edit
    // happens at all -- which is not knowable until the call returns, because a
    // split on an item's own edge and a duplicate at kMaxArrItems are both
    // legitimate no-ops. Copying a lane is a vector of items, which is why this
    // is a keypress path and not a per-frame one; see undoPointWith.
    std::vector<ArrangeClip> before = ses_.tracks[(size_t)t].arrange;
    const u32 ch = verb == 0   ? arrView_->deleteSelected(ctx)
                   : verb == 1 ? arrView_->splitSelected(ctx)
                               : arrView_->duplicateSelected(ctx);
    if (ch & ArrangeView::Items)
        undoPointWith(what, ses_.tracks[(size_t)t].arrange, before);
    return ch;
}

bool App::arrangeKey(int verb, const char* what) {
    if (!arrView_ || !arrView_->hasSelection()) return false;
    ArrangeContext ctx;
    buildArrangeContext(ctx, nullptr);          // the verbs need lanes, not targets
    const u32 ch = arrangeVerb(ctx, verb, what);
    if (!ch) return false;
    arrangeCommit(ctx, ch);
    return true;
}

// ---------------------------------------------------------------------------
// the two headless hooks (§7.7)
//
// Nothing inside gamescope can drag a clip, and dragging a clip is precisely
// the part a screenshot cannot check. Both are once per run, both are in the
// NXTAKT_DEBUG_* shape the undo, ADDFX, AUTOLANE and MIDIMAP hooks already use,
// and both drive the REAL paths -- the real verbs, the real undo entry, the real
// publisher -- rather than a test double.
// ---------------------------------------------------------------------------

void App::debugSeedArrangement() {
    if (arrDebugSeeded_) return;
    const char* want = env("DEBUG_ARRANGE");
    if (!want) return;
    arrDebugSeeded_ = true;

    // The view switch comes FIRST and unconditionally: this hook is the only
    // way into Arrangement view without a mouse, and a set with nothing to seed
    // is exactly the case a screenshot of the empty timeline is for.
    view_ = MainView::Arrangement;
    showDetail_ = true;
    detailTab_ = DetailTab::Clip;

    int t = 0;
    std::sscanf(want, "%d", &t);
    if (t < 0 || t >= (int)ses_.tracks.size()) {
        LOGW("NXTAKT_DEBUG_ARRANGE: no track %d", t);
        return;
    }
    TrackModel& tr = ses_.tracks[(size_t)t];
    // Slot 0, or the first slot that holds anything: a hook that seeds nothing
    // because the demo set happens to leave scene A empty is a hook that checks
    // nothing.
    int slot = -1;
    for (int i = 0; i < kMaxScenes; ++i)
        if (tr.slots[i].valid()) { slot = i; break; }
    if (slot < 0) {
        LOGW("NXTAKT_DEBUG_ARRANGE: track %d has no clip to seed from", t);
        return;
    }
    const ClipModel& src = tr.slots[slot];
    const f64 len = src.lengthBeats > 1.0 ? src.lengthBeats : 4.0;

    // The scripted figure §7.7 asks for: FOUR items, one of them a split, one
    // CROSSFADE PAIR at exactly kMaxOverlapBeats, one fade-in, and one item at a
    // NON-ZERO offset. Every case the drawing and the invariant have, in one
    // frame a screenshot can look at.
    auto mk = [&](f64 start, f64 length, f64 offset, f64 fin, f64 fout) {
        ArrangeClip c;
        c.uid = ses_.newUid();
        c.start = start;
        c.length = length;
        c.offset = offset;
        c.fadeIn = fin;
        c.fadeOut = fout;
        c.sourceUid = src.uid;
        c.src = src;
        c.src.uid = c.uid;
        return c;
    };
    tr.arrange.clear();
    // 1 + 2: the split. One item cut in two at len/2, butt-jointed, which is R3's
    // own case: no fades on the inner edges, and `offset` carried across the cut.
    tr.arrange.push_back(mk(0.0,        len * 0.5, 0.0,       len * 0.25, 0.0));
    tr.arrange.push_back(mk(len * 0.5,  len * 0.5, len * 0.5, 0.0,        kMaxOverlapBeats));
    // 3: the other half of the crossfade pair, overlapping by exactly
    // kMaxOverlapBeats with a matching fadeIn -- the one overlap the invariant
    // admits, and the reason it is admitted at all.
    tr.arrange.push_back(mk(len - kMaxOverlapBeats, len, 0.0, kMaxOverlapBeats, 0.0));
    // 4: a non-zero offset, well clear of the pair.
    tr.arrange.push_back(mk(len * 2.5, len * 0.75, len * 0.25, 0.0, 0.0));

    const bool repaired = arrangeRepair(tr.arrange);

    // One track automation lane, so the expanded row has something in it.
    if (tr.arrangeAutos.empty()) {
        AutoLane al;
        al.address = addr::trackField(tr.uid, "vol");
        al.points.push_back(AutoPoint{0.0,            0.30f, 0, {}});
        al.points.push_back(AutoPoint{len * 0.5,      0.95f, 0, {}});
        al.points.push_back(AutoPoint{len * 1.5,      0.55f, 0, {}});
        al.points.push_back(AutoPoint{len * 3.0,      0.85f, 0, {}});
        tr.arrangeAutos.push_back(std::move(al));
    }

    if (t < kMaxTracks) arrExpanded_[t] = true;
    selectTrack(t);
    // Item 1 (the second), so the panel draws a clip at a NON-ZERO offset and the
    // split's inner edge is what the selection outline is around.
    if (tr.arrange.size() > 1) {
        arrSelTrack_ = t;
        arrSelItem_  = tr.arrange[1].uid;
        if (arrView_) arrView_->selectItem(t, arrSelItem_);
    }
    publishArrangementFor(t);

    // What the LOG line checks, which a screenshot cannot: that arrangeRepair
    // left the invariant intact. Re-derived here from the lane rather than
    // trusted, which is the whole point of stating it.
    bool ok = true;
    const std::vector<ArrangeClip>& v = tr.arrange;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].length < kMinArrBeats) ok = false;
        if (v[i].fadeIn < 0.0 || v[i].fadeOut < 0.0) ok = false;
        if (v[i].fadeIn + v[i].fadeOut > v[i].length + kArrOverlapEps) ok = false;
        if (i == 0) continue;
        if (v[i].start < v[i - 1].start) ok = false;
        const f64 ov = v[i - 1].end() - v[i].start;
        if (ov > kArrOverlapEps &&
            !(ov <= kMaxOverlapBeats && v[i - 1].fadeOut >= ov && v[i].fadeIn >= ov))
            ok = false;
        if (i >= 2 && v[i].start < v[i - 2].end() - kArrOverlapEps) ok = false;
    }
    LOGI("NXTAKT_DEBUG_ARRANGE: track %d '%s' seeded %zu items from slot %d, "
         "repair %s, invariant %s",
         t, tr.name.c_str(), v.size(), slot, repaired ? "changed" : "clean",
         ok ? "HOLDS" : "VIOLATED");
    for (size_t i = 0; i < v.size(); ++i)
        LOGI("  item %zu uid %llu  start %.3f len %.3f off %.3f fin %.3f fout %.3f  %s",
             i, (unsigned long long)v[i].uid, v[i].start, v[i].length, v[i].offset,
             v[i].fadeIn, v[i].fadeOut,
             v[i].src.kind == ClipKind::Midi ? "midi" : "audio");
    status_ = std::string("NXTAKT_DEBUG_ARRANGE: seeded ") + std::to_string(v.size()) +
              " items on " + tr.name + (ok ? " - invariant holds" : " - INVARIANT VIOLATED");
}

// NXTAKT_DEBUG_ARREDIT=<verb>, one of split / dup / delete / move.
//
// Drives ONE arrangement edit through exactly the path a mouse would: the same
// verb, the same undo entry, the same republish. What it prints is the two
// things a screenshot cannot see -- that the gesture produced EXACTLY ONE undo
// entry, and that the lane it touched came back through arrangeRepair intact.
void App::debugArrangeEdit() {
    if (arrDebugEdited_) return;
    const char* want = env("DEBUG_ARREDIT");
    if (!want) return;
    arrDebugEdited_ = true;
    if (!arrView_ || !arrView_->hasSelection()) {
        LOGW("NXTAKT_DEBUG_ARREDIT: nothing selected (seed with NXTAKT_DEBUG_ARRANGE first)");
        return;
    }
    const int t = arrView_->selectedTrack();
    const size_t undoBefore = undo_.size();
    const size_t nBefore = ses_.tracks[(size_t)t].arrange.size();

    int verb = -1;
    if      (icontains(want, "split"))  verb = 1;
    else if (icontains(want, "dup"))    verb = 2;
    else if (icontains(want, "del"))    verb = 0;
    if (verb < 0) {
        LOGW("NXTAKT_DEBUG_ARREDIT: unknown verb \"%s\" (split | dup | delete)", want);
        return;
    }
    // "<verb>[:<item index>]". The seed leaves the selection on item 1, which is
    // inside the crossfade pair -- so a split there has its head reclaimed by
    // arrangeRepair, which is correct and is exactly the wrong thing to
    // demonstrate a split with. The index says which item to act on instead.
    if (const char* colon = std::strchr(want, ':')) {
        const int want_i = std::atoi(colon + 1);
        const std::vector<ArrangeClip>& v0 = ses_.tracks[(size_t)t].arrange;
        if (want_i >= 0 && want_i < (int)v0.size()) {
            arrSelTrack_ = t;
            arrSelItem_  = v0[(size_t)want_i].uid;
            arrView_->selectItem(t, arrSelItem_);
        }
    }
    // The cursor a split lands on. Nothing has moved a mouse, so it is put in
    // the middle of the selected item -- which is where a hand would aim.
    if (verb == 1) {
        for (const ArrangeClip& c : ses_.tracks[(size_t)t].arrange)
            if (c.uid == arrView_->selectedItem())
                arrView_->setCursorBeat(c.start + c.length * 0.5);
    }
    const bool did = arrangeKey(verb, verb == 0   ? "delete clip"
                                      : verb == 1 ? "split clip"
                                                  : "duplicate clip");
    const std::vector<ArrangeClip>& v = ses_.tracks[(size_t)t].arrange;
    LOGI("NXTAKT_DEBUG_ARREDIT: %s on track %d -> %s, items %zu -> %zu, "
         "undo entries %zu -> %zu (%s)",
         want, t, did ? "applied" : "refused", nBefore, v.size(),
         undoBefore, undo_.size(),
         (undo_.size() == undoBefore + (did ? 1u : 0u)) ? "EXACTLY ONE" : "WRONG COUNT");
    status_ = std::string("NXTAKT_DEBUG_ARREDIT: ") + want +
              (did ? " applied" : " refused");
}

// NXTAKT_DEBUG_ARRTAKE=<track>: the whole of §5 without a mouse.
//
// Scripts a session performance -- launch the track's first clip at beat 0,
// switch to its second at beat 4, stop at beat 8 -- drains the JOURNAL the engine
// wrote while performing it, and commits the take through the real accumulator,
// the real buildTake and the real App::commitTake. What it prints is what a
// screenshot cannot see: the beats the engine stamped, that the sequence numbers
// were contiguous, the items the commit produced, and that the whole thing cost
// EXACTLY ONE undo entry.
//
// It drives a PRIVATE, OFFLINE Engine rather than the app's own, and the reason
// is not convenience. A performance takes eight beats of wall clock; init() would
// have to block for four seconds and would depend on an audio device existing at
// all, which inside gamescope it may not. An engine driven synchronously here
// answers the same question in milliseconds and answers it deterministically --
// and it is the SAME Engine class, the same scheduler, the same journal ring, so
// nothing about the path under test is a double. Only the transport is local.
void App::debugArrangeTake() {
    if (arrDebugTook_) return;
    const char* want = env("DEBUG_ARRTAKE");
    if (!want) return;
    arrDebugTook_ = true;

    int t = 0;
    std::sscanf(want, "%d", &t);
    if (t < 0 || t >= (int)ses_.tracks.size()) {
        LOGW("NXTAKT_DEBUG_ARRTAKE: no track %d", t);
        return;
    }
    TrackModel& tr = ses_.tracks[(size_t)t];
    // Two slots, so the performance has a switch in it and not merely a launch.
    // The second falls back to the first, which still exercises a relaunch --
    // and a relaunch is the case the commit rule is about (see arrtake.h).
    int slotA = -1, slotB = -1;
    for (int i = 0; i < kMaxScenes && i < (int)ses_.scenes.size(); ++i) {
        if (!tr.slots[i].valid() || tr.slots[i].kind == ClipKind::Midi) continue;
        if (slotA < 0) slotA = i; else if (slotB < 0) { slotB = i; break; }
    }
    if (slotA < 0) {
        LOGW("NXTAKT_DEBUG_ARRTAKE: track %d has no audio clip to perform with", t);
        return;
    }
    if (slotB < 0) slotB = slotA;

    // The hook's own minimal ClipModel -> RtClip conversion. NOT pushClip: that
    // one publishes note arrays, envelope sets and warp maps into tables keyed to
    // the app's engine, and pointing those at a private engine would corrupt the
    // bookkeeping the real audio thread depends on. Audio clips only, which is
    // why the slot search above skips MIDI.
    const auto rtOf = [](const ClipModel& m) {
        RtClip rc;
        rc.data        = m.sample->data.data();
        rc.frames      = m.sample->frames;
        rc.channels    = m.sample->channels;
        rc.loopStart   = m.loopStart;
        rc.loopEnd     = m.loopEnd > m.loopStart ? m.loopEnd : m.sample->frames;
        rc.clipBpm     = m.clipBpm;
        rc.warp        = (int)m.warp;
        rc.lengthBeats = m.lengthBeats;
        rc.gain        = m.gain;
        rc.loop        = m.loop;
        rc.quantumIdx  = m.quantumIdx;
        rc.valid       = true;
        return rc;
    };

    Engine eng;
    const f64 sr = 48000.0;
    const int blk = 512;
    eng.prepare(sr, blk);
    const auto cmd = [&](Cmd type, i32 a = 0, i32 b = 0, f64 x = 0.0) {
        Command c; c.type = type; c.a = a; c.b = b; c.x = x; eng.pushCommand(c);
    };
    cmd(Cmd::SetTempo, 0, 0, ses_.tempo);
    cmd(Cmd::SetQuantum, 4);                       // 1 Bar, so a launch is quantized
    { Command c; c.type = Cmd::SetClip; c.a = t; c.b = slotA; c.clip = rtOf(tr.slots[slotA]);
      eng.pushCommand(c); }
    if (slotB != slotA) {
        Command c; c.type = Cmd::SetClip; c.a = t; c.b = slotB; c.clip = rtOf(tr.slots[slotB]);
        eng.pushCommand(c);
    }

    // Arm, and only then perform: the arm is what makes the pass a take, and a
    // pass that opened before the arm is deliberately not recorded (there is no
    // TakeStart to record against, and inventing one would be the GUI stamping
    // the recording -- §5.2).
    const bool wasArmed = arrArm_;
    arrArm_ = true;

    std::vector<f32> l((size_t)blk), r((size_t)blk);
    const f64 bps = ses_.tempo / 60.0 / sr;
    const auto runTo = [&](f64 beat) {
        while (eng.beat.load() < beat - 1e-9) {
            eng.process(nullptr, nullptr, l.data(), r.data(), blk);
            pumpJournal(eng);                       // the REAL drain, per "frame"
        }
    };
    cmd(Cmd::LaunchClip, t, slotA);                 // fires at beat 0
    runTo(3.0);
    cmd(Cmd::LaunchClip, t, slotB);                 // asked for at 3, HAPPENS at 4
    runTo(7.0);
    cmd(Cmd::StopTrack, t);                         // asked for at 7, happens at 8
    runTo(9.0);
    cmd(Cmd::SetPlaying, 0);
    eng.process(nullptr, nullptr, l.data(), r.data(), blk);

    // What the commit will be measured against.
    const size_t undoBefore = undo_.size();
    const size_t itemsBefore = tr.arrange.size();
    const u32 gapsSeen = takeGaps_;
    const size_t entries = takeLog_.size();
    // The pass as drained SO FAR. The terminator is the entry that triggers the
    // commit, and the commit clears the accumulator, so it is not in this copy --
    // its beat is the second number in the status line the commit prints.
    std::vector<ArrJournal> log = takeLog_;

    pumpJournal(eng);                               // drains the TakeEnd -> commits
    (void)bps;

    const std::vector<ArrangeClip>& v = tr.arrange;
    LOGI("NXTAKT_DEBUG_ARRTAKE: track %d '%s' performed slots %d,%d -> %zu journal "
         "entries (as drained), %u gaps, %u engine drops, items %zu -> %zu, "
         "undo entries %zu -> %zu (%s)",
         t, tr.name.c_str(), slotA, slotB, entries, (unsigned)gapsSeen,
         (unsigned)eng.journalDropped.load(), itemsBefore, v.size(),
         undoBefore, undo_.size(),
         (undo_.size() == undoBefore + 1) ? "EXACTLY ONE" : "WRONG COUNT");
    static const char* kKind[] = {"none", "TakeStart", "TakeEnd", "ClipOn", "ClipOff",
                                  "NoteOn", "NoteOff", "Locate", "LoopWrap"};
    for (const ArrJournal& e : log)
        LOGI("  seq %u  %-9s track %d  a %d  beat %.6f", (unsigned)e.seq,
             e.kind < 9 ? kKind[e.kind] : "?", e.track, e.a, e.beat);
    for (size_t i = 0; i < v.size(); ++i)
        LOGI("  item %zu uid %llu  start %.3f len %.3f off %.3f  from clip '%s'",
             i, (unsigned long long)v[i].uid, v[i].start, v[i].length, v[i].offset,
             v[i].src.name.c_str());
    // The override is the other half of §4: the launches took the track out of
    // the arrangement, and it STAYS out until Back to Arrangement -- which is why
    // the commit is coherent rather than a track that silently changed masters.
    LOGI("  arrOverride now 0x%x (track %d %s), status: %s",
         (unsigned)eng.arrOverride.load(), t,
         (eng.arrOverride.load() & (1u << t)) ? "OVERRIDDEN, as it should be"
                                             : "NOT overridden - WRONG",
         status_.c_str());
    // --- and then the refusal, provoked on purpose --------------------------
    //
    // §10.6 calls this the single most important assertion in the milestone,
    // because the failure it guards is SILENT: a take committed with four bars
    // missing looks exactly like a performance that had four bars of rest. So
    // the hook does not merely record a good pass, it also stalls its own drain
    // while a second pass pours notes into the ring, and then checks that the
    // take was thrown away whole — no items, no undo entry, and a status line
    // that says how many entries were lost.
    const size_t itemsGood = tr.arrange.size();
    const size_t undoGood  = undo_.size();
    for (int a = 0; a < 8; ++a) cmd(Cmd::TrackArm, a, 1);
    cmd(Cmd::SetPlaying, 1);
    eng.process(nullptr, nullptr, l.data(), r.data(), blk);
    pumpJournal(eng);                               // the pass opens...
    const bool opened = takeOpen_;
    for (int b = 0; b < 4; ++b) {                   // ...and then the GUI "stalls"
        for (int i = 0; i < 250; ++i) {
            MidiMsg m; m.status = 0x90; m.d1 = (u8)(40 + (i % 40)); m.d2 = 100;
            m.frame = i % blk;
            eng.pushMidi(m);
        }
        eng.process(nullptr, nullptr, l.data(), r.data(), blk);
    }
    cmd(Cmd::SetPlaying, 0);
    eng.process(nullptr, nullptr, l.data(), r.data(), blk);
    const u32 enginDrops = eng.journalDropped.load();
    pumpJournal(eng);                               // drains, sees the gap, REFUSES
    arrArm_ = wasArmed;

    LOGI("NXTAKT_DEBUG_ARRTAKE: forced overflow -> %u entries refused by the ring, "
         "take %s (items %zu -> %zu, undo %zu -> %zu) %s",
         (unsigned)enginDrops, opened ? "opened" : "NEVER OPENED - WRONG",
         itemsGood, tr.arrange.size(), undoGood, undo_.size(),
         (enginDrops > 0 && tr.arrange.size() == itemsGood && undo_.size() == undoGood)
             ? "REFUSED CLEANLY" : "WRONG - a gapped take changed something");
    LOGI("  status: %s", status_.c_str());

    if (t < kMaxTracks) arrExpanded_[t] = true;
    view_ = MainView::Arrangement;
}

void App::drawArrangementView(const Rect& r) {
    if (!arrView_) arrView_ = std::make_unique<ArrangeView>();
    std::vector<AutoTargets> targets;
    ArrangeContext ctx;
    buildArrangeContext(ctx, &targets);
    // A drag in flight from the browser (visible in this view) or from the
    // session grid (started before a Tab). See the report: §7.5 lists the second
    // as a gesture, and the two views are never on screen at once.
    ctx.dropActive = drag_.kind != DragState::Kind::None && drag_.armed;

    // The automation lanes' undo entry, and the one thing the pendingEdit
    // handshake cannot cover. A breakpoint edit mutates on the very frame of the
    // press (click-adds-a-point is the lane's whole gesture), so there is no
    // frame in between for the view to warn on -- the roll has the same problem
    // and solves it the same way, by copying the thing being edited before the
    // draw and handing the copy back for the length of the snapshot.
    //
    // Only the EXPANDED tracks are copied, because only an expanded track's
    // lanes are on screen to be clicked. That is what keeps this bounded: a
    // track's lanes can hold kMaxArrPoints breakpoints, and copying every
    // track's every frame would be the cost §7.6 warns about, paid for nothing.
    autosBefore_.clear();
    for (size_t i = 0; i < ses_.tracks.size() && i < kMaxTracks; ++i)
        if (arrExpanded_[i]) autosBefore_.push_back({(int)i, ses_.tracks[i].arrangeAutos});

    const u32 changed = arrView_->draw(ui_, r, ctx);
    arrangeCommit(ctx, changed);
}

// The Autos half of the commit, split out only because it is the half with a
// snapshot behind it. One entry per gesture, coalesced on whatever widget owns
// the mouse -- so dragging a breakpoint across the timeline is one entry and not
// one per frame, exactly as it is in the roll.
void App::arrangeCommitAutos(ArrangeContext& ctx, u32 changed) {
    if (!(changed & ArrangeView::Autos)) return;
    for (int t : ctx.dirty) {
        for (auto& snap : autosBefore_) {
            if (snap.first != t) continue;
            undoPointWith("automation edit", ses_.tracks[(size_t)t].arrangeAutos,
                          snap.second, ui_.active);
            // The snapshot has served its purpose for this gesture; leaving it
            // would make the NEXT frame of the same drag compare against a state
            // two frames old, which undoCoalesce already refuses to act on but
            // which would be a lie if it ever did.
            snap.second = ses_.tracks[(size_t)t].arrangeAutos;
            return;
        }
    }
}

void App::drawStatusBar(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, r.w, 1 * s}, pal::divider);
    // Ui::tip is what the control under the cursor wants said about itself, and
    // the status bar is the one place in the program with room to say it. It
    // wins over `status_` only while it is set — the bar is drawn last in the
    // frame, so every widget has already had its chance to ask — and the status
    // message is still there the moment the pointer moves off. Nothing else
    // rendered tips before this; the automation lane's key block needs them,
    // because a plugin parameter's name does not fit in a 46 px gutter.
    const bool tip = !ui_.tip.empty();
    rend_.textIn(fSmall_, r, tip ? ui_.tip.c_str() : status_.c_str(),
                 tip ? pal::textDim : pal::textFaint, Align::Left, 8 * s);

    // The MIDI tag carries the sequencer client id: nothing is auto-connected,
    // so the number is what the user needs to hand aconnect or qpwgraph.
    char midiTag[32] = "";
    if (midi_.running()) snprintf(midiTag, sizeof midiTag, " · MIDI %d:0", midi_.clientId());

    // Delay compensation, when the engine is applying any. It is latency the
    // user did not ask for and cannot see anywhere else, and it moves when a
    // plugin is added to a chain, so it belongs beside the buffer size.
    char pdcTag[24] = "";
    const int pdc = engine_.latencyFrames.load();
    if (pdc > 0) snprintf(pdcTag, sizeof pdcTag, " · PDC %d", pdc);

    char buf[224];
    snprintf(buf, sizeof buf, "%s · %s %.0f Hz / %d fr%s%s · %.0f fps · %d draws",
             win_.backendName(),
             audio_ ? audio_->name() : "silent",
             audio_ ? audio_->sampleRate() : 0.0,
             audio_ ? audio_->bufferSize() : 0,
             pdcTag,
             midiTag,
             fps_, rend_.drawCalls());
    rend_.textIn(fSmall_, r, buf, pal::textFaint, Align::Right, 8 * s);
}


// ---------------------------------------------------------------------------
// REMOTE CONTROL: MIDI-learn + OSC
//
// The block declared at the end of app.h. Three jobs, in order:
//
//   resolveControl   an ADDRESS -> the live control it names today, with its
//                    range and its current value, in the target's own units.
//                    Answers "does this name anything" — the resolution step
//                    PARAM-ADDRESS.md puts GUI-side, and the one that has to
//                    fail soft for a mapping to a device that has been deleted.
//   applyControl     write it exactly as the widget for that control does:
//                    model, engine command, ONE undo entry per gesture,
//                    autoCapture. Nothing here reaches the engine directly.
//   drainControlInput  once a frame, from drawControlBar.
//
// NORMALISATION lives at the boundary: both transports speak 0..1 (see
// learn.h), this file converts to and from the target's units, and nothing
// outside it needs a table of which parameter runs how far.
// ---------------------------------------------------------------------------

// The grammar, handed to the mapping layer as its structure check. src/control
// cannot see lat::addr (it is inside the UI's headers and would drag the whole
// session model into a file that is meant to link standalone), so the check is
// INJECTED rather than duplicated — which is what keeps one parser in the
// program while still rejecting a malformed address in midimap.conf.
static bool controlAddressOk(const std::string& a) {
    if (!ctl::lexicalAddressOk(a)) return false;
    addr::Parsed p;
    return addr::parse(a, p);
}

// A gesture id for an OSC path. The MIDI side gets one from the binding's
// trigger; OSC has no controller to key on, so the path is the control.
static u64 oscGesture(const char* path) {
    u64 h = 0xCBF29CE484222325ull;
    for (const char* p = path; *p; ++p) { h ^= (u8)*p; h *= 0x100000001B3ull; }
    return h ? h : 1;
}

bool App::resolveControl(const std::string& address, ControlRef& out) const {
    out = ControlRef{};
    addr::Parsed p;
    if (!addr::parse(address, p)) return false;

    if (p.scope == addr::Parsed::Scope::Scene) {
        if (p.field != addr::Parsed::Field::SceneLaunch) return false;
        for (size_t i = 0; i < ses_.scenes.size(); ++i) {
            if (ses_.scenes[i].uid != p.scopeUid) continue;
            out.kind = ControlRef::Kind::SceneLaunch;
            out.scene = (int)i;
            out.label = "scene launch";
            return true;
        }
        return false;
    }
    // `master/vol` parses and resolves to NOTHING, deliberately and reluctantly:
    // the master fader is a function-local static inside drawMasterStrip
    // (app_session.cpp), so there is no model field to write and a command sent
    // straight to the engine would leave the drawn fader lying about the gain.
    // Mapping it needs that fader promoted to a Session member first; until
    // then it behaves exactly as a deleted device does — silently inert.
    if (p.scope != addr::Parsed::Scope::Track) return false;

    int t = -1;
    for (size_t i = 0; i < ses_.tracks.size(); ++i)
        if (ses_.tracks[i].uid == p.scopeUid) { t = (int)i; break; }
    if (t < 0) return false;
    const TrackModel& tr = ses_.tracks[t];
    out.track = t;

    switch (p.field) {
    case addr::Parsed::Field::Vol:
        // The fader POSITION, not the gain — the same units the envelope stores
        // and the same units the vFader edits (docs/AUTOMATION.md §2.3).
        out.kind = ControlRef::Kind::TrackVol;
        out.lo = 0.f; out.hi = 1.f; out.value = tr.fader; out.label = "volume";
        return true;
    case addr::Parsed::Field::Pan:
        out.kind = ControlRef::Kind::TrackPan;
        out.lo = -1.f; out.hi = 1.f; out.value = tr.pan; out.label = "pan";
        return true;
    case addr::Parsed::Field::Send:
        if (p.sendIndex < 0 || p.sendIndex >= kMaxReturns) return false;
        out.kind = ControlRef::Kind::TrackSend;
        out.sendIndex = p.sendIndex;
        out.lo = 0.f; out.hi = 1.f; out.value = tr.sends[p.sendIndex];
        out.label = kSendUndo[p.sendIndex];
        return true;
    case addr::Parsed::Field::Mute:
        out.kind = ControlRef::Kind::TrackMute;
        out.isBool = true; out.value = tr.mute ? 1.f : 0.f; out.label = "mute";
        return true;
    case addr::Parsed::Field::Solo:
        out.kind = ControlRef::Kind::TrackSolo;
        out.isBool = true; out.value = tr.solo ? 1.f : 0.f; out.label = "solo";
        return true;
    case addr::Parsed::Field::Arm:
        out.kind = ControlRef::Kind::TrackArm;
        out.isBool = true; out.value = tr.arm ? 1.f : 0.f; out.label = "arm";
        return true;
    case addr::Parsed::Field::DeviceParam: {
        // The index in `devices`, NOT the published chain slot: a parameter is
        // written through the PluginInstance, which every DeviceModel with an
        // instance has, chain or no chain. (resolveAutoLane needs the chain slot
        // because the ENGINE writes that one; this path never leaves the GUI.)
        int di = -1;
        for (size_t i = 0; i < tr.devices.size(); ++i)
            if (tr.devices[i].uid == p.devUid && tr.devices[i].inst) { di = (int)i; break; }
        if (di < 0) return false;                  // deleted, or its plugin is gone
        const PluginInstance& inst = *tr.devices[di].inst;
        const int pc = inst.paramCount();
        for (int i = 0; i < pc; ++i) {
            const ParamInfo& info = inst.paramInfo(i);
            if (info.id != p.paramId) continue;
            out.kind = ControlRef::Kind::DeviceParam;
            out.devIndex = di;
            out.paramIndex = i;
            // Sorted, because a backend that reports them the wrong way round
            // would otherwise clamp every value to one end.
            out.lo = std::min(info.min, info.max);
            out.hi = std::max(info.min, info.max);
            out.value = tr.devices[di].inst->getParam(i);
            out.isBool = info.isBool;
            out.label = info.name.c_str();
            return true;
        }
        return false;                              // the plugin renumbered its params
    }
    default:
        // Clip fields have no control path yet; a scope on its own names no
        // value. Both parse and both resolve to nothing, which is the correct
        // answer and not an error.
        return false;
    }
}

bool App::applyControl(const std::string& address, f32 value, u64 gesture) {
    ControlRef ref;
    if (!resolveControl(address, ref)) { ++ctlInert_; return false; }

    // ONE UNDO ENTRY PER GESTURE. A knob sweep arrives as fifty CC messages
    // across as many frames, and a mouse drag's coalescing cannot be borrowed:
    // it keys on ui_.active, which App::frame() clears at the top of every
    // frame precisely because no widget owns the mouse. So the gesture is
    // tracked here and ended by GOING QUIET, which is the only end a knob with
    // no mouse-up has. `undoGesture_` is then handed the same id so that
    // autoCapture's own undo point (docs/AUTOMATION.md §5.4) coalesces into
    // ours instead of taking a second snapshot per message.
    const f64 now = nowSeconds();
    const bool fresh = gesture == 0 || gesture != ctlGesture_ ||
                       (now - ctlGestureAt_) > kCtlGestureGap;
    ctlGesture_ = gesture;
    ctlGestureAt_ = now;

    if (ref.kind == ControlRef::Kind::SceneLaunch) {
        // Transport, not an edit: no undo entry, exactly as clicking the scene
        // takes none (see the "what is not undoable" block in app.h).
        if (value < 0.5f) return false;
        send(Cmd::LaunchScene, ref.scene);
        status_ = "Launched " + (ses_.scenes[ref.scene].name.empty()
                                     ? ("scene " + std::to_string(ref.scene + 1))
                                     : ses_.scenes[ref.scene].name);
        ++ctlApplied_;
        ctlFlashAt_ = now;
        return true;
    }

    if (fresh) {
        // Clear the widget-side latch first. undoCoalesce() would otherwise
        // refuse this entry on the strength of the PREVIOUS gesture from the
        // same control still sitting in undoGesture_ — App::frame() normally
        // clears it once a frame, but two gestures on one knob can land inside
        // a single frame and the second one is still a second edit.
        undoGesture_ = 0;
        undoPoint(ref.label, gesture);
    } else {
        undoGesture_ = gesture;
    }

    const f32 v = clampv(value, std::min(ref.lo, ref.hi), std::max(ref.lo, ref.hi));
    TrackModel& tr = ses_.tracks[ref.track];

    switch (ref.kind) {
    case ControlRef::Kind::TrackVol:
        tr.fader = v;
        send(Cmd::TrackVol, ref.track, 0, faderToGain(v));
        autoCapture(address, v, gesture);
        break;
    case ControlRef::Kind::TrackPan:
        tr.pan = v;
        send(Cmd::TrackPan, ref.track, 0, v);
        autoCapture(address, v, gesture);
        break;
    case ControlRef::Kind::TrackSend:
        tr.sends[ref.sendIndex] = v;
        send(Cmd::SendLevel, ref.track, ref.sendIndex, v);
        autoCapture(address, v, gesture);
        break;
    case ControlRef::Kind::TrackMute:
        tr.mute = v >= 0.5f;
        send(Cmd::TrackMute, ref.track, tr.mute ? 1 : 0);
        autoCapture(address, tr.mute ? 1.f : 0.f, gesture);
        break;
    case ControlRef::Kind::TrackSolo:
        tr.solo = v >= 0.5f;
        send(Cmd::TrackSolo, ref.track, tr.solo ? 1 : 0);
        break;
    case ControlRef::Kind::TrackArm:
        tr.arm = v >= 0.5f;
        send(Cmd::TrackArm, ref.track, tr.arm ? 1 : 0);
        // Armed from outside is still armed by hand: the exclusive arm that
        // follows the selection must not take it away again (see selectTrack).
        if (ref.track == autoArmed_) autoArmed_ = -1;
        break;
    case ControlRef::Kind::DeviceParam: {
        DeviceModel& d = tr.devices[ref.devIndex];
        if (!d.inst) return false;
        d.inst->setParam(ref.paramIndex, v);
        autoCapture(address, v, gesture);
        break;
    }
    default:
        return false;
    }

    ++ctlApplied_;
    ctlFlashAt_ = now;
    return true;
}

// A Hit is normalised (learn.h); this is where it meets the target's real
// range. Nudge and Toggle both need the CURRENT value, which is why the
// mapping layer cannot compute a final value on its own and does not try.
bool App::applyControlHit(const ctl::Hit& h) {
    ControlRef ref;
    if (!resolveControl(h.address, ref)) { ++ctlInert_; return false; }
    const f32 span = ref.hi - ref.lo;
    const f32 cur = span != 0.f ? (ref.value - ref.lo) / span : 0.f;

    f32 n = cur;
    switch (h.act) {
    case ctl::Hit::Act::Set:   n = h.norm; break;
    case ctl::Hit::Act::Nudge: n = cur + h.norm; break;
    case ctl::Hit::Act::Toggle:
        // Flip between the binding's two ends. Which end "off" is follows the
        // binding, so an inverted mapping toggles the other way round and a
        // limited one toggles between its own limits rather than 0 and 1.
        n = (cur >= (h.lo + h.hi) * 0.5f) ? std::min(h.lo, h.hi) : std::max(h.lo, h.hi);
        break;
    }
    return applyControl(h.address, ref.lo + clampv(n, 0.f, 1.f) * span, h.gesture);
}

bool App::routeControlMidi(const MidiMsg& m) {
    ctlEnsureInit();
    bool learned = false;
    const std::optional<ctl::Hit> hit = midiMap_.consume(m, &learned);
    if (learned) {
        ctlSaveMap();
        if (const ctl::Binding* b = midiMap_.at(midiMap_.size() - 1)) {
            char buf[192];
            snprintf(buf, sizeof buf, "Mapped %s %d (ch %d) to %s",
                     b->isNote() ? "note" : "CC", (int)b->data1, (int)b->channel + 1,
                     b->address.c_str());
            status_ = buf;
        }
        return true;
    }
    if (!hit) return false;
    applyControlHit(*hit);
    return true;
}

void App::routeControlOsc(const ctl::OscHit& oh) {
    std::string address;
    if (!ctl::oscPathToAddress(oh.path, address)) return;
    ControlRef ref;
    if (!resolveControl(address, ref)) { ++ctlInert_; return; }
    // A message with no argument at all is a trigger — the shape a scene-launch
    // button on a phone sends. Everything else is normalised 0..1 (osc.h).
    const f32 n = oh.type ? clampv(oh.value, 0.f, 1.f) : 1.f;
    applyControl(address, ref.lo + n * (ref.hi - ref.lo), oscGesture(oh.path));
}

void App::drainControlInput() {
    ctlEnsureInit();

    // Bounded per frame on both rings: a controller sweeping while the GUI is
    // busy, or a hostile flood on the OSC socket, must not turn one frame into
    // an unbounded amount of work. Anything left over is drained next frame.
    MidiMsg m;
    for (int i = 0; i < 256 && ctl::midiTapPop(m); ++i) routeControlMidi(m);
    ctl::OscHit oh;
    for (int i = 0; i < 256 && osc_.poll(oh); ++i) routeControlOsc(oh);

    // A control gesture ends by going quiet — see applyControl.
    if (ctlGesture_ && nowSeconds() - ctlGestureAt_ > kCtlGestureGap) ctlGesture_ = 0;
}

void App::ctlEnsureInit() {
    if (ctlInit_) return;
    ctlInit_ = true;

    midiMap_.setAddressCheck(&controlAddressOk);
    // The self-test writes bindings, so it is never allowed near the user's own
    // configuration: the variable names the scratch file it may have.
    const char* selfTest = env("DEBUG_MIDIMAP");
    ctlMapPath_ = (selfTest && *selfTest) ? std::string(selfTest) : ctl::defaultMapPath();
    std::string err;
    if (!midiMap_.load(ctlMapPath_, &err)) {
        // Refused wholesale, and the file is left exactly as it is: see
        // ctlMapReadable_ in app.h for why a partial recovery is worse.
        ctlMapReadable_ = false;
        LOGW("midimap: %s — starting with no bindings; the file is left alone", err.c_str());
        status_ = "MIDI map could not be read: " + err;
    } else if (midiMap_.size()) {
        LOGI("midimap: %zu bindings from %s", midiMap_.size(), ctlMapPath_.c_str());
    }

    const ctl::OscServer::Config cfg = ctl::OscServer::configFromEnvironment();
    if (cfg.enabled) {
        std::string oerr;
        if (!osc_.start(cfg, &oerr)) {
            LOGW("osc: %s", oerr.c_str());
            status_ = "OSC: " + oerr;
        }
    }

    if (selfTest && *selfTest) debugMidiMapSelfTest();
}

// See the declaration in app.h. Everything below goes through the production
// path: a message is pushed into the reader-thread ring exactly as midi_in.cpp
// would push it, popped exactly as drainControlInput pops it, and routed by
// routeControlMidi. Nothing here reaches into the mapping layer to shortcut a
// step, because the steps are what is being checked.
void App::debugMidiMapSelfTest() {
    if (ses_.tracks.size() < 3) { LOGW("midimap self-test: needs three tracks"); return; }
    int pass = 0, fail = 0;
    auto ck = [&](bool ok, const char* what) {
        if (ok) { ++pass; return; }
        ++fail;
        LOGW("midimap self-test: FAIL %s", what);
    };
    auto feed = [&](u8 status, u8 d1, u8 d2) {
        MidiMsg m; m.status = status; m.d1 = d1; m.d2 = d2;
        ctl::midiTap(m);
        MidiMsg got;
        while (ctl::midiTapPop(got)) routeControlMidi(got);
    };

    const u64 t0 = ses_.tracks[0].uid, t1 = ses_.tracks[1].uid, t2 = ses_.tracks[2].uid;

    // --- absolute: learn CC 7 on channel 0 for track 1's fader -------------
    cycleMidiLearn(addr::trackField(t0, "vol"));
    ck(midiMap_.learning(), "learn armed");
    feed(0xB0, 7, 64);
    ck(!midiMap_.learning() && midiMap_.size() == 1, "learned from the first control message");
    feed(0xB0, 7, 0);
    ck(ses_.tracks[0].fader == 0.f, "CC 0 -> fader 0");
    feed(0xB0, 7, 127);
    ck(ses_.tracks[0].fader == 1.f, "CC 127 -> fader 1");

    // ONE undo entry for a whole sweep, exactly as a drag takes one. The reset
    // stands in for the pause that ends the previous gesture — the self-test
    // runs inside a single frame, so no wall-clock gap can elapse on its own.
    ctlGesture_ = 0;
    const size_t undoBefore = undo_.size();
    for (int v = 0; v <= 127; v += 8) feed(0xB0, 7, (u8)v);
    ck(undo_.size() == undoBefore + 1, "a sweep of 16 messages takes ONE undo entry");

    // --- toggle: learn note 36 on channel 9 for track 2's mute -------------
    const bool muteBefore = ses_.tracks[1].mute;
    cycleMidiLearn(addr::trackField(t1, "mute"));
    feed(0x99, 36, 100);
    ck(midiMap_.size() == 2 && midiMap_.at(1)->mode == ctl::Mode::Toggle, "note learned as a toggle");
    feed(0x99, 36, 100);
    ck(ses_.tracks[1].mute != muteBefore, "note-on flips mute");
    feed(0x89, 36, 0);
    ck(ses_.tracks[1].mute != muteBefore, "note-off does not flip it back");
    feed(0x99, 36, 100);
    ck(ses_.tracks[1].mute == muteBefore, "the next note-on flips it back");

    // --- relative: an endless encoder on track 3's pan ---------------------
    ses_.tracks[2].pan = 0.f;
    midiMap_.beginLearn(addr::trackField(t2, "pan"), ctl::Mode::Relative);
    feed(0xB0, 20, 65);                       // learns, and latches offset-64
    ck(midiMap_.size() == 3, "encoder learned");
    for (int i = 0; i < 32; ++i) feed(0xB0, 20, 65);
    const f32 up = ses_.tracks[2].pan;
    ck(up > 0.4f && up < 0.6f, "32 detents up move pan about half its span");
    for (int i = 0; i < 64; ++i) feed(0xB0, 20, 63);
    ck(ses_.tracks[2].pan < -0.4f, "and back down past centre");
    ck(midiMap_.at(2)->rel_seen == ctl::Rel::Offset64, "offset-64 auto-detected");

    // --- a dangling address is inert, not a crash --------------------------
    {
        ctl::Binding b;
        b.status = 0xB0; b.channel = 0; b.data1 = 30;
        b.address = "t:999999/vol";
        ck(midiMap_.bind(b) >= 0, "a binding to a non-existent track is storable");
        const u64 inertBefore = ctlInert_;
        feed(0xB0, 30, 100);
        ck(ctlInert_ == inertBefore + 1, "and resolves to nothing, silently");
        midiMap_.unbindAddress("t:999999/vol");
    }

    // --- the config file -----------------------------------------------------
    ctlSaveMap();
    const std::string wrote = midiMap_.serialize();
    ctl::MidiMap back;
    back.setAddressCheck(&controlAddressOk);
    std::string err;
    ck(back.load(ctlMapPath_, &err), "the saved map reloads");
    ck(back.serialize() == wrote, "byte-identical round trip through the file");

    LOGI("midimap self-test: %d passed, %d failed (%zu bindings -> %s)",
         pass, fail, midiMap_.size(), ctlMapPath_.c_str());
    status_ = "MIDI map self-test: " + std::to_string(pass) + " passed, " +
              std::to_string(fail) + " failed";
}

void App::ctlSaveMap() {
    if (!ctlMapReadable_) {
        status_ = "MIDI map not saved — " + ctlMapPath_ + " could not be read";
        return;
    }
    std::string err;
    if (midiMap_.save(ctlMapPath_, &err)) midiMap_.clearDirty();
    else { LOGW("midimap: %s", err.c_str()); status_ = "Could not save the MIDI map"; }
}

void App::cycleMidiLearn(const std::string& address) {
    ctlEnsureInit();
    if (midiMap_.learningFor(address)) {
        midiMap_.cancelLearn();
        status_ = "MIDI learn cancelled";
        return;
    }
    if (midiMap_.findAddress(address) >= 0) {
        midiMap_.unbindAddress(address);
        ctlSaveMap();
        status_ = "MIDI mapping cleared for " + address;
        return;
    }
    midiMap_.beginLearn(address);
    if (!midiMap_.learning()) {          // the address does not name anything mappable
        status_ = "Cannot map " + address;
        return;
    }
    status_ = "MIDI learn: move a control to map " + address;
    // Say so HERE rather than leaving the user turning knobs at a chip that
    // never changes: both of these are states nothing else in the UI reports.
    if (!midi_.running()) status_ += " — but no MIDI input is open";
    else if (ctl::midiTapCount() == 0 && midi_.received() > 0)
        status_ += " — but MIDI input is not tapped";
}


} // namespace lat
