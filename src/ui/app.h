// Session model (GUI-side truth) and the application shell.
//
// The model here is the editable, allocating, std::string-carrying version.
// Whenever something changes that the audio thread needs, it is pushed across
// as a Command; the engine keeps its own realtime-safe mirror.
#pragma once
#include "../audio/engine.h"
#include "../audio/sample.h"
#include "../audio/backend.h"
#include "../gfx/renderer.h"
#include "widgets.h"
#include "window.h"
#include <memory>
#include <string>
#include <vector>

namespace lat {

struct ClipModel {
    SampleRef sample;
    std::string name;
    int  colorIdx = 0;
    f32  gain = 1.0f;
    Warp warp = Warp::Beats;
    bool loop = true;
    f64  clipBpm = 120.0;
    f64  lengthBeats = 4.0;
    i64  loopStart = 0, loopEnd = 0;
    int  quantumIdx = -1;              // -1 => follow the global quantum
    bool valid() const { return sample != nullptr; }
};

struct TrackModel {
    std::string name = "Track";
    int   colorIdx = 0;
    ClipModel slots[kMaxScenes];
    f32   fader = 0.85f;               // 0..1, mapped through faderToGain
    f32   pan   = 0.f;                 // -1..1
    bool  mute = false, solo = false, arm = false;
    f32   width = 94.f;
};

struct SceneModel {
    std::string name;
    f64 tempo = 0.0;                   // 0 => no tempo change on launch
};

struct Session {
    std::vector<TrackModel> tracks;
    std::vector<SceneModel> scenes;
    f64  tempo = 120.0;
    int  sigNum = 4, sigDen = 4;
    int  quantumIdx = 4;               // index into kQuantumBeats -> "1 Bar"
    bool metronome = false;
    std::string name = "Untitled";
    std::string path;                  // last saved location, empty if never
};

enum class MainView { Session, Arrangement };

struct BrowserEntry {
    std::string name, path;
    bool isDir = false;
    bool isAudio = false;
};

// A drag in flight, either from the browser or between clip slots.
struct DragState {
    enum class Kind { None, BrowserFile, Clip } kind = Kind::None;
    std::string path;                  // BrowserFile
    int srcTrack = -1, srcSlot = -1;   // Clip
    f32 startX = 0, startY = 0;
    bool armed = false;                // past the movement threshold
};

class App {
public:
    bool init(int argc, char** argv);
    void run();
    void shutdown();

private:
    // --- frame ---
    void frame();
    void handleShortcuts();
    void pumpEngineEvents();

    // --- views ---
    void drawControlBar(const Rect& r);
    void drawBrowser(const Rect& r);
    void drawSessionView(const Rect& r);
    void drawTrackHeaders(const Rect& r, f32 scrollX);
    void drawClipGrid(const Rect& r, f32 scrollX);
    void drawSceneColumn(const Rect& r);
    void drawMixer(const Rect& r, f32 scrollX);
    void drawMasterStrip(const Rect& r);
    void drawClipDetail(const Rect& r);
    void drawArrangementView(const Rect& r);
    void drawStatusBar(const Rect& r);
    void drawDragGhost();

    // --- clip helpers ---
    void  drawClipSlot(const Rect& r, int track, int slot);
    void  drawWaveform(const Rect& r, const SampleBuffer& sb, const Col& c,
                       f64 t0 = 0.0, f64 t1 = 1.0);
    void  loadClipInto(int track, int slot, const std::string& path);
    void  clearClip(int track, int slot);
    void  pushClip(int track, int slot);          // sync one slot to the engine
    void  pushTrack(int track);                   // sync mixer state
    void  pushAll();
    void  addTrack();
    void  addScene();

    // --- transport helpers ---
    void  send(Cmd t, i32 a = 0, i32 b = 0, f64 x = 0.0);
    void  setTempo(f64 bpm);
    void  togglePlay();

    // --- browser ---
    void  refreshBrowser();
    void  browseTo(const std::string& dir);

    Window   win_;
    Renderer rend_;
    Ui       ui_{};
    Font     fSmall_, fBody_, fBold_, fBig_;
    Engine   engine_;
    std::unique_ptr<AudioBackend> audio_;

    Session  ses_;
    MainView view_ = MainView::Session;

    // selection + interaction
    int  selTrack_ = 0, selSlot_ = 0;
    bool running_ = true;
    DragState drag_{};
    f32  gridScrollX_ = 0.f;
    f32  browserW_ = 210.f;
    f32  detailH_ = 168.f;
    bool showBrowser_ = true;
    bool showDetail_ = true;

    // browser state
    std::string browserDir_;
    std::vector<BrowserEntry> browserItems_;
    std::vector<std::string> browserPlaces_;
    f32  browserScroll_ = 0.f;
    int  browserSel_ = -1;

    // per-frame UI feedback
    std::string status_;
    f32  peakHoldT_[kMaxTracks]{};
    f32  peakHoldM_[2]{};
    f64  lastFrameTime_ = 0.0;
    f32  fps_ = 0.f;
};

} // namespace lat
