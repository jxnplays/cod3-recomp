#pragma once

// The PC render layer: the draw and the resolve the title's registers
// describe, turned into commands the Direct3D executor runs.
//
// This is where the console's terms end. A draw comes in as the register
// file (RenderState::Snapshot) and goes out as a DrawCommand: programs and
// pipeline states as handles from the caches, textures and buffers as the
// views the caches keep, the scissor in host pixels, the constants and the
// indices already streamed into their rings. What is not needed to draw
// the same picture on a PC is decided here and does not reach the
// executor: the EDRAM tile a surface is named by becomes a render target
// handle, a fetch constant becomes a view and a sampler, a byte swapped
// index list becomes a range of the index ring.

#include "render_state.h"

namespace RenderCommands
{
    // The draw the registers describe, as a command. False when there is
    // nothing to draw: the skip has been counted with its reason.
    bool PrepareDraw(uint32_t initiator, uint32_t indexBase, RenderState::DrawCommand& out);

    // The resolve the registers describe, as a command. False when it
    // amounts to nothing.
    bool PrepareResolve(RenderState::ResolveCommand& out);

    // The draws that were not made, by reason, for the report.
    void ReportSkips();

    // Whether the title's blur while aiming down the sights is drawn: its
    // depth of field pass, three draws the pixel programs name.
    void SetAimBlur(bool drawn);
}
