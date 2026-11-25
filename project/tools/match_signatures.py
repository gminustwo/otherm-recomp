"""Names Other M's SDK and middleware functions by code signature.

The source is Dolphin's Sys/totaldb.dsy. The signature is Dolphin's masked instruction hash keyed by size
(ppcsig.py); for every function start in MAP.txt the hash is extended one instruction at a time up to the next
start and the longest length found in the database wins. The database also holds names from games' own code,
which match small functions by accident: keep only SDK and runtime library names before adding any to names_sig.txt.

A signature is only a name when it is unambiguous: one distinct name behind the (hash, size), and one Other M
address with it. Everything else (two names for one hash, or the same code at several addresses: tiny getters,
"li r3,0; blr", identical static helpers) goes to the candidates file, to be settled by hand from their
position among named neighbors.

Usage: match_signatures.py <main.dol> <otherm_linked.rel> <otherm_rel_layout.json> <MAP.txt> <totaldb.dsy>
                           --out-names <names.txt> --out-candidates <candidates.txt>
"""
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_function_map import Image  # noqa: E402
from ppcsig import hash_step  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from ppcbin import Dol  # noqa: E402

MIN_SIZE = 8


def load_dsy(path, db):
    data = open(path, "rb").read()
    (count,) = struct.unpack("<I", data[:4])
    for i in range(count):
        o = 4 + i * 136
        checksum, size = struct.unpack("<II", data[o:o + 8])
        full = data[o + 8:o + 136].split(b"\0")[0].decode("latin-1")
        name, _, origin = full.partition("\t")
        db[(checksum, size)].append((name.strip(), "dolphin", origin.strip()))


def main():
    args = sys.argv[1:]
    out_names = args[args.index("--out-names") + 1]
    out_candidates = args[args.index("--out-candidates") + 1]
    positional = [a for i, a in enumerate(args) if a not in ("--out-names", "--out-candidates")
                  and (i == 0 or args[i - 1] not in ("--out-names", "--out-candidates"))]
    dol_path, rel_path, layout_path, map_path, dsy_path = positional[:5]

    db = collections.defaultdict(list)
    load_dsy(dsy_path, db)
    sizes_by_hash = collections.defaultdict(set)
    for (h, size) in db:
        sizes_by_hash[h].add(size)
    max_size = max(size for (_h, size) in db)

    img = Image(Dol(dol_path), open(rel_path, "rb").read(), json.load(open(layout_path)))
    starts = sorted(int(line.split()[0], 16) for line in open(map_path) if line[0] not in "#\n")

    # A match must cover the whole function: its extent is up to the next map start, less the alignment padding
    # (zero words or nops) the linker put after it. A prefix that happens to equal some tiny database entry says
    # nothing about the function.
    matches = {}  # addr -> list of (name, source, origin)
    for i, start in enumerate(starts):
        limit = starts[i + 1] if i + 1 < len(starts) else start + max_size
        words = []
        a = start
        while a < limit and a - start <= max_size:
            try:
                w = img.u32(a)
            except KeyError:
                break
            if not img.in_text(a):
                break
            words.append(w)
            a += 4
        while len(words) > 1 and words[-1] in (0, 0x60000000):
            words.pop()
        extent = 4 * len(words)
        if extent < MIN_SIZE:
            continue
        h = 0
        for w in words:
            h = hash_step(h, w)
        if (h, extent) in db:
            matches[start] = db[(h, extent)]

    # Distinct names per address, and addresses per name.
    names_at = {}
    for addr, entries in matches.items():
        distinct = {}
        for name, source, origin in entries:
            distinct.setdefault(name, (source, origin))
        names_at[addr] = distinct
    addrs_of = collections.defaultdict(set)
    for addr, distinct in names_at.items():
        for name in distinct:
            addrs_of[name].add(addr)

    unique = {}
    candidates = {}
    for addr, distinct in names_at.items():
        if len(distinct) == 1:
            name = next(iter(distinct))
            if len(addrs_of[name]) == 1:
                unique[addr] = (name,) + distinct[name]
                continue
        candidates[addr] = distinct
    with open(out_names, "w", newline="\n") as out:
        out.write("# Other M (R3OE01 rev 1): unambiguous signature matches (match_signatures.py).\n")
        for addr in sorted(unique):
            name, source, origin = unique[addr]
            out.write("%08X %s  # signature: %s\n" % (addr, name, origin or source))
    with open(out_candidates, "w", newline="\n") as out:
        out.write("# Other M: ambiguous signature matches, one line per address: addr<TAB>name[,name...]\n")
        for addr in sorted(candidates):
            out.write("%08X\t%s\n" % (addr, ",".join(sorted(candidates[addr]))))
    by_source = collections.Counter(unique[a][1] for a in unique)
    print("signature names: %d unique (%s); %d ambiguous addresses -> candidates; %d function starts"
          % (len(unique), ", ".join("%s %d" % kv for kv in by_source.most_common()), len(candidates), len(starts)))


if __name__ == "__main__":
    main()
