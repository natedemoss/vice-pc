// XCRDAPI.dll — Xbox DirectStorage.
//
// HRESULT DStorageGetFactory(REFIID riid, void** factory)
//
// The Xbox factory/queue layouts aren't public, so the objects are probe objects
// (every call logged) with the slots the reference title uses filled in:
//
//   factory slot 8  (0x2000000, ...)            SetStagingBufferSize(32 MB)?   -> S_OK
//   factory slot 4  (path, riid, out)           OpenFile: one per .rpf archive  -> file object
//   factory slot 5  (capacity, name, riid, out) CreateStatusArray              -> status array
//   factory slot 10 (desc, riid, out)           CreateQueue                     -> queue
//
// File objects really open the file (G:\ translated), so reads can be served once the
// queue's request layout is decoded from the log.

#include "hle.h"
#include "probe.h"
#include "../inflate.h"
#include "../log.h"

#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::atomic<int> g_logged{0};

void log_guid(const char* what, const GUID* g) {
    if (!g) return;
    LOG("[dstorage] %s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", what, g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

ProbeType* file_type() {
    static ProbeType* t = probe_type("IDStorageFileX");
    return t;
}
// Queue: same method order as PC IDStorageQueue.
//   3 EnqueueRequest(const request*)   4 EnqueueStatus(status array, index)
//   5 EnqueueSignal(fence, value)      6 Submit()
std::atomic<int> g_req_logged{0};

ProbeType* file_type();

bool read_at(HANDLE h, uint64_t offset, void* dst, uint32_t size) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    return ReadFile(h, dst, size, &got, &ov) && got == size;
}

std::atomic<uint64_t> g_reads{0}, g_read_bytes{0}, g_compressed{0};

// Xbox request (reference title, 0x48 bytes):
//   +0x00 options (low byte: compression format, 0 = none)
//   +0x08 destination memory          +0x18 destination size
//   +0x20 source file object          +0x28 file offset      +0x30 source size
// Requests are served synchronously, so status/fence entries enqueued after them are already done.
void STDMETHODCALLTYPE Queue_EnqueueRequest(ProbeObject*, const uint64_t* req) {
    if (!req) return;
    const uint64_t opts = req[0];
    auto* dest = reinterpret_cast<uint8_t*>(req[1]);
    const uint64_t dest_size = req[3];
    auto* file = reinterpret_cast<ProbeObject*>(req[4]);
    const uint64_t offset = req[5];
    const uint32_t size = static_cast<uint32_t>(req[6]);
    const bool log = g_req_logged.fetch_add(1) < 24;

    if (!file || file->type != file_type() || !dest) {
        static std::atomic<int> bad{0}; if (bad.fetch_add(1) < 40) LOG("[dstorage] EnqueueRequest: unsupported source/destination (opts=0x%llx src=%p dst=%p)",
                     (unsigned long long)opts, file, dest);
        return;
    }
    HANDLE h = file->user;
    const uint32_t fmt = static_cast<uint32_t>(opts & 0xFF);
    if (fmt == 0) {
        uint32_t n = static_cast<uint32_t>(dest_size < size ? dest_size : size);
        bool ok = read_at(h, offset, dest, n);
        g_reads.fetch_add(1);
        g_read_bytes.fetch_add(n);
        if (log || !ok) LOG("[dstorage] read %u bytes @0x%llx -> %p %s", n, (unsigned long long)offset, dest, ok ? "ok" : "FAILED");
        return;
    }
    // Format 1: zlib (blocks start 78 DA). Read the source block, inflate into the destination.
    // No thread_local here: the host must not use implicit TLS (the title owns slot 0, see src/tls.cpp).
    std::vector<uint8_t> src(size);
    bool ok = read_at(h, offset, src.data(), size);
    long long n = -1;
    if (ok && fmt == 1) n = zlib_inflate(src.data(), size, dest, static_cast<size_t>(dest_size));
    g_compressed.fetch_add(1);
    g_read_bytes.fetch_add(n > 0 ? static_cast<uint64_t>(n) : 0);
    const bool odd = fmt != 1 || (n >= 0 && static_cast<uint64_t>(n) != dest_size);
    static std::atomic<int> odd_logged{0};
    if (log || n < 0 || (odd && odd_logged.fetch_add(1) < 40))
        LOG("[dstorage] fmt=%u %u -> %llu bytes @0x%llx: %s (%lld bytes)%s", fmt, size, (unsigned long long)dest_size,
            (unsigned long long)offset, !ok ? "READ FAILED" : n < 0 ? "DECODE FAILED" : "inflated", n,
            odd ? " [unusual]" : "");
    uint64_t total = g_compressed.load();
    if ((total & (total - 1)) == 0 && total >= 1024)
        LOG("[dstorage] %llu compressed requests, %llu uncompressed, %llu MB delivered", (unsigned long long)total,
            (unsigned long long)g_reads.load(), (unsigned long long)(g_read_bytes.load() >> 20));
}

void STDMETHODCALLTYPE Queue_EnqueueStatus(ProbeObject*, void* status, uint32_t index) {
    if (g_req_logged.load() < 24) LOG("[dstorage] EnqueueStatus(%p, %u)", status, index);
}

void STDMETHODCALLTYPE Queue_EnqueueSignal(ProbeObject*, ID3D12Fence* fence, uint64_t value) {
    static std::atomic<int> sig{0};
    if (sig.fetch_add(1) < 10 || value > (1ull << 48))
        LOG("[dstorage] EnqueueSignal(%p, 0x%llx) -> signalled", fence, (unsigned long long)value);
    if (fence) fence->Signal(value);
}

void STDMETHODCALLTYPE Queue_Submit(ProbeObject*) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 24) LOG("[dstorage] Submit");
}

ProbeType* queue_type() {
    static ProbeType* t = [] {
        ProbeType* q = probe_type("IDStorageQueueX");
        probe_override(q, 3, reinterpret_cast<void*>(&Queue_EnqueueRequest), "EnqueueRequest");
        probe_override(q, 4, reinterpret_cast<void*>(&Queue_EnqueueStatus), "EnqueueStatus");
        probe_override(q, 5, reinterpret_cast<void*>(&Queue_EnqueueSignal), "EnqueueSignal");
        probe_override(q, 6, reinterpret_cast<void*>(&Queue_Submit), "Submit");
        return q;
    }();
    return t;
}

// Status array: 3 IsComplete(index) -> bool, 4 GetHResult(index) -> HRESULT.
// Requests complete synchronously here, so every entry reports done.
bool STDMETHODCALLTYPE Status_IsComplete(ProbeObject*, uint32_t) { return true; }
HRESULT STDMETHODCALLTYPE Status_GetHResult(ProbeObject*, uint32_t) { return S_OK; }

ProbeType* status_type() {
    static ProbeType* t = [] {
        ProbeType* s = probe_type("IDStorageStatusArrayX");
        probe_override(s, 3, reinterpret_cast<void*>(&Status_IsComplete), "IsComplete");
        probe_override(s, 4, reinterpret_cast<void*>(&Status_GetHResult), "GetHResult");
        return s;
    }();
    return t;
}

// factory slot 4
HRESULT STDMETHODCALLTYPE Factory_OpenFile(ProbeObject*, const wchar_t* path, const GUID* riid, void** out) {
    if (!out) return E_POINTER;
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring host;
    if (path) {
        host = hle_translate_path(path);
        h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
    }
    if (g_logged.fetch_add(1) < 64) {
        LOG("[dstorage] OpenFile(%ls) -> %s (handle %p)", path ? path : L"(null)",
            h != INVALID_HANDLE_VALUE ? "opened" : "NOT FOUND", h);
        if (g_logged.load() == 1) log_guid("  file IID", riid);
    }
    if (h == INVALID_HANDLE_VALUE) {
        *out = nullptr;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    *out = probe_create(file_type(), h);
    return S_OK;
}

// factory slot 5
HRESULT STDMETHODCALLTYPE Factory_CreateStatusArray(ProbeObject*, uint32_t capacity, const char* name,
                                                    const GUID* riid, void** out) {
    LOG("[dstorage] CreateStatusArray(%u, \"%s\")", capacity, name ? name : "");
    log_guid("  status array IID", riid);
    if (!out) return E_POINTER;
    *out = probe_create(status_type());
    return S_OK;
}

// factory slot 10. Desc (reference title): +0 source type (u64), +8 capacity (u16),
// +0xA priority (u8), +0x10 name, +0x18 device/flags (1).
HRESULT STDMETHODCALLTYPE Factory_CreateQueue(ProbeObject*, const uint8_t* desc, const GUID* riid, void** out) {
    if (desc) {
        LOG("[dstorage] CreateQueue(source=%llu capacity=%u priority=%d name=\"%s\")",
            (unsigned long long)*reinterpret_cast<const uint64_t*>(desc),
            *reinterpret_cast<const uint16_t*>(desc + 8), static_cast<int8_t>(desc[0xA]),
            *reinterpret_cast<const char* const*>(desc + 0x10) ? *reinterpret_cast<const char* const*>(desc + 0x10)
                                                                : "");
    }
    log_guid("  queue IID", riid);
    if (!out) return E_POINTER;
    *out = probe_create(queue_type());
    return S_OK;
}

HRESULT __cdecl DStorageGetFactory_hle(const GUID* riid, void** factory) {
    log_guid("DStorageGetFactory", riid);
    if (!factory) return E_POINTER;
    static ProbeType* t = [] {
        ProbeType* f = probe_type("IDStorageFactoryX");
        probe_override(f, 4, reinterpret_cast<void*>(&Factory_OpenFile), "OpenFile");
        probe_override(f, 5, reinterpret_cast<void*>(&Factory_CreateStatusArray), "CreateStatusArray");
        probe_override(f, 10, reinterpret_cast<void*>(&Factory_CreateQueue), "CreateQueue");
        return f;
    }();
    *factory = probe_create(t);
    return S_OK;
}

const HleExport kExports[] = {
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&DStorageGetFactory_hle)},
};

HleRegistrar g_reg(kExports);

}  // namespace
