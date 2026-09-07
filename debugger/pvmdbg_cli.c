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

#include "disasm.h"
#include "pvmdbg.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PVM_DEFAULT_BYTECODE
#define PVM_DEFAULT_BYTECODE "bytecode_listing.bin"
#endif

static Transport_s* g_link 	= 0;
static char 				g_frame[PVMDBG_CFG_BUF_SIZE];

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
	g_link->settle(g_link);
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

static void ShowState(void){
	const uint8_t* d;
	uint16_t 			 cmd;
	uint16_t 			 pc;

	if(Reply(Send(DBGCMD_STATE, 0, 0), &cmd, &d) < 8){ NoAnswer(); return; }

	pc = Word(d, 0);
	printf("  pc=0x%04X  sp=%u  fp=%u  %s\n", pc, Word(d, 1), Word(d, 2), StopReason(d[7]));

	/* The instruction about to run, right under the numbers: an address on its
	 * own means looking it up in a listing every single step. */
	ShowCode(pc, 1);
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
		"  break <addr>    set a breakpoint, e.g. 'break 0x10' or 'break 16'\n"
		"  clear <addr>    remove one\n"
		"  breaks, b       list the ones that are set\n"
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

static void BreakpointCmd(uint16_t cmd, const char* arg){
	const uint8_t* d;
	uint16_t 			 addr, replyCmd;
	uint8_t 			 payload[2];

	if(!arg || !ParseAddr(arg, &addr)){
		printf("  expected an address, e.g. 'break 0x10'\n");
		return;
	}

	payload[0] = (uint8_t)(addr & 0xFF);
	payload[1] = (uint8_t)(addr >> 8);

	if(Reply(Send(cmd, payload, 2), &replyCmd, &d) < 0){ NoAnswer(); return; }

	if(replyCmd == DBGCMD_ERROR)
		printf("  refused: 0x%04X is outside the program, or there is no slot left\n", addr);
	else
		printf("  0x%04X %s\n", addr,
						(cmd == DBGCMD_BRKP_SET) ? "will stop the program" : "no longer will");
}

/* Returns 0 when the session should end. */
static int Execute(char* line){
	char* verb = strtok(line, " \t\r\n");
	char* arg  = strtok(0, " \t\r\n");

	if(!verb) return 1;

	if(!strcmp(verb, "exit") || !strcmp(verb, "quit") || !strcmp(verb, "q")) return 0;

	if(!strcmp(verb, "help") || !strcmp(verb, "?")) 				 ShowHelp();
	else if(!strcmp(verb, "state") || !strcmp(verb, "i")) 	 ShowState();
	else if(!strcmp(verb, "breaks") || !strcmp(verb, "b"))	 ShowBreakpoints();
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
	else
		printf("  unknown command '%s' -- try 'help'\n", verb);

	return 1;
}

/* ---- session ---------------------------------------------------------- */

static void Usage(void){
	fprintf(stderr,
		"usage: pvmdbg [program.bin]      debug a program in this process\n"
		"       pvmdbg --port COM3        debug a board over a serial port\n");
}

int main(int argc, char** argv){
	const char* port = 0;
	const char* path = 0;
	char 				line[256];

	for(int i = 1; i < argc; i++){
		if(!strcmp(argv[i], "--port") || !strcmp(argv[i], "-p")){
			if(++i >= argc){ Usage(); return EXIT_FAILURE; }
			port = argv[i];
		}
		else if(argv[i][0] == '-'){ Usage(); return EXIT_FAILURE; }
		else path = argv[i];
	}

	if(port && path){
		fprintf(stderr, "pvmdbg: a board already has its program -- pass a file or a port, not both\n");
		return EXIT_FAILURE;
	}

	g_link = port ? TransportSerialOpen(port)
								: TransportLocalOpen(path ? path : PVM_DEFAULT_BYTECODE);
	if(!g_link) return EXIT_FAILURE;

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
	printf("pvmdbg: 'help' for commands, 'exit' or Ctrl+C to finish\n");
	ShowState();

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
