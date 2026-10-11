# Graphics hooks and modernization plan

## The constraint, and the way around it

Call of Duty 3 imports **no Direct3D functions whatsoever**. Verified two ways:

1. The 179 resolved imports contain zero `D3D*` symbols. The only
   graphics-adjacent kernel imports are eight display-mode calls:
   `VdGetCurrentDisplayGamma`, `VdGetCurrentDisplayInformation`,
   `VdPersistDisplay`, `VdQueryVideoFlags`, `VdQueryVideoMode`, `VdSetDisplayMode`,
   `XGetVideoMode`, `XamReadTileToTexture`.
2. The Xbox 360 graphics stack (NGL, the Xenos D3D9 derivative) is **statically
   linked** into the executable, complete with its own debug strings:
   * `[1.1]NGL 3.0.0`
   * `nglPresent`, `nglBackBuffer`
   * `"D3D: GPU initialization (HSIO training) has failed so no graphics will render."`
   * `"The GPU is hung! D3D version is %i.%i %s, kernel is %i, frame is %i."`
   * 81 `ngl*` debug strings in total

So there is no *import* boundary to wrap. But that does **not** mean there is no
graphics boundary at all, and an earlier version of this document wrongly
concluded that Xenos GPU emulation was the only option. It is not.

## The approach: intercept NGL, drive real D3D9

Measured on this image: **211 vtable-like function-pointer tables live in
`.rdata`**, the largest with 629 entries. That is a COM-style object model, and
it means the title calls into the graphics stack *through method tables*, above
the point where anything becomes Xenos-specific.

That gives the shape of the solution:

```
  +----------------------------------------------------------+
  |  Call of Duty 3 renderer (game code)                     |
  +----------------------------------------------------------+
  |  NGL method tables  <-- INTERCEPT HERE                   |   level C
  +----------------------------------------------------------+
  |  Xenos command processor + register bus (never reached)   |   level B
  +----------------------------------------------------------+
  |  Vd* / XGetVideoMode kernel calls                         |   level A
  +----------------------------------------------------------+
```

At each intercepted flush point the guest device's full state is read out and
translated into calls on a real `IDirect3DDevice9`. The Xenos command processor
never has to be emulated.

### Hook the flush points, not the setters

The guest's D3D setters should be left alone. They only write shadow state, emit
no commands, and several are inlined into engine code where they could not be
hooked anyway. Intercepting them would be both fragile and unnecessary.

Instead, hook where work is actually submitted:

* `Draw*` (primitive, indexed, immediate)
* `Clear`
* `Resolve`
* `Swap` / present
* constant-buffer updates and resource registration entry points

At each draw, read the complete current state from the guest device and emit
equivalent D3D9 calls. State that was set through unhooked setters is still
correct, because it was written to the guest's own shadow state and is read back
at submit time.

### What actually has to be built

1. **Identify the NGL method tables** and map their entries to D3D9 semantics.
   The vtables are unlabelled, so this means reading the device object's
   construction and matching call sites.
2. **A guest device shadow** mirroring the guest's own state, so the full state
   can be read at each flush point.
3. **State translation** from guest state to `IDirect3DDevice9` calls.

### The hard part: shader translation

D3D9 *calls* translate cleanly. Shaders do not.

The title's shaders are Xenos bytecode — the image contains
`Microsoft (R) Xbox 360 Shader Compiler 2.0.x` — and PC D3D9 requires DXBC.
An intercepted draw therefore needs its shaders translated, either
Xenos→DXBC or by shipping a DXBC recompile path.

This is the dominant remaining graphics cost and should not be underestimated,
but it is a well-scoped compiler problem rather than a hardware emulator, which
is a materially better position than emulating a GPU command processor.

## Level A — display mode (implemented)

**Status: implemented.** This is the only level that crosses the kernel
boundary, and it is the cheapest meaningful win.

The original hardware is pinned to a small set of modes. By answering the
`Vd*`/`XGetVideoMode` queries with the *host desktop* mode instead, the game
builds its render targets and backbuffer at PC-native resolution.

Implemented in `src/runtime/kernel_imports_core.cpp`:

| Import | Behaviour |
|---|---|
| `XGetVideoMode` | Reports the host mode via `cod3_host_display_mode()` |
| `VdGetCurrentDisplayInformation` | Fills the 0x28-byte descriptor with host width/height |
| `VdSetDisplayMode` | No-op; the host window owns the mode |
| `VdQueryVideoMode` | Returns a zeroed descriptor |
| `VdQueryVideoFlags`, `VdPersistDisplay` | Success / no-op |
| `VdGetCurrentDisplayGamma`, `VdSetDisplayGamma` | Gamma fixed at 1.0 |

`cod3_host_display_mode()` currently returns 1280x720 (the Xbox 360 pipeline's
own maximum), and is the single place to raise that once the host window and
backbuffer handling are wired up.

**Caveat, stated plainly:** changing the reported mode only works if the
renderer then renders through our path. On its own it changes the numbers the
game *asks for*; whether pixels reach the screen depends on level B.

## Level B — Xenos command processor emulation (**not required**)

Documented for completeness, because it is the fallback if the NGL interception
route turns out not to be reachable.

The Xbox 360 GPU is not a display controller; it is a general-purpose vector
processor driven by command packets pushed over a register bus. Emulating it
would mean a register bus shim, a packet stream interpreter, and a rasteriser.

**This is no longer the plan.** The port intercepts at level C instead, so the
command processor is never reached. `MmMapIoSpace` remains implemented in
`kernel_imports_core.cpp`; it hands back ordinary guest RAM and logs that
hardware MMIO is not emulated, which is accurate and harmless on the level-C
path.

## Level C — intercepting NGL (planned; this is the approach)

This is the level the port should target, and level B is **not** a prerequisite
for it. Earlier revisions of this document said otherwise and were wrong.

XenonRecomp provides two facilities for it, both available today:

**Function-level overrides.** PPC functions are emitted with a weak alias to an
implementation stub:

```cpp
PPC_FUNC_IMPL(__imp__sub_821FB464);
PPC_FUNC(sub_821FB464)      // weak: replace with your own body
{
    __imp__sub_821FB464(ctx, base);
}
```

Providing a strong definition of any recompiled function replaces it while the
original stays reachable. This is how the NGL flush points get intercepted.

**Mid-asm hooks.** The recompiler TOML can inject a call at a specific
instruction address, passing selected registers:

```toml
[[midasm_hook]]
name = "ViewportOverrideHook"
address = 0x82E26244
registers = ["r3"]
```

This is the right tool for the value-level options below, because it intercepts
*values* (vp dimensions, projection extents) rather than replacing whole
functions.

### Why the setters are deliberately not hooked

The guest's D3D setters (`SetRenderState`, `SetTexture`, `SetVertexShader`, ...)
only write shadow state. They emit no commands, and several are inlined into
engine code where they could not be hooked anyway. Hooking them would be both
fragile and pointless.

Instead, hook the points where work is actually submitted — `Draw*`, `Clear`,
`Resolve`, present/swap, and the constant-buffer and resource-registration entry
points — and read the *full* current state out of the guest device at that
moment. State set through unhooked setters is still correct, because it was
written to the guest's own shadow state and is read back at submit time.

## Options that become available, and where each would attach

| Option | Hook level | Where |
|---|---|---|
| **Resolution scaling** | A + C | Report host mode from `VdGetCurrentDisplayInformation`; scale viewport and backbuffer at NGL device creation |
| **Aspect ratio correction** | C | Override the projection extents NGL builds from the mode descriptor, so 16:9 isn't stretched from the original 4:3 |
| **Higher FOV** | C | Override the horizontal FOV in the projection matrix construction |
| **Anti-aliasing** | C | Override the multisample description at device creation |
| **Shadow / texture quality** | C | Override quality-level lookups inside the asset system |
| **Frame-rate cap / VSync** | C | Wrap the present path and pace submissions from the host thread |

None of these can be delivered before the title boots and a present path exists,
because none can be observed otherwise. They are specified here so the work is
not rediscovered, but they are **not implemented**, and no code in this
repository claims otherwise.

## Prerequisites, in order

1. **The title must boot.** This is the current blocker and is independent of
   graphics; see [STATUS.md](STATUS.md).
2. **Identify the NGL method tables** and map entries to D3D9 semantics.
3. **Build the state translation** to a real `IDirect3DDevice9`.
4. **Solve shader translation** (Xenos bytecode → DXBC).

Steps 2–4 are substantial but bounded. They are *not* the same project as
emulating a GPU, and treating them as such would be a mistake.

## Deliberately not done

The original release's `config/` directory carries console video mode settings.
Those are console-side overrides read by the *kernel*, not the title, and
applying them here would be theatre: the values would not reach the
recompiled code. They are left alone.