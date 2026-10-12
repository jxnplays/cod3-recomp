// A disassembler for Xenos shader microcode.
//
// The command stream says which triangles to draw but not what they look like.
// That is decided by two programs the title uploads before each draw, written
// in the GPU's own instruction set, and until those can be read there is no
// way to know what colour anything is meant to be.
//
// The encoding here was checked against the programs this title actually
// uploads rather than assumed. Two of them agree in a way a wrong guess would
// not: every vertex program begins by allocating a position export and every
// pixel program by allocating a pixel export, which is what the hardware
// requires and what the alloc type field has to mean.
//
//   XenosDis shader.bin [shader.bin ...]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    std::vector<uint32_t> ReadWords(const char* path)
    {
        std::vector<uint32_t> words;
        FILE* file = fopen(path, "rb");
        if (file == nullptr) return words;

        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);

        std::vector<uint8_t> bytes(size_t(size < 0 ? 0 : size));
        if (!bytes.empty()) fread(bytes.data(), 1, bytes.size(), file);
        fclose(file);

        // Microcode is stored the way the console stores it, most significant
        // byte first.
        for (size_t i = 0; i + 4 <= bytes.size(); i += 4)
        {
            words.push_back(uint32_t(bytes[i]) << 24 | uint32_t(bytes[i + 1]) << 16 |
                            uint32_t(bytes[i + 2]) << 8 | uint32_t(bytes[i + 3]));
        }
        return words;
    }

    // --- control flow ------------------------------------------------------

    const char* ControlFlowName(uint32_t opcode)
    {
        switch (opcode)
        {
        case 0:  return "nop";
        case 1:  return "exec";
        case 2:  return "exec_end";
        case 3:  return "cond_exec";
        case 4:  return "cond_exec_end";
        case 5:  return "cond_pred_exec";
        case 6:  return "cond_pred_exec_end";
        case 7:  return "loop_start";
        case 8:  return "loop_end";
        case 9:  return "cond_call";
        case 10: return "return";
        case 11: return "cond_jmp";
        case 12: return "alloc";
        case 13: return "cond_exec_pred_clean";
        case 14: return "cond_exec_pred_clean_end";
        default: return "mark_vs_fetch_done";
        }
    }

    const char* AllocName(uint32_t type)
    {
        switch (type)
        {
        case 0:  return "no memory";
        case 1:  return "position";
        case 2:  return "interpolators or pixel";
        default: return "memory export";
        }
    }

    // --- arithmetic --------------------------------------------------------

    const char* VectorName(uint32_t opcode)
    {
        static const char* const names[32] = {
            "add", "mul", "max", "min", "seq", "sgt", "sge", "sne",
            "frc", "trunc", "floor", "mad", "cndeq", "cndge", "cndgt", "dp4",
            "dp3", "dp2add", "cube", "max4", "setp_eq_push", "setp_ne_push",
            "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge",
            "kill_ne", "dst", "maxa", "?", "?" };
        return names[opcode & 31];
    }

    const char* ScalarName(uint32_t opcode)
    {
        static const char* const names[64] = {
            "adds", "adds_prev", "muls", "muls_prev", "muls_prev2",
            "maxs", "mins", "seqs", "sgts", "sges", "snes",
            "frcs", "truncs", "floors", "exp", "logc", "log",
            "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq",
            "maxas", "maxasf", "subs", "subs_prev", "setp_eq", "setp_ne",
            "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr",
            "setp_rstr", "kills_eq", "kills_gt", "kills_ge", "kills_ne",
            "kills_one", "sqrt", "?", "mulsc", "mulsc", "addsc", "addsc",
            "subsc", "subsc", "sin", "cos", "retain_prev",
            "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?" };
        return names[opcode & 63];
    }

    // A destination register, named the way the pipeline stage sees it.
    void PrintDestination(uint32_t reg, bool exported, bool pixelShader)
    {
        if (!exported) { printf("r%u", reg); return; }
        if (pixelShader)
        {
            if (reg <= 3) printf("oC%u", reg);
            else if (reg == 61) printf("oDepth");
            else printf("export%u", reg);
            return;
        }
        if (reg == 62) printf("oPos");
        else if (reg == 63) printf("oPointSize");
        else printf("oInterp%u", reg);
    }

    // A source operand: which file it comes from and how its components are
    // rearranged. A swizzle field of zero is the identity.
    void PrintSource(uint32_t reg, bool isTemporary, uint32_t swizzle, bool negate)
    {
        printf("%s%s%u", negate ? "-" : "", isTemporary ? "r" : "c", reg);
        if (swizzle == 0) return;

        static const char component[4] = { 'x', 'y', 'z', 'w' };
        printf(".");
        for (uint32_t i = 0; i < 4; i++)
            printf("%c", component[(i + ((swizzle >> (i * 2)) & 3)) & 3]);
    }

    void PrintAlu(uint32_t index, uint32_t d0, uint32_t d1, uint32_t d2,
                  bool pixelShader)
    {
        const uint32_t vectorDest = d0 & 0x3F;
        const uint32_t scalarDest = (d0 >> 8) & 0x3F;
        const bool exported = ((d0 >> 15) & 1) != 0;
        const uint32_t vectorMask = (d0 >> 16) & 0xF;
        const uint32_t scalarMask = (d0 >> 20) & 0xF;
        const uint32_t scalarOpcode = (d0 >> 26) & 0x3F;

        const uint32_t src3Swizzle = d1 & 0xFF;
        const uint32_t src2Swizzle = (d1 >> 8) & 0xFF;
        const uint32_t src1Swizzle = (d1 >> 16) & 0xFF;
        // Which file each source reads from lives in the top three bits of the
        // last word. This is not where it first looked: the giveaway was a
        // vertex program whose position export decoded as a constant, which
        // would collapse every triangle it draws to a point. Reading these
        // three bits instead makes it export the register the vertex fetch
        // wrote, which is what a working program does.
        const bool src1Temporary = ((d2 >> 31) & 1) != 0;
        const bool src2Temporary = ((d2 >> 30) & 1) != 0;
        const bool src3Temporary = ((d2 >> 29) & 1) != 0;
        const bool predicated = ((d1 >> 30) & 1) != 0;

        const uint32_t src3Reg = d2 & 0x3F;
        const uint32_t src2Reg = (d2 >> 8) & 0x3F;
        const uint32_t src1Reg = (d2 >> 16) & 0x3F;
        const uint32_t vectorOpcode = (d2 >> 24) & 0x1F;
        const bool src3Negate = ((d2 >> 6) & 1) != 0;
        const bool src2Negate = ((d2 >> 14) & 1) != 0;
        const bool src1Negate = ((d2 >> 22) & 1) != 0;

        static const char component[4] = { 'x', 'y', 'z', 'w' };

        printf("    %3u  ", index);
        if (predicated) printf("[pred] ");

        if (vectorMask != 0)
        {
            printf("%s ", VectorName(vectorOpcode));
            PrintDestination(vectorDest, exported, pixelShader);
            if (vectorMask != 0xF)
            {
                printf(".");
                for (uint32_t i = 0; i < 4; i++)
                    if (vectorMask & (1u << i)) printf("%c", component[i]);
            }
            printf(", ");
            PrintSource(src1Reg, src1Temporary, src1Swizzle, src1Negate);
            printf(", ");
            PrintSource(src2Reg, src2Temporary, src2Swizzle, src2Negate);
            if (vectorOpcode == 11 || (vectorOpcode >= 12 && vectorOpcode <= 14))
            {
                printf(", ");
                PrintSource(src3Reg, src3Temporary, src3Swizzle, src3Negate);
            }
            printf("\n");
        }

        if (scalarMask != 0)
        {
            printf("         %s ", ScalarName(scalarOpcode));
            PrintDestination(scalarDest, exported, pixelShader);
            printf(", ");
            PrintSource(src3Reg, src3Temporary, src3Swizzle, src3Negate);
            printf("\n");
        }

        if (vectorMask == 0 && scalarMask == 0)
            printf("(nothing written)\n");
    }

    void PrintFetch(uint32_t index, uint32_t d0, uint32_t d1, uint32_t d2)
    {
        const uint32_t opcode = d0 & 0x1F;
        const uint32_t sourceReg = (d0 >> 5) & 0x3F;
        const uint32_t destReg = (d0 >> 12) & 0x3F;
        const uint32_t constant = (d0 >> 20) & 0x1F;

        printf("    %3u  ", index);
        if (opcode == 0)
        {
            // A vertex fetch pulls one attribute out of a vertex buffer.
            // The stride and the offset share the last word: eight bits of
            // stride, then the offset. Reading the whole word as an offset made
            // a three float position look like it started 768 dwords into the
            // buffer, which no vertex layout does.
            const uint32_t format = (d1 >> 16) & 0x3F;
            const uint32_t stride = d2 & 0xFF;
            const uint32_t offset = (d2 >> 8) & 0x7FFFFF;
            const uint32_t select = (d0 >> 25) & 3;
            const bool mini = ((d1 >> 30) & 1) != 0;
            printf("vfetch r%u, r%u, vertex constant %u, format %u, "
                   "offset %u, stride %u%s\n",
                destReg, sourceReg, constant * 3 + select, format, offset,
                stride, mini ? ", reusing the previous stride" : "");
        }
        else if (opcode == 1)
        {
            printf("tfetch r%u, r%u, texture constant %u\n",
                destReg, sourceReg, constant);
        }
        else
        {
            printf("fetch opcode %u, r%u, r%u, constant %u\n",
                opcode, destReg, sourceReg, constant);
        }
        (void)d1;
    }

    void Disassemble(const char* path)
    {
        const std::vector<uint32_t> words = ReadWords(path);
        printf("\n%s: %zu dwords\n", path, words.size());
        if (words.size() < 3) { printf("  too short to be a program\n"); return; }

        // The control flow block comes first. Instructions are 48 bits, packed
        // two into every three dwords. Its length is not stated, so it runs
        // until the first exec instruction's target, which is the earliest
        // address any of them can point at.
        uint32_t firstTarget = uint32_t(words.size() / 3);
        std::vector<uint64_t> controlFlow;

        for (uint32_t slot = 0; slot * 3 + 2 < words.size(); slot++)
        {
            if (slot >= firstTarget) break;

            const uint32_t d0 = words[slot * 3 + 0];
            const uint32_t d1 = words[slot * 3 + 1];
            const uint32_t d2 = words[slot * 3 + 2];

            const uint64_t a = uint64_t(d0) | (uint64_t(d1 & 0xFFFF) << 32);
            const uint64_t b = uint64_t(d1 >> 16) | (uint64_t(d2) << 16);

            for (uint64_t instruction : { a, b })
            {
                controlFlow.push_back(instruction);
                const uint32_t opcode = uint32_t(instruction >> 44) & 0xF;
                if (opcode >= 1 && opcode <= 6)
                {
                    const uint32_t address = uint32_t(instruction) & 0xFFF;
                    if (address != 0 && address < firstTarget) firstTarget = address;
                }
            }
        }

        // Is this a pixel program? A vertex program allocates a position
        // export and a pixel program does not, which is the one reliable way
        // to tell them apart from the microcode alone.
        bool pixelShader = true;
        for (uint64_t instruction : controlFlow)
        {
            const uint32_t opcode = uint32_t(instruction >> 44) & 0xF;
            if (opcode == 12 && ((uint32_t(instruction >> 41) & 3) == 1))
                pixelShader = false;
        }
        printf("  looks like a %s program\n", pixelShader ? "pixel" : "vertex");

        printf("  control flow:\n");
        for (size_t i = 0; i < controlFlow.size(); i++)
        {
            const uint64_t instruction = controlFlow[i];
            const uint32_t opcode = uint32_t(instruction >> 44) & 0xF;
            if (opcode == 0 && instruction == 0) continue;

            printf("    %3zu  %s", i, ControlFlowName(opcode));
            if (opcode >= 1 && opcode <= 6)
            {
                printf(" address %u, count %u",
                    uint32_t(instruction) & 0xFFF,
                    (uint32_t(instruction) >> 12) & 7);
            }
            else if (opcode == 12)
            {
                printf(" %s, size %u",
                    AllocName((uint32_t(instruction >> 41)) & 3),
                    uint32_t(instruction) & 7);
            }
            printf("\n");
        }

        // Everything after the control flow block is instructions the exec
        // instructions point at. Whether a slot holds arithmetic or a fetch is
        // not in the slot itself: each exec carries two bits per instruction it
        // runs, and the low bit of the pair says the instruction is a fetch.
        // Deciding it by the shape of the word instead gets it wrong, which is
        // how a texture read first came out as an add.
        std::vector<uint8_t> isFetch(words.size() / 3 + 1, 0);
        for (uint64_t instruction : controlFlow)
        {
            const uint32_t opcode = uint32_t(instruction >> 44) & 0xF;
            if (opcode < 1 || opcode > 6) continue;

            const uint32_t address = uint32_t(instruction) & 0xFFF;
            const uint32_t count = (uint32_t(instruction) >> 12) & 7;
            const uint32_t serialize = uint32_t(instruction >> 16) & 0xFFF;
            for (uint32_t i = 0; i < count; i++)
            {
                if (address + i >= isFetch.size()) break;
                isFetch[address + i] = ((serialize >> (i * 2)) & 1) != 0;
            }
        }

        printf("  instructions:\n");
        for (uint32_t slot = firstTarget; slot * 3 + 2 < words.size(); slot++)
        {
            const uint32_t d0 = words[slot * 3 + 0];
            const uint32_t d1 = words[slot * 3 + 1];
            const uint32_t d2 = words[slot * 3 + 2];
            if (d0 == 0 && d1 == 0 && d2 == 0) continue;

            if (slot < isFetch.size() && isFetch[slot]) PrintFetch(slot, d0, d1, d2);
            else PrintAlu(slot, d0, d1, d2, pixelShader);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: XenosDis shader.bin [shader.bin ...]\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) Disassemble(argv[i]);
    return 0;
}
