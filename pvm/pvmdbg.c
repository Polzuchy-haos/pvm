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
 * Every frame is the same 12 bytes -- sync, cmd, 4 bytes of data, CRC; see
 * pvmdbg.h for the layout and for what each command puts in the data.
 *
 * The frame is read and written byte by byte rather than through a
 * PVMDbgFrame_s* cast: the caller's buffer carries no alignment guarantee, and
 * a uint32 read from an odd address is undefined behavior that Cortex-M0 turns
 * into a HardFault -- the same reason pvm.c's _FetchWord exists.
 */
#include "pvmdbg.h"

#include <string.h>

#define DATA_AT (6)
#define CRC_AT 	(DATA_AT + PVMDBG_DATA_SIZE)

static uint16_t _Get16(const uint8_t* p);
static void 		_Put16(uint8_t* p, uint16_t value);
static void 		_Put32(uint8_t* p, uint32_t value);
static uint16_t _Crc16(const uint8_t* p, int len);
static int 			_Reply(uint8_t* frame, uint16_t cmd);
static int 			_Dispatch(PVMDbg_s* dbg, uint8_t* frame, uint16_t cmd, const uint8_t* in);
static int 			_BrkpSet(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr);
static int 			_BrkpClr(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr);
static int 			_BrkpList(PVMDbg_s* dbg, uint8_t* frame, int slot);
static int 			_ReadStack(PVMDbg_s* dbg, uint8_t* frame, int index);
static int 			_ReadCode(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr);
static int 			_MemInfo(PVMDbg_s* dbg, uint8_t* frame, int which);
static int 			_ExecInstr(PVMDbg_s* dbg, uint8_t* frame, const uint8_t* in);
static int 			_PcInInstrArea(const PVMDbg_s* dbg);
static void 		_FillHalt(PVMDbg_s* dbg);


void PVMDBG_Init(PVMDbg_s* dbg, PVM_State_s* state){
  dbg->state  = state;
  /* Running, not stopped: a board that boots with nothing attached to its
   * debug port still has to run its program. The host stops it with PAUSE. */
  dbg->mode   = DBGMODE_RUNNING;
  dbg->reason = DBGSTOP_NONE;
  dbg->resume = 0;

  _FillHalt(dbg);

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++)
    dbg->brkp[i] = 0;
}

/* The program has to go on as if nobody had been there. If the session ran
 * code from the host, PC is pointing at the instruction area and SP/FP at
 * whatever that code left -- none of it the board's own program -- so they
 * start over exactly as PVM_Init left them. Otherwise PC is the program's
 * own, and it simply carries on from there. */
void PVMDBG_Detach(PVMDbg_s* dbg){
  if(_PcInInstrArea(dbg)){
    dbg->state->PC = dbg->state->code_ptr;
    dbg->state->SP = dbg->state->stack;
    dbg->state->FP = dbg->state->stack;
  }

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++)
    dbg->brkp[i] = 0;

  dbg->mode 	= DBGMODE_RUNNING;
  dbg->reason = DBGSTOP_NONE;
  dbg->resume = 0;
}

int PVMDBG_IsAccessExecTick(PVMDbg_s* dbg){
  if(dbg->mode == DBGMODE_STOPPED)
    return 0;

  /* Executing HALT over and over changes nothing and would spin the main loop
   * forever, so the program ending is treated as a reason to stop. PC is left
   * sitting on the HALT, which is exactly where a debugger wants to show it. */
  if(*dbg->state->PC == INSTR_HALT){
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
  uint8_t 			 in[PVMDBG_DATA_SIZE];
  uint16_t 			 cmd;
  int 					 dataLen;

  if(!PVMDBG_FrameParse(buf, *len, &cmd, &data, &dataLen)){
    *len = 0;
    return;
  }

  /* The reply is written over the request, so the request's data is set aside
   * first and the reply's cleared: whatever a handler leaves unwritten goes
   * out as zero, as the format says. */
  memcpy(in, data, PVMDBG_DATA_SIZE);
  memset((uint8_t*)buf + DATA_AT, 0, PVMDBG_DATA_SIZE);

  *len = _Dispatch(dbg, (uint8_t*)buf, cmd, in);
}

int PVMDBG_FrameBuild(char* buf, uint16_t cmd, const uint8_t* data, int dataLen){
  uint8_t* frame = (uint8_t*)buf;

  if(!data || dataLen < 0) 			 dataLen = 0;
  if(dataLen > PVMDBG_DATA_SIZE) dataLen = PVMDBG_DATA_SIZE;

  if(dataLen > 0) memmove(frame + DATA_AT, data, (size_t)dataLen);
  memset(frame + DATA_AT + dataLen, 0, (size_t)(PVMDBG_DATA_SIZE - dataLen));

  return _Reply(frame, cmd);
}

int PVMDBG_FrameParse(const char* buf, int len, uint16_t* cmd,
                      const uint8_t** data, int* dataLen){
  const uint8_t* frame = (const uint8_t*)buf;

  if(len != PVMDBG_FRAME_SIZE || PVMDBG_FrameLength(buf, len) != len) return 0;
  if(_Get16(frame + CRC_AT) != _Crc16(frame, CRC_AT)) 									return 0;

  *cmd 		 = _Get16(frame + 4);
  *data 	 = frame + DATA_AT;
  *dataLen = PVMDBG_DATA_SIZE;
  return 1;
}

int PVMDBG_FrameLength(const char* buf, int available){
  const uint8_t* frame = (const uint8_t*)buf;

  /* The sync word is checked as soon as enough of it has arrived, so a stream
   * full of noise is rejected a byte at a time instead of waiting for a frame's
   * worth of it. */
  for(int i = 0; i < 4 && i < available; i++)
    if(frame[i] != (uint8_t)((PVMDBG_SYNC >> (8 * i)) & 0xFF)) return -1;

  return (available < PVMDBG_FRAME_SIZE) ? 0 : PVMDBG_FRAME_SIZE;
}


static int _Dispatch(PVMDbg_s* dbg, uint8_t* frame, uint16_t cmd, const uint8_t* in){
  uint8_t* out = frame + DATA_AT;

  switch(cmd){
  case DBGCMD_PING:
    return _Reply(frame, cmd);

  case DBGCMD_STATE:
    /* Positions are sent as offsets, not pointers: the host has no idea where
     * this board's arrays live, and a pointer would not fit in 16 bits anyway.
     * PC on the EXEC_INSTR area is in no program at all, and says so: 0xFFFF,
     * which no program address can be. */
    _Put16(out, _PcInInstrArea(dbg) ? 0xFFFFu
                                    : (uint16_t)(dbg->state->PC - dbg->state->code_ptr));
    out[2] = dbg->mode;
    out[3] = dbg->reason;
    return _Reply(frame, cmd);

  case DBGCMD_REGS:
    _Put16(out + 0, (uint16_t)(dbg->state->SP - dbg->state->stack));
    _Put16(out + 2, (uint16_t)(dbg->state->FP - dbg->state->stack));
    return _Reply(frame, cmd);

  case DBGCMD_MEM_INFO:
    return _MemInfo(dbg, frame, in[0]);

  case DBGCMD_READ_DATA:
  {
    PVM_Word_t value;

    if(PVM_ReadData(dbg->state, (PVM_Word_t)_Get16(in), &value) != ERRORCODE_NO_ERROR)
      return _Reply(frame, DBGCMD_ERROR);

    _Put16(out, (uint16_t)value);
    return _Reply(frame, cmd);
  }

  case DBGCMD_RUN:
    dbg->mode 	= DBGMODE_RUNNING;
    dbg->reason = DBGSTOP_NONE;
    dbg->resume = 1;
    return _Reply(frame, cmd);

  case DBGCMD_PAUSE:
    dbg->mode 	= DBGMODE_STOPPED;
    dbg->reason = DBGSTOP_HOST;
    return _Reply(frame, cmd);

  case DBGCMD_STEP:
    dbg->mode 	= DBGMODE_STEP;
    dbg->reason = DBGSTOP_NONE;
    return _Reply(frame, cmd);

  case DBGCMD_RESET:
    dbg->state->PC = dbg->state->code_ptr;
    dbg->state->SP = dbg->state->stack;
    dbg->state->FP = dbg->state->stack;
    dbg->mode 		 = DBGMODE_STOPPED;
    dbg->reason 	 = DBGSTOP_HOST;
    return _Reply(frame, cmd);

  case DBGCMD_DETACH:
    PVMDBG_Detach(dbg);
    return _Reply(frame, cmd);

  case DBGCMD_BRKP_SET:
    return _BrkpSet(dbg, frame, _Get16(in));

  case DBGCMD_BRKP_CLR:
    return _BrkpClr(dbg, frame, _Get16(in));

  case DBGCMD_BRKP_LIST:
    return _BrkpList(dbg, frame, in[0]);

  case DBGCMD_READ_STACK:
    return _ReadStack(dbg, frame, in[0]);

  case DBGCMD_READ_STACK_AT:
  {
    int depth = (int)(dbg->state->SP - dbg->state->stack);
    int index = _Get16(in);

    if(index >= depth) return _Reply(frame, DBGCMD_ERROR);

    _Put16(out + 0, (uint16_t)dbg->state->stack[index]);
    _Put16(out + 2, (uint16_t)depth);
    return _Reply(frame, cmd);
  }

  case DBGCMD_READ_CODE:
    return _ReadCode(dbg, frame, _Get16(in));

  case DBGCMD_EXEC_INSTR:
    return _ExecInstr(dbg, frame, in);

  default:
    return _Reply(frame, DBGCMD_ERROR);
  }
}

static int _BrkpSet(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr){
  uint8_t* target = dbg->state->code_ptr + addr;
  int 		 slot 	= -1;

  if(addr >= dbg->state->code_len)
    return _Reply(frame, DBGCMD_ERROR);

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++){
    if(dbg->brkp[i] == target) 									return _Reply(frame, DBGCMD_BRKP_SET);
    if(dbg->brkp[i] == 0 && slot < 0) slot = i;
  }

  if(slot < 0)
    return _Reply(frame, DBGCMD_ERROR); /* table full */

  dbg->brkp[slot] = target;
  return _Reply(frame, DBGCMD_BRKP_SET);
}

/* Clearing an address that was never armed is not an error: the host asked for
 * a state, and that state already holds -- which is what makes "clear them all"
 * on the host side a plain loop rather than a special case. */
static int _BrkpClr(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr){
  uint8_t* target = dbg->state->code_ptr + addr;

  for(int i = 0; i < PVMDBG_CFG_BRKP_SIZE; i++)
    if(dbg->brkp[i] == target)
      dbg->brkp[i] = 0;

  return _Reply(frame, DBGCMD_BRKP_CLR);
}

/* One slot per frame; the slot count in every reply tells the host where to
 * stop, so it needs no idea of this build's table size. */
static int _BrkpList(PVMDbg_s* dbg, uint8_t* frame, int slot){
  uint8_t* out = frame + DATA_AT;

  if(slot >= PVMDBG_CFG_BRKP_SIZE)
    return _Reply(frame, DBGCMD_ERROR);

  if(dbg->brkp[slot]){
    _Put16(out, (uint16_t)(dbg->brkp[slot] - dbg->state->code_ptr));
    out[2] = 1;
  }
  out[3] = PVMDBG_CFG_BRKP_SIZE;
  return _Reply(frame, DBGCMD_BRKP_LIST);
}

/* Counted from the top, which is the order the values are interesting in:
 * whatever the instruction just executed left behind is index 0. The depth
 * comes back with every word, so the host knows when to stop asking. */
static int _ReadStack(PVMDbg_s* dbg, uint8_t* frame, int index){
  uint8_t* out 	 = frame + DATA_AT;
  int 		 depth = (int)(dbg->state->SP - dbg->state->stack);

  if(index >= depth)
    return _Reply(frame, DBGCMD_ERROR);

  _Put16(out + 0, (uint16_t)dbg->state->SP[-(index + 1)]);
  _Put16(out + 2, (uint16_t)depth);
  return _Reply(frame, DBGCMD_READ_STACK);
}

/* Past the end is padded with HALT rather than refused, so a window that runs
 * off the program still comes back whole -- and decodes as what the VM would do
 * there anyway. Starting past the end is an error. */
static int _ReadCode(PVMDbg_s* dbg, uint8_t* frame, uint16_t addr){
  uint8_t* out = frame + DATA_AT;

  if(addr >= dbg->state->code_len)
    return _Reply(frame, DBGCMD_ERROR);

  for(int i = 0; i < PVMDBG_DATA_SIZE; i++)
    out[i] = (addr + i < dbg->state->code_len) ? dbg->state->code_ptr[addr + i] : INSTR_HALT;

  return _Reply(frame, DBGCMD_READ_CODE);
}

static int _MemInfo(PVMDbg_s* dbg, uint8_t* frame, int which){
  uint8_t* out = frame + DATA_AT;

  switch(which){
  case 0:
    _Put16(out + 0, dbg->state->stack_len);
    _Put16(out + 2, dbg->state->ram_len);
    break;
  case 1:
    _Put16(out + 0, dbg->state->code_len);
    _Put16(out + 2, dbg->state->usrsect_ptr ? (uint16_t)dbg->state->usrsect_len : 0u);
    break;
  default:
    return _Reply(frame, DBGCMD_ERROR);
  }

  return _Reply(frame, DBGCMD_MEM_INFO);
}

/* ---- one instruction from the host -------------------------------------- */

static void _FillHalt(PVMDbg_s* dbg){
  for(int i = 0; i < PVMDBG_CFG_INSTR_SIZE; i++)
    dbg->instr[i] = INSTR_HALT;
}

static int _PcInInstrArea(const PVMDbg_s* dbg){
  return dbg->state->PC >= dbg->instr && dbg->state->PC < dbg->instr + PVMDBG_CFG_INSTR_SIZE;
}

/*
 * Runs the instruction right now rather than on a later turn of the main loop,
 * so the reply can say how it went -- the only way an execution error ever
 * reaches the host. Only PC is moved: code_ptr and code_len stay the
 * program's, which is all the jump checks in pvm.c would look at, and jumps
 * are refused here anyway. After the tick PC sits on the HALT that follows the
 * instruction, so a stray RUN stops there instead of wandering off -- which is
 * why the length travels with the instruction: the bytes after a 1-byte
 * instruction must stay HALT, not become whatever padding the frame carried.
 *
 * Control flow is refused: a jump target is a program address, and the program
 * lives on the host, which follows jumps itself. Refusing them here is what
 * stops a host bug from sending the VM into the weeds.
 */
static int _ExecInstr(PVMDbg_s* dbg, uint8_t* frame, const uint8_t* in){
  PVM_State_s* vm  = dbg->state;
  uint8_t* 		 out = frame + DATA_AT;
  int 				 len = in[3];
  int 				 status;

  if((len != 1 && len != 3) || dbg->mode != DBGMODE_STOPPED)
    return _Reply(frame, DBGCMD_ERROR);

  switch(in[0]){
  case INSTR_JMP: case INSTR_JZ: case INSTR_JNZ:
  case INSTR_CALL: case INSTR_RET: case INSTR_HALT:
    return _Reply(frame, DBGCMD_ERROR);
  default:
    break;
  }

  _FillHalt(dbg);
  for(int i = 0; i < len; i++)
    dbg->instr[i] = in[i];

  vm->PC = dbg->instr;
  status = PVM_ExecTick(vm);

  out[0] = (uint8_t)(int8_t)status;
  _Put16(out + 1, (uint16_t)(vm->SP - vm->stack));
  return _Reply(frame, DBGCMD_EXEC_INSTR);
}

/* Writes the header around data already in place at DATA_AT, then the CRC
 * after it; returns the frame's length. How the device answers in the very
 * buffer the request came in. */
static int _Reply(uint8_t* frame, uint16_t cmd){
  _Put32(frame, PVMDBG_SYNC);
  _Put16(frame + 4, cmd);
  _Put16(frame + CRC_AT, _Crc16(frame, CRC_AT));
  return PVMDBG_FRAME_SIZE;
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
