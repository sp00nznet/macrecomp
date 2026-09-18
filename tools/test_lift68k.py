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

    # ---- second batch: the arithmetic the stack block decoder leans on -------
    dict(
        name="andi.l masks a longword",
        code=b"\x70\xff" + b"\x02\x80\x00\x00\x00\x7f" + RTS,
        setup="",
        checks=[("M.d[0]", 0x7F)],
    ),
    dict(
        name="or.b combines only the low byte",
        code=b"\x70\xf0" + b"\x72\x0f" + b"\x80\x01" + RTS,
        setup="",
        # d0 = 0xFFFFFFF0, d1 = 0x0F: the byte becomes 0xFF, the rest is kept.
        checks=[("(uint8_t)M.d[0]", 0xFF), ("M.d[0]", 0xFFFFFFFF)],
    ),
    dict(
        name="asl.w #2 shifts only the low word",
        code=b"\x30\x3c\x12\x34" + b"\xe5\x40" + RTS,
        setup="M.d[0]=0xAAAA0000;",
        checks=[("(uint16_t)M.d[0]", 0x48D0), ("M.d[0] >> 16", 0xAAAA)],
    ),
    dict(
        name="indexed addressing sign-extends a word index",
        code=b"\x22\x30\x00\x04" + RTS,          # move.l $4(a0,d0.w),d1
        setup=("M.a[0]=0x1000; M.d[0]=0xFFFFFFF8;"     # index -8, so 0x1000+4-8
               " m68k_w32(0x0FFC,0x12345678);"),
        checks=[("M.d[1]", 0x12345678)],
    ),
    dict(
        name="not.l complements every bit",
        code=b"\x70\x0f" + b"\x46\x80" + RTS,
        setup="",
        checks=[("M.d[0]", 0xFFFFFFF0)],
    ),
    dict(
        name="neg.l negates and sets carry",
        code=b"\x70\x05" + b"\x44\x80" + RTS,
        setup="",
        checks=[("(int32_t)M.d[0]", -5), ("M.c", 1)],
    ),
    dict(
        name="mulu.w multiplies unsigned words into a longword",
        code=b"\x30\x3c\xff\xff" + b"\x32\x3c\x00\x02" + b"\xc0\xc1" + RTS,
        setup="",
        # 0xFFFF * 2 = 0x1FFFE, which needs the full 32-bit destination.
        checks=[("M.d[0]", 0x1FFFE)],
    ),
    dict(
        name="divu.w leaves quotient low and remainder high",
        code=b"\x20\x3c\x00\x00\x00\x11" + b"\x32\x3c\x00\x04" + b"\x80\xc1" + RTS,
        setup="",
        # 17 / 4 = 4 remainder 1.
        checks=[("(uint16_t)M.d[0]", 4), ("M.d[0] >> 16", 1)],
    ),
    dict(
        name="btst sets Z from the complement of the bit",
        code=b"\x70\x08" + b"\x08\x00\x00\x03" + RTS,   # d0 = 8, test bit 3
        setup="",
        # Bit 3 of 8 is set, so Z is clear.
        checks=[("M.z", 0)],
    ),
    dict(
        name="btst on a clear bit sets Z",
        code=b"\x70\x08" + b"\x08\x00\x00\x02" + RTS,
        setup="",
        checks=[("M.z", 1)],
    ),
    dict(
        name="lsl.l by a register count",
        code=b"\x70\x01" + b"\x72\x04" + b"\xe3\xa8" + RTS,  # d0=1, d1=4, lsl.l d1,d0
        setup="",
        checks=[("M.d[0]", 0x10)],
    ),
    dict(
        name="asr.l keeps the sign bit",
        code=b"\x70\xff" + b"\xe2\x80" + RTS,   # d0 = -1, asr.l #1
        setup="",
        checks=[("(int32_t)M.d[0]", -1)],
    ),

    # ---- third batch: register lists, addressing, memory read-modify-write ---
    dict(
        # movem to -(An) stores in *reverse* register order, and the matching
        # (An)+ restore reads forward. A prologue and its epilogue only agree if
        # both orders are right, so the round trip is the check that matters.
        name="movem round-trips through the stack",
        code=b"\x48\xe7\xc0\x80" + b"\x4c\xdf\x01\x03" + RTS,
        setup="M.d[0]=0x11111111; M.d[1]=0x22222222; M.a[0]=0x33333333;",
        checks=[("M.d[0]", 0x11111111), ("M.d[1]", 0x22222222),
                ("M.a[0]", 0x33333333)],
    ),
    dict(
        name="a movem push/pop pair is stack-neutral",
        code=b"\x48\xe7\xc0\x80" + b"\x4c\xdf\x01\x03" + RTS,
        setup="",
        # Three longwords down and back again, then rts pops the return
        # sentinel m68k_call pushed -- so SP lands exactly where it started.
        checks=[("SP", 0x8000)],
    ),
    dict(
        name="exg swaps two registers whole",
        code=b"\x70\x01" + b"\x72\x02" + b"\xc1\x41" + RTS,
        setup="",
        checks=[("M.d[0]", 2), ("M.d[1]", 1)],
    ),
    dict(
        name="lea computes an address without touching memory",
        code=b"\x41\xe8\x00\x10" + RTS,          # lea $10(a0),a0
        setup="M.a[0]=0x1000; m68k_w32(0x1010,0xDEADBEEF);",
        checks=[("M.a[0]", 0x1010), ("m68k_r32(0x1010)", 0xDEADBEEF)],
    ),
    dict(
        name="bset on memory sets the bit and reports the old one",
        code=b"\x08\xd0\x00\x05" + RTS,          # bset #5,(a0)
        setup="M.a[0]=0x1000; m68k_w8(0x1000,0x00);",
        # Z is set from the bit's previous value, which was clear.
        checks=[("m68k_r8(0x1000)", 0x20), ("M.z", 1)],
    ),
    dict(
        name="bset on an already-set bit clears Z",
        code=b"\x08\xd0\x00\x05" + RTS,
        setup="M.a[0]=0x1000; m68k_w8(0x1000,0x20);",
        checks=[("m68k_r8(0x1000)", 0x20), ("M.z", 0)],
    ),
    dict(
        name="seq writes a whole byte of condition to memory",
        code=b"\x70\x00" + b"\x57\xd0" + RTS,   # moveq #0 (sets Z) ; seq (a0)
        setup="M.a[0]=0x1000; m68k_w8(0x1000,0x00);",
        checks=[("m68k_r8(0x1000)", 0xFF)],
    ),

    dict(
        # A divisor fetched through (a7)+ must actually pop. Without the
        # postincrement the division is right and every later stack read is one
        # slot out -- which is how a hash lookup ended up adding its own divisor
        # instead of the table base and returning a wild pointer.
        name="divu.w (a0)+ pops its divisor",
        code=b"\x80\xd8" + RTS,          # divu.w (a0)+,d0
        setup=("M.a[0]=0x1000; m68k_w16(0x1000,0x0004);"
               " m68k_w16(0x1002,0xBEEF); M.d[0]=17;"),
        # 17/4 = 4 remainder 1, and A0 must have advanced past the divisor.
        checks=[("(uint16_t)M.d[0]", 4), ("M.d[0] >> 16", 1),
                ("M.a[0]", 0x1002)],
    ),
    dict(
        # A character-class table indexed off the program counter: the
        # tokeniser idiom. The displacement capstone reports is already
        # absolute within the segment, and the index is sign-extended.
        name="move.b d(pc,Xn) reads a table in the code",
        code=b"\x12\x3b\x00\x06" + RTS + b"\xaa\xbb\xcc\xdd",
        setup="M.d[0]=1;",
        # The instruction is 4 bytes, rts 2, so the table begins at offset 6 and
        # capstone already reports the base as the absolute 8. Index 1 lands on
        # the fourth table byte.
        checks=[("(uint8_t)M.d[1]", 0xDD)],
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
    /* The segment's own bytes have to be in guest memory as well as lifted:
     * PC-relative *data* reads -- constant tables, string literals -- go
     * through M.mem at g_seg_base+offset, and read zero without this. The real
     * loader has the same requirement. */
    {   static const unsigned char seg[] = { @@CODEBYTES@@ };
        for(size_t i = 0; i < sizeof seg; i++) M.mem[i] = seg[i]; }
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
        codebytes = ",".join(str(x) for x in case["code"])
        f.write(HARNESS.replace("@@SETUP@@", case["setup"])
                       .replace("@@CHECKS@@", checks)
                       .replace("@@CODEBYTES@@", codebytes))

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
