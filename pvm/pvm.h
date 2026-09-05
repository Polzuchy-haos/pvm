/*
 * pvm.h
 *
 *  Created on: 3 сент. 2026 г.
 *      Author: userkdi
 */

#ifndef PVM_H_
#define PVM_H_

#include <stdint.h>

#define PVM_CFG_STACK_SIZE			(8) //word
#define PVM_CFG_RAM_SIZE				(4) //word
#define PVM_CFG_USRSECT_BGN			(0x4000)

typedef int16_t	PWM_Word_t;
typedef int (*PWM_FuncSection)(int adr, PWM_Word_t* data, int rw);

typedef struct PWM_State_s {
	uint8_t* code_ptr;
	uint16_t code_len;

	PWM_Word_t* SP;
	uint8_t* 		PC;
	void* usrsect_ptr;
	PWM_Word_t usrsect_type;
	PWM_Word_t usrsect_len;

	PWM_Word_t stack[PVM_CFG_STACK_SIZE];
	PWM_Word_t ram[PVM_CFG_RAM_SIZE];
}PWM_State_s;

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

int PWM_Init(PWM_State_s* state, char* code, int len);
int PWM_UsrSect(PWM_State_s* state, void* ptr, int size, int func);
int PWM_ExecTick(PWM_State_s* state);

void PWM_PrintInstr(PWM_State_s* state);

#endif /* PVM_H_ */
