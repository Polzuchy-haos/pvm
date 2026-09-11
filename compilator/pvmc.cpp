/*
 * pvmc.cpp
 *
 * Command-line entry point for the PVM assembler:
 *   pvmc <input.pvms> [-o output] [-f bin|hex|carray] [-l [listing.lst]] [-g [map.dbgmap]]
 *
 *   -f bin     raw bytecode (default)
 *   -f hex     Intel HEX (for MCU flashing/loading tools)
 *   -f carray  a "const uint8_t name[] = {...};" C source file
 *   -l         also emit a source-correlated listing (address, bytes, mnemonic)
 *   -g         also emit a debug map (address -> .hlc file and line), for pvmdbg
 */
#include "assembler.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

enum class OutputFormat { Bin, Hex, CArray };

std::string ReplaceExtension(const std::string& input, const std::string& ext){
	std::filesystem::path p(input);
	p.replace_extension(ext);
	return p.string();
}

std::string SanitizeIdentifier(const std::string& s){
	std::string out;
	for(char c : s)
		out += (std::isalnum((unsigned char)c) ? c : '_');
	if(out.empty() || std::isdigit((unsigned char)out[0]))
		out = "_" + out;
	return out;
}

void WriteBin(const std::string& path, const std::vector<uint8_t>& code){
	std::ofstream out(path, std::ios::binary);
	if(!out)
		throw AssemblerError("pvmc: error: cannot open output file '" + path + "'");
	out.write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(code.size()));
}

void WriteHexByte(std::ostream& out, unsigned value, unsigned* checksum){
	out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (value & 0xFF);
	if(checksum) *checksum += value & 0xFF;
}

/* Intel HEX: ":LLAAAATT<data>CC" data records (16 bytes each) + an EOF record. */
void WriteIntelHex(const std::string& path, const std::vector<uint8_t>& code){
	std::ofstream out(path);
	if(!out)
		throw AssemblerError("pvmc: error: cannot open output file '" + path + "'");

	const size_t kRecordSize = 16;
	for(size_t offset = 0; offset < code.size(); offset += kRecordSize){
		size_t len = std::min(kRecordSize, code.size() - offset);
		unsigned checksum = 0;

		out << ':';
		WriteHexByte(out, static_cast<unsigned>(len), &checksum);
		WriteHexByte(out, static_cast<unsigned>((offset >> 8) & 0xFF), &checksum);
		WriteHexByte(out, static_cast<unsigned>(offset & 0xFF), &checksum);
		WriteHexByte(out, 0x00, &checksum); /* record type: data */
		for(size_t i = 0; i < len; i++)
			WriteHexByte(out, code[offset + i], &checksum);

		unsigned checksumByte = (0x100u - (checksum & 0xFF)) & 0xFF;
		WriteHexByte(out, checksumByte, nullptr);
		out << "\n";
	}
	out << ":00000001FF\n"; /* EOF record */
}

void WriteCArray(const std::string& path, const std::vector<uint8_t>& code, const std::string& arrayName){
	std::ofstream out(path);
	if(!out)
		throw AssemblerError("pvmc: error: cannot open output file '" + path + "'");

	out << "#include <stdint.h>\n\n";
	out << "const uint8_t " << arrayName << "[] = {";
	for(size_t i = 0; i < code.size(); i++){
		if(i % 12 == 0) out << "\n\t";
		out << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
				<< static_cast<unsigned>(code[i]);
		if(i + 1 < code.size()) out << ", ";
	}
	out << "\n};\n\n" << std::dec;
	out << "const unsigned " << arrayName << "_len = " << code.size() << ";\n";
}

} // namespace

int main(int argc, char** argv){
	if(argc < 2){
		std::cerr << "usage: " << argv[0] << " <input.pvms> [-o output] [-f bin|hex|carray] [-l [listing.lst]] [-g [map.dbgmap]]\n";
		return 1;
	}

	std::string input = argv[1];
	std::string output;
	bool have_output = false;
	OutputFormat format = OutputFormat::Bin;
	bool want_listing = false;
	std::string listing_path;
	bool want_debugmap = false;
	std::string debugmap_path;

	for(int i = 2; i < argc; i++){
		std::string arg = argv[i];

		if(arg == "-o" && i + 1 < argc){
			output = argv[++i];
			have_output = true;
		}
		else if(arg == "-f" && i + 1 < argc){
			std::string f = argv[++i];
			if(f == "bin") 			format = OutputFormat::Bin;
			else if(f == "hex") 	format = OutputFormat::Hex;
			else if(f == "carray") format = OutputFormat::CArray;
			else{
				std::cerr << "pvmc: error: unknown format '" << f << "' (expected bin, hex or carray)\n";
				return 1;
			}
		}
		else if(arg == "-l"){
			want_listing = true;
			if(i + 1 < argc && std::string(argv[i + 1]).rfind("-", 0) != 0)
				listing_path = argv[++i];
		}
		else if(arg == "-g"){
			want_debugmap = true;
			if(i + 1 < argc && std::string(argv[i + 1]).rfind("-", 0) != 0)
				debugmap_path = argv[++i];
		}
		else{
			std::cerr << "pvmc: error: unrecognized argument '" << arg << "'\n";
			return 1;
		}
	}

	if(!have_output){
		const char* ext = (format == OutputFormat::Hex) ? ".hex" : (format == OutputFormat::CArray) ? ".c" : ".bin";
		output = ReplaceExtension(input, ext);
	}
	if(want_listing && listing_path.empty())
		listing_path = ReplaceExtension(output, ".lst");
	if(want_debugmap && debugmap_path.empty())
		debugmap_path = ReplaceExtension(output, ".dbgmap");

	AssembleResult result;
	try{
		result = PVMC_Assemble(input);

		switch(format){
			case OutputFormat::Bin: 		WriteBin(output, result.code); break;
			case OutputFormat::Hex: 		WriteIntelHex(output, result.code); break;
			case OutputFormat::CArray: 	WriteCArray(output, result.code, SanitizeIdentifier(std::filesystem::path(output).stem().string())); break;
		}

		if(want_listing){
			std::ofstream listing(listing_path);
			if(!listing)
				throw AssemblerError("pvmc: error: cannot open listing file '" + listing_path + "'");
			listing << result.listing;
		}

		if(want_debugmap){
			std::ofstream map(debugmap_path);
			if(!map)
				throw AssemblerError("pvmc: error: cannot open debug map file '" + debugmap_path + "'");
			map << result.debugMap;
		}
	}
	catch(const std::exception& e){
		std::cerr << e.what() << "\n";
		return 1;
	}

	std::cout << "pvmc: assembled " << result.code.size() << " bytes of bytecode, wrote '" << output << "'\n";
	if(want_listing)
		std::cout << "pvmc: wrote listing to '" << listing_path << "'\n";
	if(want_debugmap)
		std::cout << "pvmc: wrote debug map to '" << debugmap_path << "'\n";
	return 0;
}
