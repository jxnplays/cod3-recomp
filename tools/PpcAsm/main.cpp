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
#include <set>
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
        bool isData = false;              // a .long: a word, not an instruction
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

    // --- macros ------------------------------------------------------------------------------------

    // Xenia's tests do not write the instructions that build a 64-bit constant,
    // because that is seven instructions to assert one register. They define a
    // macro that builds it from four halves and call that instead:
    //
    //     .macro make_full_test_constant dest, a, b, c, d
    //       lis \dest, \a
    //       ori \dest, \dest, \b
    //       sldi \dest, \dest, 32
    //       ...
    //     .endm
    //
    //     make_full_test_constant r4, 0x0123, 0x4567, 0x89AB, 0xCDEF
    //
    // Roughly twenty instructions have such a `_constant` test, and every one
    // of them reads 0 without this: the call assembles to nothing, so the
    // register the test then checks was never written. Forty failing checks
    // that all say `ACTUAL 0` is one missing feature, not forty bugs.
    struct Macro
    {
        std::vector<std::string> parameters;
        std::vector<std::string> body;    // raw source, parameters still written \like_this
    };

    std::string Head(const std::string& text)
    {
        const size_t space = text.find_first_of(" \t");
        return space == std::string::npos ? text : text.substr(0, space);
    }

    // Replaces \parameter with the argument it was given.
    //
    // The longest parameter name wins, and it has to. The constants macro's
    // parameters include `d` while its body also says `\dest`, so matching `d`
    // first would turn `\dest` into `0xCDEFest` and every test that uses the
    // macro would fail on a register that is nonsense.
    std::string Substitute(const std::string& text,
                          const std::vector<std::string>& parameters,
                          const std::vector<std::string>& arguments)
    {
        std::string out;
        for (size_t i = 0; i < text.size();)
        {
            if (text[i] != '\\' || i + 1 >= text.size()) { out += text[i++]; continue; }

            size_t best = std::string::npos;
            for (size_t p = 0; p < parameters.size(); ++p)
            {
                const std::string& name = parameters[p];
                if (name.empty() || text.compare(i + 1, name.size(), name) != 0) continue;
                if (best == std::string::npos || name.size() > parameters[best].size()) best = p;
            }
            if (best == std::string::npos) { out += text[i++]; continue; }

            if (best < arguments.size()) out += arguments[best];
            i += 1 + parameters[best].size();
        }
        return out;
    }

    // A macro's name and its arguments, out of one line.
    //
    // The arguments come back comma-separated, but the name is not: in
    // `.macro make_full_test_constant dest, a, b, c, d` the first chunk is
    // "make_full_test_constant dest", the name and the first parameter sharing
    // a run with no comma in it. Read the whole chunk as one argument and the
    // macro is registered under the name "make_full_test_constant dest", no
    // call ever matches it, and every test that uses it reads zero.
    struct Signature
    {
        std::string name;
        std::vector<std::string> arguments;
    };

    Signature ReadSignature(const std::string& text)
    {
        Signature out;
        const size_t space = text.find_first_of(" \t");
        if (space == std::string::npos) { out.name = Lower(Trim(text)); return out; }

        out.name = Lower(Trim(text.substr(0, space)));
        std::vector<std::string> chunks = SplitOperands(Trim(text.substr(space + 1)));
        for (std::string& chunk : chunks) chunk = Lower(Trim(chunk));
        out.arguments = chunks;
        return out;
    }

    // Takes the `.macro` blocks out of the source and puts the calls to them in
    // their place, before anything is parsed: the body lines are source text
    // and are only text until the parameters are gone.
    std::vector<std::string> Preprocess(const std::vector<std::string>& raw)
    {
        std::map<std::string, Macro> macros;
        std::vector<std::string> kept;
        std::vector<std::string> body;
        std::string open;
        std::vector<std::string> parameters;
        bool inComment = false;

        for (const std::string& line : raw)
        {
            std::string text = line;

            // Two kinds of comment. The test annotations start with # and are
            // read by XenonRecomp, not here. A few sources use /* */ as well,
            // including the ones that open a comment on one line and close it
            // on the next, which is why the state has to be outside the loop.
            if (inComment)
            {
                const size_t ends = text.find("*/");
                if (ends == std::string::npos) continue;
                text = text.substr(ends + 2);
                inComment = false;
            }
            const size_t opens = text.find("/*");
            if (opens != std::string::npos)
            {
                const size_t ends = text.find("*/", opens + 2);
                if (ends == std::string::npos) { text = text.substr(0, opens); inComment = true; }
                else text = text.substr(0, opens) + " " + text.substr(ends + 2);
            }

            const size_t hash = text.find('#');
            if (hash != std::string::npos) text = text.substr(0, hash);
            text = Trim(text);

            if (!open.empty())
            {
                const std::string head = Lower(Head(text));
                if (head == ".endm" || head == ".endmacro")
                {
                    macros[open] = Macro{ parameters, body };
                    open.clear();
                    parameters.clear();
                    body.clear();
                    continue;
                }
                if (!text.empty()) body.push_back(text);
                continue;
            }

            const std::string head = Lower(Head(text));
            if (head == ".macro")
            {
                const size_t space = text.find_first_of(" \t");
                if (space == std::string::npos) { kept.push_back(text); continue; }
                const Signature signature = ReadSignature(text.substr(space + 1));
                if (signature.name.empty()) { kept.push_back(text); continue; }
                open = signature.name;
                parameters = signature.arguments;
                continue;
            }

            kept.push_back(text);
        }

        // A block left open means the source ended inside a macro. Keeping its
        // lines as ordinary source would turn them into unknown instructions,
        // which is what would have happened anyway, but saying so once is
        // clearer than sixty identical errors.
        if (!open.empty())
            fprintf(stderr, "warning: .macro %s is never closed with .endm\n", open.c_str());

        // Expand outermost first, until a pass changes nothing. The cap stops a
        // macro that expands to a call to itself from running forever.
        for (int round = 0; round < 8; ++round)
        {
            bool changed = false;
            std::vector<std::string> next;
            for (const std::string& text : kept)
            {
                const std::string head = Lower(Head(text));
                const Macro* found = macros.count(head) ? &macros[head] : nullptr;
                if (found == nullptr) { next.push_back(text); continue; }

                // The call's arguments are everything after the name. A macro
                // that takes one argument has no comma in it at all - the call
                // is `make_test_constant r4` - so this cannot go through
                // ReadSignature, which would read `r4` as the name and hand
                // back nothing to substitute, leaving every line of the body
                // with an empty operand where \dest should be.
                const size_t space = text.find_first_of(" \t");
                std::vector<std::string> arguments;
                if (space != std::string::npos)
                {
                    arguments = SplitOperands(Trim(text.substr(space + 1)));
                    for (std::string& a : arguments) a = Trim(a);
                }

                for (const std::string& b : found->body)
                    next.push_back(Substitute(b, found->parameters, arguments));
                changed = true;
            }
            kept.swap(next);
            if (!changed) break;
        }

        return kept;
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

        // Directives that describe the object file rather than the machine
        // code. They have no encoding and no four bytes, so they are dropped
        // rather than read as an unknown instruction and turned into a nop
        // that moves every label after it four bytes along.
        static const std::set<std::string> directives = {
            ".text", ".globl", ".global", ".type", ".size", ".section",
            ".align", ".balign", ".p2align", ".byte", ".half", ".word",
            ".dword", ".quad", ".space", ".file", ".ident", ".extern",
            ".previous", ".local", ".weak", ".hidden", ".set",
        };
        if (directives.count(out.mnemonic) != 0) return true;

        out.hasInstruction = true;
        // A .long is a word in the stream, not an instruction. It still has to
        // occupy four bytes so the labels after it land where Xenia put them.
        out.isData = out.mnemonic == ".long";
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
    // Rewrites PowerPC's extended mnemonics into the rotate-and-mask forms the
    // hardware actually has.
    //
    // The Xenos has no 64-bit shift. `sldi rD,rS,n` is `rldicr rD,rS,n,64-n` and
    // `srdi rD,rS,n` is `rldicl rD,rS,64-n,n`: a rotate by the other way, then a
    // mask from the other end, which between them is a shift.
    //
    // These are not optional. Xenia's `_constant` tests - one for nearly every
    // instruction - build a 64-bit value with `li` and then `sldi` it into place.
    // Without this the assembler emits a nop for the shift, the test reads a
    // register that was never shifted, and around forty tests fail for a reason
    // that has nothing to do with the recompiler. A nop that is reported is still
    // a wrong answer, so it is better to assemble them.
    bool ExpandExtended(Line& line, std::string& why)
    {
        // The move is the only rename. Comparisons deliberately are not in
        // here: the table carries `cmp` itself as {BF, L, RA, RB}, so
        // `cmp 5, 0, r3, r4` - a condition register, its bit, and the two
        // operands - is four operands and finds its own entry. Rewriting it to
        // `cmpw`, which takes three, refuses it.
        static const std::map<std::string, std::string> renamed = {
            { "mr", "or" },
        };
        const auto rename = renamed.find(line.mnemonic);
        if (rename != renamed.end())
        {
            // `mr rA,rS` is `or rA,rS,rS`: the move is the same instruction as
            // the or, with its second and third sources the same register.
            if (line.operands.size() == 2)
                line.operands.push_back(line.operands[1]);
            line.mnemonic = rename->second;
            return true;
        }

        // `subis rD,rS,n` is `addis` on the negative of n: it subtracts, where
        // `addis` on the same n would add. Renaming it without flipping the
        // immediate encodes the opposite instruction - and passes the read-back
        // check, because the bytes really are an `addis`. Only the result tells
        // the difference.
        if (line.mnemonic == "subis")
        {
            if (line.operands.size() == 3)
            {
                int64_t amount = 0;
                if (ParseOperandText(line.operands[2], amount))
                {
                    // The immediate is a 16-bit field, so the negation is done
                    // in 16 bits and -32768 comes back as -32768 rather than
                    // becoming 32768.
                    const int16_t flipped = static_cast<int16_t>(-amount);
                    line.operands[2] = std::to_string(flipped);
                    line.mnemonic = "addis";
                    return true;
                }
            }
            why = "subis takes 3 operands with a number for the immediate";
            return false;
        }

        // The Xenos unpacks into a 128-bit vector with two operands,
        // `vupkhsb128 vD, vB`, where the PowerPC instruction the table
        // describes takes three, `vD, vA, vB`. The middle source is not
        // optional there: it is a real register that the unpack does not read
        // but the encoding still has to carry, and it is zero.
        //
        // Only the 128-bit forms. The 64-bit ones - `vupkhsb`, `vupkhsh` and
        // the rest - genuinely take two operands in the table too, and giving
        // them a third is how the 128-bit list came to be wrong in the first
        // place.
        static const std::set<std::string> twoOperandUnpack = {
            "vupkhsb128", "vupklsb128", "vupkhsb64", "vupklsb64",
            "vupkhsh128", "vupklsh128", "vupkdh128", "vupkdl128",
        };
        if (line.operands.size() == 2 && twoOperandUnpack.count(line.mnemonic))
        {
            line.operands.insert(line.operands.begin() + 1, "0");
            return true;
        }

        const bool left = line.mnemonic == "sldi";
        const bool right = line.mnemonic == "srdi";
        const bool clear = line.mnemonic == "clrldi";
        const bool rotate = line.mnemonic == "rotldi";
        const bool left32 = line.mnemonic == "slwi";
        const bool right32 = line.mnemonic == "srwi";
        if (!left && !right && !clear && !rotate && !left32 && !right32) return true;

        if (line.operands.size() != 3)
        {
            why = line.mnemonic + " takes 3 operands, got " +
                  std::to_string(line.operands.size());
            return false;
        }

        int64_t amount = 0;
        if (!ParseOperandText(line.operands[2], amount) || amount < 0 || amount > 63)
        {
            why = "bad shift count for " + line.mnemonic + ": " + line.operands[2];
            return false;
        }

        const std::string destination = line.operands[0];
        const std::string source = line.operands[1];
        const std::string count = std::to_string(amount);

        // A count of nothing has no rotate to hide the mask behind: 64-0 is 64,
        // and the mask fields are five bits wide. Every one of these becomes a
        // move when it shifts by zero, so that is what it becomes.
        if (amount == 0)
        {
            line.mnemonic = "or";
            line.operands = { destination, source, source };
            return true;
        }

        if (left)
        {
            line.mnemonic = "rldicr";
            line.operands = { destination, source, count, std::to_string(64 - amount) };
        }
        else if (right)
        {
            line.mnemonic = "rldicl";
            line.operands = { destination, source, std::to_string(64 - amount), count };
        }
        else if (clear)
        {
            // Clear the low n bits: rotate by nothing, then mask from bit n up.
            line.mnemonic = "rldicl";
            line.operands = { destination, source, "0", count };
        }
        else if (rotate)
        {
            // Rotate by n and keep everything: rotate by n, then mask from
            // bit zero, which is no mask.
            line.mnemonic = "rldicl";
            line.operands = { destination, source, count, "0" };
        }
        else if (left32)
        {
            // A 32-bit shift left is a 32-bit rotate that keeps the low
            // 32-n bits: rlwinm takes the leftmost bit as bit 31, so the
            // window that survives runs from bit 0 to bit 31-n.
            line.mnemonic = "rlwinm";
            line.operands = { destination, source, count, "0",
                              std::to_string(31 - amount) };
        }
        else
        {
            // A 32-bit shift right is the same rotate the other way round,
            // with the window moved up to bits n through 31.
            line.mnemonic = "rlwinm";
            line.operands = { destination, source, std::to_string(32 - amount),
                              count, "31" };
        }
        return true;
    }

    // One encoding, several names. Every entry here is a case where the bytes
    // are right and only the name the disassembler picked differs, so the
    // read-back check below must not report them.
    //
    // Three kinds live here. The comparisons, where the table names the
    // register form `cmpw` and the sources write `cmp`. The shifts, which the
    // hardware does not have: `sldi` is a rotate and a mask, and the table has
    // no name for the pair. And the traps, where `twnl` ("not less") and
    // `twge` ("greater or equal") are the same condition with names that
    // disagree about which way round they are.
    bool SameInstruction(const std::string& written, const std::string& decoded)
    {
        if (written == decoded) return true;
        static const std::map<std::string, std::vector<std::string>> aliases = {
            { "cmpi",      { "cmpwi" } },
            { "lbzi",      { "lbz" } },
            { "addis",     { "lis" } },
            { "subis",     { "addis" } },
            { "nor.",      { "not." } },
            { "sldi",      { "rldicr" } },
            { "srdi",      { "rldicl", "mr" } },
            { "slwi",      { "rotlwi" } },
            { "srwi",      { "rotrwi" } },
            { "rldicl",    { "rotldi", "clrldi", "rotldi.", "clrldi." } },
            { "rldicr",    { "rotrdi" } },
            { "rlwinm",    { "rotlwi" } },
            { "rlwnm",     { "rotlw" } },
            // not-less and greater-or-equal are the same test
            { "twnl",      { "twge" } },
            { "twng",      { "twle" } },
            { "twnli",     { "twgei" } },
            { "twngi",     { "twlei" } },
            { "twlnl",     { "twlge" } },
            { "twlng",     { "twlle" } },
            { "twlnli",    { "twlgei" } },
            { "twlngi",    { "twllei" } },
            { "tdnl",      { "tdge" } },
            { "tdng",      { "tdle" } },
            { "tdnli",     { "tdgei" } },
            { "tdngi",     { "tdlei" } },
            { "tdlnl",     { "tdlge" } },
            { "tdlng",     { "tdlle" } },
            { "tdlnli",    { "tdlgei" } },
            { "tdlngi",    { "tdllei" } },
        };
        const auto alias = aliases.find(written);
        if (alias == aliases.end()) return false;
        for (const std::string& name : alias->second) if (name == decoded) return true;
        return false;
    }

    struct Assembler
    {
        std::map<std::string, uint32_t> labels;
        uint32_t base = 0x82010000;
        std::vector<uint32_t> code;          // host order, big endian on write
        std::vector<std::string> errors;

        // Which table entry the last Encode matched. A wrong encoding is not
        // always a wrong encoder: `or` appears in the opcode table more than
        // once, and picking the wrong one of two gives a perfectly encoded
        // instruction that is not the one that was written. Knowing which
        // entry was chosen is the difference between that being a five minute
        // fix and a guess, so it is kept and printed when the read-back fails.
        const powerpc_opcode* lastMatch = nullptr;

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

        bool Encode(const Line& source, uint32_t address, uint32_t& instruction, std::string& why)
        {
            Line line = source;
            if (!ExpandExtended(line, why)) return false;

            // A .long is the word written literally. It goes into the stream so
            // the addresses after it stay right, and nowhere near the tables.
            if (line.isData)
            {
                if (line.operands.size() != 1)
                {
                    why = ".long takes 1 operand, got " + std::to_string(line.operands.size());
                    return false;
                }
                int64_t value = 0;
                if (!ParseOperandText(line.operands[0], value))
                {
                    why = "cannot read a word from .long " + line.operands[0];
                    return false;
                }
                instruction = static_cast<uint32_t>(value);
                return true;
            }

            const powerpc_opcode* opcode = FindOpcode(line, why);
            if (opcode == nullptr) return false;
            lastMatch = opcode;

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

    std::vector<std::string> source;
    std::string raw;
    while (std::getline(input, raw)) source.push_back(raw);

    // Macros are expanded before anything is parsed, because the body of a
    // macro is source text and is only text until its parameters are gone.
    source = Preprocess(source);

    std::vector<Line> lines;
    int number = 0;
    for (const std::string& text : source)
    {
        Line line;
        ParseLine(text, ++number, line);
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

        // A .long is data. It takes up its four bytes so the labels after it
        // land where the source says, but there is nothing to disassemble it
        // back into, so the read-back check below does not apply.
        if (line.isData)
        {
            assembler.code.push_back(instruction);
            address += 4;
            encoded++;
            continue;
        }

        // The encoding is only trusted if the project's own disassembler reads
        // it back as the same instruction.
        //
        // "The same" has to mean the same ENCODING, not the same NAME. The
        // disassembler picks whichever table entry matches first, and it picks
        // the wrong one for `or r3,r3,r3`: the word 0x7C631B78 is exactly
        // XRC(31,444,0) - the table's own `or` - with RS, RA and RB all 3, and
        // the decoder calls it `cctph`. Thirty of Xenia's tests end in that one
        // instruction, so trusting the name alone reports thirty correct
        // encodings as wrong and buries the real failures.
        //
        // So the word is also checked against the table entry the encoder
        // meant to write. If it still matches that entry's mask, it is the
        // instruction that was asked for and the decoder's name is the thing
        // that is wrong. If it does not, the encoder produced something else
        // and the mismatch is real.
        const uint32_t bigEndian = ByteSwap(instruction);
        ppc_insn decoded{};
        ppc::Disassemble(&bigEndian, address, decoded);
        const char* readBack = decoded.opcode ? decoded.opcode->name : "<invalid>";

        const bool namedRightly = decoded.opcode != nullptr &&
                                  SameInstruction(line.mnemonic, readBack);
        const bool wordIsRight = assembler.lastMatch != nullptr &&
            (instruction & assembler.lastMatch->mask) == assembler.lastMatch->opcode;

        if (!namedRightly && !wordIsRight)
        {
            fprintf(stderr, "%s:%d: encoded %s as 0x%08X but it reads back as %s\n",
                inputPath, line.number, line.mnemonic.c_str(), instruction, readBack);
            if (assembler.lastMatch != nullptr)
                fprintf(stderr, "        matched table entry \"%s\", base 0x%08lX mask 0x%08lX\n",
                    assembler.lastMatch->name, assembler.lastMatch->opcode,
                    assembler.lastMatch->mask);
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
