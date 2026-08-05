#include "app.h"
#include "../core/project.h"
#include "../gfx/gl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

namespace lat {

// Layout constants, in logical px before the DPI scale is applied.
namespace lay {
constexpr f32 controlBarH = 38.f;
constexpr f32 statusH     = 20.f;
constexpr f32 trackHeadH  = 21.f;
constexpr f32 slotH       = 21.f;
constexpr f32 sceneColW   = 96.f;
constexpr f32 masterW     = 92.f;
constexpr f32 mixerH      = 152.f;
constexpr f32 gutter      = 1.f;
}

static f64 nowSeconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

static std::string homeDir() {
    if (const char* h = getenv("HOME")) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

static bool isAudioFile(const std::string& n) {
    static const char* ext[] = {".wav", ".flac", ".aiff", ".aif", ".ogg", ".mp3", ".opus", ".w64", nullptr};
    const size_t dot = n.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = n.substr(dot);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    for (int i = 0; ext[i]; ++i) if (e == ext[i]) return true;
    return false;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool App::init(int argc, char** argv) {
    if (!win_.create("Lattice", 1360, 860)) return false;
    if (!rend_.init()) return false;

    const f32 s = win_.dpiScale();
    const std::string reg = findSystemFont(false);
    const std::string bold = findSystemFont(true);
    if (reg.empty()) { LOGE("no usable system font found"); return false; }
    fSmall_.load(reg.c_str(),  (int)std::lround(9.f * s));
    fBody_.load(reg.c_str(),   (int)std::lround(11.f * s));
    fBold_.load(bold.empty() ? reg.c_str() : bold.c_str(), (int)std::lround(11.f * s));
    fBig_.load(bold.empty() ? reg.c_str() : bold.c_str(),  (int)std::lround(15.f * s));

    ui_.r = &rend_;
    ui_.in = &win_.input();
    ui_.fSmall = &fSmall_;
    ui_.fBody = &fBody_;
    ui_.fBold = &fBold_;
    ui_.fBig = &fBig_;

    audio_ = createBackend(engine_, getenv("LATTICE_AUDIO"));
    if (!audio_) {
        LOGW("no audio backend available - running silent");
        engine_.prepare(48000.0, 1024);
    }

    // Default set: eight audio tracks, eight scenes, same as a fresh Live set.
    ses_.tracks.resize(8);
    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        char buf[32];
        snprintf(buf, sizeof buf, "%zu Audio", i + 1);
        ses_.tracks[i].name = buf;
        ses_.tracks[i].colorIdx = (int)(i * 3 + 4) % pal::clipColorCount;
    }
    ses_.scenes.resize(8);
    for (size_t i = 0; i < ses_.scenes.size(); ++i) {
        char buf[32];
        snprintf(buf, sizeof buf, "Scene %zu", i + 1);
        ses_.scenes[i].name = buf;
    }

    browserPlaces_ = {homeDir() + "/Music", homeDir() + "/Downloads", homeDir(), "/usr/share/sounds"};
    browseTo(browserPlaces_[0]);
    if (browserItems_.empty()) browseTo(homeDir());

    // A project path on the command line loads instead of the default set.
    if (argc > 1) {
        std::string err;
        if (!loadProject(ses_, argv[1], engine_.sampleRate(), &err))
            LOGW("could not load %s: %s", argv[1], err.c_str());
    }

    pushAll();
    status_ = "Ready";
    LOGI("backend: %s   audio: %s", win_.backendName(), audio_ ? audio_->name() : "none");
    return true;
}

void App::shutdown() {
    if (audio_) { audio_->stop(); audio_.reset(); }
    fSmall_.destroy(); fBody_.destroy(); fBold_.destroy(); fBig_.destroy();
    rend_.shutdown();
    win_.destroy();
}

void App::run() {
    lastFrameTime_ = nowSeconds();
    while (running_ && win_.pump()) {
        frame();
        win_.swap();
    }
}

// ---------------------------------------------------------------------------
// engine plumbing
// ---------------------------------------------------------------------------

void App::send(Cmd t, i32 a, i32 b, f64 x) {
    Command c;
    c.type = t; c.a = a; c.b = b; c.x = x;
    engine_.pushCommand(c);
}

void App::pushClip(int track, int slot) {
    Command c;
    c.type = Cmd::SetClip;
    c.a = track; c.b = slot;
    const ClipModel& m = ses_.tracks[track].slots[slot];
    if (!m.valid()) { c.type = Cmd::ClearClip; engine_.pushCommand(c); return; }

    RtClip rc;
    rc.data        = m.sample->data.data();
    rc.frames      = m.sample->frames;
    rc.channels    = m.sample->channels;
    rc.loopStart   = m.loopStart;
    rc.loopEnd     = m.loopEnd > m.loopStart ? m.loopEnd : m.sample->frames;
    rc.clipBpm     = m.clipBpm;
    rc.lengthBeats = m.lengthBeats;
    rc.gain        = m.gain;
    rc.warp        = (int)m.warp;
    rc.loop        = m.loop;
    rc.quantumIdx  = m.quantumIdx;
    rc.valid       = true;
    c.clip = rc;
    engine_.pushCommand(c);
}

void App::pushTrack(int t) {
    const TrackModel& tr = ses_.tracks[t];
    send(Cmd::TrackVol,  t, 0, faderToGain(tr.fader));
    send(Cmd::TrackPan,  t, 0, tr.pan);
    send(Cmd::TrackMute, t, tr.mute ? 1 : 0);
    send(Cmd::TrackSolo, t, tr.solo ? 1 : 0);
    send(Cmd::TrackArm,  t, tr.arm ? 1 : 0);
}

void App::pushAll() {
    send(Cmd::SetTempo, 0, 0, ses_.tempo);
    send(Cmd::SetQuantum, ses_.quantumIdx);
    send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    for (size_t t = 0; t < ses_.tracks.size(); ++t) {
        pushTrack((int)t);
        for (int s = 0; s < (int)ses_.scenes.size(); ++s) pushClip((int)t, s);
    }
}

void App::pumpEngineEvents() {
    Event e;
    while (engine_.popEvent(e)) {
        // Reserved for undo/recording hooks; the UI polls atomics for state.
        (void)e;
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

void App::loadClipInto(int track, int slot, const std::string& path) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;
    SampleRef sb = loadSample(path, engine_.sampleRate());
    if (!sb) { status_ = "Could not load " + path; return; }

    ClipModel& m = ses_.tracks[track].slots[slot];
    m.sample = sb;
    m.name = sb->name;
    const size_t dot = m.name.find_last_of('.');
    if (dot != std::string::npos) m.name = m.name.substr(0, dot);
    m.colorIdx = ses_.tracks[track].colorIdx;
    m.clipBpm = sb->guessedBpm;
    m.lengthBeats = sb->guessedBeats;
    m.loopStart = 0;
    m.loopEnd = sb->frames;
    m.gain = 1.f;
    m.warp = Warp::Beats;
    m.loop = true;
    pushClip(track, slot);
    selTrack_ = track; selSlot_ = slot;
    status_ = "Loaded " + m.name;
}

void App::clearClip(int track, int slot) {
    ses_.tracks[track].slots[slot] = ClipModel{};
    send(Cmd::ClearClip, track, slot);
}

void App::addTrack() {
    if (ses_.tracks.size() >= kMaxTracks) return;
    TrackModel t;
    char buf[32];
    snprintf(buf, sizeof buf, "%zu Audio", ses_.tracks.size() + 1);
    t.name = buf;
    t.colorIdx = (int)(ses_.tracks.size() * 3 + 4) % pal::clipColorCount;
    ses_.tracks.push_back(t);
    pushTrack((int)ses_.tracks.size() - 1);
}

void App::addScene() {
    if (ses_.scenes.size() >= kMaxScenes) return;
    SceneModel s;
    char buf[32];
    snprintf(buf, sizeof buf, "Scene %zu", ses_.scenes.size() + 1);
    s.name = buf;
    ses_.scenes.push_back(s);
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
// frame
// ---------------------------------------------------------------------------

void App::frame() {
    const f64 t = nowSeconds();
    const f32 dt = (f32)(t - lastFrameTime_);
    lastFrameTime_ = t;
    fps_ = fps_ * 0.92f + (dt > 0.f ? 1.f / dt : 0.f) * 0.08f;

    pumpEngineEvents();

    const f32 s = win_.dpiScale();
    const f32 W = (f32)win_.width(), H = (f32)win_.height();

    rend_.begin(win_.width(), win_.height(), s);
    glClearColor(pal::appBg.r, pal::appBg.g, pal::appBg.b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ui_.beginFrame();
    handleShortcuts();

    Rect full{0, 0, W, H};
    Rect bar = {0, 0, W, lay::controlBarH * s};
    Rect status = {0, H - lay::statusH * s, W, lay::statusH * s};
    Rect body = {0, bar.bottom(), W, status.y - bar.bottom()};

    drawControlBar(bar);

    Rect detail{};
    if (showDetail_ && view_ == MainView::Session) {
        detail = {0, body.bottom() - detailH_ * s, W, detailH_ * s};
        body.h -= detail.h;
    }

    Rect main = body;
    if (showBrowser_) {
        Rect br = {0, body.y, browserW_ * s, body.h};
        drawBrowser(br);
        main = {br.right(), body.y, W - br.right(), body.h};
    }

    if (view_ == MainView::Session) drawSessionView(main);
    else                            drawArrangementView(main);

    if (showDetail_ && view_ == MainView::Session) drawClipDetail(detail);
    drawStatusBar(status);
    drawDragGhost();

    ui_.endFrame();
    win_.setCursor(ui_.cursor);
    rend_.end();
    (void)full;
}

void App::handleShortcuts() {
    Input& in = win_.input();
    if (ui_.editId) return;                      // typing takes precedence

    if (in.keyPressed[' ']) togglePlay();
    if (in.keyPressed[KeyTab])
        view_ = (view_ == MainView::Session) ? MainView::Arrangement : MainView::Session;
    if (in.keyPressed['b'] && in.ctrl()) showBrowser_ = !showBrowser_;
    if (in.keyPressed['d'] && in.ctrl()) showDetail_ = !showDetail_;
    if (in.keyPressed['m'] && !in.ctrl()) {
        ses_.metronome = !ses_.metronome;
        send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    }
    if (in.keyPressed['t'] && in.ctrl()) addTrack();
    if (in.keyPressed[KeyEnter] && in.ctrl()) addScene();

    if (in.keyPressed[KeyEscape]) send(Cmd::StopAll);
    if (in.keyPressed[KeyDelete] || (in.keyPressed[KeyBackspace] && !in.ctrl()))
        clearClip(selTrack_, selSlot_);

    const int nt = (int)ses_.tracks.size(), ns = (int)ses_.scenes.size();
    if (in.keyPressed[KeyLeft])  selTrack_ = clampv(selTrack_ - 1, 0, nt - 1);
    if (in.keyPressed[KeyRight]) selTrack_ = clampv(selTrack_ + 1, 0, nt - 1);
    if (in.keyPressed[KeyUp])    selSlot_  = clampv(selSlot_ - 1, 0, ns - 1);
    if (in.keyPressed[KeyDown])  selSlot_  = clampv(selSlot_ + 1, 0, ns - 1);
    if (in.keyPressed[KeyEnter] && !in.ctrl()) {
        if (ses_.tracks[selTrack_].slots[selSlot_].valid())
            send(Cmd::LaunchClip, selTrack_, selSlot_);
    }

    if (in.keyPressed['s'] && in.ctrl()) {
        std::string err;
        const std::string p = ses_.path.empty() ? (homeDir() + "/" + ses_.name + ".lattice") : ses_.path;
        status_ = saveProject(ses_, p, &err) ? ("Saved " + p) : ("Save failed: " + err);
    }
}

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
        if (now - lastTap < 3.0) setTempo(clampv(60.0 / (now - lastTap), 20.0, 999.0));
        lastTap = now;
    }
    x += tapR.w + 4 * s;

    Rect tempoR{x, cy, 62 * s, h};
    f64 bpm = ses_.tempo;
    if (ui_.dragNumber(uiId(1, 1), tempoR, &bpm, 20.0, 999.0, 0.15, "%.2f")) setTempo(bpm);
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
        ses_.metronome = !ses_.metronome;
        send(Cmd::SetMetronome, ses_.metronome ? 1 : 0);
    }
    x += metR.w + 12 * s;

    // --- global launch quantum ---
    rend_.textIn(fSmall_, {x, cy, 26 * s, h}, "Q", pal::textFaint, Align::Left, 0);
    Rect quantR{x + 16 * s, cy, 62 * s, h};
    if (ui_.selector(uiId(1, 3), quantR, &ses_.quantumIdx, kQuantumNames, kQuantumCount))
        send(Cmd::SetQuantum, ses_.quantumIdx);
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

    Rect recR{x, cy, 30 * s, h};
    ui_.button(uiId(1, 6), recR, "");
    rend_.circle(recR.cx(), recR.cy(), 5 * s, pal::recRed);
    x += recR.w + 12 * s;

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
// session view
// ---------------------------------------------------------------------------

void App::drawSessionView(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::appBg);

    const f32 masterW = lay::masterW * s;
    const f32 sceneW  = lay::sceneColW * s;
    Rect masterCol{r.right() - masterW, r.y, masterW, r.h};
    Rect sceneCol{masterCol.x - sceneW, r.y, sceneW, r.h};
    Rect tracksCol{r.x, r.y, sceneCol.x - r.x, r.h};

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

        if (ui_.textField(uiId(3, 1000 + (int)i), cell, &t.name,
                          Col(0, 0, 0, 0), sel ? pal::text : pal::textDim, Align::Left)) {}
        if (hot && in.pressed[0]) selTrack_ = (int)i;
    }

    // "+" to append a track.
    Rect add{x, r.y, 22 * s, h};
    if (add.x < r.right()) {
        if (ui_.button(uiId(3, 900), add, "+")) addTrack();
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

    const int state = engine_.slotState[ti].load();
    const int active = engine_.activeSlot[ti].load();
    const int pending = engine_.pendingSlot[ti].load();
    const bool playing = (state == (int)SlotState::Playing || state == (int)SlotState::StopQueued) && active == si;
    const bool queued  = pending == si;

    if (!m.valid()) {
        rend_.roundRect(cell, 2 * s, hot ? pal::slotHover : pal::slotEmpty);
        if (sel) rend_.roundRectOutline(cell, 2 * s, 1 * s, pal::accent);
        if (hot) {
            ui_.cursor = Cursor::Hand;
            if (in.pressed[0]) { selTrack_ = ti; selSlot_ = si; }
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

    rend_.textIn(fBody_, {cell.x + btnW, cell.y, cell.w - btnW - 2 * s, cell.h},
                 m.name.c_str(), pal::textOnClip, Align::Left, 2 * s);

    // Playback progress along the bottom edge.
    if (playing) {
        const f64 ph = clampv(engine_.clipPhase[ti].load(), 0.0, 1.0);
        rend_.rect({cell.x, cell.bottom() - 2 * s, cell.w * (f32)ph, 2 * s}, pal::textOnClip.alpha(0.45f));
    }
    if (sel) rend_.roundRectOutline(cell, 2 * s, 1 * s, pal::accent);

    if (hot) {
        ui_.cursor = Cursor::Hand;
        if (in.pressed[0]) {
            selTrack_ = ti; selSlot_ = si;
            send(Cmd::LaunchClip, ti, si);
            drag_.kind = DragState::Kind::Clip;
            drag_.srcTrack = ti; drag_.srcSlot = si;
            drag_.startX = in.mx; drag_.startY = in.my;
            drag_.armed = false;
        }
        if (in.pressed[2]) { selTrack_ = ti; selSlot_ = si; clearClip(ti, si); }
    }

    // Drop target for a drag in flight.
    if (drag_.kind != DragState::Kind::None && drag_.armed && hot && in.released[0]) {
        if (drag_.kind == DragState::Kind::BrowserFile) loadClipInto(ti, si, drag_.path);
        else if (drag_.srcTrack != ti || drag_.srcSlot != si) {
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
        ui_.textField(uiId(5, 1000 + si), {cell.x + 14 * s, cell.y, cell.w - 16 * s, cell.h},
                      &ses_.scenes[si].name, Col(0, 0, 0, 0), pal::text, Align::Left);

        if (hot) ui_.cursor = Cursor::Hand;
        if (hot && in.pressed[0]) { selSlot_ = si; send(Cmd::LaunchScene, si); }
    }

    Rect stopAll{r.x + 2 * s, top + ns * slotH, r.w - 4 * s, slotH - lay::gutter * s};
    if (stopAll.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 900), stopAll, "STOP ALL")) send(Cmd::StopAll);
    }

    Rect add{r.x + 2 * s, stopAll.bottom() + 4 * s, r.w - 4 * s, 18 * s};
    if (add.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 901), add, "+ Scene")) addScene();
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
        if (ui_.squareToggle(uiId(6, (int)ti, 0), mr, "M", &t.mute, pal::meterAmber))
            send(Cmd::TrackMute, (int)ti, t.mute ? 1 : 0);
        if (ui_.squareToggle(uiId(6, (int)ti, 1), sr, "S", &t.solo, pal::soloBlue))
            send(Cmd::TrackSolo, (int)ti, t.solo ? 1 : 0);
        // Record-arm is a filled dot in Live, and the glyph atlas is ASCII-only,
        // so draw the dot rather than trying to letter it.
        if (ui_.squareToggle(uiId(6, (int)ti, 2), ar, "", &t.arm, pal::armRed))
            send(Cmd::TrackArm, (int)ti, t.arm ? 1 : 0);
        rend_.circle(ar.cx(), ar.cy(), 3.5f * s, t.arm ? pal::textOnClip : pal::armRed);
        y += 20 * s;

        // Pan
        Rect pan{col.cx() - 11 * s, y, 22 * s, 22 * s};
        if (ui_.knob(uiId(6, (int)ti, 3), pan, &t.pan, -1.f, 1.f, 0.f))
            send(Cmd::TrackPan, (int)ti, 0, t.pan);
        y += 26 * s;

        // Fader + meter
        const f32 fh = col.bottom() - y - 6 * s;
        Rect fader{col.x + 10 * s, y, 16 * s, fh};
        Rect meter{fader.right() + 5 * s, y, 9 * s, fh};
        if (ui_.vFader(uiId(6, (int)ti, 4), fader, &t.fader))
            send(Cmd::TrackVol, (int)ti, 0, faderToGain(t.fader));

        const f32 lvl = std::max(engine_.meterL[ti].load(), engine_.meterR[ti].load());
        peakHoldT_[ti] = std::max(lvl, peakHoldT_[ti] * 0.985f);
        ui_.meterV(meter, lvl, peakHoldT_[ti]);
    }
    rend_.popClip();
}

void App::drawMasterStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::panelAlt);
    rend_.rect({r.x, r.y, 1 * s, r.h}, pal::divider);

    Rect head{r.x, r.y, r.w, lay::trackHeadH * s};
    rend_.rect(head, pal::panel);
    rend_.textIn(fBold_, head, "MASTER", pal::text, Align::Center);

    const f32 top = r.bottom() - lay::mixerH * s;
    Rect mix{r.x, top, r.w, lay::mixerH * s};
    rend_.rect({mix.x, mix.y, mix.w, 1 * s}, pal::divider);

    static f32 masterFader = 0.85f;
    f32 y = mix.y + 26 * s;
    const f32 fh = mix.bottom() - y - 6 * s;
    Rect fader{mix.x + 12 * s, y, 16 * s, fh};
    Rect meterL{fader.right() + 6 * s, y, 9 * s, fh};
    Rect meterR{meterL.right() + 3 * s, y, 9 * s, fh};

    if (ui_.vFader(uiId(7, 0), fader, &masterFader))
        send(Cmd::MasterVol, 0, 0, faderToGain(masterFader));

    const f32 l = engine_.masterMeterL.load(), rr = engine_.masterMeterR.load();
    peakHoldM_[0] = std::max(l, peakHoldM_[0] * 0.985f);
    peakHoldM_[1] = std::max(rr, peakHoldM_[1] * 0.985f);
    ui_.meterV(meterL, l, peakHoldM_[0]);
    ui_.meterV(meterR, rr, peakHoldM_[1]);
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

void App::drawClipDetail(const Rect& r) {
    const f32 s = win_.dpiScale();
    rend_.rect(r, pal::panel);
    rend_.rect({r.x, r.y, r.w, 1 * s}, pal::divider);

    ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
    if (!m.valid()) {
        rend_.textIn(fBody_, r, "No clip selected  —  drag a file from the browser onto a slot",
                     pal::textFaint, Align::Center);
        return;
    }

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

    {   // Warp mode
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("WARP", row);
        static const char* warpNames[] = {"Off", "Repitch", "Beats"};
        int wi = (int)m.warp;
        Rect sel{row.x + lblW, row.y, 84 * s, row.h};
        if (ui_.selector(uiId(8, 0), sel, &wi, warpNames, 3)) {
            m.warp = (Warp)wi;
            send(Cmd::ClipWarp, selTrack_, selSlot_, (f64)wi);
        }
        Rect lp{sel.right() + 6 * s, row.y, 52 * s, row.h};
        if (ui_.button(uiId(8, 1), lp, "LOOP", m.loop, pal::accent)) {
            m.loop = !m.loop;
            send(Cmd::ClipLoop, selTrack_, selSlot_, m.loop ? 1.0 : 0.0);
        }
        y += rowH + 4 * s;
    }
    {   // Clip tempo
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("CLIP BPM", row);
        f64 bpm = m.clipBpm;
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 2), dn, &bpm, 20.0, 400.0, 0.1, "%.2f")) {
            m.clipBpm = bpm;
            pushClip(selTrack_, selSlot_);
        }
        // Halve / double, exactly like Live's :2 and *2 buttons.
        Rect h2{dn.right() + 6 * s, row.y, 26 * s, row.h};
        Rect d2{h2.right() + 3 * s, row.y, 26 * s, row.h};
        if (ui_.button(uiId(8, 3), h2, ":2")) { m.clipBpm *= 0.5; pushClip(selTrack_, selSlot_); }
        if (ui_.button(uiId(8, 4), d2, "*2")) { m.clipBpm *= 2.0; pushClip(selTrack_, selSlot_); }
        y += rowH + 4 * s;
    }
    {   // Gain
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("GAIN", row);
        f64 db = gainToDb(m.gain);
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 5), dn, &db, -70.0, 12.0, 0.1, "%.1f dB")) {
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
            m.quantumIdx = qi - 1;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Read-out of what the engine will actually do
        Rect row{ctrl.x, y, ctrl.w, rowH};
        char buf[96];
        const f64 rate = (m.warp == Warp::Off) ? 1.0 : m.clipBpm / ses_.tempo;
        snprintf(buf, sizeof buf, "%.2f beats  ·  rate %.3fx  ·  %d ch",
                 m.lengthBeats, rate, m.sample->channels);
        rend_.textIn(fSmall_, row, buf, pal::textFaint, Align::Left, 0);
    }

    // --- waveform ---
    Rect wave{ctrl.right() + 12 * s, head.bottom() + 6 * s,
              r.right() - ctrl.right() - 20 * s, r.bottom() - head.bottom() - 12 * s};
    rend_.roundRect(wave, 2 * s, pal::appBg);
    rend_.pushClip(wave.inset(2 * s));
    drawWaveform(wave.inset(3 * s), *m.sample, ccol.scale(0.85f));

    // Playhead, when this clip is the one sounding on its track.
    if (engine_.activeSlot[selTrack_].load() == selSlot_) {
        const f64 ph = clampv(engine_.clipPhase[selTrack_].load(), 0.0, 1.0);
        const f32 px = wave.x + 3 * s + (wave.w - 6 * s) * (f32)ph;
        rend_.rect({px, wave.y + 2 * s, 1.5f * s, wave.h - 4 * s}, pal::playGreen);
    }
    rend_.popClip();
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
    rend_.textIn(fSmall_, r, status_.c_str(), pal::textFaint, Align::Left, 8 * s);

    char buf[160];
    snprintf(buf, sizeof buf, "%s · %s %.0f Hz / %d fr · %.0f fps · %d draws",
             win_.backendName(),
             audio_ ? audio_->name() : "silent",
             audio_ ? audio_->sampleRate() : 0.0,
             audio_ ? audio_->bufferSize() : 0,
             fps_, rend_.drawCalls());
    rend_.textIn(fSmall_, r, buf, pal::textFaint, Align::Right, 8 * s);
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
