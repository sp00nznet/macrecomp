# Changelog

All notable changes to this project are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project uses
[SemVer](https://semver.org/).

## [Unreleased]

### Added

- **TextEdit (`runtime/textedit.c`)** - all 22 TextEdit traps over HyperCard's
  81 call sites. Coverage 77% -> **79%** of call sites, 47% -> **52%** of
  distinct traps; the conformance harness reported it as a `GAIN`, which is what
  that check exists for. The TERec lives in **guest memory**: a TEHandle
  dereferences to it and the guest reads `teLength`, `selStart`/`selEnd`,
  `hText`, `nLines` and `lineStarts` directly, so a host-side mirror would only
  be a second copy to keep in sync. Line breaking is arithmetic rather than
  measurement because the HAL's font is fixed-pitch. Not included: styled text
  (`TEStyleNew`), word-break and click-loop hooks, and scrolling -- `TEScroll`
  and `TEPinScroll` reflow instead of moving `destRect`, marked `ponytail:`.
- `mr_alloc` in `toolbox.h`, so a HAL module other than `toolbox.c` can take
  memory from the guest heap without a second allocator.
- **Three debugging instruments**, all kept: `MRMAXCALLS=<n>` stops after n
  lifted transfers and prints the shadow stack; `MRSTACK=1` prints the whole
  shadow stack at every trap; and each `MRTRACE` line now carries the calling
  function and the call depth. The transfer watchdog counts tail jumps as well
  as calls -- a loop spanning functions goes round through `m68k_jump`, and
  counting only calls misses it.
- TextEdit checks in `examples/hal_selftest.c`, asserted on the TERec fields the
  guest reads back: CR line splitting, selection clamping and normalisation,
  `TEKey` insert and caret advance, backspace, and a `TECut`/`TEPaste`
  round-trip.

### Changed

- **TextEdit did not clear the hang, and the ROADMAP now says so.** HyperCard
  still stops after exactly 306 Toolbox calls. `TENew` returning nothing was a
  coincidence of ordering, not the cause. What the hang is *not* is now measured:
  not entry dispatch, not an unimplemented instruction, not TextEdit, and not a
  missing trap at the stall -- the last eight calls are `NewEmptyHandle` and
  `NewHandle`, all implemented and all succeeding. It performs no traps, no
  calls and no tail jumps while looping (`MRMAXCALLS` does not fire at three
  million), which places it inside a single lifted function. The shadow stack
  narrows it to six frames; the ROADMAP lists them.

- **`rol`/`ror` in the lifter and runtime.** `m68k_rol`/`m68k_ror` rotate a bit
  at a time rather than by a shift pair, because a full-width rotate returns the
  value unchanged but still sets C from the last bit rotated out -- which by
  then has come all the way round, so it is the original bit 0, not bit 7.
  Neither touches X (that is ROXL/ROXR). Static unimplemented instructions
  across HyperCard's 21 segments drop 417 -> 384 of 102,941 (0.37%), and a whole
  run now executes **zero** unimplemented instructions: the two it used to hit
  were exactly these.
- **Caller context in the two `no function at` messages.** They now name the last
  two functions entered and the call depth, from the shadow stack the runtime
  already keeps. An address with no owner was previously reported with no hint of
  who asked for it.
- **Resource-lookup tracing under `MRTRACE`.** `res_get` logs each type/id and
  whether it hit. A miss is the interesting case: a title that cannot find a
  resource it needs usually quits rather than complains, so this is often the
  last useful line in a log -- which is exactly how a `Get1Resource` with a null
  type got found.
- Rotate wrap cases added to `examples/entry_dispatch_test.c`.

### Changed

- **Measured: entry dispatch holds.** HyperCard was run from a locally built
  title tree (never committed, house rules section 3). Not one mid-function jump
  fails in a whole run; the `no function at 5210bc` loop is gone. It now reaches
  **306 Toolbox calls** and stops in a hard loop at `TENew` while building a text
  field. **The blocker is TextEdit**, not the lifter. See ROADMAP for the trace,
  and for the two loader details that cost the most time to rediscover: segment
  bytes have to be copied into guest memory at the load base (PC-relative *data*
  reads still go through `M.mem`), and segment bases must not overlap, because
  the range search added for entry dispatch assumes disjoint extents.

- **Entry-point dispatch: a jump into the middle of a function now lands.** The
  original code reaches computed addresses through a register, and such a target
  is typically not a branch target anywhere in the binary, so a table of
  function *starts* could not express it -- `m68k_jump` reported
  `no function at 5210bc` and dropped the transfer. Lifted functions now take an
  `entry` address (0 = start from the top) and carry a prologue that switches on
  it and `goto`s the matching label; the lifter labels **every instruction**,
  not only branch targets, because the address that blocked HyperCard is not a
  branch target. In the runtime, `m68k_register` takes `[start, end)` and a
  lookup that misses the start hash falls back to a binary search for the
  function whose body covers the address. This is a **breaking ABI change** for
  generated code: re-lift, do not mix old and new segments.
- `examples/entry_dispatch_test.c`, wired into `ctest`: entry at a start, at two
  interior boundaries, into the second of two adjacent functions, and through
  `m68k_call` with the stack balanced across it -- plus both failure modes, an
  interior address that is not an instruction boundary (`m68k_entry_miss`) and
  an address no function owns.
- **`tools/conformance.py` + `tools/corpus.json` - the conformance harness**
  required by house rules. Loops extract -> scan -> coverage over the corpus,
  one row per title, and **fails on a drop below a recorded baseline** rather
  than only on zero. Corpus images are copyrighted Mac media and live outside
  the repo (`MACRECOMP_CORPUS`), so an absent title reports `SKIP` -- never a
  silent pass, never a failure -- and CI exercises the harness either way.
- **CI** (`.github/workflows/ci.yml`): build with `-Wall -Wextra -Werror`, run
  `ctest`, run the Python self-checks, and run the conformance harness, on every
  push and PR to `main`.

### Changed

- `m68k_register` now takes `(start, end, fn)`. An `end <= start` registers a
  start-only entry, which cannot be entered part-way.
- Re-measured HyperCard against the current HAL while the fixture was in hand:
  **77%** of call sites and **47%** of distinct traps (was 76%/45%).

### Fixed

- Three dead static helpers (`push16`/`push32` in `toolbox.c`, `clamp` in
  `quickdraw.c`) and an unused packed-row length in `CopyBits`, all surfaced by
  turning warnings into errors for CI. The row length is skipped rather than
  read because `unpackbits_row` stops on output length; that is now written
  down, with the bound noted as a `ponytail:` shortcut.

- **Partitioned Mac CD images** in `extract_resources.py`. A Mac CD-ROM is not a
  bare HFS volume: it opens with an `ER` driver descriptor and an Apple
  partition map, and the HFS volume sits at whatever block the `Apple_HFS`
  entry names. `load_hfs` previously handled DiskCopy 4.2 and raw HFS only, so
  a CD was read from offset 0 and parsed as garbage. Every CD-sourced title was
  unreachable; this is the container layer, so the fix lands once for all of
  them. `tools/test_extract_resources.py` covers the three containers.
- **HyperCard 1.x identified as 1.2.2** (Apple, 1987-88), extracted from *The
  Electronic Whole Earth Catalog* CD-ROM: 22 `CODE` segments, 326,088 bytes,
  1,110 jump-table functions over 21 segments, 3,166 trap call sites across 418
  distinct traps. That is byte-for-byte the fixture already in the coverage
  table, so the corpus's "user-supplied CD" row is not a second HyperCard — it
  is a name and a version number for the one being worked on.

- **`tools/relocs.py` - THINK C far-model relocations.** An application built
  in the far model does not reach its globals through A5; it puts **absolute
  32-bit data offsets inline in the code** and ships `CREL`/`DREL` to say where
  they are. Until those are applied, a string a function passes to `printf` is
  an integer nobody can follow, so neither a disassembly nor the lifter can say
  what the code is talking about.
- `CREL <n>` is a flat list of ascending 16-bit offsets into `CODE <n>`, each
  naming a longword that holds a `DATA` offset - sometimes arriving as two
  ascending runs that concatenate into one list. **Confirmed**: reading the
  longword at each listed site lands on real string starts - `CArray.c`,
  `CObject.c`, `CWindow.c`, an assertion message - which random offsets would
  not. On the application this was written against, 8,165 fixups with 85 on
  string starts, the rest pointing at variables and tables.
- `DREL` does the same for `DATA`, with 32-bit entries (zero high word) then
  16-bit ones, both locations measured as `DATA_size + (v - 0x10000)`.
  **Confirmed only in range** - all 2,776 entries map inside `DATA`, and one
  slot demonstrably holds a string pointer as a magnitude below A5 - so the
  docstring says so rather than claiming the rest.
- Recorded as a known gap: **some segments carry no `CREL` at all** (five of
  twenty here). They reach their data some other way and this tool does not yet
  say how.
- **A real 5x7 text font** (`runtime/font5x7.h`). `qd_draw_char` drew a hollow
  box per character, so every title rendered unreadable word-shapes. Original
  glyph data; fixed-pitch stand-in, not a metric match for Chicago or Geneva.
- **Jump-table extraction** (`extract_resources.py` -> `jumptable.json`). The
  lifter requires one, but only `unprotect.py` emitted it, so an *unprotected*
  title could not be lifted at all. Extraction is the right layer: every title
  has a jump table, only protected ones need decrypting first.
- **Low-memory globals** (`ROM85`, `ROMBase`, `MemTop`, `ScrnBase`, `Ticks`,
  `CurrentA5`). Classic Mac code reads these addresses directly rather than
  through a trap; zeroes read as a machine with no ROM.
- `SysEnvirons`, reporting a machine that matches what the runtime actually is:
  68000, 1-bit screen, 128K ROM. Claiming a 68020 or Color QuickDraw would send
  titles down paths the HAL does not implement.
- `MRSHOT=<path>` writes the framebuffer to a PGM on every present, so a run can
  be captured without a display -- including from inside a modal loop.
- Traps: `SetApplLimit`, `ShutDown`, `FreeMem`, `SetFScaleDisable`,
  `SetFractEnable`; alerts now build a real dialog from their `ALRT`/`DITL`
  rather than returning 1 without drawing.
- **Dialog Manager + Control Manager** (`runtime/dialog.c`): DITL parsing,
  `GetNewDialog`/`NewDialog`, `ModalDialog`, `GetDialogItem`, item text with
  `ParamText` `^0`-`^3` expansion, hide/show, `FindDialogItem`, and real
  `ControlRecord`s so `SetControlValue` on a dialog item works.
- **QuickDraw regions**: `NewRgn`, `RectRgn`, `SectRgn`, `UnionRgn`, `DiffRgn`,
  `PtInRgn`, `SetClip`/`GetClip` and the region drawing calls. Rectangular
  (bounding-box) regions only — a non-rectangular region degrades to its bbox.
- Toolbox utilities: the `Bit*` calls, `FixMul`/`FixRatio`/`FixRound`, `Random`,
  `StringWidth`/`TextWidth`, the `RoundRect` calls, `UnLoadSeg`, Scrap stubs.
- Memory Manager: `GetHandleSize`/`SetHandleSize` (growing preserves contents),
  `NewEmptyHandle`, `GetZone`, `MaxMem`, `PurgeSpace`.
- `scan_traps.py --coverage <toolbox.c>` — reports how much of a title's trap
  set the HAL already dispatches, grouped into per-manager work packages and
  ranked by call sites. Turns the scope gate into a tracked number.
- Warning when a scanned directory does not look like 68k code. Protected
  titles disassemble to near-random A-line words, which previously produced a
  confident and entirely wrong trap set; the scanner now names the cause and
  points at `unprotect.py`.
- `tools/test_scan_traps.py` — self-check for the manager table, the HAL trap
  parser, and the not-code heuristic.
- `ROADMAP.md` and this file, per house rules.

### Changed

- `scan_traps.py --json` output now carries a `coverage` object when
  `--coverage` is passed.
- `examples/hal_selftest.c` now asserts rather than only drawing: regions,
  dialogs, the Memory Manager and the Pascal result convention are checked
  through the trap interface, and the harness exits non-zero on failure.

### Fixed

- **The jump-table map's bounds checks disagreed.** `m68k_jt_set` accepted
  `a5off < 8192` while `m68k_jt_call` accepted `a5off < 65536`, so a jump table
  larger than 8 KB had its tail silently dropped on the way in and read back as
  zero -- a null dispatch a long way from the cause. HyperCard's table is 8880
  bytes, so its last ~91 routines were unreachable; Shufflepuck's 2 KB table
  never reached the cliff. One capacity, one predicate, and an entry that does
  not fit now says so.
- **Operand normalisation was applied in `parse()` but not in
  `indirect_call`/`indirect_jump`**, so capstone's `$10ae(pc, d1.w)` spacing
  silently dropped whole addressing modes to unimplemented. Shared as `norm_op`.
- **Odd-addressed function starts.** The 68000 fetches instructions on word
  boundaries, so a function can never begin at an odd address; 188 such starts
  were being lifted, each one a linear disassembly beginning mid-instruction
  that then ran as if it were the program.
- **Function starts decoding to non-68000 forms.** capstone renders 68020
  memory-indirect `([$fffc,a2])` and scaled-index `(a0,d3.l * 4)` even in
  `M68K_000` mode, but a 68000 binary cannot contain either -- so their presence
  at a candidate start proves it is not an instruction boundary. 30 more bogus
  starts rejected, on the same architectural argument as the alignment rule.
- Added addressing modes `(An,Xn)` and `d(pc,Xn)`, and the computed
  `jmp d(pc,Xn)`: 1221 -> 417 unimplemented instructions across HyperCard's
  21 segments.
- **`GetTrapAddress` returned 0 for every trap.** The standard feature probe
  compares a trap's address with `_Unimplemented`'s; returning 0 for both makes
  every trap look absent. This is why HyperCard refused to start.
- **The lifter crashed on an unknown addressing mode** instead of emitting a
  counted stub, so one unhandled operand took down a whole segment. Unknown
  forms now degrade and are recorded in `UNPARSED` for ranking.
- **Indexed addressing never lifted at all.** capstone writes `$74(a5, d7.w)`
  with a space after the comma; the operand patterns spelled it without one.
- **`movem` only handled the predecrement/postincrement forms**, missing
  `movem.l -$10(a6),regs` -- the frame-pointer epilogue, so it appeared in
  almost every compiled routine.
- Function-boundary resolution raised on a branch sitting below every known
  entry (code ahead of the first jump-table routine).
- **Pascal function results were pushed instead of written.** The caller
  reserves the result slot below the arguments, so after popping them SP already
  points at it; pushing left SP two bytes short and the result where nothing
  read it. Affected `SectRect`, `PtInRect`, `TickCount`, `Button`, `StillDown`,
  `GetNextEvent`, `FrontWindow`, `SizeRsrc`, `ResError` and others. Added
  `ret16`/`ret32` so the convention is named once.
- **`GetNextEvent` popped its arguments in the wrong order**, so the caller's
  `EventRecord` pointer was garbage and no event ever reached the guest.
- **`PtInRect` treated its `Point` as a pointer.** A Point is passed by value.
- **OS trap numbers are 9 bits, not 8.** `norm()` masked with `$FF`, folding
  `NewHandle` (`$A122`) onto `$A022`, which is not a trap — so `NewHandle` fell
  through to the unimplemented log. Same for `GetTrapAddress`, `PurgeSpace` and
  `NewEmptyHandle`.
- **`heap_alloc` could write past the end of guest memory.** It zero-filled via
  `M.mem[]` directly, and the heap was pinned at 8-30 MB regardless of
  `M.memsize`. Now bounds-checked, and the heap falls back to the top half of
  whatever memory the app allocated.
- `CMakeLists.txt` linked `m` unconditionally, which fails on MSVC (there is no
  `m.lib`; the math functions are in the CRT).

### Measured

- Shufflepuck Cafe: 925/995 call sites covered (92%), 126/182 traps (69%) —
  up from 86%/53%.
- HyperCard 1.x: 2423/3166 call sites covered (76%), 189/418 traps (45%) —
  up from 53%/27%.
