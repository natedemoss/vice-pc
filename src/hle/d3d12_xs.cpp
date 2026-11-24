// d3d12_xs.dll — the Xbox flavour of Direct3D 12.
//
// Only the pieces the reference title needs to get past startup live here for now.
// The real work (D3D12XboxCreateDevice → PC D3D12 device wrapper) is milestone 4.

#include "hle.h"
#include "../log.h"

#include <windows.h>
#include <cstdint>

namespace {

// HRESULT D3D12ConfigureVirtualMemoryX(const <32-byte config>* config)
// The title zero-fills a 32-byte struct, sets the dword at +4 to a value derived
// from its GPU heap size, and treats any failing HRESULT as fatal. On PC the
// D3D12 runtime manages GPU virtual memory itself, so there is nothing to configure.
HRESULT __cdecl D3D12ConfigureVirtualMemoryX_hle(const uint32_t* config) {
    LOG("[d3d12_xs] D3D12ConfigureVirtualMemoryX(+0=0x%x, +4=0x%x) -> S_OK", config ? config[0] : 0,
        config ? config[1] : 0);
    return S_OK;
}

const HleExport kExports[] = {
    {"d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX", reinterpret_cast<void*>(&D3D12ConfigureVirtualMemoryX_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
