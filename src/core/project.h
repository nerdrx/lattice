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
// Saving always writes the current version; versions 1 through 3 all load,
// through one parser, with every field a file does not mention taking its
// default.
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
