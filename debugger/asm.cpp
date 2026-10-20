/*
 * asm.cpp
 *
 * C face of the C++ assembler, for the same reason disasm.cpp exists: the
 * session stays a C program, and the instruction set stays defined in one place.
 */
#include "asm.h"

#include "assembler.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void SetError(char* err, int errLen, const std::string& message){
	std::snprintf(err, (size_t)errLen, "%s", message.c_str());
}

static int CopyOut(const AssembleResult& r, uint8_t* out, int outMax, char* err, int errLen){
	if((int)r.code.size() > outMax){
		SetError(err, errLen, std::to_string(r.code.size()) + " bytes of bytecode, the device's buffer holds " +
													std::to_string(outMax));
		return -1;
	}

	if(!r.code.empty()) std::memcpy(out, r.code.data(), r.code.size());
	return (int)r.code.size();
}

static void ReplaceAll(std::string* s, const std::string& from, const std::string& to){
	if(from.empty()) return;
	for(size_t at = s->find(from); at != std::string::npos; at = s->find(from, at + to.size()))
		s->replace(at, from.size(), to);
}

int PvmDbgAssembleFile(const char* path, uint8_t* out, int outMax, char* err, int errLen){
	try{
		return CopyOut(PVMC_Assemble(path), out, outMax, err, errLen);
	}
	catch(const std::exception& e){
		SetError(err, errLen, e.what());
		return -1;
	}
}

/*
 * The assembler reads files -- .import is resolved relative to one -- so the
 * snippet goes through a temporary file rather than a second, string-based
 * entry point into it. One instruction per line, and the section directives
 * after them rather than before, so that line N of an error is the N-th piece
 * the user typed.
 */
int PvmDbgAssembleText(const char* text, int ramLen, int stackLen,
                       uint8_t* out, int outMax, char* err, int errLen){
	std::error_code ec;
	fs::path 				tmp = fs::temp_directory_path(ec);
	std::string 		source(text);
	int 						result;

	if(ec){
		SetError(err, errLen, "no temporary directory to assemble the snippet in");
		return -1;
	}

	/* time() rather than std::chrono: steady_clock pulls in libwinpthread's
	 * clock_gettime, and an older libwinpthread-1.dll first on PATH (Git Bash
	 * ships one) then keeps pvmdbg from starting at all. */
	tmp /= "pvmdbg_snippet_" + std::to_string((long long)std::time(nullptr)) + ".pvms";

	ReplaceAll(&source, ";", "\n");
	source += "\n";
	if(ramLen >= 0) 	source += ".ram " + std::to_string(ramLen) + "\n";
	if(stackLen > 0) 	source += ".stack " + std::to_string(stackLen) + "\n";

	{
		std::ofstream file(tmp);
		if(!(file << source)){
			SetError(err, errLen, "cannot write '" + tmp.string() + "'");
			return -1;
		}
	}

	try{
		result = CopyOut(PVMC_Assemble(tmp.string()), out, outMax, err, errLen);
	}
	catch(const std::exception& e){
		std::string message = e.what();

		/* Whichever spelling of the path the assembler chose to print. */
		ReplaceAll(&message, tmp.string(), "snippet");
		ReplaceAll(&message, tmp.generic_string(), "snippet");
		SetError(err, errLen, message);
		result = -1;
	}

	fs::remove(tmp, ec);
	return result;
}
