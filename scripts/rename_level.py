"""Makes a recompiled level DLL linkable next to the title's own code and
the other levels'.

XenonRecomp names guest functions by address, so a level's sub_89xxxxxx
functions never clash with the title's sub_82xxxxxx ones; but every level
DLL is based at 0x89000000, so two levels' functions do clash with each
other, as do the register save/restore helpers, the entry point and the
function table, which are named the same in every output. This prefixes
them all with the level's name. Run after XenonRecomp:
python scripts/rename_level.py saint_lo
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: rename_level.py <level>", file=sys.stderr)
        return 1
    level = sys.argv[1]
    folder = ROOT / "CoD3RecompLib" / "levels" / level
    if not folder.is_dir():
        print(f"missing {folder}", file=sys.stderr)
        return 1

    helpers = re.compile(r"__(save|rest)(gprlr|fpr|vmx)_")
    # The level's own functions, by their address in the DLL's range, in
    # every spelling (sub_, __imp__sub_); the title's (sub_82...) are left
    # as they are, so the level can call them. Idempotent.
    own = re.compile(rf"(?<!{level}_)sub_(89[0-9A-F]{{6}})\b")
    changed = 0
    for path in list(folder.glob("*.cpp")) + list(folder.glob("*.h")):
        text = path.read_text(encoding="ascii")
        updated = helpers.sub(lambda m: f"__{level}_{m.group(1)}{m.group(2)}_", text)
        updated = own.sub(lambda m: f"{level}_sub_{m.group(1)}", updated)
        # The entry point is named _xstart in every output.
        updated = re.sub(rf"(?<!{level})_xstart", f"_{level}_xstart", updated)
        if path.name == "ppc_func_mapping.cpp":
            updated = updated.replace("PPCFuncMapping PPCFuncMappings[]",
                                      f"PPCFuncMapping PPCFuncMappings_{level}[]")
        if updated != text:
            path.write_text(updated, encoding="ascii", newline="\n")
            changed += 1
    print(f"{level}: {changed} files renamed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
