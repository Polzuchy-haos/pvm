#ifndef PVM_OPCODES_H_
#define PVM_OPCODES_H_

#include <string>

/*
 * Single source of truth for instruction names used by the host tools
 * (assembler, disassembler): name <-> numeric opcode, and whether the
 * instruction carries a 2-byte immediate operand.
 */
struct OpcodeInfo {
	const char* name;
	int 				opcode;
	bool 				hasOperand;
};

extern const OpcodeInfo kOpcodeTable[];
extern const int 			 kOpcodeTableSize;

const OpcodeInfo* FindOpcodeByName(const std::string& name);
const OpcodeInfo* FindOpcodeByValue(int opcode);

#endif /* PVM_OPCODES_H_ */
