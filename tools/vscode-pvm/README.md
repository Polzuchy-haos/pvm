**Русский** | [English](README.en.md)

# Расширение PVM для VS Code

Подсветка синтаксиса и сниппеты для `.hlc` (высокоуровневый язык) и `.pvms`
(ассемблер), плюс **отладка `.hlc` прямо в редакторе**: точки останова кликом
по полю, F5/F10/F11, подсветка текущей строки, стек в панели переменных.

Чего нет: проверки ошибок, автодополнения и переходов к определению — для
этого нужен language server, его тут нет.

## Установка

Расширение не опубликовано в Marketplace — ставится вручную, копированием
папки в каталог расширений VS Code.

1. Скопируйте всю эту папку (`vscode-extension`) в каталог расширений VS Code:

   - Windows: `%USERPROFILE%\.vscode\extensions\`
   - Linux/macOS: `~/.vscode/extensions/`

   Имя папки внутри `extensions` не важно для работы, но по соглашению VS Code
   называется `<publisher>.<name>-<version>`, то есть переименуйте
   скопированную папку в `pvm-project.pvm-lang-0.4.0`.

   PowerShell (копируется **содержимое** папки, через `\*`: иначе, если
   каталог назначения уже существует после прошлой установки, `Copy-Item`
   положит папку внутрь него, и VS Code увидит старую версию):
   ```powershell
   $dest = "$env:USERPROFILE\.vscode\extensions\pvm-project.pvm-lang-0.4.0"
   New-Item -ItemType Directory -Force $dest | Out-Null
   Copy-Item -Recurse -Force "vscode-extension\*" $dest
   ```

   Копировать нужно всё, включая `node_modules`: там лежит `@vscode/debugadapter`,
   без которого расширение не загрузится, и VS Code скажет `Couldn't find a debug
   adapter descriptor for debug type 'pvm'`. Проверить, что установилось целиком:
   ```powershell
   ls $dest\src, $dest\node_modules\@vscode
   ```
   Должны быть четыре файла в `src` и две папки в `node_modules\@vscode`.

   bash (то же самое: `/.` копирует содержимое, а не саму папку):
   ```
   mkdir -p ~/.vscode/extensions/pvm-project.pvm-lang-0.4.0
   cp -r vscode-extension/. ~/.vscode/extensions/pvm-project.pvm-lang-0.4.0/
   ```

   Если уже стояла более старая версия — удалите её папку (например,
   `pvm-project.pvm-lang-0.2.0`), чтобы не остались две одновременно.

2. Перезапустите VS Code (или **Ctrl+Shift+P** → **Developer: Reload Window**).

3. Откройте любой `.hlc` или `.pvms` файл — подсветка подключится
   автоматически по расширению файла (язык виден в правом нижнем углу окна).

## Что подсвечивается

- **`.hlc`**: ключевые слова (`if/else/while/do/for/return/halt`),
  `import/extern/from`, `svc(...)`/`sizeof(...)` как встроенные вызовы, типы
  `int/void`, `volatile`/`const`, числа (десятичные и `0x`), символьные
  литералы (`'A'`, `'\n'`), тернарный оператор `?:`, имена функций, операторы
  (включая составные присваивания `+= -= *= ...` и `++`/`--` — каждый символ
  подсвечивается как оператор, хоть и не единым токеном), скобки.
- **`.pvms`**: `#`-комментарии, директивы `.const`/`.import`/`.entry`
  (с отдельной подсветкой имени константы/метки/пути), все мнемоники
  инструкций ВМ, метки (`name:`), числа (включая отрицательные — офсеты
  `LOADFP`/`STOREFP`).

## Сниппеты (`.hlc`)

`func`, `vfunc`, `main`, `if`, `ifelse`, `while`, `dowhile`, `for`, `extern`,
`import`, `const`, `volatile`, `svc`, `ternary`, `arrinit` — начните набирать
префикс и выберите из автодополнения.

## Автоотступ

При наборе `{`/`}` VS Code сам увеличивает/уменьшает отступ следующей строки
(`language-configuration-hlc.json`, `indentationRules`).

## Исходники

Исходники этого же расширения лежат в репозитории в `tools/vscode-pvm/` —
эта папка в `Release/` лишь снимок для удобной раздачи, править грамматику
удобнее там.

## Отладка

Требуется `pvmdbg.exe` (лежит рядом, в `Release/`) и карта отладки от
`pvmc -g`. Всё общение с ВМ идёт через `pvmdbg`: кадры протокола, CRC и
последовательный порт живут там, расширение только переводит между ним и
VS Code.

Создайте `.vscode/launch.json` в своём проекте (VS Code предложит заготовки
после выбора типа `PVM`):

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "pvm",
      "request": "launch",
      "name": "PVM: отладка байткода",
      "program": "${workspaceFolder}/build/bytecode/blink.bin",
      "pvmdbg": "${workspaceFolder}/Release/pvmdbg.exe",
      "stopOnEntry": true
    },
    {
      "type": "pvm",
      "request": "launch",
      "name": "PVM: отладка на плате",
      "port": "COM3",
      "map": "${workspaceFolder}/build/bytecode/blink.dbgmap",
      "pvmdbg": "${workspaceFolder}/Release/pvmdbg.exe",
      "stopOnEntry": true
    }
  ]
}
```

| Параметр | Зачем |
|---|---|
| `program` | `.bin` для отладки в ВМ внутри `pvmdbg`, на этой машине |
| `port` | последовательный порт платы; вместо `program` — у платы программа своя |
| `tcp` | `host:port` — плата через TCP (мост к её UART или `pvmboard`); вместо `port` |
| `map` | карта от `pvmc -g`; при `program` угадывается рядом с ним, при `port` обязательна |
| `pvmdbg` | путь к `pvmdbg.exe` |
| `stopOnEntry` | остановиться на первой инструкции (по умолчанию да) |

Что работает: точки останова по строкам `.hlc`, продолжение, шаг по строкам
(F10 не проваливается внутрь вызова, F11 проваливается), панель переменных с
`pc`/`sp`/`fp` и содержимым стека ВМ.

Чего пока нет: многокадрового стека вызовов (показывается один кадр — где
именно стоим) и имён локальных переменных (стек показан значениями). Точки
останова ставятся только в `.hlc`; для `.pvms` их нет, потому что карту
отладки делает компилятор высокоуровневого языка.

Ограничений по количеству точек останова четыре — столько слотов у агента на
устройстве (`PVMDBG_CFG_BRKP_SIZE`).
