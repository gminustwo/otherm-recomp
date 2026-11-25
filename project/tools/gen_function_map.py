"""Function map for Metroid: Other M (R3OE01 rev 1), built from the binaries alone (no decompilation exists).

Writes one "hexaddr name" line per function start in main.dol's text and the pre-linked game.rel text
(prelink_otherm_rel.py output). Evidence, strongest first:
  1. the DOL entry point, the REL's prolog/epilog/unresolved, and every bl target (direct calls);
  2. words in data sections that point into text (vtables, callback tables, the boot step table) and land on
     a plausible start (after a blr / unconditional branch / padding, or on a prologue);
  3. lis+addi/ori pairs in code that build a text address (function pointers passed as arguments);
  4. gap fill: after a blr or unconditional b, skipping zero padding, a following prologue
     (stwu r1,-x(r1) or mflr r0) that nothing above reached.
CodeWarrior's save/restore thunks (_save_gpr_14.._31, _rest_gpr_*, _save_fpr_14.._31, _rest_fpr_*) are found by
their instruction pattern and named, since the translator recognizes them by those names (GuestSaveRestoreThunks).
Names files ("hexaddr name  # comment"; later files win) replace the fn_ placeholders: names_align.txt (SDK link
order), names_sig.txt (code signatures), names_pattern.txt (find_by_pattern.py) and names.txt (checked by hand).

Usage: gen_function_map.py <main.dol> <otherm_linked.rel> <otherm_rel_layout.json> <out MAP.txt> [names files...]
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from ppcbin import Dol, decode  # noqa: E402


class Image:
    """main.dol plus the pre-linked REL as one address space: text ranges, data ranges, word reads."""

    def __init__(self, dol, rel_bytes, layout):
        self.dol = dol
        self.rel = rel_bytes
        self.rel_base = int(layout["image"], 16)
        self.text = list(dol.text_ranges())
        self.data = [(s[2], s[2] + s[3]) for s in dol.sections if s[0] == "data"]
        for sec in layout["sections"].values():
            lo = int(sec["addr"], 16)
            hi = lo + int(sec["size"], 16)
            (self.text if sec["exec"] else self.data).append((lo, hi))
        self.bss = (int(layout["bss"], 16), int(layout["end"], 16))
        self.data = [r for r in self.data if r[0] != self.bss[0]]

    def u32(self, addr):
        if self.rel_base <= addr < self.rel_base + len(self.rel):
            o = addr - self.rel_base
            return struct.unpack(">I", self.rel[o:o + 4])[0]
        return self.dol.u32(addr)

    def in_text(self, addr):
        return addr & 3 == 0 and any(lo <= addr < hi for lo, hi in self.text)

    def text_words(self):
        for lo, hi in self.text:
            for a in range(lo, hi, 4):
                yield a, self.u32(a)

    def data_words(self):
        for lo, hi in self.data:
            for a in range(lo & ~3, hi - 3, 4):
                yield a, self.u32(a)


def is_prologue(w):
    return (w & 0xFFFF8000) == 0x94218000 or w == 0x7C0802A6  # stwu r1,-x(r1) / mflr r0


def is_terminator(w):
    if w == 0x4E800020 or w == 0x4E800420:  # blr / bctr
        return True
    return (w >> 26) == 18 and (w & 1) == 0  # b (not bl)


def find_thunks(img):
    """CodeWarrior _savegpr/_restgpr/_savefpr/_restfpr runs: k-th entry handles register 14+k, all through r11."""
    found = {}
    kinds = (("_save_gpr_", 36), ("_rest_gpr_", 32), ("_save_fpr_", 54), ("_rest_fpr_", 50))  # stw/lwz/stfd/lfd
    for lo, hi in img.text:
        a = lo
        while a < hi - 18 * 4:
            w = img.u32(a)
            op, rs, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            for prefix, want in kinds:
                if op == want and ra == 11 and rs == 14:
                    ok = all((img.u32(a + 4 * k) >> 26) == want and ((img.u32(a + 4 * k) >> 21) & 31) == 14 + k
                             and ((img.u32(a + 4 * k) >> 16) & 31) == 11 for k in range(18))
                    if ok:
                        for k in range(18):
                            found[a + 4 * k] = "%s%d" % (prefix, 14 + k)
                        a += 4 * 17
                    break
            a += 4
    return found


def main():
    dol_path, rel_path, layout_path, out_path = sys.argv[1:5]
    names_paths = sys.argv[5:]
    dol = Dol(dol_path)
    layout = json.load(open(layout_path))
    img = Image(dol, open(rel_path, "rb").read(), layout)

    starts = {}  # addr -> evidence tag

    def add(addr, tag):
        if img.in_text(addr) and addr not in starts:
            starts[addr] = tag

    add(dol.entry, "entry")
    for key in ("prolog", "epilog", "unresolved"):
        add(int(layout[key], 16), "rel_" + key)
    for lo, _hi in img.text:  # a text section starts with a function (0x80006A80 is only reached as a callback)
        add(lo, "section")

    # 1. direct calls
    prev = {}
    for a, w in img.text_words():
        m, f = decode(w, a)
        if m == "bl":
            add(f["target"], "bl")

    def plausible(addr):
        if addr in starts:
            return True
        w = img.u32(addr)
        if is_prologue(w):
            return True
        try:
            before = img.u32(addr - 4)
        except KeyError:
            return True
        return is_terminator(before) or before == 0

    # Switch jump tables: lis/addi of a data address, then lwzx + mtctr + bctr within a few instructions. Their
    # entries are case labels inside one function (each after the previous case's b), not function starts.
    case_labels = set()
    for lo, hi in img.text:
        regs = {}
        for a in range(lo, hi, 4):
            m, f = decode(img.u32(a), a)
            if m == "lis":
                regs[f["rd"]] = (a, (f["imm"] << 16) & 0xFFFFFFFF)
            elif m == "addi" and f["ra"] in regs and a - regs[f["ra"]][0] <= 32:
                table = (regs[f["ra"]][1] + f["imm"]) & 0xFFFFFFFF
                if not any(dlo <= table < dhi for dlo, dhi in img.data):
                    continue
                window = [img.u32(a + 4 * k) for k in range(1, 12)]
                lwzx = any((w >> 26) == 31 and ((w >> 1) & 0x3FF) == 23 for w in window)
                bctr = any(w == 0x4E800420 for w in window)
                if not (lwzx and bctr):
                    continue
                t = table
                while any(dlo <= t < dhi for dlo, dhi in img.data):
                    target = img.u32(t)
                    if not img.in_text(target) or abs(target - a) > 0x10000:
                        break
                    # A case label cannot sit before its own function: a target below the table load with a blr in
                    # between belongs to another function, so this is a table of functions (game.rel 0x807B7C94
                    # tail-calls through 0x80BD6200 into the GX helpers at 0x807B7BCC.., boot 42), not a switch.
                    if target < a and any(img.u32(x) == 0x4E800020 for x in range(target, a, 4)):
                        t += 4
                        continue
                    case_labels.add(target)
                    t += 4

    # 2. data words into text
    for a, w in img.data_words():
        if img.in_text(w) and w not in case_labels and plausible(w):
            add(w, "data")

    # 3. lis+addi/ori pairs in code. Compilers keep a lis base in a saved register for a whole function
    # (main.dol 0x80006D40 lis r30 ... 0x80006DC4 addi r3,r30,0x6A80), so the pair may be far apart; plausible()
    # keeps the result honest.
    for lo, hi in img.text:
        regs = {}
        for a in range(lo, hi, 4):
            w = img.u32(a)
            m, f = decode(w, a)
            if m == "lis":
                regs[f["rd"]] = (a, (f["imm"] << 16) & 0xFFFFFFFF)
                continue
            if m == "addi" and f["ra"] in regs and a - regs[f["ra"]][0] <= 1024:
                v = (regs[f["ra"]][1] + f["imm"]) & 0xFFFFFFFF
                if img.in_text(v) and v not in case_labels and plausible(v):
                    add(v, "lisaddi")
            elif m == "ori" and f["rs"] in regs and a - regs[f["rs"]][0] <= 1024:
                v = regs[f["rs"]][1] | f["imm"]
                if img.in_text(v) and v not in case_labels and plausible(v):
                    add(v, "lisaddi")
            if is_terminator(w):
                regs = {}

    # 4. gap fill after terminators
    for lo, hi in img.text:
        a = lo
        while a < hi:
            w = img.u32(a)
            if is_terminator(w):
                b = a + 4
                while b < hi and img.u32(b) == 0:
                    b += 4
                if b < hi and b not in starts and is_prologue(img.u32(b)):
                    add(b, "gap")
            a += 4

    thunks = find_thunks(img)
    names = {}
    for names_path in names_paths:
        if not os.path.exists(names_path):
            continue
        for line in open(names_path, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line:
                addr, name = line.split(None, 1)
                names[int(addr, 16)] = name.strip()

    allstarts = set(starts) | set(thunks) | {a for a in names if img.in_text(a)}
    rel_lo = int(layout["sections"]["1"]["addr"], 16)
    with open(out_path, "w", newline="\n") as out:
        out.write("# Metroid: Other M (R3OE01 rev 1) function starts: main.dol + game.rel pre-linked at %s.\n"
                  % layout["image"])
        out.write("# Generated by project/tools/gen_function_map.py; names from names_sig.txt / names.txt.\n")
        for a in sorted(allstarts):
            name = names.get(a) or thunks.get(a) or ("rel_%08X" % a if a >= rel_lo else "fn_%08X" % a)
            out.write("%08X %s\n" % (a, name))

    tags = {}
    for t in starts.values():
        tags[t] = tags.get(t, 0) + 1
    dol_count = sum(1 for a in allstarts if a < rel_lo)
    print("function starts: %d (main.dol %d, game.rel %d); evidence %s; thunks %d; named %d; "
          "switch case labels skipped %d"
          % (len(allstarts), dol_count, len(allstarts) - dol_count, tags, len(thunks), len(names), len(case_labels)))


if __name__ == "__main__":
    main()
