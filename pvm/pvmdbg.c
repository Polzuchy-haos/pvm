/*
 * pvmdbg.c
 *
 * Device side of the debug protocol. Deliberately transport-agnostic: it never
 * touches a UART itself, it is handed a buffer that already holds one received
 * frame and fills the same buffer with the reply (see the sketch at the top of
 * pvmdbg.h). On a microcontroller the caller reads that buffer from a serial
 * port and writes it back out; on a desktop it can just as well be a pipe, a
 * socket or a test harness.
 *
 * Frame layout on the wire, little-endian throughout, matching PVMDbgFrame_s:
 *
 *   offset 0  uint32  sync   (PVMDBG_SYNC)
 *   offset 4  uint16  cmd    (PVMDbgCmd_e)
 *   offset 6  uint8   len    (payload bytes that follow)
 *   offset 7  uint8[] data   (command-specific, may be empty)
 *   last 2    uint16  crc16  (CCITT, over everything before it)
 *
 * The frame is read and written byte by byte rather than through a
 * PVMDbgFrame_s* cast: the caller's buffer carries no alignment guarantee, and
 * a uint32 read from an odd address is undefined behavior that Cortex-M0 turns
 * into a HardFault -- the same reason pvm.c's _FetchWord exists.
 */
#include "pvmdbg.h"

#include <string.h>

#define HDR_SIZE (7)
#define CRC_SIZE (2)

static uint16_t _Get16(const uint8_t* p);
static void 		_Put16(uint8_t* p, uint16_t value);
static void 		_Put32(uint8_t* p, uint32_t value);
static uint16_t _Crc16(const uint8_t* p, int len);
static int 			_Reply(uint8_t* frame, uint16_t cmd, int dataLen);
static int 			_Dispatch(PVMDbg_s* dbg, uint8_t* frame, uint16_t cmd,
													const uint8_t* data, int dataLen);
static int 			_BrkpSet(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr);
static int 			_BrkpClr(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr);
static int 			_BrkpList(PVMDbg_s* dbg, uint8_t* frame);
static int 			_ReadStack(PVMDbg_s* dbg, uint8_t* frame, int count);
static int 			_ReadCode(PVMDbg_s* dbg, uint8_t* frame, int addr, int count);
static int 			_ExecWrite(PVMDbg_s* dbg, uint8_t* frame, const uint8_t* data, int dataLen);
static int 			_ExecRun(PVMDbg_s* dbg, uint8_t* frame, int len);
static int 			_ExecStatus(PVMDbg_s* dbg, uint8_t* frame);
static int 			_ExecInstr(PVMDbg_s* dbg, uint8_t* frame, const uint8_t* data, int dataLen);
static void 		_ExecLeave(PVMDbg_s* dbg, uint8_t outcome);
static uint8_t* _ProgCode(PVMDbg_s* dbg);
static uint16_t _ProgLen(PVMDbg_s* dbg);


void PVMDBG_Init(PVMDbg_s* dbg, PVM_State_s* state){
  dbg->state  = state;
  /* Running, not stopped: a board that boots with nothing attached to its
   * debug port still has to run its program. The host stops it with PAUSE. */
  dbg->mode   = DBGMODE_RUNNING;
  dbg->reason = DBGSTOP_NONE;
  dbg->resume = 0;

  dbg->exec_state = DBGEXEC_IDLE;
  dbg->exec_count = 0;

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++)
    dbg->brkp[i] = 0;
}

int PVMDBG_IsAccessExecTick(PVMDbg_s* dbg){
  if(dbg->mode == DBGMODE_STOPPED)
    return 0;

  /* Executing HALT over and over changes nothing and would spin the main loop
   * forever, so the program ending is treated as a reason to stop. PC is left
   * sitting on the HALT, which is exactly where a debugger wants to show it. */
  if(*dbg->state->PC == INSTR_HALT){
    /* The HALT that ends a snippet is not the end of the program: put the
     * program back where it was and stop there, so the host sees the snippet
     * finish and the program it interrupted in one and the same STATE. */
    if(dbg->exec_state == DBGEXEC_ACTIVE){
      _ExecLeave(dbg, DBGEXEC_DONE);
      dbg->mode 	= DBGMODE_STOPPED;
      dbg->reason = DBGSTOP_EXEC_DONE;
      return 0;
    }

    dbg->mode 	= DBGMODE_STOPPED;
    dbg->reason = DBGSTOP_HALTED;
    return 0;
  }

  /* A single step always executes, breakpoint or not: the host asked for one
   * instruction, and refusing to run the very one it is stopped on is exactly
   * how a stepper gets stuck. */
  if(dbg->mode == DBGMODE_STEP){
    dbg->mode 	= DBGMODE_STOPPED;
    dbg->reason = DBGSTOP_STEP;
    return 1;
  }

  /* Whatever breakpoint PC is on right now is the one already reported, and RUN
   * is the host saying "carry on past it", so it is ignored exactly once.
   * Without this the same address is re-reported on every pass through the main
   * loop and the program never advances a single instruction. */
  if(dbg->resume){
    dbg->resume = 0;
    return 1;
  }

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++){
    if(dbg->brkp[i] == dbg->state->PC){
      dbg->mode 	= DBGMODE_STOPPED;
      dbg->reason = DBGSTOP_BREAKPOINT;
      return 0;
    }
  }

  return 1;
}

/*
 * *len is the number of bytes received on the way in, and the number of bytes
 * to send back on the way out -- zero meaning "nothing to send", which is the
 * answer to anything that does not parse. A frame that fails its CRC gets no
 * reply at all rather than an error one: the cmd field of a corrupt frame is
 * not to be trusted either, so there is nothing to answer.
 */
void PVMDBG_Poll(PVMDbg_s* dbg, char* buf, int* len){
  const uint8_t* data;
  uint16_t 			 cmd;
  int 					 dataLen;

  if(!PVMDBG_FrameParse(buf, *len, &cmd, &data, &dataLen)){
    *len = 0;
    return;
  }

  *len = _Dispatch(dbg, (uint8_t*)buf, cmd, data, dataLen);
}

int PVMDBG_FrameBuild(char* buf, uint16_t cmd, const uint8_t* data, int dataLen){
  uint8_t* frame = (uint8_t*)buf;
  int 		 total = HDR_SIZE + dataLen;

  if(data && data != frame + HDR_SIZE)
    memcpy(frame + HDR_SIZE, data, (size_t)dataLen);

  _Put32(frame, PVMDBG_SYNC);
  _Put16(frame + 4, cmd);
  frame[6] = (uint8_t)dataLen;
  _Put16(frame + total, _Crc16(frame, total));

  return total + CRC_SIZE;
}

int PVMDBG_FrameParse(const char* buf, int len, uint16_t* cmd,
                      const uint8_t** data, int* dataLen){
  const uint8_t* frame = (const uint8_t*)buf;

  if(PVMDBG_FrameLength(buf, len) != len) 														 return 0;
  if(_Get16(frame + len - CRC_SIZE) != _Crc16(frame, len - CRC_SIZE))  return 0;

  *cmd 		 = _Get16(frame + 4);
  *data 	 = frame + HDR_SIZE;
  *dataLen = frame[6];
  return 1;
}

int PVMDBG_FrameLength(const char* buf, int available){
  const uint8_t* frame = (const uint8_t*)buf;
  int 					 payload;

  /* The sync word is checked as soon as enough of it has arrived, so a stream
   * full of noise is rejected a byte at a time instead of waiting for a length
   * field that is never coming. */
  for(int i = 0; i < 4 && i < available; i++)
    if(frame[i] != (uint8_t)((PVMDBG_SYNC >> (8 * i)) & 0xFF)) return -1;

  if(available < HDR_SIZE) return 0;

  payload = frame[6];
  if(payload > PVMDBG_CFG_DATA_MAX) return -1; /* longer than this build can hold */

  return (available < HDR_SIZE + payload + CRC_SIZE) ? 0 : HDR_SIZE + payload + CRC_SIZE;
}


static int _Dispatch(PVMDbg_s* dbg, uint8_t* frame, uint16_t cmd,
                     const uint8_t* data, int dataLen){
  uint8_t* out = frame + HDR_SIZE;

  switch(cmd){
  case DBGCMD_PING:
    return _Reply(frame, cmd, 0);

  case DBGCMD_STATE:
    /* Positions are sent as offsets, not pointers: the host has no idea where
     * this board's arrays live, and a pointer would not fit in 16 bits anyway. */
    _Put16(out + 0, (uint16_t)(dbg->state->PC - dbg->state->code_ptr));
    _Put16(out + 2, (uint16_t)(dbg->state->SP - dbg->state->stack));
    _Put16(out + 4, (uint16_t)(dbg->state->FP - dbg->state->stack));
    out[6] = dbg->mode;
    out[7] = dbg->reason;
    /* Appended rather than slotted in, so a host that only knows the first
     * eight bytes goes on reading them exactly as before. While it is set, PC
     * is an offset into the snippet, not the program. */
    out[8] = (dbg->exec_state == DBGEXEC_ACTIVE);
    return _Reply(frame, cmd, 9);

  case DBGCMD_MEM_INFO:
    _Put16(out + 0, dbg->state->stack_len);
    _Put16(out + 2, dbg->state->ram_len);
    return _Reply(frame, cmd, 4);

  case DBGCMD_RUN:
    dbg->mode 	= DBGMODE_RUNNING;
    dbg->reason = DBGSTOP_NONE;
    dbg->resume = 1;
    return _Reply(frame, cmd, 0);

  case DBGCMD_PAUSE:
    dbg->mode 	= DBGMODE_STOPPED;
    dbg->reason = DBGSTOP_HOST;
    return _Reply(frame, cmd, 0);

  case DBGCMD_STEP:
    dbg->mode 	= DBGMODE_STEP;
    dbg->reason = DBGSTOP_NONE;
    return _Reply(frame, cmd, 0);

  case DBGCMD_RESET:
    /* "Back to the start" means the program's start, not the snippet's. */
    if(dbg->exec_state == DBGEXEC_ACTIVE)
      _ExecLeave(dbg, DBGEXEC_ABORTED);

    dbg->state->PC = dbg->state->code_ptr;
    dbg->state->SP = dbg->state->stack;
    dbg->state->FP = dbg->state->stack;
    dbg->mode 		 = DBGMODE_STOPPED;
    dbg->reason 	 = DBGSTOP_HOST;
    return _Reply(frame, cmd, 0);

  case DBGCMD_BRKP_SET:
    if(dataLen < 2) return _Reply(frame, DBGCMD_ERROR, 0);
    return _BrkpSet(dbg, frame, _Get16(data));

  case DBGCMD_BRKP_CLR:
    if(dataLen < 2) return _Reply(frame, DBGCMD_ERROR, 0);
    return _BrkpClr(dbg, frame, _Get16(data));

  case DBGCMD_READ_STACK:
    return _ReadStack(dbg, frame, (dataLen >= 1) ? data[0] : 0);

  case DBGCMD_READ_CODE:
    if(dataLen < 3) return _Reply(frame, DBGCMD_ERROR, 0);
    return _ReadCode(dbg, frame, _Get16(data), data[2]);

  case DBGCMD_BRKP_LIST:
    return _BrkpList(dbg, frame);

  case DBGCMD_EXEC_WRITE:
    return _ExecWrite(dbg, frame, data, dataLen);

  case DBGCMD_EXEC_RUN:
    if(dataLen < 1) return _Reply(frame, DBGCMD_ERROR, 0);
    return _ExecRun(dbg, frame, data[0]);

  case DBGCMD_EXEC_STATUS:
    return _ExecStatus(dbg, frame);

  case DBGCMD_EXEC_INSTR:
    return _ExecInstr(dbg, frame, data, dataLen);

  /* Like BRKP_CLR, asking for a state that already holds is not an error. */
  case DBGCMD_EXEC_ABORT:
    if(dbg->exec_state == DBGEXEC_ACTIVE){
      _ExecLeave(dbg, DBGEXEC_ABORTED);
      dbg->mode 	= DBGMODE_STOPPED;
      dbg->reason = DBGSTOP_HOST;
    }
    return _Reply(frame, cmd, 0);

  default:
    return _Reply(frame, DBGCMD_ERROR, 0);
  }
}

static int _BrkpSet(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr){
  uint8_t* target = _ProgCode(dbg) + addr;
  int 		 slot 	= -1;

  if(addr >= _ProgLen(dbg))
    return _Reply(frame, DBGCMD_ERROR, 0);

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++){
    if(dbg->brkp[i] == target) 									return _Reply(frame, DBGCMD_BRKP_SET, 0);
    if(dbg->brkp[i] == 0 && slot < 0) slot = i;
  }

  if(slot < 0)
    return _Reply(frame, DBGCMD_ERROR, 0); /* table full */

  dbg->brkp[slot] = target;
  return _Reply(frame, DBGCMD_BRKP_SET, 0);
}

/* Clearing an address that was never armed is not an error: the host asked for
 * a state, and that state already holds -- which is what makes "clear them all"
 * on the host side a plain loop rather than a special case. */
static int _BrkpClr(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr){
  uint8_t* target = _ProgCode(dbg) + addr;

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++)
    if(dbg->brkp[i] == target)
      dbg->brkp[i] = 0;

  return _Reply(frame, DBGCMD_BRKP_CLR, 0);
}

static int _BrkpList(PVMDbg_s* dbg, uint8_t* frame){
  uint8_t* data = frame + HDR_SIZE;
  int 		 n 		= 0;

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++){
    if(dbg->brkp[i] == 0) continue;

    _Put16(data + 1 + 2 * n, (uint16_t)(dbg->brkp[i] - _ProgCode(dbg)));
    n++;
  }

  data[0] = (uint8_t)n;
  return _Reply(frame, DBGCMD_BRKP_LIST, 1 + 2 * n);
}

/* Top of the stack first, which is the order the values are interesting in:
 * whatever the instruction just executed left behind is item 0. Asking for more
 * than there is, or more than fits in one frame, is not an error -- the count in
 * the reply says how many actually came back. */
static int _ReadStack(PVMDbg_s* dbg, uint8_t* frame, int count){
  uint8_t* data 	= frame + HDR_SIZE;
  int 		 depth 	= (int)(dbg->state->SP - dbg->state->stack);
  int 		 n 			= count;

  if(n > depth) 											 n = depth;
  if(n > PVMDBG_CFG_STACK_READ_MAX) 	 n = PVMDBG_CFG_STACK_READ_MAX;
  if(n < 0) 													 n = 0;

  for(int i = 0; i < n; i++)
    _Put16(data + 1 + 2 * i, (uint16_t)dbg->state->SP[-(i + 1)]);

  data[0] = (uint8_t)n;
  return _Reply(frame, DBGCMD_READ_STACK, 1 + 2 * n);
}

/* Same shape as _ReadStack: asking for more than exists, or more than one frame
 * carries, is not an error -- the count in the reply says what actually came. */
static int _ReadCode(PVMDbg_s* dbg, uint8_t* frame, int addr, int count){
  uint8_t* out = frame + HDR_SIZE;
  int 		 n 	 = count;

  if(addr < 0 || addr >= (int)dbg->state->code_len)
    return _Reply(frame, DBGCMD_ERROR, 0);

  if(n > PVMDBG_CFG_CODE_READ_MAX) 					 n = PVMDBG_CFG_CODE_READ_MAX;
  if(addr + n > (int)dbg->state->code_len) 	 n = (int)dbg->state->code_len - addr;
  if(n < 0) 																 n = 0;

  for(int i = 0; i < n; i++)
    out[1 + i] = dbg->state->code_ptr[addr + i];

  out[0] = (uint8_t)n;
  return _Reply(frame, DBGCMD_READ_CODE, 1 + n);
}

/* ---- running uploaded code -------------------------------------------- */

/* Breakpoint addresses always mean the program, also while a snippet has the
 * VM: the host sets them from a listing or a source line of the program. */
static uint8_t* _ProgCode(PVMDbg_s* dbg){
  return (dbg->exec_state == DBGEXEC_ACTIVE) ? dbg->saved_code_ptr : dbg->state->code_ptr;
}

static uint16_t _ProgLen(PVMDbg_s* dbg){
  return (dbg->exec_state == DBGEXEC_ACTIVE) ? dbg->saved_code_len : dbg->state->code_len;
}

/* Uploaded in pieces because a frame carries a few dozen bytes at most, and
 * the buffer is bigger than that. Rewriting the buffer under a snippet that is
 * still running would change the code beneath its PC, so that is refused. */
static int _ExecWrite(PVMDbg_s* dbg, uint8_t* frame, const uint8_t* data, int dataLen){
  int offset, n;

  if(dataLen < 1 || dbg->exec_state == DBGEXEC_ACTIVE)
    return _Reply(frame, DBGCMD_ERROR, 0);

  offset = data[0];
  n 		 = dataLen - 1;

  if(offset + n > PVMDBG_CFG_EXEC_SIZE)
    return _Reply(frame, DBGCMD_ERROR, 0);

  for(int i = 0; i < n; i++)
    dbg->exec_code[offset + i] = data[1 + i];

  return _Reply(frame, DBGCMD_EXEC_WRITE, 0);
}

/*
 * The VM is pointed at the buffer instead of the program -- code_ptr, code_len
 * and PC are all it executes from, so nothing in pvm.c has to know -- and set
 * running. Only from a stopped VM: the host should know where the program was
 * interrupted, and gets to see it again once the snippet is done.
 *
 * Every byte after the snippet is HALT, and code_len covers one of them, so a
 * snippet needs no HALT of its own and a jump to its very end is a jump to the
 * finish rather than a bad address.
 */
static int _ExecRun(PVMDbg_s* dbg, uint8_t* frame, int len){
  PVM_State_s* vm = dbg->state;

  if(len < 1 || len > PVMDBG_CFG_EXEC_SIZE ||
     dbg->exec_state == DBGEXEC_ACTIVE || dbg->mode != DBGMODE_STOPPED)
    return _Reply(frame, DBGCMD_ERROR, 0);

  for(int i = len; i < PVMDBG_CFG_EXEC_SIZE + PVMDBG_EXEC_PAD; i++)
    dbg->exec_code[i] = INSTR_HALT;

  dbg->saved_code_ptr = vm->code_ptr;
  dbg->saved_code_len = vm->code_len;
  dbg->saved_PC 			= vm->PC;
  dbg->saved_SP 			= vm->SP;
  dbg->saved_FP 			= vm->FP;

  vm->code_ptr = dbg->exec_code;
  vm->code_len = (uint16_t)(len + 1);
  vm->PC 			 = dbg->exec_code;

  dbg->exec_state = DBGEXEC_ACTIVE;
  dbg->exec_count = 0;
  dbg->mode 			= DBGMODE_RUNNING;
  dbg->reason 		= DBGSTOP_NONE;
  dbg->resume 		= 0;

  return _Reply(frame, DBGCMD_EXEC_RUN, 0);
}

/* Same shape as READ_STACK's reply with the state in front: the words are what
 * the snippet left above the stack it started on, top first. */
static int _ExecStatus(PVMDbg_s* dbg, uint8_t* frame){
  uint8_t* out = frame + HDR_SIZE;

  out[0] = dbg->exec_state;
  out[1] = dbg->exec_count;

  for(int i = 0; i < dbg->exec_count; i++)
    _Put16(out + 2 + 2 * i, (uint16_t)dbg->exec_result[i]);

  return _Reply(frame, DBGCMD_EXEC_STATUS, 2 + 2 * dbg->exec_count);
}

/*
 * Runs one instruction the host sent, right now rather than on a later turn of
 * the main loop, so the reply can say how it went -- the only way an execution
 * error ever reaches the host. The VM is pointed at the instruction for exactly
 * one PVM_ExecTick and then back at whatever it was executing before, so the
 * flashed program (if there is one) is not disturbed, only the data it shares.
 *
 * Control flow is refused: a jump target is a program address, and the program
 * lives on the host, which follows jumps itself. Refusing them here is what
 * stops a host bug from sending the VM into the weeds.
 */
static int _ExecInstr(PVMDbg_s* dbg, uint8_t* frame, const uint8_t* data, int dataLen){
  PVM_State_s* vm  = dbg->state;
  uint8_t* 		 out = frame + HDR_SIZE;
  uint8_t* 		 codePtr;
  uint16_t 		 codeLen;
  uint8_t* 		 pc;
  int 				 status;

  if(dataLen < 1 || dataLen > 3 ||
     dbg->mode != DBGMODE_STOPPED || dbg->exec_state == DBGEXEC_ACTIVE)
    return _Reply(frame, DBGCMD_ERROR, 0);

  switch(data[0]){
  case INSTR_JMP: case INSTR_JZ: case INSTR_JNZ:
  case INSTR_CALL: case INSTR_RET: case INSTR_HALT:
    return _Reply(frame, DBGCMD_ERROR, 0);
  default:
    break;
  }

  /* The snippet buffer is idle (checked above), so it doubles as the place to
   * run from; HALT after the instruction covers an operand the host cut short. */
  for(int i = 0; i < PVMDBG_EXEC_PAD + 1; i++)
    dbg->exec_code[i] = (i < dataLen) ? data[i] : INSTR_HALT;

  codePtr = vm->code_ptr;
  codeLen = vm->code_len;
  pc 			= vm->PC;

  vm->code_ptr = dbg->exec_code;
  vm->code_len = (uint16_t)dataLen;
  vm->PC 			 = dbg->exec_code;

  status = PVM_ExecTick(vm);

  vm->code_ptr = codePtr;
  vm->code_len = codeLen;
  vm->PC 			 = pc;

  out[0] = (uint8_t)(int8_t)status;
  _Put16(out + 1, (uint16_t)(vm->SP - vm->stack));
  _Put16(out + 3, (uint16_t)(vm->FP - vm->stack));
  return _Reply(frame, DBGCMD_EXEC_INSTR, 5);
}

/*
 * Hands the VM back to the program. What a finished snippet pushed is copied
 * out first, since putting SP back is about to make it unreachable -- that is
 * how a snippet answers a question ("LOAD 3" and the host reads the value).
 * Anything it wrote to RAM, the user section or below its starting SP stays
 * written: changing the program's data on the fly is the point.
 */
static void _ExecLeave(PVMDbg_s* dbg, uint8_t outcome){
  PVM_State_s* vm = dbg->state;
  int 				 n 	= 0;

  if(outcome == DBGEXEC_DONE){
    n = (int)(vm->SP - dbg->saved_SP);
    if(n > PVMDBG_CFG_STACK_READ_MAX) n = PVMDBG_CFG_STACK_READ_MAX;
    if(n < 0) 												n = 0;

    for(int i = 0; i < n; i++)
      dbg->exec_result[i] = vm->SP[-(i + 1)];
  }

  vm->code_ptr = dbg->saved_code_ptr;
  vm->code_len = dbg->saved_code_len;
  vm->PC 			 = dbg->saved_PC;
  vm->SP 			 = dbg->saved_SP;
  vm->FP 			 = dbg->saved_FP;

  dbg->exec_state = outcome;
  dbg->exec_count = (uint8_t)n;
}

/* Writes the header around data the caller has already placed at HDR_SIZE, then
 * the CRC after it; returns the whole frame's length. */
static int _Reply(uint8_t* frame, uint16_t cmd, int dataLen){
  return PVMDBG_FrameBuild((char*)frame, cmd, 0, dataLen);
}


static uint16_t _Get16(const uint8_t* p){
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void _Put16(uint8_t* p, uint16_t value){
  p[0] = (uint8_t)(value 			 & 0xFF);
  p[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void _Put32(uint8_t* p, uint32_t value){
  p[0] = (uint8_t)(value 				 & 0xFF);
  p[1] = (uint8_t)((value >> 8)  & 0xFF);
  p[2] = (uint8_t)((value >> 16) & 0xFF);
  p[3] = (uint8_t)((value >> 24) & 0xFF);
}

/* CRC-16/CCITT-FALSE, computed a bit at a time rather than through a lookup
 * table: a 512-byte table is a poor trade for frames this short on a part with
 * this little flash. */
static uint16_t _Crc16(const uint8_t* p, int len){
  uint16_t crc = 0xFFFF;

  for(int i = 0; i < len; i++){
    crc ^= (uint16_t)((uint16_t)p[i] << 8);

    for(int bit = 0; bit < 8; bit++)
      crc = (crc & 0x8000) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021)
                          : (uint16_t)(crc << 1);
  }

  return crc;
}
