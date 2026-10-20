/*
 * pvminit.cpp
 *
 * Generates the build and debug scripts for a PVM project, so that starting one
 * does not begin with copying a Makefile and a launch.json out of this
 * repository and editing every path in them by hand:
 *
 *   pvminit myproject --main main.hlc --port COM6
 *
 * What comes out is ordinary, readable files -- a Makefile and a .vscode/ --
 * not a hidden build system. They are meant to be edited afterwards, which is
 * why an existing file is never overwritten without --force.
 *
 * The one thing such files cannot work out for themselves is where the tools
 * live, and that is the whole reason this is a program rather than a template
 * in the documentation: pvminit finds that path and writes it into what it
 * generates -- see ToolsDirectory for where it looks.
 */
#include "exepath.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options{
	fs::path 		dir 	 = ".";
	std::string main;             /* main .hlc source, relative to dir */
	std::string name;             /* base name of the build outputs */
	std::string port 	 = "COM3";
	fs::path 		tools;            /* where pvmc/pvmcc/pvmsim/pvmdbg live */
	std::string make;             /* what to call make in tasks.json */
	bool 				force  = false;
	bool 				vscode = true;
};

class InitError : public std::runtime_error{
public:
	explicit InitError(const std::string& what) : std::runtime_error(what) {}
};

/* ---- templates -------------------------------------------------------- */
/*
 * Comments inside the generated files are in Russian, matching the manuals:
 * they are read in an editor by whoever owns the project. What pvminit itself
 * prints is in English, matching the rest of the toolchain -- that goes to a
 * Windows console, whose code page mangles anything else.
 */

const char* kMakefile = R"MAKE(# Makefile проекта PVM -- создан утилитой pvminit.
# Обычный файл, правьте как угодно: pvminit перезапишет его только с --force.
#
#   make          собрать (то же, что make build)
#   make run      собрать и выполнить в симуляторе на этой машине
#   make debug    собрать и открыть сессию пошаговой отладки в ВМ
#   make board    то же, но на плате: make board PORT=COM6
#   make hex      Intel HEX -- для прошивальщика
#   make carray   байткод массивом C -- чтобы вшить его в прошивку
#   make disasm   дизассемблировать собранный .bin
#   make clean    удалить каталог сборки

NAME  := @NAME@
SRC   := @SRC@
BUILD := build
PORT  ?= @PORT@

# Каталог с утилитами. Подставлен pvminit-ом -- тем, откуда он сам был запущен.
# Переопределяется, не трогая файл:  make PVM_TOOLS=/другой/каталог
PVM_TOOLS ?= @TOOLS@

# Каждая утилита отдельной переменной -- чтобы подменить одну, не трогая остальные.
PVMCC     ?= $(PVM_TOOLS)/pvmcc
PVMC      ?= $(PVM_TOOLS)/pvmc
PVMSIM    ?= $(PVM_TOOLS)/pvmsim
PVMDBG    ?= $(PVM_TOOLS)/pvmdbg
PVMDISASM ?= $(PVM_TOOLS)/pvmdisasm

# Дополнительные каталоги для "import ...;" -- свой lib/ рядом с pvmcc он и так
# найдёт, здесь перечисляются только чужие. Через пробел.
INCLUDES :=

# ---------------------------------------------------------------------------
# Ниже правится редко.
# ---------------------------------------------------------------------------

# Windows и всё остальное различаются ровно двумя вещами: чем создать каталог и
# чем его удалить. SHELL задан явно -- иначе make берёт то cmd.exe, то найденный
# в PATH sh, и одна и та же строка ведёт себя по-разному на двух машинах.
ifeq ($(OS),Windows_NT)
  SHELL := cmd.exe
  .SHELLFLAGS := /C
  native = $(subst /,\,$1)
  MKDIR = if not exist "$(call native,$1)" mkdir "$(call native,$1)"
  RMDIR = if exist "$(call native,$1)" rmdir /s /q "$(call native,$1)"
else
  native = $1
  MKDIR = mkdir -p $1
  RMDIR = rm -rf $1
endif

PVMS := $(BUILD)/$(NAME).pvms
BIN  := $(BUILD)/$(NAME).bin
LST  := $(BUILD)/$(NAME).lst
MAP  := $(BUILD)/$(NAME).dbgmap
HEX  := $(BUILD)/$(NAME).hex
CARR := $(BUILD)/$(NAME).c
DIS  := $(BUILD)/$(NAME).disasm.txt

# Любой .hlc рядом может оказаться подключённым через import, а какие именно --
# знает только компилятор, и наружу он этого пока не сообщает. Поэтому
# пересборка завязана на все сразу: лишний вызов pvmcc дешевле правки, которую
# сборка не заметила.
SOURCES := $(wildcard *.hlc) $(wildcard */*.hlc)

.PHONY: all build run debug board hex carray disasm clean

all: build
build: $(BIN)

# Исходник передаётся абсолютным путём намеренно: ровно этот путь попадает в
# карту отладки, а VS Code открывает по нему файл, когда отладчик встаёт на
# строку. С относительным путём подсветка строки молча не работала бы -- каталог,
# относительно которого редактор его раскроет, ничем не задан.
$(PVMS): $(SOURCES)
	@$(call MKDIR,$(BUILD))
	$(call native,$(PVMCC)) $(CURDIR)/$(SRC) $(addprefix -I,$(INCLUDES)) -o $@

# .lst и .dbgmap рождаются этой же командой, отдельных правил у них нет: иначе
# make вызвал бы ассемблер по разу на каждый из трёх файлов.
$(BIN): $(PVMS)
	$(call native,$(PVMC)) $< -o $@ -f bin -l $(LST) -g $(MAP)

$(HEX): $(PVMS)
	$(call native,$(PVMC)) $< -o $@ -f hex

$(CARR): $(PVMS)
	$(call native,$(PVMC)) $< -o $@ -f carray

$(DIS): $(BIN)
	$(call native,$(PVMDISASM)) $< -o $@

hex: $(HEX)
carray: $(CARR)
disasm: $(DIS)

run: $(BIN)
	$(call native,$(PVMSIM)) $(BIN)

# --map: без неё отладчик покажет адреса байткода вместо строк исходника.
debug: $(BIN)
	$(call native,$(PVMDBG)) $(BIN) --map $(MAP)

# У платы программа своя, залитая в прошивку, поэтому .bin здесь не передаётся --
# но карта нужна, и обязана быть от той же сборки, что и прошивка.
board: $(BIN)
	$(call native,$(PVMDBG)) --port $(PORT) --map $(MAP)

clean:
	@$(call RMDIR,$(BUILD))
)MAKE";

const char* kLaunchJson = R"JSON({
	// Отладка .hlc прямо в редакторе: точки останова кликом по полю, F5/F10/F11,
	// подсветка текущей строки, стек ВМ в панели переменных.
	//
	// Нужно установленное расширение PVM (см. vscode-extension/README.md рядом с
	// утилитами) и собранный проект -- .bin и .dbgmap берутся из build/, поэтому
	// перед запуском выполняется задача "pvm: build" из tasks.json.
	"version": "0.2.0",
	"configurations": [
		{
			"type": "pvm",
			"request": "launch",
			"name": "PVM: @NAME@ в ВМ на этой машине",
			"program": "${workspaceFolder}/build/@NAME@.bin",
			// Карту рядом с .bin расширение находит и само; здесь она указана
			// явно, чтобы было видно, откуда берутся строки исходника.
			"map": "${workspaceFolder}/build/@NAME@.dbgmap",
			"pvmdbg": "@PVMDBG@",
			"stopOnEntry": true,
			"preLaunchTask": "pvm: build"
		},
		{
			"type": "pvm",
			"request": "launch",
			"name": "PVM: плата на @PORT@",
			// Без program: у платы программа своя, залитая в прошивку. Поэтому
			// карту угадывать не из чего и указать её обязательно -- и она должна
			// быть от той же сборки, что и прошивка.
			"port": "@PORT@",
			"map": "${workspaceFolder}/build/@NAME@.dbgmap",
			"pvmdbg": "@PVMDBG@",
			"stopOnEntry": true,
			"preLaunchTask": "pvm: build"
		}
	]
}
)JSON";

const char* kTasksJson = R"JSON({
	// Сборка из редактора: Ctrl+Shift+B, и она же запускается перед отладкой
	// (preLaunchTask в launch.json). Вся настоящая работа -- в Makefile.
	"version": "2.0.0",
	"tasks": [
		{
			"label": "pvm: build",
			"type": "shell",
			"command": "@MAKE@",
			"args": ["build"],
			"options": { "cwd": "${workspaceFolder}" },
			"group": { "kind": "build", "isDefault": true },
			// pvmcc и pvmc сообщают об ошибках как "файл:строка: error: текст" --
			// отсюда VS Code делает кликабельный список в панели Problems.
			"problemMatcher": {
				"owner": "pvm",
				"fileLocation": ["autoDetect", "${workspaceFolder}"],
				"pattern": {
					"regexp": "^(.*):(\\d+): (error|warning): (.*)$",
					"file": 1,
					"line": 2,
					"severity": 3,
					"message": 4
				}
			}
		},
		{
			"label": "pvm: run",
			"type": "shell",
			"command": "@MAKE@",
			"args": ["run"],
			"options": { "cwd": "${workspaceFolder}" },
			"problemMatcher": []
		},
		{
			"label": "pvm: clean",
			"type": "shell",
			"command": "@MAKE@",
			"args": ["clean"],
			"options": { "cwd": "${workspaceFolder}" },
			"problemMatcher": []
		}
	]
}
)JSON";

const char* kMainHlc = R"HLC(// Заготовка, созданная pvminit. Сборка -- make, запуск -- make run,
// пошаговая отладка -- make debug (или F5 в VS Code).

int result;
int i;

int add(int a, int b) {
    return a + b;
}

void main() {
    result = 0;
    for (i = 1; i <= 10; i = i + 1) {
        result = add(result, i);
    }
    halt;
}
)HLC";

const char* kGitignore = R"IGN(# Всё, что собирается из исходников.
build/
)IGN";

/* ---- helpers ----------------------------------------------------------- */

std::string Replace(std::string text, const std::string& what, const std::string& with){
	for(size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size()))
		text.replace(at, what.size(), with);
	return text;
}

/*
 * Which of make's several names this machine actually has. Written into
 * tasks.json rather than left as a plain "make", because on Windows the one
 * that comes with MinGW is called mingw32-make, and a task naming the wrong one
 * fails with nothing but "command not found".
 */
std::string FindMake(){
	const char* path = std::getenv("PATH");
	if(!path) return "make";

#ifdef _WIN32
	const char 				separator = ';';
	const std::string suffix 		= ".exe";
#else
	const char 				separator = ':';
	const std::string suffix 		= "";
#endif

	std::vector<std::string> dirs;
	std::string 						 current;

	for(const char* p = path; ; p++){
		if(*p == separator || *p == '\0'){
			if(!current.empty()) dirs.push_back(current);
			current.clear();
			if(*p == '\0') break;
		}else{
			current += *p;
		}
	}

	for(const char* name : {"make", "mingw32-make", "gmake"})
		for(const std::string& dir : dirs){
			std::error_code ec;
			if(fs::exists(fs::path(dir) / (std::string(name) + suffix), ec))
				return name;
		}

	return "make";
}

#ifdef _WIN32
const std::string kExeSuffix = ".exe";
#else
const std::string kExeSuffix = "";
#endif

/* Whether a directory is a toolchain and not just any directory. Two of the
 * tools are enough to tell: nothing else keeps a pvmc next to a pvmcc. */
bool HasTools(const fs::path& dir){
	std::error_code ec;
	return fs::exists(dir / ("pvmc" + kExeSuffix), ec) &&
				 fs::exists(dir / ("pvmcc" + kExeSuffix), ec);
}

/*
 * Where to say the tools are, when --tools was not given.
 *
 * The directory pvminit was called from comes first: it is what makes
 * "cd <toolchain> && pvminit <project>" mean what it looks like it means, and
 * it lets one copy of pvminit set up projects against whichever build of the
 * toolchain you happen to be standing in.
 *
 * Its own directory is the fallback, for the ordinary case of calling pvminit
 * from somewhere that has no tools in it -- a path pointing at a directory with
 * no pvmc in it would only produce a project that cannot build. `fellBack` says
 * which of the two it was, because the answer is worth printing.
 */
fs::path ToolsDirectory(bool* fellBack){
	std::error_code ec;
	fs::path 				here = fs::current_path(ec);

	if(!ec && HasTools(here)){
		*fellBack = false;
		return here;
	}

	*fellBack = true;
	return GetExecutableDir();
}

/*
 * The only .hlc in the directory, when there is exactly one -- so that running
 * pvminit in a folder that already holds a program picks that up, instead of
 * generating a skeleton beside it and building the wrong file. Empty when there
 * is none, or more than one and the user has to say which.
 */
std::string SoleSource(const fs::path& dir){
	std::error_code ec;
	std::string 		found;

	for(const auto& entry : fs::directory_iterator(dir, ec)){
		if(ec) return "";
		if(!entry.is_regular_file() || entry.path().extension() != ".hlc") continue;
		if(!found.empty()) return "";
		found = entry.path().filename().string();
	}

	return found;
}

void WriteFile(const fs::path& path, const std::string& text, const Options& o, int* written){
	std::error_code ec;

	if(fs::exists(path, ec) && !o.force){
		std::cout << "  kept      " << path.generic_string() << " (already there)\n";
		return;
	}

	fs::create_directories(path.parent_path(), ec);

	std::ofstream out(path, std::ios::binary); /* "\n" as written, on every platform */
	if(!out)
		throw InitError("pvminit: error: cannot write '" + path.generic_string() + "'");
	out << text;

	std::cout << "  written   " << path.generic_string() << "\n";
	(*written)++;
}

void Usage(){
	std::cout <<
		"usage: pvminit [directory] [options]\n"
		"\n"
		"Generates a Makefile and a .vscode/ for a PVM project: build, run and\n"
		"step-debug in the VM or on a board, with the tool paths already filled in.\n"
		"\n"
		"  -m, --main <file.hlc>  main source (default: the only .hlc there, else main.hlc)\n"
		"  -n, --name <name>      base name of the build outputs (default: the main source's)\n"
		"  -p, --port <COM3>      serial port of the board, for the debug configuration\n"
		"      --tools <dir>      where pvmc/pvmcc/pvmsim/pvmdbg live (default: the current\n"
		"                         directory when they are in it, else beside pvminit)\n"
		"      --make <program>   what to call make in tasks.json (default: whichever is on PATH)\n"
		"      --no-vscode        Makefile only, no .vscode/\n"
		"  -f, --force            overwrite files that are already there\n"
		"  -h, --help\n";
}

const char* Argument(int argc, char** argv, int* i, const char* option){
	if(*i + 1 >= argc)
		throw InitError(std::string("pvminit: error: ") + option + " needs a value");
	return argv[++(*i)];
}

Options Parse(int argc, char** argv, bool* helpAsked){
	Options o;
	bool 		dirGiven = false;

	for(int i = 1; i < argc; i++){
		std::string arg = argv[i];

		if(arg == "-h" || arg == "--help"){ *helpAsked = true; return o; }
		else if(arg == "-m" || arg == "--main") 	o.main 	 = Argument(argc, argv, &i, "--main");
		else if(arg == "-n" || arg == "--name") 	o.name 	 = Argument(argc, argv, &i, "--name");
		else if(arg == "-p" || arg == "--port") 	o.port 	 = Argument(argc, argv, &i, "--port");
		else if(arg == "--tools") 								o.tools  = Argument(argc, argv, &i, "--tools");
		else if(arg == "--make") 									o.make 	 = Argument(argc, argv, &i, "--make");
		else if(arg == "--no-vscode") 						o.vscode = false;
		else if(arg == "-f" || arg == "--force") 	o.force  = true;
		else if(!arg.empty() && arg[0] == '-')
			throw InitError("pvminit: error: unknown option '" + arg + "'");
		else if(dirGiven)
			throw InitError("pvminit: error: more than one directory given ('" + arg + "')");
		else{
			o.dir 	 = arg;
			dirGiven = true;
		}
	}

	return o;
}

} /* namespace */

int main(int argc, char** argv){
	try{
		bool 		helpAsked = false;
		Options o 				= Parse(argc, argv, &helpAsked);

		if(helpAsked){
			Usage();
			return EXIT_SUCCESS;
		}

		std::error_code ec;
		fs::create_directories(o.dir, ec);
		if(!fs::is_directory(o.dir))
			throw InitError("pvminit: error: cannot create directory '" + o.dir.generic_string() + "'");

		if(o.main.empty()){
			o.main = SoleSource(o.dir);
			if(o.main.empty()) o.main = "main.hlc";
		}
		if(fs::path(o.main).extension() != ".hlc")
			throw InitError("pvminit: error: the main source must be a .hlc file, got '" + o.main + "'");
		if(o.name.empty())
			o.name = fs::path(o.main).stem().string();

		bool toolsFellBack = false;
		if(o.tools.empty()) o.tools = ToolsDirectory(&toolsFellBack);
		o.tools = fs::absolute(o.tools, ec).lexically_normal();
		if(o.make.empty()) o.make = FindMake();

		/* Forward slashes throughout: make and VS Code both take them on every
		 * platform, while a backslash inside a JSON string has to be doubled. The
		 * generated Makefile turns them back where cmd.exe insists on it. */
		const std::string tools 	= o.tools.generic_string();
		const std::string pvmdbg 	= tools + "/pvmdbg" + kExeSuffix;

		std::cout << "pvminit: " << fs::absolute(o.dir, ec).lexically_normal().generic_string() << "\n";

		int written = 0;

		std::string makefile = Replace(Replace(Replace(Replace(kMakefile,
			"@NAME@", o.name), "@SRC@", o.main), "@PORT@", o.port), "@TOOLS@", tools);
		WriteFile(o.dir / "Makefile", makefile, o, &written);

		WriteFile(o.dir / o.main, kMainHlc, o, &written);
		WriteFile(o.dir / ".gitignore", kGitignore, o, &written);

		if(o.vscode){
			std::string launch = Replace(Replace(Replace(kLaunchJson,
				"@NAME@", o.name), "@PORT@", o.port), "@PVMDBG@", pvmdbg);
			WriteFile(o.dir / ".vscode" / "launch.json", launch, o, &written);

			WriteFile(o.dir / ".vscode" / "tasks.json", Replace(kTasksJson, "@MAKE@", o.make), o, &written);
		}

		std::cout << "\ntools:  " << tools;
		if(toolsFellBack) std::cout << "  (beside pvminit: no tools in the current directory)";
		std::cout << "\n";
		if(!HasTools(o.tools))
			std::cout << "        no pvmc/pvmcc there -- set PVM_TOOLS in the Makefile,"
									 " or re-run with --tools\n";
		std::cout << "make:   " << o.make << "\n";
		std::cout << "\nnext:   " << o.make << " run                 build and run it in the simulator\n";
		std::cout << "        " << o.make << " debug               step through it in the VM\n";
		std::cout << "        " << o.make << " board PORT=" << o.port << "     step through it on the board\n";

		if(written == 0)
			std::cout << "\nnothing was written -- pass --force to overwrite what is already there\n";

		return EXIT_SUCCESS;
	}catch(const std::exception& e){
		std::cerr << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
