#!/usr/bin/env python3
"""Self-check for the trap scanner's coverage mode.  python tools/test_scan_traps.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scan_traps import (  # noqa: E402
    implemented_traps, looks_like_data, manager, trap_name,
)


def test_manager_table():
    # Exact names beat substrings: NewPtrClear contains "Pt" but is Memory Mgr,
    # AppendResMenu contains "Res" but is Menu Mgr.
    cases = {
        "NewPtrClear": "Memory Mgr",
        "AppendResMenu": "Menu Mgr",
        "GetMenuHandle": "Menu Mgr",
        "GetDialogItem": "Dialog Mgr",
        "HideDialogItem": "Dialog Mgr",
        "SetControlValue": "Control Mgr",
        "TESetSelect": "TextEdit",
        "OffsetRect": "QuickDraw",
        "CopyBits": "QuickDraw",
        "FP68K": "SANE (float)",
        "FixRatio": "SANE (float)",
        "GetTrapAddress": "Trap Mgr",
        "UnLoadSeg": "Segment Ldr",
        "SndDoCommand": "Sound Mgr",
        "BringToFront": "Window Mgr",
        "Open": "File Mgr",
        "MoveHHi": "Memory Mgr",
        "MoveWindow": "Window Mgr",
    }
    for name, want in cases.items():
        got = manager(name)
        assert got == want, f"{name}: got {got!r}, want {want!r}"
    assert manager(None) is None
    assert manager("NotARealTrap") is None


def test_trap_names():
    assert trap_name(0xA873) == "SetPort"
    # $A800-$ABFF is one flat 10-bit Toolbox block, so neighbours are distinct
    # traps, not flagged aliases -- and a word outside the table is not a trap.
    assert trap_name(0xA973) != "SetPort"
    assert trap_name(0xAAAB) is None


def test_looks_like_data():
    from collections import Counter
    # Real code reuses a few traps heavily.
    real = Counter({0xA873: 400, 0xA8A1: 200, 0xA89B: 200, 0xA9F4: 100})
    assert looks_like_data(real) is None
    # Encrypted bytes disassemble to near-unique words.
    noise = Counter({0xA000 + i: 1 for i in range(300)})
    assert looks_like_data(noise) is not None
    # Too small a sample to judge either way.
    assert looks_like_data(Counter({0xA873: 3})) is None


def test_implemented_traps():
    src = """
        switch (w) {
        case 0xA873: /*SetPort*/ break;
        case 0xa874: case 0xA89B: break;
        }
        uint32_t not_a_case = 0xA999;
    """
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        got = implemented_traps(path)
        assert got == {0xA873, 0xA874, 0xA89B}, f"got {[hex(x) for x in sorted(got)]}"
    finally:
        os.unlink(path)


if __name__ == "__main__":
    test_manager_table()
    test_trap_names()
    test_looks_like_data()
    test_implemented_traps()
    print("scan_traps self-check OK")
