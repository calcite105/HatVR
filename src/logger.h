#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <cstring>

namespace ahitvr {

// HatVR central logger
// DxvkPathTrace() are compatibility wrappers only; they no longer own files.
//
// the D3D9/Vulkan proxy DLLs may still need tiny bootstrap logs before HatVR.dll
namespace log_detail {
inline FILE*& File() { static FILE* f = nullptr; return f; }
inline std::mutex& Mutex() { static std::mutex m; return m; }
inline bool& Initialized() { static bool initialized = false; return initialized; }
inline char* Path() { static char path[MAX_PATH] = {}; return path; }

inline void WritePrefix(FILE* f, const char* category) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::fprintf(f, "[%02u:%02u:%02u.%03u] [%s] ",
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        (category && *category) ? category : "CORE");
}

constexpr long long kMaxLogBytes = 8ll * 1024ll * 1024ll;
// output is centralized; individual census code already has its own caps.
constexpr bool kVerboseDiagnostics = false;
} // namespace log_detail

inline void LogInit() {
    std::lock_guard<std::mutex> lock(log_detail::Mutex());
    if (log_detail::Initialized()) return;
    log_detail::Initialized() = true;

    char modulePath[MAX_PATH] = {};
    char directory[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&LogInit), &self) &&
        GetModuleFileNameA(self, modulePath, MAX_PATH))
    {
        strcpy_s(directory, modulePath);
        char* slash = strrchr(directory, '\\');
        if (!slash) slash = strrchr(directory, '/');
        if (slash) *(slash + 1) = '\0';
        else directory[0] = '\0';
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const DWORD pid = GetCurrentProcessId();

    // Release logging uses one predictable file. Opening with "w" below
    // clears the previous run so HatVR never accumulates per-session logs.
    std::sprintf(log_detail::Path(), "%sAHiTVR.log", directory);

    fopen_s(&log_detail::File(), log_detail::Path(), "w");
    FILE* f = log_detail::File();
    if (!f) return;

    std::fprintf(f, "============================================================\n");
    std::fprintf(f, "HATVR SESSION START\n");
    std::fprintf(f, "BUILD: PHASE8.5 + CLEAN_RUNTIME_1\n");
    std::fprintf(f, "BUILD ID: 2026-09-20-CLEAN\n");
    std::fprintf(f, "FEATURES: DIRECT_SBS=1 UI_STAGING=1 GPU_ONLY=1 CENTRAL_LOG=1\n");
    std::fprintf(f, "PROCESS ID: %lu\n", static_cast<unsigned long>(pid));
    std::fprintf(f, "LOCAL START: %04u-%02u-%02u %02u:%02u:%02u.%03u\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::fprintf(f, "LOG FILE: %s\n", log_detail::Path());
    if (modulePath[0])
        std::fprintf(f, "MODULE: %s\n", modulePath);
    std::fprintf(f, "LOGGING: centralized; verbose diagnostics disabled\n");
    std::fprintf(f, "============================================================\n");
    std::fflush(f);
}

inline bool IsReleaseLogNoise(const char* fmt) {
    if (!fmt) return true;
    // subsystem, but far too hot for a normal player log (many run per draw/frame).
    static const char* noisy[] = {
        "V13 PRESENT stage=", "V41 SETVP", "V41 SCISSOR",
        "PHASE8.3 CALC-STEP", "CAMERA HASH PROBE", "c0  = [", "c1  = [",
        "c2  = [", "c3  = [", "c4  = [", "c5  = [", "c6  = [",
        "c7  = [", "c8  = [", "c9  = [", "c10 = [", "c11 = [",
        "c12 = [", "c13 = [", "c14 = [", "c15 = [",
        "FLUSH hit=", "DIPUP48 hit=", "PERIODIC GetPlayerViewPoint",
        "PERIODIC CalcSceneView", "FP_V126_ROT", "V38 SOURCE-VIEWS",
        "FP_OCCLUSION] semantic match", "V10 BeginOpenXRFrameForRender call="
    };
    for (const char* p : noisy) if (std::strstr(fmt, p)) return true;
    return false;
}

inline void LogV(const char* category, const char* fmt, va_list args) {
    if (!fmt || IsReleaseLogNoise(fmt)) return;
    if (!log_detail::Initialized()) LogInit();

    std::lock_guard<std::mutex> lock(log_detail::Mutex());
    FILE* f = log_detail::File();
    if (!f) return;

    const long pos = std::ftell(f);
    if (pos >= 0 && static_cast<long long>(pos) >= log_detail::kMaxLogBytes)
        return;

    log_detail::WritePrefix(f, category);
    std::vfprintf(f, fmt, args);

    // blank line while keeping printf-style callers convenient.
    const size_t n = std::strlen(fmt);
    if (n == 0 || fmt[n - 1] != '\n')
        std::fputc('\n', f);

    std::fflush(f);
}

inline void LogCategory(const char* category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(category, fmt, args);
    va_end(args);
}

// Old generic logger: retained so existing call sites compile, but it is now
// just the CORE category of the single session log.
inline void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV("CORE", fmt, args);
    va_end(args);
}

inline void DiagnosticLog(const char* text) {
    if (!log_detail::kVerboseDiagnostics || !text || !*text) return;
    LogCategory("DIAG", "%s", text);
}

} // namespace ahitvr
