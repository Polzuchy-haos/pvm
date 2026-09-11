/*
 * disasm.cpp
 *
 * The one C++ file in the debugger, and only because the disassembler it reuses
 * is written in C++. Everything it exposes is plain C (see disasm.h), so the
 * session itself stays a C program.
 */
#include "disasm.h"

#include "disassembler.h"
#include "opcodes.h"

#include <cstdio>
#include <string>
#include <vector>

int PvmDbgDisasm(const uint8_t* code, int len, int baseAddr, int maxLines,
                 char lines[][PVMDBG_DISASM_LINE_MAX]){
	std::vector<uint8_t> 		 window(code, code + (len > 0 ? len : 0));
	std::vector<DisasmInstr> instrs = PVMD_Disassemble(window);
	int 										 written = 0;

	for(const DisasmInstr& ins : instrs){
		if(written >= maxLines) break;

		/* An instruction whose operand fell outside the window is an artefact of
		 * where the window happened to end, not something to show: printing a bare
		 * "PUSH" whose operand was simply not read yet would be a lie. */
		const OpcodeInfo* info = FindOpcodeByValue(code[ins.address]);
		if(info && info->hasOperand && ins.size < 3) break;

		/* The window's own addresses start at zero; the device's start wherever it
		 * was asked to read from. */
		std::string text = ins.mnemonic;
		if(!ins.operandText.empty()) text += " " + ins.operandText;

		std::snprintf(lines[written], PVMDBG_DISASM_LINE_MAX, "0x%04X  %-9s  %s",
									(unsigned)(baseAddr + ins.address), ins.bytesHex.c_str(), text.c_str());
		written++;
	}

	return written;
}
