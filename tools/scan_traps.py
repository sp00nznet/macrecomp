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

# --- coverage: the Toolbox surface a title needs vs. what the HAL implements ---

# Which manager owns a trap. Inside Macintosh groups traps by manager but
# traps.json is only word->name, so the grouping lives here.
#
# Each entry is (manager, "exact names", "Substrings"). Exact names win over
# every substring; substrings are then tried in table order, first match wins.
# Order therefore matters: "Ptr" (Memory) must be tried before "Pt"
# (QuickDraw), and "Menu" before "Res" so AppendResMenu lands in Menu Mgr.
# Traps this table cannot place are reported as unclassified rather than
# quietly bucketed -- most of those are data bytes misread as A-line opcodes.
MANAGERS = [
    ("SANE (float)", "DECSTR68K SetFractEnable", "FP68K Elems68K Frac Fix X2 2X"),
    ("Trap Mgr",     "", "TrapAddress"),
    ("Script Mgr",   "ScriptUtil KeyScript Font2Script", ""),
    ("TextEdit",     "", "TE"),
    ("Dialog Mgr",   "InitDialogs ParamText ErrorSound", "Dialog Alert"),
    ("Control Mgr",  "", "Control Ctl"),
    ("Menu Mgr",     "SysEdit SetItem GetItem CheckItem CountMItems EnableItem "
                     "DisableItem SetItemMark GetItemMark SetItemIcon SetItemStyle",
                     "Menu"),
    ("Window Mgr",   "VisRegionChanged CalcVis CalcVBehind PaintOne PaintBehind "
                     "BringToFront SendBehind FrontWindow",
                     "Window WMgr UpDate Update GoAway GrowIcon WTitle WRefCon"),
    ("Segment Ldr",  "", "LoadSeg"),
    ("Sound Mgr",    "SysBeep SetSoundVol GetSoundVol StartSound StopSound SoundDone",
                     "Snd SoundDispatch"),
    ("Desk Mgr",     "OpenDeskAcc CloseDeskAcc SystemClick SystemEdit SystemTask", ""),
    ("Print Mgr",    "PrGlue", ""),
    ("Resource Mgr", "OpenRF OpenRFPerm AddReference UniqueID Unique1ID CreateResFile",
                     "Res"),
    ("File Mgr",     "HFSDispatch FSDispatch Eject Allocate SFGetFile SFPutFile "
                     "SFPGetFile SFPPutFile GetFName Open Close Read Write Create "
                     "Delete Rename GetEOF SetEOF GetFPos SetFPos FlushFile",
                     "PB Vol File FInfo HGet HSet HOpen HCreate HDelete HRename"),
    ("Memory Mgr",   "BlockMove MoveHHi MaxMem FreeMem CompactMem PurgeMem PurgeSpace "
                     "StackSpace MoreMasters MaxApplZone SetApplLimit ResrvMem "
                     "EmptyHandle ReallocHandle RecoverHandle HeapDispatch "
                     "HLock HUnlock HPurge HNoPurge NewString SetString GetString",
                     "Ptr Handle Zone HandToHand"),
    ("Event Mgr",    "GetNextEvent WaitNextEvent EventAvail PostEvent FlushEvents "
                     "GetMouse Button StillDown WaitMouseUp TickCount GetKeys "
                     "KeyTranslate GetCaretTime GetDblTime", ""),
    ("Scrap Mgr",    "", "Scrap"),
    ("Font Mgr",     "InitFonts RealFont FMSwapFont SetFScaleDisable", "Font FNum"),
    ("Package Mgr",  "UnpackBits PackBits", "Pack"),
    ("List Mgr",     "LNew LDispose LAddRow LDelRow LAddColumn LDelColumn LSetCell "
                     "LGetCell LClick LUpdate LActivate LDraw LSetSelect LGetSelect "
                     "LScroll LSize LFind LRect LNextCell LAutoScroll", ""),
    ("Device Mgr",   "", "Device"),
    ("QuickDraw",    "InitGraf InitPort OpenPort ClosePort SetOrigin ClipRect BackPat "
                     "ForeColor BackColor GlobalToLocal LocalToGlobal SpaceExtra "
                     "ScrollRect SeedFill CalcMask GetPixel Random DeltaPoint "
                     "StdText StuffHex Move MoveTo Line LineTo CopyMask CopyBits",
                     "Rect Rgn Poly Oval Arc Pic Pen Pt Port Clip Cursor Bits Text "
                     "Draw Std Paint Frame Fill Invert Inver Erase Hilite Color"),
    ("OS Utilities", "OSDispatch StripAddress SysEnvirons Gestalt Delay ShutDown "
                     "ReadDateTime GetDateTime SetDateTime DateToSeconds SecondsToDate "
                     "ExitToShell Launch Chain Debugger DebugStr SwapMMUMode Status "
                     "Enqueue Dequeue HWPriv AUXDispatch DisplayDispatch Unimplemented",
                     ""),
    ("Toolbox Util", "Munger GetIcon PlotIcon ShieldCursor",
                     "Bit HiWord LoWord LongMul Num Str IU"),
]

_EXACT = {n: m for m, ex, _ in MANAGERS for n in ex.split()}
_SUBSTR = [(s, m) for m, _, sub in MANAGERS for s in sub.split()]


def manager(name):
    """Which Toolbox manager a trap belongs to, or None if the table lacks it."""
    if not name:
        return None
    if name in _EXACT:
        return _EXACT[name]
    for needle, mgr in _SUBSTR:
        if needle in name:
            return mgr
    return None


def implemented_traps(path):
    """Trap words a C HAL dispatches, read from its `case 0xAxxx:` labels."""
    src = open(path, encoding="utf-8", errors="replace").read()
    return {int(w, 16) for w in re.findall(r"case\s+0x([Aa][0-9A-Fa-f]{3})\s*:", src)}


def report_coverage(total, hal_path):
    """Print the implemented/missing split, grouped into work packages."""
    impl = implemented_traps(hal_path)
    sites = sum(total.values())
    hit_sites = sum(c for w, c in total.items() if w in impl)
    hit_kinds = sum(1 for w in total if w in impl)
    pct = lambda a, b: 100 * a // b if b else 0

    print(f"\n=== COVERAGE vs {os.path.basename(hal_path)} ===")
    print(f"  call sites  {hit_sites}/{sites} ({pct(hit_sites, sites)}%)")
    print(f"  distinct    {hit_kinds}/{len(total)} ({pct(hit_kinds, len(total))}%)")

    by_mgr, unknown = {}, []
    for w, c in total.items():
        name = trap_name(w)
        mgr = manager(name)
        if mgr is None:
            unknown.append((c, name or f"?${w:04X}"))
            mgr = "unclassified"
        d = by_mgr.setdefault(mgr, {"have": 0, "need": 0, "missing": []})
        if w in impl:
            d["have"] += c
        else:
            d["need"] += c
            d["missing"].append((c, name or f"${w:04X}"))

    rows = sorted(by_mgr.items(), key=lambda kv: -kv[1]["need"])
    print("\n  missing, by manager, ranked by call sites:")
    for mgr, d in rows:
        if not d["need"]:
            continue
        top = sorted(d["missing"], reverse=True)
        names = ", ".join(n for _, n in top[:6])
        more = "" if len(top) <= 6 else f", +{len(top) - 6} more"
        print(f"    {d['need']:>5} sites {len(top):>3} traps  {mgr:<14} {names}{more}")

    done = [m for m, d in rows if d["have"] and not d["need"]]
    if done:
        print(f"\n  fully covered: {', '.join(sorted(done))}")
    if unknown:
        print(f"\n  {len(unknown)} trap(s) not in the manager table: "
              + ", ".join(n for _, n in sorted(unknown, reverse=True)[:12]))

    return {
        "sites_total": sites, "sites_covered": hit_sites,
        "distinct_total": len(total), "distinct_covered": hit_kinds,
        "by_manager": {
            m: {"have": d["have"], "need": d["need"],
                "missing": [n for _, n in sorted(d["missing"], reverse=True)]}
            for m, d in by_mgr.items()
        },
    }


def looks_like_data(total):
    """True if the 'trap set' looks like noise rather than a real one.

    Real 68k code reuses a small set of traps heavily -- SetPort and friends run
    to hundreds of sites -- so distinct/sites sits near 0.1-0.2. Encrypted or
    packed segments disassemble to random words, where almost every hit is
    unique and the ratio approaches 1. Catches pointing the scanner at a
    protected title's still-encrypted CODE, which otherwise reports a confident
    and completely wrong trap set.

    ponytail: one ratio, no entropy test. If a real title ever trips it, print
    the ratio and raise the threshold rather than reaching for something clever.
    """
    sites = sum(total.values())
    if sites < 50:
        return None
    ratio = len(total) / sites
    return ratio if ratio > 0.40 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code_dir", help="directory of CODE_*.bin from extract_resources.py")
    ap.add_argument("--json", help="write full result JSON here")
    ap.add_argument("--coverage", metavar="TOOLBOX_C",
                    help="report how much of this title's trap set the HAL "
                         "already dispatches (e.g. runtime/toolbox.c)")
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
    ratio = looks_like_data(total)
    if ratio is not None:
        print(f"\n  !! {len(total)} distinct traps over {sum(total.values())} sites "
              f"({ratio:.0%} unique) -- this does not look like 68k code.\n"
              f"     Protected titles must be run through unprotect.py first; "
              f"scan the decrypted segments (work/unpacked/), not work/code/.")

    unknown = [n for n, w, c in named if n.startswith("?$")]
    if unknown:
        print(f"\n  {len(unknown)} unnamed word(s) (likely embedded data, not traps): "
              + ", ".join(unknown))

    cov = report_coverage(total, args.coverage) if args.coverage else None

    if args.json:
        json.dump({"jump_table": jt, "per_segment": per_seg, "coverage": cov,
                   "traps": [{"word": f"0x{w:04X}", "name": trap_name(w), "count": c}
                             for _, w, c in named]},
                  open(args.json, "w"), indent=2)
        print(f"\nwrote {args.json}")

if __name__ == "__main__":
    main()
