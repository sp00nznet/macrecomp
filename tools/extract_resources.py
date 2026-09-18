#!/usr/bin/env python3
"""Extract a classic-Mac application's resources for static recompilation.

Mounts a DiskCopy 4.2 (.dc42/.img) or raw HFS image, finds the target
application, and writes:
  <out>/code/CODE_<id>.bin   each 68k CODE segment (what the lifter consumes)
  <out>/rsrc/<TYPE>_<id>.bin every other resource (assets), verbatim
  <out>/inventory.json       full resource map (type, id, name, size, offsets)
  <out>/jumptable.json       the A5 jump table: one entry per exported routine

Pure Python: only depends on `machfs` and `macresources` (pip install both).
No Apple ROM/System code is touched — just the app's own resource fork.
"""
import argparse, json, struct, sys
from pathlib import Path
from collections import Counter, defaultdict

from machfs import Volume
import macresources

DC42_HEADER = 84
HFS_800K = 819200


def load_hfs(raw: bytes) -> bytes:
    """Return the HFS volume bytes from a .dc42, a partitioned CD, or raw HFS."""
    # DiskCopy 4.2: 84-byte header; data-size field at offset 64 (big-endian).
    if len(raw) > DC42_HEADER + 512:
        data_size = struct.unpack(">I", raw[64:68])[0]
        if data_size in (409600, 819200, 1474560) and len(raw) >= DC42_HEADER + data_size:
            return raw[DC42_HEADER:DC42_HEADER + data_size]
    # Mac CD-ROM: 'ER' driver descriptor, then a map of 'PM' entries at 0x200.
    # The HFS volume is a partition inside it, not at offset 0.
    if raw[:2] == b"ER":
        n, i = 1, 0
        while i < n:
            b = raw[0x200 + i * 512:0x200 + (i + 1) * 512]
            if b[:2] != b"PM":
                break
            n = struct.unpack(">I", b[4:8])[0]
            start, blocks = struct.unpack(">II", b[8:16])
            if b[48:80].split(bytes(1))[0] == b"Apple_HFS":
                return raw[start * 512:(start + blocks) * 512]
            i += 1
    return raw


def parse_jump_table(code0: bytes):
    """CODE 0 is the A5 world header + jump table; return the lifter's entry list.

    Header: aboveA5(4), belowA5(4), jtLen(4), jtOffset(4). Then 8-byte entries:
    routine offset(2), then either the unloaded thunk `MOVE.W #seg,-(SP)` /
    `_LoadSeg` ($3F3C, seg, $A9F0), or an already-loaded JMP. Only thunks name a
    routine the lifter can turn into a function.

    unprotect.py emits the same shape for protected titles, after decrypting."""
    if len(code0) < 16:
        return None
    jt_len, jt_off = struct.unpack(">II", code0[8:16])
    body, entries = code0[16:], []
    for i in range(0, min(jt_len, len(body)) - 7, 8):
        off, push, seg, trap = struct.unpack(">HHHH", body[i:i + 8])
        entries.append({"idx": i // 8, "offset": off, "segment": seg,
                        "thunk": push == 0x3F3C and trap == 0xA9F0})
    return {"jt_offset": jt_off, "jt_len": jt_len, "entries": entries}


def find_app(vol, want=None):
    """Yield (path, file) for the APPL (or the named file) in the volume tree."""
    def walk(folder, path=""):
        for name, item in folder.items():
            p = path + "/" + name
            if hasattr(item, "items"):
                yield from walk(item, p)
            else:
                yield p, item
    files = list(walk(vol))
    if want:
        for p, f in files:
            if p.rstrip("/").endswith(want):
                return p, f
        sys.exit(f"file not found in image: {want!r}")
    appls = [(p, f) for p, f in files if getattr(f, "type", b"") == b"APPL"]
    if not appls:
        sys.exit("no APPL found; pass --file to pick one of:\n  " +
                 "\n  ".join(p for p, _ in files))
    if len(appls) > 1:
        sys.stderr.write("multiple APPLs; using first. others:\n  " +
                         "\n  ".join(p for p, _ in appls[1:]) + "\n")
    return appls[0]


def dump_forks(vol, out):
    """Write every file's two forks, and an index naming them.

    A classic-Mac file is two streams, and an application opens them by name
    through the File Manager -- so a HAL that wants to serve real files needs
    both forks of every file, not just the resources of one application. Names
    are kept verbatim in files.json (they may contain '/' or ':' or non-ASCII)
    and the files themselves are numbered, so the index is the only place the
    original name has to survive."""
    out.mkdir(parents=True, exist_ok=True)
    index = []
    for n, (path, f) in enumerate(sorted(walk_files(vol))):
        data, rsrc = bytes(f.data or b""), bytes(f.rsrc or b"")
        if data: (out / f"{n}.data").write_bytes(data)
        if rsrc: (out / f"{n}.rsrc").write_bytes(rsrc)
        index.append({"n": n, "path": path, "name": path.rsplit("/", 1)[-1],
                      "type": f.type.decode("mac_roman", "replace"),
                      "creator": f.creator.decode("mac_roman", "replace"),
                      "data": len(data), "rsrc": len(rsrc)})
    (out / "files.json").write_text(json.dumps({"volume": vol.name, "files": index}, indent=2))
    tot = sum(e["data"] + e["rsrc"] for e in index)
    print(f"forks: {len(index)} files, {tot} bytes -> {out}/")


def walk_files(vol):
    def walk(folder, path=""):
        for name, item in folder.items():
            p = path + "/" + name
            if hasattr(item, "items"):
                yield from walk(item, p)
            else:
                yield p, item
    return walk(vol)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="DiskCopy 4.2 / raw HFS disk image")
    ap.add_argument("-o", "--out", default="work", help="output dir (default: work)")
    ap.add_argument("--file", help="extract this filename instead of the first APPL")
    ap.add_argument("--forks", metavar="DIR",
                    help="also write EVERY file's data and resource forks to DIR, "
                         "plus files.json -- what a File Manager HAL serves")
    args = ap.parse_args()

    raw = Path(args.image).read_bytes()
    vol = Volume(); vol.read(load_hfs(raw))
    if args.forks:
        dump_forks(vol, Path(args.forks))
    path, app = find_app(vol, args.file)
    print(f"volume {vol.name!r}  app {path!r}  "
          f"type={app.type} creator={app.creator} data={len(app.data or b'')} "
          f"rsrc={len(app.rsrc or b'')}")

    out = Path(args.out); (out / "code").mkdir(parents=True, exist_ok=True)
    (out / "rsrc").mkdir(parents=True, exist_ok=True)
    if app.data:
        (out / "data_fork.bin").write_bytes(app.data)

    inv, by, code0 = [], defaultdict(int), None
    for r in macresources.parse_file(app.rsrc):
        t = bytes(r.type); tname = t.decode("mac_roman", "replace")
        data = bytes(r.data); by[tname] += len(data)
        nm = r.name.decode("mac_roman", "replace") if isinstance(r.name, (bytes, bytearray)) else (r.name or "")
        safe = tname.strip().replace("/", "_") or "____"
        if t == b"CODE" and r.id == 0:
            code0 = data
        sub = "code" if t == b"CODE" else "rsrc"
        stem = f"CODE_{r.id}" if t == b"CODE" else f"{safe}_{r.id}"
        (out / sub / f"{stem}.bin").write_bytes(data)
        inv.append({"type": tname, "id": r.id, "name": nm, "size": len(data),
                    "attrs": int(getattr(r, "attributes", 0) or 0)})

    (out / "inventory.json").write_text(json.dumps(
        {"volume": vol.name, "app": path, "type": app.type.decode("mac_roman"),
         "creator": app.creator.decode("mac_roman"),
         "data_fork": len(app.data or b""), "resources": inv}, indent=2))

    jt = parse_jump_table(code0) if code0 else None
    if jt:
        thunks = [e for e in jt["entries"] if e["thunk"]]
        dist = Counter(e["segment"] for e in thunks)
        (out / "jumptable.json").write_text(json.dumps(
            {"n_entries": len(jt["entries"]), "n_functions": len(thunks),
             "jt_offset": jt["jt_offset"], "jt_len": jt["jt_len"],
             "per_segment": {str(k): v for k, v in sorted(dist.items())},
             "entries": jt["entries"]}, indent=2))
        print(f"jump table: {len(thunks)} functions over {len(dist)} segments "
              f"-> {out}/jumptable.json")
    elif code0 is None:
        print("no CODE 0: this resource fork has no jump table")

    print(f"\n{len(inv)} resources -> {out}/  (by size:)")
    for tname in sorted(by, key=lambda k: -by[k]):
        n = sum(1 for e in inv if e["type"] == tname)
        print(f"  {tname:5s} x{n:3d}  {by[tname]:8d} B")


if __name__ == "__main__":
    main()
