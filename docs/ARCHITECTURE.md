# macrecomp architecture

Two halves: a **dig kit** (Python, `tools/`) that turns a Mac app into liftable
inputs, and a **runtime** (C, `runtime/` + `include/`) the lifted code links
against.

## Why the Toolbox is the whole game

A 68k Mac app is not a flat executable. Its 68k code lives in numbered `CODE`
resources loaded by the **Segment Loader** through the **A5 world** (a per-app
globals block whose jump table dispatches inter-segment calls). The code barely
touches hardware directly — it calls the **Toolbox** via *A-line traps*: opcodes
in the `$A000–$AFFF` range that trap into ROM/System routines like
`_CopyBits`, `_GetNextEvent`, `_NewWindow`, `_MenuSelect`, `_SndPlay`.

So recompiling a Mac app is 20% lifting 68k arithmetic and 80% providing the
Toolbox those traps expect. macrecomp's runtime *is* that Toolbox, mapped to
SDL2, implemented lazily per game — you only build the traps a title actually
calls, which `tools/scan_traps.py` enumerates up front.

## The A5 world (what the loader rebuilds)

```
        low mem
   ┌───────────────┐
   │  application   │  ← A5 points here
   │  globals       │
   ├───────────────┤ A5
   │  jump table    │  CODE 0 defines it; each 8-byte entry
   │  (CODE 0)      │  is a "load segment N + jump" thunk
   ├───────────────┤
   │  QuickDraw     │
   │  globals       │
   └───────────────┘
```

The runtime allocates an A5 world, installs CODE 0's jump table, and resolves
each entry to a lifted C function instead of a 68k segment-load thunk.

## Trap HAL mapping (grows per game)

| Toolbox manager | Traps (examples) | SDL2 backing |
|-----------------|------------------|--------------|
| QuickDraw | `_CopyBits` `_DrawPicture` `_PaintRect` `_FrameRgn` | software 1-bit framebuffer → RGBA texture |
| Event Mgr | `_GetNextEvent` `_WaitNextEvent` `_Button` | SDL event queue → EventRecord |
| Window/Menu/Dialog | `_NewWindow` `_MenuSelect` `_ModalDialog` | rendered chrome + hit-testing |
| Sound Mgr | `_SndPlay` `_SndNewChannel` | SDL_mixer / SDL audio |
| Memory/Resource | `_NewHandle` `_GetResource` | host malloc + the extracted resource map |

## Reference, not reuse

Trap *semantics* come from *Inside Macintosh* and the open-source Executor
clean-room Toolbox. No Apple ROM, System file, or copyrighted app data is
included or redistributed. Each recomp supplies its own extracted resources at
build time from a user-provided disk image.
