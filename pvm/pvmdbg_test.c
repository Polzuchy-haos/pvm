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

/* Deliberately not the PVM_CFG_ defaults, so MEM_INFO is seen reporting the
 * pools it was actually handed rather than a compile-time constant. */
#define TEST_STACK_LEN (32)
#define TEST_RAM_LEN 	 (5)

static PVM_Word_t g_stack[TEST_STACK_LEN];
static PVM_Word_t g_ram[TEST_RAM_LEN];

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

  CHECK(PVM_Init(&vm, g_code, (int)sizeof(g_code), 0, TEST_STACK_LEN, g_ram, TEST_RAM_LEN)
          == ERRORCODE_BAD_CONFIG, "PVM_Init refuses a VM with no stack");
  CHECK(PVM_Init(&vm, g_code, (int)sizeof(g_code), g_stack, TEST_STACK_LEN, 0, TEST_RAM_LEN)
          == ERRORCODE_BAD_CONFIG, "and a RAM length with no RAM behind it");
  CHECK(PVM_Init(&vm, g_code, (int)sizeof(g_code), g_stack, TEST_STACK_LEN, g_ram, TEST_RAM_LEN)
          == ERRORCODE_NO_ERROR, "and accepts a pair of real pools");
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

  len = Exchange(&dbg, buf, DBGCMD_MEM_INFO, 0, 0);
  CHECK(ReplyCmd(buf) == DBGCMD_MEM_INFO, "MEM_INFO answers with MEM_INFO");
  CHECK(len == 9 + 4, "with two words of payload");
  CHECK(ReplyWord(buf, 0) == TEST_STACK_LEN, "the first is the stack pool's size in words");
  CHECK(ReplyWord(buf, 1) == TEST_RAM_LEN, "the second is the RAM pool's");

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

  /* ---- running uploaded code ---- */
  {
    /* PUSH 42; STORE 2; LOAD 2; PUSH 1; ADD -- writes RAM, leaves 43 behind, no HALT */
    static const uint8_t snippet[] = {
      0x00, 0x2A, 0x00, 0x0E, 0x02, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x01, 0x00, 0x04
    };
    uint8_t chunk[1 + 8];
    PVM_Word_t top = 0;

    PVM_Push(&vm, 77); /* something of the program's own, to be found intact afterwards */

    chunk[0] = PVMDBG_CFG_EXEC_SIZE - 2; chunk[1] = chunk[2] = chunk[3] = 0;
    Exchange(&dbg, buf, DBGCMD_EXEC_WRITE, chunk, 4);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "EXEC_WRITE past the end of the buffer is refused");

    /* Two pieces at two offsets, the way a host uploads anything bigger than a frame. */
    chunk[0] = 0; memcpy(chunk + 1, snippet, 8);
    Exchange(&dbg, buf, DBGCMD_EXEC_WRITE, chunk, 1 + 8);
    CHECK(ReplyCmd(buf) == DBGCMD_EXEC_WRITE, "EXEC_WRITE fills the buffer");
    chunk[0] = 8; memcpy(chunk + 1, snippet + 8, sizeof(snippet) - 8);
    Exchange(&dbg, buf, DBGCMD_EXEC_WRITE, chunk, 1 + (int)sizeof(snippet) - 8);

    arg[0] = (uint8_t)sizeof(snippet);
    Exchange(&dbg, buf, DBGCMD_EXEC_RUN, arg, 1);
    CHECK(ReplyCmd(buf) == DBGCMD_EXEC_RUN, "EXEC_RUN starts a snippet from a stopped VM");

    Exchange(&dbg, buf, DBGCMD_STATE, 0, 0);
    CHECK(ReplyWord(buf, 0) == 0, "STATE shows PC at the start of the snippet");
    CHECK(((const uint8_t*)buf)[PAYLOAD_AT + 8] == 1, "and says the VM is in a snippet");

    CHECK(RunLoop(&dbg, 100) == 5, "the snippet runs its five instructions and stops on the HALT after them");
    CHECK(g_ram[2] == 42, "what the snippet stored stays stored");
    CHECK(PcOf(&vm) == 7, "PC is back where the program was interrupted");
    CHECK(vm.code_ptr == (uint8_t*)g_code, "and so is the program's code");
    CHECK(PVM_Pop(&vm, &top) == ERRORCODE_NO_ERROR && top == 77 && vm.SP == vm.stack,
          "the program's stack is exactly as it was");

    Exchange(&dbg, buf, DBGCMD_STATE, 0, 0);
    CHECK(((const uint8_t*)buf)[PAYLOAD_AT + 7] == DBGSTOP_EXEC_DONE, "STATE says why it stopped");
    CHECK(((const uint8_t*)buf)[PAYLOAD_AT + 8] == 0, "and that the snippet is over");

    Exchange(&dbg, buf, DBGCMD_EXEC_STATUS, 0, 0);
    CHECK(((const uint8_t*)buf)[PAYLOAD_AT] == DBGEXEC_DONE, "EXEC_STATUS reports it done");
    CHECK(((const uint8_t*)buf)[PAYLOAD_AT + 1] == 1, "with the one word it left behind");
    CHECK(ReplyWord(buf, 1) == 43, "which is LOAD 2 + 1");

    /* JMP 0: a snippet that never finishes, which is what ABORT is for. */
    chunk[0] = 0; chunk[1] = 0x0A; chunk[2] = 0x00; chunk[3] = 0x00;
    Exchange(&dbg, buf, DBGCMD_EXEC_WRITE, chunk, 4);
    arg[0] = 3;
    Exchange(&dbg, buf, DBGCMD_EXEC_RUN, arg, 1);
    CHECK(RunLoop(&dbg, 50) == 50, "a looping snippet keeps the VM busy");

    Exchange(&dbg, buf, DBGCMD_EXEC_WRITE, chunk, 4);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "the buffer cannot be rewritten under a running snippet");
    Exchange(&dbg, buf, DBGCMD_EXEC_RUN, arg, 1);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "nor can a second snippet be started");

    arg[0] = 0; arg[1] = 0; arg[2] = 4;
    Exchange(&dbg, buf, DBGCMD_READ_CODE, arg, 3);
    CHECK(ListedCount(buf) == 4 && ((const uint8_t*)buf)[PAYLOAD_AT + 1] == 0x0A &&
          ((const uint8_t*)buf)[PAYLOAD_AT + 4] == INSTR_HALT,
          "READ_CODE shows the snippet, with the HALT the agent put after it");

    arg[0] = 3; arg[1] = 0;
    Exchange(&dbg, buf, DBGCMD_BRKP_SET, arg, 2);
    Exchange(&dbg, buf, DBGCMD_BRKP_LIST, 0, 0);
    CHECK(ListedCount(buf) == 1 && ListedAddr(buf, 0) == 3,
          "a breakpoint set during a snippet is still a program address");
    Exchange(&dbg, buf, DBGCMD_BRKP_CLR, arg, 2);

    Exchange(&dbg, buf, DBGCMD_EXEC_ABORT, 0, 0);
    CHECK(ReplyCmd(buf) == DBGCMD_EXEC_ABORT, "EXEC_ABORT drops it");
    CHECK(PcOf(&vm) == 7 && vm.SP == vm.stack, "and hands the VM back to the program");
    CHECK(PVMDBG_IsAccessExecTick(&dbg) == 0, "stopped");
    Exchange(&dbg, buf, DBGCMD_EXEC_STATUS, 0, 0);
    CHECK(((const uint8_t*)buf)[PAYLOAD_AT] == DBGEXEC_ABORTED &&
          ((const uint8_t*)buf)[PAYLOAD_AT + 1] == 0, "an aborted snippet leaves no result");

    Exchange(&dbg, buf, DBGCMD_RUN, 0, 0);
    arg[0] = 3;
    Exchange(&dbg, buf, DBGCMD_EXEC_RUN, arg, 1);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "a snippet is only started from a stopped VM");
    Exchange(&dbg, buf, DBGCMD_PAUSE, 0, 0);
  }

  /* ---- one instruction at a time, for a program that lives on the host ---- */
  {
    PVM_Word_t top = 0;

    arg[0] = INSTR_PUSH; arg[1] = 9; arg[2] = 0;
    len = Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 3);
    CHECK(ReplyCmd(buf) == DBGCMD_EXEC_INSTR && len == 9 + 5, "EXEC_INSTR runs PUSH and says how it went");
    CHECK((int8_t)((const uint8_t*)buf)[PAYLOAD_AT] == ERRORCODE_NO_ERROR, "status: no error");

    arg[1] = 4;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 3);
    arg[0] = INSTR_SUB;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 1);
    {
      const uint8_t* p = (const uint8_t*)buf + PAYLOAD_AT + 1;
      CHECK((p[0] | (p[1] << 8)) == 1, "and reports SP after it: 9 4 SUB leaves one word");
    }
    CHECK(PVM_Pop(&vm, &top) == ERRORCODE_NO_ERROR && top == 5, "which is 9 - 4, on the device's own stack");

    arg[0] = INSTR_ADD;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 1);
    CHECK((int8_t)((const uint8_t*)buf)[PAYLOAD_AT] == ERRORCODE_PAIR_STACK,
          "an execution error comes back as the status, not as silence");

    arg[0] = INSTR_JMP; arg[1] = 0; arg[2] = 0;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 3);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "control flow is the host's job, and is refused");
    CHECK(PcOf(&vm) == 7 && vm.code_ptr == (uint8_t*)g_code, "the program's own PC and code are untouched");

    Exchange(&dbg, buf, DBGCMD_RUN, 0, 0);
    arg[0] = INSTR_DUP;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 1);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "and nothing is executed behind a running program's back");
    Exchange(&dbg, buf, DBGCMD_PAUSE, 0, 0);
  }

  /* ---- the VM keeps to the pool it was given ---- */
  {
    int pushed = 0;

    while(PVM_Push(&vm, 1) == ERRORCODE_NO_ERROR && pushed <= TEST_STACK_LEN) pushed++;
    CHECK(pushed == TEST_STACK_LEN, "the stack overflows at the pool's length, not the default's");
  }

  printf("%d check(s), %d failed\n", g_checks, g_failed);
  if(g_failed == 0) printf("ALL OK\n");

  return g_failed == 0 ? 0 : 1;
}
