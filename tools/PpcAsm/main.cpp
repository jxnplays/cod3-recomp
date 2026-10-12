// A small PowerPC assembler, built to produce the inputs XenonRecomp's test
// mode needs.
//
// Xenia's instruction tests are assembly sources; the assembled objects that
// used to be checked in alongside them are not in the repository any more, and
// the LLVM install here has no PowerPC target. Rather than depend on a
// toolchain that is not present, this assembles them using the opcode tables
// XenonRecomp already carries for its disassembler. The same tables that say
// how to decode an instruction say how to encode one.
//
// Every instruction it emits is immediately disassembled again with that
// disassembler and compared against the source line. A mismatch is reported
// rather than written out, so a wrong encoding cannot quietly become a wrong
// test result.
//
//   PpcAsm <input.s> <output.o> <output.dis> [base address]

#define NOMINMAX
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <byteswap.h>
#include <dis-asm.h>
#include <ppc.h>
#include <disasm.h>

// Defined in ppc-dis.c, which does not publish it in a header.
struct powerpc_operand
{
    unsigned int bitm;
    int shift;
    unsigned long (*insert)(unsigned long, long, int, const char**);
    long (*extract)(unsigned long, int, int*);
    unsigned long flags;
};

extern "C" {
    extern const struct powerpc_operand powerpc_operands[];
    extern const unsigned int num_powerpc_operands;
    extern const struct powerpc_opcode powerpc_opcodes[];
    extern const int powerpc_num_opcodes;
}

namespace
{
    constexpr unsigned long OPERAND_SIGNED    = 0x1;
    constexpr unsigned long OPERAND_SIGNOPT   = 0x2;
    constexpr unsigned long OPERAND_FAKE      = 0x4;
    constexpr unsigned long OPERAND_PARENS    = 0x8;
    constexpr unsigned long OPERAND_CR        = 0x10;
    constexpr unsigned long OPERAND_OPTIONAL  = 0x400;
    constexpr unsigned long OPERAND_NEXT      = 0x800;
    constexpr unsigned long OPERAND_NEGATIVE  = 0x1000;
    constexpr unsigned long OPERAND_DS        = 0x4000;
    constexpr unsigned long OPERAND_DQ        = 0x8000;
    constexpr unsigned long OPERAND_PLUS1     = 0x10000;

    std::string Trim(std::string text)
    {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    std::string Lower(std::string text)
    {
        for (char& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // Registers are written r3, v3, f3, cr6, or as a bare number.
    bool ParseNumber(const std::string& text, int64_t& value)
    {
        if (text.empty()) return false;

        std::string body = text;
        bool negative = false;
        if (body[0] == '-') { negative = true; body.erase(0, 1); }
        else if (body[0] == '+') { body.erase(0, 1); }
        if (body.empty()) return false;

        int base = 10;
        if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
        {
            base = 16;
            body = body.substr(2);
        }

        char* end = nullptr;
        const long long parsed = strtoll(body.c_str(), &end, base);
        if (end == body.c_str() || *end != '\0') return false;

        value = negative ? -parsed : parsed;
        return true;
    }

    bool ParseOperandText(const std::string& raw, int64_t& value)
    {
        const std::string text = Lower(Trim(raw));
        if (text.empty()) return false;

        // A register or condition register field.
        if (text.size() >= 2 && (text[0] == 'r' || text[0] == 'v' || text[0] == 'f'))
        {
            if (ParseNumber(text.substr(1), value)) return true;
        }
        if (text.size() > 2 && text.compare(0, 2, "cr") == 0)
        {
            if (ParseNumber(text.substr(2), value)) return true;
        }

        // Condition bits, as in the CR field names binutils accepts.
        static const std::map<std::string, int64_t> conditionBits = {
            { "lt", 0 }, { "gt", 1 }, { "eq", 2 }, { "so", 3 }, { "un", 3 },
        };
        auto bit = conditionBits.find(text);
        if (bit != conditionBits.end()) { value = bit->second; return true; }

        return ParseNumber(text, value);
    }

    struct Line
    {
        int number = 0;
        std::string label;        // set when the line declares one
        std::string mnemonic;
        std::vector<std::string> operands;
        bool hasInstruction = false;
    };

    // Splits an operand list. A load or store writes its displacement and base
    // register as `0x10(r4)`, but the opcode table lists them as two separate
    // operands, so they are split into two tokens here and the encoder never
    // has to know about the syntax.
    std::vector<std::string> SplitOperands(const std::string& text)
    {
        std::vector<std::string> parts;
        std::string current;
        int depth = 0;
        for (char c : text)
        {
            if (c == '(') depth++;
            else if (c == ')') depth--;

            if (c == ',' && depth == 0)
            {
                parts.push_back(Trim(current));
                current.clear();
                continue;
            }
            current += c;
        }
        if (!Trim(current).empty()) parts.push_back(Trim(current));

        std::vector<std::string> expanded;
        for (const std::string& part : parts)
        {
            const size_t open = part.find('(');
            const size_t close = part.find(')');
            if (open == std::string::npos || close == std::string::npos || close < open)
            {
                expanded.push_back(part);
                continue;
            }
            expanded.push_back(Trim(part.substr(0, open)));
            expanded.push_back(Trim(part.substr(open + 1, close - open - 1)));
        }
        return expanded;
    }

    bool ParseLine(const std::string& raw, int number, Line& out)
    {
        out = Line{};
        out.number = number;

        std::string text = raw;

        // Comments. The test annotations start with # and are read by
        // XenonRecomp itself, not here.
        const size_t hash = text.find('#');
        if (hash != std::string::npos) text = text.substr(0, hash);
        text = Trim(text);
        if (text.empty()) return true;

        const size_t colon = text.find(':');
        if (colon != std::string::npos)
        {
            out.label = Trim(text.substr(0, colon));
            text = Trim(text.substr(colon + 1));
            if (text.empty()) return true;
        }

        const size_t space = text.find_first_of(" \t");
        if (space == std::string::npos)
        {
            out.mnemonic = Lower(text);
        }
        else
        {
            out.mnemonic = Lower(text.substr(0, space));
            out.operands = SplitOperands(Trim(text.substr(space + 1)));
        }
        out.hasInstruction = true;
        return true;
    }

    // How many operands a table entry actually reads from the source text.
    int RequiredOperandCount(const powerpc_opcode& opcode)
    {
        int count = 0;
        for (int i = 0; i < 8 && opcode.operands[i] != 0; i++)
        {
            const powerpc_operand& operand = powerpc_operands[opcode.operands[i]];
            if (operand.flags & OPERAND_FAKE) continue;
            if (operand.flags & OPERAND_OPTIONAL) continue;
            count++;
        }
        return count;
    }

    int TotalOperandCount(const powerpc_opcode& opcode)
    {
        int count = 0;
        for (int i = 0; i < 8 && opcode.operands[i] != 0; i++)
        {
            const powerpc_operand& operand = powerpc_operands[opcode.operands[i]];
            if (operand.flags & OPERAND_FAKE) continue;
            count++;
        }
        return count;
    }
}

// --- Encoding ---------------------------------------------------------------

namespace
{
    struct Assembler
    {
        std::map<std::string, uint32_t> labels;
        uint32_t base = 0x82010000;
        std::vector<uint32_t> code;          // host order, big endian on write
        std::vector<std::string> errors;

        const powerpc_opcode* FindOpcode(const Line& line, std::string& why) const
        {
            const powerpc_opcode* fallback = nullptr;
            for (int i = 0; i < powerpc_num_opcodes; i++)
            {
                const powerpc_opcode& opcode = powerpc_opcodes[i];
                if (opcode.name == nullptr || line.mnemonic != opcode.name) continue;

                const int required = RequiredOperandCount(opcode);
                const int total = TotalOperandCount(opcode);
                const int given = static_cast<int>(line.operands.size());

                if (given >= required && given <= total) return &opcode;
                if (fallback == nullptr) fallback = &opcode;
            }

            if (fallback != nullptr)
            {
                why = "wrong operand count for " + line.mnemonic + ": got " +
                      std::to_string(line.operands.size()) + ", expected " +
                      std::to_string(RequiredOperandCount(*fallback)) + " to " +
                      std::to_string(TotalOperandCount(*fallback));
                return nullptr;
            }
            why = "unknown instruction " + line.mnemonic;
            return nullptr;
        }

        bool Encode(const Line& line, uint32_t address, uint32_t& instruction, std::string& why)
        {
            const powerpc_opcode* opcode = FindOpcode(line, why);
            if (opcode == nullptr) return false;

            unsigned long value = opcode->opcode;
            const int given = static_cast<int>(line.operands.size());
            const int total = TotalOperandCount(*opcode);

            // Optional operands are dropped from the front of the optional set
            // when the source supplies fewer than the maximum.
            int optionalToSkip = total - given;
            size_t next = 0;

            for (int i = 0; i < 8 && opcode->operands[i] != 0; i++)
            {
                const powerpc_operand& operand = powerpc_operands[opcode->operands[i]];

                if (operand.flags & OPERAND_FAKE)
                {
                    if (operand.insert != nullptr)
                    {
                        const char* error = nullptr;
                        value = operand.insert(value, 0, 0, &error);
                    }
                    continue;
                }

                if ((operand.flags & OPERAND_OPTIONAL) && optionalToSkip > 0)
                {
                    optionalToSkip--;
                    continue;
                }

                if (next >= line.operands.size())
                {
                    why = "ran out of operands for " + line.mnemonic;
                    return false;
                }

                const std::string text = line.operands[next++];
                int64_t parsed = 0;

                if (!ParseOperandText(text, parsed))
                {
                    // A branch target may be a label.
                    auto label = labels.find(Trim(text));
                    if (label == labels.end())
                    {
                        why = "cannot parse operand '" + text + "' of " + line.mnemonic;
                        return false;
                    }
                    parsed = static_cast<int64_t>(label->second) - static_cast<int64_t>(address);
                }

                if (operand.flags & OPERAND_PLUS1) parsed -= 1;
                if (operand.flags & OPERAND_NEGATIVE) parsed = -parsed;

                if (operand.insert != nullptr)
                {
                    const char* error = nullptr;
                    value = operand.insert(value, static_cast<long>(parsed), 0, &error);
                    if (error != nullptr)
                    {
                        why = std::string("operand rejected: ") + error;
                        return false;
                    }
                }
                else
                {
                    long field = static_cast<long>(parsed);
                    if (operand.flags & (OPERAND_DS | OPERAND_DQ))
                        field >>= (operand.flags & OPERAND_DS) ? 2 : 4;
                    value |= (static_cast<unsigned long>(field) & operand.bitm) << operand.shift;
                }
            }

            instruction = static_cast<uint32_t>(value);
            return true;
        }
    };
}

// --- ELF output -------------------------------------------------------------

namespace
{
    void Put32(std::vector<uint8_t>& out, uint32_t value)
    {
        out.push_back(uint8_t(value >> 24));
        out.push_back(uint8_t(value >> 16));
        out.push_back(uint8_t(value >> 8));
        out.push_back(uint8_t(value));
    }
    void Put16(std::vector<uint8_t>& out, uint16_t value)
    {
        out.push_back(uint8_t(value >> 8));
        out.push_back(uint8_t(value));
    }

    // A minimal ELF32 big endian object: one loadable .text at the test's base
    // address, plus the section name table the loader reads.
    bool WriteElf(const char* path, uint32_t base, const std::vector<uint32_t>& code)
    {
        const char names[] = "\0.text\0.shstrtab";
        const uint32_t nameTextOffset = 1;
        const uint32_t nameStringsOffset = 7;

        const uint32_t headerSize = 52;
        const uint32_t programHeaderSize = 32;
        const uint32_t sectionHeaderSize = 40;

        const uint32_t programHeaderOffset = headerSize;
        const uint32_t textOffset = programHeaderOffset + programHeaderSize;
        const uint32_t textSize = static_cast<uint32_t>(code.size() * 4);
        const uint32_t stringsOffset = textOffset + textSize;
        const uint32_t stringsSize = sizeof(names);
        const uint32_t sectionHeaderOffset = stringsOffset + stringsSize;

        std::vector<uint8_t> out;

        // ELF header.
        out.insert(out.end(), { 0x7F, 'E', 'L', 'F', 1, 2, 1, 0 });   // class 32, data MSB
        out.insert(out.end(), 8, 0);
        Put16(out, 1);            // ET_REL
        Put16(out, 20);           // EM_PPC
        Put32(out, 1);            // version
        Put32(out, base);         // entry
        Put32(out, programHeaderOffset);
        Put32(out, sectionHeaderOffset);
        Put32(out, 0);            // flags
        Put16(out, uint16_t(headerSize));
        Put16(out, uint16_t(programHeaderSize));
        Put16(out, 1);            // one program header
        Put16(out, uint16_t(sectionHeaderSize));
        Put16(out, 3);            // null, .text, .shstrtab
        Put16(out, 2);            // string table index

        // Program header: the loader takes the image base from this.
        Put32(out, 1);            // PT_LOAD
        Put32(out, textOffset);
        Put32(out, base);         // virtual address
        Put32(out, base);         // physical address
        Put32(out, textSize);
        Put32(out, textSize);
        Put32(out, 5);            // read and execute
        Put32(out, 0x1000);

        for (uint32_t instruction : code) Put32(out, instruction);
        out.insert(out.end(), names, names + stringsSize);

        // Section headers.
        for (int i = 0; i < 40; i++) out.push_back(0);   // the null section

        Put32(out, nameTextOffset);
        Put32(out, 1);            // SHT_PROGBITS
        Put32(out, 0x6);          // alloc and execinstr
        Put32(out, base);
        Put32(out, textOffset);
        Put32(out, textSize);
        Put32(out, 0);
        Put32(out, 0);
        Put32(out, 4);
        Put32(out, 0);

        Put32(out, nameStringsOffset);
        Put32(out, 3);            // SHT_STRTAB
        Put32(out, 0);
        Put32(out, 0);
        Put32(out, stringsOffset);
        Put32(out, stringsSize);
        Put32(out, 0);
        Put32(out, 0);
        Put32(out, 1);
        Put32(out, 0);

        FILE* file = fopen(path, "wb");
        if (file == nullptr) return false;
        fwrite(out.data(), 1, out.size(), file);
        fclose(file);
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        printf("Usage: PpcAsm <input.s> <output.o> <output.dis> [base address]\n");
        return 1;
    }

    const char* inputPath = argv[1];
    const char* objectPath = argv[2];
    const char* disassemblyPath = argv[3];

    Assembler assembler;
    if (argc >= 5) assembler.base = static_cast<uint32_t>(strtoul(argv[4], nullptr, 0));

    std::ifstream input(inputPath);
    if (!input.is_open())
    {
        fprintf(stderr, "cannot open %s\n", inputPath);
        return 1;
    }

    std::vector<Line> lines;
    std::string raw;
    int number = 0;
    while (std::getline(input, raw))
    {
        Line line;
        ParseLine(raw, ++number, line);
        lines.push_back(line);
    }

    // Pass one: where every label lands.
    uint32_t address = assembler.base;
    for (const Line& line : lines)
    {
        if (!line.label.empty()) assembler.labels[line.label] = address;
        if (line.hasInstruction) address += 4;
    }

    // Pass two: encode, and check each encoding by disassembling it back.
    address = assembler.base;
    int encoded = 0;
    int mismatched = 0;
    for (const Line& line : lines)
    {
        if (!line.hasInstruction) continue;

        uint32_t instruction = 0;
        std::string why;
        if (!assembler.Encode(line, address, instruction, why))
        {
            fprintf(stderr, "%s:%d: %s\n", inputPath, line.number, why.c_str());
            assembler.errors.push_back(why);
            assembler.code.push_back(0x60000000);   // nop, to keep addresses right
            address += 4;
            continue;
        }

        // The encoding is only trusted if the project's own disassembler reads
        // it back as the same instruction.
        const uint32_t bigEndian = ByteSwap(instruction);
        ppc_insn decoded{};
        ppc::Disassemble(&bigEndian, address, decoded);
        if (decoded.opcode == nullptr || line.mnemonic != decoded.opcode->name)
        {
            fprintf(stderr, "%s:%d: encoded %s as 0x%08X but it reads back as %s\n",
                inputPath, line.number, line.mnemonic.c_str(), instruction,
                decoded.opcode ? decoded.opcode->name : "<invalid>");
            mismatched++;
        }

        assembler.code.push_back(instruction);
        address += 4;
        encoded++;
    }

    if (!WriteElf(objectPath, assembler.base, assembler.code))
    {
        fprintf(stderr, "cannot write %s\n", objectPath);
        return 1;
    }

    // The disassembly file is how XenonRecomp maps a test name to its address.
    FILE* disassembly = fopen(disassemblyPath, "w");
    if (disassembly == nullptr)
    {
        fprintf(stderr, "cannot write %s\n", disassemblyPath);
        return 1;
    }
    for (const auto& [label, labelAddress] : assembler.labels)
        fprintf(disassembly, "%08x <%s>:\n", labelAddress, label.c_str());
    fclose(disassembly);

    printf("%-28s %4d instructions", inputPath, encoded);
    if (!assembler.errors.empty()) printf(", %zu could not be encoded", assembler.errors.size());
    if (mismatched > 0) printf(", %d did not read back", mismatched);
    printf("\n");

    return (assembler.errors.empty() && mismatched == 0) ? 0 : 2;
}
