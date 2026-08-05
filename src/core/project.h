// Project persistence.
//
// The on-disk format is line-oriented plain text rather than binary or JSON:
// a set that a user can diff, grep, and hand-repair after a bad merge is worth
// far more than the bytes a packed layout would save, and it keeps the core
// free of a serialization dependency.
//
// Version 2 of the format adds stable identifiers (`nextuid` plus a `uid` on
// every track, scene and clip), the clip's generative fields, and a track's
// device chain. Saving always writes version 2; version 1 files load unchanged,
// with every addition taking its default. A device is read and written only as
// the passive SavedDevice form, so nothing here knows how to instantiate a
// plugin.
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
