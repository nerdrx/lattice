// Project persistence.
//
// The on-disk format is line-oriented plain text rather than binary or JSON:
// a set that a user can diff, grep, and hand-repair after a bad merge is worth
// far more than the bytes a packed layout would save, and it keeps the core
// free of a serialization dependency.
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
// On a genuine parse error `s` is left untouched, *err is set, and false is
// returned.
bool loadProject(Session& s, const std::string& path, f64 engineRate, std::string* err);

} // namespace lat
