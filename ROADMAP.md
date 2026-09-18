# Roadmap

What's next, what's deferred, what's out of scope. Ordered by measured need, not
by guess — the numbers come from `scan_traps.py --coverage`.

## Where the HAL stands

| Title | 68k | CODE segs | Distinct traps | Call sites | Sites covered | Traps covered |
|---|---|---|---|---|---|---|
| Shufflepuck Cafe (1988) | ~53 KB | 6 | 182 | 995 | **92%** | 69% |
| HyperCard 1.2.2 (1988) | 326 KB | 22 | 418 | 3166 | **81%** | 56% |

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

### Instruments kept

`MRMAXCALLS=<n>` stops after n transfers and prints the shadow stack; `MRSTACK=1`
prints the whole shadow stack at every trap; `MRMAXLOOPS=<n>` (with
`-DMACRECOMP_LOOPGUARD`) prints the hottest backward branches; and each
`MRTRACE` line carries the calling function and the call depth. `gdb` in the
MSYS2 toolchain here is broken -- missing `libxxhash.dll` -- so a native
backtrace was never available, and these stand in for it.

### File Manager (landed) -- and why the catalog still is not on screen

`runtime/files.c` serves a title's own media read-only: `Open`/`OpenRF`,
`Read`, `Close`, `GetEOF`, `Get`/`SetFPos`, `GetFileInfo`, `GetVol`/`SetVol`,
and the `FSDispatch`/`HFSDispatch` selectors worth answering. These are **OS
traps** -- A0 is the parameter block, D0 the result -- so nothing is read off
the Pascal stack. Forks are read from the host on demand: one CD-ROM's forks are
422 MB and a stack is read a few hundred bytes at a time.

`extract_resources.py --forks DIR` writes every file's two forks plus a
`files.json` index, which is what a File Manager HAL needs and what extracting
one application's resources does not give you.

Writes report `wrPermErr` rather than succeeding silently. A title told it
cannot write can say so; one told "fine" loses data.

Covered by `examples/hal_selftest.c`: open, `fnfErr` for a missing file, a read
from the mark, `GetFPos`, an absolute `SetFPos`, a read across the end that
delivers a short count *and* `eofErr`, `wrPermErr` on write, and `rfNumErr`
after close. 79% -> **80%** of call sites.

**HyperCard still does not open a stack**, and the reason is no longer the File
Manager. Traced through:

```
fn_3_00e4 -> fn_21_1e1e -> fn_21_1bec -> Open glue -> _Open
```

`fn_21_1bec` does `movea.l $8(a6),a4` and then addresses everything as
`-$4ae(a4)` -- the parameter block is a local in its *caller's* frame, and the
caller passes its own `a6`. The pointer arriving at `_Open` is `0xFFFFFB52`,
which is exactly `-0x4ae` with **a4 = 0**: the caller's frame pointer was null.
A5 is correct (checked), `link`/`unlk` lift correctly (checked), and the
call does enter `fn_21_1e1e` at its start, where `link.w a6,#$f304` runs. So a
valid frame pointer is becoming 0 somewhere between that `link` and the
`move.l a6,-(a7)` that passes it. That is the next thread, and it is a lifter or
calling-convention question rather than a HAL one.

Two other leads, both probably the same fault: `m68k_jump: no function at
000000` fires repeatedly through this sequence (a return address or function
pointer reading as null), and `no function at cbfe0000` -- one bit from the
`0xCAFE0000` return sentinel -- suggests arithmetic landing on a return address.

Deliberately **not** built: the Finder startup handshake (an `AppParmHandle`
block that `CountAppFiles`/`GetAppFiles` walk to learn which document to open).
It was written, then removed unused -- HyperCard 1.2.2 never calls
`GetAppParms`, so it was forty lines answering a question nothing asked. The
layout is recorded in a comment for whenever a title does ask.

Two other things the run settled:

- The framebuffer is blank, but `SetPBits` at call 280 has pointed the port at
  an offscreen bitmap, so that is consistent rather than contradictory. Nothing
  is expected on screen yet.
- Whole-run unimplemented *instructions* are now **zero**. The two that used to
  fire were `rol.b`/`ror.b`, which the lifter did not lower; both now do.

`FSDispatch` and `FP68K` (SANE) are still stubs, and HyperCard cannot open its
Home stack without the File Manager, so a rendered card is still not the next
thing to expect -- but TextEdit is what the program is asking for right now.

Two loader details worth recording, because both cost time to rediscover and
neither is in the repo:

- **Segment bytes must be copied into guest memory at the load base**, not just
  registered as functions. Lifted code is C, but PC-relative *data* reads --
  string literals, constant tables, the resource type handed to `Get1Resource`
  -- still go through `M.mem`. Without the copy they read zero, and HyperCard
  quits during init after a `Get1Resource` with a null type. That one mistake
  cost 280 of the 306 calls.
- **Segment load bases must not overlap.** The range search added for entry
  dispatch assumes function extents are disjoint, so the spacing has to exceed
  the largest segment (28 KB here), not the average one.

Entry dispatch is covered by `examples/entry_dispatch_test.c` (`ctest`), which
checks entry at a start, at two interior boundaries, into the second of two
adjacent functions, through `m68k_call` with the stack balanced across it, and
the two failure modes: an interior address that is not an instruction boundary,
and an address no function owns.

Two things worth knowing before touching the boundary code again -- both still
true, because entry dispatch resolves *where a jump lands*, not *what the sweep
believes is code*:

- **The linear sweep in `main()` drifts.** It disassembles each segment once
  from offset 0, so any embedded data knocks it out of alignment and everything
  after decodes as plausible-looking nonsense until it resynchronises. It found
  46 `jmp d(pc,Xn)` "switch dispatches" across the 21 segments; *none* survive
  into generated code, because none lie at a real instruction boundary. Branch
  discovery rests on this sweep, so treat anything it alone reports as a lead,
  not a fact.
- **Verify an instruction against its function's real start.** The same bytes
  decode differently from different alignments, and both readings look like
  ordinary code. A site here was read as a switch dispatch, then as data, and
  was in fact neither -- only disassembling from the enclosing function's start
  settled it.

Still stubs regardless: `FSDispatch` and `FP68K` (SANE). HyperCard cannot open
its Home stack without the File Manager, so no card can render until that lands.

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
