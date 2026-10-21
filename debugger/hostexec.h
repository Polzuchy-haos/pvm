#ifndef PVMDBG_HOSTEXEC_H_
#define PVMDBG_HOSTEXEC_H_

/*
 * Running a program that lives on the host, on a device that has never had it
 * flashed: the host is the front end of the processor and the device its
 * execution unit.
 *
 * The host holds the bytecode and the program counter. It fetches each
 * instruction and decodes it, and then either
 *   - sends it to the device (EXEC_INSTR), which executes it on its own stack,
 *     RAM, peripherals and SVCs and answers with the status -- everything that
 *     touches data goes this way; or
 *   - carries it out itself, if it is control flow, which only moves the PC:
 *       JMP       the PC moves, the device is not involved
 *       JZ/JNZ    the host reads the top of the device's stack, has the device
 *                 POP it, and moves the PC or not
 *       CALL      the prologue: the device is sent "PUSH <return address>", the
 *                 same word CALL itself would push, so frames, ENTER/LEAVE and
 *                 LOADFP offsets are laid out exactly as compiled; the PC then
 *                 moves into the function
 *       RET       the host reads the return address off the device's stack, has
 *                 it POPped, and carries on after the CALL
 *       HALT      the end
 *
 * Breakpoints are the host's, then: the device never sees a program address,
 * so its four slots do not apply and there is no limit of four either.
 */

#include "transport.h"

#include <stdint.h>

#define HOSTEXEC_BRKP_MAX (32)
#define HOSTEXEC_FUNC_MAX (64)
#define HOSTEXEC_NAME_MAX (32)
#define HOSTEXEC_RESULT_MAX (8) /* words kept from what a call or snippet left */

/* The return address "call" pushes under a function it starts. A program PC is
 * never 0xFFFF, so a RET to it can only mean that function is done. */
#define HOSTEXEC_RET_SENTINEL (0xFFFF)

enum HostStop_e{
  HOSTSTOP_STEP = 0,    /* one instruction done, nothing to report */
  HOSTSTOP_BREAKPOINT,
  HOSTSTOP_HALTED,      /* PC is on a HALT */
  HOSTSTOP_RETURNED,    /* the function "call" started has returned */
  HOSTSTOP_ERROR,       /* see HostExec_s::message */
  HOSTSTOP_INTERRUPTED, /* Ctrl+C */
  HOSTSTOP_NO_ANSWER,   /* the device went quiet */
};

typedef struct{
  char     name[HOSTEXEC_NAME_MAX];
  uint16_t addr;
} HostFunc_s;

typedef struct{
  Transport_s* link;
  char*        frame; /* PVMDBG_CFG_BUF_SIZE bytes */

  uint8_t*     code;
  int          len;
  uint16_t     pc;
  uint16_t     sp, fp; /* the device's, as of the last instruction it ran */

  char         message[128]; /* what went wrong, for HOSTSTOP_ERROR */

  uint16_t     brkp[HOSTEXEC_BRKP_MAX];
  int          brkpCount;

  /* From the listing pvmc -l writes next to the .bin, when there is one. */
  HostFunc_s   funcs[HOSTEXEC_FUNC_MAX];
  int          funcCount;

  /* A "call" in progress, and what the last one to finish left behind. */
  int          calling;
  uint16_t     callReturnPc;
  uint16_t     callBaseSp;
  int16_t      result[HOSTEXEC_RESULT_MAX]; /* top of stack first */
  int          resultCount;
} HostExec_s;

/* Loads the program (and its .lst, if present). 0 on failure, explained on
 * stderr. The PC starts at 0. */
int  HostExecOpen(HostExec_s* hx, Transport_s* link, char* frame, const char* binPath);
void HostExecClose(HostExec_s* hx);

/* PC back to 0, and the device's stack emptied (its RESET). */
void HostExecReset(HostExec_s* hx);

/* One instruction. Returns a HostStop_e: HOSTSTOP_STEP when all went well. */
int  HostExecStep(HostExec_s* hx);

/* Instructions until something stops it. A breakpoint on the instruction the PC
 * is already on is stepped over, the way RUN does on the device. */
int  HostExecRun(HostExec_s* hx);

/* The same, saying whether a breakpoint on the PC it starts from counts: not
 * after stopping there (that is what RUN means), but yes when the PC has just
 * been put there -- "call" into a function with a breakpoint on its entry. */
int  HostExecRunFrom(HostExec_s* hx, int skipBreakHere);

/*
 * Sets up a call to the function at `addr`: pushes `args` in order on the
 * device's stack, then HOSTEXEC_RET_SENTINEL as its return address, and moves
 * the PC there; HostExecRun/Step then execute it. When it returns, whatever is
 * above the stack depth it started from (the arguments, and any result slot
 * passed among them) is copied into `result` and popped, and the PC goes back
 * where it was. Returns 0 with `message` set if it cannot start.
 */
int  HostExecCall(HostExec_s* hx, uint16_t addr, const int16_t* args, int argCount);

/*
 * Runs `code` -- a few instructions assembled on the host, not part of the
 * program -- the same way the program runs: data instructions on the device,
 * control flow here, jump addresses counting from the snippet's own start. The
 * program's PC, its breakpoints and any call in progress are left as they are.
 * Whatever the snippet leaves above the stack depth it started at goes into
 * `result` and is popped, so the stack is as it was -- unless the snippet took
 * the program's own values off it. Returns a HostStop_e: HOSTSTOP_HALTED when
 * it ran to its end.
 */
int  HostExecSnippet(HostExec_s* hx, const uint8_t* code, int len);

/* 1 when set/cleared, 0 with `message` set otherwise. */
int  HostExecBreak(HostExec_s* hx, uint16_t addr, int set);
int  HostExecIsBreak(const HostExec_s* hx, uint16_t addr);

/* Name -> address, and the function an address belongs to (null if unknown). */
int         HostExecFindFunc(const HostExec_s* hx, const char* name, uint16_t* addr);
const char* HostExecFuncAt(const HostExec_s* hx, uint16_t pc);

#endif /* PVMDBG_HOSTEXEC_H_ */
