#include "parser.h"

namespace {

class Parser {
public:
	Parser(const std::vector<Token>& tokens, const std::string& fileName)
		: tokens_(tokens), file_(fileName) {}

	Program Run(){
		Program program;

		while(!Check(TokenType::End)){
			if(Check(TokenType::KwVolatile)){
				Advance();
				Expect(TokenType::KwInt, "expected 'int' after 'volatile'");
				program.globals.push_back(ParseGlobalTail(true));
			}
			else if(Check(TokenType::KwVoid)){
				Advance();
				program.funcs.push_back(ParseFunc(true));
			}
			else if(Check(TokenType::KwInt)){
				Advance();
				Token name = Expect(TokenType::Identifier, "expected a name after 'int'");
				if(Check(TokenType::LParen))
					program.funcs.push_back(ParseFuncAfterName(false, name));
				else
					program.globals.push_back(ParseGlobalTailAfterName(false, name));
			}
			else{
				Fail("expected a global declaration or a function ('int'/'void'/'volatile')");
			}
		}

		return program;
	}

private:
	const std::vector<Token>& tokens_;
	std::string 							 file_;
	size_t 										 pos_ = 0;

	const Token& Cur() const { return tokens_[pos_]; }
	bool Check(TokenType t) const { return Cur().type == t; }

	const Token& Advance(){
		const Token& t = tokens_[pos_];
		if(tokens_[pos_].type != TokenType::End) pos_++;
		return t;
	}

	[[noreturn]] void Fail(const std::string& message){
		throw HlcError(file_ + ":" + std::to_string(Cur().line) + ": error: " + message +
										" (got '" + (Cur().type == TokenType::End ? "<eof>" : Cur().text) + "')");
	}

	const Token& Expect(TokenType t, const std::string& message){
		if(!Check(t)) Fail(message);
		return Advance();
	}

	bool Match(TokenType t){
		if(Check(t)){ Advance(); return true; }
		return false;
	}

	/* ---- top level ---- */

	GlobalVarDecl ParseGlobalTail(bool isVolatile){
		Token name = Expect(TokenType::Identifier, "expected a global variable name");
		return ParseGlobalTailAfterName(isVolatile, name);
	}

	GlobalVarDecl ParseGlobalTailAfterName(bool isVolatile, const Token& name){
		GlobalVarDecl g;
		g.name 			= name.text;
		g.isVolatile = isVolatile;
		g.line 			= name.line;

		if(Match(TokenType::At)){
			Token addr = Expect(TokenType::IntLiteral, "expected an address after '@'");
			g.hasAddress = true;
			g.address 	 = addr.intValue;
		}
		if(Match(TokenType::LBracket)){
			Token size = Expect(TokenType::IntLiteral, "expected an array size");
			Expect(TokenType::RBracket, "expected ']'");
			g.isArray 	 = true;
			g.arraySize  = size.intValue;
		}
		if(Match(TokenType::Assign)){
			if(g.isArray) Fail("array globals cannot have an initializer in this language version");
			g.init = ParseExpr();
		}
		Expect(TokenType::Semicolon, "expected ';'");
		return g;
	}

	FuncDecl ParseFunc(bool isVoid){
		Token name = Expect(TokenType::Identifier, "expected a function name");
		return ParseFuncAfterName(isVoid, name);
	}

	FuncDecl ParseFuncAfterName(bool isVoid, const Token& name){
		FuncDecl f;
		f.isVoid = isVoid;
		f.name 	 = name.text;
		f.line 	 = name.line;

		Expect(TokenType::LParen, "expected '(' after function name");
		if(!Check(TokenType::RParen)){
			do{
				Expect(TokenType::KwInt, "expected 'int' in parameter list");
				Token p = Expect(TokenType::Identifier, "expected a parameter name");
				f.params.push_back(Param{p.text});
			} while(Match(TokenType::Comma));
		}
		Expect(TokenType::RParen, "expected ')'");

		f.body = ParseFuncBody();
		return f;
	}

	/* Function body: local declarations (C89-style, must come first), then statements. */
	std::unique_ptr<BlockStmt> ParseFuncBody(){
		auto block = std::make_unique<BlockStmt>();
		block->line = Cur().line;

		Expect(TokenType::LBrace, "expected '{'");

		while(Check(TokenType::KwInt)){
			block->stmts.push_back(ParseVarDecl());
		}
		while(!Check(TokenType::RBrace)){
			block->stmts.push_back(ParseStatement());
		}

		Expect(TokenType::RBrace, "expected '}'");
		return block;
	}

	StmtPtr ParseVarDecl(){
		Token kw = Expect(TokenType::KwInt, "expected 'int'");
		Token name = Expect(TokenType::Identifier, "expected a variable name");
		if(Check(TokenType::LBracket))
			Fail("local arrays are not supported in this language version (use a global array)");

		auto decl = std::make_unique<VarDeclStmt>();
		decl->line = kw.line;
		decl->name = name.text;
		if(Match(TokenType::Assign))
			decl->init = ParseExpr();
		Expect(TokenType::Semicolon, "expected ';'");
		return decl;
	}

	/* ---- statements (nested blocks: no declarations allowed, see ast.h) ---- */

	StmtPtr ParseStatement(){
		if(Check(TokenType::LBrace)) return ParseBlock();
		if(Check(TokenType::KwIf)) return ParseIf();
		if(Check(TokenType::KwWhile)) return ParseWhile();
		if(Check(TokenType::KwFor)) return ParseFor();
		if(Check(TokenType::KwReturn)) return ParseReturn();
		if(Check(TokenType::KwHalt)){
			auto h = std::make_unique<HaltStmt>();
			h->line = Advance().line;
			Expect(TokenType::Semicolon, "expected ';'");
			return h;
		}
		if(Check(TokenType::KwInt))
			Fail("local declarations must appear at the very start of the function body");

		auto stmt = std::make_unique<ExprStmt>();
		stmt->line = Cur().line;
		stmt->expr = ParseSimpleStmt();
		Expect(TokenType::Semicolon, "expected ';'");
		return stmt;
	}

	StmtPtr ParseBlock(){
		auto block = std::make_unique<BlockStmt>();
		block->line = Cur().line;
		Expect(TokenType::LBrace, "expected '{'");
		while(!Check(TokenType::RBrace)) block->stmts.push_back(ParseStatement());
		Expect(TokenType::RBrace, "expected '}'");
		return block;
	}

	StmtPtr ParseIf(){
		auto s = std::make_unique<IfStmt>();
		s->line = Advance().line; /* 'if' */
		Expect(TokenType::LParen, "expected '(' after 'if'");
		s->cond = ParseExpr();
		Expect(TokenType::RParen, "expected ')'");
		s->thenStmt = ParseStatement();
		if(Match(TokenType::KwElse))
			s->elseStmt = ParseStatement();
		return s;
	}

	StmtPtr ParseWhile(){
		auto s = std::make_unique<WhileStmt>();
		s->line = Advance().line; /* 'while' */
		Expect(TokenType::LParen, "expected '(' after 'while'");
		s->cond = ParseExpr();
		Expect(TokenType::RParen, "expected ')'");
		s->body = ParseStatement();
		return s;
	}

	StmtPtr ParseFor(){
		auto s = std::make_unique<ForStmt>();
		s->line = Advance().line; /* 'for' */
		Expect(TokenType::LParen, "expected '(' after 'for'");

		if(!Check(TokenType::Semicolon)){
			auto stmt = std::make_unique<ExprStmt>();
			stmt->expr = ParseSimpleStmt();
			s->init = std::move(stmt);
		}
		Expect(TokenType::Semicolon, "expected ';' after for-init");

		if(!Check(TokenType::Semicolon)) s->cond = ParseExpr();
		Expect(TokenType::Semicolon, "expected ';' after for-condition");

		if(!Check(TokenType::RParen)){
			auto stmt = std::make_unique<ExprStmt>();
			stmt->expr = ParseSimpleStmt();
			s->step = std::move(stmt);
		}
		Expect(TokenType::RParen, "expected ')' after for-step");

		s->body = ParseStatement();
		return s;
	}

	StmtPtr ParseReturn(){
		auto s = std::make_unique<ReturnStmt>();
		s->line = Advance().line; /* 'return' */
		if(!Check(TokenType::Semicolon))
			s->value = ParseExpr();
		Expect(TokenType::Semicolon, "expected ';'");
		return s;
	}

	/* IDENT "=" expr | IDENT "[" expr "]" "=" expr | IDENT "(" args? ")" */
	ExprPtr ParseSimpleStmt(){
		Token name = Expect(TokenType::Identifier, "expected an identifier");

		if(Match(TokenType::Assign)){
			auto a = std::make_unique<AssignExpr>();
			a->line = name.line;
			a->name = name.text;
			a->value = ParseExpr();
			return a;
		}
		if(Match(TokenType::LBracket)){
			auto a = std::make_unique<ArrayAssignExpr>();
			a->line = name.line;
			a->name = name.text;
			a->index = ParseExpr();
			Expect(TokenType::RBracket, "expected ']'");
			Expect(TokenType::Assign, "expected '=' after array index");
			a->value = ParseExpr();
			return a;
		}
		if(Match(TokenType::LParen)){
			auto c = std::make_unique<CallExpr>();
			c->line = name.line;
			c->name = name.text;
			if(!Check(TokenType::RParen)){
				do{ c->args.push_back(ParseExpr()); } while(Match(TokenType::Comma));
			}
			Expect(TokenType::RParen, "expected ')'");
			return c;
		}

		Fail("expected '=', '[' or '(' after identifier");
	}

	/* ---- expressions ---- */

	ExprPtr ParseExpr(){ return ParseLogicalOr(); }

	ExprPtr ParseLogicalOr(){
		ExprPtr lhs = ParseLogicalAnd();
		while(Check(TokenType::OrOr)){
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = "||"; b->lhs = std::move(lhs); b->rhs = ParseLogicalAnd();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseLogicalAnd(){
		ExprPtr lhs = ParseEquality();
		while(Check(TokenType::AndAnd)){
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = "&&"; b->lhs = std::move(lhs); b->rhs = ParseEquality();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseEquality(){
		ExprPtr lhs = ParseRelational();
		while(Check(TokenType::EqEq) || Check(TokenType::NotEq)){
			std::string op = Cur().type == TokenType::EqEq ? "==" : "!=";
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = op; b->lhs = std::move(lhs); b->rhs = ParseRelational();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseRelational(){
		ExprPtr lhs = ParseBitOr();
		while(Check(TokenType::Lt) || Check(TokenType::Gt) || Check(TokenType::Le) || Check(TokenType::Ge)){
			std::string op = Cur().text;
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = op; b->lhs = std::move(lhs); b->rhs = ParseBitOr();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseBitOr(){
		ExprPtr lhs = ParseBitXor();
		while(Check(TokenType::Pipe)){
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = "|"; b->lhs = std::move(lhs); b->rhs = ParseBitXor();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseBitXor(){
		ExprPtr lhs = ParseBitAnd();
		while(Check(TokenType::Caret)){
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = "^"; b->lhs = std::move(lhs); b->rhs = ParseBitAnd();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseBitAnd(){
		ExprPtr lhs = ParseShift();
		while(Check(TokenType::Amp)){
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = "&"; b->lhs = std::move(lhs); b->rhs = ParseShift();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseShift(){
		ExprPtr lhs = ParseAdditive();
		while(Check(TokenType::Shl) || Check(TokenType::Shr)){
			std::string op = Cur().type == TokenType::Shl ? "<<" : ">>";
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = op; b->lhs = std::move(lhs); b->rhs = ParseAdditive();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseAdditive(){
		ExprPtr lhs = ParseMultiplicative();
		while(Check(TokenType::Plus) || Check(TokenType::Minus)){
			std::string op = Cur().type == TokenType::Plus ? "+" : "-";
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = op; b->lhs = std::move(lhs); b->rhs = ParseMultiplicative();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseMultiplicative(){
		ExprPtr lhs = ParseUnary();
		while(Check(TokenType::Star) || Check(TokenType::Slash)){
			std::string op = Cur().type == TokenType::Star ? "*" : "/";
			int line = Advance().line;
			auto b = std::make_unique<BinaryExpr>();
			b->line = line; b->op = op; b->lhs = std::move(lhs); b->rhs = ParseUnary();
			lhs = std::move(b);
		}
		return lhs;
	}

	ExprPtr ParseUnary(){
		if(Check(TokenType::Minus) || Check(TokenType::Not) || Check(TokenType::Tilde)){
			std::string op = Cur().type == TokenType::Minus ? "-" : Cur().type == TokenType::Not ? "!" : "~";
			int line = Advance().line;
			auto u = std::make_unique<UnaryExpr>();
			u->line = line; u->op = op; u->operand = ParseUnary();
			return u;
		}
		return ParsePrimary();
	}

	ExprPtr ParsePrimary(){
		if(Check(TokenType::IntLiteral)){
			Token t = Advance();
			auto e = std::make_unique<IntLiteralExpr>();
			e->line = t.line; e->value = t.intValue;
			return e;
		}
		if(Match(TokenType::LParen)){
			ExprPtr e = ParseExpr();
			Expect(TokenType::RParen, "expected ')'");
			return e;
		}
		if(Check(TokenType::Identifier)){
			Token name = Advance();

			if(Match(TokenType::LParen)){
				auto c = std::make_unique<CallExpr>();
				c->line = name.line; c->name = name.text;
				if(!Check(TokenType::RParen)){
					do{ c->args.push_back(ParseExpr()); } while(Match(TokenType::Comma));
				}
				Expect(TokenType::RParen, "expected ')'");
				return c;
			}
			if(Match(TokenType::LBracket)){
				auto idx = std::make_unique<ArrayIndexExpr>();
				idx->line = name.line; idx->name = name.text;
				idx->index = ParseExpr();
				Expect(TokenType::RBracket, "expected ']'");
				return idx;
			}

			auto ref = std::make_unique<VarRefExpr>();
			ref->line = name.line; ref->name = name.text;
			return ref;
		}

		Fail("expected an expression");
	}
};

} // namespace

Program Parse(const std::vector<Token>& tokens, const std::string& fileName){
	Parser parser(tokens, fileName);
	return parser.Run();
}
