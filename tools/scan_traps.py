#!/usr/bin/env python3
"""Enumerate the Toolbox A-line traps a classic-Mac app calls — the recomp
scope gate. Also summarises CODE 0's A5 jump table.

Method: disassemble each CODE segment with capstone-M68K in skipdata mode.
Real instructions (and their immediate/extension words) are consumed by the
decoder, so only genuine line-A opcodes ($A000-$AFFF) surface as skipped words.
That is exactly the set of Toolbox/OS traps the code invokes. Data bytes that
happen to be embedded in a code segment are a possible false positive and are
flagged in the notes.

Usage:  python scan_traps.py work/code/           # dir of CODE_*.bin
        python scan_traps.py work/code/ --json out.json
"""
import argparse, glob, json, os, re, struct, sys
from collections import Counter
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

TRAPS = json.load(open(os.path.join(os.path.dirname(__file__), "traps.json")))["traps"]

def trap_name(word):
    key = f"0x{word:04X}"
    if key in TRAPS: return TRAPS[key]
    canon = (0xA800 | (word & 0x03FF)) if (word & 0x0800) else (0xA000 | (word & 0x00FF))
    return TRAPS.get(f"0x{canon:04X}")

def seg_id(path):
    m = re.search(r'CODE[_-]?(\d+)', os.path.basename(path))
    return int(m.group(1)) if m else -1

def parse_jt(code0):
    """CODE 0 = A5 world header + jump table. Return list of (seg, offset)."""
    if len(code0) < 16: return None, {}
    above, below, jtlen, jtoff = struct.unpack(">IIII", code0[:16])
    entries, dist = [], Counter()
    body = code0[16:]
    for i in range(0, min(jtlen, len(body)) - 7, 8):
        off, push, seg, trap = struct.unpack(">HHHH", body[i:i+8])
        # unloaded entry looks like: <off> 3F3C <seg> A9F0(_LoadSeg)
        if push == 0x3F3C and trap == 0xA9F0:
            entries.append((seg, off)); dist[seg] += 1
    return {"above_a5": above, "below_a5": below, "jt_len": jtlen,
            "jt_off_from_a5": jtoff, "n_entries": len(entries),
            "per_segment": dict(sorted(dist.items()))}, entries

def scan_segment(data):
    """Return Counter of A-line trap words in one CODE segment (after 4B header)."""
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000); md.skipdata = True
    code = data[4:]  # skip the 4-byte segment header (jt-offset, jt-count)
    traps = Counter(); insns = 0
    for insn in md.disasm(code, 0):
        b = insn.bytes
        if insn.id == 0 and len(b) == 2 and 0xA0 <= b[0] <= 0xAF:
            traps[(b[0] << 8) | b[1]] += 1
        else:
            insns += 1
    return traps, insns

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code_dir", help="directory of CODE_*.bin from extract_resources.py")
    ap.add_argument("--json", help="write full result JSON here")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.code_dir, "CODE_*.bin")), key=seg_id)
    if not files: sys.exit(f"no CODE_*.bin in {args.code_dir}")

    total = Counter(); per_seg = {}
    jt = None
    for f in files:
        sid = seg_id(f); data = open(f, "rb").read()
        if sid == 0:
            jt, _ = parse_jt(data)
            continue
        traps, insns = scan_segment(data)
        per_seg[sid] = {"bytes": len(data), "insns": insns,
                        "trap_sites": sum(traps.values()), "distinct": len(traps)}
        total.update(traps)

    print("=== A5 jump table (CODE 0) ===")
    print(json.dumps(jt, indent=2) if jt else "  (no CODE 0)")
    print("\n=== per-segment ===")
    for sid in sorted(per_seg):
        s = per_seg[sid]
        print(f"  CODE {sid}: {s['bytes']:6d}B  ~{s['insns']:5d} insns  "
              f"{s['trap_sites']:4d} trap sites  {s['distinct']:3d} distinct")

    named = [(trap_name(w) or f"?${w:04X}", w, c) for w, c in total.items()]
    named.sort(key=lambda x: -x[2])
    print(f"\n=== TRAP SET: {len(total)} distinct, {sum(total.values())} total sites ===")
    for name, w, c in named:
        print(f"  {c:4d}x  ${w:04X}  {name}")
    unknown = [n for n, w, c in named if n.startswith("?$")]
    if unknown:
        print(f"\n  {len(unknown)} unnamed word(s) (likely embedded data, not traps): "
              + ", ".join(unknown))

    if args.json:
        json.dump({"jump_table": jt, "per_segment": per_seg,
                   "traps": [{"word": f"0x{w:04X}", "name": trap_name(w), "count": c}
                             for _, w, c in named]},
                  open(args.json, "w"), indent=2)
        print(f"\nwrote {args.json}")

if __name__ == "__main__":
    main()
