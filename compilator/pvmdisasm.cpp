/*
 * pvmdisasm.cpp
 *
 * Standalone disassembler: pvmdisasm <input.bin> [-o output.txt]
 * Prints every instruction as "0xADDR  raw bytes  MNEMONIC operand" (stdout by default).
 */
#include "disassembler.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <ostream>
#include <vector>

int main(int argc, char** argv){
	if(argc < 2){
		std::cerr << "usage: " << argv[0] << " <input.bin> [-o output.txt]\n";
		return 1;
	}

	std::string inputPath = argv[1];
	std::string outputPath;
	bool haveOutput = false;

	for(int i = 2; i < argc; i++){
		if(std::string(argv[i]) == "-o" && i + 1 < argc){
			outputPath = argv[++i];
			haveOutput = true;
		}
	}

	std::ifstream in(inputPath, std::ios::binary);
	if(!in){
		std::cerr << "pvmdisasm: error: cannot open '" << inputPath << "'\n";
		return 1;
	}

	std::vector<uint8_t> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::vector<DisasmInstr> instrs = PVMD_Disassemble(code);

	std::ofstream outFile;
	if(haveOutput){
		outFile.open(outputPath);
		if(!outFile){
			std::cerr << "pvmdisasm: error: cannot open output file '" << outputPath << "'\n";
			return 1;
		}
	}
	std::ostream& out = haveOutput ? static_cast<std::ostream&>(outFile) : std::cout;

	for(const DisasmInstr& ins : instrs){
		out << "0x" << std::uppercase << std::hex << std::right << std::setw(4) << std::setfill('0') << ins.address
				<< std::dec
				<< "  " << std::left << std::setw(9) << std::setfill(' ') << ins.bytesHex
				<< "  " << ins.mnemonic;
		if(!ins.operandText.empty())
			out << " " << ins.operandText;
		out << "\n";
	}

	return 0;
}
