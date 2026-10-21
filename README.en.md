[Русский](README.md) | **English**

# PVM — Pico Virtual Machine

A small stack virtual machine designed specifically for microcontrollers and
embedded systems: a minimal C core (no dynamic allocation, meant to be compiled
straight into a firmware image), its own assembler, and a small C-like
high-level language that compiles to the same bytecode.

> **Developed with AI.** A substantial part of the code in this repository was
> written together with an AI assistant (Claude): architectural decisions were
> made by a human, the implementation, tests and documentation jointly.
> Everything touching hardware was verified on a real STM32G071 board, not only
> in the simulator.

## Repository layout

- [`pvm/`](pvm/) — the VM core (`pvm.h`/`pvm.c`): the stack, the full
  instruction set (arithmetic, bitwise, jumps, memory), PVM function calls via
  `ENTER`/`LEAVE`/`LOADFP`/`STOREFP`, and calls into C via `SVC`. This is the
  only part of the project that actually ends up on the MCU — it compiles as
  is, unchanged.
- [`compilator/`](compilator/) — the low-level assembler `pvmc`
  (`.pvms` → bytecode: raw `bin`, Intel HEX for flashing, a C array for
  embedding without a filesystem, and a listing with a memory map showing
  where every function and variable lives) and the disassembler `pvmdisasm`.
- [`hlc/`](hlc/) — `pvmcc`, the compiler for a small C-like high-level language
  (`.hlc` → `.pvms` text, which then goes through `pvmc` unmodified).
  Functions, local variables, `if`/`while`/`for`, global arrays, MMIO
  variables, `import`, `extern` (calling hand-written assembly), `svc(...)`
  (calling into C), dead code elimination, and target memory sizes declared
  with `section` — overrunning a section is caught at compile time rather than
  at runtime on the device, worst-case stack depth included.
- [`simulator/`](simulator/) — `pvmsim`: runs compiled bytecode and traces
  every instruction. `pvmenv.c` here holds the demonstration `SVC` handlers
  (time, print, add) and the peripheral block, shared with the debugger.
- [`debugger/`](debugger/) — `pvmdbg`: an interactive debugging session in the
  terminal — stepping, breakpoints on `.hlc` source lines, stack inspection,
  disassembly. It talks to the device side (`pvm/pvmdbg.c`, compiled into the
  firmware) in protocol frames; see [`docs/debugger.en.md`](docs/debugger.en.md).
- [`pvminit/`](pvminit/) — `pvminit`: the build and debug script generator. It
  gives a project of your own a ready `Makefile` and `.vscode/` (`make run`,
  `make debug`, F5 in the editor) with the tool paths already filled in; see
  [`docs/project-generator.en.md`](docs/project-generator.en.md).
- [`tools/vscode-pvm/`](tools/vscode-pvm/) — the VS Code extension: syntax
  highlighting and `.hlc` debugging inside the editor (breakpoints in the
  gutter, F5/F10/F11, the VM stack in the variables pane), on top of the same
  `pvmdbg`.
- [`lib/`](lib/) — a small standard library for `.hlc` (`mathlib.hlc`,
  `time.hlc` — timekeeping that handles counter wraparound correctly).
- [`example/`](example/) — example programs in both languages, from a simple
  blinking LED to calling an assembly function from high-level code.
- [`docs/`](docs/) — the language and tool manuals:
  [`pvms-language.en.md`](docs/pvms-language.en.md) (assembler),
  [`hlc-language.en.md`](docs/hlc-language.en.md) (high-level language),
  [`debugger.en.md`](docs/debugger.en.md) (the debugger and its protocol),
  [`project-generator.en.md`](docs/project-generator.en.md) (the script
  generator).
- [`Release/`](Release/) — the folder meant to be handed over: the built tools,
  the VM core in source, `lib/`, the documentation and the VS Code extension.
  It is assembled entirely by `make` and is not kept in git (apart from its own
  [README](Release/README.en.md)) — everything in it already exists in this
  repository, in source form.

## Building

```
make            # or: cmake -S . -B build && cmake --build build
make test       # ctest -- runs every example and checks the results
make run        # build and run the simulator on the default example
```

## Example

```c
// blink.hlc
import time;

volatile int LED @ 0x4000;

void main() {
    LED = 0;
    while (1) {
        LED = LED ^ 1;
        Time_Delay(1);
    }
}
```

```
pvmcc blink.hlc -o blink.pvms
pvmc blink.pvms -o blink.bin
pvmsim blink.bin
```

## A project of your own

Rather than writing those calls out by hand, a project of your own is set up by
the generator, which produces a `Makefile` and a `.vscode/` with the tool paths
already filled in:

```
pvminit myproject --port COM6
cd myproject
make run        # build it and run it in the simulator
make debug      # step through it; F5 in VS Code does the same
```

The details are in
[`docs/project-generator.en.md`](docs/project-generator.en.md).

For more on the two languages see [`docs/hlc-language.en.md`](docs/hlc-language.en.md)
and [`docs/pvms-language.en.md`](docs/pvms-language.en.md); for the tools
themselves, [`Release/README.en.md`](Release/README.en.md).
