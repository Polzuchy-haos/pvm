#ifndef PVMDBG_DBGCLIENT_H_
#define PVMDBG_DBGCLIENT_H_

/*
 * The host's side of the debug protocol, one level up from frames: what the
 * session, the machine mode and the --host executor all need to ask a device,
 * written once.
 *
 * A frame carries 4 bytes of data, so anything bigger -- several stack words, a
 * window of code, the breakpoint table -- is assembled here from one request
 * per piece. That is the price of the fixed frame; at 9600 baud each piece is
 * a round trip of a few tens of milliseconds, which is why callers ask for no
 * more than they will show.
 *
 * Every function takes the link and a PVMDBG_CFG_BUF_SIZE frame buffer, and
 * returns -1 when the device did not answer at all.
 */

#include "transport.h"

#include <stdint.h>

typedef struct{
	uint16_t pc; 		 /* 0xFFFF: PC is on the agent's EXEC_INSTR area, in no program */
	uint16_t sp, fp; /* stack depths */
	uint8_t  mode; 	 /* PVMDbgMode_e */
	uint8_t  reason; /* PVMDbgStop_e */
} DbgState_s;

#define DBG_PC_NOT_IN_PROGRAM (0xFFFF)

/* One command: its reply's code and its 4 data bytes (into `data`, which may be
 * null). Returns the reply's command code, or -1 for no answer. */
int DbgCommand(Transport_s* link, char* frame, uint16_t cmd, const uint8_t* arg, int argLen,
							 uint8_t* data);

/* STATE and REGS together. 1, or -1. */
int DbgGetState(Transport_s* link, char* frame, DbgState_s* state);

/* SP and FP alone (REGS). 1, or -1. */
int DbgGetRegs(Transport_s* link, char* frame, uint16_t* sp, uint16_t* fp);

/* Up to `max` words off the top of the stack, top first; how many there were. */
int DbgReadStack(Transport_s* link, char* frame, int16_t* words, int max);

/* Up to `max` bytes of code from `addr`, never past `codeLen` (pass 0xFFFF if
 * it is not known -- the device pads the end with HALT). How many came back;
 * 0 for an address past the end. */
int DbgReadCode(Transport_s* link, char* frame, uint16_t addr, uint8_t* bytes, int max,
								uint16_t codeLen);

/* MEM_INFO `which`: 1 with *a and *b filled in, 0 when the device refused it
 * (an agent that predates it), -1 for no answer. */
int DbgMemInfo(Transport_s* link, char* frame, int which, uint16_t* a, uint16_t* b);

/* The armed breakpoints' addresses, walking the device's table; how many. */
int DbgListBreakpoints(Transport_s* link, char* frame, uint16_t* addrs, int max);

/* A word of data memory -- RAM from 0, the user section from 0x4000. 1 with
 * *value filled in, 0 for an address that is neither, -1 for no answer. */
int DbgReadData(Transport_s* link, char* frame, uint16_t addr, int16_t* value);

#endif /* PVMDBG_DBGCLIENT_H_ */
