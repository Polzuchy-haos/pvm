#include "lexer.h"
#include "modulepath.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace {

const std::unordered_map<std::string, TokenType> kKeywords = {
	{"int", 			TokenType::KwInt},
	{"void", 			TokenType::KwVoid},
	{"volatile", 	TokenType::KwVolatile},
	{"const", 		TokenType::KwConst},
	{"if", 				TokenType::KwIf},
	{"else", 			TokenType::KwElse},
	{"while", 		TokenType::KwWhile},
	{"do", 				TokenType::KwDo},
	{"for", 			TokenType::KwFor},
	{"return", 		TokenType::KwReturn},
	{"halt", 			TokenType::KwHalt},
	{"svc", 			TokenType::KwSvc},
	{"import", 		TokenType::KwImport},
	{"extern", 		TokenType::KwExtern},
	{"from", 			TokenType::KwFrom},
	{"sizeof", 		TokenType::KwSizeof},
	{"section", 	TokenType::KwSection},
};

class Lexer {
public:
	Lexer(const std::string& source, const std::string& fileName)
		: src_(source), file_(fileName) {}

	std::vector<Token> Run(){
		std::vector<Token> tokens;

		for(;;){
			SkipWhitespaceAndComments();
			if(pos_ >= src_.size()){
				tokens.push_back({TokenType::End, "", 0, line_});
				break;
			}

			char c = src_[pos_];

			if(std::isdigit((unsigned char)c)){
				tokens.push_back(LexNumber());
				continue;
			}
			if(std::isalpha((unsigned char)c) || c == '_'){
				tokens.push_back(LexIdentifierOrKeyword());
				continue;
			}
			if(c == '\''){
				tokens.push_back(LexCharLiteral());
				continue;
			}

			tokens.push_back(LexOperatorOrPunct());
		}

		/* Stamped in one pass at the end rather than in every Lex* method: the
		 * whole run comes from one file, and this way adding a token kind cannot
		 * forget to record where it came from. */
		for(Token& t : tokens) t.file = file_;

		return tokens;
	}

private:
	const std::string& src_;
	std::string 				file_;
	size_t 							pos_ 	= 0;
	int 								line_ = 1;

	char Peek(size_t ahead = 0) const {
		size_t i = pos_ + ahead;
		return i < src_.size() ? src_[i] : '\0';
	}

	void Advance(){
		if(pos_ < src_.size()){
			if(src_[pos_] == '\n') line_++;
			pos_++;
		}
	}

	[[noreturn]] void Fail(const std::string& message){
		throw HlcError(file_ + ":" + std::to_string(line_) + ": error: " + message);
	}

	void SkipWhitespaceAndComments(){
		for(;;){
			while(pos_ < src_.size() && std::isspace((unsigned char)src_[pos_])) Advance();

			if(Peek() == '/' && Peek(1) == '/'){
				while(pos_ < src_.size() && src_[pos_] != '\n') Advance();
				continue;
			}
			if(Peek() == '/' && Peek(1) == '*'){
				Advance(); Advance();
				while(pos_ < src_.size() && !(Peek() == '*' && Peek(1) == '/')) Advance();
				if(pos_ >= src_.size()) Fail("unterminated /* comment");
				Advance(); Advance();
				continue;
			}
			break;
		}
	}

	Token LexNumber(){
		int startLine = line_;
		size_t start = pos_;

		if(Peek() == '0' && (Peek(1) == 'x' || Peek(1) == 'X')){
			Advance(); Advance();
			while(std::isxdigit((unsigned char)Peek())) Advance();
		}
		else{
			while(std::isdigit((unsigned char)Peek())) Advance();
		}

		std::string text = src_.substr(start, pos_ - start);
		Token t;
		t.type 			= TokenType::IntLiteral;
		t.text 			= text;
		t.intValue 	= (int)std::strtol(text.c_str(), nullptr, 0);
		t.line 			= startLine;
		return t;
	}

	/* 'x' or a handful of C-style escapes ('\n' '\t' '\r' '\0' '\\' '\'' '\"')
	 * -- resolves straight to an IntLiteral holding the ASCII code, so the
	 * parser/codegen never need to know character literals exist at all. */
	Token LexCharLiteral(){
		int startLine = line_;
		size_t start = pos_;
		Advance(); /* opening ' */

		if(pos_ >= src_.size()) Fail("unterminated character literal");

		int value;
		if(Peek() == '\''){
			Fail("empty character literal");
		}
		else if(Peek() == '\\'){
			Advance();
			char esc = Peek();
			if(esc == 'n') 			value = '\n';
			else if(esc == 't') 	value = '\t';
			else if(esc == 'r') 	value = '\r';
			else if(esc == '0') 	value = '\0';
			else if(esc == '\\') 	value = '\\';
			else if(esc == '\'') 	value = '\'';
			else if(esc == '"') 	value = '"';
			else Fail(std::string("unknown escape sequence '\\") + esc + "' in a character literal");
			Advance();
		}
		else{
			value = (unsigned char)Peek();
			Advance();
		}

		if(Peek() != '\'') Fail("expected closing ' after a character literal");
		Advance();

		Token t;
		t.type 			= TokenType::IntLiteral;
		t.text 			= src_.substr(start, pos_ - start);
		t.intValue 	= value;
		t.line 			= startLine;
		return t;
	}

	Token LexIdentifierOrKeyword(){
		int startLine = line_;
		size_t start = pos_;

		while(std::isalnum((unsigned char)Peek()) || Peek() == '_') Advance();

		std::string text = src_.substr(start, pos_ - start);
		auto it = kKeywords.find(text);

		Token t;
		t.type 	= (it != kKeywords.end()) ? it->second : TokenType::Identifier;
		t.text 	= text;
		t.line 	= startLine;
		return t;
	}

	Token LexOperatorOrPunct(){
		int startLine = line_;
		char c = Peek();

		auto make = [&](TokenType type, int len, const char* text){
			for(int i = 0; i < len; i++) Advance();
			return Token{type, text, 0, startLine};
		};

		switch(c){
			case '{': return make(TokenType::LBrace, 1, "{");
			case '}': return make(TokenType::RBrace, 1, "}");
			case '(': return make(TokenType::LParen, 1, "(");
			case ')': return make(TokenType::RParen, 1, ")");
			case '[': return make(TokenType::LBracket, 1, "[");
			case ']': return make(TokenType::RBracket, 1, "]");
			case ';': return make(TokenType::Semicolon, 1, ";");
			case ',': return make(TokenType::Comma, 1, ",");
			case '@': return make(TokenType::At, 1, "@");
			case '?': return make(TokenType::Question, 1, "?");
			case ':': return make(TokenType::Colon, 1, ":");
			case '+':
				if(Peek(1) == '+') return make(TokenType::PlusPlus, 2, "++");
				if(Peek(1) == '=') return make(TokenType::PlusEq, 2, "+=");
				return make(TokenType::Plus, 1, "+");
			case '-':
				if(Peek(1) == '-') return make(TokenType::MinusMinus, 2, "--");
				if(Peek(1) == '=') return make(TokenType::MinusEq, 2, "-=");
				return make(TokenType::Minus, 1, "-");
			case '*':
				if(Peek(1) == '=') return make(TokenType::StarEq, 2, "*=");
				return make(TokenType::Star, 1, "*");
			case '/':
				if(Peek(1) == '=') return make(TokenType::SlashEq, 2, "/=");
				return make(TokenType::Slash, 1, "/");
			case '~': return make(TokenType::Tilde, 1, "~");
			case '^':
				if(Peek(1) == '=') return make(TokenType::CaretEq, 2, "^=");
				return make(TokenType::Caret, 1, "^");
			case '&':
				if(Peek(1) == '&') return make(TokenType::AndAnd, 2, "&&");
				if(Peek(1) == '=') return make(TokenType::AmpEq, 2, "&=");
				return make(TokenType::Amp, 1, "&");
			case '|':
				if(Peek(1) == '|') return make(TokenType::OrOr, 2, "||");
				if(Peek(1) == '=') return make(TokenType::PipeEq, 2, "|=");
				return make(TokenType::Pipe, 1, "|");
			case '<':
				if(Peek(1) == '<' && Peek(2) == '=') return make(TokenType::ShlEq, 3, "<<=");
				if(Peek(1) == '<') return make(TokenType::Shl, 2, "<<");
				if(Peek(1) == '=') return make(TokenType::Le, 2, "<=");
				return make(TokenType::Lt, 1, "<");
			case '>':
				if(Peek(1) == '>' && Peek(2) == '=') return make(TokenType::ShrEq, 3, ">>=");
				if(Peek(1) == '>') return make(TokenType::Shr, 2, ">>");
				if(Peek(1) == '=') return make(TokenType::Ge, 2, ">=");
				return make(TokenType::Gt, 1, ">");
			case '=':
				if(Peek(1) == '=') return make(TokenType::EqEq, 2, "==");
				return make(TokenType::Assign, 1, "=");
			case '!':
				if(Peek(1) == '=') return make(TokenType::NotEq, 2, "!=");
				return make(TokenType::Not, 1, "!");
			default:
				Fail(std::string("unexpected character '") + c + "'");
		}
	}
};

} // namespace

std::vector<Token> Tokenize(const std::string& source, const std::string& fileName){
	Lexer lexer(source, fileName);
	return lexer.Run();
}

namespace {

namespace fs = std::filesystem;

std::string ReadFile(const std::string& path){
	std::ifstream in(path, std::ios::binary);
	if(!in) throw HlcError("pvmcc: error: cannot open '" + path + "'");
	std::ostringstream buf;
	buf << in.rdbuf();
	return buf.str();
}


std::vector<Token> TokenizeFileRecursive(const std::string& path, std::vector<std::string>* importStack,
																					const std::vector<std::string>& includePaths){
	fs::path norm = fs::absolute(path).lexically_normal();
	std::string normStr = norm.string();

	if(std::find(importStack->begin(), importStack->end(), normStr) != importStack->end())
		throw HlcError("pvmcc: error: circular import involving '" + path + "'");

	std::vector<Token> tokens = Tokenize(ReadFile(path), path);
	importStack->push_back(normStr);

	std::vector<Token> result;
	for(size_t i = 0; i < tokens.size(); i++){
		if(tokens[i].type != TokenType::KwImport){
			result.push_back(tokens[i]);
			continue;
		}

		if(i + 2 >= tokens.size() || tokens[i + 1].type != TokenType::Identifier ||
			 tokens[i + 2].type != TokenType::Semicolon)
			throw HlcError(path + ":" + std::to_string(tokens[i].line) + ": error: expected \"import name;\"");

		std::string resolved = ResolveModulePath(path, tokens[i + 1].text, ".hlc", includePaths);
		std::vector<Token> imported = TokenizeFileRecursive(resolved, importStack, includePaths);
		if(!imported.empty() && imported.back().type == TokenType::End)
			imported.pop_back(); /* only the outermost file's End survives to the final stream */

		result.insert(result.end(), imported.begin(), imported.end());
		i += 2; /* skip the name and ';'; the loop's own i++ skips 'import' itself */
	}

	importStack->pop_back();
	return result;
}

} // namespace

std::vector<Token> TokenizeFile(const std::string& path, const std::vector<std::string>& includePaths){
	std::vector<std::string> importStack;
	return TokenizeFileRecursive(path, &importStack, includePaths);
}
