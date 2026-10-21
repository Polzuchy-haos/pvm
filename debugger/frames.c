/*
 * frames.c
 *
 * The call stack, rebuilt on the host -- see frames.h for the frame layout it
 * relies on.
 */
#include "frames.h"

#include "dbgclient.h"
#include "pvm.h"

#include <string.h>

/* The return address the host's "call" (hostexec.c) plants under a function
 * it starts: there is no frame above it to walk into. */
#define CALL_SENTINEL (0xFFFF)

int FramesWalk(Transport_s* link, char* buf, uint16_t pc, int opcodeAtPc, int maxFrames,
							 CallStack_s* cs){
	uint16_t sp, fp, start = 0;
	int 		 retIndex, nextFp;
	int16_t  word;

	cs->count = 0;
	if(DbgGetRegs(link, buf, &sp, &fp) < 0) return -1;
	cs->sp = sp;

	{
		Frame_s* f = &cs->frames[cs->count++];

		f->pc 	 = pc;
		f->func  = DebugSymFuncAt(pc, &start);
		f->limit = sp;
		f->fp 	 = fp;
		f->built = 1;

		/* Without symbols there is no telling one frame from the next. */
		if(!f->func) return cs->count;

		if(opcodeAtPc < 0 && pc != start){
			uint8_t op;
			int 		n = DbgReadCode(link, buf, pc, &op, 1, 0xFFFF);

			if(n < 0) return -1;
			opcodeAtPc = (n == 1) ? op : -1;
		}

		/* On ENTER the frame is not built yet, and on RET it is already gone --
		 * either way FP is the caller's and the return address is on top. ENTER
		 * would put this function's FP one above it, which is where the
		 * parameters still are counted from. */
		if(pc == start || opcodeAtPc == INSTR_RET){
			f->fp 	 = sp + 1;
			f->built = 0;
			nextFp 	 = fp;
			retIndex = sp - 1;
		}
		else{
			retIndex = fp - 2;
			nextFp 	 = 0;
		}

		/* main has nobody above it; the rest need the caller's saved FP. */
		if(!strcmp(f->func, "main") || maxFrames <= 1) return cs->count;

		if(f->built){
			if(fp < 2) return cs->count;

			switch(DbgReadStackAt(link, buf, sp, fp - 1, &word)){
			case -1: return -1;
			case 0:  return cs->count;
			}
			nextFp = (uint16_t)word;
		}
	}

	while(cs->count < FRAMES_MAX && cs->count < maxFrames){
		Frame_s* f;
		uint16_t ret;

		switch(DbgReadStackAt(link, buf, sp, retIndex, &word)){
		case -1: return -1;
		case 0:  return cs->count;
		}
		ret = (uint16_t)word;
		if(ret == CALL_SENTINEL) return cs->count;

		f = &cs->frames[cs->count++];
		f->pc 	 = ret;
		f->func  = DebugSymFuncAt(ret, 0);
		f->fp 	 = nextFp;
		f->built = 1;
		f->limit = retIndex; /* the callee's arguments and return slot sit below it, and are this frame's */

		/* Not a frame of ours, or a saved FP that does not point below the
		 * frame it came from: stop rather than read nonsense. */
		if(!f->func || f->fp >= f->limit || f->fp < 1){ cs->count--; return cs->count; }
		if(!strcmp(f->func, "main")) 												return cs->count;
		if(f->fp < 2) 																			return cs->count;

		retIndex = f->fp - 2;
		switch(DbgReadStackAt(link, buf, sp, f->fp - 1, &word)){
		case -1: return -1;
		case 0:  return cs->count;
		}
		nextFp = (uint16_t)word;
	}

	return cs->count;
}

const DebugVar_s* FramesFindVar(const Frame_s* frame, const char* name){
	if(!frame->func) return 0;

	for(int i = 0; i < DebugSymVarCount(); i++){
		const DebugVar_s* v = DebugSymVar(i);
		if(!strcmp(v->func, frame->func) && !strcmp(v->name, name)) return v;
	}
	return 0;
}

int FramesReadVar(Transport_s* link, char* buf, const CallStack_s* cs, const Frame_s* frame,
									const DebugVar_s* var, int16_t* value){
	int index = frame->fp + var->offset;

	/* A local exists once its declaration has run, and only in a built frame;
	 * a parameter, as soon as the caller has pushed it. */
	if(!var->isParam && (!frame->built || frame->pc < var->from)) return 0;
	if(index < 0 || index >= frame->limit) 												 return 0;

	return DbgReadStackAt(link, buf, cs->sp, index, value);
}
