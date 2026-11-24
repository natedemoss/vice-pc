// DXGI side of the Xbox device.
//
// On Xbox the D3D12 device also answers QueryInterface(IDXGIDevice). The reference
// title does: device->QI(IDXGIDevice) -> GetAdapter() -> adapter->EnumOutputs(0).
// A PC D3D12 device has no IDXGIDevice, so we supply a small one whose adapter is the
// real DXGI adapter behind our D3D12 device (matched by LUID). Adapter and output
// vtables are patched so Xbox-only DXGI methods are logged instead of crashing.

#include "dxgi.h"
#include "vtpatch.h"

#include "../../log.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>

namespace {

const VtInterface kVtDxgiDevice = {"IDXGIDevice", 14, {}};    // IDXGIDevice1

IDXGIAdapter* g_adapter = nullptr;

IDXGIAdapter* adapter_for(ID3D12Device* dev) {
    if (g_adapter) return g_adapter;
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return nullptr;
    LUID luid = dev->GetAdapterLuid();
    IDXGIAdapter4* a = nullptr;
    HRESULT hr = f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&a));
    f->Release();
    if (FAILED(hr)) return nullptr;
    DXGI_ADAPTER_DESC3 d{};
    a->GetDesc3(&d);
    LOG("[dxgi] adapter: %ls (%llu MB VRAM)", d.Description, (unsigned long long)(d.DedicatedVideoMemory >> 20));
    // Not patched (DXGI uses internal vtable slots past the public interface).
    g_adapter = a;
    return g_adapter;
}

class XboxDxgiDevice final : public IDXGIDevice1 {
public:
    explicit XboxDxgiDevice(ID3D12Device* dev) : dev_(dev) { dev_->AddRef(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) ||
            riid == __uuidof(IDXGIDevice1)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        return dev_->QueryInterface(riid, out);  // back to the D3D12 device
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // lives for the process

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* p) override {
        return dev_->SetPrivateData(g, n, p);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* p) override {
        return dev_->SetPrivateDataInterface(g, p);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* p) override {
        return dev_->GetPrivateData(g, n, p);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** out) override {
        IDXGIAdapter* a = adapter_for(dev_);
        return a ? a->QueryInterface(riid, out) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** out) override {
        IDXGIAdapter* a = adapter_for(dev_);
        LOG("[dxgi] DXGIDevice::GetAdapter -> %p", a);
        if (!a) return E_FAIL;
        a->AddRef();
        *out = a;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*,
                                            IDXGISurface**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const*, DXGI_RESIDENCY* st, UINT n) override {
        for (UINT i = 0; i < n; ++i) st[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* p) override {
        *p = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT n) override {
        latency_ = n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* n) override {
        *n = latency_;
        return S_OK;
    }

private:
    ID3D12Device* dev_;
    std::atomic<ULONG> refs_{1};
    UINT latency_ = 3;
};

XboxDxgiDevice* g_dxgi_device = nullptr;

}  // namespace

bool dxgi_is_device_iid(REFIID riid) {
    return riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIObject);
}

void* dxgi_device_for(ID3D12Device* dev) {
    if (!g_dxgi_device) {
        g_dxgi_device = new XboxDxgiDevice(dev);
        vt_patch(g_dxgi_device, kVtDxgiDevice);  // Xbox-only DXGI device methods -> logged stubs
    }
    g_dxgi_device->AddRef();
    return static_cast<IDXGIDevice1*>(g_dxgi_device);
}
