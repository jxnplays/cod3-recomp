#!/usr/bin/env python3
"""Show the translated form of a guest function from XenonRecomp's output.

XenonRecomp already decodes each PowerPC instruction and emits it as a comment
above the corresponding C++ in build/ppc/ppc_recomp.*.cpp. This tool extracts
that for one function so the boot path can be read without a separate
disassembler. It reads what the recompiler actually emitted, so it can never
disagree with the code that is running.

Usage:
    python ppc_disasm.py <guest_addr_or_name> [--dir build/ppc]
"""

import argparse
import glob
import os
import re
import sys

FUNC_RE = re.compile(r"^PPC_FUNC(?:_IMPL)?\(__imp__(\w+)\) \{$")
# Instruction comments look like:  // stwu r1,-496(r1)
INSN_RE = re.compile(r"^\s*//\s+(.+?)\s*$")


def find_function(ppc_dir, target):
    """Locate the generated definition of `target` (name or 0x address)."""
    want_name = target
    if target.lower().startswith("0x"):
        want_name = "sub_" + target[2:].upper()

    files = sorted(glob.glob(os.path.join(ppc_dir, "ppc_recomp.*.cpp")))
    for path in files:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        except OSError:
            continue

        for i, line in enumerate(lines):
            m = FUNC_RE.match(line.strip())
            if not m:
                continue
            name = m.group(1)
            if name != want_name and name != target:
                continue

            # Collect until the matching closing brace.
            out = [f"// {name}  ({os.path.basename(path)})"]
            for j in range(i + 1, len(lines)):
                if lines[j].strip() == "}":
                    break
                im = INSN_RE.match(lines[j])
                if im:
                    out.append(f"    {im.group(1)}")
            return out, None

    return None, want_name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", help="function name (e.g. _xstart) or address (0x82344D00)")
    ap.add_argument("--dir", default="build/ppc")
    ap.add_argument("--all", action="store_true",
                    help="dump every function rather than requiring a match")
    args = ap.parse_args()

    if args.all:
        names = []
        for path in sorted(glob.glob(os.path.join(args.dir, "ppc_recomp.*.cpp"))):
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = FUNC_RE.match(line.strip())
                    if m:
                        names.append(m.group(1))
        print(f"{len(names)} functions generated")
        for n in names[:50]:
            print("   ", n)
        return

    out, wanted = find_function(args.dir, args.target)
    if out is None:
        print(f"no generated function matching '{args.target}' (tried '{wanted}')",
              file=sys.stderr)
        print("hint: the entry point is named _xstart; try --all to list names",
              file=sys.stderr)
        sys.exit(1)

    print("\n".join(out))


if __name__ == "__main__":
    main()