#include "disassembler.h"
#include "hexutil.h"
#include "opcodes.h"
#include "pvm.h"

#include <iomanip>
#include <sstream>

namespace {

bool IsAddressOperand(int opcode){
	switch(opcode){
		case INSTR_JMP:
		case INSTR_JZ:
		case INSTR_JNZ:
		case INSTR_CALL:
		case INSTR_LOAD:
		case INSTR_STORE:
			return true;
		default:
			return false;
	}
}

std::string FormatOperand(int opcode, uint8_t lo, uint8_t hi){
	int16_t value = static_cast<int16_t>(static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8));
	std::ostringstream out;

	if(IsAddressOperand(opcode))
		out << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
				<< static_cast<uint16_t>(value);
	else
		out << std::dec << value;

	return out.str();
}

} // namespace

std::vector<DisasmInstr> PVMD_Disassemble(const std::vector<uint8_t>& code){
	std::vector<DisasmInstr> result;
	size_t address = 0;

	while(address < code.size()){
		uint8_t 					 opcodeByte = code[address];
		const OpcodeInfo*  info 			 = FindOpcodeByValue(opcodeByte);

		if(!info){
			DisasmInstr ins;
			ins.address = static_cast<int>(address);
			ins.size 		= 1;
			ins.bytesHex = BytesToHex(code, address, 1);
			ins.mnemonic = "DB";

			std::ostringstream operand;
			operand << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
							<< static_cast<unsigned>(opcodeByte);
			ins.operandText = operand.str();

			result.push_back(ins);
			address += 1;
			continue;
		}

		int size = info->hasOperand ? 3 : 1;
		if(address + static_cast<size_t>(size) > code.size())
			size = static_cast<int>(code.size() - address); /* truncated tail */

		DisasmInstr ins;
		ins.address  = static_cast<int>(address);
		ins.size 		 = size;
		ins.bytesHex = BytesToHex(code, address, size);
		ins.mnemonic = info->name;

		if(info->hasOperand && size == 3)
			ins.operandText = FormatOperand(info->opcode, code[address + 1], code[address + 2]);

		result.push_back(ins);
		address += size;
	}

	return result;
}
