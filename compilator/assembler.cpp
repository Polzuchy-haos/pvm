/*
 * assembler.cpp
 *
 * Two-pass assembler for the .pvms text format (see example/bytecode_listing.pvms):
 *   - '#' line comments
 *   - ".import file"  : textually inlines another .pvms file (recursively, cycle-safe)
 *   - ".const NAME V" : named integer constant
 *   - ".entry label"  : moves the block starting at `label` to address 0
 *   - "name:"         : label starting a new code block
 *   - "MNEMONIC [operand]" : one instruction (operand is a number, .const name or label)
 */
#include "assembler.h"
#include "hexutil.h"
#include "opcodes.h"
#include "pvm.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace {

namespace fs = std::filesystem;

struct Line {
	std::string text;
	std::string file;
	int 				lineNo;
};

struct Instr {
	int 				opcode;
	bool 				hasOperand;
	std::string operand;
	int 				address = 0;
	std::string file;
	int 				lineNo;
};

struct Block {
	std::string 			label;
	std::vector<Instr> instrs;
	int 							address = 0;
};

std::string Where(const std::string& file, int lineNo){
	return file + ":" + std::to_string(lineNo) + ": error: ";
}

void Trim(std::string& s){
	size_t end = s.find_last_not_of(" \t\r\n");
	s.erase(end == std::string::npos ? 0 : end + 1);
	size_t start = s.find_first_not_of(" \t\r\n");
	s.erase(0, start == std::string::npos ? s.size() : start);
}

bool StartsWithDirective(const std::string& line, const std::string& name){
	if(line.rfind(name, 0) != 0) return false;
	return line.size() == name.size() || std::isspace((unsigned char)line[name.size()]);
}

bool ParseNumber(const std::string& s, int* out){
	if(s.empty()) return false;
	try{
		size_t pos = 0;
		long v = std::stol(s, &pos, 0);
		if(pos != s.size()) return false;
		*out = static_cast<int>(v);
		return true;
	}
	catch(...){
		return false;
	}
}

bool IsLabelLine(const std::string& line, std::string* label){
	if(line.size() < 2 || line.back() != ':') return false;
	for(size_t i = 0; i + 1 < line.size(); i++)
		if(!(std::isalnum((unsigned char)line[i]) || line[i] == '_')) return false;

	*label = line.substr(0, line.size() - 1);
	return true;
}

fs::path ResolveImportPath(const fs::path& importerPath, const std::string& importName){
	fs::path dir 			= importerPath.parent_path();
	fs::path candidate = dir / importName;

	if(fs::exists(candidate))
		return candidate;

	return dir / (importName + ".pvms");
}

class Assembler {
public:
	AssembleResult Run(const std::string& inputPath){
		LoadFile(inputPath);
		Parse();
		ReorderForEntry();
		AssignAddresses();

		AssembleResult result;
		result.code 		= Encode();
		result.listing 	= GenerateListing(result.code);
		return result;
	}

private:
	std::vector<Line> 									lines_;
	std::vector<std::string> 						importStack_;
	std::unordered_map<std::string, int> consts_;
	std::vector<Block> 									blocks_;
	std::string 												entryLabel_;
	bool 																haveEntry_ = false;
	int 																totalLen_ 	= 0;

	void LoadFile(const std::string& path){
		fs::path norm 			= fs::absolute(path).lexically_normal();
		std::string normStr = norm.string();

		if(std::find(importStack_.begin(), importStack_.end(), normStr) != importStack_.end())
			throw AssemblerError("pvmc: error: circular .import involving '" + path + "'");

		std::ifstream file(path);
		if(!file)
			throw AssemblerError("pvmc: error: cannot open '" + path + "'");

		importStack_.push_back(normStr);

		std::string raw;
		int lineNo = 0;
		while(std::getline(file, raw)){
			lineNo++;

			std::string line = raw;
			size_t hash = line.find('#');
			if(hash != std::string::npos) line.erase(hash);
			Trim(line);
			if(line.empty()) continue;

			if(StartsWithDirective(line, ".import")){
				std::istringstream iss(line.substr(7));
				std::string name;
				iss >> name;
				LoadFile(ResolveImportPath(path, name).string());
				continue;
			}

			lines_.push_back(Line{line, path, lineNo});
		}

		importStack_.pop_back();
	}

	void Parse(){
		Block* current = nullptr;

		for(const Line& l : lines_){
			const std::string& line = l.text;

			if(StartsWithDirective(line, ".entry")){
				std::istringstream iss(line.substr(6));
				iss >> entryLabel_;
				haveEntry_ = true;
				continue;
			}

			if(StartsWithDirective(line, ".const")){
				std::istringstream iss(line.substr(6));
				std::string name, valtext;
				if(!(iss >> name >> valtext))
					throw AssemblerError(Where(l.file, l.lineNo) + "malformed .const directive");

				int value;
				if(!ParseNumber(valtext, &value))
					throw AssemblerError(Where(l.file, l.lineNo) + "invalid constant value '" + valtext + "'");

				consts_[name] = value;
				continue;
			}

			std::string label;
			if(IsLabelLine(line, &label)){
				blocks_.push_back(Block{label, {}, 0});
				current = &blocks_.back();
				continue;
			}

			std::istringstream iss(line);
			std::string mnemonic, operand;
			iss >> mnemonic;
			bool hasOperandToken = static_cast<bool>(iss >> operand);

			const OpcodeInfo* def = FindOpcodeByName(mnemonic);
			if(!def)
				throw AssemblerError(Where(l.file, l.lineNo) + "unknown instruction '" + mnemonic + "'");
			if(def->hasOperand && !hasOperandToken)
				throw AssemblerError(Where(l.file, l.lineNo) + "'" + mnemonic + "' requires an operand");
			if(!def->hasOperand && hasOperandToken)
				throw AssemblerError(Where(l.file, l.lineNo) + "'" + mnemonic + "' takes no operand");

			if(!current){
				blocks_.push_back(Block{"", {}, 0});
				current = &blocks_.back();
			}

			current->instrs.push_back(Instr{def->opcode, def->hasOperand, operand, 0, l.file, l.lineNo});
		}
	}

	void ReorderForEntry(){
		if(!haveEntry_) return;

		/*
		 * Everything from the entry label to end-of-file is one contiguous unit (it may
		 * contain further labels that are just local jump targets, e.g. a loop inside
		 * main), so this is a rotation of the whole block sequence, not "move one block,
		 * keep the rest in place".
		 */
		auto it = std::find_if(blocks_.begin(), blocks_.end(),
			[this](const Block& b){ return b.label == entryLabel_; });

		if(it == blocks_.end())
			throw AssemblerError("pvmc: error: .entry label '" + entryLabel_ + "' not found");

		std::rotate(blocks_.begin(), it, blocks_.end());
	}

	void AssignAddresses(){
		int running = 0;
		for(Block& b : blocks_){
			b.address = running;
			for(Instr& ins : b.instrs){
				ins.address = running;
				running += ins.hasOperand ? 3 : 1;
			}
		}
		totalLen_ = running;
	}

	bool FindLabelAddress(const std::string& name, int* out) const {
		for(const Block& b : blocks_)
			if(!b.label.empty() && b.label == name){ *out = b.address; return true; }
		return false;
	}

	bool ResolveOperand(const std::string& text, int* out) const {
		auto it = consts_.find(text);
		if(it != consts_.end()){ *out = it->second; return true; }
		if(FindLabelAddress(text, out)) return true;
		if(ParseNumber(text, out)) return true;
		return false;
	}

	std::vector<uint8_t> Encode(){
		std::vector<uint8_t> code(totalLen_ > 0 ? totalLen_ : 0, 0);

		for(Block& b : blocks_){
			for(Instr& ins : b.instrs){
				code[ins.address] = static_cast<uint8_t>(ins.opcode);

				if(ins.hasOperand){
					int value;
					if(!ResolveOperand(ins.operand, &value))
						throw AssemblerError(Where(ins.file, ins.lineNo) + "undefined symbol '" + ins.operand + "'");

					code[ins.address + 1] = static_cast<uint8_t>(value & 0xFF);
					code[ins.address + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
				}
			}
		}

		return code;
	}

	std::string GenerateListing(const std::vector<uint8_t>& code) const {
		std::ostringstream out;

		for(const Block& b : blocks_){
			if(!b.label.empty())
				out << b.label << ":\n";

			for(const Instr& ins : b.instrs){
				int size = ins.hasOperand ? 3 : 1;

				out << "0x" << std::uppercase << std::hex << std::right << std::setw(4) << std::setfill('0') << ins.address
						<< std::dec
						<< "  " << std::left << std::setw(9) << std::setfill(' ') << BytesToHex(code, ins.address, size)
						<< "  " << FindOpcodeByValue(ins.opcode)->name;

				if(ins.hasOperand)
					out << " " << ins.operand;

				out << "\n";
			}
		}

		return out.str();
	}
};

} // namespace

AssembleResult PVMC_Assemble(const std::string& input_path){
	Assembler assembler;
	return assembler.Run(input_path);
}
