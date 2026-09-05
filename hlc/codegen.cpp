#include "codegen.h"
#include "pvm.h"

#include <sstream>
#include <unordered_map>

namespace {

struct FuncInfo {
	bool isVoid;
	int 	paramCount;
};

struct GlobalInfo {
	bool isArray;
	int 	size;
	int 	address;
};

class Codegen {
public:
	explicit Codegen(const std::string& fileName) : file_(fileName) {}

	std::string Run(const Program& program){
		for(const FuncDecl& f : program.funcs){
			if(funcs_.count(f.name))
				Fail(f.line, "function '" + f.name + "' redefined");
			funcs_[f.name] = FuncInfo{f.isVoid, (int)f.params.size()};
		}

		auto mainIt = funcs_.find("main");
		if(mainIt == funcs_.end())
			throw HlcError(file_ + ": error: no 'main' function defined");
		if(mainIt->second.paramCount != 0)
			throw HlcError(file_ + ": error: 'main' must take no parameters");
		if(!mainIt->second.isVoid)
			throw HlcError(file_ + ": error: 'main' must be declared 'void main()' "
											"(it has no caller, so a return value has nowhere to go)");

		for(const GlobalVarDecl& g : program.globals)
			RegisterGlobal(g);

		out_ << ".entry main\n\n";

		for(const GlobalVarDecl& g : program.globals)
			EmitGlobalConst(g);
		if(!program.globals.empty()) out_ << "\n";

		for(const FuncDecl& f : program.funcs)
			EmitFunction(f, program);

		return out_.str();
	}

private:
	std::string 													 file_;
	std::ostringstream 										 out_;
	std::unordered_map<std::string, FuncInfo>   funcs_;
	std::unordered_map<std::string, GlobalInfo> globals_;
	int 																	 nextRamAddr_ = 0;
	int 																	 labelCounter_ = 0;

	/* per-function codegen state */
	std::unordered_map<std::string, int> locals_;
	int 	nextLocalOffset_ 		= 0;
	bool 	curFuncIsVoid_ 			= false;
	bool 	curFuncIsMain_ 			= false;
	int 	curReturnSlotOffset_ = 0;

	void Line(const std::string& s){ out_ << "  " << s << "\n"; }
	void Label(const std::string& name){ out_ << name << ":\n"; }

	std::string NewLabel(const std::string& prefix){
		return "__" + prefix + std::to_string(labelCounter_++);
	}

	[[noreturn]] void Fail(int line, const std::string& msg){
		throw HlcError(file_ + ":" + std::to_string(line) + ": error: " + msg);
	}

	/* ---- globals ---- */

	void RegisterGlobal(const GlobalVarDecl& g){
		if(globals_.count(g.name) || funcs_.count(g.name))
			Fail(g.line, "'" + g.name + "' redefined");

		int size = g.isArray ? g.arraySize : 1;
		if(size <= 0)
			Fail(g.line, "array size must be positive");

		int addr;
		if(g.hasAddress){
			addr = g.address;
		}
		else{
			if(nextRamAddr_ + size > PVM_CFG_RAM_SIZE)
				Fail(g.line, "global '" + g.name + "' does not fit in RAM (PVM_CFG_RAM_SIZE=" +
											std::to_string(PVM_CFG_RAM_SIZE) + ")");
			addr = nextRamAddr_;
			nextRamAddr_ += size;
		}

		globals_[g.name] = GlobalInfo{g.isArray, size, addr};
	}

	void EmitGlobalConst(const GlobalVarDecl& g){
		out_ << ".const " << g.name << " " << globals_[g.name].address << "\n";
	}

	void EmitGlobalInitializers(const Program& program){
		for(const GlobalVarDecl& g : program.globals){
			if(!g.init) continue;
			auto lit = dynamic_cast<IntLiteralExpr*>(g.init.get());
			if(!lit)
				Fail(g.line, "global initializer for '" + g.name + "' must be a constant integer literal");
			Line("PUSH " + std::to_string(lit->value));
			Line("STORE " + g.name);
		}
	}

	/* ---- functions ---- */

	void EmitFunction(const FuncDecl& f, const Program& program){
		locals_.clear();
		nextLocalOffset_ 		= 0;
		curFuncIsVoid_ 			= f.isVoid;
		curFuncIsMain_ 			= (f.name == "main");
		curReturnSlotOffset_ = -(3 + (int)f.params.size());

		int n = (int)f.params.size();
		for(int i = 0; i < n; i++){
			if(locals_.count(f.params[i].name))
				Fail(f.line, "duplicate parameter name '" + f.params[i].name + "'");
			locals_[f.params[i].name] = -(n + 2 - i);
		}

		Label(f.name);
		Line("ENTER");

		if(curFuncIsMain_) EmitGlobalInitializers(program);

		for(const StmtPtr& stmt : f.body->stmts)
			EmitStmt(stmt.get());

		Line("LEAVE");
		Line(curFuncIsMain_ ? "HALT" : "RET");
		out_ << "\n";
	}

	/* ---- statements ---- */

	void EmitStmt(Stmt* s){
		if(auto x = dynamic_cast<BlockStmt*>(s)){ for(const StmtPtr& st : x->stmts) EmitStmt(st.get()); return; }
		if(auto x = dynamic_cast<IfStmt*>(s)){ EmitIf(x); return; }
		if(auto x = dynamic_cast<WhileStmt*>(s)){ EmitWhile(x); return; }
		if(auto x = dynamic_cast<ForStmt*>(s)){ EmitFor(x); return; }
		if(auto x = dynamic_cast<ReturnStmt*>(s)){ EmitReturn(x); return; }
		if(dynamic_cast<HaltStmt*>(s)){ Line("HALT"); return; }
		if(auto x = dynamic_cast<ExprStmt*>(s)){ EmitExprDiscard(x->expr.get()); return; }
		if(auto x = dynamic_cast<VarDeclStmt*>(s)){ EmitVarDecl(x); return; }
		Fail(s->line, "internal: unknown statement node");
	}

	void EmitIf(IfStmt* s){
		std::string labelElse = NewLabel("if_else");
		std::string labelEnd  = NewLabel("if_end");

		EmitExpr(s->cond.get());
		Line("JZ " + labelElse);
		EmitStmt(s->thenStmt.get());
		if(s->elseStmt) Line("JMP " + labelEnd);
		Label(labelElse);
		if(s->elseStmt){
			EmitStmt(s->elseStmt.get());
			Label(labelEnd);
		}
	}

	void EmitWhile(WhileStmt* s){
		std::string labelStart = NewLabel("while_start");
		std::string labelEnd 	= NewLabel("while_end");

		Label(labelStart);
		EmitExpr(s->cond.get());
		Line("JZ " + labelEnd);
		EmitStmt(s->body.get());
		Line("JMP " + labelStart);
		Label(labelEnd);
	}

	void EmitFor(ForStmt* s){
		if(s->init) EmitStmt(s->init.get());

		std::string labelStart = NewLabel("for_start");
		std::string labelEnd 	= NewLabel("for_end");

		Label(labelStart);
		if(s->cond){
			EmitExpr(s->cond.get());
			Line("JZ " + labelEnd);
		}
		EmitStmt(s->body.get());
		if(s->step) EmitStmt(s->step.get());
		Line("JMP " + labelStart);
		Label(labelEnd);
	}

	void EmitReturn(ReturnStmt* s){
		if(s->value){
			if(curFuncIsVoid_) Fail(s->line, "void function cannot return a value");
			EmitExpr(s->value.get());
			Line("STOREFP " + std::to_string(curReturnSlotOffset_));
		}
		else if(!curFuncIsVoid_){
			Fail(s->line, "non-void function must return a value");
		}
		Line("LEAVE");
		Line(curFuncIsMain_ ? "HALT" : "RET");
	}

	void EmitVarDecl(VarDeclStmt* s){
		if(locals_.count(s->name))
			Fail(s->line, "'" + s->name + "' redeclared");
		int offset = nextLocalOffset_++;
		locals_[s->name] = offset;
		if(s->init) EmitExpr(s->init.get());
		else Line("PUSH 0");
	}

	/* Statement-context expression: a call to a void function emits nothing
	 * extra to discard; anything else leaves one value that must be popped. */
	void EmitExprDiscard(Expr* e){
		if(auto call = dynamic_cast<CallExpr*>(e)){ EmitCall(call, false); return; }
		EmitExpr(e);
		Line("POP");
	}

	/* ---- expressions (each leaves exactly one value on the stack) ---- */

	void EmitExpr(Expr* e){
		if(auto x = dynamic_cast<IntLiteralExpr*>(e)){ Line("PUSH " + std::to_string(x->value)); return; }
		if(auto x = dynamic_cast<VarRefExpr*>(e)){ EmitVarRead(x->line, x->name); return; }
		if(auto x = dynamic_cast<ArrayIndexExpr*>(e)){ EmitArrayRead(x); return; }
		if(auto x = dynamic_cast<UnaryExpr*>(e)){ EmitUnary(x); return; }
		if(auto x = dynamic_cast<BinaryExpr*>(e)){ EmitBinary(x); return; }
		if(auto x = dynamic_cast<AssignExpr*>(e)){ EmitAssign(x); return; }
		if(auto x = dynamic_cast<ArrayAssignExpr*>(e)){ EmitArrayAssign(x); return; }
		if(auto x = dynamic_cast<CallExpr*>(e)){ EmitCall(x, true); return; }
		Fail(e->line, "internal: unknown expression node");
	}

	void EmitVarRead(int line, const std::string& name){
		auto it = locals_.find(name);
		if(it != locals_.end()){ Line("LOADFP " + std::to_string(it->second)); return; }

		auto git = globals_.find(name);
		if(git != globals_.end()){
			if(git->second.isArray) Fail(line, "'" + name + "' is an array; use " + name + "[index]");
			Line("LOAD " + name);
			return;
		}
		Fail(line, "undefined variable '" + name + "'");
	}

	void EmitVarWrite(int line, const std::string& name){
		auto it = locals_.find(name);
		if(it != locals_.end()){ Line("STOREFP " + std::to_string(it->second)); return; }

		auto git = globals_.find(name);
		if(git != globals_.end()){
			if(git->second.isArray) Fail(line, "'" + name + "' is an array; use " + name + "[index] = ...");
			Line("STORE " + name);
			return;
		}
		Fail(line, "undefined variable '" + name + "'");
	}

	void EmitArrayRead(ArrayIndexExpr* x){
		auto git = globals_.find(x->name);
		if(git == globals_.end()) Fail(x->line, "undefined array '" + x->name + "'");
		if(!git->second.isArray) Fail(x->line, "'" + x->name + "' is not an array");

		EmitExpr(x->index.get());
		Line("PUSH " + x->name);
		Line("ADD");
		Line("LOADI");
	}

	void EmitArrayAssign(ArrayAssignExpr* x){
		auto git = globals_.find(x->name);
		if(git == globals_.end()) Fail(x->line, "undefined array '" + x->name + "'");
		if(!git->second.isArray) Fail(x->line, "'" + x->name + "' is not an array");

		EmitExpr(x->value.get());
		Line("DUP");
		EmitExpr(x->index.get());
		Line("PUSH " + x->name);
		Line("ADD");
		Line("STOREI");
	}

	void EmitAssign(AssignExpr* x){
		EmitExpr(x->value.get());
		Line("DUP");
		EmitVarWrite(x->line, x->name);
	}

	void EmitCall(CallExpr* call, bool wantValue){
		auto it = funcs_.find(call->name);
		if(it == funcs_.end()) Fail(call->line, "undefined function '" + call->name + "'");
		const FuncInfo& info = it->second;

		if((int)call->args.size() != info.paramCount)
			Fail(call->line, "function '" + call->name + "' expects " + std::to_string(info.paramCount) +
										" argument(s), got " + std::to_string(call->args.size()));
		if(info.isVoid && wantValue)
			Fail(call->line, "cannot use void function '" + call->name + "' as a value");

		if(!info.isVoid) Line("PUSH 0"); /* reserve the return slot */
		for(const ExprPtr& arg : call->args) EmitExpr(arg.get());
		Line("CALL " + call->name);
		for(size_t i = 0; i < call->args.size(); i++) Line("POP"); /* drop the stale argument copies */
		if(!info.isVoid && !wantValue) Line("POP"); /* caller doesn't want the value either */
	}

	void EmitUnary(UnaryExpr* x){
		if(x->op == "-"){ Line("PUSH 0"); EmitExpr(x->operand.get()); Line("SUB"); return; }
		if(x->op == "~"){ EmitExpr(x->operand.get()); Line("NOT"); return; }
		if(x->op == "!"){ EmitExpr(x->operand.get()); Line("PUSH 0"); Line("EQ"); return; }
		Fail(x->line, "internal: unknown unary operator '" + x->op + "'");
	}

	void EmitBinary(BinaryExpr* b){
		if(b->op == "&&"){ EmitLogicalAnd(b); return; }
		if(b->op == "||"){ EmitLogicalOr(b); return; }

		static const std::unordered_map<std::string, std::string> kDirect = {
			{"+", "ADD"}, {"-", "SUB"}, {"*", "MUL"}, {"/", "DIV"},
			{"&", "AND"}, {"|", "OR"}, {"^", "XOR"}, {"<<", "SHL"}, {">>", "SHR"},
			{"==", "EQ"}, {"<", "LT"},
		};

		auto it = kDirect.find(b->op);
		if(it != kDirect.end()){
			EmitExpr(b->lhs.get());
			EmitExpr(b->rhs.get());
			Line(it->second);
			return;
		}

		/* Only LT/EQ exist as opcodes; synthesize the rest from them. */
		if(b->op == ">"){ EmitExpr(b->rhs.get()); EmitExpr(b->lhs.get()); Line("LT"); return; }
		if(b->op == "<="){ EmitExpr(b->rhs.get()); EmitExpr(b->lhs.get()); Line("LT"); Line("PUSH 0"); Line("EQ"); return; }
		if(b->op == ">="){ EmitExpr(b->lhs.get()); EmitExpr(b->rhs.get()); Line("LT"); Line("PUSH 0"); Line("EQ"); return; }
		if(b->op == "!="){ EmitExpr(b->lhs.get()); EmitExpr(b->rhs.get()); Line("EQ"); Line("PUSH 0"); Line("EQ"); return; }

		Fail(b->line, "internal: unknown binary operator '" + b->op + "'");
	}

	/* Short-circuit: neither opcode nor a general "boolean" type exists in the VM,
	 * so && / || are lowered to explicit branches, like if/while, with the result
	 * normalized to strictly 0/1 (PUSH 0;EQ;PUSH 0;EQ is "!!x", same trick C uses). */
	void EmitLogicalAnd(BinaryExpr* b){
		std::string labelFalse = NewLabel("and_false");
		std::string labelEnd 	= NewLabel("and_end");

		EmitExpr(b->lhs.get());
		Line("JZ " + labelFalse);
		EmitExpr(b->rhs.get());
		Line("PUSH 0"); Line("EQ"); Line("PUSH 0"); Line("EQ");
		Line("JMP " + labelEnd);
		Label(labelFalse);
		Line("PUSH 0");
		Label(labelEnd);
	}

	void EmitLogicalOr(BinaryExpr* b){
		std::string labelTrue = NewLabel("or_true");
		std::string labelEnd  = NewLabel("or_end");

		EmitExpr(b->lhs.get());
		Line("JNZ " + labelTrue);
		EmitExpr(b->rhs.get());
		Line("PUSH 0"); Line("EQ"); Line("PUSH 0"); Line("EQ");
		Line("JMP " + labelEnd);
		Label(labelTrue);
		Line("PUSH 1");
		Label(labelEnd);
	}
};

} // namespace

std::string Generate(const Program& program, const std::string& fileName){
	Codegen codegen(fileName);
	return codegen.Run(program);
}
