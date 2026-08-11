#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace mithril {
namespace util {

namespace {
// Defaults to Info. MITHRIL_LOG_LEVEL overrides: 0=Error 1=Warn 2=Info
// 3=Debug (set e.g. 3 in the launcher's environment to trace texture
// uploads / shader compilation / draws on device).
int g_min_level = [] {
    if (const char* e = std::getenv("MITHRIL_LOG_LEVEL")) {
        char* end = nullptr;
        long v = std::strtol(e, &end, 10);
        if (end != e && v >= 0 && v <= 3) return (int)v;
    }
    return 2;  // Info
}();
}

void Log(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) > g_min_level)
        return;

    static const char* kPrefix[] = {"[MR-ERR] ", "[MR-WARN]", "[MR-INFO]", "[MR-DBG ]"};
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s %s\n", kPrefix[static_cast<int>(level)], buf);
}

} // namespace util
} // namespace mithril