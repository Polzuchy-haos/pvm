[Русский](pvms-language.md) | **English**

# The PVM assembly language (`.pvms`)

The text format `pvmc` assembles into PVM bytecode. One file is one instruction
stream; control flow, functions and data are all done with labels, `CALL`/`RET`
and fixed RAM/peripheral addresses.

The VM's default configuration (see `pvm/pvm.h`): a 1024-word data stack
(`PVM_CFG_STACK_SIZE`), 64 words of RAM (`PVM_CFG_RAM_SIZE`, addresses
`0..63`), peripherals/usrsect from `0x4000` upwards (`PVM_CFG_USRSECT_BGN`). A
word is a signed 16-bit value (`PVM_Word_t` = `int16_t`). If the target device
is configured differently, the sizes are declared in the source itself with the
`.code`/`.ram`/`.stack`/`.usrsect` directives (below). The stack and RAM pools
themselves are allocated by the firmware and handed to `PVM_Init`, so the
actual sizes on a board are the firmware's to choose; the debugger's `mem`
command shows them (see `docs/debugger.en.md`).

## File syntax

```
# a line comment -- runs to the end of the line

.entry main          # which label the program actually starts at
.import modulefile    # splice in another .pvms file, textually
.const NAME 0x4000    # a named constant
.var counter 0 1      # a variable: name, address, size in words
.line 21 blink.hlc    # the next instruction came from this .hlc line

.code    8192         # bytes available for bytecode
.ram     64           # words of RAM on the target VM
.stack   1024         # words in the data stack
.usrsect 0x4000 8     # base and length of the peripheral section

label:                 # a label -- begins a new code block
  MNEMONIC              # an instruction with no operand
  MNEMONIC 123           # an instruction with one (a number, hex, a .const name or a label)
```

- A **label** is an identifier (letters/digits/`_`) alone on a line, ending in
  `:`. It begins a new code block and can be referenced as an address in the
  operand of `JMP`/`JZ`/`JNZ`/`CALL`/`LOAD`/`STORE` and so on.
- **`.const NAME value`** — a named integer constant (decimal or `0x...`).
  Substituted everywhere `NAME` is used as an operand.
- **`.var NAME ADDRESS SIZE`** — the same as `.const NAME ADDRESS` as far as
  operands are concerned, but it also says this is a **variable** of a known
  extent: it appears in the listing's memory map, and its whole range (not just
  its first word) is checked against the data section. This is what `pvmcc`
  emits for every global variable.
- **`.line NUMBER path`** — ties the **next** instruction to a line of `.hlc`
  source. It occupies no bytecode; the path is the rest of the line, unquoted
  (as with `.import`): Windows paths are full of backslashes and can contain
  spaces, and quoting rules would be one more thing to get wrong in a file no
  human writes by hand. The mark lives on the instruction rather than on an
  address, so it travels with it when `.entry` reorders the blocks. `pvmcc`
  emits these; `pvmc -g` collects them into a map (below).
- **`.import file`** — splices the contents of another `.pvms` file in at this
  point, textually (like `#include`). The path is looked for next to the
  importing file: the exact name first, then with `.pvms` appended. Works
  recursively, with cycle detection.
- **`.entry label`** — moves the **whole contiguous run of code** from `label`
  to the end of the file (including labels inside it — those are just local
  jump targets, not separate functions) to address `0`; everything that came
  before it in the file follows after. Without `.entry`, code runs in the order
  it was written, from address `0`.
- An instruction's operand is resolved in this order: a `.const` name → a label
  name → a number (decimal or `0x...`, optionally signed with `-`).

### Section sizes and bounds checking

`.code`, `.ram`, `.stack` and `.usrsect` describe the memory of the **target**
VM. They have exactly one purpose: to turn what would otherwise be a runtime
error (`ERRORCODE_BAD_DADR`/`ERRORCODE_BAD_IADR`, somewhere in the middle of
the device doing its job) into an assembly error.

| Directive | Units | What it checks |
|---|---|---|
| `.code N` | bytes | the final program size; `JMP`/`JZ`/`JNZ`/`CALL` targets |
| `.ram N` | words | `LOAD`/`STORE` addresses (`0..N-1` are valid) |
| `.usrsect BASE LEN` | words | `LOAD`/`STORE` addresses in the peripheral section |
| `.stack N` | words | `LOADFP`/`STOREFP` offsets |

- Anything undeclared comes from `pvm/pvm.h`, so a file without these
  directives assembles exactly as it did before they existed. The one exception
  is usrsect's length: `pvm.h` has no constant for it (the VM learns it at
  runtime, from `PVM_UsrSect`), so without an explicit `.usrsect` any address
  from the base upwards is accepted. Checking that section is opt-in.
- Only immediate operands are checked. `LOADI`/`STOREI` take their address off
  the stack at runtime, where nothing is known about it statically — that stays
  a runtime concern. `.stack` is similar: where the frame sits is a runtime
  property, so all that can be caught is an offset that could not land inside
  the stack from any frame at all.
- Declaring the same section twice with the same value (via `.import`, say) is
  fine; declaring it with a different one is an error.
- Separately, an operand is checked to fit the 16-bit field at all: `PUSH
  100000` used to silently lose its high bits.

A worked example using all four directives is `example/sections_demo.hlc` (at
the `.hlc` level, see `docs/hlc-language.en.md`); examples of what no longer
assembles are in `example/bounds/`.

### The debug map (`-g`)

`pvmc … -g file.dbgmap` collects every `.line` directive into a flat
"address → file and line" table, one entry per line, in address order:

```
0x0000 F:/Project/pvm/example/blink.hlc:19
0x0009 F:/Project/pvm/example/blink.hlc:21
0x0027 F:/Project/pvm/lib/time.hlc:14
```

The addresses here are the final ones, after `.entry` has reordered the blocks.
The file name is kept beside the number for a reason: `import` splices one
file's statements into another, and "line 14" without a file would name a place
in whichever file the reader happened to assume. `pvmdbg` reads this map to
show source and to put breakpoints on lines — see `docs/debugger.en.md`.

### The memory map in the listing

The listing (`pvmc … -l file.lst`) opens with a memory map: how much of each
section is in use, which address each function starts at, and where every
variable lives.

```
# ---- memory map ----
#
# code    : 84 of 512 bytes
# ram     : 1 of 16 words
# stack   : 256 words
# usrsect : 0x4000..0x4007 (8 words)
#
# functions:
#   0x0000  main
#
# variables:
#   0x0000  total                  1 word   ram
#   0x4000  PERIPH_FIRST           1 word   usrsect
```

- Under **functions**, only labels that something `CALL`s are listed, plus the
  entry point: compiler-generated labels (`if_end_7` and friends) would bury
  them, and they are visible in the body of the listing anyway.
- **Variables** are the `.var` entries; a plain `.const` goes to a separate
  `constants` section, because a number need not be an address.
- `ram: N of M` is not the sum of the sizes but the high-water mark: variables
  pinned to fixed addresses can leave gaps.

## The instruction set

Every instruction is one opcode byte; those taking an operand add two more
(little-endian). Below, `a`/`b` are what was pushed earlier/later
(`PUSH a; PUSH b; OP` ⇒ an operation of the form `a OP b`, unless stated
otherwise).

### Stack

| Mnemonic | Operand | Effect |
|---|---|---|
| `PUSH n` | number | put `n` on the stack |
| `POP` | — | drop the top |
| `DUP` | — | duplicate the top |
| `SWAP` | — | exchange the top two |

### Arithmetic and bits

| Mnemonic | Effect |
|---|---|
| `ADD` `SUB` `MUL` `DIV` | `a + b`, `a - b`, `a * b`, `a / b` |
| `AND` `OR` `XOR` | `a & b`, `a \| b`, `a ^ b` |
| `SHL` `SHR` | `a << b`, `a >> b` (`a` is the value, `b` the shift amount) |
| `NOT` | `~top`, without popping (inverts in place) |
| `EQ` | `a == b` → `1`/`0` |
| `LT` | `a < b` → `1`/`0` |

The VM has no `>`/`<=`/`>=`/`!=` of its own — they are built from `LT`/`EQ`
(see the table in `hlc/codegen.cpp`; this is exactly what the high-level
compiler does):

```
a > b   ⇒  PUSH b; PUSH a; LT
a <= b  ⇒  PUSH b; PUSH a; LT; PUSH 0; EQ
a >= b  ⇒  PUSH a; PUSH b; LT; PUSH 0; EQ
a != b  ⇒  PUSH a; PUSH b; EQ; PUSH 0; EQ
```

### Jumps

| Mnemonic | Operand | Effect |
|---|---|---|
| `JMP addr` | address | unconditional jump |
| `JZ addr` | address | pop the top, jump if it was `0` |
| `JNZ addr` | address | pop the top, jump if it was not `0` |

### Memory

| Mnemonic | Operand | Effect |
|---|---|---|
| `LOAD addr` | address (immediate) | push the value at `addr` in RAM/peripherals |
| `STORE addr` | address (immediate) | pop the top, write it to `addr` |
| `LOADI` | — | pop an address, push the value at it (indirect read) |
| `STOREI` | — | pop an address (top), pop a value (next), write it |

An address below the RAM size is a RAM cell; an address at or above `0x4000`
(and within the registered peripheral section) reaches the peripherals through
`PVM_UsrSect`. Anything else is the runtime error `ERRORCODE_BAD_DADR`. Both
bounds are configurable — see "Section sizes" above.

The order for `STOREI` is `PUSH value; PUSH addr; STOREI` — the address goes
last, because it has to be on top.

### Functions

| Mnemonic | Operand | Effect |
|---|---|---|
| `CALL addr` | address | push the return address, jump to `addr` |
| `RET` | — | pop the top as an address and jump there |
| `ENTER` | — | prologue: save the current `FP`, `FP := SP` (open a frame) |
| `LEAVE` | — | epilogue: `SP := FP`, restore `FP` (leaving the return address on top for `RET`) |
| `LOADFP offset` | signed number | push `FP[offset]` |
| `STOREFP offset` | signed number | pop the top into `FP[offset]` |

`CALL`/`RET` on their own (without `ENTER`/`LEAVE`) are all a function with no
arguments and no frame needs. `ENTER`/`LEAVE`/`LOADFP`/`STOREFP` come in when a
function takes arguments on the stack, returns a value on the stack, or uses
local variables. The convention (visible done by hand in
`example/test_frame_pointer.pvms`, and generated automatically by `pvmcc` — see
`docs/hlc-language.en.md`):

```
main:                       func:
  PUSH 0    ; result slot     ENTER
  PUSH 7    ; argument x      LOADFP -3    ; x
  CALL func                   LOADFP -3    ; x again
  POP       ; drop the copy   MUL
  STORE R                     STOREFP -4   ; result -> the reserved slot
  HALT                        LEAVE
                              RET
```

After `ENTER`: `FP[-1]` is the saved `FP`, `FP[-2]` the return address,
`FP[-3]` the last argument pushed, ..., `FP[-(2+N)]` the first argument, and
`FP[-(3+N)]` the slot the caller reserved for the result (if one is wanted).
Local variables sit at positive offsets `FP[0]`, `FP[1]`, ... in declaration
order. Every `LOADFP`/`STOREFP` checks that the address falls inside the stack;
if not, `ERRORCODE_BAD_DADR`.

### Calls into C (SVC)

| Mnemonic | Operand | Effect |
|---|---|---|
| `SVC` | — | pop the call number, invoke `svc_table[number]` |

The call number is passed **on the stack** rather than as an immediate:
`PUSH number; SVC`. A handler is an ordinary C function
`int (*)(PVM_State_s* state)`, registered in advance with
`PVM_RegisterSvc(state, table, count)` (much like `PVM_UsrSect` for
peripherals). Inside, the handler pops its own arguments and pushes its own
result with `PVM_Pop`/`PVM_Push` — no `ENTER`/`LEAVE`-style convention is
involved, because this is an ordinary synchronous call into C, not a PVM
function. An unknown or unregistered number is the runtime error
`ERRORCODE_BAD_SVC`.

```
PUSH 42   ; argument for the handler
PUSH 1    ; call number: SVC #1 (Svc_Print in simulator/pvmenv.c)
SVC       ; the handler pops 42 with PVM_Pop
```

`simulator/pvmenv.c` currently registers `SVC #0` — `Svc_Tick` (a system time
tick, see `lib/time.hlc`), `SVC #1` — `Svc_Print` (print a number), and
`SVC #2` — `Svc_Add` (the sum of two numbers). That is one particular table,
shared by the simulator and the debugger, not part of the language: on a real
MCU the numbers and handlers will be your own.

See `example/test_svc.pvms` and the `Svc_Print` handler in
`simulator/pvmenv.c`.

### Halting

| Mnemonic | Effect |
|---|---|
| `HALT` | stop execution (`pvmsim` prints `pvmsim: halted` and exits with code 0) |

## Where to look next

- `example/bytecode_listing.pvms` — labels, `.const`, `.entry`, peripherals, a loop.
- `example/test_new_instructions.pvms` — bitwise operations, `LOADI`/`STOREI`, `HALT`.
- `example/test_frame_pointer.pvms` — `ENTER`/`LEAVE`/`LOADFP`/`STOREFP` by hand.
- `example/test_svc.pvms` — calling into C through `SVC`.
- `docs/hlc-language.en.md` — how `pvmcc`, the high-level compiler, automates all of this.
