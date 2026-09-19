#!/usr/bin/env python3
"""Find computed-jump targets the lifter cannot reach.

A `jmp d(pc,Dn.w)` -- 0x4EFB -- is a switch: an extension word, then a table of
16-bit offsets, then the arms.  Nothing in the binary *names* those arms, so a
linear decode only lands on one by luck, and a target it missed becomes an
`m68k_entry_miss` at run time: the function returns without running, silently.
One such address in CODE 12 was the whole reason HyperTalk did not execute.

This reads each table, checks every target against the generated code, and
prints the ones with neither a `case` label nor a registered function start --
i.e. exactly the addresses to hand back as `lift68k.py --entry`.

    python tools/find_entries.py work/hypercard-ewec/code work/gen

A target is dropped once an entry points backwards, outside the segment, or at
an odd address: the table has ended and what follows is the arms themselves.
"""
import os, re, struct, sys

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
            base = i + 2                          # targets are relative to this
            for n in range(64):
                o = base + 2 + n * 2
                if o + 2 > len(d): break
                t = base + struct.unpack('>H', d[o:o+2])[0]
                if t <= o or t >= len(d) or (t & 1): break
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
