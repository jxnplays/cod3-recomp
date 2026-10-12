# The title's D3D device

The Xbox 360 D3D linked into default.xex keeps its whole GPU state in one
device structure, a copy of the GPU's registers, and writes it out as PM4
packets only when a draw needs it. Everything below was read from the
draw (`sub_822F3A28`) and the shader load (`sub_822FB4B0`); see
[native-d3d11-plan.md](native-d3d11-plan.md) for what it is for.

The device is at **0x40003C00** in a level (the four worker threads all
draw on it, one at a time; the title serialises them), reached through
the pointer at 0x82001144.

## The draw

`sub_822F3A28(device r3, primitive r4, index offset r5, start index r6, index count r7)`,
~890 calls a frame. Before the draw packet it writes out whatever the dirty
masks say changed, then writes `VGT_INDX_OFFSET` and a predicated
`DRAW_INDX` with the index buffer from device+12164.

## Dirty masks

Five 64 bit masks at the start; the draw writes a register block for each
set bit group and clears the mask.

| field | what it covers |
| --- | --- |
| +0 | vertex shader float constants (the block at +1792) |
| +8 | pixel shader float constants (the block at +5888) |
| +16 | the shaders (`sub_822FB4B0`), and the blocks at +10400, +10420 and the fetch/sampler state |
| +24 | the blocks at +9984 (booleans and loops), +10240, +10316, +10468 |
| +32 | the blocks at +10552, +10704, and fetch constants (`sub_822FA420`) |

## Register copies

`sub_822FA360(device, bits, first register, copy)` writes a block of
registers from its copy. So a register's value is at
`device + copy + (register - first) * 4`:

| copy at | first register | holds |
| --- | --- | --- |
| +1792 | 0x4000 | vertex shader float constants (256 x 4) |
| +5888 | 0x4400 | pixel shader float constants (256 x 4) |
| +9984 | 0x4900 | boolean and loop constants |
| +10240 | 0x2000 | |
| +10316 | 0x2100 | |
| +10400 | 0x2180 | SQ_PROGRAM_CNTL and the shader setup |
| +10420 | 0x2200 | the render backend: RB_MODECONTROL is +10452 (0x2208) |
| +10468 | 0x2280 | |
| +10552 | 0x2300 | |
| +10704 | 0x2380 | |

## Other fields

| field | holds |
| --- | --- |
| +40, +48 | the command buffer being written: write pointer, end of the segment |
| +10452 | RB_MODECONTROL; the shader load sets it to 5 (depth only) when there is no pixel shader, 4 otherwise |
| +10772 | the scratch block: its +4 is the flag the stream waits on after a swap, which the vertical blank clears |
| +10808..+10810 | flag bytes; bit 1 of +10808 sends the draw down another path with more packets, not understood yet |
| +12164 | the index buffer object: +0 flags and format, +24 address |
| +12416 | the pixel shader object; nought is none, and the draw is depth only |
| +12420 | the vertex shader object |
| +13476, +13480 | the ring: base, size mask |
| +15120..+15136 | the vertical blank callback, the blank counter, the last flip's blank, the swap countdown |

A shader object's microcode is at `*(object + *(object + 64) + 40) + *(object + 24)`,
`*(object + *(object + 64) + 44) / 4` words long (the pixel shader path of
`sub_822FB4B0`).
