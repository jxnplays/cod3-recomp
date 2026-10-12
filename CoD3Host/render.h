#pragma once

// The renderer: the title's frames drawn on the host GPU through Direct3D 11.
//
// The title's own graphics driver writes a Xenos command stream, and the
// command processor in gpu.cpp decodes that into register writes and the
// calls below. Nothing past this point is a model of the console's GPU:
// the registers of a draw are read once into a plain render state
// (render_state.h), the state is mapped to cached Direct3D objects
// (render_pipeline.h), the title's programs are translated to HLSL once
// and kept (render_shaders.h), its vertex buffers and textures are uploaded
// once and kept (render_resources.h), and the draw is one Direct3D call.
// The layers, from the title down:
//
//   game logic (recompiled)  ->  PM4 packets  ->  register file (gpu.cpp)
//     -> Render::Draw / Resolve / Swap             (this interface)
//     -> DrawState, the registers read once         (render_state.h)
//     -> DrawCommand: handles, not descriptions     (render_state.h)
//     -> caches: pipeline, shaders, resources       (render_*.h)
//     -> Direct3D 11, one context, one thread       (render_d3d11.cpp)
//
// The command thread draws; the window thread only pumps messages. The
// swap packet ends the title's frame, and that is where the frame is put on
// the window, with or without vertical sync as the settings say.

#include <cstdint>

namespace Render
{
    // Whether the host GPU is used at all. The device is made on first use.
    bool Enabled();

    // A draw from the command stream: the initiator word, and for an indexed
    // draw the guest address of the indices and their size word.
    void Draw(uint32_t initiator, uint32_t indexBase, uint32_t indexWord);

    // A resolve: the render target the registers describe into the texture
    // at the destination they name, with the clears they ask for.
    void Resolve();

    // A shader upload from the stream: the stage, and where the microcode
    // lies in guest memory. The program becomes the stage's current one.
    void ShaderLoaded(bool pixel, uint32_t guestAddress, uint32_t sizeDwords, uint32_t start = 0);

    // A draw's program from its native record (native_state.cpp): as a
    // load from the stream, the microcode's hash known already.
    void UseProgram(bool pixel, uint32_t guestAddress, uint32_t sizeDwords, uint64_t hash);

    // VdSwap reached in the stream: the front buffer the title finished, by
    // physical address. The frame goes on the window from here.
    void Swap(uint32_t frontBufferPhysical, uint32_t width, uint32_t height);

    // The window thread: the window the frames go on, and its client size
    // whenever it changes. Presenting is not the window thread's business.
    void SetWindow(void* hwnd, int clientWidth, int clientHeight);

    // The window thread, when nothing has been swapped for a while: the
    // last frame again, so the window is not stale under a resize or the
    // overlay. Does nothing when a frame went out recently.
    void PresentIdle();

    // Guest memory the title freed, from any thread: what was uploaded from
    // it (textures, vertex buffers, resolved surfaces) is dropped by the
    // command thread before its next draw, since the address will hold
    // something else soon.
    void MemoryFreed(uint32_t address, uint32_t size);

    // The hash of the stage's current program, for the traces.
    uint64_t CurrentProgramHash(bool pixel);

    // The screenshot key: the next frame's draws go to the log in full, as
    // COD3_D3DFRAME logs one, so a fault seen while playing can be read.
    void LogNextFrame();

    // The ten second report.
    void Report();
}
