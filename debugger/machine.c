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
 *   > quit                  < ok
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

#include "pvmdbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Long enough that a breakpoint about to be hit is reported without a visible
 * wait, short enough that a pause never feels stuck. */
#define POLL_BUDGET_MS (60)

static Transport_s* g_link = 0;
static char 				g_frame[PVMDBG_CFG_BUF_SIZE];

static int Send(uint16_t cmd, const uint8_t* data, int dataLen){
	return g_link->exchange(g_link, cmd, data, dataLen, g_frame);
}

static int Reply(int len, uint16_t* cmd, const uint8_t** data){
	int dataLen;
	return (len > 0 && PVMDBG_FrameParse(g_frame, len, cmd, data, &dataLen)) ? dataLen : -1;
}

static uint16_t Word(const uint8_t* p, int index){
	return (uint16_t)((uint16_t)p[2 * index] | ((uint16_t)p[2 * index + 1] << 8));
}

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
	const uint8_t* d;
	uint16_t 			 replyCmd;
	uint8_t 			 payload[2];

	if(argc > 0){
		payload[0] = (uint8_t)(a0 & 0xFF);
		payload[1] = (uint8_t)((a0 >> 8) & 0xFF);
	}

	if(Reply(Send(cmd, argc > 0 ? payload : 0, argc > 0 ? 2 : 0), &replyCmd, &d) < 0){
		Fail("no-answer");
		return;
	}

	if(replyCmd == DBGCMD_ERROR) Fail("refused");
	else 												 Ok("");
}

static void CmdState(void){
	const uint8_t* d;
	uint16_t 			 cmd;
	char 					 tail[96];

	if(Reply(Send(DBGCMD_STATE, 0, 0), &cmd, &d) < 8){ Fail("no-answer"); return; }

	snprintf(tail, sizeof(tail), "pc=%u sp=%u fp=%u mode=%u reason=%u",
					 Word(d, 0), Word(d, 1), Word(d, 2), d[6], d[7]);
	Ok(tail);
}

/* Shared by "stack" and "breaks" and "code": a count, then that many values. */
static void ReplyList(const uint8_t* d, int wordSized, const char* prefix){
	char 	tail[512];
	int 	used = snprintf(tail, sizeof(tail), "n=%u", d[0]);

	for(int i = 0; i < d[0] && used > 0 && used < (int)sizeof(tail); i++){
		int value = wordSized ? (int)(int16_t)Word(d + 1, i) : (int)d[1 + i];
		used += snprintf(tail + used, sizeof(tail) - (size_t)used, " %s%d=%d", prefix, i, value);
	}

	Ok(tail);
}

static void CmdStack(long count){
	const uint8_t* d;
	uint16_t 			 cmd;
	uint8_t 			 arg = (uint8_t)count;

	if(Reply(Send(DBGCMD_READ_STACK, &arg, 1), &cmd, &d) < 1){ Fail("no-answer"); return; }
	ReplyList(d, 1, "v");
}

static void CmdBreaks(void){
	const uint8_t* d;
	uint16_t 			 cmd;

	if(Reply(Send(DBGCMD_BRKP_LIST, 0, 0), &cmd, &d) < 1){ Fail("no-answer"); return; }
	ReplyList(d, 1, "a");
}

static void CmdCode(long addr, long count){
	const uint8_t* d;
	uint16_t 			 cmd;
	uint8_t 			 arg[3];

	arg[0] = (uint8_t)(addr & 0xFF);
	arg[1] = (uint8_t)((addr >> 8) & 0xFF);
	arg[2] = (uint8_t)count;

	if(Reply(Send(DBGCMD_READ_CODE, arg, 3), &cmd, &d) < 1){ Fail("no-answer"); return; }
	if(cmd == DBGCMD_ERROR){ Fail("refused"); return; }
	ReplyList(d, 0, "b");
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
	Send(DBGCMD_PAUSE, 0, 0);

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
		else if(!strcmp(verb, "stack")) 	 CmdStack(arg0 ? a0 : PVMDBG_CFG_STACK_READ_MAX);
		else if(!strcmp(verb, "code")) 	 CmdCode(a0, arg1 ? a1 : PVMDBG_CFG_CODE_READ_MAX);
		else 														 Fail("unknown-command");
	}

	link->close(link);
	return EXIT_SUCCESS;
}
