[Русский](README.md) | **English**

# The PVM extension for VS Code

Syntax highlighting and snippets for `.hlc` (the high-level language) and
`.pvms` (the assembler), plus **debugging `.hlc` inside the editor**:
breakpoints by clicking the gutter, F5/F10/F11, the current line highlighted,
and the stack in the variables pane.

What it does not do: error checking, completion or go-to-definition — those
need a language server, and there is none here.

## Installing

The extension is not published on the Marketplace; it is installed by hand,
copying the folder into VS Code's extensions directory.

1. Copy this whole folder (`vscode-extension`) into VS Code's extensions
   directory:

   - Windows: `%USERPROFILE%\.vscode\extensions\`
   - Linux/macOS: `~/.vscode/extensions/`

   The folder's name inside `extensions` does not matter for it to work, but
   VS Code's convention is `<publisher>.<name>-<version>`, so rename the copy
   to `pvm-project.pvm-lang-0.4.0`.

   PowerShell (this copies the folder's **contents**, hence the `\*`: otherwise,
   if the destination already exists from an earlier install, `Copy-Item` puts
   the folder inside it and VS Code goes on seeing the old version):
   ```powershell
   $dest = "$env:USERPROFILE\.vscode\extensions\pvm-project.pvm-lang-0.4.0"
   New-Item -ItemType Directory -Force $dest | Out-Null
   Copy-Item -Recurse -Force "vscode-extension\*" $dest
   ```

   Everything has to be copied, `node_modules` included: that is where
   `@vscode/debugadapter` lives, and without it the extension will not load and
   VS Code will say `Couldn't find a debug adapter descriptor for debug type
   'pvm'`. To check that the install is complete:
   ```powershell
   ls $dest\src, $dest\node_modules\@vscode
   ```
   There should be four files in `src` and two folders in
   `node_modules\@vscode`.

   bash (the same thing: `/.` copies the contents rather than the folder):
   ```
   mkdir -p ~/.vscode/extensions/pvm-project.pvm-lang-0.4.0
   cp -r vscode-extension/. ~/.vscode/extensions/pvm-project.pvm-lang-0.4.0/
   ```

   If an older version was already installed, delete its folder (for example
   `pvm-project.pvm-lang-0.2.0`) so the two do not sit there at once.

2. Restart VS Code (or **Ctrl+Shift+P** → **Developer: Reload Window**).

3. Open any `.hlc` or `.pvms` file — highlighting attaches automatically by
   file extension (the language is shown in the bottom right corner).

## What is highlighted

- **`.hlc`**: keywords (`if/else/while/do/for/return/halt`),
  `import/extern/from`, `svc(...)`/`sizeof(...)` as built-in calls, the
  `int/void` types, `volatile`/`const`, numbers (decimal and `0x`), character
  literals (`'A'`, `'\n'`), the ternary `?:`, function names, operators
  (compound assignments `+= -= *= ...` and `++`/`--` included — each character
  is highlighted as an operator, though not as a single token), brackets.
- **`.pvms`**: `#` comments, the `.const`/`.import`/`.entry` directives (with
  the constant, label or path name highlighted separately), every VM
  instruction mnemonic, labels (`name:`), numbers (negative ones included — the
  `LOADFP`/`STOREFP` offsets).

## Snippets (`.hlc`)

`func`, `vfunc`, `main`, `if`, `ifelse`, `while`, `dowhile`, `for`, `extern`,
`import`, `const`, `volatile`, `svc`, `ternary`, `arrinit` — start typing the
prefix and pick it from the completion list.

## Auto-indent

Typing `{`/`}` makes VS Code increase or decrease the next line's indentation
(`language-configuration-hlc.json`, `indentationRules`).

## Sources

The sources of this same extension live in the repository under
`tools/vscode-pvm/`; the copy in `Release/` is only a snapshot for handing out,
and the grammar is easier to edit at the source.

## Debugging

You need `pvmdbg.exe` (it sits right next to this folder, in `Release/`) and a
debug map from `pvmc -g`. All communication with the VM goes through `pvmdbg`:
protocol frames, CRCs and the serial port live there, and the extension only
translates between it and VS Code.

Create a `.vscode/launch.json` in your project (VS Code offers templates once
you pick the `PVM` type):

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "pvm",
      "request": "launch",
      "name": "PVM: debug bytecode",
      "program": "${workspaceFolder}/build/bytecode/blink.bin",
      "pvmdbg": "${workspaceFolder}/Release/pvmdbg.exe",
      "stopOnEntry": true
    },
    {
      "type": "pvm",
      "request": "launch",
      "name": "PVM: debug on hardware",
      "port": "COM3",
      "map": "${workspaceFolder}/build/bytecode/blink.dbgmap",
      "pvmdbg": "${workspaceFolder}/Release/pvmdbg.exe",
      "stopOnEntry": true
    }
  ]
}
```

| Setting | What it is for |
|---|---|
| `program` | the `.bin` to debug in a VM inside `pvmdbg`, on this machine |
| `port` | the board's serial port; used instead of `program`, since a board holds its own |
| `map` | the map from `pvmc -g`; guessed next to `program`, required with `port` |
| `pvmdbg` | the path to `pvmdbg.exe` |
| `stopOnEntry` | stop on the first instruction (true by default) |

What works: breakpoints on `.hlc` lines, continuing, stepping by line (F10
stays out of calls, F11 goes into them), and a variables pane with `pc`/`sp`/`fp`
and the contents of the VM stack.

What is not there yet: a multi-frame call stack (one frame is shown — where
execution currently is) and local variable names (the stack is shown as
values). Breakpoints can only be set in `.hlc`; there are none for `.pvms`,
because the debug map is produced by the high-level compiler.

There is a limit of four breakpoints — that is how many slots the device-side
agent has (`PVMDBG_CFG_BRKP_SIZE`).
