/*
 * hostexec.c
 *
 * The host half of running a program that lives on the host -- see hostexec.h
 * for the division of labour. The device half is EXEC_INSTR in pvm/pvmdbg.c.
 *
 * Every rule below mirrors what PVM_ExecTick does for the same instruction, so
 * a program behaves the same whether the VM fetches it from flash or the host
 * feeds it in: same checks, in the same order, with the same error codes.
 */
#include "hostexec.h"

#include "dbgclient.h"
#include "disasm.h"
#include "pvm.h"
#include "pvmdbg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- talking to the device ---------------------------------------------- */

static uint16_t Get16(const uint8_t* p){
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int Fail(HostExec_s* hx, const char* format, ...){
	va_list args;

	va_start(args, format);
	vsnprintf(hx->message, sizeof(hx->message), format, args);
	va_end(args);

	return HOSTSTOP_ERROR;
}

static const char* ErrorName(int status){
	switch(status){
	case ERRORCODE_OVERFLOW_STACK: return "stack overflow";
	case ERRORCODE_EMPTY_STACK: 	 return "stack is empty";
	case ERRORCODE_PAIR_STACK: 		 return "needs two words on the stack";
	case ERRORCODE_BAD_IADR: 			 return "bad code address";
	case ERRORCODE_BAD_DADR: 			 return "bad data address";
	case ERRORCODE_BAD_SVC: 			 return "no such SVC";
	default: 											 return "execution error";
	}
}

/* Has the device execute one instruction. HOSTSTOP_STEP when it went through,
 * otherwise why not -- with the device's own error code when it has one. The
 * length goes along as the fourth byte: the frame always carries four, and the
 * device has to know which of them are the instruction. */
static int Device(HostExec_s* hx, const uint8_t* instr, int size){
	uint8_t arg[PVMDBG_DATA_SIZE] = { instr[0], size > 1 ? instr[1] : 0, size > 2 ? instr[2] : 0, (uint8_t)size };
	uint8_t d[PVMDBG_DATA_SIZE];
	int 		r = DbgCommand(hx->link, hx->frame, DBGCMD_EXEC_INSTR, arg, PVMDBG_DATA_SIZE, d);
	int8_t 	status;

	if(r < 0){
		Fail(hx, "no answer from the debug agent");
		return HOSTSTOP_NO_ANSWER;
	}

	if(r != DBGCMD_EXEC_INSTR)
		return Fail(hx, "the device refused the instruction: it has to be stopped "
										"(or its firmware is too old for EXEC_INSTR)");

	status = (int8_t)d[0];
	hx->sp = Get16(d + 1);

	if(status != ERRORCODE_NO_ERROR)
		return Fail(hx, "execution error %d (%s) at 0x%04X", status, ErrorName(status), hx->pc);

	return HOSTSTOP_STEP;
}

static int DevicePush(HostExec_s* hx, uint16_t value){
	uint8_t instr[3] = { INSTR_PUSH, (uint8_t)(value & 0xFF), (uint8_t)(value >> 8) };
	return Device(hx, instr, 3);
}

static int DevicePop(HostExec_s* hx){
	uint8_t instr = INSTR_POP;
	return Device(hx, &instr, 1);
}

/* The top `count` words of the device's stack, top first; returns how many
 * there were, or -1 for no answer. */
static int DeviceTop(HostExec_s* hx, int16_t* out, int count){
	return DbgReadStack(hx->link, hx->frame, out, count);
}

static int DeviceDepth(HostExec_s* hx, uint16_t* sp){
	uint16_t fp;
	return DbgGetRegs(hx->link, hx->frame, sp, &fp) > 0;
}

/* ---- control flow, done here -------------------------------------------- */

static int ValidTarget(const HostExec_s* hx, int16_t target){
	return target >= 0 && target < hx->len;
}

/* Keeps what lies above stack depth `base` in `result`, then takes it off, so
 * the program's own stack is as it was before a call or a snippet. */
static int TakeAbove(HostExec_s* hx, uint16_t sp, uint16_t base){
	int above = (int)sp - (int)base;
	int n 		= above;

	if(n > HOSTEXEC_RESULT_MAX) n = HOSTEXEC_RESULT_MAX;
	if(n < 0) 									n = 0;

	hx->resultCount = (n > 0) ? DeviceTop(hx, hx->result, n) : 0;
	if(hx->resultCount < 0){ hx->resultCount = 0; return HOSTSTOP_NO_ANSWER; }

	for(int i = 0; i < above; i++){
		int r = DevicePop(hx);
		if(r != HOSTSTOP_STEP) return r;
	}

	return HOSTSTOP_STEP;
}

/* The function "call" started has just been returned from. */
static int FinishCall(HostExec_s* hx){
	int r = TakeAbove(hx, hx->sp, hx->callBaseSp);
	if(r != HOSTSTOP_STEP) return r;

	hx->pc 			= hx->callReturnPc;
	hx->calling = 0;
	return HOSTSTOP_RETURNED;
}

int HostExecStep(HostExec_s* hx){
	uint16_t pc = hx->pc;
	uint8_t  op;
	int 		 size, r;
	int16_t  operand = 0, top;

	if(pc >= hx->len) return Fail(hx, "PC 0x%04X is past the end of the program", pc);

	op 	 = hx->code[pc];
	size = PvmDbgInstrSize(op);

	if(size == 0) 						 return Fail(hx, "0x%02X at 0x%04X is not an instruction", op, pc);
	if(pc + size > hx->len) return Fail(hx, "the instruction at 0x%04X is cut off by the end of the program", pc);

	if(size == 3) operand = (int16_t)Get16(hx->code + pc + 1);

	switch(op){
	case INSTR_HALT:
		return HOSTSTOP_HALTED; /* PC stays on it, as on the device */

	case INSTR_JMP:
		if(!ValidTarget(hx, operand)) return Fail(hx, "JMP to 0x%04X, outside the program", (uint16_t)operand);
		hx->pc = (uint16_t)operand;
		return HOSTSTOP_STEP;

	case INSTR_JZ:
	case INSTR_JNZ:
	{
		int taken;

		r = DeviceTop(hx, &top, 1);
		if(r < 0) 	return HOSTSTOP_NO_ANSWER;
		if(r == 0) return Fail(hx, "%s at 0x%04X: the stack is empty", op == INSTR_JZ ? "JZ" : "JNZ", pc);

		taken = (op == INSTR_JZ) ? (top == 0) : (top != 0);
		if(taken && !ValidTarget(hx, operand))
			return Fail(hx, "jump to 0x%04X, outside the program", (uint16_t)operand);

		r = DevicePop(hx);
		if(r != HOSTSTOP_STEP) return r;

		hx->pc = taken ? (uint16_t)operand : (uint16_t)(pc + 3);
		return HOSTSTOP_STEP;
	}

	case INSTR_CALL:
		if(!ValidTarget(hx, operand)) return Fail(hx, "CALL to 0x%04X, outside the program", (uint16_t)operand);

		/* The prologue CALL would have done itself: the return address goes on
		 * the device's stack, where the callee's frame expects to find it. */
		r = DevicePush(hx, (uint16_t)(pc + 3));
		if(r != HOSTSTOP_STEP) return r;

		hx->pc = (uint16_t)operand;
		return HOSTSTOP_STEP;

	case INSTR_RET:
		r = DeviceTop(hx, &top, 1);
		if(r < 0) 	return HOSTSTOP_NO_ANSWER;
		if(r == 0) return Fail(hx, "RET at 0x%04X: no return address on the stack", pc);

		if(hx->calling && (uint16_t)top == HOSTEXEC_RET_SENTINEL){
			r = DevicePop(hx);
			return (r == HOSTSTOP_STEP) ? FinishCall(hx) : r;
		}

		if(!ValidTarget(hx, top)) return Fail(hx, "RET to 0x%04X, outside the program", (uint16_t)top);

		r = DevicePop(hx);
		if(r != HOSTSTOP_STEP) return r;

		hx->pc = (uint16_t)top;
		return HOSTSTOP_STEP;

	default:
		r = Device(hx, hx->code + pc, size);
		if(r != HOSTSTOP_STEP) return r;

		hx->pc = (uint16_t)(pc + size);
		return HOSTSTOP_STEP;
	}
}

int HostExecRun(HostExec_s* hx){
	int first = 1;

	for(;;){
		int r;

		if(TransportInterrupted()) 												 return HOSTSTOP_INTERRUPTED;
		if(!first && HostExecIsBreak(hx, hx->pc)) return HOSTSTOP_BREAKPOINT;
		first = 0;

		r = HostExecStep(hx);
		if(r != HOSTSTOP_STEP) return r;
	}
}

int HostExecCall(HostExec_s* hx, uint16_t addr, const int16_t* args, int argCount){
	uint16_t base;

	if(hx->calling){ Fail(hx, "a call is already in progress"); return 0; }
	if(!ValidTarget(hx, (int16_t)addr)){ Fail(hx, "0x%04X is outside the program", addr); return 0; }
	if(!DeviceDepth(hx, &base)){ Fail(hx, "no answer from the debug agent"); return 0; }

	for(int i = 0; i < argCount; i++)
		if(DevicePush(hx, (uint16_t)args[i]) != HOSTSTOP_STEP) return 0;

	if(DevicePush(hx, HOSTEXEC_RET_SENTINEL) != HOSTSTOP_STEP) return 0;

	hx->callBaseSp 	 = base;
	hx->callReturnPc = hx->pc;
	hx->pc 					 = addr;
	hx->calling 		 = 1;
	hx->resultCount  = 0;
	return 1;
}

int HostExecSnippet(HostExec_s* hx, const uint8_t* code, int len){
	uint8_t* saveCode 	 = hx->code;
	int 		 saveLen 		 = hx->len;
	uint16_t saveP 			 = hx->pc;
	int 		 saveBrkp 	 = hx->brkpCount;
	int 		 saveCalling = hx->calling;
	uint8_t* buf 				 = malloc((size_t)len + 1);
	uint16_t base, sp;
	int 		 stop, r;

	if(!buf) return Fail(hx, "out of memory");
	if(!DeviceDepth(hx, &base)){ free(buf); Fail(hx, "no answer from the debug agent"); return HOSTSTOP_NO_ANSWER; }

	/* A HALT after it, so a snippet needs none of its own. */
	memcpy(buf, code, (size_t)len);
	buf[len] = INSTR_HALT;

	hx->code 			= buf;
	hx->len 			= len + 1;
	hx->pc 				= 0;
	hx->brkpCount = 0; /* the program's addresses, meaningless in here */
	hx->calling 	= 0;

	stop = HostExecRun(hx);

	hx->code 			= saveCode;
	hx->len 			= saveLen;
	hx->pc 				= saveP;
	hx->brkpCount = saveBrkp;
	hx->calling 	= saveCalling;
	free(buf);

	/* Tidied up even when it failed partway: the program's stack is the point. */
	if(!DeviceDepth(hx, &sp)){ Fail(hx, "no answer from the debug agent"); return HOSTSTOP_NO_ANSWER; }
	r = TakeAbove(hx, sp, base);

	return (stop == HOSTSTOP_HALTED && r != HOSTSTOP_STEP) ? r : stop;
}

void HostExecReset(HostExec_s* hx){
	hx->link->exchange(hx->link, DBGCMD_RESET, 0, 0, hx->frame);
	hx->pc 			= 0;
	hx->sp 			= 0;
	hx->fp 			= 0;
	hx->calling = 0;
}

/* ---- breakpoints and names ---------------------------------------------- */

int HostExecIsBreak(const HostExec_s* hx, uint16_t addr){
	for(int i = 0; i < hx->brkpCount; i++)
		if(hx->brkp[i] == addr) return 1;
	return 0;
}

int HostExecBreak(HostExec_s* hx, uint16_t addr, int set){
	for(int i = 0; i < hx->brkpCount; i++){
		if(hx->brkp[i] != addr) continue;
		if(!set) hx->brkp[i] = hx->brkp[--hx->brkpCount];
		return 1;
	}

	if(!set) return 1; /* clearing what is not set is not an error, as on the device */

	if(addr >= hx->len){
		Fail(hx, "0x%04X is outside the program", addr);
		return 0;
	}
	if(hx->brkpCount >= HOSTEXEC_BRKP_MAX){
		Fail(hx, "all %d breakpoints are in use", HOSTEXEC_BRKP_MAX);
		return 0;
	}

	hx->brkp[hx->brkpCount++] = addr;
	return 1;
}

int HostExecFindFunc(const HostExec_s* hx, const char* name, uint16_t* addr){
	for(int i = 0; i < hx->funcCount; i++){
		if(strcmp(hx->funcs[i].name, name)) continue;
		*addr = hx->funcs[i].addr;
		return 1;
	}
	return 0;
}

const char* HostExecFuncAt(const HostExec_s* hx, uint16_t pc){
	const HostFunc_s* best = 0;

	for(int i = 0; i < hx->funcCount; i++)
		if(hx->funcs[i].addr <= pc && (!best || hx->funcs[i].addr > best->addr)) best = &hx->funcs[i];

	return best ? best->name : 0;
}

/* ---- loading ------------------------------------------------------------ */

/* The "# functions:" block of a pvmc listing: "#   0x0012  square" per line,
 * up to the next bare "#". No listing is not an error -- calls then take
 * addresses rather than names. */
static void LoadFunctions(HostExec_s* hx, const char* binPath){
	char 				line[256];
	char 				path[512];
	const char* dot = strrchr(binPath, '.');
	FILE* 			f;
	int 				inBlock = 0;

	if(!dot || (size_t)(dot - binPath) + 5 >= sizeof(path)) return;
	snprintf(path, sizeof(path), "%.*s.lst", (int)(dot - binPath), binPath);

	f = fopen(path, "r");
	if(!f) return;

	while(fgets(line, (int)sizeof(line), f) && hx->funcCount < HOSTEXEC_FUNC_MAX){
		unsigned addr;
		char 		 name[HOSTEXEC_NAME_MAX];

		if(!strncmp(line, "# functions:", 12)){ inBlock = 1; continue; }
		if(!inBlock) continue;
		if(sscanf(line, "# 0x%x %31s", &addr, name) != 2) break;

		hx->funcs[hx->funcCount].addr = (uint16_t)addr;
		strcpy(hx->funcs[hx->funcCount].name, name);
		hx->funcCount++;
	}

	fclose(f);
}

int HostExecOpen(HostExec_s* hx, Transport_s* link, char* frame, const char* binPath){
	FILE* f = fopen(binPath, "rb");
	long 	size;

	memset(hx, 0, sizeof(*hx));
	hx->link 	= link;
	hx->frame = frame;

	if(!f){
		fprintf(stderr, "pvmdbg: cannot open '%s'\n", binPath);
		return 0;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if(size <= 0 || size > 0xFFFF){
		fprintf(stderr, "pvmdbg: '%s' is not a program (%ld bytes)\n", binPath, size);
		fclose(f);
		return 0;
	}

	hx->code = malloc((size_t)size);
	if(!hx->code || fread(hx->code, 1, (size_t)size, f) != (size_t)size){
		fprintf(stderr, "pvmdbg: cannot read '%s'\n", binPath);
		fclose(f);
		free(hx->code);
		hx->code = 0;
		return 0;
	}

	fclose(f);
	hx->len = (int)size;

	LoadFunctions(hx, binPath);
	return 1;
}

void HostExecClose(HostExec_s* hx){
	free(hx->code);
	hx->code = 0;
}
