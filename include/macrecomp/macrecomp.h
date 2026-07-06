/* macrecomp — drop-in classic Macintosh Toolbox for 68k static recompilation.
 *
 * This is the v0 public surface. The runtime substrate (m68k CPU state, the A5
 * world + segment loader, the resource manager, and the QuickDraw/Event/Sound
 * HAL onto SDL2) lands per-game starting in Phase 4 — see README "Status".
 * For now this exposes only version/init so the submodule + CMake integration
 * is real from day one and grows in place. */
#ifndef MACRECOMP_H
#define MACRECOMP_H

#ifdef __cplusplus
extern "C" {
#endif

#define MACRECOMP_VERSION_MAJOR 0
#define MACRECOMP_VERSION_MINOR 1

/* Human-readable "0.1" etc. Never NULL. */
const char *macrecomp_version(void);

#ifdef __cplusplus
}
#endif
#endif /* MACRECOMP_H */
