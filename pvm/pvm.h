/*
 * pvm.h
 *
 *  Created on: 3 сент. 2026 г.
 *      Author: userkdi
 */

#ifndef PVM_H_
#define PVM_H_

#include <stdint.h>

#define PVM_CFG_STACK_SIZE			(32) //word
#define PVM_CFG_RAM_SIZE				(32) //word
#define PVM_CFG_USRSECT_BGN			(0x4000)

typedef int16_t	PVM_Word_t;
typedef int (*PVM_FuncSection)(int adr, PVM_Word_t* data, int rw);

typedef struct PVM_State_s {
	uint8_t* code_ptr;
	uint16_t code_len;

	PVM_Word_t* SP;
	PVM_Word_t* FP;
	uint8_t* 		PC;
	void* usrsect_ptr;
	PVM_Word_t usrsect_type;
	PVM_Word_t usrsect_len;

	PVM_Word_t stack[PVM_CFG_STACK_SIZE];
	PVM_Word_t ram[PVM_CFG_RAM_SIZE];
}PVM_State_s;

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
};

enum PVM_ExecErrorCode{
	ERRORCODE_NO_ERROR					=	 0,
	ERRORCODE_OVERFLOW_STACK		=	-1,
	ERRORCODE_EMPTY_STACK				=	-2,
	ERRORCODE_PAIR_STACK				=	-3,
	ERRORCODE_BAD_IADR					=	-4,
	ERRORCODE_BAD_DADR					=	-5,
};

enum PVM_ExecStatusCode{
	PVM_STATUS_HALTED					=	 1,
};

int PVM_Init(PVM_State_s* state, char* code, int len);
int PVM_UsrSect(PVM_State_s* state, void* ptr, int size, int func);
int PVM_ExecTick(PVM_State_s* state);

void PVM_PrintInstr(PVM_State_s* state);

#endif /* PVM_H_ */
