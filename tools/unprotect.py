#!/usr/bin/env python3
"""Unpack a self-decrypting classic-Mac app (static, reproducible).

Reverses the runtime decryptor that ships in a plaintext CODE segment, so the
encrypted CODE resources can be disassembled/lifted without executing anything.
Currently implements the scheme used by *Shufflepuck Cafe* (Broderbund 1988,
"HLS Duplication" protection); the transforms are parameterised so sibling
titles can be added.

Confirmed stages (see the game repo's docs/PROTECTION.md for the full RE):
  * Stage 1 — the loader self-decrypts a local key/string table by word-negation.
  * Jump table — CODE 0's entries 1..N are decrypted in the A5 world by an
    `eor <key>` + additive forward chain (key $F55C for Shufflepuck). This
    recovers the full function directory (every routine's segment + offset).

Not yet reversed (WIP): the second decryptor pass that finishes the tail
jump-table entries, and the per-segment body cipher (seeded by each encrypted
segment's header word, e.g. CODE 1 header $400A). Those still need tracing.

Usage:  python unprotect.py work/code -o work/unpacked
"""
import argparse, glob, json, os, re, struct

JT_KEY = 0xF55C   # Shufflepuck jump-table decrypt key


def u16(b, o): return struct.unpack_from(">H", b, o)[0]


def decrypt_jump_table(code0, key=JT_KEY):
    """CODE 0 -> (decrypted bytes, list of entries). Entry 0 is plaintext; the
    rest are recovered by the eor+add forward chain over the A5-world copy."""
    above, below, jtlen, jtoff = struct.unpack(">IIII", code0[:16])
    body = bytearray(code0[16:])
    w = list(struct.unpack(">%dH" % (len(body) // 2), body[: len(body) // 2 * 2]))
    start = 8 // 2                      # first word of entry 1 (A5+0x28)
    n_words = (jtlen - 8) // 2          # all entries except entry 0
    for k in range(n_words):
        i = start + k
        if i + 1 >= len(w):
            break
        d3 = (w[i] ^ key) & 0xFFFF
        w[i + 1] = (w[i + 1] + d3) & 0xFFFF
    dec = bytearray()
    for x in w:
        dec += struct.pack(">H", x)
    entries = []
    for e in range(jtlen // 8):
        off = e * 8
        if off + 8 > len(dec):
            break
        o, push, seg, trap = struct.unpack(">HHHH", dec[off:off + 8])
        entries.append({"idx": e, "offset": o, "segment": seg,
                        "valid": bool(push == 0x3F3C and trap == 0xA9F0 and 1 <= seg <= 15)})
    return bytes(struct.pack(">IIII", above, below, jtlen, jtoff)) + bytes(dec), entries


def unnegate_table(code4, lo=0x02, hi=0x42):
    """Stage 1: undo the loader's word-negation of its local table (CODE 4)."""
    seg = bytearray(code4[4:])         # drop 4-byte seg header
    for off in range(lo, hi, 2):
        seg[off:off + 2] = struct.pack(">H", (-u16(seg, off)) & 0xFFFF)
    strings = re.findall(rb"[\x20-\x7e]{4,}", bytes(seg))
    return bytes(seg), [s.decode("mac_roman") for s in strings]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code_dir", help="dir of CODE_*.bin from extract_resources.py")
    ap.add_argument("-o", "--out", default="unpacked")
    ap.add_argument("--key", type=lambda x: int(x, 0), default=JT_KEY)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    def seg_id(p): return int(re.search(r"(\d+)", os.path.basename(p)).group(1))
    files = {seg_id(f): f for f in glob.glob(os.path.join(args.code_dir, "CODE_*.bin"))}

    # --- jump table (CODE 0) ---
    c0 = open(files[0], "rb").read()
    dec0, entries = decrypt_jump_table(c0, args.key)
    open(os.path.join(args.out, "CODE_0.dec.bin"), "wb").write(dec0)
    valid = [e for e in entries if e["valid"]]
    from collections import Counter
    dist = Counter(e["segment"] for e in valid)
    json.dump({"key": f"0x{args.key:04X}", "n_entries": len(entries),
               "n_valid": len(valid), "per_segment": dict(sorted(dist.items())),
               "entries": entries},
              open(os.path.join(args.out, "jumptable.json"), "w"), indent=2)
    print(f"jump table: {len(valid)}/{len(entries)} valid thunks  "
          f"per-segment {dict(sorted(dist.items()))}")

    # --- Stage 1 table (CODE 4), if present & plaintext loader ---
    if 4 in files:
        seg4, strings = unnegate_table(open(files[4], "rb").read())
        open(os.path.join(args.out, "CODE_4.table.bin"), "wb").write(seg4)
        hit = [s for s in strings if "Shuffle" in s or "Duplication" in s]
        print(f"stage-1 table: strings {hit or strings[:3]}")

    # --- encrypted segment bodies (WIP) ---
    print("segment body cipher (WIP) - per-segment header key words:")
    for sid in sorted(s for s in files if s not in (0, 4)):
        hdr = open(files[sid], "rb").read()[:4]
        print(f"  CODE {sid}: header {hdr.hex()}  (candidate key ${u16(hdr,2):04X})")

    # --- self-checks ---
    e1 = entries[1]
    assert e1["valid"] and e1["segment"] == 1, f"entry1 should be a seg-1 thunk, got {e1}"
    if 4 in files:
        assert any("Shufflepuck" in s for s in strings), "stage-1 negation failed"
    print("self-check OK")


if __name__ == "__main__":
    main()
