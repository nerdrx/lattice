#include "project.h"
#include "../ui/app.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace lat {
namespace {

constexpr int kFormatVersion = 1;

// ---------------------------------------------------------------------------
// number formatting
// ---------------------------------------------------------------------------

// Shortest decimal that still reads back bit-identical. A fixed %.17g would
// round-trip too, but it litters the file with "0.850000024"; widening only
// until the value survives keeps the common cases readable while still making
// save -> load -> save byte-stable.
std::string fmtF64(f64 v) {
    if (!std::isfinite(v)) v = 0.0;
    char buf[64];
    for (int p = 6; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

std::string fmtF32(f32 v) {
    if (!std::isfinite(v)) v = 0.f;
    char buf[64];
    for (int p = 4; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, (f64)v);
        if ((f32)std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

// ---------------------------------------------------------------------------
// string escaping
// ---------------------------------------------------------------------------

// Only the characters that would break the line structure are escaped, so
// paths and names with spaces, quotes or unicode stay legible as-is.
std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:   o += c;      break;
        }
    }
    return o;
}

std::string unesc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        switch (s[++i]) {
        case 'n':  o += '\n';  break;
        case 'r':  o += '\r';  break;
        case 't':  o += '\t';  break;
        case '\\': o += '\\';  break;
        default:   o += s[i];  break;   // unknown escape: keep the character
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// clamps
// ---------------------------------------------------------------------------
//
// Applied symmetrically on save and on load. Clamping only on load would let
// an out-of-range value in memory change between the first and second save and
// break round-trip identity.

f64 clTempo(f64 t)      { return clampv(t, 20.0, 999.0); }
f64 clSceneTempo(f64 t) { return t <= 0.0 ? 0.0 : clampv(t, 20.0, 999.0); }
f64 clBpm(f64 t)        { return clampv(t, 1.0, 9999.0); }
f64 clBeats(f64 b)      { return clampv(b, 0.0, 1e7); }
int clSig(int v)        { return clampv(v, 1, 64); }
int clQuantum(int v)    { return clampv(v, 0, kQuantumCount - 1); }
int clClipQuantum(int v){ return clampv(v, -1, kQuantumCount - 1); }
int clColor(int v)      { return clampv(v, 0, 255); }
f32 clFader(f32 v)      { return std::isfinite(v) ? clampv(v, 0.f, 1.f) : 0.85f; }
f32 clPan(f32 v)        { return std::isfinite(v) ? clampv(v, -1.f, 1.f) : 0.f; }
f32 clGain(f32 v)       { return std::isfinite(v) ? clampv(v, 0.f, 8.f) : 1.f; }
f32 clWidth(f32 v)      { return std::isfinite(v) ? clampv(v, 24.f, 1024.f) : 94.f; }
int clWarp(int v)       { return clampv(v, (int)Warp::Off, (int)Warp::Beats); }
i64 clFrame(i64 v)      { return v < 0 ? 0 : v; }

// A slot counts as occupied if it holds audio or was given a name. There is no
// explicit "used" flag in ClipModel, and a clip whose file went missing must
// still survive a save/load cycle, so the name carries the occupancy.
bool clipOccupied(const ClipModel& c) { return c.sample != nullptr || !c.name.empty(); }

// Number of scene rows the file must describe: the model's own scene list,
// widened to cover any clip that lives below it. Writing the wider count is
// what makes the second save match the first.
size_t sceneRowCount(const Session& s) {
    size_t n = s.scenes.size();
    for (const auto& t : s.tracks)
        for (int i = 0; i < kMaxScenes; ++i)
            if (clipOccupied(t.slots[i]) && (size_t)i + 1 > n) n = (size_t)i + 1;
    return n;
}

std::string baseName(const std::string& p) {
    const size_t sl = p.find_last_of('/');
    std::string b = (sl == std::string::npos) ? p : p.substr(sl + 1);
    const size_t dot = b.find_last_of('.');
    if (dot != std::string::npos && dot > 0) b = b.substr(0, dot);
    return b;
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

// Emits "key value", or a bare "key" for the empty string. The bare form keeps
// trailing whitespace out of the file while still round-tripping empty names.
void kv(std::string& o, const char* indent, const char* key, const std::string& val) {
    o += indent;
    o += key;
    if (!val.empty()) { o += ' '; o += esc(val); }
    o += '\n';
}

void kn(std::string& o, const char* indent, const char* key, const std::string& num) {
    o += indent; o += key; o += ' '; o += num; o += '\n';
}

void writeClip(std::string& o, const ClipModel& c, int idx) {
    o += "  clip " + std::to_string(idx) + "\n";
    if (c.sample && !c.sample->path.empty()) kv(o, "    ", "file", c.sample->path);
    kv(o, "    ", "name", c.name);
    kn(o, "    ", "color",  std::to_string(clColor(c.colorIdx)));
    kn(o, "    ", "gain",   fmtF32(clGain(c.gain)));
    kn(o, "    ", "warp",   std::to_string(clWarp((int)c.warp)));
    kn(o, "    ", "loop",   c.loop ? "1" : "0");
    kn(o, "    ", "bpm",    fmtF64(clBpm(c.clipBpm)));
    kn(o, "    ", "beats",  fmtF64(clBeats(c.lengthBeats)));
    kn(o, "    ", "range",  std::to_string(clFrame(c.loopStart)) + " " +
                            std::to_string(clFrame(c.loopEnd)));
    kn(o, "    ", "quantum", std::to_string(clClipQuantum(c.quantumIdx)));
    o += "  endclip\n";
}

void writeTrack(std::string& o, const TrackModel& t, int idx) {
    o += "track " + std::to_string(idx) + "\n";
    kv(o, "  ", "name", t.name);
    kn(o, "  ", "color", std::to_string(clColor(t.colorIdx)));
    kn(o, "  ", "fader", fmtF32(clFader(t.fader)));
    kn(o, "  ", "pan",   fmtF32(clPan(t.pan)));
    kn(o, "  ", "flags", std::string(t.mute ? "1" : "0") + " " +
                         (t.solo ? "1" : "0") + " " + (t.arm ? "1" : "0"));
    kn(o, "  ", "width", fmtF32(clWidth(t.width)));
    for (int i = 0; i < kMaxScenes; ++i)
        if (clipOccupied(t.slots[i])) writeClip(o, t.slots[i], i);
    o += "endtrack\n";
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

// Walks the numeric tail of a line. Anything left over after the expected
// fields is ignored, which is what lets "flags 0 0 0   # mute solo arm" parse.
struct Scan {
    const char* p;
    explicit Scan(const std::string& s) : p(s.c_str()) {}
    bool num(f64& out) {
        char* e = nullptr;
        errno = 0;
        const double v = std::strtod(p, &e);
        if (e == p) return false;
        p = e;
        out = v;
        return true;
    }
    bool integer(i64& out) {
        char* e = nullptr;
        errno = 0;
        const long long v = std::strtoll(p, &e, 10);
        if (e == p) return false;
        p = e;
        out = (i64)v;
        return true;
    }
    bool integer(int& out) { i64 v; if (!integer(v)) return false; out = (int)clampv(v, (i64)INT32_MIN, (i64)INT32_MAX); return true; }
};

enum class St { Top, Track, Clip, Scene };

bool readWholeFile(const std::string& path, std::string& out, std::string* err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        if (err) *err = "read error on " + path;
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

bool saveProject(const Session& s, const std::string& path, std::string* err) {
    std::string o;
    o.reserve(4096);

    o += "lattice " + std::to_string(kFormatVersion) + "\n";
    kn(o, "", "tempo",     fmtF64(clTempo(s.tempo)));
    kn(o, "", "sig",       std::to_string(clSig(s.sigNum)) + " " + std::to_string(clSig(s.sigDen)));
    kn(o, "", "quantum",   std::to_string(clQuantum(s.quantumIdx)));
    kn(o, "", "metronome", s.metronome ? "1" : "0");
    kv(o, "", "name", s.name);

    const size_t nTracks = std::min(s.tracks.size(), (size_t)kMaxTracks);
    for (size_t i = 0; i < nTracks; ++i) writeTrack(o, s.tracks[i], (int)i);

    const size_t nScenes = std::min(sceneRowCount(s), (size_t)kMaxScenes);
    const SceneModel deflt{};
    for (size_t i = 0; i < nScenes; ++i) {
        const SceneModel& sc = (i < s.scenes.size()) ? s.scenes[i] : deflt;
        o += "scene " + std::to_string(i) + "\n";
        kv(o, "  ", "name", sc.name);
        kn(o, "  ", "tempo", fmtF64(clSceneTempo(sc.tempo)));
        o += "endscene\n";
    }

    // Write-then-rename: a crash or a full disk leaves the old project intact.
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (err) *err = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    const size_t wrote = std::fwrite(o.data(), 1, o.size(), f);
    const bool ok = (wrote == o.size()) && (std::fflush(f) == 0);
    std::fclose(f);
    if (!ok) {
        std::remove(tmp.c_str());
        if (err) *err = "short write to " + tmp;
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        if (err) *err = "cannot replace " + path + ": " + std::strerror(errno);
        return false;
    }

    // The API takes the session by const reference, but "last saved location"
    // is bookkeeping about the save itself, so it is updated here rather than
    // making every caller remember to do it.
    const_cast<Session&>(s).path = path;
    LOGI("saved %zu tracks / %zu scenes -> %s", nTracks, nScenes, path.c_str());
    return true;
}

// ---------------------------------------------------------------------------

bool loadProject(Session& s, const std::string& path, f64 engineRate, std::string* err) {
    std::string text;
    if (!readWholeFile(path, text, err)) return false;

    // Built into a scratch session so a parse failure leaves the caller's
    // session exactly as it was.
    Session out;
    out.tracks.clear();
    out.scenes.clear();
    out.name.clear();

    int lineNo = 0;
    auto fail = [&](const std::string& m) {
        if (err) *err = path + ":" + std::to_string(lineNo) + ": " + m;
        return false;
    };

    St st = St::Top;
    bool sawHeader = false;
    int  ti = -1, ci = -1, sci = -1;
    std::string clipFile;
    bool clipSawRange = false, clipSawBpm = false, clipSawBeats = false;
    int  missing = 0;

    // Resolves the pending clip once its body has been read: the sample load is
    // deferred to `endclip` so `file` may appear in any order.
    auto finishClip = [&]() {
        if (ti < 0 || ci < 0) return;
        ClipModel& c = out.tracks[(size_t)ti].slots[ci];
        if (!clipFile.empty()) {
            c.sample = loadSample(clipFile, engineRate);
            if (!c.sample) {
                ++missing;
                LOGW("project: missing sample '%s' (slot %d/%d kept empty)", clipFile.c_str(), ti, ci);
                if (c.name.empty()) c.name = baseName(clipFile);
            } else {
                // Only fill in what the file did not state, so a project that
                // spells out every field re-saves byte-for-byte identically.
                if (c.name.empty())  c.name = c.sample->name;
                if (!clipSawBpm)     c.clipBpm = clBpm(c.sample->guessedBpm);
                if (!clipSawBeats)   c.lengthBeats = clBeats(c.sample->guessedBeats);
                if (!clipSawRange) { c.loopStart = 0; c.loopEnd = c.sample->frames; }
            }
        }
        if (c.name.empty() && c.sample == nullptr) {
            // Nothing identifies this slot; drop it rather than leave a ghost.
            c = ClipModel{};
        }
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        if (pos == text.size() && text.empty()) break;
        size_t nl = text.find('\n', pos);
        const bool last = (nl == std::string::npos);
        std::string line = text.substr(pos, last ? std::string::npos : nl - pos);
        pos = last ? text.size() + 1 : nl + 1;
        ++lineNo;

        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Trim leading indentation only; a trailing space is part of a name.
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        if (b) line.erase(0, b);
        if (line.empty() || line[0] == '#') continue;

        size_t sp = line.find(' ');
        const std::string key = line.substr(0, sp);
        const std::string rest = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
        Scan sc(rest);

        if (!sawHeader) {
            if (key != "lattice") return fail("not a lattice project (expected 'lattice <version>')");
            int v = 0;
            if (!sc.integer(v)) return fail("missing format version");
            if (v != kFormatVersion) return fail("unsupported format version " + std::to_string(v));
            sawHeader = true;
            continue;
        }

        switch (st) {
        // ---------------------------------------------------------------
        case St::Top: {
            if (key == "tempo") {
                f64 v; if (!sc.num(v)) return fail("tempo: expected a number");
                out.tempo = clTempo(v);
            } else if (key == "sig") {
                int n = 0, d = 0;
                if (!sc.integer(n) || !sc.integer(d)) return fail("sig: expected two integers");
                out.sigNum = clSig(n); out.sigDen = clSig(d);
            } else if (key == "quantum") {
                int v; if (!sc.integer(v)) return fail("quantum: expected an integer");
                out.quantumIdx = clQuantum(v);
            } else if (key == "metronome") {
                int v; if (!sc.integer(v)) return fail("metronome: expected 0 or 1");
                out.metronome = v != 0;
            } else if (key == "name") {
                out.name = unesc(rest);
            } else if (key == "track") {
                int v; if (!sc.integer(v)) return fail("track: expected an index");
                if (v < 0 || v >= kMaxTracks)
                    return fail("track index " + std::to_string(v) + " out of range");
                if ((size_t)v >= out.tracks.size()) out.tracks.resize((size_t)v + 1);
                ti = v;
                st = St::Track;
            } else if (key == "scene") {
                int v; if (!sc.integer(v)) return fail("scene: expected an index");
                if (v < 0 || v >= kMaxScenes)
                    return fail("scene index " + std::to_string(v) + " out of range");
                if ((size_t)v >= out.scenes.size()) out.scenes.resize((size_t)v + 1);
                sci = v;
                st = St::Scene;
            } else {
                return fail("unexpected '" + key + "' at top level");
            }
            break;
        }
        // ---------------------------------------------------------------
        case St::Track: {
            TrackModel& t = out.tracks[(size_t)ti];
            if (key == "name") {
                t.name = unesc(rest);
            } else if (key == "color") {
                int v; if (!sc.integer(v)) return fail("color: expected an integer");
                t.colorIdx = clColor(v);
            } else if (key == "fader") {
                f64 v; if (!sc.num(v)) return fail("fader: expected a number");
                t.fader = clFader((f32)v);
            } else if (key == "pan") {
                f64 v; if (!sc.num(v)) return fail("pan: expected a number");
                t.pan = clPan((f32)v);
            } else if (key == "flags") {
                int m = 0, so = 0, a = 0;
                if (!sc.integer(m) || !sc.integer(so) || !sc.integer(a))
                    return fail("flags: expected three integers (mute solo arm)");
                t.mute = m != 0; t.solo = so != 0; t.arm = a != 0;
            } else if (key == "width") {
                f64 v; if (!sc.num(v)) return fail("width: expected a number");
                t.width = clWidth((f32)v);
            } else if (key == "clip") {
                int v; if (!sc.integer(v)) return fail("clip: expected an index");
                if (v < 0 || v >= kMaxScenes)
                    return fail("clip index " + std::to_string(v) + " out of range");
                ci = v;
                t.slots[ci] = ClipModel{};
                clipFile.clear();
                clipSawRange = clipSawBpm = clipSawBeats = false;
                st = St::Clip;
            } else if (key == "endtrack") {
                ti = -1;
                st = St::Top;
            } else {
                return fail("unexpected '" + key + "' inside track");
            }
            break;
        }
        // ---------------------------------------------------------------
        case St::Clip: {
            ClipModel& c = out.tracks[(size_t)ti].slots[ci];
            if (key == "file") {
                clipFile = unesc(rest);
            } else if (key == "name") {
                c.name = unesc(rest);
            } else if (key == "color") {
                int v; if (!sc.integer(v)) return fail("clip color: expected an integer");
                c.colorIdx = clColor(v);
            } else if (key == "gain") {
                f64 v; if (!sc.num(v)) return fail("clip gain: expected a number");
                c.gain = clGain((f32)v);
            } else if (key == "warp") {
                int v; if (!sc.integer(v)) return fail("clip warp: expected an integer");
                c.warp = (Warp)clWarp(v);
            } else if (key == "loop") {
                int v; if (!sc.integer(v)) return fail("clip loop: expected 0 or 1");
                c.loop = v != 0;
            } else if (key == "bpm") {
                f64 v; if (!sc.num(v)) return fail("clip bpm: expected a number");
                c.clipBpm = clBpm(v); clipSawBpm = true;
            } else if (key == "beats") {
                f64 v; if (!sc.num(v)) return fail("clip beats: expected a number");
                c.lengthBeats = clBeats(v); clipSawBeats = true;
            } else if (key == "range") {
                i64 a = 0, e = 0;
                if (!sc.integer(a) || !sc.integer(e))
                    return fail("range: expected two frame counts");
                c.loopStart = clFrame(a); c.loopEnd = clFrame(e); clipSawRange = true;
            } else if (key == "quantum") {
                int v; if (!sc.integer(v)) return fail("clip quantum: expected an integer");
                c.quantumIdx = clClipQuantum(v);
            } else if (key == "endclip") {
                finishClip();
                ci = -1;
                st = St::Track;
            } else {
                return fail("unexpected '" + key + "' inside clip");
            }
            break;
        }
        // ---------------------------------------------------------------
        case St::Scene: {
            SceneModel& scn = out.scenes[(size_t)sci];
            if (key == "name") {
                scn.name = unesc(rest);
            } else if (key == "tempo") {
                f64 v; if (!sc.num(v)) return fail("scene tempo: expected a number");
                scn.tempo = clSceneTempo(v);
            } else if (key == "endscene") {
                sci = -1;
                st = St::Top;
            } else {
                return fail("unexpected '" + key + "' inside scene");
            }
            break;
        }
        }
    }

    if (!sawHeader) return fail("empty or truncated project file");
    if (st != St::Top) {
        const char* what = (st == St::Clip) ? "endclip" : (st == St::Track) ? "endtrack" : "endscene";
        return fail(std::string("unexpected end of file, missing '") + what + "'");
    }

    // A clip below the last declared scene would be unreachable in the grid,
    // and saving would then re-widen the scene list; widen it here instead so
    // the model and the file agree.
    size_t need = out.scenes.size();
    for (const auto& t : out.tracks)
        for (int i = 0; i < kMaxScenes; ++i)
            if (clipOccupied(t.slots[i]) && (size_t)i + 1 > need) need = (size_t)i + 1;
    if (need > out.scenes.size()) out.scenes.resize(need);

    out.path = path;
    s = std::move(out);
    LOGI("loaded %zu tracks / %zu scenes from %s%s", s.tracks.size(), s.scenes.size(),
         path.c_str(), missing ? " (some samples missing)" : "");
    return true;
}

} // namespace lat
