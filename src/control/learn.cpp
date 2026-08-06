// MIDI-learn: the mapping table. See learn.h for the design, the units and the
// reason the persistence file is not the project.
#include "learn.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

namespace lat {
namespace ctl {
namespace {

// ---------------------------------------------------------------------------
// clamps. Applied symmetrically on read and on write, exactly as project.cpp
// does: clamping only on read would let an out-of-range value in memory change
// between the first and second save and break round-trip identity.
// ---------------------------------------------------------------------------
f32 clNorm(f32 v) { return std::isfinite(v) ? clampv(v, 0.f, 1.f) : 0.f; }
u8  clChannel(int v) { return (u8)clampv(v, 0, 15); }
u8  clData1(int v)   { return (u8)clampv(v, 0, 127); }

// Shortest decimal that still reads back bit-identical, the same trick and the
// same reason as project.cpp's fmtF32: a flat %.9g would litter the file with
// "0.850000024" while a flat %.4g would not round-trip.
std::string fmtF32(f32 v) {
    if (!std::isfinite(v)) v = 0.f;
    char buf[48];
    for (int p = 4; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, (f64)v);
        if ((f32)std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

const char* modeWord(Mode m, Rel r) {
    switch (m) {
    case Mode::Toggle:   return "toggle";
    case Mode::Relative: return r == Rel::TwosComp ? "relc2" : r == Rel::Offset64 ? "rel64" : "rel";
    case Mode::Absolute: break;
    }
    return "abs";
}

bool parseModeWord(const std::string& w, Mode& m, Rel& r) {
    r = Rel::Auto;
    if (w == "abs")    { m = Mode::Absolute; return true; }
    if (w == "toggle") { m = Mode::Toggle;   return true; }
    if (w == "rel")    { m = Mode::Relative; return true; }
    if (w == "relc2")  { m = Mode::Relative; r = Rel::TwosComp; return true; }
    if (w == "rel64")  { m = Mode::Relative; r = Rel::Offset64; return true; }
    return false;
}

// Strict non-negative decimal int. A lenient parse ("74abc", " 74", "+74")
// would make two different texts mean the same record, which is the one thing
// the round-trip property cannot survive.
bool parseInt(const std::string& s, int& out) {
    if (s.empty() || s.size() > 9) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// Same strictness for the limits: whatever strtof accepts must be ALL of the
// token, and it must be finite. "0.5x", "nan" and "1e999" are structure errors.
bool parseF32(const std::string& s, f32& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0') return false;
    if (!std::isfinite(d)) return false;
    out = (f32)d;
    return std::isfinite(out);
}

void splitWs(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        const size_t b = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        out.push_back(line.substr(b, i - b));
    }
}

// --- the reader-thread tap ring -------------------------------------------
// File-scope rather than a member of anything: the producer is the ALSA reader
// thread, which is handed an Engine& and nothing else, and threading a MidiMap
// pointer down to it would mean owning its lifetime across a thread boundary
// for no gain. A ring of 512 is four seconds of a controller sweep at the rate
// a human can generate.
Ring<MidiMsg, 512>  g_tap;
std::atomic<u64>    g_tapCount{0};
std::atomic<u64>    g_tapDropped{0};

} // namespace

void midiTap(const MidiMsg& m) {
    if (g_tap.push(m)) g_tapCount.fetch_add(1, std::memory_order_relaxed);
    else               g_tapDropped.fetch_add(1, std::memory_order_relaxed);
}
bool midiTapPop(MidiMsg& out) { return g_tap.pop(out); }
u64  midiTapCount()   { return g_tapCount.load(std::memory_order_relaxed); }
u64  midiTapDropped() { return g_tapDropped.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// addresses
// ---------------------------------------------------------------------------

bool lexicalAddressOk(const std::string& a) {
    if (a.empty() || a.size() > 128) return false;
    // "Addresses are ASCII, no spaces" (PARAM-ADDRESS.md) — which is also what
    // keeps them safe as an OSC path and as the tail of a line in this file.
    for (unsigned char c : a) if (c <= 0x20 || c >= 0x7f) return false;
    if (a.front() == '/' || a.back() == '/') return false;
    for (size_t i = 1; i < a.size(); ++i) if (a[i] == '/' && a[i - 1] == '/') return false;
    return true;
}

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

int MidiMap::findAddress(const std::string& address) const {
    for (size_t i = 0; i < bindings_.size(); ++i)
        if (bindings_[i].address == address) return (int)i;
    return -1;
}

int MidiMap::findTrigger(u8 status, u8 channel, u8 data1) const {
    for (size_t i = 0; i < bindings_.size(); ++i) {
        const Binding& b = bindings_[i];
        if (b.status == status && b.channel == channel && b.data1 == data1) return (int)i;
    }
    return -1;
}

int MidiMap::bind(const Binding& in) {
    if (!addressOk(in.address)) return -1;
    if (in.status != 0xB0 && in.status != 0x90) return -1;

    Binding b = in;
    b.channel = clChannel(b.channel);
    b.data1   = clData1(b.data1);
    b.lo = clNorm(b.lo);
    b.hi = clNorm(b.hi);
    b.rel_seen = b.rel;               // a configured convention needs no detecting
    b.hits = 0;

    // Evict both collisions before inserting, and in this order: the address
    // first, so re-learning the same control for the same parameter does not
    // depend on which of the two lookups fires.
    for (size_t i = bindings_.size(); i-- > 0;) {
        const Binding& e = bindings_[i];
        if (e.address == b.address ||
            (e.status == b.status && e.channel == b.channel && e.data1 == b.data1))
            bindings_.erase(bindings_.begin() + (long)i);
    }
    if ((int)bindings_.size() >= kMaxBindings) return -1;

    bindings_.push_back(std::move(b));
    dirty_ = true;
    return (int)bindings_.size() - 1;
}

bool MidiMap::unbindAddress(const std::string& address) {
    const int i = findAddress(address);
    if (i < 0) return false;
    bindings_.erase(bindings_.begin() + i);
    dirty_ = true;
    return true;
}

void MidiMap::beginLearn(const std::string& address, Mode mode) {
    if (!addressOk(address)) { cancelLearn(); return; }
    learning_ = true;
    learnAddress_ = address;
    learnMode_ = mode;
}

// ---------------------------------------------------------------------------
// matching
// ---------------------------------------------------------------------------
namespace {

// "Looks like a control", for the learn state machine. A control change or a
// note-on, and nothing else: note-OFF would learn the release of the very key
// that was meant to learn the press, and the channel-mode messages (CC 120-127
// are all-sound-off, all-notes-off, omni and mono/poly) are panic buttons a
// controller emits on its own — binding a fader to one of those would produce a
// mapping the user never touched and cannot explain.
bool learnable(const MidiMsg& m) {
    const u8 kind = (u8)(m.status & 0xF0);
    if (kind == 0x90) return m.d2 > 0;
    if (kind == 0xB0) return m.d1 < 120;
    return false;
}

// One encoder detent -> a signed count, under whichever convention applies.
// Returns false when the value carries no movement at all.
bool relDelta(Binding& b, u8 v, int& out) {
    Rel r = b.rel != Rel::Auto ? b.rel : b.rel_seen;
    if (r == Rel::Auto) {
        // The disjoint slow-turn values, and only those: anything else is a
        // fast spin, which both conventions spell ambiguously.
        if (v == 1 || v == 127)     r = Rel::TwosComp;
        else if (v == 63 || v == 65) r = Rel::Offset64;
        if (r != Rel::Auto) b.rel_seen = r;
    }
    if (r == Rel::Auto) return false;      // still undecided; wait for a detent
    out = (r == Rel::Offset64) ? (int)v - 64
                               : (v < 64 ? (int)v : (int)v - 128);
    return out != 0;
}

} // namespace

std::optional<Hit> MidiMap::consume(const MidiMsg& m, bool* learned) {
    if (learned) *learned = false;

    if (learning_) {
        if (!learnable(m)) return std::nullopt;
        Binding b;
        b.status  = (u8)(m.status & 0xF0);
        b.channel = (u8)(m.status & 0x0F);
        b.data1   = m.d1;
        b.address = learnAddress_;
        // A key has no travel, so it can only ever be a toggle however the
        // caller asked; a fader asked to be a toggle is honoured, because a
        // toggle on a fader ("past halfway = on") is a real thing people want.
        b.mode = b.isNote() ? Mode::Toggle : learnMode_;
        b.lo = 0.f; b.hi = 1.f;
        b.rel = Rel::Auto;
        const int idx = bind(b);
        cancelLearn();
        if (idx >= 0 && learned) *learned = true;
        return std::nullopt;               // the message was spent learning
    }

    for (size_t i = 0; i < bindings_.size(); ++i) {
        Binding& b = bindings_[i];
        if (!b.matches(m)) continue;

        Hit h;
        h.index = i;
        h.address = b.address;
        h.lo = b.lo; h.hi = b.hi;
        h.gesture = b.gesture();

        switch (b.mode) {
        case Mode::Absolute:
            h.act = Hit::Act::Set;
            h.norm = b.lo + (b.hi - b.lo) * ((f32)m.d2 / 127.f);
            break;
        case Mode::Relative: {
            int d = 0;
            if (!relDelta(b, m.d2, d)) return std::nullopt;
            h.act = Hit::Act::Nudge;
            // One detent = 1/127th of the mapped span. The sign follows
            // (hi - lo), so an inverted binding inverts the direction too,
            // which is the only reading of "inverted" that is not a surprise.
            h.norm = (f32)d * (b.hi - b.lo) / 127.f;
            break;
        }
        case Mode::Toggle:
            // Buttons send a press and a release; only the press flips. A pad
            // in note mode never reaches here on its note-off, because the
            // binding's status is 0x90 and a release is 0x80.
            if (b.isCC() && m.d2 < 64) return std::nullopt;
            h.act = Hit::Act::Toggle;
            break;
        }
        ++b.hits;
        return h;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

std::string MidiMap::serialize() const {
    std::string out = "nxtakt-midimap ";
    out += std::to_string(kVersion);
    out += "\n";
    for (const Binding& b : bindings_) {
        out += "bind ";
        out += b.isNote() ? "note" : "cc";
        out += " ";
        out += std::to_string((int)clChannel(b.channel));
        out += " ";
        out += std::to_string((int)clData1(b.data1));
        out += " ";
        out += modeWord(b.mode, b.rel);
        out += " ";
        out += fmtF32(clNorm(b.lo));
        out += " ";
        out += fmtF32(clNorm(b.hi));
        out += " ";
        out += b.address;
        out += "\n";
    }
    return out;
}

bool MidiMap::parse(const std::string& text, std::string* err) {
    std::vector<Binding> built;
    int lineNo = 0;
    auto fail = [&](const char* what) {
        if (err) *err = std::to_string(lineNo) + ": " + what;
        return false;
    };

    bool sawHeader = false;
    std::vector<std::string> t;
    size_t pos = 0;
    while (pos <= text.size()) {
        if (pos == text.size() && text.empty()) break;
        const size_t nl = text.find('\n', pos);
        const bool last = (nl == std::string::npos);
        std::string line = text.substr(pos, last ? std::string::npos : nl - pos);
        pos = last ? text.size() + 1 : nl + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        splitWs(line, t);
        // Blank lines and comments are accepted so the file stays hand-editable
        // — it is a machine's configuration, not a document — but they are NOT
        // preserved: the round-trip guarantee is that anything THIS program
        // wrote reloads and re-serialises byte-identically.
        if (t.empty() || t[0][0] == '#') continue;

        if (!sawHeader) {
            if (t.size() != 2 || t[0] != "nxtakt-midimap") return fail("not a midimap file");
            int v = 0;
            if (!parseInt(t[1], v)) return fail("bad version");
            if (v < 1 || v > kVersion) return fail("unsupported version");
            sawHeader = true;
            continue;
        }

        if (t[0] != "bind") return fail("unknown record");
        if (t.size() != 8) return fail("bind takes 7 fields");

        Binding b;
        if      (t[1] == "cc")   b.status = 0xB0;
        else if (t[1] == "note") b.status = 0x90;
        else return fail("bind kind must be cc or note");

        int ch = 0, d1 = 0;
        if (!parseInt(t[2], ch) || ch > 15)  return fail("channel out of range");
        if (!parseInt(t[3], d1) || d1 > 127) return fail("data1 out of range");
        b.channel = (u8)ch;
        b.data1   = (u8)d1;

        if (!parseModeWord(t[4], b.mode, b.rel)) return fail("unknown mode");
        if (!parseF32(t[5], b.lo)) return fail("bad lo");
        if (!parseF32(t[6], b.hi)) return fail("bad hi");
        // VALUES are clamped where STRUCTURE is rejected: a limit of 2 is a
        // number in the wrong place, not a broken file.
        b.lo = clNorm(b.lo);
        b.hi = clNorm(b.hi);

        b.address = t[7];
        if (!addressOk(b.address)) return fail("malformed address");

        if ((int)built.size() >= kMaxBindings) return fail("too many bindings");
        // Last record wins, as bind() does, so a hand-edited duplicate cannot
        // produce a table that behaves differently from the one a re-save
        // would write.
        for (size_t i = built.size(); i-- > 0;) {
            const Binding& e = built[i];
            if (e.address == b.address ||
                (e.status == b.status && e.channel == b.channel && e.data1 == b.data1))
                built.erase(built.begin() + (long)i);
        }
        b.rel_seen = b.rel;
        built.push_back(std::move(b));
    }
    // An empty file is not "no bindings" — it is a file that says nothing at
    // all, which is what a truncated write leaves behind. A map with no
    // bindings still writes its header line, so the two are distinguishable and
    // the empty one is refused. (A file that is not there at all IS "no
    // bindings"; see load().)
    if (!sawHeader) {
        lineNo = 1;
        return fail("not a midimap file");
    }

    bindings_.swap(built);
    cancelLearn();
    dirty_ = false;
    return true;
}

bool MidiMap::load(const std::string& path, std::string* err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        // No file is not an error: a machine with no controller has no mapping,
        // and the very first save is what creates it.
        if (err) err->clear();
        bindings_.clear();
        dirty_ = false;
        return true;
    }
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) { if (err) *err = "read error"; return false; }
    if (!parse(text, err)) {
        if (err) *err = path + ":" + *err;
        return false;
    }
    return true;
}

bool MidiMap::save(const std::string& path, std::string* err) const {
    if (!ensureParentDir(path)) { if (err) *err = "could not create " + path; return false; }
    const std::string text = serialize();
    // Write-and-rename: a config truncated by a crash mid-write would come back
    // as "not a midimap file" and take the whole mapping with it.
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { if (err) *err = "could not open " + tmp; return false; }
    const size_t n = std::fwrite(text.data(), 1, text.size(), f);
    const bool ok = (n == text.size()) && (std::fflush(f) == 0);
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        if (err) *err = "could not write " + path;
        return false;
    }
    return true;
}

std::string defaultMapPath() {
    std::string base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h) base = std::string(h) + "/.config";
    else if (passwd* pw = getpwuid(getuid())) base = std::string(pw->pw_dir) + "/.config";
    else base = "/tmp";
    return base + "/nxtakt/midimap.conf";
}

bool ensureParentDir(const std::string& path) {
    const size_t sl = path.find_last_of('/');
    if (sl == std::string::npos || sl == 0) return true;
    const std::string dir = path.substr(0, sl);
    // mkdir -p, one component at a time. EEXIST is success.
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

} // namespace ctl
} // namespace lat
