#ifndef HLC_LEXER_H_
#define HLC_LEXER_H_

#include <stdexcept>
#include <string>
#include <vector>

class HlcError : public std::runtime_error {
public:
	explicit HlcError(const std::string& message) : std::runtime_error(message) {}
};

enum class TokenType {
	End,
	Identifier,
	IntLiteral,
	KwInt, KwVoid, KwVolatile, KwIf, KwElse, KwWhile, KwFor, KwReturn, KwHalt,
	LBrace, RBrace, LParen, RParen, LBracket, RBracket,
	Semicolon, Comma, At,
	Plus, Minus, Star, Slash,
	Amp, Pipe, Caret, Tilde, Shl, Shr,
	EqEq, NotEq, Lt, Gt, Le, Ge,
	AndAnd, OrOr, Not,
	Assign,
};

struct Token {
	TokenType 	type;
	std::string text;			/* identifier name, or the raw text for diagnostics */
	int 				intValue = 0;	/* valid when type == IntLiteral */
	int 				line = 0;
};

/* Tokenizes the whole source up front (the file is small; a streaming lexer buys nothing here). */
std::vector<Token> Tokenize(const std::string& source, const std::string& fileName);

#endif /* HLC_LEXER_H_ */
