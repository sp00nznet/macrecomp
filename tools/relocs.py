#!/usr/bin/env python3
"""Read a THINK C application's CREL and DREL relocation resources.

A 68k Mac application compiled by THINK C in its far model does not address
globals through A5 the way the small model does. Instead the compiler emits
**absolute 32-bit offsets into the application's data**, inline in the code and
in the data itself, and ships two resources saying where they are so the loader
can add the real base:

  CREL <n>   a flat list of ascending 16-bit offsets into CODE <n>. At each,
             the segment holds a 32-bit DATA offset.
  DREL       the same idea for DATA itself. Entries come in two forms - 32-bit
             values with a zero high word, then 16-bit values - and both are
             locations measured as `DATA_size + (v - 0x10000)`.

Why this matters for a recomp: until the fixups are applied, a string literal a
function passes to `printf` is an integer nobody can follow, so neither a
disassembly nor a lifter can say what the code is talking about. Applied, the
constants become addresses and the code becomes readable.

What is verified here and what is not, because the difference matters:

  * CREL's shape is **confirmed**. Reading a longword at each listed offset and
    checking it against the string starts in DATA lands on real ones -
    `CArray.c`, `CObject.c`, `CWindow.c`, an assertion message - which random
    offsets would not.
  * DREL's addressing is **confirmed only in range**: all entries map inside
    DATA under the formula above, and at least one slot demonstrably holds a
    string pointer stored as a magnitude below A5. What the other slots hold is
    not established.
  * Some segments have **no CREL at all**. In the application this was written
    against, five of twenty did. Those reference their data some other way, and
    this tool does not yet say how.

usage:
  relocs.py <dir of CODE_*.bin, CREL_*.bin, DATA.bin, DREL.bin>
  relocs.py <dir> --apply <out dir>     write fixed-up copies
  relocs.py --selftest
"""
import glob
import os
import re
import struct
import sys


def be32(b, o):
    return struct.unpack(">I", b[o:o + 4])[0]


def crel(rel):
    """CREL: ascending 16-bit offsets into the segment.

    It sometimes arrives as two ascending runs rather than one. That is not a
    different format - concatenating them gives one ordered list - so they are
    simply read in file order and sorted.
    """
    return sorted(struct.unpack(">%dH" % (len(rel) // 2), rel[:len(rel) // 2 * 2]))


def drel(rel, data_size):
    """DREL: locations in DATA, as 32-bit entries then 16-bit ones.

    Both encode the same thing - a distance below A5 - so both map to a DATA
    offset the same way. The 32-bit form exists because the value needs more
    than a signed 16-bit field.
    """
    out, i = [], 0
    while i + 4 <= len(rel) and rel[i] == 0 and rel[i + 1] == 0:
        out.append(be32(rel, i))
        i += 4
    for j in range(i, len(rel) - 1, 2):
        v = struct.unpack(">H", rel[j:j + 2])[0]
        if v:
            out.append(v)
    return [data_size + (v - 0x10000) for v in out]


def string_starts(data):
    """Where a printable, NUL-terminated string begins in DATA."""
    return set(m.start(1) for m in re.finditer(rb"(?:^|\x00)([ -~]{6,})\x00", data))


def segments(path):
    out = {}
    for cf in sorted(glob.glob(os.path.join(path, "CODE_*.bin"))):
        n = int(os.path.basename(cf)[5:-4])
        rf = os.path.join(path, "CREL_%d.bin" % n)
        out[n] = (open(cf, "rb").read(),
                  crel(open(rf, "rb").read()) if os.path.exists(rf) else None)
    return out


def report(path):
    data = open(os.path.join(path, "DATA.bin"), "rb").read()
    starts = string_starts(data)
    segs = segments(path)
    print("DATA %d bytes, %d string starts" % (len(data), len(starts)))

    total = hit = 0
    for n in sorted(segs):
        code, sites = segs[n]
        if sites is None:
            print("  CODE_%-2d %6d bytes   no CREL" % (n, len(code)))
            continue
        vals = [be32(code, o) for o in sites if o + 4 <= len(code)]
        inside = [v for v in vals if v < len(data)]
        on = [v for v in inside if v in starts]
        total += len(vals)
        hit += len(on)
        print("  CODE_%-2d %6d bytes  %4d fixups  %4d into DATA  %3d on a string"
              % (n, len(code), len(vals), len(inside), len(on)))
    print("\n%d fixups, %d landing exactly on a string start" % (total, hit))

    rf = os.path.join(path, "DREL.bin")
    if os.path.exists(rf):
        offs = drel(open(rf, "rb").read(), len(data))
        good = [o for o in offs if 0 <= o <= len(data) - 4]
        print("DREL: %d entries, %d inside DATA" % (len(offs), len(good)))


def apply_to(path, out):
    """Write each segment with its fixups turned into DATA offsets plus a base.

    The base is left at zero here: the point is to mark which longwords are
    addresses, which is what a disassembler or a lifter needs to know. A
    consumer that has placed DATA somewhere can add its own base.
    """
    data = open(os.path.join(path, "DATA.bin"), "rb").read()
    os.makedirs(out, exist_ok=True)
    for n, (code, sites) in sorted(segments(path).items()):
        if sites is None:
            continue
        marks = [o for o in sites if o + 4 <= len(code) and be32(code, o) < len(data)]
        with open(os.path.join(out, "CODE_%d.refs" % n), "w") as fh:
            for o in marks:
                v = be32(code, o)
                s = data[v:v + 60].split(b"\0")[0]
                fh.write("%04X\t%04X\t%s\n" % (o, v, s.decode("latin1")))
        print("  CODE_%-2d %4d references written" % (n, len(marks)))


def selftest():
    """Built here, so the readers are not only ever tried on one application."""
    # CREL: ascending 16-bit offsets, and the two-run case joins into one list.
    assert crel(struct.pack(">4H", 0x10, 0x20, 0x08, 0x0C)) == [0x08, 0x0C, 0x10, 0x20]

    # DREL: a 32-bit entry with a zero high word, then 16-bit ones, both
    # measured as size + (v - 0x10000).
    rel = struct.pack(">I", 0xD4D2) + struct.pack(">2H", 0xFD0E, 0x8846)
    got = drel(rel, 0x98B8)
    assert got == [0x98B8 - 0x2B2E, 0x98B8 - 0x2F2, 0x98B8 - 0x77BA], [hex(x) for x in got]
    assert all(0 <= x < 0x98B8 for x in got)

    # A zero 16-bit entry is padding, not a location at offset -0x10000.
    assert drel(struct.pack(">I", 0x8000) + struct.pack(">H", 0), 0x9000) == [0x9000 - 0x8000]

    data = b"\0hello there\0" + b"\0" * 8 + b"another one\0"
    st = string_starts(data)
    assert 1 in st and 21 in st, sorted(st)
    assert 0 not in st

    print("selftest: ok")


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if len(argv) < 2:
        sys.exit(__doc__)
    if "--apply" in argv:
        return apply_to(argv[1], argv[argv.index("--apply") + 1])
    report(argv[1])


if __name__ == "__main__":
    main(sys.argv)
