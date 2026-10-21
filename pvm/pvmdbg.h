/**
 * int main(){
 *
 *   ...some code...
 *   PVM_Init(&vm, code, len, stack, stack_len, ram, ram_len);
 *   PVMDBG_Init();
 *
 *   while(1){
 *     stdin(buf, len);
 *     PVMDBG_Poll(buf, len);
 *     if(PVMDBG_IsAccessExecTick())
 *       PVM_ExecTick();
 *     stdout(buf, len);
 *   }
 * }
 *
 *
 *
 */

#ifndef PVMDBG_H_
#define PVMDBG_H_

#include "pvm.h"

#define PVMDBG_CFG_BRKP_SIZE   (4)
#define PVMDBG_SYNC (0x1F78DADA)

/* The one-instruction area EXEC_INSTR runs from, see PVMDbg_s::instr. The
 * longest instruction is 3 bytes (opcode plus a 16-bit operand); the rest is
 * HALT, so whatever the VM does after that instruction, it stops. */
#define PVMDBG_CFG_INSTR_SIZE (8)

/*
 * Every frame, request or reply, is the same 12 bytes, little-endian:
 *
 *   offset 0   uint32    sync   PVMDBG_SYNC
 *   offset 4   uint16    cmd    PVMDbgCmd_e
 *   offset 6   uint8[4]  data   command-specific, unused bytes zero
 *   offset 10  uint16    crc16  CCITT, over the 10 bytes before it
 *
 * Fixed rather than length-prefixed: a reader never has to trust a length
 * field from a stream that may be corrupt, and both ends need one 12-byte
 * buffer. The price is that what does not fit in 4 bytes is asked for a piece
 * at a time -- one stack word, one breakpoint slot, 4 bytes of code.
 */
#define PVMDBG_DATA_SIZE  (4)
#define PVMDBG_FRAME_SIZE (12)

/* What the buffer handed to PVMDBG_Poll must hold: one frame, which is both the
 * request coming in and the reply going out. */
#define PVMDBG_CFG_BUF_SIZE PVMDBG_FRAME_SIZE

typedef struct PVMDbg_s PVMDbg_s;
typedef struct PVMDbgFrame_s PVMDbgFrame_s;

enum PVMDbgMode_e{
  DBGMODE_STOPPED = 0, /* PVMDBG_IsAccessExecTick says no until told otherwise */
  DBGMODE_RUNNING = 1,
  DBGMODE_STEP    = 2, /* let one instruction through, then back to STOPPED */
};

/* Why the VM is sitting where it is, reported by STATE. "Stopped at 0x0034" on
 * its own leaves the host unable to tell a breakpoint from the end of the
 * program, which are rather different pieces of news. */
enum PVMDbgStop_e{
  DBGSTOP_NONE       = 0, /* not stopped */
  DBGSTOP_HOST       = 1, /* PAUSE or RESET asked for it */
  DBGSTOP_STEP       = 2, /* the single instruction STEP allows has been run */
  DBGSTOP_BREAKPOINT = 3,
  DBGSTOP_HALTED     = 4, /* PC is on a HALT: the program is over */
};

struct PVMDbg_s{
  PVM_State_s* state;
  uint8_t* brkp[PVMDBG_CFG_BRKP_SIZE];
  uint8_t  mode;   /* PVMDbgMode_e */
  uint8_t  reason; /* PVMDbgStop_e */
  uint8_t  resume; /* step over the breakpoint PC is sitting on -- see the .c */

  /* Where EXEC_INSTR puts the one instruction the host sent, PC pointing at
   * it, the rest HALT -- and nothing else is kept: the stack, RAM and the
   * program's code stay in the VM as they are. */
  uint8_t  instr[PVMDBG_CFG_INSTR_SIZE];
};

/* The layout above, for reference. Never cast a buffer to it: the buffer has
 * no alignment guarantee -- frames are read and written byte by byte. */
struct PVMDbgFrame_s{
  uint32_t sync;
  uint16_t cmd;
  uint8_t  data[PVMDBG_DATA_SIZE];
  uint16_t crc16;
};

/*
 * Command codes carried in PVMDbgFrame_s::cmd, with what goes in the 4 data
 * bytes each way (request -> reply; 16-bit values little-endian, unused bytes
 * zero). A reply carries the request's code, or DBGCMD_ERROR in its place.
 *
 * Values are written out explicitly and grouped by area: they travel between
 * two separately built binaries (host tool and firmware), so renumbering by
 * insertion would silently break whichever side wasn't rebuilt.
 */
enum PVMDbgCmd_e{
  DBGCMD_NONE       = 0x00, /* nothing pending -- PVMDBG_Poll saw no frame */
  DBGCMD_PING       = 0x01, /* -> -- anyone there, is the framing in sync */

  /* -> pc(2) mode(1) reason(1). pc is an offset into the program, or 0xFFFF
   * while PC is on the EXEC_INSTR area -- no program address is that. */
  DBGCMD_STATE      = 0x02,

  /* index(1) -> word(2) depth(2): the word `index` places below the top (0 is
   * the top), and how deep the stack is. ERROR for an index past the bottom.
   * One word a time: stepping a stack machine blind is no use, but a frame
   * holds one. */
  DBGCMD_READ_STACK = 0x03,

  /* addr(2) -> bytes(4) of bytecode from addr, HALT past the end of the
   * program; ERROR when addr itself is past it. From the device rather than a
   * local copy, so the host disassembles what is actually running -- and a
   * session over a wire needs no copy of the program at all. */
  DBGCMD_READ_CODE  = 0x04,

  /* which(1) -> two 16-bit sizes:
   *   which 0: stack_len, ram_len (words)
   *   which 1: code_len (bytes), usrsect_len (words)
   * Asked rather than assumed: the pools and the program are the firmware's to
   * size (see PVM_Init), and the host has to know where they end. */
  DBGCMD_MEM_INFO   = 0x05,

  /* -> sp(2) fp(2), as stack depths. */
  DBGCMD_REGS       = 0x06,

  /* addr(2) -> word(2): a word of data memory, addressed the way LOAD does --
   * RAM from 0, the user section from PVM_CFG_USRSECT_BGN -- which is also how
   * the assembler's listing gives the addresses of variables. ERROR for an
   * address that is neither. */
  DBGCMD_READ_DATA  = 0x07,

  /* index(2) -> word(2) depth(2): like READ_STACK, but counted from the bottom
   * of the stack and two bytes wide -- how the host reads a variable at a
   * known place in a frame (FP + offset) however deep the frame is. ERROR for
   * an index at or past the top. */
  DBGCMD_READ_STACK_AT = 0x08,

  /* Execution control. They decide what PVMDBG_IsAccessExecTick answers on the
   * following turns of the main loop rather than executing anything
   * themselves. No data either way. */
  DBGCMD_RUN        = 0x10, /* let ticks through until a breakpoint or PAUSE */
  DBGCMD_PAUSE      = 0x11, /* stop letting ticks through */
  DBGCMD_STEP       = 0x12, /* let exactly one tick through, then stop again */
  DBGCMD_RESET      = 0x13, /* PC back to 0 and stack emptied, still stopped */
  /* The debugger is leaving: breakpoints cleared and the program let go. If
   * PC was on the EXEC_INSTR area -- the session ran a program from the host --
   * PC, SP and FP go back to where PVM_Init put them first, since nothing of
   * the board's own program is left in them. Same as PVMDBG_Detach. */
  DBGCMD_DETACH     = 0x14,

  DBGCMD_BRKP_SET   = 0x20, /* addr(2) -> -- arm a breakpoint; ERROR past the end or table full */
  DBGCMD_BRKP_CLR   = 0x21, /* addr(2) -> -- disarm one; not armed is not an error */
  /* slot(1) -> addr(2) armed(1) slots(1): one slot of the table, and how many
   * there are, so the host can walk them. ERROR for a slot past the end. */
  DBGCMD_BRKP_LIST  = 0x22,

  /* op(1) lo(1) hi(1) len(1) -> status(1, signed) sp(2): one instruction of
   * len 1 or 3 bytes, executed on the spot inside PVMDBG_Poll. The area in
   * PVMDbg_s::instr is filled with HALT, the instruction written at its start,
   * PC pointed there and one PVM_ExecTick run; status is what it returned. PC
   * is left in the area afterwards, on a HALT -- the next EXEC_INSTR points it
   * at the start again, and DETACH or RESET hand it back to the program.
   *
   * This is the device half of running a program that lives on the host: the
   * host fetches, decodes and does all the control flow -- JMP/JZ/JNZ/CALL/
   * RET/HALT, which are refused here because their addresses mean nothing on
   * the device -- and the device does the work on its own stack, RAM,
   * peripherals and SVCs. Only while stopped. */
  DBGCMD_EXEC_INSTR = 0x34,

  /* Only ever sent by the device, in place of the command it could not carry
   * out (unknown code, address past the end of the program, breakpoint table
   * full). Without it a failed command is indistinguishable from one that
   * worked, which is a miserable thing to debug a debugger with. */
  DBGCMD_ERROR      = 0x7F,
};

void PVMDBG_Init(PVMDbg_s* dbg, PVM_State_s* state);
int  PVMDBG_IsAccessExecTick(PVMDbg_s* dbg);

/* What DETACH does, for firmware that can tell for itself that the debugger
 * has gone -- a closed connection, a cable it can sense. */
void PVMDBG_Detach(PVMDbg_s* dbg);

/* `buf` holds `*len` received bytes; on return it holds the reply and *len is
 * PVMDBG_FRAME_SIZE, or 0 when there is nothing to send. */
void PVMDBG_Poll(PVMDbg_s* dbg, char* buf, int* len);

/*
 * The frame codec, deliberately public: the host builds its requests with the
 * very same code the device builds its replies with, so the two ends cannot
 * drift apart the way two hand-written implementations of one wire format
 * always eventually do.
 *
 * PVMDBG_FrameBuild puts sync, cmd, up to PVMDBG_DATA_SIZE bytes of `data`
 * (zero-filled after `dataLen`; `data` may be null for none) and the CRC into
 * `buf`, and returns PVMDBG_FRAME_SIZE.
 *
 * PVMDBG_FrameParse returns 1 for a frame whose length, sync word and CRC all
 * check out, 0 otherwise; `data` comes back pointing into `buf`, and *dataLen
 * is always PVMDBG_DATA_SIZE.
 */
int PVMDBG_FrameBuild(char* buf, uint16_t cmd, const uint8_t* data, int dataLen);
int PVMDBG_FrameParse(const char* buf, int len, uint16_t* cmd,
                      const uint8_t** data, int* dataLen);

/*
 * Whether the `available` bytes at buf are the start of a frame. Returns
 * PVMDBG_FRAME_SIZE once a whole one is there, 0 while more bytes are still
 * needed, and -1 when the bytes at the front cannot begin a frame at all -- on
 * which the caller drops the first byte and asks again, which is how a stream
 * resynchronises after line noise or a half-sent frame. The sync word is what
 * is checked; the CRC is FrameParse's business.
 *
 * Whoever is reading from a serial port needs this at both ends: bytes arrive a
 * handful at a time with no regard for where frames begin or end.
 */
int PVMDBG_FrameLength(const char* buf, int available);

#endif //PVMDBG_H_
