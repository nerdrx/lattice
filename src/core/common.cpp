#include "common.h"
#include <cstdarg>
#include <ctime>

namespace lat {

void logImpl(const char* lvl, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[lattice %s] %s\n", lvl, msg);
}

} // namespace lat
