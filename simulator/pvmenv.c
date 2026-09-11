#include "pvmenv.h"

#include <stdio.h>

static PVM_Word_t  g_periph[8] = {0};
static const char* g_toolName  = "pvm";

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
		printf("%s: SVC print -> %d\n", g_toolName, value);

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

void PVMENV_Attach(PVM_State_s* state, const char* toolName){
	g_toolName = toolName;

	PVM_UsrSect(state, g_periph, sizeof(g_periph) / sizeof(PVM_Word_t));
	PVM_RegisterSvc(state, kSvcTable, sizeof(kSvcTable) / sizeof(kSvcTable[0]));
}
