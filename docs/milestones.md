# Milestones

Xbox Series titles are x86-64 Windows-format (PE32+) executables, so no CPU
translation is needed. The job is a loader plus a compatibility layer for the
Xbox-only system libraries the game imports.

Reference target: `game_scarlett_master.exe`, package version 1.0.2.1
(about 120 MB, 10 sections, 67 imported DLLs).

## 1. Loader ✅ done

- [x] Map the PE at its preferred base (0x140000000) or relocate it
- [x] Resolve imports: real Windows DLLs, host HLE functions, or logging stubs
  - Reference title: 476 native, 5 HLE, 106 stubbed across 68 DLLs
- [x] Register `.pdata` so SEH and C++ exceptions unwind through game code
- [x] Override `_CxxThrowException` so C++ catches work in a manually mapped image
- [x] Implicit TLS for the main thread and every thread the game creates
- [x] Crash and exception logging with game-relative addresses
- [x] `--no-run` and `--stop-on-stub` modes
- [x] Run the real title and record the first unimplemented call it reaches:
  **`d3d12_xs.dll!D3D12ConfigureVirtualMemoryX`** (called from game+0x1e1cfe5) after
  CRT startup and the TLS callbacks

## 2. Boot to main loop (in progress)

Each run of `--stop-on-stub` gets further. Stops so far, in order:

| # | Stop | Resolution |
|---|---|---|
| 1 | `d3d12_xs!D3D12ConfigureVirtualMemoryX` | HLE: return `S_OK` (PC D3D12 manages GPU VA itself) |
| 2 | `xmem!XMemVirtualAlloc` (2 × 24 GB GPU reservations + ~4.7 GB of pools) | HLE: `VirtualAlloc`, Xbox-only type/protect bits stripped |
| 3 | Bad virtual call in a static constructor | **Loader fix:** title reads TLS from slot 0 directly (local-exec model), so the game's block now lives in slot 0 |
| 4 | `__debugbreak()` after `XGameRuntimeInitialize` fails | Loader steps over title debug breaks (`--break-on-int3` to disable). Game continues into `main` |
| 5 | `GameInput!GameInputCreate` (ordinal 1) | Use Windows' own GameInput (same v0 interface) |
| 6 | `XCRDAPI!DStorageGetFactory` | Probe COM object (logs every vtable call). Game calls slot 8 (`SetStagingBufferSize(32 MB)`?) |
| 7 | 5 asserts, then null `memset` in engine init | Game asked for `G:\appdata0.rpf`. **`G:\` (Xbox package mount) now maps to the dump folder.** `GetModuleFileName/Handle(NULL)` now report the game, not the loader |
| 8 | Opens all 12 `.rpf` archives, calls DStorage factory slot 4 per archive (`OpenFile`?) | Probe returns `S_OK` without an object, so this needs a real file object next |
| 9 | `xmem!XMemAllocatePhysicalPages` | HLE: 64 KB page ids; `XMemMapPhysicalPages` commits/decommits at the target VA |
| 10 | `d3d12_xs!D3D12XboxCreateDevice` | **Real PC D3D12 device** (`ID3D12Device10`) with a patched vtable (see below) |
| 11 | Device slots 48 / 73 (Xbox-only signatures) | Xbox `CreateCommandQueue` variant and `CreateCommandList(desc*, ...)` → PC equivalents. 5 queues, 5 fences, 384 command lists created |
| 12 | `QueryInterface(IDXGIDevice)` → `GetAdapter` → `EnumOutputs(0)` | DXGI device stand-in returning the real adapter (by LUID) |
| 13 | `VirtualFree(MEM_PRESERVE_PLACEHOLDER)` fails | `XMemVirtualAlloc` keeps placeholder flags via `VirtualAlloc2`. 185 MB split + 2,960 physical pages mapped |
| 14 | Crash in D3D12Core from `GetCPUDescriptorHandleForHeapStart` | **Calling convention:** title uses the C interface, so 8-byte struct returns come back in RAX |
| 15 | Descriptor handle assert, then a crash in the GPU driver writing a descriptor | Xbox heap desc carries a title-chosen CPU address: heap start reported at that address, every handle translated (`src/hle/d3d12/gpumem.*`) |
| 16 | Device slot 133: buffer at a title-chosen GPU address (174 MB) | Real buffer + GPU-VA translation table; `GetGPUVirtualAddress` reports the title's address |
| 17 | Device slot 70: SRV made straight from a GPU address | View built on the resource that backs that address |
| 18 | `d3d12_xs!D3D12SerializeVersionedRootSignature` | Forwarded to PC D3D12 |
| 19 | `xg_xs` texture layout library | Linear-layout stand-ins (`XGSuggestSwizzleMode`, texture computer) |
| 20 | DirectStorage queue waits forever | Real file objects, queue, status array; requests served from the `.rpf` files |
| 21 | Compressed streaming blocks (`78 DA`) | Built-in zlib/deflate decoder (`src/inflate.cpp`, unit-tested) |
| 22 | Fail-fast (0xC0000409) with no handler output | `--trace-d3d` found it: Xbox device slots 44+ differ from PC, forwarding them kills the runtime. Now stubs; slot 62 (console pipeline blob) fails cleanly |
| 23 | Init skipped: `CreateCommandSignature` rejected (mesh dispatch) | Falls back to compute dispatch on GPUs without mesh shaders |
| 24 | Texture objects corrupted | Hardware watchpoint caught it: the title calls some struct-returning methods C++-style and others C-style. Thunk now detects which per call |
| 25 | Null frame context | Stack corruption: Xbox queue slot 18 is `GetTimestampFrequency`, PC slot 18 is `GetDesc` (16 bytes into an 8-byte variable) |
| 26 | GPU device removed | Xbox queue slots 16-19 are PC 14-17 (`Signal` was overwriting fences); texture views reinterpreting formats; buffers placed over title memory the title later remaps. All fixed (sharing title memory is now opt-in) |
| 27 | Pipeline caches full of nulls | Xbox pipeline creators (device slots 55/57/62/67) return a placeholder pipeline. The title builds its whole library (16,000+ pipelines) |
| 28 | Title scribbles over PC command-list objects | **Xbox command lists are user-mode GPU packet buffers** the title writes into inline. Title lists are now title-owned buffers (packets kept, not decoded yet) |
| 29 | Fence completed-value pointer, audio processor (AcpHal), `GetResourceAllocationInfo` | Fence value mirror thread; APU heap + shape contexts; allocation info with sanitised descs |
| 30 | Title storage drive `T:\` missing | `T:\`, `S:\`, `D:\` map to `%LOCALAPPDATA%\vice-pc\<letter>` |
| 31 | Asset lookup fails (title assert at `game+0xcc2834`) | Breakpoint tracing: the title's texture-size routine uses Xbox device slot 108 (allocation info + per-resource info); as a stub it left sizes unset, the GPU pool allocator failed. Implemented |
| 32 | GPU device removed (`DXGI_ERROR_INVALID_CALL`) | Post-call device checks found it: buffer views running past their buffer (Xbox doesn't check, PC removes the device). Views clamped. Also: Map on title-address resources, Signal/Wait only on device fences |
| 33 | Null shadow-map / display targets | Device slot 127 (committed resources: 3840×2160 display buffers). Xbox allows depth-stencil + UAV on one resource, PC rejects it; views validated against resource flags |
| 34 | **The title runs without crashing (60 s+), main loop submitting, intro video requested (`BinkOpenWithOptions`)** | **Current**: nothing is presented yet (packets not decoded, shaders not translated, Bink not implemented) |

### What the loading screen still needs

The title now gets through device, memory, streaming, pipeline and audio setup without
the PC runtime rejecting anything. Nothing is drawn because two big pieces don't exist yet:

1. **GPU command processor**: decode the packets the title writes into its command lists
   and replay them as PC D3D12 work (the Xbox driver does this in user mode).
2. **Shader translation**: turn the console pipelines (RDNA2 machine code) into DXIL.

Both are large, research-level projects.

### Why D3D12 works at all

The Xbox D3D12 interfaces keep PC's method order for the core methods (device slot 26 is
called with exactly `GetCustomHeapProperties(nodeMask=1, HEAP_TYPE_DEFAULT)`'s arguments).
So the title talks to genuine PC D3D12 objects whose vtables are patched (`src/hle/d3d12/vtpatch.*`):

- struct returns > 8 bytes: hidden pointer comes first → swap rcx/rdx (only when the caller is game code)
- 8-byte struct returns (descriptor handles, LUID): returned in RAX → thunk supplies the hidden slot
- slots past the PC interface are Xbox-only methods → logged stubs
- `Create*` methods: Xbox IIDs replaced with PC IIDs, Xbox-only flags dropped, results patched in turn

### The current wall: Xbox GPU memory model

On Xbox the CPU and GPU share one address space that the **title** manages: it reserves GPU
VA ranges, maps physical pages into them, creates resources *at chosen GPU addresses*
(Xbox-only device slot 133, `ID3D12Resource`), and computes descriptor handles itself. On PC
the driver owns GPU memory and addresses. Bridging this needs:

1. Resources created at title-chosen GPU VAs → backing heaps + a GPU VA translation table
2. Translating every GPU VA the title puts into command lists, root arguments, VB/IB views and indirect args
3. Descriptor handles: map title-computed CPU/GPU handles onto real descriptor heaps
4. Texture layouts: Xbox tiled/swizzled formats → PC layouts
5. Shaders: Xbox shader blobs → something PC D3D12 accepts (not yet examined)

Hardware note: the test PC's GPU is a Radeon RX 580 (FL 12_0, no DXR / mesh shaders). A 2026
title's renderer will likely need a newer GPU even once translation exists.

Still to do:

- `xmem.dll`: physical-page APIs, `XMemTransferMemory`
- DirectStorage: real `OpenFile` / queue / request objects reading from the `.rpf` files
- `xgameruntime.dll` stand-in (XUser, XTaskQueue, XGameSave, ...). `XGameRuntimeInitialize` currently fails
- Other Xbox mount points (persistent local storage, temp) as the logs show them

## 3. Audio and input

- `xapu.dll`, `AcpHal.dll`, `chad.dll` (Xbox audio hardware): replace with XAudio2 or WASAPI
- ~~`GameInput.dll`~~: works with Windows' built-in GameInput
- `Bink2Scarlett.dll`: video playback

## 4. Graphics (the hard part)

- `d3d12_xs.dll`: `D3D12XboxCreateDevice`, `D3D12ConfigureVirtualMemoryX`. Wrap PC D3D12 and emulate the Xbox-only extensions
- `xg_xs.dll`: texture tiling/swizzle layouts → linear or PC layouts
- `shaders.rpf`: shaders are precompiled for the console GPU (RDNA2 ISA), not DXIL. They need a translator
- `XboxBvhBuilder.dll`: ray-tracing acceleration structures → DXR
- `amd_fidelityfx_*_xbox.dll`: swap in PC FidelityFX

## 5. Online

- `libHttpClient.GDK.dll`, `XCurl.dll`, Xbox Live services: stub out for offline play
