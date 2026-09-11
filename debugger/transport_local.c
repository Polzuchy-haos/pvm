/*
 * transport_local.c
 *
 * Debugging a program in this very process: the VM, the debug agent and the
 * session all live here, and a "frame" never leaves memory.
 *
 * The loop in Settle is deliberately the same shape as the firmware main loop
 * sketched in pvm/pvmdbg.h -- poll, then tick if the agent allows it. Nothing
 * here reaches into PVM_State_s to answer a question that the protocol could
 * answer, even though it trivially could: the moment it did, this transport
 * would start behaving differently from the serial one and stop being a useful
 * rehearsal for debugging a real board.
 */
#include "transport.h"

#include "pvm.h"
#include "pvmdbg.h"
#include "pvmenv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A program with no breakpoints and no HALT would otherwise spin here forever.
 * Only applies when the caller gave no time budget: one that did is watching
 * the clock itself and decides for itself when enough is enough. */
#define MAX_TICKS_PER_RUN (2000000L)

/* clock() costs far more than an instruction does, so it is consulted rarely. */
#define TICKS_BETWEEN_CLOCK_CHECKS (1024)

typedef struct{
	Transport_s base;
	PVM_State_s vm;
	PVMDbg_s 		dbg;
	char* 			code;
} Local_s;

static int LocalExchange(Transport_s* self, uint16_t cmd,
												 const uint8_t* data, int dataLen, char* frame){
	Local_s* local = (Local_s*)self;
	int 		 len 	 = PVMDBG_FrameBuild(frame, cmd, data, dataLen);

	PVMDBG_Poll(&local->dbg, frame, &len);
	return len;
}

static int LocalSettle(Transport_s* self, int budgetMs){
	Local_s* local = (Local_s*)self;
	char 		 frame[PVMDBG_CFG_BUF_SIZE];
	long 		 ticks = 0;
	clock_t 	start = clock();

	while(!TransportInterrupted() && PVMDBG_IsAccessExecTick(&local->dbg)){
		int status = PVM_ExecTick(&local->vm);

		if(status < 0){
			/* The protocol gives the device no way to volunteer this, so a board
			 * would go quiet here instead; in-process the loop can at least say so.
			 * On stderr, because stdout may be carrying a machine protocol. */
			fprintf(stderr, "  execution error %d -- the VM refused to run that instruction\n", status);
			LocalExchange(self, DBGCMD_PAUSE, 0, 0, frame);
			return 1;
		}

		ticks++;

		if(budgetMs <= 0){
			if(ticks >= MAX_TICKS_PER_RUN){
				fprintf(stderr, "  %ld ticks without stopping -- pausing, set a breakpoint\n", ticks);
				LocalExchange(self, DBGCMD_PAUSE, 0, 0, frame);
				return 1;
			}
		}
		else if(ticks % TICKS_BETWEEN_CLOCK_CHECKS == 0 &&
						(long)((clock() - start) * 1000 / CLOCKS_PER_SEC) >= budgetMs){
			return 0; /* still going -- the caller wanted the wheel back */
		}
	}

	/* Nothing to do for an interrupt here: the session pauses the VM itself, the
	 * same way it does for a board, so both come to rest identically. */
	return 1;
}

static void LocalClose(Transport_s* self){
	Local_s* local = (Local_s*)self;

	free(local->code);
	free(local);
}

static char* LoadFile(const char* path, int* outLen){
	FILE* f = fopen(path, "rb");
	long 	size;
	char* buf;

	if(!f){
		fprintf(stderr, "pvmdbg: cannot open '%s'\n", path);
		return 0;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = malloc((size_t)size);
	if(buf) fread(buf, 1, (size_t)size, f);
	fclose(f);

	*outLen = (int)size;
	return buf;
}

Transport_s* TransportLocalOpen(const char* bytecodePath){
	Local_s* local;
	char* 	 code;
	int 		 len = 0;

	code = LoadFile(bytecodePath, &len);
	if(!code) return 0;

	local = calloc(1, sizeof(Local_s));
	if(!local){ free(code); return 0; }

	local->code 					 = code;
	local->base.exchange 	 = LocalExchange;
	local->base.settle 		 = LocalSettle;
	local->base.close 		 = LocalClose;
	snprintf(local->base.description, sizeof(local->base.description),
					 "'%s', %d bytes of bytecode, running in this process", bytecodePath, len);

	PVM_Init(&local->vm, code, len);
	PVMENV_Attach(&local->vm, "pvmdbg");
	PVMDBG_Init(&local->dbg, &local->vm);

	return &local->base;
}
