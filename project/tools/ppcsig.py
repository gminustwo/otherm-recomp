"""Function signatures the way Dolphin's HashSignatureDB computes them.

A signature is the running hash of a function's instructions with their address-dependent fields masked
(immediates and displacements, branch targets), keyed by the function's size: identical code at a different
address gives the same signature, so an SDK function is recognized wherever it was linked.
"""

def masked(opcode):
    """Dolphin HashSignatureDB::ComputeCodeChecksum's per-instruction value."""
    op = opcode & 0xFC000000
    op2 = 0
    op3 = 0
    auxop = op >> 26
    if auxop == 4:  # paired singles
        op2 = opcode & 0x0000003F
        if op2 in (0, 8, 16, 21, 22):
            op3 = opcode & 0x000007C0
    elif auxop in (7, 8, 10, 11, 12, 13, 14, 15):
        op2 = opcode & 0x03FF0000
    elif auxop in (19, 31, 63):
        op2 = opcode & 0x000007FF
    elif auxop == 59:
        op2 = opcode & 0x0000003F
        if op2 < 16:
            op3 = opcode & 0x000007C0
    elif 32 <= auxop < 56:
        op2 = opcode & 0x03FF0000
    return op | op2 | op3


def hash_step(h, opcode):
    return (((h << 17) & 0xFFFE0000) | ((h >> 15) & 0x0001FFFF)) ^ masked(opcode)


def hash_words(words):
    h = 0
    for w in words:
        h = hash_step(h, w)
    return h

