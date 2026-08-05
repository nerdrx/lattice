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

void App::drawControlBar(const Rect& r) {
    const f32 s = win_.dpiScale();
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


} // namespace lat
