cmake_minimum_required(VERSION 3.10)
project(pvm C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

# Release/ is the folder meant to be handed over: the tools, the VM core, the
# library, the manuals and the editor extension, all in one place. It is
# assembled here rather than committed, so it cannot fall behind the sources it
# is made of. copy_if_different leaves an unchanged file alone, so a build that
# changed nothing leaves the working tree clean.
set(PVM_RELEASE_DIR ${CMAKE_SOURCE_DIR}/Release)

# Hung off the published file rather than off the target's POST_BUILD: a
# POST_BUILD step only runs when the target relinks, so deleting something out
# of Release/ would never bring it back. With the copy declared as producing
# that file, a missing one is simply rebuilt -- which matters now that Release/
# is not in version control and the build is the only thing that can restore it.
function(pvm_publish_tool target)
	set(published ${PVM_RELEASE_DIR}/${target}${CMAKE_EXECUTABLE_SUFFIX})

	add_custom_command(
		OUTPUT ${published}
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> ${published}
		DEPENDS ${target}
		COMMENT "Publishing ${target} to Release/"
	)
	add_custom_target(${target}_published ALL DEPENDS ${published})
endfunction()

# The manuals ship with the tools for the same reason: someone handed only this
# folder should not have to come back to the repository to read how any of it
# works. Copied on every build so the snapshot cannot quietly fall behind the
# source it was written against; copy_if_different leaves untouched files alone.
#
# Globbed at configure time, so a brand new .md needs a re-run of cmake before it
# is picked up -- which is what "cmake -S . -B build" already does.
file(GLOB PVM_DOC_SOURCES ${CMAKE_SOURCE_DIR}/docs/*.md)

add_custom_target(pvm_publish_docs ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory ${PVM_RELEASE_DIR}/docs
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PVM_DOC_SOURCES} ${PVM_RELEASE_DIR}/docs
	COMMENT "Publishing docs/ to Release/docs/"
)

#
# The rest of what Release/ holds. Everything in that folder except its own
# README is assembled here, which is what lets it stay out of version control:
# a folder that is entirely reproducible does not need to be committed, and
# committing built binaries is how a repository quietly turns into a warehouse.
#
#   pvm/              the VM core, to compile straight into a firmware -- the
#                     one part of this project that ends up on the MCU
#   lib/              the .hlc standard library pvmcc looks for beside itself
#   vscode-extension/ the editor extension, its dependencies included so that
#                     installing it needs no Node (test/ is left behind: it is
#                     for developing the extension, not for using it)
#
add_custom_target(pvm_publish_extras ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory ${PVM_RELEASE_DIR}/pvm
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
					${CMAKE_SOURCE_DIR}/pvm/pvm.c ${CMAKE_SOURCE_DIR}/pvm/pvm.h
					${CMAKE_SOURCE_DIR}/pvm/pvmdbg.c ${CMAKE_SOURCE_DIR}/pvm/pvmdbg.h
					${CMAKE_SOURCE_DIR}/pvm/pvmprint.c ${CMAKE_SOURCE_DIR}/pvm/pvmprint.h
					${PVM_RELEASE_DIR}/pvm

	COMMAND ${CMAKE_COMMAND} -E copy_directory
					${CMAKE_SOURCE_DIR}/lib ${PVM_RELEASE_DIR}/lib

	COMMAND ${CMAKE_COMMAND} -E copy_directory
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/src
					${PVM_RELEASE_DIR}/vscode-extension/src
	COMMAND ${CMAKE_COMMAND} -E copy_directory
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/syntaxes
					${PVM_RELEASE_DIR}/vscode-extension/syntaxes
	COMMAND ${CMAKE_COMMAND} -E copy_directory
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/snippets
					${PVM_RELEASE_DIR}/vscode-extension/snippets
	COMMAND ${CMAKE_COMMAND} -E copy_directory
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/node_modules
					${PVM_RELEASE_DIR}/vscode-extension/node_modules
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/package.json
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/README.md
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/README.en.md
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/language-configuration-hlc.json
					${CMAKE_SOURCE_DIR}/tools/vscode-pvm/language-configuration-pvms.json
					${PVM_RELEASE_DIR}/vscode-extension

	COMMENT "Assembling the rest of Release/ (pvm, lib, vscode-extension)"
)

add_subdirectory(pvm)
add_subdirectory(hlc)
add_subdirectory(compilator)
add_subdirectory(simulator)
add_subdirectory(debugger)
add_subdirectory(pvminit)

enable_testing()

add_test(NAME run_pvmdbg_test COMMAND $<TARGET_FILE:pvmdbg_test>)
set_tests_properties(run_pvmdbg_test PROPERTIES PASS_REGULAR_EXPRESSION "ALL OK")

# Drives the interactive tool itself, commands on stdin -- see run_session.cmake
# for why this needs a script rather than a plain COMMAND.
add_test(NAME run_pvmdbg_session COMMAND ${CMAKE_COMMAND}
	-DTOOL=$<TARGET_FILE:pvmdbg>
	-DBIN=${PVM_HLC_SQUARE_BYTECODE}
	-DSESSION=${CMAKE_SOURCE_DIR}/debugger/smoke_session.txt
	-P ${CMAKE_SOURCE_DIR}/debugger/run_session.cmake)

# The program kept on the host, the VM only executing what it is sent.
add_test(NAME run_pvmdbg_host COMMAND ${CMAKE_COMMAND}
	-DTOOL=$<TARGET_FILE:pvmdbg>
	-DBIN=${PVM_HLC_SQUARE_BYTECODE}
	-DSESSION=${CMAKE_SOURCE_DIR}/debugger/host_session.txt
	-P ${CMAKE_SOURCE_DIR}/debugger/run_host_session.cmake)

# Over TCP, against pvmboard -- see run_tcp_session.cmake. The ports are fixed
# and unlikely to be taken; two, because each pvmboard serves one session.
add_test(NAME run_pvmdbg_tcp COMMAND ${CMAKE_COMMAND}
	-DTOOL=$<TARGET_FILE:pvmdbg>
	-DBOARD=$<TARGET_FILE:pvmboard>
	-DBIN=${PVM_HLC_SQUARE_BYTECODE}
	-DMAP=${CMAKE_BINARY_DIR}/bytecode/square.dbgmap
	-DSESSION=${CMAKE_SOURCE_DIR}/debugger/tcp_session.txt
	-DHOST_SESSION=${CMAKE_SOURCE_DIR}/debugger/host_session.txt
	-DPORT=47021
	-DWORK=${CMAKE_CURRENT_BINARY_DIR}
	-P ${CMAKE_SOURCE_DIR}/debugger/run_tcp_session.cmake)

# Variables by name -- the debug map's symbols, read back as a call stack.
add_test(NAME run_pvmdbg_vars COMMAND ${CMAKE_COMMAND}
	-DTOOL=$<TARGET_FILE:pvmdbg>
	-DBIN=${PVM_HLC_ARRLOGIC_BYTECODE}
	-DSESSION=${CMAKE_SOURCE_DIR}/debugger/vars_session.txt
	-P ${CMAKE_SOURCE_DIR}/debugger/run_vars_session.cmake)

# The same tool in the mode the VS Code extension drives it in.
add_test(NAME run_pvmdbg_machine COMMAND ${CMAKE_COMMAND}
	-DTOOL=$<TARGET_FILE:pvmdbg>
	-DBIN=${PVM_HLC_SQUARE_BYTECODE}
	-DSESSION=${CMAKE_SOURCE_DIR}/debugger/machine_session.txt
	-P ${CMAKE_SOURCE_DIR}/debugger/run_machine.cmake)

# The VS Code extension's adapter, driven with real DAP traffic over a pipe and
# a real pvmdbg underneath -- no editor involved. Skipped where Node is not
# installed, since nothing else in this project needs it.
find_program(PVM_NODE node)
if(PVM_NODE)
	add_test(NAME run_vscode_dap_session
		COMMAND ${PVM_NODE} ${CMAKE_SOURCE_DIR}/tools/vscode-pvm/test/dapSession.test.js
						$<TARGET_FILE:pvmdbg> ${PVM_HLC_SQUARE_BYTECODE})
	set_tests_properties(run_vscode_dap_session PROPERTIES PASS_REGULAR_EXPRESSION "ALL OK")
else()
	message(STATUS "node not found -- skipping the VS Code adapter test")
endif()

# pvminit generates a project, and the test builds and runs that project with
# make -- the only way to find out that what it writes still works. Skipped
# where make is missing, which is possible on Windows even though everything
# else here builds.
find_program(PVM_MAKE NAMES make mingw32-make gmake)
if(PVM_MAKE)
	add_test(NAME run_pvminit_generated COMMAND ${CMAKE_COMMAND}
		-DPVMINIT=$<TARGET_FILE:pvminit>
		-DMAKE=${PVM_MAKE}
		-DPVMCC=$<TARGET_FILE:pvmcc>
		-DPVMC=$<TARGET_FILE:pvmc>
		-DPVMSIM=$<TARGET_FILE:pvmsim>
		-DPVMDISASM=$<TARGET_FILE:pvmdisasm>
		-DTOOLS=${PVM_RELEASE_DIR}
		-DWORK=${CMAKE_BINARY_DIR}/pvminit_test
		-P ${CMAKE_SOURCE_DIR}/pvminit/run_generated.cmake)
	set_tests_properties(run_pvminit_generated PROPERTIES PASS_REGULAR_EXPRESSION "GENERATED PROJECT OK")
else()
	message(STATUS "make not found -- skipping the pvminit test")
endif()

add_test(NAME run_bytecode_listing COMMAND $<TARGET_FILE:pvmsim> ${PVM_EXAMPLE_BYTECODE})
set_tests_properties(run_bytecode_listing PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_test_new_instructions COMMAND $<TARGET_FILE:pvmsim> ${PVM_TESTINSTR_BYTECODE})
set_tests_properties(run_test_new_instructions PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_test_frame_pointer COMMAND $<TARGET_FILE:pvmsim> ${PVM_TESTFP_BYTECODE})
set_tests_properties(run_test_frame_pointer PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_test_svc COMMAND $<TARGET_FILE:pvmsim> ${PVM_TESTSVC_BYTECODE})
set_tests_properties(run_test_svc PROPERTIES PASS_REGULAR_EXPRESSION "STORE 0x4007 -> 7")

add_test(NAME run_hlc_square COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_SQUARE_BYTECODE})
set_tests_properties(run_hlc_square PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_counter COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_COUNTER_BYTECODE})
set_tests_properties(run_hlc_counter PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_arrays_and_logic COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_ARRLOGIC_BYTECODE})
set_tests_properties(run_hlc_arrays_and_logic PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_import_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_IMPORTDEMO_BYTECODE})
set_tests_properties(run_hlc_import_demo PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_svc_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_SVCDEMO_BYTECODE})
set_tests_properties(run_hlc_svc_demo PROPERTIES PASS_REGULAR_EXPRESSION "STORE 0x0 -> 7")

add_test(NAME run_hlc_extern_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_EXTERNDEMO_BYTECODE})
set_tests_properties(run_hlc_extern_demo PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_time_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_TIMEDEMO_BYTECODE})
set_tests_properties(run_hlc_time_demo PROPERTIES PASS_REGULAR_EXPRESSION "STORE 0x1 -> 10")

add_test(NAME run_hlc_const_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_CONSTDEMO_BYTECODE})
set_tests_properties(run_hlc_const_demo PROPERTIES PASS_REGULAR_EXPRESSION "STORE 0x4007 -> 24")

add_test(NAME run_hlc_sugar_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_SUGARDEMO_BYTECODE})
set_tests_properties(run_hlc_sugar_demo PROPERTIES PASS_REGULAR_EXPRESSION "STORE 0x4007 -> 56")

add_test(NAME run_hlc_more_sugar_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_MORESUGARDEMO_BYTECODE})
set_tests_properties(run_hlc_more_sugar_demo PROPERTIES PASS_REGULAR_EXPRESSION "STORE 0x4007 -> 155")

add_test(NAME run_hlc_sections_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_SECTIONSDEMO_BYTECODE})
set_tests_properties(run_hlc_sections_demo PROPERTIES PASS_REGULAR_EXPRESSION "STORE 0x4007 -> 20")

# Контроль границ секций: эти исходники обязаны НЕ собираться, причём с
# конкретной диагностикой. CTest при заданном PASS_REGULAR_EXPRESSION смотрит
# только на вывод и игнорирует код возврата, поэтому проверяется именно текст
# ошибки — тест не пройдёт и в случае, если сборка вдруг окажется успешной.
set(PVM_BOUNDS_DIR ${CMAKE_SOURCE_DIR}/example/bounds)
set(PVM_BOUNDS_OUT ${CMAKE_BINARY_DIR}/bounds_should_not_exist)

add_test(NAME reject_pvms_bad_data_address
	COMMAND $<TARGET_FILE:pvmc> ${PVM_BOUNDS_DIR}/bad_ram.pvms -o ${PVM_BOUNDS_OUT}.bin)
set_tests_properties(reject_pvms_bad_data_address PROPERTIES
	PASS_REGULAR_EXPRESSION "data address 100 .*is out of range")

add_test(NAME reject_pvms_code_overflow
	COMMAND $<TARGET_FILE:pvmc> ${PVM_BOUNDS_DIR}/bad_code.pvms -o ${PVM_BOUNDS_OUT}.bin)
set_tests_properties(reject_pvms_code_overflow PROPERTIES
	PASS_REGULAR_EXPRESSION "exceeds the declared .code size of 4 bytes")

add_test(NAME reject_hlc_ram_overflow
	COMMAND $<TARGET_FILE:pvmcc> ${PVM_BOUNDS_DIR}/bad_globals.hlc -o ${PVM_BOUNDS_OUT}.pvms)
set_tests_properties(reject_hlc_ram_overflow PROPERTIES
	PASS_REGULAR_EXPRESSION "'table' does not fit in RAM \\(8 words\\)")

add_test(NAME reject_hlc_bad_pinned_address
	COMMAND $<TARGET_FILE:pvmcc> ${PVM_BOUNDS_DIR}/bad_address.hlc -o ${PVM_BOUNDS_OUT}.pvms)
set_tests_properties(reject_hlc_bad_pinned_address PROPERTIES
	PASS_REGULAR_EXPRESSION "'OUT_OF_RANGE' is pinned to an address outside addressable memory")

add_test(NAME reject_hlc_stack_overflow
	COMMAND $<TARGET_FILE:pvmcc> ${PVM_BOUNDS_DIR}/bad_stack.hlc -o ${PVM_BOUNDS_OUT}.pvms)
set_tests_properties(reject_hlc_stack_overflow PROPERTIES
	PASS_REGULAR_EXPRESSION "worst-case stack use is [0-9]+ words, which does not fit")

# В отличие от остальных — этот файл компилируется успешно, проверяется именно
# то, что анализатор честно признаёт невозможность оценки при рекурсии.
add_test(NAME warn_hlc_recursion
	COMMAND $<TARGET_FILE:pvmcc> ${PVM_BOUNDS_DIR}/recursion.hlc -o ${PVM_BOUNDS_OUT}.pvms)
set_tests_properties(warn_hlc_recursion PROPERTIES
	PASS_REGULAR_EXPRESSION "recursion found -- worst-case depth cannot be bounded")
