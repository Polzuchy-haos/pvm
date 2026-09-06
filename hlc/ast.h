#ifndef HLC_AST_H_
#define HLC_AST_H_

#include <memory>
#include <string>
#include <vector>

/*
 * v1 simplification, deliberate (see hlc/codegen.cpp for why): local variable
 * declarations are only allowed at the very start of a function body, before
 * any other statement (C89-style) -- every declared local must run exactly
 * once, unconditionally, or its compile-time FP offset stops matching what is
 * actually on the stack at runtime. The parser enforces this, not the AST.
 */

struct Expr {
	int line = 0;
	virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLiteralExpr : Expr {
	int value = 0;
};

struct VarRefExpr : Expr {
	std::string name;
};

struct ArrayIndexExpr : Expr {
	std::string name;
	ExprPtr 		index;
};

struct UnaryExpr : Expr {
	std::string op; /* "-", "!", "~" */
	ExprPtr 		operand;
};

struct BinaryExpr : Expr {
	std::string op; /* + - * / & | ^ << >> == != < > <= >= && || */
	ExprPtr 		lhs, rhs;
};

/* cond ? thenExpr : elseExpr -- exactly one branch is evaluated at runtime
 * (like if/else), leaving exactly one value on the stack either way. */
struct TernaryExpr : Expr {
	ExprPtr cond, thenExpr, elseExpr;
};

struct AssignExpr : Expr {
	std::string name;
	ExprPtr 		value;
};

struct ArrayAssignExpr : Expr {
	std::string name;
	ExprPtr 		index;
	ExprPtr 		value;
};

struct CallExpr : Expr {
	std::string 				name;
	std::vector<ExprPtr> args;
};

/* svc(numberExpr, arg1, ..., argN) -- pops the call number and dispatches to
 * a C handler at runtime; by convention it always leaves exactly one value
 * on the stack (the handler's result), same as any other Expr. */
struct SvcCallExpr : Expr {
	ExprPtr 						 number;
	std::vector<ExprPtr> args;
};

/* sizeof(arrayName) -- resolves to the array's element count, known once its
 * declaration has been registered (see Codegen::RegisterGlobalMeta); folds
 * straight to "PUSH <size>", same as a literal. Not usable in a 'const'
 * initializer (constants are evaluated before any global is registered --
 * see Codegen::Run) or in an @address/[size] position -- only inside
 * function bodies. */
struct SizeofExpr : Expr {
	std::string name;
};

struct Stmt {
	int line = 0;
	virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct BlockStmt : Stmt {
	std::vector<StmtPtr> stmts;
};

struct IfStmt : Stmt {
	ExprPtr cond;
	StmtPtr thenStmt;
	StmtPtr elseStmt; /* may be null */
};

struct WhileStmt : Stmt {
	ExprPtr cond;
	StmtPtr body;
};

/* do <body> while (<cond>); -- body runs once unconditionally, then repeats
 * while cond holds (checked after the body, unlike WhileStmt). */
struct DoWhileStmt : Stmt {
	ExprPtr cond;
	StmtPtr body;
};

struct ForStmt : Stmt {
	StmtPtr init; /* simple statement (assignment/call), may be null */
	ExprPtr cond; /* may be null */
	StmtPtr step; /* simple statement, may be null */
	StmtPtr body;
};

struct ReturnStmt : Stmt {
	ExprPtr value; /* may be null (void return) */
};

struct HaltStmt : Stmt {};

struct ExprStmt : Stmt {
	ExprPtr expr;
};

struct VarDeclStmt : Stmt {
	std::string name;
	ExprPtr 		init; /* may be null */
};

struct Param {
	std::string name;
};

struct FuncDecl {
	bool 									isVoid = false;
	std::string 					name;
	std::vector<Param> 		params;
	std::unique_ptr<BlockStmt> body; /* null for "extern ... from asmFile;" */
	bool 									isExtern = false;
	std::string 					asmFile; /* only meaningful when isExtern */
	int 									line = 0;
};

struct GlobalVarDecl {
	std::string name;
	bool 				isVolatile 		= false;
	bool 				hasAddress 		= false;
	ExprPtr 		addressExpr; 	/* valid when hasAddress; must fold to a constant (see codegen) */
	bool 				isArray 			= false;
	ExprPtr 		arraySizeExpr;/* valid when isArray; must fold to a constant (see codegen) */
	ExprPtr 		init; 				/* scalar initializer; may be null; must fold to a constant (see codegen) */
	std::vector<ExprPtr> arrayInit; /* array initializer ("= {a, b, ...}"); empty if none; each element must fold to a constant */
	int 				line 					= 0;
};

/*
 * const int NAME = <constant expression>;
 * Compile-time only: never allocates RAM and never appears in the emitted
 * .pvms as a symbol -- every use is inlined as a plain "PUSH <value>" (or
 * folded further, e.g. into a global's address/array size) by the codegen's
 * constant evaluator (see Codegen::EvalConstExpr).
 */
struct ConstDecl {
	std::string name;
	ExprPtr 		value;
	int 				line = 0;
};

struct Program {
	std::vector<GlobalVarDecl> globals;
	std::vector<ConstDecl> 	 consts;
	std::vector<FuncDecl> 		 funcs;
};

#endif /* HLC_AST_H_ */
