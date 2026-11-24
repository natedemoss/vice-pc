// xg_xs.dll — Xbox texture layout library (tiling / swizzle modes).
//
// The reference title imports two functions:
//
//   XGSuggestSwizzleMode(...)             -> swizzle mode for a texture
//   XGCreateTextureComputer(desc*, out**) -> "address computer" COM object
//
// The title uses the computer to write linear texel data into the console's tiled
// layout: per subresource it calls slot 8 as
//   (this, dest_base, plane = 0, subresource, src, row_pitch, slice_pitch)
// and releases it afterwards (game+0x16f71fb).
//
// On PC the driver owns texture layouts, so everything is kept linear: swizzle mode
// 0 and a computer whose methods are logged. Slot 8 is a no-op for now; texture
// uploads get a real path once the title creates its textures.

#include "hle.h"
#include "probe.h"
#include "../log.h"

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace {

std::atomic<int> g_logged{0};

uint32_t __cdecl XGSuggestSwizzleMode_hle(uint64_t a, uint64_t b, uint64_t c) {
    if (g_logged.fetch_add(1) < 20) LOG("[xg] XGSuggestSwizzleMode(0x%llx, 0x%llx, 0x%llx) -> 0 (linear)",
                                        (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    return 0;
}

HRESULT STDMETHODCALLTYPE Computer_CopyIntoSubresource(ProbeObject*, void* dest, uint32_t plane, uint32_t sub,
                                                       const void* src, uint32_t row_pitch, uint32_t slice_pitch) {
    if (g_logged.fetch_add(1) < 40)
        LOG("[xg] TextureComputer::CopyIntoSubresource(dest=%p plane=%u sub=%u src=%p row=%u slice=%u) (skipped)",
            dest, plane, sub, src, row_pitch, slice_pitch);
    return S_OK;
}

HRESULT __cdecl XGCreateTextureComputer_hle(const uint32_t* desc, void** out) {
    if (g_logged.fetch_add(1) < 40 && desc) {
        LOG("[xg] XGCreateTextureComputer(desc: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x)",
            desc[0], desc[1], desc[2], desc[3], desc[4], desc[5], desc[6], desc[7], desc[8], desc[9], desc[10],
            desc[11], desc[12], desc[13]);
    }
    if (!out) return E_POINTER;
    static ProbeType* type = [] {
        ProbeType* t = probe_type("XGTextureAddressComputer");
        probe_override(t, 8, reinterpret_cast<void*>(&Computer_CopyIntoSubresource), "CopyIntoSubresource");
        return t;
    }();
    *out = probe_create(type);
    return S_OK;
}

const HleExport kExports[] = {
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&XGSuggestSwizzleMode_hle)},
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
