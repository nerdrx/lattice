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
// Version 6 adds the arrangement (docs/ARRANGEMENT.md §8) and the clip's warp
// map. Three things are new:
//
//   * two top-level lines, `loop <start> <end>` and `loopon <0|1>`, beside
//     `tempo` and `quantum` -- there is one timeline and one brace, so they
//     belong to the set and not to a track. Both sparse: a brace that is off at
//     the default range writes nothing;
//   * an `arrangement` ... `endarrangement` block inside a track, after its
//     clips, holding a sparse `arrheight`, zero or more positional `aclip`
//     blocks (an item's seven fields, then a clip body identical to a `clip`'s)
//     and zero or more `autolane <address>` ... `endautolane` blocks, whose
//     `pt` and `off` lines are `env`'s verbatim;
//   * `wm <srcFrame> <beat>` lines inside an audio clip, one per warp marker.
//
// Every one of them is sparse in the way everything above is -- a set with no
// arrangement emits no `arrangement` block, no `loop`, no `loopon`, and a clip
// with no markers emits no `wm` -- so a set that uses none of it saves exactly
// the bytes version 5 saved apart from the header line.
//
// The `aclip` body is written by ONE shared writeClipBody and read by ONE
// shared clipBodyKey, which is what makes an item's payload PROVABLY the same
// grammar as a slot clip's -- `env` blocks, kind gating and the endclip checks
// included -- rather than a second copy that agrees today and drifts at the next
// format addition.
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
// Version 7 adds time-signature CHANGES, and adds them to the line that was
// already there. `sig` gains a three-token form:
//
//     sig 4 4          the session signature, at bar 0 -- v1's line, unchanged
//     sig 16 3 4       from bar 16 on, 3/4
//     sig 24 7 8       from bar 24 on, 7/8
//
// The two-token form is bar 0 and stays the canonical spelling for it, which is
// the whole reason the bar was appended to the FRONT of the new form rather than
// the back: a set in one signature -- which is every set that exists -- writes
// the identical byte sequence it wrote at v6, so the v6 -> v7 diff for it is
// exactly the header line. Later changes are sparse in the way everything above
// is sparse: no changes, no extra lines.
//
// Denominators are powers of two in 1..32 and numerators are 1..32
// (clSigNum/clSigDen in session.h, applied on save and on load like every other
// clamp here). Changes are sorted and deduplicated on load, last-wins, by
// Session::normalizeSigs -- the same normalizer every edit and the publisher
// use, so a hand-written file and an edited one cannot land in different shapes.
// Version 8 adds device STATE: one optional `state <opaque>` line inside a
// `device` block, carrying whatever a device needs to describe itself beyond
// its parameters.
//
//     device
//       uid 12
//       plugin nxtakt:rack
//       name Rack
//       bypass 0
//       param 1 0.4
//       state nxrack1;m=0,0,0.4,0,0,0,0,0;d=nxtakt%3Aeq3,0,-,0:100;x=2,0,0,6,30
//     enddevice
//
// Today exactly one device writes it -- `nxtakt:rack`, whose whole contents are
// that one line -- and THIS FILE DOES NOT KNOW THAT. The value is an opaque
// scalar: stored verbatim, escaped by esc() like any other string, never parsed
// here. src/core owns the project format, src/plugin owns what a rack means,
// and the string is the seam. (It is also what keeps gen_demo linkable: the
// rack's codec lives in internal_devices.cpp, which the tool does not link.)
//
// The compact form is printable ASCII with no whitespace by construction, so
// esc()/unesc() are the identity on everything the rack writes and the line
// round-trips byte for byte. A hand-edited file that puts a newline or a tab in
// there round-trips too, through the escapes every other string uses.
//
// Sparse, like everything since v2: a device with no state writes no line, so a
// set with no rack in it -- which is every set that existed before this version
// -- produces the identical bytes v7 produced apart from the header line.
//
// A state the rack layer cannot parse is NOT a parse error here. It cannot be:
// the format has no way to tell an unreadable state from one written by a newer
// build, and refusing the file would lose a whole set over one device. The
// string survives the round trip either way; the rack drops what it cannot read
// when it restores, exactly as a `param` naming a control the plugin no longer
// has is dropped.
constexpr int kFormatVersion = 8;
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
// The signature clamps live in session.h beside the map they constrain
// (clSigNum / clSigDen / clSigBar), because the editor and the publisher need
// exactly the same ones and a second copy here would be a second answer to
// "what is a legal denominator".
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

// The arrangement. Same symmetry, same structure/value split: a negative `at`
// or a `len` of NaN is a value and is pulled into range, while a missing `at` is
// structure and fails the load.
//
// clArrLen's floor is kMinArrBeats and not 0 for the reason clNoteLen's is
// kMinNoteLen: an item shorter than that cannot be grabbed at any zoom and
// cannot carry a fade, so it is not an item. The editor deletes such a thing
// (arrangeRepair step 2); the reader, which never deletes content, clamps up.
f64 clArrBeat(f64 v)   { return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }
f64 clArrLen(f64 v)    { return std::isfinite(v) ? clampv(v, kMinArrBeats, 1e7) : 1.0; }
f64 clFade(f64 v)      { return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }
// clCurve verbatim, and for the same argument: the byte is PRESERVED, not
// normalized. A newer build writes `fadeshape 3`; this build must load that set,
// draw the fade straight, and hand the 3 back on the next save rather than
// silently flattening somebody's curve into a file that can never say so again.
u8  clFadeShape(i64 v) { return (u8)clampv(v, (i64)0, (i64)255); }
f32 clArrHeight(f32 v) { return std::isfinite(v) ? clampv(v, 16.f, 1024.f) : kArrHeightDefault; }
// A warp marker's clip-relative beat. clEnvBeat's body; named apart because the
// two clamp different things and one of them may move later.
f64 clMarkBeat(f64 v)  { return std::isfinite(v) ? clampv(v, 0.0, 1e7) : 0.0; }

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
    // v8, and sparse: no state, no line. It goes AFTER the parameters purely so
    // the block reads in the order the loader has to apply it (params, then
    // state -- see SavedDevice::state); the reader does not care.
    if (!d.state.empty()) kv(o, "    ", "state", d.state);
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
//
// Everything in a clip block after its `uid`, at the given indent -- and the
// ONE place that knowledge lives. A slot clip and an arrangement item write the
// same bytes through this function, so an `aclip`'s payload is provably the same
// grammar as a `clip`'s rather than a second copy that agrees today and drifts
// at the next format addition. Its reader half is clipBodyKey.
//
// `indent` is the body's indent; envelope contents sit two spaces deeper, which
// is the only place the nesting shows.
void writeClipBody(std::string& o, const ClipModel& c, const char* indent) {
    const bool midi = (c.kind == ClipKind::Midi);
    const std::string innerS = std::string(indent) + "  ";
    const char* inner = innerS.c_str();
    // Immediately after the uid, i.e. in front of everything that could depend
    // on it. The reader does not actually need it early (the kind-sensitive
    // checks happen at `endclip`, so a hand-shuffled file still parses), but a
    // human scanning a diff should not have to read to the end of the block to
    // find out what kind of clip it is.
    if (midi) kv(o, indent, "kind", "midi");
    // ClipModel::path is the authority: unlike the sample's own path it outlives
    // a file that failed to load, so an offline set keeps its references instead
    // of quietly dropping the `file` line on the next save. The sample is only
    // consulted for in-memory clips built before `path` was populated.
    if (!midi) {
        if (!c.path.empty())                          kv(o, indent, "file", c.path);
        else if (c.sample && !c.sample->path.empty()) kv(o, indent, "file", c.sample->path);
    }
    kv(o, indent, "name", c.name);
    kn(o, indent, "color",  std::to_string(clColor(c.colorIdx)));
    kn(o, indent, "gain",   fmtF32(clGain(c.gain)));
    if (!midi)
        kn(o, indent, "warp", std::to_string(clWarp((int)c.warp)));
    kn(o, indent, "loop",   c.loop ? "1" : "0");
    if (!midi)
        kn(o, indent, "bpm", fmtF64(clBpm(c.clipBpm)));
    kn(o, indent, "beats",  fmtF64(clBeats(c.lengthBeats)));
    if (!midi)
        kn(o, indent, "range", std::to_string(clFrame(c.loopStart)) + " " +
                              std::to_string(clFrame(c.loopEnd)));
    kn(o, indent, "quantum", std::to_string(clClipQuantum(c.quantumIdx)));
    // Sparse: the generative fields are off on almost every clip, and a set of
    // 300 clips should not carry 900 lines saying so. Emitting only non-default
    // values stays round-trip stable because the value a missing line loads as
    // is exactly the value that suppresses the line.
    const f64 prob = clProb(c.prob);
    const int fol  = clFollow((int)c.followAction);
    const f64 fb   = clFollowBeats(c.followBeats);
    if (prob != 1.0)              kn(o, indent, "prob", fmtF64(prob));
    if (fol != (int)Follow::None) kn(o, indent, "follow", std::to_string(fol));
    if (fb != 0.0)                kn(o, indent, "followbeats", fmtF64(fb));
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
        kn(o, indent, "note", fmtF64(clNoteBeat(n.beat)) + " " +
                              fmtF64(clNoteLen(n.len)) + " " +
                              std::to_string((int)clPitch(n.pitch)) + " " +
                              std::to_string((int)clVel(n.vel)));
    // The warp map, after the notes and before the envelopes: content, like
    // both of them, and the two content vectors are mutually exclusive anyway
    // (only a MIDI clip has notes, only an audio clip has markers).
    //
    // One line per marker, `wm <srcFrame> <beat>`, rather than a `warp` ...
    // `endwarp` block, and that is a decision rather than a preference: `warp`
    // is ALREADY a clip key -- `warp 2` is the warp mode -- so a block by that
    // name would have to be told apart from the scalar by whether the rest of
    // the line is empty, which is exactly the kind of ambiguity this format does
    // not have anywhere else. A flat line needs no new parser state, costs
    // nothing when there are no markers, and preserves order on both ends the
    // way `note` and `pt` do.
    //
    // Gated on the kind exactly as `note` is, and mirrored by a rejection at
    // `endclip`: a MIDI clip has no sample, so a marker pinning a source frame
    // inside one is content this format cannot carry. The writer must never
    // produce a file the reader refuses.
    //
    // NOT sorted and NOT validated here. warpMapValid() is the publisher's gate
    // (a map that is empty, single or non-monotone is simply not published and
    // the clip warps at its clipBpm ratio), and the same argument `note` makes
    // applies: the editor owns the invariant, the format preserves what it is
    // given.
    if (!midi) for (const auto& m : c.markers)
        kn(o, indent, "wm", std::to_string(clFrame(m.srcFrame)) + " " +
                            fmtF64(clMarkBeat(m.beat)));
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
        kv(o, indent, "env", lane.address);
        // Sparse, like every flag in this file: `enabled` is true on every lane
        // anyone has ever drawn, so the common case emits nothing and only a
        // deactivated lane costs a line. Round-trip stable because the value a
        // missing `off` loads as (true) is exactly the value that suppresses it.
        // It leads the block so a human scanning a diff sees the lane is dead
        // before reading 200 breakpoints.
        if (!lane.enabled) kv(o, inner, "off", std::string());
        for (const auto& p : lane.points) {
            // The curve byte is written only when non-zero -- the same
            // discipline prob/follow/send use and, again, for the same
            // round-trip reason: the value a missing field loads as (0, linear)
            // is exactly the value that suppresses it. Note it is preserved,
            // not normalized; see clCurve.
            const u8 curve = clCurve((i64)p.curve);
            std::string ln = fmtF64(clEnvBeat(p.beat)) + " " + fmtF32(clEnvValue(p.value));
            if (curve) ln += " " + std::to_string((int)curve);
            kn(o, inner, "pt", ln);
        }
        o += indent; o += "endenv\n";
    }
}

// A slot clip: the indexed header, the uid, the shared body, the terminator.
// The index is on the `clip` line because a slot is a cell in a fixed array and
// the file has to say which one; contrast `aclip`, which is a list entry.
void writeClip(std::string& o, const ClipModel& c, int idx) {
    o += "  clip " + std::to_string(idx) + "\n";
    writeUid(o, "    ", c.uid);
    writeClipBody(o, c, "    ");
    o += "  endclip\n";
}

// One arrangement item. NO INDEX: it is positional, exactly like `device`, and
// for exactly the reason writeDevice's comment gives -- load order is the list's
// order, which here is timeline order. An index on a list is a second statement
// of the ordering and therefore a second place for it to disagree with the
// first.
//
// The uid on this block is the ITEM's, not the copied clip's: an item is the
// entity the arrangement addresses (the automation publisher keys retirement on
// it, and an insertion renumbers every index after it). ArrangeClip::src.uid is
// deliberately not written -- two placements of one loop would otherwise both
// claim one identity -- so it comes back as 0, exactly as a MIDI clip's clipBpm
// comes back as the default.
void writeAClip(std::string& o, const ArrangeClip& a) {
    o += "    aclip\n";
    writeUid(o, "      ", a.uid);
    // `at` and `len` are NOT sparse: every item has both, and a missing `at`
    // silently meaning 0 is exactly the failure a required field prevents.
    kn(o, "      ", "at",  fmtF64(clArrBeat(a.start)));
    kn(o, "      ", "len", fmtF64(clArrLen(a.length)));
    // The rest are, in the way everything in this file is: the value a missing
    // line loads as is exactly the value that suppresses the line.
    //
    // `off` here is the item's OFFSET into its clip. Inside an `env` block the
    // same word means "this lane is deactivated". They never collide, because
    // the parser is in different states and neither key is legal in the other's
    // -- but a human reading a file meets two `off`s meaning two things, so it
    // is said once here and once at the reader.
    const f64 off  = clArrBeat(a.offset);
    const f64 fin  = clFade(a.fadeIn);
    const f64 fout = clFade(a.fadeOut);
    const u8  shp  = clFadeShape((i64)a.fadeShape);
    if (off  != 0.0) kn(o, "      ", "off", fmtF64(off));
    if (fin  != 0.0) kn(o, "      ", "fadein",  fmtF64(fin));
    if (fout != 0.0) kn(o, "      ", "fadeout", fmtF64(fout));
    if (shp  != 0)   kn(o, "      ", "fadeshape", std::to_string((int)shp));
    // Provenance, and dangling is the ordinary case rather than an error: a
    // `source` naming a clip that has been deleted is written back unchanged
    // forever, exactly as a dangling parameter address is.
    if (a.sourceUid) kn(o, "      ", "source", std::to_string(a.sourceUid));
    writeClipBody(o, a.src, "      ");
    o += "    endaclip\n";
}

// A track's arrangement, after its clips: everything in it is per-track, and a
// top-level block would have to re-state which track it belongs to -- a second
// way of naming a track, which is a second way to get it wrong. Position within
// the track is the same argument notes-after-scalars makes: a block reads
// header-then-content, and a track's mixer settings should not sit below two
// hundred lines of timeline.
//
// Sparse as a whole, which is what makes the v5 -> v6 diff for a set with no
// arrangement exactly the header line: no items, no worth-writing lanes and a
// default height write no block at all.
void writeArrangement(std::string& o, const TrackModel& t) {
    const f32 h = clArrHeight(t.arrHeight);
    bool anyLane = false;
    for (const auto& l : t.arrangeAutos) if (laneWorthWriting(l)) { anyLane = true; break; }
    if (t.arrange.empty() && !anyLane && h == clArrHeight(kArrHeightDefault)) return;

    o += "  arrangement\n";
    if (h != clArrHeight(kArrHeightDefault)) kn(o, "    ", "arrheight", fmtF32(h));
    for (const auto& a : t.arrange) writeAClip(o, a);
    // `autolane`, not `env`: these lanes are absolute-timeline, and a reader
    // meeting `env` at arrangement scope would have to infer which beat space it
    // was in from context. Two names, two meanings, no inference. The contents
    // are `env`'s verbatim, down to the sparse `off` and the optional curve.
    for (const auto& lane : t.arrangeAutos) {
        if (!laneWorthWriting(lane)) continue;
        kv(o, "    ", "autolane", lane.address);
        if (!lane.enabled) kv(o, "      ", "off", std::string());
        for (const auto& p : lane.points) {
            const u8 curve = clCurve((i64)p.curve);
            std::string ln = fmtF64(clEnvBeat(p.beat)) + " " + fmtF32(clEnvValue(p.value));
            if (curve) ln += " " + std::to_string((int)curve);
            kn(o, "      ", "pt", ln);
        }
        o += "    endautolane\n";
    }
    o += "  endarrangement\n";
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
    // The timeline sits after the grid, and writes nothing at all when there is
    // none of it.
    writeArrangement(o, t);
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

// Parser states. The three arrangement ones are §8.6: St::Arrange opens from
// St::Track and closes back to it, St::AClip and St::AutoLane open from
// St::Arrange, and St::Env now has TWO entry points -- a slot clip and an
// arrangement item -- which is why it has to remember what to close back to
// (`envPrev`), exactly as St::Device already remembers `devPrev`.
enum class St { Top, Track, Device, Clip, Env, Scene, Return, Master,
                Arrange, AClip, AutoLane };

// What a clip's body needs the reader to remember beyond the ClipModel itself:
// the sample path, whose load is deferred to the block's terminator so `file`
// may appear in any order, and which fields the file actually stated (so a
// project that spells out every field re-saves byte-for-byte identically).
struct ClipReadState {
    std::string file;
    bool sawRange = false, sawBpm = false, sawBeats = false, sawName = false;
    void reset() { file.clear(); sawRange = sawBpm = sawBeats = sawName = false; }
};

// Offering a line to a shared handler has three outcomes and not two: the key
// was consumed, the key belongs to the caller's own block, or the key was ours
// and the line was broken. A bool plus an out-parameter would fold two of them
// together at exactly the place the format's structure/value split lives.
enum class BodyKey { No, Yes, Bad };

// `pt` and `off`, shared by St::Env (a clip envelope) and St::AutoLane (an
// arrangement lane) so the two block types cannot drift apart. §8.2's whole
// argument for two block NAMES is that the beat space differs; the contents are
// the same grammar, and this is what makes that true rather than asserted.
BodyKey autoLaneKey(AutoLane& lane, const std::string& key, Scan& sc, std::string& err) {
    if (key == "pt") {
        // Structure rejected, values clamped -- as for `note`, `param` and every
        // scalar. `beat` and `value` are required, so a line missing either, or
        // spelling one as something that is not a number, is a broken line with
        // no sane guess behind it.
        //
        // `curve` is optional, and that is the one place this line is stricter
        // than `note`: `note` ignores whatever trails its four fields, but here
        // "nothing follows" is itself meaningful (curve 0, linear), so trailing
        // text that is NOT a number cannot be waved through -- it would be an
        // unreadable third field silently loading as the value that suppresses
        // it.
        f64 beat = 0.0, value = 0.0;
        if (!sc.num(beat) || !sc.num(value)) { err = "pt: expected a beat and a value"; return BodyKey::Bad; }
        i64 curve = 0;
        if (!sc.exhausted() && !sc.integer(curve)) { err = "pt: curve must be an integer"; return BodyKey::Bad; }
        lane.points.push_back(AutoPoint{clEnvBeat(beat), clEnvValue((f32)value), clCurve(curve), {}});
        return BodyKey::Yes;
    }
    if (key == "off") {
        // Presence is the whole statement, like `master`. A value is not read
        // and not required: the line exists only when the lane is deactivated,
        // so "off 0" would be a contradiction the writer can never produce.
        //
        // THE COLLISION, said once at the reader as it is once at the writer:
        // `off` inside an `aclip` is that item's offset in beats and carries a
        // number. It cannot be confused with this one, because the parser is in
        // a different state and neither key is legal in the other's.
        lane.enabled = false;
        return BodyKey::Yes;
    }
    return BodyKey::No;
}

// The reader half of writeClipBody: every key inside a clip block except `uid`
// and the terminator, which belong to whichever block opened it. One handler, so
// an `aclip`'s payload is provably the same grammar as a `clip`'s.
BodyKey clipBodyKey(ClipModel& c, const std::string& key, const std::string& rest,
                    Scan& sc, ClipReadState& crs, St& st, St& envPrev, std::string& err) {
    if (key == "kind") {
        // Only the two kinds that exist. `kind audio` is accepted even though
        // the writer never emits it -- it names the default, and refusing a
        // redundant statement of the truth would be perverse -- but it is
        // dropped on the next save, like any other line whose value a clip of
        // that kind does not carry.
        if (rest == "midi")       c.kind = ClipKind::Midi;
        else if (rest == "audio") c.kind = ClipKind::Audio;
        else { err = "clip kind: expected 'audio' or 'midi'"; return BodyKey::Bad; }
    } else if (key == "note") {
        // Structure is rejected, values are clamped -- the same split as `param`
        // and every scalar above. A note missing a field is a broken line and
        // there is no sane guess for what it meant; a note at pitch 300 is a
        // line that says something, just not something MIDI can express, so it
        // is pulled into range.
        f64 beat = 0.0, len = 0.0;
        i64 pitch = 0, vel = 0;
        if (!sc.num(beat) || !sc.num(len) || !sc.integer(pitch) || !sc.integer(vel)) {
            err = "note: expected beat, length, pitch and velocity";
            return BodyKey::Bad;
        }
        // Appended, never sorted: see writeClipBody. File order is the order the
        // session gets.
        c.notes.push_back(NoteModel{clNoteBeat(beat), clNoteLen(len), clPitch(pitch), clVel(vel)});
    } else if (key == "wm") {
        // A warp marker: a source frame pinned to a clip-relative beat. Both
        // fields required (there is no defaulting one of a pair of coordinates),
        // both clamped. The map is neither sorted nor validated here -- an
        // unusable map is simply not published, and the clip then warps at its
        // single clipBpm/tempo ratio, which is a working clip and not a broken
        // one.
        f64 beat = 0.0;
        i64 frame = 0;
        if (!sc.integer(frame) || !sc.num(beat)) {
            err = "wm: expected a source frame and a beat";
            return BodyKey::Bad;
        }
        c.markers.push_back(WarpMarker{clFrame(frame), clMarkBeat(beat)});
    } else if (key == "file") {
        crs.file = unesc(rest);
    } else if (key == "name") {
        c.name = unesc(rest); crs.sawName = true;
    } else if (key == "color") {
        int v; if (!sc.integer(v)) { err = "clip color: expected an integer"; return BodyKey::Bad; }
        c.colorIdx = clColor(v);
    } else if (key == "gain") {
        f64 v; if (!sc.num(v)) { err = "clip gain: expected a number"; return BodyKey::Bad; }
        c.gain = clGain((f32)v);
    } else if (key == "warp") {
        int v; if (!sc.integer(v)) { err = "clip warp: expected an integer"; return BodyKey::Bad; }
        c.warp = (Warp)clWarp(v);
    } else if (key == "loop") {
        int v; if (!sc.integer(v)) { err = "clip loop: expected 0 or 1"; return BodyKey::Bad; }
        c.loop = v != 0;
    } else if (key == "bpm") {
        f64 v; if (!sc.num(v)) { err = "clip bpm: expected a number"; return BodyKey::Bad; }
        c.clipBpm = clBpm(v); crs.sawBpm = true;
    } else if (key == "beats") {
        f64 v; if (!sc.num(v)) { err = "clip beats: expected a number"; return BodyKey::Bad; }
        c.lengthBeats = clBeats(v); crs.sawBeats = true;
    } else if (key == "range") {
        i64 a = 0, e = 0;
        if (!sc.integer(a) || !sc.integer(e)) { err = "range: expected two frame counts"; return BodyKey::Bad; }
        c.loopStart = clFrame(a); c.loopEnd = clFrame(e); crs.sawRange = true;
    } else if (key == "quantum") {
        int v; if (!sc.integer(v)) { err = "clip quantum: expected an integer"; return BodyKey::Bad; }
        c.quantumIdx = clClipQuantum(v);
    } else if (key == "prob") {
        f64 v; if (!sc.num(v)) { err = "clip prob: expected a number"; return BodyKey::Bad; }
        c.prob = clProb(v);
    } else if (key == "follow") {
        int v; if (!sc.integer(v)) { err = "clip follow: expected an integer"; return BodyKey::Bad; }
        c.followAction = (Follow)clFollow(v);
    } else if (key == "followbeats") {
        f64 v; if (!sc.num(v)) { err = "clip followbeats: expected a number"; return BodyKey::Bad; }
        c.followBeats = clFollowBeats(v);
    } else if (key == "env") {
        // The address is the whole rest of the line, escaped like `name` and
        // `plugin`, and validated AFTER unescaping because the unescaped text is
        // what the model holds and what the next save writes back.
        //
        // Malformed -> the load fails. See validAddress for the full argument;
        // the short version is that it is structure, and a repaired address is
        // automation silently moved onto some other parameter. A *dangling*
        // address -- well-formed, naming a uid nothing answers to -- is content
        // and is kept.
        //
        // Not gated on the clip kind: an audio clip may carry lanes.
        const std::string addr = unesc(rest);
        if (!validAddress(addr)) {
            err = "env: malformed parameter address '" + addr + "'";
            return BodyKey::Bad;
        }
        c.envelopes.push_back(AutoLane{addr, {}, true});
        envPrev = st;              // a clip block, or an arrangement item's
        st = St::Env;
    } else {
        return BodyKey::No;
    }
    return BodyKey::Yes;
}

// The kind-sensitive checks, run at a clip block's terminator rather than at the
// offending line so the block may be written in any order: `note` before
// `kind midi` is still a MIDI clip, and the reader has to have seen the whole
// block before it can say otherwise. Returns the failure, or null.
//
// These are rejections, not silent repairs, because each combination describes
// content this format cannot carry and the next save would therefore throw away:
// a MIDI clip has nowhere to keep a sample path or a warp marker, and an audio
// clip has nowhere to keep notes. Guessing (promoting the clip to MIDI and
// dropping its file, or the reverse) would destroy one half of what the file
// says. Failing loudly leaves the session untouched and the file intact for the
// user to fix. Inapplicable *scalars* are the tolerated case, not this one: a
// `bpm` or `range` line inside a MIDI clip parses and is then simply not
// re-emitted, since nothing is lost that the clip was actually using.
const char* clipBodyClose(const ClipModel& c, const ClipReadState& crs) {
    if (c.kind == ClipKind::Midi && !crs.file.empty())
        return "a midi clip cannot have a 'file' line";
    if (c.kind != ClipKind::Midi && !c.notes.empty())
        return "'note' is only valid inside a midi clip";
    if (c.kind == ClipKind::Midi && !c.markers.empty())
        return "'wm' is only valid inside an audio clip";
    return nullptr;
}

// Resolves a clip once its body has been read: the sample load is deferred to
// the terminator so `file` may appear in any order. `what` names the clip for
// the log ("slot 0/3", "arrangement item 2 of track 1").
void finishClipBody(ClipModel& c, const ClipReadState& crs, f64 engineRate,
                    int& missing, const std::string& what) {
    // The reference is recorded whether or not the audio can be decoded -- that
    // is what lets the next save write the same `file` line back.
    c.path = crs.file;
    if (crs.file.empty()) return;
    c.sample = loadSample(crs.file, engineRate);
    if (!c.sample) {
        ++missing;
        LOGW("project: missing sample '%s' (%s kept, path preserved)", crs.file.c_str(), what.c_str());
        if (!crs.sawName) c.name = baseName(crs.file);
        return;
    }
    // Only fill in what the file did not state, so a project that spells out
    // every field re-saves byte-for-byte identically. An explicitly empty `name`
    // counts as stated.
    if (!crs.sawName)  c.name = c.sample->name;
    if (!crs.sawBpm)   c.clipBpm = clBpm(c.sample->guessedBpm);
    if (!crs.sawBeats) c.lengthBeats = clBeats(c.sample->guessedBeats);
    if (!crs.sawRange) { c.loopStart = 0; c.loopEnd = c.sample->frames; }
}

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
    // The signature map, written from a NORMALIZED copy rather than from the
    // model as it stands: saving must not depend on whether the caller
    // remembered to normalize, and the sorted/deduped/clamped form is the only
    // one that round-trips. Bar 0 keeps the two-token spelling every version
    // since 1 has used -- that identity is what makes the v6 -> v7 diff for a
    // set with no changes exactly the header line -- and each later change adds
    // one three-token line.
    {
        const std::vector<SigChange> m = normalizedSigMap(s.sigs, s.sigNum, s.sigDen);
        kn(o, "", "sig", std::to_string(m[0].num) + " " + std::to_string(m[0].den));
        for (size_t i = 1; i < m.size(); ++i)
            kn(o, "", "sig", std::to_string(m[i].bar) + " " +
                             std::to_string(m[i].num) + " " + std::to_string(m[i].den));
    }
    kn(o, "", "quantum",   std::to_string(clQuantum(s.quantumIdx)));
    kn(o, "", "metronome", s.metronome ? "1" : "0");
    // Always written, unlike the per-entity uids: the counter is what keeps
    // identifiers unique across a save, so "no line" would mean handing out
    // numbers that are already in use.
    kn(o, "", "nextuid",   std::to_string(clNextUid(s.nextUid)));
    kv(o, "", "name", s.name);
    // The arrangement loop brace, beside the tempo and the quantum because there
    // is one timeline and one brace. `loop <start> <end>` on one line because it
    // is one range; `loopon` separate because a disabled brace still remembers
    // where it was, which is what makes toggling it useful.
    //
    // Both sparse, and that is what keeps the v5 -> v6 diff for a set with no
    // arrangement down to the header line: the default range and a brace that is
    // off write nothing, and the values a missing line loads as are exactly the
    // ones that suppress it.
    {
        const Session deflt{};
        const f64 ls = clArrBeat(s.loopStart), le = clArrBeat(s.loopEnd);
        if (ls != clArrBeat(deflt.loopStart) || le != clArrBeat(deflt.loopEnd))
            kn(o, "", "loop", fmtF64(ls) + " " + fmtF64(le));
        if (s.loopOn) kn(o, "", "loopon", "1");
    }

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
    ClipReadState crs;
    int  missing = 0;
    // The clip a body is being read into -- a slot's, or an arrangement item's
    // `src`. St::Env is reachable from both, so the lane it appends to has to be
    // named through this rather than through ti/ci. Set when a clip block opens,
    // cleared when it closes, and dereferenced nowhere else: the only things
    // that could invalidate it (a new `track`, a new `aclip`) are keys that fail
    // the load inside a clip block, exactly as devOwner's comment argues.
    ClipModel* curClip = nullptr;
    // The arrangement item being read, for the same window and the same reason.
    ArrangeClip* curItem = nullptr;
    bool aclipSawAt = false, aclipSawLen = false;
    // What St::Env closes back to: a slot clip's block or an item's. §8.3's one
    // consequence, and the same trick openDevice already uses for devPrev.
    St envPrev = St::Clip;

    // The slot-clip flavour of finishClipBody: same resolution, plus the ghost
    // check a fixed array needs and a list does not.
    auto finishClip = [&]() {
        if (ti < 0 || ci < 0) return;
        ClipModel& c = out.tracks[(size_t)ti].slots[ci];
        finishClipBody(c, crs, engineRate, missing,
                       "slot " + std::to_string(ti) + "/" + std::to_string(ci));
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
                // Two shapes on one key, told apart by COUNT: `sig <num> <den>`
                // is bar 0 and `sig <bar> <num> <den>` is a change. That needs
                // exhausted(), for the same reason `pt` needs it -- "absent" and
                // "present but not a number" are different answers and strtoll
                // cannot tell them apart. A trailing token that is not an
                // integer, or a fourth one, is STRUCTURE and is refused; the
                // three numbers themselves are values and are clamped.
                i64 a = 0, b = 0, c = 0;
                if (!sc.integer(a) || !sc.integer(b))
                    return fail("sig: expected '<num> <den>' or '<bar> <num> <den>'");
                bool three = false;
                if (!sc.exhausted()) {
                    if (!sc.integer(c))
                        return fail("sig: expected '<num> <den>' or '<bar> <num> <den>'");
                    three = true;
                }
                if (!sc.exhausted()) return fail("sig: too many values");
                SigChange sg{};
                sg.bar = three ? clSigBar(a) : 0;
                sg.num = clSigNum((int)clampv(three ? b : a, (i64)INT32_MIN, (i64)INT32_MAX));
                sg.den = clSigDen((int)clampv(three ? c : b, (i64)INT32_MIN, (i64)INT32_MAX));
                // Appended, not resolved: normalizeSigs() below sorts, dedupes
                // last-wins and rebases, so the order lines appear in a
                // hand-edited file is the only thing that decides a tie and the
                // parser stays a parser.
                out.sigs.push_back(sg);
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
            } else if (key == "loop") {
                // Top level, beside `tempo`: there is one timeline and one
                // brace. Both numbers are required -- a range is one statement
                // with two halves and there is no sane guess for a missing one
                // -- and both are clamped. An inverted or empty range is NOT
                // repaired here: the engine reads `loopStart >= loopEnd` as "no
                // loop", so it is a value that says something, and inventing a
                // length would be inventing content.
                f64 a = 0.0, b = 0.0;
                if (!sc.num(a) || !sc.num(b)) return fail("loop: expected a start and an end");
                out.loopStart = clArrBeat(a); out.loopEnd = clArrBeat(b);
            } else if (key == "loopon") {
                int v; if (!sc.integer(v)) return fail("loopon: expected 0 or 1");
                out.loopOn = v != 0;
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
                curClip = &t.slots[ci];
                crs.reset();
                st = St::Clip;
            } else if (key == "arrangement") {
                // No index and no scalars of its own beyond the height: the
                // block exists to scope this track's timeline. It is a track's
                // block and not a top-level one precisely so that it never has
                // to re-state which track it belongs to.
                st = St::Arrange;
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
            } else if (key == "state") {
                // v8. Opaque: taken verbatim, never parsed here. `rest` is the
                // whole remainder of the line, so a state carrying anything the
                // escape layer knows about comes back exactly as written. Last
                // line wins, as a repeated scalar does everywhere in this file.
                d.state = unesc(rest);
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
                // The block's own key, not the body's: a slot clip's uid is the
                // clip's identity, while an `aclip`'s is the ITEM's. One shared
                // body handler, two different things called `uid` around it.
                u64 v; if (!sc.uid(v)) return fail("clip uid: expected an integer");
                c.uid = v;
            } else if (key == "endclip") {
                if (const char* bad = clipBodyClose(c, crs)) return fail(bad);
                finishClip();
                ci = -1;
                curClip = nullptr;
                st = St::Track;
            } else {
                std::string e;
                const BodyKey r = clipBodyKey(c, key, rest, sc, crs, st, envPrev, e);
                if (r == BodyKey::Bad) return fail(e);
                if (r == BodyKey::No)  return fail("unexpected '" + key + "' inside clip");
            }
            break;
        }
        // ---------------------------------------------------------------
        // A track's arrangement. Three keys and a terminator; everything with
        // content in it is one of the two blocks below.
        //
        // No count is enforced here, and that is the same call `device` and
        // St::Env already make: kMaxArrItems, kMaxArrNotes, kMaxArrLanes and
        // kMaxArrPoints are real ceilings, but they belong to the editor and the
        // publisher, never to the reader. Silently dropping the tail of a user's
        // timeline at load time would be the worse failure.
        case St::Arrange: {
            TrackModel& t = out.tracks[(size_t)ti];
            if (key == "arrheight") {
                f64 v; if (!sc.num(v)) return fail("arrheight: expected a number");
                t.arrHeight = clArrHeight((f32)v);
            } else if (key == "aclip") {
                // Positional, like `device`: no index, because load order IS
                // timeline order. The list is not sorted here -- the format
                // preserves the order it finds, and the sort that the engine's
                // O(1) cursor depends on happens in App::adoptSession, next to
                // the editing code that upholds the invariant.
                t.arrange.emplace_back();
                curItem = &t.arrange.back();
                curClip = &curItem->src;
                crs.reset();
                aclipSawAt = aclipSawLen = false;
                st = St::AClip;
            } else if (key == "autolane") {
                // `env`'s grammar under a different name, because these lanes
                // are absolute-timeline and a reader meeting `env` at
                // arrangement scope would have to infer the beat space from
                // context. The address rule is identical: malformed fails the
                // load, dangling is kept and written back forever.
                const std::string addr = unesc(rest);
                if (!validAddress(addr))
                    return fail("autolane: malformed parameter address '" + addr + "'");
                t.arrangeAutos.push_back(AutoLane{addr, {}, true});
                st = St::AutoLane;
            } else if (key == "endarrangement") {
                st = St::Track;
            } else {
                return fail("unexpected '" + key + "' inside arrangement");
            }
            break;
        }
        // ---------------------------------------------------------------
        // One placed item: its own seven fields, then a clip body identical to a
        // slot clip's. `curItem` is always arrange.back() -- `aclip` is reachable
        // only from St::Arrange, and the only thing that grows that vector is the
        // emplace_back that opened this block -- which is devOwner's argument
        // exactly.
        case St::AClip: {
            ArrangeClip& a = *curItem;
            if (key == "uid") {
                u64 v; if (!sc.uid(v)) return fail("aclip uid: expected an integer");
                a.uid = v;
            } else if (key == "at") {
                f64 v; if (!sc.num(v)) return fail("at: expected a number");
                a.start = clArrBeat(v); aclipSawAt = true;
            } else if (key == "len") {
                f64 v; if (!sc.num(v)) return fail("len: expected a number");
                a.length = clArrLen(v); aclipSawLen = true;
            } else if (key == "off") {
                // The item's offset into its clip. The `off` inside an `env`
                // block is a different key in a different state and takes no
                // value; see autoLaneKey.
                f64 v; if (!sc.num(v)) return fail("off: expected a number");
                a.offset = clArrBeat(v);
            } else if (key == "fadein") {
                f64 v; if (!sc.num(v)) return fail("fadein: expected a number");
                a.fadeIn = clFade(v);
            } else if (key == "fadeout") {
                f64 v; if (!sc.num(v)) return fail("fadeout: expected a number");
                a.fadeOut = clFade(v);
            } else if (key == "fadeshape") {
                i64 v; if (!sc.integer(v)) return fail("fadeshape: expected an integer");
                a.fadeShape = clFadeShape(v);
            } else if (key == "source") {
                // Provenance, and never resolved: a `source` naming a clip that
                // no longer exists dangles soft and is written back unchanged,
                // exactly as a parameter address naming a deleted device does.
                u64 v; if (!sc.uid(v)) return fail("source: expected an integer");
                a.sourceUid = v;
            } else if (key == "endaclip") {
                // `at` and `len` are the two required fields: an item with no
                // position is not an item, and a missing `at` silently meaning
                // beat 0 is exactly the failure a required field prevents.
                if (!aclipSawAt)  return fail("aclip: missing 'at'");
                if (!aclipSawLen) return fail("aclip: missing 'len'");
                if (const char* bad = clipBodyClose(a.src, crs)) return fail(bad);
                finishClipBody(a.src, crs, engineRate, missing,
                               "arrangement item " + std::to_string(out.tracks[(size_t)ti].arrange.size() - 1) +
                               " of track " + std::to_string(ti));
                curItem = nullptr;
                curClip = nullptr;
                st = St::Arrange;
            } else {
                std::string e;
                const BodyKey r = clipBodyKey(a.src, key, rest, sc, crs, st, envPrev, e);
                if (r == BodyKey::Bad) return fail(e);
                if (r == BodyKey::No)  return fail("unexpected '" + key + "' inside aclip");
            }
            break;
        }
        // ---------------------------------------------------------------
        // An arrangement automation lane. `pt` and `off` are `env`'s, through
        // the same handler, so the two cannot drift apart.
        case St::AutoLane: {
            AutoLane& lane = out.tracks[(size_t)ti].arrangeAutos.back();
            std::string e;
            const BodyKey r = autoLaneKey(lane, key, sc, e);
            if (r == BodyKey::Bad) return fail(e);
            if (r == BodyKey::No) {
                if (key != "endautolane")
                    return fail("unexpected '" + key + "' inside autolane");
                st = St::Arrange;
            }
            break;
        }
        // ---------------------------------------------------------------
        // An envelope block, opened from a clip block -- a slot's or an
        // arrangement item's -- and closing back to whichever it was. The lane is
        // always curClip->envelopes.back(): `env` is reachable only from a clip,
        // that clip stays valid for the whole block, and nothing else appends to
        // its vector in between.
        //
        // No count is enforced here. kMaxClipLanes and kMaxClipAutoPoints are
        // real ceilings, but AUTOMATION.md §2.1 puts them on the editor and the
        // publisher and says "never by the parser" -- deliberately, and it is
        // the same call `device` makes one block up: refusing a file, or
        // silently truncating a user's automation at load time, is a worse
        // failure than a set the publisher will clamp anyway. The engine's
        // fixed-width lane array is the publisher's problem, not the format's.
        case St::Env: {
            AutoLane& lane = curClip->envelopes.back();
            std::string e;
            const BodyKey r = autoLaneKey(lane, key, sc, e);
            if (r == BodyKey::Bad) return fail(e);
            if (r == BodyKey::No) {
                if (key != "endenv") return fail("unexpected '" + key + "' inside env");
                st = envPrev;
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
        const char* what = (st == St::Env)      ? "endenv"
                         : (st == St::Clip)     ? "endclip"
                         : (st == St::Device)   ? "enddevice"
                         : (st == St::AClip)    ? "endaclip"
                         : (st == St::AutoLane) ? "endautolane"
                         : (st == St::Arrange)  ? "endarrangement"
                         : (st == St::Track)    ? "endtrack"
                         : (st == St::Return)   ? "endreturn"
                         : (st == St::Master)   ? "endmaster" : "endscene";
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

    // The signature map, sorted, deduplicated last-wins, guaranteed an entry at
    // bar 0 and rebased. A v1..v6 file mentions no changes and comes out with
    // the single entry its `sig` line (or its absence) says, which is what makes
    // the whole of this feature invisible to every set that predates it. Also
    // where sigNum/sigDen get their value: the parser only ever appends, so the
    // mirror is set here, once, from the map.
    out.normalizeSigs();

    out.path = path;
    s = std::move(out);
    LOGI("loaded %zu tracks / %zu scenes from %s%s", s.tracks.size(), s.scenes.size(),
         path.c_str(), missing ? " (some samples missing)" : "");
    return true;
}

} // namespace lat
