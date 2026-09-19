# Roadmap

What's next, what's deferred, what's out of scope. Ordered by measured need, not
by guess — the numbers come from `scan_traps.py --coverage`.

## Where the HAL stands

| Title | 68k | CODE segs | Distinct traps | Call sites | Sites covered | Traps covered |
|---|---|---|---|---|---|---|
| Shufflepuck Cafe (1988) | ~53 KB | 6 | 182 | 995 | **92%** | 69% |
| HyperCard 1.2.2 (1988) | 326 KB | 22 | 418 | 3166 | **83%** | 60% |

Reproduce either row:

```bash
python tools/scan_traps.py work/unpacked --coverage runtime/toolbox.c
```

Shufflepuck's remaining traps are cold paths. HyperCard is the real target: 6×
the code, and the binary that makes every HyperCard stack a recomp target at
once. Regions, the Dialog Manager and the Control Manager landed together and
moved it from 53% to 76% of call sites.

## Where HyperCard actually stops

It boots, runs the full Toolbox init sequence, loads its resources, and executes
~305 Toolbox calls. All 21 segments lift at 97-100% instruction coverage, and a
whole run now hits only **2** unimplemented instructions.

It does not currently render. It reaches the same depth it did when it was
drawing an error dialog, but no longer takes the error path -- so nothing is
drawn at all. The dialog was the only thing it ever displayed.

### Entry-point dispatch (landed)

The blocker was an **indirect jump to a mid-function address**.
`m68k_jump: no function at 5210bc` repeated forever: segment 16 offset `0x10bc`,
sitting *inside* `fn_16_0fea`. Nothing in the generated code jumps there
literally, so it is a register-indirect `jmp` whose target is computed at run
time, and a table of *function starts* cannot enter a function partway.

Two things had to change, and the second is the one that mattered:

- **The runtime** keeps a second, range-based view of the function table.
  `m68k_register` now takes `[start, end)`, and a lookup that misses the hash
  falls back to a binary search for the function whose body covers the address.
- **The lifter** labels **every instruction**, not just branch targets, and
  gives each function a prologue that switches on an entry address and `goto`s
  the matching label. Labelling only branch targets would not have worked here:
  `0x10bc` is not a branch target anywhere in the binary, which is exactly why
  the plain disassembly never revealed it.

`0x10bc` now lifts to `case 0x10bcu: goto L10bc;` in `fn_16_0fea`, so the
transfer resolves. All 21 segments still lift and compile.

### The run, measured

A title tree was built locally to answer this (in `work/`, never committed) and
HyperCard was run headless. **Entry dispatch holds: not one mid-function jump
fails in a whole run.** The `no function at 5210bc` loop is gone.

It now reaches **306 Toolbox calls** and stops in a hard loop -- the same count
on every run, so it is a loop and not slow progress. The last calls name the
cause exactly:

```
[278] DrawChar   [279] TextFont  [280] SetPBits  [281] SetPort
[283] TextSize   [284] TextFace  [285] TextMode  [286] SetRect
[287] TENew      <- unimplemented
[289] GetDeviceList  [291] GetNextDevice  [295] ScriptUtil   <- unimplemented
[297..300] NewEmptyHandle x4   [301..304] NewHandle x4   <- then it spins
```

HyperCard is setting up a text field, asks `TENew` for a `TEHandle`, gets
nothing back, and loops allocating.

### TextEdit (landed) -- and the loop is not it

`runtime/textedit.c` implements all 22 TextEdit traps over HyperCard's 81 call
sites, and coverage went 77% -> 79% of call sites (the conformance harness
reported it as a `GAIN`, which is what that check is for). The TERec lives in
**guest memory**: a TEHandle dereferences to it and the guest reads `teLength`,
`selStart`/`selEnd`, `hText`, `nLines` and `lineStarts` directly, so a host-side
mirror would only be a second copy to keep in sync.

It did not clear the hang: the run still stopped at exactly 306 calls, so
`TENew` returning nothing had been a coincidence of ordering rather than the
cause.

### The hang: a Ticks busy-wait with no clock -- fixed

The loop made **no traps, no calls and no tail jumps**, so nothing the runtime
normally watches could see it. What every loop does do is go round a **backward
branch**, so the lifter now emits a hook there; built with
`-DMACRECOMP_LOOPGUARD` and run with `MRMAXLOOPS`, it counts branches by address
and prints the hottest. One address took 19,999,889 of 20,000,000 ticks, and
disassembling there ended the search:

```
0fdc  movea.l #$16a, a4     ; a4 = the low-memory Ticks global
0fe2  move.l  (a4), d7
0fe4  cmp.l   (a4), d7
0fe6  beq.b   $fe4          ; spin until Ticks changes
0fea  addq.l  #$1, d6       ; then count iterations per tick
0ff2  lsr.l   #$2, d0       ; ...and derive a machine-speed rating
```

HyperCard calibrates machine speed by busy-waiting on `Ticks` (0x16A). The
runtime advanced Ticks only from `m68k_call`, and this loop makes no calls, so
the compare was always equal and the wait could only be infinite. A real Mac
advanced Ticks from the VBL interrupt; there is no interrupt here.

**The fix is the same hook.** A backward branch now decrements a counter inline
and, every 2048 of them, advances Ticks -- so time passes inside a loop that
does nothing else. An ordinary loop pays an add and a branch. This is not
specific to HyperCard: every classic-Mac timing busy-wait needs it, and the
same shape would have hung any of them.

### Result: HyperCard renders

![HyperCard drawing a modal dialog](img/hypercard-first-render.png)

306 -> **372 Toolbox calls**, and the framebuffer is no longer blank. The tail
of the trace is `InsetRect`, `FrameRoundRect`, `PenNormal`, `SetPort`,
`ModalDialog` -- HyperCard drawing its own modal dialog frame, drop shadow and
all, through the QuickDraw HAL.

The box is empty because the title asks for `DLOG 0` and its resource fork has
no such resource, so the Dialog Manager builds an empty one. That is an early
exit path, not the Home stack: `FSDispatch` and SANE are still stubs, so it
cannot open a stack yet. The run then sits in `ModalDialog` waiting for an
event that a headless harness never sends, which is correct behaviour rather
than a hang.

### Computed-jump entry points (`tools/find_entries.py`)

A `jmp d(pc,Dn.w)` -- `0x4EFB` -- is a switch: an extension word, a table of
16-bit offsets, then the arms. **Nothing in the binary names those arms**, so a
linear decode only lands on one by luck, and a target it missed becomes an
`m68k_entry_miss` at run time: the function returns without doing anything, in
silence. One such address in `CODE 12` was the entire reason HyperTalk did not
execute.

`find_entries.py` reads every table, checks each target against the generated
code, and prints the ones with neither a `case` label nor a registered function
start -- exactly what to hand back as `--entry`. Re-run it after each lift
until it reports nothing: fixing one set shifts the boundaries and can expose
another. For this binary it converges in two rounds at **30 addresses**:

| segment | `--entry` |
|---|---|
| CODE 1 | `0x2200,0x2240,0x22d6,0x2416,0x241c,0x3ff0,0x4000,0x4038,0x403c` |
| CODE 4 | `0x4a2,0x4ac,0x4c0,0x4d2,0x4dc,0x4f6,0x50e` |
| CODE 6 | `0x4b12` |
| CODE 9 | `0x1bc0,0x2720` |
| CODE 10 | `0x1360,0x17ec,0x35f6,0x4de2` |
| CODE 12 | `0x1322,0x263a,0x2f5c` |
| CODE 13 | `0xc32,0x446c,0x45b6,0x47a0,0x4c14` |
| CODE 16 | `0x2ca6,0x4bc8` |
| CODE 21 | `0x1d4,0x3f18` |

Dropped deliberately: an entry that points backwards, outside the segment, or
at an odd address means the table has ended and the arms have begun.

### Instruments kept

`MRMAXCALLS=<n>` stops after n transfers and prints the shadow stack; `MRSTACK=1`
prints the whole shadow stack at every trap; `MRMAXLOOPS=<n>` (with
`-DMACRECOMP_LOOPGUARD`) prints the hottest backward branches; and each
`MRTRACE` line carries the calling function and the call depth. `gdb` in the
MSYS2 toolchain here is broken -- missing `libxxhash.dll` -- so a native
backtrace was never available, and these stand in for it.

### Where HyperCard actually is now

**It opens and reads its Home stack.** From a cold start it walks the disc's
catalogue by index, finds `Home` among the 279 files, opens the data fork,
opens the resource fork, reads the `STAK` block (6144 bytes, in two reads) and
the `MAST` index (512 bytes) -- and the bytes land in guest memory byte-exact,
checked against the file.

Then it asks its block cache for **block id 0**, which no stack contains, and
reports `Unexpected error 1250` in a dialog of its own with its own text. The
zero is the whole question: it read a block id of zero out of the stack header
where a real id belongs. `fn_20_04d6` calls `fn_20_0b06` to fetch the block,
gets a record whose id field is zero, and raises 1250.

Ruled out, by measurement rather than assumption:

- The File Manager. Reads are byte-exact in guest memory; `STAK` and `MAST`
  headers were compared against the file.
- Unimplemented instructions. A whole run executes none.
- Missing traps at the point of failure. Five remain unimplemented anywhere in a
  run -- `GetDeviceList`, `GetNextDevice`, `GetCursor`, `SetStdProcs`,
  `ScriptUtil` -- and none is called near it.
- Frame and stack corruption. `MRWATCH` reports no A6 clobber and no stack
  pointer outside the address space.

So the next thread is another silent mis-lift or a HAL routine returning the
wrong shape, somewhere between reading the `STAK` header and reading a block id
out of it. The instruments to use are already here: `MRSTACK` for the frame
chain, and a probe in the generated function once the suspect is named.

### Where it stops: one unhandled type tag (was: the HyperTalk parser)

**The parser failure is fixed, and it was a decode boundary, not the parser.**
`CODE 12` is entered at `0x1322` through a *computed* jump -- no instruction in
the binary names that address, so the linear decode never put a boundary there
and ran past it. `fn_12_12ee` hit `m68k_entry_miss` and returned without its
epilogue, leaving A6 pointing into its own frame; its caller is the parser
(`seg14+0x1d26` -> `fn_14_298e`), so the `Can't understand what's after "if"`
report was a corrupted frame, not a parse.

Lifting `CODE 12` with `--entry 0x1322` closes it:

| | before | after |
|---|---|---|
| `returned with A6` failures | 9 | **0** |
| `no function at` / entry-miss | 5 | **0** |
| HyperTalk executes | no | **yes** |

`hide menuBar` from the catalogue's `on openStack` now reaches low memory
0x0BAA, confirmed by watchpoint -- the first HyperTalk statement this recomp
has ever run. The flag is per-title and passed by hand: a computed entry is
invisible to static analysis, which is what `--entry` exists for.

**The assertion that follows was self-inflicted.** Replacing `Home` with the
catalogue's Table of Contents stack -- the trick used to get the catalogue on
screen at all -- is what caused it. HyperCard sets a byte tag at `a5-0x49aa`
during start-up; with the catalogue standing in for Home the tag ends at 0,
`fn_9_1670` dispatches on 1..4 only, and the fall-through raises
`0x421BEBE` -- *"Unexpected error 69320382"*, `ALRT 3003` -- and calls
`ExitToShell`.

**With the real Home stack in place HyperCard runs clean.** No error, no quit:

| | catalogue as Home | real Home |
|---|---|---|
| traps in a 100 s run | 818 | **931,482** |
| `GetNextEvent` | 0 | **5,611** |
| error dialog | `ALRT 3003`, then quit | none |

So the catalogue must be *navigated to*, not substituted for Home. That is what
the Menu Manager work below is for.

Two dead ends, recorded so they are not retried: hiding Home (`MRSKIP=Home`)
does not let a catalogue stack be opened directly, because HyperCard errors on
Home itself; and emptying Home's script gets *fewer* calls, not more, most
likely because the `STAK` block carries a checksum at +0x0C.

### Clicks reach the right button

A synthesised click was never acted on, and the reason was not delivery. A
title tracks a press with `Button()` and `GetMouse()`; both reported the real
cursor, so HyperCard read every synthetic click as "moved away before the
release" and cancelled it. `MRCLICK` now pins the reported mouse position and
holds the button down, ageing the press on *every* route the guest can observe
the mouse -- a title that is tracking stops polling `GetNextEvent`, so a
release timed off the event loop alone never arrives. The click counter also
moved into the Event Manager shim, because the modal-dialog loop polls
`plat_next_event` directly and was eating the click before HyperCard's own
event loop existed.

Measured, with `MRHIT=1`: a click at 93,96 answers `PtInRect` true against
`81,2,112,185` -- the catalogue's first Table of Contents button, whose script
is `on mouseUp / visual effect barn door open / go to stack "WHOLE SYSTEMS" /
end mouseUp`. The click reaches the right control; what it cannot yet do is run
the handler.

### The menu bar was inert, and now reaches Standard File

`MenuSelect`, `MenuKey`, `GetMHandle`, `GetItem` and a real `CountMItems` were
all missing -- the first two also leaked their arguments. A title whose only
route to a document is *File > Open* therefore had no route at all, and
HyperCard 1.x turns a menu choice into a HyperTalk `doMenu "<item text>"`, so
it needs the item's **text**, not its number. `GetItem` and `CountMItems` read
that straight out of the `MENU` resource the Resource Manager shim already
serves; without `CountMItems` the name search never matched and HyperCard
reported `Can't find menu item "Open Stack..."` (`STR# 1002` string 13).

`MRMENU=<menuID>,<item>` makes the choice once, since there is no menu to pull
down. With `MRCLICK` putting a click in the menu bar, the chain now runs end to
end:

    MRCLICK=40,8,300 MRMENU=10,2 MRDOC="WHOLE EARTH"

    [menu] choosing menu 10 item 2
    [File] StandardFile selector 2 reply=3ef5f8 -> WHOLE EARTH [
     01 00 53 54 41 4b ff ff 00 00 0b 57 48 4f 4c 45 ]

-- a click in the menu bar, resolved to `doMenu "Open Stack..."`, executed, and
landing in Standard File, which `MRDOC` answers with a well-formed `SFReply`:
`good`=1, type `STAK`, vRefNum -1 (which is what this File Manager reports),
name `WHOLE EARTH`.

**Where it stops now: `go to stack` never opens the file.**

The click works. So does the script. An earlier version of this section said
the handler never ran -- that was wrong, and the giveaway was in the message
counts all along:

| message | dispatches with a click |
|---|---|
| `mouseDown` | **4** -- propagating up button, card, background, stack, unhandled |
| `mouseUp` | **1** -- *handled at the button, never passed on* |
| `go` | 1 without a click, **5** with one |

A message that stops after one dispatch is a message that found a handler.
`on mouseUp` in the Whole Earth button runs, and it executes its `go to stack
"Whole Earth"`: the navigation executor `fn_21_0fcc` goes from 4 entries to 6.

And then nothing reaches the disc. Not one File Manager call follows the
click -- no `Open`, no `GetFileInfo`, no `PBGetCatInfo`, no Standard File.
HyperCard carries on idling quite happily (1.2 million traps after the click),
so it has not crashed or hung; the `go` simply resolves to nothing and gives
up without asking the file system anything.

**The `go` command itself is `fn_12_11c2`, in CODE 12.** It is reached
through `jt 0x207a`, whose only caller is in that segment; none of the five
direct callers of `fn_21_0fcc` moves with a click, which is why this took a
while to find. It builds a destination record in `-$5c(a6)`, classifies the
destination with `jt 0x8f2`, and dispatches:

| `jt 0x8f2` | arm | what it does |
|---|---|---|
| 1 | `0x11e8` | kind 1; name into `-$15c(a6)` via **`jt 0x146a`**, then `jt 0x1fb2` |
| 2 | `0x120c` | kind 1; `jt 0x144a` into `-$56(a6)` |
| 3 | `0x121e` | kind 2; `jt 0x144a` into `-$52(a6)` |
| 4 | `0x1232` | `jt 0x1452` over the whole record |
| else | `0x1242` | straight to the navigator with the record as-is |

**The destination record carries no stack name, and this time the evidence is
behavioural.** `fn_21_04a6` opens the destination stack at `0x055c`
(`jsr $1e1e(pc)`), and it only gets there when `d4` is non-zero at `0x0520`.
`d4` is the inverse of `fn_21_3978`, "is this the stack we are already in".
Measured: **`fn_21_1e1e` is entered twice a run with a click and twice
without** -- the stack open is never reached. So `d4` is zero, so
`fn_21_3978` answered *yes, same stack*.

It cannot have answered that by comparing names. `a5-0x9fe`, the name it
compares against, reads `Home`; the destination is `Whole Earth`; the
compare would fail. The only other way out with the default answer of 1 is
the first test in the function:

    399a  move.b -$100(a6), d0    ; the destination name's length byte
    399e  tst.w  d0
    39a0  beq.w  $3ab6            ; length 0 -> return "same stack"

So the name length is zero. The record reaching the navigator has no stack
name in it, which is why the stack is never opened, no file is touched and
nothing is reported: HyperCard was asked to go to a stack with no name, and
concluded it was already there.

(An earlier note here said this, then retracted it on the grounds that the
dump address might not have been the live record. The retraction was wrong.
The call count for `fn_21_1e1e` settles it without depending on any dump.)

**How the record is built, for whoever picks this up.** `fn_12_11c2`'s arm 4
calls `jt 0x1452` = `fn_14_1620` as `(record, 0x5c, 2)`. That function pops
HyperCard's parse stack, which is three parallel arrays indexed by a position
held in `a5-0x4684`:

| array | holds |
|---|---|
| `a5-0x4704` | the element's **type byte** (what `fn_9_01a4` = `jt 0x8f2` reads back) |
| `a5-0x4904` | a **length**, as a longword |
| `a5-0x4984` | a second byte, set to `0x19` on pop |

The data itself lives in a pool. `fn_14_1620` subtracts the popped length from
the offset in `a5-0x4988`, forms `*(a5-0x4990) + (a5-0x4988)`, and copies from
there into the caller's record. At the `go` command the top element's length
reads `0x5c` -- 92 bytes, exactly the record size -- so the parser builds the
whole destination descriptor on that pool and this pops it off.

**The descriptor parser is `fn_12_12ee`**, the same function whose missing
epilogue `--entry 0x1322` fixed. It classifies with `jt 0x8f2` and dispatches
through a PC-relative table at `0x1316`, targets at `0x1314 + entry`:

    type 1 -> 1322   type 2 -> 1332   type 3 -> 139e
    type 4 -> 140e   type 5 -> 1450   type 6 -> 1460

All six are decoded instruction boundaries with `case` labels in the generated
switch -- checked, because `0x1322` needing a hand-fed entry made the rest
suspect. They are fine, so the dispatch is not the fault.

So the empty stack name is written by whatever parses `stack "Whole Earth"`
into that 92-byte descriptor, and that is where to look next. The pool is
reused between parses, so it has to be read at the right moment rather than
sampled at a fixed address -- the mistake this file has already recorded
twice.

**Where the `go` gives up, named.** `fn_21_0fcc` reaches its general arm at
`0x10e2` and calls `fn_21_04a6` -- the destination resolver -- at `0x1102`;
a false answer there branches to `0x1268` and the `go` ends quietly, which is
exactly what is observed. Inside `fn_21_04a6` the destination kind byte
(`-$5c(a6)`, copied from the record) dispatches at `0x07a2`: 0, 1, **2**, 3, 4
to `0x7c4`, `0x7e2`, `0x83a`, `0x936`, `0x9ee`. Kind 2 is the stack arm, and
`0x077c` is what sets the kind to 2. The stack arm opens with
`move.b d4,d0; bne.w $a88` -- d4 being the inverse of `fn_21_3978`, which
compares the destination name against `a5-0x9fe` and answers "is this the
stack we are already in". So the chain to read next is
`fn_21_04a6 + 0x83a` onwards, with `MRBRK=6504a6`.

**The catalogue opens and draws.** Clicking the Whole Earth button on Home's
first card runs its `on mouseUp`, `go to stack "Whole Earth"` reaches the disc,
and the card is rendered:

    [File] Open 'Whole Earth' -> Whole Earth
    [File] OpenRF 'Whole Earth' -> refNum 20 (41445 bytes)
    19 block reads, including one of 7,456 bytes

Two HAL faults were behind it, both general rather than HyperCard-specific:

1. **An OS trap must leave the condition codes set from D0.** HyperCard's
   string-table insert is `a024 _SetHandleSize` then `660c bne.b` past the
   append; with stale flags that branch was a coin toss, and a failed insert
   left the table grown but empty. Stack names are interned into two such
   tables and the destination descriptor carries the index pair, so the `go`
   got a null reference and `fn_21_3978` concluded "already in this stack".
2. **The Window Manager's low-level half was missing** -- `CalcVis`,
   `CalcVBehind`, `ClipAbove`, `PaintOne`, `PaintBehind`, `SaveOld`, `DrawNew`
   ($A909-$A90F). Falling through to the unimplemented-trap log meant their
   arguments were never popped, four to eight bytes of rubbish per call, at
   exactly the moment a new stack's window appears. With them in place the
   block reads went from 13 to 19 and the card artwork reached the screen.

**What is still wrong, in the order it matters:**

- **The card stops at row 53 of 342, and the mechanism is now known.** The
  blit source (`a5-0x1318` = 0x84932c) holds 7,707 lit pixels across rows 0-52
  and nothing below. Both of the catalogue's `BMAP` blocks are read in full --
  `req=14368 got=14368` and `req=7456 got=7456`, matching their block sizes
  exactly -- so the data is all there and the decoder is what stops.

  `fn_21_59e2` is the WOBA driver. Its row loop ends at `0x5d50`:
  `moveq #$40,d0` (rowBytes 64, correct), advance the destination, `row++`,
  loop while `row <= -$184(a6)`. Each row begins at `0x5afe` with

      move.b -$181(a6), d0
      bne.w  $5d0c              ; flag set -> skip this row entirely

  and `-$181(a6)` is cleared **once, before the loop** (`0x5aea`) and set to 1
  at `0x5c9a` and `0x5cd2` -- never cleared inside it. So the first row that
  sets it silences every row after.

  It is set right after `jt 0x1b2a`, which both sites call first and which
  looks like the decoder's abort. The second site reaches it from an explicit
  consistency check at `0x5cc4`: decode a row, then
  `move.l -$26(a6),d0; sub.l a4,d0; cmp.l -$12(a6),d0; beq $5cda` -- the bytes
  produced must equal the expected row length, or the decode is abandoned.

  The row decoder itself is `jt 0x1cc2` = `fn_18_1e00`. Its loop is

      1e18  move.b (a0)+, d0            ; opcode
      1e1a  bmi.w  $1ed6                ; >= 0x80
      1e1e  move.b $1e2a(pc, d0.w), d1  ; byte table -> copy count
      1e22  and.w  d2, d0               ; d0 &= 0x0f
      1e24  adda.w d0, a1               ; skip that many
      1e26  jmp    $1e2a(pc, d1.w)      ; into the unrolled copy chain
      ...
      1eb8  cmpa.l a2, a1               ; a2 = row start + row width
      1eba  bcs.w  $1e18                ; a1 < a2 -> next opcode

  so the row ends when `a1` reaches `a2`, and overshooting it is exactly the
  mismatch the caller rejects.

  **Checked and correct, so none of this needs redoing:** the eight targets of
  the byte table at `0x1e2a` are all decoded boundaries (note it is a *byte*
  table -- `find_entries.py` only reads 16-bit ones, so it cannot see this
  shape); `cmpa.l a2,a1` lifts to `fl_cmp(a2, a1, a1-a2, 4)`, the right operand
  order, and `fl_sub`'s carry is the standard borrow, so `bcs` means `a1 < a2`
  as it should; `lsl.b #3, d0` on the `>= 0xe0` arm masks to a byte
  (`m68k_lsl` ANDs with the size mask) and `SET_DB` leaves the upper bits
  alone, which `moveq #0,d0` had cleared -- so `adda.w d0,a1` advances by the
  right amount; and the copy primitives' Duff's-device jump (`4efb 1002` at
  `seg17+0x10aa`) lands on decoded boundaries too.

  **The within-row opcode set, decoded from the table at `seg18+0x1e2a`.**
  That table is 128 bytes of `0x8e,0x8c,...,0x80` in blocks of sixteen, and
  the jump target is `0x1e2a + table[op]` into a chain of `move.b (a0)+,(a1)+`
  ending at `0x1eb8`, so the count is `(0x8e - table[op]) / 2`. It works out
  exactly as:

  | opcode | meaning |
  |---|---|
  | `0x00-0x7f` | skip `op & 0x0f` bytes, then copy `op >> 4` |
  | `0x80-0xbf` | end of row (`bra $1ebe`, return) |
  | `0xc0-0xdf` | copy `op & 0x1f` bytes |
  | `0xe0-0xff` | skip `(op & 0x1f) * 16` -- `lsl.b #3` then `adda.w d0,a1` **twice** |

  A reference decoder built from this gets 342 rows out of `BMAP 4202` but
  cannot be compared against the guest yet, because the row-to-row half --
  XOR against the previous row, and the repeat counter at `-$152(a6)` -- lives
  in `fn_21_59e2` and is not modelled. Finishing that model is the way to get
  ground truth for which row first disagrees.

  The card being drawn is the right one: after the open, `a5-0x9d2` goes
  `0xed5` (card 3797, the intro) then `0xafa` (**card 2810, the Table of
  Contents**), so the catalogue's own `on openCard / go to card "theContents"`
  runs and navigates. The 14 KB `BMAP 4202` is that card's.

  What is left is the opcode handling in `fn_21_59e2` itself
- **A script error still fires**, now `Can't understand what's after "pass"`
  (`DLOG 1684`, `STR# 1002`) plus an `ALRT 3003` "Unexpected error 673082".
  The catalogue's script uses `pass doMenu` and `pass idle`. The dialog draws
  over rows 91-168 of the card.
- **A chunk expression loses two characters.** The catalogue's script builds a
  path from `the long name of this stack`; with the volume renamed `ABCDEFGH`
  the request comes out `CDEFGH:`, i.e. `char 1 to i` returning `char 3 to i`.

Ruled out for the script error: computed-jump entry points. `find_entries.py`
reports none missing for either `4EFB` or `4EBB` after the 30 fixes.


### What "on screen and navigable" still needs

1. **The block machinery above.** No card can be built without it.
2. **Card rendering.** `BMAP` blocks are compressed 1-bit images; `PAGE`/`CARD`
   blocks hold the objects. QuickDraw already draws what it is given.
3. **Real events.** The headless harness answers `GetNextEvent` with nulls, so
   nothing can be clicked. Navigation needs mouse events fed in, and the button
   scripts behind them need **SANE** (88 sites, still stubbed) for HyperTalk
   arithmetic.

### File Manager (landed)

`runtime/files.c` serves a title's own media read-only: `Open`/`OpenRF`,
`Read`, `Close`, `GetEOF`, `Get`/`SetFPos`, `GetFileInfo`, `GetVol`/`SetVol`,
and the `FSDispatch`/`HFSDispatch` selectors worth answering -- `PBGetCatInfo`
by index, by name and by directory id, `PBGetWDInfo`, `PBGetFCBInfo`. These are
**OS traps**: A0 is the parameter block, D0 the result.

`extract_resources.py --forks DIR` writes every file's two forks plus a
`files.json` index. Forks are then read from the host on demand -- this disc's
are 422 MB.

A document's **resource fork** is parsed on `OpenRFPerm` and handed to the
Resource Manager, which searches the current file first and the application
second. Writes report `wrPermErr` rather than succeeding silently.

Two answers that each cost a full debugging session, recorded so they are not
rediscovered:

- **`PBGetCatInfo` must fill in the parent directory id.** HyperCard climbs
  towards the root and, without it, asks for the same directory forever --
  712,029 calls in one run.
- **Standard File must answer.** A title that cannot find its document asks the
  user; with no answer it asks forever. `MRDOC` names the document.

## Next: the HyperCard trap gap

743 missing call sites across 229 traps, re-measured after the region/dialog
work. Ranked by sites, which is the order that buys the most working program per
trap implemented.

| Sites | Traps | Package | Notes |
|---:|---:|---|---|
| 162 | 47 | **QuickDraw, the tail** | `EmptyRect`, `Pt2Rect`, `GetPen`, `SetCursor`, `SetStdProcs`. Individually trivial; it is a long tail, not a structure. |
| 88 | 17 | **SANE** | `FP68K`, `Elems68K`, `DECSTR68K`, the `Fix`/`Frac`/`X2` conversions. The one genuinely new subsystem left: 80-bit extended arithmetic, a package rather than a HAL passthrough. HyperTalk arithmetic needs it. |
| 41 | 14 | Window Manager | `MoveWindow`, `FindWindow`, `DragWindow` — real windows rather than one full-screen port. |
| 38 | 13 | Resource Manager | `GetResInfo`, `OpenRFPerm`, `Count1Resources`. |
| 35 | 1 | Script Manager | `ScriptUtil` alone. A wrong stub is worse than none: it is a selector-dispatched call, so it needs the selectors decoded before it returns anything. |
| 34 | 8 | Menu Manager | `SetMenuItemText`, `GetMenuHandle`, `MenuSelect`. |
| 33 | 16 | Memory Manager | `NewString`, `SetString`, `HeapDispatch`, `StackSpace`. |
| 26 | 7 | Sound Manager | `SndNewChannel`, `SndDoCommand`. |
| 26 | 4 | Font Manager | `GetFontInfo`, `GetFNum`, `RealFont`. |
| 14 | 8 | Control Manager | `TestControl`, `MoveControl`, `GetControlMaximum`. |
| 14 | 1 | Dialog Manager | `SelectDialogItemText` — waiting on TextEdit. |

The ~14 words the scanner still cannot name are data bytes inside CODE
segments, not traps. See the unclassified line in the report.

## Conformance harness

`tools/conformance.py`, run in CI on every push. It loops extract → scan →
coverage over the corpus and prints one row per title:

```bash
MACRECOMP_CORPUS=/path/to/images python tools/conformance.py
```

```
=== macrecomp conformance (1 measured, 3 skipped) ===
  ok   HyperCard 1.2.2   2452/3166 sites (77%)
  SKIP HyperCard 2.4.1   HyperCardBootSystem6.img not in corpus dir
```

The tracked number is **covered call sites**, and a title that drops below its
recorded baseline fails the run — so a HAL change that quietly breaks a title
that used to work is caught here. Baselines live in `tools/corpus.json`;
`--update` rewrites them from the current run, and a gain is reported as `GAIN`
rather than silently accepted, so the baseline moves deliberately.

A title whose image is absent is `SKIP`, never a silent pass and never a
failure, so CI without a corpus still exercises the whole harness.

### Corpus

Classic-Mac media is not redistributable, so the corpus lives outside the repo
and the harness fetches or skips with a clear message. All of these are images
`extract_resources.py` already reads — raw HFS `.img`, DiskCopy 4.2, or a
partitioned Mac CD:

| Source | Contains | Why |
|---|---|---|
| `archive.org/details/HyperCardBootSystem6` (6.4 MB) | System 6.0.8 + HyperCard 2.4.1 | Smallest complete fixture; good CI default. |
| `archive.org/details/HyperCardBootSystem7` | System 7.0.1 + HyperCard 2.4 | Second HyperCard generation. |
| `archive.org/details/BMUGHyperCardStacks` (250 MB) | HyperCard 1.2.1 + BMUG PD-ROM stacks | Hundreds of stacks — the breadth corpus. |
| `archive.org/details/HyperCardVol1`, `…Vol2` | System 7.5.3 + HyperCard 2.4 + stacks | Cross-check against the boot drives. |
| *The Electronic Whole Earth Catalog* CD (1988) | HyperCard **1.2.2** | The build already measured in the table above — identified, not a new fixture. |

Four HyperCard builds across both major generations, which is what keeps the
lifter and HAL from overfitting to one binary.

## Out of scope

- Shipping any Apple ROM, System software, or HyperCard binary. The tool ships;
  the output never does.
- Color QuickDraw beyond what a scanned title actually calls.
- A HyperTalk interpreter. If HyperCard recompiles, its own interpreter comes
  along with it — writing a second one is the thing this approach avoids.

## Deferred

- `.dec.bin` vs `.bin` segment selection is still the user's job; the scanner
  only warns when it detects it is reading encrypted bytes.
- The manager table in `scan_traps.py` is hand-maintained. It places every trap
  the two scanned titles use; new titles may add unclassified names.

## The Electronic Whole Earth Catalog renders

HyperCard 1.2.2 opens the catalog's `WHOLE EARTH` stack and draws its Table of
Contents card on screen: the globe, the heading, the "INTRODUCTION &" banner and
the contents icons. The top of the card paints; the lower part does not yet -- HyperCard's WOBA
bitmap decoder stops after roughly 105 of 342 rows, although the composite and
the expander are both handed the full card rect (0,0,342,512) and the block
carries 14296 bytes of image data.

`MRCLICK=x,y[,n]` synthesises a click, and the first Table of Contents button
is at rect t=81 l=2 b=112 r=185 (read out of the stack file). **No effect of
that click has been demonstrated.** An earlier note here claimed the click made
HyperCard search its stacks; it did not. That enumeration -- twenty names,
NOMADICS through QUICK SEARCH -- happens at startup while HyperCard looks for
its Home stack, and the count is identical with and without a click. Nor does
the click reliably produce the `Can't understand what's after "if"` error: that
appears intermittently in both cases, because `TickCount` is wired to real time
and no two runs reach the same point. Anything measured across runs here needs
repeating before it means anything.

The `if` error itself is traced.
`fn_14_298e` raises it at `seg14+0x2b20`, where it loads the handle at
`a5-0x551e`, dereferences it, and requires the first word of the block to be
non-zero. The handle is set -- by `fn_3_0e3a` -- and points at a block whose
first word reads 0.

Unlike the `end` error this is *not* decode drift, which is worth recording so
the same search is not repeated: the parser segments contain no unimplemented
instruction that is ever executed, and the one function that fails the
callee-saved check, `fn_14_298e` itself, has no `movem` to begin with, so that
report is a false positive of the same kind as the register-convention leaf
helpers.

Two root causes had to be fixed before anything could appear:

- **Pascal Booleans.** A Boolean result occupies the two-byte result slot but is
  read as a byte at the slot's address -- `move.b (a7)+,d0` takes the *high*
  byte. Returning it in the low byte made `SectRect` and every other Boolean
  trap answer false, and HyperCard's card composite exits early when `SectRect`
  says the rects miss.
- **`ScreenRow`** (low memory 0x106) was never set. A title that blits with its
  own code steps rows by `base + row * ScreenRow`; at zero every row lands on
  the first.

## Where HyperCard 1.2.2 stops

The HyperTalk parse error is fixed; HyperCard runs clean on the unmodified Home
stack, executes scripts, draws to the screen through the message box, and
navigates between cards. What it does not do is paint a card.

The render chain is traced end to end and every link is confirmed to run by
breakpoint count: the update handler, the paint dispatch, the renderer
(`fn_16_402e`, 6878x), the WOBA bitmap expander (`fn_21_59e2`, which fills
0x843da0), the composite (`fn_16_05c0`, 6850x) and the card-to-screen blit
(`fn_16_06fe`, 6886x).

The composite exits on its first instruction. It intersects the card rect
against the dirty rect at `a5-0x1d0a`, and that rect is (0,0,0,0), so the blit
source at `a5-0x1318` is never filled and the blit copies emptiness.

The dirty rect is filled only by the mode-1 full-redraw path, which needs the
word at `a5-0x1022` to be 1; it reads 0. Every routine that sets it lives in
CODE 13, and none of them is ever called: `fn_13_3dd6`, `fn_13_4e60`,
`fn_13_5c86` and `fn_13_5d62` all measure zero entries. Twenty-two call sites
lead into CODE 13 and several of them run constantly, but every one of them is
gated on the mode already being 1 -- `fn_1_26a2`, which would call the full
redraw, tests `cmp.w -$1022(a5)` first and is itself never reached.

Forcing that word to 1 (`MRFORCEMODE=1`, a probe rather than a fix) shows the
gate is real and that there is a **second, independent problem behind it**: the
mode-1 path then runs, the dirty rect fills correctly with (0,0,342,512), and
the composite executes with it -- and buffer A is *still* empty afterwards. So
`fn_16_05c0` gets past its `SectRect` and produces nothing.

Two things to find, then, not one:

1. what normally drives HyperCard into a card show with a mode greater than 3;
2. why the composite writes nothing even when handed the whole card.
