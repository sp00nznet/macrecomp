#!/usr/bin/env python3
"""Find computed-jump targets the lifter cannot reach.

A `jmp d(pc,Dn.w)` -- 0x4EFB -- is a switch. Nothing in the binary *names* its
arms, so a linear decode only lands on one by luck, and a target it missed
becomes an `m68k_entry_miss` at run time: the function returns without running,
silently. One such address in CODE 12 was the whole reason HyperTalk did not
execute.

Two table shapes, both here:

  words   the usual switch -- a table of 16-bit offsets right after the jmp,
          each arm at (jmp base) + offset.
  bytes   a Duff's device -- `move.b tbl(pc,Dn.w),Dm` picks a byte out of a
          table, `jmp tbl(pc,Dm.w)` lands that many bytes into an unrolled copy
          chain. The table sits *at* the jmp's own base rather than after it,
          and its entries are one byte each. Reading it as words finds nothing,
          which is how CODE 18's literal-run copier stayed unreachable: its
          eight landing points had to be worked out by hand, and until they
          were, HyperCard's WOBA row decoder ran off into the table itself.

This reads each table, checks every target against the generated code, and
prints the ones with neither a `case` label nor a registered function start --
i.e. exactly the addresses to hand back as `lift68k.py --entry`.

    python tools/find_entries.py work/hypercard-ewec/code work/gen

A target is dropped once an entry points backwards, outside the segment, or at
an odd address: the table has ended and what follows is the arms themselves.
"""
import os, re, struct, sys

def word_table(d, base, limit=64):
    """The plain switch: 16-bit offsets starting two bytes past the jmp base.

    `base` is the extension word's own address. Strictly the 68000 adds the
    extension word's displacement to that, but every word table met so far
    carries a displacement of 0 or is measured from here regardless, and the
    arms this finds are the ones already known good. Left alone deliberately.
    """
    out = []
    for n in range(limit):
        o = base + 2 + n * 2
        if o + 2 > len(d): break
        t = base + struct.unpack('>H', d[o:o+2])[0]
        if t <= o or t >= len(d) or (t & 1): break
        out.append(t)
    return out


def byte_table(d, base, jmp, limit=256):
    """The Duff's device: one byte per arm, the table sitting at the jmp base.

    Only read when a `move.b d(pc,Dn.w),Dm` ($103B + reg<<9) shortly before the
    jmp shows the index really did come out of a byte table -- otherwise every
    switch in the segment would be read a second time as garbage.
    """
    lo = max(0, jmp - 16)
    if not any(d[k] & 0xF1 == 0x10 and d[k+1] == 0x3B for k in range(lo, jmp, 2)):
        return []
    out = []
    for n in range(limit):
        o = base + n
        if o >= len(d): break
        t = base + d[o]
        if t <= o or t >= len(d) or (t & 1): break
        out.append(t)
    return out


def scan(code_dir, gen_dir, segs=range(1, 22)):
    out = {}
    for seg in segs:
        binp = os.path.join(code_dir, 'CODE_%d.bin' % seg)
        genp = os.path.join(gen_dir, 'code_%d.c' % seg)
        if not (os.path.exists(binp) and os.path.exists(genp)): continue
        d = open(binp, 'rb').read()[4:]          # skip the CODE header
        gen = open(genp, encoding='utf-8', errors='replace').read()
        known  = set(int(m, 16) for m in re.findall(r'case 0x([0-9a-f]+)u:', gen))
        known |= set(int(m, 16) for m in re.findall(r'm68k_register\(base\+0x([0-9a-f]+)u', gen))
        miss = []
        for i in range(0, len(d) - 4, 2):
            if d[i] != 0x4E or d[i+1] != 0xFB: continue
            ext = i + 2                           # the brief extension word
            # The low byte of the extension word is a signed displacement from
            # the extension word's own address. A byte table sits exactly
            # there, and its arm offsets are measured from the same point.
            disp = (d[ext + 1] ^ 0x80) - 0x80
            for t in word_table(d, ext) + byte_table(d, ext + disp, i):
                if t not in known: miss.append(t)
        if miss: out[seg] = sorted(set(miss))
    return out

if __name__ == '__main__':
    code_dir = sys.argv[1] if len(sys.argv) > 1 else 'work/hypercard-ewec/code'
    gen_dir  = sys.argv[2] if len(sys.argv) > 2 else 'work/gen'
    found = scan(code_dir, gen_dir)
    for seg in sorted(found):
        print('CODE %-2d  --entry %s' % (seg, ','.join(hex(t) for t in found[seg])))
    print('%d unreachable target(s) in %d segment(s)'
          % (sum(len(v) for v in found.values()), len(found)))
