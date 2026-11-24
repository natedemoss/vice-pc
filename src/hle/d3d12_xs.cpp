// d3d12_xs.dll — the Xbox flavour of Direct3D 12.
//
// Only the pieces the reference title needs to get past startup live here for now.
// The real work (D3D12XboxCreateDevice → PC D3D12 device wrapper) is milestone 4.

#include "hle.h"
#include "probe.h"
#include "d3d12/device.h"
#include "../log.h"

#include <windows.h>
#include <d3d12.h>
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

// HRESULT D3D12XboxCreateDevice(IGraphicsUnknown* adapter, const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params,
//                               REFIID riid, void** device)
// Reference title: adapter = NULL, params->Version = 0x20004, riid = {5405C344-D457-444E-B4DD-2366E45AEE39}
// (an Xbox-only device IID). The device is copied into ~10 globals (one per interface version).
// For now the device is a probe: every method call is logged so slots can be matched to
// the PC ID3D12Device vtable and forwarded to a real device.
HRESULT __cdecl D3D12XboxCreateDevice_hle(void* adapter, const uint32_t* params, const GUID* riid, void** device) {
    LOG("[d3d12_xs] D3D12XboxCreateDevice(adapter=%p, riid={%08lX-...}) params:", adapter, riid ? riid->Data1 : 0);
    if (params) {
        for (int i = 0; i < 16; i += 4) {
            LOG("            +%02x: %08x %08x %08x %08x", i * 4, params[i], params[i + 1], params[i + 2], params[i + 3]);
        }
    }
    if (!device) return E_POINTER;
    HRESULT hr = d3d12_create_title_device(device);
    LOG("[d3d12_xs] D3D12XboxCreateDevice -> 0x%08lx (real PC device, patched vtable)", hr);
    return hr;
}

// HRESULT D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob** blob,
//                                              ID3DBlob** error)
// Same signature as PC d3d12.dll: forward it. Failures are logged with the runtime's error text.
HRESULT __cdecl D3D12SerializeVersionedRootSignature_hle(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                         ID3DBlob** blob, ID3DBlob** error) {
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(desc, blob, &err);
    static int logged = 0;
    if (FAILED(hr) || logged++ < 20) {
        UINT flags = 0, params = 0;
        if (desc && desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
            flags = desc->Desc_1_1.Flags;
            params = desc->Desc_1_1.NumParameters;
        } else if (desc) {
            flags = desc->Desc_1_0.Flags;
            params = desc->Desc_1_0.NumParameters;
        }
        LOG("[d3d12_xs] D3D12SerializeVersionedRootSignature(v=0x%x, %u params, flags=0x%x) -> 0x%08lx%s%s",
            desc ? desc->Version : 0, params, flags, hr, err ? ": " : "",
            err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    }
    if (error) *error = err;
    else if (err) err->Release();
    return hr;
}

const HleExport kExports[] = {
    {"d3d12_xs.dll", "D3D12SerializeVersionedRootSignature",
     reinterpret_cast<void*>(&D3D12SerializeVersionedRootSignature_hle)},
    {"d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX", reinterpret_cast<void*>(&D3D12ConfigureVirtualMemoryX_hle)},
    {"d3d12_xs.dll", "D3D12XboxCreateDevice", reinterpret_cast<void*>(&D3D12XboxCreateDevice_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
