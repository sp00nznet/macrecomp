#!/usr/bin/env python3
"""Differential self-check for the lifter.  python tools/test_lift68k.py

Lifts short 68k byte sequences whose effect is known from the processor manual,
runs the generated C, and compares the machine state against what a 68000 would
have produced. Every bug this file covers was found the hard way -- by chasing a
symptom hundreds of thousands of instructions downstream of the instruction that
actually misbehaved:

  * `dbra` loops were emitted with an always-true condition, so every counted
    loop in the program did nothing at all;
  * `move.l (a7)+,(a7)` and `movea.l (a7)+,a7` had their operand order wrong, so
    the Pascal epilogue silently failed and the stack pointer drifted;
  * `cmpm` was missing, so byte-by-byte string comparison always looked equal.

None of those announces itself. A wrong answer here costs minutes; the same
wrong answer found by bisecting a title costs a day, so the cases are cheap at
almost any number.

Needs a C compiler. Without one the check reports SKIP rather than passing
quietly, because a test that silently does nothing is worse than no test.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LIFT = os.path.join(HERE, "lift68k.py")

# Each case: 68k bytes, setup C, and the assertions to make afterwards.
# `rts` (0x4E75) ends every snippet so the lifted function returns.
RTS = b"\x4e\x75"

CASES = [
    dict(
        name="dbra runs the body exactly d0+1 times",
        # moveq #3,d0 ; addq.l #1,d1 ; dbra d0,-6 ; rts
        code=b"\x70\x03" + b"\x52\x81" + b"\x51\xc8\xff\xfc" + RTS,
        setup="M.d[1]=0;",
        # dbra: body runs until the counter reaches -1, so 4 times for d0=3.
        checks=[("(int32_t)M.d[1]", 4), ("(int16_t)M.d[0]", -1)],
    ),
    dict(
        # The Pascal epilogue is `move.l (a7)+,(a7)`, which lifts the return
        # address over the parameter; A0 is used here for the same reason as
        # above. The source is fetched and the register incremented *before* the
        # destination address is worked out, so the value lands four bytes on.
        name="move.l (a0)+,(a0) lifts a longword over the next one",
        code=b"\x20\x98" + RTS,                      # move.l (a0)+,(a0)
        setup=("M.a[0]=0x1000; m68k_w32(0x1000,0xAAAAAAAA);"
               " m68k_w32(0x1004,0xBBBBBBBB);"),
        checks=[("m68k_r32(0x1004)", 0xAAAAAAAA), ("M.a[0]", 0x1004)],
    ),
    dict(
        # The instance that mattered was `movea.l (a7)+,a7` in the variadic
        # glue, but A7 cannot be set up from here -- m68k_call pushes its return
        # sentinel first. A0 exercises the same rule: the postincrement belongs
        # to the source fetch, so the register ends as the value read and not
        # that value plus four.
        name="movea.l (a0)+,a0 ends as the value read, not value+4",
        code=b"\x20\x58" + RTS,                      # movea.l (a0)+,a0
        setup="M.a[0]=0x1000; m68k_w32(0x1000,0x00003000);",
        checks=[("M.a[0]", 0x3000)],
    ),
    dict(
        name="cmpm.b compares and advances both pointers",
        code=b"\xb3\x08" + RTS,                      # cmpm.b (a0)+,(a1)+
        setup=("M.a[0]=0x1000; M.a[1]=0x2000;"
               " m68k_w8(0x1000,0x41); m68k_w8(0x2000,0x41);"),
        checks=[("M.z", 1), ("M.a[0]", 0x1001), ("M.a[1]", 0x2001)],
    ),
    dict(
        name="cmpm.b reports inequality",
        code=b"\xb3\x08" + RTS,
        setup=("M.a[0]=0x1000; M.a[1]=0x2000;"
               " m68k_w8(0x1000,0x41); m68k_w8(0x2000,0x42);"),
        checks=[("M.z", 0)],
    ),
    dict(
        name="cmp sets flags from destination minus source",
        code=b"\x70\x05" + b"\x72\x03" + b"\xb2\x80" + RTS,   # moveq 5,d0; moveq 3,d1; cmp.l d0,d1
        setup="",
        # 3 - 5 is negative: N set, Z clear.
        checks=[("M.n", 1), ("M.z", 0)],
    ),
    dict(
        name="addq to an address register does not touch the flags",
        code=b"\x70\x00" + b"\x52\x88" + RTS,        # moveq #0,d0 (sets Z) ; addq.l #1,a0
        setup="M.a[0]=0x40;",
        # moveq #0 sets Z; addq on An must leave it alone.
        checks=[("M.a[0]", 0x41), ("M.z", 1)],
    ),
    dict(
        name="rol.b by 8 returns the value and carries bit 0",
        code=b"\x70\x81" + b"\xe1\x18" + RTS,        # moveq #-127,d0 ; rol.b #8,d0
        setup="",
        checks=[("(uint8_t)M.d[0]", 0x81), ("M.c", 1)],
    ),
    dict(
        name="lsr.l #8 shifts a longword",
        code=b"\x20\x3c\x12\x34\x56\x78" + b"\xe0\x88" + RTS,  # move.l #$12345678,d0; lsr.l #8,d0
        setup="",
        checks=[("M.d[0]", 0x00123456)],
    ),
    dict(
        name="ext.l sign-extends a word",
        code=b"\x30\x3c\xff\xfe" + b"\x48\xc0" + RTS,          # move.w #-2,d0 ; ext.l d0
        setup="",
        checks=[("(int32_t)M.d[0]", -2)],
    ),
    dict(
        # moveq sign-extends its 8-bit immediate. `moveq #-1,dN` is how a
        # routine spells "not found"; read as unsigned it becomes 255 and every
        # caller that compares against -1 silently misses.
        name="moveq #-1 sign-extends to a full -1",
        code=b"\x70\xff" + RTS,                      # moveq #$ff,d0
        setup="",
        checks=[("(int32_t)M.d[0]", -1)],
    ),
    dict(
        name="moveq #127 stays positive",
        code=b"\x70\x7f" + RTS,                      # moveq #$7f,d0
        setup="",
        checks=[("(int32_t)M.d[0]", 127)],
    ),
    dict(
        name="swap exchanges the halves of a longword",
        code=b"\x20\x3c\x12\x34\x56\x78" + b"\x48\x40" + RTS,
        setup="",
        checks=[("M.d[0]", 0x56781234)],
    ),
]

HARNESS = r"""
#include "macrecomp/m68k.h"
#include <stdio.h>
#include <stdlib.h>
void register_seg_1(uint32_t);
void m68k_trap(uint16_t w){ (void)w; }
int plat_stub;
static int fails;
static void check(const char *what, long got, long want){
    if(got != want){ fails++;
        fprintf(stderr, "  FAIL %s: got %ld (0x%lx), want %ld (0x%lx)\n",
                what, got, (unsigned long)got, want, (unsigned long)want); }
}
int main(void){
    M.memsize = 0x10000; M.mem = calloc(1, M.memsize);
    SP = 0x8000;
    register_seg_1(0);
    @@SETUP@@
    m68k_call(0);
@@CHECKS@@
    if(fails){ fprintf(stderr, "  (%d failed)\n", fails); return 1; }
    return 0;
}
"""


def find_cc():
    for cc in ("gcc", "cc", "clang"):
        p = shutil.which(cc)
        if p:
            return p
    for p in (r"C:\msys64\mingw64\bin\gcc.exe", r"C:\mingw64\bin\gcc.exe"):
        if os.path.isfile(p):
            # A toolchain found by absolute path still needs its own bin
            # directory on PATH to load its DLLs. Without it gcc exits 1 and
            # prints nothing at all, which reads as a compile error in the code
            # under test rather than a broken invocation.
            os.environ["PATH"] = os.path.dirname(p) + os.pathsep + os.environ.get("PATH", "")
            return p
    return None


def run_case(cc, tmp, case):
    seg = os.path.join(tmp, "CODE_1.bin")
    # A CODE resource starts with a 4-byte header the lifter skips.
    with open(seg, "wb") as f:
        f.write(b"\0\0\0\0" + case["code"])
    jt = os.path.join(tmp, "jt.json")
    with open(jt, "w") as f:
        f.write('{"entries":[{"idx":0,"offset":0,"segment":1,"thunk":true}]}')

    gen = os.path.join(tmp, "gen")
    r = subprocess.run([sys.executable, LIFT, seg, "--seg", "1", "--jt", jt, "-o", gen],
                       capture_output=True, text=True)
    if r.returncode:
        return "lifter failed: " + (r.stderr.strip().splitlines() or [""])[-1]

    checks = "\n".join('    check("%s", (long)(%s), (long)%d);' % (c[0].replace('"', ''), c[0], c[1])
                       for c in case["checks"])
    main_c = os.path.join(tmp, "main.c")
    with open(main_c, "w") as f:
        f.write(HARNESS.replace("@@SETUP@@", case["setup"]).replace("@@CHECKS@@", checks))

    exe = os.path.join(tmp, "t.exe")
    r = subprocess.run([cc, "-I", os.path.join(ROOT, "include"), "-O1", "-o", exe,
                        main_c, os.path.join(gen, "code_1.c"),
                        os.path.join(ROOT, "runtime", "m68k.c"),
                        "-DMACRECOMP_NO_TRAP_STUB"],
                       capture_output=True, text=True)
    if r.returncode:
        return "compile failed: " + (r.stderr.strip()[:400] or ("rc=%d, no output" % r.returncode))

    r = subprocess.run([exe], capture_output=True, text=True)
    return None if r.returncode == 0 else r.stderr.strip()


def main():
    cc = find_cc()
    if not cc:
        print("lift68k self-check SKIP: no C compiler found")
        return 0
    bad = 0
    for case in CASES:
        with tempfile.TemporaryDirectory() as tmp:
            err = run_case(cc, tmp, case)
        if err:
            bad += 1
            print("FAIL %s" % case["name"])
            print(err)
        else:
            print("ok   %s" % case["name"])
    if bad:
        print("\n%d of %d lifter checks failed" % (bad, len(CASES)))
        return 1
    print("\nlift68k self-check OK (%d cases)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
