#include "opcodes.h"
#include "pvm.h"

const OpcodeInfo kOpcodeTable[] = {
	{"PUSH", 		INSTR_PUSH, 	true},
	{"POP", 		INSTR_POP, 		false},
	{"SWAP", 		INSTR_SWAP, 	false},
	{"DUP", 		INSTR_DUP, 		false},
	{"ADD", 		INSTR_ADD, 		false},
	{"SUB", 		INSTR_SUB, 		false},
	{"MUL", 		INSTR_MUL, 		false},
	{"DIV", 		INSTR_DIV, 		false},
	{"EQ", 			INSTR_EQ, 		false},
	{"LT", 			INSTR_LT, 		false},
	{"JMP", 		INSTR_JMP, 		true},
	{"JZ", 			INSTR_JZ, 		true},
	{"JNZ", 		INSTR_JNZ, 		true},
	{"LOAD", 		INSTR_LOAD, 	true},
	{"STORE", 	INSTR_STORE, 	true},
	{"CALL", 		INSTR_CALL, 	true},
	{"RET", 		INSTR_RET, 		false},
	{"LOADI", 	INSTR_LOADI, 	false},
	{"STOREI", 	INSTR_STOREI, false},
	{"HALT", 		INSTR_HALT, 	false},
	{"AND", 		INSTR_AND, 		false},
	{"OR", 			INSTR_OR, 		false},
	{"XOR", 		INSTR_XOR, 		false},
	{"SHL", 		INSTR_SHL, 		false},
	{"SHR", 		INSTR_SHR, 		false},
	{"NOT", 		INSTR_NOT, 		false},
	{"ENTER", 	INSTR_ENTER, 	false},
	{"LEAVE", 	INSTR_LEAVE, 	false},
	{"LOADFP", 	INSTR_LOADFP, true},
	{"STOREFP",	INSTR_STOREFP,true},
};

const int kOpcodeTableSize = static_cast<int>(sizeof(kOpcodeTable) / sizeof(kOpcodeTable[0]));

const OpcodeInfo* FindOpcodeByName(const std::string& name){
	for(int i = 0; i < kOpcodeTableSize; i++)
		if(name == kOpcodeTable[i].name) return &kOpcodeTable[i];
	return nullptr;
}

const OpcodeInfo* FindOpcodeByValue(int opcode){
	for(int i = 0; i < kOpcodeTableSize; i++)
		if(kOpcodeTable[i].opcode == opcode) return &kOpcodeTable[i];
	return nullptr;
}
