/*
 * pvmprint.h
 *
 * Instruction tracing for desktop tools. Not needed to run the VM, and kept
 * apart from it so that the core stays free of stdio -- see pvmprint.c.
 */
#ifndef PVMPRINT_H_
#define PVMPRINT_H_

#include "pvm.h"

/* Prints the instruction at PC, with the operands it is about to use, to
 * stdout: "0x1C ADD 3 4 -> 7 SP 2 - 1". Changes nothing. */
void PVM_PrintInstr(PVM_State_s* state);

#endif /* PVMPRINT_H_ */
