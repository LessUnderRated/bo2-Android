# Toolchain

What `scripts/build_steam.sh` needs on an Apple Silicon Mac, and what each piece does. The script checks all of it
before starting and stops with the exact install command when something is missing.

## Install

```bash
xcode-select --install                                  # Apple clang + macOS SDK
brew install llvm@17 cmake ninja libvpx unicorn
pip3 install capstone
```

Nothing else is installed by hand:

- **Remill** (v6.0.1, Apache-2.0) is cloned into `tools/remill-src`, patched with `tools/remill-patches/*.patch` and
  built against Homebrew's LLVM 17 by `scripts/build_remill.sh` (about 12 minutes and 0.7 GB the first time).
- **HLSLcc** (`third_party/HLSLcc`) is built into `boot_cache/hlslcc_build` on the first run.
- The **Ghidra catalogue** of each exe ships without game bytes in `tools/ghidra-{sps,mps,zms}/`;
  `tools/ghidra_csv.py remplir` refills it from the player's own exe. Ghidra itself is only needed to regenerate
  that catalogue, not to build.

## Disk

`build_steam.sh` refuses to start, or to continue a step, with less than 8 GB free. It resumes where it stopped.

## What each tool does

- **Remill**: x86-32 instructions → LLVM IR with memory-access intrinsics. The core lift.
- **LLVM 17 / clang**: folds the lifted IR at -O2 (`scripts/fold_lift.sh`) and compiles it with the runtime into an
  arm64 Mach-O.
- **HLSLcc**: translates the game's D3D11 shader bytecode to Metal Shading Language at run time.
- **Unicorn**: runs the few guest code paths that are only known at run time (code written to the heap, exception
  handlers) in an x86 VM.
- **Capstone**: x86 disassembly for the build scripts (call-graph closure, sound filter extraction).
- **libvpx**: video decoding.
