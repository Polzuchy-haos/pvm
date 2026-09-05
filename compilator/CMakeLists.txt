add_executable(pvmc
	pvmc.c
	assembler.c
	assembler.h
)

target_include_directories(pvmc PRIVATE ${CMAKE_SOURCE_DIR}/pvm)

# Assemble the example listing at build time so pvmsim has something to run out of the box.
set(PVM_EXAMPLE_SRC ${CMAKE_SOURCE_DIR}/example/bytecode_listing.pvms)
set(PVM_EXAMPLE_BYTECODE ${CMAKE_BINARY_DIR}/bytecode/bytecode_listing.bin)

add_custom_command(
	OUTPUT ${PVM_EXAMPLE_BYTECODE}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bytecode
	COMMAND $<TARGET_FILE:pvmc> ${PVM_EXAMPLE_SRC} -o ${PVM_EXAMPLE_BYTECODE}
	DEPENDS pvmc ${PVM_EXAMPLE_SRC}
	COMMENT "Assembling example/bytecode_listing.pvms"
)
add_custom_target(example_bytecode ALL DEPENDS ${PVM_EXAMPLE_BYTECODE})

set(PVM_EXAMPLE_BYTECODE ${PVM_EXAMPLE_BYTECODE} PARENT_SCOPE)
