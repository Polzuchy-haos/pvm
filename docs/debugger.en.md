[Русский](debugger.md) | **English**

# The PVM debugger

Stepping through a program: stopping it, stepping an instruction at a time,
breakpoints, inspecting the stack. Three parts, talking in frames over any byte
stream:

- **`pvm/pvmdbg.c`** — the device side. It knows nothing about UARTs: it is
  handed a buffer holding a received frame and puts the reply in the same
  buffer. Compiled into the firmware along with the VM itself.
- **`pvmdbg`** — a session tool for the terminal. Two transports: hold the VM
  inside its own process (debugging on a desktop), or talk to a board over a
  serial port.
- **the VS Code extension** (`tools/vscode-pvm/`) — debugging `.hlc` inside the
  editor, on top of that same `pvmdbg` (see "In VS Code" below).

## The tool

```
pvmdbg build/bytecode/square.bin        # a VM inside the tool's own process
pvmdbg --port COM3                      # a board over a serial port, 115200 8N1
pvmdbg --port COM3 --map blink.dbgmap   # plus source lines, from a debug map
pvmdbg --host square.bin --port COM3     # program on the host, the board only executes
pvmdbg --tcp 192.168.1.20:5555           # a board over TCP, through a bridge to its UART
```

A port and a file together make no sense: a board already holds its own
program, flashed into it (to run a program the board was never flashed with,
use `--host`, below). The session opens stopped (the tool sends `PAUSE`
immediately) and lives until `exit` or `Ctrl+C`.

Commands, output and behaviour are the same for both transports — the whole
session is written against the protocol and knows nothing about where the VM
actually runs. There is exactly one difference, and it is in `settle`: locally
the tool turns the tick loop itself, whereas a board runs the program on its
own, and after `run` the host can only keep asking `STATE` until it stops (the
protocol has no events from the device).

`Ctrl+C` means two different things depending on what is happening, as it does
in gdb: while a program is running it stops it and returns you to the prompt;
at the prompt it ends the session. There is no limit on how long a `run` may
take: firmware is usually a loop that never ends, and hitting a timeout on
every single `run` is pointless. Polling starts brisk (10 ms), so a breakpoint
about to be hit is reported at once, and backs off to 250 ms — every question
costs the board a reply it has to stop and send. Silence across several
questions in a row is an unplugged cable or a reset, and the tool says so.

| Command | Short | What it does |
|---|---|---|
| `run` | `r` | run until a breakpoint or the end of the program |
| `step` | `s` | one instruction |
| `pause` | `p` | stop |
| `reset` | | rewind PC to the start, empty the stack, stay stopped |
| `state` | `i` | where PC, SP and FP are, and why we stopped |
| `stack [n]` | | the top `n` words of the stack (8 by default) |
| `mem` | `m` | stack and RAM sizes as the device reports them, and how much stack is in use |
| `dis` | `d` | disassemble from the current PC |
| `break <where>` | | breakpoint: `break 0x12` or `break blink.hlc:21` |
| `clear <addr>` | | remove one |
| `breaks` | `b` | which ones are set |
| `exec <code>` | | run code on the device without reflashing (see below) |
| `abort` | | drop a snippet that has not finished, back to the program |
| `help` | `?` | this list |
| `exit` | `quit`, `q` | end the session |

Addresses come from the memory map in the listing (`pvmc … -l file.lst`, see
`docs/pvms-language.en.md`), which says where each function begins. Usually you
will not need addresses at all — see the next section.

### Source instead of addresses

Given a debug map (`pvmc … -g file.dbgmap`), the tool prints the source line at
every stop, and breakpoints can be set on lines:

```
(pvmdbg) break blink.hlc:22
  0x000F will stop the program
(pvmdbg) run
  pc=0x000F  sp=1  fp=1  breakpoint
  -> 0x000F  0D 00 40   LOAD 0x4000
     blink.hlc:22          LED = LED ^ 1;
```

- **Locally** the map is found on its own, next to the `.bin` (which is where
  `pvmc -g` puts it). Without one you simply get no source lines; nothing
  breaks.
- **Over a port** the map has to be named explicitly (`--map`): the program
  lives in the board's flash, the host has never seen it, and there is nothing
  to guess from. Making sure the map comes from the same build as the firmware
  is up to you.
- An address between two map entries belongs to the earlier one: a source line
  usually compiles to several instructions, and only the first carries a
  marker.
- A source file that is not on this machine is not an error: `file:line` is
  printed without the text. That is what you get when someone else built the
  firmware.

```
(pvmdbg) break 0x12
  0x0012 will stop the program
(pvmdbg) run
  pc=0x0012  sp=4  fp=1  breakpoint
  -> 0x0012  1A         ENTER
(pvmdbg) stack
  [0] 10      <- top
  [1] 7
(pvmdbg) dis
  -> 0x0012  1A         ENTER
     0x0013  1C FD FF   LOADFP -3
     0x0016  1C FD FF   LOADFP -3
     0x0019  06         MUL
```

The instruction under `PC` is printed by itself after every stop — otherwise
every single step would mean looking the address up in a listing. The bytes for
it are read **from the device** (the `READ_CODE` command) rather than from a
local copy of the program: over a port the host has no copy at all, and where
it does have one it may be a rebuild ahead of what is actually flashed, leaving
the disassembly quietly disagreeing with the machine. The bytes are decoded by
the same disassembler `pvmdisasm` uses; there is no second table of mnemonics.

Peripherals and SVC handlers are the same as `pvmsim`'s (they share
`simulator/pvmenv.c`), so a program behaves the same under the debugger as it
does in an ordinary run. Locally, a program with no breakpoints and no `HALT`
will not hang the tool: after two million instructions the session pauses
itself (on a board, `Ctrl+C` does that job).

### Running code without reflashing

`exec` uploads a small piece of bytecode to the device (up to 64 bytes,
`PVMDBG_CFG_EXEC_SIZE`) and runs it in the middle of the stopped program:

```
(pvmdbg) exec PUSH 1234; STORE 0
  pc=0x0004  sp=2  fp=1  snippet finished, back in the program
  -> 0x0004  00 07 00   PUSH 7
     square.hlc:12      RESULT = square(7);
  snippet left nothing on the stack
(pvmdbg) exec LOAD 0; PUSH 1; ADD
  ...
  snippet left on the stack, top first: 1235
```

- **Where the code comes from.** A line of instructions separated by `;`
  (`exec LOAD 3; PUSH 1; ADD`), a `.pvms` file (`exec snippet.pvms`), or a
  ready `.bin` from `pvmc -f bin`. It is assembled by the same assembler as
  `pvmc`; an inline line is checked against the RAM and stack the device
  reports (`mem`), not against the defaults. No `HALT` is needed at the end —
  the agent fills the buffer after the code with `HALT`s, so a snippet cannot
  run off its end.
- **What the snippet sees.** The program's stack, RAM, usrsect and SVCs: read
  or change a variable, poke a peripheral, try out an expression. Jump
  addresses inside a snippet count from its start; a snippet cannot `CALL` a
  function of the program — a jump outside the buffer is refused.
- **Afterwards.** On the snippet's `HALT` the agent gives the program back its
  `PC`, `SP` and `FP`, and it carries on as if nothing had happened. What was
  written to RAM and peripherals stays written. Whatever the snippet left on
  the stack above the original `SP` is its answer: the agent keeps up to the
  top 8 words, and the tool prints them. If the snippet popped the program's
  own values (below the original `SP`), they stay clobbered.
- **If it does not finish.** An execution error or an endless loop leaves the
  VM inside the snippet (`[in snippet]` on the state line); from there `step`,
  `run`, `dis` and `stack` work as they do in the program, and `abort` drops
  the snippet and returns to the program. `reset` leaves the snippet first,
  too. Breakpoints do not apply to a snippet, and while one is running they are
  still set by program address.
- A snippet can only be started from a stopped program.

### Over TCP (`--tcp`)

```
pvmdbg --tcp 192.168.1.20:5555 --map blink.dbgmap   # a board behind a bridge on the network
pvmdbg --tcp 5555                                   # just a port means this machine
pvmdbg --host square.bin --tcp 5555                 # --host works too
```

TCP carries exactly the frames the serial port does, byte for byte, so a
bridge between the network and a board's UART needs to know nothing about the
protocol: it moves bytes both ways (one connection is one board, as with a
UART). Picking frames out of the stream, waiting for a reply and polling a
running board are shared by both transports (`debugger/transport_stream.c`),
so the two cannot drift apart.

- If nothing is listening yet, `pvmdbg` keeps trying to connect for 3 seconds —
  a bridge started alongside the debugger has time to come up.
- `TCP_NODELAY` is on: frames are small and each waits for an answer, and with
  Nagle's algorithm a request could sit in the stack for tens of milliseconds.
- Whatever a bridge buffered before the connection is thrown away — like
  `PurgeComm` on a serial port.

**`pvmboard`** is a board that is a process, for working without hardware: the
VM and the agent run just as in firmware, with a TCP socket where the UART would
be. Peripherals and SVCs are `pvmsim`'s.

```
pvmboard build/bytecode/square.bin --paused     # listens on 127.0.0.1:5555
pvmdbg --tcp 5555 --map build/bytecode/square.dbgmap
```

`--listen [host:]port` says where to accept the debugger (by default only
`127.0.0.1`; `0.0.0.0:port` opens it to other machines), `--paused` holds the
program until the debugger's first command, `--once` exits when the debugger
disconnects. The TCP tests (`run_pvmdbg_tcp`) run against it.

### The program on the host (`--host`)

```
pvmdbg --host build/square.bin              # the "device" is the VM in this process
pvmdbg --host build/square.bin --port COM3  # a board the program was never flashed to
```

The whole program stays on the host; nothing is flashed. The host is the
processor's front end: it holds `PC`, fetches each instruction, decodes it and
decides what to do with it. The device is the execution unit: it is sent one
instruction at a time (`EXEC_INSTR`), executes it on its own stack, RAM,
peripherals and SVCs, and answers with the status — so an execution error is
seen at once, with its code and address.

| Instruction | Executed by |
|---|---|
| data: `PUSH`, arithmetic, `LOAD`/`STORE`, `ENTER`/`LEAVE`, `LOADFP`/`STOREFP`, `SVC`, … | the device |
| `JMP` | the host: it just moves `PC` |
| `JZ`/`JNZ` | the host reads the top of the device's stack, the device `POP`s it, the host jumps or not |
| `CALL` | the prologue: the device is sent `PUSH <return address>` (the same word `CALL` itself would push, so frames, `ENTER`/`LEAVE` and `LOADFP` offsets come out exactly as compiled), and `PC` moves into the function |
| `RET` | the host reads the return address off the device's stack, the device `POP`s it, and execution carries on after the `CALL` |
| `HALT` | the end |

The commands are the same: `step`, `run`, `state`, `dis`,
`break`/`clear`/`breaks` (breakpoints are now the host's, with no four-slot
limit), `reset` (`PC` to 0, the device's stack emptied); `stack`, `mem` and
`exec` still go to the device. Plus `call` — run one function and stop:

```
(pvmdbg) call square 0 9
  pc=0x0011  sp=0  fp=0  function returned  [host: main]
  ...
  it left on the stack (arguments included), top first: 9 81
```

The words after the name are pushed in the order given, the way the calling
code would push them (a result slot is an argument too, if the calling
convention has one), followed by a special return address, `0xFFFF`. A `RET` to
it means the function has returned: whatever is above the stack depth it
started at is printed and popped, and `PC` goes back where it was. Function
names come from the `pvmc -l` listing next to the `.bin`; without one, give an
address (`call 0x12 0 9`).

The price: every instruction is one frame exchange. Locally that is
unnoticeable; over a serial port at 115200 it is a few hundred instructions a
second — plenty for debugging, not for real work. Machine mode (`--machine`)
and VS Code do not support this mode yet.

## In VS Code

Breakpoints in the editor gutter, F5/F10/F11, the current line highlighted, the
stack in the variables pane. Installation and `launch.json` are described in
`Release/vscode-extension/README.en.md`.

The extension does **not** talk to the board itself: it runs
`pvmdbg --machine` and exchanges lines with it. Everything hard or dangerous —
the frame format, the CRC, the serial port, resynchronising a byte stream,
tracking breakpoints on the device — stays in C, where it is already proven
against hardware. A second implementation of the wire format in JavaScript is
exactly the thing that would drift.

The machine mode is line-based, one command and one reply per line:

```
> state          < ok pc=39 sp=11 fp=9 mode=0 reason=3
> break 39       < ok
> run            < ok
> poll           < ok running=1
> poll           < ok running=0
> stack 4        < ok n=2 v0=3 v1=5
> mem            < ok stack=1024 ram=64
```

Deliberately not JSON: only numbers cross this boundary, so `key=value` pairs
need no parser on either side. File names never cross it at all — the extension
holds the debug map, which it needs anyway to know which file to highlight.

`run` only sets the program going; `poll` lets it advance and says whether it
has stopped. They are split so that pausing stays possible: a single blocking
"run until it stops" would leave an endless loop unstoppable, and a background
thread in a tool this size is one more thing to break.

Stepping in the editor is by source line rather than by instruction: a single
instruction usually leaves the highlight where it was, and F10 would look
broken. F10 stays out of calls (while the frame pointer is deeper than it
started, keep going); F11 stops at the first change of line.

What the extension does not have yet: a multi-frame call stack (one frame is
shown) and local variable names — the stack is shown as values. And no more
than four breakpoints: that is how many slots the device-side agent has.

## The device side

The firmware's main loop looks like this (there is a sketch in
`pvm/pvmdbg.h` too):

```c
static PVM_Word_t vm_stack[PVM_CFG_STACK_SIZE];
static PVM_Word_t vm_ram[PVM_CFG_RAM_SIZE];

PVM_Init(&vm, code, len, vm_stack, PVM_CFG_STACK_SIZE, vm_ram, PVM_CFG_RAM_SIZE);
PVMDBG_Init(&dbg, &vm);

while(1){
  /* receive a frame into buf, len = how many bytes arrived */
  PVMDBG_Poll(&dbg, buf, &len);
  if(PVMDBG_IsAccessExecTick(&dbg))
    PVM_ExecTick(&vm);
  /* if len > 0, send buf back out */
}
```

The stack and RAM are not part of `PVM_State_s`: they are pools the firmware
allocates (usually as static arrays of the size it wants) and hands to
`PVM_Init`; the VM state only keeps pointers to them and their lengths in
words. That lets one firmware give a small program a small stack without
rebuilding the VM, and lets the debugger read the actual sizes with `MEM_INFO`
(`mem` in the tool). `PVM_Init` zeroes both pools and returns
`ERRORCODE_BAD_CONFIG` if there is no stack, the stack is longer than `0x7FFF`
words, or RAM runs into the usrsect at `0x4000`.

`PVMDBG_Poll` parses the frame, carries out the command and puts the reply in
the same buffer (`len` on the way in is how much arrived, on the way out how
much to send, `0` meaning "nothing to answer"). Commands do not execute the
program themselves: all they change is what `PVMDBG_IsAccessExecTick` answers
on the following turns of the loop.

Three things in that logic are not obvious but matter:

- **`RUN` from a breakpoint.** If `PC` is sitting exactly on an armed
  breakpoint, it has to be skipped once — otherwise the loop reports the same
  stop forever and the program never advances a single instruction.
- **`STEP` ignores breakpoints.** One instruction was asked for, so it runs,
  even if we are standing on a breakpoint.
- **`HALT` is a reason to stop.** Executing it again changes nothing, and the
  loop would spin for no purpose. `PC` stays on the `HALT`, which is exactly
  where a debugger wants to show it.

By default `PVMDBG_Init` leaves the VM running: a board powered up with nothing
attached to its debug port has to work. The tool sends `PAUSE` the moment it
connects.

## The protocol

The frame, little-endian throughout:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `sync` = `PVMDBG_SYNC` |
| 4 | 2 | `cmd` (`PVMDbgCmd_e`) |
| 6 | 1 | `len` — how many payload bytes follow |
| 7 | `len` | `data`, command-specific |
| last 2 | 2 | `crc16` (CCITT) over everything before it |

The `len` field is not decoration: it is what makes a frame findable in a
stream. A port hands over whatever has arrived, and without a length "the frame
ended here" is indistinguishable from "the rest has not turned up yet". Given
the bytes accumulated so far, `PVMDBG_FrameLength` answers: the length of a
complete frame, `0` for "wait for more", or `-1` for "there is rubbish at the
front, drop a byte and ask again". That is how the stream resynchronises after
line noise.

The frame is read and written byte by byte rather than by casting the buffer to
a `PVMDbgFrame_s*`: the buffer carries no alignment guarantee, and a `uint32`
read from an odd address on Cortex-M0 is a HardFault (the same reason `pvm.c`
has `_FetchWord`). Building and parsing frames
(`PVMDBG_FrameBuild`/`PVMDBG_FrameParse`) live in the header and are used by
both ends, so two implementations of one format cannot drift apart.

| Command | Request data | Reply data |
|---|---|---|
| `PING` `0x01` | — | — |
| `STATE` `0x02` | — | `pc`, `sp`, `fp` (2 bytes each), `mode`, `reason`, `exec` (1 while the VM is in a snippet, `pc` then being an offset into it) |
| `READ_STACK` `0x03` | `count` (1 byte) | `n`, then `n` words, top first |
| `READ_CODE` `0x04` | address (2 bytes) + `count` (1 byte) | `n`, then `n` bytes of bytecode |
| `MEM_INFO` `0x05` | — | stack size, RAM size (2 bytes each, in words) |
| `RUN` `0x10` | — | — |
| `PAUSE` `0x11` | — | — |
| `STEP` `0x12` | — | — |
| `RESET` `0x13` | — | — |
| `BRKP_SET` `0x20` | address (2 bytes) | — |
| `BRKP_CLR` `0x21` | address (2 bytes) | — |
| `BRKP_LIST` `0x22` | — | `n`, then `n` addresses |
| `EXEC_WRITE` `0x30` | offset (1 byte), then bytecode | — |
| `EXEC_RUN` `0x31` | length (1 byte) | — |
| `EXEC_STATUS` `0x32` | — | state (`PVMDbgExec_e`), `n`, then the `n` words the snippet left, top first |
| `EXEC_ABORT` `0x33` | — | — |
| `EXEC_INSTR` `0x34` | an instruction (1 or 3 bytes; jumps, `CALL`/`RET`/`HALT` are refused) | `PVM_ExecTick`'s status (1 signed byte), `sp`, `fp` |
| `ERROR` `0x7F` | — | only ever sent by the device, in place of a command it could not carry out |

A frame that fails its CRC gets no reply at all: the `cmd` field of a corrupt
frame cannot be trusted either, so there is nothing to answer. Command codes
are written out explicitly and with gaps between the groups — they travel
between a host and a firmware that are built separately.

## What is not there yet

- **Events from the device.** The host learns about a stop by asking `STATE`;
  the device never volunteers anything. An event would turn the polling in
  `run` into a wait.
- **Execution errors over the protocol.** The `ERRORCODE_*` values from
  `PVM_ExecTick` are seen only by the loop; they do not reach `STATE`, so in a
  session against a real board such an error is invisible. The exception is
  `--host` mode, where every instruction comes back with its status.
- **A multi-frame call stack.** Both the terminal tool and the extension show
  one frame — where execution currently is. Walking the real stack means
  reading saved frame pointers back out of the device and working out which
  function each return address belongs to.
- **Local variable names.** The VM stack is shown as values; mapping frame
  offsets back to the names in the `.hlc` source would need another table
  alongside the line map.
