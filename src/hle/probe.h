#pragma once

// Probe COM objects: stand-ins for Xbox-only COM interfaces whose layout isn't public.
//
// Every vtable slot logs its index, first arguments and the game address that called
// it, then returns S_OK (0). Slots 0-2 implement IUnknown for real. Once a slot's
// meaning is known (from the log + disassembly of the call site), override it with a
// real implementation via probe_override().

#include <cstdint>

constexpr int kProbeSlots = 192;  // D3D12 device interfaces have 80+ methods; Xbox adds more

struct ProbeType;

struct ProbeObject {
    void** vtbl;      // must be first: this is what the game dereferences
    ProbeType* type;
    volatile long refs;
    void* user;       // per-object state for overridden slots
};

// Returns the type registered under `name`, creating it with logging defaults.
ProbeType* probe_type(const char* name);

// New object with refcount 1.
ProbeObject* probe_create(ProbeType* type, void* user = nullptr);

// Replace slot `index` (>= 3) with `fn`. `fn` receives the ProbeObject* as `this`.
void probe_override(ProbeType* type, int index, void* fn, const char* slot_name);

const char* probe_type_name(const ProbeType* type);
