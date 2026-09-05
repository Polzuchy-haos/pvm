/*
 * hlc.cpp
 *
 * Command-line entry point for the high-level PVM compiler:
 *   pvmcc input.hlc [-o output.pvms]
 *
 * Emits PVM assembly text; feed that into the existing `pvmc` to get bytecode.
 */
#include "codegen.h"
#include "lexer.h"
#include "parser.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string ReplaceExtension(const std::string& input){
	std::filesystem::path p(input);
	p.replace_extension(".pvms");
	return p.string();
}

} // namespace

int main(int argc, char** argv){
	if(argc < 2){
		std::cerr << "usage: " << argv[0] << " <input.hlc> [-o output.pvms]\n";
		return 1;
	}

	std::string input = argv[1];
	std::string output;
	bool have_output = false;

	for(int i = 2; i < argc; i++){
		if(std::string(argv[i]) == "-o" && i + 1 < argc)
			output = argv[++i], have_output = true;
	}
	if(!have_output) output = ReplaceExtension(input);

	std::ifstream in(input, std::ios::binary);
	if(!in){
		std::cerr << "pvmcc: error: cannot open '" << input << "'\n";
		return 1;
	}
	std::ostringstream buf;
	buf << in.rdbuf();

	try{
		std::vector<Token> tokens = Tokenize(buf.str(), input);
		Program program = Parse(tokens, input);
		std::string pvms = Generate(program, input);

		std::ofstream out(output);
		if(!out){
			std::cerr << "pvmcc: error: cannot open output file '" << output << "'\n";
			return 1;
		}
		out << pvms;
	}
	catch(const std::exception& e){
		std::cerr << e.what() << "\n";
		return 1;
	}

	std::cout << "pvmcc: wrote '" << output << "'\n";
	return 0;
}
