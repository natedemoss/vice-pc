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
| 6 | **`XCRDAPI!DStorageGetFactory`** | **Next:** a DirectStorage factory backed by file I/O on the dump |

Still to do:

- `xmem.dll`: physical-page APIs, `XMemTransferMemory`
- `xgameruntime.dll` stand-in (XUser, XTaskQueue, XGameSave, ...). `XGameRuntimeInitialize` currently fails
- File system: map Xbox package paths to the dump folder; read `.rpf` archives and `.ttpf` streaming packs
- `GetModuleHandle(NULL)` and the PEB image base should point at the game, not the loader

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
