# Setup and build

End-to-end instructions for getting a Windows PC binary out of the Xbox 360
dump.

## What you need

| Requirement | Version used | Notes |
|---|---|---|
| Windows | 10/11 x64 | The output is a native Win32 x64 binary |
| Visual Studio Build Tools | 2022, MSVC 14.44 | Any edition; needs the x64 C++ toolset and Windows SDK |
| LLVM / clang-cl | **20.1.8** (18+ required) | `C:\Program Files\LLVM` |
| CMake + Ninja | Bundled with VS Build Tools | CMake 3.20+ |
| Python | 3.11 | Analysis helpers only; needs `pycryptodome` |

All of these are checked by `tools/setup.ps1`. Nothing is installed silently —
if something is missing the script stops and says what to install.

**Important LLVM note.** The *binary* LLVM distribution is sufficient. The
XenonRecomp build links no LLVM libraries; it only needs clang as a compiler.
A stock `clang+llvm-*-win64` install works as-is. If you want to be sure you
have the dev package, `llvm-config.exe` is a convenient canary — its absence is
fine.

## One-time setup

```powershell
git clone https://github.com/jxnplays/cod3-recomp.git
cd cod3-recomp
powershell -ExecutionPolicy Bypass -File tools\setup.ps1
```

This clones `hedge-dev/XenonRecomp` into `.tools/` (recursively — it needs the
`fmt`, `tomlplusplus`, `xxHash`, `libmspack`, `tiny-AES-c` and `simde`
submodules) and builds `XenonRecomp.exe` and `XenonAnalyse.exe`.

## Full build

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

The pipeline is:

| Stage | Tool | Output |
|---|---|---|
| 1 | `XenonAnalyse` | `config/recomp/COD3_switch_tables.toml` — 323 jump tables |
| 2 | `XenonRecomp` | `build/ppc/*.cpp` — 29,731 functions, ~98 MB of C++ |
| 3 | `tools/gen_import_stubs.py` | `build/runtime/kernel_imports_stubs.cpp` — 150 stubs |
| 4 | CMake + clang-cl | `build/host/COD3.exe` |

Every stage is incremental. Useful switches:

```powershell
tools\build.ps1 -Recompile      # force stages 1-4 to re-run
tools\build.ps1 -Jobs 4         # limit compiler parallelism (default 8)
tools\build.ps1 -SkipTools      # skip the toolchain check
tools\build.ps1 -Xex "D:\other\default.xex"
```

Stage 2 is the slow one and takes a few minutes; stage 4 compiles 3.1M lines
and takes longer still. A single generated translation unit compiles in about
8 seconds, and the default `-Jobs 8` keeps peak memory around 8 GB.

### Note on paths in the recompiler config

XenonRecomp resolves `file_path` by **literal string concatenation** with the
directory containing its TOML, not by path resolution:

```cpp
file = LoadFile((config.directoryPath + config.filePath).c_str());
```

So an absolute path in `COD3.toml` becomes `config\recomp\X:/...`, silently
loads nothing, and then dereferences a null pointer. `build.ps1` rewrites
`file_path` as a relative path on every run, which is why the default config
points at `../../../Call of Duty 3/default.xex`. If you move the dump, re-run
`build.ps1` rather than editing the TOML by hand.

## Running

```powershell
build\host\COD3.exe --xex "X:\AI\Recomp Projects\Call of Duty 3\default.xex"
```

| Option | Effect |
|---|---|
| `--xex <path>` | Explicit XEX path |
| `--title <text>` | Window title |
| `--no-window` | Headless; useful when bringing the runtime up |
| `--help` | Usage |

Environment:

| Variable | Effect |
|---|---|
| `COD3_QUIET_IMPORTS=1` | Suppress the per-call log of kernel APIs that are not implemented yet |
| `COD3_QUIET_VM=1` | Suppress the per-allocation log from the guest virtual memory manager |
| `COD3_TRACE_LAUNCH=1` | Dump the launch register state (r1..r6, r12, lr) before entering the title |

The loader looks for `default.xex` next to the executable and then at the two
relative locations in `src/host/main.cpp`, so in the default layout you can
just run `build\host\COD3.exe`.

## Analysis helpers

These are standalone and do not require the build.

```powershell
# XEX container metadata
python tools\xex_info.py "X:\...\default.xex" --imports

# Decrypt + decompress to a raw PE image (BASIC compression only)
python tools\xex_extract.py "X:\...\default.xex" build\default.image --sections

# Locate the register save/restore helpers
python tools\find_symbols.py build\default.image

# Preview the import split between hand-written and stubbed
python tools\gen_import_stubs.py build\ppc out.cpp --dry-run
```

`xex_extract.py` needs `pycryptodome` (`pip install pycryptodome`). It handles
`NONE` and `BASIC` compression; `default.xex` happens to be `BASIC`, so it works
here, but an `LZX`-compressed title would need XenonRecomp itself.

## Repository layout

```
cod3-recomp/
  config/recomp/
    COD3.toml                  XenonRecomp configuration (addresses auto-detected)
    COD3_switch_tables.toml    XenonAnalyse output
  src/
    CMakeLists.txt             builds the host + recompiled library
    host/main.cpp              launcher: loads the XEX, enters the entry point
    runtime/
      cod3_runtime.h           runtime interface
      ppc_runtime.cpp          image loading, guest dispatch, import tracing
      kernel_imports_core.cpp  hand-written Xbox 360 kernel APIs
  tools/
    setup.ps1                  toolchain fetch + build
    build.ps1                  full pipeline
    env.bat                    MSVC/clang/CMake environment
    gen_import_stubs.py        generates the import stub table
    xex_info.py                XEX header parser
    xex_extract.py             XEX decrypt/decompress
    find_symbols.py            register save/restore helper scanner
  docs/
    ANALYSIS.md                dump analysis and all measured addresses
    SETUP.md                   this file
    GRAPHICS.md                graphics hooks and modernization plan
    STATUS.md                  what works, what does not, what is next
```

`build/` and `.tools/` are gitignored: one is generated, the other is an
external checkout.

## What you cannot get yet

This build produces a binary that loads the recompiled executable and enters its
entry point. It does **not** yet display the game. The reason is specific and
documented in [GRAPHICS.md](GRAPHICS.md): Call of Duty 3 has no D3D boundary,
because the Xbox 360 graphics stack is statically linked into the executable,
so presenting requires implementing the Xenos command processor. See
[STATUS.md](STATUS.md) for the exact stopping point.