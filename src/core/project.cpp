#include "project.h"
#include "../ui/app.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace lat {
namespace {

// Version 2 adds: `nextuid` at the top level, a `uid` line on every track,
// scene and clip, the clip's generative fields (prob / follow / followbeats),
// and `device` blocks inside a track.
//
// Version 3 adds MIDI clips: a `kind midi` line and zero or more `note` lines
// inside a clip block. An audio clip writes neither, so every set that has no
// MIDI in it saves exactly the bytes version 2 saved -- only the header line
// moves.
//
// Version 4 adds the mixer's bus topology: `send <idx> <level>` lines inside a
// track, `return <idx>` ... `endreturn` blocks and one `master` ... `endmaster`
// block between the tracks and the scenes. Every one of those constructs is
// emitted only when it carries something (see writeTrack's send loop,
// returnWorthWriting and writeMaster), so a set that uses none of them -- which
// is every set written before this version existed -- again saves exactly the
// bytes version 3 saved apart from the header line.
//
// Version 5 adds clip automation. A clip block gains, after its notes, zero or
// more `env <address>` ... `endenv` blocks:
//
//     env t:7/dev:12/p:3
//       pt 0 200
//       pt 2 4000
//     endenv
//
// with an optional `off` line marking a deactivated lane. Every part of it is
// sparse in the same way everything above is -- a clip with no envelopes writes
// nothing, a lane with no points is dropped, `off` appears only when the lane
// is disabled and the curve byte only when it is non-zero -- so a set that uses
// none of it saves exactly the bytes version 4 saved apart from the header
// line. See writeClip and validAddress for the details.
//
// There is deliberately ONE parser for all versions rather than a reader per
// version. The additions are all new keys with defaults, so an older file
// simply never mentions them and comes out with the defaults; the version
// number gates nothing on the read side beyond the upper bound. The
// alternative -- rejecting v3 keys inside a file that calls itself v1 -- would
// only punish someone who hand-edited the header, and buys no safety: an old
// build already refuses every one of those keys, so no half-understood file
// can be read either way. Saving always writes the current version.
//
// THE HEADER WORD IS THE MAGIC. There is no separate signature: the first
// token of the first line both identifies the format and carries the version.
// The product was called Lattice through versions 1..4, so that token was
// `lattice`; it is `nxtakt` from the rename on. Both spellings name the SAME
// format in the SAME version space (1..5 and counting) -- the rename did not
// fork the format, and a `nxtakt 4` file is byte-identical to the `lattice 4`
// file it replaced apart from that one word.
//
// So: the writer emits `nxtakt <version>`, and the reader accepts either word,
// forever. Not for a deprecation window -- forever. Every set a user has ever
// saved says `lattice`, those files are their work, and there is no upgrade
// step they could be expected to run. Dropping the old spelling would turn
// every one of them into "not a project file", which is the single worst thing
// a DAW can say about a file that is in fact perfectly readable.
//
// Deliberately NOT done: rewriting the caller's file on load, or keeping the
// word that was read so a re-save preserves it. Load-and-save flips the header
// to the new spelling, and that is the only thing it flips. See
// kHeaderWord/isHeaderWord below.
constexpr int kFormatVersion = 5;
constexpr int kMinFormatVersion = 1;

// What saveProject writes. Reading accepts this and every spelling in
// kLegacyHeaderWords.
constexpr const char* kHeaderWord = "nxtakt";
// Every word that has ever meant "this is a project file". Append-only: a
// spelling may never be removed from this list.
constexpr const char* kLegacyHeaderWords[] = { "lattice" };

bool isHeaderWord(const std::string& k) {
    if (k == kHeaderWord) return true;
    for (const char* w : kLegacyHeaderWords) if (k == w) return true;
    return false;
}

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
// A send level is a linear gain into a return bus, 0 meaning "off". Like the
// clip's generative fields it is written sparsely, so the non-finite arm has to
// fold to exactly the value that suppresses the line: a NaN send must not be
// written as "send 0 0" when the next save would omit it.
f32 clSend(f32 v)       { return std::isfinite(v) ? clampv(v, 0.f, 1.f) : 0.f; }
int clWarp(int v)       { return clampv(v, (int)Warp::Off, (int)Warp::Beats); }
i64 clFrame(i64 v)      { return v < 0 ? 0 : v; }
f32 clParam(f32 v)      { return std::isfinite(v) ? v : 0.f; }
int clFollow(int v)     { return clampv(v, (int)Follow::None, (int)Follow::Random); }
// The non-finite arms matter for more than tidiness: the sparse fields below
// are written only when they differ from their default, and the value tested
// has to be the value printed. Folding NaN to the default here is what stops a
// NaN probability from emitting "prob 1" -- a line the next save would omit.
f64 clProb(f64 v)       { return std::isfinite(v) ? clampv(v, 0.0, 1.0) : 1.0; }
f64 clFollowBeats(f64 v){ return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }
// The counter only ever hands out identifiers, so 0 (the "unassigned" marker)
// and anything below it are nonsense.
u64 clNextUid(u64 v)    { return v < 1 ? 1 : v; }

// Notes. The same reasoning as above applies: these are applied on save and on
// load, so a value the model holds out of range is written once, read back
// unchanged, and written again identically.
//
// A zero-length note is not a note -- it would sound for no frames and could
// never be grabbed again in the piano roll -- so the length has a floor rather
// than being clamped to 0. A 32nd is the smallest grid the editor offers.
constexpr f64 kMinNoteLen = 1.0 / 32.0;
f64 clNoteBeat(f64 v) { return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }
f64 clNoteLen(f64 v)  { return std::isfinite(v) ? clampv(v, kMinNoteLen, 1e7) : 0.25; }
// Both are u8 in the model, so only the top of the range can be violated in
// memory; the parameter is widened to i64 so a negative number in a file is
// caught here instead of wrapping. Velocity 0 is a note-off in MIDI, never a
// note, hence the floor of 1.
u8 clPitch(i64 v)     { return (u8)clampv(v, (i64)0, (i64)127); }
u8 clVel(i64 v)       { return (u8)clampv(v, (i64)1, (i64)127); }

// Automation breakpoints. Same symmetry, same reasoning.
//
// A breakpoint's value is in the TARGET'S OWN units (AUTOMATION.md §2.3) -- a
// fader position, a pan, a plugin parameter in whatever range that plugin
// declared -- so there is no range this layer could clamp it to that would not
// be wrong for some target. Only non-finite is folded, exactly as clParam does
// for a device parameter, and for the same reason: NaN is not a value, and the
// range check belongs to the publisher, which has the ParamInfo.
f64 clEnvBeat(f64 v)  { return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }
f32 clEnvValue(f32 v) { return std::isfinite(v) ? v : 0.f; }
// The curve byte is PRESERVED, not normalized. `curve` is reserved and every
// shape but 0 (linear) is unimplemented, so the tempting thing is to clamp it
// to 0 -- and that would be wrong. AUTOMATION.md §2.1 is explicit: "a reader
// that meets a non-zero curve renders it as linear and writes it back
// unchanged", which is what makes the byte a forward-compatibility slot rather
// than dead weight. A newer build writes `pt 0 1 3`; this build must load that
// set, draw the segment straight, and hand the 3 back on the next save instead
// of silently flattening somebody's ease curve into a file that can never say
// so again. So the clamp is to the field's own width and nothing more -- the
// parameter is widened to i64 so a negative in a file is caught here rather
// than wrapping, exactly as clPitch does.
u8 clCurve(i64 v)     { return (u8)clampv(v, (i64)0, (i64)255); }

// ---------------------------------------------------------------------------
// parameter addresses -- SHAPE ONLY
// ---------------------------------------------------------------------------
//
// An automation lane names its target by canonical address (docs/PARAM-ADDRESS.md,
// plus the `send:` segment AUTOMATION.md §4.1 adds). The reader checks that
// text against the grammar and NOTHING ELSE. It never asks whether track uid 7
// exists, whether device uid 12 is in the set, or whether the plugin declaring
// param 3 is installed on this machine -- resolution lives GUI-side, and
// PARAM-ADDRESS.md's "dangling addresses resolve to nothing and must fail soft"
// is the whole reason the address is stored as text in the first place.
//
// That gives the two halves of AUTOMATION.md §7.2, which is the same split
// `send <idx> <level>` already makes one screen up:
//
//   * Syntactically malformed -> the load FAILS. It is structure. There is no
//     right answer for what `t:7//vol` or `t:seven/vol` meant, and guessing
//     would silently move somebody's automation onto a different parameter.
//   * Syntactically valid but naming a uid nothing answers to -> KEPT, and
//     written back unchanged on the next save. Losing a filter sweep because
//     the set was opened on a machine without the plugin is the worst bug this
//     feature can have; it is the same promise ClipModel::path makes for a
//     missing sample and DeviceModel::lostParams for a missing plugin.
//
// Grammar, from PARAM-ADDRESS.md:
//
//     address   := scope ( "/" segment )?
//     scope     := "master" | "t:" UID | "s:" UID
//     segment   := "vol" | "pan" | "mute" | "solo" | "arm" | "launch"
//                | "send:" INDEX
//                | "dev:" UID "/p:" PARAMID
//                | "clip:" UID "/" clipfield
//     clipfield := gain | prob | follow | followBeats | warp | loop
//
// Three readings of that grammar are decisions rather than transcription, and
// all three lean the same way -- toward accepting -- because a false reject
// costs the user the whole file while a false accept costs a lane that
// resolves to nothing:
//
//   * The document writes the segment list as `( "/" segment )*`. It is read
//     here as "at most one": every alternative is already a complete leaf and
//     no two of them compose, so `t:7/vol/pan` is a typo, not a nesting.
//   * A bare scope (`master`, `t:7`) has zero segments and is ACCEPTED. It
//     names no field, so it resolves to nothing -- which is a resolution
//     outcome, handled by the rule above, not a shape error.
//   * `launch` appears in PARAM-ADDRESS.md only as the reserved `s:4/launch`
//     example and not in the segment list at all. It is accepted for any
//     scope: policing which fields belong to which scope is resolution, and
//     rejecting a reserved spelling that a future build emits would be exactly
//     the failure mode this function exists to avoid.
//
// Ranges are not checked either, for the same reason: `t:7/send:99` names a bus
// this build does not have, and that is a dangling address, not a broken one.
// (Contrast the track's own `send 99 0.5` line, which IS rejected -- there the
// index is a slot in a fixed array this file is writing into, so there is
// nowhere to put it. Here it is text that round-trips.)

// A decimal u64 with at least one digit and nothing else. Leading zeros are
// fine -- the text is written back verbatim, so `t:007` costs nothing -- but a
// value that is not representable is not a uid.
bool addrUid(const std::string& s) {
    if (s.empty() || s.size() > 20) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    errno = 0;
    (void)std::strtoull(s.c_str(), nullptr, 10);
    return errno != ERANGE;
}

// A decimal u32: ParamInfo::id and a send index are both that width.
bool addrIndex(const std::string& s) {
    if (s.empty() || s.size() > 10) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return std::strtoull(s.c_str(), nullptr, 10) <= (unsigned long long)UINT32_MAX;
}

bool validAddress(const std::string& a) {
    // "Addresses are ASCII, no spaces" -- which also keeps them safe as an OSC
    // path and as the tail of a line in this file. A control character or a
    // space here means the line was not an address at all.
    if (a.empty()) return false;
    for (unsigned char c : a) if (c <= 0x20 || c >= 0x7f) return false;

    std::vector<std::string> tok;
    for (size_t start = 0;;) {
        const size_t sl = a.find('/', start);
        tok.push_back(a.substr(start, sl == std::string::npos ? sl : sl - start));
        if (sl == std::string::npos) break;
        start = sl + 1;
    }
    // Catches "t:7/", "/vol" and "t:7//vol" in one line.
    for (const std::string& t : tok) if (t.empty()) return false;

    size_t i = 0;
    const std::string scope = tok[i++];
    if (scope != "master") {
        if (scope.compare(0, 2, "t:") != 0 && scope.compare(0, 2, "s:") != 0) return false;
        if (!addrUid(scope.substr(2))) return false;
    }
    if (i == tok.size()) return true;               // bare scope; see above

    const std::string seg = tok[i++];
    if (seg == "vol" || seg == "pan" || seg == "mute" || seg == "solo" ||
        seg == "arm" || seg == "launch") {
        // a leaf field
    } else if (seg.compare(0, 5, "send:") == 0) {
        if (!addrIndex(seg.substr(5))) return false;
    } else if (seg.compare(0, 4, "dev:") == 0) {
        if (!addrUid(seg.substr(4))) return false;
        if (i == tok.size()) return false;
        const std::string p = tok[i++];
        if (p.compare(0, 2, "p:") != 0 || !addrIndex(p.substr(2))) return false;
    } else if (seg.compare(0, 5, "clip:") == 0) {
        if (!addrUid(seg.substr(5))) return false;
        if (i == tok.size()) return false;
        const std::string f = tok[i++];
        if (f != "gain" && f != "prob" && f != "follow" && f != "followBeats" &&
            f != "warp" && f != "loop") return false;
    } else {
        return false;
    }
    return i == tok.size();                         // nothing may trail a leaf
}

// Is this lane worth a block in the file?
//
// Two suppressions, both round-trip stable because nothing reads back what
// they drop:
//
//   * An EMPTY LANE is dropped (AUTOMATION.md §7.3). A lane with an address and
//     no points is UI state -- the user picked a parameter in the chooser and
//     has not drawn anything yet -- not content.
//   * A lane whose address is MALFORMED is dropped, for the reason writeClip
//     gates `note` on the clip kind: the reader refuses such a lane, so writing
//     one would produce a file this build cannot load back. It cannot arise
//     from a loaded set (the load would have failed) -- only from a model
//     someone built in memory -- but ClipModel is public and the writer must
//     not be the thing that makes an unreadable file.
bool laneWorthWriting(const AutoLane& l) {
    return !l.points.empty() && validAddress(l.address);
}

// A slot counts as occupied if it holds audio, remembers a source file, was
// given a name, or is a MIDI clip. There is no explicit "used" flag in
// ClipModel; the path is what keeps a clip whose media went offline alive
// across a save/load cycle, and the name covers clips that never had a file at
// all. A MIDI clip occupies its slot unconditionally: an empty, unnamed
// pattern is still launchable (it plays silence for its length), so it is a
// clip the user made and not a ghost -- which is also why ClipModel::valid()
// is true for it. Every derivation of "is this slot used" in this file goes
// through here: what gets written, which rows the scene list has to cover
// (sceneRowCount), and whether a just-parsed clip is kept.
bool clipOccupied(const ClipModel& c) {
    return c.sample != nullptr || !c.path.empty() || !c.name.empty() ||
           c.kind == ClipKind::Midi;
}

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

// A uid of 0 means "not assigned yet"; the App sweeps after load and fills the
// gaps. Writing "uid 0" would be noise, so the line is omitted and its absence
// reads back as 0 -- which keeps a file written by version 1 (no uids at all)
// and one written today identical wherever nothing has an identity yet.
void writeUid(std::string& o, const char* indent, u64 uid) {
    if (uid) kn(o, indent, "uid", std::to_string(uid));
}

// Serialized from TrackModel::savedDevices, never from the live DeviceModel:
// core has no business knowing that a plugin can be instantiated. Blocks are
// positional, so no index is written -- load order is chain order.
void writeDevice(std::string& o, const SavedDevice& d) {
    o += "  device\n";
    writeUid(o, "    ", d.uid);
    kv(o, "    ", "plugin", d.uri);
    kv(o, "    ", "name",   d.name);
    kn(o, "    ", "bypass", d.bypass ? "1" : "0");
    for (const auto& p : d.params)
        kn(o, "    ", "param", std::to_string(p.first) + " " + fmtF32(clParam(p.second)));
    o += "  enddevice\n";
}

// The field set is per kind. A MIDI clip emits, in this order:
//
//     uid, kind, name, color, gain, loop, beats, quantum,
//     [prob] [follow] [followbeats], note*
//
// and an audio clip emits what it always did (uid, file, name, color, gain,
// warp, loop, bpm, beats, range, quantum, then the sparse generative fields).
// So `kind`, and `note`, are the only two lines version 2 never saw, and only a
// MIDI clip has them.
//
// The four audio-only lines are dropped from a MIDI clip because they describe
// a sample being played back, and there is no sample: `file` names one, `bpm`
// and `warp` say how to stretch it to the transport, and `range` is a pair of
// frame offsets into it. `beats` is NOT in that group -- it is the clip's
// length in musical time, which is exactly what the piano roll edits, so MIDI
// clips keep it. Ditto `loop`, `quantum` and the generative fields, none of
// which care what is inside the clip.
//
// Dropping a field whose in-memory value is not the default is safe here in a
// way it would not be for the sparse fields above, because the suppression is
// unconditional: the writer never emits it, so the reader never sets it, so
// the second save suppresses exactly what the first one did. The one visible
// consequence is that a MIDI clip which somehow carries, say, a clipBpm of 150
// comes back from a load with the default 120 -- the field is not part of what
// a MIDI clip means, and nothing reads it for one.
void writeClip(std::string& o, const ClipModel& c, int idx) {
    const bool midi = (c.kind == ClipKind::Midi);
    o += "  clip " + std::to_string(idx) + "\n";
    writeUid(o, "    ", c.uid);
    // Immediately after the uid, i.e. in front of everything that could depend
    // on it. The reader does not actually need it early (the kind-sensitive
    // checks happen at `endclip`, so a hand-shuffled file still parses), but a
    // human scanning a diff should not have to read to the end of the block to
    // find out what kind of clip it is.
    if (midi) kv(o, "    ", "kind", "midi");
    // ClipModel::path is the authority: unlike the sample's own path it outlives
    // a file that failed to load, so an offline set keeps its references instead
    // of quietly dropping the `file` line on the next save. The sample is only
    // consulted for in-memory clips built before `path` was populated.
    if (!midi) {
        if (!c.path.empty())                          kv(o, "    ", "file", c.path);
        else if (c.sample && !c.sample->path.empty()) kv(o, "    ", "file", c.sample->path);
    }
    kv(o, "    ", "name", c.name);
    kn(o, "    ", "color",  std::to_string(clColor(c.colorIdx)));
    kn(o, "    ", "gain",   fmtF32(clGain(c.gain)));
    if (!midi)
        kn(o, "    ", "warp", std::to_string(clWarp((int)c.warp)));
    kn(o, "    ", "loop",   c.loop ? "1" : "0");
    if (!midi)
        kn(o, "    ", "bpm", fmtF64(clBpm(c.clipBpm)));
    kn(o, "    ", "beats",  fmtF64(clBeats(c.lengthBeats)));
    if (!midi)
        kn(o, "    ", "range", std::to_string(clFrame(c.loopStart)) + " " +
                               std::to_string(clFrame(c.loopEnd)));
    kn(o, "    ", "quantum", std::to_string(clClipQuantum(c.quantumIdx)));
    // Sparse: the generative fields are off on almost every clip, and a set of
    // 300 clips should not carry 900 lines saying so. Emitting only non-default
    // values stays round-trip stable because the value a missing line loads as
    // is exactly the value that suppresses the line.
    const f64 prob = clProb(c.prob);
    const int fol  = clFollow((int)c.followAction);
    const f64 fb   = clFollowBeats(c.followBeats);
    if (prob != 1.0)              kn(o, "    ", "prob", fmtF64(prob));
    if (fol != (int)Follow::None) kn(o, "    ", "follow", std::to_string(fol));
    if (fb != 0.0)                kn(o, "    ", "followbeats", fmtF64(fb));
    // Notes last, after every scalar, so the block reads header-then-content
    // and a clip with 400 notes still shows its settings at the top.
    //
    // Written in vector order, not sorted here. ClipModel::notes is kept sorted
    // by beat by the editor, and that is where the ordering contract lives: the
    // writer emits the order it is given and the reader preserves the order it
    // finds. Neither end reorders, so a file whose notes were shuffled by hand
    // still round-trips byte-identically -- it just loads as a session whose
    // note vector is not sorted, which is the writer's problem, not the
    // format's. (The App does not sort on load either. If that ever becomes a
    // requirement it belongs in the App, next to the editing code that upholds
    // the invariant, and not here.)
    //
    // Gated on the kind rather than just on the vector being non-empty: the
    // reader refuses `note` inside an audio clip, so an audio ClipModel that
    // somehow carries leftover notes must not be written into a file that
    // cannot be read back.
    if (midi) for (const auto& n : c.notes)
        kn(o, "    ", "note", fmtF64(clNoteBeat(n.beat)) + " " +
                              fmtF64(clNoteLen(n.len)) + " " +
                              std::to_string((int)clPitch(n.pitch)) + " " +
                              std::to_string((int)clVel(n.vel)));
    // Envelopes after the notes, for the reason the notes come after the
    // scalars: the block reads header-then-content, and a clip with 400 notes
    // and 3 lanes still shows its settings at the top.
    //
    // NOT gated on the kind, unlike `note` (AUTOMATION.md §7.1). An audio clip
    // has nowhere to keep notes, but an envelope is meaningful on both kinds --
    // clip gain, a track fader, a device parameter -- and the reader accepts one
    // inside an audio clip today so that wave 8's audio-clip lanes are a UI
    // change and not a format change.
    //
    // Lane order and point order are the model's, preserved on both ends and
    // sorted by neither (§7.3): the editor holds the sorted-by-beat invariant,
    // the writer emits what it is given, the reader keeps what it finds. A
    // hand-shuffled file therefore still round-trips byte-identically and
    // merely loads as a session whose vectors are unsorted -- the same contract
    // `note` has, for the same reason.
    for (const auto& lane : c.envelopes) {
        if (!laneWorthWriting(lane)) continue;
        kv(o, "    ", "env", lane.address);
        // Sparse, like every flag in this file: `enabled` is true on every lane
        // anyone has ever drawn, so the common case emits nothing and only a
        // deactivated lane costs a line. Round-trip stable because the value a
        // missing `off` loads as (true) is exactly the value that suppresses it.
        // It leads the block so a human scanning a diff sees the lane is dead
        // before reading 200 breakpoints.
        if (!lane.enabled) kv(o, "      ", "off", std::string());
        for (const auto& p : lane.points) {
            // The curve byte is written only when non-zero -- the same
            // discipline prob/follow/send use and, again, for the same
            // round-trip reason: the value a missing field loads as (0, linear)
            // is exactly the value that suppresses it. Note it is preserved,
            // not normalized; see clCurve.
            const u8 curve = clCurve((i64)p.curve);
            std::string ln = fmtF64(clEnvBeat(p.beat)) + " " + fmtF32(clEnvValue(p.value));
            if (curve) ln += " " + std::to_string((int)curve);
            kn(o, "      ", "pt", ln);
        }
        o += "    endenv\n";
    }
    o += "  endclip\n";
}

void writeTrack(std::string& o, const TrackModel& t, int idx) {
    o += "track " + std::to_string(idx) + "\n";
    writeUid(o, "  ", t.uid);
    kv(o, "  ", "name", t.name);
    kn(o, "  ", "color", std::to_string(clColor(t.colorIdx)));
    kn(o, "  ", "fader", fmtF32(clFader(t.fader)));
    kn(o, "  ", "pan",   fmtF32(clPan(t.pan)));
    kn(o, "  ", "flags", std::string(t.mute ? "1" : "0") + " " +
                         (t.solo ? "1" : "0") + " " + (t.arm ? "1" : "0"));
    // Post-fader sends, immediately after the rest of the mixer scalars and
    // sparse: 0 is both the default and the "this bus gets nothing" value, and
    // a set of 32 tracks should not carry 128 lines saying so. The suppression
    // is round-trip stable for the same reason the clip's generative fields are
    // -- the value a missing line loads as is exactly the value that suppresses
    // the line -- and it is what makes a v3 set re-save with only the header
    // line changed.
    for (int i = 0; i < kMaxReturns; ++i) {
        const f32 lvl = clSend(t.sends[i]);
        if (lvl != 0.f) kn(o, "  ", "send", std::to_string(i) + " " + fmtF32(lvl));
    }
    kn(o, "  ", "width", fmtF32(clWidth(t.width)));
    // The chain sits between the track scalars and the clips.
    for (const auto& d : t.savedDevices) writeDevice(o, d);
    for (int i = 0; i < kMaxScenes; ++i)
        if (clipOccupied(t.slots[i])) writeClip(o, t.slots[i], i);
    o += "endtrack\n";
}

// Is this return bus worth a block in the file?
//
// The returns are a fixed array, not a list: every set has kMaxReturns of them
// whether the user touched one or not. Writing all four unconditionally would
// put ~20 lines into every project that has never used a send, and would make
// the v3 -> v4 re-save diff far more than the header line it is required to be.
// So a return is written exactly when it differs from a freshly constructed one
// in any respect a file can carry: it has an identity, a chain, a renamed bus
// or a moved fader.
//
// The predicate is deliberately "differs from the default" and not "has
// devices": a user who renames Return B and pulls its fader down has said
// something about the set even with an empty chain, and losing that on the next
// save would be a bug. It is round-trip stable because everything it tests is
// also what the block writes, so a skipped return loads back as the default
// that made it skippable, and a written one loads back as itself.
//
// Not tested: ReturnModel::devices. Live instances never reach this file --
// the App serializes devices -> savedDevices before saving, exactly as it does
// for tracks -- so a return whose chain exists only as instances is, correctly,
// a return this layer knows nothing about.
bool returnWorthWriting(const ReturnModel& r) {
    const ReturnModel deflt{};
    return r.uid != 0 || !r.savedDevices.empty() || r.name != deflt.name ||
           clFader(r.fader) != clFader(deflt.fader);
}

// Same shape as a track's mixer half: identity, name, fader, then the chain.
// No clips, no sends (a return that fed a return would be a feedback path the
// engine has no cycle detection for), and no index inside the block -- the
// index is on the `return` line, because unlike device blocks these are not
// positional: skipping the untouched ones is the whole point.
void writeReturn(std::string& o, const ReturnModel& r, int idx) {
    o += "return " + std::to_string(idx) + "\n";
    writeUid(o, "  ", r.uid);
    kv(o, "  ", "name", r.name);
    kn(o, "  ", "fader", fmtF32(clFader(r.fader)));
    for (const auto& d : r.savedDevices) writeDevice(o, d);
    o += "endreturn\n";
}

// The master chain. Device blocks and nothing else: Session carries no master
// fader, name or uid, and inventing lines for state the model does not have
// would be inventing state. An empty chain writes nothing at all, which is
// what keeps a set with no master processing byte-identical to its v3 self.
void writeMaster(std::string& o, const std::vector<SavedDevice>& devices) {
    if (devices.empty()) return;
    o += "master\n";
    for (const auto& d : devices) writeDevice(o, d);
    o += "endmaster\n";
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
    // Identifiers use the full u64 range, which strtoll would saturate. strtoull
    // is the right width but silently wraps a leading '-', so a negative uid is
    // caught here and read as "unassigned" instead of as a huge one.
    bool uid(u64& out) {
        const char* q = p;
        while (*q == ' ' || *q == '\t') ++q;
        const bool neg = (*q == '-');
        char* e = nullptr;
        errno = 0;
        const unsigned long long v = std::strtoull(p, &e, 10);
        if (e == p) return false;
        p = e;
        out = neg ? 0 : (u64)v;
        return true;
    }
    // Is there anything but whitespace left on the line? Only `pt` needs this,
    // because it is the one line in the format with an OPTIONAL numeric field:
    // "absent" and "present but not a number" have to be told apart, and
    // strtoll cannot do it on its own.
    bool exhausted() const {
        const char* q = p;
        while (*q == ' ' || *q == '\t') ++q;
        return *q == '\0';
    }
};

enum class St { Top, Track, Device, Clip, Env, Scene, Return, Master };

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

    o += std::string(kHeaderWord) + " " + std::to_string(kFormatVersion) + "\n";
    kn(o, "", "tempo",     fmtF64(clTempo(s.tempo)));
    kn(o, "", "sig",       std::to_string(clSig(s.sigNum)) + " " + std::to_string(clSig(s.sigDen)));
    kn(o, "", "quantum",   std::to_string(clQuantum(s.quantumIdx)));
    kn(o, "", "metronome", s.metronome ? "1" : "0");
    // Always written, unlike the per-entity uids: the counter is what keeps
    // identifiers unique across a save, so "no line" would mean handing out
    // numbers that are already in use.
    kn(o, "", "nextuid",   std::to_string(clNextUid(s.nextUid)));
    kv(o, "", "name", s.name);

    const size_t nTracks = std::min(s.tracks.size(), (size_t)kMaxTracks);
    for (size_t i = 0; i < nTracks; ++i) writeTrack(o, s.tracks[i], (int)i);

    // Buses sit between the tracks and the scenes: after the things that send
    // into them, before the things that launch. Both are sparse -- see
    // returnWorthWriting and writeMaster -- so a set that uses neither adds not
    // one byte here.
    size_t nReturns = 0;
    for (int i = 0; i < kMaxReturns; ++i)
        if (returnWorthWriting(s.returns[i])) { writeReturn(o, s.returns[i], i); ++nReturns; }
    writeMaster(o, s.masterSavedDevices);

    const size_t nScenes = std::min(sceneRowCount(s), (size_t)kMaxScenes);
    const SceneModel deflt{};
    for (size_t i = 0; i < nScenes; ++i) {
        const SceneModel& sc = (i < s.scenes.size()) ? s.scenes[i] : deflt;
        o += "scene " + std::to_string(i) + "\n";
        writeUid(o, "  ", sc.uid);
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
    LOGI("saved %zu tracks / %zu scenes / %zu returns%s -> %s", nTracks, nScenes, nReturns,
         s.masterSavedDevices.empty() ? "" : " + master chain", path.c_str());
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
    int  ti = -1, ci = -1, sci = -1, ri = -1;
    // Device blocks now appear in three places (a track, a return, the master
    // chain), so St::Device no longer knows where it is on its own: the owning
    // vector and the state to fall back to at `enddevice` are recorded when the
    // block opens. The pointer is only dereferenced between `device` and
    // `enddevice`, and the only thing that grows that vector in between is the
    // emplace_back that opened the block, so it cannot dangle.
    std::vector<SavedDevice>* devOwner = nullptr;
    St devPrev = St::Track;
    auto openDevice = [&](std::vector<SavedDevice>& owner, St back) {
        owner.emplace_back();
        devOwner = &owner;
        devPrev  = back;
        st = St::Device;
    };
    std::string clipFile;
    bool clipSawRange = false, clipSawBpm = false, clipSawBeats = false, clipSawName = false;
    int  missing = 0;

    // Resolves the pending clip once its body has been read: the sample load is
    // deferred to `endclip` so `file` may appear in any order.
    auto finishClip = [&]() {
        if (ti < 0 || ci < 0) return;
        ClipModel& c = out.tracks[(size_t)ti].slots[ci];
        // The reference is recorded whether or not the audio can be decoded --
        // that is what lets the next save write the same `file` line back.
        c.path = clipFile;
        if (!clipFile.empty()) {
            c.sample = loadSample(clipFile, engineRate);
            if (!c.sample) {
                ++missing;
                LOGW("project: missing sample '%s' (slot %d/%d kept, path preserved)", clipFile.c_str(), ti, ci);
                if (!clipSawName) c.name = baseName(clipFile);
            } else {
                // Only fill in what the file did not state, so a project that
                // spells out every field re-saves byte-for-byte identically.
                // An explicitly empty `name` counts as stated.
                if (!clipSawName)    c.name = c.sample->name;
                if (!clipSawBpm)     c.clipBpm = clBpm(c.sample->guessedBpm);
                if (!clipSawBeats)   c.lengthBeats = clBeats(c.sample->guessedBeats);
                if (!clipSawRange) { c.loopStart = 0; c.loopEnd = c.sample->frames; }
            }
        }
        if (!clipOccupied(c)) {
            // Nothing identifies this slot; drop it rather than leave a ghost.
            // A MIDI clip never lands here, however empty and unnamed it is:
            // the block in the file is itself the statement that the slot is
            // taken, and clipOccupied agrees.
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
            // Either spelling, same version space. See kHeaderWord.
            if (!isHeaderWord(key))
                return fail("not a project file (expected 'nxtakt <version>', "
                            "or 'lattice <version>' from before the rename)");
            int v = 0;
            if (!sc.integer(v)) return fail("missing format version");
            if (v < kMinFormatVersion || v > kFormatVersion)
                return fail("unsupported format version " + std::to_string(v));
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
            } else if (key == "nextuid") {
                u64 v; if (!sc.uid(v)) return fail("nextuid: expected an integer");
                out.nextUid = clNextUid(v);
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
            } else if (key == "return") {
                int v; if (!sc.integer(v)) return fail("return: expected an index");
                // Structure, not a value: the returns are a fixed array, so an
                // index past the end names a bus this build does not have.
                // Clamping it would silently move somebody's chain onto a
                // different bus, which is worse than refusing the file.
                if (v < 0 || v >= kMaxReturns)
                    return fail("return index " + std::to_string(v) + " out of range");
                ri = v;
                st = St::Return;
            } else if (key == "master") {
                // No index and no scalars of its own; the block exists only to
                // scope the device chain.
                st = St::Master;
            } else {
                return fail("unexpected '" + key + "' at top level");
            }
            break;
        }
        // ---------------------------------------------------------------
        case St::Track: {
            TrackModel& t = out.tracks[(size_t)ti];
            if (key == "uid") {
                u64 v; if (!sc.uid(v)) return fail("track uid: expected an integer");
                t.uid = v;
            } else if (key == "name") {
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
            } else if (key == "send") {
                int idx = 0; f64 v = 0.0;
                if (!sc.integer(idx) || !sc.num(v))
                    return fail("send: expected an index and a level");
                // The same split as everywhere else: structure is rejected, the
                // value is clamped. An index naming a bus that does not exist
                // cannot be repaired -- there is no right answer for which of
                // the four it meant -- while a level of 2 or of NaN is a line
                // that says something, just not something a linear gain can be.
                if (idx < 0 || idx >= kMaxReturns)
                    return fail("send index " + std::to_string(idx) + " out of range");
                t.sends[idx] = clSend((f32)v);
            } else if (key == "width") {
                f64 v; if (!sc.num(v)) return fail("width: expected a number");
                t.width = clWidth((f32)v);
            } else if (key == "device") {
                // Positional: the chain is rebuilt in file order. No cap is
                // imposed here -- the App decides how many of these it can
                // actually instantiate; silently dropping the tail of a user's
                // chain at load time would be the worse failure.
                openDevice(t.savedDevices, St::Track);
            } else if (key == "clip") {
                int v; if (!sc.integer(v)) return fail("clip: expected an index");
                if (v < 0 || v >= kMaxScenes)
                    return fail("clip index " + std::to_string(v) + " out of range");
                ci = v;
                t.slots[ci] = ClipModel{};
                clipFile.clear();
                clipSawRange = clipSawBpm = clipSawBeats = clipSawName = false;
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
        case St::Device: {
            SavedDevice& d = devOwner->back();
            if (key == "uid") {
                u64 v; if (!sc.uid(v)) return fail("device uid: expected an integer");
                d.uid = v;
            } else if (key == "plugin") {
                d.uri = unesc(rest);
            } else if (key == "name") {
                d.name = unesc(rest);
            } else if (key == "bypass") {
                int v; if (!sc.integer(v)) return fail("device bypass: expected 0 or 1");
                d.bypass = v != 0;
            } else if (key == "param") {
                i64 id = 0; f64 v = 0.0;
                if (!sc.integer(id) || !sc.num(v))
                    return fail("param: expected an id and a value");
                d.params.emplace_back((u32)clampv(id, (i64)0, (i64)UINT32_MAX), clParam((f32)v));
            } else if (key == "enddevice") {
                // Back to whichever block opened this one, not unconditionally
                // to St::Track: the same device grammar serves tracks, returns
                // and the master chain.
                st = devPrev;
            } else {
                return fail("unexpected '" + key + "' inside device");
            }
            break;
        }
        // ---------------------------------------------------------------
        case St::Clip: {
            ClipModel& c = out.tracks[(size_t)ti].slots[ci];
            if (key == "uid") {
                u64 v; if (!sc.uid(v)) return fail("clip uid: expected an integer");
                c.uid = v;
            } else if (key == "kind") {
                // Only the two kinds that exist. `kind audio` is accepted even
                // though the writer never emits it -- it names the default, and
                // refusing a redundant statement of the truth would be perverse
                // -- but it is dropped on the next save, like any other line
                // whose value a clip of that kind does not carry.
                if (rest == "midi")       c.kind = ClipKind::Midi;
                else if (rest == "audio") c.kind = ClipKind::Audio;
                else return fail("clip kind: expected 'audio' or 'midi'");
            } else if (key == "note") {
                // Structure is rejected, values are clamped -- the same split as
                // `param` and every scalar above. A note missing a field is a
                // broken line and there is no sane guess for what it meant; a
                // note at pitch 300 is a line that says something, just not
                // something MIDI can express, so it is pulled into range.
                f64 beat = 0.0, len = 0.0;
                i64 pitch = 0, vel = 0;
                if (!sc.num(beat) || !sc.num(len) || !sc.integer(pitch) || !sc.integer(vel))
                    return fail("note: expected beat, length, pitch and velocity");
                // Appended, never sorted: see writeClip. File order is the
                // order the session gets.
                c.notes.push_back(NoteModel{clNoteBeat(beat), clNoteLen(len),
                                            clPitch(pitch), clVel(vel)});
            } else if (key == "file") {
                clipFile = unesc(rest);
            } else if (key == "name") {
                c.name = unesc(rest); clipSawName = true;
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
            } else if (key == "prob") {
                f64 v; if (!sc.num(v)) return fail("clip prob: expected a number");
                c.prob = clProb(v);
            } else if (key == "follow") {
                int v; if (!sc.integer(v)) return fail("clip follow: expected an integer");
                c.followAction = (Follow)clFollow(v);
            } else if (key == "followbeats") {
                f64 v; if (!sc.num(v)) return fail("clip followbeats: expected a number");
                c.followBeats = clFollowBeats(v);
            } else if (key == "env") {
                // The address is the whole rest of the line, escaped like
                // `name` and `plugin`, and validated AFTER unescaping because
                // the unescaped text is what the model holds and what the next
                // save writes back.
                //
                // Malformed -> the load fails. See validAddress for the full
                // argument; the short version is that it is structure, and a
                // repaired address is automation silently moved onto some other
                // parameter. A *dangling* address -- well-formed, naming a uid
                // nothing answers to -- is content and is kept.
                //
                // Not gated on the clip kind: an audio clip may carry lanes.
                const std::string addr = unesc(rest);
                if (!validAddress(addr))
                    return fail("env: malformed parameter address '" + addr + "'");
                c.envelopes.push_back(AutoLane{addr, {}, true});
                st = St::Env;
            } else if (key == "endclip") {
                // Checked here rather than at the offending line so the block
                // may be written in any order: `note` before `kind midi` is
                // still a MIDI clip, and the reader has to have seen the whole
                // block before it can say otherwise.
                //
                // These two are rejections, not silent repairs, because both
                // combinations describe content that this format cannot carry
                // and the next save would therefore throw away: a MIDI clip has
                // nowhere to keep a sample path, and an audio clip has nowhere
                // to keep notes. Guessing (promoting the clip to MIDI and
                // dropping its file, or the reverse) would destroy one half of
                // what the file says. Failing loudly leaves the session
                // untouched and the file on disk intact for the user to fix.
                // Inapplicable *scalars* are the tolerated case, not this one:
                // a `bpm` or `range` line inside a MIDI clip parses and is then
                // simply not re-emitted, since nothing is lost that the clip
                // was actually using.
                if (c.kind == ClipKind::Midi && !clipFile.empty())
                    return fail("a midi clip cannot have a 'file' line");
                if (c.kind != ClipKind::Midi && !c.notes.empty())
                    return fail("'note' is only valid inside a midi clip");
                finishClip();
                ci = -1;
                st = St::Track;
            } else {
                return fail("unexpected '" + key + "' inside clip");
            }
            break;
        }
        // ---------------------------------------------------------------
        // An envelope block, opened from St::Clip and closing back to it. The
        // lane is always envelopes.back(): `env` is reachable only from a clip,
        // ti/ci stay valid for the whole block, and nothing else appends to that
        // vector in between.
        //
        // No count is enforced here. kMaxClipLanes and kMaxClipAutoPoints are
        // real ceilings, but AUTOMATION.md §2.1 puts them on the editor and the
        // publisher and says "never by the parser" -- deliberately, and it is
        // the same call `device` makes one block up: refusing a file, or
        // silently truncating a user's automation at load time, is a worse
        // failure than a set the publisher will clamp anyway. The engine's
        // fixed-width lane array is the publisher's problem, not the format's.
        case St::Env: {
            AutoLane& lane = out.tracks[(size_t)ti].slots[ci].envelopes.back();
            if (key == "pt") {
                // Structure rejected, values clamped -- as for `note`, `param`
                // and every scalar. `beat` and `value` are required, so a line
                // missing either, or spelling one as something that is not a
                // number, is a broken line with no sane guess behind it.
                //
                // `curve` is optional, and that is the one place this line is
                // stricter than `note`: `note` ignores whatever trails its four
                // fields, but here "nothing follows" is itself meaningful (curve
                // 0, linear), so trailing text that is NOT a number cannot be
                // waved through -- it would be an unreadable third field
                // silently loading as the value that suppresses it.
                f64 beat = 0.0, value = 0.0;
                if (!sc.num(beat) || !sc.num(value))
                    return fail("pt: expected a beat and a value");
                i64 curve = 0;
                if (!sc.exhausted() && !sc.integer(curve))
                    return fail("pt: curve must be an integer");
                lane.points.push_back(AutoPoint{clEnvBeat(beat), clEnvValue((f32)value),
                                                clCurve(curve), {}});
            } else if (key == "off") {
                // Presence is the whole statement, like `master`. A value is not
                // read and not required: the line exists only when the lane is
                // deactivated, so "off 0" would be a contradiction the writer
                // can never produce.
                lane.enabled = false;
            } else if (key == "endenv") {
                st = St::Clip;
            } else {
                return fail("unexpected '" + key + "' inside env");
            }
            break;
        }
        // ---------------------------------------------------------------
        case St::Scene: {
            SceneModel& scn = out.scenes[(size_t)sci];
            if (key == "uid") {
                u64 v; if (!sc.uid(v)) return fail("scene uid: expected an integer");
                scn.uid = v;
            } else if (key == "name") {
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
        // ---------------------------------------------------------------
        case St::Return: {
            ReturnModel& r = out.returns[(size_t)ri];
            if (key == "uid") {
                u64 v; if (!sc.uid(v)) return fail("return uid: expected an integer");
                r.uid = v;
            } else if (key == "name") {
                r.name = unesc(rest);
            } else if (key == "fader") {
                f64 v; if (!sc.num(v)) return fail("return fader: expected a number");
                r.fader = clFader((f32)v);
            } else if (key == "device") {
                openDevice(r.savedDevices, St::Return);
            } else if (key == "endreturn") {
                ri = -1;
                st = St::Top;
            } else {
                // A nested `return`, a `send`, a `clip` -- anything that is not
                // one of the four lines above lands here and fails the load.
                // There is no half-understood middle ground: a return that
                // contained a clip is a file this format cannot represent, and
                // skipping the line would drop it on the next save.
                return fail("unexpected '" + key + "' inside return");
            }
            break;
        }
        // ---------------------------------------------------------------
        case St::Master: {
            if (key == "device") {
                openDevice(out.masterSavedDevices, St::Master);
            } else if (key == "endmaster") {
                st = St::Top;
            } else {
                return fail("unexpected '" + key + "' inside master");
            }
            break;
        }
        }
    }

    if (!sawHeader) return fail("empty or truncated project file");
    if (st != St::Top) {
        const char* what = (st == St::Env)    ? "endenv"
                         : (st == St::Clip)   ? "endclip"
                         : (st == St::Device) ? "enddevice"
                         : (st == St::Track)  ? "endtrack"
                         : (st == St::Return) ? "endreturn"
                         : (st == St::Master) ? "endmaster" : "endscene";
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
