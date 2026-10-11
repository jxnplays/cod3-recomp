# Graphics hooks and modernization plan

## The constraint that shapes everything

Call of Duty 3 imports **no Direct3D functions whatsoever**. Verified two ways:

1. The 179 resolved imports (`tools/gen_import_stubs.py` output, derived from
   XenonRecomp's own symbol resolution) contain zero `D3D*` symbols. The only
   graphics-adjacent kernel imports are eight display-mode calls:
   `VdGetCurrentDisplayGamma`, `VdGetCurrentDisplayInformation`,
   `VdPersistDisplay`, `VdQueryVideoFlags`, `VdQueryVideoMode`, `VdSetDisplayMode`,
   `XGetVideoMode`, `XamReadTileToTexture`.
2. String extraction from the extracted PE image finds the Xbox 360 graphics
   stack's own debug strings compiled into `.text`:
   * `nglPresent`, `nglBackBuffer`
   * `"D3D: GPU initialization (HSIO training) has failed so no graphics will render."`
   * `"ERR[D3D]: Unanticipated CPU_INTERRUPT. Sign of a corrupt command buffer?"`
   * `"The GPU is hung! D3D version is %i.%i %s, kernel is %i, frame is %i."`
   * `"NGL: Texture %s destroyed while still referenced by the async renderer."`

**Consequence:** the conventional approach — wrap D3D9/D3D11 and upscale — has
no boundary to attach to. The Xbox 360's Xenos GPU runs a command processor fed
by a proprietary register bus, and the layer that drives it is *inside the
executable we recompiled*. Any graphics work therefore has to happen at one of
three levels, all inside our own code:

```
  +----------------------------------------------------------+
  |  Call of Duty 3 renderer (game code)                     |
  +----------------------------------------------------------+
  |  NGL — Xbox 360 D3D9 derivative, statically linked        |   <- level C
  +----------------------------------------------------------+
  |  Xenos command processor + register bus                    |   <- level B
  +----------------------------------------------------------+
  |  Vd* / XGetVideoMode kernel calls (the only real boundary)|   <- level A
  +----------------------------------------------------------+
```

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

## Level B — command processor emulation (not implemented)

This is the actual blocker for showing anything. The Xbox 360 GPU is not a
display controller; it is a general-purpose vector processor that the game
drives by writing command packets into a push buffer over a register bus, and
the results are read back through another bus. Drawing is therefore not a
matter of calling `DrawIndexedPrimitive` — it is a matter of interpreting a
packet stream and rasterising it.

The work is:

1. **Register bus shim.** `MmMapIoSpace` is the guest's route to the GPU
   registers. It is already implemented in `kernel_imports_core.cpp`, but it
   currently hands back a zeroed scratch buffer and says so loudly. Every
   register write the game makes lands there and is currently discarded.
2. **Packet stream interpreter.** Parse the push buffer the game fills and
   replay the commands (set render target, vertex buffer, texture, draw,
   clear, state blocks).
3. **Rasteriser.** Turn those into something that can be presented. The lowest
   -fidelity but fastest-to-land option is translating packets into
   desktop D3D9 calls; the highest-fidelity option is a software rasteriser
   that reproduces Xenos behaviour including its tile-based deferred setup.

Nothing here is stubbed or faked in the current tree — the port renders nothing.

## Level C — hooks inside NGL (planned)

XenonRecomp has two facilities that make this possible once level B exists, and
both are available today:

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
original stays reachable. This is how NGL's device creation, `nglPresent`, and
render-target setup get intercepted.

**Mid-asm hooks.** The recompiler TOML can inject a call at a specific
instruction address, passing selected registers:

```toml
[[midasm_hook]]
name = "ViewportOverrideHook"
address = 0x82E26244
registers = ["r3"]
```

This is the right tool for the options below, because it intercepts *values*
(vp dimensions, projection extents) rather than replacing whole functions.

## Options that become available, and where each would attach

| Option | Hook level | Where |
|---|---|---|
| **Resolution scaling** | A + C | Report host mode from `VdGetCurrentDisplayInformation`; scale viewport and backbuffer at NGL device creation |
| **Aspect ratio correction** | C | Override the projection extents NGL builds from the mode descriptor, so 16:9 isn't stretched from the original 4:3 |
| **Higher FOV** | C | Override the horizontal FOV in the projection matrix construction |
| **Anti-aliasing** | C | Override the multisample description at device creation |
| **Shadow / texture quality** | C | Override quality-level lookups inside the asset system |
| **Frame-rate cap / VSync** | C | Wrap `nglPresent` and pace submissions from the host thread |

None of these can be delivered before level B exists, because none of them can
be observed without a working present path. The hooks are specified here so the
work is not rediscovered later, but they are **not implemented**, and no code in
this repository claims otherwise.

## Deliberately not done

The original release's `config/` directory carries console video mode settings.
Those are console-side overrides read by the *kernel*, not the title, and
applying them here would be theatre: the values would not reach the
recompiled code. They are left alone.