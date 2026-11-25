"""Small PowerPC helpers for Metroid: Other M's main.dol and game.rel: section loading and a minimal
decoder for the handful of instructions the project tools look for (no third-party packages).

    python -I tools/ppcbin.py <main.dol>        prints sections, entry point and the r2/r13 SDA bases
"""
import struct
import sys


class Dol:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        offs = struct.unpack(">18I", d[0x00:0x48])
        addrs = struct.unpack(">18I", d[0x48:0x90])
        sizes = struct.unpack(">18I", d[0x90:0xD8])
        self.bss_addr, self.bss_size, self.entry = struct.unpack(">III", d[0xD8:0xE4])
        # (kind, index, address, size, file offset); text sections are 0..6, data 7..17
        self.sections = []
        for i in range(18):
            if sizes[i]:
                kind = "text" if i < 7 else "data"
                self.sections.append((kind, i if i < 7 else i - 7, addrs[i], sizes[i], offs[i]))

    def section_at(self, addr):
        for s in self.sections:
            if s[2] <= addr < s[2] + s[3]:
                return s
        return None

    def read(self, addr, n):
        s = self.section_at(addr)
        if s is None:
            raise KeyError(hex(addr))
        o = s[4] + (addr - s[2])
        return self.data[o:o + n]

    def u32(self, addr):
        return struct.unpack(">I", self.read(addr, 4))[0]

    def text_ranges(self):
        return [(s[2], s[2] + s[3]) for s in self.sections if s[0] == "text"]


def simm(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(ins, pc):
    """Returns (mnemonic, fields) for the few forms the tools need, else (None, None)."""
    op = ins >> 26
    rd = (ins >> 21) & 31
    ra = (ins >> 16) & 31
    imm = ins & 0xFFFF
    if op == 18:  # b / bl
        li = ins & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        target = (li if ins & 2 else pc + li) & 0xFFFFFFFF
        return ("bl" if ins & 1 else "b"), {"target": target}
    if op == 15:
        return ("lis" if ra == 0 else "addis"), {"rd": rd, "ra": ra, "imm": simm(imm)}
    if op == 14:
        return ("li" if ra == 0 else "addi"), {"rd": rd, "ra": ra, "imm": simm(imm)}
    if op == 24:
        return "ori", {"ra": ra, "rs": rd, "imm": imm}
    if op == 19 and ((ins >> 1) & 0x3FF) == 16 and rd == 20:
        return "blr", {}
    return None, None


def find_sda_bases(dol):
    """__start calls __init_registers first; it loads r1, r2 and r13 with lis/ori (or lis/addi) pairs."""
    pc = dol.entry
    callees = []
    for _ in range(64):
        m, f = decode(dol.u32(pc), pc)
        if m == "bl":
            callees.append(f["target"])
        pc += 4
    for fn in callees:
        regs = {}
        found = {}
        pc = fn
        for _ in range(64):
            m, f = decode(dol.u32(pc), pc)
            if m == "lis":
                regs[f["rd"]] = (f["imm"] << 16) & 0xFFFFFFFF
            elif m == "ori" and f["rs"] in regs and f["ra"] == f["rs"]:
                regs[f["ra"]] = regs[f["rs"]] | f["imm"]
                found[f["ra"]] = regs[f["ra"]]
            elif m == "addi" and f["ra"] in regs and f["rd"] == f["ra"]:
                regs[f["rd"]] = (regs[f["ra"]] + f["imm"]) & 0xFFFFFFFF
                found[f["rd"]] = regs[f["rd"]]
            elif m == "blr":
                break
            pc += 4
        if 2 in found and 13 in found:
            return fn, found
    return None, {}


def main():
    dol = Dol(sys.argv[1])
    print("entry 0x%08X  bss 0x%08X+0x%X" % (dol.entry, dol.bss_addr, dol.bss_size))
    for kind, idx, addr, size, off in dol.sections:
        print("  %s%d 0x%08X..0x%08X size 0x%06X file 0x%06X" % (kind, idx, addr, addr + size, size, off))
    fn, regs = find_sda_bases(dol)
    if fn is None:
        print("SDA bases: __init_registers not found")
        return
    print("__init_registers 0x%08X" % fn)
    for r in sorted(regs):
        print("  r%d = 0x%08X" % (r, regs[r]))


if __name__ == "__main__":
    main()
