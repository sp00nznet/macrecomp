# Roadmap

What's next, what's deferred, what's out of scope. Ordered by measured need, not
by guess — the numbers come from `scan_traps.py --coverage`.

## Where the HAL stands

| Title | 68k | CODE segs | Distinct traps | Call sites | Sites covered | Traps covered |
|---|---|---|---|---|---|---|
| Shufflepuck Cafe (1988) | ~53 KB | 6 | 182 | 995 | **92%** | 69% |
| HyperCard 1.2.2 (1988) | 326 KB | 22 | 418 | 3166 | **77%** | 47% |

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

The remaining blocker is an **indirect jump to a mid-function address**.

`m68k_jump: no function at 5210bc` repeats forever. That is segment 16 offset
`0x10bc`, which sits *inside* `fn_16_0fea` -- a function that disassembles
cleanly from its real start (`link.w a6,#$fff4` / `movem.l d3-d7/a2-a4,-(a7)`,
through to `unlk`/`rts`). Nothing in the generated code jumps there literally, so
it is a register-indirect `jmp` whose target is computed at run time.

The function table maps *function starts* to C functions, so it cannot enter a
function partway. That is the structural limit reached here. The fix is
**entry-point dispatch**: the lifter already emits an `L<addr>:` label for every
branch target, so each lifted function can take a prologue that jumps to a label
by address, and `m68k_jump` can resolve any address inside a known function
rather than only its first instruction.

Two things worth knowing before touching the boundary code again:

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
| 80 | 21 | **TextEdit** | `TENew`, `TESetText`, `TEUpdate`, `TEClick`. HyperCard's fields are TextEdit; the dialog edit items are stubs until this lands. |
| 41 | 14 | Window Manager | `MoveWindow`, `FindWindow`, `DragWindow` — real windows rather than one full-screen port. |
| 39 | 20 | File Manager | `HFSDispatch`, `FSDispatch`. Needed to open a stack at all. |
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

Required by house rules and not yet built. The measurement already exists —
`--coverage` prints a sites/traps figure per title — so the harness is the loop
around it:

- Run extract → scan → coverage over a fixed corpus, one row per title.
- Fail on regression in covered-sites count, not merely on zero.
- Surface the current figures in this file's table.

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
