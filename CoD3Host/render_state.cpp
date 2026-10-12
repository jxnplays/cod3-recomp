#include "render_state.h"
#include "render.h"
#include "gpu.h"

#include <cstring>

namespace
{
    thread_local const uint32_t* t_registers = nullptr;

    inline uint32_t Reg(uint32_t index)
    {
        if (t_registers != nullptr) return t_registers[index];
        return Gpu::RegisterFile()[index].load(std::memory_order_relaxed);
    }

    inline float RegFloat(uint32_t index)
    {
        const uint32_t bits = Reg(index);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }
}

void RenderState::Read(uint32_t initiator, uint32_t indexBase, Snapshot& out)
{
    out.primitive = initiator & 0x3F;
    out.sourceSelect = (initiator >> 6) & 3;
    out.indexCount = initiator >> 16;
    out.wideIndices = ((initiator >> 11) & 1) != 0;
    out.indexBase = indexBase;

    out.pitch = Reg(SurfaceInfo) & 0x3FFF;
    out.colorInfo[0] = Reg(ColorInfo0);
    out.colorInfo[1] = Reg(ColorInfo1);
    out.colorInfo[2] = Reg(ColorInfo2);
    out.colorInfo[3] = Reg(ColorInfo3);
    out.depthInfo = Reg(DepthInfo);
    out.modeControl = Reg(ModeControl) & 7;
    out.colorMask = Reg(ColorMask);
    out.depthControl = Reg(DepthControl);
    out.stencilRefMask = Reg(StencilRefMask);

    out.suScModeControl = Reg(SuScModeControl);
    for (uint32_t i = 0; i < 4; i++) out.polyOffset[i] = RegFloat(PolyOffsetFrontScale + i);
    out.scissorTopLeft = Reg(ScissorTopLeft);
    out.scissorBottomRight = Reg(ScissorBottomRight);
    out.windowOffset = Reg(WindowOffset);
    out.indexOffset = int32_t(Reg(IndexOffset));
    out.minVertexIndex = Reg(MinVertexIndex);
    out.maxVertexIndex = Reg(MaxVertexIndex);
    out.resetIndex = Reg(ResetIndex);
    out.pointSize = Reg(PointSize);
    out.vertexControl = Reg(VertexControl);

    out.blendControl[0] = Reg(BlendControl0);
    out.blendControl[1] = Reg(BlendControl1);
    out.blendControl[2] = Reg(BlendControl2);
    out.blendControl[3] = Reg(BlendControl3);
    for (uint32_t i = 0; i < 4; i++) out.blendFactor[i] = RegFloat(BlendRed + i);

    out.vteControl = Reg(VteControl);
    out.colorControl = Reg(ColorControl);
    out.alphaReference = Reg(AlphaReference);
    out.programControl = Reg(ProgramControl);
    out.contextMisc = Reg(ContextMisc);
    for (uint32_t i = 0; i < 6; i++) out.viewport[i] = RegFloat(ViewportXScale + i);

    out.vertexProgram = Render::CurrentProgramHash(false);
    out.pixelProgram = Render::CurrentProgramHash(true);
}

void RenderState::ReadTextureFetch(uint32_t slot, uint32_t words[6])
{
    for (uint32_t i = 0; i < 6; i++) words[i] = Reg(FetchConstants + slot * 6 + i);
}

void RenderState::ReadVertexFetch(uint32_t slot, uint32_t& word0, uint32_t& word1)
{
    word0 = Reg(FetchConstants + slot * 2);
    word1 = Reg(FetchConstants + slot * 2 + 1);
}

void RenderState::ReadResolve(ResolveSnapshot& out)
{
    out.control = Reg(CopyControl);
    out.destBase = Reg(CopyDestBase);
    out.destInfo = Reg(CopyDestInfo);
    out.pitch = Reg(SurfaceInfo) & 0x3FFF;
    out.scissorTopLeft = Reg(ScissorTopLeft);
    out.scissorBottomRight = Reg(ScissorBottomRight);
    out.colorInfo[0] = Reg(ColorInfo0);
    out.colorInfo[1] = Reg(ColorInfo1);
    out.colorInfo[2] = Reg(ColorInfo2);
    out.colorInfo[3] = Reg(ColorInfo3);
    out.depthInfo = Reg(DepthInfo);
    out.colorClearValue = Reg(CopyColorClear);
    out.depthClearValue = Reg(CopyDepthClear);
}

void RenderState::UseRegisters(const uint32_t* registers) { t_registers = registers; }

bool RenderState::UsingRecord() { return t_registers != nullptr; }

uint32_t RenderState::Register(uint32_t index) { return Reg(index); }

const uint32_t* RenderState::Registers()
{
    // The file's words are written on this thread: plain reads of them.
    return t_registers != nullptr ? t_registers : reinterpret_cast<const uint32_t*>(Gpu::RegisterFile());
}
