"""Patches the recompiled code where the recompiler's model does not reach.

XenonRecomp turns every `blr` into a return and can only enter a function at
its start. The title's script threads are coroutines that resume by loading
their registers and branching to wherever they left off, which is neither.
The runtime runs them on host fibers (CoD3Host/coroutines.cpp), and these
two patches are the seams:

  main    The engine's resume point at the end of sub_824A6150: the branch
          into the thread becomes Coroutines::Resume, keyed by the thread
          object the function was called with.
  levels  A level's yield functions call the engine's suspend through the
          interface table (slots 76 and 80) and never expect it to return;
          the recompiler carried on into the next function's code after the
          call. A return is put after each such call, so that a resumed
          thread returns to the script that yielded.

Run after XenonRecomp:  python scripts/patch_recomp.py main
                        python scripts/patch_recomp.py saint_lo
Both are idempotent.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TAB = "\t"


def patch_main() -> int:
    folder = ROOT / "CoD3RecompLib" / "ppc"
    for path in folder.glob("ppc_recomp.*.cpp"):
        text = path.read_text(encoding="ascii")
        start = text.find("PPC_FUNC_IMPL(__imp__sub_824A6150)")
        if start < 0:
            continue
        if "Coroutines::Resume" in text:
            print("main: already patched")
            return 0
        end = text.find("\n}\n", start)
        body = text[start:end]

        switch = TAB + "ctx.r1.u64 = ctx.r11.u64 | ctx.r10.u64;\n"
        at = body.find(switch)
        if at < 0:
            print("main: the stack switch in sub_824A6150 was not found", file=sys.stderr)
            return 1
        key = TAB + "const uint32_t coroutineKey = PPC_LOAD_U32(ctx.r1.u32 + 436);\n"
        body = body[:at] + key + body[at:]

        # The engine's own registers, kept across the thread's run: a thread
        # that finishes returns, on the console, through the frames of the
        # call that began it, into the code after that call with the
        # engine's registers as they were. Here that is a jump to the same
        # place with the same registers.
        body = body[:at] + TAB + "const PPCContext engineRegisters = ctx;\n" + body[at:]
        at += len(TAB + "const PPCContext engineRegisters = ctx;\n")

        # The first return after the switch is the branch into the thread.
        ret = body.find(TAB + "return;\n", at + len(key) + len(switch))
        if ret < 0:
            print("main: the branch after the stack switch was not found", file=sys.stderr)
            return 1
        body = (body[:ret]
                + TAB + "Coroutines::Resume(ctx, base, coroutineKey);\n"
                + TAB + "if (Coroutines::Finished(coroutineKey)) { ctx = engineRegisters; ctx.r1.u32 = engineFrame - 8192; goto loc_thread_finished; }\n"
                + body[ret:])

        # The thread's first run: the engine calls its start on the stack
        # region it reserved, and the thread's first yield longjmps back. That
        # call runs on the thread's fiber from the beginning.
        birth = TAB + "sub_823C0B98(ctx, base);\n"
        at = body.find(birth)
        if at < 0:
            print("main: the thread start call in sub_824A6150 was not found", file=sys.stderr)
            return 1
        body = (body[:at] + TAB + "Coroutines::Begin(ctx, base, PPC_LOAD_U32(ctx.r1.u32 + 8192 + 436), sub_823C0B98);\n"
                + "loc_thread_finished:\n"
                + body[at + len(birth):])

        # The frame's own stack pointer, from the prologue.
        prologue = TAB + "ctx.r1.u32 = ea;\n"
        at = body.find(prologue)
        if at < 0:
            print("main: the prologue of sub_824A6150 was not found", file=sys.stderr)
            return 1
        body = body[:at + len(prologue)] + TAB + "const uint32_t engineFrame = ctx.r1.u32;\n" + body[at + len(prologue):]

        declaration = ("namespace Coroutines {\n"
                       "    void Resume(::PPCContext& ctx, uint8_t* base, uint32_t key);\n"
                       "    void Begin(::PPCContext& ctx, uint8_t* base, uint32_t key, PPCFunc* entry);\n"
                       "    bool Finished(uint32_t key);\n"
                       "}\n")
        text = text[:start] + body + text[end:]
        text = text.replace('#include "ppc_recomp_shared.h"\n',
                            '#include "ppc_recomp_shared.h"\n' + declaration, 1)
        path.write_text(text, encoding="ascii", newline="\n")
        print(f"main: patched {path.name}")
        return 0
    print("main: sub_824A6150 was not found", file=sys.stderr)
    return 1


YIELD = re.compile(
    r"(\tctx\.r11\.s64 = (-?\d+);\n(?:\t//[^\n]*\n)?"
    r"\tctx\.r11\.s64 = ctx\.r11\.s64 \+ (-?\d+);\n(?:\t//[^\n]*\n)?"
    r"\tctx\.r11\.u64 = PPC_LOAD_U32\(ctx\.r11\.u32 \+ (?:76|80)\);\n(?:\t//[^\n]*\n)?"
    r"\tctx\.ctr\.u64 = ctx\.r11\.u64;\n(?:\t//[^\n]*\n)?"
    r"\tctx\.lr = 0x[0-9A-F]+;\n"
    r"\tPPC_CALL_INDIRECT_FUNC\(ctx\.ctr\.u32\);\n)(?!\treturn;   // the suspend)")


def patch_level(name: str) -> int:
    folder = ROOT / "CoD3RecompLib" / "levels" / name
    if not folder.is_dir():
        print(f"{name}: missing {folder}", file=sys.stderr)
        return 1
    sites = 0
    tables = set()
    for path in folder.glob("ppc_recomp.*.cpp"):
        text = path.read_text(encoding="ascii")

        def replacement(match: re.Match) -> str:
            nonlocal sites
            tables.add((int(match.group(2)) + int(match.group(3))) & 0xFFFFFFFF)
            sites += 1
            return match.group(1) + TAB + "return;   // the suspend does not return; a resumed thread returns to its caller\n"

        updated = YIELD.sub(replacement, text)
        if updated != text:
            path.write_text(updated, encoding="ascii", newline="\n")
    if sites == 0:
        print(f"{name}: no yield sites found (already patched, or the level has none)")
    elif len(tables) != 1:
        print(f"{name}: yield sites use {len(tables)} different tables, which is not expected", file=sys.stderr)
        return 1
    else:
        print(f"{name}: {sites} yield sites patched, interface table at {next(iter(tables)):#x}")
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch_recomp.py main|<level>", file=sys.stderr)
        return 1
    return patch_main() if sys.argv[1] == "main" else patch_level(sys.argv[1])


if __name__ == "__main__":
    sys.exit(main())
