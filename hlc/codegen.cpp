#include "codegen.h"
#include "modulepath.h"
#include "pvm.h"

#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

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
	Codegen(const std::string& fileName, const std::vector<std::string>& includePaths)
		: file_(fileName), includePaths_(includePaths) {}

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
			RegisterGlobalMeta(g);

		/*
		 * Compile every function unconditionally (into its own text buffer, not
		 * the final output) -- this is what catches errors (undefined names, arity
		 * mismatches, etc.) in dead code too, same as a real compiler type-checks
		 * unreferenced functions before a linker ever gets a chance to drop them.
		 * EmitCall/EmitVarRead/EmitVarWrite/EmitArrayRead/EmitArrayAssign record
		 * the call graph and global references as a side effect along the way.
		 */
		for(const FuncDecl& f : program.funcs){
			if(f.isExtern) continue; /* no body to compile -- implementation lives in hand-written .pvms */
			std::ostringstream funcBuf;
			activeOut_ = &funcBuf;
			EmitFunction(f, program);
			funcText_[f.name] = funcBuf.str();
		}
		activeOut_ = &out_;

		/* Now do the actual dead-code elimination: only "main" and whatever it
		 * transitively calls survive; only globals referenced by surviving
		 * functions keep their RAM slot (or their .const line, for @address ones). */
		std::set<std::string> reachable = ComputeReachable("main");

		std::set<std::string> usedGlobals;
		for(const std::string& fn : reachable){
			auto it = globalRefsByFunc_.find(fn);
			if(it != globalRefsByFunc_.end())
				usedGlobals.insert(it->second.begin(), it->second.end());
		}

		for(const GlobalVarDecl& g : program.globals)
			if(usedGlobals.count(g.name))
				AssignGlobalAddress(g);

		/* Only pull in assembly for extern functions that actually survived
		 * dead-code elimination, same rule as everything else. Resolved to an
		 * absolute path here (not left as a bareword) because the generated
		 * .pvms lives in a different directory (typically the build tree) than
		 * the .hlc source the "from" name should be resolved relative to --
		 * pvmc's own (relative-only) ".import" then just works unmodified,
		 * since an absolute path always wins over path-joining. */
		std::set<std::string> asmImports;
		for(const FuncDecl& f : program.funcs)
			if(f.isExtern && reachable.count(f.name))
				asmImports.insert(ResolveModulePath(file_, f.asmFile, ".pvms", includePaths_));

		out_ << ".entry main\n\n";

		for(const std::string& asmPath : asmImports)
			out_ << ".import " << asmPath << "\n";
		if(!asmImports.empty()) out_ << "\n";

		for(const GlobalVarDecl& g : program.globals)
			if(usedGlobals.count(g.name))
				EmitGlobalConst(g);
		if(!usedGlobals.empty()) out_ << "\n";

		for(const FuncDecl& f : program.funcs)
			if(reachable.count(f.name))
				out_ << funcText_[f.name];

		return out_.str();
	}

private:
	std::string 													 file_;
	std::vector<std::string> 							 includePaths_;
	std::ostringstream 										 out_;
	std::ostringstream* 									 activeOut_ = &out_;
	std::unordered_map<std::string, FuncInfo>   funcs_;
	std::unordered_map<std::string, GlobalInfo> globals_;
	std::unordered_map<std::string, std::string> funcText_;
	std::unordered_map<std::string, std::set<std::string>> callGraph_;
	std::unordered_map<std::string, std::set<std::string>> globalRefsByFunc_;
	int 																	 nextRamAddr_ = 0;
	int 																	 labelCounter_ = 0;

	/* per-function codegen state */
	std::string currentFuncName_;
	std::unordered_map<std::string, int> locals_;
	int 	nextLocalOffset_ 		= 0;
	bool 	curFuncIsVoid_ 			= false;
	bool 	curFuncIsMain_ 			= false;
	int 	curReturnSlotOffset_ = 0;

	void Line(const std::string& s){ *activeOut_ << "  " << s << "\n"; }
	void Label(const std::string& name){ *activeOut_ << name << ":\n"; }

	std::set<std::string> ComputeReachable(const std::string& root){
		std::set<std::string> reachable;
		std::vector<std::string> worklist = {root};

		while(!worklist.empty()){
			std::string cur = worklist.back();
			worklist.pop_back();
			if(!reachable.insert(cur).second) continue;

			auto it = callGraph_.find(cur);
			if(it != callGraph_.end())
				for(const std::string& callee : it->second) worklist.push_back(callee);
		}

		return reachable;
	}

	void RecordCall(const std::string& callee){ callGraph_[currentFuncName_].insert(callee); }
	void RecordGlobalUse(const std::string& name){ globalRefsByFunc_[currentFuncName_].insert(name); }

	std::string NewLabel(const std::string& prefix){
		return "__" + prefix + std::to_string(labelCounter_++);
	}

	[[noreturn]] void Fail(int line, const std::string& msg){
		throw HlcError(file_ + ":" + std::to_string(line) + ": error: " + msg);
	}

	/* ---- globals ---- */

	/* Validate + record shape (array-ness/size) so EmitArrayRead/EmitVarRead can
	 * look a name up while compiling function bodies. Deliberately does NOT
	 * assign a RAM address yet -- that only happens for globals that survive
	 * dead-code elimination (see AssignGlobalAddress), so an unused global costs
	 * nothing and doesn't shift the addresses of the ones that are kept. */
	void RegisterGlobalMeta(const GlobalVarDecl& g){
		if(globals_.count(g.name) || funcs_.count(g.name))
			Fail(g.line, "'" + g.name + "' redefined");

		int size = g.isArray ? g.arraySize : 1;
		if(size <= 0)
			Fail(g.line, "array size must be positive");

		globals_[g.name] = GlobalInfo{g.isArray, size, /*address*/ 0};
	}

	void AssignGlobalAddress(const GlobalVarDecl& g){
		GlobalInfo& info = globals_[g.name];

		if(g.hasAddress){
			info.address = g.address;
			return;
		}

		if(nextRamAddr_ + info.size > PVM_CFG_RAM_SIZE)
			Fail(g.line, "global '" + g.name + "' does not fit in RAM (PVM_CFG_RAM_SIZE=" +
										std::to_string(PVM_CFG_RAM_SIZE) + ")");
		info.address = nextRamAddr_;
		nextRamAddr_ += info.size;
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
			EmitVarWrite(g.line, g.name); /* also marks the global as used, same as any other write */
		}
	}

	/* ---- functions ---- */

	void EmitFunction(const FuncDecl& f, const Program& program){
		currentFuncName_ 		= f.name;
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
		*activeOut_ << "\n";
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
		if(auto x = dynamic_cast<SvcCallExpr*>(e)){ EmitSvcCall(x); return; }
		Fail(e->line, "internal: unknown expression node");
	}

	/* By convention every SVC handler pops its own arguments and pushes exactly
	 * one result (see docs/pvms-language.md) -- the compiler can't verify this,
	 * it's a runtime contract between the .hlc program and the registered
	 * PVM_SvcFunc, same as any real syscall ABI. */
	void EmitSvcCall(SvcCallExpr* x){
		for(const ExprPtr& arg : x->args) EmitExpr(arg.get());
		EmitExpr(x->number.get());
		Line("SVC");
	}

	void EmitVarRead(int line, const std::string& name){
		auto it = locals_.find(name);
		if(it != locals_.end()){ Line("LOADFP " + std::to_string(it->second)); return; }

		auto git = globals_.find(name);
		if(git != globals_.end()){
			if(git->second.isArray) Fail(line, "'" + name + "' is an array; use " + name + "[index]");
			RecordGlobalUse(name);
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
			RecordGlobalUse(name);
			Line("STORE " + name);
			return;
		}
		Fail(line, "undefined variable '" + name + "'");
	}

	void EmitArrayRead(ArrayIndexExpr* x){
		auto git = globals_.find(x->name);
		if(git == globals_.end()) Fail(x->line, "undefined array '" + x->name + "'");
		if(!git->second.isArray) Fail(x->line, "'" + x->name + "' is not an array");
		RecordGlobalUse(x->name);

		EmitExpr(x->index.get());
		Line("PUSH " + x->name);
		Line("ADD");
		Line("LOADI");
	}

	void EmitArrayAssign(ArrayAssignExpr* x){
		auto git = globals_.find(x->name);
		if(git == globals_.end()) Fail(x->line, "undefined array '" + x->name + "'");
		if(!git->second.isArray) Fail(x->line, "'" + x->name + "' is not an array");
		RecordGlobalUse(x->name);

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

		RecordCall(call->name);

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

std::string Generate(const Program& program, const std::string& fileName,
											const std::vector<std::string>& includePaths){
	Codegen codegen(fileName, includePaths);
	return codegen.Run(program);
}
