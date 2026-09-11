[Русский](README.md) | **English**

# PVM — the built tools

Here are the assembled PVM toolchain binaries (Windows, statically linked
against libgcc/libstdc++, so they run without installing MinGW), and next to
them everything needed for this folder to stand on its own: the documentation,
the VM core in source, the standard library and the VS Code extension.

**The whole folder is assembled by `make`** (or `cmake --build build`) from the
project root — it is not kept in git, because everything in it already exists
in the repository in source form. The one exception is this README, which is
written by hand rather than copied in. If something is missing or out of date,
just rebuild.

## What is in here

| | |
|---|---|
| `*.exe` | the tools themselves — `pvmc`, `pvmcc`, `pvmsim`, `pvmdisasm`, `pvmdbg` |
| [`pvm/`](pvm/) | the VM core in source: what gets compiled into an MCU firmware |
| [`lib/`](lib/) | the `.hlc` standard library — `pvmcc` looks for it beside itself |
| [`docs/`](docs/) | the manuals for both languages and for the debugger |
| [`vscode-extension/`](vscode-extension/README.en.md) | the VS Code extension |

## Documentation

- [`docs/pvms-language.en.md`](docs/pvms-language.en.md) — the low-level
  assembler;
- [`docs/hlc-language.en.md`](docs/hlc-language.en.md) — the high-level C-like
  language;
- [`docs/debugger.en.md`](docs/debugger.en.md) — the debugger, its protocol,
  and what the firmware side needs.

References inside those texts to source files (`hlc/codegen.cpp`, `example/…`)
point into the repository: only the manuals themselves are copied here.

## pvm/ — the VM core for your firmware

This is the only part of the project that actually reaches the
microcontroller. It compiles as is, unchanged, with no dynamic allocation:

- `pvm.c` / `pvm.h` — the virtual machine itself;
- `pvmdbg.c` / `pvmdbg.h` — the debug agent: with it, `pvmdbg --port …` and the
  VS Code extension can debug a program running on the board. If you do not
  want it, simply leave those two files out of your build — the VM does not
  depend on them.

How to wire the agent into a firmware (the main loop, receiving frames from a
UART) is in [`docs/debugger.en.md`](docs/debugger.en.md).

## The pipeline

```
prog.hlc --[pvmcc]--> prog.pvms --[pvmc]--> prog.bin --[pvmsim]--> execution
                                         \-> prog.hex   (Intel HEX, for flashing an MCU)
                                         \-> prog.c     (a C array, to embed in a firmware)
                                         \-> prog.lst   (a listing: address + bytes + mnemonic)

prog.bin --[pvmdisasm]--> a disassembly (no source, bytecode only)
prog.bin --[pvmdbg]-----> stepping: breakpoints, single steps, stack inspection
```

If you write assembly (`.pvms`) directly, skip `pvmcc` and start at `pvmc`.

## pvmc — the assembler (`.pvms` → bytecode)

```
pvmc <input.pvms> [-o output] [-f bin|hex|carray] [-l [listing.lst]] [-g [map.dbgmap]]
```

- `-o output` — where to write the result. Without the flag: the input's name
  with a new extension (`.bin`/`.hex`/`.c`, depending on `-f`).
- `-f bin` (the default) — raw bytecode.
- `-f hex` — Intel HEX (`:LLAAAATT...CC` with a checksum), which is what MCU
  flashing tools understand.
- `-f carray` — a file containing
  `const uint8_t name[] = {...}; const unsigned name_len = N;`, to embed the
  bytecode in a firmware with no filesystem.
- `-l [listing.lst]` — also write a listing (address, the instruction's hex
  bytes, the original mnemonic and operand — with label and `.const` names
  rather than bare numbers). The path may be omitted, giving `<output>.lst`.
- `-g [map.dbgmap]` — a debug map, "address → `.hlc` file and line", which
  `pvmdbg` reads in order to show source. Empty if the `.pvms` has no `.line`
  directives (that is, for files written in assembly by hand).

Examples:
```
pvmc example.pvms                          # example.bin
pvmc example.pvms -f hex -o example.hex
pvmc example.pvms -f carray -o example.c
pvmc example.pvms -l example.lst           # example.bin + example.lst
```

The syntax of `.pvms` itself is in
[`docs/pvms-language.en.md`](docs/pvms-language.en.md).

## pvmcc — the high-level compiler (`.hlc` → `.pvms`)

```
pvmcc <input.hlc> [-o output.pvms] [-I path]...
```

One step of the pipeline only: it turns `.hlc` source into assembly text. The
result is an ordinary `.pvms`, which `pvmc` then handles (above). Without `-o`
the result is placed next to the source with a `.pvms` extension.

`import name;` in an `.hlc` file looks for `name`/`name.hlc` in this order:
next to the file being compiled → `lib/` next to this very `pvmcc.exe` (it is
already right here, in `Release/lib/`) → each `-I` path (the flag may be
repeated, and they are tried in the order given).

```
pvmcc program.hlc                        # program.pvms
pvmcc program.hlc -o out.pvms
pvmcc program.hlc -I ../common -I ../vendor
```

The language is described in
[`docs/hlc-language.en.md`](docs/hlc-language.en.md).

## pvmdisasm — the disassembler (bytecode → mnemonics)

```
pvmdisasm <input.bin> [-o output.txt]
```

Reads an already compiled `.bin` and prints
`address  hex bytes  MNEMONIC operand` for every instruction — with no source,
so label and constant names are not recovered (unlike `pvmc -l`'s listing, the
disassembler sees only bytes). An unknown opcode byte is printed as `DB 0xXX`
rather than aborting the decode. Without `-o` it prints to stdout.

```
pvmdisasm program.bin
pvmdisasm program.bin -o program.disasm.txt
```

## pvmsim — the simulator (runs bytecode)

```
pvmsim [input.bin]
```

Loads a `.bin` and runs it until `HALT`, an execution error, or a limit of
100000 steps (a guard against hung programs), tracing every instruction
(`address MNEMONIC operands -> result  SP N`). With no path given it tries to
run `bytecode_listing.bin` from the directory the build example sits in (which
only matters when running from inside the `build/` tree).

```
pvmsim program.bin
```

A program that reaches `HALT` exits with code 0 and the words
`pvmsim: halted`; a program with an execution error (a bad address, say) exits
non-zero with a message on stderr. A `while(1)` program (`blink`, for instance)
never reaches `HALT` at all — that is normal and intended.

## pvmdbg — the debugger (stepping through a program)

```
pvmdbg [program.bin]       # a VM inside the tool's own process
pvmdbg --port COM3         # a board over a serial port, 115200 8N1
pvmdbg --port COM3 --map prog.dbgmap    # plus source lines
```

Unlike `pvmsim`, which runs a program straight through, `pvmdbg` opens a
session and waits for commands. The program is stopped on its first
instruction.

`Ctrl+C` works as it does in gdb: while a program is running it stops it and
returns you to the prompt; at the prompt it ends the session (as does `exit`).

| Command | Short | What it does |
|---|---|---|
| `run` | `r` | run until a breakpoint or the end of the program |
| `step` | `s` | one instruction |
| `pause` | `p` | stop |
| `reset` | | rewind to the start, empty the stack |
| `state` | `i` | where PC, SP and FP are, and why we stopped |
| `stack [n]` | | the top `n` words of the stack |
| `dis` | `d` | disassemble from the current PC |
| `break <where>` | | breakpoint: `break 0x12` or `break blink.hlc:21` |
| `clear <addr>` | | remove one |
| `breaks` | `b` | which ones are set |
| `help` | `?` | this list |
| `exit` | `quit`, `q` | end the session |

```
(pvmdbg) break 0x12
  0x0012 will stop the program
(pvmdbg) run
  pc=0x0012  sp=4  fp=1  breakpoint
  -> 0x0012  1A         ENTER
(pvmdbg) stack
  [0] 10      <- top
  [1] 7
```

The instruction under `PC` is shown automatically after every stop; `dis`
prints the ones that follow. The bytes are read from the device, so the
disassembly shows what is actually running rather than the contents of a local
`.bin`.

If a debug map from `pvmc -g` sits next to the `.bin` (found automatically
locally, named with `--map` over a port), the source line is printed alongside
the instruction, and breakpoints can be set as `break blink.hlc:21`.

Addresses come from the memory map in the listing (`pvmc … -l file.lst`), which
says where each function starts and where each variable lives.

The `--port` form needs a firmware with `pvm/pvmdbg.c` (the debug agent)
compiled in, plus a layer that receives frames from a UART. A file is not
passed together with `--port`: a board already holds its own program. The
protocol and what the firmware side needs are in
[`docs/debugger.en.md`](docs/debugger.en.md).

## A complete example

```
pvmcc ..\example\square.hlc -o square.pvms
pvmc square.pvms -o square.bin
pvmsim square.bin
```
