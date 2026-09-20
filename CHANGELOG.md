# Changelog

All notable changes to this project are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project uses
[SemVer](https://semver.org/).

## [Unreleased]

### Verified

- **A second, unrelated HyperCard title runs.** Everything so far had been
  measured against the one stack set the fixture shipped with, which cannot
  tell "HyperCard works" from "HyperCard works on this disc". `John C. Dvorak`
  (1990, a sound-button stack from archive.org, nothing to do with the Whole
  Earth Catalog) opens through File > Open Stack and draws: its border art, its
  text field, and all eight buttons. Clicking the home icon runs the stack's
  own script and opens the Home stack, so scripts in a foreign stack dispatch
  and navigate too.

  The stack-format check works as well, which is the other half of the result.
  Handed `The Oracle` -- format 10, written by HyperCard 2.x -- 1.2.2 opens both
  forks, reads the header and refuses with its own message, `New file format
  requires new version of HyperCard.` Most archived collections are format 10
  for the same reason: HyperCard 2 converts a 1.x stack the first time it opens
  one, so a 1.x stack only survives where nothing ever did.

  Neither image is in the repo; both live under gitignored `work/corpus/`.


### Fixed

- **`FMSwapFont` was a null pointer, and calling it corrupted the parser.**
  The Font Manager publishes it through the low-memory vector at `$08E0`
  rather than a trap, and nothing was ever written there. `jsr (a0)` on zero
  does nothing *and* pops none of its arguments, so `fn_17_1818` lost four
  bytes per call, restored A3, A4 and D4-D7 from the wrong slots at its
  epilogue, and took the HyperTalk it was compiling with it. The vector now
  points at a HAL routine that fills an FMOutput and reports the font
  unscaled (1:1), which is what the caller compares numer against denom to
  learn. `fn_17_1818`'s frame balances exactly again (`sp-a6` back to -52).

  With it in place the catalog's section cards draw their *contents* -- the
  HEALTH card lists Nutrition, Cooking, Joy of Cooking, The New Laurel's
  Kitchen and the rest -- where before they were empty frames.

### Added

- **A stack-leak detector, `MRWATCH`.** A callee pops the sentinel return
  address and, in Pascal, its own arguments, so SP always comes back at least
  four bytes higher than it went in. Lower means bytes were left behind, and
  that is the shape of every corruption chased in this repo: the leak walks
  the stack out from under an enclosing frame's saved registers, whose
  epilogue `movem` then restores neighbours instead. It names the leaker
  directly; bisecting the victim by hand is what made the Pack6 one take a
  day. A call to a null address now prints its whole call chain too.

### Known

- A card deeper in still fails with `Can't understand arguments to command
  put`. The argument parser consumes the line cleanly -- the leftover buffer
  is empty -- so this one is not a leak: CODE 14 `0x1d9e` builds a Boolean
  from comparisons of D7 against `a5-0x651e`, `-0x651c` and `-0x651a` and
  branches to the error when it comes out false. Different mechanism,
  unfinished.
- `fn_1_4e98` still leaks 2 bytes once at startup (the `$AC14` auto-pop form
  of `SetFractEnable`). `$A8B5 ScriptUtil` and `$A0FC vCheckLoad` remain
  unimplemented.
- Text measurement is approximate: the HEALTH card wraps "Basics" one
  character early. The FMOutput widMax is a stand-in for one fixed font.


### Fixed

- **The catalog is navigable: `pass idle` compiles, and clicking a card's
  buttons moves between cards.** The cause was not in the parser at all. An
  unimplemented Toolbox trap does not pop its arguments, and Pack6's auto-pop
  glue form (`$ADED`, reached from HyperCard's `fn_1_4d56`) left **14 bytes**
  on the guest stack per call. That walked `fn_11_013e`'s stack pointer out
  from under the registers it had saved, so its epilogue `movem` restored A3
  from the wrong slots; `fn_14_21c4` then wrote `A3-0x54` into HyperCard's
  current-handler global `a5-0x57f0`, which ended up pointing into a dead
  frame. `fn_10_0a8c` -- which enforces the rule that the word after `pass`
  names the enclosing handler -- read a meaningless descriptor from it,
  `getName` handed back a Pascal string of length `0xff`, the comparison
  against a perfectly good `idle` failed, and STR# 1002 item 53 stopped the
  session behind a modal alert.

  `IUMagString`/`IUMagIDString` are now implemented and the frame balances
  exactly (`sp-a6` back to -574). The error is gone and the run navigates
  Home -> the catalog's table of contents -> its section index.

- **`GetTrapAddress` claimed every trap exists.** The Mac way to ask whether a
  machine has a trap is to compare its address with `_Unimplemented`'s;
  handing back a distinct address for all 4096 answers "yes" to every one, so
  HyperCard called `AUXDispatch` -- A/UX only -- and lost six more bytes. Traps
  the HAL does not have are now named and reported absent.

- **`DrawText`, `GetFontInfo`, `GetIcon` and `ShieldCursor` were unimplemented**,
  so they leaked their arguments too. `DrawText` now draws, which is why the
  catalog's section index renders its button text.

### Known

- A card deeper in still fails with `Can't understand arguments to command put`,
  and `fn_17_1818` loses its saved registers the same way. `$A8B5 ScriptUtil`
  and `$A0FC vCheckLoad` remain unimplemented. Pack6's non-comparison
  selectors are still unhandled; guessing their argument sizes made the drift
  worse, so they are left logged rather than guessed at.


### Fixed

- **257 branches to odd addresses, across 75 functions, were being lifted as
  code.** A 68000 fetches on word boundaries, so every `bra`/`bsr`/`jsr`/`jmp`
  target inside a segment is even. capstone will still decode a desynced
  stream into something that reads as `bra $2e07`, and that odd target is
  proof the bytes are not an instruction -- the decode began mid-instruction or
  walked into data. `decode_stream` now records such a word as data and
  resumes after it, the same architectural-impossibility argument
  `not_68000()` already makes for a function start.

  `fn_3_2dfe` was the clearest case: no `link`, no `movem`, a branch to
  `0x2e07`, and it clobbered D5/D6 every time it ran. After the rule the whole
  build has **0** odd code targets (was 257), `find_entries.py` still reports
  0 unreachable targets, and the run has 0 entry misses. The catalog renders
  byte-identically, so nothing regressed.


### Fixed

- **A dialog left its pixels on the screen for good.** `qd_fb` is an overlay --
  a set pixel wins over whatever the title drew into its own screen memory --
  and `dlg_dispose` only marked the slot free. Everything an alert painted
  therefore kept hiding the card underneath it forever. Erasing the dialog's
  rect (plus its drop shadow) *is* the restore: a clear pixel falls through to
  the title's screen memory, which was never damaged.

- **A dialog could be drawn into the title's offscreen buffer.** `dlg_draw` and
  `ctl_draw` used whatever port was current. HyperCard draws its card into an
  offscreen bitmap, so an alert raised mid-draw would erase card art rather
  than cover it. Both now force the screen port and restore it on every exit.

### Corrected

- **The catalog does render, full-screen, in normal operation.** Earlier
  entries here reported that only part of the card reached the screen and that
  an error dialog covered rows 91-168. Both were artifacts of the `MRKEYS`
  probe. A frame sequence (`MRSHOTSEQ`, below) settles it: the card is complete
  at frame 302 and byte-identical for the next 8,255 frames when nothing is
  injected. The ink drop always landed at frame 3002 -- exactly where `MRKEYS`
  fires its first synthetic Return. One overwritten screenshot cannot tell
  "never drew" from "drew, then something else happened"; a sequence can.

### Added

- `MRSHOTSEQ=1` keeps every frame instead of overwriting one file.
- `MRKEYSN=<n>` caps the injected Returns. Unattended Returns get a run past a
  modal dialog, but a title that keeps receiving them walks its own menus and
  quits -- which was silently ending these runs seconds after the dismissal.
- `MRCLICK` now takes a `;`-separated click script, so one run can open a stack
  and then follow a link on the card it lands on.
- `MRTRAPS=1` prints the busiest traps at exit. A full log says nothing about a
  steady-state loop; the shape is in the counts.
- `MRBRKFIND=<text>` finds that text in guest memory and reports every register
  and A5 global pointing into it, plus the text itself.
- `MRDLG=1` prints what a dialog actually says.
- `MRODD=1` reports the first moment SP or A6 goes odd. A 68000 stack is always
  even; once it is not, every later frame is a byte out and reads back garbage
  that looks plausible.
- `MRSTACK` now also applies at an `MRBRK` breakpoint, and `MRBRK` prints the
  frame as text as well as hex -- Pascal passes short strings by value, so the
  argument you want is often *in* the frame rather than behind a pointer.
- `MRWATCHREGS=1` adds the registers and the call chain to each `MRWATCHADDR`
  hit; "who wrote this" is half an answer when the value came from a register
  loaded somewhere else.

### Investigated

- **`pass idle` fails to compile, and that is what halts a session.** The
  failing handler is the *Home* stack's `on idle`, so it fires whatever stack
  is in front. Traced end to end:
  `fn_9_3358` (end-of-statement check) -> `fn_9_02b2` (classifier) -> WTLK 1
  entry 294 = `pass`, type byte 2 -> the type-2 arm -> `fn_10_0a8c`, the
  argument parser reached through the A5 table at `a5-0x31be`, entry 3.

  `fn_10_0a8c` enforces HyperCard's real rule: the word after `pass` must name
  the enclosing handler. It takes that name from `*(a5-0x57f0)`, HyperCard's
  "current handler" pointer, and at the failure that pointer holds **0x3efa4f
  -- an odd address**. A 68000 bus-errors reading a longword there, so the
  descriptor it yields is meaningless; `getName` (CODE 9 `0x1248`) then returns
  a Pascal string of length `0xff` full of zeros instead of `idle`, the
  comparison against the live token (which *is* a correct `idle` on the
  stack) fails, and `fn_9_3306` raises STR# 1002 item 53.

  The bad pointer is written by `fn_14_21c4` as `lea -$54(a3),a0`, with `a3`
  = its argument = a caller's frame pointer, arriving as 0x3efaa3. The string
  pool it all resolves against is fine: `a5-0x2bb0` is WTLK 4, and the bytes
  in memory at the offsets used match the resource exactly.

  Ruled out along the way, each by reading the emitted C against the 68000
  manual: `moveq` sign-extension, the A7 byte-size `-(a7)`/`(a7)+` special
  case, the word-indexed table reads, and the zero table at `a5-0x32ea` --
  that one is *correctly* zero, because all 75 registrations in `fn_3_1378`
  push `clr.l` for it.

  Still open: where the odd frame pointer is made. It is not a misaligned
  stack -- `MRODD` watches SP and A6 at every call entry for a whole run and
  never fires -- and it is not an odd argument into `fn_14_21c4`, which
  `MRBRK`+`MRSTACK` also rule out. The chain that carries it is
  `fn_9_3f1e -> fn_14_298e -> fn_14_2928 -> fn_14_21c4`, and `fn_14_298e`
  passes its own `a6` by value (`move.l a6,-(a7)`), so the next step is a
  step-level trace of that frame rather than another breakpoint.

### Fixed

- **The Window Manager's low-level half was missing, and leaking arguments.**
  `CalcVis`, `CalcVBehind`, `ClipAbove`, `PaintOne`, `PaintBehind`, `SaveOld`
  and `DrawNew` ($A909-$A90F) all fell through to the unimplemented-trap log,
  which does not pop arguments -- so every call left four or eight bytes of
  rubbish on the guest stack. HyperCard calls them when a new stack's window
  appears, which is exactly when a recomp can least afford a corrupted stack.

  They need no real region arithmetic here (one framebuffer, one visible
  window) but they do have to take their arguments off; `DrawNew` also marks
  the window for update. With them in place HyperCard reads **19 blocks** of
  the catalogue instead of 13, including a 7,456-byte one, and **the
  catalogue's card artwork reaches the screen**.

- **OS traps were not setting the condition codes.** The register-based half
  of the trap table returns its result in `D0` *and leaves the flags set from
  it*; compiled code branches on that directly. HyperCard's string-table
  insert is

      a024        _SetHandleSize
      660c        bne.b  <skip the append>

  and with the flags left over from whatever ran before the trap, that branch
  was a coin toss. When it went the wrong way the table was grown and nothing
  written into it, so every later lookup missed.

  That is what made `go to stack "Whole Earth"` resolve to a nameless
  destination: the stack's name is interned into two tables and the
  descriptor carries the pair of indices, so a failed insert produced a null
  reference, `fn_21_3978` concluded "this is the stack we are already in",
  and the open was skipped -- silently, because nothing had gone wrong as far
  as HyperCard could tell.

  **With the flags set, clicking the catalogue's button opens the catalogue**:
  `Open 'Whole Earth'`, `OpenRF 'Whole Earth' -> refNum 20 (41445 bytes)`.
  Toolbox traps (bit 11 set) return on the stack and are left alone.

- **`SFPGetFile` was writing its answer to address 0.** The call takes nine
  arguments -- `where, prompt, fileFilter, numTypes, typeList, dlgHook,
  VAR reply, dlgID, filterProc` -- so `reply` is the **third** thing off the
  stack, not the first. Taking the first pop as the reply handed back
  `filterProc`, which is nil, so the record was written nowhere and every
  caller read an untouched reply, i.e. "cancelled", however well `MRDOC`
  named the document. The sequence also popped 30 bytes rather than 32.
  `SFGetFile` (selector 2) was already right.
- **`PostEvent` ($A02F) was unimplemented.** A title posting an event into its
  own queue expects `GetNextEvent` to hand it back; the trap answered "error"
  instead, and HyperCard abandoned what it was doing. It now goes into the
  same event ring as everything else.

- **Every window's `portRect` was transposed.** `rect_set` takes
  `(left, top, right, bottom)`, and four call sites passed it
  `(0, 0, height, width)` -- so on a 512x342 screen every window came out
  **342 wide and 512 tall**. A title that sizes its own blit from `portRect`
  then writes 43-byte rows into a 64-byte framebuffer, which is exactly how
  HyperCard's window came to be clipped to the left two-thirds of the screen
  while its own offscreen buffer held the full-width picture.

  With it fixed the screen spans all 512 pixels again: guest screen memory
  goes from x 0..341 to x 0..511 and from 10,267 lit pixels to **17,657**.
  `NewWindow`, `GetNewWindow`, the Dialog Manager's window rect and the
  window-bounds table were all affected.

- **Standing the catalogue in for `Home` was itself causing a crash.** With the
  catalogue's Table of Contents stack renamed to `Home`, HyperCard's start-up
  leaves a byte tag at `a5-0x49aa` at 0, `fn_9_1670` dispatches on 1..4 only,
  and the fall-through raises its own assertion -- `ALRT 3003`, *"Unexpected
  error 69320382"* -- and calls `ExitToShell`. With the real `Home` restored
  the same build runs clean: 931,482 traps and 5,611 event-loop polls in the
  run where the substitution managed 818 and zero. The catalogue has to be
  navigated to, not substituted in.

- **HyperTalk did not execute at all, and one missing decode boundary was why.**
  `CODE 12` is entered at offset `0x1322` through a computed jump -- no
  instruction anywhere in the binary names that address, so nothing put a
  boundary there and the linear decode ran straight past it. `fn_12_12ee`
  therefore reached `m68k_entry_miss` and returned *without its epilogue*,
  leaving A6 pointing into its own frame. Its caller is the HyperTalk parser
  (`seg14+0x1d26` -> `fn_14_298e`), so every frame above it was wrong and the
  parser reported `Can't understand what's after "if"` on a script that is
  perfectly valid.

  Lifting `CODE 12` with `--entry 0x1322` closes it. The whole cascade goes:
  nine `returned with A6` failures and every `no function at` error drop to
  **zero**, and HyperCard now runs the stack script -- `hide menuBar` from the
  catalogue's `on openStack` reaches low memory 0x0BAA, which no run had ever
  done before.

  The flag is per-title and has to be passed by hand; the lifter cannot find a
  computed entry by static analysis, which is exactly what `--entry` is for.

- **A synthesised click was never acted on because the mouse did not stay
  under it.** `MRCLICK` posted mouseDown and mouseUp back to back, but a title
  tracks a press with `Button()` and `GetMouse()`, and both reported the *real*
  cursor. HyperCard read that as "moved away before the release" and cancelled
  the click every time. The press now pins the reported position and holds the
  button down, ageing on every route the guest can observe the mouse rather
  than on the event queue alone -- a title that tracks a press stops polling
  `GetNextEvent` while it waits, so a release timed off the event loop never
  arrives.

  The click count also moved out of the platform layer and into the Event
  Manager shim: the modal-dialog loop polls `plat_next_event` directly, long
  before the title reaches its own event loop, and was eating the click.

  **HyperCard now hit-tests the click against the right button**: a click at
  93,96 answers `PtInRect` true against `81,2,112,185`, which is the
  catalogue's first Table of Contents button, the one whose script is
  `go to stack "WHOLE SYSTEMS"`.

### Added

- **Menu Manager: `MenuSelect`, `MenuKey`, `GetMHandle`, `GetItem`, and a real
  `CountMItems`.** All were missing; `MenuSelect` and `MenuKey` also leaked
  their arguments. A title whose only route to a document is *File > Open* had
  no route at all.

  HyperCard 1.x turns a menu choice into a HyperTalk `doMenu "<item text>"`, so
  it needs the item's **text**: it calls `GetMHandle`, then walks each menu with
  `CountMItems` and `GetItem` looking for the name. `CountMItems` answering
  zero meant the search never matched and HyperCard reported `Can't find menu
  item "Open Stack..."`. Both now read the `MENU` resource the Resource Manager
  shim already serves.

  `MRMENU=<menuID>,<item>` makes the choice once, there being no menu to pull
  down. The chain now runs end to end: a click in the menu bar becomes
  `doMenu "Open Stack..."`, which executes and lands in Standard File, answered
  by `MRDOC` with a well-formed `SFReply`. `SetItem` and `CheckItem` now pop
  their arguments as well.

- **`MRWATCH` now reports a callee that pops past its caller's frame.** A
  Pascal callee pops its own arguments, so SP legitimately comes back higher
  than it went in -- but never above the caller's frame pointer, where the
  saved A6 and return address live. The existing A6 check only fires once the
  damage is done, several frames later and in a function that did nothing
  wrong; this one names the callee that did it. It is what found the decode
  boundary above.
- **`MRTEXT=1` echoes what the title draws through `DrawString`.** A dialog a
  title puts up is usually saying exactly what went wrong, and reading it off a
  1-bit framebuffer is much harder than reading it here.
- **`MRHIT=1` reports every `PtInRect` that answers true**, with the point and
  the rectangle -- how a click is confirmed to have reached the right control.
- `MRSHOT` now skips an all-*black* frame as well as an all-white one.
  HyperCard paints the screen solid black as it quits, which was overwriting
  the one frame worth keeping.

- **`ScreenRow` (low memory 0x106) was never set.** It is how a title steps
  from one screen row to the next when it blits with its own code instead of
  going through `CopyBits`. Left at zero, `mulu.w` against it makes every row
  offset zero, so every row lands on the first one and the picture never
  appears. HyperCard's card blitter (`fn_18_1f16`, 3582 calls a run) does
  exactly that.

  **With it set, HyperCard's card window is on screen** -- desktop pattern,
  card area, border and drop shadow -- where before the framebuffer was blank.
  `ScrVRes`, `ScrHRes` and `MBarHeight` are set alongside it.
- **Every Pascal Boolean trap was returning false.** A Boolean result occupies
  the 2-byte result slot but is read as a *byte at the slot's address*: compiled
  code does `move.b (a7)+,d0`, which on a big-endian machine takes the **high**
  byte. `ret16` put the value in the low byte, so `SectRect`, `EmptyRect`,
  `PtInRect`, `EqualRect`, `EmptyRgn`, `EqualRgn`, `PtInRgn`, `RectInRgn`,
  `Button`, `StillDown`, `GetNextEvent`, `EventAvail`, `IsDialogEvent` and
  `BitTst` all answered false however correct the answer was.

  This is what kept the card off the screen. HyperCard's composite,
  `fn_16_05c0`, begins by intersecting the card rect with the region to repaint
  and returns early if they miss; `SectRect` saying "no" every time meant it
  exited on its first instruction, 6850 times a run, and the blit faithfully
  copied an empty buffer. With the convention right, **HyperCard composites the
  card: buffer A goes from 0 to 17657 pixels and a card frame, border and
  content bands appear in it.**

  The HAL selftest asserted the wrong convention -- it read the slot as a word
  and compared with 1, which passes only against a HAL that puts the value in
  the wrong half. It now reads the byte, as a title does.
- **The presented frame is the union of `qd_fb` and guest screen memory.**
  QuickDraw draws into `qd_fb`, but a title that blits with its own code writes
  straight into the screen block, and showing `qd_fb` alone leaves that
  invisible.
- **`ASL` computed its overflow flag from the endpoints.** V is set if the sign
  bit changes at *any* point during the shift, not merely if the first and last
  signs differ: `0x40000000` shifted left twice passes through `0x80000000` and
  back to 0, so V is set even though it starts and ends positive. Comparing only
  the endpoints misses exactly the cases V exists to catch, and every signed
  branch after an `ASL` then takes the wrong arm. Found by adding eight signed
  condition-code cases to the differential check (46 -> **54**), which also
  confirmed `slt`/`sge`/`sgt`/`sle` and `subq`'s V are right.
- **The HyperTalk parse error is gone.** `Can't understand what's after "end"`
  was never about the script: the lifter's linear decode drifted through a table
  of data embedded in `fn_17_0ec6`, and because 68k instructions are
  variable-length it came out the other side one byte off. A branch back into
  the real code then landed on an address the decode had never produced, so the
  lifted function had no label for it and left **without running its
  `movem.l (a7)+,d3-d7/a2` epilogue** -- handing its caller a corrupted card
  index and, further along, a parser working on corrupted state.

  A branch target *is* an instruction boundary, so no instruction may span one.
  The decode now re-synchronises: the bytes up to the target are recorded as
  data and decoding resumes there, repeated until the target set stops growing.
  Only even targets count -- 68k instructions are word-aligned, so an odd one
  came from a decode that had already drifted, and splitting there would
  manufacture instructions that cannot exist.

  HyperCard 1.2.2 on the unmodified Home stack went from stopping at 2450
  Toolbox calls with a parse error to **running clean with no errors at all**.
  `go to next card` also stopped failing (`Unexpected error 836587`).

- **`SectRect` left its destination unwritten when the rectangles did not
  intersect.** Inside Macintosh specifies (0,0,0,0); leaving it alone hands the
  caller whatever happened to be there.
- `MRGFX` now reports `SetPort` with the bitmap it draws into, each `DrawChar`
  with its pen position, and the first `EmptyRect`/`SectRect` calls with their
  operands. Between them these turn "nothing is drawn" into a specific claim
  about which decision skipped the drawing.
- `MRWATCH` now also reports a callee that fails to preserve D3-D7 or A2-A4.
  Mac Pascal preserves them across a call, so a lifted function that returns
  with one altered has corrupted a value its caller still holds -- which shows
  up later as a wrong index with nothing to connect it to the callee that did
  it. That check is what found the drift. It has false positives on
  register-convention leaf helpers (no `link`, no `movem`), which is why it is
  a diagnostic rather than an assertion.

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
- **Non-local exits are now real.** Compiled Pascal unwinds several frames at
  once with `movea.l <saved frame>,a6; lea -n(a6),a7` and then simply carries
  on -- the guest abandons every frame in between. Run as an ordinary jump,
  those abandoned frames stayed on the C stack, ran their epilogues on the way
  out and popped the guest stack again for each one. HyperCard's idle handler
  does this, and it walked SP down past zero: every later read returned 0, so
  arguments arrived null, returns jumped to address 0, and the errors that
  followed had nothing to do with the cause. (Those "no function at 000000"
  messages were the symptom -- I had dismissed them as harmless, and they were
  not.) The fake return address now carries the call depth that pushed it, so a
  return can be told from an unwind, and the C stack unwinds with `longjmp` the
  same way the guest unwinds its own. **Stack and frame corruption reports:
  8 -> 0. Null jumps: dozens -> 0. HyperCard 6768 -> 10000 Toolbox calls.**
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
- **The clip rect was global rather than per-port.** A clip narrowed for one
  offscreen port stayed in force for every port used after it. HyperCard
  measures its font in a port clipped to the top 64 rows, so everything drawn
  below that line afterwards was silently discarded. `SetPort` now restores the
  port's own `clipRgn`, and `ClipRect`/`SetClip` record into it.
- **The handle-size table was a fixed 4096 entries and silently overflowed.**
  Past that, every new handle reported size 0: `SetHandleSize` took its
  "unknown" path, `HandToHand` and `HandAndHand` copied nothing, and
  `GetHandleSize` lied -- all silently, and all only once a run had been going
  a while, which is the worst possible shape for a bug. HyperCard allocates far
  more handles than that. It is now an open-addressed hash that grows on
  demand, which also drops a linear scan from a trap called constantly.
- **Update events are now delivered per window.** A title can have several
  windows open and paints each from its own update event; tracking only "the
  front one" meant just one of HyperCard's five was ever told to repaint.
- **`InvalRect`/`ValidRect` apply to the current port**, not to whichever window
  is front. Validating one window was clearing another's pending update.
- **`ValidRect` and `ValidRgn` validated nothing.** Removing area from the
  update region is the whole point of them; as no-ops they left the window
  permanently dirty, which matters as soon as the update region is real.
- **Windows had no `updateRgn`.** An update event is only half the story: told
  to repaint, a Mac application asks `EmptyRgn(theWindow->updateRgn)` whether
  there is anything to repaint, and a window with no update region at all
  answers "no" and draws nothing. Windows now get a real region, marked dirty
  wherever an update is queued and emptied by `EndUpdate`.
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
