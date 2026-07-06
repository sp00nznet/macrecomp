<div align="center">

# 🖥️ macrecomp

**Drop-in classic Macintosh for your static recompilation project.**

```
   68k CODE resources                     Your recompiled C
   from a 1980s Mac app                   (one function per 68k sub)
          │                                       │
          │  extract · disassemble · lift         │  calls Toolbox traps
          ▼                                       ▼
   ┌──────────────────────────────────────────────────────┐
   │                     macrecomp                          │
   │   m68k CPU state · A5 world · resource mgr · QuickDraw │
   │   Event/Menu/Window/Dialog Mgr · Sound Mgr             │
   └──────────────────────────────────────────────────────┘
          │
          ▼
   SDL2 window + audio
```

*The System 6/7 Toolbox, chopped into a linkable C library — so you never reverse-engineer QuickDraw twice.*

</div>

---

## What is this?

Static recompilation takes an old program's machine code and turns it into
native C that runs today. For a console game the hard part is the *hardware*
(that's what [snesrecomp](https://github.com/sp00nznet/snesrecomp) provides). For
a **classic Macintosh** application the hard part is the **Toolbox** — the ~1000
ROM/System traps every Mac app calls for graphics, windows, menus, events and
sound.

Every 68k Mac app draws through QuickDraw, pumps `GetNextEvent`, and plays
sound through the Sound Manager. So why reimplement that for every recomp?

**macrecomp** packages the pieces a lifted 68k Mac app needs — a small m68k CPU
state model, the A5 world + segment/jump-table loader, a resource-fork manager,
and a growing HAL that maps Toolbox traps onto **SDL2** — as a linkable library
plus the tooling to get you there. This is the same "chop the platform into
libraries, let each game link against them" approach as N64Recomp and snesrecomp,
aimed at the Mac.

It is the sibling of the x86/DOS [pcrecomp](../tools) toolkit, for 68k + Toolbox
instead of 8086 + DOS INTs.

## The pipeline

```
  App (HFS resource fork: CODE 0..N + PICT/snd/ASND/MENU…)
        │  ① extract     tools/extract_resources.py   (dc42/HFS → CODE + assets + inventory.json)
        ▼
  CODE segments + asset catalog
        │  ② disassemble  tools/disasm_code.py         (capstone M68K)
        │  ③ classify     tools/scan_traps.py          (enumerate A-line Toolbox traps  ← the scope gate)
        ▼
  function table + trap set
        │  ④ lift         tools/lift68k.py             (68k → C against the macrecomp runtime)
        ▼
  C source ──⑤ HAL──▶  QuickDraw→SDL2, traps→runtime ──⑥ build──▶ native app
```

Steps ①–③ are cheap and tell you the real cost of a title *before* you commit to
it: the **trap set** from ③ is exactly the Toolbox surface you must implement.

## Status

🚧 **v0 — tooling first.** The dig tools are real; the runtime HAL grows per game.

| Component | What it does | State |
|-----------|--------------|-------|
| `extract_resources.py` | DiskCopy 4.2 / raw HFS → CODE segments, asset catalog, `inventory.json` | ✅ working |
| `disasm_code.py` | capstone-M68K disassembly of a CODE segment | 🔨 next |
| `scan_traps.py` | enumerate the A-line (`$Axxx`) Toolbox traps a binary calls | 🔨 next |
| `lift68k.py` | mechanical 68k → C lifter | ⬜ |
| runtime: m68k state + A5 world + resource mgr | the execution substrate | ⬜ |
| runtime: QuickDraw → SDL2 (CopyBits, PICT, regions) | the video HAL | ⬜ |
| runtime: Event/Menu/Window/Dialog + Sound Mgr | the OS HAL | ⬜ |

First customer: [**shufflepuck-cafe**](https://github.com/sp00nznet/shufflepuck-cafe)
(Broderbund, 1988) — 6 CODE segments, ~52 KB of 68k, B&W QuickDraw.

## Using macrecomp in your project

Add it as a submodule (this is how the game repos consume it):

```bash
git submodule add https://github.com/sp00nznet/macrecomp.git ext/macrecomp
```

```cmake
# In your game's CMakeLists.txt:
add_subdirectory(ext/macrecomp)
target_link_libraries(my_recomp PRIVATE macrecomp SDL2::SDL2main)
```

Extract a title's resources to start digging:

```bash
python ext/macrecomp/tools/extract_resources.py "MyGame.dc42" -o work/
# → work/code/CODE_0.bin … CODE_N.bin, work/inventory.json, work/assets/…
```

## Repo layout

```
macrecomp/
  tools/      extract / disasm / scan-traps / lift  (the dig kit, Python)
  runtime/    the linkable C library — m68k state, A5 world, Toolbox HAL → SDL2
  include/    public headers (macrecomp/*.h)
  docs/       ARCHITECTURE.md and trap-mapping notes
  examples/   minimal harnesses
```

## Credits & prior art

- **[macresources](https://github.com/tashtego/macresources) + [machfs](https://github.com/tashtego/machfs)** by Elliot Nunn — pure-Python HFS + resource-fork parsing. The extractor stands on these.
- **[capstone](https://www.capstone-engine.org/)** — M68K disassembly.
- *Inside Macintosh* (Apple, 1985–) and the open-source **[Executor](https://github.com/autc04/executor)** clean-room Toolbox — reference semantics for the trap HAL. No Apple ROM or code is used or redistributed here.
- Approach borrowed from **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** and **[snesrecomp](https://github.com/sp00nznet/snesrecomp)**.

## License

MIT — see [LICENSE](LICENSE). The toolkit is original work. It ships **no** Apple
ROM, System software, or copyrighted game data — you bring your own copy of
whatever you're recompiling.
