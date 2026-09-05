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
	KwSvc, KwImport, KwExtern, KwFrom,
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

/* Tokenizes already-read source text (no import expansion). */
std::vector<Token> Tokenize(const std::string& source, const std::string& fileName);

/*
 * Reads and tokenizes `path`, then expands "import name;" in place (splicing in
 * the tokens of the named file, recursively, cycle-safe) -- the token-level
 * equivalent of the .pvms assembler's ".import" text splicing.
 *
 * Each "import name;" is resolved by searching, in order: (1) next to the
 * importing file, (2) <dir of the pvmcc executable>/lib, (3) each entry of
 * `includePaths` (the -I flags, in the order given) -- in each directory,
 * the exact name first, then with ".hlc" appended. This is the entry point
 * pvmcc actually uses.
 */
std::vector<Token> TokenizeFile(const std::string& path, const std::vector<std::string>& includePaths = {});

#endif /* HLC_LEXER_H_ */
