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
  INSTR_PUSH, 0x05, 0x00, /* 0: PUSH 5 */
  INSTR_PUSH, 0x03, 0x00, /* 3: PUSH 3 */
  INSTR_ADD, 							/* 6: ADD */
  INSTR_HALT 							/* 7: HALT */
};

#define BRKP_AT_ADD (6)

/* Deliberately not the PVM_CFG_ defaults, so MEM_INFO is seen reporting the
 * pools it was actually handed rather than a compile-time constant. */
#define TEST_STACK_LEN 	 (32)
#define TEST_RAM_LEN 		 (5)
#define TEST_USRSECT_LEN (3)

static PVM_Word_t g_stack[TEST_STACK_LEN];
static PVM_Word_t g_ram[TEST_RAM_LEN];
static PVM_Word_t g_usrsect[TEST_USRSECT_LEN];

#define DATA_AT (6) /* sync(4) + cmd(2) */

static uint16_t ReplyCmd(const char* buf){
  const uint8_t* f = (const uint8_t*)buf;
  return (uint16_t)((uint16_t)f[4] | ((uint16_t)f[5] << 8));
}

static uint8_t ReplyByte(const char* buf, int offset){
  return ((const uint8_t*)buf)[DATA_AT + offset];
}

/* The 16-bit value at `offset` bytes into the reply's data. */
static uint16_t ReplyWord(const char* buf, int offset){
  const uint8_t* d = (const uint8_t*)buf + DATA_AT + offset;
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
  uint8_t 		arg[PVMDBG_DATA_SIZE];
  int 				len;

  CHECK(PVM_Init(&vm, g_code, (int)sizeof(g_code), 0, TEST_STACK_LEN, g_ram, TEST_RAM_LEN)
          == ERRORCODE_BAD_CONFIG, "PVM_Init refuses a VM with no stack");
  CHECK(PVM_Init(&vm, g_code, (int)sizeof(g_code), g_stack, TEST_STACK_LEN, 0, TEST_RAM_LEN)
          == ERRORCODE_BAD_CONFIG, "and a RAM length with no RAM behind it");
  CHECK(PVM_Init(&vm, g_code, (int)sizeof(g_code), g_stack, TEST_STACK_LEN, g_ram, TEST_RAM_LEN)
          == ERRORCODE_NO_ERROR, "and accepts a pair of real pools");
  PVM_UsrSect(&vm, g_usrsect, TEST_USRSECT_LEN);
  PVM_RegisterSvc(&vm, 0, 0);
  PVMDBG_Init(&dbg, &vm);

  /* ---- framing ---- */

  CHECK(PVMDBG_CFG_BUF_SIZE == 12 && PVMDBG_DATA_SIZE == 4, "a frame is 12 bytes, 4 of them data");

  len = Exchange(&dbg, buf, DBGCMD_PING, 0, 0);
  CHECK(len == PVMDBG_FRAME_SIZE, "every reply is one fixed-size frame");
  CHECK(ReplyCmd(buf) == DBGCMD_PING, "the reply echoes the command it answers");
  CHECK(ReplyWord(buf, 0) == 0 && ReplyWord(buf, 2) == 0, "and data it does not use is zero");

  len = PVMDBG_FrameBuild(buf, DBGCMD_PING, 0, 0);
  buf[4] ^= 0xFF; /* corrupt the command, which the CRC covers */
  PVMDBG_Poll(&dbg, buf, &len);
  CHECK(len == 0, "a frame that fails its CRC is answered with silence");

  len = PVMDBG_FrameBuild(buf, DBGCMD_PING, 0, 0);
  len--;
  PVMDBG_Poll(&dbg, buf, &len);
  CHECK(len == 0, "so is a frame a byte short");

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
  }

  /* ---- execution control and state ---- */

  Exchange(&dbg, buf, DBGCMD_PAUSE, 0, 0);
  CHECK(PVMDBG_IsAccessExecTick(&dbg) == 0, "PAUSE stops ticks getting through");
  CHECK(PcOf(&vm) == 0, "and nothing ran while stopped");

  Exchange(&dbg, buf, DBGCMD_STEP, 0, 0);
  CHECK(RunLoop(&dbg, 10) == 1, "STEP lets exactly one instruction through");
  CHECK(PcOf(&vm) == 3, "which is the one PC was sitting on");

  Exchange(&dbg, buf, DBGCMD_STATE, 0, 0);
  CHECK(ReplyCmd(buf) == DBGCMD_STATE, "STATE answers with STATE");
  CHECK(ReplyWord(buf, 0) == 3, "STATE reports PC as a code offset");
  CHECK(ReplyByte(buf, 2) == DBGMODE_STOPPED && ReplyByte(buf, 3) == DBGSTOP_STEP,
        "and the mode, and why it stopped");

  Exchange(&dbg, buf, DBGCMD_REGS, 0, 0);
  CHECK(ReplyCmd(buf) == DBGCMD_REGS && ReplyWord(buf, 0) == 1 && ReplyWord(buf, 2) == 0,
        "REGS reports SP and FP as stack depths");

  /* ---- memory ---- */

  arg[0] = 0;
  Exchange(&dbg, buf, DBGCMD_MEM_INFO, arg, 1);
  CHECK(ReplyCmd(buf) == DBGCMD_MEM_INFO, "MEM_INFO answers with MEM_INFO");
  CHECK(ReplyWord(buf, 0) == TEST_STACK_LEN && ReplyWord(buf, 2) == TEST_RAM_LEN,
        "MEM_INFO 0 gives the stack and RAM pools it was handed");

  arg[0] = 1;
  Exchange(&dbg, buf, DBGCMD_MEM_INFO, arg, 1);
  CHECK(ReplyWord(buf, 0) == sizeof(g_code) && ReplyWord(buf, 2) == TEST_USRSECT_LEN,
        "MEM_INFO 1 gives the program's length and the user section's");

  arg[0] = 2;
  Exchange(&dbg, buf, DBGCMD_MEM_INFO, arg, 1);
  CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "and anything else is refused");

  g_ram[4] 		 = -1234;
  g_usrsect[2] = 0x55AA;
  arg[0] = 4; arg[1] = 0;
  Exchange(&dbg, buf, DBGCMD_READ_DATA, arg, 2);
  CHECK(ReplyCmd(buf) == DBGCMD_READ_DATA && (int16_t)ReplyWord(buf, 0) == -1234,
        "READ_DATA reads a RAM word by its address");
  arg[0] = (uint8_t)((PVM_CFG_USRSECT_BGN + 2) & 0xFF); arg[1] = (uint8_t)((PVM_CFG_USRSECT_BGN + 2) >> 8);
  Exchange(&dbg, buf, DBGCMD_READ_DATA, arg, 2);
  CHECK(ReplyWord(buf, 0) == 0x55AA, "and a user-section word by its, as LOAD does");
  arg[0] = TEST_RAM_LEN; arg[1] = 0;
  Exchange(&dbg, buf, DBGCMD_READ_DATA, arg, 2);
  CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "an address past RAM is refused");
  arg[0] = (uint8_t)((PVM_CFG_USRSECT_BGN + TEST_USRSECT_LEN) & 0xFF);
  arg[1] = (uint8_t)((PVM_CFG_USRSECT_BGN + TEST_USRSECT_LEN) >> 8);
  Exchange(&dbg, buf, DBGCMD_READ_DATA, arg, 2);
  CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "and so is one past the user section");

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

  {
    int armed = 0, at = -1, slots = 0;

    for(int slot = 0; ; slot++){
      arg[0] = (uint8_t)slot;
      Exchange(&dbg, buf, DBGCMD_BRKP_LIST, arg, 1);
      if(ReplyCmd(buf) != DBGCMD_BRKP_LIST) break;
      slots = ReplyByte(buf, 3);
      if(ReplyByte(buf, 2)){ armed++; at = ReplyWord(buf, 0); }
    }
    CHECK(slots == PVMDBG_CFG_BRKP_SIZE, "BRKP_LIST says how many slots there are to walk");
    CHECK(armed == 1 && at == BRKP_AT_ADD, "arming the same address twice still lists it once");
  }

  /* ---- the whole point: run, stop, and get going again ---- */

  Exchange(&dbg, buf, DBGCMD_RUN, 0, 0);
  RunLoop(&dbg, 100);
  CHECK(PcOf(&vm) == BRKP_AT_ADD, "RUN stops on the breakpoint");
  CHECK(PVMDBG_IsAccessExecTick(&dbg) == 0, "and stays stopped there");

  arg[0] = 0;
  Exchange(&dbg, buf, DBGCMD_READ_STACK, arg, 1);
  CHECK((int16_t)ReplyWord(buf, 0) == 3 && ReplyWord(buf, 2) == 2, "READ_STACK 0 is the top, with the depth");
  arg[0] = 1;
  Exchange(&dbg, buf, DBGCMD_READ_STACK, arg, 1);
  CHECK((int16_t)ReplyWord(buf, 0) == 5, "READ_STACK 1 the one below it");
  arg[0] = 2;
  Exchange(&dbg, buf, DBGCMD_READ_STACK, arg, 1);
  CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "and past the bottom is refused");

  /* ---- reading bytecode back off the device ---- */

  arg[0] = 6; arg[1] = 0; /* ADD, HALT, then past the end */
  Exchange(&dbg, buf, DBGCMD_READ_CODE, arg, 2);
  CHECK(ReplyByte(buf, 0) == INSTR_ADD && ReplyByte(buf, 1) == INSTR_HALT, "READ_CODE returns the actual opcodes");
  CHECK(ReplyByte(buf, 2) == INSTR_HALT && ReplyByte(buf, 3) == INSTR_HALT,
        "with HALT for the bytes past the end of the program");

  arg[0] = 0; arg[1] = 0;
  Exchange(&dbg, buf, DBGCMD_READ_CODE, arg, 2);
  CHECK(ReplyByte(buf, 0) == INSTR_PUSH && ReplyByte(buf, 1) == 0x05 && ReplyByte(buf, 3) == INSTR_PUSH,
        "four bytes at a time from where it was asked");

  arg[0] = 8; arg[1] = 0;
  Exchange(&dbg, buf, DBGCMD_READ_CODE, arg, 2);
  CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "starting past the end of the code is refused");

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

  arg[0] = BRKP_AT_ADD; arg[1] = 0;
  Exchange(&dbg, buf, DBGCMD_BRKP_CLR, arg, 2);
  arg[0] = 0;
  Exchange(&dbg, buf, DBGCMD_BRKP_LIST, arg, 1);
  CHECK(ReplyByte(buf, 2) == 0, "CLR disarms the breakpoint");

  arg[0] = BRKP_AT_ADD; arg[1] = 0;
  Exchange(&dbg, buf, DBGCMD_BRKP_CLR, arg, 2);
  CHECK(ReplyCmd(buf) == DBGCMD_BRKP_CLR, "clearing an address twice is not an error");

  /* ---- one instruction at a time, for a program that lives on the host ---- */
  {
    PVM_Word_t top 		 = 0;
    int 			 allHalt = 1;

    for(int i = 0; i < PVMDBG_CFG_INSTR_SIZE; i++) allHalt &= (dbg.instr[i] == INSTR_HALT);
    CHECK(allHalt, "the instruction area starts out as nothing but HALT");

    arg[0] = INSTR_PUSH; arg[1] = 9; arg[2] = 0; arg[3] = 3;
    len = Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 4);
    CHECK(ReplyCmd(buf) == DBGCMD_EXEC_INSTR, "EXEC_INSTR runs PUSH and says how it went");
    CHECK((int8_t)ReplyByte(buf, 0) == ERRORCODE_NO_ERROR, "status: no error");
    CHECK(ReplyWord(buf, 1) == 1, "and SP after it");
    CHECK(vm.PC == dbg.instr + 3 && *vm.PC == INSTR_HALT, "PC is left on the HALT that follows the instruction");
    CHECK(vm.code_ptr == (uint8_t*)g_code, "while code_ptr stays the program's");

    Exchange(&dbg, buf, DBGCMD_STATE, 0, 0);
    CHECK(ReplyWord(buf, 0) == 0xFFFF, "STATE reports a PC in the area as no program address");

    arg[1] = 4;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 4);
    /* A 1-byte instruction whose frame still carries the last operand: the
     * length is what says those bytes are not part of it. */
    arg[0] = INSTR_SUB; arg[3] = 1;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 4);
    CHECK(dbg.instr[0] == INSTR_SUB && dbg.instr[1] == INSTR_HALT && dbg.instr[2] == INSTR_HALT,
          "the area is all HALT again behind each instruction, whatever the frame carried");
    CHECK(ReplyWord(buf, 1) == 1, "9 4 SUB leaves one word");
    CHECK(PVM_Pop(&vm, &top) == ERRORCODE_NO_ERROR && top == 5, "which is 9 - 4, on the device's own stack");

    arg[0] = INSTR_ADD; arg[3] = 1;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 4);
    CHECK((int8_t)ReplyByte(buf, 0) == ERRORCODE_PAIR_STACK,
          "an execution error comes back as the status, not as silence");

    arg[0] = INSTR_DUP; arg[3] = 2;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 4);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "a length that is no instruction's is refused");

    arg[0] = INSTR_JMP; arg[1] = 0; arg[2] = 0; arg[3] = 3;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 4);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "control flow is the host's job, and is refused");

    /* A failed instruction leaves PC on itself, as on any VM; one that works
     * leaves it on the HALT after it, which is what the next check is about. */
    arg[0] = INSTR_PUSH; arg[1] = 1; arg[2] = 0; arg[3] = 3;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 4);

    Exchange(&dbg, buf, DBGCMD_RUN, 0, 0);
    arg[0] = INSTR_DUP; arg[3] = 1;
    Exchange(&dbg, buf, DBGCMD_EXEC_INSTR, arg, 4);
    CHECK(ReplyCmd(buf) == DBGCMD_ERROR, "nothing is executed behind a running VM's back");
    CHECK(RunLoop(&dbg, 10) == 0 && dbg.reason == DBGSTOP_HALTED,
          "and a RUN with PC in the area stops at once, on its HALT");
    PVM_Pop(&vm, &top);
  }

  /* ---- the debugger leaving ---- */
  {
    PVM_Word_t top = 0;

    PVM_Push(&vm, 11); /* what the host's code left on the stack */
    arg[0] = BRKP_AT_ADD; arg[1] = 0;
    Exchange(&dbg, buf, DBGCMD_BRKP_SET, arg, 2);

    Exchange(&dbg, buf, DBGCMD_DETACH, 0, 0);
    CHECK(ReplyCmd(buf) == DBGCMD_DETACH, "DETACH is answered");
    CHECK(PcOf(&vm) == 0 && vm.SP == vm.stack && vm.FP == vm.stack,
          "after code from the host, PC, SP and FP are back where PVM_Init put them");
    arg[0] = 0;
    Exchange(&dbg, buf, DBGCMD_BRKP_LIST, arg, 1);
    CHECK(ReplyByte(buf, 2) == 0, "the breakpoints are cleared");
    CHECK(RunLoop(&dbg, 100) == 3 && PcOf(&vm) == 7, "and the program runs, from the start, to its end");
    CHECK(PVM_Pop(&vm, &top) == ERRORCODE_NO_ERROR && top == 8, "computing what it always does");

    Exchange(&dbg, buf, DBGCMD_RESET, 0, 0);
    Exchange(&dbg, buf, DBGCMD_STEP, 0, 0);
    RunLoop(&dbg, 10);
    Exchange(&dbg, buf, DBGCMD_DETACH, 0, 0);
    CHECK(PcOf(&vm) == 3 && vm.SP == vm.stack + 1, "a program's own PC and stack are left where they are");
    CHECK(RunLoop(&dbg, 100) == 2 && PcOf(&vm) == 7, "and it carries on from there");
    PVM_Pop(&vm, &top);
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
