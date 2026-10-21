/*
 * pvm.h
 *
 *  Created on: 3 сент. 2026 г.
 *      Author: userkdi
 */

#ifndef PVM_H_
#define PVM_H_

#include <stdint.h>

/* Default pool sizes. The VM itself no longer depends on them -- it works with
 * whatever pools PVM_Init is handed -- they are what the tools (pvmsim, pvmdbg,
 * the assembler's .stack/.ram defaults) size their own pools by. */
#define PVM_CFG_STACK_SIZE			(1024) //word
#define PVM_CFG_RAM_SIZE				(64) //word
#define PVM_CFG_USRSECT_BGN			(0x4000)

/* ENTER saves FP as a stack index in a PVM_Word_t slot, so a stack longer than
 * the largest positive word could not have its frames restored. */
#define PVM_STACK_LEN_MAX				(0x7FFF)

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

	/* Both pools belong to the embedder -- typically static arrays sized for the
	 * program at hand -- and are only borrowed here, see PVM_Init. Keeping them
	 * out of the struct is what lets one firmware give a small program a small
	 * stack without rebuilding the VM, and the debugger read the sizes back. */
	PVM_Word_t* stack;
	uint16_t 		stack_len; /* words */
	PVM_Word_t* ram;
	uint16_t 		ram_len; 	 /* words; valid data addresses are 0..ram_len-1 */
};

/* HALT is 0x00 so that zeroed memory stops the VM: a PC that runs off the end
 * of a program into erased or cleared bytes halts rather than pushing zeros
 * forever. Everything that came before HALT moved up one to make room. */
enum PVM_Instruction_e{
	INSTR_HALT 			=	0x00,
	INSTR_PUSH 			=	0x01,
	INSTR_POP 			=	0x02,
	INSTR_SWAP 			=	0x03,
	INSTR_DUP 			=	0x04,
	INSTR_ADD 			=	0x05,
	INSTR_SUB 			=	0x06,
	INSTR_MUL 			=	0x07,
	INSTR_DIV 			=	0x08,
	INSTR_EQ 				=	0x09,
	INSTR_LT 				=	0x0A,
	INSTR_JMP 			=	0x0B,
	INSTR_JZ 				=	0x0C,
	INSTR_JNZ 			=	0x0D,
	INSTR_LOAD 			=	0x0E,
	INSTR_STORE 		=	0x0F,
	INSTR_CALL 			=	0x10,
	INSTR_RET 			=	0x11,
	INSTR_LOADI 		=	0x12,
	INSTR_STOREI 		=	0x13,
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
	ERRORCODE_BAD_CONFIG				=	-7, /* PVM_Init was handed pools it cannot use */
};

enum PVM_ExecStatusCode{
	PVM_STATUS_HALTED					=	 1,
};

/*
 * `stack` and `ram` are pools of `stack_len` and `ram_len` words that the caller
 * owns and keeps alive for as long as the VM runs, e.g.
 *
 *   static PVM_Word_t vm_stack[PVM_CFG_STACK_SIZE];
 *   static PVM_Word_t vm_ram[PVM_CFG_RAM_SIZE];
 *
 *   PVM_Init(&vm, code, len, vm_stack, PVM_CFG_STACK_SIZE, vm_ram, PVM_CFG_RAM_SIZE);
 *
 * Both are zeroed here. A program with no globals may pass ram = 0, ram_len = 0.
 * Returns ERRORCODE_BAD_CONFIG, leaving the state unusable, for a missing or
 * empty stack, a stack over PVM_STACK_LEN_MAX words, a RAM length without a RAM
 * pointer, or RAM long enough to run into the user section at
 * PVM_CFG_USRSECT_BGN.
 */
int PVM_Init(PVM_State_s* state, char* code, int len,
						 PVM_Word_t* stack, int stack_len,
						 PVM_Word_t* ram, int ram_len);
int PVM_UsrSect(PVM_State_s* state, PVM_Word_t* ptr, int size);
int PVM_RegisterSvc(PVM_State_s* state, const PVM_SvcFunc* table, int count);
int PVM_ExecTick(PVM_State_s* state);

/* Public stack access for SVC handlers (and anyone else embedding the VM). */
int PVM_Pop(PVM_State_s* state, PVM_Word_t* value);
int PVM_Push(PVM_State_s* state, PVM_Word_t value);

/* A data word as LOAD would see it -- RAM or the user section -- or
 * ERRORCODE_BAD_DADR for an address that is neither. */
int PVM_ReadData(PVM_State_s* state, PVM_Word_t adr, PVM_Word_t* data);

/* PVM_PrintInstr lives in pvmprint.h. */

#endif /* PVM_H_ */
