/*
 * assembler.cpp
 *
 * Two-pass assembler for the .pvms text format (see example/bytecode_listing.pvms):
 *   - '#' line comments
 *   - ".import file"  : textually inlines another .pvms file (recursively, cycle-safe)
 *   - ".const NAME V" : named integer constant
 *   - ".line N path"  : ties the next instruction to a line of .hlc source
 *   - ".func NAME" ... ".endfunc"
 *                     : the instructions between belong to function NAME, for
 *                       the debugger's symbols (see GenerateDebugMap)
 *   - ".param NAME OFFSET" / ".local NAME OFFSET"
 *                     : a variable of the current function, OFFSET words from
 *                       FP, there from the next instruction on
 *   - ".var NAME ADDR SIZE"
 *                     : named variable -- resolves like .const, but also carries
 *                       its size into the listing's memory map and gets its whole
 *                       span bounds-checked
 *   - ".entry label"  : moves the block starting at `label` to address 0
 *   - ".code N" / ".ram N" / ".stack N" / ".usrsect BASE LEN"
 *                     : declare the target VM's section sizes (see Sections below)
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
#include <set>
#include <sstream>
#include <unordered_map>

namespace {

namespace fs = std::filesystem;

struct Line {
	std::string text;
	std::string file;
	int 				lineNo;
};

/* A ".param" or ".local": where in its function's frame a variable lives. */
struct FrameVar {
	std::string kind; /* "param" or "local" */
	std::string name;
	int 				offset; /* words from FP */
};

struct Instr {
	int 				opcode;
	bool 				hasOperand;
	std::string operand;
	int 				address = 0;
	std::string file;		/* the .pvms file and line, for diagnostics */
	int 				lineNo;

	/* Where in the original .hlc source this came from, when a ".line" said so.
	 * Kept on the instruction rather than tied to an address, so it travels with
	 * the instruction when .entry reorders the blocks. */
	bool 				hasDebugLine = false;
	std::string debugFile;
	int 				debugLine 	 = 0;

	/* The function it belongs to (an index into funcNames_, -1 for none), and the
	 * frame variables that exist from this instruction on -- also kept on the
	 * instruction, for the same reason as the debug line. */
	int 									func = -1;
	std::vector<FrameVar> vars;
};

struct Block {
	std::string 			label;
	std::vector<Instr> instrs;
	int 							address = 0;
};

/* A ".var" -- a named data address with a known extent, as opposed to a ".const",
 * which is just a number that may or may not be an address at all. */
struct Var {
	std::string name;
	int 				address;
	int 				size; 	/* in words */
	std::string file;
	int 				lineNo;
};

/* A data address travels through the VM as a PVM_Word_t (int16_t), so no
 * section may reach past what that can express. */
const int kMaxDataAddr = 32767;
/* PVM_State_s::code_len is a uint16_t, so the VM cannot address a longer program
 * regardless of how much storage the target actually has. */
const int kMaxCodeSize = 65535;

/*
 * The target VM's memory layout, declared by the .code/.ram/.stack/.usrsect
 * directives. Its whole purpose is to turn what would otherwise be a runtime
 * ERRORCODE_BAD_DADR/ERRORCODE_BAD_IADR into an assembly-time error.
 *
 * The defaults mirror pvm.h, so a file that declares nothing keeps assembling
 * exactly as it did before these directives existed. The one deliberate gap is
 * usrsect's length: pvm.h has no constant for it (the VM learns it at runtime,
 * from PVM_UsrSect), so until a file says otherwise every address at or above
 * the usrsect base is accepted -- checking that region requires opting in via
 * ".usrsect BASE LEN".
 */
struct Sections {
	int codeMax 		= kMaxCodeSize;
	int ram 				= PVM_CFG_RAM_SIZE; 		/* words; valid data addresses are 0..ram-1 */
	int stack 			= PVM_CFG_STACK_SIZE; 	/* words */
	int usrsectBase = PVM_CFG_USRSECT_BGN;
	int usrsectLen 	= -1; 									/* words; -1 == undeclared, so the upper bound is unknown */
};

std::string Where(const std::string& file, int lineNo){
	return file + ":" + std::to_string(lineNo) + ": error: ";
}

std::string Hex(int value){
	std::ostringstream s;
	s << "0x" << std::uppercase << std::hex << value;
	return s.str();
}

/* Same shape the listing already uses for code addresses, so a map entry and a
 * listing line can be matched up by eye. */
std::string Hex4(int value){
	std::ostringstream s;
	s << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << value;
	return s.str();
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
		ValidateSections();
		ReorderForEntry();
		AssignAddresses();
		CheckCodeSize();

		AssembleResult result;
		result.code 		= Encode();
		result.listing 	= GenerateListing(result.code);
		result.debugMap = GenerateDebugMap();
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
	Sections 														sec_;
	/* Which sections were declared explicitly, so a redeclaration that contradicts
	 * an earlier one (typically via .import) is an error rather than a silent
	 * last-one-wins -- repeating the same value is harmless and stays allowed. */
	std::unordered_map<std::string, int> declaredSections_;
	std::vector<Var> 										vars_;
	bool 																havePendingDebug_ = false;
	std::string 												pendingDebugFile_;
	int 																pendingDebugLine_ = 0;
	std::vector<std::string> 						funcNames_;
	int 																currentFunc_ = -1;
	std::vector<FrameVar> 							pendingVars_;

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

			if(StartsWithDirective(line, ".code") 	|| StartsWithDirective(line, ".ram") ||
				 StartsWithDirective(line, ".stack") || StartsWithDirective(line, ".usrsect")){
				ParseSectionDirective(l);
				continue;
			}

			if(StartsWithDirective(line, ".line")){
				ParseLineDirective(l);
				continue;
			}

			if(StartsWithDirective(line, ".var")){
				ParseVarDirective(l);
				continue;
			}

			if(StartsWithDirective(line, ".func") || StartsWithDirective(line, ".endfunc") ||
				 StartsWithDirective(line, ".param") || StartsWithDirective(line, ".local")){
				ParseSymbolDirective(l);
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

			Instr ins{def->opcode, def->hasOperand, operand, 0, l.file, l.lineNo};

			/* Whatever ".line" was last seen belongs to this instruction, and to
			 * this one only: it is consumed here rather than left standing, so a
			 * stray directive cannot silently label unrelated code further down. */
			if(havePendingDebug_){
				ins.hasDebugLine = true;
				ins.debugFile 	 = pendingDebugFile_;
				ins.debugLine 	 = pendingDebugLine_;
				havePendingDebug_ = false;
			}

			ins.func = currentFunc_;
			ins.vars.swap(pendingVars_);

			current->instrs.push_back(std::move(ins));
		}
	}

	/*
	 * ".line N path" -- the path is the rest of the line, unquoted, the same way
	 * ".import" takes its argument: Windows paths are full of backslashes and can
	 * contain spaces, and quoting rules would be one more thing to get wrong in a
	 * file no human writes by hand.
	 */
	void ParseLineDirective(const Line& l){
		std::istringstream iss(l.text);
		std::string directive, lineText;

		iss >> directive >> lineText;
		if(!ParseNumber(lineText, &pendingDebugLine_) || pendingDebugLine_ <= 0)
			throw AssemblerError(Where(l.file, l.lineNo) + "'.line' expects a line number");

		std::getline(iss, pendingDebugFile_);
		Trim(pendingDebugFile_);
		if(pendingDebugFile_.empty())
			throw AssemblerError(Where(l.file, l.lineNo) + "'.line' expects a source file after the number");

		havePendingDebug_ = true;
	}

	/*
	 * ".func NAME" / ".endfunc" / ".param NAME OFFSET" / ".local NAME OFFSET" --
	 * what the debugger needs to put names on a stack frame. They generate no
	 * code, and a .pvms written by hand need not have any.
	 */
	void ParseSymbolDirective(const Line& l){
		std::istringstream iss(l.text);
		std::string directive, name, offsetText, extra;
		int offset;

		iss >> directive;

		if(directive == ".endfunc"){
			currentFunc_ = -1;
			pendingVars_.clear(); /* a variable with no instruction after it never exists */
			return;
		}

		if(!(iss >> name))
			throw AssemblerError(Where(l.file, l.lineNo) + "'" + directive + "' expects a name");

		if(directive == ".func"){
			funcNames_.push_back(name);
			currentFunc_ = (int)funcNames_.size() - 1;
			pendingVars_.clear();
			return;
		}

		if(currentFunc_ < 0)
			throw AssemblerError(Where(l.file, l.lineNo) + "'" + directive + "' outside a .func");
		if(!(iss >> offsetText) || !ParseNumber(offsetText, &offset))
			throw AssemblerError(Where(l.file, l.lineNo) + "'" + directive + "' expects NAME OFFSET");
		if(iss >> extra)
			throw AssemblerError(Where(l.file, l.lineNo) + "unexpected '" + extra + "' after '" + directive + "'");

		pendingVars_.push_back(FrameVar{directive.substr(1), name, offset});
	}

	/* ".var NAME ADDR SIZE" -- a variable rather than a bare number, so the
	 * listing's memory map can show where it lives and how much room it takes,
	 * and so its whole span (not just its first word) gets bounds-checked. */
	void ParseVarDirective(const Line& l){
		std::istringstream iss(l.text);
		std::string directive, name, addrText, sizeText;
		int address, size;

		iss >> directive;
		if(!(iss >> name >> addrText >> sizeText))
			throw AssemblerError(Where(l.file, l.lineNo) + "malformed .var directive "
														"(expected '.var NAME ADDRESS SIZE')");
		if(!ParseNumber(addrText, &address))
			throw AssemblerError(Where(l.file, l.lineNo) + "invalid address '" + addrText + "'");
		if(!ParseNumber(sizeText, &size) || size <= 0)
			throw AssemblerError(Where(l.file, l.lineNo) + "invalid size '" + sizeText + "'");

		consts_[name] = address; /* resolves in operands exactly like a .const */
		vars_.push_back(Var{name, address, size, l.file, l.lineNo});
	}

	/* ".code N" / ".ram N" / ".stack N" / ".usrsect BASE LEN" -- see Sections. */
	void ParseSectionDirective(const Line& l){
		std::istringstream iss(l.text);
		std::string name;
		iss >> name;

		if(name == ".usrsect"){
			int base = ReadSectionValue(iss, l, name, "a base address");
			int len 	= ReadSectionValue(iss, l, name, "a length in words");
			ExpectEndOfDirective(iss, l, name);

			if(base < 0 || base > kMaxDataAddr)
				throw AssemblerError(Where(l.file, l.lineNo) + ".usrsect base must be 0.." +
															std::to_string(kMaxDataAddr));
			if(len < 0 || base + len - 1 > kMaxDataAddr)
				throw AssemblerError(Where(l.file, l.lineNo) + ".usrsect ends past the highest addressable "
															"word (" + std::to_string(kMaxDataAddr) + ")");

			RecordSection(l, ".usrsect base", base, &sec_.usrsectBase);
			RecordSection(l, ".usrsect length", len, &sec_.usrsectLen);
			return;
		}

		int value = ReadSectionValue(iss, l, name, "a size");
		ExpectEndOfDirective(iss, l, name);

		if(name == ".code"){
			if(value <= 0 || value > kMaxCodeSize)
				throw AssemblerError(Where(l.file, l.lineNo) + ".code must be 1.." + std::to_string(kMaxCodeSize) +
															" bytes (the VM holds a program's length in a uint16_t)");
			RecordSection(l, name, value, &sec_.codeMax);
		}
		else if(name == ".ram"){
			if(value < 0 || value > kMaxDataAddr + 1)
				throw AssemblerError(Where(l.file, l.lineNo) + ".ram must be 0.." +
															std::to_string(kMaxDataAddr + 1) + " words");
			RecordSection(l, name, value, &sec_.ram);
		}
		else{
			if(value <= 0)
				throw AssemblerError(Where(l.file, l.lineNo) + ".stack must be at least 1 word");
			RecordSection(l, name, value, &sec_.stack);
		}
	}

	int ReadSectionValue(std::istringstream& iss, const Line& l,
											 const std::string& name, const char* what) const {
		std::string text;
		int value;

		if(!(iss >> text) || !ParseNumber(text, &value))
			throw AssemblerError(Where(l.file, l.lineNo) + "'" + name + "' expects " + what);

		return value;
	}

	void ExpectEndOfDirective(std::istringstream& iss, const Line& l, const std::string& name) const {
		std::string extra;
		if(iss >> extra)
			throw AssemblerError(Where(l.file, l.lineNo) + "unexpected '" + extra + "' after '" + name + "'");
	}

	void RecordSection(const Line& l, const std::string& what, int value, int* slot){
		auto it = declaredSections_.find(what);
		if(it != declaredSections_.end() && it->second != value)
			throw AssemblerError(Where(l.file, l.lineNo) + "conflicting '" + what + "': already declared as " +
														std::to_string(it->second) + ", now " + std::to_string(value));

		declaredSections_[what] = value;
		*slot = value;
	}

	void ValidateSections() const {
		if(sec_.ram > sec_.usrsectBase)
			throw AssemblerError("pvmc: error: .ram (" + std::to_string(sec_.ram) +
														" words) runs into .usrsect, which starts at " +
														std::to_string(sec_.usrsectBase));

		/* Checked here rather than where .var is parsed: a file may well declare its
		 * variables before the sections they have to fit in. */
		for(const Var& v : vars_)
			if(!IsValidDataAddr(v.address) || !IsValidDataAddr(v.address + v.size - 1))
				throw AssemblerError(Where(v.file, v.lineNo) + "'" + v.name + "' occupies " +
															std::to_string(v.size) + " word(s) from " + std::to_string(v.address) +
															" (" + Hex(v.address) + "), which does not fit (" + DataRangeText() + ")");
	}

	void CheckCodeSize() const {
		if(totalLen_ > sec_.codeMax)
			throw AssemblerError("pvmc: error: program is " + std::to_string(totalLen_) +
														" bytes, which exceeds the declared .code size of " +
														std::to_string(sec_.codeMax) + " bytes");
	}

	bool IsValidDataAddr(int addr) const {
		if(addr >= 0 && addr < sec_.ram) return true;
		if(addr >= sec_.usrsectBase &&
			 (sec_.usrsectLen < 0 || addr < sec_.usrsectBase + sec_.usrsectLen)) return true;
		return false;
	}

	std::string DataRangeText() const {
		std::ostringstream s;

		if(sec_.ram > 0) 	s << "RAM is 0.." << (sec_.ram - 1);
		else 							s << "RAM is empty";

		if(sec_.usrsectLen < 0)
			s << ", usrsect is " << Hex(sec_.usrsectBase) << " and up (declare '.usrsect BASE LEN' to bound it)";
		else if(sec_.usrsectLen == 0)
			s << ", usrsect is empty";
		else
			s << ", usrsect is " << Hex(sec_.usrsectBase) << ".." << Hex(sec_.usrsectBase + sec_.usrsectLen - 1);

		return s.str();
	}

	/*
	 * The whole point of the section directives: an address the VM would reject at
	 * runtime (ERRORCODE_BAD_DADR / ERRORCODE_BAD_IADR) is rejected here instead.
	 * Only immediate operands can be checked -- LOADI/STOREI take their address off
	 * the stack, so those stay a runtime-only concern.
	 */
	void CheckOperandRange(const Instr& ins, int value) const {
		/* Operands are encoded as 2 bytes, so anything wider silently lost its high
		 * bits until now, whatever the instruction. */
		if(value < -32768 || value > 65535)
			throw AssemblerError(Where(ins.file, ins.lineNo) + "operand " + std::to_string(value) +
														" does not fit in the 16-bit operand field");

		switch(ins.opcode){
		case INSTR_JMP:
		case INSTR_JZ:
		case INSTR_JNZ:
		case INSTR_CALL:
			if(value < 0 || value >= totalLen_)
				throw AssemblerError(Where(ins.file, ins.lineNo) + "jump target " + std::to_string(value) +
															" is outside the program (0.." + std::to_string(totalLen_ - 1) + ")");
			break;

		case INSTR_LOAD:
		case INSTR_STORE:
			if(!IsValidDataAddr(value))
				throw AssemblerError(Where(ins.file, ins.lineNo) + "data address " + std::to_string(value) +
															" (" + Hex(value) + ") is out of range (" + DataRangeText() + ")");
			break;

		case INSTR_LOADFP:
		case INSTR_STOREFP:
			/* The frame pointer's position is a runtime property, so only the offset's
			 * own magnitude can be judged here: one this large cannot land inside the
			 * stack from any frame at all. */
			if(value <= -sec_.stack || value >= sec_.stack)
				throw AssemblerError(Where(ins.file, ins.lineNo) + "frame offset " + std::to_string(value) +
															" cannot land inside a " + std::to_string(sec_.stack) + "-word stack");
			break;

		default:
			break;
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

					CheckOperandRange(ins, value);

					code[ins.address + 1] = static_cast<uint8_t>(value & 0xFF);
					code[ins.address + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
				}
			}
		}

		return code;
	}

	/*
	 * A linker-map-style header for the listing: how much of each section is in
	 * use, which label each function starts at, and where every .var lives.
	 * Written as '#' comments so the listing stays readable as .pvms-ish text.
	 *
	 * Only functions are listed under "code" -- a label that is the target of a
	 * CALL, or the program's entry point. Compiler-generated jump targets
	 * (if_end_7 and friends) would bury them, and they are right there in the
	 * body of the listing anyway.
	 */
	std::string GenerateMemoryMap() const {
		std::ostringstream out;
		std::set<std::string> functions;

		if(haveEntry_) functions.insert(entryLabel_);
		for(const Block& b : blocks_)
			for(const Instr& ins : b.instrs)
				if(ins.opcode == INSTR_CALL) functions.insert(ins.operand);

		out << "# ---- memory map ----\n#\n";
		out << "# code    : " << totalLen_ << " of " << sec_.codeMax << " bytes\n";
		out << "# ram     : " << RamHighWaterMark() << " of " << sec_.ram << " words\n";
		out << "# stack   : " << sec_.stack << " words\n";
		out << "# usrsect : " << Hex(sec_.usrsectBase)
				<< (sec_.usrsectLen < 0 ? " and up (undeclared length)"
																: ".." + Hex(sec_.usrsectBase + sec_.usrsectLen - 1) +
																	" (" + std::to_string(sec_.usrsectLen) + " words)")
				<< "\n#\n";

		out << "# functions:\n";
		for(const Block& b : blocks_)
			if(!b.label.empty() && (functions.count(b.label) || b.address == 0))
				out << "#   " << Hex4(b.address) << "  " << b.label << "\n";

		if(!vars_.empty()){
			std::vector<Var> sorted = vars_;
			std::sort(sorted.begin(), sorted.end(),
				[](const Var& a, const Var& b){ return a.address < b.address; });

			out << "#\n# variables:\n";
			for(const Var& v : sorted)
				out << "#   " << Hex4(v.address) << "  " << std::left << std::setw(20) << std::setfill(' ') << v.name
						<< std::right << std::setw(4) << v.size << (v.size == 1 ? " word " : " words")
						<< "  " << (v.address < sec_.ram ? "ram" : "usrsect") << "\n";
		}

		if(!consts_.empty()){
			/* consts_ is a hash map and .var names live in it too -- sort by name and
			 * drop the variables, so the listing is stable and says each thing once. */
			std::vector<std::pair<std::string, int>> plain;
			for(const auto& c : consts_){
				bool isVar = false;
				for(const Var& v : vars_) if(v.name == c.first){ isVar = true; break; }
				if(!isVar) plain.emplace_back(c.first, c.second);
			}
			std::sort(plain.begin(), plain.end());

			if(!plain.empty()){
				out << "#\n# constants:\n";
				for(const auto& c : plain)
					out << "#   " << std::left << std::setw(20) << std::setfill(' ') << c.first
							<< std::right << c.second << " (" << Hex(c.second) << ")\n";
			}
		}

		out << "\n";
		return out.str();
	}

	/* How far into RAM the variables reach, which is what actually limits how much
	 * room is left -- not their total size, since fixed @addresses can leave gaps. */
	int RamHighWaterMark() const {
		int high = 0;
		for(const Var& v : vars_)
			if(v.address < sec_.ram && v.address + v.size > high) high = v.address + v.size;
		return high;
	}

	/*
	 * "0xADDR file:line" per tagged instruction, in address order -- everything a
	 * debugger needs to turn a program counter back into a place in the source.
	 * Written after the blocks have been reordered and addressed, so the
	 * addresses are the ones the program will actually run at.
	 */
	std::string GenerateDebugMap() const {
		std::ostringstream out;

		for(const Block& b : blocks_)
			for(const Instr& ins : b.instrs)
				if(ins.hasDebugLine)
					out << Hex4(ins.address) << " " << ins.debugFile << ":" << ins.debugLine << "\n";

		/*
		 * Symbols, after the line entries. Every record starts with '@', which no
		 * reader of the line entries takes for an address -- they look for "0x"
		 * or a hex number -- so a map with symbols still reads as before.
		 *
		 *   @func   0xSTART 0xEND NAME          [START, END) belongs to NAME; a
		 *                                       function moved by .entry may have
		 *                                       more than one such range
		 *   @var    FUNC KIND NAME OFFSET 0xADDR
		 *                                       KIND param|local, OFFSET words from
		 *                                       FP, there from instruction ADDR on
		 *   @global NAME 0xADDR SIZE            a .var: data address, size in words
		 */
		int runFunc = -1, runStart = 0, end = 0;

		for(const Block& b : blocks_)
			for(const Instr& ins : b.instrs){
				if(ins.func != runFunc){
					if(runFunc >= 0)
						out << "@func " << Hex4(runStart) << " " << Hex4(ins.address) << " " << funcNames_[runFunc] << "\n";
					runFunc  = ins.func;
					runStart = ins.address;
				}
				end = ins.address + (ins.hasOperand ? 3 : 1);
			}
		if(runFunc >= 0)
			out << "@func " << Hex4(runStart) << " " << Hex4(end) << " " << funcNames_[runFunc] << "\n";

		for(const Block& b : blocks_)
			for(const Instr& ins : b.instrs)
				for(const FrameVar& v : ins.vars)
					out << "@var " << funcNames_[ins.func] << " " << v.kind << " " << v.name << " "
							<< v.offset << " " << Hex4(ins.address) << "\n";

		for(const Var& v : vars_)
			out << "@global " << v.name << " " << Hex4(v.address) << " " << v.size << "\n";

		return out.str();
	}

	std::string GenerateListing(const std::vector<uint8_t>& code) const {
		std::ostringstream out;

		out << GenerateMemoryMap();

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
