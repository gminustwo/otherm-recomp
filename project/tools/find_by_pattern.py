"""Names a few SDK functions by patterns that are evidence on their own:
  - one-instruction special-register helpers: PPCMfhid0 is "mfspr r3,HID0; blr", PPCMtdec "mtdec r3; blr" ...
    (named by the SPR number: the signature of such a function is the same for every SPR, so signatures cannot
    tell them apart);
  - each SDK library's init function calls OSRegisterVersion(__XXVersion) with its "<< RVL_SDK - XX ... >>"
    string, so the function that builds that string's address is the library's init (VIInit, WPADInit ...);
    OSRegisterVersion itself is the function those calls branch to.

Usage: find_by_pattern.py <main.dol> <MAP.txt> <out names_pattern.txt>
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from ppcbin import Dol, decode, find_sda_bases  # noqa: E402

SPR_NAMES = {22: "dec", 920: "hid2", 921: "wpar", 922: "dmau", 923: "dmal", 952: "mmcr0", 953: "pmc1",
             954: "pmc2", 956: "mmcr1", 957: "pmc3", 958: "pmc4", 1008: "hid0", 1011: "hid4", 1017: "l2cr",
             1019: "ictc"}
# Library name in the version string -> its init function (the one that registers the version).
INIT_BY_LIBRARY = {"AI": "AIInit", "AX": "AXInit", "DSP": "DSPInit", "DVD": "DVDInit", "EXI": "EXIInit",
                   "GX": "GXInit", "KPAD": "KPADInit", "NAND": "NANDInit", "OS": "OSInit", "SC": "SCInit",
                   "SI": "SIInit", "VI": "VIInit", "WPAD": "WPADInit", "HBM": "HBMCreate"}


def main():
    dol_path, map_path, out_path = sys.argv[1:4]
    dol = Dol(dol_path)
    starts = sorted(int(line.split()[0], 16) for line in open(map_path) if line[0] not in "#\n")
    names = {}

    def function_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else None

    # Special-register helpers.
    for lo, hi in dol.text_ranges():
        for a in range(lo, hi - 4, 4):
            w = dol.u32(a)
            if a not in starts or dol.u32(a + 4) != 0x4E800020:
                continue
            if (w >> 26) != 31 or ((w >> 1) & 0x3FF) not in (339, 467):  # mfspr / mtspr
                continue
            spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
            if spr in SPR_NAMES and ((w >> 21) & 31) == 3:
                kind = "Mf" if ((w >> 1) & 0x3FF) == 339 else "Mt"
                names[a] = ("PPC%s%s" % (kind, SPR_NAMES[spr]), "mfspr/mtspr r3,%d; blr" % spr)

    # Version strings -> init functions, and OSRegisterVersion. The SDK keeps each string behind a pointer
    # variable (const char* __VIVersion), so the code loads the pointer: lwz r3,off(r13) (small data) or
    # lis/addi of the variable, then bl OSRegisterVersion.
    data = dol.data
    version_at = {}
    for m in re.finditer(rb"<< RVL_SDK - ([A-Z]+) \t[^>]*>>", data):
        for kind, idx, addr, size, off in dol.sections:
            if off <= m.start() < off + size:
                version_at[addr + (m.start() - off)] = m.group(1).decode()
    pointer_at = {}  # address of a data word holding a version string's address -> library
    for kind, idx, addr, size, off in dol.sections:
        if kind != "data":
            continue
        for o in range(0, size - 3, 4):
            w = int.from_bytes(data[off + o:off + o + 4], "big")
            if w in version_at:
                pointer_at[addr + o] = version_at[w]
    _fn, bases = find_sda_bases(dol)
    r13, r2 = bases.get(13, 0), bases.get(2, 0)

    def note_reference(a, var_addr):
        lib = pointer_at[var_addr]
        fn = function_of(a)
        if lib in INIT_BY_LIBRARY and fn is not None:
            names.setdefault(fn, (INIT_BY_LIBRARY[lib], "loads the RVL_SDK %s version string pointer (%08X) at %08X"
                                  % (lib, var_addr, a)))
        for k in range(1, 6):  # the bl that follows is OSRegisterVersion
            m2, f2 = decode(dol.u32(a + 4 * k), a + 4 * k)
            if m2 == "bl":
                register_version_callers[f2["target"]] = register_version_callers.get(f2["target"], 0) + 1
                break

    register_version_callers = {}
    for lo, hi in dol.text_ranges():
        regs = {}
        for a in range(lo, hi, 4):
            w = dol.u32(a)
            m, f = decode(w, a)
            if m == "lis":
                regs[f["rd"]] = (a, (f["imm"] << 16) & 0xFFFFFFFF)
            elif m == "addi" and f["ra"] in regs and a - regs[f["ra"]][0] <= 64:
                target = (regs[f["ra"]][1] + f["imm"]) & 0xFFFFFFFF
                if target in pointer_at:
                    note_reference(a, target)
            elif (w >> 26) == 32:  # lwz rD, d(rA)
                ra = (w >> 16) & 31
                d = w & 0xFFFF
                d = d - 0x10000 if d & 0x8000 else d
                base = r13 if ra == 13 else r2 if ra == 2 else None
                if base is not None and (base + d) & 0xFFFFFFFF in pointer_at:
                    note_reference(a, (base + d) & 0xFFFFFFFF)
            elif m == "blr":
                regs = {}
    if register_version_callers:
        target = max(register_version_callers, key=register_version_callers.get)
        names.setdefault(target, ("OSRegisterVersion", "called with %d of the RVL_SDK version strings"
                                  % register_version_callers[target]))

    # DVDLow (the 2009 SDK talks to /dev/di through IOS): each DVDLow* function stores its DI command byte into
    # the command buffer with "li rA,<cmd>; stbx rA,rB,rC" and nothing else in the game does that with these
    # bytes. The command numbers are the DI ioctl set (Dolphin's DI emulation lists them).
    di_commands = {0x12: "DVDLowInquiry", 0x70: "DVDLowReadDiskID", 0x71: "DVDLowRead", 0x7A: "DVDLowPrepareCoverRegister",
                   0x86: "DVDLowClearCoverInterrupt", 0x88: "DVDLowGetCoverStatus", 0x8A: "DVDLowReset",
                   0x8B: "DVDLowOpenPartition", 0x8C: "DVDLowClosePartition", 0x8D: "DVDLowUnencryptedRead",
                   0xAB: "DVDLowSeek", 0xE0: "DVDLowRequestError", 0xE3: "DVDLowStopMotor",
                   0xE4: "DVDLowAudioBufferConfig"}
    for lo, hi in dol.text_ranges():
        for a in range(lo, hi - 4, 4):
            w = dol.u32(a)
            m, f = decode(w, a)
            w2 = dol.u32(a + 4)
            if m == "li" and f["imm"] in di_commands and (w2 >> 26) == 31 and ((w2 >> 1) & 0x3FF) == 215 \
                    and ((w2 >> 21) & 31) == f["rd"]:  # stbx rS,rA,rB with rS the li target
                fn = function_of(a)
                if fn is not None:
                    names.setdefault(fn, (di_commands[f["imm"]], "stores DI command 0x%02X at %08X" % (f["imm"], a)))

    with open(out_path, "w", newline="\n") as out:
        out.write("# Other M (R3OE01 rev 1): names from instruction and string patterns (find_by_pattern.py).\n")
        for addr in sorted(names):
            name, evidence = names[addr]
            out.write("%08X %s  # %s\n" % (addr, name, evidence))
    print("pattern names: %d" % len(names))
    for addr in sorted(names):
        print("  %08X %s  (%s)" % (addr, names[addr][0], names[addr][1]))


if __name__ == "__main__":
    main()
