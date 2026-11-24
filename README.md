<p align="center">
  <img src="docs/logo.svg" alt="Vice PC" width="860">
</p>

# Vice PC

An experimental loader for running Xbox Series X|S (GDK, "Scarlett") title
executables on Windows PCs.

> **Status: early development. No game is playable, and none boots yet.**
> This repository contains **no game code or assets** and never will.
> You need a dump of a game you own, made from your own console.
> Not affiliated with Microsoft, Rockstar Games or any game publisher.

<p align="center">
  <img src="docs/progress.svg" alt="Boot progress: loader 86%, boot to main loop 10%, audio and input 0%, graphics 0%, online 0%. Does not boot yet." width="860">
</p>

## How it works

Xbox Series games are x86-64 programs in the same file format as Windows
executables. The CPU instructions already run natively on a PC. What's missing is
the Xbox operating system underneath them. Vice PC:

1. Maps the game executable into memory at its preferred base address
2. Links its imports:
   - Windows DLLs (kernel32, the C runtime, WinHTTP, ...) are loaded natively
   - Xbox-only functions with a host implementation go to [`src/hle`](src/hle)
   - Everything else becomes a logging stub that records who called it
3. Sets up thread-local storage, exception unwinding and crash logging
4. Jumps to the game's entry point

The stub log shows the next function that needs implementing. See
[docs/milestones.md](docs/milestones.md) for the roadmap.

## Building

Requires Visual Studio 2022 (or the Build Tools) with the C++ workload, and CMake 3.20+.

```
cmake -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

If the build fails with `FTK1011` errors, your folder path is too long for
MSBuild. Clone to a short path (e.g. `C:\src\vice-pc`) or pass a short `-B` build folder.

The tests run a small stand-in "game" (`tests/fake_game.cpp`) through the
loader. It checks TLS on several threads, C++ exceptions and import stubbing.

## Usage

```
vice-pc <path\to\game.exe> [--no-run] [--stop-on-stub] [--log file]
```

| Option | What it does |
|---|---|
| `--no-run` | Map and link only, print the import report, don't execute |
| `--stop-on-stub` | Stop at the first call into an unimplemented Xbox function |
| `--stub-dll <name>` | Treat another DLL as Xbox-only |
| `--root <dir>` | Game package root (default: the folder containing `MicrosoftGame.Config`) |
| `--log <file>` | Log file (default `vice-pc.log`) |

`tools/peinfo.py <exe> [dll ...]` prints sections and imports without running anything.

## Contributing

Pick a stubbed function from the log and give it an implementation in
`src/hle/`. See `xmem.cpp` for the pattern. Please don't open issues or PRs
containing game files, decrypted binaries or keys. They will be deleted.

## License

MIT. See [LICENSE](LICENSE).
