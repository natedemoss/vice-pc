// Windows kernel APIs that need adjusting for a manually mapped title.
//
// - GetModuleHandle(NULL) / GetModuleFileName(NULL) must describe the *game*, not
//   vice-pc.exe, or the title looks for its data next to the loader.
// - File APIs are wrapped to log the paths the title touches (failures always,
//   successes for the first few), so we can see what it expects on disk.

#include "hle.h"
#include "../log.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>

namespace {

std::atomic<int> g_ok_logged{0};
std::atomic<int> g_fail_logged{0};

void log_path(const char* api, const wchar_t* path, bool ok, DWORD err) {
    if (ok) {
        if (g_ok_logged.fetch_add(1) < 60) LOG("[fs] %s(\"%ls\") ok", api, path ? path : L"(null)");
    } else if (g_fail_logged.fetch_add(1) < 300) {
        LOG("[fs] %s(\"%ls\") FAILED (%lu)", api, path ? path : L"(null)", err);
    }
}

// Xbox mounts the title's package at G:\. Map it to the dump folder.
// Returns the translated path, or the input unchanged.
std::wstring translate(const wchar_t* path) {
    if (!path) return std::wstring();
    if ((path[0] == L'G' || path[0] == L'g') && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        std::wstring out = hle_package_root();
        if (!out.empty() && out.back() != L'\\') out += L'\\';
        out += path + 3;
        return out;
    }
    // Title storage drives: T:\ (temp), S:\ and D:\ (persistent / scratch). Map each to
    // %LOCALAPPDATA%\vice-pc\<letter>, created on first use.
    wchar_t drive = static_cast<wchar_t>(towupper(path[0]));
    if ((drive == L'T' || drive == L'S' || drive == L'D') && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        wchar_t base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n && n < MAX_PATH) {
            std::wstring dir = std::wstring(base) + L"\\vice-pc";
            CreateDirectoryW(dir.c_str(), nullptr);
            dir += L'\\';
            dir += drive;
            CreateDirectoryW(dir.c_str(), nullptr);
            return dir + L"\\" + (path + 3);
        }
    }
    return path;
}

bool is_main_module(HMODULE m) {
    return m == nullptr || m == reinterpret_cast<HMODULE>(hle_image_base());
}

// ---- module identity -------------------------------------------------------

HMODULE WINAPI GetModuleHandleW_hle(LPCWSTR name) {
    if (!name) return reinterpret_cast<HMODULE>(hle_image_base());
    return GetModuleHandleW(name);
}

HMODULE WINAPI GetModuleHandleA_hle(LPCSTR name) {
    if (!name) return reinterpret_cast<HMODULE>(hle_image_base());
    return GetModuleHandleA(name);
}

DWORD WINAPI GetModuleFileNameW_hle(HMODULE m, LPWSTR buf, DWORD size) {
    if (!is_main_module(m)) return GetModuleFileNameW(m, buf, size);
    const std::wstring& p = hle_game_path();
    if (!buf || size == 0) return 0;
    DWORD n = (DWORD)std::min<size_t>(p.size(), size - 1);
    wmemcpy(buf, p.c_str(), n);
    buf[n] = 0;
    if (n < p.size()) SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return n;
}

DWORD WINAPI GetModuleFileNameA_hle(HMODULE m, LPSTR buf, DWORD size) {
    if (!is_main_module(m)) return GetModuleFileNameA(m, buf, size);
    const std::wstring& p = hle_game_path();
    if (!buf || size == 0) return 0;
    int n = WideCharToMultiByte(CP_ACP, 0, p.c_str(), (int)p.size(), buf, (int)size - 1, nullptr, nullptr);
    buf[n] = 0;
    return (DWORD)n;
}

HMODULE WINAPI LoadLibraryExW_hle(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE m = LoadLibraryExW(name, file, flags);
    LOG("[ldr] LoadLibraryExW(\"%ls\", 0x%lx) -> %p%s", name ? name : L"(null)", flags, m, m ? "" : " FAILED");
    return m;
}

HMODULE WINAPI LoadLibraryExA_hle(LPCSTR name, HANDLE file, DWORD flags) {
    HMODULE m = LoadLibraryExA(name, file, flags);
    LOG("[ldr] LoadLibraryExA(\"%s\", 0x%lx) -> %p%s", name ? name : "(null)", flags, m, m ? "" : " FAILED");
    return m;
}

// ---- file system -------------------------------------------------------------

HANDLE WINAPI CreateFileW_hle(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                              DWORD flags, HANDLE tmpl) {
    std::wstring p = translate(path);
    HANDLE h = CreateFileW(p.c_str(), access, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    log_path("CreateFileW", p.c_str(), h != INVALID_HANDLE_VALUE, err);
    SetLastError(err);
    return h;
}

DWORD WINAPI GetFileAttributesW_hle(LPCWSTR path) {
    std::wstring p = translate(path);
    DWORD a = GetFileAttributesW(p.c_str());
    DWORD err = GetLastError();
    log_path("GetFileAttributesW", p.c_str(), a != INVALID_FILE_ATTRIBUTES, err);
    SetLastError(err);
    return a;
}

DWORD WINAPI GetFileAttributesA_hle(LPCSTR path) {
    wchar_t w[MAX_PATH] = L"";
    if (path) MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH);
    std::wstring p = translate(w);
    DWORD a = GetFileAttributesW(p.c_str());
    DWORD err = GetLastError();
    log_path("GetFileAttributesA", p.c_str(), a != INVALID_FILE_ATTRIBUTES, err);
    SetLastError(err);
    return a;
}

BOOL WINAPI GetFileAttributesExW_hle(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    std::wstring p = translate(path);
    BOOL ok = GetFileAttributesExW(p.c_str(), level, info);
    DWORD err = GetLastError();
    log_path("GetFileAttributesExW", p.c_str(), ok != FALSE, err);
    SetLastError(err);
    return ok;
}

HANDLE WINAPI FindFirstFileW_hle(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    std::wstring p = translate(path);
    HANDLE h = FindFirstFileW(p.c_str(), data);
    DWORD err = GetLastError();
    log_path("FindFirstFileW", p.c_str(), h != INVALID_HANDLE_VALUE, err);
    SetLastError(err);
    return h;
}

#define FN(p) reinterpret_cast<void*>(p)
const HleExport kExports[] = {
    {"api-ms-win-core-libraryloader-l1-2-0.dll", "GetModuleHandleW", FN(&GetModuleHandleW_hle)},
    {"api-ms-win-core-libraryloader-l1-2-0.dll", "GetModuleHandleA", FN(&GetModuleHandleA_hle)},
    {"api-ms-win-core-libraryloader-l1-2-0.dll", "GetModuleFileNameW", FN(&GetModuleFileNameW_hle)},
    {"api-ms-win-core-libraryloader-l1-2-0.dll", "GetModuleFileNameA", FN(&GetModuleFileNameA_hle)},
    {"api-ms-win-core-libraryloader-l1-2-0.dll", "LoadLibraryExW", FN(&LoadLibraryExW_hle)},
    {"api-ms-win-core-libraryloader-l1-2-0.dll", "LoadLibraryExA", FN(&LoadLibraryExA_hle)},
    {"api-ms-win-core-file-l1-1-0.dll", "CreateFileW", FN(&CreateFileW_hle)},
    {"api-ms-win-core-file-l1-1-0.dll", "GetFileAttributesW", FN(&GetFileAttributesW_hle)},
    {"api-ms-win-core-file-l1-1-0.dll", "GetFileAttributesA", FN(&GetFileAttributesA_hle)},
    {"api-ms-win-core-file-l1-1-0.dll", "GetFileAttributesExW", FN(&GetFileAttributesExW_hle)},
    {"api-ms-win-core-file-l1-1-0.dll", "FindFirstFileW", FN(&FindFirstFileW_hle)},
};
#undef FN

HleRegistrar g_reg(kExports);

}  // namespace

std::wstring hle_translate_path(const wchar_t* path) { return translate(path); }
