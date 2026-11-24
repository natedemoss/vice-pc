#pragma once

#include <windows.h>

// Creates (once) the real PC D3D12 device the title talks to, with its vtable patched
// for title use. Returns an AddRef'd pointer in *out.
HRESULT d3d12_create_title_device(void** out);
