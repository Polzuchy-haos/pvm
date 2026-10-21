#ifndef PVM_ENV_H_
#define PVM_ENV_H_

#include "pvm.h"

/*
 * The demo environment a host-side tool hangs off a VM: a small peripheral
 * block mapped at the usrsect base, and the SVC handlers that lib/time.hlc and
 * the examples in example/ expect to find:
 *
 *   SVC #0  the system tick (lib/time.hlc)
 *   SVC #1  free -- there is nothing to print to on a board, so there is no
 *           "print" here either; a program shows a result by storing it
 *           (example/: the output register at 0x4007, visible in the trace)
 *   SVC #2  add two numbers: a call that takes arguments and returns a value
 *
 * Shared by pvmsim, pvmdbg and pvmboard rather than copied into each, so that a
 * program behaves the same whether it is run straight through or stepped -- a
 * debugger whose peripherals differ from the simulator's is worse than no
 * debugger.
 */

/* Words of the peripheral block, from PVM_CFG_USRSECT_BGN. */
#define PVMENV_PERIPH_WORDS (8)

void PVMENV_Attach(PVM_State_s* state);

#endif /* PVM_ENV_H_ */
