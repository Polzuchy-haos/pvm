/*
 * dbgclient.c
 *
 * Host-side requests to the debug agent, built from fixed 12-byte frames --
 * see dbgclient.h, and pvm/pvmdbg.h for what each command carries.
 */
#include "dbgclient.h"

#include "pvmdbg.h"

#include <string.h>

static uint16_t Get16(const uint8_t* p){
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int DbgCommand(Transport_s* link, char* frame, uint16_t cmd, const uint8_t* arg, int argLen,
							 uint8_t* data){
	const uint8_t* d;
	uint16_t 			 replyCmd;
	int 					 dataLen;
	int 					 len = link->exchange(link, cmd, arg, argLen, frame);

	if(len <= 0 || !PVMDBG_FrameParse(frame, len, &replyCmd, &d, &dataLen)) return -1;

	if(data) memcpy(data, d, PVMDBG_DATA_SIZE);
	return replyCmd;
}

int DbgGetRegs(Transport_s* link, char* frame, uint16_t* sp, uint16_t* fp){
	uint8_t d[PVMDBG_DATA_SIZE];

	if(DbgCommand(link, frame, DBGCMD_REGS, 0, 0, d) != DBGCMD_REGS) return -1;

	*sp = Get16(d);
	*fp = Get16(d + 2);
	return 1;
}

int DbgGetState(Transport_s* link, char* frame, DbgState_s* state){
	uint8_t d[PVMDBG_DATA_SIZE];

	if(DbgCommand(link, frame, DBGCMD_STATE, 0, 0, d) != DBGCMD_STATE) return -1;

	state->pc 		= Get16(d);
	state->mode 	= d[2];
	state->reason = d[3];

	return DbgGetRegs(link, frame, &state->sp, &state->fp);
}

int DbgReadStack(Transport_s* link, char* frame, int16_t* words, int max){
	uint8_t d[PVMDBG_DATA_SIZE];
	int 		n = 0;

	/* Each reply carries the depth, so the walk stops at the bottom without a
	 * separate question -- or at the first refusal, which means the same. */
	while(n < max && n < 256){
		uint8_t index = (uint8_t)n;
		int 		r 		= DbgCommand(link, frame, DBGCMD_READ_STACK, &index, 1, d);

		if(r < 0) 								return -1;
		if(r != DBGCMD_READ_STACK) break;

		words[n++] = (int16_t)Get16(d);
		if(n >= Get16(d + 2)) break;
	}

	return n;
}

int DbgReadStackAt(Transport_s* link, char* frame, uint16_t sp, int index, int16_t* word){
	uint8_t d[PVMDBG_DATA_SIZE];
	uint8_t arg[2];
	int 		r, fromTop;

	if(index < 0 || index >= sp) return 0;

	arg[0] = (uint8_t)(index & 0xFF);
	arg[1] = (uint8_t)(index >> 8);
	r = DbgCommand(link, frame, DBGCMD_READ_STACK_AT, arg, 2, d);
	if(r < 0) return -1;
	if(r == DBGCMD_READ_STACK_AT){ *word = (int16_t)Get16(d); return 1; }

	fromTop = sp - 1 - index;
	if(fromTop > 255) return 0;

	arg[0] = (uint8_t)fromTop;
	r = DbgCommand(link, frame, DBGCMD_READ_STACK, arg, 1, d);
	if(r < 0) 								return -1;
	if(r != DBGCMD_READ_STACK) return 0;

	*word = (int16_t)Get16(d);
	return 1;
}

int DbgReadCode(Transport_s* link, char* frame, uint16_t addr, uint8_t* bytes, int max,
								uint16_t codeLen){
	uint8_t d[PVMDBG_DATA_SIZE];
	int 		n = 0;

	if(addr >= codeLen) return 0;
	if(max > codeLen - addr) max = codeLen - addr;

	while(n < max){
		uint16_t at 	= (uint16_t)(addr + n);
		uint8_t  arg[2] = { (uint8_t)(at & 0xFF), (uint8_t)(at >> 8) };
		int 		 r 		= DbgCommand(link, frame, DBGCMD_READ_CODE, arg, 2, d);

		if(r < 0) 								return -1;
		if(r != DBGCMD_READ_CODE) break; /* past the end */

		for(int i = 0; i < PVMDBG_DATA_SIZE && n < max; i++) bytes[n++] = d[i];
	}

	return n;
}

int DbgMemInfo(Transport_s* link, char* frame, int which, uint16_t* a, uint16_t* b){
	uint8_t d[PVMDBG_DATA_SIZE];
	uint8_t arg = (uint8_t)which;
	int 		r 	= DbgCommand(link, frame, DBGCMD_MEM_INFO, &arg, 1, d);

	if(r < 0) 							 return -1;
	if(r != DBGCMD_MEM_INFO) return 0;

	*a = Get16(d);
	*b = Get16(d + 2);
	return 1;
}

int DbgListBreakpoints(Transport_s* link, char* frame, uint16_t* addrs, int max){
	uint8_t d[PVMDBG_DATA_SIZE];
	int 		n 		= 0;
	int 		slots = 1; /* until the first reply says otherwise */

	for(int slot = 0; slot < slots && slot < 256; slot++){
		uint8_t arg = (uint8_t)slot;
		int 		r 	= DbgCommand(link, frame, DBGCMD_BRKP_LIST, &arg, 1, d);

		if(r < 0) 								return -1;
		if(r != DBGCMD_BRKP_LIST) break;

		slots = d[3];
		if(d[2] && n < max) addrs[n++] = Get16(d);
	}

	return n;
}

int DbgReadData(Transport_s* link, char* frame, uint16_t addr, int16_t* value){
	uint8_t d[PVMDBG_DATA_SIZE];
	uint8_t arg[2] = { (uint8_t)(addr & 0xFF), (uint8_t)(addr >> 8) };
	int 		r 		 = DbgCommand(link, frame, DBGCMD_READ_DATA, arg, 2, d);

	if(r < 0) 								return -1;
	if(r != DBGCMD_READ_DATA) return 0;

	*value = (int16_t)Get16(d);
	return 1;
}
