#!/usr/bin/env python3
"""Statically unpack a self-decrypting classic-Mac app (no emulator needed).

Reverses the runtime decryptor that ships in a plaintext CODE segment, recovering
the plaintext CODE resources for disassembly/lifting. Implements the scheme used
by *Shufflepuck Cafe* (Broderbund 1988, "HLS Duplication" protection), fully
reverse-engineered — see the game repo's docs/PROTECTION.md.

The scheme, all confirmed (byte-exact vs a Ghidra p-code emulation of the real
handler):
  1. Jump table (CODE 0): entries 1..N decrypted by  next += (cur ^ JT_KEY),
     a forward additive chain (JT_KEY = $F55C). Recovers the function directory
     AND the embedded _GetResource decrypt-handler (in the A5 world).
  2. Each CODE body is decrypted by that handler:
       seed = (header_word with bit14 cleared) ^ resourceID     # its `bclr #6`
       K    = crc16(seed, key_material = handler code $750..$868, poly $32E6)
       body: for size/2-2 words from CODE+2:  next += (cur ^ K)   (skip cur==0)

Per-title constants live in PROFILE; the algorithm is general.

Usage:  python unprotect.py work/code -o work/unpacked
"""
import argparse, glob, json, os, re, struct
from collections import Counter

PROFILE = {                       # Shufflepuck Cafe
    "jt_key":       0xF55C,
    "crc_poly":     0x32E6,
    "km_start":     0x750,        # key-material region in the A5 world (jump table + handlers)
    "km_end":       0x868,        # (= the _GetResource handler's own code)
    "a5_jt_off":    0x20,         # jump table offset from A5 (CODE 0 header field)
    "enc_segments": (1, 2, 3, 5), # encrypted via the handler (0 = jump table, 4 = plaintext loader)
}


def u16(b, o): return struct.unpack_from(">H", b, o)[0]


def decrypt_jump_table(code0, key):
    above, below, jtlen, jtoff = struct.unpack(">IIII", code0[:16])
    body = bytearray(code0[16:])
    w = list(struct.unpack(">%dH" % (len(body) // 2), body[: len(body) // 2 * 2]))
    for k in range((jtlen - 8) // 2):
        i = (8 // 2) + k                    # start at entry 1 (A5+0x28)
        if i + 1 >= len(w):
            break
        w[i + 1] = (w[i + 1] + ((w[i] ^ key) & 0xFFFF)) & 0xFFFF
    dec = b"".join(struct.pack(">H", x) for x in w)
    entries = []
    for e in range(jtlen // 8):
        o, push, seg, trap = struct.unpack(">HHHH", dec[e * 8:e * 8 + 8])
        entries.append({"idx": e, "offset": o, "segment": seg,
                        "thunk": bool(push == 0x3F3C and trap == 0xA9F0 and 1 <= seg <= 15)})
    return struct.pack(">IIII", above, below, jtlen, jtoff) + dec, entries, jtoff


def crc16(seed, key_material_words, poly):
    """16-bit CRC the handler computes over its own code to derive a body key."""
    d0 = seed & 0xFFFF
    for hw in key_material_words:
        d1 = hw & 0xFFFF
        for _ in range(16):
            x = (d1 >> 15) & 1; d1 = (d1 << 1) & 0xFFFF   # lsl.w #1,d1
            d0 ^= (x << 15)                               # (moveq#0,d2; roxr; eor.w d2,d0)
            c = (d0 >> 15) & 1; d0 = (d0 << 1) & 0xFFFF   # lsl.w #1,d0
            if c: d0 ^= poly                              # eor.w d3,d0
    return d0 & 0xFFFF


def decrypt_segment(raw, resid, key_material_words, poly):
    w = list(struct.unpack(">%dH" % (len(raw) // 2), raw[: len(raw) // 2 * 2]))
    w[1] &= ~0x4000                                       # bclr #6 of header high byte
    K = crc16((w[1] ^ resid) & 0xFFFF, key_material_words, poly)
    a0 = 1
    for _ in range(len(raw) // 2 - 2):
        cur = w[a0]; a0 += 1
        if cur != 0:
            w[a0] = (w[a0] + ((cur ^ K) & 0xFFFF)) & 0xFFFF
    return b"".join(struct.pack(">H", x) for x in w), K


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code_dir")
    ap.add_argument("-o", "--out", default="unpacked")
    p = PROFILE
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    def sid(f): return int(re.search(r"(\d+)", os.path.basename(f)).group(1))
    files = {sid(f): f for f in glob.glob(os.path.join(args.code_dir, "CODE_*.bin"))}

    # 1. jump table -> function directory + the A5 world (contains the handler)
    dec0, entries, jtoff = decrypt_jump_table(open(files[0], "rb").read(), p["jt_key"])
    open(os.path.join(args.out, "CODE_0.dec.bin"), "wb").write(dec0)
    a5 = dec0[16:]                                        # A5+jtoff == a5[0]
    km = a5[p["km_start"] - jtoff: p["km_end"] - jtoff]   # CRC key material (handler code)
    km_words = list(struct.unpack(">%dH" % (len(km) // 2), km))
    thunks = [e for e in entries if e["thunk"]]
    dist = Counter(e["segment"] for e in thunks)
    json.dump({"n_entries": len(entries), "n_functions": len(thunks),
               "per_segment": dict(sorted(dist.items())), "entries": entries},
              open(os.path.join(args.out, "jumptable.json"), "w"), indent=2)
    print(f"jump table: {len(thunks)} functions  per-segment {dict(sorted(dist.items()))}")

    # 2. decrypt each encrypted CODE body
    keys = {}
    for s in p["enc_segments"]:
        if s not in files:
            continue
        body, K = decrypt_segment(open(files[s], "rb").read(), s, km_words, p["crc_poly"])
        open(os.path.join(args.out, f"CODE_{s}.dec.bin"), "wb").write(body)
        keys[s] = K
        print(f"CODE {s}: key=${K:04X} -> CODE_{s}.dec.bin")
    if 4 in files:                                        # plaintext loader, copied through
        import shutil; shutil.copy(files[4], os.path.join(args.out, "CODE_4.dec.bin"))

    # self-checks against known-good keys (byte-exact vs Ghidra emulation)
    assert entries[1]["thunk"] and entries[1]["segment"] == 1
    for s, want in {1: 0x02AE, 2: 0xCF4C, 5: 0x4900}.items():
        if s in keys:
            assert keys[s] == want, f"CODE {s} key ${keys[s]:04X} != ${want:04X}"
    print("self-check OK (keys match reference)")


if __name__ == "__main__":
    main()
