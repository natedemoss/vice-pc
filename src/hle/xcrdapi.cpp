// XCRDAPI.dll — Xbox DirectStorage.
//
// HRESULT DStorageGetFactory(REFIID riid, void** factory)
//
// The reference title calls this once during startup, treats any failure as fatal,
// keeps the factory in a global and immediately calls vtable slot 8 with 0x2000000
// (32 MB, most likely SetStagingBufferSize). The Xbox factory/queue layouts aren't
// public, so for now the factory is a probe object that logs every call. Slots get
// real file-backed implementations as the call sites are worked out.

#include "hle.h"
#include "probe.h"
#include "../log.h"

#include <windows.h>
#include <cstdio>

namespace {

HRESULT __cdecl DStorageGetFactory_hle(const GUID* riid, void** factory) {
    if (riid) {
        LOG("[dstorage] DStorageGetFactory({%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}) -> probe factory",
            riid->Data1, riid->Data2, riid->Data3, riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3],
            riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7]);
    }
    if (!factory) return E_POINTER;
    *factory = probe_create(probe_type("IDStorageFactoryX"));
    return S_OK;
}

const HleExport kExports[] = {
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&DStorageGetFactory_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
