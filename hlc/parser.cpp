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
				Token name = Expect(TokenType::Identifier, "expected a global variable name");
				ParseGlobalDeclList(true, name, program.globals);
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
					ParseGlobalDeclList(false, name, program.globals);
			}
			else if(Check(TokenType::KwExtern)){
				program.funcs.push_back(ParseExternFunc());
			}
			else if(Check(TokenType::KwConst)){
				program.consts.push_back(ParseConstDecl());
			}
			else if(Check(TokenType::KwSection)){
				program.sections.push_back(ParseSectionDecl());
			}
			else{
				Fail("expected a global declaration or a function "
							"('int'/'void'/'volatile'/'const'/'extern'/'section')");
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

	/* Records where a statement or function came from. The file travels with the
	 * line because "import" splices another file's tokens into this stream, so
	 * "line 12" on its own does not identify a place in the source. */
	template<typename T> T* Mark(T* node, const Token& t){
		node->line = t.line;
		node->file = t.file;
		return node;
	}

	bool Match(TokenType t){
		if(Check(t)){ Advance(); return true; }
		return false;
	}

	/* ---- top level ---- */

	/* "{" expr ("," expr)* "}" -- an array initializer's element list. Each
	 * element must fold to a constant (checked in codegen, same as any other
	 * global initializer); the parser only enforces the surface syntax. */
	std::vector<ExprPtr> ParseArrayInitList(){
		std::vector<ExprPtr> values;
		Expect(TokenType::LBrace, "expected '{' to start an array initializer");
		if(!Check(TokenType::RBrace)){
			do{ values.push_back(ParseExpr()); } while(Match(TokenType::Comma));
		}
		Expect(TokenType::RBrace, "expected '}'");
		return values;
	}

	/* One declarator's tail: optional "@ address", optional "[size]", optional
	 * "= init" (a single expression, or a "{...}" list when it's an array).
	 * Does NOT consume a trailing ';' or ',' -- see ParseGlobalDeclList. */
	GlobalVarDecl ParseOneGlobalDeclarator(bool isVolatile, const Token& name){
		GlobalVarDecl g;
		g.name 			= name.text;
		g.isVolatile = isVolatile;
		g.line 			= name.line;

		if(Match(TokenType::At)){
			g.hasAddress 	= true;
			g.addressExpr = ParseExpr(); /* usually a literal or a 'const', folded in codegen */
		}
		if(Match(TokenType::LBracket)){
			g.isArray 			= true;
			g.arraySizeExpr = ParseExpr(); /* usually a literal or a 'const', folded in codegen */
			Expect(TokenType::RBracket, "expected ']'");
		}
		if(Match(TokenType::Assign)){
			if(g.isArray) g.arrayInit = ParseArrayInitList();
			else g.init = ParseExpr();
		}
		return g;
	}

	/* "int"/"volatile int" was already consumed; `firstName` is the identifier
	 * right after it. Parses one or more comma-separated declarators (each may
	 * have its own "@..."/"[...]"/"=...") and the final ';'. */
	void ParseGlobalDeclList(bool isVolatile, const Token& firstName, std::vector<GlobalVarDecl>& out){
		out.push_back(ParseOneGlobalDeclarator(isVolatile, firstName));
		while(Match(TokenType::Comma)){
			Token name = Expect(TokenType::Identifier, "expected a global variable name");
			out.push_back(ParseOneGlobalDeclarator(isVolatile, name));
		}
		Expect(TokenType::Semicolon, "expected ';'");
	}

	/* const int NAME = <constant expression>; -- codegen checks the expression
	 * actually folds to a constant (see Codegen::EvalConstExpr); the parser
	 * only enforces the surface syntax. */
	ConstDecl ParseConstDecl(){
		Token kw = Expect(TokenType::KwConst, "expected 'const'");
		Expect(TokenType::KwInt, "expected 'int' after 'const'");
		Token name = Expect(TokenType::Identifier, "expected a constant name");
		Expect(TokenType::Assign, "'const' requires an initializer ('= <expression>')");

		ConstDecl c;
		c.line 	= kw.line;
		c.name 	= name.text;
		c.value = ParseExpr();
		Expect(TokenType::Semicolon, "expected ';'");
		return c;
	}

	/* "section <name> <expr>;" -- plus a second, comma-separated expression for
	 * usrsect, whose first value is a base address rather than a size. Which
	 * names are meaningful (and which of them take two values) is decided by the
	 * codegen, together with everything else about what the sizes mean. */
	SectionDecl ParseSectionDecl(){
		Token kw 	 = Expect(TokenType::KwSection, "expected 'section'");
		Token name = Expect(TokenType::Identifier, "expected a section name after 'section'");

		SectionDecl s;
		s.line 	= kw.line;
		s.name 	= name.text;
		s.value = ParseExpr();
		if(Match(TokenType::Comma))
			s.length = ParseExpr();
		Expect(TokenType::Semicolon, "expected ';'");
		return s;
	}

	FuncDecl ParseFunc(bool isVoid){
		Token name = Expect(TokenType::Identifier, "expected a function name");
		return ParseFuncAfterName(isVoid, name);
	}

	FuncDecl ParseFuncAfterName(bool isVoid, const Token& name){
		FuncDecl f;
		f.isVoid = isVoid;
		f.name 	 = name.text;
		Mark(&f, name);

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

	/*
	 * extern int f(int a, int b) from asmfile;
	 * No body: the implementation is a hand-written .pvms label named `f`,
	 * pulled in via ".import asmfile" (codegen resolves asmfile to a real
	 * path and emits that -- see hlc/codegen.cpp). The compiler trusts that
	 * the label follows the same ENTER/LEAVE/LOADFP/STOREFP calling
	 * convention it generates for every other function.
	 */
	FuncDecl ParseExternFunc(){
		Token kw = Expect(TokenType::KwExtern, "expected 'extern'");

		FuncDecl f;
		f.isExtern = true;
		Mark(&f, kw);

		if(Check(TokenType::KwVoid)){ f.isVoid = true; Advance(); }
		else Expect(TokenType::KwInt, "expected 'int' or 'void' after 'extern'");

		Token name = Expect(TokenType::Identifier, "expected a function name");
		f.name = name.text;

		Expect(TokenType::LParen, "expected '(' after function name");
		if(!Check(TokenType::RParen)){
			do{
				Expect(TokenType::KwInt, "expected 'int' in parameter list");
				Token p = Expect(TokenType::Identifier, "expected a parameter name");
				f.params.push_back(Param{p.text});
			} while(Match(TokenType::Comma));
		}
		Expect(TokenType::RParen, "expected ')'");

		Expect(TokenType::KwFrom, "expected 'from' after extern function signature");
		Token asmFile = Expect(TokenType::Identifier, "expected an assembly file name after 'from'");
		f.asmFile = asmFile.text;

		Expect(TokenType::Semicolon, "expected ';'");
		return f;
	}

	/* Function body: local declarations (C89-style, must come first), then statements. */
	std::unique_ptr<BlockStmt> ParseFuncBody(){
		auto block = std::make_unique<BlockStmt>();
		Mark(block.get(), Cur());

		Expect(TokenType::LBrace, "expected '{'");

		while(Check(TokenType::KwInt)){
			ParseVarDeclList(block->stmts);
		}
		while(!Check(TokenType::RBrace)){
			block->stmts.push_back(ParseStatement());
		}

		Expect(TokenType::RBrace, "expected '}'");
		return block;
	}

	/* "int" NAME ("=" expr)? ("," NAME ("=" expr)?)* ";" -- appends one
	 * VarDeclStmt per name, in order, to `out`. */
	void ParseVarDeclList(std::vector<StmtPtr>& out){
		Token kw = Expect(TokenType::KwInt, "expected 'int'");
		do{
			Token name = Expect(TokenType::Identifier, "expected a variable name");
			if(Check(TokenType::LBracket))
				Fail("local arrays are not supported in this language version (use a global array)");

			auto decl = std::make_unique<VarDeclStmt>();
			Mark(decl.get(), kw);
			decl->name = name.text;
			if(Match(TokenType::Assign))
				decl->init = ParseExpr();
			out.push_back(std::move(decl));
		} while(Match(TokenType::Comma));
		Expect(TokenType::Semicolon, "expected ';'");
	}

	/* ---- statements (nested blocks: no declarations allowed, see ast.h) ---- */

	StmtPtr ParseStatement(){
		if(Check(TokenType::LBrace)) return ParseBlock();
		if(Check(TokenType::KwIf)) return ParseIf();
		if(Check(TokenType::KwWhile)) return ParseWhile();
		if(Check(TokenType::KwDo)) return ParseDoWhile();
		if(Check(TokenType::KwFor)) return ParseFor();
		if(Check(TokenType::KwReturn)) return ParseReturn();
		if(Check(TokenType::KwHalt)){
			auto h = std::make_unique<HaltStmt>();
			Mark(h.get(), Advance());
			Expect(TokenType::Semicolon, "expected ';'");
			return h;
		}
		if(Check(TokenType::KwInt))
			Fail("local declarations must appear at the very start of the function body");

		if(Check(TokenType::KwSvc)){
			auto stmt = std::make_unique<ExprStmt>();
			Mark(stmt.get(), Cur());
			stmt->expr = ParseSvcCall();
			Expect(TokenType::Semicolon, "expected ';'");
			return stmt;
		}

		auto stmt = std::make_unique<ExprStmt>();
		Mark(stmt.get(), Cur());
		stmt->expr = ParseSimpleStmt();
		Expect(TokenType::Semicolon, "expected ';'");
		return stmt;
	}

	StmtPtr ParseBlock(){
		auto block = std::make_unique<BlockStmt>();
		Mark(block.get(), Cur());
		Expect(TokenType::LBrace, "expected '{'");
		while(!Check(TokenType::RBrace)) block->stmts.push_back(ParseStatement());
		Expect(TokenType::RBrace, "expected '}'");
		return block;
	}

	StmtPtr ParseIf(){
		auto s = std::make_unique<IfStmt>();
		Mark(s.get(), Advance()); /* 'if' */
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
		Mark(s.get(), Advance()); /* 'while' */
		Expect(TokenType::LParen, "expected '(' after 'while'");
		s->cond = ParseExpr();
		Expect(TokenType::RParen, "expected ')'");
		s->body = ParseStatement();
		return s;
	}

	StmtPtr ParseDoWhile(){
		auto s = std::make_unique<DoWhileStmt>();
		Mark(s.get(), Advance()); /* 'do' */
		s->body = ParseStatement();
		Expect(TokenType::KwWhile, "expected 'while' after 'do' block");
		Expect(TokenType::LParen, "expected '(' after 'while'");
		s->cond = ParseExpr();
		Expect(TokenType::RParen, "expected ')'");
		Expect(TokenType::Semicolon, "expected ';' after 'do ... while (...)'");
		return s;
	}

	StmtPtr ParseFor(){
		auto s = std::make_unique<ForStmt>();
		Mark(s.get(), Advance()); /* 'for' */
		Expect(TokenType::LParen, "expected '(' after 'for'");

		if(!Check(TokenType::Semicolon)){
			auto stmt = std::make_unique<ExprStmt>();
			Mark(stmt.get(), Cur());
			stmt->expr = ParseSimpleStmt();
			s->init = std::move(stmt);
		}
		Expect(TokenType::Semicolon, "expected ';' after for-init");

		if(!Check(TokenType::Semicolon)) s->cond = ParseExpr();
		Expect(TokenType::Semicolon, "expected ';' after for-condition");

		if(!Check(TokenType::RParen)){
			auto stmt = std::make_unique<ExprStmt>();
			Mark(stmt.get(), Cur());
			stmt->expr = ParseSimpleStmt();
			s->step = std::move(stmt);
		}
		Expect(TokenType::RParen, "expected ')' after for-step");

		s->body = ParseStatement();
		return s;
	}

	StmtPtr ParseReturn(){
		auto s = std::make_unique<ReturnStmt>();
		Mark(s.get(), Advance()); /* 'return' */
		if(!Check(TokenType::Semicolon))
			s->value = ParseExpr();
		Expect(TokenType::Semicolon, "expected ';'");
		return s;
	}

	/* Token for one of "+= -= *= /= &= |= ^= <<= >>=", or "" if `t` isn't one. */
	static std::string CompoundAssignOp(TokenType t){
		switch(t){
			case TokenType::PlusEq: 	return "+";
			case TokenType::MinusEq: return "-";
			case TokenType::StarEq: 	return "*";
			case TokenType::SlashEq: return "/";
			case TokenType::AmpEq: 	return "&";
			case TokenType::PipeEq: 	return "|";
			case TokenType::CaretEq: return "^";
			case TokenType::ShlEq: 	return "<<";
			case TokenType::ShrEq: 	return ">>";
			default: 					return "";
		}
	}

	/* Desugars "name op= rhs" (and "name++"/"name--", rhs = literal 1) into
	 * "name = name op rhs" -- reuses AssignExpr/BinaryExpr as-is, so codegen
	 * needs no changes at all. Only ever applied to plain names (locals,
	 * globals, consts, params): array elements don't get compound assignment
	 * in this language version, since desugaring "arr[i] op= rhs" the same
	 * way would evaluate the index expression `i` twice -- fine if `i` is a
	 * simple variable, silently wrong if it has a side effect (a call, svc).
	 */
	ExprPtr MakeCompoundAssign(const Token& name, const std::string& op, ExprPtr rhs){
		auto ref = std::make_unique<VarRefExpr>();
		ref->line = name.line;
		ref->name = name.text;

		auto bin = std::make_unique<BinaryExpr>();
		bin->line = name.line;
		bin->op 	= op;
		bin->lhs 	= std::move(ref);
		bin->rhs 	= std::move(rhs);

		auto assign = std::make_unique<AssignExpr>();
		assign->line 	= name.line;
		assign->name 	= name.text;
		assign->value = std::move(bin);
		return assign;
	}

	/*
	 * IDENT "=" expr
	 * IDENT ("+=".."/=" | "&=" | "|=" | "^=" | "<<=" | ">>=") expr
	 * IDENT ("++" | "--")
	 * IDENT "[" expr "]" "=" expr
	 * IDENT "(" args? ")"
	 */
	ExprPtr ParseSimpleStmt(){
		Token name = Expect(TokenType::Identifier, "expected an identifier");

		if(Match(TokenType::Assign)){
			auto a = std::make_unique<AssignExpr>();
			a->line = name.line;
			a->name = name.text;
			a->value = ParseExpr();
			return a;
		}

		std::string compoundOp = CompoundAssignOp(Cur().type);
		if(!compoundOp.empty()){
			Advance();
			return MakeCompoundAssign(name, compoundOp, ParseExpr());
		}

		if(Check(TokenType::PlusPlus) || Check(TokenType::MinusMinus)){
			std::string op = Check(TokenType::PlusPlus) ? "+" : "-";
			Advance();
			auto one = std::make_unique<IntLiteralExpr>();
			one->line = name.line;
			one->value = 1;
			return MakeCompoundAssign(name, op, std::move(one));
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

		Fail("expected '=', a compound assignment, '++', '--', '[' or '(' after identifier");
	}

	/* ---- expressions ---- */

	ExprPtr ParseExpr(){ return ParseTernary(); }

	/* cond ? thenExpr : elseExpr -- lowest precedence, right-associative
	 * (both branches recurse into ParseExpr/ParseTernary, so "a ? b : c ? d : e"
	 * parses as "a ? b : (c ? d : e)", same as C). */
	ExprPtr ParseTernary(){
		ExprPtr cond = ParseLogicalOr();
		if(!Match(TokenType::Question)) return cond;

		auto t = std::make_unique<TernaryExpr>();
		t->line = cond->line;
		t->cond = std::move(cond);
		t->thenExpr = ParseExpr();
		Expect(TokenType::Colon, "expected ':' in ternary expression");
		t->elseExpr = ParseTernary();
		return t;
	}

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

	/* svc(numberExpr, arg1, ..., argN) */
	ExprPtr ParseSvcCall(){
		Token kw = Expect(TokenType::KwSvc, "expected 'svc'");
		Expect(TokenType::LParen, "expected '(' after 'svc'");

		auto call = std::make_unique<SvcCallExpr>();
		call->line = kw.line;
		call->number = ParseExpr();
		while(Match(TokenType::Comma)) call->args.push_back(ParseExpr());

		Expect(TokenType::RParen, "expected ')'");
		return call;
	}

	ExprPtr ParsePrimary(){
		if(Check(TokenType::KwSvc)) return ParseSvcCall();
		if(Check(TokenType::KwSizeof)){
			Token kw = Advance();
			Expect(TokenType::LParen, "expected '(' after 'sizeof'");
			Token name = Expect(TokenType::Identifier, "expected an array name inside 'sizeof(...)'");
			Expect(TokenType::RParen, "expected ')'");
			auto s = std::make_unique<SizeofExpr>();
			s->line = kw.line;
			s->name = name.text;
			return s;
		}
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
