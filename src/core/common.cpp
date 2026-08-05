#include "common.h"
#include <cstdarg>
#include <cstdlib>
#include <ctime>

namespace lat {

// See the declaration in common.h. The two lookups are done on fixed-size
// stack buffers rather than std::string so this is callable from anywhere,
// including the daemon's early startup before anything else exists.
const char* env(const char* suffix) {
    if (!suffix || !*suffix) return nullptr;
    char buf[128];
    std::snprintf(buf, sizeof buf, "NXTAKT_%s", suffix);
    if (const char* v = std::getenv(buf)) return v;
    std::snprintf(buf, sizeof buf, "LATTICE_%s", suffix);   // pre-rename spelling
    return std::getenv(buf);
}

void logImpl(const char* lvl, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[nxtakt %s] %s\n", lvl, msg);
}

} // namespace lat
