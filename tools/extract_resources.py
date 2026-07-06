#!/usr/bin/env python3
"""Extract a classic-Mac application's resources for static recompilation.

Mounts a DiskCopy 4.2 (.dc42/.img) or raw HFS image, finds the target
application, and writes:
  <out>/code/CODE_<id>.bin   each 68k CODE segment (what the lifter consumes)
  <out>/rsrc/<TYPE>_<id>.bin every other resource (assets), verbatim
  <out>/inventory.json       full resource map (type, id, name, size, offsets)

Pure Python: only depends on `machfs` and `macresources` (pip install both).
No Apple ROM/System code is touched — just the app's own resource fork.
"""
import argparse, json, struct, sys
from pathlib import Path
from collections import defaultdict

from machfs import Volume
import macresources

DC42_HEADER = 84
HFS_800K = 819200


def load_hfs(raw: bytes) -> bytes:
    """Return the HFS volume bytes from a .dc42, or the input if already raw HFS."""
    # DiskCopy 4.2: 84-byte header; data-size field at offset 64 (big-endian).
    if len(raw) > DC42_HEADER + 512:
        data_size = struct.unpack(">I", raw[64:68])[0]
        if data_size in (409600, 819200, 1474560) and len(raw) >= DC42_HEADER + data_size:
            return raw[DC42_HEADER:DC42_HEADER + data_size]
    return raw


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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="DiskCopy 4.2 / raw HFS disk image")
    ap.add_argument("-o", "--out", default="work", help="output dir (default: work)")
    ap.add_argument("--file", help="extract this filename instead of the first APPL")
    args = ap.parse_args()

    raw = Path(args.image).read_bytes()
    vol = Volume(); vol.read(load_hfs(raw))
    path, app = find_app(vol, args.file)
    print(f"volume {vol.name!r}  app {path!r}  "
          f"type={app.type} creator={app.creator} data={len(app.data or b'')} "
          f"rsrc={len(app.rsrc or b'')}")

    out = Path(args.out); (out / "code").mkdir(parents=True, exist_ok=True)
    (out / "rsrc").mkdir(parents=True, exist_ok=True)
    if app.data:
        (out / "data_fork.bin").write_bytes(app.data)

    inv, by = [], defaultdict(int)
    for r in macresources.parse_file(app.rsrc):
        t = bytes(r.type); tname = t.decode("mac_roman", "replace")
        data = bytes(r.data); by[tname] += len(data)
        nm = r.name.decode("mac_roman", "replace") if isinstance(r.name, (bytes, bytearray)) else (r.name or "")
        safe = tname.strip().replace("/", "_") or "____"
        sub = "code" if t == b"CODE" else "rsrc"
        stem = f"CODE_{r.id}" if t == b"CODE" else f"{safe}_{r.id}"
        (out / sub / f"{stem}.bin").write_bytes(data)
        inv.append({"type": tname, "id": r.id, "name": nm, "size": len(data),
                    "attrs": int(getattr(r, "attributes", 0) or 0)})

    (out / "inventory.json").write_text(json.dumps(
        {"volume": vol.name, "app": path, "type": app.type.decode("mac_roman"),
         "creator": app.creator.decode("mac_roman"),
         "data_fork": len(app.data or b""), "resources": inv}, indent=2))

    print(f"\n{len(inv)} resources -> {out}/  (by size:)")
    for tname in sorted(by, key=lambda k: -by[k]):
        n = sum(1 for e in inv if e["type"] == tname)
        print(f"  {tname:5s} x{n:3d}  {by[tname]:8d} B")


if __name__ == "__main__":
    main()
