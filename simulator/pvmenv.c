#include "pvmenv.h"

static PVM_Word_t g_periph[PVMENV_PERIPH_WORDS] = {0};

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

/* SVC #2: pop two values, push their sum -- demonstrates reading multiple
 * arguments and pushing a real (non-status) result back for the caller. */
static int Svc_Add(PVM_State_s* s){
	PVM_Word_t a, b;
	int status = PVM_Pop(s, &b); /* last-pushed argument */

	if(status == ERRORCODE_NO_ERROR) status = PVM_Pop(s, &a); /* first-pushed argument */
	if(status == ERRORCODE_NO_ERROR) status = PVM_Push(s, (PVM_Word_t)(a + b));

	return status;
}

/* #1 is left empty rather than closed up, so that programs calling #2 keep
 * working; the VM answers a call to an empty slot with ERRORCODE_BAD_SVC. */
static const PVM_SvcFunc kSvcTable[] = {
	Svc_Tick,
	0,
	Svc_Add,
};

void PVMENV_Attach(PVM_State_s* state){
	PVM_UsrSect(state, g_periph, PVMENV_PERIPH_WORDS);
	PVM_RegisterSvc(state, kSvcTable, sizeof(kSvcTable) / sizeof(kSvcTable[0]));
}
