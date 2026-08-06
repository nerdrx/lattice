// Project persistence.
//
// The on-disk format is line-oriented plain text rather than binary or JSON:
// a set that a user can diff, grep, and hand-repair after a bad merge is worth
// far more than the bytes a packed layout would save, and it keeps the core
// free of a serialization dependency.
//
// Version 2 of the format adds stable identifiers (`nextuid` plus a `uid` on
// every track, scene and clip), the clip's generative fields, and a track's
// device chain. A device is read and written only as the passive SavedDevice
// form, so nothing here knows how to instantiate a plugin.
//
// Version 3 adds MIDI clips. A clip block gains a `kind midi` line and, after
// its scalars, one `note <beat> <length> <pitch> <velocity>` line per note.
// Audio clips write neither line, so a set without MIDI in it produces the same
// bytes version 2 produced apart from the header. A MIDI clip in turn omits the
// four fields that only describe sample playback (`file`, `warp`, `bpm`,
// `range`) and keeps everything musical, `beats` -- the clip length the piano
// roll edits -- included. Notes are written and read in vector order; keeping
// them sorted by beat is the editor's job, not the format's.
//
// Version 4 adds the mixer's bus topology. A track gains sparse
// `send <idx> <level>` lines (only the buses it actually feeds), and two new
// top-level blocks sit between the tracks and the scenes: `return <idx>` ...
// `endreturn`, carrying a uid, a name, a fader and a device chain, and one
// `master` ... `endmaster` carrying nothing but a device chain. Both are
// emitted only when they hold something -- a return that is still the default
// bus, and an empty master chain, write nothing at all -- so a set that uses
// none of this produces the same bytes version 3 produced apart from the
// header. Devices in a return or on the master are the same passive
// SavedDevice blocks a track writes, read and written through the same code.
//
// Version 5 adds clip automation. A clip block gains, after its notes, zero or
// more `env <address>` ... `endenv` blocks, each holding an optional `off` line
// (the lane is deactivated) and one `pt <beat> <value> [curve]` line per
// breakpoint. A clip with no envelopes emits nothing, an envelope with no
// points is dropped, and the curve byte is written only when non-zero -- so a
// set that uses none of this again produces the same bytes version 4 produced
// apart from the header. Envelopes are NOT gated on clip kind: an audio clip
// may carry them too.
//
// Version 6 adds the arrangement (docs/ARRANGEMENT.md §8) and warp markers.
// Two top-level lines, `loop <start> <end>` and `loopon <0|1>`, carry the one
// timeline's brace. A track gains an `arrangement` ... `endarrangement` block
// after its clips, holding a sparse `arrheight`, zero or more POSITIONAL
// `aclip` ... `endaclip` blocks -- an item's `at`/`len`/`off`/`fadein`/
// `fadeout`/`fadeshape`/`source`, then a clip body byte-for-byte identical to a
// slot clip's, written and read by the same two functions -- and zero or more
// `autolane <address>` ... `endautolane` blocks, whose `pt` and `off` lines are
// `env`'s verbatim in absolute timeline beats. A clip gains `wm <srcFrame>
// <beat>` lines, one per warp marker, on audio clips only.
//
// Every construct is sparse: a set with no arrangement writes no block, no
// brace and no height, and a clip with no markers writes no `wm` -- so such a
// set again produces the same bytes version 5 produced apart from the header.
// The one thing the format does NOT do is sort: `aclip` order is file order
// here, and the sort the engine's cursor depends on belongs to App::adoptSession,
// beside the editing code that upholds the invariant.
//
// Version 7 adds time-signature CHANGES, on the `sig` line that was already
// there. `sig <num> <den>` still means the session signature at bar 0 and is
// still written exactly as it was; `sig <bar> <num> <den>` adds a change, one
// sparse line each, sorted and deduplicated last-wins on load. A set in one
// signature -- which is every set that existed before this version -- therefore
// writes the identical bytes version 6 wrote apart from the header line.
// Numerators are 1..32 and denominators are powers of two in 1..32; a
// denominator between two powers rounds down, so the clamp is stable across a
// re-save.
//
// Version 8 adds one optional `state <opaque>` line inside a `device` block:
// whatever that device needs to describe itself beyond its parameters, as a
// single line of text this layer stores verbatim and never parses. Exactly one
// device writes it today -- `nxtakt:rack` puts its entire contents there -- and
// the format deliberately does not know that. A device with no state writes no
// line, so a set with no rack in it again produces the identical bytes version
// 7 produced apart from the header. A state string the device layer cannot make
// sense of is not a parse error: it round-trips, and the device drops what it
// cannot use, exactly as a `param` naming a control that no longer exists is
// dropped.
//
// Saving always writes the current version; versions 1 through 8 all load,
// through one parser, with every field a file does not mention taking its
// default.
//
// NAMING, after the Lattice -> NxTakt rename:
//
//   * The header word is `nxtakt` on write and `nxtakt` OR `lattice` on read,
//     forever. It doubles as the format's magic, so this is what keeps every
//     already-saved set loadable. The full argument is at kHeaderWord in
//     project.cpp.
//   * The file EXTENSION stays `.lattice`. This is a deliberate hold, not an
//     oversight. The extension is not a compatibility problem the way the
//     header word is -- the loader never looks at it -- but changing it is a
//     user-facing problem: it splits a user's own set folder into two
//     extensions with no way to tell which build wrote which, it invalidates
//     whatever file-manager association and shell glob they have built up, and
//     it means `open recent` and muscle memory stop agreeing. A rename pass is
//     the wrong moment to spend that. If it does move (`.nxt` is the obvious
//     candidate), the change is: write the new extension, keep opening both,
//     and offer the new one as the default in the save dialog only.
//     Tracked as open; nothing in the codebase depends on the decision either
//     way, because nothing dispatches on the extension.
#pragma once
#include "common.h"
#include <string>

namespace lat {

struct Session;   // src/ui/app.h

// Writes `s` to `path`. The file is written to a sibling temp file and renamed
// so a failure halfway through never destroys the previous good project.
// On success `s.path` is updated to `path`.
bool saveProject(const Session& s, const std::string& path, std::string* err);

// Replaces the whole contents of `s` with the project at `path`. Audio is
// decoded at `engineRate` up front, exactly as the browser does it.
//
// A clip whose audio file has gone missing is *kept* (with a null sample) and
// logged; losing a whole set because one sample moved is never the right call.
// Its ClipModel::path is filled in regardless of whether the audio decoded, so
// the reference survives the next save and the set repairs itself once the file
// comes back.
// On a genuine parse error `s` is left untouched, *err is set, and false is
// returned.
bool loadProject(Session& s, const std::string& path, f64 engineRate, std::string* err);

} // namespace lat
