"""Guest memory out of the port's save states, for finding where the game keeps a value.

A state (runtime/src/standalone_save_state.cpp) is "OTMS", version 1, a build id, then tagged sections: a u32 tag,
a u64 payload size, the payload. MEM1 (0x80000000, 24 MiB) and MEM2 (0x90000000, 128 MiB) are stored as the guest
sees them (big-endian).

  python -I tools/state_mem.py find   <state> <u8|u16|u32|f32|f64> <value> [--out list.txt]
  python -I tools/state_mem.py narrow <list.txt> <state> <type> <value|same|changed|inc|dec> [--prev <state>] [--out f]
  python -I tools/state_mem.py read   <state> <addr> [len]
  python -I tools/state_mem.py refs   <state> <addr> [--range 0x40]  words in memory that point at addr (or near it)

A list file is one hex address per line. `narrow` keeps the addresses whose value in <state> passes the test
(`same`/`changed`/`inc`/`dec` compare with --prev).
"""
import argparse
import mmap
import struct
import sys

MEM1, MEM1_SIZE = 0x80000000, 24 << 20
MEM2, MEM2_SIZE = 0x90000000, 128 << 20
FMT = {'u8': '>B', 'u16': '>H', 'u32': '>I', 'f32': '>f', 'f64': '>d', 's8': '>b', 's16': '>h', 's32': '>i'}


class State:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.map = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        m = self.map
        if m[0:4] != b'OTMS':
            raise SystemExit(f'{path}: not a state')
        at = 4 + 4 + 8  # magic, version, build id
        self.sections = {}
        while at + 12 <= len(m):
            tag = m[at:at + 4].decode('latin-1')
            size = struct.unpack_from('<Q', m, at + 4)[0]
            self.sections[tag] = (at + 12, size)
            at += 12 + size
        self.mem1 = self.sections.get('MEM1')
        self.mem2 = self.sections.get('MEM2')
        if not self.mem1 or not self.mem2:
            raise SystemExit(f'{path}: no MEM1/MEM2 sections ({sorted(self.sections)})')

    def offset(self, addr, size=1):
        for base, length, (off, _) in ((MEM1, MEM1_SIZE, self.mem1), (MEM2, MEM2_SIZE, self.mem2)):
            if base <= addr and addr + size <= base + length:
                return off + addr - base
        return None

    def read(self, addr, size):
        off = self.offset(addr, size)
        return None if off is None else self.map[off:off + size]

    def value(self, addr, kind):
        fmt = FMT[kind]
        data = self.read(addr, struct.calcsize(fmt))
        return None if data is None else struct.unpack(fmt, data)[0]

    def regions(self):
        yield MEM1, self.map[self.mem1[0]:self.mem1[0] + MEM1_SIZE]
        yield MEM2, self.map[self.mem2[0]:self.mem2[0] + MEM2_SIZE]


def parse_value(kind, text):
    if kind.startswith('f'):
        return float(text)
    return int(text, 0)


def matches(kind, got, want):
    if got is None:
        return False
    if kind.startswith('f'):
        return abs(got - want) <= max(1e-4, abs(want) * 1e-6)
    return got == want


def cmd_find(a):
    st = State(a.state)
    fmt = FMT[a.type]
    size = struct.calcsize(fmt)
    want = parse_value(a.type, a.value)
    if a.type.startswith('f'):
        needle = struct.pack(fmt, want)
    else:
        needle = struct.pack(fmt.replace('>', '>').upper() if want < 0 else fmt, want) if want >= 0 else \
            struct.pack(fmt.lower().replace('i', 'i').replace('h', 'h').replace('b', 'b'), want)
    hits = []
    align = 1 if size == 1 else (2 if size == 2 else 4)
    for base, data in st.regions():
        at = data.find(needle)
        while at != -1:
            if at % align == 0:
                hits.append(base + at)
            at = data.find(needle, at + 1)
    out = open(a.out, 'w') if a.out else None
    for h in hits:
        if out:
            out.write(f'{h:08X}\n')
    if out:
        out.close()
    print(f'{len(hits)} address(es)')
    for h in hits[:a.show]:
        print(f'  {h:08X}')


def cmd_narrow(a):
    st = State(a.state)
    prev = State(a.prev) if a.prev else None
    addrs = [int(l.strip(), 16) for l in open(a.list) if l.strip()]
    kept = []
    for addr in addrs:
        got = st.value(addr, a.type)
        test = a.value
        if test in ('same', 'changed', 'inc', 'dec'):
            if prev is None:
                raise SystemExit('--prev <state> needed')
            old = prev.value(addr, a.type)
            if got is None or old is None:
                continue
            ok = {'same': got == old, 'changed': got != old, 'inc': got > old, 'dec': got < old}[test]
        else:
            ok = matches(a.type, got, parse_value(a.type, test))
        if ok:
            kept.append(addr)
    with open(a.out or a.list, 'w') as f:
        for k in kept:
            f.write(f'{k:08X}\n')
    print(f'{len(addrs)} -> {len(kept)} address(es)')
    for k in kept[:a.show]:
        print(f'  {k:08X} = {st.value(k, a.type)}')


def cmd_read(a):
    st = State(a.state)
    addr, length = int(a.addr, 16), int(a.len, 0)
    data = st.read(addr, length)
    if data is None:
        raise SystemExit('address outside MEM1/MEM2')
    for row in range(0, length, 16):
        chunk = data[row:row + 16]
        words = ' '.join(chunk[i:i + 4].hex().upper() for i in range(0, len(chunk), 4))
        text = ''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)
        print(f'{addr + row:08X}  {words:<36} {text}')


def cmd_refs(a):
    st = State(a.state)
    target, rng = int(a.addr, 16), int(a.range, 0)
    for base, data in st.regions():
        words = memoryview(data).cast('B')
        for at in range(0, len(data) - 3, 4):
            w = (words[at] << 24) | (words[at + 1] << 16) | (words[at + 2] << 8) | words[at + 3]
            if target - rng <= w <= target:
                print(f'  {base + at:08X} -> {w:08X} (+0x{target - w:X})')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    f = sub.add_parser('find')
    f.add_argument('state'); f.add_argument('type', choices=FMT); f.add_argument('value')
    f.add_argument('--out'); f.add_argument('--show', type=int, default=40)
    n = sub.add_parser('narrow')
    n.add_argument('list'); n.add_argument('state'); n.add_argument('type', choices=FMT); n.add_argument('value')
    n.add_argument('--prev'); n.add_argument('--out'); n.add_argument('--show', type=int, default=40)
    r = sub.add_parser('read')
    r.add_argument('state'); r.add_argument('addr'); r.add_argument('len', nargs='?', default='0x40')
    rf = sub.add_parser('refs')
    rf.add_argument('state'); rf.add_argument('addr'); rf.add_argument('--range', default='0')
    a = p.parse_args()
    {'find': cmd_find, 'narrow': cmd_narrow, 'read': cmd_read, 'refs': cmd_refs}[a.cmd](a)


if __name__ == '__main__':
    main()
