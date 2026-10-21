[Русский](project-generator.md) | **English**

# `pvminit` — the build and debug script generator

A program of your own in `.hlc` always comes with the same handful of files
around it: what builds it, what runs it, what debugs it, and how to explain all
of that to VS Code. They are small files, but writing them from scratch means
remembering the order of `pvmcc` and `pvmc`, every flag that matters (`-l`,
`-g`) and the full path to each tool. `pvminit` writes them for you:

```
pvminit myproject --port COM6
```

```
myproject/
  main.hlc              a skeleton program (when there isn't one yet)
  Makefile              build, run, debug, flashing formats
  .gitignore            build/
  .vscode/
    launch.json         F5 — debug .hlc in the editor
    tasks.json          Ctrl+Shift+B — build, and the build before debugging
```

Everything after that is plain `make`:

```
make            build
make run        run it in the simulator on this machine
make debug      step through it in the VM
make board      the same on a board: make board PORT=COM6
```

What comes out is ordinary, readable files rather than a hidden build system —
they are meant to be edited afterwards. Which is why an existing file is
**never** overwritten without `--force`.

## Why a program and not a template in the documentation

The one thing such files cannot work out for themselves is where the tools
live. `pvminit` finds that path and writes it into what it generates; a template
copied out of a README would have to be corrected by hand, which is exactly the
annoying part.

It looks in this order:

1. `--tools`, when given explicitly;
2. **the directory `pvminit` was called from** — if `pvmc` and `pvmcc` are in
   it. That is what makes `cd <toolchain> && pvminit <project>` mean what it
   looks like it means, and it lets one copy of `pvminit` set up projects
   against whichever build of the toolchain you are standing in;
3. otherwise the directory `pvminit.exe` itself lives in. This is the ordinary
   case: the generator gets called from anywhere, and a path to a directory with
   no `pvmc` in it would only produce a project that cannot build.

Which of the three it was is printed:

```
tools:  F:/Project/pvm/Release  (beside pvminit: no tools in the current directory)
```

## Options

```
pvminit [directory] [options]
```

| Option | What it does |
|---|---|
| `-m`, `--main <file.hlc>` | main source. Default: the only `.hlc` in the directory, or `main.hlc` when there is none (a skeleton is generated) |
| `-n`, `--name <name>` | base name of the build outputs (`build/name.bin`). Default: the main source's |
| `-p`, `--port <COM3>` | the board's serial port, for the debug configuration |
| `--tools <dir>` | where the tools live. Default: the current directory when the tools are in it, otherwise beside `pvminit` itself (see above) |
| `--make <program>` | what to call make in `tasks.json`. Default: the first of `make`, `mingw32-make`, `gmake` found on `PATH` |
| `--no-vscode` | the `Makefile` only, no `.vscode/` |
| `-f`, `--force` | overwrite files that are already there |
| `-h`, `--help` | usage |

The directory is created when missing. With no arguments at all it sets up the
current directory — and when exactly one `.hlc` is already sitting there, that
one becomes the main source, no options needed:

```
cd myproject
pvminit
```

## Inside the Makefile

Everything meant to be adjusted is in the first few lines; below them it is
rarely touched.

| Variable | What for |
|---|---|
| `NAME` | base name of what lands in `build/` |
| `SRC` | the main `.hlc` |
| `PORT` | the board's port, overridable on the command line: `make board PORT=COM7` |
| `PVM_TOOLS` | the directory holding the tools |
| `PVMCC`, `PVMC`, `PVMSIM`, `PVMDBG`, `PVMDISASM` | each tool separately, so one can be swapped without touching the rest |
| `INCLUDES` | extra directories for `import ...;` (the `lib/` beside `pvmcc` is found anyway) |

All of them are declared with `?=`, so they can be overridden straight from the
command line without editing the file:

```
make PVM_TOOLS=D:/pvm/Release
```

### Targets

| Target | What it does |
|---|---|
| `make` / `make build` | `build/NAME.bin` plus the `.lst` listing and the `.dbgmap` debug map |
| `make run` | build and execute it in `pvmsim` |
| `make debug` | build and open a `pvmdbg` session against the VM on this machine |
| `make board` | the same, but on a board over a serial port |
| `make hex` | Intel HEX, for a flashing tool |
| `make carray` | the bytecode as a C array, to build into firmware |
| `make disasm` | disassemble the built `.bin` |
| `make clean` | delete the build directory |

### Three things in it that are not obvious

**The source is compiled by absolute path** (`$(CURDIR)/$(SRC)`). Exactly the
path `pvmcc` was handed is what ends up in the debug map, and VS Code opens the
file by it when the debugger stops on a line. With a relative path the line
highlight would quietly not work: nothing says which directory the editor should
resolve it against.

**`.bin`, `.lst` and `.dbgmap` are produced by one command,** and there is one
rule for them — on the `.bin`. Three separate rules would have make run the
assembler three times.

**A rebuild is hung off every `.hlc` beside it** (`$(wildcard *.hlc)`), not just
the main one: which files are pulled in through `import` is known only to the
compiler, and it does not report that yet. One redundant run of `pvmcc` is
cheaper than an edit the build failed to notice.

## Debugging in VS Code

The PVM extension has to be installed — see `vscode-extension/README.md` beside
the tools. After that, **F5**: `tasks.json` builds the project (that is the
`preLaunchTask`), `launch.json` starts the debugger.

Two configurations come ready:

- **`PVM: <name> в ВМ на этой машине`** — the program runs on the virtual
  machine inside `pvmdbg`; no hardware needed;
- **`PVM: плата на <port>`** — the same interface, but a real board takes the
  steps. The board runs its own program, already flashed, so no `.bin` is passed
  to it — and the debug map is then mandatory, and has to come from the **same**
  build as the firmware.

`Ctrl+Shift+B` builds without starting the debugger. Errors from `pvmcc` and
`pvmc` are picked up by the `problemMatcher` and land in the Problems panel as a
clickable list.

## Several programs in one place

`pvminit` sets up the build of one main program, which is enough for the vast
majority of firmware. When there really are several, the simplest thing is a
directory per program:

```
pvminit blink  --main blink.hlc
pvminit sensor --main sensor.hlc
```

Shared code goes beside them and is pulled in with `import`, with its directory
named in `INCLUDES` in both `Makefile`s.

## What it needs to work

- **make** — any GNU make. `pvminit` works out what it is called on this
  machine (`make`, `mingw32-make`, `gmake`) and writes that into `tasks.json`.
  The generated `Makefile` sets `SHELL` explicitly (`cmd.exe` on Windows):
  otherwise make picks either `cmd.exe` or whichever `sh` is on `PATH`, and one
  and the same line behaves differently on two machines.
- **The PVM extension for VS Code** — only for debugging with F5. `make debug`
  works without it, in the terminal.

> The comments inside the generated files are in Russian, matching the manuals.
> Pass `--help` for the English usage text.
