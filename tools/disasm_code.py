#!/usr/bin/env python3
"""Disassemble a classic-Mac CODE segment (capstone-M68K), annotated for recomp.

Annotates the two things that matter when reading Mac 68k:
  * A-line Toolbox traps        ($Axxx  -> trap name)
  * inter-segment jump-table calls  (jsr/jmp/pea  $NNN(a5)  -> the A5 jump table)

The 4-byte CODE segment header (jt-first-entry-offset, jt-entry-count) is skipped;
addresses shown are offsets into the segment's code.

Usage:  python disasm_code.py work/code/CODE_1.bin [--start 0x42] [--count N]
"""
import argparse, json, os, re
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

TRAPS = json.load(open(os.path.join(os.path.dirname(__file__), "traps.json")))["traps"]

def trap_name(word):
    key = f"0x{word:04X}"
    if key in TRAPS: return TRAPS[key]
    canon = (0xA800 | (word & 0x03FF)) if (word & 0x0800) else (0xA000 | (word & 0x00FF))
    return TRAPS.get(f"0x{canon:04X}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code_bin")
    ap.add_argument("--start", default="0x4", help="file offset to start (default 0x4, after seg header)")
    ap.add_argument("--count", type=int, default=0, help="max instructions (0 = all)")
    ap.add_argument("--raw", action="store_true", help="do not skip the 4-byte seg header")
    args = ap.parse_args()

    data = open(args.code_bin, "rb").read()
    start = int(args.start, 0)
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000); md.skipdata = True

    n = 0
    for insn in md.disasm(data[start:], start):
        b = insn.bytes; note = ""
        if insn.id == 0 and len(b) == 2 and 0xA0 <= b[0] <= 0xAF:
            w = (b[0] << 8) | b[1]
            note = f"   ; trap ${w:04X} {trap_name(w) or '?'}"
        else:
            m = re.search(r'\$([0-9a-fA-F]+)\(a5\)', insn.op_str)
            if m and insn.mnemonic in ("jsr", "jmp", "pea", "lea", "move.l"):
                note = f"   ; A5+${int(m.group(1),16):x} jump-table / global"
        print(f"{insn.address:06x}: {b.hex():<12s} {insn.mnemonic:<8s} {insn.op_str}{note}")
        n += 1
        if args.count and n >= args.count: break

if __name__ == "__main__":
    main()
