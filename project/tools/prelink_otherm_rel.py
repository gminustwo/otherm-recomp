"""Offline OSLinkFixed for Metroid: Other M (R3OE01 rev 1): files/rel/Release/game.rel -> one pre-linked image.

How the game loads it (main.dol, see project/NOTES.md):
  0x80006E58  opens /rel/Release/game.rel, rounds its length up to 0x20 and takes that many bytes from the bottom
              of the MEM1 arena: OSAllocFromMEM1ArenaLo(size, 0x20) (0x80109E90). Module pointer -> r13-0x7890.
  0x80006F80  version >= 3: BSS = image + fixSize rounded up to bssAlign. When that BSS ends below the arena's low
              end (it does: the relocation tables after fixSize are dropped), OSSetArenaLo(round32(bss end)) gives
              the rest back; then OSLinkFixed(module, bss) (0x8010FF40) and the module's prolog.
So the whole layout follows from the image address. The runtime pins that address
(runtime/src/hle/otherm_rel_pin.cpp), and this tool links the REL for it ahead of time so it can be translated like main.dol.

Usage: prelink_otherm_rel.py <game.rel> <out.rel> <out_layout.json> [image_base_hex]
The output is a synthetic REL (module id 0x7E, no imports, sections at absolute offsets from the image) that the
translator loads at the image address; its BSS bytes are zero, as OSLinkFixed leaves them.
"""
import json
import struct
import sys

DEFAULT_IMAGE = 0x8052B280  # where the game itself puts it: the runtime's "[relpin] game.rel natural image" line, 2026-11-05


def main():
    src, out_rel, out_json = sys.argv[1:4]
    image = int(sys.argv[4], 16) if len(sys.argv) > 4 else DEFAULT_IMAGE
    raw = open(src, "rb").read()
    (mid, _n, _p, nsec, secoff, _no, _ns, ver, bss_size, rel_off, imp_off, imp_size, prolog_sec, epilog_sec,
     unres_sec, _bss_sec, prolog, epilog, unres) = struct.unpack(">IIIIIIIIIIIIBBBBIII", raw[:0x40])
    align, bss_align, fix_size = struct.unpack(">III", raw[0x40:0x4C])
    assert ver == 3, ver
    assert image % align == 0, hex(image)

    secs = []
    for i in range(nsec):
        o, s = struct.unpack(">II", raw[secoff + 8 * i:secoff + 8 * i + 8])
        secs.append((o & ~1, bool(o & 1), s))
    bss_index = next(i for i, (o, e, s) in enumerate(secs) if o == 0 and s and i != 0)
    assert secs[bss_index][2] == bss_size

    bss = (image + fix_size + bss_align - 1) & ~(bss_align - 1)
    end = bss + bss_size
    arena_lo_after = (end + 0x1F) & ~0x1F
    alloc_size = (len(raw) + 0x1F) & ~0x1F

    def sec_addr(i):
        return bss if i == bss_index else image + secs[i][0]

    mem = bytearray(end - image)
    mem[0:fix_size] = raw[0:fix_size]

    def rd32(a):
        return struct.unpack(">I", mem[a - image:a - image + 4])[0]

    def wr32(a, v):
        mem[a - image:a - image + 4] = struct.pack(">I", v & 0xFFFFFFFF)

    def wr16(a, v):
        mem[a - image:a - image + 2] = struct.pack(">H", v & 0xFFFF)

    counts = {}
    for k in range(imp_size // 8):
        imp_mod, ro = struct.unpack(">II", raw[imp_off + 8 * k:imp_off + 8 * k + 8])
        assert imp_mod in (0, mid), imp_mod
        cur = 0
        c = ro
        while True:
            delta, typ, ssec, add = struct.unpack(">HBBI", raw[c:c + 8])
            c += 8
            if typ == 203:  # R_DOLPHIN_END
                break
            if typ == 202:  # R_DOLPHIN_SECTION
                cur = sec_addr(ssec)
                continue
            cur += delta
            if typ == 201:  # R_DOLPHIN_NOP
                continue
            dst = cur
            tgt = add if imp_mod == 0 else sec_addr(ssec) + add
            counts[typ] = counts.get(typ, 0) + 1
            if typ == 1:  # ADDR32
                wr32(dst, tgt)
            elif typ == 2:  # ADDR24
                wr32(dst, (rd32(dst) & ~0x03FFFFFC) | (tgt & 0x03FFFFFC))
            elif typ in (3, 4):  # ADDR16 / ADDR16_LO
                wr16(dst, tgt)
            elif typ == 5:  # ADDR16_HI
                wr16(dst, tgt >> 16)
            elif typ == 6:  # ADDR16_HA
                wr16(dst, (tgt >> 16) + (1 if tgt & 0x8000 else 0))
            elif typ in (7, 8, 9):  # ADDR14*
                wr32(dst, (rd32(dst) & ~0xFFFC) | (tgt & 0xFFFC))
            elif typ == 10:  # REL24
                wr32(dst, (rd32(dst) & ~0x03FFFFFC) | ((tgt - dst) & 0x03FFFFFC))
            elif typ in (11, 12, 13):  # REL14*
                wr32(dst, (rd32(dst) & ~0xFFFC) | ((tgt - dst) & 0xFFFC))
            else:
                sys.exit("unhandled relocation type %d" % typ)
    # A REL24 into main.dol must stay within +-32 MB of the image; OSLink does not check, neither would hardware.
    print("relocations applied:", {k: counts[k] for k in sorted(counts)})

    # Synthetic REL: same section numbering, offsets now relative to the image, BSS at its linked place.
    syn = [(0, False, 0)]
    for i, (o, e, s) in enumerate(secs):
        if i == 0:
            continue
        if not s:
            syn.append((0, False, 0))
        elif i == bss_index:
            syn.append((bss - image, False, s))
        else:
            syn.append((o, e, s))
    hdr_size = 0x4C + 8 * len(syn)
    first = min(o for o, e, s in syn if s)
    assert hdr_size <= first, (hex(hdr_size), hex(first))
    size = len(mem)
    hdr = struct.pack(">IIIIIIIIIIIIBBBBIII", 0x7E, 0, 0, len(syn), 0x4C, 0, 0, 3, 0, size, size, 0,
                      0, 0, 0, 0, 0, 0, 0) + struct.pack(">III", 0x20, 0x20, size)
    out = bytearray(mem)
    out[0:hdr_size] = hdr + b"".join(struct.pack(">II", a | (1 if e else 0), s) for a, e, s in syn)
    open(out_rel, "wb").write(out)

    layout = {
        "image": "0x%08X" % image,
        "alloc_size": "0x%X" % alloc_size,
        "fix_size": "0x%X" % fix_size,
        "bss": "0x%08X" % bss,
        "bss_size": "0x%X" % bss_size,
        "end": "0x%08X" % end,
        "arena_lo_after_link": "0x%08X" % arena_lo_after,
        "prolog": "0x%08X" % (sec_addr(prolog_sec) + prolog),
        "epilog": "0x%08X" % (sec_addr(epilog_sec) + epilog),
        "unresolved": "0x%08X" % (sec_addr(unres_sec) + unres),
        "sections": {str(i): {"addr": "0x%08X" % sec_addr(i), "size": "0x%X" % s, "exec": e}
                     for i, (o, e, s) in enumerate(secs) if s},
    }
    json.dump(layout, open(out_json, "w"), indent=2)
    print(json.dumps(layout, indent=1))


if __name__ == "__main__":
    main()
