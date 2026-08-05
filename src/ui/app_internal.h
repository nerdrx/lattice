// Internal shared bits for the app_*.cpp translation units: the file-scope
// helpers and constants that used to sit at the top of app.cpp, promoted to a
// header now the shell is split across eight TUs. None of this is part of
// class App; it is the glue the draw and model halves share.
//
// Everything is `inline`, not `static`: the eight TUs must see ONE
// kReturnLetter and ONE nowSeconds(), not eight private copies. Included only
// by the app_*.cpp files — never by app.h or session.h.
#pragma once
#include "app.h"
#include <chrono>
#include <string>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>

namespace lat {

// Layout constants, in logical px before the DPI scale is applied.
namespace lay {
inline constexpr f32 controlBarH = 38.f;
inline constexpr f32 statusH     = 20.f;
inline constexpr f32 trackHeadH  = 21.f;
inline constexpr f32 slotH       = 21.f;
inline constexpr f32 sceneColW   = 96.f;
inline constexpr f32 masterW     = 92.f;
// A return bus has no clips and no M/S/arm, so its strip is barely wider than
// a fader and a meter side by side.
inline constexpr f32 returnW     = 54.f;
// Tall enough for the M/S/arm row, the 2x2 send grid, pan, and a fader with
// enough travel left to mix with. The clip grid gives up the difference and
// still shows twice the scenes a default set has.
inline constexpr f32 mixerH      = 186.f;
inline constexpr f32 gutter      = 1.f;
}

// Every uiId kind in the app. Adding a widget family means adding a line
// HERE — the ids are hashed, so a duplicate kind is silent misbehaviour and
// not a compile error. This replaces the old "listed at its call site"
// convention, which could not survive the call sites landing in eight files.
enum UiKind : int {
    UiControlBar = 1, UiFileBrowser, UiTrackHead, UiClipGrid, UiSceneCol,
    UiMixer, UiMasterStrip, UiClipDetail, UiDetailTab, UiPluginBrowser,
    UiDeviceStrip, UiParamKnob, UiReturnStrip, UiUnused14, UiArrowGesture,
    UiTempo, UiKindCount
};

// The return buses, as the UI says them. Letters for the strips and the send
// knobs; the undo labels are spelled out because that is what the status bar
// reads back after an undo.
inline const char* const kReturnLetter[kMaxReturns] = {"A", "B", "C", "D"};
inline const char* const kSendUndo[kMaxReturns] = {"send A", "send B", "send C", "send D"};
static_assert(kMaxReturns == 4, "the return strips are lettered A-D by hand");
// ReturnModel's default name. A bus still wearing it has not been named, and
// the strip shows its letter instead; the project format leans on the same
// value to decide a return is worth writing at all.
inline const char* const kReturnPlaceholder = "Return";

// The undo gesture the auto-repeating arrow keys hold while a note is being
// nudged. Widget gestures are identified by the widget's own id, so this only
// has to avoid colliding with one: every uiId `kind` in use is listed at its
// call site, and 15 is not one of them.
inline const u64 kArrowGesture = uiId(15, 0);

inline f64 nowSeconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

inline std::string homeDir() {
    if (const char* h = getenv("HOME")) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

inline bool isAudioFile(const std::string& n) {
    static const char* ext[] = {".wav", ".flac", ".aiff", ".aif", ".ogg", ".mp3", ".opus", ".w64", nullptr};
    const size_t dot = n.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = n.substr(dot);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    for (int i = 0; ext[i]; ++i) if (e == ext[i]) return true;
    return false;
}

// Case-insensitive substring test. Used by the plugin filter and by the
// NXTAKT_DEBUG_ADDFX hook, both of which match on what the user typed rather
// than on an exact name.
inline bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    const size_t n = hay.size() - needle.size();
    for (size_t i = 0; i <= n; ++i)
        if (strncasecmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0) return true;
    return false;
}

} // namespace lat
