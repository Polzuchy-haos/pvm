#ifndef PVM_DISASSEMBLER_H_
#define PVM_DISASSEMBLER_H_

#include <cstdint>
#include <string>
#include <vector>

struct DisasmInstr {
	int 				address;
	int 				size;				/* instruction length in bytes: 1 or 3 */
	std::string bytesHex;		/* raw bytes, e.g. "0D 01 00" */
	std::string mnemonic;
	std::string operandText;	/* empty if the instruction takes no operand */
};

/*
 * Decodes raw PVM bytecode back into instructions, purely from the bytes
 * (no source/symbol information available, unlike the assembler's listing).
 * An unrecognized opcode byte is emitted as a single-byte "DB 0xXX" pseudo-instruction
 * rather than aborting, so a disassembly never stops partway through.
 */
std::vector<DisasmInstr> PVMD_Disassemble(const std::vector<uint8_t>& code);

#endif /* PVM_DISASSEMBLER_H_ */
