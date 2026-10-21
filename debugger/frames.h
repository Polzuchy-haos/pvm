#ifndef PVMDBG_FRAMES_H_
#define PVMDBG_FRAMES_H_

/*
 * The call stack of the program on a device, rebuilt from its stack and the
 * symbols in the .dbgmap (debugsym.h), and the values of the variables in each
 * frame.
 *
 * The frame layout is pvmcc's (hlc/codegen.cpp):
 *
 *   FP-(3+n)        the result slot (functions that return a value)
 *   FP-(n+2-i)      parameter i of n
 *   FP-2            the return address CALL pushed
 *   FP-1            the caller's FP, saved by ENTER
 *   FP+0, FP+1 ..   locals, in the order they are declared
 *
 * so one frame leads to the next through FP-1 and FP-2. Everything is read
 * off the device with READ_STACK_AT; nothing here needs the agent to know what
 * a frame is.
 */

#include "debugsym.h"
#include "transport.h"

#include <stdint.h>

#define FRAMES_MAX (32)

typedef struct{
	uint16_t 		pc;  	 /* frame 0: PC; the others: the return address into them */
	const char* func;  /* null when no symbol covers pc */
	int 				fp; 	 /* the stack index this frame's offsets count from */
	int 				limit; /* words at or above this index are not this frame's */
	int 				built; /* 0 while PC is on ENTER or RET: the frame is not there,
											* its parameters are, its locals are not */
} Frame_s;

typedef struct{
	Frame_s 	frames[FRAMES_MAX];
	int 			count;
	uint16_t 	sp; /* the stack depth when they were read */
} CallStack_s;

/*
 * Walks the stack from `pc`, `maxFrames` frames at most -- 1 when only the
 * current function matters, which spares the round trips the others cost.
 * `opcodeAtPc` is the instruction there if the caller knows it, -1 to have it
 * read off the device. Returns the number of frames (at least 1), or -1 for no
 * answer. Stops at main, at the return address the host's "call" plants
 * (0xFFFF), or at anything that does not look like a frame.
 */
int FramesWalk(Transport_s* link, char* buf, uint16_t pc, int opcodeAtPc, int maxFrames,
							 CallStack_s* cs);

/* A variable of `frame`'s function: 1 with *value read, 0 when it does not
 * exist at that point (not declared yet, or the frame is not built), -1 for no
 * answer. */
int FramesReadVar(Transport_s* link, char* buf, const CallStack_s* cs, const Frame_s* frame,
									const DebugVar_s* var, int16_t* value);

/* The variable `name` of `frame`'s function, or null. */
const DebugVar_s* FramesFindVar(const Frame_s* frame, const char* name);

#endif /* PVMDBG_FRAMES_H_ */
