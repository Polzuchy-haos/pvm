/*
 * pvmprint.c
 *
 * PVM_PrintInstr: a one-line trace of the instruction about to execute, for
 * the desktop simulator (pvmsim prints one before every tick).
 *
 * Kept out of pvm.c so that the VM core needs no stdio: a firmware that never
 * traces does not compile printf in, whatever its linker does with unused
 * functions. It only reads the state, through the same public calls anyone
 * embedding the VM has -- data addresses go through PVM_ReadData, so the rules
 * for what is RAM and what is the user section stay written down once, in pvm.c.
 */
#include "pvmprint.h"

#include <stdio.h>

/* Same byte-at-a-time read as pvm.c's _FetchWord, for the same reason: an
 * operand can sit on an odd address. */
static PVM_Word_t FetchWord(const uint8_t* p){
	return (PVM_Word_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int StackDepth(PVM_State_s* state){
	return (int)(state->SP - state->stack);
}

/* index 0 is the top of the stack */
static PVM_Word_t* Top(PVM_State_s* state, int index){
	return state->SP - index - 1;
}

static int IsDataAddr(PVM_State_s* state, PVM_Word_t adr){
	PVM_Word_t data;
	return PVM_ReadData(state, adr, &data) == ERRORCODE_NO_ERROR;
}

void PVM_PrintInstr(PVM_State_s* state){
	uint8_t optcode = *state->PC;
	int adr = state->PC - state->code_ptr;

	switch(optcode){
		case INSTR_PUSH:
			printf(	"0x%X PUSH -> %i SP %i + 1\n", adr,
							FetchWord(state->PC + 1),
							StackDepth(state));
			break;
		case INSTR_POP:
			printf("0x%X POP SP %i - 1\n", adr, StackDepth(state));
			break;
		case INSTR_SWAP:
			printf(	"0x%X SWAP %i %i -> %i %i SP %i\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 0),
							*Top(state, 1),
							StackDepth(state));
			break;
		case INSTR_DUP:
			printf("0x%X DUP %i SP %i + 1\n", adr, *Top(state, 0), StackDepth(state));
			break;
		case INSTR_ADD:
			printf(	"0x%X ADD %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) + *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_SUB:
			printf(	"0x%X SUB %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) - *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_MUL:
			printf(	"0x%X MUL %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) * *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_DIV:
			printf(	"0x%X DIV %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) / *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_EQ:
			printf(	"0x%X EQ %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) == *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_LT:
			printf(	"0x%X LT %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) < *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_JMP:
			printf(	"0x%X JMP -> 0x%X SP %i\n", adr,
							FetchWord(state->PC + 1),
							StackDepth(state));
			break;
		case INSTR_JZ:
		case INSTR_JNZ:
		{
			const char* func;
			int flag;

			if(optcode == INSTR_JZ){
				func = "JZ";
				flag = *Top(state, 0) == 0;
			}
			else{
				func = "JNZ";
				flag = *Top(state, 0) != 0;
			}

			if(flag)
				printf("0x%X %s -> 0x%X SP %i - 1\n", adr,
								func, FetchWord(state->PC + 1), StackDepth(state));
			else
				printf("0x%X %s -> nope SP %i - 1\n", adr, func, StackDepth(state));

		}
			break;
		case INSTR_LOAD:
		{
			PVM_Word_t adr_data = FetchWord(state->PC + 1);
			PVM_Word_t data = 0;

			if(PVM_ReadData(state, adr_data, &data) == ERRORCODE_NO_ERROR)
				printf(	"0x%X LOAD 0x%X -> %i SP %i + 1\n", adr, adr_data,
								 data,
								StackDepth(state));
			else
				printf(	"0x%X LOAD 0x%X -> BadAdr SP %i + 1\n", adr, adr_data,
							StackDepth(state));
		}
			break;
		case INSTR_STORE:
		{
			PVM_Word_t adr_data = FetchWord(state->PC + 1);
			PVM_Word_t data = *Top(state, 0);

			if(IsDataAddr(state, adr_data)){
				printf(	"0x%X STORE 0x%X -> %i SP %i - 1\n", adr, adr_data,
								 data,
								StackDepth(state));
			}
			else
				printf(	"0x%X STORE 0x%X -> BadAdr SP %i - 1\n", adr, adr_data,
								StackDepth(state));
		}
			break;
		case INSTR_LOADI:
		{
			PVM_Word_t adr_data = *Top(state, 0);
			PVM_Word_t data = 0;

			if(PVM_ReadData(state, adr_data, &data) == ERRORCODE_NO_ERROR)
				printf(	"0x%X LOADI 0x%X -> %i SP %i\n", adr, adr_data,
								data, StackDepth(state));
			else
				printf(	"0x%X LOADI 0x%X -> BadAdr SP %i\n", adr, adr_data,
								StackDepth(state));
		}
			break;
		case INSTR_STOREI:
			printf(	"0x%X STOREI 0x%X -> %i SP %i - 2\n", adr,
							*Top(state, 0),
							*Top(state, 1),
							StackDepth(state));
			break;
		case INSTR_HALT:
			printf("0x%X HALT\n", adr);
			break;
		case INSTR_AND:
			printf(	"0x%X AND %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) & *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_OR:
			printf(	"0x%X OR %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) | *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_XOR:
			printf(	"0x%X XOR %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) ^ *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_SHL:
			printf(	"0x%X SHL %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) << *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_SHR:
			printf(	"0x%X SHR %i %i -> %i SP %i - 1\n", adr,
							*Top(state, 1),
							*Top(state, 0),
							*Top(state, 1) >> *Top(state, 0),
							StackDepth(state));
			break;
		case INSTR_NOT:
			printf(	"0x%X NOT %i -> %i SP %i\n", adr,
							*Top(state, 0),
							(PVM_Word_t)~(*Top(state, 0)),
							StackDepth(state));
			break;
		case INSTR_ENTER:
			printf(	"0x%X ENTER FP %i -> %i SP %i + 1\n", adr,
							(int)(state->FP - state->stack),
							(int)(state->SP - state->stack),
							StackDepth(state));
			break;
		case INSTR_LEAVE:
			printf(	"0x%X LEAVE FP %i -> %i SP %i - 1\n", adr,
							(int)(state->FP - state->stack),
							(state->FP > state->stack) ? (int)*(state->FP - 1) : -1,
							StackDepth(state));
			break;
		case INSTR_LOADFP:
		{
			PVM_Word_t offset = FetchWord(state->PC + 1);
			PVM_Word_t* addr = state->FP + offset;

			if(addr >= state->stack && addr < state->stack + state->stack_len)
				printf(	"0x%X LOADFP %i -> %i SP %i + 1\n", adr, offset, *addr, StackDepth(state));
			else
				printf(	"0x%X LOADFP %i -> BadAdr SP %i + 1\n", adr, offset, StackDepth(state));
		}
			break;
		case INSTR_STOREFP:
		{
			PVM_Word_t offset = FetchWord(state->PC + 1);
			PVM_Word_t* addr = state->FP + offset;

			if(addr >= state->stack && addr < state->stack + state->stack_len)
				printf(	"0x%X STOREFP %i -> %i SP %i - 1\n", adr, offset,
								*Top(state, 0), StackDepth(state));
			else
				printf(	"0x%X STOREFP %i -> BadAdr SP %i - 1\n", adr, offset, StackDepth(state));
		}
			break;
		case INSTR_SVC:
			if(StackDepth(state) >= 1)
				printf("0x%X SVC #%i SP %i - 1\n", adr, *Top(state, 0), StackDepth(state));
			else
				printf("0x%X SVC SP %i\n", adr, StackDepth(state));
			break;
		case INSTR_CALL:
			printf(	"0x%X CALL -> 0x%X SP %i + 1\n", adr,
							FetchWord(state->PC + 1),
							StackDepth(state));
			break;
		case INSTR_RET:
			printf(	"0x%X RET -> 0x%X SP %i - 1\n", adr,
							*Top(state, 0),
							StackDepth(state));
			break;
		default:
			break;
		}
}
