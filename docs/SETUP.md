# Setting up COD3 Recomp

Getting from a fresh clone to a running game. Written against this build
and checked on it — every command here was run before it was written
down.

Expect **2–4 hours for the first build**, and it is almost entirely
waiting. The tools build in about 30 seconds; the rest is one
19,433-function translation pass, then 15 more level libraries.

---

## What you cannot skip

**The repository does not contain the game, and never will.** There is no
`default.xex`, no level archives, no shaders, no audio in it. This is
not an omission to be fixed — it is Activision's copyrighted material
and shipping it would make the repository both unusable and illegal.

So a clone alone gets you the tools. To get a game you supply your own
legally owned copy. That is the same bargain every project of this kind
makes.

| | |
|---|---|
| Works from a bare clone | XenonRecomp, XenonAnalyse, PpcAsm, CoD3Scan, XenosDis |
| Needs your copy of the game | `CoD3.exe` and everything it runs |

If you only want to work on the tools, you can stop after step 2. If you
want a playable game, you need steps 3 onwards, and you need the disc.

---

## 1. Prerequisites

| | Needed | Notes |
|---|---|---|
| **Windows** | 10 or 11, 64-bit | The host is Windows-only. Not a WINE or MSYS situation. |
| **Visual Studio** | 2022, or the 2022 **Build Tools** | Only for the Windows SDK and `rc.exe`. The full IDE is not needed. |
| **LLVM** | **20.1.8** | Not optional. See below. |
| **CMake** | 3.20+ | Ships with Visual Studio; the scripts find it there. |
| **Ninja** | any | Same. |
| **Python** | 3.8+ | The recompile steps are Python scripts. |
| **PowerShell** | 5.1+ | Comes with Windows. |
| **Disk** | ~10 GB free | See the table under step 6. |
| **RAM** | 32 GB recommended | 16 GB works at lower parallelism. |

### Why LLVM 20.1.8 specifically

The generated game code uses Clang intrinsics and the `alias` attribute
for function hooking. MSVC's `cl.exe` will not compile it. Pinning the
version matters because the intrinsics change between releases.

```powershell
choco install llvm --version=20.1.8 -y
```

Confirm it installed where the scripts look:

```powershell
& 'C:\Program Files\LLVM\bin\clang-cl.exe' --version
```

---

## 2. Clone and patch

```powershell
git clone --recurse-submodules https://github.com/jxnplays/cod3-recomp.git
cd cod3-recomp
```

The `--recurse-submodules` flag is required. `tools/XenonRecomp` is an
empty directory without it, and CMake will report the source directory
as missing.

Now apply the patch. The recompiler carries fixes this project depends
on; they live as a patch so they apply to a clean checkout:

```powershell
git -C tools\XenonRecomp apply ..\patches\XenonRecomp.patch
```

Expect no output. Check it worked:

```powershell
git -C tools\XenonRecomp status --porcelain   # 4 files modified
```

**If you skip this, the recompiler mistranslates the game and the title
will not boot.** It is the single most important line in this document.

Every push to the repository re-applies this patch to a pristine checkout
in CI, so it is known to apply cleanly.

---

## 3. Fetch the FFmpeg headers

COD3's audio is XMA, decoded through FFmpeg. FFmpeg is GPL, so its
headers are deliberately not committed; this downloads them:

```powershell
.\scripts\get_ffmpeg_headers.ps1
```

Pinned to **FFmpeg 8.1 (avcodec-62)**. Without this the build produces an
executable with no audio and no warning.

You will also need the FFmpeg runtime DLLs next to `CoD3.exe` at run
time. `thirdparty/ffmpeg/README.md` says where they come from.

---

## 4. Build the tools

```powershell
.\scripts\recompile.ps1 -BuildTools
```

About **30 seconds** — measured at 31 s from a clean tree on this machine.
Produces `XenonRecomp.exe` and `XenonAnalyse.exe`.

**This is a checkpoint you can stop at.** If you have no disc, this is
as far as you can go, and you have verified the build works.

---

## 4b. Build the level scanner

`recompile_level.ps1` needs `tools\build-scan\CoD3Scan.exe`, and **no script in
the repository builds it** — `recompile.ps1` builds XenonRecomp and
XenonAnalyse only. Without this, step 6b stops with
`CoD3Scan.exe is missing; build the tools first`.

It is a separate CMake project because it pulls in XenonRecomp's libraries:

```powershell
$bt = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
& cmd /c "`"$bt\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set PATH=$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\PROGRA~1\LLVM\bin;%PATH% && cmake -S tools/CoD3Scan -B tools/build-scan -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl && cmake --build tools/build-scan --target CoD3Scan --parallel 4"
```

Both `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` have to be named. The project
adds XenonRecomp as a subdirectory, and CMake refuses a mixed clang/clang-cl
pair — which is what it assumes when only the C++ one is given.

## 5. Supply your copy of the game

**You need an extracted Xbox 360 game folder — and nothing else.** If you
have one, you can skip the three options below and go straight to step 6:
the scripts find it themselves.

That folder is the one containing `default.xex` at its root:

```
<your game>\default.xex
<your game>\sp\<level>\<level>.dll      fifteen of them
<your game>\sp\global.cod
<your game>\config\
```

Any of these three makes it work:

**A. Let the host install it, and record where it went.** Run the
executable once against your disc image or extracted folder. It writes
`game.path` **beside the executable**, which is `build\CoD3Host\`, and both
recompile scripts look there as well as at the repository root.

```powershell
# from a disc image
.\build\CoD3Host\CoD3.exe --iso "D:\Call of Duty 3.iso"

# or from a folder you already extracted, used where it is
.\build\CoD3Host\CoD3.exe --game "D:\Call of Duty 3"

.\build\CoD3Host\CoD3.exe --install-only     # install, do not start the game
```

If the game is later moved or the drive letter changes, the stale path is
reported and skipped rather than believed — delete the `game.path` and run the
executable again, or just use `-GameRoot`.

**B. Or point the scripts at it directly.** Works without running the
host at all — useful if you want to check the path before installing:

```powershell
.\scripts\recompile.ps1      -GameRoot "D:\Call of Duty 3" -BuildLib
.\scripts\recompile_level.ps1 -All -GameRoot "D:\Call of Duty 3"
```

**C. Or put a `game` folder beside the repository.** Also found
automatically.

Whichever route, the scripts copy what they need into
`CoD3RecompLib/private/`, which is in `.gitignore` and must stay that way.
Nothing from your disc is ever committed.

If none of them finds it, the error message lists every path that was
tried and the three commands above.

**Where to get the game:** from a disc or dump you own. This document
does not say where to obtain one, deliberately.

---

## 6. Recompile the game

### 6a. The main game

```powershell
.\scripts\recompile.ps1 -BuildLib
```

Takes roughly **1–2 hours**. It translates 19,433 functions across 77
translation units into `CoD3RecompLib/ppc/`.

### 6b. Every level

```powershell
.\scripts\recompile_level.ps1 -All
```

Another **1–2 hours**, 15 libraries.

> **This step is not optional and is the step most people miss.**
> `recompile.ps1` does *not* build the level libraries — they are separate
> targets that are not in the default build. Skipping 6b produces a host
> that starts, shows a black window, and hangs before drawing anything.
> That looks like a graphics driver fault and is not one.

### 6c. Configure and build

The level libraries are discovered by CMake at **configure** time, so
step 6b created 15 targets the existing build tree knows nothing about.
Run CMake directly:

```powershell
$bt = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
& cmd /c "`"$bt\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set PATH=$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\PROGRA~1\LLVM\bin;%PATH% && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl && cmake --build build --parallel 4"
```

Substitute your own Visual Studio path. This whole line runs in one
`cmd` because `vcvars64.bat` sets the compiler environment in its own
shell.

You should end up with `build\CoD3Host\CoD3.exe`, about 209 MB.

> **On `--parallel 4`.** The default in `recompile.ps1` is your core
> count, and that will run a 32 GB machine out of memory — Clang holds
> several GB per translation unit across 549 MB of generated code. Four
> is what works. Going higher buys little and risks
> `LLVM ERROR: out of memory`, which was hit here at 16 jobs. Lower it if
> you are short on memory.

### What you should have on disk

| Path | Size | |
|---|---|---|
| `build/` | ~2.1 GB | rebuildable in 10 minutes |
| `CoD3RecompLib/ppc/` | 93 MB | hours to regenerate |
| `CoD3RecompLib/levels/` | 460 MB | hours to regenerate |
| `CoD3RecompLib/private/` | 39 MB | **your copy of the game, irreplaceable** |

---

## 7. Install the game and run it

`CoD3.exe` looks for a `game` folder beside itself and offers to install
if there isn't one.

```powershell
cd build\CoD3Host

# From a disc image (XGD1/XGD2/XGD3 all read):
.\CoD3.exe --iso "D:\Call of Duty 3.iso"

# From a folder you already extracted, used where it is (no 7 GB copy):
.\CoD3.exe --game "D:\Call of Duty 3 (USA, Europe)"

# Check an image before committing to the extraction:
.\CoD3.exe --list-iso "D:\Call of Duty 3.iso"

.\CoD3.exe --help
```

| Key | |
|---|---|
| `F11` | settings menu |
| `F12` | screenshot |
| `` ` `` | the title's console |
| `Escape` | pause |

---

## 8. Verify

```powershell
.\scripts\verify.ps1         # mechanical build and behaviour checks
.\scripts\verify.ps1 -All    # plus Xenia's instruction tests
```

`-All` runs Xenia's **165 instruction tests** (all but two
emulator-mechanics tests, which are excluded at selection time) and
currently reports **73 failing checks across 27 distinct tests**. That is
the known baseline, not a regression you introduced: the failures share a
root cause in Xenia's four-argument `make_full_test_constant` macro and
the `clrldi r3,r3,32` inside it. `verify.ps1` prints the first 40 failing
lines and then stops listing, so the count of lines you see is not the
count of failures. See `docs/GAPS.md`.

---

## Getting to a specific level without playing through

The host reads a few environment variables. These are the useful ones:

```powershell
# Start Saint-Lô 20 seconds in (the main menu is up by then)
$env:COD3_MAP="saint_lo,20"
$env:COD3_PAD="5:start 8:a"     # scripted button presses: 5 seconds in, press Start
$env:COD3_NOFILMS="1"           # skip the intro films
.\CoD3.exe --game "D:\Call of Duty 3"
```

| Variable | |
|---|---|
| `COD3_MAP=name[,second]` | start a level at that second (default 12) |
| `COD3_PAD="second:button;..."` | press pad buttons — `5:start`, `8:a` |
| `COD3_EXEC="cmd;cmd"` | console commands at startup |
| `COD3_CMD="second:cmd;..."` | console commands at those seconds |
| `COD3_NOFILMS=1` | skip films |
| `COD3_SCANMEM=text` | report where a string sits in guest memory |
| `COD3_DUMP=addr:bytes` | dump guest memory at an address |
| `COD3_MOUSELOG=1` | log what mouse look took |

All 15 level names, from `build\CoD3Host\cod3_levels.inc`:

```
saint_lo   blkbrn   chambois  credits  crssrds  falaise   forest
fuelplnt   hostage  island    laison   mace2    mayenne   nightd
stbert
```

---

## If something goes wrong

**Black window, hangs, 0 fps, 0 draws.**
You skipped step 6b, or skipped the reconfigure in 6c. Both produce a
mismatched build: the host links against level libraries that were never
built. Run 6b then 6c.

**`clang-cl` not found at C:\Program Files\LLVM\bin.**
LLVM is not installed, or is a version this project does not use. See
step 1.

**`vcvars64.bat not found`.**
Visual Studio 2022 is not installed, or is somewhere unusual.
`recompile.ps1` globs for it across all editions and both install roots;
if it cannot find yours, the scripts need the path.

**`LLVM ERROR: out of memory`.**
Too many parallel jobs. Use `--parallel 4`, or lower.

**The source directory "tools/XenonRecomp" does not exist.**
The submodule was not cloned. Re-clone with `--recurse-submodules`, or
run `git submodule update --init tools/XenonRecomp`.

**Patch fails to apply with "No valid patches in input".**
The patch file has been re-saved as UTF-16. This happens if it is
redirected from PowerShell with `>`, which writes UTF-16. Use `cmd` for
redirection. CI checks for exactly this.

**No audio.**
FFmpeg headers were not fetched, or the runtime DLLs are not beside
`CoD3.exe`.

**Mouse look does not turn the view.**
Expected on this build. A previous attempt to fix it was reverted in
full; `docs/MOUSE.md` has the measurements and the leads already
exhausted.

**Frame rate varies wildly between runs.**
Known and unexplained — 22, 144 and 299 fps have all been observed on
the same build and level. Recorded as an open question in `docs/GAPS.md`.

---

## What is verified, and what is not

Measured on this build:

- Saint-Lô (`saint_lo`) loads and runs end to end — **138–143 fps, 74
  files, 195 MB streamed**, 26–46 million draws
- The build is clean: zero errors, zero warnings
- A fresh clone applies the patch and builds the tools from nothing
- `set cg_fov 30` reaches the title and is executed, confirmed by the
  title's own parsed command record in guest memory

**Not verified here:** the other 14 levels. They are compiled and linked,
but only Saint-Lô has been run. `STATUS.md` describes far more than
this — cutscenes, saves, mission flow — and that description is
inherited, not re-measured by us.

Read [README.md](../README.md) for what works, and
[docs/GAPS.md](GAPS.md) for the open questions.