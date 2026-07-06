/* macrecomp runtime — v0 stub.
 * Real content (m68k state, A5 world, resource mgr, Toolbox HAL) arrives in
 * Phase 4 once shufflepuck-cafe's trap set is enumerated. Kept as a compiling
 * target so `add_subdirectory(ext/macrecomp)` works today. */
#include "macrecomp/macrecomp.h"

#define STR(x) #x
#define VER(a, b) STR(a) "." STR(b)

const char *macrecomp_version(void) {
    return VER(MACRECOMP_VERSION_MAJOR, MACRECOMP_VERSION_MINOR);
}
