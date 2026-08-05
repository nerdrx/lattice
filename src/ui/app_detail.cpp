// Clip detail: waveform, the detail-panel tab header, and the CLIP tab that
// hosts the PianoRoll. Moved verbatim from app.cpp.
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
        if (detailTab_ == DetailTab::Clip) {
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
    if (detailTab_ == DetailTab::Clip) drawClipDetail(content);
    else                               drawDeviceDetail(content);
}

void App::drawClipDetail(const Rect& r) {
    const f32 s = win_.dpiScale();

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

    // --- the material: a piano roll for a pattern, a waveform for a sample ---
    Rect wave{ctrl.right() + 12 * s, head.bottom() + 6 * s,
              r.right() - ctrl.right() - 20 * s, r.bottom() - head.bottom() - 12 * s};

    // Where the clip is, in its own beats, so both editors can draw the same
    // playhead from the same number.
    const bool active = engine_.activeSlot[selTrack_].load() == selSlot_;
    const f64  phase  = clampv(engine_.clipPhase[selTrack_].load(), 0.0, 1.0);

    if (midi) {
        if (!roll_) roll_ = std::make_unique<PianoRoll>();
        // The roll edits m.notes (and its length) in place and says whether it
        // touched anything; republishing is ours, and pushClip is what retires
        // the array the engine is still reading from.
        //
        // The undo entry therefore needs the clip as it was *before* the call,
        // which is why the copy is taken unconditionally: whether an edit
        // happens is not knowable until draw() returns, and by then m already
        // has it. A clip is a note vector, two strings and a shared pointer --
        // cheap enough to copy once a frame, and it buys the one thing that
        // matters here, which is that a click that adds a note can be undone.
        // The roll owns ui_.active for the length of a drag, so a note dragged
        // across the grid leaves one entry and not one per frame.
        const ClipModel before = m;
        if (roll_->draw(ui_, wave, m, active ? phase * m.lengthBeats : 0.0, active)) {
            undoPointWith("note edit", m, before);
            pushClip(selTrack_, selSlot_);
        }
        // Auditioning is the caller's job: the roll only names the pitches that
        // want to be heard (from this draw, and from any keyboard edit earlier
        // in the frame — handleShortcuts runs first). See previews_ for why
        // these reach the right instrument.
        u8 pv[PianoRoll::kPreviewMax];
        const int np = roll_->drainPreview(pv, PianoRoll::kPreviewMax);
        for (int i = 0; i < np; ++i) startPreview((int)pv[i], m.uid);
        return;
    }

    rend_.roundRect(wave, 2 * s, pal::appBg);
    rend_.pushClip(wave.inset(2 * s));
    drawWaveform(wave.inset(3 * s), *m.sample, ccol.scale(0.85f));

    // Playhead, when this clip is the one sounding on its track.
    if (active) {
        const f32 px = wave.x + 3 * s + (wave.w - 6 * s) * (f32)phase;
        rend_.rect({px, wave.y + 2 * s, 1.5f * s, wave.h - 4 * s}, pal::playGreen);
    }
    rend_.popClip();
}


} // namespace lat
