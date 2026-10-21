# Checks pvminit by using what it produces, rather than by comparing its output
# against a copy of the expected text: generate a project into a scratch
# directory, then actually build and run it with make. A template that no longer
# assembles is the failure worth catching, and only a real build catches it.
#
# The tools are overridden on make's command line so the freshly built ones are
# exercised, whatever happens to be sitting in Release/ at the time.
#
# Driven from CTest -- see the top-level CMakeLists.txt.

function(fail message)
	message(FATAL_ERROR "pvminit test: ${message}")
endfunction()

function(expect_file path)
	if(NOT EXISTS "${path}")
		fail("expected '${path}' to be generated")
	endif()
endfunction()

function(expect_contains path needle)
	file(READ "${path}" content)
	string(FIND "${content}" "${needle}" at)
	if(at LESS 0)
		fail("'${path}' does not contain '${needle}'")
	endif()
endfunction()

# JSON with // comments in it, which VS Code accepts and CMake's parser does
# not. Only whole comment lines are removed, which is all the templates ever
# write -- and unlike stripping every "//" it cannot damage a path inside a
# string. Read with file(READ) rather than file(STRINGS), which cuts lines short
# at the first non-ASCII byte and so would shred the Russian comments.
function(expect_valid_jsonc path out_json)
	file(READ "${path}" content)
	string(REGEX REPLACE "(^|\n)[ \t]*//[^\n]*" "\\1" stripped "${content}")

	string(JSON version ERROR_VARIABLE error GET "${stripped}" version)
	if(error)
		fail("'${path}' is not valid JSON once its comments are removed: ${error}")
	endif()

	set(${out_json} "${stripped}" PARENT_SCOPE)
endfunction()

function(run_ok label)
	execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
	if(NOT code EQUAL 0)
		fail("${label} failed (${code})\n${out}\n${err}")
	endif()
	set(last_output "${out}${err}" PARENT_SCOPE)
endfunction()

# Same, but started from a particular directory -- which pvminit reads as an
# argument of its own, so it cannot be tested any other way.
function(run_ok_in directory label)
	execute_process(COMMAND ${ARGN} WORKING_DIRECTORY ${directory}
		RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
	if(NOT code EQUAL 0)
		fail("${label} failed (${code})\n${out}\n${err}")
	endif()
	set(last_output "${out}${err}" PARENT_SCOPE)
endfunction()

# ---- a project generated from nothing -----------------------------------

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

set(project ${WORK}/fresh)

run_ok("pvminit" ${PVMINIT} ${project} --port COM9 --tools ${TOOLS})

expect_file(${project}/Makefile)
expect_file(${project}/main.hlc)
expect_file(${project}/.gitignore)
expect_file(${project}/.vscode/launch.json)
expect_file(${project}/.vscode/tasks.json)

expect_contains(${project}/Makefile "NAME  := main")
expect_contains(${project}/Makefile "PVM_TOOLS ?= ${TOOLS}")
expect_contains(${project}/Makefile "PORT  ?= COM9")

expect_valid_jsonc(${project}/.vscode/launch.json launch)
string(JSON type GET "${launch}" configurations 0 type)
if(NOT type STREQUAL "pvm")
	fail("the first launch configuration is of type '${type}', not 'pvm'")
endif()
string(JSON port GET "${launch}" configurations 1 port)
if(NOT port STREQUAL "COM9")
	fail("the board configuration points at '${port}', not the requested COM9")
endif()

expect_valid_jsonc(${project}/.vscode/tasks.json tasks)
string(JSON label GET "${tasks}" tasks 0 label)
if(NOT label STREQUAL "pvm: build")
	fail("the first task is '${label}', but launch.json asks for 'pvm: build'")
endif()

# ---- and it has to build, run and clean ---------------------------------

set(overrides PVMCC=${PVMCC} PVMC=${PVMC} PVMSIM=${PVMSIM} PVMDISASM=${PVMDISASM})

run_ok("make build" ${MAKE} -C ${project} build ${overrides})

expect_file(${project}/build/main.bin)
expect_file(${project}/build/main.lst)
expect_file(${project}/build/main.dbgmap)

# Absolute, or VS Code has no way to know which file to open on a breakpoint.
file(READ ${project}/build/main.dbgmap map)
if(NOT map MATCHES "0x0000 ${project}/main.hlc:[0-9]+")
	fail("the debug map does not point at the generated source by absolute path:\n${map}")
endif()

run_ok("make run" ${MAKE} -C ${project} run ${overrides})
if(NOT last_output MATCHES "pvmsim: halted")
	fail("the generated program did not run to completion:\n${last_output}")
endif()

run_ok("make disasm" ${MAKE} -C ${project} disasm ${overrides})
expect_file(${project}/build/main.disasm.txt)

run_ok("make clean" ${MAKE} -C ${project} clean ${overrides})
if(EXISTS ${project}/build)
	fail("make clean left the build directory behind")
endif()

# ---- generating over an existing project --------------------------------

# Files already there are left alone, which is what makes it safe to re-run
# pvminit in a project that has been edited since.
file(WRITE ${project}/main.hlc "// edited by hand\nvoid main() { halt; }\n")
run_ok("pvminit again" ${PVMINIT} ${project} --tools ${TOOLS})
file(READ ${project}/main.hlc kept)
if(NOT kept MATCHES "edited by hand")
	fail("re-running pvminit overwrote a source file it should have kept")
endif()

# ...and --force is how you ask for the opposite.
run_ok("pvminit --force" ${PVMINIT} ${project} --tools ${TOOLS} --force)
file(READ ${project}/main.hlc replaced)
if(replaced MATCHES "edited by hand")
	fail("--force did not overwrite the source file")
endif()

# ---- a directory that already holds one program -------------------------

# The single .hlc in it is the program, whatever it is called -- no --main
# needed, and no skeleton generated next to it under another name.
set(existing ${WORK}/existing)
file(MAKE_DIRECTORY ${existing})
file(WRITE ${existing}/blink.hlc "int led;\nvoid main() { led = 1; halt; }\n")

run_ok("pvminit on an existing source" ${PVMINIT} ${existing} --tools ${TOOLS})
expect_contains(${existing}/Makefile "NAME  := blink")
expect_contains(${existing}/Makefile "SRC   := blink.hlc")
if(EXISTS ${existing}/main.hlc)
	fail("a skeleton was generated beside the program that was already there")
endif()

run_ok("make build (existing)" ${MAKE} -C ${existing} build ${overrides})
expect_file(${existing}/build/blink.bin)

# ---- finding the tools without being told where they are ----------------

# Started from a directory that holds a toolchain, that is the toolchain the
# project is wired to -- whichever copy of pvminit was the one invoked.
set(from_tools ${WORK}/from_tools)
run_ok_in(${TOOLS} "pvminit from the toolchain directory" ${PVMINIT} ${from_tools})
expect_contains(${from_tools}/Makefile "PVM_TOOLS ?= ${TOOLS}")

# Started from anywhere else, it falls back to its own directory: a path with no
# pvmc in it would only produce a project that cannot build.
get_filename_component(beside ${PVMINIT} DIRECTORY)
set(from_elsewhere ${WORK}/from_elsewhere)
run_ok_in(${WORK} "pvminit from a directory with no tools" ${PVMINIT} ${from_elsewhere})
expect_contains(${from_elsewhere}/Makefile "PVM_TOOLS ?= ${beside}")

message(STATUS "GENERATED PROJECT OK")
