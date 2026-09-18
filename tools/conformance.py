#!/usr/bin/env python3
"""Conformance harness: measure HAL coverage per title against a fixed corpus.

Runs extract -> scan -> coverage for every title in `corpus.json` and prints one
row per title. The number that matters is **covered call sites**: how many of the
Toolbox calls a title actually makes are ones the HAL dispatches. Falling below a
recorded baseline fails the run, so a HAL change that quietly breaks an existing
title is caught here rather than in a lifted game.

Classic-Mac media is not redistributable, so the corpus lives outside the repo:

    MACRECOMP_CORPUS=/path/to/images python tools/conformance.py

A title whose image is absent is reported SKIP with the file it wanted and where
that file comes from -- never a silent pass, and never a failure either, so CI
without a corpus still runs the harness end to end.

    --update    rewrite the baselines from this run (review the diff)
    --json P    write the full result to P
"""
import argparse
import io
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_resources import load_hfs, find_app  # noqa: E402
from scan_traps import parse_jt, report_coverage, scan_segment  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(HERE, "corpus.json")
HAL = os.path.join(ROOT, "runtime", "toolbox.c")


def code_segments(image_path, want_file=None):
    """The CODE resources of one app in a disk image: {id: bytes}."""
    from machfs import Volume
    import macresources

    raw = open(image_path, "rb").read()
    vol = Volume()
    vol.read(load_hfs(raw))
    _, app = find_app(vol, want_file)
    return {r.id: bytes(r.data) for r in macresources.parse_file(app.rsrc)
            if bytes(r.type) == b"CODE"}


def measure(image_path, want_file=None):
    """Coverage for one title, in the shape report_coverage returns."""
    segs = code_segments(image_path, want_file)
    if not segs:
        raise SystemExit(f"{image_path}: no CODE resources -- not a 68k application")
    total = Counter()
    for sid, data in sorted(segs.items()):
        if sid == 0:                       # CODE 0 is the jump table, not code
            parse_jt(data)
            continue
        traps, _ = scan_segment(data)
        total.update(traps)
    # report_coverage prints its own detailed breakdown; the harness wants the
    # numbers, and one row per title, so swallow the long form.
    held, sys.stdout = sys.stdout, io.StringIO()
    try:
        return report_coverage(total, HAL)
    finally:
        sys.stdout = held


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", default=os.environ.get("MACRECOMP_CORPUS", ""),
                    help="directory holding the corpus images "
                         "(default: $MACRECOMP_CORPUS)")
    ap.add_argument("--update", action="store_true",
                    help="rewrite baselines from this run")
    ap.add_argument("--json", help="write the full result here")
    args = ap.parse_args()

    titles = json.load(open(CORPUS))["titles"]
    rows, results, regressions, ran, skipped = [], {}, [], 0, 0

    for t in titles:
        name, base = t["name"], t.get("baseline")
        path = os.path.join(args.corpus, t["image"]) if args.corpus else t["image"]
        if not os.path.isfile(path):
            skipped += 1
            rows.append((name, "SKIP", f"{t['image']} not in corpus dir"))
            continue
        cov = measure(path, t.get("file"))
        results[name] = cov
        ran += 1
        got, tot = cov["sites_covered"], cov["sites_total"]
        pct = 100 * got // tot if tot else 0
        if base is None:
            rows.append((name, "NEW", f"{got}/{tot} sites ({pct}%) -- no baseline yet"))
        elif got < base["sites_covered"]:
            regressions.append(name)
            rows.append((name, "FAIL", f"{got}/{tot} sites ({pct}%) -- "
                                       f"was {base['sites_covered']}"))
        elif got > base["sites_covered"]:
            rows.append((name, "GAIN", f"{got}/{tot} sites ({pct}%) -- "
                                       f"was {base['sites_covered']}; --update to record"))
        else:
            rows.append((name, "ok", f"{got}/{tot} sites ({pct}%)"))

    w = max(len(r[0]) for r in rows) if rows else 0
    print(f"=== macrecomp conformance ({ran} measured, {skipped} skipped) ===")
    for name, verdict, note in rows:
        print(f"  {verdict:<4} {name:<{w}}  {note}")

    if skipped and not args.corpus:
        print("\n  No corpus directory set. Point MACRECOMP_CORPUS at a directory\n"
              "  holding the images named above; sources are listed in ROADMAP.md.")

    if args.update:
        for t in titles:
            if t["name"] in results:
                c = results[t["name"]]
                t["baseline"] = {k: c[k] for k in
                                 ("sites_covered", "sites_total",
                                  "distinct_covered", "distinct_total")}
        json.dump({"titles": titles}, open(CORPUS, "w"), indent=2)
        print(f"\nbaselines updated in {CORPUS}")

    if args.json:
        json.dump(results, open(args.json, "w"), indent=2)
        print(f"wrote {args.json}")

    if regressions:
        print(f"\nREGRESSION in {len(regressions)} title(s): {', '.join(regressions)}")
        print("The HAL dispatches fewer of their calls than it used to.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
