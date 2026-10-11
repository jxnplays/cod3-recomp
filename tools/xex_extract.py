#!/usr/bin/env python3
"""Extract the decompressed PE image out of an Xbox 360 XEX2 container.

Handles the two container transforms that matter for retail Xbox 360 titles:

  * AES-128-CBC decryption (XEX2_RETAIL_KEY wrapped around the per-title key)
  * "BASIC" compression, a trivial dataSize/zeroSize block interleave

LZX ("NORMAL") compression is not implemented here; use XenonRecomp/XenonAnalyse
for containers that require it.

Usage:
    python xex_extract.py <default.xex> [output image path] [--sections]
"""

import argparse
import struct
import sys

try:
    from Crypto.Cipher import AES
except ImportError:  # pragma: no cover
    sys.exit("pycryptodome is required: pip install pycryptodome")

# XEX2 retail key (XenonUtils/xex.h -> Xex2RetailKey)
RETAIL_KEY = bytes([
    0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91,
])
BLANK_IV = b"\x00" * 16

# IMAGE_SCN_CNT_CODE
SCN_CNT_CODE = 0x00000020


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def be16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def read_cstring(buf, off):
    end = buf.index(b"\x00", off)
    return buf[off:end]


def parse_header(data):
    assert data[0:4] == b"XEX2", "not a XEX2 container"
    header_size = be32(data, 0x08)
    security_offset = be32(data, 0x10)
    header_count = be32(data, 0x14)

    opt = {}
    for i in range(header_count):
        key = be32(data, 0x18 + i * 8)
        opt[key] = be32(data, 0x18 + i * 8 + 4)

    sec = security_offset
    image_size = be32(data, sec + 0x04)
    load_address = be32(data, sec + 0x110)
    aes_key = data[sec + 0x150:sec + 0x160]

    enc_type, comp_type = 0, 0
    if 0x000003FF in opt:
        p = opt[0x000003FF]
        enc_type = be16(data, p + 4)
        comp_type = be16(data, p + 6)

    image_base = opt.get(0x00010201, load_address)
    entry_point = opt.get(0x00010100)

    return {
        "header_size": header_size,
        "image_size": image_size,
        "enc_type": enc_type,
        "comp_type": comp_type,
        "aes_key": aes_key,
        "image_base": image_base,
        "entry_point": entry_point,
    }


def decrypt_and_decompress(data, hdr):
    """Return the raw PE image bytes."""
    header_size = hdr["header_size"]
    payload = data[header_size:]

    if hdr["enc_type"] == 1:
        # The per-title AES key is itself encrypted under the retail key.
        key = hdr["aes_key"]
        c = AES.new(RETAIL_KEY, AES.MODE_CBC, BLANK_IV)
        title_key = c.decrypt(key)
        c = AES.new(title_key, AES.MODE_CBC, BLANK_IV)
        payload = c.decrypt(payload)

    comp = hdr["comp_type"]

    if comp == 0:
        return bytearray(payload[: hdr["image_size"]])

    if comp == 1:
        # BASIC: array of (dataSize, zeroSize) blocks describing the image.
        # Located immediately after the Xex2OptFileFormatInfo header.
        p = opt_offset = 0x000003FF
        ffi_off = None
        header_count = be32(data, 0x14)
        for i in range(header_count):
            if be32(data, 0x18 + i * 8) == 0x000003FF:
                ffi_off = be32(data, 0x18 + i * 8 + 4)
                break
        info_size = be32(data, ffi_off + 0)
        num_blocks = (info_size // 16) - 1

        block_off = ffi_off + 16
        out = bytearray()
        src = 0
        for _ in range(num_blocks):
            data_size = be32(data, block_off)
            zero_size = be32(data, block_off + 4)
            block_off += 8
            out += payload[src:src + data_size]
            src += data_size
            out += b"\x00" * zero_size
        return out

    if comp == 2:
        raise SystemExit(
            "XEX uses LZX (NORMAL) compression; this script only handles "
            "NONE/BASIC. Use XenonRecomp/XenonAnalyse instead."
        )

    raise SystemExit(f"unsupported compression type {comp}")


def parse_sections(image):
    """Parse the PE32 image and return (entry_rva, [section dicts])."""
    e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    sig = image[e_lfanew:e_lfanew + 4]
    assert sig == b"PE\0\0", f"bad PE signature {sig!r}"

    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", image, coff + 2)[0]
    size_opt = struct.unpack_from("<H", image, coff + 16)[0]
    opt = coff + 20

    magic = struct.unpack_from("<H", image, opt)[0]
    assert magic == 0x10B, f"expected PE32 (0x10B), got 0x{magic:X}"

    entry_rva = struct.unpack_from("<I", image, opt + 0x10)[0]
    size_of_image = struct.unpack_from("<I", image, opt + 0x38)[0]
    pe_image_base = struct.unpack_from("<I", image, opt + 0x1C)[0]

    sec_off = opt + size_opt
    sections = []
    for i in range(num_sections):
        s = sec_off + i * 40
        name = image[s:s + 8].rstrip(b"\x00").decode("ascii", "replace")
        vsize = struct.unpack_from("<I", image, s + 8)[0]
        vaddr = struct.unpack_from("<I", image, s + 12)[0]
        rawsize = struct.unpack_from("<I", image, s + 16)[0]
        rawptr = struct.unpack_from("<I", image, s + 20)[0]
        chars = struct.unpack_from("<I", image, s + 36)[0]
        sections.append({
            "name": name,
            "vaddr": vaddr,
            "vsize": vsize,
            "rawsize": rawsize,
            "rawptr": rawptr,
            "characteristics": chars,
            "is_code": bool(chars & SCN_CNT_CODE),
        })

    return entry_rva, size_of_image, pe_image_base, sections


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xex")
    ap.add_argument("out", nargs="?", default=None)
    ap.add_argument("--sections", action="store_true")
    args = ap.parse_args()

    with open(args.xex, "rb") as f:
        data = f.read()

    hdr = parse_header(data)
    print(f"XEX: {args.xex}")
    print(f"  encryption  : {hdr['enc_type']}  compression: {hdr['comp_type']}")
    print(f"  image size  : 0x{hdr['image_size']:X} ({hdr['image_size']:,} bytes)")
    print(f"  xex base    : 0x{hdr['image_base']:08X}")
    print(f"  xex entry   : 0x{hdr['entry_point']:08X}")

    image = decrypt_and_decompress(data, hdr)
    print(f"  extracted   : 0x{len(image):X} ({len(image):,} bytes)")

    entry_rva, size_of_image, pe_base, sections = parse_sections(image)
    print(f"  PE base     : 0x{pe_base:08X}  size of image: 0x{size_of_image:X}")
    print(f"  PE entry RVA: 0x{entry_rva:X} -> VA 0x{pe_base + entry_rva:08X}")

    if args.sections:
        print("\n  sections:")
        for s in sections:
            print(f"    {s['name']:<10} rva=0x{s['vaddr']:08X} vsize=0x{s['vsize']:08X} "
                  f"raw=0x{s['rawsize']:08X}@0x{s['rawptr']:08X} "
                  f"chars=0x{s['characteristics']:08X}{' CODE' if s['is_code'] else ''}")

    if args.out:
        with open(args.out, "wb") as f:
            f.write(image)
        print(f"\n  wrote image -> {args.out}")


if __name__ == "__main__":
    main()