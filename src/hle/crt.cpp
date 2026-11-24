// Fail-fast traps.
//
// abort(), terminate() and the CRT's invalid-parameter handler end in __fastfail,
// which kills the process without running exception handlers (exit 0xC0000409).
// These wrappers log who called them, with a backtrace, then do the real thing.

#include "hle.h"
#include "../log.h"

#include <windows.h>
#include <cstdlib>
#include <exception>

void hle_log_backtrace(const char* why);  // main.cpp

namespace {

void caller_line(const char* api, void* ra) {
    uint64_t rva;
    if (hle_rva(ra, &rva)) LOG("[fatal] %s called from game+0x%llx", api, (unsigned long long)rva);
    else LOG("[fatal] %s called from %p", api, ra);
}

[[noreturn]] void __cdecl abort_hle() {
    caller_line("abort()", _ReturnAddress());
    hle_log_backtrace("abort");
    abort();
}

[[noreturn]] void __cdecl terminate_hle() {
    caller_line("terminate()", _ReturnAddress());
    hle_log_backtrace("terminate");
    std::terminate();
}

[[noreturn]] void __cdecl invalid_parameter_noreturn_hle() {
    caller_line("_invalid_parameter_noinfo_noreturn()", _ReturnAddress());
    hle_log_backtrace("invalid parameter");
    _invalid_parameter_noinfo_noreturn();
}

BOOL WINAPI TerminateProcess_hle(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) {
        caller_line("TerminateProcess(self)", _ReturnAddress());
        LOG("[fatal] exit code 0x%08x", code);
        hle_log_backtrace("TerminateProcess");
    }
    return TerminateProcess(process, code);
}

const HleExport kExports[] = {
    {"api-ms-win-crt-runtime-l1-1-0.dll", "abort", reinterpret_cast<void*>(&abort_hle)},
    {"api-ms-win-crt-runtime-l1-1-0.dll", "terminate", reinterpret_cast<void*>(&terminate_hle)},
    {"api-ms-win-crt-runtime-l1-1-0.dll", "_invalid_parameter_noinfo_noreturn",
     reinterpret_cast<void*>(&invalid_parameter_noreturn_hle)},
    {"vcruntime140.dll", "__std_terminate", reinterpret_cast<void*>(&terminate_hle)},
    {"api-ms-win-core-processthreads-l1-1-0.dll", "TerminateProcess", reinterpret_cast<void*>(&TerminateProcess_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
