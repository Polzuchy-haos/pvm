/*
 * pvmdbg_cli.c
 *
 * Interactive front end for the debug protocol in pvm/pvmdbg.c: it opens a
 * session, holds it, and takes commands from the terminal until "exit" or
 * Ctrl+C -- the shape a debugger wants, rather than one process per command.
 *
 * What it is driving is decided once, at startup, and never again: a VM in this
 * process, or a board over a serial port (see transport.h). Every command below
 * is written against the protocol alone, so the two behave the same.
 */
#include "transport.h"

#include "asm.h"
#include "disasm.h"
#include "hostexec.h"
#include "machine.h"
#include "pvmdbg.h"
#include "sourcemap.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PVM_DEFAULT_BYTECODE
#define PVM_DEFAULT_BYTECODE "bytecode_listing.bin"
#endif

static Transport_s* g_link 	= 0;
static char 				g_frame[PVMDBG_CFG_BUF_SIZE];

/* --host: the program is here, and the device only executes what it is sent. */
static HostExec_s 	g_host;
static int 					g_hostMode = 0;

static volatile sig_atomic_t g_interrupted = 0;
static volatile sig_atomic_t g_inRun 			 = 0;

int TransportInterrupted(void){
	return g_interrupted;
}

/* Reads the flag and clears it, so that an interrupt which stopped a run does
 * not go on to be read as "and now quit". */
static int TakeInterrupt(void){
	int raised 		= g_interrupted;
	g_interrupted = 0;
	return raised;
}

/*
 * Ctrl+C means two different things depending on what is happening, exactly as
 * it does in gdb: stop the program while one is running, end the session while
 * one is not. Interrupting a program that will not stop by itself is the common
 * case; quitting the debugger is not.
 *
 * The session has to end from inside the handler rather than by setting a flag:
 * at the prompt the main loop is sitting in fgets, which a console Ctrl+C does
 * not interrupt on Windows, so a flag would not be looked at until the user
 * pressed Enter for no reason.
 */
static void OnInterrupt(int sig){
	(void)sig;
	signal(SIGINT, OnInterrupt); /* the CRT drops back to the default handler otherwise */

	if(g_inRun){
		g_interrupted = 1;
		return;
	}

	printf("\npvmdbg: session closed\n");
	exit(EXIT_SUCCESS);
}

/* Lets the program run, and comes back once it has stopped -- by reaching a
 * breakpoint, by ending, or because the user gave up waiting. */
static void RunUntilStopped(void){
	g_inRun = 1;
	g_link->settle(g_link, 0); /* no budget: at a terminal, Ctrl+C is the way out */
	g_inRun = 0;

	if(TakeInterrupt()){
		g_link->exchange(g_link, DBGCMD_PAUSE, 0, 0, g_frame);
		printf("  interrupted\n");
	}
}

/* ---- talking to the agent --------------------------------------------- */

static int Send(uint16_t cmd, const uint8_t* data, int dataLen){
	return g_link->exchange(g_link, cmd, data, dataLen, g_frame);
}

/* Returns the reply's payload length, or -1 when nothing usable came back. */
static int Reply(int len, uint16_t* cmd, const uint8_t** data){
	int dataLen;
	return (len > 0 && PVMDBG_FrameParse(g_frame, len, cmd, data, &dataLen)) ? dataLen : -1;
}

static uint16_t Word(const uint8_t* p, int index){
	return (uint16_t)((uint16_t)p[2 * index] | ((uint16_t)p[2 * index + 1] << 8));
}

static void NoAnswer(void){
	printf("  no answer from the debug agent\n");
}

/* ---- output ----------------------------------------------------------- */

static const char* StopReason(uint8_t reason){
	switch(reason){
	case DBGSTOP_HOST: 			 return "paused";
	case DBGSTOP_STEP: 			 return "stepped";
	case DBGSTOP_BREAKPOINT: return "breakpoint";
	case DBGSTOP_HALTED: 		 return "halted -- program finished";
	case DBGSTOP_EXEC_DONE:  return "snippet finished, back in the program";
	default: 								 return "running";
	}
}

/*
 * Bytecode is fetched from the device rather than from a local copy of the
 * program: over a wire the host has no copy at all, and where it does have one
 * it could be a rebuild ahead of what is actually running -- a disassembly that
 * quietly disagrees with the machine is worse than none at all.
 */
static void ShowCode(uint16_t addr, int maxLines){
	const uint8_t* d;
	uint16_t 			 cmd;
	uint8_t 			 arg[3];
	char 					 lines[PVMDBG_CFG_CODE_READ_MAX][PVMDBG_DISASM_LINE_MAX];
	int 					 count;

	arg[0] = (uint8_t)(addr & 0xFF);
	arg[1] = (uint8_t)(addr >> 8);
	arg[2] = PVMDBG_CFG_CODE_READ_MAX;

	if(Reply(Send(DBGCMD_READ_CODE, arg, 3), &cmd, &d) < 1 || cmd != DBGCMD_READ_CODE)
		return;

	count = PvmDbgDisasm(d + 1, d[0], addr, maxLines, lines);

	for(int i = 0; i < count; i++)
		printf("  %s %s\n", (i == 0) ? "->" : "  ", lines[i]);
}

/* The .hlc line the program counter came from, when a debug map says so. Silent
 * without one, and silent about a source file that is not on this machine --
 * neither is a fault, just less to show. */
static void ShowSource(uint16_t pc){
	int 				line;
	const char* file = SourceMapFileFor(pc, &line);
	const char* text;

	if(!file) return;

	text = SourceMapText(file, line);
	printf("     %s:%d%s%s\n", SourceMapBaseName(file), line,
					text ? "  " : "", text ? text : "");
}

/* What a finished snippet left on the stack -- its answer, if it was asked a
 * question. The agent kept it, because the stack itself is the program's again. */
static void ShowExecResult(void){
	const uint8_t* d;
	uint16_t 			 cmd;

	if(Reply(Send(DBGCMD_EXEC_STATUS, 0, 0), &cmd, &d) < 2 || cmd != DBGCMD_EXEC_STATUS) return;
	if(d[0] == DBGEXEC_ACTIVE){ printf("  snippet stopped before its end -- 'abort' drops it\n"); return; }
	if(d[0] != DBGEXEC_DONE) return;

	if(d[1] == 0){ printf("  snippet left nothing on the stack\n"); return; }

	printf("  snippet left on the stack, top first:");
	for(int i = 0; i < d[1]; i++) printf(" %d", (int)(int16_t)Word(d + 2, i));
	printf("\n");
}

static void ShowState(void){
	const uint8_t* d;
	uint16_t 			 cmd;
	uint16_t 			 pc;
	uint8_t 			 reason;
	int 					 len, inSnippet;

	len = Reply(Send(DBGCMD_STATE, 0, 0), &cmd, &d);
	if(len < 8){ NoAnswer(); return; }

	/* Taken out now: every Send below reuses the frame d points into. */
	pc 				= Word(d, 0);
	reason 		= d[7];
	inSnippet = (len >= 9 && d[8]); /* an agent without EXEC sends eight bytes */

	printf("  pc=0x%04X  sp=%u  fp=%u  %s%s\n", pc, Word(d, 1), Word(d, 2), StopReason(reason),
					inSnippet ? "  [in snippet]" : "");

	/* The instruction about to run, right under the numbers: an address on its
	 * own means looking it up in a listing every single step. */
	ShowCode(pc, 1);

	/* A snippet's addresses are offsets into the device's buffer; the debug map
	 * would match them to whatever program line happens to share the number. */
	if(!inSnippet) ShowSource(pc);

	if(reason == DBGSTOP_EXEC_DONE) ShowExecResult();
}

static void ShowStack(int count){
	const uint8_t* d;
	uint16_t 			 cmd;
	uint8_t 			 arg = (uint8_t)count;

	if(Reply(Send(DBGCMD_READ_STACK, &arg, 1), &cmd, &d) < 1 || cmd != DBGCMD_READ_STACK){
		NoAnswer();
		return;
	}

	if(d[0] == 0){ printf("  stack is empty\n"); return; }

	for(int i = 0; i < d[0]; i++)
		printf("  [%d] %-6d%s\n", i, (int)(int16_t)Word(d + 1, i), (i == 0) ? "  <- top" : "");
}

/* Sizes come from the device, not from PVM_CFG_*: the firmware picks its own
 * pools, and the defaults this host was built with say nothing about them.
 * Returns -1 for no answer, 0 for an agent built before MEM_INFO existed (it
 * answers ERROR), 1 with the sizes filled in. */
static int ReadMemInfo(uint16_t* stackLen, uint16_t* ramLen){
	const uint8_t* d;
	uint16_t 			 cmd;
	int 					 len = Reply(Send(DBGCMD_MEM_INFO, 0, 0), &cmd, &d);

	if(len < 0) 													 return -1;
	if(cmd != DBGCMD_MEM_INFO || len < 4) return 0;

	*stackLen = Word(d, 0);
	*ramLen 	= Word(d, 1);
	return 1;
}

static void ShowMemory(void){
	const uint8_t* d;
	uint16_t 			 cmd;
	uint16_t 			 stackLen, ramLen;
	int 					 known = ReadMemInfo(&stackLen, &ramLen);

	if(known < 0){ NoAnswer(); return; }
	if(known == 0){ printf("  the device does not report its memory sizes\n"); return; }

	/* How much of the stack is taken is what makes its size worth knowing. */
	if(Reply(Send(DBGCMD_STATE, 0, 0), &cmd, &d) < 8)
		printf("  stack  %u words\n", stackLen);
	else
		printf("  stack  %u words, %u in use\n", stackLen, Word(d, 1));

	if(ramLen == 0) printf("  ram    none\n");
	else 						printf("  ram    %u words (addresses 0..%u)\n", ramLen, (unsigned)(ramLen - 1));
}

static void ShowBreakpoints(void){
	const uint8_t* d;
	uint16_t 			 cmd;

	if(Reply(Send(DBGCMD_BRKP_LIST, 0, 0), &cmd, &d) < 1){ NoAnswer(); return; }

	if(d[0] == 0){ printf("  no breakpoints set\n"); return; }

	printf("  %u breakpoint(s):", d[0]);
	for(int i = 0; i < d[0]; i++) printf(" 0x%04X", Word(d + 1, i));
	printf("\n");
}

static void ShowHelp(void){
	printf(
		"  run, r          continue until a breakpoint or the end of the program\n"
		"  step, s         execute a single instruction\n"
		"  pause, p        stop a running program\n"
		"  reset           rewind to the first instruction and stop there\n"
		"  state, i        where the program counter, stack and frame pointer are\n"
		"  stack [n]       the top n words of the stack (8 by default)\n"
		"  mem, m          stack and RAM sizes, and how much of the stack is in use\n"
		"  break <where>   set a breakpoint: 'break 0x10', or 'break blink.hlc:21'\n"
		"  clear <addr>    remove one\n"
		"  breaks, b       list the ones that are set\n"
		"  exec <code>     run code on the device without reflashing, e.g.\n"
		"                  'exec LOAD 3; PUSH 1; ADD; STORE 3', or 'exec snippet.pvms'\n"
		"                  or 'exec snippet.bin' -- at most 64 bytes of bytecode\n"
		"  abort           drop a snippet that has not finished, back to the program\n"
		"  call <fn> [w..] with --host: push the words, run one function, stop when it\n"
		"                  returns: 'call square 0 7' (a name needs the .lst beside the .bin)\n"
		"  help, ?         this list\n"
		"  exit, quit, q   end the session (so does Ctrl+C)\n");
}

/* ---- commands --------------------------------------------------------- */

static int ParseAddr(const char* text, uint16_t* out){
	char* end;
	long 	value = strtol(text, &end, 0); /* base 0: both 16 and 0x10 work */

	if(end == text || *end != '\0' || value < 0 || value > 0xFFFF) return 0;

	*out = (uint16_t)value;
	return 1;
}

/* Either a bytecode address, or "file.hlc:12" when a debug map can turn that
 * into one. The colon tells them apart -- an address never has one. */
static int ResolveTarget(const char* text, uint16_t* addr){
	const char* colon = strrchr(text, ':');
	char 				file[260];
	size_t 			nameLen;
	int 				line;

	if(!colon){
		if(ParseAddr(text, addr)) return 1;
		printf("  expected an address or a source line, e.g. 'break 0x10' or 'break blink.hlc:21'\n");
		return 0;
	}

	nameLen = (size_t)(colon - text);
	line 		= atoi(colon + 1);

	if(nameLen == 0 || nameLen >= sizeof(file) || line <= 0){
		printf("  expected 'file.hlc:12'\n");
		return 0;
	}

	memcpy(file, text, nameLen);
	file[nameLen] = '\0';

	if(!SourceMapLoaded()){
		printf("  no debug map loaded -- pass --map, or give an address instead\n");
		return 0;
	}
	if(!SourceMapAddrFor(file, line, addr)){
		printf("  no code was generated for %s\n", text);
		return 0;
	}

	return 1;
}

static void BreakpointCmd(uint16_t cmd, const char* arg){
	const uint8_t* d;
	uint16_t 			 addr, replyCmd;
	uint8_t 			 payload[2];

	if(!arg){
		printf("  expected an address or a source line, e.g. 'break 0x10' or 'break blink.hlc:21'\n");
		return;
	}
	if(!ResolveTarget(arg, &addr)) return;

	payload[0] = (uint8_t)(addr & 0xFF);
	payload[1] = (uint8_t)(addr >> 8);

	if(Reply(Send(cmd, payload, 2), &replyCmd, &d) < 0){ NoAnswer(); return; }

	if(replyCmd == DBGCMD_ERROR)
		printf("  refused: 0x%04X is outside the program, or there is no slot left\n", addr);
	else
		printf("  0x%04X %s\n", addr,
						(cmd == DBGCMD_BRKP_SET) ? "will stop the program" : "no longer will");
}

static int EndsWith(const char* text, const char* suffix){
	size_t n = strlen(text), m = strlen(suffix);
	return n >= m && !strcmp(text + n - m, suffix);
}

/* Bytecode as pvmc -f bin wrote it. One byte more than fits is read on purpose,
 * so that a file that is too big is told apart from one that fits exactly. */
static int LoadSnippetBin(const char* path, uint8_t* out, int outMax){
	FILE* f = fopen(path, "rb");
	int 	n;

	if(!f){ printf("  cannot open '%s'\n", path); return -1; }

	n = (int)fread(out, 1, (size_t)outMax, f);
	if(n == outMax && fgetc(f) != EOF){
		printf("  '%s' is bigger than the device's %d-byte buffer\n", path, outMax);
		n = -1;
	}

	fclose(f);
	return n;
}

/*
 * Uploads a snippet and runs it in the middle of the program, which stays where
 * it was stopped: the snippet has the program's stack, RAM, peripherals and
 * SVCs, and once it reaches its end the program is back exactly where it was --
 * apart from what the snippet changed on purpose. ShowState then reports what
 * it left on the stack.
 *
 * Inline code is assembled against the RAM and stack the device reports, so a
 * board with more RAM than the default is not refused addresses it really has.
 */
static void ExecCmd(const char* rest){
	uint8_t  code[PVMDBG_CFG_EXEC_SIZE];
	uint8_t  payload[1 + PVMDBG_EXEC_CHUNK_MAX];
	char 		 err[512];
	uint16_t stackLen, ramLen, cmd;
	int 		 len;
	const uint8_t* d;

	if(!*rest){
		printf("  expected code, e.g. 'exec LOAD 3; PUSH 1; ADD; STORE 3', or a .pvms/.bin file\n");
		return;
	}

	if(EndsWith(rest, ".bin"))
		len = LoadSnippetBin(rest, code, (int)sizeof(code));
	else{
		if(EndsWith(rest, ".pvms"))
			len = PvmDbgAssembleFile(rest, code, (int)sizeof(code), err, (int)sizeof(err));
		else if(ReadMemInfo(&stackLen, &ramLen) > 0)
			len = PvmDbgAssembleText(rest, ramLen, stackLen, code, (int)sizeof(code), err, (int)sizeof(err));
		else
			len = PvmDbgAssembleText(rest, -1, -1, code, (int)sizeof(code), err, (int)sizeof(err));

		if(len < 0) printf("  %s\n", err);
	}

	if(len < 0) return;
	if(len == 0){ printf("  no code to run\n"); return; }

	for(int offset = 0; offset < len; offset += PVMDBG_EXEC_CHUNK_MAX){
		int n = (len - offset < PVMDBG_EXEC_CHUNK_MAX) ? len - offset : PVMDBG_EXEC_CHUNK_MAX;

		payload[0] = (uint8_t)offset;
		memcpy(payload + 1, code + offset, (size_t)n);

		if(Reply(Send(DBGCMD_EXEC_WRITE, payload, 1 + n), &cmd, &d) < 0){ NoAnswer(); return; }
		if(cmd != DBGCMD_EXEC_WRITE){
			printf("  refused: a snippet is still running ('abort' it), or the device cannot run snippets\n");
			return;
		}
	}

	payload[0] = (uint8_t)len;
	if(Reply(Send(DBGCMD_EXEC_RUN, payload, 1), &cmd, &d) < 0){ NoAnswer(); return; }
	if(cmd != DBGCMD_EXEC_RUN){
		printf("  refused: the program has to be stopped first ('pause')\n");
		return;
	}

	RunUntilStopped();

	/* With --host the device's PC is not the program's, so only the answer is
	 * worth showing. */
	if(g_hostMode) ShowExecResult();
	else 					 ShowState();
}

/* ---- the program on the host (--host) --------------------------------- */

static const char* HostStopText(int stop){
	switch(stop){
	case HOSTSTOP_STEP: 				return "stepped";
	case HOSTSTOP_BREAKPOINT: 	return "breakpoint";
	case HOSTSTOP_HALTED: 			return "halted -- program finished";
	case HOSTSTOP_RETURNED: 		return "function returned";
	case HOSTSTOP_ERROR: 				return "stopped on an error";
	case HOSTSTOP_INTERRUPTED: 	return "interrupted";
	case HOSTSTOP_NO_ANSWER: 		return "no answer from the device";
	default: 										return "stopped";
	}
}

/* Same shape as ShowState, but the PC and the code are the host's: the device
 * only contributes the stack, which is where the data lives. */
static void HostShowState(int stop){
	const uint8_t* d;
	uint16_t 			 cmd;
	uint16_t 			 pc 	= g_host.pc;
	const char* 	 func = HostExecFuncAt(&g_host, pc);
	char 					 lines[1][PVMDBG_DISASM_LINE_MAX];

	if(stop == HOSTSTOP_ERROR || stop == HOSTSTOP_NO_ANSWER) printf("  %s\n", g_host.message);

	if(Reply(Send(DBGCMD_STATE, 0, 0), &cmd, &d) < 8){ NoAnswer(); return; }

	printf("  pc=0x%04X  sp=%u  fp=%u  %s  [host%s%s]\n", pc, Word(d, 1), Word(d, 2), HostStopText(stop),
					func ? ": " : "", func ? func : "");

	if(PvmDbgDisasm(g_host.code + pc, g_host.len - pc, pc, 1, lines) == 1) printf("  -> %s\n", lines[0]);
	ShowSource(pc);

	if(stop == HOSTSTOP_RETURNED){
		if(g_host.resultCount == 0){ printf("  it left nothing on the stack\n"); return; }

		printf("  it left on the stack (arguments included), top first:");
		for(int i = 0; i < g_host.resultCount; i++) printf(" %d", g_host.result[i]);
		printf("\n");
	}
}

static void HostRun(void){
	int stop;

	g_inRun = 1;
	stop 		= HostExecRun(&g_host);
	g_inRun = 0;
	TakeInterrupt();

	HostShowState(stop);
}

/* A function name from the listing, or whatever 'break' takes. */
static int HostResolve(const char* text, uint16_t* addr){
	return HostExecFindFunc(&g_host, text, addr) || ResolveTarget(text, addr);
}

static void HostBreakpointCmd(int set, const char* arg){
	uint16_t addr;

	if(!arg){ printf("  expected an address, a function or a source line\n"); return; }
	if(!HostResolve(arg, &addr)) return;

	if(!HostExecBreak(&g_host, addr, set)) printf("  refused: %s\n", g_host.message);
	else printf("  0x%04X %s\n", addr, set ? "will stop the program" : "no longer will");
}

static void HostShowBreakpoints(void){
	if(g_host.brkpCount == 0){ printf("  no breakpoints set\n"); return; }

	printf("  %d breakpoint(s):", g_host.brkpCount);
	for(int i = 0; i < g_host.brkpCount; i++) printf(" 0x%04X", g_host.brkp[i]);
	printf("\n");
}

/* "call square 0 7": the arguments are pushed in the order given, the way the
 * caller's code would push them -- a result slot included, if the function's
 * calling convention has one. */
static void HostCallCmd(char* rest){
	int16_t  args[16];
	int 		 argCount = 0;
	uint16_t addr;
	char* 	 target = strtok(rest, " \t");
	char* 	 word;

	if(!target){ printf("  expected a function and its arguments, e.g. 'call square 0 7'\n"); return; }
	if(!HostResolve(target, &addr)) return;

	while((word = strtok(0, " \t")) != 0){
		char* end;
		long 	value = strtol(word, &end, 0);

		if(*end || value < -32768 || value > 65535){ printf("  '%s' is not a 16-bit word\n", word); return; }
		if(argCount == (int)(sizeof(args) / sizeof(args[0]))){ printf("  too many arguments\n"); return; }
		args[argCount++] = (int16_t)value;
	}

	if(!HostExecCall(&g_host, addr, args, argCount)){ printf("  %s\n", g_host.message); return; }
	HostRun();
}

/* The commands that mean something else when the program is on the host.
 * Returns 0 for the rest, which work the same either way: stack, mem, exec. */
static int HostCommand(const char* verb, const char* arg, char* rest){
	if(!strcmp(verb, "state") || !strcmp(verb, "i")) 			HostShowState(-1);
	else if(!strcmp(verb, "run") || !strcmp(verb, "r")) 	HostRun();
	else if(!strcmp(verb, "step") || !strcmp(verb, "s")) HostShowState(HostExecStep(&g_host));
	else if(!strcmp(verb, "pause") || !strcmp(verb, "p")) printf("  nothing is running -- Ctrl+C stops a run\n");
	else if(!strcmp(verb, "reset")){ HostExecReset(&g_host); HostShowState(-1); }
	else if(!strcmp(verb, "breaks") || !strcmp(verb, "b")) HostShowBreakpoints();
	else if(!strcmp(verb, "break")) 											 HostBreakpointCmd(1, arg);
	else if(!strcmp(verb, "clear")) 											 HostBreakpointCmd(0, arg);
	else if(!strcmp(verb, "call")) 												 HostCallCmd(rest);
	else if(!strcmp(verb, "abort")){ Send(DBGCMD_EXEC_ABORT, 0, 0); HostShowState(-1); }
	else if(!strcmp(verb, "dis") || !strcmp(verb, "d")){
		char lines[PVMDBG_CFG_CODE_READ_MAX][PVMDBG_DISASM_LINE_MAX];
		int  n = PvmDbgDisasm(g_host.code + g_host.pc, g_host.len - g_host.pc, g_host.pc,
													PVMDBG_CFG_CODE_READ_MAX, lines);

		for(int i = 0; i < n; i++) printf("  %s %s\n", (i == 0) ? "->" : "  ", lines[i]);
	}
	else
		return 0;

	return 1;
}

/* Returns 0 when the session should end. */
static int Execute(char* line){
	char  rest[256];
	char* verb;
	char* arg;

	/* Everything after the verb, kept whole before strtok cuts it up: "exec"
	 * takes a line of code, not a single word. */
	{
		const char* p = line + strspn(line, " \t");
		size_t 			n;

		p += strcspn(p, " \t\r\n");
		p += strspn(p, " \t");
		n  = strlen(p);
		while(n > 0 && strchr(" \t\r\n", p[n - 1])) n--;
		if(n >= sizeof(rest)) n = sizeof(rest) - 1;
		memcpy(rest, p, n);
		rest[n] = '\0';
	}

	verb = strtok(line, " \t\r\n");
	arg  = strtok(0, " \t\r\n");

	if(!verb) return 1;

	if(!strcmp(verb, "exit") || !strcmp(verb, "quit") || !strcmp(verb, "q")) return 0;

	if(g_hostMode && HostCommand(verb, arg, rest)) return 1;

	if(!strcmp(verb, "help") || !strcmp(verb, "?")) 				 ShowHelp();
	else if(!strcmp(verb, "state") || !strcmp(verb, "i")) 	 ShowState();
	else if(!strcmp(verb, "breaks") || !strcmp(verb, "b"))	 ShowBreakpoints();
	else if(!strcmp(verb, "mem") || !strcmp(verb, "m")) 		 ShowMemory();
	else if(!strcmp(verb, "break")) 												 BreakpointCmd(DBGCMD_BRKP_SET, arg);
	else if(!strcmp(verb, "clear")) 												 BreakpointCmd(DBGCMD_BRKP_CLR, arg);
	else if(!strcmp(verb, "dis") || !strcmp(verb, "d")){
		const uint8_t* d;
		uint16_t 			 cmd;

		if(Reply(Send(DBGCMD_STATE, 0, 0), &cmd, &d) < 8) NoAnswer();
		else ShowCode(Word(d, 0), PVMDBG_CFG_CODE_READ_MAX);
	}
	else if(!strcmp(verb, "stack")){
		uint16_t count = PVMDBG_CFG_STACK_READ_MAX;
		if(arg) ParseAddr(arg, &count);
		ShowStack((int)count);
	}
	else if(!strcmp(verb, "run") || !strcmp(verb, "r")){
		Send(DBGCMD_RUN, 0, 0);
		RunUntilStopped();
		ShowState();
	}
	else if(!strcmp(verb, "step") || !strcmp(verb, "s")){
		Send(DBGCMD_STEP, 0, 0);
		RunUntilStopped();
		ShowState();
	}
	else if(!strcmp(verb, "pause") || !strcmp(verb, "p")){
		Send(DBGCMD_PAUSE, 0, 0);
		ShowState();
	}
	else if(!strcmp(verb, "reset")){
		Send(DBGCMD_RESET, 0, 0);
		ShowState();
	}
	else if(!strcmp(verb, "exec")) 													 ExecCmd(rest);
	else if(!strcmp(verb, "abort")){
		Send(DBGCMD_EXEC_ABORT, 0, 0);
		ShowState();
	}
	else
		printf("  unknown command '%s' -- try 'help'\n", verb);

	return 1;
}

/* ---- session ---------------------------------------------------------- */

static void Usage(void){
	fprintf(stderr,
		"usage: pvmdbg [program.bin]      debug a program in this process\n"
		"       pvmdbg --port COM3        debug a board over a serial port\n"
		"       pvmdbg --tcp host:port    the same over TCP: a bridge to a board's UART,\n"
		"                                 or pvmboard (just a port means this machine)\n"
		"       --map <file.dbgmap>       source lines, from pvmc -g\n"
		"       --machine                 line protocol on stdio, for the VS Code extension\n"
		"       --host program.bin        keep the program here and send the device one\n"
		"                                 instruction at a time -- nothing is flashed;\n"
		"                                 with --port/--tcp for a board, alone for this process\n");
}

/*
 * Locally the map is guessed from the program's own name, because it is what
 * pvmc writes next to the .bin. Over a wire there is nothing to guess from --
 * the program lives in the board's flash and the host has never seen it -- so
 * the map has to be named, and it is on whoever names it to pick the one that
 * matches the firmware actually running.
 */
static void LoadMapFor(const char* explicitMap, const char* bytecodePath){
	char guess[512];
	const char* dot;

	if(explicitMap){
		SourceMapLoad(explicitMap);
		return;
	}

	if(!bytecodePath) return;

	dot = strrchr(bytecodePath, '.');
	if(!dot || (size_t)(dot - bytecodePath) + 8 >= sizeof(guess)) return;

	snprintf(guess, sizeof(guess), "%.*s.dbgmap", (int)(dot - bytecodePath), bytecodePath);

	/* Checked before loading so that a missing guess stays quiet: only a map the
	 * user actually asked for is worth complaining about. */
	{
		FILE* probe = fopen(guess, "r");
		if(!probe) return;
		fclose(probe);
	}

	SourceMapLoad(guess);
}

int main(int argc, char** argv){
	const char* port 	 = 0;
	const char* tcp 	 = 0;
	const char* path 	 = 0;
	const char* map 	 = 0;
	const char* hostPath = 0;
	int 				machine = 0;
	char 				line[256];

	for(int i = 1; i < argc; i++){
		if(!strcmp(argv[i], "--port") || !strcmp(argv[i], "-p")){
			if(++i >= argc){ Usage(); return EXIT_FAILURE; }
			port = argv[i];
		}
		else if(!strcmp(argv[i], "--tcp")){
			if(++i >= argc){ Usage(); return EXIT_FAILURE; }
			tcp = argv[i];
		}
		else if(!strcmp(argv[i], "--host")){
			if(++i >= argc){ Usage(); return EXIT_FAILURE; }
			hostPath = argv[i];
		}
		else if(!strcmp(argv[i], "--map") || !strcmp(argv[i], "-m")){
			if(++i >= argc){ Usage(); return EXIT_FAILURE; }
			map = argv[i];
		}
		else if(!strcmp(argv[i], "--machine")){ machine = 1; }
		else if(argv[i][0] == '-'){ Usage(); return EXIT_FAILURE; }
		else path = argv[i];
	}

	if(port && tcp){
		fprintf(stderr, "pvmdbg: one board at a time -- --port or --tcp, not both\n");
		return EXIT_FAILURE;
	}
	if((port || tcp) && path){
		fprintf(stderr, "pvmdbg: a board already has its program -- pass a file or a port, not both\n"
										"        (or --host program.bin, to run one the board does not have)\n");
		return EXIT_FAILURE;
	}
	if(hostPath && path){
		fprintf(stderr, "pvmdbg: --host already names the program\n");
		return EXIT_FAILURE;
	}
	if(hostPath && machine){
		fprintf(stderr, "pvmdbg: --host is not available in --machine mode yet\n");
		return EXIT_FAILURE;
	}

	/* Locally the in-process VM is the "device"; it is handed the same program
	 * only because it needs one to start, and never runs it -- with --host, the
	 * session feeds it instructions itself. */
	if(port) 		 g_link = TransportSerialOpen(port);
	else if(tcp) g_link = TransportTcpOpen(tcp);
	else 				 g_link = TransportLocalOpen(hostPath ? hostPath : (path ? path : PVM_DEFAULT_BYTECODE));
	if(!g_link) return EXIT_FAILURE;

	/* Before anything is printed: from here on stdout carries the protocol, and
	 * the source map is the caller's business rather than ours. */
	if(machine) return MachineRun(g_link);

	/* With --host the program is right here, so its map can be guessed even
	 * when the device is at the far end of a cable. */
	LoadMapFor(map, hostPath ? hostPath : ((port || tcp) ? 0 : (path ? path : PVM_DEFAULT_BYTECODE)));

	if(hostPath){
		if(!HostExecOpen(&g_host, g_link, g_frame, hostPath)){
			g_link->close(g_link);
			return EXIT_FAILURE;
		}
		g_hostMode = 1;
	}

	/* The agent leaves the VM running, which is right for a board that boots on
	 * its own but wrong for a session someone opened to look at it: stop first,
	 * so nothing moves while the first prompt is being read. */
	if(Send(DBGCMD_PAUSE, 0, 0) == 0){
		printf("pvmdbg: %s is not answering\n", g_link->description);
		g_link->close(g_link);
		return EXIT_FAILURE;
	}

	signal(SIGINT, OnInterrupt);

	printf("pvmdbg: %s\n", g_link->description);

	if(g_hostMode){
		/* A fresh stack for a fresh program: whatever the device's own program
		 * left on it is not this one's. RAM is left alone, as a reflash would. */
		HostExecReset(&g_host);
		printf("pvmdbg: '%s' (%d bytes) runs from here, the device executes it instruction by instruction\n",
					 hostPath, g_host.len);
	}

	printf("pvmdbg: 'help' for commands, 'exit' or Ctrl+C to finish\n");
	if(g_hostMode) HostShowState(-1);
	else 					 ShowState();

	for(;;){
		printf("(pvmdbg) ");
		fflush(stdout);

		/* Only "exit" and end-of-input finish the session from here; a Ctrl+C at
		 * the prompt is dealt with inside OnInterrupt, which never returns. */
		if(!fgets(line, (int)sizeof(line), stdin)) break;
		if(!Execute(line)) break;
	}

	printf("pvmdbg: session closed\n");

	g_link->close(g_link);
	return EXIT_SUCCESS;
}
