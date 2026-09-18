# Changelog

All notable changes to this project are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project uses
[SemVer](https://semver.org/).

## [Unreleased]

### Added

- **A window.** `runtime/platform_sdl.c` was written but never linked: it
  includes `<SDL.h>`, and nothing put SDL2's include directory on the compile
  line, so every build quietly fell back to the loader's headless stubs. Those
  stubs are now behind `#ifndef MACRECOMP_SDL`, `main` calls `plat_open`, and a
  build with `-DMACRECOMP_SDL` puts the title on screen with a real mouse and
  keyboard. HyperCard 1.2.2 draws recognisable Mac dialogs -- frame, drop
  shadow, default-button ring, Chicago text -- so QuickDraw and the Dialog
  Manager are confirmed against something other than a pixel count.
- **`addx`, `subx`, `negx`, `roxl`, `roxr`** in the lifter, with seven cases in
  the differential check (46 total). HyperCard 2.4 executed `addx.l` sixteen
  times in its first 721 traps. `addx`/`subx`/`negx` have a **sticky Z**: a zero
  result leaves Z alone rather than setting it, so a multi-word add ends Z-set
  only when every word was zero; using `fl_add` here would let the top word
  alone decide a 64-bit comparison. `roxl`/`roxr` rotate *through* X, and with a
  zero count C takes X's value instead of being cleared.
- **`FindWindow`, `SetCursor`, `GetKeys`.** FindWindow is what turns a click
  into a destination; unimplemented, it left every click pointing at the desktop
  and nothing could be navigated. One full-screen card window means the only
  distinction that matters is menu bar versus content.
- **Debug probes**: `MRBRK=<hex addr>` reports the arguments a chosen function
  is called with, `MRBRKA5=<offsets>` shows the A5 globals beside them, and
  `MRPARSE` reports every register and stack slot pointing at script text. An
  assertion that compares two globals says nothing until you can see what they
  hold; this is how error 123452 was traced to a single `cmp.l` in CODE 16.
- A recorded conformance baseline for **HyperCard 2.4.1 (70%)**, so the harness
  can now fail on a regression there rather than only reporting a number.

### Fixed

- **`$A914` was labelled `GetWMgrPort`; it is `DisposeWindow`** (GetWMgrPort is
  `$A910`). The old case popped the WindowPtr and wrote zero *through* it,
  clearing the first field of the port it was handed.
- **`SetHandleSize` silently emptied any handle the HAL had not recorded.**
  `hsz_get` returns 0 both for a zero-length handle and for an unknown one, so
  growing an unknown handle allocated a new block, copied zero bytes into it,
  and returned it as if the data had moved. Unknown is now asked separately, and
  an unknown handle copies what the caller asked for -- safe, because this heap
  never reuses a block. A wiped handle is indistinguishable further on from data
  that was garbage all along, which is the worst kind of bug to chase.
- **`EventAvail` was consuming events.** It shares an implementation with
  `GetNextEvent`, but `EventAvail` reports the next event and *leaves it in the
  queue*. Every peek ate an event, so a title that polls with `EventAvail` and
  then fetches with `GetNextEvent` -- the ordinary idiom, and what HyperCard
  does five times more often than it fetches -- lost nearly all of them, mouse
  clicks included. One slot of pushback tells them apart.
- **Nothing ever raised an `updateEvt`.** A Mac application paints a window's
  contents only when handed one, so the window was shown and then never
  painted, which looks exactly like a title that failed to draw. `ShowWindow`,
  `SelectWindow`, `InvalRect`, `InvalRgn` and a new window now queue one, and it
  is delivered once per exposure -- cleared on delivery rather than on
  `BeginUpdate`, so a title that never calls `BeginUpdate` cannot spin on it.
- **The `WindowRecord`'s `visible` byte was never written.** Past the 108-byte
  GrafPort sit `windowKind`, `visible` and `hilited`; a window whose `visible`
  byte reads zero is one the title will not draw into, however complete the port
  is. Setting it is what finally got HyperCard to call `BeginUpdate` at all.
- **`g_last_call` was not restored when a call returned**, so "last function
  entered" stayed pointing at whatever was called deepest and every trap
  reported after a return named the wrong caller. That sends you reading a
  function with nothing to do with the trap, which cost real time here.
- **`SetPortBits` retargeted drawing without updating the port.** The real trap
  *copies* the BitMap into `thePort->portBits`; this one only pointed QuickDraw
  at the new buffer and left the port's own `baseAddr` in guest memory stale. A
  title that reads it back to check which buffer it is drawing into then decides
  it is somewhere it is not. HyperCard asserts exactly that, which is what
  `Unexpected error 123452` was. Copying the 14-byte BitMap into the current
  port takes HyperCard from **1096 to 6803 Toolbox calls** with no error at all.
- **A write watchpoint**, `MRWATCHADDR=<hex>`, reporting every longword write to
  one guest address with the function that made it. "Who set this global?" is
  otherwise unanswerable when nothing stores to it at a literal offset -- the
  write arrives through a register, a struct copy, or a Toolbox trap writing
  through a caller's pointer, which is how this bug was finally cornered. Note
  that "off" is `0xFFFFFFFF`, not 0: writes to address 0 are real and worth
  seeing.
- **`InitGraf` was a no-op that threw the QuickDraw globals away.** The pointer
  it is handed is the *last* field of `QDGlobals` (`thePort`), so `screenBits`,
  the five standard patterns, the arrow cursor and `randSeed` all sit at fixed
  negative offsets from it. None of them were ever written, so a title that
  reads `qd.screenBits.baseAddr` to find the screen, or `qd.gray` to fill with
  it, got whatever happened to be in memory.
- **The screen's `baseAddr` was the literal constant 1.** That works only as
  long as nobody looks at it. A title that keeps its own copy of the screen base
  and asserts its port still points there compares a real pointer against 1 and
  concludes the port has been redirected -- which is what HyperCard's
  `Unexpected error 123452` is. The screen now gets a genuine block of guest
  memory, sized like a real 1-bit screen; drawing still goes to `qd_fb`, but the
  address is real and comparisons against it hold.
- **An offscreen draw could run off the end of its buffer and into the heap.**
  `qd_set_port` took the BitMap's `bounds.top` and `.left` and dropped `.bottom`
  and `.right`, so the plot path checked only that the local coordinates were
  non-negative. `m68k_w8` keeps a stray write inside guest memory, so nothing
  crashed -- whatever had been allocated after the buffer was quietly rewritten
  instead. `rowBytes` fixes the row width exactly and now bounds x; the bounds
  rect bounds y.
- `MRSHOT` no longer overwrites the one frame worth keeping with the blank one a
  title leaves behind on the way out.

### Investigation

HyperCard 1.2.2 still stops at `Can't understand what's after "end"` before it
opens a catalog stack. What is now known:

- **The Home stack has no checksum**, so its scripts can be edited in place and
  bisected. (An earlier note guessed there was one at STAK+0x0C; there is not --
  emptying the script changes behaviour because the handlers stop existing, not
  because the file is rejected.)
- The error needs a **chain**, not a single line: `show card field "Copyright"`
  *and* `pass startup` in the card script of CARD 5341, *and* `go to card "User
  Preferences"` *and* the two `set lock...` lines in the stack's `getHomeInfo`.
  Blanking any one link removes it. No single script is mis-parsed -- each of
  them is ordinary, and all of them shipped with HyperCard.
- Nothing points at the failing text by the time `ParamText` is reached, and the
  one stack slot that still holds script text (`on resume`, offset 382) is
  stale: blanking that handler does not move the error.
- Patching `getHomeInfo` out reaches further -- HyperCard enumerates the
  catalog's own stacks, `COMMUNICATIONS` through `HOUSEHOLD` -- and then fails a
  different assertion, `Unexpected error 123452`, which is `cmp.l -$1318(a5),d0`
  against `-$11c2(a5)` in CODE 16. `-$1318(a5)` holds a real pointer;
  `-$11c2(a5)` holds 1. Its three writers all store pointers and the one taking
  a parameter is never called, so the value arrives from somewhere else. That
  assertion fires only *after* a HyperTalk error, so it is downstream of the
  parse failure rather than a separate blocker.

### Added

- **1021 -> 2267 Toolbox calls.** `SetWTitle`, `MoveWindow`, `SizeWindow`,
  `EqualRect`, `EmptyRect`, `GetCursor`, `GetWTitle`, `DeleteMenu`,
  `SetStdProcs`, `SndNewChannel`, and the Colour QuickDraw device list, which
  honestly reports no devices rather than something that cannot be walked.
  Coverage 81% -> **83%**.
- `StripAddress` as an explicit **no-op**. It exists because a 24-bit machine
  kept flags in a pointer's top byte; masking on a 32-bit clean address space
  would truncate every heap pointer above 16 MB, and this runtime's heap starts
  at 8 MB.
- A lifter case for `move.b d(pc,Xn)` -- the character-class table lookup a
  tokeniser uses -- and the harness now copies the segment's own bytes into
  guest memory, because PC-relative *data* reads go through `M.mem` and read
  zero otherwise. The real loader has the same requirement; the check did not,
  so it could not have tested this at all.

- **HyperCard runs HyperTalk.** From 386 Toolbox calls to **1021**: it opens the
  Home stack, walks its `MAST` index, reads the `LIST`, `PAGE`, `BKGD` and
  `BMAP` blocks, reaches `ShowWindow` on its card window, and executes the
  stack's script -- far enough to report on a command's arguments by name.
- **Handle copiers** `PtrToHand`, `PtrToXHand`, `PtrAndHand`, `HandAndHand`.
  Register-based like the Memory Manager, so an unimplemented one leaves garbage
  in A0 and the title reports *out of memory* -- which is the dialog HyperCard
  was putting up.
- **`StackSpace`**. A recursion guard reads it and stops when it looks small, so
  an unimplemented one reads as a stack already full. HyperCard was reporting
  *too much recursion* on a stack that was barely used.
- **Calls that land inside the A5 jump table now resolve through it.** Code
  normally arrives with `jsr d(a5)`, but a computed call hands over the absolute
  address, which previously read as a call into nowhere. Worth 300 more calls on
  its own.
- `FreeMem`, `MaxMem`, `PurgeSpace` and `MaxBlock` report **what is actually
  left** rather than a flattering constant, and the heap says so once when it
  runs dry -- a title told there is room and then refused an allocation has no
  way to cope.

### Fixed

- **Alignment padding after an unconditional transfer.** An assembler pads to an
  even boundary with zero words after `jmp`/`bra`/`rts`, and a linear decode
  swallows them into the instruction that follows -- so every boundary after
  that is wrong, and code reached only by a computed jump has no label to enter
  at. The padding is now recorded as data so the address after it stays a
  boundary.

- **`mul` and `div` never applied their source operand's addressing side
  effects** -- and this is the one that had HyperCard stuck. `divu.w (a7)+,d0`
  read its divisor and left A7 exactly where it was, so the *next* instruction,
  `add.l (a7)+,d0`, added the divisor instead of the table base it wanted. That
  is HyperCard's block-record hash: `(id folded) mod count * 12 + table`. It was
  returning a wild pointer, every write through it was dropped, the block cache
  never populated, and the title reported `Unexpected error 1250`.

  With the postincrement emitted, HyperCard goes from 386 Toolbox calls to
  **625**, from 3 file reads to **15**, and the error is gone. It now reads the
  stack's `MAST` index, `LIST`, `PAGE`, `BKGD` and `BMAP` blocks -- the actual
  card structures -- and reaches `ShowWindow`.

### Added

- The lifter check grew to **32 cases**, covering the arithmetic and addressing
  the stack-block decoder leans on: masking, indexed addressing with a
  sign-extended word index, `mulu`/`divu` result placement, `btst` on a bit,
  shifts by a register count, `movem` round-tripping through the stack, and
  read-modify-write on memory. All pass, which is useful negative information:
  the decode path's arithmetic is not where the remaining fault is.
- **`exg`**, which was not lifted at all (4 sites) -- found by the check above.

- **`tools/test_lift68k.py` - a differential self-check for the lifter**, run in
  CI. It lifts short 68k byte sequences whose effect is fixed by the processor
  manual, runs the generated C, and compares the machine state against what a
  68000 would produce. Every lifter bug found so far was found the hard way, by
  bisecting a title hundreds of thousands of instructions downstream of the
  instruction that actually misbehaved; each of those is now a case here. 13 to
  start, and cheap to add to.

### Fixed

Both of these were found by the new check within minutes of it working, which is
the argument for it:

- **Resource handles had no recorded size**, so `GetHandleSize` answered 0 for
  every resource and a caller asking how big one is concluded it was empty.
- **`moveq` did not sign-extend.** It carries an 8-bit immediate and extends it
  to 32 bits, so `moveq #-1,dN` -- how a routine spells "not found" or "end of
  list" -- was becoming **255**, and every caller comparing against -1 silently
  missed.
- **`addq`/`addi`/`subq` to an address register set the flags.** On a 68000 an
  `An` destination leaves the condition codes alone, so `addq #1,a0` between a
  test and its branch was quietly changing which way the branch went.

- **A document's own resource fork** (`OpenRFPerm`, `OpenResFile`). A stack
  carries its own resources and HyperCard opens them before reading a card. The
  fork is a self-contained database, so `files.c` parses it once -- header, type
  list, reference lists, data area -- and hands the contents to the Resource
  Manager rather than reading it back a piece at a time. `res_get` now searches
  the current resource file first and the application's own fork second, which
  is the Resource Manager's chain shortened to the two links that exist here.
  Left unimplemented, `OpenRFPerm` did not merely return nothing: it left three
  arguments on the Pascal stack.

### Fixed

- **`BlockMove` was unbounded.** It indexed `M.mem` directly, so a stale pointer
  read and wrote *outside* the guest address space and corrupted the host's own
  heap -- damage that then surfaces anywhere at all with nothing tying it back.
  It is now bounds-checked like every other guest access, and handles
  overlapping moves, which BlockMove is required to.

- **`dbra` loops were all no-ops - 563 of them.** `DBcc` loops while its
  condition is **false**, the opposite of `Bcc`. `dbra` is the assembler's
  spelling of `dbf`, so looking "ra" up in the branch table yielded "always
  true" and made the loop body unreachable: every counted loop in HyperCard --
  block copies, string walks, table scans -- did nothing at all, and the
  counter never moved. This is the single largest correctness fix so far.
- **`move`/`movea` operand order.** The 68000 fetches the source, applying its
  postincrement, *before* computing the destination address. Two idioms depend
  on it exactly: the Pascal epilogue `move.l (a7)+,(a7)`, which lifts the return
  address over the parameter, and the variadic glue's `movea.l (a7)+,a7`, which
  loads a new stack pointer off the old stack. Emitting the increment afterwards
  made the first a no-op -- so callers resumed four bytes low and read results
  out of their own arguments -- and made the second add 4 to the stack pointer
  it had just loaded, until SP walked out of the address space entirely.
- **`cmpm` was unimplemented** (37 sites) - the memory-to-memory compare with
  both operands postincrementing, i.e. the string-compare instruction. It sets
  flags exactly like `cmp`. Without it HyperCard's check of whether the file it
  opened really is the home stack never ran.
- **Auto-pop package traps.** Bit 10 of a Toolbox trap means the dispatcher
  returns to the address on the stack rather than to the instruction after the
  trap; the package glue pops its return address, pushes the selector under it
  and traps. The lifter now emits a return after such a trap, and Standard File
  reads its arguments in that order. Previously the selector read as garbage and
  execution ran on into the next glue entry.
- **`GetNewDialog` read the item-list ID from the wrong offset.** `itemsID` sits
  at offset 18 of a `DLOG`; reading at 20 lands on the title's length byte and
  first character. HyperCard's error dialog asked for item list 1349 -- which
  does not exist -- and so came up empty. It now shows its real text.

### Added

- **HyperCard opens and reads the Home stack.** It walks the catalogue by index,
  finds `Home` among the disc's stacks, opens it and reads it (1536, 4608 and
  512 bytes). It then reports `Unexpected error 1250` in a legible dialog of its
  own -- a specific diagnostic with `ParamText` substitution working, where
  before the same dialog was empty and the error had no number.
- **A directory model in the File Manager**: one volume (vRefNum -1) whose root
  is the standard HFS root (dirID 2). `PBGetCatInfo` answers by index, by name,
  and by directory id, and **fills in the parent directory id** -- without which
  HyperCard climbs towards the root forever, 712,029 calls in one run.
  `PBGetFCBInfo` reports what is open on a refNum, which HyperCard asks for
  immediately after opening a stack.
- **Standard File (`Pack3`)**, answered from `MRDOC`. A title that cannot find
  its document asks the user, and with no answer it asks forever.
- `MRWATCH=1` also reports when a callee leaves the stack pointer outside the
  address space, naming the callee rather than the eventual victim.

- **Resource enumeration**: `GetIndResource`/`Get1IxResource`,
  `CountTypes`/`Count1Types`, `GetIndType`/`Get1IxType`, `GetResInfo`,
  `SetResInfo`, `LoadResource`. HyperCard walks its own resources by index at
  startup and stops if it cannot; the table the Resource Manager already serves
  is the answer, so these are cheap. 80% -> **81%** of call sites.
- `ParamText` logs its `^0`-`^3` substitutions under `MRTRACE`. A title that
  reports an error by number puts the number there, so it is often the only
  place the program says what went wrong.

### Fixed

- **A start after embedded data is kept even when the decode disagrees.** The
  boundary filter below was too strict on its own: a string constant inside a
  function body drifts the reference decode, and a genuine routine *after* that
  data then looks mid-instruction. A candidate opening with a prologue
  (`link aN,#d` or `movem.l <regs>,-(a7)`) is now kept regardless -- that is the
  stronger evidence, and it is what resynchronises the decode. 99 dropped starts
  become 73, recovering 26 real functions. One of them, `fn_3_1e20`, sits behind
  the string `"23846"` and is HyperCard's WildTalk resource loader; it now runs
  and loads `WTLK 1-4`.
- **Function starts that sat *inside* an instruction** - 99 of them across
  HyperCard's 21 segments. Starts come partly from a linear sweep that drifts
  over embedded data, and the two existing filters (odd addresses, non-68000
  forms) do not catch a candidate that is merely one byte into a longer
  instruction. `confirm_starts` now decodes forward from a start already known
  good and keeps only candidates that land on a real boundary; jump-table
  entries are trusted outright, because the Segment Loader enters there by
  definition.

  The damage was not subtle once traced. A start one byte inside a 6-byte
  `move.l` truncated `fn_3_1296` before its epilogue and turned the remainder
  into a fresh function whose first instruction was `unlk a6` **with no matching
  `link`**. Every call through it walked the caller's frame pointer down four
  bytes, and the corruption surfaced far away as a null pointer handed to
  `_Open` - which is why HyperCard could not open a stack. A6 clobbers per run:
  **61 -> 0**.

### Added

- **`MRWATCH=1`** - checks that a lifted function returns A6 unchanged, and
  names the callee that did not. A6 is the frame pointer, so a callee that
  alters it has corrupted its caller's frame; every `-n(a6)` the caller touches
  afterwards is wrong, far from the call that caused it. This is the instrument
  that found the above.

- **File Manager (`runtime/files.c`)** - a title's own media, served read-only:
  `Open`/`OpenRF`, `Read`, `Close`, `GetEOF`, `Get`/`SetFPos`, `GetFileInfo`,
  `GetVol`/`SetVol`, and the `FSDispatch`/`HFSDispatch` selectors worth
  answering. **OS traps**, so A0 is the parameter block and D0 the result;
  nothing is read off the Pascal stack. Forks are read from the host on demand -
  one CD-ROM's forks are 422 MB, and a stack is read a few hundred bytes at a
  time. Writes report `wrPermErr` rather than succeeding silently: a title told
  it cannot write can say so, one told "fine" loses data. 79% -> **80%** of call
  sites. Checked in `hal_selftest` down to the awkward cases - a read across the
  end delivers a short count *and* `eofErr`, and a read after `Close` is
  `rfNumErr`.
- **`extract_resources.py --forks DIR`** - every file's data and resource forks
  plus a `files.json` index. Extracting one application's resources is not
  enough to serve a File Manager; it needs both forks of every file.

### Removed

- The Finder startup handshake, written and then taken out unused. HyperCard
  1.2.2 never calls `GetAppParms`, so an `AppParmHandle` block for
  `CountAppFiles` to walk was forty lines answering a question nothing asked.
  The layout survives as a comment for whenever a title does ask.

- **HyperCard renders.** 306 -> **372 Toolbox calls**, and the framebuffer is no
  longer blank: it draws its own modal dialog frame, drop shadow and all,
  through the QuickDraw HAL. Screenshot in the README. The box is empty because
  the title asks for `DLOG 0`, which its resource fork does not contain -- an
  early-exit path, not the Home stack.
- **Backward-branch hook (`MR_LOOPTICK`), emitted by the lifter.** Every loop in
  the original code goes round a backward branch, which makes it the one place a
  runtime can get control inside a loop that performs no trap, call or tail
  jump. It does two jobs: it advances the clock (below), and -- built with
  `-DMACRECOMP_LOOPGUARD`, run with `MRMAXLOOPS=<n>` -- it counts branches by
  address and prints the hottest, which names a spinning instruction outright.
  The census is debug-only; the clock is not.

### Fixed

- **A `Ticks` busy-wait that could only be infinite.** Classic Mac code waits on
  the low-memory `Ticks` global (0x16A) for timing; HyperCard spins on it to
  calibrate machine speed:

      0fdc  movea.l #$16a, a4     ; a4 = Ticks
      0fe4  cmp.l   (a4), d7
      0fe6  beq.b   $fe4          ; spin until Ticks changes

  The runtime advanced `Ticks` only from `m68k_call`, and that loop makes no
  calls, so the compare was always equal. A real Mac advanced it from the VBL
  interrupt; there is none here. A backward branch now decrements a counter
  inline and advances `Ticks` every 2048 of them, so time passes inside a loop
  that does nothing else -- an ordinary loop pays an add and a branch. **This is
  not specific to HyperCard**: the same shape would hang any classic-Mac title
  that waits on `Ticks`, which is most of them.

  Found by the loop census: one address took 19,999,889 of 20,000,000 ticks.

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
