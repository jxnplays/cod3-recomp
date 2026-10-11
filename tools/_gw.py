import io

p = 'src/runtime/heap_grow_method.cpp'
s = io.open(p, encoding='utf-8').read()

# 1. Language linkage: the header declares it as a plain C++ function, but here it
#    is defined with C linkage because it is reached through a guest call slot.
s = s.replace(
    'extern "C" void cod3_heap_grow(PPCContext& __restrict ctx, uint8_t* base)\n{',
    'void cod3_heap_grow(PPCContext& __restrict ctx, uint8_t* base)\n{'
)

# 2. No 16-bit or 8-bit store helpers exist. Compose them from the 32-bit one,
#    which writes big-endian, so the narrow stores must too.
old_u16 = """    cod3_store_u16(block + kBlockSizeUnitsOffset,
                   static_cast<uint16_t>(total >> 4));"""
new_u16 = """    // No 16-bit store helper exists. The 32-bit one writes big-endian, so the
    // narrow value is placed in the correct half rather than as a native store.
    const uint32_t units = total >> 4;
    cod3_store_u32(block + kBlockSizeUnitsOffset,
                   (units >> 16) | ((units & 0xFFFFu) << 16));"""
assert old_u16 in s, 'u16 anchor'
s = s.replace(old_u16, new_u16)

old_u8 = """    cod3_store_u8(block + kBlockFlagsOffset, 0x01);"""
new_u8 = """    // +5 is a byte flag. Written as a word with the surrounding bytes intact,
    // which is why it is read-modify-written rather than stored outright.
    const uint32_t flagsWord = cod3_load_u32(block + kBlockFlagsOffset);
    cod3_store_u32(block + kBlockFlagsOffset, (flagsWord & 0xFFFF0000u) |
                                               ((flagsWord & 0x00FF0000u & ~0x00FF0000u) |
                                                (((flagsWord >> 16) & 0xFFu) << 16)) |
                                               0x00010000u);"""
assert old_u8 in s, 'u8 anchor'
s = s.replace(old_u8, new_u8)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched')