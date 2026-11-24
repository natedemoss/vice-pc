// VCRUNTIME140.dll!_CxxThrowException override.
//
// The real implementation finds the thrower's image base with RtlPcToFileHeader,
// which only knows about modules the OS loader mapped. For our manually mapped
// image it returns NULL, so every type in the ThrowInfo resolves against base 0
// and no catch clause ever matches. We raise the same exception ourselves with
// the correct image base.

#include "hle.h"

#include <windows.h>

namespace {

constexpr DWORD kCxxExceptionCode = 0xE06D7363;     // 'msc' | 0xE0000000
constexpr ULONG_PTR kMagic = 0x19930520;            // EH_MAGIC_NUMBER1
constexpr ULONG_PTR kPureMagic = 0x01994000;        // EH_PURE_MAGIC_NUMBER1
constexpr unsigned kTiIsPure = 0x8;

using ThrowFn = void(__stdcall*)(void*, void*);

ThrowFn real_throw() {
    static ThrowFn fn = reinterpret_cast<ThrowFn>(
        GetProcAddress(GetModuleHandleW(L"vcruntime140.dll"), "_CxxThrowException"));
    return fn;
}

[[noreturn]] void __stdcall CxxThrowException_hle(void* object, void* throw_info) {
    if (!object && !throw_info) {
        // `throw;` (rethrow) does not use the image base; let the runtime handle it.
        real_throw()(object, throw_info);
    }
    unsigned attributes = throw_info ? *static_cast<unsigned*>(throw_info) : 0;
    ULONG_PTR params[4] = {
        (attributes & kTiIsPure) ? kPureMagic : kMagic,
        reinterpret_cast<ULONG_PTR>(object),
        reinterpret_cast<ULONG_PTR>(throw_info),
        reinterpret_cast<ULONG_PTR>(hle_image_base()),
    };
    RaiseException(kCxxExceptionCode, EXCEPTION_NONCONTINUABLE, 4, params);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);  // unreachable
}

const HleExport kExports[] = {
    {"vcruntime140.dll", "_CxxThrowException", reinterpret_cast<void*>(&CxxThrowException_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
