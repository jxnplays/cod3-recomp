#!/usr/bin/env python3
"""Parse an Xbox 360 XEX2 container header and report image/format metadata.

Pure-stdlib, big-endian aware. Mirrors the structures in XenonUtils/xex.h so
that the values reported here match what XenonRecomp/XenonAnalyse will do.

Usage:
    python xex_info.py <default.xex> [--imports] [--scan] [--json]
"""

import argparse
import json
import struct
import sys

# Keys from XenonUtils/xex.h (Xex2HeaderKeys)
K_RESOURCE_INFO = 0x000002FF
K_FILE_FORMAT_INFO = 0x000003FF
K_DELTA_PATCH_DESCRIPTOR = 0x000005FF
K_BASE_REFERENCE = 0x00000405
K_BOUNDING_PATH = 0x000080FF
K_DEVICE_ID = 0x00008105
K_ORIGINAL_BASE_ADDRESS = 0x00010001
K_ENTRY_POINT = 0x00010100
K_IMAGE_BASE_ADDRESS = 0x00010201
K_IMPORT_LIBRARIES = 0x000103FF
K_CHECKSUM_TIMESTAMP = 0x00018002
K_STATIC_LIBRARIES = 0x000200FF
K_TLS_INFO = 0x00020104
K_DEFAULT_STACK_SIZE = 0x00020200
K_DEFAULT_FILESYSTEM_CACHE_SIZE = 0x00020301
K_DEFAULT_HEAP_SIZE = 0x00020401
K_PAGE_HEAP_SIZE_AND_FLAGS = 0x00028002
K_SYSTEM_FLAGS = 0x00030000
K_EXECUTION_INFO = 0x00040006
K_TITLE_WORKSPACE_SIZE = 0x00040201
K_GAME_RATINGS = 0x00040310
K_LAN_KEY = 0x00040404
K_XBOX360_LOGO = 0x000405FF
K_MULTIDISC_MEDIA_IDS = 0x000406FF
K_ALTERNATE_TITLE_IDS = 0x000407FF
K_ADDITIONAL_TITLE_MEMORY = 0x00040801
K_EXPORTS_BY_NAME = 0x00E10402

KEY_NAMES = {
    K_RESOURCE_INFO: "RESOURCE_INFO",
    K_FILE_FORMAT_INFO: "FILE_FORMAT_INFO",
    K_DELTA_PATCH_DESCRIPTOR: "DELTA_PATCH_DESCRIPTOR",
    K_BASE_REFERENCE: "BASE_REFERENCE",
    K_BOUNDING_PATH: "BOUNDING_PATH",
    K_DEVICE_ID: "DEVICE_ID",
    K_ORIGINAL_BASE_ADDRESS: "ORIGINAL_BASE_ADDRESS",
    K_ENTRY_POINT: "ENTRY_POINT",
    K_IMAGE_BASE_ADDRESS: "IMAGE_BASE_ADDRESS",
    K_IMPORT_LIBRARIES: "IMPORT_LIBRARIES",
    K_CHECKSUM_TIMESTAMP: "CHECKSUM_TIMESTAMP",
    K_STATIC_LIBRARIES: "STATIC_LIBRARIES",
    K_TLS_INFO: "TLS_INFO",
    K_DEFAULT_STACK_SIZE: "DEFAULT_STACK_SIZE",
    K_DEFAULT_FILESYSTEM_CACHE_SIZE: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    K_DEFAULT_HEAP_SIZE: "DEFAULT_HEAP_SIZE",
    K_PAGE_HEAP_SIZE_AND_FLAGS: "PAGE_HEAP_SIZE_AND_FLAGS",
    K_SYSTEM_FLAGS: "SYSTEM_FLAGS",
    K_EXECUTION_INFO: "EXECUTION_INFO",
    K_TITLE_WORKSPACE_SIZE: "TITLE_WORKSPACE_SIZE",
    K_GAME_RATINGS: "GAME_RATINGS",
    K_LAN_KEY: "LAN_KEY",
    K_XBOX360_LOGO: "XBOX360_LOGO",
    K_MULTIDISC_MEDIA_IDS: "MULTIDISC_MEDIA_IDS",
    K_ALTERNATE_TITLE_IDS: "ALTERNATE_TITLE_IDS",
    K_ADDITIONAL_TITLE_MEMORY: "ADDITIONAL_TITLE_MEMORY",
    K_EXPORTS_BY_NAME: "EXPORTS_BY_NAME",
}

ENCRYPTION = {0: "NONE", 1: "NORMAL (AES-128-CBC)"}
COMPRESSION = {0: "NONE", 1: "BASIC", 2: "NORMAL (LZX)", 3: "DELTA"}


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def parse(data):
    info = {}
    magic = data[0:4]
    info["magic"] = magic.decode("ascii", "replace")
    if magic != b"XEX2":
        raise SystemExit(f"not a XEX2 container (magic={magic!r})")

    module_flags = be32(data, 0x04)
    header_size = be32(data, 0x08)
    reserved = be32(data, 0x0C)
    security_offset = be32(data, 0x10)
    header_count = be32(data, 0x14)

    info["module_flags"] = module_flags
    info["module_flags_str"] = []
    if module_flags & 0x10:
        info["module_flags_str"].append("MODULE_PATCH")
    if module_flags & 0x20:
        info["module_flags_str"].append("PATCH_FULL")
    if module_flags & 0x40:
        info["module_flags_str"].append("PATCH_DELTA")
    info["header_size"] = header_size
    info["security_offset"] = security_offset
    info["header_count"] = header_count

    # Optional headers: array of (key, value/offset) big-endian u32 pairs.
    opt = {}
    opt_off = 0x18
    for i in range(header_count):
        key = be32(data, opt_off + i * 8)
        val = be32(data, opt_off + i * 8 + 4)
        opt[key] = val
    info["opt_headers"] = opt

    # Security info lives at securityOffset.
    so = security_offset
    # Layout mirrors Xex2SecurityInfo in XenonUtils/xex.h.
    #   0x00 headerSize, 0x04 imageSize, 0x08 rsaSignature[0x100],
    #   0x108 unknown, 0x10C imageFlags, 0x110 loadAddress,
    #   0x114 sectionDigest[0x14], 0x128 importTableCount,
    #   0x12C importTableDigest[0x14], 0x140 xgd2MediaId[0x10],
    #   0x150 aesKey[0x16], 0x160 exportTable, 0x164 headerDigest[0x14],
    #   0x178 region, 0x17C allowedMediaTypes, 0x180 pageDescriptorCount
    sec = {
        "headerSize": be32(data, so + 0x00),
        "imageSize": be32(data, so + 0x04),
        "unknown": be32(data, so + 0x108),
        "imageFlags": be32(data, so + 0x10C),
        "loadAddress": be32(data, so + 0x110),
        "importTableCount": be32(data, so + 0x128),
        "xgd2MediaId": data[so + 0x140:so + 0x150].hex(),
        "aesKey": data[so + 0x150:so + 0x160].hex(),
        "exportTable": be32(data, so + 0x160),
        "region": be32(data, so + 0x178),
        "allowedMediaTypes": be32(data, so + 0x17C),
        "pageDescriptorCount": be32(data, so + 0x180),
    }
    info["security"] = sec

    # File format info: key&0xFF == 0xFF => `offset` is a pointer into the file.
    ffi = None
    if K_FILE_FORMAT_INFO in opt:
        p = opt[K_FILE_FORMAT_INFO]
        ffi = {
            "infoSize": be32(data, p + 0),
            "encryptionType": struct.unpack_from(">H", data, p + 4)[0],
            "compressionType": struct.unpack_from(">H", data, p + 6)[0],
        }
    info["file_format"] = ffi

    # Inline scalar headers: key&0xFF == 0 -> value; == 1 -> address of value.
    def scalar(key):
        if key not in opt:
            return None
        return opt[key]

    info["image_base"] = scalar(K_IMAGE_BASE_ADDRESS)
    if info["image_base"] is None:
        info["image_base"] = sec["loadAddress"]
    info["entry_point"] = scalar(K_ENTRY_POINT)
    info["original_base_address"] = scalar(K_ORIGINAL_BASE_ADDRESS)
    info["tls_info"] = scalar(K_TLS_INFO)
    info["default_stack_size"] = scalar(K_DEFAULT_STACK_SIZE)
    info["default_heap_size"] = scalar(K_DEFAULT_HEAP_SIZE)
    info["title_workspace_size"] = scalar(K_TITLE_WORKSPACE_SIZE)
    info["additional_title_memory"] = scalar(K_ADDITIONAL_TITLE_MEMORY)
    info["bounding_path"] = scalar(K_BOUNDING_PATH)

    return info, opt


def read_cstring(data, off):
    end = data.index(b"\x00", off)
    return data[off:end].decode("ascii", "replace")


def parse_imports(data, opt):
    """Walk the XEX import library table (raw, still compressed container)."""
    if K_IMPORT_LIBRARIES not in opt:
        return None
    base = opt[K_IMPORT_LIBRARIES]
    size_of_header = be32(data, base + 0)
    size_of_string_table = be32(data, base + 4)
    num_imports = be32(data, base + 8)

    str_table_off = base + 12
    strings = []
    cur = str_table_off
    for _ in range(num_imports):
        strings.append(read_cstring(data, cur))
        cur += ((len(strings[-1]) + 1) + 3) & ~3

    libs = []
    lib_off = base + 12 + size_of_string_table
    for i in range(num_imports):
        size = be32(data, lib_off + 0x00)
        lib_id = be32(data, lib_off + 0x18)
        lib_version = be32(data, lib_off + 0x1C)
        lib_minver = be32(data, lib_off + 0x20)
        name_off = struct.unpack_from(">H", data, lib_off + 0x24)[0]
        num = struct.unpack_from(">H", data, lib_off + 0x26)[0]
        name = strings[name_off] if name_off < len(strings) else f"<idx {name_off}>"
        desc_off = lib_off + 0x28
        thunks = []
        for j in range(num):
            first_thunk = be32(data, desc_off + j * 4)
            thunks.append(first_thunk)
        libs.append({
            "name": name,
            "id": lib_id,
            "version": lib_version,
            "min_version": lib_minver,
            "num_imports": num,
            "first_thunks": thunks,
        })
        lib_off = lib_off + 0x28 + num * 4
    return {"size_of_header": size_of_header, "num_libraries": num_imports, "libraries": libs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xex")
    ap.add_argument("--imports", action="store_true", help="dump import libraries")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    with open(args.xex, "rb") as f:
        data = f.read()

    info, opt = parse(data)

    imports = parse_imports(data, opt) if args.imports else None

    if args.json:
        print(json.dumps({"info": info, "imports": imports}, indent=2, default=str))
        return

    print(f"=== {args.xex} ===")
    print(f"magic            : {info['magic']}")
    print(f"module flags     : 0x{info['module_flags']:08X} {info['module_flags_str']}")
    print(f"header size      : 0x{info['header_size']:X}  (opt headers: {info['header_count']})")
    print(f"security offset  : 0x{info['security_offset']:X}")
    sec = info["security"]
    print(f"image size       : 0x{sec['imageSize']:X} ({sec['imageSize']:,} bytes)")
    print(f"load address     : 0x{sec['loadAddress']:08X}")
    print(f"page descriptors : {sec['pageDescriptorCount']}")
    print(f"media id         : {sec['xgd2MediaId']}")
    print(f"region           : 0x{sec['region']:08X}")
    print(f"media types      : 0x{sec['allowedMediaTypes']:08X}")
    ffi = info["file_format"]
    if ffi:
        print(f"encryption       : {ENCRYPTION.get(ffi['encryptionType'], ffi['encryptionType'])}")
        print(f"compression      : {COMPRESSION.get(ffi['compressionType'], ffi['compressionType'])}")
    else:
        print("encryption       : (no FILE_FORMAT_INFO -> raw/None)")
        print("compression      : (no FILE_FORMAT_INFO -> raw/None)")
    print(f"image base       : 0x{(info['image_base'] or 0):08X}")
    print(f"entry point      : 0x{(info['entry_point'] or 0):08X}")
    print(f"TLS info         : 0x{(info['tls_info'] or 0):08X}")
    print(f"stack size       : 0x{(info['default_stack_size'] or 0):08X}")
    print(f"heap size        : 0x{(info['default_heap_size'] or 0):08X}")
    print(f"title workspace  : 0x{(info['title_workspace_size'] or 0):08X}")
    print(f"addl title mem   : 0x{(info['additional_title_memory'] or 0):08X}")

    print("\n--- optional headers present ---")
    for key, val in sorted(opt.items()):
        name = KEY_NAMES.get(key, f"UNKNOWN_0x{key:08X}")
        kind = "inline" if (key & 0xFF) in (0, 1) else "offset"
        print(f"  0x{key:08X} {name:<32} {kind:<7} 0x{val:08X}")

    if imports:
        print(f"\n--- imports ({imports['num_libraries']} libraries) ---")
        total = 0
        for lib in imports["libraries"]:
            total += lib["num_imports"]
            print(f"  {lib['name']:<24} v{lib['version']:<6} funcs={lib['num_imports']}")
        print(f"  TOTAL imported functions: {total}")


if __name__ == "__main__":
    main()