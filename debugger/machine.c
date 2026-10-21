/*
 * machine.c
 *
 * Line protocol for a program driving the debugger (see machine.h).
 *
 *   > state                 < ok pc=39 sp=11 fp=9 mode=0 reason=3
 *   > break 39              < ok
 *   > run                   < ok
 *   > poll                  < ok running=1          (still going)
 *   > poll                  < ok running=0          (came to rest)
 *   > stack 4               < ok n=2 v0=3 v1=5
 *   > code 39 8             < ok n=8 b0=26 b1=0 ...
 *   > data 0x4000 2         < ok n=2 v0=1 v1=0      (data memory, from an address)
 *   > mem                   < ok stack=1024 ram=64
 *   > frames                < ok n=2 pc0=22 fn0=square built0=1 pc1=10 fn1=main built1=1
 *   > vars 0                < ok n=1 name0=x kind0=param off0=-3 val0=7
 *   > globals               < ok n=2 name0=arr addr0=0 size0=5 val0=0,2,4,6,8 name1=SUM ...
 *
 * frames/vars/globals need the debug map's symbols (--map, or the map beside
 * the program); without them "frames" answers n=0. A variable that does not
 * exist at that point -- not declared yet, or the frame not built -- has val=?.
 *   > quit                  < ok          (and the board is let go: DETACH)
 *
 * Anything that goes wrong answers "error <reason>", so the caller never has to
 * guess from silence.
 *
 * "run" only sets the program going; "poll" is what lets it make progress and
 * says whether it has stopped. Splitting them is what keeps the caller in
 * control: a single blocking "run" would leave a program that never stops with
 * no way to be paused, and a background thread here is precisely the sort of
 * thing that has no business in a tool this small.
 */
#include "machine.h"

#include "dbgclient.h"
#include "debugsym.h"
#include "frames.h"
#include "pvmdbg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Long enough that a breakpoint about to be hit is reported without a visible
 * wait, short enough that a pause never feels stuck. */
#define POLL_BUDGET_MS (60)

/* Most a single "stack"/"code"/"data" hands back: every item is a request of
 * its own now, and the caller asks again for more. */
#define LIST_MAX (32)

static Transport_s* g_link = 0;
static char 				g_frame[PVMDBG_CFG_BUF_SIZE];

/* The call stack as last walked. An editor asks for frames, then scopes, then
 * each scope's variables, all at one stop -- walking again for each would cost
 * a dozen round trips apiece on a slow wire. Anything that can move the
 * program throws it away. */
static CallStack_s 	g_cs;
static int 					g_csValid = 0;

static void Fail(const char* reason){
	printf("error %s\n", reason);
	fflush(stdout);
}

static void Ok(const char* tail){
	printf("ok%s%s\n", tail && *tail ? " " : "", tail ? tail : "");
	fflush(stdout);
}

/* A command that carries a 16-bit argument and expects nothing back. */
static void Simple(uint16_t cmd, int argc, long a0){
	uint8_t payload[2];
	int 		r;

	payload[0] = (uint8_t)(a0 & 0xFF);
	payload[1] = (uint8_t)((a0 >> 8) & 0xFF);

	g_csValid = 0;
	r = DbgCommand(g_link, g_frame, cmd, argc > 0 ? payload : 0, argc > 0 ? 2 : 0, 0);
	if(r < 0) 							 Fail("no-answer");
	else if(r == DBGCMD_ERROR) Fail("refused");
	else 										 Ok("");
}

static void CmdState(void){
	DbgState_s s;
	char 			 tail[96];

	if(DbgGetState(g_link, g_frame, &s) < 0){ Fail("no-answer"); return; }

	snprintf(tail, sizeof(tail), "pc=%u sp=%u fp=%u mode=%u reason=%u", s.pc, s.sp, s.fp, s.mode, s.reason);
	Ok(tail);
}

/* Shared by "stack", "breaks", "code" and "data": a count, then that many values. */
static void ReplyList(const int* values, int n, const char* prefix){
	char tail[1024];
	int  used = snprintf(tail, sizeof(tail), "n=%d", n);

	for(int i = 0; i < n && used > 0 && used < (int)sizeof(tail); i++)
		used += snprintf(tail + used, sizeof(tail) - (size_t)used, " %s%d=%d", prefix, i, values[i]);

	Ok(tail);
}

static int Clamp(long count){
	return (count < 0) ? 0 : (count > LIST_MAX) ? LIST_MAX : (int)count;
}

static void CmdStack(long count){
	int16_t words[LIST_MAX];
	int 		values[LIST_MAX];
	int 		n = DbgReadStack(g_link, g_frame, words, Clamp(count));

	if(n < 0){ Fail("no-answer"); return; }
	for(int i = 0; i < n; i++) values[i] = words[i];
	ReplyList(values, n, "v");
}

static void CmdBreaks(void){
	uint16_t addrs[64];
	int 		 values[64];
	int 		 n = DbgListBreakpoints(g_link, g_frame, addrs, 64);

	if(n < 0){ Fail("no-answer"); return; }
	for(int i = 0; i < n; i++) values[i] = addrs[i];
	ReplyList(values, n, "a");
}

static void CmdCode(long addr, long count){
	uint8_t  bytes[LIST_MAX];
	int 		 values[LIST_MAX];
	uint16_t codeLen = 0xFFFF, unused;
	int 		 n;

	if(DbgMemInfo(g_link, g_frame, 1, &codeLen, &unused) < 0){ Fail("no-answer"); return; }
	if(addr < 0 || addr >= codeLen){ Fail("refused"); return; }

	n = DbgReadCode(g_link, g_frame, (uint16_t)addr, bytes, Clamp(count), codeLen);
	if(n < 0){ Fail("no-answer"); return; }

	for(int i = 0; i < n; i++) values[i] = bytes[i];
	ReplyList(values, n, "b");
}

/* Words of data memory from an address -- RAM from 0, the user section from
 * 0x4000 -- stopping at the first one the device refuses. */
static void CmdData(long addr, long count){
	int values[LIST_MAX];
	int n = 0;

	for(int i = 0; i < Clamp(count); i++){
		int16_t value;
		int 		r = DbgReadData(g_link, g_frame, (uint16_t)(addr + i), &value);

		if(r < 0){ Fail("no-answer"); return; }
		if(r == 0) break;
		values[n++] = value;
	}

	if(n == 0){ Fail("refused"); return; }
	ReplyList(values, n, "v");
}

/* Pool sizes in words, as the device reports them. */
static void CmdMem(void){
	uint16_t stackLen, ramLen;
	char 		 tail[48];
	int 		 r = DbgMemInfo(g_link, g_frame, 0, &stackLen, &ramLen);

	if(r < 0){ Fail("no-answer"); return; }
	if(r == 0){ Fail("refused"); return; }

	snprintf(tail, sizeof(tail), "stack=%u ram=%u", stackLen, ramLen);
	Ok(tail);
}

static void CmdPoll(void){
	int stopped = g_link->settle(g_link, POLL_BUDGET_MS);

	g_csValid = 0;
	Ok(stopped ? "running=0" : "running=1");
}

/* ---- variables by name -------------------------------------------------- */

/* Walks the stack unless the last walk still stands. 1 when g_cs holds at
 * least one frame with symbols, 0 when there are none to have, -1 for no
 * answer. */
static int Frames(void){
	DbgState_s s;

	if(g_csValid) return g_cs.count > 0 && g_cs.frames[0].func;

	g_cs.count = 0;
	if(DbgGetState(g_link, g_frame, &s) < 0) 					 return -1;
	if(s.pc == DBG_PC_NOT_IN_PROGRAM){ g_csValid = 1; return 0; }
	if(FramesWalk(g_link, g_frame, s.pc, -1, FRAMES_MAX, &g_cs) < 0) return -1;

	g_csValid = 1;
	return g_cs.frames[0].func != 0;
}

/* Appends " key=value" while it fits. */
static void Put(char* tail, size_t size, int* used, const char* format, ...){
	va_list args;

	if(*used < 0 || *used >= (int)size) return;
	va_start(args, format);
	*used += vsnprintf(tail + *used, size - (size_t)*used, format, args);
	va_end(args);
}

static void CmdFrames(void){
	char tail[2048];
	int  used = 0;
	int  r 		= Frames();

	if(r < 0){ Fail("no-answer"); return; }
	if(r == 0){ Ok("n=0"); return; }

	Put(tail, sizeof(tail), &used, "n=%d", g_cs.count);
	for(int i = 0; i < g_cs.count; i++)
		Put(tail, sizeof(tail), &used, " pc%d=%u fn%d=%s built%d=%d",
				i, g_cs.frames[i].pc, i, g_cs.frames[i].func, i, g_cs.frames[i].built);
	Ok(tail);
}

static void CmdVars(long index){
	char tail[2048];
	int  used = 0, n = 0;
	int  r 		= Frames();
	const Frame_s* f;

	if(r < 0){ Fail("no-answer"); return; }
	if(r == 0 || index < 0 || index >= g_cs.count){ Fail("no-frame"); return; }
	f = &g_cs.frames[index];

	/* The count goes first, so it is written last into a gap left for it. */
	for(int v = 0; v < DebugSymVarCount(); v++){
		const DebugVar_s* var = DebugSymVar(v);
		int16_t 					value;
		int 							got;

		if(strcmp(var->func, f->func)) continue;

		got = FramesReadVar(g_link, g_frame, &g_cs, f, var, &value);
		if(got < 0){ Fail("no-answer"); return; }

		Put(tail, sizeof(tail), &used, " name%d=%s kind%d=%s off%d=%d", n, var->name,
				n, var->isParam ? "param" : "local", n, var->offset);
		if(got) Put(tail, sizeof(tail), &used, " val%d=%d", n, value);
		else 		Put(tail, sizeof(tail), &used, " val%d=?", n);
		n++;
	}

	{
		char line[2100];
		snprintf(line, sizeof(line), "n=%d%s", n, tail);
		Ok(line);
	}
}

/* Arrays come back as their words joined by commas, the first 16 at most. */
static void CmdGlobals(void){
	char tail[4096];
	int  used = 0;

	Put(tail, sizeof(tail), &used, "n=%d", DebugSymGlobalCount());

	for(int i = 0; i < DebugSymGlobalCount(); i++){
		const DebugGlobal_s* g = DebugSymGlobal(i);
		int 								 shown = g->size < 16 ? g->size : 16;

		Put(tail, sizeof(tail), &used, " name%d=%s addr%d=%u size%d=%d val%d=", i, g->name, i, g->addr, i, g->size, i);
		for(int w = 0; w < shown; w++){
			int16_t value;
			int 		r = DbgReadData(g_link, g_frame, (uint16_t)(g->addr + w), &value);

			if(r < 0){ Fail("no-answer"); return; }
			if(r == 0) Put(tail, sizeof(tail), &used, "%s?", w ? "," : "");
			else 			 Put(tail, sizeof(tail), &used, "%s%d", w ? "," : "", value);
		}
	}

	Ok(tail);
}

int MachineRun(Transport_s* link){
	char line[256];

	g_link = link;

	/* Stopped before the first command, exactly as the terminal session does, so
	 * nothing moves while the far end is still deciding what to ask for. */
	DbgCommand(g_link, g_frame, DBGCMD_PAUSE, 0, 0, 0);

	while(fgets(line, (int)sizeof(line), stdin)){
		char* verb = strtok(line, " \t\r\n");
		char* arg0 = strtok(0, " \t\r\n");
		char* arg1 = strtok(0, " \t\r\n");
		long  a0 	 = arg0 ? strtol(arg0, 0, 0) : 0;
		long  a1 	 = arg1 ? strtol(arg1, 0, 0) : 0;

		if(!verb) continue;

		if(!strcmp(verb, "quit")){ Ok(""); break; }

		if(!strcmp(verb, "state")) 			 CmdState();
		else if(!strcmp(verb, "poll")) 	 CmdPoll();
		else if(!strcmp(verb, "run")) 		 Simple(DBGCMD_RUN, 0, 0);
		else if(!strcmp(verb, "step")) 	 Simple(DBGCMD_STEP, 0, 0);
		else if(!strcmp(verb, "pause")) 	 Simple(DBGCMD_PAUSE, 0, 0);
		else if(!strcmp(verb, "reset")) 	 Simple(DBGCMD_RESET, 0, 0);
		else if(!strcmp(verb, "breaks"))  CmdBreaks();
		else if(!strcmp(verb, "break")) 	 Simple(DBGCMD_BRKP_SET, 1, a0);
		else if(!strcmp(verb, "clear")) 	 Simple(DBGCMD_BRKP_CLR, 1, a0);
		else if(!strcmp(verb, "stack")) 	 CmdStack(arg0 ? a0 : 8);
		else if(!strcmp(verb, "code")) 	 CmdCode(a0, arg1 ? a1 : 16);
		else if(!strcmp(verb, "data")) 	 CmdData(a0, arg1 ? a1 : 1);
		else if(!strcmp(verb, "mem")) 		 CmdMem();
		else if(!strcmp(verb, "frames")) 	 CmdFrames();
		else if(!strcmp(verb, "vars")) 	 CmdVars(arg0 ? a0 : 0);
		else if(!strcmp(verb, "globals")) CmdGlobals();
		else 														 Fail("unknown-command");
	}

	/* As the terminal session does on "exit": the board goes back to running
	 * its program rather than staying paused with nobody attached. */
	DbgCommand(g_link, g_frame, DBGCMD_DETACH, 0, 0, 0);
	link->close(link);
	return EXIT_SUCCESS;
}
