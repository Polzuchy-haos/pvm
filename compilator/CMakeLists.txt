add_library(pvmopcodes STATIC
	opcodes.cpp
	opcodes.h
)
target_include_directories(pvmopcodes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(pvmopcodes PRIVATE ${CMAKE_SOURCE_DIR}/pvm)

add_library(pvmdisasm_lib STATIC
	disassembler.cpp
	disassembler.h
)
target_link_libraries(pvmdisasm_lib PUBLIC pvmopcodes)
target_include_directories(pvmdisasm_lib PRIVATE ${CMAKE_SOURCE_DIR}/pvm)

add_executable(pvmc
	pvmc.cpp
	assembler.cpp
	assembler.h
)
target_link_libraries(pvmc PRIVATE pvmopcodes)
target_include_directories(pvmc PRIVATE ${CMAKE_SOURCE_DIR}/pvm)

add_executable(pvmdisasm
	pvmdisasm.cpp
)
target_link_libraries(pvmdisasm PRIVATE pvmdisasm_lib)
target_include_directories(pvmdisasm PRIVATE ${CMAKE_SOURCE_DIR}/pvm)

if(MINGW)
	target_link_options(pvmc PRIVATE -static-libgcc -static-libstdc++)
	target_link_options(pvmdisasm PRIVATE -static-libgcc -static-libstdc++)
endif()

pvm_publish_tool(pvmc)
pvm_publish_tool(pvmdisasm)

# Run every example .pvms through the whole toolchain at build time, so there's
# always a known-good set of outputs to look at (bin/hex/carray/listing/disasm)
# and something for the CTest smoke tests (see top-level CMakeLists.txt) to run.
set(PVM_BYTECODE_DIR ${CMAKE_BINARY_DIR}/bytecode)

function(pvm_compile_example name src)
	set(bin 		${PVM_BYTECODE_DIR}/${name}.bin)
	set(lst 		${PVM_BYTECODE_DIR}/${name}.lst)
	set(hexf 		${PVM_BYTECODE_DIR}/${name}.hex)
	set(carray 	${PVM_BYTECODE_DIR}/${name}.c)
	set(disasm 	${PVM_BYTECODE_DIR}/${name}.disasm.txt)
	set(dbgmap 	${PVM_BYTECODE_DIR}/${name}.dbgmap)

	add_custom_command(
		OUTPUT ${bin} ${lst} ${dbgmap}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${PVM_BYTECODE_DIR}
		COMMAND $<TARGET_FILE:pvmc> ${src} -o ${bin} -f bin -l ${lst} -g ${dbgmap}
		DEPENDS pvmc ${src}
		COMMENT "Assembling ${name} (bin + listing + debug map)"
	)
	add_custom_command(
		OUTPUT ${hexf}
		COMMAND $<TARGET_FILE:pvmc> ${src} -o ${hexf} -f hex
		DEPENDS pvmc ${src}
		COMMENT "Assembling ${name} (Intel HEX)"
	)
	add_custom_command(
		OUTPUT ${carray}
		COMMAND $<TARGET_FILE:pvmc> ${src} -o ${carray} -f carray
		DEPENDS pvmc ${src}
		COMMENT "Assembling ${name} (C array)"
	)
	add_custom_command(
		OUTPUT ${disasm}
		COMMAND $<TARGET_FILE:pvmdisasm> ${bin} -o ${disasm}
		DEPENDS pvmdisasm ${bin}
		COMMENT "Disassembling ${name}.bin"
	)

	add_custom_target(${name}_bytecode ALL DEPENDS ${bin} ${lst} ${dbgmap} ${hexf} ${carray} ${disasm})

	set(${name}_BYTECODE ${bin} PARENT_SCOPE)
endfunction()

# Same as above, but for a .hlc (high-level language) source: compile it down to
# .pvms via pvmcc first, then reuse pvm_compile_example unchanged for the rest.
function(pvm_compile_hlc_example name hlc_src)
	set(generated_pvms ${PVM_BYTECODE_DIR}/${name}.pvms)

	add_custom_command(
		OUTPUT ${generated_pvms}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${PVM_BYTECODE_DIR}
		COMMAND $<TARGET_FILE:pvmcc> ${hlc_src} -o ${generated_pvms}
		DEPENDS pvmcc ${PVM_LIB_STAMP} ${hlc_src}
		COMMENT "Compiling ${name} (hlc -> pvms)"
	)

	pvm_compile_example(${name} ${generated_pvms})
	set(${name}_BYTECODE ${${name}_BYTECODE} PARENT_SCOPE)
endfunction()

pvm_compile_example(bytecode_listing 			${CMAKE_SOURCE_DIR}/example/bytecode_listing.pvms)
pvm_compile_example(test_new_instructions 	${CMAKE_SOURCE_DIR}/example/test_new_instructions.pvms)
pvm_compile_example(test_frame_pointer 		${CMAKE_SOURCE_DIR}/example/test_frame_pointer.pvms)
pvm_compile_example(test_svc 							${CMAKE_SOURCE_DIR}/example/test_svc.pvms)

pvm_compile_hlc_example(square 						${CMAKE_SOURCE_DIR}/example/square.hlc)
pvm_compile_hlc_example(counter 						${CMAKE_SOURCE_DIR}/example/counter.hlc)
pvm_compile_hlc_example(arrays_and_logic 	${CMAKE_SOURCE_DIR}/example/arrays_and_logic.hlc)
pvm_compile_hlc_example(blink 							${CMAKE_SOURCE_DIR}/example/blink.hlc)
pvm_compile_hlc_example(import_demo 				${CMAKE_SOURCE_DIR}/example/import_demo.hlc)
pvm_compile_hlc_example(svc_demo 					${CMAKE_SOURCE_DIR}/example/svc_demo.hlc)
pvm_compile_hlc_example(extern_demo 				${CMAKE_SOURCE_DIR}/example/extern_demo.hlc)
pvm_compile_hlc_example(time_demo 					${CMAKE_SOURCE_DIR}/example/time_demo.hlc)
pvm_compile_hlc_example(const_demo 				${CMAKE_SOURCE_DIR}/example/const_demo.hlc)
pvm_compile_hlc_example(sugar_demo 				${CMAKE_SOURCE_DIR}/example/sugar_demo.hlc)
pvm_compile_hlc_example(more_sugar_demo 		${CMAKE_SOURCE_DIR}/example/more_sugar_demo.hlc)
pvm_compile_hlc_example(sections_demo 			${CMAKE_SOURCE_DIR}/example/sections_demo.hlc)
# example/bounds/ намеренно НЕ собирается здесь: эти файлы обязаны падать с
# ошибкой, их проверяют отдельные тесты в корневом CMakeLists.txt.
# lib/mathlib.hlc and lib/time.hlc have no main() -- they're modules meant to
# be "import"-ed (see import_demo.hlc / time_demo.hlc), not compiled
# standalone. Same for example/fastmath.pvms, pulled in by extern_demo.hlc's
# "extern ... from fastmath;".

set(PVM_EXAMPLE_BYTECODE 		${bytecode_listing_BYTECODE} 			PARENT_SCOPE)
set(PVM_TESTINSTR_BYTECODE ${test_new_instructions_BYTECODE} 	PARENT_SCOPE)
set(PVM_TESTFP_BYTECODE 		${test_frame_pointer_BYTECODE} 		PARENT_SCOPE)
set(PVM_TESTSVC_BYTECODE 	${test_svc_BYTECODE} 							PARENT_SCOPE)
set(PVM_HLC_SQUARE_BYTECODE 			${square_BYTECODE} 						PARENT_SCOPE)
set(PVM_HLC_COUNTER_BYTECODE 			${counter_BYTECODE} 					PARENT_SCOPE)
set(PVM_HLC_ARRLOGIC_BYTECODE 		${arrays_and_logic_BYTECODE} 	PARENT_SCOPE)
set(PVM_HLC_IMPORTDEMO_BYTECODE 	${import_demo_BYTECODE} 			PARENT_SCOPE)
set(PVM_HLC_SVCDEMO_BYTECODE 			${svc_demo_BYTECODE} 					PARENT_SCOPE)
set(PVM_HLC_EXTERNDEMO_BYTECODE 	${extern_demo_BYTECODE} 			PARENT_SCOPE)
set(PVM_HLC_TIMEDEMO_BYTECODE 		${time_demo_BYTECODE} 				PARENT_SCOPE)
set(PVM_HLC_CONSTDEMO_BYTECODE 	${const_demo_BYTECODE} 			PARENT_SCOPE)
set(PVM_HLC_SUGARDEMO_BYTECODE 	${sugar_demo_BYTECODE} 			PARENT_SCOPE)
set(PVM_HLC_MORESUGARDEMO_BYTECODE ${more_sugar_demo_BYTECODE} 	PARENT_SCOPE)
set(PVM_HLC_SECTIONSDEMO_BYTECODE 	${sections_demo_BYTECODE} 		PARENT_SCOPE)
