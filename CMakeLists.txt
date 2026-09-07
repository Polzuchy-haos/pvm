cmake_minimum_required(VERSION 3.10)
project(pvm C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

add_subdirectory(pvm)
add_subdirectory(hlc)
add_subdirectory(compilator)
add_subdirectory(simulator)

enable_testing()

add_test(NAME run_pvmdbg_test COMMAND $<TARGET_FILE:pvmdbg_test>)
set_tests_properties(run_pvmdbg_test PROPERTIES PASS_REGULAR_EXPRESSION "ALL OK")

add_test(NAME run_bytecode_listing COMMAND $<TARGET_FILE:pvmsim> ${PVM_EXAMPLE_BYTECODE})
set_tests_properties(run_bytecode_listing PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_test_new_instructions COMMAND $<TARGET_FILE:pvmsim> ${PVM_TESTINSTR_BYTECODE})
set_tests_properties(run_test_new_instructions PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_test_frame_pointer COMMAND $<TARGET_FILE:pvmsim> ${PVM_TESTFP_BYTECODE})
set_tests_properties(run_test_frame_pointer PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_test_svc COMMAND $<TARGET_FILE:pvmsim> ${PVM_TESTSVC_BYTECODE})
set_tests_properties(run_test_svc PROPERTIES PASS_REGULAR_EXPRESSION "SVC print -> 42")

add_test(NAME run_hlc_square COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_SQUARE_BYTECODE})
set_tests_properties(run_hlc_square PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_counter COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_COUNTER_BYTECODE})
set_tests_properties(run_hlc_counter PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_arrays_and_logic COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_ARRLOGIC_BYTECODE})
set_tests_properties(run_hlc_arrays_and_logic PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_import_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_IMPORTDEMO_BYTECODE})
set_tests_properties(run_hlc_import_demo PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_svc_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_SVCDEMO_BYTECODE})
set_tests_properties(run_hlc_svc_demo PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: SVC print -> 42")

add_test(NAME run_hlc_extern_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_EXTERNDEMO_BYTECODE})
set_tests_properties(run_hlc_extern_demo PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_hlc_time_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_TIMEDEMO_BYTECODE})
set_tests_properties(run_hlc_time_demo PROPERTIES PASS_REGULAR_EXPRESSION "STORE 0x1 -> 10")

add_test(NAME run_hlc_const_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_CONSTDEMO_BYTECODE})
set_tests_properties(run_hlc_const_demo PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: SVC print -> 24")

add_test(NAME run_hlc_sugar_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_SUGARDEMO_BYTECODE})
set_tests_properties(run_hlc_sugar_demo PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: SVC print -> 56")

add_test(NAME run_hlc_more_sugar_demo COMMAND $<TARGET_FILE:pvmsim> ${PVM_HLC_MORESUGARDEMO_BYTECODE})
set_tests_properties(run_hlc_more_sugar_demo PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: SVC print -> 155")

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
