// CoD3Scan - XEX reconnaissance for a XenonRecomp project.
// Finds the addresses XenonRecomp's TOML requires and reports image layout.
#define NOMINMAX
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include <file.h>
#include <image.h>
#include <disasm.h>
#include <xbox.h>
#include <fmt/core.h>

// Byte patterns for the register save/restore helpers (from XenonRecomp README).
struct Pattern
{
    const char* tomlKey;
    const char* name;
    std::vector<uint8_t> bytes;
};

static const std::vector<Pattern> kPatterns = {
    { "restgprlr_14_address", "__restgprlr_14", { 0xe9, 0xc1, 0xff, 0x68 } },
    { "savegprlr_14_address", "__savegprlr_14", { 0xf9, 0xc1, 0xff, 0x68 } },
    { "restfpr_14_address",   "__restfpr_14",   { 0xc9, 0xcc, 0xff, 0x70 } },
    { "savefpr_14_address",   "__savefpr_14",   { 0xd9, 0xcc, 0xff, 0x70 } },
    { "restvmx_14_address",   "__restvmx_14",   { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x60, 0xce } },
    { "savevmx_14_address",   "__savevmx_14",   { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x61, 0xce } },
    { "restvmx_64_address",   "__restvmx_64",   { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x60, 0xcb } },
    { "savevmx_64_address",   "__savevmx_64",   { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x61, 0xcb } },
};

static std::vector<size_t> FindAll(const Section& s, const std::vector<uint8_t>& pat)
{
    std::vector<size_t> hits;
    if (s.size < pat.size()) return hits;
    for (size_t i = 0; i + pat.size() <= s.size; i += 4)
    {
        if (memcmp(s.data + i, pat.data(), pat.size()) == 0)
            hits.push_back(s.base + i);
    }
    return hits;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: CoD3Scan <input XEX> [--disasm ADDR COUNT]\n");
        return 1;
    }

    const auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        printf("ERROR: could not read %s\n", argv[1]);
        return 1;
    }
    fmt::println("File: {} ({} bytes on disk)", argv[1], file.size());

    auto image = Image::ParseImage(file.data(), file.size());

    fmt::println("");
    fmt::println("=== IMAGE ===");
    fmt::println("base        = 0x{:X}", image.base);
    fmt::println("size        = 0x{:X} ({:.2f} MB)", image.size, image.size / 1048576.0);
    fmt::println("entry_point = 0x{:X}", image.entry_point);

    fmt::println("");
    fmt::println("=== SECTIONS ===");
    const Section* text = nullptr;
    const Section* pdata = nullptr;
    for (const auto& s : image.sections)
    {
        fmt::println("{:<10} base=0x{:08X} size=0x{:08X} flags={}{}",
            s.name, s.base, s.size,
            (s.flags & SectionFlags_Code) ? "CODE " : "",
            (s.flags & SectionFlags_Data) ? "DATA" : "");
        if (s.name == ".text") text = &s;
        if (s.name == ".pdata") pdata = &s;
    }

    if (pdata == nullptr)
        fmt::println("\n!! WARNING: no .pdata section - Recompiler::Analyse would crash.");
    else
        fmt::println("\n.pdata holds {} runtime function entries",
            pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION));

    // ---- Register save/restore helper discovery -------------------------
    fmt::println("");
    fmt::println("=== SAVE/RESTORE HELPERS (paste into [main]) ===");
    std::map<std::string, size_t> found;
    for (const auto& p : kPatterns)
    {
        std::vector<size_t> hits;
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code)) continue;
            auto h = FindAll(s, p.bytes);
            hits.insert(hits.end(), h.begin(), h.end());
        }

        if (hits.empty())
        {
            fmt::println("# {:<22} NOT FOUND", p.name);
        }
        else
        {
            found[p.tomlKey] = hits[0];
            fmt::println("{} = 0x{:X}   # {}{}", p.tomlKey, hits[0], p.name,
                hits.size() > 1 ? fmt::format(" ({} candidates!)", hits.size()) : "");
            for (size_t i = 1; i < hits.size() && i < 8; i++)
                fmt::println("#   also at 0x{:X}", hits[i]);
        }
    }

    // ---- Imports ---------------------------------------------------------
    fmt::println("");
    fmt::println("=== IMPORT THUNKS (named symbols) ===");
    size_t namedCount = 0;
    size_t rtlUnwind = 0;
    for (const auto& sym : image.symbols)
    {
        if (sym.name.empty()) continue;
        namedCount++;
        if (sym.name.find("RtlUnwind") != std::string::npos)
        {
            rtlUnwind = sym.address;
            fmt::println("  {} @ 0x{:X}", sym.name, sym.address);
        }
    }
    fmt::println("named symbols total: {}", namedCount);

    // ---- Callers of RtlUnwind (longjmp lives among them) -----------------
    if (rtlUnwind != 0 && text != nullptr)
    {
        fmt::println("");
        fmt::println("=== CALLERS OF RtlUnwind (longjmp candidates) ===");
        int n = 0;
        for (size_t i = 0; i + 4 <= text->size; i += 4)
        {
            uint32_t insn = ByteSwap(*(uint32_t*)(text->data + i));
            if (PPC_OP(insn) == PPC_OP_B && PPC_BL(insn))
            {
                size_t target = text->base + i + PPC_BI(insn);
                if (target == rtlUnwind)
                {
                    fmt::println("  bl at 0x{:X}", text->base + i);
                    if (++n >= 20) { fmt::println("  ..."); break; }
                }
            }
        }
        if (n == 0) fmt::println("  none (game may not use setjmp/longjmp)");
    }

    // ---- Tail of .text: padding / handler data for invalid_instructions --
    if (text != nullptr)
    {
        fmt::println("");
        fmt::println("=== TAIL OF .text (last 16 words) ===");
        size_t start = text->size >= 64 ? text->size - 64 : 0;
        for (size_t i = start; i + 4 <= text->size; i += 4)
        {
            uint32_t w = ByteSwap(*(uint32_t*)(text->data + i));
            fmt::println("  0x{:08X}: 0x{:08X}", text->base + i, w);
        }

        // Count distinct word values that appear as inter-function filler.
        std::map<uint32_t, size_t> zeroRuns;
        size_t zeros = 0;
        for (size_t i = 0; i + 4 <= text->size; i += 4)
            if (*(uint32_t*)(text->data + i) == 0) zeros++;
        fmt::println("\nzero words in .text: {}", zeros);
    }

    // ---- Dump every import the guest needs from the kernel ---------------
    // This is the work list for the runtime: each name has to be implemented
    // or stubbed before the recompiled code can link and run.
    if (argc >= 3 && strcmp(argv[2], "--imports") == 0)
    {
        fmt::println("");
        fmt::println("=== IMPORTS ({} named symbols) ===", namedCount);
        std::vector<std::pair<std::string, size_t>> imports;
        for (const auto& sym : image.symbols)
            if (!sym.name.empty()) imports.emplace_back(sym.name, sym.address);
        std::sort(imports.begin(), imports.end());
        for (const auto& [name, addr] : imports)
            fmt::println("  0x{:X}  {}", addr, name);
        return 0;
    }

    // ---- Derive explicit function boundaries from a recompiler log -------
    // XenonAnalyse's function analyzer cuts a function short at a jump-table
    // `bctr`, because the table data that follows does not look like code.
    // The recompiler then reports every switch case that lands past the end.
    // This reads those reports and works out the real function extent.
    if (argc >= 4 && strcmp(argv[2], "--fixfuncs") == 0)
    {
        struct Broken { uint32_t size = 0; uint32_t maxLabel = 0; int cases = 0; std::vector<uint32_t> labels; };
        std::map<uint32_t, Broken> broken;

        FILE* f = fopen(argv[3], "rb");
        if (f == nullptr) { fmt::println("ERROR: cannot open log {}", argv[3]); return 1; }
        char line[1024];
        while (fgets(line, sizeof(line), f))
        {
            // Logs may be UTF-16 from PowerShell redirection; skip NULs.
            std::string s;
            for (char* p = line; *p; ++p) if (*p != '\0') s += *p;
            const char* m = strstr(s.c_str(), "is trying to jump outside function: ");
            if (m == nullptr) continue;
            uint32_t label = 0, fnBase = 0, fnSize = 0;
            if (sscanf(m, "is trying to jump outside function: %X [fn 0x%X size 0x%X]",
                       &label, &fnBase, &fnSize) != 3)
                continue;
            auto& b = broken[fnBase];
            b.size = fnSize;
            b.maxLabel = std::max(b.maxLabel, label);
            b.labels.push_back(label);
            b.cases++;
        }
        fclose(f);

        // Authoritative function starts, used as a hard stop for the scan.
        std::vector<uint32_t> pdataStarts;
        if (pdata != nullptr)
        {
            size_t count = pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
            auto* pf = (IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
            for (size_t i = 0; i < count; i++)
                pdataStarts.push_back(ByteSwap(pf[i].BeginAddress));
            std::sort(pdataStarts.begin(), pdataStarts.end());
        }

        // Pass 1: bound each reported function.
        struct Range { uint32_t base, end, wasSize; int cases; };
        std::vector<Range> ranges;
        int bad = 0;
        for (const auto& [fnBase, b] : broken)
        {
            // Hard stop: the next function start .pdata knows about.
            uint32_t limit = 0xFFFFFFFF;
            auto pit = std::upper_bound(pdataStarts.begin(), pdataStarts.end(), b.maxLabel);
            if (pit != pdataStarts.end()) limit = *pit;

            // Follow every case body to its terminator and take the furthest
            // one. Bounding by the next padding word instead would overshoot
            // into the following function whenever the two are adjacent with
            // no alignment gap between them.
            uint32_t end = 0;
            for (uint32_t label : b.labels)
            {
                for (uint32_t a = label; a < limit; a += 4)
                {
                    const void* code = image.Find(a);
                    if (code == nullptr) break;
                    uint32_t w = ByteSwap(*(const uint32_t*)code);
                    if (w == 0) { end = std::max(end, a); break; }        // hit padding
                    bool terminator = (w == 0x4E800020)                   // blr
                                   || (w == 0x4E800420)                   // bctr
                                   || ((w >> 26) == 18 && (w & 1) == 0);  // b, not bl
                    if (terminator) { end = std::max(end, a + 4); break; }
                    if (a - label > 0x4000) break;                        // runaway guard
                }
            }

            if (end <= fnBase || end <= b.maxLabel || end - fnBase > 0x20000)
            {
                fmt::println("# FAILED 0x{:X}: could not bound (maxLabel 0x{:X}, limit 0x{:X})",
                    fnBase, b.maxLabel, limit);
                bad++;
                continue;
            }

            ranges.push_back({ fnBase, end, b.size, b.cases });
        }

        // Pass 2: merge overlaps. A function holding several jump tables gets
        // reported once per table, and each report bounds to the same padding,
        // so the inner reports are fragments of one real function.
        std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b) { return a.base < b.base; });

        std::vector<Range> merged;
        int absorbed = 0;
        for (const auto& r : ranges)
        {
            if (!merged.empty() && r.base < merged.back().end)
            {
                merged.back().end = std::max(merged.back().end, r.end);
                merged.back().cases += r.cases;
                absorbed++;
                continue;
            }
            merged.push_back(r);
        }

        // Pass 3: sanity-check each start. A real function start is preceded by
        // padding or by a terminator (blr / unconditional branch).
        fmt::println("");
        fmt::println("# {} reports -> {} functions ({} fragments absorbed)",
            ranges.size(), merged.size(), absorbed);
        fmt::println("# Bounded by the alignment padding after the last switch case.");
        int unverified = 0;
        for (auto& m : merged)
        {
            const void* prev = image.Find(m.base - 4);
            bool okStart = false;
            if (prev != nullptr)
            {
                uint32_t w = ByteSwap(*(const uint32_t*)prev);
                okStart = (w == 0)                              // padding
                       || (w == 0x4E800020)                     // blr
                       || ((w >> 26) == 18 && (w & 1) == 0);    // b (not bl)
            }
            if (!okStart) { m.cases = -m.cases; unverified++; } // flag for output
        }
        fmt::println("functions = [");
        for (const auto& m : merged)
        {
            fmt::println("    {{ address = 0x{:X}, size = 0x{:X} }},{} # was 0x{:X}, {} cases",
                m.base, m.end - m.base,
                m.cases < 0 ? " # UNVERIFIED START" : "",
                m.wasSize, m.cases < 0 ? -m.cases : m.cases);
        }
        fmt::println("]");
        fmt::println("# {} functions, {} with an unverified start, {} unbounded",
            merged.size(), unverified, bad);
        return bad == 0 ? 0 : 2;
    }

    // ---- Every call site that targets an address -------------------------
    // Finding who calls a function is how a wrapper gets traced back to the
    // code that uses it, which is the usual question when working out why an
    // object is never signalled.
    if (argc >= 4 && strcmp(argv[2], "--callers") == 0)
    {
        const uint32_t wanted = static_cast<uint32_t>(strtoull(argv[3], nullptr, 0));

        fmt::println("");
        fmt::println("=== CALLERS OF 0x{:X} ===", wanted);
        int found = 0;
        for (const auto& section : image.sections)
        {
            if (!(section.flags & SectionFlags_Code)) continue;
            for (size_t offset = 0; offset + 4 <= section.size; offset += 4)
            {
                const uint32_t insn = ByteSwap(*(uint32_t*)(section.data + offset));
                if (PPC_OP(insn) != PPC_OP_B || !PPC_BL(insn)) continue;

                const size_t site = section.base + offset;
                if (size_t(site + PPC_BI(insn)) != wanted) continue;

                fmt::println("  0x{:08X}", site);
                if (++found >= 60) { fmt::println("  ..."); break; }
            }
            if (found >= 60) break;
        }
        if (found == 0) fmt::println("  none");
        return 0;
    }

    // ---- Every store to a given structure offset -------------------------
    //
    // A field that is never written is worth finding, and searching for one
    // encoding at a time means guessing which registers the compiler used.
    // This walks the code and matches on the displacement instead.
    if (argc >= 4 && strcmp(argv[2], "--storeoff") == 0)
    {
        const int32_t wanted =
            static_cast<int32_t>(strtoll(argv[3], nullptr, 0));

        fmt::println("");
        fmt::println("=== STORES TO OFFSET {} (0x{:X}) ===", wanted, wanted);
        int found = 0;
        for (const auto& section : image.sections)
        {
            if (!(section.flags & SectionFlags_Code)) continue;
            for (size_t offset = 0; offset + 4 <= section.size; offset += 4)
            {
                const uint32_t insn = ByteSwap(*(uint32_t*)(section.data + offset));
                const uint32_t opcode = insn >> 26;

                // 36 stw, 37 stwu, 38 stb, 44 sth, 54 stfd, 62 std/stdu.
                if (opcode != 36 && opcode != 37 && opcode != 62) continue;

                int32_t displacement = int16_t(insn & 0xFFFF);
                if (opcode == 62) displacement &= ~3;   // the low bits are the form
                if (displacement != wanted) continue;

                fmt::println("  0x{:08X}  {} r{}, {}(r{})",
                    section.base + offset,
                    opcode == 62 ? "std" : (opcode == 37 ? "stwu" : "stw"),
                    (insn >> 21) & 31, displacement, (insn >> 16) & 31);
                if (++found >= 40) { fmt::println("  ..."); break; }
            }
            if (found >= 40) break;
        }
        if (found == 0) fmt::println("  none");
        return 0;
    }

    // ---- Generic byte-pattern search ------------------------------------
    if (argc >= 4 && strcmp(argv[2], "--find") == 0)
    {
        std::string hex = argv[3];
        std::vector<uint8_t> pat;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            pat.push_back((uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16));

        fmt::println("");
        fmt::println("=== SEARCH {} ({} bytes) ===", hex, pat.size());
        int n = 0;
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code)) continue;
            for (size_t a : FindAll(s, pat))
            {
                fmt::println("  0x{:X}  [{}]", a, s.name);
                if (++n >= 40) { fmt::println("  ..."); break; }
            }
            if (n >= 40) break;
        }
        if (n == 0) fmt::println("  no match");
    }

    // ---- .pdata function lookup -----------------------------------------
    if (argc >= 4 && strcmp(argv[2], "--func") == 0 && pdata != nullptr)
    {
        size_t addr = strtoull(argv[3], nullptr, 0);
        size_t count = pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
        auto* pf = (IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
        fmt::println("");
        fmt::println("=== .pdata LOOKUP for 0x{:X} ===", addr);
        bool hit = false;
        for (size_t i = 0; i < count; i++)
        {
            auto fn = pf[i];
            fn.BeginAddress = ByteSwap(fn.BeginAddress);
            fn.Data = ByteSwap(fn.Data);
            size_t begin = fn.BeginAddress;
            size_t len = fn.FunctionLength * 4;
            if (addr >= begin && addr < begin + len)
            {
                fmt::println("  entry {}: 0x{:X} .. 0x{:X}  (size 0x{:X}, {} instrs)",
                    i, begin, begin + len, len, (uint32_t)fn.FunctionLength);
                fmt::println("  prologLength={} thirtyTwoBit={} exceptionFlag={}",
                    (uint32_t)fn.PrologLength, (uint32_t)fn.ThirtyTwoBit, (uint32_t)fn.ExceptionFlag);
                hit = true;
                // Show the neighbours too.
                for (int d = -1; d <= 1; d += 2)
                {
                    long j = (long)i + d;
                    if (j < 0 || (size_t)j >= count) continue;
                    auto nb = pf[j];
                    fmt::println("  neighbour {}: 0x{:X} (size 0x{:X})",
                        j, (uint32_t)ByteSwap(nb.BeginAddress),
                        ((ByteSwap(nb.Data) >> 8) & 0x3FFFFF) * 4);
                }
                break;
            }
        }
        if (!hit) fmt::println("  address is not covered by any .pdata entry");
    }

    // ---- Optional disassembly window ------------------------------------
    if (argc >= 4 && strcmp(argv[2], "--disasm") == 0)
    {
        size_t addr = strtoull(argv[3], nullptr, 0);
        size_t count = argc >= 5 ? strtoull(argv[4], nullptr, 0) : 16;
        fmt::println("");
        fmt::println("=== DISASM 0x{:X} ===", addr);
        for (size_t i = 0; i < count; i++)
        {
            size_t a = addr + i * 4;
            const void* code = image.Find(a);
            if (code == nullptr) { fmt::println("  0x{:X}: <unmapped>", a); break; }
            ppc_insn insn;
            ppc::Disassemble(code, a, insn);
            if (insn.opcode == nullptr)
            {
                fmt::println("  0x{:08X}: {:08X}  <invalid>", a, ByteSwap(*(uint32_t*)code));
                continue;
            }

            // Operands matter more than the mnemonic when reading a routine to
            // work out what it does, so print the text form the disassembler
            // builds rather than just the name.
            fmt::println("  0x{:08X}: {:08X}  {:<12}{}", a, ByteSwap(*(uint32_t*)code),
                insn.opcode->name, insn.op_str);
        }
    }

    return 0;
}
