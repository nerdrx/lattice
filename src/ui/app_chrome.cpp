// Chrome: the control bar, the file browser (model + draw), the status bar
// and the arrangement placeholder. Independent leftovers that share a file
// only because they share no state. Moved verbatim from app.cpp.
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
        x = autoR.right() + 12 * s;
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

void App::drawArrangementView(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::appBg);

    // Timeline ruler so the view is oriented even before it does anything.
    Rect ruler{r.x, r.y, r.w, 22 * s};
    rend_.rect(ruler, pal::panel);
    const f32 pxPerBar = 48 * s;
    for (int bar_ = 0; bar_ * pxPerBar < r.w; ++bar_) {
        const f32 x = r.x + bar_ * pxPerBar;
        rend_.rect({x, ruler.y, 1 * s, ruler.h}, pal::ridge);
        if (bar_ % 4 == 0) {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", bar_ + 1);
            rend_.text(fSmall_, x + 3 * s, ruler.y + 4 * s, buf, pal::textFaint);
        }
    }

    f32 y = ruler.bottom();
    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        Rect lane{r.x, y, r.w, 44 * s};
        rend_.rect({lane.x, lane.y, lane.w, lane.h - 1 * s},
                   i % 2 ? pal::appBg : pal::appBg.scale(1.12f));
        rend_.rect({lane.x, lane.y, 3 * s, lane.h - 1 * s},
                   pal::clipColors[ses_.tracks[i].colorIdx % pal::clipColorCount]);
        rend_.textIn(fBody_, {lane.x + 10 * s, lane.y, 160 * s, lane.h},
                     ses_.tracks[i].name.c_str(), pal::textDim, Align::Left, 0);
        y += 44 * s;
        if (y > r.bottom()) break;
    }

    // Playhead against the same bar grid.
    const f64 beat = engine_.beat.load();
    const f32 px = r.x + (f32)(beat / ses_.sigNum) * pxPerBar;
    if (px >= r.x && px <= r.right())
        rend_.rect({px, ruler.bottom(), 1.5f * s, r.bottom() - ruler.bottom()}, pal::playGreen);

    rend_.textIn(fBody_, {r.x, r.bottom() - 40 * s, r.w, 20 * s},
                 "Arrangement recording is not wired up yet — Tab returns to Session",
                 pal::textFaint, Align::Center);
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
