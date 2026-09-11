[Русский](hlc-language.md) | **English**

# The PVM high-level language (`.hlc`)

A small C-like language that the `pvmcc` compiler turns into `.pvms` assembly
text (see `docs/pvms-language.en.md`), which then goes through `pvmc`
unmodified. The compiler works out the frame layout for you
(`ENTER`/`LEAVE`/`LOADFP`/`STOREFP`), places the labels for `if`/`while`/`for`,
and substitutes the addresses of global variables — none of that has to be done
by hand.

**Dead code elimination.** Only `main` and whatever it can reach through the
call graph end up in the generated `.pvms` (the graph is entirely static — the
language has no function pointers), and only those globals and arrays something
actually refers to keep their storage. Unused functions take no space in the
bytecode and unused globals take no RAM cell. That said, **every function is
compiled and checked for errors**, unreachable ones included — dead code is
dropped after checking, the way a linker discards unused sections, not by
skipping compilation. This matters most with `import`: you can pull in a whole
library file (`import mathlib;`) and use only part of it, and the rest never
reaches the firmware.

## import

```c
import mathlib;   // found in lib/ next to pvmcc (see the search order below)
```

By analogy with `.import` in `.pvms`: no quotes, no extension. The splice is
recursive and happens at the token level, before parsing, so declarations from
the included file become visible as if they were declarations of this one (and
fall under the same rules — a name declared twice across files is a
"redefined" error just the same). Circular imports are detected and are a
compile error. `import` may appear anywhere in a file, but by intent it is a
top-level directive, like `.const`/`.import` in `.pvms`.

A file that is only ever imported (like `lib/mathlib.hlc`) need not contain
`main()`: the `void main()` requirement is checked once, for the assembled
program as a whole.

### How a file is found

As with `#include` in C — in each directory, the exact name first, then the
name with `.hlc` appended, in this order:

1. **next to the file being compiled** (the directory of the `.hlc` that
   contains the `import`, not of the "root" file, if the import is nested);
2. **`lib/` next to the `pvmcc` executable** — the compiler asks the OS where
   it is (`GetModuleFileName` on Windows) rather than trusting `argv[0]`, so
   this works even when `pvmcc` was started by bare name through `PATH`. This
   is where `lib/` from the repository root ends up: CMake copies it next to
   the built `pvmcc.exe` automatically (`hlc/CMakeLists.txt`), and in
   `Release/` it already sits beside the binary;
3. **the paths given with `-I path`** — the flag may be repeated, and the
   directories are tried in the order given on the command line
   (`pvmcc prog.hlc -I ../common -I ../vendor`).

If the file is found nowhere, the error lists every directory that was tried.

## extern — calling a function written in assembly

```c
extern int fastmul(int a, int b) from fastmath;   // implemented in example/fastmath.pvms

void main() {
    RESULT = fastmul(6, 7);   // an ordinary call, like any other function
}
```

Declares a signature with no body. The compiler treats calls to `fastmul(...)`
exactly as it treats calls to a normal `.hlc` function (checking the argument
count, `void` versus a value, and so on) but generates no body: the
implementation is a `fastmul:` label in a hand-written `.pvms` file, following
the same calling convention (`ENTER`/`LEAVE`/`LOADFP`/`STOREFP`, the reserved
result slot — see `docs/pvms-language.en.md`) that `pvmcc` generates itself.
The compiler cannot check that; it is a contract between the `.hlc` program and
the assembly file, as `svc(...)` is.

`from name` is resolved by **the same search** as `import` (next to the file
being compiled → `lib/` next to `pvmcc` → `-I`), but with the `.pvms` extension
instead of `.hlc`, and straight to an absolute path: the generated `.pvms` may
end up in a different directory (a build tree, typically), so the compiler
emits `.import <absolute path>` — and `pvmc`'s own (relative-only) import
resolution picks absolute paths up correctly, so nothing there needed changing.
One `.import` is added per `extern` file that survived dead code elimination,
even if several functions come from it.

See `example/extern_demo.hlc` and `example/fastmath.pvms`.

## Types

There is one data type: `int` (a signed 16-bit word, like `PVM_Word_t` in the
VM). `void` exists only as a function's return type; there is no `void`
variable type.

Character literals like `'A'` are not a separate type but another way of
writing an integer (its ASCII code): `'A'` is the same as `65`, and the lexer
expands them into an `IntLiteral` before the parser ever sees them. The
escapes `'\n' '\t' '\r' '\0' '\\' '\'' '\"'` are supported.

## section — the target VM's memory sizes

```c
section code    8192;       // bytes for bytecode
section ram     64;         // words of RAM: addresses 0..63
section stack   1024;       // words in the data stack
section usrsect 0x4000, 8;  // base and length of the peripheral section
```

Declares how much memory the **target** VM has, so that overrunning it is a
compile error rather than a runtime error on the device. The compiler uses
these sizes itself (laying out globals, checking addresses given after `@`) and
passes them on to the assembler as the `.code`/`.ram`/`.stack`/`.usrsect`
directives, which check the generated code: program size, jump targets,
`LOAD`/`STORE` addresses, frame offsets (the details and the exact list of
checks are in `docs/pvms-language.en.md`).

- Each section may be declared at most once, anywhere in the file.
- The size is a constant expression (see "Named constants"), so
  `const int PERIPH = 0x4000; section usrsect PERIPH, 8;` works.
- An undeclared section takes its value from `pvm/pvm.h`, so a file without any
  `section` compiles as before. usrsect's length is the exception: `pvm.h` has
  no constant for it, and until it is declared any address from the base
  upwards is accepted.
- You may declare **less** than the VM actually has — the checks simply get
  stricter. Declaring more than the real device has will not be noticed by the
  VM: `section` is a promise to the compiler, not a setting for the VM.

A full example is `example/sections_demo.hlc`; examples of what no longer
compiles are in `example/bounds/`.

### Stack depth analysis

`pvmcc` works out how many stack words the program will use in the worst case
and says so:

```
pvmcc: stack: worst case 12 of 1024 words
```

The same line goes into the head of the generated `.pvms` as a comment. If the
worst case does not fit `section stack`, that is a compile error.

How it is computed: while generating code the compiler tracks the stack depth
inside each function (intermediate expression values and branches included — at
a label it takes the depth that jumps to it carry, so the two arms of a ternary
are not added together), then walks the call graph and takes the maximum over
all chains: the depth at the call site, plus one word for the return address,
plus whatever the callee itself needs.

The limits are honest ones, and worth knowing:

- **Recursion** makes the depth a function of the input rather than a property
  of the code. A cycle in the call graph is not unrolled: the compiler reports
  `recursion found -- worst-case depth cannot be bounded` and gives no number
  (this is not treated as an error).
- **Functions written in assembly** (`extern ... from ...;`) are invisible to
  the compiler, and their own stack use is not in the total. The line then
  gains `not counting hand-written asm functions`, and the number should be
  read as a lower bound.
- The estimate covers only code generated from `.hlc`. For hand-written `.pvms`
  there is no analysis — there the assembler only checks `LOADFP`/`STOREFP`
  offsets against `.stack`.

## Debug information

The compiler marks every statement with a `.line` directive in the generated
`.pvms` (see `docs/pvms-language.en.md`). The bytecode is unchanged by this —
the directive weighs nothing — but `pvmc -g` can collect those marks into an
"address → file and line" map, and then `pvmdbg` shows source and accepts
`break file.hlc:21` instead of addresses. See `docs/debugger.en.md`.

## Global variables

```c
int COUNTER;                    // an ordinary variable in RAM
int COUNTER = 5;                // with an initial value (a constant expression)
volatile int LED @ 0x4000;      // pinned to an address -- peripherals/MMIO
int table[10];                  // an array in RAM (no initialiser)
int table2[3] = {1, 2, 3};      // an array with an initialiser list
int a, b = 3, c;                // several variables in one declaration
```

`@address` puts a variable at a fixed address (peripherals, usually, `>=
0x4000`) rather than in an automatically allocated RAM cell. Ordinary globals
and arrays (those without `@`) are laid out in RAM by the compiler, one after
another from address `0`, and checked to fit the RAM size (see "section"). An
address given after `@` is one the compiler cannot move, so it checks that too:
it has to land either in RAM or in the declared `usrsect` section.

An initialiser is allowed only on non-`volatile` globals. The scalar
initialiser, the elements of an array's `{...}` list, the address after `@` and
the array size in `[...]` are all *constant expressions* (see the next
section): a literal, the name of a `const`, or arithmetic and bitwise
operations over them — the compiler evaluates them completely, and nothing
reaches the runtime.

For `int arr[N] = {v0, v1, ...};` the number of values in `{...}` must match
`N` **exactly** (stricter than C, where missing elements are silently zeroed) —
otherwise it is a compile error.

`int a, b = 3, c;` is one declaration for several names (the same `int`,
comma-separated); each name may independently have its own
`@address`/`[size]`/`= initialiser`, including
`volatile int X @ addr1, Y @ addr2;`.

## Named constants

```c
const int MAX_SPEED = 100;
const int LED_ADDR   = 0x4000;
const int LED_MASK   = 1 << 2;     // arithmetic is allowed

volatile int led @ LED_ADDR;       // a constant as an MMIO address
int history[MAX_SPEED];            // a constant as an array size
int limit = MAX_SPEED / 2;         // a constant in a global's initialiser
```

`const int NAME = <constant expression>;`, at file top level only. The key
difference from an ordinary global: a constant takes no RAM cell and does not
appear in the generated `.pvms` as a symbol of its own — every use of the name
is substituted by the compiler as `PUSH <value>` (or, in an address or array
size, simply as a number), so an unused constant costs literally nothing.

A constant expression may contain integer literals, the names of previously
declared `const`s (declarations are read top to bottom — no forward references,
as in C), the unary `- ~ !` and any binary operator
(`+ - * / & | ^ << >> == != < > <= >= && ||`) over them. Variables, arrays,
function calls and `svc(...)` are not allowed in a constant expression — that
is a compile error. Assigning to a constant (`NAME = ...;`) is a compile error
too.

A constant's name may not collide with a function, a global variable or
another constant.

## Functions

```c
int square(int x) {
    return x * x;
}

void increment() {
    LED = LED + 1;
}

void main() {
    ...
}
```

- `main` is required and must be declared exactly as `void main()` — no
  parameters, no return value. The reason: `main` is the entry point
  (`.entry main`), it has no caller and therefore nowhere to return a result
  with an ordinary `RET`; leaving `main` (an explicit `return;` or simply the
  end of the function) compiles to `HALT`, not to `RET`.
- Any other non-`void` function needs `return expr;` on every path; a `void`
  function may not return a value (a bare `return;` is fine).
- Arguments are passed by value and are always `int`. The number of arguments
  at a call must match the declaration, or it is a compile error.
- Recursion works — each call gets its own frame (`ENTER`/`LEAVE`).

## svc — calling into C

```c
svc(1, 42);              // as a statement -- the result is discarded
RESULT = svc(2, 3, 4);   // as an expression -- the returned value is used
```

`svc(number, arguments...)` compiles to the low-level `SVC` instruction (see
`docs/pvms-language.en.md`): the arguments are pushed left to right, then the
call number, then `SVC`. It is a built-in language construct rather than an
ordinary function call — the compiler neither checks nor knows how many
arguments a given number takes, or whether it returns anything: that is a
contract between the `.hlc` program and the C handler registered through
`PVM_RegisterSvc` (as in `.pvms`, the compiler simply trusts that the two
agree). By convention every handler leaves exactly one result on the stack, so
`svc(...)` always behaves as an ordinary single-valued expression, usable both
as a statement (`svc(1, 42);`) and inside an expression (`x = svc(2, 3, 4);`).

`simulator/pvmenv.c` currently registers `SVC #0` (`Svc_Tick`), `#1`
(`Svc_Print`) and `#2` (`Svc_Add`) — one particular table shared by the
simulator and the debugger, not part of the language. For a fuller example of
why `svc` is worth having, see the `lib/time.hlc` library:

- `int Time_GetTick()` — the current tick (a wrapper over `svc(0)`);
- `int Time_Elapsed(int old)` — the difference, accounting for the 16-bit
  counter wrapping around, written entirely in `.hlc` with no change to the VM
  (plain `-` already works modulo 2¹⁶);
- `void Time_Delay(int ticks)` — a busy-wait of `ticks` counts built on the
  previous two; on an MCU with a real timer behind `SVC #0` this becomes a real
  delay without a single change to this code (see `example/blink.hlc`).

See also `example/time_demo.hlc` (which exercises `Time_Elapsed` across the
wraparound).

See `example/svc_demo.hlc` and the `Svc_Print`/`Svc_Add` handlers in
`simulator/pvmenv.c`.

## Local variables — an important restriction

```c
void f() {
    int a;        // OK -- the very start of the function
    int b = 1;    // OK
    int c, d = 2; // OK -- several names in one "int", as with globals
    a = 2;        // from here on, ordinary statements; another "int" is no longer allowed
}
```

**Every local declaration must come as one block at the very start of the
function body**, before any other statement (a C89-flavoured rule) — including
inside the body of an `if`/`while`/`for` and in a `for` initialiser: locals
cannot be declared there, only used. This is not a forgotten feature but a
deliberate restriction: the layout of locals relative to `FP` is static (the
compiler decides once, at compile time, which variable gets which offset), and
that only holds if every declaration is guaranteed to run exactly once per
call. A declaration inside a loop or a condition would run a varying number of
times and would desynchronise the addressing of the rest of the frame.

**There are no local arrays** — only global ones. `LOADFP`/`STOREFP` take a
constant offset rather than an index off the stack, so there is nothing to
address an array on the frame with; an array in RAM is addressed as
`base + index` through `LOADI`/`STOREI`, which is what the compiler does for
global arrays.

There is no block scoping or shadowing: the names of a function's locals and
parameters live in one flat namespace for the whole function.

## Statements

```c
{ ... }                              // a block
if (cond) stmt [else stmt]
while (cond) stmt
do stmt while (cond);                // the body runs at least once
for (simple_statement?; cond?; simple_statement?) stmt
return [expr];
halt;
name = expr;
name += expr;                        // and -= *= /= &= |= ^= <<= >>=
name++;  name--;
name[index] = expr;
name(arguments);                     // a call as a statement
```

`do ... while (cond);` checks the condition **after** the body, so it always
runs at least once (unlike `while`).

Compound assignments and `++`/`--` are pure sugar: the parser expands `x += e`
into `x = x + e`, and `x++`/`x--` into `x = x + 1`/`x = x - 1`, before code
generation ever happens, so to the compiler they are literally the same
ordinary assignment (see `hlc/parser.cpp`, `MakeCompoundAssign`). They work on
a plain name only (a local, a global, a parameter) — **not on an array
element** (`arr[i] += 1;` is not supported): expanding that the same way would
evaluate the index `i` twice, which breaks invisibly when the index has a side
effect (a function call, an `svc`).

## Expressions and operators (highest precedence last)

| Precedence | Operators | Associativity |
|---|---|---|
| lowest | `?:` (ternary) | right to left |
| | `\|\|` | left to right |
| | `&&` | left to right |
| | `==` `!=` | left to right |
| | `<` `>` `<=` `>=` | left to right |
| | `\|` | left to right |
| | `^` | left to right |
| | `&` | left to right |
| | `<<` `>>` | left to right |
| | `+` `-` | left to right |
| | `*` `/` | left to right |
| highest | unary `-` `!` `~` | right to left |

Plus indexing `arr[i]` (global arrays only), function calls `f(...)`,
parentheses `(expr)`, integer and character literals (`123`, `0x7B`, `'A'`),
and `sizeof(name)`.

`cond ? a : b` behaves as in C: exactly one branch is evaluated (`a` if `cond`
is non-zero, otherwise `b`), and both leave one value on the stack — from the
code generator's point of view this is no different from an `if`/`else`
(`JZ`/`JMP`). It is right-associative, so `a ? b : c ? d : e` means
`a ? b : (c ? d : e)`. It also works as a constant expression (`const`,
`@address`, `[size]`), where only the chosen branch is evaluated, so
`cond ? x : 1 / 0` will not blow up on the division when `cond` is true.

`sizeof(name)` is the element count of the global array `name` (a compile error
for anything else), known at compile time and folded straight into
`PUSH <size>`, like a literal. That is exactly why, unlike an ordinary read of
an array element, `sizeof` does **not** mark the array as used: if an array
appears nowhere in the program except inside `sizeof(...)`, it gets no RAM cell
at all (dead code elimination discards it entirely, the number having already
been substituted). It is available only inside function bodies — not in a
`const`, an `@address` or a `[size]` — because at the point those are evaluated
the list of globals is not ready yet (see `hlc/codegen.cpp`, `Codegen::Run`).

`&&`/`||` **short-circuit**, as in C (the right-hand side is not evaluated when
the left already decides the answer), and the result is always normalised to
`0` or `1`. Assignment is only a top-level statement inside `simple_statement`
(`a = b = 5` and `if ((a = f()) != 0)` are not supported), but as an expression
it does **yield the assigned value**, so it can be used in a `for` step, for
instance.

## Comments

`// to the end of the line` and `/* block */`, as in C (independent of
`.pvms`'s `#` comments — this language has its own lexer).

## Deliberately absent from this version

- Types other than `int`/`void` (no `char`, `bool`, `float`, pointers or
  `struct`).
- Local arrays (see above) and multidimensional arrays.
- Block scoping and shadowing.
- Assignment as part of an arbitrary expression.
- "Does every path return a value" analysis — the compiler simply always adds
  `LEAVE`/`RET` (or `LEAVE`/`HALT` for `main`) at the end of a function body,
  even where every path already returned explicitly (a couple of bytes of
  unreachable code, not a bug).

## A complete example

```c
int arr[5];
int SUM;

void fill() {
    int i;
    for (i = 0; i < 5; i = i + 1) {
        arr[i] = i * 2;
    }
}

int total() {
    int i;
    int sum;
    sum = 0;
    for (i = 0; i < 5; i = i + 1) {
        sum = sum + arr[i];
    }
    return sum;
}

void main() {
    fill();
    SUM = total();   // 0+2+4+6+8 = 20
}
```

More examples: `example/square.hlc` (an argument and a return value),
`example/counter.hlc` (MMIO and `while`), `example/blink.hlc` (an endless loop
with a software delay), `example/arrays_and_logic.hlc` (arrays, `for`, every
comparison, `&&`/`||`), `example/import_demo.hlc` + `lib/mathlib.hlc`
(`import`), `example/svc_demo.hlc` (`svc(...)`), `example/extern_demo.hlc` +
`example/fastmath.pvms` (`extern ... from ...;`), `lib/time.hlc` +
`example/time_demo.hlc` (timekeeping on top of `svc`).

## Error reporting

The compiler stops at the first error and prints `file:line: error: message` to
stderr, exiting with code `1`. It checks for unknown identifiers and functions,
argument count mismatches, use of a `void` function as a value, local
declarations that are not at the start of a function, local arrays, globals
overflowing RAM, a `main` with parameters or one that is not `void`, and so on.
