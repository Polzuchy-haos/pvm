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
add_subdirectory(compilator)
add_subdirectory(simulator)

enable_testing()

add_test(NAME run_bytecode_listing COMMAND $<TARGET_FILE:pvmsim> ${PVM_EXAMPLE_BYTECODE})
set_tests_properties(run_bytecode_listing PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_test_new_instructions COMMAND $<TARGET_FILE:pvmsim> ${PVM_TESTINSTR_BYTECODE})
set_tests_properties(run_test_new_instructions PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")

add_test(NAME run_test_frame_pointer COMMAND $<TARGET_FILE:pvmsim> ${PVM_TESTFP_BYTECODE})
set_tests_properties(run_test_frame_pointer PROPERTIES PASS_REGULAR_EXPRESSION "pvmsim: halted")
