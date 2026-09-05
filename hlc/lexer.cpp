#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace {

const std::unordered_map<std::string, TokenType> kKeywords = {
	{"int", 			TokenType::KwInt},
	{"void", 			TokenType::KwVoid},
	{"volatile", 	TokenType::KwVolatile},
	{"if", 				TokenType::KwIf},
	{"else", 			TokenType::KwElse},
	{"while", 		TokenType::KwWhile},
	{"for", 			TokenType::KwFor},
	{"return", 		TokenType::KwReturn},
	{"halt", 			TokenType::KwHalt},
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

			tokens.push_back(LexOperatorOrPunct());
		}

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
			case '+': return make(TokenType::Plus, 1, "+");
			case '-': return make(TokenType::Minus, 1, "-");
			case '*': return make(TokenType::Star, 1, "*");
			case '/': return make(TokenType::Slash, 1, "/");
			case '~': return make(TokenType::Tilde, 1, "~");
			case '^': return make(TokenType::Caret, 1, "^");
			case '&':
				if(Peek(1) == '&') return make(TokenType::AndAnd, 2, "&&");
				return make(TokenType::Amp, 1, "&");
			case '|':
				if(Peek(1) == '|') return make(TokenType::OrOr, 2, "||");
				return make(TokenType::Pipe, 1, "|");
			case '<':
				if(Peek(1) == '<') return make(TokenType::Shl, 2, "<<");
				if(Peek(1) == '=') return make(TokenType::Le, 2, "<=");
				return make(TokenType::Lt, 1, "<");
			case '>':
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
