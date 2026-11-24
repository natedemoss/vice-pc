#pragma once

// Vtable patching for real PC D3D12 objects handed to the title.
//
// The Xbox D3D12 interfaces keep the PC method order (verified: device slot 26 is
// called with GetCustomHeapProperties(nodeMask=1, HEAP_TYPE_DEFAULT) arguments), so
// the title can call real PC objects directly, with two fixes:
//
//  1. Struct-returning methods: the title passes the hidden return buffer *before*
//     `this` (Itanium-style sret); the MSVC-built PC runtime expects `this` first.
//     Those slots get a thunk that swaps rcx/rdx, only when the caller is game code.
//  2. Slots past the end of the PC interface are Xbox-only methods: they become
//     logging stubs (and honour --stop-on-stub).
//
// Each distinct original vtable is copied once; the object's vptr is redirected to
// the copy, so the object itself stays a genuine PC object (no unwrapping needed).


// The title calls D3D through the C interface (This->lpVtbl->Method(This, ...)), so
// struct returns follow the Windows x64 *C* convention, not MSVC's C++ one:
//   - structs of 8 bytes (descriptor handles, LUID) come back in RAX, no hidden pointer
//   - larger structs: hidden return pointer first (rcx), `this` second (rdx)
// The MSVC-built PC runtime always takes `this` first and a hidden pointer second.
struct VtInterface {
    const char* name;
    int pc_slots;   // number of methods in the PC interface we map to
    int sret[8];    // slots returning a struct > 8 bytes (swap rcx/rdx); 0 = unused entry
    int ret8[4];    // slots returning an 8-byte struct (return in RAX); 0 = unused entry
};

// Known interfaces (slot counts / sret slots from d3d12.h).
extern const VtInterface kVtDevice;
extern const VtInterface kVtCommandQueue;
extern const VtInterface kVtCommandAllocator;
extern const VtInterface kVtGraphicsCommandList;
extern const VtInterface kVtDescriptorHeap;
extern const VtInterface kVtResource;
extern const VtInterface kVtHeap;
extern const VtInterface kVtFence;
extern const VtInterface kVtRootSignature;
extern const VtInterface kVtPipelineState;
extern const VtInterface kVtQueryHeap;
extern const VtInterface kVtCommandSignature;
extern const VtInterface kVtGeneric;   // unknown: IUnknown + ID3D12Object only

// Patch `obj` for title use (idempotent). Returns obj.
void* vt_patch(void* obj, const VtInterface& iface);

// Override a slot on the patched vtable for `obj`'s class (call after vt_patch).
void vt_override(void* obj, int slot, void* fn);

// Original (unpatched) function for `slot` of a patched object.
void* vt_original(void* obj, int slot);

// True if obj's vptr is one of our patched tables (a live, title-facing object).
bool vt_is_patched(const void* obj);
