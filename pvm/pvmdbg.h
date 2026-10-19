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

/* Most stack words READ_STACK will hand back in one frame. Capped rather than
 * unbounded because the reply has to fit in the caller's buffer, whose size is
 * fixed below -- a host that wants more asks again. */
#define PVMDBG_CFG_STACK_READ_MAX (8)

/* Most code bytes READ_CODE hands back at once -- enough for a useful window of
 * instructions around the program counter (5 to 16 of them, depending on how
 * many carry an operand), and chosen so the reply still fits the frame the
 * other commands already need. */
#define PVMDBG_CFG_CODE_READ_MAX (16)

/* The longest frame either side can send: the 6-byte header, the biggest
 * payload (a count plus one 16-bit item per slot, for whichever of BRKP_LIST
 * and READ_STACK has more slots) and the trailing CRC. The buffer handed to
 * PVMDBG_Poll has to be at least this big -- it carries the reply as well as
 * the request. */
#define PVMDBG_CFG_ITEM_MAX (PVMDBG_CFG_BRKP_SIZE > PVMDBG_CFG_STACK_READ_MAX ? \
                             PVMDBG_CFG_BRKP_SIZE : PVMDBG_CFG_STACK_READ_MAX)
#define PVMDBG_CFG_DATA_MAX (1 + (2 * PVMDBG_CFG_ITEM_MAX > PVMDBG_CFG_CODE_READ_MAX ? \
                                  2 * PVMDBG_CFG_ITEM_MAX : PVMDBG_CFG_CODE_READ_MAX))
#define PVMDBG_CFG_BUF_SIZE (7 + PVMDBG_CFG_DATA_MAX + 2)

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
};

/*
 * `len` counts the payload bytes only. It is what makes the frame findable in a
 * byte stream at all: a serial port hands over whatever has arrived so far, and
 * without a length there is no way to tell "the frame ends here" from "the rest
 * has not turned up yet" -- see PVMDBG_FrameLength.
 */
struct PVMDbgFrame_s{
  uint32_t sync;
  uint16_t cmd;
  uint8_t  len;
  uint8_t  data[];
  //uint16_t crc16; end item
};

/*
 * Command codes carried in PVMDbgFrame_s::cmd. Values are written out
 * explicitly and grouped by area: they travel between two separately built
 * binaries (host tool and firmware), so renumbering by insertion would silently
 * break whichever side wasn't rebuilt.
 */
enum PVMDbgCmd_e{
  DBGCMD_NONE      = 0x00, /* nothing pending -- PVMDBG_Poll saw no frame */
  DBGCMD_PING      = 0x01, /* anyone there, and is the framing still in sync */
  DBGCMD_STATE     = 0x02, /* PC/SP/FP, plus whether the VM is currently stopped */
  DBGCMD_READ_STACK= 0x03, /* the top N words -- stepping a stack machine blind is no use */
  /* Bytecode straight from the device, so the host can disassemble what is
   * actually running rather than trust a local copy that may be out of date --
   * and so a session over a wire needs no copy of the program at all. */
  DBGCMD_READ_CODE = 0x04,
  /* How big this board's stack and RAM pools are, in words. Asked rather than
   * assumed because the pools are the firmware's to size (see PVM_Init): without
   * it the host cannot tell how close SP is to overflowing, or whether the
   * program it is debugging was even built for this much RAM. */
  DBGCMD_MEM_INFO  = 0x05,

  /* Execution control. All four work the same way: they decide what
   * PVMDBG_IsAccessExecTick answers on the following turns of the main loop,
   * rather than executing anything themselves. */
  DBGCMD_RUN       = 0x10, /* let ticks through until a breakpoint or PAUSE */
  DBGCMD_PAUSE     = 0x11, /* stop letting ticks through */
  DBGCMD_STEP      = 0x12, /* let exactly one tick through, then stop again */
  DBGCMD_RESET     = 0x13, /* PC back to 0 and stack emptied, still stopped */

  DBGCMD_BRKP_SET  = 0x20, /* arm a breakpoint at a code address */
  DBGCMD_BRKP_CLR  = 0x21, /* disarm one */
  DBGCMD_BRKP_LIST = 0x22, /* which addresses are armed right now */

  /* Only ever sent by the device, in place of the command it could not carry
   * out (unknown code, address past the end of the program, breakpoint table
   * full). Without it a failed command is indistinguishable from one that
   * worked, which is a miserable thing to debug a debugger with. */
  DBGCMD_ERROR     = 0x7F,
};

void PVMDBG_Init(PVMDbg_s* dbg, PVM_State_s* state);
int  PVMDBG_IsAccessExecTick(PVMDbg_s* dbg);
void PVMDBG_Poll(PVMDbg_s* dbg, char* buf, int* len);

/*
 * The frame codec, deliberately public: the host builds its requests with the
 * very same code the device builds its replies with, so the two ends cannot
 * drift apart the way two hand-written implementations of one wire format
 * always eventually do.
 *
 * PVMDBG_FrameBuild fills sync/cmd/CRC around `dataLen` payload bytes and
 * returns the whole frame's length. `data` may be null when the payload has
 * already been written into the buffer at offset 6, which is how the device
 * replies without needing a second buffer to copy from.
 *
 * PVMDBG_FrameParse returns 1 for a frame whose length, sync word and CRC all
 * check out, 0 otherwise; `data` comes back pointing into `buf`.
 */
int PVMDBG_FrameBuild(char* buf, uint16_t cmd, const uint8_t* data, int dataLen);
int PVMDBG_FrameParse(const char* buf, int len, uint16_t* cmd,
                      const uint8_t** data, int* dataLen);

/*
 * How long the frame starting at buf is, given the `available` bytes that have
 * turned up so far. Returns that length once the whole frame is there, 0 while
 * more bytes are still needed, and -1 when the bytes at the front cannot begin
 * a frame at all -- on which the caller drops the first byte and asks again,
 * which is how a stream resynchronises after line noise or a half-sent frame.
 *
 * Whoever is reading from a serial port needs this at both ends: bytes arrive a
 * handful at a time with no regard for where frames begin or end.
 */
int PVMDBG_FrameLength(const char* buf, int available);

#endif //PVMDBG_H_




