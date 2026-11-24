#include "vtpatch.h"

#include "../hle.h"
#include "../../log.h"
#include "../../stubs.h"

#include <windows.h>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

// Slot counts per interface, counting IUnknown (3) + ID3D12Object (4) + ID3D12DeviceChild (1).
// sret slots are the methods that return a struct by value.
const VtInterface kVtDevice = {"ID3D12Device", 79, {25, 26, 56, 68}, {43}};  // up to ID3D12Device10
const VtInterface kVtCommandQueue = {"ID3D12CommandQueue", 19, {18}};
const VtInterface kVtCommandAllocator = {"ID3D12CommandAllocator", 9, {}};
const VtInterface kVtGraphicsCommandList = {"ID3D12GraphicsCommandList", 79, {}};  // up to List7
const VtInterface kVtDescriptorHeap = {"ID3D12DescriptorHeap", 11, {8}, {9, 10}};
const VtInterface kVtResource = {"ID3D12Resource", 17, {10, 16}};
const VtInterface kVtHeap = {"ID3D12Heap", 10, {8}};
const VtInterface kVtFence = {"ID3D12Fence", 11, {}};
const VtInterface kVtRootSignature = {"ID3D12RootSignature", 8, {}};
const VtInterface kVtPipelineState = {"ID3D12PipelineState", 9, {}};
const VtInterface kVtQueryHeap = {"ID3D12QueryHeap", 8, {}};
const VtInterface kVtCommandSignature = {"ID3D12CommandSignature", 8, {}};
const VtInterface kVtGeneric = {"ID3D12Object", 7, {}};

namespace {

constexpr int kExtraSlots = 96;  // room for Xbox-only methods past the PC interface

struct Patched {
    void** orig;
    void** table;
    int size;
};

std::mutex g_mu;
std::unordered_map<void**, Patched*>& by_orig = *new std::unordered_map<void**, Patched*>();
std::unordered_map<void**, Patched*>& by_table = *new std::unordered_map<void**, Patched*>();

uint8_t* g_code = nullptr;
size_t g_code_used = 0;
constexpr size_t kCodePage = 64 * 1024;

uint8_t* alloc_code(size_t n) {
    if (!g_code || g_code_used + n > kCodePage) {
        g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCodePage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        g_code_used = 0;
    }
    uint8_t* p = g_code + g_code_used;
    g_code_used += (n + 15) & ~size_t(15);
    return p;
}

// Swap rcx/rdx when called from game code, then tail-jump to `target`.
void* make_sret_thunk(void* target) {
    uint64_t lo = reinterpret_cast<uint64_t>(hle_image_base());
    uint64_t hi = lo + hle_image_size();
    uint8_t* c = alloc_code(64);
    uint8_t* p = c;
    auto emit = [&](std::initializer_list<uint8_t> b) { for (auto x : b) *p++ = x; };
    auto imm64 = [&](uint64_t v) { memcpy(p, &v, 8); p += 8; };
    emit({0x48, 0x8B, 0x04, 0x24});           // mov rax, [rsp]   (return address)
    emit({0x49, 0xBA}); imm64(lo);            // mov r10, image_lo
    emit({0x4C, 0x39, 0xD0});                 // cmp rax, r10
    emit({0x72, 0x12});                       // jb  native
    emit({0x49, 0xBA}); imm64(hi);            // mov r10, image_hi
    emit({0x4C, 0x39, 0xD0});                 // cmp rax, r10
    emit({0x73, 0x03});                       // jae native
    emit({0x48, 0x87, 0xCA});                 // xchg rdx, rcx
    emit({0x48, 0xB8}); imm64(reinterpret_cast<uint64_t>(target));  // native: mov rax, target
    emit({0xFF, 0xE0});                       // jmp rax
    return c;
}

// 8-byte struct return: from game code, call target(this, &tmp) and return tmp in RAX.
void* make_ret8_thunk(void* target) {
    uint64_t lo = reinterpret_cast<uint64_t>(hle_image_base());
    uint64_t hi = lo + hle_image_size();
    uint8_t* c = alloc_code(80);
    uint8_t* p = c;
    auto emit = [&](std::initializer_list<uint8_t> b) { for (auto x : b) *p++ = x; };
    auto imm64 = [&](uint64_t v) { memcpy(p, &v, 8); p += 8; };
    emit({0x48, 0x8B, 0x04, 0x24});           // mov rax, [rsp]
    emit({0x49, 0xBA}); imm64(lo);            // mov r10, image_lo
    emit({0x4C, 0x39, 0xD0});                 // cmp rax, r10
    emit({0x72, 0x2E});                       // jb  native
    emit({0x49, 0xBA}); imm64(hi);            // mov r10, image_hi
    emit({0x4C, 0x39, 0xD0});                 // cmp rax, r10
    emit({0x73, 0x1F});                       // jae native
    emit({0x48, 0x83, 0xEC, 0x38});           // sub rsp, 38h   (16-byte aligned for the call)
    emit({0x48, 0x8D, 0x54, 0x24, 0x28});     // lea rdx, [rsp+28h]  (hidden return slot)
    emit({0x48, 0xB8}); imm64(reinterpret_cast<uint64_t>(target));  // mov rax, target
    emit({0xFF, 0xD0});                       // call rax
    emit({0x48, 0x8B, 0x44, 0x24, 0x28});     // mov rax, [rsp+28h]
    emit({0x48, 0x83, 0xC4, 0x38});           // add rsp, 38h
    emit({0xC3});                             // ret
    emit({0x48, 0xB8}); imm64(reinterpret_cast<uint64_t>(target));  // native: mov rax, target
    emit({0xFF, 0xE0});                       // jmp rax
    return c;
}

Patched* lookup_obj(void* obj) {
    void** vt = *static_cast<void***>(obj);
    auto it = by_table.find(vt);
    return it == by_table.end() ? nullptr : it->second;
}

}  // namespace

void* vt_patch(void* obj, const VtInterface& iface) {
    if (!obj) return obj;
    std::lock_guard<std::mutex> lk(g_mu);
    void** vt = *static_cast<void***>(obj);
    if (by_table.count(vt)) return obj;  // already patched
    Patched* p;
    auto it = by_orig.find(vt);
    if (it != by_orig.end()) {
        p = it->second;
    } else {
        p = new Patched();
        p->orig = vt;
        p->size = iface.pc_slots + kExtraSlots;
        p->table = new void*[p->size];
        for (int i = 0; i < iface.pc_slots; ++i) p->table[i] = vt[i];
        int n_sret = 0, n_ret8 = 0;
        for (int s : iface.sret) {
            if (s > 0 && s < iface.pc_slots) { p->table[s] = make_sret_thunk(vt[s]); ++n_sret; }
        }
        for (int s : iface.ret8) {
            if (s > 0 && s < iface.pc_slots) { p->table[s] = make_ret8_thunk(vt[s]); ++n_ret8; }
        }
        for (int i = iface.pc_slots; i < p->size; ++i) {
            std::string n = "slot" + std::to_string(i) + " (Xbox-only)";
            p->table[i] = stub_create(iface.name, _strdup(n.c_str()), stub_stop_mode());
        }
        by_orig[vt] = p;
        by_table[p->table] = p;
        LOG("[d3d12] patched %s vtable (%d PC slots, %d sret, %d ret8)", iface.name, iface.pc_slots, n_sret, n_ret8);
    }
    *static_cast<void***>(obj) = p->table;
    return obj;
}

void vt_override(void* obj, int slot, void* fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    Patched* p = lookup_obj(obj);
    if (p && slot >= 0 && slot < p->size) p->table[slot] = fn;
}

void* vt_original(void* obj, int slot) {
    std::lock_guard<std::mutex> lk(g_mu);
    Patched* p = lookup_obj(obj);
    if (p) return p->orig[slot];
    return (*static_cast<void***>(obj))[slot];
}
