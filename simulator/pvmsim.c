/*
 ============================================================================
 Name        : pvmsim.c
 Description : Runs a compiled PVM bytecode file to completion (HALT/error),
               tracing every instruction via PVM_PrintInstr.
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>

#include "pvm.h"

#ifndef PVM_DEFAULT_BYTECODE
#define PVM_DEFAULT_BYTECODE "bytecode_listing.bin"
#endif

#define MAX_STEPS (100000)

PVM_State_s state;
PVM_Word_t  periph[8] = {0};

/*
 * SVC #0: "system tick" -- returns a free-running counter, no arguments.
 * Backs lib/time.hlc's Time_GetTick()/Time_Elapsed(). On real hardware this
 * would read an actual RTC/systick peripheral; here it's a synthetic counter
 * that (a) starts six ticks from the 16-bit wraparound boundary and
 * (b) advances by a small, fixed step per call, purely so a short demo
 * program can deterministically exercise Time_Elapsed()'s wraparound
 * handling without waiting around for a real 18-hour period to roll over.
 */
static PVM_Word_t g_ticks = -6; /* bit pattern of unsigned 65530, i.e. "6 ticks before wrap" */

static int Svc_Tick(PVM_State_s* s){
	PVM_Word_t now = g_ticks;
	g_ticks += 10;
	return PVM_Push(s, now);
}

/* SVC #1: pop one value and print it -- a minimal demo of calling into C
 * (a real firmware might expose things like UART output or flash access this way). */
static int Svc_Print(PVM_State_s* s){
	PVM_Word_t value;
	int status = PVM_Pop(s, &value);

	if(status == ERRORCODE_NO_ERROR)
		printf("pvmsim: SVC print -> %d\n", value);

	return status;
}

/* SVC #2: pop two values, push their sum -- demonstrates reading multiple
 * arguments and pushing a real (non-status) result back for the caller. */
static int Svc_Add(PVM_State_s* s){
	PVM_Word_t a, b;
	int status = PVM_Pop(s, &b); /* last-pushed argument */

	if(status == ERRORCODE_NO_ERROR) status = PVM_Pop(s, &a); /* first-pushed argument */
	if(status == ERRORCODE_NO_ERROR) status = PVM_Push(s, (PVM_Word_t)(a + b));

	return status;
}

static const PVM_SvcFunc kSvcTable[] = {
	Svc_Tick,
	Svc_Print,
	Svc_Add,
};

static char* LoadFile(const char* path, int* out_len){
	FILE* f = fopen(path, "rb");
	long size;
	char* buf;

	if(!f){
		fprintf(stderr, "pvmsim: cannot open '%s'\n", path);
		return 0;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = malloc(size);
	fread(buf, 1, size, f);
	fclose(f);

	*out_len = (int)size;
	return buf;
}

int main(int argc, char** argv){
	const char* path = (argc > 1) ? argv[1] : PVM_DEFAULT_BYTECODE;
	int len = 0;
	char* code = LoadFile(path, &len);
	int step;

	if(!code)
		return EXIT_FAILURE;

	PVM_Init(&state, code, len);
	PVM_UsrSect(&state, periph, sizeof(periph) / sizeof(PVM_Word_t));
	PVM_RegisterSvc(&state, kSvcTable, sizeof(kSvcTable) / sizeof(kSvcTable[0]));

	for(step = 0; step < MAX_STEPS; step++){
		int status;

		PVM_PrintInstr(&state);
		fflush(stdout);

		status = PVM_ExecTick(&state);
		if(status == PVM_STATUS_HALTED){
			printf("pvmsim: halted\n");
			break;
		}
		if(status < 0){
			fprintf(stderr, "pvmsim: execution error %d at step %d\n", status, step);
			free(code);
			return EXIT_FAILURE;
		}
	}

	free(code);
	return EXIT_SUCCESS;
}
