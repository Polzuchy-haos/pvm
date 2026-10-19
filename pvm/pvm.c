/*
 * pvm.c
 *
 *  Created on: 3 сент. 2026 г.
 *      Author: userkdi
 */
#include "pvm.h"
#include <stdio.h>


static int 					_StackItemsNum(PVM_State_s* state);
static int					_StackPush(PVM_State_s* state, PVM_Word_t value);
static int					_StackPop(PVM_State_s* state, PVM_Word_t* value);
static PVM_Word_t* 	_StackIndexOfTop(PVM_State_s* state, int index);
static int					_IsValidDataAddr(PVM_State_s* state, PVM_Word_t adr);
static int					_DataRead(PVM_State_s* state, PVM_Word_t adr, PVM_Word_t* data);
static int					_DataWrite(PVM_State_s* state, PVM_Word_t adr, PVM_Word_t data);
static PVM_Word_t		_FetchWord(const uint8_t* p);


int PVM_Init(PVM_State_s* state, char* code, int len,
						 PVM_Word_t* stack, int stack_len,
						 PVM_Word_t* ram, int ram_len){
	if(stack == 0 || stack_len <= 0 || stack_len > PVM_STACK_LEN_MAX ||
		 ram_len < 0 || (ram == 0 && ram_len > 0) || ram_len > PVM_CFG_USRSECT_BGN)
		return ERRORCODE_BAD_CONFIG;

	state->code_ptr = (uint8_t*)code;
	state->code_len = len;

	state->stack 		 = stack;
	state->stack_len = (uint16_t)stack_len;
	state->ram 			 = ram;
	state->ram_len 	 = (uint16_t)ram_len;

	/* Zeroed here rather than left to the caller: a pool that happens to be a
	 * static array starts out zero anyway, and one that is not would otherwise
	 * hand the program whatever the previous user left behind. */
	for(int i = 0; i < stack_len; i++) stack[i] = 0;
	for(int i = 0; i < ram_len; i++) 	 ram[i] 	= 0;

	state->PC = state->code_ptr;
	state->SP = state->stack;
	state->FP = state->stack;

	return ERRORCODE_NO_ERROR;
}

int PVM_UsrSect(PVM_State_s* state, PVM_Word_t* ptr, int size){
	state->usrsect_ptr = ptr;
	state->usrsect_len = size;

	return ERRORCODE_NO_ERROR;
}

int PVM_RegisterSvc(PVM_State_s* state, const PVM_SvcFunc* table, int count){
	state->svc_table = table;
	state->svc_count = count;

	return ERRORCODE_NO_ERROR;
}

int PVM_Pop(PVM_State_s* state, PVM_Word_t* value){
	return _StackPop(state, value);
}

int PVM_Push(PVM_State_s* state, PVM_Word_t value){
	return _StackPush(state, value);
}

int PVM_ExecTick(PVM_State_s* state){
	int status = ERRORCODE_NO_ERROR;

	uint8_t optcode = *state->PC;

	switch(optcode){
	case INSTR_PUSH:
		status = _StackPush(state, _FetchWord(state->PC + 1));
		if(status == ERRORCODE_NO_ERROR) state->PC += 2;
		break;

	case INSTR_POP:
		status = _StackPop(state, 0);
		break;

	case INSTR_SWAP:
		if(_StackItemsNum(state) >= 2){
			PVM_Word_t *a = _StackIndexOfTop(state, 1);
			PVM_Word_t *b = _StackIndexOfTop(state, 0);
			PVM_Word_t tmp = 	*a;

			*a = *b;
			*b = tmp;
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;

	case INSTR_DUP:
		if(_StackItemsNum(state) > 0)
			status = _StackPush(state, *_StackIndexOfTop(state, 0));
		else
			status = ERRORCODE_EMPTY_STACK;

		break;

	case INSTR_ADD:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) += *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_SUB:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) -= *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_MUL:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) *= *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_DIV:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) /= *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_EQ:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) = *_StackIndexOfTop(state, 1) == *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_LT:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) = *_StackIndexOfTop(state, 1) < *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	/* A target is a signed word, so "< code_len" alone lets a negative one through
	 * (both sides promote to int) and moves PC before the start of the program --
	 * every jump below checks the lower bound too. */
	case INSTR_JMP:
	{
		PVM_Word_t target = _FetchWord(state->PC + 1);

		if(target >= 0 && target < state->code_len)
			state->PC = state->code_ptr + target - 1;
		else
			status = ERRORCODE_BAD_IADR;
	}
		break;
	case INSTR_JZ:
	case INSTR_JNZ:
		if(_StackItemsNum(state) >= 1){
			int flag = 	(optcode == INSTR_JZ) ?
									(!*_StackIndexOfTop(state, 0)) :
									(*_StackIndexOfTop(state, 0));
			if(flag){
				PVM_Word_t target = _FetchWord(state->PC + 1);

				if(target >= 0 && target < state->code_len)
					state->PC = state->code_ptr + target - 1;
				else
					status = ERRORCODE_BAD_IADR;
			}
			else
				state->PC += 2;

			_StackPop(state, 0);

		}
		else
			status = ERRORCODE_EMPTY_STACK;
		break;

	case INSTR_LOAD:
	{
		PVM_Word_t adr = _FetchWord(state->PC + 1);
		PVM_Word_t data = 0;

		status = _DataRead(state, adr, &data);
		if(status == ERRORCODE_NO_ERROR){
			_StackPush(state, data);
			state->PC += 2;
		}
	}
		break;
	case INSTR_STORE:
	{
		PVM_Word_t adr = _FetchWord(state->PC + 1);
		PVM_Word_t data = 0;

		if(_IsValidDataAddr(state, adr)){
			_StackPop(state, &data);
			status = _DataWrite(state, adr, data);
			if(status == ERRORCODE_NO_ERROR) state->PC += 2;
		}
		else
			status = ERRORCODE_BAD_DADR;
	}
		break;
	case INSTR_LOADI:
	{
		PVM_Word_t adr = 0;
		PVM_Word_t data = 0;

		if(_StackItemsNum(state) >= 1){
			_StackPop(state, &adr);
			status = _DataRead(state, adr, &data);
			if(status == ERRORCODE_NO_ERROR)
				_StackPush(state, data);
		}
		else
			status = ERRORCODE_EMPTY_STACK;
	}
		break;
	case INSTR_STOREI:
	{
		PVM_Word_t adr = 0;
		PVM_Word_t data = 0;

		if(_StackItemsNum(state) >= 2){
			_StackPop(state, &adr);
			if(_IsValidDataAddr(state, adr)){
				_StackPop(state, &data);
				status = _DataWrite(state, adr, data);
			}
			else
				status = ERRORCODE_BAD_DADR;
		}
		else
			status = ERRORCODE_PAIR_STACK;
	}
		break;
	case INSTR_HALT:
		status = PVM_STATUS_HALTED;
		break;
	case INSTR_AND:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) &= *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_OR:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) |= *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_XOR:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) ^= *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_SHL:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) <<= *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_SHR:
		if(_StackItemsNum(state) >= 2){
			*_StackIndexOfTop(state, 1) >>= *_StackIndexOfTop(state, 0);
			_StackPop(state, 0);
		}
		else
			status = ERRORCODE_PAIR_STACK;
		break;
	case INSTR_NOT:
		if(_StackItemsNum(state) >= 1)
			*_StackIndexOfTop(state, 0) = ~(*_StackIndexOfTop(state, 0));
		else
			status = ERRORCODE_EMPTY_STACK;
		break;

	case INSTR_ENTER:
		/* Prologue: save the caller's frame pointer (as an index, not a raw pointer,
		 * so it round-trips through a PVM_Word_t stack slot) and open a new frame here. */
		status = _StackPush(state, (PVM_Word_t)(state->FP - state->stack));
		if(status == ERRORCODE_NO_ERROR)
			state->FP = state->SP;
		break;

	case INSTR_LEAVE:
		/* Epilogue: drop this frame's locals and restore the caller's frame pointer,
		 * leaving the return address CALL pushed back on top for RET. */
		if(state->FP > state->stack && state->FP <= state->stack + state->stack_len){
			PVM_Word_t savedFpIndex;

			state->SP = state->FP;
			status = _StackPop(state, &savedFpIndex);
			if(status == ERRORCODE_NO_ERROR)
				state->FP = state->stack + savedFpIndex;
		}
		else
			status = ERRORCODE_BAD_DADR;
		break;

	case INSTR_LOADFP:
	{
		PVM_Word_t offset = _FetchWord(state->PC + 1);
		PVM_Word_t* addr = state->FP + offset;

		if(addr >= state->stack && addr < state->stack + state->stack_len){
			status = _StackPush(state, *addr);
			if(status == ERRORCODE_NO_ERROR) state->PC += 2;
		}
		else
			status = ERRORCODE_BAD_DADR;
	}
		break;

	case INSTR_STOREFP:
	{
		PVM_Word_t offset = _FetchWord(state->PC + 1);
		PVM_Word_t* addr = state->FP + offset;

		if(addr >= state->stack && addr < state->stack + state->stack_len){
			PVM_Word_t data;
			status = _StackPop(state, &data);
			if(status == ERRORCODE_NO_ERROR){
				*addr = data;
				state->PC += 2;
			}
		}
		else
			status = ERRORCODE_BAD_DADR;
	}
		break;

	case INSTR_SVC:
	{
		PVM_Word_t callNum;

		if(_StackItemsNum(state) >= 1){
			_StackPop(state, &callNum);
			if(callNum >= 0 && callNum < state->svc_count && state->svc_table[callNum])
				status = state->svc_table[callNum](state);
			else
				status = ERRORCODE_BAD_SVC;
		}
		else
			status = ERRORCODE_EMPTY_STACK;
	}
		break;

	case INSTR_CALL:
	{
		PVM_Word_t target = _FetchWord(state->PC + 1);

		if(target >= 0 && target < state->code_len){
			_StackPush(state, (state->PC + 3) - state->code_ptr);
			state->PC = state->code_ptr + target - 1;
		}
		else
			status = ERRORCODE_BAD_IADR;
	}
		break;
	case INSTR_RET:
		/* The return address comes off the stack, so unlike CALL/JMP this one also
		 * has to confirm there is something there to read at all. */
		if(_StackItemsNum(state) >= 1){
			PVM_Word_t target = *_StackIndexOfTop(state, 0);

			if(target >= 0 && target < state->code_len){
				state->PC = state->code_ptr + target - 1;
				_StackPop(state, 0);
			}
			else
				status = ERRORCODE_BAD_IADR;
		}
		else
			status = ERRORCODE_EMPTY_STACK;
		break;
	default:
		break;
	}

	if(status == 0)
		state->PC += 1;

	return status;
}

void PVM_PrintInstr(PVM_State_s* state){
	uint8_t optcode = *state->PC;
	int adr = state->PC - state->code_ptr;

	switch(optcode){
		case INSTR_PUSH:
			printf(	"0x%X PUSH -> %i SP %i + 1\n", adr,
							_FetchWord(state->PC + 1),
							_StackItemsNum(state));
			break;
		case INSTR_POP:
			printf("0x%X POP SP %i - 1\n", adr, _StackItemsNum(state));
			break;
		case INSTR_SWAP:
			printf(	"0x%X SWAP %i %i -> %i %i SP %i\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1),
							_StackItemsNum(state));
			break;
		case INSTR_DUP:
			printf("0x%X DUP %i SP %i + 1\n", adr, *_StackIndexOfTop(state, 0), _StackItemsNum(state));
			break;
		case INSTR_ADD:
			printf(	"0x%X ADD %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) + *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_SUB:
			printf(	"0x%X SUB %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) - *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_MUL:
			printf(	"0x%X MUL %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) * *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_DIV:
			printf(	"0x%X DIV %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) / *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_EQ:
			printf(	"0x%X EQ %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) == *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_LT:
			printf(	"0x%X LT %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) < *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_JMP:
			printf(	"0x%X JMP -> 0x%X SP %i\n", adr,
							_FetchWord(state->PC + 1),
							_StackItemsNum(state));
			break;
		case INSTR_JZ:
		case INSTR_JNZ:
		{
			const char* func;
			int flag;

			if(optcode == INSTR_JZ){
				func = "JZ";
				flag = *_StackIndexOfTop(state, 0) == 0;
			}
			else{
				func = "JNZ";
				flag = *_StackIndexOfTop(state, 0) != 0;
			}

			if(flag)
				printf("0x%X %s -> 0x%X SP %i - 1\n", adr,
								func, _FetchWord(state->PC + 1), _StackItemsNum(state));
			else
				printf("0x%X %s -> nope SP %i - 1\n", adr, func, _StackItemsNum(state));

		}
			break;
		case INSTR_LOAD:
		{
			PVM_Word_t adr_data = _FetchWord(state->PC + 1);
			PVM_Word_t data = 0;

			if(_DataRead(state, adr_data, &data) == ERRORCODE_NO_ERROR)
				printf(	"0x%X LOAD 0x%X -> %i SP %i + 1\n", adr, adr_data,
								 data,
								_StackItemsNum(state));
			else
				printf(	"0x%X LOAD 0x%X -> BadAdr SP %i + 1\n", adr, adr_data,
							_StackItemsNum(state));
		}
			break;
		case INSTR_STORE:
		{
			PVM_Word_t adr_data = _FetchWord(state->PC + 1);
			PVM_Word_t data = *_StackIndexOfTop(state, 0);

			if(_IsValidDataAddr(state, adr_data)){
				printf(	"0x%X STORE 0x%X -> %i SP %i - 1\n", adr, adr_data,
								 data,
								_StackItemsNum(state));
			}
			else
				printf(	"0x%X STORE 0x%X -> BadAdr SP %i - 1\n", adr, adr_data,
								_StackItemsNum(state));
		}
			break;
		case INSTR_LOADI:
		{
			PVM_Word_t adr_data = *_StackIndexOfTop(state, 0);
			PVM_Word_t data = 0;

			if(_DataRead(state, adr_data, &data) == ERRORCODE_NO_ERROR)
				printf(	"0x%X LOADI 0x%X -> %i SP %i\n", adr, adr_data,
								data, _StackItemsNum(state));
			else
				printf(	"0x%X LOADI 0x%X -> BadAdr SP %i\n", adr, adr_data,
								_StackItemsNum(state));
		}
			break;
		case INSTR_STOREI:
			printf(	"0x%X STOREI 0x%X -> %i SP %i - 2\n", adr,
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1),
							_StackItemsNum(state));
			break;
		case INSTR_HALT:
			printf("0x%X HALT\n", adr);
			break;
		case INSTR_AND:
			printf(	"0x%X AND %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) & *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_OR:
			printf(	"0x%X OR %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) | *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_XOR:
			printf(	"0x%X XOR %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) ^ *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_SHL:
			printf(	"0x%X SHL %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) << *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_SHR:
			printf(	"0x%X SHR %i %i -> %i SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 1),
							*_StackIndexOfTop(state, 0),
							*_StackIndexOfTop(state, 1) >> *_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		case INSTR_NOT:
			printf(	"0x%X NOT %i -> %i SP %i\n", adr,
							*_StackIndexOfTop(state, 0),
							(PVM_Word_t)~(*_StackIndexOfTop(state, 0)),
							_StackItemsNum(state));
			break;
		case INSTR_ENTER:
			printf(	"0x%X ENTER FP %i -> %i SP %i + 1\n", adr,
							(int)(state->FP - state->stack),
							(int)(state->SP - state->stack),
							_StackItemsNum(state));
			break;
		case INSTR_LEAVE:
			printf(	"0x%X LEAVE FP %i -> %i SP %i - 1\n", adr,
							(int)(state->FP - state->stack),
							(state->FP > state->stack) ? (int)*(state->FP - 1) : -1,
							_StackItemsNum(state));
			break;
		case INSTR_LOADFP:
		{
			PVM_Word_t offset = _FetchWord(state->PC + 1);
			PVM_Word_t* addr = state->FP + offset;

			if(addr >= state->stack && addr < state->stack + state->stack_len)
				printf(	"0x%X LOADFP %i -> %i SP %i + 1\n", adr, offset, *addr, _StackItemsNum(state));
			else
				printf(	"0x%X LOADFP %i -> BadAdr SP %i + 1\n", adr, offset, _StackItemsNum(state));
		}
			break;
		case INSTR_STOREFP:
		{
			PVM_Word_t offset = _FetchWord(state->PC + 1);
			PVM_Word_t* addr = state->FP + offset;

			if(addr >= state->stack && addr < state->stack + state->stack_len)
				printf(	"0x%X STOREFP %i -> %i SP %i - 1\n", adr, offset,
								*_StackIndexOfTop(state, 0), _StackItemsNum(state));
			else
				printf(	"0x%X STOREFP %i -> BadAdr SP %i - 1\n", adr, offset, _StackItemsNum(state));
		}
			break;
		case INSTR_SVC:
			if(_StackItemsNum(state) >= 1)
				printf("0x%X SVC #%i SP %i - 1\n", adr, *_StackIndexOfTop(state, 0), _StackItemsNum(state));
			else
				printf("0x%X SVC SP %i\n", adr, _StackItemsNum(state));
			break;
		case INSTR_CALL:
			printf(	"0x%X CALL -> 0x%X SP %i + 1\n", adr,
							_FetchWord(state->PC + 1),
							_StackItemsNum(state));
			break;
		case INSTR_RET:
			printf(	"0x%X RET -> 0x%X SP %i - 1\n", adr,
							*_StackIndexOfTop(state, 0),
							_StackItemsNum(state));
			break;
		default:
			break;
		}
}


/*
 * Reads a little-endian 16-bit operand byte-by-byte rather than casting to a
 * PVM_Word_t* and dereferencing: bytecode is a packed byte stream, so an
 * operand can land on an odd address, and a typed pointer cast there is
 * undefined behavior. x86 tolerates the resulting unaligned load silently;
 * many ARM Cortex-M cores (M0/M0+ always, M3/M4/M7 when UNALIGN_TRP is set)
 * raise a HardFault on it instead. Single-byte reads never have this problem.
 */
static PVM_Word_t _FetchWord(const uint8_t* p){
	return (PVM_Word_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int _StackItemsNum(PVM_State_s* state){
	return (int)(state->SP - state->stack);
}

static int	_StackPush(PVM_State_s* state, PVM_Word_t value){
	int status = ERRORCODE_NO_ERROR;

	if(_StackItemsNum(state) < state->stack_len){
		*state->SP = value;
		state->SP += 1;
	}
	else
		status = ERRORCODE_OVERFLOW_STACK;

	return status;
}

static int _StackPop(PVM_State_s* state, PVM_Word_t* value){
	int status = ERRORCODE_NO_ERROR;

	if(_StackItemsNum(state) > 0){
		if(value != (void*)0)
			*value = *(state->SP - 1);
		*(state->SP - 1) = 0;
		state->SP -= 1;
	}
	else
		status = ERRORCODE_EMPTY_STACK;

	return status;
}

static PVM_Word_t* _StackIndexOfTop(PVM_State_s* state, int index){
	return state->SP - index - 1;
}

/* PVM_Word_t is signed, so the lower bound is a real check and not a formality:
 * without it a negative address passes "adr < ram_len" and indexes
 * state->ram from before its start. */
static int _IsValidDataAddr(PVM_State_s* state, PVM_Word_t adr){
	return 	(adr >= 0 && adr < state->ram_len) ||
					(state->usrsect_ptr != 0 &&
					 adr >= PVM_CFG_USRSECT_BGN &&
					 adr < PVM_CFG_USRSECT_BGN + state->usrsect_len);
}

static int _DataRead(PVM_State_s* state, PVM_Word_t adr, PVM_Word_t* data){
	int status = ERRORCODE_NO_ERROR;

	if(adr >= 0 && adr < state->ram_len)
		*data = state->ram[adr];
	else if(state->usrsect_ptr != 0 &&
					adr >= PVM_CFG_USRSECT_BGN &&
					adr < PVM_CFG_USRSECT_BGN + state->usrsect_len)
		*data = state->usrsect_ptr[adr - PVM_CFG_USRSECT_BGN];
	else
		status = ERRORCODE_BAD_DADR;

	return status;
}

static int _DataWrite(PVM_State_s* state, PVM_Word_t adr, PVM_Word_t data){
	int status = ERRORCODE_NO_ERROR;

	if(adr >= 0 && adr < state->ram_len)
		state->ram[adr] = data;
	else if(state->usrsect_ptr != 0 &&
					adr >= PVM_CFG_USRSECT_BGN &&
					adr < PVM_CFG_USRSECT_BGN + state->usrsect_len)
		state->usrsect_ptr[adr - PVM_CFG_USRSECT_BGN] = data;
	else
		status = ERRORCODE_BAD_DADR;

	return status;
}


