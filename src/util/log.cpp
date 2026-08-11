#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace mithril {
namespace util {

namespace {
// Defaults to Debug so [MR-DBG ] traces (texture uploads / shader
// compilation / draws) are emitted on device without requiring a launcher
// environment override. MITHRIL_LOG_LEVEL still selects the floor when set:
// 0=Error 1=Warn 2=Info 3=Debug.
int g_min_level = [] {
    if (const char* e = std::getenv("MITHRIL_LOG_LEVEL")) {
        char* end = nullptr;
        long v = std::strtol(e, &end, 10);
        if (end != e && v >= 0 && v <= 3) return (int)v;
    }
    return 3;  // Debug
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