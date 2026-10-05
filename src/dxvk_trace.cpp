#include "dxvk_trace.h"
#include "logger.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace ahitvr {

void DxvkPathTrace(const char* format, ...) {
    // old dxvk paths still call this. keep them on the normal logger.
    va_list args;
    va_start(args, format);
    LogV("DXVK", format, args);
    va_end(args);
}

}
