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
	KwInt, KwVoid, KwVolatile, KwConst, KwIf, KwElse, KwWhile, KwDo, KwFor, KwReturn, KwHalt,
	KwSvc, KwImport, KwExtern, KwFrom, KwSizeof, KwSection,
	LBrace, RBrace, LParen, RParen, LBracket, RBracket,
	Semicolon, Comma, At, Question, Colon,
	Plus, Minus, Star, Slash,
	Amp, Pipe, Caret, Tilde, Shl, Shr,
	EqEq, NotEq, Lt, Gt, Le, Ge,
	AndAnd, OrOr, Not,
	Assign,
	PlusEq, MinusEq, StarEq, SlashEq, AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,
	PlusPlus, MinusMinus,
};

struct Token {
	TokenType 	type;
	std::string text;			/* identifier name, or the raw text for diagnostics */
	int 				intValue = 0;	/* valid when type == IntLiteral */
	int 				line = 0;
	/* Which file this token was read from. Needed because "import" splices another
	 * file's tokens into this one's stream, so a line number on its own does not
	 * say where in the source something is. Last on purpose: the lexer builds
	 * tokens with positional initialisers, and this field is filled in afterwards
	 * (see Lexer::Run), so putting it anywhere else would silently shift them. */
	std::string file;
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
