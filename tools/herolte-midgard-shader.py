"""Hand-assemble the one-bundle Midgard fragment shader for the T880 test.

Bundle (TAG_ALU_12 + 4 = writeout variant, as Mesa's scheduler emits it on
GPUs without MIDGARD_NO_UPPER_ALU, e.g. 0x880):
    vmul: imov r0, r26 (embedded constants, all four 32-bit lanes)
    vadd: iadd r1.w, r31.x, #0          (Mesa: "r1.w = return address")
    br  : extended writeout, cond always, back to this bundle until it lands
Layout: control word, 2 register words, 2 x 48-bit vector ALU, 48-bit
extended branch, zero padding to 32 bytes, 16 bytes of constants.
Field layouts are Mesa's midgard.h bitfields, packed LSB first.
"""
import struct

TAG = 0xA + 4          # TAG_ALU_12_WRITEOUT
NEXT = 1               # lookahead of the last bundle: TAG_BREAK
VMUL, VADD, BRANCH = 1 << 17, 1 << 21, 1 << 27
OP_IMOV, OP_IADD = 0x7B, 0x40
REG_MODE_32, SHRINK_NONE, OUTMOD_KEEPLO = 2, 2, 2
REGISTER_UNUSED, REGISTER_CONSTANT = 24, 26


def reg_word(src1, src2, out, imm):
    return src1 | src2 << 5 | out << 10 | imm << 15


def vector_src(swizzle, mod=0, expand=0):
    return mod | expand << 2 | swizzle << 5


def vector_alu(op, src1, src2, mask):
    v = op | REG_MODE_32 << 8 | src1 << 10 | src2 << 23
    v |= SHRINK_NONE << 36 | OUTMOD_KEEPLO << 38 | mask << 40
    return v.to_bytes(6, 'little')


def expand_mask(m):     # 32-bit lanes: 2 mask bits per component
    return sum(3 << (2 * i) for i in range(4) if m >> i & 1)


def branch_writeout(dest_tag, offset):
    cond = 0xFFFF       # midgard_condition_always duplicated over the LUT
    v = 7 | dest_tag << 3 | 0 << 7 | (offset & 0x7FFFFF) << 9 | cond << 32
    return v.to_bytes(6, 'little')


def assemble(color_words):
    body = struct.pack('<I', VMUL | VADD | BRANCH | TAG | NEXT << 4)
    body += struct.pack('<H', reg_word(REGISTER_UNUSED, REGISTER_CONSTANT, 0, 0))
    body += struct.pack('<H', reg_word(31, 0, 1, 1))
    swz_xyzw = 0 | 1 << 2 | 2 << 4 | 3 << 6
    body += vector_alu(OP_IMOV, 0, vector_src(swz_xyzw), expand_mask(0xF))
    body += vector_alu(OP_IADD, vector_src(0), 0, expand_mask(0x8))
    body += branch_writeout(TAG, -3)         # this bundle is 3 quadwords
    body += b'\0' * (32 - len(body))
    body += struct.pack('<4I', *color_words)
    assert len(body) == 48
    return body


if __name__ == '__main__':
    # 0x3c003c00: fp16 pairs (1.0, 1.0); as fp32 it is ~0.0078.
    code = assemble([0x3C003C00] * 4)
    words = struct.unpack('<12I', code)
    print(', '.join(f'0x{w:08x}' for w in words))
