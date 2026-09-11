#ifndef PVMDBG_MACHINE_H_
#define PVMDBG_MACHINE_H_

#include "transport.h"

/*
 * The same session, for a program instead of a person: one command per line on
 * stdin, one reply per line on stdout, until end of input.
 *
 * It exists so that the VS Code extension does not have to reimplement any of
 * this. Frames, CRCs, the serial port, resynchronising a byte stream -- all of
 * that is here, in C, already tested and already proven against a board. The
 * extension only translates, and a second implementation of the wire format in
 * a second language is exactly the thing that would drift.
 *
 * Deliberately not JSON: every value crossing this boundary is a number, so
 * "key=value" pairs separated by spaces need no parser on either side. Source
 * file names never cross it at all -- the extension holds the debug map, which
 * is what it needs anyway to tell VS Code which file to highlight.
 *
 * Returns the process exit code.
 */
int MachineRun(Transport_s* link);

#endif /* PVMDBG_MACHINE_H_ */
