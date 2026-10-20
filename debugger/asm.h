#ifndef PVMDBG_ASM_H_
#define PVMDBG_ASM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bytecode for a snippet to run on the device (the debugger's "exec"), made by
 * the assembler pvmc uses rather than a second one living here.
 *
 * Both write at most `outMax` bytes to `out` and return how many, or -1 with a
 * ready-to-print message in `err` -- an assembler diagnostic, or the snippet
 * being bigger than `outMax`.
 *
 * PvmDbgAssembleText takes instructions separated by ';' ("LOAD 3; PUSH 1;
 * ADD; STORE 3"), which is the shape a command line lends itself to. `ramLen`
 * and `stackLen` are the device's, so an address is checked against the RAM
 * the board actually has rather than the default; pass -1 for either to fall
 * back to the assembler's default. Diagnostics name the snippet "snippet", and
 * its line numbers count the ';'-separated pieces.
 */
int PvmDbgAssembleFile(const char* path, uint8_t* out, int outMax, char* err, int errLen);
int PvmDbgAssembleText(const char* text, int ramLen, int stackLen,
                       uint8_t* out, int outMax, char* err, int errLen);

#ifdef __cplusplus
}
#endif

#endif /* PVMDBG_ASM_H_ */
