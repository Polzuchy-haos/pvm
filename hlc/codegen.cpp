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

		/* Constants are evaluated in declaration order (each may reference an
		 * earlier one, C-enum-style) before anything else looks a name up, since
		 * global metadata (array size) and addresses can themselves be constant
		 * expressions built out of them. */
		for(const ConstDecl& c : program.consts)
			RegisterConst(c);

		/* Before any global is placed: section sizes decide what "fits in RAM" and
		 * which fixed @addresses are legal, and may themselves be built out of the
		 * constants registered just above. */
		RegisterSections(program);

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

		stackSummary_ = AnalyzeStack(reachable);

		out_ << stackSummary_;
		out_ << ".entry main\n\n";

		/* Passed on rather than kept private, so the same limits also apply to the
		 * generated code itself (jump targets, program size) and to any hand-written
		 * .pvms pulled in below -- that check lives in the assembler. */
		if(!sectionText_.empty()) out_ << sectionText_ << "\n";

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

	/* Same text as the comment at the top of the generated .pvms, so pvmcc can
	 * also say it out loud rather than only writing it into a file. */
	const std::string& StackSummary() const { return stackSummary_; }

private:
	std::string 													 file_;
	std::vector<std::string> 							 includePaths_;
	std::ostringstream 										 out_;
	std::ostringstream* 									 activeOut_ = &out_;
	std::unordered_map<std::string, FuncInfo>   funcs_;
	std::unordered_map<std::string, GlobalInfo> globals_;
	std::unordered_map<std::string, int> 			 consts_;
	std::unordered_map<std::string, std::string> funcText_;
	std::unordered_map<std::string, std::set<std::string>> callGraph_;
	std::unordered_map<std::string, std::set<std::string>> globalRefsByFunc_;
	int 																	 nextRamAddr_ = 0;
	int 																	 labelCounter_ = 0;

	/* Section sizes default to the pvm.h this compiler was built against; a
	 * "section" declaration overrides one for a target configured differently.
	 * usrsect's length is the exception with no pvm.h constant behind it (the VM
	 * is told at runtime, by PVM_UsrSect), so it stays unbounded until declared --
	 * the same rule the assembler applies to its own .usrsect directive.
	 * "code" is the exception with nothing to keep: it constrains the generated
	 * code rather than any decision made here, so it is only passed through to the
	 * assembler. */
	int 																	 ramSize_ 		 = PVM_CFG_RAM_SIZE;
	int 																	 stackSize_ 	 = PVM_CFG_STACK_SIZE;
	int 																	 usrsectBase_ = PVM_CFG_USRSECT_BGN;
	int 																	 usrsectLen_ 	 = -1;
	std::string 													 sectionText_; /* the .pvms directives to hand on */
	std::string 													 stackSummary_;

	/* per-function codegen state */
	std::string currentFuncName_;
	std::unordered_map<std::string, int> locals_;
	int 	nextLocalOffset_ 		= 0;
	bool 	curFuncIsVoid_ 			= false;
	bool 	curFuncIsMain_ 			= false;
	int 	curReturnSlotOffset_ = 0;

	/* ---- stack depth tracking (see AnalyzeStack) ---- */
	int curDepth_ = 0;
	int maxDepth_ = 0;
	std::unordered_map<std::string, int> labelDepth_; /* per function, cleared by EmitFunction */
	std::unordered_map<std::string, int> frameMax_; 	/* function -> its own peak depth */
	/* function -> every (callee, depth at that CALL) pair inside it */
	std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> callSites_;

	void Line(const std::string& s){
		*activeOut_ << "  " << s << "\n";
		TrackStack(s);
	}

	void Label(const std::string& name){
		*activeOut_ << name << ":\n";

		/* Code is emitted in source order, but a label is reached by a jump: both
		 * arms of a ternary leave one value, yet the second is emitted right after
		 * the first, so counting straight through would double-count it. Picking
		 * the depth recorded by the jumps here is what keeps the two apart. */
		auto it = labelDepth_.find(name);
		if(it != labelDepth_.end()) SetDepth(it->second);
	}

	void SetDepth(int depth){
		curDepth_ = depth;
		if(curDepth_ > maxDepth_) maxDepth_ = curDepth_;
	}

	void TrackStack(const std::string& text){
		size_t 		 space 		 = text.find(' ');
		std::string mnemonic = (space == std::string::npos) ? text : text.substr(0, space);

		SetDepth(curDepth_ + StackDelta(mnemonic));

		if(mnemonic == "JMP" || mnemonic == "JZ" || mnemonic == "JNZ"){
			std::string target = text.substr(space + 1);
			auto 				it 		 = labelDepth_.find(target);

			if(it == labelDepth_.end()) 		labelDepth_[target] = curDepth_;
			else if(curDepth_ > it->second) it->second 					= curDepth_;
		}
	}

	/*
	 * How one emitted instruction changes the stack depth.
	 *
	 * CALL is zero because the callee cleans up after itself -- what it needs
	 * while it runs is accounted for separately, per call site (see StackNeed).
	 * SVC is zero here and corrected in EmitSvcCall, the only place that knows how
	 * many arguments the handler pops. LEAVE/RET are zero rather than "drop the
	 * frame" because an early return emits them mid-function, and the code after
	 * one runs at the depth it had before, not at zero.
	 */
	static int StackDelta(const std::string& mnemonic){
		if(mnemonic == "PUSH" 	|| mnemonic == "DUP" || mnemonic == "LOAD" ||
			 mnemonic == "LOADFP" || mnemonic == "ENTER") return +1;

		if(mnemonic == "POP" || mnemonic == "STORE" || mnemonic == "STOREFP" ||
			 mnemonic == "JZ" 	|| mnemonic == "JNZ" 	 ||
			 mnemonic == "ADD" 	|| mnemonic == "SUB" 	 || mnemonic == "MUL" || mnemonic == "DIV" ||
			 mnemonic == "EQ" 	|| mnemonic == "LT" 	 || mnemonic == "AND" || mnemonic == "OR" 	||
			 mnemonic == "XOR" 	|| mnemonic == "SHL" 	 || mnemonic == "SHR") return -1;

		if(mnemonic == "STOREI") return -2;

		return 0; /* SWAP, NOT, LOADI, JMP, HALT, CALL, SVC, LEAVE, RET */
	}

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

	/* ---- constants ---- */

	void RegisterConst(const ConstDecl& c){
		if(funcs_.count(c.name) || consts_.count(c.name))
			Fail(c.line, "'" + c.name + "' redefined");
		consts_[c.name] = EvalConstExpr(c.value.get());
	}

	/* Folds a compile-time constant expression: integer literals, previously
	 * declared 'const' names, and the usual arithmetic/bitwise/comparison
	 * operators over them. Nothing that reads memory or has side effects
	 * (variables, arrays, calls, svc) is allowed here -- there is no "runtime"
	 * for a constant, only a value the compiler already knows. */
	int EvalConstExpr(Expr* e){
		if(auto x = dynamic_cast<IntLiteralExpr*>(e)) return x->value;

		if(auto x = dynamic_cast<VarRefExpr*>(e)){
			auto it = consts_.find(x->name);
			if(it == consts_.end())
				Fail(x->line, "'" + x->name + "' cannot be used in a constant expression "
											"(only literals and earlier 'const' names can)");
			return it->second;
		}

		if(auto x = dynamic_cast<UnaryExpr*>(e)){
			int v = EvalConstExpr(x->operand.get());
			if(x->op == "-") return -v;
			if(x->op == "~") return ~v;
			if(x->op == "!") return !v;
			Fail(x->line, "internal: unknown unary operator in constant expression");
		}

		if(auto x = dynamic_cast<BinaryExpr*>(e)){
			int l = EvalConstExpr(x->lhs.get());
			int r = EvalConstExpr(x->rhs.get());
			if(x->op == "+") 	return l + r;
			if(x->op == "-") 	return l - r;
			if(x->op == "*") 	return l * r;
			if(x->op == "/"){
				if(r == 0) Fail(x->line, "division by zero in constant expression");
				return l / r;
			}
			if(x->op == "&") 	return l & r;
			if(x->op == "|") 	return l | r;
			if(x->op == "^") 	return l ^ r;
			if(x->op == "<<") return l << r;
			if(x->op == ">>") return l >> r;
			if(x->op == "==") return l == r;
			if(x->op == "!=") return l != r;
			if(x->op == "<") 	return l < r;
			if(x->op == ">") 	return l > r;
			if(x->op == "<=") return l <= r;
			if(x->op == ">=") return l >= r;
			if(x->op == "&&") return (l != 0) && (r != 0);
			if(x->op == "||") return (l != 0) || (r != 0);
			Fail(x->line, "internal: unknown binary operator in constant expression");
		}

		if(auto x = dynamic_cast<TernaryExpr*>(e))
			return EvalConstExpr(x->cond.get()) != 0 ? EvalConstExpr(x->thenExpr.get())
																								: EvalConstExpr(x->elseExpr.get());

		Fail(e->line, "expected a compile-time constant expression");
	}

	/* ---- sections ---- */

	void RegisterSections(const Program& program){
		std::set<std::string> seen;
		std::ostringstream 		directives;

		for(const SectionDecl& s : program.sections){
			if(!seen.insert(s.name).second)
				Fail(s.line, "section '" + s.name + "' declared more than once");

			int value = EvalConstExpr(s.value.get());
			if(value < 0)
				Fail(s.line, "section '" + s.name + "' cannot be negative");

			if(s.name == "usrsect"){
				if(!s.length)
					Fail(s.line, "'section usrsect' needs a base address and a length "
												"('section usrsect 0x4000, 16;')");

				int length = EvalConstExpr(s.length.get());
				if(length < 0)
					Fail(s.line, "section 'usrsect' cannot have a negative length");

				usrsectBase_ = value;
				usrsectLen_ 	= length;
				directives << ".usrsect " << value << " " << length << "\n";
				continue;
			}

			if(s.length)
				Fail(s.line, "section '" + s.name + "' takes a single size");

			if(s.name == "code") 	directives << ".code " << value << "\n";
			else if(s.name == "stack"){
				if(value < 1)
					Fail(s.line, "section 'stack' must be at least 1 word");
				stackSize_ = value;
				directives << ".stack " << value << "\n";
			}
			else if(s.name == "ram"){
				ramSize_ = value;
				directives << ".ram " << value << "\n";
			}
			else
				Fail(s.line, "unknown section '" + s.name +
											"' (expected 'code', 'ram', 'stack' or 'usrsect')");
		}

		sectionText_ = directives.str();
	}

	bool IsValidDataAddr(int addr) const {
		if(addr >= 0 && addr < ramSize_) return true;
		if(addr >= usrsectBase_ &&
			 (usrsectLen_ < 0 || addr < usrsectBase_ + usrsectLen_)) return true;
		return false;
	}

	std::string DataRangeText() const {
		std::ostringstream s;

		if(ramSize_ > 0) 	s << "RAM is 0.." << (ramSize_ - 1);
		else 							s << "RAM is empty";

		s << std::uppercase << std::hex;
		if(usrsectLen_ < 0)
			s << ", usrsect is 0x" << usrsectBase_ << " and up";
		else if(usrsectLen_ == 0)
			s << ", usrsect is empty";
		else
			s << ", usrsect is 0x" << usrsectBase_ << "..0x" << (usrsectBase_ + usrsectLen_ - 1);

		return s.str();
	}

	/* ---- stack analysis ---- */

	static const int kStackUnbounded = -1;

	/*
	 * Worst-case stack use of `name` together with everything it can reach, in
	 * words above the return address its caller pushed.
	 *
	 * Each call site contributes its own depth (return slot and arguments already
	 * pushed), plus the one word CALL pushes for the return address, plus whatever
	 * the callee needs on top of that. Recursion makes the sum infinite, so a
	 * cycle is reported rather than unrolled.
	 */
	int StackNeed(const std::string& name, std::set<std::string>& onPath,
								std::unordered_map<std::string, int>& memo){
		auto cached = memo.find(name);
		if(cached != memo.end()) return cached->second;
		if(onPath.count(name)) return kStackUnbounded;

		onPath.insert(name);

		auto frame = frameMax_.find(name);
		int 	need 	= (frame != frameMax_.end()) ? frame->second : 0; /* extern: no body to measure */

		auto sites = callSites_.find(name);
		if(sites != callSites_.end()){
			for(const auto& site : sites->second){
				int callee = StackNeed(site.first, onPath, memo);
				if(callee == kStackUnbounded){ need = kStackUnbounded; break; }

				int total = site.second + 1 + callee;
				if(total > need) need = total;
			}
		}

		onPath.erase(name);
		memo[name] = need;
		return need;
	}

	/* main is entered directly rather than called, so nothing was pushed below its
	 * frame and its own requirement is the whole program's. */
	std::string AnalyzeStack(const std::set<std::string>& reachable){
		std::set<std::string> 							 onPath;
		std::unordered_map<std::string, int> memo;
		int 																 need = StackNeed("main", onPath, memo);

		if(need == kStackUnbounded)
			return "# stack: recursion found -- worst-case depth cannot be bounded\n";

		bool haveOpaque = false;
		for(const std::string& fn : reachable)
			if(!frameMax_.count(fn)) haveOpaque = true;

		if(need > stackSize_)
			throw HlcError(file_ + ": error: worst-case stack use is " + std::to_string(need) +
											" words, which does not fit in the declared stack of " +
											std::to_string(stackSize_) + " words");

		return "# stack: worst case " + std::to_string(need) + " of " +
						std::to_string(stackSize_) + " words" +
						(haveOpaque ? ", not counting hand-written asm functions" : "") + "\n";
	}

	/* ---- globals ---- */

	/* Validate + record shape (array-ness/size) so EmitArrayRead/EmitVarRead can
	 * look a name up while compiling function bodies. Deliberately does NOT
	 * assign a RAM address yet -- that only happens for globals that survive
	 * dead-code elimination (see AssignGlobalAddress), so an unused global costs
	 * nothing and doesn't shift the addresses of the ones that are kept. */
	void RegisterGlobalMeta(const GlobalVarDecl& g){
		if(globals_.count(g.name) || funcs_.count(g.name) || consts_.count(g.name))
			Fail(g.line, "'" + g.name + "' redefined");

		int size = g.isArray ? EvalConstExpr(g.arraySizeExpr.get()) : 1;
		if(size <= 0)
			Fail(g.line, "array size must be positive");
		if(!g.arrayInit.empty() && (int)g.arrayInit.size() != size)
			Fail(g.line, "array '" + g.name + "' has " + std::to_string(size) + " element(s) but " +
										std::to_string(g.arrayInit.size()) + " initializer(s) were given");

		globals_[g.name] = GlobalInfo{g.isArray, size, /*address*/ 0};
	}

	void AssignGlobalAddress(const GlobalVarDecl& g){
		GlobalInfo& info = globals_[g.name];

		if(g.hasAddress){
			info.address = EvalConstExpr(g.addressExpr.get());

			/* A fixed address is the one place the compiler cannot move a global out of
			 * harm's way, so it is also the only place worth rejecting outright. */
			if(!IsValidDataAddr(info.address) || !IsValidDataAddr(info.address + info.size - 1))
				Fail(g.line, "'" + g.name + "' is pinned to an address outside addressable memory (" +
											DataRangeText() + ")");
			return;
		}

		if(nextRamAddr_ + info.size > ramSize_)
			Fail(g.line, "global '" + g.name + "' does not fit in RAM (" +
										std::to_string(ramSize_) + " words)");
		info.address = nextRamAddr_;
		nextRamAddr_ += info.size;
	}

	/* ".var" rather than ".const": it resolves identically in operands, but also
	 * tells the assembler the variable's extent, which lands in the listing's
	 * memory map and gets the whole span bounds-checked. */
	void EmitGlobalConst(const GlobalVarDecl& g){
		const GlobalInfo& info = globals_[g.name];
		out_ << ".var " << g.name << " " << info.address << " " << info.size << "\n";
	}

	void EmitGlobalInitializers(const Program& program){
		for(const GlobalVarDecl& g : program.globals){
			if(g.init){
				Line("PUSH " + std::to_string(EvalConstExpr(g.init.get())));
				EmitVarWrite(g.line, g.name); /* also marks the global as used, same as any other write */
				continue;
			}
			if(!g.arrayInit.empty()){
				/* One STOREI per element, address computed fresh each time (no DUP
				 * of a "result value" here, unlike EmitArrayAssign -- this isn't an
				 * expression, there's nothing for a surrounding expression to use). */
				for(size_t i = 0; i < g.arrayInit.size(); i++){
					Line("PUSH " + std::to_string(EvalConstExpr(g.arrayInit[i].get())));
					Line("PUSH " + std::to_string((int)i));
					Line("PUSH " + g.name);
					Line("ADD");
					Line("STOREI");
				}
				RecordGlobalUse(g.name); /* also marks the array as used, same as any other write */
			}
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

		/* Depth is counted from the function's own base -- the word just above the
		 * return address its caller pushed -- so every function starts at zero and
		 * StackNeed can add the frames up along a call chain. */
		curDepth_ = 0;
		maxDepth_ = 0;
		labelDepth_.clear();

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

		frameMax_[f.name] = maxDepth_;
	}

	/* ---- statements ---- */

	void EmitStmt(Stmt* s){
		if(auto x = dynamic_cast<BlockStmt*>(s)){ for(const StmtPtr& st : x->stmts) EmitStmt(st.get()); return; }
		if(auto x = dynamic_cast<IfStmt*>(s)){ EmitIf(x); return; }
		if(auto x = dynamic_cast<WhileStmt*>(s)){ EmitWhile(x); return; }
		if(auto x = dynamic_cast<DoWhileStmt*>(s)){ EmitDoWhile(x); return; }
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

	/* Unlike EmitWhile, the condition check comes after the body -- so the
	 * body always runs at least once. JNZ back to the top is enough, no JZ
	 * "skip past the loop" needed since there's no way to skip the first
	 * iteration. */
	void EmitDoWhile(DoWhileStmt* s){
		std::string labelStart = NewLabel("do_start");

		Label(labelStart);
		EmitStmt(s->body.get());
		EmitExpr(s->cond.get());
		Line("JNZ " + labelStart);
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
		if(auto x = dynamic_cast<SizeofExpr*>(e)){ EmitSizeof(x); return; }
		if(auto x = dynamic_cast<TernaryExpr*>(e)){ EmitTernary(x); return; }
		Fail(e->line, "internal: unknown expression node");
	}

	/* By convention every SVC handler pops its own arguments and pushes exactly
	 * one result (see docs/pvms-language.md) -- the compiler can't verify this,
	 * it's a runtime contract between the .hlc program and the registered
	 * PVM_SvcFunc, same as any real syscall ABI. */
	void EmitSvcCall(SvcCallExpr* x){
		int base = curDepth_;

		for(const ExprPtr& arg : x->args) EmitExpr(arg.get());
		EmitExpr(x->number.get());
		Line("SVC");

		/* The VM pops the call number and the handler pops its own arguments, then
		 * pushes exactly one result -- a shape only this call site knows, so the
		 * depth is set here rather than guessed from the mnemonic. */
		SetDepth(base + 1);
	}

	/* Deliberately does NOT call RecordGlobalUse: the array's size is known at
	 * compile time regardless of whether its storage ever gets touched, so
	 * "only sizeof(arr) used, arr itself never read/written" still lets dead-
	 * code elimination drop the array's RAM slot entirely (see docs). Not
	 * usable in a 'const' initializer or an @address/[size] expression -- see
	 * ast.h's SizeofExpr comment for why. */
	void EmitSizeof(SizeofExpr* x){
		auto git = globals_.find(x->name);
		if(git == globals_.end() || !git->second.isArray)
			Fail(x->line, "'sizeof' expects the name of a global array");
		Line("PUSH " + std::to_string(git->second.size));
	}

	void EmitVarRead(int line, const std::string& name){
		auto it = locals_.find(name);
		if(it != locals_.end()){ Line("LOADFP " + std::to_string(it->second)); return; }

		auto cit = consts_.find(name);
		if(cit != consts_.end()){ Line("PUSH " + std::to_string(cit->second)); return; }

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

		if(consts_.count(name)) Fail(line, "cannot assign to constant '" + name + "'");

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
		if(consts_.count(x->name)) Fail(x->line, "'" + x->name + "' is a constant, not an array");
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
		if(consts_.count(x->name)) Fail(x->line, "'" + x->name + "' is a constant, not an array");
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

	/* Same shape as EmitIf, but both arms leave a value (the ternary's own
	 * invariant is "exactly one value on the stack", same as every Expr). */
	void EmitTernary(TernaryExpr* x){
		std::string labelElse = NewLabel("ternary_else");
		std::string labelEnd 	= NewLabel("ternary_end");

		EmitExpr(x->cond.get());
		Line("JZ " + labelElse);
		EmitExpr(x->thenExpr.get());
		Line("JMP " + labelEnd);
		Label(labelElse);
		EmitExpr(x->elseExpr.get());
		Label(labelEnd);
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

		/* Recorded before the CALL, with the return slot and arguments already on
		 * the stack: that is exactly what the callee's own frame gets stacked on. */
		callSites_[currentFuncName_].emplace_back(call->name, curDepth_);
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
											const std::vector<std::string>& includePaths,
											std::string* stackSummary){
	Codegen 		codegen(fileName, includePaths);
	std::string pvms = codegen.Run(program);

	if(stackSummary) *stackSummary = codegen.StackSummary();
	return pvms;
}
