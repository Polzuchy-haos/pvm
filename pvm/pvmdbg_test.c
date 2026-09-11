/*
 * pvmdbg_test.c
 *
 * Drives pvmdbg.c the way the main loop in pvmdbg.h's sketch does -- hand it a
 * buffer holding one frame, read the reply out of the same buffer -- with no
 * serial port anywhere in sight. That is the whole reason PVMDBG_Poll takes a
 * buffer instead of talking to a UART itself.
 */
#include "pvmdbg.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond, what) do{ \
    g_checks++; \
    if(!(cond)){ g_failed++; printf("FAIL: %s (line %d)\n", (what), __LINE__); } \
  }while(0)

/* PUSH 5; PUSH 3; ADD; HALT -- ADD sits at address 6, deliberately not the last
 * instruction, so "stopped at a breakpoint" and "ran to the end" cannot be
 * confused with one another. */
static char g_code[] = {
  0x00, 0x05, 0x00, /* 0: PUSH 5 */
  0x00, 0x03, 0x00, /* 3: PUSH 3 */
  0x04, 						/* 6: ADD */
  0x13 							/* 7: HALT */
};

#define BRKP_AT_ADD (6)

static uint16_t ReplyCmd(const char* buf){
  const uint8_t* f = (const uint8_t*)buf;
  return (uint16_t)((uint16_t)f[4] | ((uint16_t)f[5] << 8));
}

#define PAYLOAD_AT (7) /* sync(4) + cmd(2) + len(1) */

static uint16_t ReplyWord(const char* buf, int index){
  const uint8_t* d = (const uint8_t*)buf + PAYLOAD_AT + 2 * index;
  return (uint16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
}

/* BRKP_LIST's payload is a one-byte count followed by the addresses, so these
 * are not on the even offsets ReplyWord assumes. */
static uint8_t 	ListedCount(const char* buf){ return ((const uint8_t*)buf)[PAYLOAD_AT]; }
static uint16_t ListedAddr(const char* buf, int index){
  const uint8_t* d = (const uint8_t*)buf + PAYLOAD_AT + 1 + 2 * index;
  return (uint16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
}

/* One command in, one reply out -- exactly the stdin/Poll/stdout shape from the
 * header's sketch. Returns the reply length (0 when there is nothing to send). */
static int Exchange(PVMDbg_s* dbg, char* buf, uint16_t cmd, const uint8_t* data, int dataLen){
  int len = PVMDBG_FrameBuild(buf, cmd, data, dataLen);
  PVMDBG_Poll(dbg, buf, &len);
  return len;
}

/* The main loop, minus the I/O: tick for as long as the debugger allows it. */
static int RunLoop(PVMDbg_s* dbg, int maxTicks){
  int ticks = 0;

  while(ticks < maxTicks && PVMDBG_IsAccessExecTick(dbg)){
    if(PVM_ExecTick(dbg->state) != ERRORCODE_NO_ERROR) break;
    ticks++;
  }

  return ticks;
}

static int PcOf(const PVM_State_s* vm){
  return (int)(vm->PC - vm->code_ptr);
}

int main(void){
  PVM_State_s vm;
  PVMDbg_s 		dbg;
  char 				buf[PVMDBG_CFG_BUF_SIZE];
  uint8_t 		arg[3];
  int 				len;

  PVM_Init(&vm, g_code, (int)sizeof(g_code));
  PVMDBG_Init(&dbg, &vm);

  /* ---- framing ---- */

  len = Exchange(&dbg, buf, DBGCMD_PING, 0, 0);
  CHECK(len == 9, "PING is answered with a bare header plus CRC");
  CHECK(ReplyCmd(buf) == DBGCMD_PING, "the reply echoes the command it answers");

  len = PVMDBG_FrameBuild(buf, DBGCMD_PING, 0, 0);
  buf[4] ^= 0xFF; /* corrupt the command, which the CRC covers */
  PVMDBG_Poll(&dbg, buf, &len);
  CHECK(len == 0, "a frame that fails its CRC is answered with silence");

  len = Exchange(&dbg, buf, 0x1234, 0, 0);
  CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "an unknown command code comes back as ERROR");

  /* ---- finding a frame in a stream of bytes ---- */
  {
    char stream[64];
    int 	whole = PVMDBG_FrameBuild(buf, DBGCMD_PING, 0, 0);
    int 	found = 0;

    CHECK(PVMDBG_FrameLength(buf, 0) == 0, "nothing received yet means 'keep waiting'");
    CHECK(PVMDBG_FrameLength(buf, 3) == 0, "so does half a sync word");
    CHECK(PVMDBG_FrameLength(buf, whole - 1) == 0, "and so does a frame one byte short");
    CHECK(PVMDBG_FrameLength(buf, whole) == whole, "a complete frame reports its length");
    CHECK(PVMDBG_FrameLength(buf, whole + 5) == whole,
          "trailing bytes belong to the next frame, not this one");

    /* Line noise ahead of a real frame: the caller drops a byte at a time until
     * the frame turns up, which is the whole resynchronisation strategy. */
    stream[0] = 0x11; stream[1] = 0x22; stream[2] = 0x33;
    memcpy(stream + 3, buf, (size_t)whole);

    for(int start = 0; start < 3 + whole; start++){
      int n = PVMDBG_FrameLength(stream + start, 3 + whole - start);
      if(n > 0){ found = (start == 3 && n == whole); break; }
      CHECK(n == -1, "a byte that cannot start a frame is rejected rather than awaited");
    }
    CHECK(found, "sliding one byte at a time finds the frame behind the noise");

    buf[6] = (uint8_t)(PVMDBG_CFG_DATA_MAX + 1);
    CHECK(PVMDBG_FrameLength(buf, whole) == -1,
          "a payload longer than this build can hold is rejected, not awaited forever");
  }

  /* ---- execution control ---- */

  Exchange(&dbg, buf, DBGCMD_PAUSE, 0, 0);
  CHECK(PVMDBG_IsAccessExecTick(&dbg) == 0, "PAUSE stops ticks getting through");
  CHECK(PcOf(&vm) == 0, "and nothing ran while stopped");

  Exchange(&dbg, buf, DBGCMD_STEP, 0, 0);
  CHECK(RunLoop(&dbg, 10) == 1, "STEP lets exactly one instruction through");
  CHECK(PcOf(&vm) == 3, "which is the one PC was sitting on");

  Exchange(&dbg, buf, DBGCMD_STATE, 0, 0);
  CHECK(ReplyCmd(buf) == DBGCMD_STATE, "STATE answers with STATE");
  CHECK(ReplyWord(buf, 0) == 3, "STATE reports PC as a code offset");
  CHECK(ReplyWord(buf, 1) == 1, "STATE reports SP as a stack depth");

  Exchange(&dbg, buf, DBGCMD_RESET, 0, 0);
  CHECK(PcOf(&vm) == 0, "RESET rewinds PC to the start");
  CHECK(vm.SP == vm.stack, "RESET empties the stack");
  CHECK(PVMDBG_IsAccessExecTick(&dbg) == 0, "RESET leaves the VM stopped");

  /* ---- breakpoints ---- */

  arg[0] = 100; arg[1] = 0; /* past the end of an 8-byte program */
  Exchange(&dbg, buf, DBGCMD_BRKP_SET, arg, 2);
  CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "a breakpoint past the end of the code is refused");

  arg[0] = BRKP_AT_ADD; arg[1] = 0;
  Exchange(&dbg, buf, DBGCMD_BRKP_SET, arg, 2);
  CHECK(ReplyCmd(buf) == DBGCMD_BRKP_SET, "a breakpoint inside the code is accepted");

  Exchange(&dbg, buf, DBGCMD_BRKP_SET, arg, 2);
  Exchange(&dbg, buf, DBGCMD_BRKP_LIST, 0, 0);
  CHECK(ListedCount(buf) == 1, "arming the same address twice still lists it once");
  CHECK(ListedAddr(buf, 0) == BRKP_AT_ADD, "and lists the address that was armed");

  /* ---- the whole point: run, stop, and get going again ---- */

  Exchange(&dbg, buf, DBGCMD_RUN, 0, 0);
  RunLoop(&dbg, 100);
  CHECK(PcOf(&vm) == BRKP_AT_ADD, "RUN stops on the breakpoint");
  CHECK(PVMDBG_IsAccessExecTick(&dbg) == 0, "and stays stopped there");

  arg[0] = 4; /* more than is actually on the stack */
  Exchange(&dbg, buf, DBGCMD_READ_STACK, arg, 1);
  CHECK(ListedCount(buf) == 2, "READ_STACK returns what is there, not what was asked for");
  CHECK((int16_t)ListedAddr(buf, 0) == 3, "top of stack first");
  CHECK((int16_t)ListedAddr(buf, 1) == 5, "then the one below it");

  /* ---- reading bytecode back off the device ---- */

  arg[0] = 6; arg[1] = 0; arg[2] = 8; /* from ADD, more bytes than remain */
  Exchange(&dbg, buf, DBGCMD_READ_CODE, arg, 3);
  CHECK(ListedCount(buf) == 2, "READ_CODE stops at the end of the program");
  CHECK(((const uint8_t*)buf)[PAYLOAD_AT + 1] == 0x04, "and returns the actual opcodes (ADD)");
  CHECK(((const uint8_t*)buf)[PAYLOAD_AT + 2] == 0x13, "followed by HALT");

  arg[0] = 0; arg[1] = 0; arg[2] = 3;
  Exchange(&dbg, buf, DBGCMD_READ_CODE, arg, 3);
  CHECK(ListedCount(buf) == 3, "asking for fewer bytes than exist returns just those");

  arg[0] = 200; arg[1] = 0; arg[2] = 4; /* past the end of an 8-byte program */
  Exchange(&dbg, buf, DBGCMD_READ_CODE, arg, 3);
  CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "reading past the end of the code is refused");

  arg[0] = BRKP_AT_ADD; arg[1] = 0; /* restore the breakpoint argument */

  /* Sitting on an armed breakpoint is the case that traps a naive
   * implementation: RUN has to get past the instruction under PC instead of
   * re-reporting the breakpoint it already reported. */
  Exchange(&dbg, buf, DBGCMD_RUN, 0, 0);
  RunLoop(&dbg, 100);
  CHECK(PcOf(&vm) == 7, "RUN from an armed breakpoint makes progress rather than sticking");

  {
    PVM_Word_t top = 0;
    CHECK(PVM_Pop(&vm, &top) == ERRORCODE_NO_ERROR && top == 8, "and ADD really did run (5+3)");
  }

  Exchange(&dbg, buf, DBGCMD_BRKP_CLR, arg, 2);
  Exchange(&dbg, buf, DBGCMD_BRKP_LIST, 0, 0);
  CHECK(ListedCount(buf) == 0, "CLR disarms the breakpoint");

  Exchange(&dbg, buf, DBGCMD_BRKP_CLR, arg, 2);
  CHECK(ReplyCmd(buf) == DBGCMD_BRKP_CLR, "clearing an address twice is not an error");

  printf("%d check(s), %d failed\n", g_checks, g_failed);
  if(g_failed == 0) printf("ALL OK\n");

  return g_failed == 0 ? 0 : 1;
}
