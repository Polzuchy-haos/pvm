#ifndef PVMDBG_SOURCEMAP_H_
#define PVMDBG_SOURCEMAP_H_

#include <stdint.h>

/*
 * The "address -> .hlc file and line" map pvmc writes with -g, which is what
 * lets the session talk about source rather than about bytecode addresses.
 *
 * Everything here is optional: a program assembled from hand-written .pvms has
 * no map, and the session simply says less. Nothing fails for want of one.
 */

/* Reads a .dbgmap. Returns 0 and explains itself on stderr if it cannot. */
int SourceMapLoad(const char* path);

int SourceMapLoaded(void);

/*
 * Where `addr` came from, or null. Addresses between two entries belong to the
 * earlier one -- a single source line usually compiles to several instructions,
 * and only the first of them carries a marker.
 */
const char* SourceMapFileFor(uint16_t addr, int* line);

/*
 * The lowest address generated for `file`:`line`, for setting a breakpoint on a
 * line rather than an address. `file` is matched on its last path component, so
 * "blink.hlc" finds an entry recorded as an absolute path.
 */
int SourceMapAddrFor(const char* file, int line, uint16_t* addr);

/*
 * The text of that source line, or null when the file cannot be read -- it may
 * simply not be on this machine, which is normal when debugging a board that
 * someone else built the firmware for.
 */
const char* SourceMapText(const char* file, int line);

/* Just the file name out of a path, whichever slash it uses. */
const char* SourceMapBaseName(const char* path);

#endif /* PVMDBG_SOURCEMAP_H_ */
