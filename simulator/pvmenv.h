#ifndef PVM_ENV_H_
#define PVM_ENV_H_

#include "pvm.h"

/*
 * The demo environment a host-side tool hangs off a VM: a small peripheral
 * block mapped at the usrsect base, and the SVC handlers that lib/time.hlc and
 * the examples in example/ expect to find.
 *
 * Shared by pvmsim and pvmdbg rather than copied into each, so that a program
 * behaves the same whether it is run straight through or stepped -- a debugger
 * whose peripherals differ from the simulator's is worse than no debugger.
 *
 * `toolName` prefixes whatever the handlers print, so a line of output says
 * which tool produced it.
 */
void PVMENV_Attach(PVM_State_s* state, const char* toolName);

#endif /* PVM_ENV_H_ */
