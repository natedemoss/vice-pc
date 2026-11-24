#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

static CRITICAL_SECTION g_lock;
static bool g_lock_ready = false;
static FILE* g_file = nullptr;

void log_init(const wchar_t* file_path) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    if (file_path && *file_path) {
        _wfopen_s(&g_file, file_path, L"w");
    }
}

void log_write(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    if (g_lock_ready) EnterCriticalSection(&g_lock);
    fputs(buf, stdout);
    fputc('\n', stdout);
    if (g_file) {
        fputs(buf, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

void log_flush() {
    fflush(stdout);
    if (g_file) fflush(g_file);
}
