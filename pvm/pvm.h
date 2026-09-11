/*
 * pvm.h
 *
 *  Created on: 3 сент. 2026 г.
 *      Author: userkdi
 */

#ifndef PVM_H_
#define PVM_H_

#include <stdint.h>

#define PVM_CFG_STACK_SIZE			(1024) //word
#define PVM_CFG_RAM_SIZE				(64) //word
#define PVM_CFG_USRSECT_BGN			(0x4000)

typedef int16_t	PVM_Word_t;

/* Forward-declared so a callback can take a pointer to the (still incomplete) state. */
typedef struct PVM_State_s PVM_State_s;

/* An SVC handler: reads its own arguments and pushes its own return value via
 * PVM_Pop/PVM_Push, exactly like any other caller of the VM stack. */
typedef int (*PVM_SvcFunc)(PVM_State_s* state);

struct PVM_State_s {
	uint8_t* code_ptr;
	uint16_t code_len;

	PVM_Word_t* SP;
	PVM_Word_t* FP;
	uint8_t* 		PC;

	PVM_Word_t* usrsect_ptr; /* plain memory-mapped section, e.g. peripheral registers */
	PVM_Word_t 	usrsect_len;

	const PVM_SvcFunc* svc_table;
	PVM_Word_t 				 svc_count;

	PVM_Word_t stack[PVM_CFG_STACK_SIZE];
	PVM_Word_t ram[PVM_CFG_RAM_SIZE];
};

enum PVM_Instruction_e{
	INSTR_PUSH 			=	0x00,
	INSTR_POP 			=	0x01,
	INSTR_SWAP 			=	0x02,
	INSTR_DUP 			=	0x03,
	INSTR_ADD 			=	0x04,
	INSTR_SUB 			=	0x05,
	INSTR_MUL 			=	0x06,
	INSTR_DIV 			=	0x07,
	INSTR_EQ 				=	0x08,
	INSTR_LT 				=	0x09,
	INSTR_JMP 			=	0x0A,
	INSTR_JZ 				=	0x0B,
	INSTR_JNZ 			=	0x0C,
	INSTR_LOAD 			=	0x0D,
	INSTR_STORE 		=	0x0E,
	INSTR_CALL 			=	0x0F,
	INSTR_RET 			=	0x10,
	INSTR_LOADI 		=	0x11,
	INSTR_STOREI 		=	0x12,
	INSTR_HALT 			=	0x13,
	INSTR_AND 			=	0x14,
	INSTR_OR 				=	0x15,
	INSTR_XOR 			=	0x16,
	INSTR_SHL 			=	0x17,
	INSTR_SHR 			=	0x18,
	INSTR_NOT 			=	0x19,
	INSTR_ENTER 		=	0x1A,
	INSTR_LEAVE 		=	0x1B,
	INSTR_LOADFP 		=	0x1C,
	INSTR_STOREFP 	=	0x1D,
	INSTR_SVC 			=	0x1E,
};

enum PVM_ExecErrorCode{
	ERRORCODE_NO_ERROR					=	 0,
	ERRORCODE_OVERFLOW_STACK		=	-1,
	ERRORCODE_EMPTY_STACK				=	-2,
	ERRORCODE_PAIR_STACK				=	-3,
	ERRORCODE_BAD_IADR					=	-4,
	ERRORCODE_BAD_DADR					=	-5,
	ERRORCODE_BAD_SVC					=	-6,
};

enum PVM_ExecStatusCode{
	PVM_STATUS_HALTED					=	 1,
};

int PVM_Init(PVM_State_s* state, char* code, int len);
int PVM_UsrSect(PVM_State_s* state, PVM_Word_t* ptr, int size);
int PVM_RegisterSvc(PVM_State_s* state, const PVM_SvcFunc* table, int count);
int PVM_ExecTick(PVM_State_s* state);

/* Public stack access for SVC handlers (and anyone else embedding the VM). */
int PVM_Pop(PVM_State_s* state, PVM_Word_t* value);
int PVM_Push(PVM_State_s* state, PVM_Word_t value);

void PVM_PrintInstr(PVM_State_s* state);

#endif /* PVM_H_ */
