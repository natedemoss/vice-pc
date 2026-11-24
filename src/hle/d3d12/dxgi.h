#pragma once

#include <windows.h>

struct ID3D12Device;

// True for IIDs the Xbox device answers with its DXGI side.
bool dxgi_is_device_iid(REFIID riid);

// The IDXGIDevice stand-in for the title's D3D12 device (AddRef'd).
void* dxgi_device_for(ID3D12Device* dev);
