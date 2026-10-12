"""Finds the functions the analyser cut into pieces and writes their real
extents for the recompiler.

XenonRecomp can only enter a function at its start and leave it by a return,
so a branch from one of its functions into another is emitted as
"// ERROR <address>" and a return. Nearly all of them come from one thing:
the analyser ended a function early (at a jump table, at a case body's
unconditional branch to a tail placed after the last case, at a compare
chain's "not found" tail), and the rest of the function became one or more
"functions" of its own whose branches back into the first are impossible.

This reads the generated code, groups each such branch with the function
that holds its target, and writes the group's extent, from the first piece
to the end of the last, into config/<name>_functions.toml, which
scripts/recompile_level.ps1 merges into the level's config on every pass.
It is run again after each recompile until nothing changes: a tail that
had itself been cut short shows up as a new piece the next time.

Pieces that begin eight bytes before a real function (the two words of
padding read as code) are left alone: nothing calls them, and their errors
are the real function's labels.

    python scripts/level_functions.py saint_lo

Prints the number of entries added or grown; exit status 0 either way.
"""
import bisect
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read_functions(folder):
    """start -> {'labels': set, 'errors': set, 'first': [first instructions]}"""
    functions = {}
    for path in sorted(folder.glob("ppc_recomp.*.cpp")):
        current = None
        for line in path.read_text(encoding="ascii", errors="replace").splitlines():
            m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", line)
            if m:
                current = int(m.group(1), 16)
                functions[current] = {"labels": set(), "errors": set(), "first": []}
                continue
            if current is None:
                continue
            m = re.match(r"loc_([0-9A-F]+):", line)
            if m:
                functions[current]["labels"].add(int(m.group(1), 16))
                continue
            m = re.search(r"// ERROR ([0-9A-F]+)", line)
            if m:
                functions[current]["errors"].add(int(m.group(1), 16))
                continue
            m = re.match(r"\s*// (\S.*)", line)
            if m and len(functions[current]["first"]) < 2:
                functions[current]["first"].append(m.group(1))
    return functions


def padding_piece(start, functions, starts):
    """A 'function' of two words before a real one: padding read as code."""
    i = bisect.bisect_right(starts, start)
    return i < len(starts) and starts[i] == start + 8


def extents(functions):
    starts = sorted(functions)

    def end_of(start):
        i = bisect.bisect_right(starts, start)
        return starts[i] if i < len(starts) else start + 4

    ranges = []
    for start in starts:
        errors = functions[start]["errors"]
        if not errors or padding_piece(start, functions, starts):
            continue
        # The piece each target lies in: the one whose labels name it, or
        # else the one whose range holds it (a target inside a piece that
        # nothing else branches to has no label there).
        holders = set(h for h in starts if functions[h]["labels"] & errors)
        for target in errors:
            i = bisect.bisect_right(starts, target) - 1
            if i >= 0 and starts[i] != start and target < end_of(starts[i]):
                holders.add(starts[i])
        for holder in sorted(holders):
            lo = min(start, holder)
            hi = max(end_of(start), end_of(holder))
            if padding_piece(lo, functions, starts):
                lo += 8
            ranges.append([lo, hi])
        if not holders:
            # The target is past every piece: the function runs at least
            # to the target's own instruction.
            ranges.append([start, max(end_of(start), max(errors) + 4)])
    ranges.sort()
    merged = []
    for lo, hi in ranges:
        if merged and lo < merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    return merged


def read_kept(path):
    kept = {}
    if path.exists():
        for m in re.finditer(r"\{ address = 0x([0-9A-F]+), size = 0x([0-9A-F]+) \}", path.read_text(encoding="ascii")):
            kept[int(m.group(1), 16)] = int(m.group(2), 16)
    return kept


def write_kept(path, name, kept):
    lines = [
        f"# Function boundaries kept by hand and by scripts/level_functions.py for {name},",
        "# merged into the recompiler's config on every pass (these win over the scan's).",
        "# Each is a function the analyser cut into pieces: to the recompiler every",
        "# branch between the pieces was \"// ERROR\" and a return, and the function did",
        "# less than it should. The large ones are compare chains over hashed names;",
        "# the tail they all reach holds the \"not found\" paths.",
        "functions = [",
    ]
    for address in sorted(kept):
        lines.append(f"    {{ address = 0x{address:08X}, size = 0x{kept[address]:X} }},")
    lines.append("]")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    name = sys.argv[1]
    folder = ROOT / "CoD3RecompLib" / "levels" / name
    kept_path = ROOT / "CoD3RecompLib" / "config" / f"{name}_functions.toml"
    functions = read_functions(folder)
    if not functions:
        print(f"{name}: no recompiled code in {folder}")
        return 1
    kept = read_kept(kept_path)
    changed = 0
    for lo, hi in extents(functions):
        size = hi - lo
        if kept.get(lo, 0) < size:
            kept[lo] = size
            changed += 1
    remaining = sum(len(f["errors"]) for s, f in functions.items() if not padding_piece(s, functions, sorted(functions)))
    if changed:
        write_kept(kept_path, name, kept)
    print(f"{name}: {changed} function extents added or grown, {len(kept)} kept in {kept_path.name}; "
          f"{remaining} branches out of their function in the code read")
    return 0


if __name__ == "__main__":
    sys.exit(main())
