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
#include <vector>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// Canonical parameter addresses — docs/PARAM-ADDRESS.md, docs/AUTOMATION.md §4.1
//
// The design puts this in src/core/address.{h,cpp} so that MIDI-learn, OSC and
// the undo system can reach it too. It lives here for now because automation is
// its first and only consumer and src/core is owned elsewhere this wave; the
// names are inside `addr` precisely so a later src/core/address.h declaring
// lat::parseAddress / lat::formatAddress can land beside it and this can be
// deleted in one edit, with no collision in between.
//
//   address   := scope ( "/" segment )*
//   scope     := "master" | "t:" UID | "s:" UID
//   segment   := "vol" | "pan" | "mute" | "solo" | "arm" | "send:" INDEX
//              | "dev:" UID "/p:" PARAMID
//              | "clip:" UID "/" clipfield
//
// `send:` is the one addition AUTOMATION.md §4.1 asks for (indexed, because the
// return buses are a fixed array). It is spelled as a Field of its own rather
// than as the doc's reserved `sendIndex` on an otherwise fieldless address, so
// that a switch over Field is exhaustive and a send can never be mistaken for a
// scope-only address.
//
// format(parse(x)) == x for every well-formed x: the project format writes back
// the string it was given, so a normalizing round trip would break byte-stable
// save / load / save.
// ---------------------------------------------------------------------------
namespace addr {

struct Parsed {
    enum class Scope { Master, Track, Scene } scope = Scope::Master;
    u64 scopeUid = 0;                 // track or scene uid; 0 for master
    enum class Field {
        None, Vol, Pan, Mute, Solo, Arm,
        Send,                         // sendIndex
        DeviceParam,                  // devUid + paramId
        ClipField,                    // clipUid + clipField
        SceneLaunch
    } field = Field::None;
    u64 devUid = 0;   u32 paramId = 0;
    u64 clipUid = 0;  int clipField = 0;
    int sendIndex = -1;
};

// Indexed by Parsed::clipField, and the order is the grammar's.
inline const char* const kClipFields[] = {"gain", "prob", "follow", "followBeats", "warp", "loop"};
inline constexpr int kClipFieldCount = 6;

// Strict decimal u64: no sign, no space, no empty string, no overflow. A lenient
// parse here would make two different texts mean the same address, which is the
// one thing the round-trip property cannot survive.
inline bool parseUid(const char* b, const char* e, u64& out) {
    if (b == e) return false;
    u64 v = 0;
    for (const char* p = b; p != e; ++p) {
        if (*p < '0' || *p > '9') return false;
        const u64 d = (u64)(*p - '0');
        if (v > (~0ull - d) / 10ull) return false;
        v = v * 10ull + d;
    }
    out = v;
    return true;
}

// True on a well-formed address. False means malformed *structure* — the caller
// treats that as "no lane" and, at load time, as a failed parse (§7.2); it never
// means "names something that is not here", which is a resolution result and is
// deliberately not this function's business.
inline bool parse(const std::string& s, Parsed& out) {
    out = Parsed{};
    std::vector<std::string> seg;
    for (size_t i = 0;;) {
        const size_t j = s.find('/', i);
        seg.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    for (const std::string& t : seg) if (t.empty()) return false;

    const std::string& s0 = seg[0];
    if (s0 == "master") {
        out.scope = Parsed::Scope::Master;
    } else if (s0.compare(0, 2, "t:") == 0) {
        out.scope = Parsed::Scope::Track;
        if (!parseUid(s0.data() + 2, s0.data() + s0.size(), out.scopeUid)) return false;
    } else if (s0.compare(0, 2, "s:") == 0) {
        out.scope = Parsed::Scope::Scene;
        if (!parseUid(s0.data() + 2, s0.data() + s0.size(), out.scopeUid)) return false;
    } else {
        return false;
    }

    if (seg.size() == 2) {
        const std::string& f = seg[1];
        if      (f == "vol")  out.field = Parsed::Field::Vol;
        else if (f == "pan")  out.field = Parsed::Field::Pan;
        else if (f == "mute") out.field = Parsed::Field::Mute;
        else if (f == "solo") out.field = Parsed::Field::Solo;
        else if (f == "arm")  out.field = Parsed::Field::Arm;
        else if (f == "launch" && out.scope == Parsed::Scope::Scene)
            out.field = Parsed::Field::SceneLaunch;
        else if (f.compare(0, 5, "send:") == 0) {
            u64 v = 0;
            if (!parseUid(f.data() + 5, f.data() + f.size(), v) || v > 63) return false;
            out.field = Parsed::Field::Send;
            out.sendIndex = (int)v;
        } else return false;
        return true;
    }
    if (seg.size() == 3) {
        if (seg[1].compare(0, 4, "dev:") == 0 && seg[2].compare(0, 2, "p:") == 0) {
            u64 pid = 0;
            if (!parseUid(seg[1].data() + 4, seg[1].data() + seg[1].size(), out.devUid)) return false;
            if (!parseUid(seg[2].data() + 2, seg[2].data() + seg[2].size(), pid)) return false;
            if (pid > 0xffffffffull) return false;
            out.field = Parsed::Field::DeviceParam;
            out.paramId = (u32)pid;
            return true;
        }
        if (seg[1].compare(0, 5, "clip:") == 0) {
            if (!parseUid(seg[1].data() + 5, seg[1].data() + seg[1].size(), out.clipUid)) return false;
            for (int k = 0; k < kClipFieldCount; ++k) {
                if (seg[2] != kClipFields[k]) continue;
                out.field = Parsed::Field::ClipField;
                out.clipField = k;
                return true;
            }
        }
        return false;
    }
    // A scope on its own names no value, and four segments name nothing at all.
    return false;
}

inline std::string format(const Parsed& p) {
    char buf[48];
    std::string s;
    switch (p.scope) {
    case Parsed::Scope::Master: s = "master"; break;
    case Parsed::Scope::Track:
        snprintf(buf, sizeof buf, "t:%llu", (unsigned long long)p.scopeUid); s = buf; break;
    case Parsed::Scope::Scene:
        snprintf(buf, sizeof buf, "s:%llu", (unsigned long long)p.scopeUid); s = buf; break;
    }
    switch (p.field) {
    case Parsed::Field::None: break;               // parse() never produces one
    case Parsed::Field::Vol:  s += "/vol";  break;
    case Parsed::Field::Pan:  s += "/pan";  break;
    case Parsed::Field::Mute: s += "/mute"; break;
    case Parsed::Field::Solo: s += "/solo"; break;
    case Parsed::Field::Arm:  s += "/arm";  break;
    case Parsed::Field::SceneLaunch: s += "/launch"; break;
    case Parsed::Field::Send:
        snprintf(buf, sizeof buf, "/send:%d", p.sendIndex); s += buf; break;
    case Parsed::Field::DeviceParam:
        snprintf(buf, sizeof buf, "/dev:%llu/p:%u", (unsigned long long)p.devUid, p.paramId);
        s += buf; break;
    case Parsed::Field::ClipField:
        snprintf(buf, sizeof buf, "/clip:%llu/", (unsigned long long)p.clipUid);
        s += buf;
        s += kClipFields[clampv(p.clipField, 0, kClipFieldCount - 1)];
        break;
    }
    return s;
}

// The three an automation lane can actually name, spelled once so the editor,
// the recorder and the self-test cannot disagree about a separator.
inline std::string trackField(u64 trackUid, const char* field) {
    Parsed p;
    p.scope = Parsed::Scope::Track;
    p.scopeUid = trackUid;
    if      (!strcmp(field, "vol")) p.field = Parsed::Field::Vol;
    else if (!strcmp(field, "pan")) p.field = Parsed::Field::Pan;
    else if (!strcmp(field, "mute")) p.field = Parsed::Field::Mute;
    else if (!strcmp(field, "solo")) p.field = Parsed::Field::Solo;
    else                             p.field = Parsed::Field::Arm;
    return format(p);
}

inline std::string trackSend(u64 trackUid, int idx) {
    Parsed p;
    p.scope = Parsed::Scope::Track;
    p.scopeUid = trackUid;
    p.field = Parsed::Field::Send;
    p.sendIndex = idx;
    return format(p);
}

inline std::string deviceParam(u64 trackUid, u64 devUid, u32 paramId) {
    Parsed p;
    p.scope = Parsed::Scope::Track;
    p.scopeUid = trackUid;
    p.field = Parsed::Field::DeviceParam;
    p.devUid = devUid;
    p.paramId = paramId;
    return format(p);
}

} // namespace addr

} // namespace lat
