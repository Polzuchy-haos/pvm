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
#include "pvmdbg.h"

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
	Ok(stopped ? "running=0" : "running=1");
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
		else 														 Fail("unknown-command");
	}

	/* As the terminal session does on "exit": the board goes back to running
	 * its program rather than staying paused with nobody attached. */
	DbgCommand(g_link, g_frame, DBGCMD_DETACH, 0, 0, 0);
	link->close(link);
	return EXIT_SUCCESS;
}
